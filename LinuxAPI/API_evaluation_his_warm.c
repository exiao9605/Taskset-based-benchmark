#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sched.h>

// 你需要提供这两个 API 的定义
void API_fragment0(void);       // Shared
void API_para_fragment0(void);  // Private

#define CPU_ID         1
#define NUM_ITERATIONS 1000

// 绑定到指定 CPU
void set_affinity_to_cpu() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(CPU_ID, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        perror("sched_setaffinity");
        exit(1);
    }
}

static void warmup_cpu_ms(int ms){
  struct timespec s,e; clock_gettime(CLOCK_MONOTONIC_RAW,&s);
  do{ for(volatile int i=0;i<2000000;i++); clock_gettime(CLOCK_MONOTONIC_RAW,&e); }
  while((e.tv_sec-s.tv_sec)*1000 + (e.tv_nsec-s.tv_nsec)/1000000 < ms);
}

// 计算时间差（纳秒）
static inline long long time_diff_ns(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000000000LL +
           (end.tv_nsec - start.tv_nsec);
}

int main(void) {
    set_affinity_to_cpu();

    FILE *f_shared  = fopen("shared_times.txt", "w");
    FILE *f_private = fopen("private_times.txt", "w");
    if (!f_shared || !f_private) {
        perror("fopen");
        return 1;
    }

    struct timespec s, e;
    long long elapsed;
    
    warmup_cpu_ms(500);
    // Shared 测试
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &s);
        API_fragment0();
        clock_gettime(CLOCK_MONOTONIC_RAW, &e);
        elapsed = time_diff_ns(s, e);
        fprintf(f_shared, "%lld\n", elapsed);
    }

    // Private 测试
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &s);
        API_para_fragment0();
        clock_gettime(CLOCK_MONOTONIC_RAW, &e);
        elapsed = time_diff_ns(s, e);
        fprintf(f_private, "%lld\n", elapsed);
    }

    fclose(f_shared);
    fclose(f_private);

    printf("Done. Results saved to shared_times.txt and private_times.txt\n");
    return 0;
}