#include "chunk.h"
#include "../core/log.h"
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* 存储                                                                */
/* ------------------------------------------------------------------ */

void mcc_store_init(mcc_chunk_store *st, int32_t min_y, int32_t height) {
    if ((min_y & 15) || (height & 15) || height <= 0 || height / 16 > MCC_CHUNK_MAX_SECTIONS)
        mcc_log_fatal("区块存储高度非法：min_y=%d height=%d", min_y, height);
    st->min_sy = min_y >> 4;
    st->nsec = height >> 4;
    mcc_hmap_init(&st->chunks, 64);
    st->listeners = NULL;
}

void mcc_store_free(mcc_chunk_store *st) {
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&st->chunks, &it, NULL, &v)) mcc_chunk_free(v);
    mcc_hmap_free(&st->chunks);
    st->listeners = NULL;
}

void mcc_store_listen(mcc_chunk_store *st, mcc_store_listener *l) {
    l->next = st->listeners;
    st->listeners = l;
}

void mcc_store_unlisten(mcc_chunk_store *st, mcc_store_listener *l) {
    for (mcc_store_listener **p = &st->listeners; *p; p = &(*p)->next)
        if (*p == l) {
            *p = l->next;
            l->next = NULL;
            return;
        }
}

/* ------------------------------------------------------------------ */
/* 区块                                                                */
/* ------------------------------------------------------------------ */

mcc_chunk *mcc_chunk_new(const mcc_chunk_store *st, int32_t cx, int32_t cz) {
    mcc_chunk *c = mcc_xcalloc(1, sizeof *c + sizeof(mcc_chunk_section *) * (size_t)st->nsec);
    c->cx = cx;
    c->cz = cz;
    c->min_sy = st->min_sy;
    c->nsec = st->nsec;
    return c;
}

void mcc_chunk_free(mcc_chunk *c) {
    if (!c) return;
    for (int i = 0; i < c->nsec; i++) mcc_section_free(c->sec[i]);
    free(c);
}

mcc_state_t mcc_chunk_get(const mcc_chunk *c, int lx, int32_t y, int lz) {
    const mcc_chunk_section *s = mcc_chunk_section_at(c, y >> 4);
    return s ? mcc_section_get(s, mcc_section_index(lx, y & 15, lz)) : 0;
}

void mcc_chunk_set_raw(mcc_chunk *c, int lx, int32_t y, int lz, mcc_state_t state) {
    if (c->store) mcc_log_fatal("区块 (%d,%d) 已加入存储，不能 raw 写入", c->cx, c->cz);
    uint32_t i = (uint32_t)((y >> 4) - c->min_sy);
    if (i >= (uint32_t)c->nsec) return;
    if (!c->sec[i]) {
        if (mcc_state_is_air(state)) return;
        c->sec[i] = mcc_section_new(0);
    }
    mcc_section_set(c->sec[i], mcc_section_index(lx, y & 15, lz), state);
}

/* ------------------------------------------------------------------ */
/* 物理位图维护                                                        */
/* ------------------------------------------------------------------ */

MCC_INLINE void secs_put(uint64_t *bits, int i, bool on) {
    if (on) bits[i >> 6] |= 1ULL << (i & 63);
    else bits[i >> 6] &= ~(1ULL << (i & 63));
}

static void sync_secs(mcc_chunk *c, int i) {
    const mcc_chunk_section *s = c->sec[i];
    secs_put(c->solid_secs, i, s && s->solid_count);
    secs_put(c->liquid_secs, i, s && s->liquid_count);
}

/* hint 为 (x, z) 所在区块时省去一次查表 */
static mcc_state_t state_at(const mcc_chunk_store *st, const mcc_chunk *hint, int32_t x, int32_t y, int32_t z) {
    const mcc_chunk *c = (hint && hint->cx == x >> 4 && hint->cz == z >> 4) ? hint : mcc_store_chunk(st, x >> 4, z >> 4);
    if (!c) return 0;
    const mcc_chunk_section *s = mcc_chunk_section_at(c, y >> 4);
    return s ? mcc_section_get(s, mcc_section_index(x & 15, y & 15, z & 15)) : 0;
}

MCC_INLINE bool full_at(const mcc_chunk_store *st, const mcc_chunk *hint, int32_t x, int32_t y, int32_t z) {
    return mcc_state_pflags(state_at(st, hint, x, y, z)) & MCC_PHYS_FULL;
}

/* 内部方块：六个面的邻居都是整块（未加载的邻居按空气） */
static bool interior(const mcc_chunk_store *st, const mcc_chunk *c, int32_t x, int32_t y, int32_t z) {
    return full_at(st, c, x - 1, y, z) && full_at(st, c, x + 1, y, z) && full_at(st, c, x, y - 1, z) &&
           full_at(st, c, x, y + 1, z) && full_at(st, c, x, y, z - 1) && full_at(st, c, x, y, z + 1);
}

/* 按定义重算一个方块的物理位（c 为其所在区块） */
static void refresh(const mcc_chunk_store *st, mcc_chunk *c, int32_t x, int32_t y, int32_t z) {
    int i = (y >> 4) - c->min_sy;
    if (i < 0 || i >= c->nsec || !c->sec[i]) return;
    mcc_chunk_section *s = c->sec[i];
    uint8_t f = mcc_state_pflags(mcc_section_get(s, mcc_section_index(x & 15, y & 15, z & 15)));
    bool solid = (f & MCC_PHYS_SOLID) && !interior(st, c, x, y, z);
    mcc_section_set_phys(s, x & 15, y & 15, z & 15, solid, f & MCC_PHYS_LIQUID);
    sync_secs(c, i);
}

static void refresh_at(const mcc_chunk_store *st, int32_t x, int32_t y, int32_t z) {
    mcc_chunk *c = mcc_store_chunk(st, x >> 4, z >> 4);
    if (c) refresh(st, c, x, y, z);
}

static bool full_block(const mcc_chunk_section *s, int x, int y, int z) {
    return s && (mcc_state_pflags(mcc_section_get(s, mcc_section_index(x, y, z))) & MCC_PHYS_FULL);
}

static uint32_t full_row(const mcc_chunk_section *s, int y, int z) {
    if (!s) return 0;
    uint32_t r = 0;
    for (int x = 0; x < 16; x++) r |= (uint32_t)full_block(s, x, y, z) << x;
    return r;
}

/* 整区段批量计算位图：先拼出带一圈邻居的整块位图，再按行求内部方块 */
static void compute_section(const mcc_chunk_store *st, mcc_chunk *c, int i) {
    mcc_chunk_section *s = c->sec[i];
    if (!s) return;
    uint16_t solid[16][16], liquid[16][16], full[16][16];
    mcc_section_flag_rows(s, solid, liquid, full);
    uint32_t any = 0;
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++) any |= (uint32_t)solid[y][z] | liquid[y][z];
    if (!any) {
        free(s->phys);
        s->phys = NULL;
        s->solid_count = s->liquid_count = 0;
        sync_secs(c, i);
        return;
    }

    /* F[y+1][z+1] 的第 x+1 位：(x, y, z) 是否整块，x/y/z 取 -1~16 */
    uint32_t F[18][18];
    memset(F, 0, sizeof F);
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++) F[y + 1][z + 1] = (uint32_t)full[y][z] << 1;
    int32_t sy = c->min_sy + i;
    const mcc_chunk *w = mcc_store_chunk(st, c->cx - 1, c->cz), *e = mcc_store_chunk(st, c->cx + 1, c->cz);
    const mcc_chunk *n = mcc_store_chunk(st, c->cx, c->cz - 1), *so = mcc_store_chunk(st, c->cx, c->cz + 1);
    const mcc_chunk_section *ws = w ? mcc_chunk_section_at(w, sy) : NULL, *es = e ? mcc_chunk_section_at(e, sy) : NULL;
    const mcc_chunk_section *ns = n ? mcc_chunk_section_at(n, sy) : NULL, *ss = so ? mcc_chunk_section_at(so, sy) : NULL;
    const mcc_chunk_section *bs = i > 0 ? c->sec[i - 1] : NULL, *ts = i + 1 < c->nsec ? c->sec[i + 1] : NULL;
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++) {
            F[y + 1][z + 1] |= (uint32_t)full_block(ws, 15, y, z) | (uint32_t)full_block(es, 0, y, z) << 17;
        }
    for (int y = 0; y < 16; y++) {
        F[y + 1][0] = full_row(ns, y, 15) << 1;
        F[y + 1][17] = full_row(ss, y, 0) << 1;
    }
    for (int z = 0; z < 16; z++) {
        F[0][z + 1] = full_row(bs, 15, z) << 1;
        F[17][z + 1] = full_row(ts, 0, z) << 1;
    }

    if (!s->phys) s->phys = mcc_xmalloc(sizeof *s->phys);
    uint32_t ns_ = 0, nl = 0;
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++) {
            uint32_t f = F[y + 1][z + 1];
            /* 内部只看六个邻居，本方块自身不必是整块（被石头包住的楼梯也算内部） */
            uint32_t in = (f >> 1) & (f << 1) & F[y + 1][z] & F[y + 1][z + 2] & F[y][z + 1] & F[y + 2][z + 1];
            uint16_t r = (uint16_t)(solid[y][z] & ~(in >> 1));
            s->phys->solid[y][z] = r;
            s->phys->liquid[y][z] = liquid[y][z];
            ns_ += (uint32_t)__builtin_popcount(r);
            nl += (uint32_t)__builtin_popcount(liquid[y][z]);
        }
    s->solid_count = (uint16_t)ns_;
    s->liquid_count = (uint16_t)nl;
    if (!ns_ && !nl) {
        free(s->phys);
        s->phys = NULL;
    }
    sync_secs(c, i);
}

/* 区块 c 朝 (dx, dz) 方向那一面的边界层：c 里有碰撞箱的方块重算（邻居区块刚加入或移出） */
static void refresh_border(const mcc_chunk_store *st, mcc_chunk *c, int dx, int dz) {
    for (int i = 0; i < c->nsec; i++) {
        const mcc_chunk_section *s = c->sec[i];
        if (!s) continue;
        int32_t by = (c->min_sy + i) << 4;
        for (int y = 0; y < 16; y++)
            for (int k = 0; k < 16; k++) {
                int lx = dx > 0 ? 15 : dx < 0 ? 0 : k;
                int lz = dz > 0 ? 15 : dz < 0 ? 0 : k;
                if (!(mcc_state_pflags(mcc_section_get(s, mcc_section_index(lx, y, lz))) & MCC_PHYS_SOLID)) continue;
                refresh(st, c, (c->cx << 4) + lx, by + y, (c->cz << 4) + lz);
            }
    }
}

static const int DIRS[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };

void mcc_store_add_chunk(mcc_chunk_store *st, mcc_chunk *c) {
    if (c->min_sy != st->min_sy || c->nsec != st->nsec) mcc_log_fatal("区块 (%d,%d) 高度与存储不符", c->cx, c->cz);
    if (mcc_store_chunk(st, c->cx, c->cz)) mcc_log_fatal("区块 (%d,%d) 重复加入存储", c->cx, c->cz);
    c->store = st;
    mcc_hmap_put(&st->chunks, mcc_key2(c->cx, c->cz), c);
    for (int i = 0; i < c->nsec; i++) compute_section(st, c, i);
    /* 四邻朝向本区块的那一面：原来按空气算的邻居现在可能是整块 */
    for (int d = 0; d < 4; d++) {
        mcc_chunk *n = mcc_store_chunk(st, c->cx + DIRS[d][0], c->cz + DIRS[d][1]);
        if (n) refresh_border(st, n, -DIRS[d][0], -DIRS[d][1]);
    }
    for (mcc_store_listener *l = st->listeners; l; l = l->next)
        if (l->chunk_added) l->chunk_added(l->ctx, c);
}

mcc_chunk *mcc_store_remove_chunk(mcc_chunk_store *st, int32_t cx, int32_t cz) {
    mcc_chunk *c = mcc_store_chunk(st, cx, cz);
    if (!c) return NULL;
    for (mcc_store_listener *l = st->listeners; l; l = l->next)
        if (l->chunk_removed) l->chunk_removed(l->ctx, c);
    mcc_hmap_remove(&st->chunks, mcc_key2(cx, cz));
    c->store = NULL;
    for (int d = 0; d < 4; d++) {
        mcc_chunk *n = mcc_store_chunk(st, cx + DIRS[d][0], cz + DIRS[d][1]);
        if (n) refresh_border(st, n, -DIRS[d][0], -DIRS[d][1]);
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* 方块读写                                                            */
/* ------------------------------------------------------------------ */

mcc_state_t mcc_store_get(const mcc_chunk_store *st, int32_t x, int32_t y, int32_t z) {
    return state_at(st, NULL, x, y, z);
}

mcc_state_t mcc_store_set(mcc_chunk_store *st, int32_t x, int32_t y, int32_t z, mcc_state_t state) {
    mcc_chunk *c = mcc_store_chunk(st, x >> 4, z >> 4);
    if (!c) return MCC_STATE_NONE;
    int i = (y >> 4) - c->min_sy;
    if (i < 0 || i >= c->nsec) return MCC_STATE_NONE;
    mcc_chunk_section *s = c->sec[i];
    if (!s) {
        if (state == 0) return 0;
        s = c->sec[i] = mcc_section_new(0);
    }
    mcc_state_t old = mcc_section_set(s, mcc_section_index(x & 15, y & 15, z & 15), state);
    if (old == state) return old;
    uint8_t changed = (uint8_t)(mcc_state_pflags(old) ^ mcc_state_pflags(state));
    if (changed) refresh(st, c, x, y, z);
    if (changed & MCC_PHYS_FULL) {
        /* 本方块是否整块变了：六个邻居的外露判断随之变化 */
        refresh_at(st, x - 1, y, z);
        refresh_at(st, x + 1, y, z);
        refresh(st, c, x, y - 1, z);
        refresh(st, c, x, y + 1, z);
        refresh_at(st, x, y, z - 1);
        refresh_at(st, x, y, z + 1);
    }
    if (!s->nonzero) {
        /* 区段变回全是状态 0（空气）：释放，读取时按空气处理 */
        mcc_section_free(s);
        c->sec[i] = NULL;
        sync_secs(c, i);
    }
    for (mcc_store_listener *l = st->listeners; l; l = l->next)
        if (l->block_changed) l->block_changed(l->ctx, x, y, z, old, state);
    return old;
}

/* ------------------------------------------------------------------ */
/* 占用查询                                                            */
/* ------------------------------------------------------------------ */

MCC_INLINE uint16_t xmask(int lo, int hi) { return (uint16_t)(((2u << hi) - 1) & ~((1u << lo) - 1)); }

/* 遍历包围盒覆盖的有置位区段；fn 返回 true 时提前结束 */
#define BOX_SECTIONS(cur, kind, mn, mx, BODY) do { \
        for (int32_t cx_ = (mn)[0] >> 4; cx_ <= (mx)[0] >> 4; cx_++) \
            for (int32_t cz_ = (mn)[2] >> 4; cz_ <= (mx)[2] >> 4; cz_++) { \
                const mcc_chunk *c_ = mcc_cursor_chunk((cur), cx_, cz_); \
                if (!c_) continue; \
                const uint64_t *secs_ = (kind) ? c_->liquid_secs : c_->solid_secs; \
                int32_t sy0_ = MCC_MAX((mn)[1] >> 4, c_->min_sy), sy1_ = MCC_MIN((mx)[1] >> 4, c_->min_sy + c_->nsec - 1); \
                for (int32_t sy_ = sy0_; sy_ <= sy1_; sy_++) { \
                    int i_ = sy_ - c_->min_sy; \
                    if (!(secs_[i_ >> 6] >> (i_ & 63) & 1)) continue; \
                    const mcc_chunk_section *s_ = c_->sec[i_]; \
                    const uint16_t(*rows)[16] = (kind) ? s_->phys->liquid : s_->phys->solid; \
                    int32_t bx = cx_ << 4, by = sy_ << 4, bz = cz_ << 4; \
                    int x0 = MCC_MAX((mn)[0] - bx, 0), x1 = MCC_MIN((mx)[0] - bx, 15); \
                    int y0 = MCC_MAX((mn)[1] - by, 0), y1 = MCC_MIN((mx)[1] - by, 15); \
                    int z0 = MCC_MAX((mn)[2] - bz, 0), z1 = MCC_MIN((mx)[2] - bz, 15); \
                    uint16_t xm = xmask(x0, x1); \
                    BODY \
                } \
            } \
    } while (0)

bool mcc_cursor_box_any(mcc_store_cursor *cur, int kind, const int32_t mn[3], const int32_t mx[3]) {
    BOX_SECTIONS(cur, kind, mn, mx, {
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++)
                if (rows[y][z] & xm) return true;
    });
    return false;
}

void mcc_cursor_box_collect(mcc_store_cursor *cur, int kind, const int32_t mn[3], const int32_t mx[3], mcc_i32vec *out) {
    BOX_SECTIONS(cur, kind, mn, mx, {
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++) {
                uint32_t r = rows[y][z] & xm;
                while (r) {
                    int x = __builtin_ctz(r);
                    r &= r - 1;
                    mcc_vec_push(out, bx + x);
                    mcc_vec_push(out, by + y);
                    mcc_vec_push(out, bz + z);
                }
            }
    });
}

/* ------------------------------------------------------------------ */
/* 统计与自检                                                          */
/* ------------------------------------------------------------------ */

void mcc_store_stats_get(const mcc_chunk_store *st, mcc_store_stats *out) {
    memset(out, 0, sizeof *out);
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&st->chunks, &it, NULL, &v)) {
        const mcc_chunk *c = v;
        out->chunks++;
        out->memory += sizeof *c + sizeof(mcc_chunk_section *) * (size_t)c->nsec;
        for (int i = 0; i < c->nsec; i++) {
            if (!c->sec[i]) continue;
            out->sections++;
            if (c->sec[i]->phys) out->phys_sections++;
            out->memory += mcc_section_memory(c->sec[i]);
        }
    }
}

bool mcc_store_verify(const mcc_chunk_store *st, char *err, size_t errlen) {
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&st->chunks, &it, NULL, &v)) {
        const mcc_chunk *c = v;
        if (c->store != st) { snprintf(err, errlen, "区块 (%d,%d) 存储指针错误", c->cx, c->cz); return false; }
        for (int i = 0; i < c->nsec; i++) {
            const mcc_chunk_section *s = c->sec[i];
            bool want_s = s && s->solid_count, want_l = s && s->liquid_count;
            if (want_s != (bool)(c->solid_secs[i >> 6] >> (i & 63) & 1) || want_l != (bool)(c->liquid_secs[i >> 6] >> (i & 63) & 1)) {
                snprintf(err, errlen, "区块 (%d,%d) 区段 %d 的区段位与计数不符", c->cx, c->cz, i);
                return false;
            }
            if (!s) continue;
            int32_t sy = c->min_sy + i;
            uint32_t non_air = 0, ns = 0, nl = 0;
            for (int idx = 0; idx < MCC_SECTION_BLOCKS; idx++) {
                int lx = idx & 15, lz = (idx >> 4) & 15, ly = idx >> 8;
                int32_t x = (c->cx << 4) + lx, y = (sy << 4) + ly, z = (c->cz << 4) + lz;
                mcc_state_t stt = mcc_section_get(s, idx);
                non_air += !mcc_state_is_air(stt);
                uint8_t f = mcc_state_pflags(stt);
                bool solid = (f & MCC_PHYS_SOLID) && !interior(st, c, x, y, z);
                bool liquid = f & MCC_PHYS_LIQUID;
                ns += solid;
                nl += liquid;
                if (solid != mcc_section_solid_bit(s, lx, ly, lz) || liquid != mcc_section_liquid_bit(s, lx, ly, lz)) {
                    snprintf(err, errlen, "方块 (%d,%d,%d) 物理位不符：应为 实心%d 流体%d，实为 %d %d", x, y, z, solid, liquid,
                             mcc_section_solid_bit(s, lx, ly, lz), mcc_section_liquid_bit(s, lx, ly, lz));
                    return false;
                }
            }
            if (non_air != s->non_air || ns != s->solid_count || nl != s->liquid_count || (bool)s->phys != (ns || nl)) {
                snprintf(err, errlen, "区块 (%d,%d) 区段 %d 计数不符：非空气 %u/%u 实心 %u/%u 流体 %u/%u", c->cx, c->cz, i, non_air,
                         s->non_air, ns, s->solid_count, nl, s->liquid_count);
                return false;
            }
        }
    }
    return true;
}
