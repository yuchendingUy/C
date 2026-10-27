/*
 * 物理场景（Sable 物理管线的 C 移植，物理引擎换成 Box3D）
 *
 * 每个区域按需持有一个场景，只由该区域的推进线程访问；
 * Box3D 内部的并行求解与接触生成分派到 mcc 通用任务线程池。
 *
 * 数据来源：物理不保存方块副本，直接读世界的区块存储（world/chunk.h），方块只存一份：
 *   - 地形 = 场景创建时给定的区块存储（本区域拥有的区块）
 *   - 子世界 = 它自己的区块存储（Sable 的地块），创建刚体时给定
 * 方块的碰撞体编号来自方块状态表，外露判断与流体占用来自区段里的物理位图，
 * 都由区块存储在写方块时同步维护，下一个物理步立即生效。物理只监听存储事件：
 * 区块加入/移出时增删地形格刚体，方块变化时唤醒附近休眠的刚体。
 *
 * 坐标约定（与 Sable 一致）：
 *   - 子世界的方块用地块坐标寻址
 *   - 子世界刚体原点 = 质心；方块在刚体局部系中的坐标 = 地块坐标 - 质心
 *   - 世界地形按 256×256 的地形格切成若干静态刚体，各自以格中心为原点（大世界精度）
 */
#ifndef MCC_PHYS_SCENE_H
#define MCC_PHYS_SCENE_H

#include "../core/common.h"
#include "../core/hmap.h"
#include "../world/chunk.h"
#include "colliders.h"
#include <box3d/box3d.h>

#define PHYS_CELL_SHIFT 8            /* 地形格 256 方块 */

typedef struct phys_scene phys_scene;

enum phys_shape_kind { PHYS_KIND_BODY = 1, PHYS_KIND_CELL = 2, PHYS_KIND_BOX = 3 };

/* 子世界 / 机械动力移动结构 的刚体信息（对应 ActiveLevelColliderInfo） */
typedef struct phys_body {
    int kind;                    /* PHYS_KIND_BODY，必须是第一个字段 */
    phys_scene *scene;
    int32_t id;
    b3BodyId body;
    b3ShapeId shape;
    bool has_bounds;
    int32_t bmin[3], bmax[3];    /* 局部方块包围盒（地块坐标，含端点） */
    bool has_com;
    double com[3];               /* 质心（地块坐标） */
    mcc_chunk_store *blocks;     /* 子世界自己的方块（不归物理所有） */
    mcc_store_listener listener; /* 方块变化时唤醒本刚体 */
    bool has_fake_vel;           /* 运动学移动结构的"假速度"，用于带动接触物体 */
    b3Vec3 fake_lin, fake_ang;   /* 刚体局部系 */
    bool kinematic;
    /* 每个游戏刻计算一次、每个物理子步重复施加的力（浮力、阻力） */
    b3Vec3 tick_force, tick_torque;
    bool has_tick_force;
} phys_body;

/* 地形格 */
typedef struct phys_cell {
    int kind;                    /* PHYS_KIND_CELL */
    phys_scene *scene;
    int32_t cx, cz;
    double origin[3];
    b3BodyId body;
    b3ShapeId shape;
    int32_t chunks;              /* 本格内已加载的区块数 */
} phys_cell;

/* 上报的碰撞（对应 ReportedCollision） */
typedef struct phys_collision {
    int32_t body_a, body_b;      /* -1 表示地形 */
    double force;
    double normal_a[3], normal_b[3];
    double point_a[3], point_b[3];
} phys_collision;

typedef struct phys_scene_config {
    double gravity[3];
    double universal_drag;
} phys_scene_config;

typedef struct phys_tuning {
    float contact_hertz, contact_damping;     /* 接触软约束 */
    int substeps;                             /* Box3D 内部子步 */
    float joint_hertz, joint_damping;
} phys_tuning;

/* ---- 场景 ---- */
/* terrain：本区域的区块存储（可为 NULL），场景监听它的事件，存储须比场景活得久 */
phys_scene *phys_scene_create(const phys_scene_config *cfg, mcc_chunk_store *terrain);
void phys_scene_destroy(phys_scene *s);
void phys_scene_set_tuning(phys_scene *s, const phys_tuning *t);
b3WorldId phys_scene_world(phys_scene *s);
mcc_chunk_store *phys_scene_terrain(phys_scene *s);

/* 每个游戏刻一次：关节更新、浮力（对应 Rapier3D_tick） */
void phys_scene_tick(phys_scene *s);
/* 一个物理子步（对应 Rapier3D_step） */
void phys_scene_step(phys_scene *s, double dt);

/* ---- 子世界 ---- */
phys_body *phys_body_get(phys_scene *s, int32_t id);
/* 遍历场景中的刚体：uint32_t it = 0; while (phys_scene_next_body(s, &it, &b)) ... */
bool phys_scene_next_body(phys_scene *s, uint32_t *it, phys_body **out);
/* blocks：子世界的方块存储（地块坐标），须在 phys_sublevel_remove 之后才能释放 */
bool phys_sublevel_create(phys_scene *s, int32_t id, const double pose[7], mcc_chunk_store *blocks);
void phys_sublevel_remove(phys_scene *s, int32_t id);
/* 区域合并/拆分时把刚体搬到另一个场景（保留位姿、速度、质量、休眠状态） */
bool phys_body_migrate(phys_scene *src, phys_scene *dst, int32_t id);
size_t phys_scene_body_count(phys_scene *s);
void phys_body_set_center_of_mass(phys_scene *s, int32_t id, const double com[3]);
void phys_body_set_local_bounds(phys_scene *s, int32_t id, const int32_t min[3], const int32_t max[3]);
void phys_body_set_mass(phys_scene *s, int32_t id, double mass, const double inertia[9]);
void phys_body_get_pose(phys_scene *s, int32_t id, double out[7]);
void phys_body_teleport(phys_scene *s, int32_t id, const double pose[7]);
void phys_body_wake(phys_scene *s, int32_t id);
void phys_body_add_velocities(phys_scene *s, int32_t id, const double lin[3], const double ang[3], bool wake);
/* 在局部点（相对质心）施加局部冲量 */
void phys_body_apply_impulse(phys_scene *s, int32_t id, const double local_point[3], const double local_impulse[3], bool wake);
void phys_body_apply_impulse_torque(phys_scene *s, int32_t id, const double local_impulse[3], const double local_torque[3], bool wake);
void phys_body_get_linear_velocity(phys_scene *s, int32_t id, double out[3]);
void phys_body_get_angular_velocity(phys_scene *s, int32_t id, double out[3]);

/* ---- 碰撞上报 ---- */
/* 取出并清空上报的碰撞（最多 100 条，与 Sable 一致） */
size_t phys_scene_drain_collisions(phys_scene *s, phys_collision *out, size_t cap);

/* 统计 */
typedef struct phys_stats {
    size_t bodies, cells, terrain_chunks;
    uint64_t manifolds, pairs;
    uint64_t collide_ns, step_ns, tick_ns; /* 累计：接触生成（各线程合计）、步进总时长、浮力等每刻工作 */
} phys_stats;
void phys_scene_stats(phys_scene *s, phys_stats *out);

/* 体素接触生成回调（collide.c） */
const b3VoxelManifold *phys_voxel_manifolds(const b3VoxelQuery *q, int *count, void *ctx);

#endif
