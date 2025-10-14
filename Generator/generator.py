
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

# c脚本
c_template = """
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <ctype.h>

//#include "LinuxAPI/linuxAPI_lib.h"
//#include "Taclebench/bench_lib.h"
#include "linuxAPI_lib.h"
#include "bench_lib.h"

#define NUM_TASKS {{ taskset.tasks|length }}
#define NUM_CORES {{ taskset.meta.M }}
#define RUN_DURATION_SEC 1 // 统一运行 10 秒

typedef struct {
    int task_id;
    int core_id;
    int period_us;   // 周期
    int wcet_us;     // 理论 wcet（作为 delay 分母）
    int segment_count;
    void (**segments)(void);
} TaskArgs;

pthread_t threads[NUM_TASKS];
TaskArgs task_args[NUM_TASKS];

// 启停与同步
static pthread_barrier_t start_barrier;
static volatile uint64_t global_start_us = 0;
static volatile uint64_t global_end_us   = 0;

// （保留）线程壁钟时间
uint64_t start_times[NUM_TASKS];
uint64_t end_times[NUM_TASKS];

// ---------- 时间工具 ----------
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

// ---------- 任务级日志（N 个文件，每任务一个） ----------
typedef struct {
    int period_idx;
    double delay_ratio;
} TaskLogEntry;

typedef struct {
    TaskLogEntry* entries;
    size_t capacity;
    size_t count;
} TaskLog;

static TaskLog task_logs[NUM_TASKS];

// 动态扩容
static int ensure_task_capacity(TaskLog* tl, size_t need) {
    if (tl->capacity >= need) return 1;
    size_t newcap = tl->capacity ? (tl->capacity * 2) : 4096; // 初始 4K 条
    while (newcap < need) newcap *= 2;
    TaskLogEntry* p = (TaskLogEntry*)realloc(tl->entries, newcap * sizeof(TaskLogEntry));
    if (!p) return 0;
    tl->entries = p;
    tl->capacity = newcap;
    return 1;
}

// ---------- 每核聚合统计（仅用于打印） ----------
typedef struct {
    double sum;
    double min;
    double max;
    uint64_t count;
} CoreAgg;

static CoreAgg core_stats[NUM_CORES];
static pthread_mutex_t core_mutex[NUM_CORES];

// ---------- 任务线程 ----------
void* task_function(void* arg) {
    TaskArgs* task = (TaskArgs*)arg;

    // 实时优先级 & 绑定核心（失败也继续）
    struct sched_param param; param.sched_priority = 10;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        perror("pthread_setschedparam failed");
    }
    cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(task->core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    // 等待统一开始
    pthread_barrier_wait(&start_barrier);

    start_times[task->task_id] = now_us();

    uint64_t next_release = global_start_us;
    uint64_t now = now_us();
    int period_idx = 0;

    while (now < global_end_us) {
        uint64_t job_start = now_us();
        for (int i = 0; i < task->segment_count; ++i) {
            task->segments[i]();
        }
        uint64_t job_end = now_us();

        uint64_t actual_us = job_end - job_start;
        double delay_ratio = (task->wcet_us > 0) ? ((double)actual_us / (double)task->wcet_us) : 0.0;

        // 记录到该任务的内存日志（无需加锁：每个任务仅由自己线程写）
        TaskLog* tl = &task_logs[task->task_id];
        if (ensure_task_capacity(tl, tl->count + 1)) {
            TaskLogEntry* e = &tl->entries[tl->count++];
            e->period_idx = period_idx;
            e->delay_ratio = delay_ratio;
        }

        // 同时更新“该核心”的聚合统计（多任务同核，需互斥）
        int c = task->core_id;
        pthread_mutex_lock(&core_mutex[c]);
        CoreAgg* cs = &core_stats[c];
        cs->sum += delay_ratio;
        cs->count += 1;
        if (delay_ratio < cs->min) cs->min = delay_ratio;
        if (delay_ratio > cs->max) cs->max = delay_ratio;
        pthread_mutex_unlock(&core_mutex[c]);

        period_idx++;

        // 下一周期（绝对时间）
        next_release += (uint64_t)task->period_us;
        now = now_us();
        if (now < next_release) {
            sleep_until_us(next_release);
            now = now_us();
        } else {
            // 超期：对齐到当前，避免漂移积累
            next_release = now;
        }
    }

    end_times[task->task_id] = now_us();
    return NULL;
}

// ----------- Jinja: 每个任务的 segments 表 -----------
{% for task in taskset.tasks %}
static void (*segments_{{ task.id }}[])() = {
    {% for seg in task.segments %}
        {% for api in seg.apis %}
    {{ api }},
        {% endfor %}
    {% endfor %}
};
{% endfor %}

// ----------- main -----------
int main() {
    printf("Running %d periodic tasks across %d cores for %d seconds...\\n",
           NUM_TASKS, NUM_CORES, RUN_DURATION_SEC);

    // 初始化任务日志
    for (int i = 0; i < NUM_TASKS; ++i) {
        task_logs[i].entries = NULL;
        task_logs[i].capacity = 0;
        task_logs[i].count = 0;
    }
    // 初始化每核聚合统计与互斥
    for (int c = 0; c < NUM_CORES; ++c) {
        core_stats[c].sum = 0.0;
        core_stats[c].min = 1e300;
        core_stats[c].max = -1e300;
        core_stats[c].count = 0;
        pthread_mutex_init(&core_mutex[c], NULL);
    }

    // 填充任务参数
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

    // 栅栏同步（+1 给主线程）
    pthread_barrier_init(&start_barrier, NULL, NUM_TASKS + 1);

    // 统一起止时间
    uint64_t t0 = now_us();
    global_start_us = t0 + 100000ULL; // +100ms
    global_end_us   = global_start_us + (uint64_t)RUN_DURATION_SEC * 1000000ULL;

    // 启动线程
    for (int i = 0; i < NUM_TASKS; i++) {
        if (pthread_create(&threads[i], NULL, task_function, &task_args[i]) != 0) {
            perror("pthread_create failed");
            return 1;
        }
    }

    // 一起放行
    pthread_barrier_wait(&start_barrier);

    // 等待结束
    for (int i = 0; i < NUM_TASKS; i++) pthread_join(threads[i], NULL);

    // ---- 打印每核聚合统计 + 列出该核心任务的理论 wcet ----
    printf("\\nPer-core delay ratio stats (actual / wcet):\\n");
    printf("Core |  count     mean         min          max   | tasks_on_core (task_id:wcet_us)\\n");
    printf("-----|------------------------------------------------|----------------------------------\\n");
    for (int c = 0; c < NUM_CORES; ++c) {
        double mean = (core_stats[c].count > 0) ? (core_stats[c].sum / (double)core_stats[c].count) : 0.0;

        // 打印一行统计
        printf("%4d | %7llu  %11.6f  %11.6f  %11.6f | ",
               c,
               (unsigned long long)core_stats[c].count,
               mean, (core_stats[c].count ? core_stats[c].min : 0.0),
               (core_stats[c].count ? core_stats[c].max : 0.0));

        // 列出该核心上的任务及其 wcet_us（理论）
        int first = 1;
        for (int i = 0; i < NUM_TASKS; ++i) {
            if (task_args[i].core_id == c) {
                if (!first) printf(", ");
                printf("%d:%d", task_args[i].task_id, task_args[i].wcet_us);
                first = 0;
            }
        }
        printf("\\n");
    }

    // ---- 输出 N 个文件：task_<id>_delays.csv（period_idx,delay_ratio）----
    for (int i = 0; i < NUM_TASKS; ++i) {
        char fname[64];
        snprintf(fname, sizeof(fname), "task_%d_delays.csv", i);
        FILE* f = fopen(fname, "w");
        if (!f) {
            perror("fopen task log");
            continue;
        }
        // 只写每次任务的延迟数据（period_idx, delay_ratio）
        fprintf(f, "period_idx,delay_ratio\\n");
        TaskLog* tl = &task_logs[i];
        for (size_t k = 0; k < tl->count; ++k) {
            TaskLogEntry* e = &tl->entries[k];
            fprintf(f, "%d,%.6f\\n", e->period_idx, e->delay_ratio);
        }
        fclose(f);
    }

    // 资源回收
    for (int i = 0; i < NUM_TASKS; ++i) free(task_logs[i].entries);
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
    M = 1
    N = 1
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