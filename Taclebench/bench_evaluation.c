//以九个fragment为一组，循环执行10000次，记录平均值/最大最小值  清除ID cache TLB

#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

#include "bench_lib.h"

// ======== 可调开关 ========
#define NUM_GROUPS     10000   // 组数：每组依次跑 9 个 fragment
#define CPU_ID         1      // 绑定的 CPU 核
#define DO_FLUSH_CACHE 1      // 0: 不清; 1: 每次调用前“重度 flush”
// ==========================

static inline long long diff_ns(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
}

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

static inline int f0(int x){ return x + 1; }
static inline int f1(int x){ return (x ^ 0x5A5A5A5A) + 3; }
static inline int f2(int x){ return (x * 3) - 7; }
static inline int f3(int x){ return (x << 1) ^ 0x9E3779B1; }
static inline void touch_reg(int x) { asm volatile("" :: "r"(x) : "memory"); }

static void icache_bp_scrub(void) {
    static uint64_t state = 0x123456789abcdefULL;
    volatile int acc = 0;

    for (int i = 0; i < (1<<15); ++i) {
        uint64_t r = xs64(&state);
        int idx = (int)(r & 3);
        int x = (int)(r >> 32);
        int y;
        switch (idx) {
            case 0: y = f0(x); break;
            case 1: y = f1(x); break;
            case 2: y = f2(x); break;
            default: y = f3(x); break;
        }
        if (r & (1ULL<<5)) acc ^= y; else acc += y;
        if (r & (1ULL<<7)) acc ^= (int)r; else acc -= (int)r;
        if ((i & 127) == 0) touch_reg(acc);
    }
#if defined(__aarch64__)
    asm volatile("isb" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
    (void)acc;
}

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

static void flush_cache_all(void) {
    static char*  buf = NULL;
    static size_t buf_sz = 0;
    static size_t line   = 64;
    static size_t llc    = 4 * 1024 * 1024;
    if (!buf) {
        get_cache_info(&line, &llc);
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

// ======= 统计 =======
typedef struct {
    long long min_v;
    long long max_v;
    long double sum;
    long double sum2;
    long double freq_sum; // NEW: 频率总和
    size_t n;
} Stats;

static void stats_init(Stats* st) {
    st->min_v = LLONG_MAX;
    st->max_v = LLONG_MIN;
    st->sum = 0.0L;
    st->sum2 = 0.0L;
    st->freq_sum = 0.0L;
    st->n = 0;
}

static void stats_push(Stats* st, long long x, long freq_khz) { // NEW
    if (x < st->min_v) st->min_v = x;
    if (x > st->max_v) st->max_v = x;
    st->sum += (long double)x;
    st->sum2 += (long double)x * (long double)x;
    st->freq_sum += (long double)freq_khz;
    st->n++;
}

static void stats_finish(const Stats* st, long double* mean, long double* sd, long double* mean_freq) { // NEW
    if (st->n == 0) { *mean = 0; *sd = 0; *mean_freq = 0; return; }
    *mean = st->sum / st->n;
    *mean_freq = st->freq_sum / st->n;
    long double var = 0.0L;
    if (st->n > 1) {
        var = (st->sum2 / st->n) - (*mean) * (*mean);
        if (var < 0) var = 0;
    }
    *sd = sqrt((double)var);
}

static void warmup_cpu_ms(int ms){
  struct timespec s,e; clock_gettime(CLOCK_MONOTONIC_RAW,&s);
  do{ for(volatile int i=0;i<2000000;i++); clock_gettime(CLOCK_MONOTONIC_RAW,&e); }
  while((e.tv_sec-s.tv_sec)*1000 + (e.tv_nsec-s.tv_nsec)/1000000 < ms);
}

// NEW: 读取当前 CPU 频率(kHz)
static long read_cur_freq_khz(int cpu) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    return read_sysfs_long(path);
}

// ======= 主程序 =======
int main(void) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(CPU_ID, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        perror("sched_setaffinity failed");
        return 1;
    }

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

    FILE* outs[9] = {0};
    char fname[64];
    for (int i = 0; i < 9; ++i) {
        snprintf(fname, sizeof(fname), "fragment%d_times.txt", i);
        outs[i] = fopen(fname, "w");
        if (!outs[i]) {
            perror("fopen");
            for (int k = 0; k < i; ++k) if (outs[k]) fclose(outs[k]);
            return 1;
        }
    }

    Stats stats[9];
    for (int i = 0; i < 9; ++i) stats_init(&stats[i]);

    //warmup_cpu_ms(500);

    for (int g = 0; g < NUM_GROUPS; ++g) {
        for (int i = 0; i < 9; ++i) {
            if (DO_FLUSH_CACHE) flush_cache_all();
            struct timespec s, e;
            clock_gettime(CLOCK_MONOTONIC, &s);
            fragments[i]();
            clock_gettime(CLOCK_MONOTONIC, &e);

            long long t = diff_ns(s, e);
            long freq = read_cur_freq_khz(CPU_ID); // NEW
            fprintf(outs[i], "%lld,%ld\n", t, freq); // NEW: 时间 + 频率
            stats_push(&stats[i], t, freq); // NEW
        }
    }

    for (int i = 0; i < 9; ++i) {
        fflush(outs[i]);
        fclose(outs[i]);
    }

    printf("Summary over %d groups (CPU%d, DO_FLUSH_CACHE=%d)\n",
           NUM_GROUPS, CPU_ID, DO_FLUSH_CACHE);
    printf("----------------------------------------------------------------------------------\n");
    printf("%-35s  %-12s %-12s %-12s %-12s %-12s\n",
           "Fragment", "Min(ns)", "Max(ns)", "Mean(ns)", "StdDev(ns)", "MeanFreq(kHz)");
    for (int i = 0; i < 9; ++i) {
        long double mean, sd, mean_freq;
        stats_finish(&stats[i], &mean, &sd, &mean_freq);
        printf("%-35s  %-12lld %-12lld %-12.2Lf %-12.2Lf %-12.0Lf\n",
               fragment_names[i],
               stats[i].min_v, stats[i].max_v,
               mean, sd, mean_freq);
    }
    return 0;
}