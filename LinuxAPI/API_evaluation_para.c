//无核心分配版本, 并行延迟测试

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include "linuxAPI_lib.h"  // 提供 benchmark_fragment10()

#define MAX_THREADS 10     // 你可以调整此宏为目标最大核数

// 用于传入线程的启动参数
struct thread_arg {
    int iterations;
};

// 每个线程执行此函数
void* thread_worker(void* arg) {
    struct thread_arg* t_arg = (struct thread_arg*)arg;
    for (int i = 0; i < t_arg->iterations; ++i) {
        API_fragment0();
    }
    return NULL;
}

// 获取纳秒级时间戳差值
long long time_diff_ns(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000000000LL +
           (end.tv_nsec - start.tv_nsec);
}

int main() {
    const int iterations_per_thread = 10000;  // 每线程执行多少次
    printf("Threads\tTotal Time (ms)\tAvg Time per Thread (ms)\n");
    printf("------------------------------------------------------\n");

    for (int num_threads = 1; num_threads <= MAX_THREADS; ++num_threads) {
        pthread_t threads[MAX_THREADS];
        struct thread_arg t_args[MAX_THREADS];

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        // 启动多个线程
        for (int i = 0; i < num_threads; ++i) {
            t_args[i].iterations = iterations_per_thread;
            pthread_create(&threads[i], NULL, thread_worker, &t_args[i]);
        }

        // 等待线程结束
        for (int i = 0; i < num_threads; ++i) {
            pthread_join(threads[i], NULL);
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        long long duration_ns = time_diff_ns(start, end);
        double duration_ms = duration_ns / 1e6;

        printf("%d\t%.2f\t\t%.2f\n",
               num_threads,
               duration_ms,
               duration_ms / num_threads);
    }

    return 0;
}