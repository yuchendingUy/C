#include "hmap.h"

static void alloc_table(mcc_hmap *m, uint32_t cap) {
    uint32_t n = 16;
    while (n < cap) n <<= 1;
    m->keys = (uint64_t*)mcc_xmalloc(sizeof(uint64_t) * n);
    m->vals = (void**)mcc_xcalloc(n, sizeof(void *));
    m->mask = n - 1;
    m->count = m->used = 0;
}

void mcc_hmap_init(mcc_hmap *m, uint32_t cap) {
    memset(m, 0, sizeof *m);
    if (cap) alloc_table(m, cap * 2);
}

void mcc_hmap_free(mcc_hmap *m) {
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

void mcc_hmap_clear(mcc_hmap *m) {
    if (!m->keys) return;
    memset(m->vals, 0, sizeof(void *) * (m->mask + 1));
    m->count = m->used = 0;
}

static void raw_put(mcc_hmap *m, uint64_t key, void *val) {
    uint32_t i = (uint32_t)mcc_hash64(key) & m->mask;
    while (m->vals[i]) i = (i + 1) & m->mask;
    m->keys[i] = key;
    m->vals[i] = val;
    m->count++;
    m->used++;
}

static void rehash(mcc_hmap *m, uint32_t cap) {
    uint64_t *ok = m->keys;
    void **ov = m->vals;
    uint32_t omask = m->mask;
    alloc_table(m, cap);
    if (ok) {
        for (uint32_t i = 0; i <= omask; i++)
            if (ov[i] && ov[i] != MCC_HMAP_TOMB) raw_put(m, ok[i], ov[i]);
        free(ok);
        free(ov);
    }
}

void mcc_hmap_put(mcc_hmap *m, uint64_t key, void *val) {
    if (!m->keys) alloc_table(m, 16);
    uint32_t i = (uint32_t)mcc_hash64(key) & m->mask;
    int64_t tomb = -1;
    for (;;) {
        void *v = m->vals[i];
        if (!v) break;
        if (v == MCC_HMAP_TOMB) { if (tomb < 0) tomb = i; }
        else if (m->keys[i] == key) { m->vals[i] = val; return; }
        i = (i + 1) & m->mask;
    }
    if (tomb >= 0) {
        m->keys[tomb] = key;
        m->vals[tomb] = val;
        m->count++;
        return;
    }
    if ((m->used + 1) * 4 > (m->mask + 1) * 3) {
        rehash(m, (m->count + 1) * 4);
        raw_put(m, key, val);
        return;
    }
    m->keys[i] = key;
    m->vals[i] = val;
    m->count++;
    m->used++;
}

void *mcc_hmap_remove(mcc_hmap *m, uint64_t key) {
    if (!m->keys) return NULL;
    uint32_t i = (uint32_t)mcc_hash64(key) & m->mask;
    for (;;) {
        void *v = m->vals[i];
        if (!v) return NULL;
        if (v != MCC_HMAP_TOMB && m->keys[i] == key) {
            m->vals[i] = MCC_HMAP_TOMB;
            m->count--;
            return v;
        }
        i = (i + 1) & m->mask;
    }
}
