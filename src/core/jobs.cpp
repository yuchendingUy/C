#include "jobs.h"
#include "log.h"
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <stdio.h>

typedef struct {
    mcc_job_fn fn;
    void *arg;
    mcc_job_group *group;
} job;

/* 环形双端队列：拥有者在尾部压入/弹出（后进先出，缓存友好），窃取者从头部取 */
typedef struct {
    pthread_spinlock_t lock;
    job *buf;
    size_t cap, head, count;
    char pad[MCC_CACHELINE];
} deque;

static void dq_init(deque *d) {
    pthread_spin_init(&d->lock, PTHREAD_PROCESS_PRIVATE);
    d->cap = 256;
    d->buf = (job*)mcc_xmalloc(d->cap * sizeof(job));
    d->head = d->count = 0;
}

static void dq_push(deque *d, job j) {
    pthread_spin_lock(&d->lock);
    if (d->count == d->cap) {
        job *nb = (job*)mcc_xmalloc(d->cap * 2 * sizeof(job));
        for (size_t i = 0; i < d->count; i++) nb[i] = d->buf[(d->head + i) % d->cap];
        free(d->buf);
        d->buf = nb;
        d->head = 0;
        d->cap *= 2;
    }
    d->buf[(d->head + d->count) % d->cap] = j;
    /* 计数会被窃取者无锁快速探测，写入用原子操作 */
    __atomic_store_n(&d->count, d->count + 1, __ATOMIC_RELAXED);
    pthread_spin_unlock(&d->lock);
}

static bool dq_pop_back(deque *d, job *out) {
    pthread_spin_lock(&d->lock);
    if (!d->count) { pthread_spin_unlock(&d->lock); return false; }
    __atomic_store_n(&d->count, d->count - 1, __ATOMIC_RELAXED);
    *out = d->buf[(d->head + d->count) % d->cap];
    pthread_spin_unlock(&d->lock);
    return true;
}

static bool dq_pop_front(deque *d, job *out) {
    if (!__atomic_load_n(&d->count, __ATOMIC_RELAXED)) return false;
    pthread_spin_lock(&d->lock);
    if (!d->count) { pthread_spin_unlock(&d->lock); return false; }
    *out = d->buf[d->head];
    d->head = (d->head + 1) % d->cap;
    __atomic_store_n(&d->count, d->count - 1, __ATOMIC_RELAXED);
    pthread_spin_unlock(&d->lock);
    return true;
}

static int g_nthreads;
static deque *g_deques;      /* 每个工作线程一个 */
static deque g_global;       /* 外部线程提交的任务 */
static pthread_t *g_threads;
static _Atomic(bool) g_stop;
static _Atomic(int64_t) g_queued;
static pthread_mutex_t g_sleep_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sleep_cond = PTHREAD_COND_INITIALIZER;
static _Atomic(int) g_sleepers;
static thread_local int t_worker = -1;
static thread_local uint64_t t_rng = 0x9E3779B97F4A7C15ULL;

static uint64_t rng_next(void) {
    t_rng ^= t_rng << 13; t_rng ^= t_rng >> 7; t_rng ^= t_rng << 17;
    return t_rng;
}

static bool find_job(job *out) {
    if (t_worker >= 0 && dq_pop_back(&g_deques[t_worker], out)) return true;
    if (dq_pop_front(&g_global, out)) return true;
    if (g_nthreads > 0) {
        size_t start = rng_next() % (size_t)g_nthreads;
        for (int i = 0; i < g_nthreads; i++) {
            size_t v = (start + (size_t)i) % (size_t)g_nthreads;
            if ((int)v == t_worker) continue;
            if (dq_pop_front(&g_deques[v], out)) return true;
        }
    }
    return false;
}

static void run_job(job *j) {
    atomic_fetch_sub_explicit(&g_queued, 1, memory_order_relaxed);
    j->fn(j->arg);
    if (j->group) atomic_fetch_sub_explicit(&j->group->pending, 1, memory_order_release);
}

static void *worker_main(void *p) {
    t_worker = (int)(intptr_t)p;
    t_rng ^= (uint64_t)(t_worker + 1) * 0xBF58476D1CE4E5B9ULL;
    job j;
    while (!atomic_load(&g_stop)) {
        if (find_job(&j)) { run_job(&j); continue; }
        /* 短暂自旋后休眠 */
        bool got = false;
        for (int spin = 0; spin < 64 && !got; spin++) {
            sched_yield();
            got = find_job(&j);
        }
        if (got) { run_job(&j); continue; }
        pthread_mutex_lock(&g_sleep_lock);
        atomic_fetch_add(&g_sleepers, 1);
        while (!atomic_load(&g_stop) && atomic_load(&g_queued) <= 0)
            pthread_cond_wait(&g_sleep_cond, &g_sleep_lock);
        atomic_fetch_sub(&g_sleepers, 1);
        pthread_mutex_unlock(&g_sleep_lock);
    }
    return NULL;
}

void mcc_jobs_start(int nthreads) {
    if (nthreads < 1) nthreads = 1;
    g_nthreads = nthreads;
    g_deques = (deque*)mcc_xaligned(MCC_CACHELINE, sizeof(deque) * (size_t)nthreads);
    for (int i = 0; i < nthreads; i++) dq_init(&g_deques[i]);
    dq_init(&g_global);
    atomic_store(&g_stop, false);
    g_threads = (pthread_t*)mcc_xmalloc(sizeof(pthread_t) * (size_t)nthreads);
    for (int i = 0; i < nthreads; i++) {
        pthread_create(&g_threads[i], NULL, worker_main, (void *)(intptr_t)i);
        char name[32];
        snprintf(name, sizeof name, "mcc-job-%d", i);
        name[15] = 0; pthread_setname_np(g_threads[i], name);
    }
    mcc_info("通用任务线程池启动：%d 个线程", nthreads);
}

void mcc_jobs_stop(void) {
    atomic_store(&g_stop, true);
    pthread_mutex_lock(&g_sleep_lock);
    pthread_cond_broadcast(&g_sleep_cond);
    pthread_mutex_unlock(&g_sleep_lock);
    for (int i = 0; i < g_nthreads; i++) pthread_join(g_threads[i], NULL);
    for (int i = 0; i < g_nthreads; i++) free(g_deques[i].buf);
    free(g_global.buf);
    free(g_deques);
    free(g_threads);
    g_nthreads = 0;
}

int mcc_jobs_thread_count(void) { return g_nthreads; }

void mcc_jobs_submit(mcc_job_group *g, mcc_job_fn fn, void *arg) {
    if (g) atomic_fetch_add_explicit(&g->pending, 1, memory_order_relaxed);
    job j = { fn, arg, g };
    atomic_fetch_add(&g_queued, 1);
    if (t_worker >= 0) dq_push(&g_deques[t_worker], j);
    else dq_push(&g_global, j);
    if (atomic_load(&g_sleepers) > 0) {
        pthread_mutex_lock(&g_sleep_lock);
        pthread_cond_signal(&g_sleep_cond);
        pthread_mutex_unlock(&g_sleep_lock);
    }
}

void mcc_jobs_wait(mcc_job_group *g) {
    job j;
    /* 只让出不睡眠：等的通常是亚毫秒级的物理阶段，而 Windows 的睡眠精度是 1~15ms，
     * 睡一次就可能把整个阶段拖慢一个数量级 */
    while (atomic_load_explicit(&g->pending, memory_order_acquire) > 0) {
        if (find_job(&j)) { run_job(&j); continue; }
        sched_yield();
    }
}

typedef struct {
    mcc_range_fn fn;
    void *arg;
    size_t begin, end;
} range_job;

static void range_trampoline(void *p) {
    range_job *r = (range_job*)p;
    r->fn(r->arg, r->begin, r->end);
}

void mcc_jobs_parallel_for(size_t n, size_t grain, mcc_range_fn fn, void *arg) {
    if (!n) return;
    if (!grain) grain = 1;
    size_t chunks = (n + grain - 1) / grain;
    if (chunks == 1 || g_nthreads == 0) { fn(arg, 0, n); return; }
    range_job stack_jobs[64];
    range_job *jobs = chunks <= 64 ? stack_jobs : (range_job *)mcc_xmalloc(chunks * sizeof(range_job));
    mcc_job_group g;
    mcc_job_group_init(&g);
    for (size_t c = 1; c < chunks; c++) {
        jobs[c] = (range_job){ fn, arg, c * grain, MCC_MIN(n, (c + 1) * grain) };
        mcc_jobs_submit(&g, range_trampoline, &jobs[c]);
    }
    fn(arg, 0, MCC_MIN(n, grain)); /* 第一块自己做 */
    mcc_jobs_wait(&g);
    if (jobs != stack_jobs) free(jobs);
}
