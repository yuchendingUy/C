/*
 * 体素接触生成（移植自 Sable sable_rapier/algo.rs + dispatcher.rs）
 *
 * 方块数据直接读世界区块存储（地形 / 子世界自己的存储），不保存副本：
 *   - 碰撞体编号：方块状态表
 *   - 外露实心 / 流体占用：区段里的物理位图（替代原来的八叉树）
 *   - 邻居状态（面/棱/角/内部）：读六个邻居是否整块，现算
 *
 * 流程：
 *   1. 找方块对：逐层遍历一侧刚体的占用位图（区段 16³ → 小块 4³ → 方块），
 *      节点外接球变换到另一侧，查询另一侧的占用位图，没有重叠的整层剪掉
 *   2. 方块对接触：两块的每个碰撞箱两两做凸包碰撞（Box3D b3CollideHulls）
 *   3. 剔除假接触：接触点沿法线略微外移后落在本侧另一个实心方块里，说明是内部接缝，丢弃
 *   4. 合并：按法向 + 材质聚类，每类精简到最多 4 个点（大结构平放时控制接触片数量）
 *
 * 在 Box3D 碰撞阶段的工作线程上并行调用，只读场景数据，线程局部缓冲区。
 */
#include "scene.h"
#include "boxbox.h"
#include "../core/log.h"
#include <math.h>
#include <float.h>

#define SUBLEVEL_BASE_FRICTION 0.525f
#define INTERIOR_SCALE_A 0.997f   /* Sable：静态侧点向碰撞箱中心收缩系数 */
#define INTERIOR_OFFSET_A 0.01    /* Sable：静态侧沿法线外移距离 */
#define INTERIOR_SCALE_B 0.99f    /* Sable INTERIOR_COLLISION_SCALE_FACTOR */
#define INTERIOR_OFFSET_B 0.015   /* Sable INTERIOR_COLLISION_CHECK_DISTANCE */
#define MAX_CLUSTERS 32
#define CLUSTER_NORMAL_DOT 0.995f

void phys_scene_count(phys_scene *s, uint64_t manifolds, uint64_t pairs);
static bool g_stats_on;
static bool g_canonical = true;
void phys_set_canonical(bool on) { g_canonical = on; }
void phys_set_stats_enabled(bool on) { g_stats_on = on; }
void phys_scene_count_time(phys_scene *s, uint64_t ns);

/* ---------------- 体素侧 ---------------- */

typedef struct vside {
    int kind;                 /* PHYS_KIND_BODY / PHYS_KIND_CELL */
    phys_body *body;          /* kind == BODY */
    phys_cell *cell;          /* kind == CELL */
    double offset[3];         /* 方块坐标 → 形状局部系：local = block - offset */
    bool interior_check;      /* 多方块刚体或地形才做内部接缝剔除 */
    mcc_store_cursor cur;     /* 方块存储读游标（缓存上次命中的区块） */
} vside;

static void make_side(vside *v, void *user) {
    memset(v, 0, sizeof *v);
    v->kind = *(int *)user;
    if (v->kind == PHYS_KIND_BODY) {
        v->body = (phys_body*)user;
        memcpy(v->offset, v->body->com, sizeof v->offset);
        const int32_t *a = v->body->bmin, *b = v->body->bmax;
        v->interior_check = a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
    } else {
        v->cell = (phys_cell*)user;
        memcpy(v->offset, v->cell->origin, sizeof v->offset);
        v->interior_check = true;
    }
    mcc_cursor_init(&v->cur, v->kind == PHYS_KIND_BODY ? v->body->blocks : phys_scene_terrain(v->cell->scene));
}

MCC_INLINE mcc_state_t side_state(vside *v, int32_t x, int32_t y, int32_t z) { return mcc_cursor_get(&v->cur, x, y, z); }

static bool side_ready(const vside *v) {
    return v->cur.store && (v->kind == PHYS_KIND_CELL || (v->body->has_com && v->body->has_bounds));
}

MCC_INLINE bool side_full(vside *v, int32_t x, int32_t y, int32_t z) {
    return mcc_state_pflags(side_state(v, x, y, z)) & MCC_PHYS_FULL;
}

/* 邻居状态（Sable VoxelNeighborhoodState）：按六个邻居是否整块，判断方块处在结构的面、棱、角还是内部 */
/* full：六个邻居是否整块，位 0..5 = -x +x -y +y -z +z */
static uint32_t side_neighborhood(vside *v, int32_t x, int32_t y, int32_t z, uint8_t *full) {
    bool all = true;
    int both = 0;
    const int d[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    uint8_t m = 0;
    for (int a = 0; a < 3; a++) {
        bool n = side_full(v, x - d[a][0], y - d[a][1], z - d[a][2]);
        bool p = side_full(v, x + d[a][0], y + d[a][1], z + d[a][2]);
        m |= (uint8_t)((n << (2 * a)) | (p << (2 * a + 1)));
        if (!n || !p) all = false;
        if (n && p) both++;
    }
    *full = m;
    if (all) return PHYS_VS_INTERIOR;
    if (both == 1) return PHYS_VS_EDGE;
    if (both == 0) return PHYS_VS_CORNER;
    return PHYS_VS_FACE;
}

/* 地形格的方块范围：x、z 落在本格内（避免相邻格重复生成接触） */
static bool cell_owns(const phys_cell *c, int32_t x, int32_t z) {
    if (c->cx == INT32_MIN) return true; /* 浮力查询用的虚拟全局地形侧 */
    return (x >> PHYS_CELL_SHIFT) == c->cx && (z >> PHYS_CELL_SHIFT) == c->cz;
}

/* ---------------- 线程局部缓冲 ---------------- */

typedef struct { int32_t s[3], d[3]; } block_pair;           /* static 侧方块、遍历侧方块（浮力按 6 个 int32 读，布局别动） */
typedef struct {
    const phys_collider *colS, *colD;
    uint8_t stS, stD;            /* 邻居状态，配对时已算好 */
    uint8_t fullS, fullD;        /* 六个邻居是否整块 */
} pair_meta;
/* 一次回调内的方块缓存：被查询侧的同一个方块会和遍历侧很多方块配对，碰撞体与邻居状态只算一次 */
typedef struct {
    int32_t x, y, z;
    uint32_t gen;
    const phys_collider *col;
    uint8_t st, full;
} memo_ent;
typedef struct {
    b3Vec3 normal;
    b3Vec3 point;
    float separation;
    uint32_t feature;
} raw_point;
typedef struct {
    b3Vec3 normal;
    float friction, restitution;
    uint32_t start, count;     /* 在 points 中的范围（按簇收集后重排） */
} cluster;

/* parry 的"规范形状"：整格方块沿着有整块邻居的方向无限延伸（再截到对方的范围），
 * 一整片平地的表层方块都变成同一块大板。两侧规范形状相同的方块对归为一组，只做一次盒子碰撞，
 * 再把接触点分回真正包含它的那对方块 */
typedef struct cgroup {
    int32_t rs[6], rd[6];        /* 两侧规范范围（方块坐标，延伸方向用 INT32_MIN/MAX 表示） */
    uint32_t first;              /* 第一对的下标（取代表方块） */
    uint32_t pt0, npts;          /* 在 gpts 中的接触点 */
    uint32_t sel;                /* 被某对方块认领的点（位） */
    b3Vec3 normal;               /* A 指向 B，A 局部系 */
} cgroup;
typedef struct gpoint {
    b3Vec3 p;                    /* A 局部系 */
    float sep;
    b3FeaturePair fp;
    uint32_t owner;              /* 认领它的方块对 */
} gpoint;

typedef struct scratch {
    MCC_VEC(block_pair) pairs;
    MCC_VEC(pair_meta) meta;
    memo_ent *memo;
    uint32_t memo_cap, memo_used, memo_gen;
    mcc_i32vec hits;
    MCC_VEC(raw_point) points;
    MCC_VEC(uint16_t) point_cluster;
    MCC_VEC(raw_point) sorted;
    cluster clusters[MAX_CLUSTERS];
    int ncluster;
    MCC_VEC(b3VoxelManifold) out;
    /* 整格大板分组 */
    MCC_VEC(struct cgroup) groups;
    MCC_VEC(uint32_t) pair_group;
    MCC_VEC(struct gpoint) gpts;
    uint32_t *gtab;
    uint32_t gtab_cap;
    MCC_VEC(uint32_t) canon_idx;
    /* 按速度放大的预判距离：超出默认距离的点，只留这一步里真可能撞上的（沿法线在靠近） */
    bool spec_filter;
    b3Vec3 vrel;          /* B 相对 A 的速度（A 局部系） */
    float rot_slack;      /* 转动带来的额外靠近速度上限 */
    float spec_base, dt;
} scratch;

static thread_local scratch t_s;

/* ---------------- 方块缓存与配对规则 ---------------- */

/* 碰撞体：流体、空碰撞箱都当作没有 */
static const phys_collider *solid_collider(vside *v, int32_t x, int32_t y, int32_t z) {
    const phys_collider *c = phys_collider_get(mcc_state_collider(side_state(v, x, y, z)));
    return c && !c->fluid && c->nboxes ? c : NULL;
}

static void memo_begin(scratch *sc) {
    if (!sc->memo) {
        sc->memo_cap = 1024;
        sc->memo = (memo_ent*)mcc_xcalloc(sc->memo_cap, sizeof *sc->memo);
    }
    if (++sc->memo_gen == 0) {
        memset(sc->memo, 0, sc->memo_cap * sizeof *sc->memo);
        sc->memo_gen = 1;
    }
    sc->memo_used = 0;
}

static const memo_ent *memo_get(scratch *sc, vside *v, int32_t x, int32_t y, int32_t z) {
    if (sc->memo_used * 2 >= sc->memo_cap) { /* 太满：换一张两倍大的空表（缓存丢了也只是重算） */
        free(sc->memo);
        sc->memo_cap *= 2;
        sc->memo = (memo_ent*)mcc_xcalloc(sc->memo_cap, sizeof *sc->memo);
        sc->memo_gen = 1;
        sc->memo_used = 0;
    }
    uint32_t mask = sc->memo_cap - 1;
    uint32_t i = (uint32_t)mcc_hash64(((uint64_t)(uint32_t)x << 32) ^ ((uint64_t)(uint32_t)z << 12) ^ (uint32_t)y) & mask;
    for (;; i = (i + 1) & mask) {
        memo_ent *e = &sc->memo[i];
        if (e->gen != sc->memo_gen) {
            e->x = x; e->y = y; e->z = z;
            e->gen = sc->memo_gen;
            e->col = solid_collider(v, x, y, z);
            e->full = 0;
            e->st = e->col ? (uint8_t)side_neighborhood(v, x, y, z, &e->full) : (uint8_t)PHYS_VS_INTERIOR;
            sc->memo_used++;
            return e;
        }
        if (e->x == x && e->y == y && e->z == z) return e;
    }
}

/* Sable can_ignore_collision（parry 体素对体素同一套规则）：
 * 内部方块不碰；面只和角碰；棱和棱、棱和角碰；角和什么都碰 */
MCC_INLINE bool can_ignore(uint32_t a, uint32_t b) {
    return a == PHYS_VS_INTERIOR || b == PHYS_VS_INTERIOR || (a == PHYS_VS_FACE && b != PHYS_VS_CORNER) ||
           (b == PHYS_VS_FACE && a != PHYS_VS_CORNER);
}

/* ---------------- 找方块对（find_collision_pairs / get_overlapping_nodes） ---------------- */

/* 查询另一侧在以 pos（方块坐标）为中心、dist 为半径的包围盒内有没有外露实心（liquid 时为流体）方块。
 * cancel_early 时只回答有无；否则把命中方块的坐标追加到 hits */
static bool overlapping(vside *o, const double pos[3], double dist, bool cancel_early, bool liquid, scratch *sc) {
    int32_t mn[3], mx[3];
    for (int i = 0; i < 3; i++) {
        mn[i] = (int32_t)floor(pos[i] - dist);
        mx[i] = (int32_t)floor(pos[i] + dist);
    }
    if (o->kind == PHYS_KIND_BODY) {
        /* 只认包围盒内的方块（与质量追踪给出的范围一致） */
        const phys_body *b = o->body;
        for (int i = 0; i < 3; i++) {
            mn[i] = MCC_MAX(mn[i], b->bmin[i]);
            mx[i] = MCC_MIN(mx[i], b->bmax[i]);
            if (mn[i] > mx[i]) return false;
        }
    }
    int kind = liquid ? MCC_OCC_LIQUID : MCC_OCC_SOLID;
    if (cancel_early) return mcc_cursor_box_any(&o->cur, kind, mn, mx);
    size_t from = sc->hits.len;
    mcc_cursor_box_collect(&o->cur, kind, mn, mx, &sc->hits);
    if (o->kind == PHYS_KIND_CELL && o->cell->cx != INT32_MIN) {
        /* 地形格只认本格的方块，相邻格各管各的，避免重复接触 */
        size_t w = from;
        for (size_t r = from; r < sc->hits.len; r += 3) {
            if (!cell_owns(o->cell, sc->hits.data[r], sc->hits.data[r + 2])) continue;
            sc->hits.data[w] = sc->hits.data[r];
            sc->hits.data[w + 1] = sc->hits.data[r + 1];
            sc->hits.data[w + 2] = sc->hits.data[r + 2];
            w += 3;
        }
        sc->hits.len = w;
    }
    return false;
}

typedef struct trav_ctx {
    vside *t;
    vside *o;
    b3Transform xf_to;
    float prediction;
    bool liquid;
    scratch *sc;
} trav_ctx;

/* 被遍历侧以 (cx, cy, cz)（方块坐标，可带小数）为中心、半边长 half 的节点：变换到另一侧，查询重叠 */
static bool node_overlaps(trav_ctx *tc, double cx, double cy, double cz, double half, bool cancel_early) {
    const vside *t = tc->t;
    b3Vec3 c = { (float)(cx - t->offset[0]), (float)(cy - t->offset[1]), (float)(cz - t->offset[2]) };
    b3Vec3 co = b3TransformPoint(tc->xf_to, c);
    double pos[3] = { co.x + tc->o->offset[0], co.y + tc->o->offset[1], co.z + tc->o->offset[2] };
    return overlapping(tc->o, pos, half * 1.7321 + tc->prediction, cancel_early, tc->liquid, tc->sc);
}

/* t：被遍历的刚体；o：被查询的一侧。xf_to：t 局部系 → o 局部系。
 * 遍历 t 的外露实心方块（只读区段位图），逐层剪枝：区段 → 4³ 小块 → 方块 */
size_t phys_find_pairs(vside *t, vside *o, b3Transform xf_to, float prediction, bool liquid, scratch *sc) {
    const phys_body *tb = t->body;
    sc->pairs.len = 0;
    sc->meta.len = 0;
    if (!liquid) memo_begin(sc);
    trav_ctx tc = { t, o, xf_to, prediction, liquid, sc };
    /* 根：整个包围盒的外接球，没有重叠直接返回（远离地面的结构、不在水里的浮力查询） */
    double ext = MCC_MAX(tb->bmax[0] - tb->bmin[0], MCC_MAX(tb->bmax[1] - tb->bmin[1], tb->bmax[2] - tb->bmin[2])) + 1;
    if (!node_overlaps(&tc, (tb->bmin[0] + tb->bmax[0] + 1) * 0.5, (tb->bmin[1] + tb->bmax[1] + 1) * 0.5,
                       (tb->bmin[2] + tb->bmax[2] + 1) * 0.5, ext * 0.5, true))
        return 0;
    for (int32_t cx = tb->bmin[0] >> 4; cx <= tb->bmax[0] >> 4; cx++)
        for (int32_t cz = tb->bmin[2] >> 4; cz <= tb->bmax[2] >> 4; cz++) {
            const mcc_chunk *ch = mcc_store_chunk(tb->blocks, cx, cz);
            if (!ch) continue;
            int32_t sy0 = MCC_MAX(tb->bmin[1] >> 4, ch->min_sy), sy1 = MCC_MIN(tb->bmax[1] >> 4, ch->min_sy + ch->nsec - 1);
            for (int32_t sy = sy0; sy <= sy1; sy++) {
                int si = sy - ch->min_sy;
                if (!(ch->solid_secs[si >> 6] >> (si & 63) & 1)) continue;
                int32_t bx = cx << 4, by = sy << 4, bz = cz << 4;
                if (!node_overlaps(&tc, bx + 8.0, by + 8.0, bz + 8.0, 8.0, true)) continue;
                const uint16_t(*rows)[16] = ch->sec[si]->phys->solid;
                for (int qy = 0; qy < 16; qy += 4)
                    for (int qz = 0; qz < 16; qz += 4) {
                        /* 这 4×16 行合起来，每 4 位对应一个 4³ 小块 */
                        uint32_t acc = 0;
                        for (int y = qy; y < qy + 4; y++)
                            for (int z = qz; z < qz + 4; z++) acc |= rows[y][z];
                        for (int qx = 0; qx < 16; qx += 4) {
                            if (!(acc >> qx & 15)) continue;
                            if (!node_overlaps(&tc, bx + qx + 2.0, by + qy + 2.0, bz + qz + 2.0, 2.0, true)) continue;
                            for (int y = qy; y < qy + 4; y++)
                                for (int z = qz; z < qz + 4; z++) {
                                    uint32_t r = (rows[y][z] >> qx) & 15;
                                    while (r) {
                                        int x = qx + __builtin_ctz(r);
                                        r &= r - 1;
                                        int32_t d[3] = { bx + x, by + y, bz + z };
                                        if (d[0] < tb->bmin[0] || d[1] < tb->bmin[1] || d[2] < tb->bmin[2] ||
                                            d[0] > tb->bmax[0] || d[1] > tb->bmax[1] || d[2] > tb->bmax[2])
                                            continue;
                                        /* 碰撞：遍历侧方块先判自身（没有碰撞箱、内部方块直接跳过），再按规则筛对方 */
                                        const phys_collider *colD = NULL;
                                        uint32_t stD = 0;
                                        uint8_t fullD = 0;
                                        if (!liquid) {
                                            colD = solid_collider(t, d[0], d[1], d[2]);
                                            if (!colD) continue;
                                            stD = side_neighborhood(t, d[0], d[1], d[2], &fullD);
                                            if (stD == PHYS_VS_INTERIOR) continue;
                                        }
                                        sc->hits.len = 0;
                                        node_overlaps(&tc, d[0] + 0.5, d[1] + 0.5, d[2] + 0.5, 0.5, false);
                                        for (size_t i = 0; i + 2 < sc->hits.len; i += 3) {
                                            block_pair p = { { sc->hits.data[i], sc->hits.data[i + 1], sc->hits.data[i + 2] },
                                                             { d[0], d[1], d[2] } };
                                            if (!liquid) {
                                                const memo_ent *e = memo_get(sc, o, p.s[0], p.s[1], p.s[2]);
                                                if (!e->col || can_ignore(e->st, stD)) continue;
                                                pair_meta m = { e->col, colD, e->st, (uint8_t)stD, e->full, fullD };
                                                mcc_vec_push(&sc->meta, m);
                                            }
                                            mcc_vec_push(&sc->pairs, p);
                                        }
                                    }
                                }
                        }
                    }
            }
        }
    return sc->pairs.len;
}

/* ---------------- 假接触剔除（is_interior_collision） ---------------- */

static bool inside_voxel(vside *v, const int32_t ignore[3], const double pos[3]) {
    int32_t b[3] = { (int32_t)floor(pos[0]), (int32_t)floor(pos[1]), (int32_t)floor(pos[2]) };
    if (b[0] == ignore[0] && b[1] == ignore[1] && b[2] == ignore[2]) return false;
    const phys_collider *c = phys_collider_get(mcc_state_collider(side_state(v, b[0], b[1], b[2])));
    if (!c || c->fluid) return false;
    float lx = (float)(pos[0] - b[0]), ly = (float)(pos[1] - b[1]), lz = (float)(pos[2] - b[2]);
    for (int i = 0; i < c->nboxes; i++) {
        const phys_box *bx = &c->boxes[i];
        if (lx >= bx->min[0] && lx <= bx->max[0] && ly >= bx->min[1] && ly <= bx->max[1] && lz >= bx->min[2] && lz <= bx->max[2])
            return true;
    }
    return false;
}

/* 规定：s = 被查询侧（Sable 的"静态侧"），d = 被遍历侧。接触计算统一在 Box3D 的 A 侧局部系 */
typedef struct pair_ctx {
    vside *a, *b;                /* Box3D 的 A、B */
    b3Transform xf_ba;           /* B 局部系 → A 局部系 */
    bool s_is_a;                 /* 静态侧是否为 A */
    float speculative;
} pair_ctx;

MCC_INLINE b3Vec3 box_center_local(const vside *v, const int32_t blk[3], b3Vec3 c) {
    return (b3Vec3){ (float)(blk[0] + (double)c.x - v->offset[0]), (float)(blk[1] + (double)c.y - v->offset[1]),
                     (float)(blk[2] + (double)c.z - v->offset[2]) };
}

static uint32_t feature_hash(const int32_t s[3], int bs, const int32_t d[3], int bd, b3FeaturePair fp) {
    uint64_t h = mcc_hash64(((uint64_t)(uint32_t)s[0] << 32) ^ (uint32_t)s[1] ^ ((uint64_t)(uint32_t)s[2] << 16));
    h = mcc_hash64(h ^ ((uint64_t)(uint32_t)d[0] << 32) ^ (uint32_t)d[1] ^ ((uint64_t)(uint32_t)d[2] << 21));
    h = mcc_hash64(h ^ ((uint64_t)bs << 8) ^ (uint64_t)bd ^ ((uint64_t)fp.owner1 << 40) ^ ((uint64_t)fp.index1 << 48) ^
                   ((uint64_t)fp.owner2 << 56) ^ ((uint64_t)fp.index2 << 24));
    return (uint32_t)h;
}

/* 一对方块的所有碰撞箱接触，结果追加到 sc->points（A 局部系），并记录所属簇 */
static void add_point(scratch *sc, b3Vec3 n, b3Vec3 p, float sep, uint32_t feat, float friction, float restitution);
extern _Atomic(uint64_t) g_prof[8];

static void collide_block_pair(const pair_ctx *pc, const block_pair *bp, const pair_meta *pm, scratch *sc) {
    const phys_collider *colS = pm->colS, *colD = pm->colD;
    const int32_t *blkA = pc->s_is_a ? bp->s : bp->d;
    const int32_t *blkB = pc->s_is_a ? bp->d : bp->s;
    const phys_collider *colA = pc->s_is_a ? colS : colD;
    const phys_collider *colB = pc->s_is_a ? colD : colS;
    float friction = SUBLEVEL_BASE_FRICTION * colA->friction * colB->friction;
    float restitution = MCC_MAX(colA->restitution, colB->restitution);

    for (int ia = 0; ia < colA->nboxes; ia++) {
        b3Vec3 cA = box_center_local(pc->a, blkA, colA->centers[ia]);
        for (int ib = 0; ib < colB->nboxes; ib++) {
            b3Vec3 cBl = box_center_local(pc->b, blkB, colB->centers[ib]);
            /* B 碰撞箱 → A 碰撞箱 的相对变换 */
            b3Transform xf = { b3Sub(b3TransformPoint(pc->xf_ba, cBl), cA), pc->xf_ba.q };
            b3LocalManifoldPoint pts[B3_MAX_MANIFOLD_POINTS];
            b3LocalManifold lm = {0};
            lm.points = pts;
            phys_collide_boxes(colA->halves[ia], colB->halves[ib], xf, pc->speculative, &lm);
            if (lm.pointCount == 0) continue;
            b3Vec3 n = lm.normal;                 /* A 指向 B */
            b3Vec3 nB = b3InvRotateVector(pc->xf_ba.q, b3Neg(n)); /* B 局部系，B 指向 A */
            for (int k = 0; k < lm.pointCount; k++) {
                if (lm.points[k].separation > pc->speculative) continue;
                b3Vec3 rel = lm.points[k].point;           /* 相对 A 碰撞箱中心 */
                b3Vec3 pA = b3Add(rel, cA);                /* A 局部系 */
                /* A 侧检查：点向 A 箱中心收缩后沿 A→B 法线外移 */
                if (pc->a->interior_check) {
                    float sc_ = pc->s_is_a ? INTERIOR_SCALE_A : INTERIOR_SCALE_B;
                    double off = pc->s_is_a ? INTERIOR_OFFSET_A : INTERIOR_OFFSET_B;
                    b3Vec3 q = b3Add(b3MulSV(sc_, rel), cA);
                    double w[3] = { q.x + pc->a->offset[0] + n.x * off, q.y + pc->a->offset[1] + n.y * off,
                                    q.z + pc->a->offset[2] + n.z * off };
                    if (inside_voxel(pc->a, blkA, w)) continue;
                }
                if (pc->b->interior_check) {
                    float sc_ = pc->s_is_a ? INTERIOR_SCALE_B : INTERIOR_SCALE_A;
                    double off = pc->s_is_a ? INTERIOR_OFFSET_B : INTERIOR_OFFSET_A;
                    b3Vec3 pB = b3InvTransformPoint(pc->xf_ba, pA);   /* B 局部系 */
                    b3Vec3 relB = b3Sub(pB, cBl);
                    b3Vec3 q = b3Add(b3MulSV(sc_, relB), cBl);
                    double w[3] = { q.x + pc->b->offset[0] + nB.x * off, q.y + pc->b->offset[1] + nB.y * off,
                                    q.z + pc->b->offset[2] + nB.z * off };
                    if (inside_voxel(pc->b, blkB, w)) continue;
                }
                uint32_t feat = pc->s_is_a ? feature_hash(bp->s, ia, bp->d, ib, lm.points[k].pair)
                                           : feature_hash(bp->d, ia, bp->s, ib, lm.points[k].pair);
                add_point(sc, n, pA, lm.points[k].separation, feat, friction, restitution);
            }
        }
    }
}

/* ---------------- 整格大板（parry CanonicalVoxelShape） ---------------- */

typedef struct canon_ctx {
    double domS[6], domD[6];     /* 两侧大板延伸截止范围（各自方块坐标，lo xyz, hi xyz） */
    b3Transform xf_ds;           /* D 局部 → S 局部 */
} canon_ctx;

/* 刚体包围盒（方块坐标）经 xf 变换到另一侧局部系，再加上对方偏移得到对方方块坐标的包围盒 */
static void bounds_to_other(const vside *from, const vside *to, b3Transform xf, double margin, double out[6]) {
    const phys_body *b = from->body;
    for (int i = 0; i < 3; i++) { out[i] = DBL_MAX; out[i + 3] = -DBL_MAX; }
    for (int k = 0; k < 8; k++) {
        b3Vec3 c = { (float)((k & 1 ? b->bmax[0] + 1 : b->bmin[0]) - from->offset[0]),
                     (float)((k & 2 ? b->bmax[1] + 1 : b->bmin[1]) - from->offset[1]),
                     (float)((k & 4 ? b->bmax[2] + 1 : b->bmin[2]) - from->offset[2]) };
        b3Vec3 q = b3TransformPoint(xf, c);
        double w[3] = { q.x + to->offset[0], q.y + to->offset[1], q.z + to->offset[2] };
        for (int i = 0; i < 3; i++) {
            out[i] = MCC_MIN(out[i], w[i] - margin);
            out[i + 3] = MCC_MAX(out[i + 3], w[i] + margin);
        }
    }
}

static void clip_own(const vside *v, double margin, double dom[6]) {
    if (v->kind != PHYS_KIND_BODY) return;
    const phys_body *b = v->body;
    for (int i = 0; i < 3; i++) {
        dom[i] = MCC_MAX(dom[i], b->bmin[i] - margin);
        dom[i + 3] = MCC_MIN(dom[i + 3], b->bmax[i] + 1 + margin);
    }
}

static void canon_init(canon_ctx *cc, const pair_ctx *pc) {
    vside *S = pc->s_is_a ? pc->a : pc->b, *D = pc->s_is_a ? pc->b : pc->a;
    cc->xf_ds = pc->s_is_a ? pc->xf_ba : b3InvertTransform(pc->xf_ba);
    const double margin = 1.0;
    /* 遍历侧 D 总是刚体 */
    bounds_to_other(D, S, cc->xf_ds, margin, cc->domS);
    clip_own(S, margin, cc->domS);
    if (S->kind == PHYS_KIND_BODY) {
        bounds_to_other(S, D, b3InvertTransform(cc->xf_ds), margin, cc->domD);
    } else {
        for (int i = 0; i < 3; i++) { cc->domD[i] = -DBL_MAX; cc->domD[i + 3] = DBL_MAX; }
    }
    clip_own(D, margin, cc->domD);
}

/* full 位 0..5 = -x +x -y +y -z +z 方向邻居是整块：该方向延伸 */
static void canon_range(const int32_t b[3], uint8_t full, int32_t r[6]) {
    for (int i = 0; i < 3; i++) {
        r[i] = full >> (2 * i) & 1 ? INT32_MIN : b[i];
        r[i + 3] = full >> (2 * i + 1) & 1 ? INT32_MAX : b[i];
    }
}

/* 规范范围 → 实际盒子（方块坐标 lo/hi），延伸方向截到 dom，且一定包住方块本身 */
static void canon_box(const int32_t r[6], const int32_t b[3], const double dom[6], double lo[3], double hi[3]) {
    for (int i = 0; i < 3; i++) {
        lo[i] = r[i] == INT32_MIN ? MCC_MIN((double)b[i], dom[i]) : b[i];
        hi[i] = r[i + 3] == INT32_MAX ? MCC_MAX((double)b[i] + 1, dom[i + 3]) : b[i] + 1.0;
    }
}

static uint32_t group_find(scratch *sc, const int32_t rs[6], const int32_t rd[6], uint32_t pair) {
    uint64_t h = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 6; i++) h = mcc_hash64(h ^ (uint32_t)rs[i] ^ ((uint64_t)(uint32_t)rd[i] << 32));
    uint32_t mask = sc->gtab_cap - 1;
    for (uint32_t i = (uint32_t)h & mask;; i = (i + 1) & mask) {
        uint32_t g = sc->gtab[i];
        if (g == UINT32_MAX) {
            cgroup cg;
            memset(&cg, 0, sizeof cg);
            memcpy(cg.rs, rs, sizeof cg.rs);
            memcpy(cg.rd, rd, sizeof cg.rd);
            cg.first = pair;
            mcc_vec_push(&sc->groups, cg);
            sc->gtab[i] = (uint32_t)sc->groups.len - 1;
            return sc->gtab[i];
        }
        const cgroup *cg = &sc->groups.data[g];
        if (!memcmp(cg->rs, rs, sizeof cg->rs) && !memcmp(cg->rd, rd, sizeof cg->rd)) return g;
    }
}

MCC_INLINE bool in_voxel(b3Vec3 p, const vside *v, const int32_t blk[3], float tol) {
    float x = (float)(blk[0] - v->offset[0]), y = (float)(blk[1] - v->offset[1]), z = (float)(blk[2] - v->offset[2]);
    return p.x >= x - tol && p.x <= x + 1 + tol && p.y >= y - tol && p.y <= y + 1 + tol && p.z >= z - tol && p.z <= z + 1 + tol;
}

/* 单位方块（中心 c，半边 0.5）沿 dir 的支撑点 */
MCC_INLINE b3Vec3 cube_support(b3Vec3 c, b3Vec3 dir) {
    return (b3Vec3){ c.x + (dir.x >= 0 ? 0.5f : -0.5f), c.y + (dir.y >= 0 ? 0.5f : -0.5f), c.z + (dir.z >= 0 ? 0.5f : -0.5f) };
}

/* 所有"两侧都是整格"的方块对：按规范形状分组碰撞 */
static void collide_canonical(const pair_ctx *pc, const uint32_t *idx, size_t n, scratch *sc) {
    if (!n) return;
    vside *S = pc->s_is_a ? pc->a : pc->b, *D = pc->s_is_a ? pc->b : pc->a;
    canon_ctx cc;
    canon_init(&cc, pc);

    sc->groups.len = 0;
    sc->gpts.len = 0;
    uint32_t need = 16;
    while (need < n * 2) need <<= 1;
    if (sc->gtab_cap < need) {
        free(sc->gtab);
        sc->gtab_cap = need;
        sc->gtab = (uint32_t*)mcc_xmalloc(sizeof(uint32_t) * need);
    }
    memset(sc->gtab, 0xFF, sizeof(uint32_t) * sc->gtab_cap);
    mcc_vec_reserve(&sc->pair_group, n);
    sc->pair_group.len = n;
    for (size_t k = 0; k < n; k++) {
        const block_pair *bp = &sc->pairs.data[idx[k]];
        const pair_meta *pm = &sc->meta.data[idx[k]];
        int32_t rs[6], rd[6];
        canon_range(bp->s, pm->fullS, rs);
        canon_range(bp->d, pm->fullD, rd);
        sc->pair_group.data[k] = group_find(sc, rs, rd, (uint32_t)k);
    }

    /* 每组一次盒子碰撞 */
    for (size_t g = 0; g < sc->groups.len; g++) {
        cgroup *cg = &sc->groups.data[g];
        const block_pair *bp = &sc->pairs.data[idx[cg->first]];
        double slo[3], shi[3], dlo[3], dhi[3];
        canon_box(cg->rs, bp->s, cc.domS, slo, shi);
        canon_box(cg->rd, bp->d, cc.domD, dlo, dhi);
        /* 盒子中心（各自局部系）与半尺寸 */
        b3Vec3 cS = { (float)((slo[0] + shi[0]) * 0.5 - S->offset[0]), (float)((slo[1] + shi[1]) * 0.5 - S->offset[1]),
                      (float)((slo[2] + shi[2]) * 0.5 - S->offset[2]) };
        b3Vec3 hS = { (float)((shi[0] - slo[0]) * 0.5), (float)((shi[1] - slo[1]) * 0.5), (float)((shi[2] - slo[2]) * 0.5) };
        b3Vec3 cD = { (float)((dlo[0] + dhi[0]) * 0.5 - D->offset[0]), (float)((dlo[1] + dhi[1]) * 0.5 - D->offset[1]),
                      (float)((dlo[2] + dhi[2]) * 0.5 - D->offset[2]) };
        b3Vec3 hD = { (float)((dhi[0] - dlo[0]) * 0.5), (float)((dhi[1] - dlo[1]) * 0.5), (float)((dhi[2] - dlo[2]) * 0.5) };
        b3Vec3 cA = pc->s_is_a ? cS : cD, hA = pc->s_is_a ? hS : hD;
        b3Vec3 cB = pc->s_is_a ? cD : cS, hB = pc->s_is_a ? hD : hS;
        b3Transform xf = { b3Sub(b3TransformPoint(pc->xf_ba, cB), cA), pc->xf_ba.q };
        b3LocalManifoldPoint pts[B3_MAX_MANIFOLD_POINTS];
        b3LocalManifold lm = { 0 };
        lm.points = pts;
        phys_collide_boxes(hA, hB, xf, pc->speculative, &lm);
        cg->pt0 = (uint32_t)sc->gpts.len;
        cg->normal = lm.normal;
        for (int k = 0; k < lm.pointCount; k++) {
            if (lm.points[k].separation > pc->speculative) continue;
            gpoint gp = { b3Add(lm.points[k].point, cA), lm.points[k].separation, lm.points[k].pair, UINT32_MAX };
            mcc_vec_push(&sc->gpts, gp);
        }
        cg->npts = (uint32_t)sc->gpts.len - cg->pt0;
    }

    /* 接触点分回真正包含它的方块对（两侧方块都要包含，留一点容差） */
    for (size_t k = 0; k < n; k++) {
        cgroup *cg = &sc->groups.data[sc->pair_group.data[k]];
        if (!cg->npts || cg->sel == (1u << cg->npts) - 1) continue;
        const block_pair *bp = &sc->pairs.data[idx[k]];
        const int32_t *blkA = pc->s_is_a ? bp->s : bp->d, *blkB = pc->s_is_a ? bp->d : bp->s;
        for (uint32_t j = 0; j < cg->npts; j++) {
            if (cg->sel >> j & 1) continue;
            gpoint *gp = &sc->gpts.data[cg->pt0 + j];
            float tol = 0.5f * fabsf(gp->sep) + 0.01f;
            if (!in_voxel(gp->p, pc->a, blkA, tol)) continue;
            if (!in_voxel(b3InvTransformPoint(pc->xf_ba, gp->p), pc->b, blkB, tol)) continue;
            cg->sel |= 1u << j;
            gp->owner = (uint32_t)k;
        }
    }

    for (size_t g = 0; g < sc->groups.len; g++) {
        const cgroup *cg = &sc->groups.data[g];
        b3Vec3 n_ = cg->normal;
        for (uint32_t j = 0; j < cg->npts; j++) {
            if (!(cg->sel >> j & 1)) continue;
            const gpoint *gp = &sc->gpts.data[cg->pt0 + j];
            const block_pair *bp = &sc->pairs.data[idx[gp->owner]];
            const pair_meta *pm = &sc->meta.data[idx[gp->owner]];
            const int32_t *blkA = pc->s_is_a ? bp->s : bp->d, *blkB = pc->s_is_a ? bp->d : bp->s;
            if (gp->sep < 0) {
                /* 穿透点：确认不是延伸出来的假几何给的法线（parry 同款检查） */
                b3Vec3 vA = { (float)(blkA[0] + 0.5 - pc->a->offset[0]), (float)(blkA[1] + 0.5 - pc->a->offset[1]),
                              (float)(blkA[2] + 0.5 - pc->a->offset[2]) };
                b3Vec3 vBl = { (float)(blkB[0] + 0.5 - pc->b->offset[0]), (float)(blkB[1] + 0.5 - pc->b->offset[1]),
                               (float)(blkB[2] + 0.5 - pc->b->offset[2]) };
                b3Vec3 sp1 = cube_support(vA, b3Neg(n_));
                b3Vec3 dirB = b3InvRotateVector(pc->xf_ba.q, n_);
                b3Vec3 sp2 = b3TransformPoint(pc->xf_ba, cube_support(vBl, dirB));
                if (!(b3Dot(b3Sub(sp2, sp1), b3Neg(n_)) < gp->sep)) continue;
            }
            const phys_collider *colA = pc->s_is_a ? pm->colS : pm->colD, *colB = pc->s_is_a ? pm->colD : pm->colS;
            float friction = SUBLEVEL_BASE_FRICTION * colA->friction * colB->friction;
            float restitution = MCC_MAX(colA->restitution, colB->restitution);
            uint32_t feat = pc->s_is_a ? feature_hash(bp->s, 0, bp->d, 0, gp->fp) : feature_hash(bp->d, 0, bp->s, 0, gp->fp);
            add_point(sc, n_, gp->p, gp->sep, feat, friction, restitution);
        }
    }
}

/* ---------------- 聚类与精简 ---------------- */

static void add_point(scratch *sc, b3Vec3 n, b3Vec3 p, float sep, uint32_t feat, float friction, float restitution) {
    if (sc->spec_filter && sep > sc->spec_base) {
        float closing = -b3Dot(sc->vrel, n) + sc->rot_slack;
        if (sep > closing * sc->dt * 1.2f + sc->spec_base) return;
    }
    int ci = -1;
    float best = -2.0f;
    int best_any = -1;
    for (int i = 0; i < sc->ncluster; i++) {
        cluster *c = &sc->clusters[i];
        float d = b3Dot(c->normal, n);
        if (d > best) { best = d; best_any = i; }
        if (d > CLUSTER_NORMAL_DOT && c->friction == friction && c->restitution == restitution) { ci = i; break; }
    }
    if (ci < 0) {
        if (sc->ncluster < MAX_CLUSTERS) {
            ci = sc->ncluster++;
            sc->clusters[ci] = (cluster){ n, friction, restitution, 0, 0 };
        } else {
            ci = best_any; /* 簇已满：并入法向最接近的簇 */
        }
    }
    raw_point rp = { n, p, sep, feat };
    mcc_vec_push(&sc->points, rp);
    mcc_vec_push(&sc->point_cluster, (uint16_t)ci);
    sc->clusters[ci].count++;
}

/* 从一簇点中选最多 4 个：最深点、离它最远点、两侧面积最大点（保持支撑面最大） */
static int reduce_cluster(const raw_point *pts, int n, b3Vec3 normal, int out_idx[4]) {
    if (n <= 4) {
        for (int i = 0; i < n; i++) out_idx[i] = i;
        return n;
    }
    int i0 = 0;
    for (int i = 1; i < n; i++)
        if (pts[i].separation < pts[i0].separation) i0 = i;
    int i1 = -1;
    float far = -1;
    for (int i = 0; i < n; i++) {
        b3Vec3 d = b3Sub(pts[i].point, pts[i0].point);
        d = b3Sub(d, b3MulSV(b3Dot(d, normal), normal));
        float l = b3Dot(d, d);
        if (l > far) { far = l; i1 = i; }
    }
    b3Vec3 e = b3Sub(pts[i1].point, pts[i0].point);
    int i2 = -1, i3 = -1;
    float amax = 0, amin = 0;
    for (int i = 0; i < n; i++) {
        float a = b3Dot(b3Cross(e, b3Sub(pts[i].point, pts[i0].point)), normal);
        if (a > amax) { amax = a; i2 = i; }
        if (a < amin) { amin = a; i3 = i; }
    }
    int k = 0;
    out_idx[k++] = i0;
    if (i1 != i0) out_idx[k++] = i1;
    if (i2 >= 0) out_idx[k++] = i2;
    if (i3 >= 0) out_idx[k++] = i3;
    return k;
}

static uint64_t cluster_key(const cluster *c) {
    int32_t qx = (int32_t)lrintf(c->normal.x * 16), qy = (int32_t)lrintf(c->normal.y * 16), qz = (int32_t)lrintf(c->normal.z * 16);
    uint32_t fb, rb;
    memcpy(&fb, &c->friction, 4);
    memcpy(&rb, &c->restitution, 4);
    return mcc_hash64(((uint64_t)(uint8_t)qx << 16) ^ ((uint64_t)(uint8_t)qy << 8) ^ (uint64_t)(uint8_t)qz ^ ((uint64_t)fb << 24) ^
                      ((uint64_t)rb << 40));
}

static const b3VoxelManifold *emit(scratch *sc, int *count) {
    sc->out.len = 0;
    if (!sc->points.len) { *count = 0; return NULL; }
    /* 按簇重排点 */
    uint32_t off = 0;
    for (int i = 0; i < sc->ncluster; i++) {
        sc->clusters[i].start = off;
        off += sc->clusters[i].count;
        sc->clusters[i].count = 0;
    }
    mcc_vec_reserve(&sc->sorted, sc->points.len);
    sc->sorted.len = sc->points.len;
    for (size_t i = 0; i < sc->points.len; i++) {
        cluster *c = &sc->clusters[sc->point_cluster.data[i]];
        sc->sorted.data[c->start + c->count++] = sc->points.data[i];
    }
    for (int i = 0; i < sc->ncluster; i++) {
        cluster *c = &sc->clusters[i];
        if (!c->count) continue;
        int idx[4];
        int k = reduce_cluster(sc->sorted.data + c->start, (int)c->count, c->normal, idx);
        b3VoxelManifold m;
        memset(&m, 0, sizeof m);
        m.normal = c->normal;
        m.pointCount = k;
        for (int j = 0; j < k; j++) {
            const raw_point *rp = &sc->sorted.data[c->start + idx[j]];
            m.points[j] = rp->point;
            m.separations[j] = rp->separation;
            m.featureIds[j] = rp->feature;
        }
        m.key = cluster_key(c);
        m.friction = c->friction;
        m.restitution = c->restitution;
        m.tangentVelocity = b3Vec3_zero;
        mcc_vec_push(&sc->out, m);
    }
    *count = (int)sc->out.len;
    return sc->out.data;
}

/* ---------------- 体素 vs 凸形状（static_world_vs_collider） ---------------- */

static void collide_voxel_convex(vside *a, const b3VoxelQuery *q, b3Transform xf_ba, scratch *sc) {
    /* B 的包围盒变换到 A 局部系 */
    b3AABB lb;
    if (q->typeB == b3_sphereShape) {
        b3Vec3 r = { q->sphereB.radius, q->sphereB.radius, q->sphereB.radius };
        lb = (b3AABB){ b3Sub(q->sphereB.center, r), b3Add(q->sphereB.center, r) };
    } else if (q->typeB == b3_capsuleShape) {
        b3Vec3 r = { q->capsuleB.radius, q->capsuleB.radius, q->capsuleB.radius };
        lb = (b3AABB){ b3Sub(b3Min(q->capsuleB.center1, q->capsuleB.center2), r), b3Add(b3Max(q->capsuleB.center1, q->capsuleB.center2), r) };
    } else {
        lb = q->hullB->aabb;
    }
    b3AABB box = b3AABB_Transform(xf_ba, lb);
    float margin = q->speculativeDistance + 0.1f;
    int32_t mn[3], mx[3];
    float lo[3] = { box.lowerBound.x, box.lowerBound.y, box.lowerBound.z };
    float hi[3] = { box.upperBound.x, box.upperBound.y, box.upperBound.z };
    for (int i = 0; i < 3; i++) {
        mn[i] = (int32_t)floor(lo[i] - margin + a->offset[i]);
        mx[i] = (int32_t)floor(hi[i] + margin + a->offset[i]);
        if (a->kind == PHYS_KIND_BODY) {
            mn[i] = MCC_MAX(mn[i], a->body->bmin[i]);
            mx[i] = MCC_MIN(mx[i], a->body->bmax[i]);
        }
    }
    if ((int64_t)(mx[0] - mn[0] + 1) * (mx[1] - mn[1] + 1) * (mx[2] - mn[2] + 1) > 1 << 20) return; /* 异常巨大，放弃 */
    for (int32_t x = mn[0]; x <= mx[0]; x++)
        for (int32_t y = mn[1]; y <= mx[1]; y++)
            for (int32_t z = mn[2]; z <= mx[2]; z++) {
                if (a->kind == PHYS_KIND_CELL && !cell_owns(a->cell, x, z)) continue;
                const phys_collider *col = phys_collider_get(mcc_state_collider(side_state(a, x, y, z)));
                if (!col || col->fluid) continue;
                int32_t blk[3] = { x, y, z };
                for (int ib = 0; ib < col->nboxes; ib++) {
                    b3Vec3 c = box_center_local(a, blk, col->centers[ib]);
                    b3Transform xf = { b3Sub(xf_ba.p, c), xf_ba.q };
                    b3LocalManifoldPoint pts[B3_MAX_MANIFOLD_POINTS];
                    b3LocalManifold lm = {0};
                    lm.points = pts;
                    if (q->typeB == b3_hullShape) {
                        b3SATCache cache = {0};
                        b3CollideHulls(&lm, B3_MAX_MANIFOLD_POINTS, &col->hulls[ib].base, q->hullB, xf, &cache);
                    } else if (q->typeB == b3_sphereShape) {
                        b3SimplexCache cache = {0};
                        b3CollideHullAndSphere(&lm, B3_MAX_MANIFOLD_POINTS, &col->hulls[ib].base, &q->sphereB, xf, &cache);
                    } else {
                        b3SimplexCache cache = {0};
                        b3CollideHullAndCapsule(&lm, B3_MAX_MANIFOLD_POINTS, &col->hulls[ib].base, &q->capsuleB, xf, &cache);
                    }
                    int32_t none[3] = { 0, 0, 0 };
                    for (int k = 0; k < lm.pointCount; k++) {
                        if (lm.points[k].separation > q->speculativeDistance) continue;
                        b3Vec3 p = b3Add(lm.points[k].point, c);
                        uint32_t feat = feature_hash(blk, ib, none, 0, lm.points[k].pair);
                        add_point(sc, lm.normal, p, lm.points[k].separation, feat, SUBLEVEL_BASE_FRICTION * col->friction, col->restitution);
                    }
                }
            }
}

/* ---------------- 回调入口 ---------------- */
_Atomic(uint64_t) g_prof[8];


static void scratch_reset(scratch *sc) {
    sc->spec_filter = false;
    sc->points.len = 0;
    sc->point_cluster.len = 0;
    sc->ncluster = 0;
}

static uint64_t body_volume(const phys_body *b) {
    return (uint64_t)(b->bmax[0] - b->bmin[0] + 1) * (uint64_t)(b->bmax[1] - b->bmin[1] + 1) * (uint64_t)(b->bmax[2] - b->bmin[2] + 1);
}

/* 这一步里两侧最多能互相靠近多少：
 * Box3D 的连续碰撞对体素形状关掉了，只靠预判接触（隔一点距离就先生成接触点，求解器保证不会越过）。
 * 默认预判距离只有 0.02 格，结构高速落下时一步能走一格多，第一次碰到时已经嵌进去超过半格，
 * 方块对的分离轴会选反方向，把结构往里推，结果穿过地面。按相对速度放大预判距离就能挡住。 */
static float side_speed_bound(const vside *v, b3Vec3 *lin) {
    *lin = b3Vec3_zero;
    if (v->kind != PHYS_KIND_BODY) return 0;
    const phys_body *b = v->body;
    *lin = b3Body_GetLinearVelocity(b->body);
    float r = 0;
    for (int k = 0; k < 3; k++) {
        float e = (float)fmax(fabs(b->bmin[k] - b->com[k]), fabs(b->bmax[k] + 1 - b->com[k]));
        r += e * e;
    }
    return b3Length(b3Body_GetAngularVelocity(b->body)) * sqrtf(r);
}

static float motion_margin(phys_scene *s, const vside *a, const vside *b, b3Quat qa, scratch *sc) {
    float dt = phys_scene_step_dt(s);
    if (dt <= 0) return 0;
    b3Vec3 la, lb;
    float rot = side_speed_bound(a, &la) + side_speed_bound(b, &lb);
    float g = phys_scene_gravity(s) * dt;
    sc->vrel = b3InvRotateVector(qa, b3Sub(lb, la));
    sc->rot_slack = rot + g;
    sc->dt = dt;
    float speed = b3Length(b3Sub(la, lb)) + rot + g;
    /* 一步靠近不到 0.4 格时不会穿（嵌入不到半格，分离轴方向还是对的），保持默认，免得改变慢速接触的手感 */
    float reach = speed * dt * 1.2f;
    return reach < 0.4f ? 0.f : fminf(reach, 4.0f);
}

const b3VoxelManifold *phys_voxel_manifolds(const b3VoxelQuery *q_in, int *count, void *ctx) {
    phys_scene *scene = (phys_scene*)ctx;
    /* Box3D 回收接触时允许两侧相对移动到回收距离才重新生成。
     * 方块结构在这段移动里可能有新的方块对碰上，所以生成时就把回收距离内的点都收进来 */
    b3VoxelQuery qv = *q_in;
    qv.speculativeDistance = MCC_MAX(qv.speculativeDistance, B3_CONTACT_RECYCLE_DISTANCE);
    const b3VoxelQuery *q = &qv;
    bool stats = g_stats_on;
    uint64_t t_start = stats ? mcc_nanotime() : 0;
    scratch *sc = &t_s;
    scratch_reset(sc);
    *count = 0;

    vside a, b;
    make_side(&a, q->userA);
    if (!side_ready(&a)) return NULL;
    b3Transform xf_ba = b3InvMulWorldTransforms(q->transformA, q->transformB);

    if (q->typeB != b3_voxelShape) {
        collide_voxel_convex(&a, q, xf_ba, sc);
        return emit(sc, count);
    }

    make_side(&b, q->userB);
    if (!side_ready(&b)) return NULL;
    sc->spec_base = qv.speculativeDistance;
    float margin = motion_margin(scene, &a, &b, q->transformA.q, sc);
    sc->spec_filter = margin > sc->spec_base;
    qv.speculativeDistance = MCC_MAX(qv.speculativeDistance, margin);

    pair_ctx pc = { &a, &b, xf_ba, true, q->speculativeDistance };
    vside *trav, *other;
    b3Transform xf_to;
    if (b.kind == PHYS_KIND_CELL || (a.kind == PHYS_KIND_BODY && b.kind == PHYS_KIND_BODY && body_volume(a.body) <= body_volume(b.body))) {
        /* 遍历 A，查询 B：静态侧为 B */
        trav = &a;
        other = &b;
        xf_to = b3InvertTransform(xf_ba);   /* A → B */
        pc.s_is_a = false;
    } else if (a.kind == PHYS_KIND_CELL) {
        trav = &b;
        other = &a;
        xf_to = xf_ba;                       /* B → A */
        pc.s_is_a = true;
    } else {
        trav = &b;
        other = &a;
        xf_to = xf_ba;
        pc.s_is_a = true;
    }
    uint64_t ta = stats ? mcc_nanotime() : 0;
    size_t np = phys_find_pairs(trav, other, xf_to, q->speculativeDistance, false, sc);
    uint64_t tb = stats ? mcc_nanotime() : 0;
    /* 两侧都是整格的走大板分组，其余（台阶、楼梯等）逐对逐碰撞箱 */
    sc->canon_idx.len = 0;
    for (size_t i = 0; i < np; i++) {
        const pair_meta *pm = &sc->meta.data[i];
        if (g_canonical && pm->colS->unit && pm->colD->unit) mcc_vec_push(&sc->canon_idx, (uint32_t)i);
        else collide_block_pair(&pc, &sc->pairs.data[i], pm, sc);
    }
    collide_canonical(&pc, sc->canon_idx.data, sc->canon_idx.len, sc);
    uint64_t tc = stats ? mcc_nanotime() : 0;
    const b3VoxelManifold *res = emit(sc, count);
    if (stats) {
        uint64_t td = mcc_nanotime();
        atomic_fetch_add(&g_prof[0], tb - ta);
        atomic_fetch_add(&g_prof[1], tc - tb);
        atomic_fetch_add(&g_prof[2], td - tc);
        atomic_fetch_add(&g_prof[3], sc->points.len);
        phys_scene_count(scene, (uint64_t)*count, np);
        phys_scene_count_time(scene, mcc_nanotime() - t_start);
    }
    return res;
}

/* 浮力用：遍历刚体八叉树，找与地形液体重叠的方块对（与 Sable compute_buoyancy 一致，prediction 0） */
size_t phys_find_liquid_pairs(phys_body *body, int32_t **out_pairs) {
    scratch *sc = &t_s;
    vside t, o;
    make_side(&t, body);
    if (!side_ready(&t)) return 0;
    /* 地形查询侧：用一个"原点在世界原点"的虚拟地形侧，坐标即世界坐标 */
    phys_cell world = {0};
    world.kind = PHYS_KIND_CELL;
    world.scene = body->scene;
    world.cx = INT32_MIN; /* 虚拟地形侧不做格归属过滤 */
    make_side(&o, &world);
    if (!side_ready(&o)) return 0;
    b3WorldTransform xf = b3Body_GetTransform(body->body);
    /* t 局部 → 世界：位置 xf.p（双精度）拆成整数基准 + 小数部分，避免单精度丢失 */
    double base[3] = { floor(xf.p.x), floor(xf.p.y), floor(xf.p.z) };
    o.offset[0] = base[0];
    o.offset[1] = base[1];
    o.offset[2] = base[2];
    b3Transform to = { { (float)(xf.p.x - base[0]), (float)(xf.p.y - base[1]), (float)(xf.p.z - base[2]) }, xf.q };
    size_t n = phys_find_pairs(&t, &o, to, 0.0f, true, sc);
    *out_pairs = (int32_t *)sc->pairs.data;
    return n;
}
