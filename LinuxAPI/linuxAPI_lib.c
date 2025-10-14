// ===== linuxAPI_fragments.h =====
#pragma once
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stdalign.h>
#include <string.h>

#ifndef MAX_CPU
#define MAX_CPU 16
#endif

// ---------- 小工具 ----------
static inline void sem_wait_retry(sem_t* s){
    while (sem_wait(s) == -1 && errno == EINTR) {}
}
static inline int cpu_index_mod(){
    int id = sched_getcpu();
    if (id < 0) id = 0;
    return id % MAX_CPU;
}
typedef struct { alignas(128) int v; char pad[128 - sizeof(int)]; } padded_int;

// ========== 0) Semaphore 版本 ==========
static sem_t sem_shared;
static sem_t sem_array[MAX_CPU];
static pthread_once_t once_sem_shared = PTHREAD_ONCE_INIT;
static pthread_once_t once_sem_array  = PTHREAD_ONCE_INIT;
static void init_sem_shared(void){ sem_init(&sem_shared, 0, 1); }
static void init_sem_array(void){ for (int i=0;i<MAX_CPU;++i) sem_init(&sem_array[i],0,1); }

static volatile int sem_global_sink = 0;
static padded_int  sem_sink_array[MAX_CPU];

// 共享：所有线程同一把信号量
__attribute__((noinline))
void API_fragment0(void){
    pthread_once(&once_sem_shared, init_sem_shared);
    for (int i=0;i<100;++i){
        sem_wait_retry(&sem_shared);
        int x = sem_global_sink;
        sem_global_sink = x + 1;
        sem_post(&sem_shared);
    }
}

// 并行：每核独立信号量
__attribute__((noinline))
void API_para_fragment0(void){
    pthread_once(&once_sem_array, init_sem_array);
    int idx = cpu_index_mod();
    sem_t* s = &sem_array[idx];
    volatile int* p = &sem_sink_array[idx].v;
    for (int i=0;i<100;++i){
        sem_wait_retry(s);
        int x = *p;
        *p = x + 1;
        sem_post(s);
    }
}

// ========== 1) Mutex 版本 ==========
static pthread_mutex_t mtx_shared;
static pthread_mutex_t mtx_array[MAX_CPU];
static pthread_once_t once_mtx_shared = PTHREAD_ONCE_INIT;
static pthread_once_t once_mtx_array  = PTHREAD_ONCE_INIT;
static void init_mtx_shared(void){ pthread_mutex_init(&mtx_shared, NULL); }
static void init_mtx_array(void){ for (int i=0;i<MAX_CPU;++i) pthread_mutex_init(&mtx_array[i], NULL); }

static volatile int mtx_global_sink = 0;
static padded_int  mtx_sink_array[MAX_CPU];

// 共享：单把 mutex
__attribute__((noinline))
void API_fragment1(void){
    pthread_once(&once_mtx_shared, init_mtx_shared);
    for (int i=0;i<100;++i){
        pthread_mutex_lock(&mtx_shared);
        int x = mtx_global_sink;
        mtx_global_sink = x + 1;
        pthread_mutex_unlock(&mtx_shared);
    }
}

// 并行：每核 mutex
__attribute__((noinline))
void API_para_fragment1(void){
    pthread_once(&once_mtx_array, init_mtx_array);
    int idx = cpu_index_mod();
    pthread_mutex_t* m = &mtx_array[idx];
    volatile int* p = &mtx_sink_array[idx].v;
    for (int i=0;i<100;++i){
        pthread_mutex_lock(m);
        int x = *p;
        *p = x + 1;
        pthread_mutex_unlock(m);
    }
}

// ========== 2) DataQueue 版本 ==========
#define QSIZE 1024
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int buf[QSIZE];
    int head, tail, count;
    char pad[64];
} BQueue;

static BQueue g_queue;
static pthread_once_t once_q_shared = PTHREAD_ONCE_INIT;
static void init_q_shared(void){
    pthread_mutex_init(&g_queue.mu, NULL);
    pthread_cond_init(&g_queue.not_empty, NULL);
    pthread_cond_init(&g_queue.not_full, NULL);
    g_queue.head = g_queue.tail = g_queue.count = 0;
    memset(g_queue.buf, 0, sizeof(g_queue.buf));
}

static BQueue q_array[MAX_CPU];
static pthread_once_t once_q_array = PTHREAD_ONCE_INIT;
static void init_q_array(void){
    for (int i=0;i<MAX_CPU;++i){
        pthread_mutex_init(&q_array[i].mu, NULL);
        pthread_cond_init(&q_array[i].not_empty, NULL);
        pthread_cond_init(&q_array[i].not_full, NULL);
        q_array[i].head = q_array[i].tail = q_array[i].count = 0;
        memset(q_array[i].buf, 0, sizeof(q_array[i].buf));
    }
}

static inline void bq_push(BQueue* q, int v){
    pthread_mutex_lock(&q->mu);
    while (q->count == QSIZE) pthread_cond_wait(&q->not_full, &q->mu);
    q->buf[q->tail] = v;
    q->tail = (q->tail + 1) % QSIZE;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}
static inline int bq_pop(BQueue* q){
    pthread_mutex_lock(&q->mu);
    while (q->count == 0) pthread_cond_wait(&q->not_empty, &q->mu);
    int v = q->buf[q->head];
    q->head = (q->head + 1) % QSIZE;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return v;
}

// 共享：全局队列
__attribute__((noinline))
void API_fragment2(void){
    pthread_once(&once_q_shared, init_q_shared);
    for (int i=0;i<50;++i){
        bq_push(&g_queue, i);
        bq_push(&g_queue, i+1);
        (void)bq_pop(&g_queue);
        (void)bq_pop(&g_queue);
    }
}

// 并行：每核队列
__attribute__((noinline))
void API_para_fragment2(void){
    pthread_once(&once_q_array, init_q_array);
    int idx = cpu_index_mod();
    BQueue* q = &q_array[idx];
    for (int i=0;i<50;++i){
        bq_push(q, idx ^ i);
        bq_push(q, idx ^ (i+1));
        (void)bq_pop(q);
        (void)bq_pop(q);
    }
}



// ===== 3) Spinlock =====
static pthread_spinlock_t spin_shared;
static pthread_spinlock_t spin_array[MAX_CPU];
static pthread_once_t once_spin_shared = PTHREAD_ONCE_INIT;
static pthread_once_t once_spin_array  = PTHREAD_ONCE_INIT;

static void init_spin_shared(void){ pthread_spin_init(&spin_shared, PTHREAD_PROCESS_PRIVATE); }
static void init_spin_array(void){ for(int i=0;i<MAX_CPU;++i) pthread_spin_init(&spin_array[i], PTHREAD_PROCESS_PRIVATE); }

static volatile int spin_global_sink = 0;
static padded_int  spin_sink_array[MAX_CPU];

__attribute__((noinline))
void API_fragment3(void){ // 共享：单把 spinlock
    pthread_once(&once_spin_shared, init_spin_shared);
    for (int i=0;i<100;++i){
        pthread_spin_lock(&spin_shared);
        int x = spin_global_sink;
        spin_global_sink = x + 1;
        pthread_spin_unlock(&spin_shared);
    }
}

__attribute__((noinline))
void API_para_fragment3(void){ // 并行：每核 spinlock
    pthread_once(&once_spin_array, init_spin_array);
    int idx = cpu_index_mod();
    pthread_spinlock_t* l = &spin_array[idx];
    volatile int* p = &spin_sink_array[idx].v;
    for (int i=0;i<100;++i){
        pthread_spin_lock(l);
        int x = *p;
        *p = x + 1;
        pthread_spin_unlock(l);
    }
}


// ===== 4) RWLock (read-dominant) =====
static pthread_rwlock_t rw_shared;
static pthread_rwlock_t rw_array[MAX_CPU];
static pthread_once_t once_rw_shared = PTHREAD_ONCE_INIT;
static pthread_once_t once_rw_array  = PTHREAD_ONCE_INIT;

static void init_rw_shared(void){ pthread_rwlock_init(&rw_shared, NULL); }
static void init_rw_array(void){ for(int i=0;i<MAX_CPU;++i) pthread_rwlock_init(&rw_array[i], NULL); }

static volatile int rw_global_sink = 0;
static padded_int  rw_sink_array[MAX_CPU];

__attribute__((noinline))
void API_fragment4(void){ // 共享：读锁（多线程下可并发）
    pthread_once(&once_rw_shared, init_rw_shared);
    for (int i=0;i<100;++i){
        pthread_rwlock_rdlock(&rw_shared);
        int x = rw_global_sink; // 仅读
        (void)x;
        pthread_rwlock_unlock(&rw_shared);
    }
}

__attribute__((noinline))
void API_para_fragment4(void){ // 并行：每核读锁
    pthread_once(&once_rw_array, init_rw_array);
    int idx = cpu_index_mod();
    pthread_rwlock_t* rwl = &rw_array[idx];
    volatile int* p = &rw_sink_array[idx].v;
    for (int i=0;i<100;++i){
        pthread_rwlock_rdlock(rwl);
        int x = *p; // 仅读
        (void)x;
        pthread_rwlock_unlock(rwl);
    }
}


// ===== 5) eventfd ping-pong =====
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/syscall.h>

static int efd_shared = -1;
static int efd_array[MAX_CPU];
static pthread_once_t once_efd_shared = PTHREAD_ONCE_INIT;
static pthread_once_t once_efd_array  = PTHREAD_ONCE_INIT;

static void init_efd_shared(void){
    efd_shared = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
}
static void init_efd_array(void){
    for(int i=0;i<MAX_CPU;++i) efd_array[i] = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
}

__attribute__((noinline))
void API_fragment5(void){ // 共享：同一个 eventfd（内核对象）
    pthread_once(&once_efd_shared, init_efd_shared);
    uint64_t one = 1, val;
    for (int i=0;i<10;++i){
        write(efd_shared, &one, sizeof(one));   // syscall
        read (efd_shared, &val, sizeof(val));   // syscall
    }
}

__attribute__((noinline))
void API_para_fragment5(void){ // 并行：每核独立 eventfd
    pthread_once(&once_efd_array, init_efd_array);
    int idx = cpu_index_mod();
    int fd = efd_array[idx];
    uint64_t one = 1, val;
    for (int i=0;i<10;++i){
        write(fd, &one, sizeof(one));
        read (fd, &val, sizeof(val));
    }
}


// ===== 6) Atomic add (lock-free) =====
static _Atomic int a_global = 0;
static padded_int a_array[MAX_CPU];

__attribute__((noinline))
void API_fragment6(void){ // 共享：同一原子变量
    for (int i=0;i<100;++i){
        atomic_fetch_add_explicit(&a_global, 1, memory_order_seq_cst);
    }
}

__attribute__((noinline))
void API_para_fragment6(void){ // 并行：每核原子变量（其实对“同核”退化为普通写）
    int idx = cpu_index_mod();
    _Atomic int* p = (_Atomic int*)&a_array[idx].v;
    for (int i=0;i<100;++i){
        atomic_fetch_add_explicit(p, 1, memory_order_seq_cst);
    }
}
