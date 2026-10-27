/*
 * Folia 式区域化：把世界按区段（16×16 区块）划分成若干独立区域，
 * 每个区域在任一时刻只被一个线程推进，彼此之间没有共享可变状态。
 *
 * 规则：
 *   - 区段内有已加载区块 → "活区段"
 *   - 活区段周围 empty_radius 圈内的区段（缓冲区段）与之同属一个区域
 *   - 两个区域的区段距离在 merge_radius 之内 → 必须合并
 *   - 区域推进完一刻后，死区段比例过高 → 删除死区段并按连通性拆分
 *
 * 状态：
 *   READY     就绪，可被调度线程抢占为 TICKING
 *   TICKING   正在被某个线程推进，其他线程不得修改
 *   TRANSIENT 等待正在推进的区域并入，不参与调度
 *   DEAD      已被合并或销毁
 */
#ifndef MCC_REGIONIZER_H
#define MCC_REGIONIZER_H

#include "../core/common.h"
#include "../core/cmap.h"
#include <pthread.h>

enum mcc_region_state {
    MCC_REGION_READY = 0,
    MCC_REGION_TICKING = 1,
    MCC_REGION_TRANSIENT = 2,
    MCC_REGION_DEAD = 3,
};

typedef struct mcc_region mcc_region;
typedef struct mcc_section mcc_section;
typedef struct mcc_regionizer mcc_regionizer;

/* 投递到区块的任务：在拥有该区块的区域线程上执行 */
typedef void (*mcc_chunk_task_fn)(mcc_region *region, int32_t cx, int32_t cz, void *arg);
typedef struct {
    int32_t cx, cz;
    mcc_chunk_task_fn fn;
    void *arg;
} mcc_chunk_task;

struct mcc_section {
    int32_t sx, sz;
    mcc_region *_Atomic region;
    int32_t chunk_count;      /* 本区段已加载的区块数 */
    int32_t buffer_refs;      /* 周围（含自身）活区段数量，为 0 即死区段 */
    uint32_t visit_mark;      /* 拆分时的遍历标记 */
    _Atomic bool removed;     /* 已从区段表删除 */
};

typedef MCC_VEC(mcc_section *) mcc_section_vec;
typedef MCC_VEC(mcc_region *) mcc_region_vec;
typedef MCC_VEC(mcc_chunk_task) mcc_task_vec;

struct mcc_region {
    uint64_t id;
    _Atomic int state;
    _Atomic int refs;   /* 区域器持 1 个引用，调度堆中每个条目持 1 个；归零后交给纪元回收 */
    mcc_regionizer *rz;

    /* 以下字段仅在持有区域器锁时修改 */
    mcc_section_vec sections;
    size_t dead_sections;
    mcc_region *merge_into_later;      /* 推进结束后要并入的目标 */
    mcc_region_vec expecting_from;     /* 等待这些区域推进结束后并入本区域 */

    /* 区块任务队列 */
    pthread_mutex_t task_lock;
    mcc_task_vec tasks;

    /* 调度器字段 */
    _Atomic bool in_schedule;
    uint64_t next_tick_start;
    uint64_t tick_count;
    double tick_time_ema_ns;

    void *data;  /* 世界层的区域数据 */
};

typedef struct mcc_region_callbacks {
    void *ctx;
    /* 新建区域数据 */
    void *(*create_data)(void *ctx, mcc_region *r);
    /* 销毁区域数据 */
    void (*destroy_data)(void *ctx, mcc_region *r);
    /* 区域变为可调度 */
    void (*on_active)(void *ctx, mcc_region *r);
    /* from 的全部数据并入 into（二者均不在推进中） */
    void (*merge)(void *ctx, mcc_region *from, mcc_region *into);
    /* from 被拆分：区段已分配完毕，用 mcc_regionizer_region_of_chunk_locked 判断归属 */
    void (*split)(void *ctx, mcc_region *from, mcc_region **into, size_t n);
} mcc_region_callbacks;

typedef struct mcc_regionizer_config {
    int section_shift;       /* 区段大小（区块位移），默认 4 */
    int empty_radius;        /* 缓冲区段半径，默认 2 */
    int merge_radius;        /* 合并半径，默认 1 */
    int max_dead_percent;    /* 死区段比例阈值，默认 10 */
    int min_dead_to_recalc;  /* 至少多少个死区段才重算，默认 4 */
} mcc_regionizer_config;

struct mcc_regionizer {
    mcc_regionizer_config cfg;
    mcc_region_callbacks cb;
    pthread_mutex_t lock;
    mcc_cmap sections;           /* 区段键 → mcc_section* */
    _Atomic uint64_t next_region_id;
    uint32_t visit_gen;
    _Atomic size_t region_count;
    /* 统计 */
    _Atomic uint64_t stat_merges, stat_deferred_merges, stat_splits, stat_kills;
};

void mcc_regionizer_init(mcc_regionizer *rz, const mcc_regionizer_config *cfg, const mcc_region_callbacks *cb);
void mcc_regionizer_destroy(mcc_regionizer *rz);

/* 区块加载/卸载登记（任意线程，内部加锁） */
void mcc_regionizer_add_chunk(mcc_regionizer *rz, int32_t cx, int32_t cz);
void mcc_regionizer_remove_chunk(mcc_regionizer *rz, int32_t cx, int32_t cz);

/* 无锁查询区块所属区域（调用方处于纪元临界区内；结果可能瞬间过期，仅作提示） */
mcc_region *mcc_regionizer_region_of_chunk(mcc_regionizer *rz, int32_t cx, int32_t cz);
/* 持锁时（合并/拆分回调里）的精确查询 */
mcc_region *mcc_regionizer_region_of_chunk_locked(mcc_regionizer *rz, int32_t cx, int32_t cz);

/* 引用计数：调度器持有区域指针期间必须持引用 */
void mcc_region_ref(mcc_region *r);
void mcc_region_unref(mcc_region *r);

/* 调度线程抢占区域：READY → TICKING */
bool mcc_regionizer_try_mark_ticking(mcc_region *r);
/* 推进结束：处理延迟合并、死区段清理与拆分。
 * 返回 true 表示区域仍存活并已回到 READY，调用方继续调度；
 * 拆分出的新区域通过 on_active 回调交给调度器。 */
bool mcc_regionizer_release(mcc_region *r);

/* 向区块所属区域投递任务；区块所在区段不存在时返回 false */
bool mcc_regionizer_queue_chunk_task(mcc_regionizer *rz, int32_t cx, int32_t cz, mcc_chunk_task_fn fn, void *arg);
/* 区域线程在推进中取出并执行本区域全部任务，返回执行数 */
size_t mcc_region_run_tasks(mcc_region *r);

/* 调试钩子：任务入队/搬家时回调（测试用，默认 NULL） */
enum { MCC_RZ_EV_PUSH = 1, MCC_RZ_EV_MOVE_MERGE = 2, MCC_RZ_EV_MOVE_SPLIT = 3 };
extern void (*mcc_regionizer_debug_hook)(int ev, uint64_t from_id, uint64_t to_id, const mcc_chunk_task *t);

/* 一致性自检（测试用，持锁） */
bool mcc_regionizer_verify(mcc_regionizer *rz, char *err, size_t errlen);

#endif
