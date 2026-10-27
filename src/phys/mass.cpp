#include "mass.h"
#include "colliders.h"

void phys_mass_init(phys_mass_tracker *t) { memset(t, 0, sizeof *t); }

void phys_mass_free(phys_mass_tracker *t) {
    for (int a = 0; a < 3; a++) free(t->h[a].cnt);
    memset(t, 0, sizeof *t);
}

/* 坐标 v 的计数加 d，必要时扩展范围（两侧各留余量，摊还 O(1)） */
static void hist_add(phys_axis_hist *h, int32_t v, int d) {
    if (!h->cnt) {
        h->len = 32;
        h->lo = v - 16;
        h->cnt = (uint32_t*)mcc_xcalloc(h->len, sizeof(uint32_t));
    }
    if (v < h->lo || v >= h->lo + (int32_t)h->len) {
        int32_t lo = MCC_MIN(h->lo, v), hi = MCC_MAX(h->lo + (int32_t)h->len, v + 1);
        uint32_t span = (uint32_t)(hi - lo);
        uint32_t nlen = span * 2;
        int32_t nlo = lo - (int32_t)(span / 2);
        uint32_t *n = (uint32_t*)mcc_xcalloc(nlen, sizeof(uint32_t));
        memcpy(n + (h->lo - nlo), h->cnt, h->len * sizeof(uint32_t));
        free(h->cnt);
        h->cnt = n;
        h->lo = nlo;
        h->len = nlen;
    }
    h->cnt[v - h->lo] += (uint32_t)d;
}

void phys_mass_add(phys_mass_tracker *t, int32_t x, int32_t y, int32_t z, mcc_state_t state, int sign) {
    if (mcc_state_is_air(state)) return;
    if (!t->anchored) {
        t->anchored = true;
        t->anchor[0] = x;
        t->anchor[1] = y;
        t->anchor[2] = z;
    }
    t->blocks += (uint64_t)(int64_t)sign;
    hist_add(&t->h[0], x, sign);
    hist_add(&t->h[1], y, sign);
    hist_add(&t->h[2], z, sign);
    double m = phys_state_mass[state] * sign;
    if (m == 0) return;
    double r[3] = { x - t->anchor[0] + 0.5, y - t->anchor[1] + 0.5, z - t->anchor[2] + 0.5 };
    t->m += m;
    for (int i = 0; i < 3; i++) t->s1[i] += m * r[i];
    t->s2[0] += m * r[0] * r[0];
    t->s2[1] += m * r[1] * r[1];
    t->s2[2] += m * r[2] * r[2];
    t->s2[3] += m * r[0] * r[1];
    t->s2[4] += m * r[0] * r[2];
    t->s2[5] += m * r[1] * r[2];
    t->ib += m / 6.0;
    t->dirty = true;
}

void phys_mass_add_chunk(phys_mass_tracker *t, const mcc_chunk *c, int sign) {
    for (int i = 0; i < c->nsec; i++) {
        const mcc_chunk_section *s = c->sec[i];
        if (!s || !s->nonzero) continue;
        int32_t by = (c->min_sy + i) << 4;
        for (int idx = 0; idx < MCC_SECTION_BLOCKS; idx++) {
            mcc_state_t st = mcc_section_get(s, idx);
            if (st) phys_mass_add(t, (c->cx << 4) + (idx & 15), by + (idx >> 8), (c->cz << 4) + ((idx >> 4) & 15), st, sign);
        }
    }
    t->dirty = true;
}

bool phys_mass_bounds(const phys_mass_tracker *t, int32_t mn[3], int32_t mx[3]) {
    if (!t->blocks) return false;
    for (int a = 0; a < 3; a++) {
        const phys_axis_hist *h = &t->h[a];
        uint32_t i = 0, j = h->len;
        while (i < h->len && !h->cnt[i]) i++;
        while (j > 0 && !h->cnt[j - 1]) j--;
        if (i >= j) return false;
        mn[a] = h->lo + (int32_t)i;
        mx[a] = h->lo + (int32_t)j - 1;
    }
    return true;
}

bool phys_mass_result(const phys_mass_tracker *t, double com[3], double *mass, double inertia[9]) {
    if (t->m <= 1e-9) return false;
    double M = t->m, c[3];
    for (int i = 0; i < 3; i++) {
        c[i] = t->s1[i] / M;
        com[i] = t->anchor[i] + c[i];
    }
    /* 绕锚点：I = Σ m(|r|²δ - r rᵀ) + Σ m/6 δ；再平移到质心：减去 M(|c|²δ - c cᵀ) */
    double xx = t->s2[0] - M * c[0] * c[0], yy = t->s2[1] - M * c[1] * c[1], zz = t->s2[2] - M * c[2] * c[2];
    double xy = t->s2[3] - M * c[0] * c[1], xz = t->s2[4] - M * c[0] * c[2], yz = t->s2[5] - M * c[1] * c[2];
    double Ixx = yy + zz + t->ib, Iyy = xx + zz + t->ib, Izz = xx + yy + t->ib;
    double I[9] = { Ixx, -xy, -xz, -xy, Iyy, -yz, -xz, -yz, Izz };
    memcpy(inertia, I, sizeof I);
    *mass = M;
    return true;
}
