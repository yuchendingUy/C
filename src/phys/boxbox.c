/*
 * 长方体 vs 长方体 接触（方块碰撞箱专用，替代通用凸包碰撞）
 *
 * A 的碰撞箱位于原点、与坐标轴对齐，半尺寸 ha；B 由 xf（B → A）给出，半尺寸 hb。
 * 分离轴测试 15 根轴：A 的 3 个面、B 的 3 个面、两两边叉积 9 个。
 *   - 面接触：参考面取所属盒，入射面取另一盒最反向的面，用参考面 4 条边裁剪得到最多 8 点，再精简到 4 点
 *   - 边接触：两条支撑边的最近点，1 个点
 * 输出约定与 Box3D 一致：法线 A 指向 B，点为两表面中点（A 盒局部系），separation 负数表示穿透。
 */
#include "boxbox.h"
#include <math.h>
#include <float.h>

#define FACE_TOL 0.005f
int g_boxbox_debug;
#include <stdio.h>   /* 面轴优先：边轴必须明显更好才采用 */

typedef struct { float m[3][3]; } mat3;  /* m[r][c]，列为 B 的轴在 A 系中的方向 */

static inline float vget(b3Vec3 v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }
static inline b3Vec3 col(const mat3 *R, int c) { return (b3Vec3){ R->m[0][c], R->m[1][c], R->m[2][c] }; }
static inline b3Vec3 axis(int i) { return (b3Vec3){ i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f }; }

static mat3 quat_to_mat(b3Quat q) {
    b3Matrix3 M = b3MakeMatrixFromQuat(q);
    mat3 R;
    R.m[0][0] = M.cx.x; R.m[1][0] = M.cx.y; R.m[2][0] = M.cx.z;
    R.m[0][1] = M.cy.x; R.m[1][1] = M.cy.y; R.m[2][1] = M.cy.z;
    R.m[0][2] = M.cz.x; R.m[1][2] = M.cz.y; R.m[2][2] = M.cz.z;
    return R;
}

/* 参考盒为"盒 1"（原点、轴对齐、半尺寸 h1），入射盒为"盒 2"（中心 t、旋转 R、半尺寸 h2）。
 * 参考面法线为 sgn * e_axis。输出点在盒 1 局部系 */
typedef struct { b3Vec3 p; float sep; uint8_t id; } clip_v;

static int clip_side(const clip_v *in, int n, int ax, float sign, float limit, clip_v *out, uint8_t edge_id) {
    /* 保留 sign * x[ax] <= limit 的部分 */
    int m = 0;
    for (int i = 0; i < n; i++) {
        const clip_v *a = &in[i], *b = &in[(i + 1) % n];
        float da = sign * vget(a->p, ax) - limit;
        float db = sign * vget(b->p, ax) - limit;
        if (da <= 0) out[m++] = *a;
        if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
            float t = da / (da - db);
            clip_v c;
            c.p = b3Lerp(a->p, b->p, t);
            c.sep = 0;
            c.id = (uint8_t)(0x80 | (edge_id << 3) | (i & 7));
            out[m++] = c;
        }
    }
    return m;
}

static int face_contact(b3Vec3 h1, b3Vec3 t, const mat3 *R, b3Vec3 h2, int ax, float sgn, float speculative,
                        b3LocalManifoldPoint *pts, uint8_t ref_tag) {
    b3Vec3 n = b3MulSV(sgn, axis(ax));
    /* 入射面：盒 2 中法线与 n 最反向的面 */
    int j = 0;
    float best = -1;
    for (int k = 0; k < 3; k++) {
        float d = fabsf(b3Dot(col(R, k), n));
        if (d > best) { best = d; j = k; }
    }
    float dj = b3Dot(col(R, j), n);
    float fs = dj > 0 ? -1.0f : 1.0f;              /* 入射面外法线 = fs * col j */
    b3Vec3 c = b3MulAdd(t, fs * vget(h2, j), col(R, j));
    int k1 = (j + 1) % 3, k2 = (j + 2) % 3;
    b3Vec3 u = b3MulSV(vget(h2, k1), col(R, k1)), v = b3MulSV(vget(h2, k2), col(R, k2));
    clip_v poly[8], tmp[8];
    poly[0] = (clip_v){ b3Add(b3Add(c, u), v), 0, 0 };
    poly[1] = (clip_v){ b3Add(b3Sub(c, u), v), 0, 1 };
    poly[2] = (clip_v){ b3Sub(b3Sub(c, u), v), 0, 2 };
    poly[3] = (clip_v){ b3Sub(b3Add(c, u), v), 0, 3 };
    int n_ = 4;
    int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
    n_ = clip_side(poly, n_, a1, 1.0f, vget(h1, a1), tmp, 0);
    if (!n_) return 0;
    n_ = clip_side(tmp, n_, a1, -1.0f, vget(h1, a1), poly, 1);
    if (!n_) return 0;
    n_ = clip_side(poly, n_, a2, 1.0f, vget(h1, a2), tmp, 2);
    if (!n_) return 0;
    n_ = clip_side(tmp, n_, a2, -1.0f, vget(h1, a2), poly, 3);
    if (!n_) return 0;

    /* 深度：相对参考面 */
    float ref = vget(h1, ax);
    clip_v keep[8];
    int nk = 0;
    for (int i = 0; i < n_; i++) {
        float s = b3Dot(poly[i].p, n) - ref;
        if (s > speculative) continue;
        keep[nk] = poly[i];
        keep[nk].sep = s;
        nk++;
    }
    if (!nk) return 0;

    /* 超过 4 点时精简：最深、离它最远、两侧面积最大 */
    int idx[4], ni;
    if (nk <= 4) {
        for (int i = 0; i < nk; i++) idx[i] = i;
        ni = nk;
    } else {
        int i0 = 0;
        for (int i = 1; i < nk; i++) if (keep[i].sep < keep[i0].sep) i0 = i;
        int i1 = i0;
        float far = -1;
        for (int i = 0; i < nk; i++) {
            float d = b3DistanceSquared(keep[i].p, keep[i0].p);
            if (d > far) { far = d; i1 = i; }
        }
        b3Vec3 e = b3Sub(keep[i1].p, keep[i0].p);
        int i2 = -1, i3 = -1;
        float amax = 0, amin = 0;
        for (int i = 0; i < nk; i++) {
            float a = b3Dot(b3Cross(e, b3Sub(keep[i].p, keep[i0].p)), n);
            if (a > amax) { amax = a; i2 = i; }
            if (a < amin) { amin = a; i3 = i; }
        }
        ni = 0;
        idx[ni++] = i0;
        if (i1 != i0) idx[ni++] = i1;
        if (i2 >= 0) idx[ni++] = i2;
        if (i3 >= 0) idx[ni++] = i3;
    }
    for (int i = 0; i < ni; i++) {
        const clip_v *cv = &keep[idx[i]];
        pts[i].point = b3MulSub(cv->p, 0.5f * cv->sep, n);   /* 两表面中点 */
        pts[i].separation = cv->sep;
        pts[i].pair = (b3FeaturePair){ ref_tag, (uint8_t)(ax * 2 + (sgn > 0)), (uint8_t)(j * 2 + (fs > 0)), cv->id };
        pts[i].triangleIndex = 0;
    }
    return ni;
}

/* 两线段最近点（Ericson 5.1.9） */
static void closest_segments(b3Vec3 p1, b3Vec3 d1, b3Vec3 p2, b3Vec3 d2, b3Vec3 *c1, b3Vec3 *c2) {
    b3Vec3 r = b3Sub(p1, p2);
    float a = b3Dot(d1, d1), e = b3Dot(d2, d2), f = b3Dot(d2, r);
    float s, t;
    float c = b3Dot(d1, r), b = b3Dot(d1, d2);
    float denom = a * e - b * b;
    s = denom > 1e-12f ? b3ClampFloat((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
    t = (b * s + f) / e;
    if (t < 0) { t = 0; s = b3ClampFloat(-c / a, 0.0f, 1.0f); }
    else if (t > 1) { t = 1; s = b3ClampFloat((b - c) / a, 0.0f, 1.0f); }
    *c1 = b3MulAdd(p1, s, d1);
    *c2 = b3MulAdd(p2, t, d2);
}

/* 边接触：两条支撑边最近点，1 个点 */
static int edge_contact(b3Vec3 ha, b3Vec3 hb, const mat3 *R, b3Vec3 t, b3Vec3 nE, int ei, int ej, float speculative, b3LocalManifold *out) {
    b3Vec3 n = nE;
    if (b3Dot(n, t) < 0) n = b3Neg(n);
    b3Vec3 pa = b3Vec3_zero;
    for (int k = 0; k < 3; k++) {
        if (k == ei) continue;
        float sgn = vget(n, k) >= 0 ? 1.0f : -1.0f;
        pa = b3MulAdd(pa, sgn * vget(ha, k), axis(k));
    }
    b3Vec3 da = b3MulSV(2.0f * vget(ha, ei), axis(ei));
    pa = b3MulSub(pa, vget(ha, ei), axis(ei));
    b3Vec3 pb = t;
    for (int k = 0; k < 3; k++) {
        if (k == ej) continue;
        float sgn = b3Dot(n, col(R, k)) >= 0 ? -1.0f : 1.0f;
        pb = b3MulAdd(pb, sgn * vget(hb, k), col(R, k));
    }
    b3Vec3 db = b3MulSV(2.0f * vget(hb, ej), col(R, ej));
    pb = b3MulSub(pb, vget(hb, ej), col(R, ej));
    b3Vec3 ca, cb;
    closest_segments(pa, da, pb, db, &ca, &cb);
    float sep = b3Dot(b3Sub(cb, ca), n);
    if (sep > speculative) return 0;
    out->normal = n;
    out->points[0].point = b3Lerp(ca, cb, 0.5f);
    out->points[0].separation = sep;
    out->points[0].pair = (b3FeaturePair){ 3, (uint8_t)ei, (uint8_t)ej, 0 };
    out->points[0].triangleIndex = 0;
    out->pointCount = 1;
    return 1;
}

int phys_collide_boxes(b3Vec3 ha, b3Vec3 hb, b3Transform xf, float speculative, b3LocalManifold *out) {
    out->pointCount = 0;
    mat3 R = quat_to_mat(xf.q);
    b3Vec3 t = xf.p;
    float AR[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) AR[i][j] = fabsf(R.m[i][j]) + 1e-6f;

    /* A 的面轴 */
    float sepA = -FLT_MAX;
    int axA = 0;
    for (int i = 0; i < 3; i++) {
        float ra = vget(ha, i);
        float rb = hb.x * AR[i][0] + hb.y * AR[i][1] + hb.z * AR[i][2];
        float s = fabsf(vget(t, i)) - ra - rb;
        if (s > speculative) return 0;
        if (s > sepA) { sepA = s; axA = i; }
    }
    /* B 的面轴 */
    float sepB = -FLT_MAX;
    int axB = 0;
    float tb[3];
    for (int j = 0; j < 3; j++) {
        tb[j] = t.x * R.m[0][j] + t.y * R.m[1][j] + t.z * R.m[2][j];
        float ra = ha.x * AR[0][j] + ha.y * AR[1][j] + ha.z * AR[2][j];
        float rb = vget(hb, j);
        float s = fabsf(tb[j]) - ra - rb;
        if (s > speculative) return 0;
        if (s > sepB) { sepB = s; axB = j; }
    }
    /* 边轴 */
    float sepE = -FLT_MAX;
    int ei = -1, ej = -1;
    b3Vec3 nE = b3Vec3_zero;
    for (int i = 0; i < 3; i++) {
        int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
        for (int j = 0; j < 3; j++) {
            int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            b3Vec3 L = b3Cross(axis(i), col(&R, j));
            float len = b3Length(L);
            if (len < 1e-4f) continue;  /* 近平行，面轴已覆盖 */
            float ra = vget(ha, i1) * AR[i2][j] + vget(ha, i2) * AR[i1][j];
            float rb = vget(hb, j1) * AR[i][j2] + vget(hb, j2) * AR[i][j1];
            float dist = vget(t, i2) * R.m[i1][j] - vget(t, i1) * R.m[i2][j];
            float s = (fabsf(dist) - ra - rb) / len;
            if (s > speculative) return 0;
            if (s > sepE) {
                sepE = s;
                ei = i;
                ej = j;
                nE = b3MulSV((dist < 0 ? -1.0f : 1.0f) / len, L);
            }
        }
    }

    float faceMax = sepA > sepB ? sepA : sepB;
    if (g_boxbox_debug) fprintf(stderr, "sepA %.4f(ax%d) sepB %.4f(ax%d) sepE %.4f(%d,%d)\n", sepA, axA, sepB, axB, sepE, ei, ej);
    if (ei >= 0 && sepE > faceMax + FACE_TOL) return edge_contact(ha, hb, &R, t, nE, ei, ej, speculative, out);
    if (sepB > sepA + FACE_TOL) {
        /* 参考面在 B：在 B 系中计算后变换回 A 系 */
        mat3 RT;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) RT.m[i][j] = R.m[j][i];
        b3Vec3 tB = { -tb[0], -tb[1], -tb[2] };  /* A 中心在 B 系 */
        float sgn = tb[axB] > 0 ? -1.0f : 1.0f;   /* B 指向 A 的面 */
        b3LocalManifoldPoint tmp[4];
        int n = face_contact(hb, tB, &RT, ha, axB, sgn, speculative, tmp, 2);
        if (!n) return ei >= 0 ? edge_contact(ha, hb, &R, t, nE, ei, ej, speculative, out) : 0;
        b3Vec3 nB = b3MulSV(sgn, axis(axB));   /* B 系，B 指向 A */
        b3Vec3 nA = { R.m[0][0] * nB.x + R.m[0][1] * nB.y + R.m[0][2] * nB.z, R.m[1][0] * nB.x + R.m[1][1] * nB.y + R.m[1][2] * nB.z,
                      R.m[2][0] * nB.x + R.m[2][1] * nB.y + R.m[2][2] * nB.z };
        out->normal = b3Neg(nA);
        for (int i = 0; i < n; i++) {
            b3Vec3 p = tmp[i].point;
            out->points[i].point = (b3Vec3){ R.m[0][0] * p.x + R.m[0][1] * p.y + R.m[0][2] * p.z + t.x,
                                             R.m[1][0] * p.x + R.m[1][1] * p.y + R.m[1][2] * p.z + t.y,
                                             R.m[2][0] * p.x + R.m[2][1] * p.y + R.m[2][2] * p.z + t.z };
            out->points[i].separation = tmp[i].separation;
            out->points[i].pair = tmp[i].pair;
            out->points[i].triangleIndex = 0;
        }
        out->pointCount = n;
        return n;
    }

    float sgn = vget(t, axA) >= 0 ? 1.0f : -1.0f;
    int n = face_contact(ha, t, &R, hb, axA, sgn, speculative, out->points, 1);
    if (!n) return ei >= 0 ? edge_contact(ha, hb, &R, t, nE, ei, ej, speculative, out) : 0;
    out->normal = b3MulSV(sgn, axis(axA));
    out->pointCount = n;
    return n;
}
