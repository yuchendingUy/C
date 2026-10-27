/*
 * 单线程开放寻址哈希表：64 位键 → 指针。
 * 只给单个线程独占的数据用（例如某个区域的物理场景），不做任何同步。
 */
#ifndef MCC_HMAP_H
#define MCC_HMAP_H

#include "common.h"

typedef struct mcc_hmap {
    uint64_t *keys;
    void **vals;      /* NULL 表示空槽，MCC_HMAP_TOMB 表示已删除 */
    uint32_t mask;
    uint32_t count;   /* 有效条目 */
    uint32_t used;    /* 有效 + 墓碑 */
} mcc_hmap;

#define MCC_HMAP_TOMB ((void *)(uintptr_t)1)

void mcc_hmap_init(mcc_hmap *m, uint32_t cap);
void mcc_hmap_free(mcc_hmap *m);
void mcc_hmap_clear(mcc_hmap *m);
void mcc_hmap_put(mcc_hmap *m, uint64_t key, void *val);   /* val 不能为 NULL */
void *mcc_hmap_remove(mcc_hmap *m, uint64_t key);

MCC_INLINE void *mcc_hmap_get(const mcc_hmap *m, uint64_t key) {
    if (MCC_UNLIKELY(!m->keys)) return NULL;
    uint32_t i = (uint32_t)mcc_hash64(key) & m->mask;
    for (;;) {
        void *v = m->vals[i];
        if (!v) return NULL;
        if (v != MCC_HMAP_TOMB && m->keys[i] == key) return v;
        i = (i + 1) & m->mask;
    }
}

/* 遍历：for (uint32_t it = 0; mcc_hmap_next(m, &it, &k, &v);) */
MCC_INLINE bool mcc_hmap_next(const mcc_hmap *m, uint32_t *it, uint64_t *key, void **val) {
    if (!m->keys) return false;
    while (*it <= m->mask) {
        uint32_t i = (*it)++;
        void *v = m->vals[i];
        if (v && v != MCC_HMAP_TOMB) {
            if (key) *key = m->keys[i];
            if (val) *val = v;
            return true;
        }
    }
    return false;
}

#endif
