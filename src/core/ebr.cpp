#include "ebr.h"
#include "log.h"
#include <pthread.h>

#define EBR_INACTIVE UINT64_MAX

typedef struct {
    _Atomic(uint64_t) epoch;   /* 当前所在纪元，EBR_INACTIVE 表示不在临界区 */
    int depth;                /* 只由本线程访问，支持嵌套进入 */
    char pad[MCC_CACHELINE - sizeof(uint64_t) - sizeof(int)];
} ebr_slot;

typedef struct retired {
    struct retired *next;
    void *p;
    void (*free_fn)(void *);
    uint64_t epoch;
} retired;

static ebr_slot g_slots[MCC_EBR_MAX_THREADS] MCC_ALIGNED;
static _Atomic(int) g_slot_count;
static _Atomic(uint64_t) g_epoch = 1;
static pthread_mutex_t g_retire_lock = PTHREAD_MUTEX_INITIALIZER;
static retired *g_retired;
static thread_local int t_slot = -1;

static ebr_slot *my_slot(void) {
    if (MCC_UNLIKELY(t_slot < 0)) {
        int s = atomic_fetch_add(&g_slot_count, 1);
        if (s >= MCC_EBR_MAX_THREADS) mcc_log_fatal("纪元回收线程槽位耗尽");
        atomic_store(&g_slots[s].epoch, EBR_INACTIVE);
        t_slot = s;
    }
    return &g_slots[t_slot];
}

void mcc_ebr_enter(void) {
    ebr_slot *s = my_slot();
    if (s->depth++ == 0) {
        /* seq_cst 保证纪元公布先于后续对共享结构的读取 */
        atomic_store(&s->epoch, atomic_load(&g_epoch));
        atomic_thread_fence(memory_order_seq_cst);
    }
}

void mcc_ebr_exit(void) {
    ebr_slot *s = my_slot();
    if (--s->depth == 0)
        atomic_store_explicit(&s->epoch, EBR_INACTIVE, memory_order_release);
}

void mcc_ebr_retire(void *p, void (*free_fn)(void *)) {
    if (!p) return;
    retired *r = (retired*)mcc_xmalloc(sizeof *r);
    r->p = p;
    r->free_fn = free_fn;
    pthread_mutex_lock(&g_retire_lock);
    r->epoch = atomic_fetch_add(&g_epoch, 1);
    r->next = g_retired;
    g_retired = r;
    pthread_mutex_unlock(&g_retire_lock);
}

size_t mcc_ebr_reclaim(void) {
    /* 先摘下退役链表（这建立了"摘链 先于 退役 先于 本次扫描"的顺序），
     * 再用全序栅栏扫描读者纪元，避免读者公布纪元与回收扫描互相错过 */
    pthread_mutex_lock(&g_retire_lock);
    retired *list = g_retired;
    g_retired = NULL;
    pthread_mutex_unlock(&g_retire_lock);
    if (!list) return 0;

    atomic_thread_fence(memory_order_seq_cst);
    uint64_t min = EBR_INACTIVE;
    int n = atomic_load(&g_slot_count);
    for (int i = 0; i < n; i++) {
        uint64_t e = atomic_load(&g_slots[i].epoch);
        if (e < min) min = e;
    }

    retired *freeable = NULL, *keep = NULL, *keep_tail = NULL;
    size_t kept = 0;
    while (list) {
        retired *r = list;
        list = r->next;
        /* 读者进入时看到的纪元若大于退役纪元，就不可能再持有该对象 */
        if (r->epoch < min) {
            r->next = freeable;
            freeable = r;
        } else {
            r->next = keep;
            if (!keep) keep_tail = r;
            keep = r;
            kept++;
        }
    }
    pthread_mutex_lock(&g_retire_lock);
    if (keep) {
        keep_tail->next = g_retired;
        g_retired = keep;
    }
    pthread_mutex_unlock(&g_retire_lock);
    (void)kept;
    size_t cnt = 0;
    while (freeable) {
        retired *r = freeable;
        freeable = r->next;
        r->free_fn(r->p);
        free(r);
        cnt++;
    }
    return cnt;
}

void mcc_ebr_drain(void) {
    pthread_mutex_lock(&g_retire_lock);
    retired *r = g_retired;
    g_retired = NULL;
    pthread_mutex_unlock(&g_retire_lock);
    while (r) {
        retired *n = r->next;
        r->free_fn(r->p);
        free(r);
        r = n;
    }
}
