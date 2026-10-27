/*
 * 区块与区块存储
 *
 * 区块（mcc_chunk）是一根 16×16 的区段柱，空气区段不分配（NULL）。
 * 区块存储（mcc_chunk_store）是一组区块：世界某个维度里一个区域拥有的区块，
 * 或一个子世界（Sable 地块）自己的方块。物理直接从这里读方块，不再有副本。
 *
 * 写方块走 mcc_store_set：同一次调用里更新状态、物理占用位图（含六个邻居的外露判断），
 * 然后通知监听者（物理据此唤醒刚体）。区块加入/移出存储时同样维护边界上的外露判断。
 *
 * 线程：一个存储只由拥有它的区域线程写；物理步进期间没有写入，可并发读。
 */
#ifndef MCC_CHUNK_H
#define MCC_CHUNK_H

#include "section.h"
#include "../core/hmap.h"

#define MCC_CHUNK_MAX_SECTIONS 256
#define MCC_STATE_NONE UINT32_MAX

typedef struct mcc_chunk_store mcc_chunk_store;

typedef struct mcc_chunk {
    int32_t cx, cz;
    int32_t min_sy, nsec;             /* 区段 y 范围 [min_sy, min_sy + nsec) */
    mcc_chunk_store *store;           /* 已加入的存储，未加入为 NULL */
    uint64_t solid_secs[MCC_CHUNK_MAX_SECTIONS / 64];   /* 第 i 位：第 i 个区段 solid_count > 0 */
    uint64_t liquid_secs[MCC_CHUNK_MAX_SECTIONS / 64];
    mcc_chunk_section *sec[];
} mcc_chunk;

typedef struct mcc_store_listener {
    void *ctx;
    void (*chunk_added)(void *ctx, mcc_chunk *c);      /* 加入并算好位图之后 */
    void (*chunk_removed)(void *ctx, mcc_chunk *c);    /* 移出之前 */
    void (*block_changed)(void *ctx, int32_t x, int32_t y, int32_t z, mcc_state_t old, mcc_state_t now);
    struct mcc_store_listener *next;
} mcc_store_listener;

struct mcc_chunk_store {
    int32_t min_sy, nsec;
    mcc_hmap chunks;                  /* mcc_key2(cx, cz) → mcc_chunk* */
    mcc_store_listener *listeners;
};

/* ---- 存储 ---- */
/* min_y、height 为方块单位，须是 16 的倍数 */
void mcc_store_init(mcc_chunk_store *st, int32_t min_y, int32_t height);
/* 释放存储及其中全部区块（不通知监听者） */
void mcc_store_free(mcc_chunk_store *st);
void mcc_store_listen(mcc_chunk_store *st, mcc_store_listener *l);
void mcc_store_unlisten(mcc_chunk_store *st, mcc_store_listener *l);

/* ---- 区块 ---- */
mcc_chunk *mcc_chunk_new(const mcc_chunk_store *st, int32_t cx, int32_t cz);
void mcc_chunk_free(mcc_chunk *c);
/* 区块内读写（lx、lz 为 0~15，y 为方块 y）。raw 写不维护位图，只用于加入存储之前填充 */
mcc_state_t mcc_chunk_get(const mcc_chunk *c, int lx, int32_t y, int lz);
void mcc_chunk_set_raw(mcc_chunk *c, int lx, int32_t y, int lz, mcc_state_t state);

/* 加入存储：计算位图、更新四邻边界、通知监听者。同坐标已有区块时致命错误 */
void mcc_store_add_chunk(mcc_chunk_store *st, mcc_chunk *c);
/* 移出存储并返回（调用方释放），不存在返回 NULL */
mcc_chunk *mcc_store_remove_chunk(mcc_chunk_store *st, int32_t cx, int32_t cz);

MCC_INLINE mcc_chunk *mcc_store_chunk(const mcc_chunk_store *st, int32_t cx, int32_t cz) {
    return (mcc_chunk *)mcc_hmap_get(&st->chunks, mcc_key2(cx, cz));
}
MCC_INLINE const mcc_chunk_section *mcc_chunk_section_at(const mcc_chunk *c, int32_t sy) {
    uint32_t i = (uint32_t)(sy - c->min_sy);
    return i < (uint32_t)c->nsec ? c->sec[i] : NULL;
}

/* ---- 方块读写（世界坐标） ---- */
mcc_state_t mcc_store_get(const mcc_chunk_store *st, int32_t x, int32_t y, int32_t z);
/* 返回旧状态；区块未加载或 y 越界时什么也不做，返回 MCC_STATE_NONE */
mcc_state_t mcc_store_set(mcc_chunk_store *st, int32_t x, int32_t y, int32_t z, mcc_state_t state);

/* ---- 读游标：缓存上一次命中的区块，供物理等热路径连续查询 ---- */
typedef struct mcc_store_cursor {
    const mcc_chunk_store *store;
    uint64_t key;
    const mcc_chunk *chunk;
} mcc_store_cursor;

MCC_INLINE void mcc_cursor_init(mcc_store_cursor *cur, const mcc_chunk_store *st) {
    cur->store = st;
    cur->key = UINT64_MAX;
    cur->chunk = NULL;
}

MCC_INLINE const mcc_chunk *mcc_cursor_chunk(mcc_store_cursor *cur, int32_t cx, int32_t cz) {
    uint64_t key = mcc_key2(cx, cz);
    if (key != cur->key) {
        cur->key = key;
        cur->chunk = mcc_store_chunk(cur->store, cx, cz);
    }
    return cur->chunk;
}

MCC_INLINE const mcc_chunk_section *mcc_cursor_section(mcc_store_cursor *cur, int32_t sx, int32_t sy, int32_t sz) {
    const mcc_chunk *c = mcc_cursor_chunk(cur, sx, sz);
    return c ? mcc_chunk_section_at(c, sy) : NULL;
}

/* 区块未加载或越界的位置按空气（状态 0）处理 */
MCC_INLINE mcc_state_t mcc_cursor_get(mcc_store_cursor *cur, int32_t x, int32_t y, int32_t z) {
    const mcc_chunk_section *s = mcc_cursor_section(cur, x >> 4, y >> 4, z >> 4);
    return s ? mcc_section_get(s, mcc_section_index(x & 15, y & 15, z & 15)) : 0;
}

/* 物理占用查询：在方块包围盒 [mn, mx]（含端点）内 */
enum mcc_occ_kind { MCC_OCC_SOLID = 0, MCC_OCC_LIQUID = 1 };
typedef MCC_VEC(int32_t) mcc_i32vec;
/* 是否有任何置位 */
bool mcc_cursor_box_any(mcc_store_cursor *cur, int kind, const int32_t mn[3], const int32_t mx[3]);
/* 把所有置位方块的坐标（x, y, z 三个一组）追加到 out */
void mcc_cursor_box_collect(mcc_store_cursor *cur, int kind, const int32_t mn[3], const int32_t mx[3], mcc_i32vec *out);

/* 统计 */
typedef struct mcc_store_stats {
    size_t chunks, sections, phys_sections;
    size_t memory;           /* 区段数据 + 位图字节数 */
} mcc_store_stats;
void mcc_store_stats_get(const mcc_chunk_store *st, mcc_store_stats *out);

/* 自检：用逐方块的定义重算全部位图并比较（测试用），不一致时写入 err 返回 false */
bool mcc_store_verify(const mcc_chunk_store *st, char *err, size_t errlen);

#endif
