
//以九个fragment为一组，循环执行10000次，记录平均值/最大最小值

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

#include "bench_lib.h"

// ======== 可调开关 ========
#define NUM_GROUPS     10000      // 组数：每组依次跑完 9 个 fragment
#define CPU_ID         1          // 绑定的 CPU 核
#define DO_FLUSH_CACHE 0        // 是否每次调用前清缓存（0: 不清, 1: 清）
// ==========================

// ---------- 辅助：从 /sys 读整数 ----------
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

// ---------- 清空 cache（按行写 2×LLC） ----------
static void flush_cache(void) {
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
    asm volatile("dsb sy" ::: "memory");
}

// ---------- 计时 ----------
static inline long long diff_ns(struct timespec s, struct timespec e) {
    return (e.tv_sec - s.tv_sec) * 1000000000LL + (e.tv_nsec - s.tv_nsec);
}
static inline void now_timespec(struct timespec* t) {
    clock_gettime(CLOCK_MONOTONIC, t);
}

// ---------- 在线统计（Welford） ----------
typedef struct {
    long long min_v;
    long long max_v;
    double mean;
    double M2;
    long long n;
} OnlineStats;

static void stats_init(OnlineStats* st) {
    st->min_v = LLONG_MAX;
    st->max_v = LLONG_MIN;
    st->mean = 0.0;
    st->M2 = 0.0;
    st->n = 0;
}
static void stats_push(OnlineStats* st, long long x) {
    if (x < st->min_v) st->min_v = x;
    if (x > st->max_v) st->max_v = x;
    st->n++;
    double delta = x - st->mean;
    st->mean += delta / st->n;
    double delta2 = x - st->mean;
    st->M2 += delta * delta2;
}
static double stats_variance(const OnlineStats* st) {
    return (st->n > 1) ? (st->M2 / (st->n - 1)) : 0.0;
}
static double stats_stddev(const OnlineStats* st) {
    return sqrt(stats_variance(st));
}

int main(void) {
    // 绑定 CPU
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(CPU_ID, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        perror("sched_setaffinity failed");
        return 1;
    }

    enum { NUM_FRAGMENTS = 9 };

    void (*fragments[NUM_FRAGMENTS])(void) = {
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

    const char *fragment_names[NUM_FRAGMENTS] = {
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

    // 打开 9 个输出文件
    FILE* fps[NUM_FRAGMENTS];
    char fname[64];
    for (int i = 0; i < NUM_FRAGMENTS; ++i) {
        snprintf(fname, sizeof(fname), "fragment%d.txt", i);
        fps[i] = fopen(fname, "w");
        if (!fps[i]) { perror("fopen"); return 1; }
        // 提升写入性能：加大缓冲
        setvbuf(fps[i], NULL, _IOFBF, 1 << 20); // 1MB
    }

    // 初始化统计
    OnlineStats stats[NUM_FRAGMENTS];
    for (int i = 0; i < NUM_FRAGMENTS; ++i) stats_init(&stats[i]);

    // 主循环：每组依次执行 9 个 fragment，共 NUM_GROUPS 组
    for (int g = 0; g < NUM_GROUPS; ++g) {
        for (int i = 0; i < NUM_FRAGMENTS; ++i) {
            if (DO_FLUSH_CACHE) flush_cache();

            struct timespec s, e;
            now_timespec(&s);
            fragments[i]();
            now_timespec(&e);

            long long t = diff_ns(s, e);

            // 写入对应文件，一行一个 ns
            fprintf(fps[i], "%lld\n", t);

            // 更新统计
            stats_push(&stats[i], t);
        }
    }

    // 关闭文件
    for (int i = 0; i < NUM_FRAGMENTS; ++i) fclose(fps[i]);

    // 打印并写 summary
    FILE* fs = fopen("summary.txt", "w");
    if (!fs) { perror("fopen summary.txt"); return 1; }

    printf("Summary (ns):\n");
    printf("Fragment                             Min         Max         Mean        StdDev\n");
    printf("--------------------------------------------------------------------------------\n");
    fprintf(fs, "Summary (ns):\n");
    fprintf(fs, "Fragment                             Min         Max         Mean        StdDev\n");
    fprintf(fs, "--------------------------------------------------------------------------------\n");

    for (int i = 0; i < NUM_FRAGMENTS; ++i) {
        double sd = stats_stddev(&stats[i]);
        printf("%-35s %-11lld %-11lld %-11.2f %-11.2f\n",
               fragment_names[i],
               stats[i].min_v, stats[i].max_v, stats[i].mean, sd);
        fprintf(fs, "%-35s %-11lld %-11lld %-11.2f %-11.2f\n",
                fragment_names[i],
                stats[i].min_v, stats[i].max_v, stats[i].mean, sd);
    }
    fclose(fs);

    printf("\nDone.\n"
           "- Per-run times: fragment0.txt ... fragment8.txt (each has %d lines)\n"
           "- Summary also saved to summary.txt\n"
           "- CPU pinned to %d, cache %s before each call\n",
           NUM_GROUPS, CPU_ID, DO_FLUSH_CACHE ? "flushed" : "NOT flushed");

    return 0;
}