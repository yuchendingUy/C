/*
 * 体素接触生成（移植自 Sable sable_rapier/algo.rs + dispatcher.rs）
 *
 * 流程：
 *   1. 找方块对：遍历一侧刚体的八叉树，节点中心变换到另一侧，查询另一侧八叉树（或地形 64³ 八叉树）
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
void phys_scene_count_time(phys_scene *s, uint64_t ns);

/* ---------------- 体素侧 ---------------- */

typedef struct vside {
    int kind;                 /* PHYS_KIND_BODY / PHYS_KIND_CELL */
    phys_body *body;          /* kind == BODY */
    phys_cell *cell;          /* kind == CELL */
    double offset[3];         /* 方块坐标 → 形状局部系：local = block - offset */
    bool interior_check;      /* 多方块刚体或地形才做内部接缝剔除 */
    const mcc_hmap *map;      /* 区块表 */
    uint64_t ckey;            /* 区段查找缓存：上一次命中的区段 */
    const phys_section *csec;
    uint64_t okey;            /* 八叉树块查找缓存 */
    const phys_octree_chunk *ochunk;
} vside;

const mcc_hmap *phys_scene_sections(phys_scene *s);

static void make_side(vside *v, void *user) {
    memset(v, 0, sizeof *v);
    v->kind = *(int *)user;
    v->ckey = UINT64_MAX;
    v->okey = UINT64_MAX;
    if (v->kind == PHYS_KIND_BODY) {
        v->body = user;
        memcpy(v->offset, v->body->com, sizeof v->offset);
        const int32_t *a = v->body->bmin, *b = v->body->bmax;
        v->interior_check = a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
    } else {
        v->cell = user;
        memcpy(v->offset, v->cell->origin, sizeof v->offset);
        v->interior_check = true;
    }
    phys_scene *sc = v->kind == PHYS_KIND_BODY ? v->body->scene : v->cell->scene;
    v->map = (v->kind == PHYS_KIND_BODY && v->body->own_chunks) ? v->body->own_chunks : phys_scene_sections(sc);
}

static phys_scene *side_scene(const vside *v) { return v->kind == PHYS_KIND_BODY ? v->body->scene : v->cell->scene; }

static uint32_t side_cell(vside *v, int32_t x, int32_t y, int32_t z) {
    uint64_t key = phys_key3(x >> 4, y >> 4, z >> 4);
    if (key != v->ckey) {
        v->ckey = key;
        v->csec = mcc_hmap_get(v->map, key);
    }
    if (!v->csec) return 0;
    return v->csec->cells[(x & 15) + ((z & 15) << 4) + ((y & 15) << 8)];
}

static const phys_octree_chunk *side_ochunk(vside *v, int32_t ox, int32_t oy, int32_t oz) {
    uint64_t key = phys_key3(ox, oy, oz);
    if (key != v->okey) {
        v->okey = key;
        v->ochunk = phys_scene_octree_chunk(side_scene(v), ox, oy, oz);
    }
    return v->ochunk;
}

static bool side_ready(const vside *v) {
    return v->kind == PHYS_KIND_CELL || (v->body->has_octree && v->body->has_com && v->body->has_bounds);
}

/* 地形格的方块范围：x、z 落在本格内（避免相邻格重复生成接触） */
static bool cell_owns(const phys_cell *c, int32_t x, int32_t z) {
    if (c->cx == INT32_MIN) return true; /* 浮力查询用的虚拟全局地形侧 */
    return (x >> PHYS_CELL_SHIFT) == c->cx && (z >> PHYS_CELL_SHIFT) == c->cz;
}

/* ---------------- 线程局部缓冲 ---------------- */

typedef struct { int32_t s[3], d[3]; } block_pair;           /* static 侧方块、遍历侧方块 */
typedef struct { uint32_t index; int32_t depth; int32_t min[3]; } trav_node;
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

typedef struct scratch {
    MCC_VEC(block_pair) pairs;
    MCC_VEC(trav_node) stack;
    MCC_VEC(int32_t) hits;
    MCC_VEC(raw_point) points;
    MCC_VEC(uint16_t) point_cluster;
    MCC_VEC(raw_point) sorted;
    cluster clusters[MAX_CLUSTERS];
    int ncluster;
    MCC_VEC(b3VoxelManifold) out;
} scratch;

static _Thread_local scratch t_s;

/* ---------------- 找方块对（find_collision_pairs / get_overlapping_nodes） ---------------- */

/* 查询另一侧在以 pos（方块坐标）为中心、dist 为半径范围内有没有方块。
 * cancel_early 时只回答有无；否则把命中的（按 log2 粒度的）方块坐标追加到 hits */
static bool overlapping(vside *o, const double pos[3], double dist, bool cancel_early, bool liquid, scratch *sc) {
    int lg = (int)floor(log2(dist * 2.0));
    if (lg < 0) lg = 0;
    int32_t mn[3], mx[3];
    for (int i = 0; i < 3; i++) {
        mn[i] = (int32_t)floor(pos[i] - dist);
        mx[i] = (int32_t)floor(pos[i] + dist);
    }
    if (o->kind == PHYS_KIND_BODY) {
        const phys_body *b = o->body;
        if (lg > b->octree.log_size) lg = b->octree.log_size;
        int32_t lo[3], hi[3];
        for (int i = 0; i < 3; i++) {
            lo[i] = MCC_MAX((mn[i] - b->bmin[i]) >> lg, 0);
            hi[i] = (mx[i] - b->bmin[i]) >> lg;
        }
        for (int32_t x = lo[0]; x <= hi[0]; x++)
            for (int32_t y = lo[1]; y <= hi[1]; y++)
                for (int32_t z = lo[2]; z <= hi[2]; z++) {
                    if (phys_octree_query(&b->octree, x << lg, y << lg, z << lg, lg) > PHYS_OCTREE_EMPTY) {
                        if (cancel_early) return true;
                        mcc_vec_push(&sc->hits, (x << lg) + b->bmin[0]);
                        mcc_vec_push(&sc->hits, (y << lg) + b->bmin[1]);
                        mcc_vec_push(&sc->hits, (z << lg) + b->bmin[2]);
                    }
                }
        return false;
    }
    /* 地形：遍历覆盖范围内的 64³ 八叉树块 */
    const int OS = PHYS_OCTREE_CHUNK_SHIFT;
    if (lg > OS) lg = OS;
    for (int32_t ox = mn[0] >> OS; ox <= mx[0] >> OS; ox++)
        for (int32_t oy = mn[1] >> OS; oy <= mx[1] >> OS; oy++)
            for (int32_t oz = mn[2] >> OS; oz <= mx[2] >> OS; oz++) {
                const phys_octree_chunk *oc = side_ochunk(o, ox, oy, oz);
                if (!oc) continue;
                const phys_octree *t = liquid ? &oc->liquid : &oc->solid;
                if (phys_octree_is_empty(t)) continue;
                for (int32_t x = mn[0] >> lg; x <= mx[0] >> lg; x++)
                    for (int32_t y = mn[1] >> lg; y <= mx[1] >> lg; y++)
                        for (int32_t z = mn[2] >> lg; z <= mx[2] >> lg; z++) {
                            if (phys_octree_query(t, (x << lg) - (ox << OS), (y << lg) - (oy << OS),
                                                  (z << lg) - (oz << OS), lg) > PHYS_OCTREE_EMPTY) {
                                if (cancel_early) return true;
                                int32_t bx = x << lg, bz = z << lg;
                                if (!cell_owns(o->cell, bx, bz)) continue;
                                mcc_vec_push(&sc->hits, bx);
                                mcc_vec_push(&sc->hits, y << lg);
                                mcc_vec_push(&sc->hits, bz);
                            }
                        }
            }
    return false;
}

/* t：被遍历的刚体（必须有八叉树）；o：被查询的一侧。xf_to：t 局部系 → o 局部系 */
size_t phys_find_pairs(const vside *t, vside *o, b3Transform xf_to, float prediction, bool liquid, scratch *sc) {
    const phys_body *tb = t->body;
    const phys_octree *oct = &tb->octree;
    sc->pairs.len = 0;
    sc->stack.len = 0;
    trav_node root = { 0, 0, { 0, 0, 0 } };
    mcc_vec_push(&sc->stack, root);
    /* 八叉树原点（tb->bmin）在 t 局部系中的位置 */
    b3Vec3 base = { (float)(tb->bmin[0] - t->offset[0]), (float)(tb->bmin[1] - t->offset[1]), (float)(tb->bmin[2] - t->offset[2]) };
    while (sc->stack.len) {
        trav_node e = sc->stack.data[--sc->stack.len];
        int32_t node = oct->buf[e.index];
        if (node == 0) continue;
        int32_t size = 1 << (oct->log_size - e.depth);
        float half = (float)size * 0.5f;
        b3Vec3 c = { base.x + (float)e.min[0] + half, base.y + (float)e.min[1] + half, base.z + (float)e.min[2] + half };
        b3Vec3 co = b3TransformPoint(xf_to, c);
        double pos[3] = { co.x + o->offset[0], co.y + o->offset[1], co.z + o->offset[2] };
        double radius = (double)half * 1.7321 + prediction;
        if (node > 0) {
            if (!overlapping(o, pos, radius, true, liquid, sc)) continue;
            int32_t h = size / 2;
            for (int i = 0; i < 8; i++) {
                trav_node ch = { (uint32_t)(node + i), e.depth + 1,
                                 { e.min[0] + (i & 1) * h, e.min[1] + ((i >> 1) & 1) * h, e.min[2] + ((i >> 2) & 1) * h } };
                mcc_vec_push(&sc->stack, ch);
            }
        } else {
            sc->hits.len = 0;
            overlapping(o, pos, radius, false, liquid, sc);
            for (size_t i = 0; i + 2 < sc->hits.len; i += 3) {
                block_pair p = { { sc->hits.data[i], sc->hits.data[i + 1], sc->hits.data[i + 2] },
                                 { e.min[0] + tb->bmin[0], e.min[1] + tb->bmin[1], e.min[2] + tb->bmin[2] } };
                mcc_vec_push(&sc->pairs, p);
            }
        }
    }
    return sc->pairs.len;
}

/* ---------------- 假接触剔除（is_interior_collision） ---------------- */

static bool inside_voxel(vside *v, const int32_t ignore[3], const double pos[3]) {
    int32_t b[3] = { (int32_t)floor(pos[0]), (int32_t)floor(pos[1]), (int32_t)floor(pos[2]) };
    if (b[0] == ignore[0] && b[1] == ignore[1] && b[2] == ignore[2]) return false;
    uint32_t cell = side_cell(v, b[0], b[1], b[2]);
    const phys_collider *c = phys_collider_get(phys_cell_collider(cell));
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

static void collide_block_pair(const pair_ctx *pc, const block_pair *bp, scratch *sc) {
    vside *S = pc->s_is_a ? pc->a : pc->b;
    vside *D = pc->s_is_a ? pc->b : pc->a;
    uint32_t cs = side_cell(S, bp->s[0], bp->s[1], bp->s[2]);
    const phys_collider *colS = phys_collider_get(phys_cell_collider(cs));
    if (!colS || colS->fluid || !colS->nboxes) return;
    uint32_t cd = side_cell(D, bp->d[0], bp->d[1], bp->d[2]);
    const phys_collider *colD = phys_collider_get(phys_cell_collider(cd));
    if (!colD || colD->fluid || !colD->nboxes) return;
    uint32_t stS = phys_cell_state(cs), stD = phys_cell_state(cd);
    /* can_ignore_collision */
    if ((stS == PHYS_VS_FACE && stD == PHYS_VS_FACE) || stS == PHYS_VS_INTERIOR || stD == PHYS_VS_INTERIOR ||
        (stS == PHYS_VS_EDGE && stD == PHYS_VS_FACE) || (stS == PHYS_VS_FACE && stD == PHYS_VS_EDGE))
        return;

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

/* ---------------- 聚类与精简 ---------------- */

static void add_point(scratch *sc, b3Vec3 n, b3Vec3 p, float sep, uint32_t feat, float friction, float restitution) {
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
                uint32_t cell = side_cell(a, x, y, z);
                const phys_collider *col = phys_collider_get(phys_cell_collider(cell));
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
_Atomic uint64_t g_prof[8];


static void scratch_reset(scratch *sc) {
    sc->points.len = 0;
    sc->point_cluster.len = 0;
    sc->ncluster = 0;
}

static uint64_t body_volume(const phys_body *b) {
    return (uint64_t)(b->bmax[0] - b->bmin[0] + 1) * (uint64_t)(b->bmax[1] - b->bmin[1] + 1) * (uint64_t)(b->bmax[2] - b->bmin[2] + 1);
}

const b3VoxelManifold *phys_voxel_manifolds(const b3VoxelQuery *q, int *count, void *ctx) {
    phys_scene *scene = ctx;
    uint64_t t_start = mcc_nanotime();
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
    uint64_t ta = mcc_nanotime();
    size_t np = phys_find_pairs(trav, other, xf_to, q->speculativeDistance, false, sc);
    uint64_t tb = mcc_nanotime();
    for (size_t i = 0; i < np; i++) collide_block_pair(&pc, &sc->pairs.data[i], sc);
    uint64_t tc = mcc_nanotime();
    const b3VoxelManifold *res = emit(sc, count);
    uint64_t td = mcc_nanotime();
    atomic_fetch_add(&g_prof[0], tb - ta);
    atomic_fetch_add(&g_prof[1], tc - tb);
    atomic_fetch_add(&g_prof[2], td - tc);
    atomic_fetch_add(&g_prof[3], sc->points.len);
    phys_scene_count(scene, (uint64_t)*count, np);
    phys_scene_count_time(scene, mcc_nanotime() - t_start);
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
    o.kind = PHYS_KIND_CELL;
    o.body = NULL;
    o.cell = &world;
    o.offset[0] = o.offset[1] = o.offset[2] = 0;
    o.interior_check = true;
    b3WorldTransform xf = b3Body_GetTransform(body->body);
    /* t 局部 → 世界：位置 xf.p（双精度）拆成整数基准 + 小数部分，避免单精度丢失 */
    double base[3] = { floor(xf.p.x), floor(xf.p.y), floor(xf.p.z) };
    o.offset[0] = base[0];
    o.offset[1] = base[1];
    o.offset[2] = base[2];
    b3Transform to = { { (float)(xf.p.x - base[0]), (float)(xf.p.y - base[1]), (float)(xf.p.z - base[2]) }, xf.q };
    /* 虚拟地形侧不做格归属过滤（cell_owns 对 cx == INT32_MIN 恒真） */
    world.cx = INT32_MIN;
    size_t n = phys_find_pairs(&t, &o, to, 0.0f, true, sc);
    *out_pairs = (int32_t *)sc->pairs.data;
    return n;
}
