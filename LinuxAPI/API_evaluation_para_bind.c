//CPU绑定版本 并行延迟测试

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include "linuxAPI_lib.h"  // 提供 API_fragment0()

#define MAX_THREADS 10     // 最大线程数，可调整
#define ITERATIONS 10000   // 每个线程执行多少次 API_fragment0()

struct thread_arg {
    int thread_id;
    int iterations;
};

void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

void* thread_worker(void* arg) {
    struct thread_arg* t_arg = (struct thread_arg*)arg;

    // 将线程绑定到对应核心
    set_affinity(t_arg->thread_id % sysconf(_SC_NPROCESSORS_ONLN));

    for (int i = 0; i < t_arg->iterations; ++i) {
        API_fragment0();
    }

    return NULL;
}

long long time_diff_ns(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000000000LL +
           (end.tv_nsec - start.tv_nsec);
}

int main() {
    printf("Threads\tTotal Time (ms)\n");
    printf("--------------------------\n");

    for (int num_threads = 1; num_threads <= MAX_THREADS; ++num_threads) {
        pthread_t threads[MAX_THREADS];
        struct thread_arg t_args[MAX_THREADS];

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        for (int i = 0; i < num_threads; ++i) {
            t_args[i].thread_id = i;
            t_args[i].iterations = ITERATIONS;
            pthread_create(&threads[i], NULL, thread_worker, &t_args[i]);
        }

        for (int i = 0; i < num_threads; ++i) {
            pthread_join(threads[i], NULL);
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        double duration_ms = time_diff_ns(start, end) / 1e6;

        printf("%d\t%.2f\n", num_threads, duration_ms);
    }

    return 0;
}