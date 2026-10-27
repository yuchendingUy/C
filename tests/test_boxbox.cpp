/*
 * 长方体专用接触 vs Box3D 通用凸包碰撞 对拍 + 速度对比
 */
#include "../src/phys/boxbox.h"
#include "../src/core/common.h"
#include "../src/core/log.h"
#include <math.h>
#include <stdio.h>

static uint64_t g_rng = 0x1234567;
static float frand(void) { g_rng = mcc_hash64(g_rng + 0x9E3779B97F4A7C15ULL); return (float)((g_rng >> 40) * (1.0 / 16777216.0)); }
static float frange(float a, float b) { return a + (b - a) * frand(); }

static b3Quat rand_quat(bool near_axis) {
    if (near_axis) {
        /* 小角度扰动，模拟静止/缓慢翻转的结构 */
        b3Vec3 ax = b3Normalize((b3Vec3){ frange(-1, 1), frange(-1, 1), frange(-1, 1) });
        float ang = frange(-0.05f, 0.05f);
        return (b3Quat){ b3MulSV(sinf(ang * 0.5f), ax), cosf(ang * 0.5f) };
    }
    b3Quat q = { { frange(-1, 1), frange(-1, 1), frange(-1, 1) }, frange(-1, 1) };
    return b3NormalizeQuat(q);
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 500000;
    const float spec = 0.02f;
    b3LocalManifoldPoint p1[4], p2[4];
    int miss[2] = {0, 0}, over[2] = {0, 0}, overlap_wrong[2] = {0, 0}, overlaps = 0, contacts = 0;
    float worst_over[2] = {0, 0};
    for (int i = 0; i < n; i++) {
        bool near_axis = i & 1;
        b3Vec3 ha = { frange(0.03f, 0.5f), frange(0.03f, 0.5f), frange(0.03f, 0.5f) };
        b3Vec3 hb = { frange(0.03f, 0.5f), frange(0.03f, 0.5f), frange(0.03f, 0.5f) };
        if (i % 4 == 0) ha = hb = (b3Vec3){ 0.5f, 0.5f, 0.5f };
        b3Quat q = rand_quat(near_axis);
        b3Vec3 dir = b3Normalize((b3Vec3){ frange(-1, 1), frange(-1, 1), frange(-1, 1) });
        if (near_axis && (i & 2)) {
            int a = (int)(frand() * 3);
            dir = (b3Vec3){ a == 0 ? (frand() < .5f ? -1.f : 1.f) : frange(-.4f, .4f), a == 1 ? (frand() < .5f ? -1.f : 1.f) : frange(-.4f, .4f),
                            a == 2 ? (frand() < .5f ? -1.f : 1.f) : frange(-.4f, .4f) };
        }
        float dist = frange(0.2f, 1.6f);
        b3Transform xf = { b3MulSV(dist, dir), q };
        b3BoxHull A = b3MakeBoxHull(ha.x, ha.y, ha.z), B = b3MakeBoxHull(hb.x, hb.y, hb.z);

        /* 真实距离 */
        b3DistanceInput di = {0};
        di.proxyA = (b3ShapeProxy){ b3GetHullPoints(&A.base), A.base.vertexCount, 0 };
        di.proxyB = (b3ShapeProxy){ b3GetHullPoints(&B.base), B.base.vertexCount, 0 };
        di.transform = xf;
        b3SimplexCache gc = {0};
        b3Simplex sx[4];
        float truth = b3ShapeDistance(&di, &gc, sx, 0).distance;
        if (truth <= 0.0f) overlaps++;

        b3LocalManifold m[2] = {{0}, {0}};
        m[0].points = p1;
        m[1].points = p2;
        b3SATCache cache = {0};
        b3CollideHulls(&m[0], 4, &A.base, &B.base, xf, &cache);
        phys_collide_boxes(ha, hb, xf, spec, &m[1]);
        for (int k = 0; k < 2; k++) {
            float sep = 1e9f;
            for (int j = 0; j < m[k].pointCount; j++) sep = fminf(sep, m[k].points[j].separation);
            bool has = m[k].pointCount > 0 && sep <= spec;
            if (k == 1 && has) contacts++;
            if (truth <= 0.002f && !has) miss[k]++;
            if (has && sep > truth + 0.006f) { over[k]++; if (sep - truth > worst_over[k]) worst_over[k] = sep - truth; }
            if (truth <= 0.0f && has && sep > 0.002f) overlap_wrong[k]++;
        }
    }
    mcc_info("对拍 %d 对（实际重叠 %d，专用算法报告接触 %d），以 GJK 真实距离为准：", n, overlaps, contacts);
    const char *nm[2] = { "Box3D 通用凸包", "长方体专用" };
    for (int k = 0; k < 2; k++)
        mcc_info("  %s：真正接触却漏判 %d，间距多报超过 6mm %d（最大多报 %.4f），重叠却报分离 %d", nm[k], miss[k], over[k], worst_over[k], overlap_wrong[k]);
    p1[0] = p1[0];
    /* 速度：静止接触最常见的近轴对齐面接触 */
    int reps = 2000000;
    b3Vec3 h = { 0.5f, 0.5f, 0.5f };
    b3BoxHull H = b3MakeBoxHull(0.5f, 0.5f, 0.5f);
    b3Transform xf = { { 0.03f, 0.995f, -0.02f }, rand_quat(true) };
    b3LocalManifold m = {0};
    m.points = p1;
    uint64_t t0 = mcc_nanotime();
    volatile int sink = 0;
    for (int i = 0; i < reps; i++) {
        b3SATCache cache = {0};
        b3CollideHulls(&m, 4, &H.base, &H.base, xf, &cache);
        sink += m.pointCount;
    }
    uint64_t t1 = mcc_nanotime();
    for (int i = 0; i < reps; i++) sink += phys_collide_boxes(h, h, xf, 0.02f, &m);
    uint64_t t2 = mcc_nanotime();
    mcc_info("速度：通用凸包 %.1f ns/次，长方体专用 %.1f ns/次，快 %.1f 倍", (t1 - t0) / (double)reps, (t2 - t1) / (double)reps,
             (double)(t1 - t0) / (double)(t2 - t1));
    bool ok = miss[1] == 0 && over[1] == 0 && overlap_wrong[1] == 0;
    if (!ok) { mcc_error("对拍失败"); return 1; }
    mcc_info("对拍通过");
    return 0;
}
