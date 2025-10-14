#include <stdio.h>
#include <time.h>
#include "bench_lib.h"


// 计算两个 timespec 的差值（以纳秒为单位）
long long diff_ns(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
}

int main(void) {
    const int NUM_FRAGMENTS = 9;
    const int NUM_ITERATIONS = 1;  // 每个函数重复调用次数
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