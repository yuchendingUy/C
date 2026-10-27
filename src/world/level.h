/*
 * 维度（世界层）：把区域器、推进调度、区块存储和物理场景接在一起。
 *
 * 每个区域拥有：
 *   - 一个区块存储：本区域的全部已加载区块（物理直接读它，方块只有这一份）
 *   - 一个物理场景：区域里有子世界时才建，最后一个子世界离开就销毁
 *   - 本区域的子世界：每个子世界自带一个方块存储（Sable 地块）和一个刚体
 *
 * 区域合并：小的一方的区块、子世界整体搬进大的一方（区块指针搬家、刚体在目标场景重建）。
 * 区域拆分：区块按所在区段归属分配；子世界按刚体当前位置所在的区块归属分配。
 * 两者都在区域器锁内、涉及的区域都不在推进时进行，不需要额外同步。
 *
 * 区块加载/卸载、子世界生成都投递成区块任务，在拥有该区块的区域线程上执行。
 */
#ifndef MCC_LEVEL_H
#define MCC_LEVEL_H

#include "chunk.h"
#include "../region/regionizer.h"
#include "../region/tick_scheduler.h"
#include "../phys/scene.h"

typedef struct mcc_level mcc_level;

/* 子世界：方块（地块坐标）+ 物理刚体，归属所在区域 */
typedef struct mcc_sublevel {
    int32_t id;
    mcc_chunk_store blocks;
    int32_t plot_cx, plot_cz;   /* 地块原点区块（地块坐标空间里的位置） */
} mcc_sublevel;

/* region->data */
typedef struct mcc_region_world {
    mcc_level *level;
    mcc_chunk_store *chunks;
    phys_scene *phys;           /* 没有子世界时为 NULL */
    mcc_hmap sublevels;         /* 编号 → mcc_sublevel* */
    uint64_t ticks;
} mcc_region_world;

typedef struct mcc_level_config {
    int32_t min_y, height;           /* 方块单位，16 的倍数 */
    int tick_threads;
    phys_scene_config phys;
    int phys_substeps;               /* 每刻物理步数，默认 2（Sable 默认 1/40 秒一步） */
    mcc_regionizer_config region;    /* 全 0 用默认值 */
    /* 每刻推进物理之前调用（游戏逻辑占位），区域线程上执行，可为 NULL */
    void (*tick_hook)(void *ctx, mcc_region *r, mcc_region_world *w);
    /* 每刻推进物理之后调用（取最新位姿等），区域线程上执行，可为 NULL */
    void (*post_tick_hook)(void *ctx, mcc_region *r, mcc_region_world *w);
    void *hook_ctx;
    /* 区域销毁时仍留在区域里的子世界（应已随区块卸载保存），为 NULL 时直接释放 */
    void (*sublevel_orphan)(void *ctx, mcc_sublevel *sl);
} mcc_level_config;

struct mcc_level {
    mcc_level_config cfg;
    mcc_regionizer rz;
    mcc_tick_scheduler *sched;
    _Atomic(int32_t) next_sublevel_id;
    _Atomic(uint64_t) stat_chunk_moves, stat_body_moves;
};

mcc_level *mcc_level_create(const mcc_level_config *cfg);
void mcc_level_start(mcc_level *lv);
void mcc_level_stop(mcc_level *lv);
void mcc_level_destroy(mcc_level *lv);

/* ---- 任意线程调用：投递给拥有区块的区域执行 ---- */
/* 交出区块的所有权（已用 mcc_chunk_set_raw 填好），区域线程上加入存储 */
void mcc_level_load_chunk(mcc_level *lv, mcc_chunk *c);
/* 区域线程上移出存储、释放，再从区域器注销 */
void mcc_level_unload_chunk(mcc_level *lv, int32_t cx, int32_t cz);

/* 生成子世界：fill 在区域线程上调用，往子世界存储里填方块；之后自动开启质量追踪。
 * 刚体放在 pose 处，按 pose 所在区块投递；该区块未加载时返回 -1，否则返回子世界编号 */
typedef void (*mcc_sublevel_fill_fn)(void *arg, mcc_region_world *w, mcc_sublevel *sl);
int32_t mcc_level_spawn_sublevel(mcc_level *lv, const double pose[7], mcc_sublevel_fill_fn fill, void *arg);

/* ---- 区域线程上调用 ---- */
mcc_sublevel *mcc_region_world_sublevel(mcc_region_world *w, int32_t id);
void mcc_region_world_remove_sublevel(mcc_region_world *w, int32_t id);

typedef struct mcc_level_stats {
    size_t regions;
    uint64_t chunk_moves, body_moves;
    mcc_tick_stats tick;
} mcc_level_stats;
void mcc_level_stats_get(mcc_level *lv, mcc_level_stats *out);

#endif
