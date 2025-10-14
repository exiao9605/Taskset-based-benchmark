#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "bench_lib.h"

#define CACHE_FLUSH_SIZE (10 * 1024 * 1024)  // 10MB，足够刷掉L1~L3 cache

// 清空cache：读写一个大数组（模拟 cache thrashing）
void flush_cache(void) {
    static char *buffer = NULL;
    if (!buffer) {
        buffer = (char *)malloc(CACHE_FLUSH_SIZE);
        memset(buffer, 0xA5, CACHE_FLUSH_SIZE);
    }
    for (int i = 0; i < CACHE_FLUSH_SIZE; i++) {
        buffer[i] += i & 0x7F;  // 模拟读写访问
    }
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
    const int NUM_ITERATIONS = 100;  // 每个函数重复调用次数（避免精度误差）

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
    long long total_ns = 0;

    for (int j = 0; j < NUM_ITERATIONS; ++j) {
        flush_cache();  // 可选：放在每次 fragment 执行前

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        fragments[i]();

        clock_gettime(CLOCK_MONOTONIC, &end);
        total_ns += diff_ns(start, end);
    }

    double avg_time = (double)total_ns / NUM_ITERATIONS;
    printf("%-35s : %10.2f ns\n", fragment_names[i], avg_time);
    }

    return 0;
}