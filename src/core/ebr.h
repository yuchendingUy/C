/*
 * 纪元回收：无锁读者读取共享结构时进入临界区，
 * 被替换下来的旧对象登记"退役"，等所有读者都离开旧纪元后再释放。
 */
#ifndef MCC_EBR_H
#define MCC_EBR_H

#include "common.h"

#define MCC_EBR_MAX_THREADS 512

void mcc_ebr_enter(void);
void mcc_ebr_exit(void);
void mcc_ebr_retire(void *p, void (*free_fn)(void *));
/* 尝试释放已经没有读者的退役对象，返回释放数量 */
size_t mcc_ebr_reclaim(void);
/* 关闭时强制释放全部（调用方保证没有读者） */
void mcc_ebr_drain(void);

#endif
