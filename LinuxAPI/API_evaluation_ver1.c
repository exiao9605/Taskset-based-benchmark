#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include "linuxAPI_lib.h"

// ===== 测试参数 =====
#define CPU_ID          1
#define NUM_ITERATIONS  1000
#define COOLDOWN_MS     100   // 测完一个函数后的冷却时间（可为 0）

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