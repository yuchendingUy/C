/*
 * 物理模块测试：方块结构落地、结构堆叠、水中漂浮、远离原点的精度，
 * 以及改世界存储里的方块后物理立即生效（地形挖坑、子世界拆掉方块）
 */
#include "../src/phys/scene.h"
#include "../src/core/jobs.h"
#include "../src/core/ebr.h"
#include "testlib.h"
#include <math.h>
#include <stdio.h>

static bool g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { mcc_error(__VA_ARGS__); g_fail = true; } else { mcc_info("  通过：" __VA_ARGS__); } } while (0)

static void load_terrain(mcc_chunk_store *st, terrain_ctx *t) {
    test_fill(st, terrain, t, (t->ox >> 4) - 2, (t->ox + 112) >> 4, (t->oz >> 4) - 2, (t->oz >> 4) + 2, 32, 79);
}

static void spawn_cube(phys_scene *s, int32_t id, mcc_chunk_store *st, cube_ctx *c, double x, double y, double z, double mass) {
    test_cube_store(st, c);
    double pose[7] = { x, y, z, 0, 0, 0, 1 };
    phys_sublevel_create(s, id, pose, st);
    int32_t mn[3] = { c->bx, c->by, c->bz }, mx[3] = { c->bx + 2, c->by + 2, c->bz + 2 };
    double com[3] = { c->bx + 1.5, c->by + 1.5, c->bz + 1.5 };
    phys_body_set_center_of_mass(s, id, com);
    phys_body_set_local_bounds(s, id, mn, mx);
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
    mcc_chunk_store world;
    mcc_store_init(&world, TEST_MIN_Y, TEST_HEIGHT);
    terrain_ctx t = { (int32_t)ox, (int32_t)oz };
    load_terrain(&world, &t);
    phys_scene_config cfg = { { 0, -9.81, 0 }, 0.1 };
    phys_scene *s = phys_scene_create(&cfg, &world);

    /* 地块坐标放在很远的地方，模拟 Sable 的地块区；每个子世界一个自己的存储 */
    mcc_chunk_store plots[3];
    cube_ctx c1 = { 20000000, 64, 20000000 };
    cube_ctx c2 = { 20000016, 64, 20000000 };
    cube_ctx c3 = { 20000032, 64, 20000000 };
    spawn_cube(s, 1, &plots[0], &c1, ox + 0.0, 70.0, oz + 0.0, 27.0);
    spawn_cube(s, 2, &plots[1], &c2, ox + 0.6, 76.0, oz + 0.3, 27.0);
    spawn_cube(s, 3, &plots[2], &c3, ox + 80.0, 76.0, oz + 0.0, 5.0);

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
    mcc_info("  8 秒模拟耗时 %.1fms；地形格 %zu，地形区块 %zu，方块对 %llu，接触片 %llu", ms, st.cells, st.terrain_chunks,
             (unsigned long long)st.pairs, (unsigned long long)st.manifolds);

    /* 改世界存储里的地形：在结构1、2 下面挖一个 8×8、深 5 的坑。不通知物理，只改存储 */
    int32_t bx = (int32_t)floor(p1[0]), bz = (int32_t)floor(p1[2]);
    for (int32_t x = bx - 4; x < bx + 4; x++)
        for (int32_t z = bz - 4; z < bz + 4; z++)
            for (int32_t y = 56; y <= 60; y++) mcc_store_set(&world, x, y, z, 0);
    run(s, 80);
    phys_body_get_pose(s, 1, p1);
    phys_body_get_pose(s, 2, p2);
    CHECK(fabs(p1[1] - 57.5) < 0.05, "挖坑后结构1 立即被唤醒并落到坑底，中心高度 %.4f ≈ 57.5", p1[1]);
    CHECK(p2[1] < 61.0 && speed(s, 2) < 0.05, "结构2 也跟着落进坑里并静止（中心高度 %.4f：叠在结构1 上为 60.5，滑到坑底为 57.5）", p2[1]);
    double before = p2[1];

    /* 改子世界自己的存储：拆掉结构2 的全部方块，它没有了碰撞体，应当穿过结构1 掉下去 */
    for (int32_t x = c2.bx; x < c2.bx + 3; x++)
        for (int32_t y = c2.by; y < c2.by + 3; y++)
            for (int32_t z = c2.bz; z < c2.bz + 3; z++) mcc_store_set(&plots[1], x, y, z, 0);
    run(s, 40);
    phys_body_get_pose(s, 2, p2);
    CHECK(p2[1] < before - 10.0, "拆掉结构2 的方块后它立即失去碰撞、穿过下方掉落（%.2f → %.2f）", before, p2[1]);

    char err[256];
    CHECK(mcc_store_verify(&world, err, sizeof err), "地形存储的物理位图与逐方块重算一致%s", "");

    phys_scene_destroy(s);
    for (int i = 0; i < 3; i++) mcc_store_free(&plots[i]);
    mcc_store_free(&world);
}

/* 方块 (x, y, z)（地块坐标）中心在世界里的位置 */
static void block_world(phys_scene *s, int32_t id, int32_t x, int32_t y, int32_t z, double out[3]) {
    phys_body *b = phys_body_get(s, id);
    b3WorldTransform xf = b3Body_GetTransform(b->body);
    b3Vec3 l = { (float)(x + 0.5 - b->com[0]), (float)(y + 0.5 - b->com[1]), (float)(z + 0.5 - b->com[2]) };
    b3Pos w = b3TransformWorldPoint(xf, l);
    out[0] = w.x; out[1] = w.y; out[2] = w.z;
}

/* L 形：3×1×3 底板 + 一根 1×3×1 柱子立在角上 */
typedef struct { int32_t bx, by, bz; } l_ctx;
static mcc_state_t l_shape(int32_t x, int32_t y, int32_t z, void *p) {
    l_ctx *c = (l_ctx*)p;
    int32_t lx = x - c->bx, ly = y - c->by, lz = z - c->bz;
    if (lx < 0 || lz < 0 || ly < 0 || lx > 2 || lz > 2 || ly > 3) return 0;
    if (ly == 0) return STONE;
    return lx == 0 && lz == 0 ? STONE : 0;
}

static void mass_tracking(void) {
    mcc_info("== 自动质量追踪");
    mcc_chunk_store world;
    mcc_store_init(&world, TEST_MIN_Y, TEST_HEIGHT);
    terrain_ctx t = { 0, 0 };
    load_terrain(&world, &t);
    phys_scene_config cfg = { { 0, -9.81, 0 }, 0.1 };
    phys_scene *s = phys_scene_create(&cfg, &world);

    mcc_chunk_store cube_st, l_st;
    cube_ctx c = { 20000000, 64, 20000000 };
    test_cube_store(&cube_st, &c);
    double pose[7] = { 0, 70, 0, 0, 0, 0, 1 };
    phys_sublevel_create(s, 1, pose, &cube_st);
    phys_body_track_mass(s, 1);
    phys_body *b = phys_body_get(s, 1);
    b3MassData md = b3Body_GetMassData(b->body);
    CHECK(fabs(md.mass - 27) < 1e-4 && fabs(b->com[0] - (c.bx + 1.5)) < 1e-9 && fabs(b->com[1] - 65.5) < 1e-9,
          "3×3×3 石头：质量 %.3f、质心 (%.3f, %.3f) 与解析值一致", md.mass, b->com[0] - c.bx, b->com[1]);
    CHECK(fabs(md.inertia.cx.x - 40.5) < 1e-3 && fabs(md.inertia.cy.x) < 1e-3 && b->bmin[0] == c.bx && b->bmax[1] == c.by + 2,
          "转动惯量 %.3f 等于解析值 27×(9+9)/12 = 40.5，包围盒正确", md.inertia.cx.x);

    run(s, 100);
    double before[3], after[3];
    block_world(s, 1, c.bx, c.by, c.bz, before);
    /* 拆掉顶上一个角：质心移动，刚体原点跟着移动，剩下的方块不应跳动 */
    mcc_store_set(&cube_st, c.bx + 2, c.by + 2, c.bz + 2, 0);
    phys_scene_tick(s);
    block_world(s, 1, c.bx, c.by, c.bz, after);
    md = b3Body_GetMassData(b->body);
    double jump = sqrt((after[0] - before[0]) * (after[0] - before[0]) + (after[1] - before[1]) * (after[1] - before[1]) +
                       (after[2] - before[2]) * (after[2] - before[2]));
    CHECK(fabs(md.mass - 26) < 1e-4 && fabs(b->com[0] - (c.bx + 1.5 - 1.0 / 26)) < 1e-6 && jump < 1e-4,
          "拆掉一个角：质量 26，质心移动 %.4f，剩余方块世界位置变化 %.6f", 1.0 / 26, jump);
    run(s, 60);
    double v[3];
    phys_body_get_linear_velocity(s, 1, v);
    CHECK(fabs(v[1]) < 0.02, "质心更新后结构仍然稳定（竖直速度 %.4f）", v[1]);

    /* L 形：只放方块、开追踪，落地后应静止 */
    l_ctx lc = { 20000016, 64, 20000000 };
    mcc_store_init(&l_st, TEST_MIN_Y, TEST_HEIGHT);
    test_fill(&l_st, l_shape, &lc, lc.bx >> 4, lc.bx >> 4, lc.bz >> 4, lc.bz >> 4, 64, 67);
    double pose2[7] = { 10, 70, 0, 0, 0, 0, 1 };
    phys_sublevel_create(s, 2, pose2, &l_st);
    phys_body_track_mass(s, 2);
    run(s, 120);
    double p2[7];
    phys_body_get_pose(s, 2, p2);
    phys_body *b2 = phys_body_get(s, 2);
    /* 底板贴地（y=61），质心高度 = 61 + (质心 - 底板底面) */
    double expect = 61.0 + (b2->com[1] - lc.by);
    CHECK(fabs(p2[1] - expect) < 0.05 && speed(s, 2) < 0.02, "L 形（底板 9 块 + 柱子 3 块）自动算出质量 %.0f，落地后质心高度 %.4f ≈ %.4f 并静止",
          (double)b3Body_GetMassData(b2->body).mass, p2[1], expect);

    phys_scene_destroy(s);
    mcc_store_free(&cube_st);
    mcc_store_free(&l_st);
    mcc_store_free(&world);
}

int main(void) {
    mcc_jobs_start(2);
    test_load_blocks();

    scenario(0, 0, "原点附近");
    scenario(10000000, 10000000, "x、z = 1000 万");
    mass_tracking();

    test_free_blocks();
    mcc_ebr_drain();
    mcc_jobs_stop();
    if (g_fail) { mcc_error("物理测试失败"); return 1; }
    mcc_info("物理测试全部通过");
    return 0;
}
