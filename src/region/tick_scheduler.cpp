#include "tick_scheduler.h"
#include "../core/ebr.h"
#include "../core/log.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

typedef struct {
    uint64_t due;
    mcc_region *r;
} heap_ent;

struct mcc_tick_scheduler {
    mcc_regionizer *rz;
    mcc_tick_config cfg;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    heap_ent *heap;
    size_t len, cap;
    pthread_t *threads;
    _Atomic(bool) stop;
    _Atomic(uint64_t) ticks, skipped, max_tick;
};

/* ---------- 小顶堆 ---------- */

static void heap_push(mcc_tick_scheduler *s, heap_ent e) {
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->heap = (heap_ent*)mcc_xrealloc(s->heap, s->cap * sizeof(heap_ent));
    }
    size_t i = s->len++;
    while (i) {
        size_t p = (i - 1) / 2;
        if (s->heap[p].due <= e.due) break;
        s->heap[i] = s->heap[p];
        i = p;
    }
    s->heap[i] = e;
}

static heap_ent heap_pop(mcc_tick_scheduler *s) {
    heap_ent top = s->heap[0];
    heap_ent last = s->heap[--s->len];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        uint64_t md = last.due;
        if (l < s->len && s->heap[l].due < md) { m = l; md = s->heap[l].due; }
        if (r < s->len && s->heap[r].due < md) { m = r; }
        if (m == i) break;
        s->heap[i] = s->heap[m];
        i = m;
    }
    if (s->len) s->heap[i] = last;
    return top;
}

static void enqueue(mcc_tick_scheduler *s, mcc_region *r, uint64_t due) {
    pthread_mutex_lock(&s->lock);
    bool earliest = s->len == 0 || due < s->heap[0].due;
    heap_push(s, (heap_ent){ due, r });
    if (earliest) pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

/* 在调度归属标记上抢占成功的一方负责入堆，保证每个区域只入堆一次 */
static void try_enqueue(mcc_tick_scheduler *s, mcc_region *r) {
    bool expect = false;
    if (atomic_compare_exchange_strong(&r->in_schedule, &expect, true)) {
        mcc_region_ref(r); /* 堆条目持有引用 */
        uint64_t now = mcc_nanotime();
        uint64_t due = r->next_tick_start > now ? r->next_tick_start : now;
        enqueue(s, r, due);
    }
}

void mcc_tick_scheduler_on_active(mcc_tick_scheduler *s, mcc_region *r) {
    try_enqueue(s, r);
}

/* ---------- 推进线程 ---------- */

static void abs_deadline(struct timespec *ts, uint64_t due_mono) {
#ifdef _WIN32
    /* Windows 的线程库按系统时钟算截止时间：把单调时钟的到期时间换算过去 */
    uint64_t now = mcc_nanotime();
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    uint64_t real = (uint64_t)rt.tv_sec * 1000000000ULL + (uint64_t)rt.tv_nsec + (due_mono > now ? due_mono - now : 0);
    ts->tv_sec = (time_t)(real / 1000000000ULL);
    ts->tv_nsec = (long)(real % 1000000000ULL);
#else
    ts->tv_sec = (time_t)(due_mono / 1000000000ULL);
    ts->tv_nsec = (long)(due_mono % 1000000000ULL);
#endif
}

static void *tick_thread(void *p) {
    mcc_tick_scheduler *s = (mcc_tick_scheduler*)p;
    uint64_t reclaim_at = 0;
    pthread_mutex_lock(&s->lock);
    while (!atomic_load(&s->stop)) {
        if (s->len == 0) {
            pthread_cond_wait(&s->cond, &s->lock);
            continue;
        }
        uint64_t now = mcc_nanotime();
        if (s->heap[0].due > now) {
            struct timespec ts;
            abs_deadline(&ts, s->heap[0].due);
            pthread_cond_timedwait(&s->cond, &s->lock, &ts);
            continue;
        }
        heap_ent e = heap_pop(s);
        /* 堆里还有到期的，叫醒别的线程一起干 */
        if (s->len && s->heap[0].due <= now) pthread_cond_signal(&s->cond);
        pthread_mutex_unlock(&s->lock);

        mcc_region *r = e.r;
        if (!mcc_regionizer_try_mark_ticking(r)) {
            /* 区域处于过渡态或已死亡：放弃这次调度。
             * 先清除入堆标记再复查状态，与 on_active 的"先置就绪再抢标记"配对，不会漏调度 */
            atomic_store(&r->in_schedule, false);
            if (atomic_load(&r->state) == MCC_REGION_READY) try_enqueue(s, r);
            mcc_region_unref(r);
            pthread_mutex_lock(&s->lock);
            continue;
        }

        uint64_t start = mcc_nanotime();
        s->cfg.tick(s->cfg.ctx, r, start);
        uint64_t end = mcc_nanotime();
        uint64_t took = end - start;
        r->tick_count++;
        r->tick_time_ema_ns = r->tick_time_ema_ns * 0.95 + (double)took * 0.05;
        r->last_tick_ns = took;
        atomic_fetch_add(&s->ticks, 1);
        uint64_t prev = atomic_load(&s->max_tick);
        while (took > prev && !atomic_compare_exchange_weak(&s->max_tick, &prev, took)) {}

        /* 下一刻：基于计划时间而不是实际时间，落后时自然追刻 */
        uint64_t next = r->next_tick_start + s->cfg.tick_interval_ns;
        if (next + s->cfg.max_catchup_ns < end) {
            atomic_fetch_add(&s->skipped, 1);
            next = end;
        }
        r->next_tick_start = next;

        if (mcc_regionizer_release(r)) {
            /* 区域仍存活：保持入堆标记和引用，直接重新入堆 */
            enqueue(s, r, next);
        } else {
            /* 区域死亡或并入别的区域：放下堆引用，内存由纪元回收 */
            mcc_region_unref(r);
        }

        if (end >= reclaim_at) {
            mcc_ebr_reclaim();
            reclaim_at = end + 100000000ULL;
        }
        pthread_mutex_lock(&s->lock);
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

mcc_tick_scheduler *mcc_tick_scheduler_create(mcc_regionizer *rz, const mcc_tick_config *cfg) {
    mcc_tick_scheduler *s = (mcc_tick_scheduler*)mcc_xcalloc(1, sizeof *s);
    s->rz = rz;
    s->cfg = *cfg;
    if (s->cfg.threads < 1) s->cfg.threads = 1;
    if (!s->cfg.tick_interval_ns) s->cfg.tick_interval_ns = 50000000ULL;
    if (!s->cfg.max_catchup_ns) s->cfg.max_catchup_ns = 1000000000ULL;
    pthread_mutex_init(&s->lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
#ifndef _WIN32
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&s->cond, &ca);
    pthread_condattr_destroy(&ca);
    return s;
}

void mcc_tick_scheduler_start(mcc_tick_scheduler *s) {
    atomic_store(&s->stop, false);
    s->threads = (pthread_t*)mcc_xmalloc(sizeof(pthread_t) * (size_t)s->cfg.threads);
    for (int i = 0; i < s->cfg.threads; i++) {
        pthread_create(&s->threads[i], NULL, tick_thread, s);
        char name[32];
        snprintf(name, sizeof name, "mcc-tick-%d", i);
        name[15] = 0; pthread_setname_np(s->threads[i], name);
    }
    mcc_info("区域推进调度器启动：%d 个推进线程", s->cfg.threads);
}

void mcc_tick_scheduler_stop(mcc_tick_scheduler *s) {
    atomic_store(&s->stop, true);
    pthread_mutex_lock(&s->lock);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    if (s->threads) {
        for (int i = 0; i < s->cfg.threads; i++) pthread_join(s->threads[i], NULL);
        free(s->threads);
        s->threads = NULL;
    }
}

void mcc_tick_scheduler_destroy(mcc_tick_scheduler *s) {
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    free(s->heap);
    free(s);
}

void mcc_tick_scheduler_stats(mcc_tick_scheduler *s, mcc_tick_stats *out) {
    out->ticks = atomic_load(&s->ticks);
    out->skipped_catchup = atomic_load(&s->skipped);
    out->max_tick_ns = atomic_load(&s->max_tick);
    pthread_mutex_lock(&s->lock);
    out->queued = s->len;
    pthread_mutex_unlock(&s->lock);
}
