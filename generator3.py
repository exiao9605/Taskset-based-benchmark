
# 改变填充算法

import os
import json
import random
import numpy as np
from jinja2 import Template
import time

# ------------------------- Fragment Cost Library -------------------------
shared_api_lib = [
    {"name": "API_fragment0", "para_name": "API_para_fragment0", "cost": 6.4},     # 冷态6.4  热态4.5
    {"name": "API_fragment1", "para_name": "API_para_fragment1", "cost": 3.8},     # 冷态3.8  热态2.2
    {"name": "API_fragment2", "para_name": "API_para_fragment2", "cost": 8},       # 冷态8    热态6.2
    {"name": "API_fragment3", "para_name": "API_para_fragment3", "cost": 5.2},     # 冷态5.2  热态3.5
    {"name": "API_fragment4", "para_name": "API_para_fragment4", "cost": 6},       # 冷态6    热态4.2
    {"name": "API_fragment5", "para_name": "API_para_fragment5", "cost": 20},      # 冷态20   热态12
   # {"name": "API_fragment6", "para_name": "API_para_fragment6", "cost": 1.4}

]

normal_api_lib = [
    {"name": "benchmark_fragment0", "cost": 40/1000},
    {"name": "benchmark_fragment1", "cost": 400/1000},
    {"name": "benchmark_fragment2", "cost": 40/1000},
    {"name": "benchmark_fragment3", "cost": 200/1000},
    {"name": "benchmark_fragment4", "cost": 3640/1000},
    {"name": "benchmark_fragment5", "cost": 21640/1000},
    {"name": "benchmark_fragment6", "cost": 6560/1000},
    {"name": "benchmark_fragment7", "cost": 560/1000},
    {"name": "benchmark_fragment8", "cost": 3400/1000}
]

api_cost_map = {api["name"]: api["cost"] for api in shared_api_lib + normal_api_lib}

# ------------------------- Segment Generation -------------------------
def generate_segments(FN, C, Cr, RaF_max):
    segments = []
    total_RaF_time = int(C * Cr)
    total_NF_time = C - total_RaF_time
    RaF_count = (FN + 1) // 2
    NF_count = FN // 2

    RaF_durations = np.random.dirichlet(np.ones(RaF_count)) * total_RaF_time
    NF_durations = np.random.dirichlet(np.ones(NF_count)) * total_NF_time

    RaF_durations = [min(int(t), RaF_max) for t in RaF_durations]
    NF_durations = [int(t) for t in NF_durations]

    for i in range(FN):
        if i % 2 == 0:
            duration = RaF_durations.pop() if RaF_durations else 0
            segments.append({"type": "RaF", "duration": duration, "apis": []})
        else:
            duration = NF_durations.pop() if NF_durations else 0
            segments.append({"type": "NF", "duration": duration, "apis": []})
    return segments

def fill_apis_for_segment(duration, api_pool, contention=0.5):
    filled = []
    remaining = duration
    # 预先计算最小 cost，避免死循环
    min_cost = min(api["cost"] for api in api_pool)

    while remaining + 1e-9 >= min_cost:  # 浮点容差
        # 只在可放下的 API 中随机
        feasible = [api for api in api_pool if api["cost"] <= remaining + 1e-9]
        if not feasible:
            break
        api = random.choice(feasible)

        # 共享/并行变体选择
        if "para_name" in api:
            if random.random() < contention:
                filled.append(api["name"])        # 共享（有竞争）
            else:
                filled.append(api["para_name"])   # 并行（无竞争）
        else:
            filled.append(api["name"])

        remaining -= api["cost"]

    return filled

# ------------------------- WCET & Period Generation -------------------------
def generate_random_wcet_and_period(N, wcet_min, wcet_max, period_factor=10):
    wcets = [random.randint(wcet_min, wcet_max) for _ in range(N)]
    periods = [w * period_factor for w in wcets]
    return wcets, periods

# ------------------------- Task Set Generation -------------------------
def generate_taskset(M, N, wcet_min, wcet_max, Cr, RaF_max, FN, contention):
    wcets, periods = generate_random_wcet_and_period(N, wcet_min, wcet_max)
    priorities = random.sample(range(1, N + 1), N)
    cores = [i % M for i in range(N)]

    taskset = {"meta": {"M": M, "N": N}, "tasks": []}
    for i in range(N):
        C = wcets[i]
        T = periods[i]
        segments = generate_segments(FN, C, Cr, RaF_max)
        for seg in segments:
            if seg["type"] == "RaF":
                seg["apis"] = fill_apis_for_segment(seg["duration"], shared_api_lib, contention)
            else:
                seg["apis"] = fill_apis_for_segment(seg["duration"], normal_api_lib)
        task = {
            "id": i,
            "core": cores[i],
            "priority": priorities[i],
            "period": T,
            "wcet": C,
            "segments": segments
        }
        taskset["tasks"].append(task)
    return taskset


c_template = r"""
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sched.h>

//#include "LinuxAPI/linuxAPI_lib.h"
//#include "Taclebench/bench_lib.h"
#include "linuxAPI_lib.h"
#include "bench_lib.h"

#define NUM_TASKS {{ taskset.tasks|length }}
#define NUM_CORES {{ taskset.meta.M }}
#define RUN_DURATION_SEC 5

typedef struct {
    int task_id;
    int core_id;
    int period_us;   // period (deadline = period)
    int wcet_us;     // theoretical WCET
    int segment_count;
    void (**segments)(void);
} TaskArgs;

pthread_t threads[NUM_TASKS];
TaskArgs  task_args[NUM_TASKS];

// global tallies for miss rate
static int job_counts[NUM_TASKS];
static int deadline_miss[NUM_TASKS];

// start/stop sync
static pthread_barrier_t start_barrier;
static volatile uint64_t global_start_us = 0;
static volatile uint64_t global_end_us   = 0;

static inline uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}
static inline void sleep_until_us(uint64_t tgt_us) {
    struct timespec ts;
    ts.tv_sec  = tgt_us / 1000000ULL;
    ts.tv_nsec = (long)((tgt_us % 1000000ULL) * 1000ULL);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

// ---- per-task log (CSV) ----
typedef struct {
    int period_idx;
    double delay_ratio; // actual_us / wcet_us
    int missed;         // 0/1
} TaskLogEntry;
typedef struct {
    TaskLogEntry* entries;
    size_t capacity;
    size_t count;
} TaskLog;
static TaskLog task_logs[NUM_TASKS];

// ---- per-core aggregates ----
typedef struct {
    double sum_delay;
    double min_delay;
    double max_delay;
    unsigned long long delay_count;
    unsigned long long jobs;
    unsigned long long misses;
} CoreAgg;
static CoreAgg core_stats[NUM_CORES];
static pthread_mutex_t core_mutex[NUM_CORES];

void* task_function(void* arg) {
    TaskArgs* t = (TaskArgs*)arg;

    // RT prio + affinity
    struct sched_param param; param.sched_priority = 10;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(t->core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    pthread_barrier_wait(&start_barrier);

    uint64_t next_release = global_start_us;
    uint64_t now = now_us();
    int k = 0;

    while (now < global_end_us) {
        uint64_t job_start = now_us();
        for (int i = 0; i < t->segment_count; ++i) t->segments[i]();
        uint64_t job_end = now_us();

        uint64_t actual_us = job_end - job_start;
        double delay_ratio = (t->wcet_us > 0) ? ((double)actual_us / (double)t->wcet_us) : 0.0;
        int missed = (actual_us > (uint64_t)t->period_us) ? 1 : 0;

        __sync_fetch_and_add(&job_counts[t->task_id], 1);
        if (missed) __sync_fetch_and_add(&deadline_miss[t->task_id], 1);

        // log
        TaskLog* tl = &task_logs[t->task_id];
        if (tl->count == tl->capacity) {
            size_t newcap = tl->capacity ? tl->capacity * 2 : 4096;
            tl->entries = (TaskLogEntry*)realloc(tl->entries, newcap * sizeof(TaskLogEntry));
            tl->capacity = newcap;
        }
        tl->entries[tl->count++] = (TaskLogEntry){ k, delay_ratio, missed };

        // per-core stats
        int c = t->core_id;
        pthread_mutex_lock(&core_mutex[c]);
        CoreAgg* cs = &core_stats[c];
        cs->sum_delay   += delay_ratio;
        cs->delay_count += 1;
        if (delay_ratio < cs->min_delay) cs->min_delay = delay_ratio;
        if (delay_ratio > cs->max_delay) cs->max_delay = delay_ratio;
        cs->jobs   += 1;
        cs->misses += missed;
        pthread_mutex_unlock(&core_mutex[c]);

        k++;
        next_release += (uint64_t)t->period_us;
        now = now_us();
        if (now < next_release) {
            sleep_until_us(next_release);
        } else {
            // overrun realign
            next_release = now;
        }
    }
    return NULL;
}

// ---- Jinja: segments arrays ----
{% for task in taskset.tasks %}
static void (*segments_{{ task.id }}[])() = {
{% for seg in task.segments %}{% for api in seg.apis %}    {{ api }},
{% endfor %}{% endfor %}
};
{% endfor %}

int main() {
    // init
    for (int i = 0; i < NUM_TASKS; ++i) {
        task_logs[i].entries = NULL; task_logs[i].capacity = 0; task_logs[i].count = 0;
        job_counts[i] = 0; deadline_miss[i] = 0;
    }
    for (int c = 0; c < NUM_CORES; ++c) {
        core_stats[c].sum_delay = 0.0;
        core_stats[c].min_delay = 1e300;
        core_stats[c].max_delay = -1e300;
        core_stats[c].delay_count = 0;
        core_stats[c].jobs = 0;
        core_stats[c].misses = 0;
        pthread_mutex_init(&core_mutex[c], NULL);
    }

    {% for task in taskset.tasks %}
    task_args[{{ task.id }}] = (TaskArgs){
        .task_id = {{ task.id }},
        .core_id = {{ task.core }},
        .period_us = {{ task.period }},
        .wcet_us = {{ task.wcet }},
        .segment_count = {{ task.segments | map(attribute='apis') | map('length') | sum }},
        .segments = segments_{{ task.id }}
    };
    {% endfor %}

    pthread_barrier_init(&start_barrier, NULL, NUM_TASKS + 1);
    global_start_us = now_us() + 100000ULL;
    global_end_us   = global_start_us + (uint64_t)RUN_DURATION_SEC * 1000000ULL;

    for (int i = 0; i < NUM_TASKS; ++i) {
        if (pthread_create(&threads[i], NULL, task_function, &task_args[i]) != 0) {
            perror("pthread_create"); return 1;
        }
    }

    pthread_barrier_wait(&start_barrier);
    for (int i = 0; i < NUM_TASKS; ++i) pthread_join(threads[i], NULL);

    // per-core
    printf("\nPer-core delay ratio (actual/wcet) and miss rate:\n");
    printf("Core | delay_count    mean        min        max  | jobs   miss  miss_rate(%%)\n");
    printf("-----|--------------------------------------------------------------------------\n");
    for (int c = 0; c < NUM_CORES; ++c) {
        double mean = core_stats[c].delay_count ? (core_stats[c].sum_delay / (double)core_stats[c].delay_count) : 0.0;
        double mr = core_stats[c].jobs ? (100.0 * (double)core_stats[c].misses / (double)core_stats[c].jobs) : 0.0;
        printf("%4d | %11llu  %10.6f  %10.6f  %10.6f | %5llu  %4llu  %10.2f\n",
               c,
               (unsigned long long)core_stats[c].delay_count,
               (core_stats[c].delay_count?mean:0.0),
               (core_stats[c].delay_count?core_stats[c].min_delay:0.0),
               (core_stats[c].delay_count?core_stats[c].max_delay:0.0),
               (unsigned long long)core_stats[c].jobs,
               (unsigned long long)core_stats[c].misses,
               mr);
    }

    // per-task + global
    int total_jobs = 0, total_misses = 0;
    printf("\nPer-task summary:\n");
    printf("task | jobs  miss  miss_rate(%%)\n");
    for (int i = 0; i < NUM_TASKS; ++i) {
        total_jobs   += job_counts[i];
        total_misses += deadline_miss[i];
        double mr = job_counts[i] ? (100.0 * (double)deadline_miss[i] / (double)job_counts[i]) : 0.0;
        printf("%4d | %4d  %4d  %10.2f\n", i, job_counts[i], deadline_miss[i], mr);
    }
    double global_miss_rate = total_jobs ? (100.0 * (double)total_misses / (double)total_jobs) : 0.0;
    printf("\nGlobal miss rate: %.2f%%  (misses=%d / jobs=%d)\n", global_miss_rate, total_misses, total_jobs);

    // CSV
    for (int i = 0; i < NUM_TASKS; ++i) {
        char fname[64]; snprintf(fname, sizeof(fname), "task_%d_delays.csv", i);
        FILE* f = fopen(fname, "w");
        if (!f) { perror("fopen"); continue; }
        fprintf(f, "period_idx,delay_ratio,missed\n");
        for (size_t k = 0; k < task_logs[i].count; ++k) {
            TaskLogEntry e = task_logs[i].entries[k];
            fprintf(f, "%d,%.6f,%d\n", e.period_idx, e.delay_ratio, e.missed);
        }
        fclose(f);
        free(task_logs[i].entries);
    }
    for (int c = 0; c < NUM_CORES; ++c) pthread_mutex_destroy(&core_mutex[c]);
    return 0;
}
"""



# ------------------------- Generator Entrypoint -------------------------
def generate_c_file(taskset, output_path="generated_taskset.c"):
    template = Template(c_template)
    code = template.render(taskset=taskset)
    with open(output_path, "w") as f:
        f.write(code)
    print(f"✅ C code written to {output_path}")

if __name__ == "__main__":
    M = 16
    N = 16
    wcet_min = 200
    wcet_max = 500
    Cr = 1 # 共享片段占比
    RaF_max = 200
    FN = 10
    contention = 1 # 70% 选择共享API（即有竞争）

    random.seed(int(time.time()))
    np.random.seed(int(time.time()))
    taskset = generate_taskset(M, N, wcet_min, wcet_max, Cr, RaF_max, FN, contention)
    generate_c_file(taskset)