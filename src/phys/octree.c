#include "octree.h"
#include "../core/log.h"

void phys_octree_init(phys_octree *t, int log_size) {
    t->log_size = log_size;
    t->cap = 256;
    t->buf = mcc_xcalloc((size_t)t->cap, sizeof(int32_t));
    t->size = 1;
    t->free_head = -1;
}

void phys_octree_free(phys_octree *t) {
    free(t->buf);
    t->buf = NULL;
    t->cap = t->size = 0;
}

size_t phys_octree_memory(const phys_octree *t) { return (size_t)t->cap * sizeof(int32_t); }

MCC_INLINE int32_t octant(int x, int y, int z) { return (x & 1) | ((y & 1) << 1) | ((z & 1) << 2); }
MCC_INLINE int32_t to_node(int32_t block) { return -block - 1; }
MCC_INLINE int32_t from_node(int32_t node) { return -node - 1; }

static int32_t allocate_branch(phys_octree *t) {
    if (t->free_head != -1) {
        int32_t index = t->free_head - 1;
        t->free_head = t->buf[index];
        return index;
    }
    if (t->size + 8 > t->cap) {
        int64_t ncap = (int64_t)((double)t->cap * 1.5 + 0.999);
        if (ncap < t->size + 8) ncap = t->size + 8;
        if (ncap > INT32_MAX - 16) mcc_log_fatal("八叉树缓冲区已满");
        t->buf = mcc_xrealloc(t->buf, (size_t)ncap * sizeof(int32_t));
        memset(t->buf + t->cap, 0, (size_t)(ncap - t->cap) * sizeof(int32_t));
        t->cap = (int32_t)ncap;
    }
    int32_t index = t->size;
    t->size += 8;
    return index;
}

static void split(phys_octree *t, int32_t index) {
    int32_t node = t->buf[index];
    int32_t start = allocate_branch(t);
    t->buf[index] = start;
    for (int i = 0; i < 8; i++) t->buf[start + i] = node;
}

static void delete_children(phys_octree *t, int32_t start) {
    t->buf[start] = t->free_head;
    t->free_head = start;
    for (int i = 1; i < 8; i++) t->buf[start + i] = 0;
}

static void merge(phys_octree *t, int32_t index) {
    int32_t start = t->buf[index];
    int32_t node = t->buf[start];
    for (int i = 1; i < 8; i++)
        if (t->buf[start + i] != node) return;
    if (node == 0) {
        t->buf[index] = node;
        delete_children(t, start);
        t->free_head = start + 1;
    }
}

bool phys_octree_insert(phys_octree *t, int x, int y, int z, int32_t block) {
    int shift = t->log_size - 1;
    int32_t index = 0;
    int32_t node = t->buf[0];
    int32_t visited[32];
    int nvisited = 0;
    const int32_t want = to_node(block);

    while (shift >= 0) {
        if (node == want) return false;
        int32_t oct = octant(x >> shift, y >> shift, z >> shift);
        if (node > 0) visited[nvisited++] = index;
        else split(t, index);
        int32_t start = t->buf[index];
        index = start + oct;
        node = t->buf[index];
        shift--;
    }
    t->buf[index] = want;
    /* 自底向上尝试合并全空分支（与原实现一致：只合并已访问过的分支） */
    for (int i = nvisited - 1; i >= 0; i--) merge(t, visited[i]);
    return true;
}

int32_t phys_octree_query(const phys_octree *t, int x, int y, int z, int log_size_of_target) {
    const int size = 1 << t->log_size;
    if (x < 0 || y < 0 || z < 0 || x >= size || y >= size || z >= size) return PHYS_OCTREE_EMPTY;
    int shift = t->log_size - 1;
    int32_t node = t->buf[0];
    while (shift >= log_size_of_target) {
        if (node < 0) return from_node(node);
        if (node == 0) return PHYS_OCTREE_EMPTY;
        node = t->buf[node + octant(x >> shift, y >> shift, z >> shift)];
        shift--;
    }
    if (node < 0) return from_node(node);
    if (node == 0) return PHYS_OCTREE_EMPTY;
    return PHYS_OCTREE_MIXED;
}
