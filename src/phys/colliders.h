/*
 * 方块碰撞体注册表（对应 Sable 的 VoxelColliderMap / RapierVoxelColliderBakery）
 *
 * 每种方块状态烘焙成一个碰撞体：若干个 0~1 体素空间内的碰撞箱 + 物理属性。
 * 编号从 1 开始，0 表示无碰撞体。启动时注册完成后只读，所有线程无锁访问。
 */
#ifndef MCC_PHYS_COLLIDERS_H
#define MCC_PHYS_COLLIDERS_H

#include "../core/common.h"
#include <box3d/collision.h>

typedef struct phys_box {
    float min[3], max[3];
} phys_box;

typedef struct phys_collider {
    int nboxes;
    phys_box *boxes;
    b3BoxHull *hulls;     /* 每个碰撞箱以自身中心为原点的凸包（预计算） */
    b3Vec3 *centers;      /* 每个碰撞箱中心（体素空间） */
    b3Vec3 *halves;       /* 半尺寸 */
    float friction;       /* 摩擦系数倍率 */
    float volume;         /* 浮力体积 */
    float restitution;
    bool fluid;
    bool dynamic;         /* 碰撞形状随位置变化（按位置查覆盖表） */
    uint16_t callback;    /* 方块碰撞回调编号，0 无 */
    bool needs_hooks;     /* 摩擦≠1、弹性≠0 或有回调 */
} phys_collider;

/* 体素邻居状态（与 Sable VoxelNeighborhoodState 顺序一致） */
enum phys_voxel_state {
    PHYS_VS_EMPTY = 0,
    PHYS_VS_FACE = 1,
    PHYS_VS_EDGE = 2,
    PHYS_VS_CORNER = 3,
    PHYS_VS_INTERIOR = 4,
};

/* 体素格打包：高 16 位碰撞体编号，低 16 位邻居状态（与 Sable packBlockState 一致） */
MCC_INLINE uint32_t phys_pack(uint32_t collider, uint32_t state) { return (collider << 16) | (state & 0xFFFF); }
MCC_INLINE uint32_t phys_cell_collider(uint32_t cell) { return cell >> 16; }
MCC_INLINE uint32_t phys_cell_state(uint32_t cell) { return cell & 0xFFFF; }

/* 注册：返回编号（>=1） */
uint32_t phys_collider_new(float friction, float volume, float restitution, bool fluid, bool dynamic, uint16_t callback);
void phys_collider_add_box(uint32_t id, const float min[3], const float max[3]);
void phys_collider_clear_boxes(uint32_t id);
/* 注册完成后调用：预计算凸包，之后注册表只读 */
void phys_colliders_finalize(void);
void phys_colliders_free(void);

uint32_t phys_collider_count(void);
const phys_collider *phys_collider_get(uint32_t id); /* id 从 1 开始，非法返回 NULL */

#endif
