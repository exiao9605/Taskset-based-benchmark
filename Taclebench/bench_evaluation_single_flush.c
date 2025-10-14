
// // 单次测试 清除D cache


#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "bench_lib.h"

// --------- 新版 flush_cache（用户态、单文件可集成）---------

// 从 /sys 读取整数（失败返回 -1）
static long read_sysfs_long(const char* path) {
    char buf[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    // 允许诸如 "32768K" / "2M" 这样的格式
    char *end = NULL;
    long val = strtol(buf, &end, 10);
    if (val <= 0) return -1;
    // 处理单位
    while (*end == ' ' || *end == '\n' || *end == '\t') end++;
    if (*end == 'K' || *end == 'k') val *= 1024L;
    else if (*end == 'M' || *end == 'm') val *= 1024L * 1024L;
    return val;
}

// 读取 LLC 大小与行大小；失败时给出保守默认
static void get_cache_info(size_t* line_size, size_t* llc_size) {
    // line size: /sys/.../index0/coherency_line_size 通常各级相同，用 index0 足够
    long ls = read_sysfs_long("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size");
    if (ls <= 0) ls = 64; // 默认 64B

    // LLC 尽量选最后一级（找最大 size）
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
    if (max_sz <= 0) max_sz = 4 * 1024 * 1024; // 默认 4MB

    *line_size = (size_t)ls;
    *llc_size  = (size_t)max_sz;
}

// 对齐分配
static void* alloc_aligned(size_t align, size_t sz) {
    void* p = NULL;
    if (posix_memalign(&p, align, sz) != 0) return NULL;
    // 预触达，避免首次缺页成本干扰后续 flush 花费
    memset(p, 0, sz);
    return p;
}

// 清空cache：按 cache line 写 2×LLC，确保逐线触达；加入 DSB 栅栏
void flush_cache(void) {
    static char* buf = NULL;
    static size_t buf_sz = 0;
    static size_t line = 64;
    static size_t llc  = 4 * 1024 * 1024;

    if (!buf) {
        get_cache_info(&line, &llc);
        // 用 2×LLC 保证把共享/私有层全部顶掉（L1/L2/L3）
        buf_sz = 2 * llc;
        buf = (char*)alloc_aligned(line, buf_sz);
        if (!buf) { // 兜底
            buf_sz = 10 * 1024 * 1024;
            buf = (char*)malloc(buf_sz);
            if (buf) memset(buf, 0, buf_sz);
        }
    }
    if (!buf) return;

    // 逐 cache line 写；volatile 防止优化；加入 DSB 栅栏序列化
    for (size_t i = 0; i < buf_sz; i += line) {
        ((volatile char*)buf)[i] ^= (char)(i & 0xFF);
    }
    asm volatile("dsb sy" ::: "memory"); // 数据同步屏障，确保写回可见
    // I-cache 对数据基准一般无关；若要彻底冷 I-cache，可在执行热点前跳转一次大页代码或 fork/exec，这里不做
}

// 计算两个 timespec 的差值（纳秒）
long long diff_ns(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
}

int main(void) {
    // 1. 绑定到 CPU 核（如 CPU0）
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(0, &mask);  // 绑定到 CPU0
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        perror("sched_setaffinity failed");
        return 1;
    }

    const int NUM_FRAGMENTS = 9;
    const int NUM_ITERATIONS = 1;  // 每个函数重复调用次数（避免精度误差）

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

    printf("Benchmark Results (Average Time per Call, ns):\n");
    printf("----------------------------------------------------------\n");

    for (int i = 0; i < NUM_FRAGMENTS; ++i) {
        //flush_cache();  // 每个 fragment 前清 cache  受到 affinity影响/interrupt

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        for (int j = 0; j < NUM_ITERATIONS; ++j) {
            
            fragments[i]();

        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        long long total_ns = diff_ns(start, end);
        double avg_time = (double)total_ns / NUM_ITERATIONS;
        printf("%-35s : %10.2f ns\n", fragment_names[i], avg_time);
    }

    return 0;
}