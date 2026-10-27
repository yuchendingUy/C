#include "chunk.h"
#include "../core/log.h"
#include <stdio.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

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

/* ---- 批量解码：区段每个方块的物理标志 ---- */

/* 标志视图：调色板先换算成标志表，读一个方块只需取下标再查表 */
typedef struct {
    const mcc_chunk_section *s;
    uint8_t pf[256];
} fview;

static void fv_init(fview *v, const mcc_chunk_section *s) {
    v->s = s;
    if (!s) return;
    if (s->bits == 0) v->pf[0] = mcc_state_pflags(s->single);
    else if (s->bits != 16)
        for (int i = 0; i < s->palette_len; i++) v->pf[i] = mcc_state_pflags(s->palette[i]);
}

MCC_INLINE uint8_t fv_at(const fview *v, int idx) {
    const mcc_chunk_section *s = v->s;
    switch (s->bits) {
    case 0: return v->pf[0];
    case 4: return v->pf[(s->d8[idx >> 1] >> ((idx & 1) << 2)) & 15];
    case 8: return v->pf[s->d8[idx]];
    default: return mcc_state_pflags(s->d16[idx]);
    }
}

/* 整个区段解码成每方块一字节标志 */
static void section_flags(const mcc_chunk_section *s, uint8_t *out) {
    fview v;
    fv_init(&v, s);
    switch (s->bits) {
    case 0: memset(out, v.pf[0], MCC_SECTION_BLOCKS); break;
    case 4:
        for (int i = 0; i < MCC_SECTION_BLOCKS / 2; i++) {
            uint8_t b = s->d8[i];
            out[2 * i] = v.pf[b & 15];
            out[2 * i + 1] = v.pf[b >> 4];
        }
        break;
    case 8:
        for (int i = 0; i < MCC_SECTION_BLOCKS; i++) out[i] = v.pf[s->d8[i]];
        break;
    default:
        for (int i = 0; i < MCC_SECTION_BLOCKS; i++) out[i] = mcc_state_pflags(s->d16[i]);
    }
}

/* 16 个方块（一行）的某个标志位压成 16 位掩码 */
MCC_INLINE uint16_t row_mask(const uint8_t *f, int bit) {
#if defined(__SSE2__)
    __m128i v = _mm_loadu_si128((const __m128i *)f);
    return (uint16_t)_mm_movemask_epi8(_mm_slli_epi16(v, 7 - bit));
#else
    uint32_t r = 0;
    for (int x = 0; x < 16; x++) r |= (uint32_t)(f[x] >> bit & 1) << x;
    return (uint16_t)r;
#endif
}

typedef struct {
    uint16_t solid[16][16], liquid[16][16], full[16][16];
} sec_rows;

static void section_rows(const mcc_chunk_section *s, sec_rows *r) {
    uint8_t f[MCC_SECTION_BLOCKS];
    section_flags(s, f);
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++) {
            const uint8_t *row = f + (y << 8) + (z << 4);
            r->solid[y][z] = row_mask(row, 0);
            r->full[y][z] = row_mask(row, 1);
            r->liquid[y][z] = row_mask(row, 2);
        }
}

/* 取 16 个方块的标志：下标 base + k*stride，位宽分支在循环外 */
static void fv_gather(const fview *v, int base, int stride, uint8_t out[16]) {
    const mcc_chunk_section *s = v->s;
    switch (s->bits) {
    case 0: memset(out, v->pf[0], 16); break;
    case 4:
        for (int k = 0, idx = base; k < 16; k++, idx += stride) out[k] = v->pf[(s->d8[idx >> 1] >> ((idx & 1) << 2)) & 15];
        break;
    case 8:
        for (int k = 0, idx = base; k < 16; k++, idx += stride) out[k] = v->pf[s->d8[idx]];
        break;
    default:
        for (int k = 0, idx = base; k < 16; k++, idx += stride) out[k] = mcc_state_pflags(s->d16[idx]);
    }
}

/* 区段的一面，同时取实心与整块两个掩码（任一输出可为 NULL）：
 *   x = 常数：out[y] 第 z 位；z = 常数：out[y] 第 x 位；y = 常数：out[z] 第 x 位 */
enum { FACE_X, FACE_Y, FACE_Z };
static void face(const fview *v, int axis, int layer, uint16_t solid[16], uint16_t full[16]) {
    if (!v->s) {
        if (solid) memset(solid, 0, 32);
        if (full) memset(full, 0, 32);
        return;
    }
    uint8_t f[16];
    for (int j = 0; j < 16; j++) {
        if (axis == FACE_X) fv_gather(v, layer + (j << 8), 16, f);
        else if (axis == FACE_Z) fv_gather(v, (layer << 4) + (j << 8), 1, f);
        else fv_gather(v, (layer << 8) + (j << 4), 1, f);
        if (solid) solid[j] = row_mask(f, 0);
        if (full) full[j] = row_mask(f, 1);
    }
}

/* 把区段的 solid 位图换成 rows（liquid 照抄），维护计数与分配 */
static void store_phys_rows(mcc_chunk *c, int i, const uint16_t solid[16][16], const uint16_t liquid[16][16]) {
    mcc_chunk_section *s = c->sec[i];
    uint32_t ns = 0, nl = 0;
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++) {
            ns += (uint32_t)__builtin_popcount(solid[y][z]);
            nl += (uint32_t)__builtin_popcount(liquid[y][z]);
        }
    if (!ns && !nl) {
        free(s->phys);
        s->phys = NULL;
    } else {
        if (!s->phys) s->phys = mcc_xmalloc(sizeof *s->phys);
        memcpy(s->phys->solid, solid, sizeof s->phys->solid);
        memcpy(s->phys->liquid, liquid, sizeof s->phys->liquid);
    }
    s->solid_count = (uint16_t)ns;
    s->liquid_count = (uint16_t)nl;
    sync_secs(c, i);
}

/* 整个区块批量计算位图：各区段只解码一次，上下相邻区段复用；四邻区块只解码贴着的一面 */
static void compute_chunk(const mcc_chunk_store *st, mcc_chunk *c) {
    sec_rows *R = mcc_xmalloc(sizeof(sec_rows) * (size_t)c->nsec);
    for (int i = 0; i < c->nsec; i++)
        if (c->sec[i]) section_rows(c->sec[i], &R[i]);
    const mcc_chunk *w = mcc_store_chunk(st, c->cx - 1, c->cz), *e = mcc_store_chunk(st, c->cx + 1, c->cz);
    const mcc_chunk *n = mcc_store_chunk(st, c->cx, c->cz - 1), *so = mcc_store_chunk(st, c->cx, c->cz + 1);
    for (int i = 0; i < c->nsec; i++) {
        if (!c->sec[i]) continue;
        sec_rows *r = &R[i];
        uint32_t any = 0;
        for (int y = 0; y < 16; y++)
            for (int z = 0; z < 16; z++) any |= (uint32_t)r->solid[y][z] | r->liquid[y][z];
        if (!any) {
            store_phys_rows(c, i, r->solid, r->liquid);
            continue;
        }
        int32_t sy = c->min_sy + i;
        fview fw, fe, fn, fs;
        fv_init(&fw, w ? mcc_chunk_section_at(w, sy) : NULL);
        fv_init(&fe, e ? mcc_chunk_section_at(e, sy) : NULL);
        fv_init(&fn, n ? mcc_chunk_section_at(n, sy) : NULL);
        fv_init(&fs, so ? mcc_chunk_section_at(so, sy) : NULL);
        uint16_t W[16], E[16], N[16], S[16];
        face(&fw, FACE_X, 15, NULL, W);
        face(&fe, FACE_X, 0, NULL, E);
        face(&fn, FACE_Z, 15, NULL, N);
        face(&fs, FACE_Z, 0, NULL, S);
        static const uint16_t zero[16];
        const uint16_t *below = i > 0 && c->sec[i - 1] ? R[i - 1].full[15] : zero;
        const uint16_t *above = i + 1 < c->nsec && c->sec[i + 1] ? R[i + 1].full[0] : zero;
        uint16_t solid[16][16];
        for (int y = 0; y < 16; y++)
            for (int z = 0; z < 16; z++) {
                uint32_t f = (uint32_t)r->full[y][z] << 1 | (W[y] >> z & 1) | (uint32_t)(E[y] >> z & 1) << 17;
                uint32_t zn = z > 0 ? r->full[y][z - 1] : N[y], zp = z < 15 ? r->full[y][z + 1] : S[y];
                uint32_t yn = y > 0 ? r->full[y - 1][z] : below[z], yp = y < 15 ? r->full[y + 1][z] : above[z];
                /* 内部只看六个邻居，本方块自身不必是整块（被石头包住的楼梯也算内部） */
                uint32_t in = (f >> 2) & f & zn & zp & yn & yp;
                solid[y][z] = (uint16_t)(r->solid[y][z] & ~in);
            }
        store_phys_rows(c, i, solid, r->liquid);
    }
    free(R);
}

/* 区块 c 朝 (dx, dz) 方向那一面的边界层重算（那边的邻居区块刚加入、移出或换了存储）。
 * 按行位运算：只解码边界层、往里一层、外侧邻居的贴面，以及两端拐角处的一列 */
static void refresh_border(const mcc_chunk_store *st, mcc_chunk *c, int dx, int dz) {
    const mcc_chunk *o = mcc_store_chunk(st, c->cx + dx, c->cz + dz);
    /* 沿边界方向 k 的两端，各属于垂直方向的邻居区块 */
    const mcc_chunk *kn = dx ? mcc_store_chunk(st, c->cx, c->cz - 1) : mcc_store_chunk(st, c->cx - 1, c->cz);
    const mcc_chunk *kp = dx ? mcc_store_chunk(st, c->cx, c->cz + 1) : mcc_store_chunk(st, c->cx + 1, c->cz);
    int layer = (dx > 0 || dz > 0) ? 15 : 0, inner = layer == 15 ? 14 : 1, outer = 15 - layer;
    for (int i = 0; i < c->nsec; i++) {
        mcc_chunk_section *s = c->sec[i];
        if (!s) continue;
        int32_t sy = c->min_sy + i;
        fview fl, fo, fb, ft, fkn, fkp;
        fv_init(&fl, s);
        fv_init(&fo, o ? mcc_chunk_section_at(o, sy) : NULL);
        fv_init(&fb, i > 0 ? c->sec[i - 1] : NULL);
        fv_init(&ft, i + 1 < c->nsec ? c->sec[i + 1] : NULL);
        fv_init(&fkn, kn ? mcc_chunk_section_at(kn, sy) : NULL);
        fv_init(&fkp, kp ? mcc_chunk_section_at(kp, sy) : NULL);
        int ax = dx ? FACE_X : FACE_Z;
        /* L* 本层、If 往里一层、Of 外侧邻居贴面：都是 [y] 第 k 位 */
        uint16_t Ls[16], Lf[16], If[16], Of[16], B[16], T[16], KN[16], KP[16];
        face(&fl, ax, layer, Ls, Lf);
        face(&fl, ax, inner, NULL, If);
        face(&fo, ax, outer, NULL, Of);
        /* 上下相邻区段贴面（[z] 第 x 位），取出本层那一行/列 */
        face(&fb, FACE_Y, 15, NULL, B);
        face(&ft, FACE_Y, 0, NULL, T);
        uint32_t yb = 0, yt = 0;
        if (dx) {
            for (int k = 0; k < 16; k++) { yb |= (uint32_t)(B[k] >> layer & 1) << k; yt |= (uint32_t)(T[k] >> layer & 1) << k; }
        } else {
            yb = B[layer];
            yt = T[layer];
        }
        /* 两端拐角：垂直方向邻居区块贴面上同一层的那一列 */
        face(&fkn, dx ? FACE_Z : FACE_X, 15, NULL, KN);
        face(&fkp, dx ? FACE_Z : FACE_X, 0, NULL, KP);
        uint16_t want[16];
        bool any = false;
        for (int y = 0; y < 16; y++) {
            uint32_t E = (uint32_t)Lf[y] << 1 | (uint32_t)(KN[y] >> layer & 1) | (uint32_t)(KP[y] >> layer & 1) << 17;
            uint32_t yn = y > 0 ? Lf[y - 1] : yb, yp = y < 15 ? Lf[y + 1] : yt;
            uint32_t in = (E & (E >> 2)) & If[y] & Of[y] & yn & yp;
            want[y] = (uint16_t)(Ls[y] & ~in);
            any |= want[y] != 0;
        }
        /* 整批写回：只动本层的位，计数按差值更新 */
        if (!s->phys) {
            if (!any) continue;
            s->phys = mcc_xcalloc(1, sizeof *s->phys);
        }
        int32_t delta = 0;
        for (int y = 0; y < 16; y++) {
            if (dx) {
                for (int k = 0; k < 16; k++) {
                    uint16_t *row = &s->phys->solid[y][k];
                    uint16_t nr = (uint16_t)((*row & ~(1u << layer)) | (uint32_t)(want[y] >> k & 1) << layer);
                    delta += __builtin_popcount(nr) - __builtin_popcount(*row);
                    *row = nr;
                }
            } else {
                uint16_t *row = &s->phys->solid[y][layer];
                delta += __builtin_popcount(want[y]) - __builtin_popcount(*row);
                *row = want[y];
            }
        }
        s->solid_count = (uint16_t)(s->solid_count + delta);
        if (!s->solid_count && !s->liquid_count) {
            free(s->phys);
            s->phys = NULL;
        }
        sync_secs(c, i);
    }
}

static const int DIRS[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };

void mcc_store_add_chunk(mcc_chunk_store *st, mcc_chunk *c) {
    if (c->min_sy != st->min_sy || c->nsec != st->nsec) mcc_log_fatal("区块 (%d,%d) 高度与存储不符", c->cx, c->cz);
    if (mcc_store_chunk(st, c->cx, c->cz)) mcc_log_fatal("区块 (%d,%d) 重复加入存储", c->cx, c->cz);
    c->store = st;
    mcc_hmap_put(&st->chunks, mcc_key2(c->cx, c->cz), c);
    compute_chunk(st, c);
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

void mcc_store_move(mcc_chunk_store *from, mcc_chunk_store *to, const uint64_t *keys, size_t n) {
    if (from == to || !n) return;
    if (from->min_sy != to->min_sy || from->nsec != to->nsec) mcc_log_fatal("区块存储高度不同，不能搬迁");
    mcc_chunk **moved = mcc_xmalloc(sizeof(mcc_chunk *) * n);
    size_t m = 0;
    mcc_hmap moving;
    mcc_hmap_init(&moving, (uint32_t)n * 2);
    for (size_t i = 0; i < n; i++) {
        mcc_chunk *c = mcc_hmap_get(&from->chunks, keys[i]);
        if (!c) continue;
        if (mcc_store_chunk(to, c->cx, c->cz)) mcc_log_fatal("区块 (%d,%d) 搬迁目标已存在", c->cx, c->cz);
        for (mcc_store_listener *l = from->listeners; l; l = l->next)
            if (l->chunk_removed) l->chunk_removed(l->ctx, c);
        mcc_hmap_remove(&from->chunks, keys[i]);
        mcc_hmap_put(&moving, keys[i], c);
        moved[m++] = c;
    }
    for (size_t i = 0; i < m; i++) {
        mcc_chunk *c = moved[i];
        c->store = to;
        mcc_hmap_put(&to->chunks, mcc_key2(c->cx, c->cz), c);
    }
    /* 交界：搬走的区块旁边留在 from 的（现在分开了），或 to 里原有的（现在挨上了） */
    for (size_t i = 0; i < m; i++) {
        mcc_chunk *c = moved[i];
        for (int d = 0; d < 4; d++) {
            int32_t nx = c->cx + DIRS[d][0], nz = c->cz + DIRS[d][1];
            mcc_chunk *nf = mcc_store_chunk(from, nx, nz);
            mcc_chunk *nt = mcc_hmap_get(&moving, mcc_key2(nx, nz)) ? NULL : mcc_store_chunk(to, nx, nz);
            if (nf) refresh_border(from, nf, -DIRS[d][0], -DIRS[d][1]);
            if (nt) refresh_border(to, nt, -DIRS[d][0], -DIRS[d][1]);
            if (nf || nt) refresh_border(to, c, DIRS[d][0], DIRS[d][1]);
        }
    }
    for (size_t i = 0; i < m; i++)
        for (mcc_store_listener *l = to->listeners; l; l = l->next)
            if (l->chunk_added) l->chunk_added(l->ctx, moved[i]);
    mcc_hmap_free(&moving);
    free(moved);
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
