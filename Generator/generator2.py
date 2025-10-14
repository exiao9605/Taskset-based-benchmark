
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


# ---------------- Jinja2 C 模板（新增 schedulability 指标） ----------------
c_template = r"""
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <stdbool.h>
#include "linuxAPI_lib.h"
#include "bench_lib.h"

#define NUM_TASKS {{ taskset.tasks|length }}
#define RUNTIME_DURATION_SEC 5

typedef struct {
    int task_id;
    int core_id;
    int period_us;
    int wcet_us;
    int segment_count;
    void (**segments)(void);
} TaskArgs;

pthread_t threads[NUM_TASKS];
TaskArgs task_args[NUM_TASKS];
int deadline_miss[NUM_TASKS];
int job_counts[NUM_TASKS];

static inline uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

void* task_function(void* arg) {
    TaskArgs* task = (TaskArgs*)arg;

    struct sched_param param;
    param.sched_priority = 10;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(task->core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    uint64_t start_time = now_us();
    uint64_t end_time = start_time + RUNTIME_DURATION_SEC * 1000000ULL;

    while (now_us() < end_time) {
        uint64_t period_start = now_us();

        for (int i = 0; i < task->segment_count; ++i) {
            task->segments[i]();
        }

        uint64_t exec = now_us() - period_start;
        __sync_fetch_and_add(&job_counts[task->task_id], 1);
        if (exec > (uint64_t)task->period_us) {
            __sync_fetch_and_add(&deadline_miss[task->task_id], 1);
        }

        int64_t slack = task->period_us - (int64_t)exec;
        if (slack > 0) usleep(slack);
    }
    return NULL;
}

{% for task in taskset.tasks %}
static void (*segments_{{ task.id }}[])() = {
{% for seg in task.segments %}{% for api in seg.apis %}    {{ api }},
{% endfor %}{% endfor %}
};
{% endfor %}

int main() {
    for (int i = 0; i < NUM_TASKS; ++i) { deadline_miss[i]=0; job_counts[i]=0; }

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

    for (int i = 0; i < NUM_TASKS; ++i)
        pthread_create(&threads[i], NULL, task_function, &task_args[i]);
    for (int i = 0; i < NUM_TASKS; ++i)
        pthread_join(threads[i], NULL);

    // 聚合每核数据
    int max_core = 0;
    for (int i = 0; i < NUM_TASKS; ++i)
        if (task_args[i].core_id > max_core) max_core = task_args[i].core_id;

    int core_jobs[max_core+1];  for (int c=0;c<=max_core;c++) core_jobs[c]=0;
    int core_miss[max_core+1];  for (int c=0;c<=max_core;c++) core_miss[c]=0;

    int total_jobs = 0, total_misses = 0;
    for (int i = 0; i < NUM_TASKS; ++i) {
        core_jobs[ task_args[i].core_id ] += job_counts[i];
        core_miss[ task_args[i].core_id ] += deadline_miss[i];
        total_jobs   += job_counts[i];
        total_misses += deadline_miss[i];
    }

    printf("\nPer-core miss rate:\n");
    printf("Core | Jobs | Miss | MissRate(%%)\n");
    for (int c = 0; c <= max_core; ++c) {
        double r = core_jobs[c] ? (100.0*core_miss[c]/core_jobs[c]) : 0.0;
        printf("%4d | %4d | %4d | %10.2f\n", c, core_jobs[c], core_miss[c], r);
    }

    // 关键指标：schedulability = 总 miss / 总 job（跨所有核心）
    double schedulability = (total_jobs>0) ? ((double)total_misses/(double)total_jobs) : 0.0;
    printf("\nSchedulability (misses/total jobs): %.6f\n", schedulability);
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
    Cr = 0.5 # 共享片段占比
    RaF_max = 200
    FN = 10
    contention = 1 # 70% 选择共享API（即有竞争）

    random.seed(int(time.time()))
    np.random.seed(int(time.time()))
    taskset = generate_taskset(M, N, wcet_min, wcet_max, Cr, RaF_max, FN, contention)
    generate_c_file(taskset)