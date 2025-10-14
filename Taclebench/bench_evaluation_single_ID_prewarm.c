
//单次测试 清空IDcache 预热

#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>

#include "bench_lib.h"

// ===== 可调参数 =====
#define CPU_ID        1      // 绑定的核
#define WARMUP_MS     300    // 预热时长（毫秒）
#define REPEAT        1      // 单次窗口内重复执行次数（短片段可设大一点，如 100）
// ====================

// ---------- 从 /sys 读整数 ----------
static long read_sysfs_long(const char* path) {
    char buf[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char *end = NULL;
    long val = strtol(buf, &end, 10);
    if (val <= 0) return -1;
    while (*end == ' ' || *end == '\n' || *end == '\t') end++;
    if (*end == 'K' || *end == 'k') val *= 1024L;
    else if (*end == 'M' || *end == 'm') val *= 1024L * 1024L;
    return val;
}

// ---------- 读取 cache 信息 ----------
static void get_cache_info(size_t* line_size, size_t* llc_size) {
    long ls = read_sysfs_long("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size");
    if (ls <= 0) ls = 64;

    const char* idx_paths[] = {
        "/sys/devices/system/cpu/cpu0/cache/index3/size",
        "/sys/devices/system/cpu/cpu0/cache/index2/size",
        "/sys/devices/system/cpu/cpu0/cache/index1/size",
        "/sys/devices/system/cpu/cpu0/cache/index0/size",
    };
    long max_sz = -1;
    for (int i = 0; i < 4; ++i) {
        long sz = read_sysfs_long(idx_paths[i]);
        if (sz > max_sz) max_sz = sz;
    }
    if (max_sz <= 0) max_sz = 4 * 1024 * 1024;

    *line_size = (size_t)ls;
    *llc_size  = (size_t)max_sz; // A73 多数情况下就是 L2 容量
}

// ---------- 对齐分配 ----------
static void* alloc_aligned(size_t align, size_t sz) {
    void* p = NULL;
    if (posix_memalign(&p, align, sz) != 0) return NULL;
    memset(p, 0, sz);
    return p;
}

// ========== 扰动/清理组件（全清） ==========

static inline uint64_t xs64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x; return x;
}

// I-cache & 分支预测扰动
static void icache_bp_scrub(void) {
    static uint64_t state = 0x123456789abcdefULL;
    volatile int acc = 0;
    for (int i = 0; i < (1<<15); ++i) {
        uint64_t r = xs64(&state);
        int idx = (int)(r & 3);
        int x = (int)(r >> 32);
        int y;
        switch (idx) {
            case 0: y = x + 1; break;
            case 1: y = (x ^ 0x5A5A5A5A) + 3; break;
            case 2: y = (x * 3) - 7; break;
            default: y = (x << 1) ^ 0x9E3779B1; break;
        }
        if (r & (1ULL<<5)) acc ^= y; else acc += y;
        if (r & (1ULL<<7)) acc ^= (int)r; else acc -= (int)r;
        if ((i & 127) == 0) asm volatile("" :: "r"(acc) : "memory");
    }
#if defined(__aarch64__)
    asm volatile("isb" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
    (void)acc;
}

// TLB 扰动（一次映射，逐页触达+权限翻转）
static void tlb_scrub(void) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    size_t total = (size_t)32 << 20; // 32MB
    void* p = mmap(NULL, total, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    volatile char *vp = (volatile char*)p;
    for (size_t off = 0; off < total; off += pg) vp[off]++;
    (void)mprotect(p, total, PROT_READ);
    (void)mprotect(p, total, PROT_READ|PROT_WRITE);
    munmap(p, total);
}

// 全清：D + I/分支预测 + TLB
static void flush_cache_all(void) {
    static char*  buf = NULL;
    static size_t buf_sz = 0;
    static size_t line   = 64;
    static size_t llc    = 4 * 1024 * 1024;

    if (!buf) {
        get_cache_info(&line, &llc);
        // 适当放小也行：1.5×llc；这里保持 2×，保证冷
        buf_sz = 2 * llc;
        buf = (char*)alloc_aligned(line, buf_sz);
        if (!buf) {
            buf_sz = 10 * 1024 * 1024;
            buf = (char*)malloc(buf_sz);
            if (buf) memset(buf, 0, buf_sz);
        }
    }
    if (!buf) return;

    for (size_t i = 0; i < buf_sz; i += line) {
        ((volatile char*)buf)[i] ^= (char)(i & 0xFF);
    }
#if defined(__aarch64__)
    asm volatile("dsb sy" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif

    icache_bp_scrub();
    tlb_scrub();
}

// 预热（busy-wait 到指定毫秒）
static void warmup_cpu_ms(int ms) {
    struct timespec s, e;
    clock_gettime(CLOCK_MONOTONIC_RAW, &s);
    do {
        for (volatile int i = 0; i < 2000000; ++i) ;
        clock_gettime(CLOCK_MONOTONIC_RAW, &e);
    } while ((e.tv_sec - s.tv_sec) * 1000 + (e.tv_nsec - s.tv_nsec) / 1000000 < ms);
}

// 计时（纳秒）
static inline long long diff_ns(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
}

int main(void) {
    // 绑核
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(CPU_ID, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        perror("sched_setaffinity failed");
        return 1;
    }
    printf("Running on CPU %d\n", sched_getcpu());

    // 预热
    warmup_cpu_ms(WARMUP_MS);

    const int NUM_FRAGMENTS = 9;

    void (*fragments[9])(void) = {
        benchmark_fragment0,
        benchmark_fragment1,
        benchmark_fragment2,
        benchmark_fragment3,
        benchmark_fragment4,
        benchmark_fragment5,
        benchmark_fragment6,
        benchmark_fragment7,
        benchmark_fragment8
    };

    const char *fragment_names[9] = {
        "fragment0: Empty Loop",
        "fragment1: Binary Search",
        "fragment2: Bit Count Kernighan",
        "fragment3: Bit Count Lookup Table",
        "fragment4: Bitonic Sort",
        "fragment5: Bubble Sort",
        "fragment6: Count Negative Matrix",
        "fragment7: Dijkstra Shortest Path",
        "fragment8: Matrix Multiplication"
    };

    printf("Benchmark Results (Full flush before each call, ns):\n");
    printf("----------------------------------------------------------\n");

    for (int i = 0; i < NUM_FRAGMENTS; ++i) {
        flush_cache_all();  // 全清

        struct timespec s, e;
        clock_gettime(CLOCK_MONOTONIC_RAW, &s);

        for (int r = 0; r < REPEAT; ++r) {
            fragments[i]();
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &e);
        long long total_ns = diff_ns(s, e);
        double per_call = (double)total_ns / REPEAT;
        printf("%-35s : %10.2f ns\n", fragment_names[i], per_call);
    }

    return 0;
}