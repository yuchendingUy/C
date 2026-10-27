/*
 * 通用任务线程池（工作窃取）。
 * 每个工作线程有自己的双端队列，空闲时从别的线程窃取；
 * 等待任务组完成的线程（包括区域推进线程）会顺手执行任务，不会干等。
 */
#ifndef MCC_JOBS_H
#define MCC_JOBS_H

#include "common.h"

typedef void (*mcc_job_fn)(void *arg);

typedef struct mcc_job_group {
    _Atomic int64_t pending;
} mcc_job_group;

void mcc_jobs_start(int nthreads);
void mcc_jobs_stop(void);
int mcc_jobs_thread_count(void);

MCC_INLINE void mcc_job_group_init(mcc_job_group *g) { atomic_init(&g->pending, 0); }

/* 提交任务；group 可为 NULL（不需要等待的后台任务） */
void mcc_jobs_submit(mcc_job_group *g, mcc_job_fn fn, void *arg);
/* 等待任务组全部完成，等待期间帮忙执行任务 */
void mcc_jobs_wait(mcc_job_group *g);

/* 并行循环：把 [0, n) 按 grain 切块并行执行 fn(arg, begin, end)，返回时全部完成 */
typedef void (*mcc_range_fn)(void *arg, size_t begin, size_t end);
void mcc_jobs_parallel_for(size_t n, size_t grain, mcc_range_fn fn, void *arg);

#endif
