/*
 * 子世界质量追踪（对应 Sable 的质量追踪）：
 * 按方块增量累加质量、一阶矩、二阶矩，放置/拆除一个方块是 O(1)；
 * 包围盒用三个坐标轴上的方块计数表维护，查询时找两端第一个非零。
 * 矩相对锚点（第一个方块）累加，地块坐标在 2000 万格也不丢精度。
 */
#ifndef MCC_PHYS_MASS_H
#define MCC_PHYS_MASS_H

#include "../world/chunk.h"

typedef struct phys_axis_hist {
    int32_t lo;          /* cnt[0] 对应的坐标 */
    uint32_t len;
    uint32_t *cnt;       /* 该坐标上的非空气方块数 */
} phys_axis_hist;

typedef struct phys_mass_tracker {
    bool anchored;
    int32_t anchor[3];
    double m;            /* 总质量 */
    double s1[3];        /* Σ m·r（r 为方块中心相对锚点） */
    double s2[6];        /* Σ m·r r：xx yy zz xy xz yz */
    double ib;           /* Σ m/6：各方块绕自身中心的转动惯量（单位立方体） */
    uint64_t blocks;     /* 非空气方块数 */
    phys_axis_hist h[3];
    bool dirty;
} phys_mass_tracker;

void phys_mass_init(phys_mass_tracker *t);
void phys_mass_free(phys_mass_tracker *t);
/* sign = +1 加入，-1 移除 */
void phys_mass_add(phys_mass_tracker *t, int32_t x, int32_t y, int32_t z, mcc_state_t state, int sign);
void phys_mass_add_chunk(phys_mass_tracker *t, const mcc_chunk *c, int sign);
/* 非空气方块的包围盒（含端点），没有方块返回 false */
bool phys_mass_bounds(const phys_mass_tracker *t, int32_t mn[3], int32_t mx[3]);
/* 质心（方块坐标）、质量、绕质心的转动惯量（列主序 3×3），质量为 0 返回 false */
bool phys_mass_result(const phys_mass_tracker *t, double com[3], double *mass, double inertia[9]);

#endif
