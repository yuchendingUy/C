/*
 * 物理模块测试：方块结构落地、结构堆叠、水中漂浮、远离原点的精度
 */
#include "../src/phys/scene.h"
#include "../src/core/jobs.h"
#include "../src/core/log.h"
#include "../src/core/ebr.h"
#include <math.h>
#include <stdio.h>

static uint32_t STONE, WATER;
static bool g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { mcc_error(__VA_ARGS__); g_fail = true; } else { mcc_info("  通过：" __VA_ARGS__); } } while (0)

/* ---- 测试用的方块世界：用函数描述哪里是什么方块 ---- */
typedef uint32_t (*block_fn)(int32_t x, int32_t y, int32_t z, void *ctx);

static bool full_solid(uint32_t collider) { return collider == STONE; }

/* VoxelNeighborhoodState::getState 的移植（全方块情形） */
static uint32_t neighborhood(block_fn f, void *ctx, int32_t x, int32_t y, int32_t z) {
    uint32_t c = f(x, y, z, ctx);
    if (c == WATER) return PHYS_VS_CORNER;
    if (!c) return PHYS_VS_EMPTY;
    bool all = true, corner = true;
    int both = 0;
    const int d[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int a = 0; a < 3; a++) {
        bool n = full_solid(f(x - d[a][0], y - d[a][1], z - d[a][2], ctx));
        bool p = full_solid(f(x + d[a][0], y + d[a][1], z + d[a][2], ctx));
        if (!n || !p) all = false;
        if (n && p) { corner = false; both++; }
    }
    if (all) return PHYS_VS_INTERIOR;
    if (both == 1) return PHYS_VS_EDGE;
    if (corner) return PHYS_VS_CORNER;
    return PHYS_VS_FACE;
}

static void upload_section(phys_scene *s, block_fn f, void *ctx, int32_t sx, int32_t sy, int32_t sz, bool global, int32_t obj) {
    static uint32_t cells[4096];
    bool any = false;
    for (int i = 0; i < 4096; i++) {
        int32_t x = (sx << 4) + (i & 15), z = (sz << 4) + ((i >> 4) & 15), y = (sy << 4) + (i >> 8);
        uint32_t c = f(x, y, z, ctx);
        cells[i] = c ? phys_pack(c, neighborhood(f, ctx, x, y, z)) : 0;
        any |= c != 0;
    }
    if (any || global) phys_section_add(s, sx, sy, sz, cells, global, obj);
}

/* 地形：y < 61 全是石头（顶面 y=61）；x∈[64,96) 的区域挖出水池，水面顶 y=71 */
typedef struct { int32_t ox, oz; } terrain_ctx;
static uint32_t terrain(int32_t x, int32_t y, int32_t z, void *p) {
    terrain_ctx *t = p;
    int32_t lx = x - t->ox, lz = z - t->oz;
    bool pool = lx >= 64 && lx < 96 && lz >= -16 && lz < 16;
    if (pool) {
        if (y < 50) return STONE;
        if (y < 71) return WATER;
        return 0;
    }
    return y < 61 ? STONE : 0;
}

/* 子世界：地块坐标里的 3×3×3 实心立方体 */
typedef struct { int32_t bx, by, bz; } cube_ctx;
static uint32_t cube(int32_t x, int32_t y, int32_t z, void *p) {
    cube_ctx *c = p;
    return (x >= c->bx && x < c->bx + 3 && y >= c->by && y < c->by + 3 && z >= c->bz && z < c->bz + 3) ? STONE : 0;
}

static void load_terrain(phys_scene *s, terrain_ctx *t) {
    for (int32_t sx = (t->ox >> 4) - 2; sx <= ((t->ox + 112) >> 4); sx++)
        for (int32_t sz = (t->oz >> 4) - 2; sz <= (t->oz >> 4) + 2; sz++)
            for (int32_t sy = 2; sy <= 4; sy++) upload_section(s, terrain, t, sx, sy, sz, true, -1);
}

static void spawn_cube(phys_scene *s, int32_t id, cube_ctx *c, double x, double y, double z, double mass) {
    double pose[7] = { x, y, z, 0, 0, 0, 1 };
    phys_sublevel_create(s, id, pose);
    int32_t mn[3] = { c->bx, c->by, c->bz }, mx[3] = { c->bx + 2, c->by + 2, c->bz + 2 };
    double com[3] = { c->bx + 1.5, c->by + 1.5, c->bz + 1.5 };
    phys_body_set_center_of_mass(s, id, com);
    phys_body_set_local_bounds(s, id, mn, mx);
    for (int32_t sx = c->bx >> 4; sx <= (c->bx + 2) >> 4; sx++)
        for (int32_t sy = c->by >> 4; sy <= (c->by + 2) >> 4; sy++)
            for (int32_t sz = c->bz >> 4; sz <= (c->bz + 2) >> 4; sz++) upload_section(s, cube, c, sx, sy, sz, false, id);
    double I = mass * (9.0 + 9.0) / 12.0;
    double inertia[9] = { I, 0, 0, 0, I, 0, 0, 0, I };
    phys_body_set_mass(s, id, mass, inertia);
}

static void run(phys_scene *s, int ticks) {
    for (int t = 0; t < ticks; t++) {
        phys_scene_tick(s);
        for (int k = 0; k < 2; k++) phys_scene_step(s, 1.0 / 40.0);
    }
}

static double speed(phys_scene *s, int32_t id) {
    double v[3], w[3];
    phys_body_get_linear_velocity(s, id, v);
    phys_body_get_angular_velocity(s, id, w);
    return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
}

static void scenario(double ox, double oz, const char *name) {
    mcc_info("== 场景：%s（原点偏移 x=%.0f）", name, ox);
    phys_scene_config cfg = { { 0, -9.81, 0 }, 0.1 };
    phys_scene *s = phys_scene_create(&cfg);
    terrain_ctx t = { (int32_t)ox, (int32_t)oz };
    load_terrain(s, &t);

    /* 地块坐标放在很远的地方，模拟 Sable 的地块区 */
    cube_ctx c1 = { 20000000, 64, 20000000 };
    cube_ctx c2 = { 20000016, 64, 20000000 };
    cube_ctx c3 = { 20000032, 64, 20000000 };
    spawn_cube(s, 1, &c1, ox + 0.0, 70.0, oz + 0.0, 27.0);
    spawn_cube(s, 2, &c2, ox + 0.6, 76.0, oz + 0.3, 27.0);
    spawn_cube(s, 3, &c3, ox + 80.0, 76.0, oz + 0.0, 5.0);

    uint64_t t0 = mcc_nanotime();
    run(s, 160); /* 8 秒 */
    double ms = (mcc_nanotime() - t0) / 1e6;

    double p1[7], p2[7], p3[7];
    phys_body_get_pose(s, 1, p1);
    phys_body_get_pose(s, 2, p2);
    phys_body_get_pose(s, 3, p3);
    mcc_info("  结构1 位置 (%.4f, %.4f, %.4f) 速度 %.5f", p1[0] - ox, p1[1], p1[2] - oz, speed(s, 1));
    mcc_info("  结构2 位置 (%.4f, %.4f, %.4f) 速度 %.5f", p2[0] - ox, p2[1], p2[2] - oz, speed(s, 2));
    mcc_info("  结构3 位置 (%.4f, %.4f, %.4f) 速度 %.5f", p3[0] - ox, p3[1], p3[2] - oz, speed(s, 3));

    CHECK(fabs(p1[1] - 62.5) < 0.05, "结构1 落地后中心高度 %.4f ≈ 62.5（地面顶 61 + 半高 1.5）", p1[1]);
    CHECK(speed(s, 1) < 0.02, "结构1 已静止（速度 %.5f）", speed(s, 1));
    CHECK(fabs(p2[1] - 65.5) < 0.1, "结构2 叠在结构1上，中心高度 %.4f ≈ 65.5", p2[1]);
    CHECK(fabs(p3[1] - 71.98) < 0.35, "结构3 浮在水面，中心高度 %.4f ≈ 71.98（受力平衡点）", p3[1]);
    CHECK(speed(s, 3) < 0.2, "结构3 漂浮稳定（速度 %.5f）", speed(s, 3));

    phys_stats st;
    phys_scene_stats(s, &st);
    mcc_info("  8 秒模拟耗时 %.1fms；地形格 %zu，区段 %zu，八叉树块 %zu，方块对 %llu，接触片 %llu", ms, st.cells, st.sections,
             st.octree_chunks, (unsigned long long)st.pairs, (unsigned long long)st.manifolds);
    phys_scene_destroy(s);
}

int main(void) {
    mcc_jobs_start(2);
    STONE = phys_collider_new(1.0f, 1.0f, 0.0f, false, false, 0);
    float mn[3] = { 0, 0, 0 }, mx[3] = { 1, 1, 1 };
    phys_collider_add_box(STONE, mn, mx);
    WATER = phys_collider_new(1.0f, 1.0f, 0.0f, true, false, 0);
    phys_collider_add_box(WATER, mn, mx);
    phys_colliders_finalize();

    scenario(0, 0, "原点附近");
    scenario(10000000, 10000000, "x、z = 1000 万");

    phys_colliders_free();
    mcc_ebr_drain();
    mcc_jobs_stop();
    if (g_fail) { mcc_error("物理测试失败"); return 1; }
    mcc_info("物理测试全部通过");
    return 0;
}
