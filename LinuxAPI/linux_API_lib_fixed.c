#include <semaphore.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <stdalign.h>

#define MAX_CPU 16

static sem_t sem_array[MAX_CPU];      // 每核一个信号量
static sem_t sem_shared;              // 全局共享信号量

// 一次性初始化
static pthread_once_t once_shared = PTHREAD_ONCE_INIT;
static pthread_once_t once_array  = PTHREAD_ONCE_INIT;

static void do_init_shared(void){ sem_init(&sem_shared, 0, 1); }
static void do_init_array(void){ for (int i = 0; i < MAX_CPU; ++i) sem_init(&sem_array[i], 0, 1); }

// 每核独立的 sink，避免伪共享
typedef struct { alignas(128) int v; char pad[128 - sizeof(int)]; } padded_int;
static padded_int sink_array[MAX_CPU];

static volatile int global_sink = 0;  // 仅供“共享版本”使用，保留热点

static inline void sem_wait_retry(sem_t* s){
    while (sem_wait(s) == -1 && errno == EINTR) {}
}

// 共享版本：所有线程用同一个信号量（极限竞争）
void API_fragment0(void) {
    pthread_once(&once_shared, do_init_shared);

    for (int i = 0; i < 100; ++i) {
        sem_wait_retry(&sem_shared);
        int local = global_sink;
        global_sink = local + 1;
        sem_post(&sem_shared);
    }
}

// 并行版本：每个线程依据 CPU 号使用不同信号量 + 每核独立 sink
void API_para_fragment0(void) {
    pthread_once(&once_array, do_init_array);

    int cpu_id = sched_getcpu();
    int idx = (cpu_id < 0) ? 0 : (cpu_id % MAX_CPU);

    sem_t* my_sem = &sem_array[idx];

    for (int i = 0; i < 100; ++i) {
        sem_wait_retry(my_sem);
        int local = sink_array[idx].v;
        sink_array[idx].v = local + 1;
        sem_post(my_sem);
    }
}