/*
 * 物理性能测试：平地上堆叠 N 个 3×3×3 结构，测每刻耗时
 * 用法：test_phys_bench [任务线程数] [结构数] [刻数]
 */
#include "../src/phys/scene.h"
#include "../src/core/jobs.h"
#include "../src/core/log.h"
#include "../src/core/ebr.h"
#include <math.h>
#include <stdio.h>

#include "testlib.h"

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

static double speed(phys_scene *s, int32_t id) {
    double v[3], w[3];
    phys_body_get_linear_velocity(s, id, v);
    phys_body_get_angular_velocity(s, id, w);
    return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
}


static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static void bench(int n, int ticks) {
    terrain_ctx t = { -200, 0 };
    /* 平地 416×416 */
    mcc_chunk_store world;
    mcc_store_init(&world, TEST_MIN_Y, TEST_HEIGHT);
    test_fill(&world, terrain, &t, -13, 12, -13, 12, 32, 79);
    phys_scene_config cfg = { { 0, -9.81, 0 }, 0.1 };
    phys_scene *s = phys_scene_create(&cfg, &world);
    cube_ctx *cubes = mcc_xmalloc(sizeof(cube_ctx) * (size_t)n);
    mcc_chunk_store *plots = mcc_xmalloc(sizeof(mcc_chunk_store) * (size_t)n);
    int side = (int)ceil(sqrt((double)n / 3.0));
    for (int i = 0; i < n; i++) {
        cubes[i] = (cube_ctx){ 20000000 + (i % 1000) * 16, 64, 20000000 + (i / 1000) * 16 };
        int layer = i / (side * side), k = i % (side * side);
        double x = (k % side) * 5.0 - side * 2.5 + (layer % 2) * 1.3;
        double z = (k / side) * 5.0 - side * 2.5 + (layer % 2) * 0.9;
        spawn_cube(s, i + 1, &plots[i], &cubes[i], x, 66.0 + layer * 4.0, z, 27.0);
    }
    double *ms = mcc_xmalloc(sizeof(double) * (size_t)ticks);
    for (int k = 0; k < ticks; k++) {
        uint64_t t0 = mcc_nanotime();
        phys_scene_tick(s);
        phys_scene_step(s, 1.0 / 40.0);
        phys_scene_step(s, 1.0 / 40.0);
        ms[k] = (mcc_nanotime() - t0) / 1e6;
    }
    double sum = 0, mx = 0;
    for (int k = 0; k < ticks; k++) { sum += ms[k]; if (ms[k] > mx) mx = ms[k]; }
    qsort(ms, (size_t)ticks, sizeof(double), cmp_d);
    int still = 0;
    for (int i = 0; i < n; i++) if (speed(s, i + 1) < 0.05) still++;
    phys_stats st;
    phys_scene_stats(s, &st);
    extern _Atomic uint64_t g_prof[8];
    mcc_info("  接触生成细分：找方块对 %.1fms，碰撞箱求接触 %.1fms，合并输出 %.1fms，原始接触点 %llu",
             g_prof[0] / 1e6, g_prof[1] / 1e6, g_prof[2] / 1e6, (unsigned long long)g_prof[3]);
    mcc_info("  耗时构成：物理步进合计 %.1fms，其中接触生成（各线程合计）%.1fms；浮力等每刻工作 %.1fms；方块对 %llu",
             st.step_ns / 1e6, st.collide_ns / 1e6, st.tick_ns / 1e6, (unsigned long long)st.pairs);
    mcc_info("结构 %4d 个：平均 %.3f mspt，中位 %.3f，99%% %.3f，最慢 %.3f；最终静止 %d 个；接触片 %llu",
             n, sum / ticks, ms[ticks / 2], ms[(int)(ticks * 0.99)], mx, still, (unsigned long long)st.manifolds);
    free(ms);
    phys_scene_destroy(s);
    for (int i = 0; i < n; i++) mcc_store_free(&plots[i]);
    free(plots);
    free(cubes);
    mcc_store_free(&world);
}

int main(int argc, char **argv) {
    int threads = argc > 1 ? atoi(argv[1]) : 2;
    mcc_jobs_start(threads);
    test_load_blocks();
    int counts[] = { 50, 200, 500, 1000 };
    int ncount = 4;
    if (argc > 2) { counts[0] = atoi(argv[2]); ncount = 1; }
    for (int i = 0; i < ncount; i++) bench(counts[i], argc > 3 ? atoi(argv[3]) : 400);
    test_free_blocks();
    mcc_ebr_drain();
    mcc_jobs_stop();
    return 0;
}
