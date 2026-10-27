/*
 * 浮力与水阻力（移植自 Sable sable_rapier/buoyancy.rs）
 *
 * 每个游戏刻对每个子世界：找出与地形液体重叠的方块对，
 *   - 阻力：在采样点施加 -速度 × 1.7 × 重叠体积
 *   - 浮力：在采样点施加向上 10.5 × 重叠体积 × 方块浮力体积
 * 小结构（包围盒三边和 < 10）每个方块用 8 个 0.25 的子采样点，否则用整块 0.5。
 * 合力与合力矩存进刚体，每个物理子步重复施加。
 */
#include "scene.h"
#include <math.h>

size_t phys_find_liquid_pairs(phys_body *body, int32_t **out_pairs);

typedef struct {
    b3WorldTransform xf;
    b3Vec3 v, w;
    b3Pos com;            /* 世界坐标质心（刚体原点即质心） */
    b3Vec3 force, torque; /* 累计 */
} buoy_ctx;

/* 以 point 为中心、半边长 size 的立方体与方块 [s, s+1] 的重叠体积 */
static double overlap_volume(const double p[3], double size, const int32_t s[3]) {
    double vol = 1.0;
    for (int i = 0; i < 3; i++) {
        double lo = MCC_MAX(p[i] - size, (double)s[i]);
        double hi = MCC_MIN(p[i] + size, (double)s[i] + 1.0);
        if (hi <= lo) return 0.0;
        vol *= hi - lo;
    }
    return vol;
}

static void add_force_at(buoy_ctx *c, const double p[3], b3Vec3 f) {
    c->force = b3Add(c->force, f);
    b3Vec3 r = { (float)(p[0] - c->com.x), (float)(p[1] - c->com.y), (float)(p[2] - c->com.z) };
    c->torque = b3Add(c->torque, b3Cross(r, f));
}

static void world_point(const buoy_ctx *c, b3Vec3 local, double out[3]) {
    b3Pos p = b3TransformWorldPoint(c->xf, local);
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}

static void do_drag(buoy_ctx *c, const int32_t s[3], b3Vec3 local, double size, double strength) {
    double p[3];
    world_point(c, local, p);
    double vol = overlap_volume(p, size, s);
    if (vol <= 0) return;
    b3Vec3 r = { (float)(p[0] - c->com.x), (float)(p[1] - c->com.y), (float)(p[2] - c->com.z) };
    b3Vec3 vel = b3Add(c->v, b3Cross(c->w, r));
    add_force_at(c, p, b3MulSV((float)(-1.7 * vol * strength), vel));
}

static void do_float(buoy_ctx *c, const int32_t s[3], b3Vec3 local, double size, double strength) {
    double p[3];
    world_point(c, local, p);
    double vol = overlap_volume(p, size, s);
    if (vol <= 0) return;
    add_force_at(c, p, (b3Vec3){ 0.0f, (float)(10.5 * vol * strength), 0.0f });
}

void phys_buoyancy_compute(phys_scene *s) {
    /* 遍历场景中所有子世界 */
    uint32_t it = 0;
    phys_body *b;
    while (phys_scene_next_body(s, &it, &b)) {
        b->has_tick_force = false;
        b->tick_force = b3Vec3_zero;
        b->tick_torque = b3Vec3_zero;
        if (!b->has_octree || !b->has_com || !b->has_bounds) continue;

        int32_t *pairs;
        size_t n = phys_find_liquid_pairs(b, &pairs);
        if (!n) continue;

        buoy_ctx c;
        c.xf = b3Body_GetTransform(b->body);
        c.v = b3Body_GetLinearVelocity(b->body);
        c.w = b3Body_GetAngularVelocity(b->body);
        c.com = c.xf.p;
        c.force = c.torque = b3Vec3_zero;

        bool complex = (b->bmax[0] - b->bmin[0]) + (b->bmax[1] - b->bmin[1]) + (b->bmax[2] - b->bmin[2]) < 10;
        for (size_t i = 0; i < n; i++) {
            const int32_t *st = pairs + i * 6;      /* 液体方块（世界坐标） */
            const int32_t *dy = pairs + i * 6 + 3;  /* 子世界方块（地块坐标） */
            b3Vec3 local = { (float)(dy[0] + 0.5 - b->com[0]), (float)(dy[1] + 0.5 - b->com[1]), (float)(dy[2] + 0.5 - b->com[2]) };
            if (complex) {
                for (int j = 0; j < 8; j++) {
                    b3Vec3 sub = { local.x + ((j & 1) * 2 - 1) * 0.25f, local.y + (((j >> 1) & 1) * 2 - 1) * 0.25f,
                                   local.z + (((j >> 2) & 1) * 2 - 1) * 0.25f };
                    do_drag(&c, st, sub, 0.25, 1.0);
                }
            } else {
                do_drag(&c, st, local, 0.5, 1.0);
            }
        }
        for (size_t i = 0; i < n; i++) {
            const int32_t *st = pairs + i * 6;
            const int32_t *dy = pairs + i * 6 + 3;
            uint32_t cell = phys_scene_cell_at(s, b, dy[0], dy[1], dy[2]);
            const phys_collider *col = phys_collider_get(phys_cell_collider(cell));
            if (!col) continue;
            b3Vec3 local = { (float)(dy[0] + 0.5 - b->com[0]), (float)(dy[1] + 0.5 - b->com[1]), (float)(dy[2] + 0.5 - b->com[2]) };
            if (complex) {
                for (int j = 0; j < 8; j++) {
                    b3Vec3 sub = { local.x + ((j & 1) * 2 - 1) * 0.25f, local.y + (((j >> 1) & 1) * 2 - 1) * 0.25f,
                                   local.z + (((j >> 2) & 1) * 2 - 1) * 0.25f };
                    do_float(&c, st, sub, 0.25, col->volume);
                }
            } else {
                do_float(&c, st, local, 0.5, col->volume);
            }
        }
        b->tick_force = c.force;
        b->tick_torque = c.torque;
        b->has_tick_force = true;
    }
}
