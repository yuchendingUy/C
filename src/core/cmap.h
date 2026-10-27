/*
 * 64 位键 → 指针 的并发哈希表：
 *   - 读取完全无锁，任意线程可读（必须处于纪元回收临界区内）
 *   - 写入由调用方在外部锁保护下串行进行
 * 键 MCC_CMAP_EMPTY_KEY（x、z 均为 INT32_MIN 的打包值）保留不可用。
 */
#ifndef MCC_CMAP_H
#define MCC_CMAP_H

#include "common.h"

#define MCC_CMAP_EMPTY_KEY 0x8000000080000000ULL

typedef struct mcc_cmap_entry {
    _Atomic(uint64_t) key;
    _Atomic(void *) val;
} mcc_cmap_entry;

typedef struct mcc_cmap_table {
    size_t mask;
    size_t used;  /* 已占用槽（含删除后保留键的槽），仅写者访问 */
    size_t live;  /* 有效条目数，仅写者访问 */
    mcc_cmap_entry e[];
} mcc_cmap_table;

typedef struct mcc_cmap {
    _Atomic(mcc_cmap_table *) table;
} mcc_cmap;

void mcc_cmap_init(mcc_cmap *m, size_t initial_cap);
void mcc_cmap_destroy(mcc_cmap *m);

/* 读：调用方需处于 mcc_ebr_enter/exit 之间 */
MCC_INLINE void *mcc_cmap_get(mcc_cmap *m, uint64_t key) {
    mcc_cmap_table *t = atomic_load_explicit(&m->table, memory_order_acquire);
    size_t i = mcc_hash64(key) & t->mask;
    for (;;) {
        uint64_t k = atomic_load_explicit(&t->e[i].key, memory_order_acquire);
        if (k == key) return atomic_load_explicit(&t->e[i].val, memory_order_acquire);
        if (k == MCC_CMAP_EMPTY_KEY) return NULL;
        i = (i + 1) & t->mask;
    }
}

/* 写：调用方持有外部写锁 */
void mcc_cmap_put(mcc_cmap *m, uint64_t key, void *val);
void *mcc_cmap_remove(mcc_cmap *m, uint64_t key);
size_t mcc_cmap_size(mcc_cmap *m);

/* 遍历（持有写锁或无并发写时使用） */
#define MCC_CMAP_FOREACH(m, kvar, vvar, body) do { \
        mcc_cmap_table *t__ = atomic_load(&(m)->table); \
        for (size_t i__ = 0; i__ <= t__->mask; i__++) { \
            uint64_t kvar = atomic_load_explicit(&t__->e[i__].key, memory_order_relaxed); \
            void *vvar = atomic_load_explicit(&t__->e[i__].val, memory_order_relaxed); \
            if (kvar == MCC_CMAP_EMPTY_KEY || !vvar) continue; \
            body \
        } \
    } while (0)

#endif
