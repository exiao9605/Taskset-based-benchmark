
//观察循环1/10/100/1000次的平均执行时间

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sched.h>

// 你需要提供这两个 API 函数的定义
void API_fragment_shared(void);
void API_fragment_private(void);

// 设置进程绑定到 CPU 0
void set_affinity_to_cpu() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

// 获取时间差（纳秒）
long long time_diff_ns(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000000000LL +
           (end.tv_nsec - start.tv_nsec);
}

int main() {
    set_affinity_to_cpu();  // 固定运行在 CPU0

    int test_sizes[] = {1, 10, 100, 1000, 10000, 100000};
    int num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);

    for (int i = 0; i < num_tests; ++i) {
        int iterations = test_sizes[i];

        // Shared semaphore 测试
        struct timespec start_shared, end_shared;
        clock_gettime(CLOCK_MONOTONIC, &start_shared);
        for (int j = 0; j < iterations; ++j) {
            API_fragment0();
        }
        clock_gettime(CLOCK_MONOTONIC, &end_shared);
        long long total_ns_shared = time_diff_ns(start_shared, end_shared);
        double avg_ns_shared = (double)total_ns_shared / iterations;

        printf("[Shared]  Iterations: %-8d Total: %-12lld Avg: %.2f ns\n",
               iterations, total_ns_shared, avg_ns_shared);

        // Private semaphore 测试
        struct timespec start_priv, end_priv;
        clock_gettime(CLOCK_MONOTONIC, &start_priv);
        for (int j = 0; j < iterations; ++j) {
            API_para_fragment0();
        }
        clock_gettime(CLOCK_MONOTONIC, &end_priv);
        long long total_ns_priv = time_diff_ns(start_priv, end_priv);
        double avg_ns_priv = (double)total_ns_priv / iterations;

        printf("[Private] Iterations: %-8d Total: %-12lld Avg: %.2f ns\n\n",
               iterations, total_ns_priv, avg_ns_priv);
    }

    return 0;
}