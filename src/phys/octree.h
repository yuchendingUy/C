/*
 * 扁平数组八叉树（移植自 Sable marten/octree.rs）
 *
 * 节点以 int32 存于一个连续数组：
 *   正数  分支节点，值为 8 个子节点的起始下标
 *   负数  叶子节点，值为 -(碰撞体编号) - 1
 *   零    空节点
 * 全空的分支会被合并回收。
 */
#ifndef MCC_PHYS_OCTREE_H
#define MCC_PHYS_OCTREE_H

#include "../core/common.h"

#define PHYS_OCTREE_EMPTY (-2)   /* 查询结果：空 */
#define PHYS_OCTREE_MIXED (-1)   /* 查询结果：目标层级上是混合分支 */

typedef struct phys_octree {
    int32_t log_size;
    int32_t *buf;
    int32_t size;      /* 已占用节点数 */
    int32_t cap;
    int32_t free_head; /* 空闲链表头（下标 + 1），-1 表示无 */
} phys_octree;

void phys_octree_init(phys_octree *t, int log_size);
void phys_octree_free(phys_octree *t);
/* 设置 (x,y,z) 处的碰撞体编号（>=0），传 -1 表示清空。返回是否改变了树 */
bool phys_octree_insert(phys_octree *t, int x, int y, int z, int32_t block);
/* 查询：返回碰撞体编号；PHYS_OCTREE_EMPTY 空；PHYS_OCTREE_MIXED 该层级为混合分支 */
int32_t phys_octree_query(const phys_octree *t, int x, int y, int z, int log_size_of_target);
MCC_INLINE bool phys_octree_is_empty(const phys_octree *t) { return t->buf[0] == 0; }
size_t phys_octree_memory(const phys_octree *t);

#endif
