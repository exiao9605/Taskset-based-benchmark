#include <semaphore.h>
#include <stdio.h>
#include <sched.h>
#include <stdatomic.h>

#define MAX_CPU 16

static sem_t sem_array[MAX_CPU];      // 每核一个信号量
static sem_t sem_shared;              // 全局共享信号量

static atomic_int initialized_shared = 0;
static atomic_int initialized_array = 0;

volatile int dummy_sink = 0;

// 共享版本：所有线程用同一个信号量
void API_fragment0(void) {
    if (atomic_exchange(&initialized_shared, 1) == 0) {
        sem_init(&sem_shared, 0, 1);
    }

    for (int i = 0; i < 100; ++i) {
        sem_wait(&sem_shared);
        int local = dummy_sink;
        dummy_sink = local + 1;
        sem_post(&sem_shared);
    }
}

// 私有版本：每个线程根据 CPU 编号选择一个信号量
void API_para_fragment0(void) {
    if (atomic_exchange(&initialized_array, 1) == 0) {
        for (int i = 0; i < MAX_CPU; ++i) {
            sem_init(&sem_array[i], 0, 1);
        }
    }

    int cpu_id = sched_getcpu();
    if (cpu_id < 0 || cpu_id >= MAX_CPU) {
        cpu_id = 0;  // fallback
    }

    sem_t* my_sem = &sem_array[cpu_id];

    for (int i = 0; i < 100; ++i) {
        sem_wait(my_sem);
        int local = dummy_sink;
        dummy_sink = local + 1;
        sem_post(my_sem);
    }
}