#include "cmap.h"
#include "ebr.h"

static mcc_cmap_table *table_new(size_t cap) {
    size_t n = 16;
    while (n < cap) n <<= 1;
    mcc_cmap_table *t = (mcc_cmap_table*)mcc_xaligned(MCC_CACHELINE, sizeof(mcc_cmap_table) + n * sizeof(mcc_cmap_entry));
    t->mask = n - 1;
    t->used = 0;
    t->live = 0;
    for (size_t i = 0; i < n; i++) {
        atomic_init(&t->e[i].key, MCC_CMAP_EMPTY_KEY);
        atomic_init(&t->e[i].val, NULL);
    }
    return t;
}

void mcc_cmap_init(mcc_cmap *m, size_t initial_cap) {
    atomic_init(&m->table, table_new(initial_cap * 2));
}

void mcc_cmap_destroy(mcc_cmap *m) {
    free(atomic_load(&m->table));
    atomic_store(&m->table, NULL);
}

static void raw_insert(mcc_cmap_table *t, uint64_t key, void *val) {
    size_t i = mcc_hash64(key) & t->mask;
    while (atomic_load_explicit(&t->e[i].key, memory_order_relaxed) != MCC_CMAP_EMPTY_KEY)
        i = (i + 1) & t->mask;
    atomic_store_explicit(&t->e[i].val, val, memory_order_relaxed);
    atomic_store_explicit(&t->e[i].key, key, memory_order_relaxed);
    t->used++;
    t->live++;
}

/* 重建：丢弃删除留下的空值槽，按有效条目数决定新容量 */
static void rebuild(mcc_cmap *m, mcc_cmap_table *old) {
    mcc_cmap_table *nt = table_new((old->live + 1) * 4);
    for (size_t i = 0; i <= old->mask; i++) {
        uint64_t k = atomic_load_explicit(&old->e[i].key, memory_order_relaxed);
        void *v = atomic_load_explicit(&old->e[i].val, memory_order_relaxed);
        if (k != MCC_CMAP_EMPTY_KEY && v) raw_insert(nt, k, v);
    }
    /* release 发布：读者看到新表指针时一定能看到表内容 */
    atomic_store_explicit(&m->table, nt, memory_order_release);
    mcc_ebr_retire(old, free);
}

void mcc_cmap_put(mcc_cmap *m, uint64_t key, void *val) {
    mcc_cmap_table *t = atomic_load_explicit(&m->table, memory_order_relaxed);
    size_t i = mcc_hash64(key) & t->mask;
    for (;;) {
        uint64_t k = atomic_load_explicit(&t->e[i].key, memory_order_relaxed);
        if (k == key) {
            void *prev = atomic_load_explicit(&t->e[i].val, memory_order_relaxed);
            atomic_store_explicit(&t->e[i].val, val, memory_order_release);
            if (!prev && val) t->live++;
            else if (prev && !val) t->live--;
            return;
        }
        if (k == MCC_CMAP_EMPTY_KEY) break;
        i = (i + 1) & t->mask;
    }
    if (!val) return;
    if ((t->used + 1) * 2 > t->mask + 1) {
        rebuild(m, t);
        t = atomic_load_explicit(&m->table, memory_order_relaxed);
        i = mcc_hash64(key) & t->mask;
        while (atomic_load_explicit(&t->e[i].key, memory_order_relaxed) != MCC_CMAP_EMPTY_KEY)
            i = (i + 1) & t->mask;
    }
    /* 先写值再发布键，读者看到键时值已就绪 */
    atomic_store_explicit(&t->e[i].val, val, memory_order_relaxed);
    atomic_store_explicit(&t->e[i].key, key, memory_order_release);
    t->used++;
    t->live++;
}

void *mcc_cmap_remove(mcc_cmap *m, uint64_t key) {
    mcc_cmap_table *t = atomic_load_explicit(&m->table, memory_order_relaxed);
    size_t i = mcc_hash64(key) & t->mask;
    for (;;) {
        uint64_t k = atomic_load_explicit(&t->e[i].key, memory_order_relaxed);
        if (k == MCC_CMAP_EMPTY_KEY) return NULL;
        if (k == key) {
            void *prev = atomic_load_explicit(&t->e[i].val, memory_order_relaxed);
            if (prev) {
                atomic_store_explicit(&t->e[i].val, NULL, memory_order_release);
                t->live--;
                /* 删除留下的槽过多时整表重建 */
                if (t->used > 64 && t->live * 4 < t->used) rebuild(m, t);
            }
            return prev;
        }
        i = (i + 1) & t->mask;
    }
}

size_t mcc_cmap_size(mcc_cmap *m) {
    return atomic_load_explicit(&m->table, memory_order_relaxed)->live;
}
