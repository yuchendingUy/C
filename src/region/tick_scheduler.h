/*
 * 区域推进调度器：每个区域按自己的节奏独立 20 TPS 推进。
 * 所有待推进区域按"下一刻开始时间"放进小顶堆，推进线程取最早到期的执行；
 * 落后时直接连续追刻，落后超过上限则放弃追赶，重新对齐当前时间。
 */
#ifndef MCC_TICK_SCHEDULER_H
#define MCC_TICK_SCHEDULER_H

#include "regionizer.h"

typedef struct mcc_tick_scheduler mcc_tick_scheduler;

typedef struct mcc_tick_config {
    int threads;                 /* 推进线程数 */
    uint64_t tick_interval_ns;   /* 默认 50ms */
    uint64_t max_catchup_ns;     /* 落后超过此值放弃追赶，默认 1s */
    /* 推进回调：在推进线程上执行，区域处于 TICKING 状态 */
    void (*tick)(void *ctx, mcc_region *r, uint64_t tick_start_ns);
    void *ctx;
} mcc_tick_config;

mcc_tick_scheduler *mcc_tick_scheduler_create(mcc_regionizer *rz, const mcc_tick_config *cfg);
void mcc_tick_scheduler_start(mcc_tick_scheduler *s);
void mcc_tick_scheduler_stop(mcc_tick_scheduler *s);
void mcc_tick_scheduler_destroy(mcc_tick_scheduler *s);

/* 区域器 on_active 回调里调用 */
void mcc_tick_scheduler_on_active(mcc_tick_scheduler *s, mcc_region *r);

typedef struct mcc_tick_stats {
    uint64_t ticks;
    uint64_t skipped_catchup;
    uint64_t max_tick_ns;
    size_t queued;
} mcc_tick_stats;
void mcc_tick_scheduler_stats(mcc_tick_scheduler *s, mcc_tick_stats *out);

#endif
