//清除cache的版本
// #define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <math.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include "linuxAPI_lib.h"


// ===== 测试参数 =====
#define CPU_ID          1
#define NUM_ITERATIONS  1000
#define COOLDOWN_MS     100   // 测完一个函数后的冷却时间（可为 0）


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
    *llc_size  = (size_t)max_sz;
}

// ---------- 对齐分配 ----------
static void* alloc_aligned(size_t align, size_t sz) {
    void* p = NULL;
    if (posix_memalign(&p, align, sz) != 0) return NULL;
    memset(p, 0, sz);
    return p;
}

// ========== 扰动/清理组件 ==========

// xorshift64
static inline uint64_t xs64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

// 一些简单函数，放在文件作用域，用于间接调用扰动 I-cache
static inline int f0(int x){ return x + 1; }
static inline int f1(int x){ return (x ^ 0x5A5A5A5A) + 3; }
static inline int f2(int x){ return (x * 3) - 7; }
static inline int f3(int x){ return (x << 1) ^ 0x9E3779B1; }

// 防止优化：把中间值送进寄存器
static inline void touch_reg(int x) { asm volatile("" :: "r"(x) : "memory"); }

// 扰动 I-cache & 分支预测器：不可预测分支 + 间接调用
static void icache_bp_scrub(void) {
    static uint64_t state = 0x123456789abcdefULL;
    volatile int acc = 0;

    for (int i = 0; i < (1<<15); ++i) {
        uint64_t r = xs64(&state);
        int idx = (int)(r & 3);
        int x = (int)(r >> 32);
        int y;

        // 间接选择不同路径/函数
        switch (idx) {
            case 0: y = f0(x); break;
            case 1: y = f1(x); break;
            case 2: y = f2(x); break;
            default: y = f3(x); break;
        }

        // 不可预测分支
        if (r & (1ULL<<5)) acc ^= y; else acc += y;
        if (r & (1ULL<<7)) acc ^= (int)r; else acc -= (int)r;

        // 偶尔写回防止被优化
        if ((i & 127) == 0) touch_reg(acc);
    }
#if defined(__aarch64__)
    asm volatile("isb" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
    (void)acc;
}

// 扰动 TLB：mmap 一大块匿名内存逐页触达并 mprotect 翻转权限
static void tlb_scrub(void) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    size_t total = (size_t)32 << 20; // 32MB
    void* p = mmap(NULL, total, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;

    volatile char *vp = (volatile char*)p;
    for (size_t off = 0; off < total; off += pg) vp[off]++;

    // 改权限触发 TLB shootdown
    (void)mprotect(p, total, PROT_READ);
    (void)mprotect(p, total, PROT_READ|PROT_WRITE);

    munmap(p, total);
}

// ---------- “重度 flush”：D-cache + I-cache/分支预测器 + TLB ----------
static void flush_cache_all(void) {
    static char*  buf = NULL;
    static size_t buf_sz = 0;
    static size_t line   = 64;
    static size_t llc    = 4 * 1024 * 1024;

    if (!buf) {
        get_cache_info(&line, &llc);
        buf_sz = 2 * llc; // 双倍 LLC
        buf = (char*)alloc_aligned(line, buf_sz);
        if (!buf) {
            buf_sz = 10 * 1024 * 1024;
            buf = (char*)malloc(buf_sz);
            if (buf) memset(buf, 0, buf_sz);
        }
    }
    if (!buf) return;

    // D-cache：逐 cache line 写
    for (size_t i = 0; i < buf_sz; i += line) {
        ((volatile char*)buf)[i] ^= (char)(i & 0xFF);
    }
#if defined(__aarch64__)
    asm volatile("dsb sy" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif

    // I-cache & 分支预测器扰动
    icache_bp_scrub();

    // TLB 扰动
    tlb_scrub();
}





// ===== 绑定到指定 CPU =====
static void set_affinity_to_cpu(void) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(CPU_ID, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        perror("sched_setaffinity");
        exit(1);
    }
}

// ===== 简单热身，避免冷启动抖动 =====
static void warmup_cpu_ms(int ms){
    struct timespec s,e;
    clock_gettime(CLOCK_MONOTONIC_RAW,&s);
    do{
        for(volatile int i=0;i<2000000;i++) { }  // 空转
        clock_gettime(CLOCK_MONOTONIC_RAW,&e);
    }while((e.tv_sec - s.tv_sec)*1000 + (e.tv_nsec - s.tv_nsec)/1000000 < ms);
}

// ===== ns 计时工具（统一使用 MONOTONIC_RAW）=====
static inline long long time_diff_ns(const struct timespec start, const struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000000000LL +
           (end.tv_nsec - start.tv_nsec);
}

// ===== 跑一个函数的基准，并把每次耗时写文件 =====
static void run_test_one(const char* label, void (*fn)(void), const char* out_path) {
    FILE* f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "fopen('%s') failed: %s\n", out_path, strerror(errno));
        exit(1);
    }
    struct timespec s, e;
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &s);
        fn();                        // 被测函数（内部一般含若干次原语操作）
        clock_gettime(CLOCK_MONOTONIC_RAW, &e);
        long long elapsed = time_diff_ns(s, e);
        fprintf(f, "%lld\n", elapsed);
        flush_cache_all();
    }
    fclose(f);
    printf("[OK] %-16s -> %s\n", label, out_path);
}

// ===== 微睡眠（毫秒）=====
static void msleep(int ms){
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(void) {
    set_affinity_to_cpu();
    warmup_cpu_ms(500);

    // ===== 共享版本 =====
    run_test_one("fragment0",  API_fragment0,      "frag0_shared_times.txt");   msleep(COOLDOWN_MS);
    run_test_one("fragment1",  API_fragment1,      "frag1_shared_times.txt");   msleep(COOLDOWN_MS);
    run_test_one("fragment2",  API_fragment2,      "frag2_shared_times.txt");   msleep(COOLDOWN_MS);
    run_test_one("fragment3",  API_fragment3,      "frag3_shared_times.txt");   msleep(COOLDOWN_MS);
    run_test_one("fragment4",  API_fragment4,      "frag4_shared_times.txt");   msleep(COOLDOWN_MS);
    run_test_one("fragment5",  API_fragment5,      "frag5_shared_times.txt");   msleep(COOLDOWN_MS);
    run_test_one("fragment6",  API_fragment6,      "frag6_shared_times.txt");   msleep(COOLDOWN_MS);

    // ===== 并行版本 =====
    run_test_one("para_fragment0", API_para_fragment0, "frag0_para_times.txt"); msleep(COOLDOWN_MS);
    run_test_one("para_fragment1", API_para_fragment1, "frag1_para_times.txt"); msleep(COOLDOWN_MS);
    run_test_one("para_fragment2", API_para_fragment2, "frag2_para_times.txt"); msleep(COOLDOWN_MS);
    run_test_one("para_fragment3", API_para_fragment3, "frag3_para_times.txt"); msleep(COOLDOWN_MS);
    run_test_one("para_fragment4", API_para_fragment4, "frag4_para_times.txt"); msleep(COOLDOWN_MS);
    run_test_one("para_fragment5", API_para_fragment5, "frag5_para_times.txt"); msleep(COOLDOWN_MS);
    run_test_one("para_fragment6", API_para_fragment6, "frag6_para_times.txt"); msleep(COOLDOWN_MS);

    puts("Done. Files generated:\n"
         "  frag0_shared_times.txt .. frag6_shared_times.txt\n"
         "  frag0_para_times.txt   .. frag6_para_times.txt");
    return 0;
}