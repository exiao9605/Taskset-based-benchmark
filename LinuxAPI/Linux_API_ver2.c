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