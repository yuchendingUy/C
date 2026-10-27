/*
 * mcc - 高性能 C 语言我的世界 1.21.1 + 机械动力 服务端
 * 公共定义
 */
#ifndef MCC_COMMON_H
#define MCC_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define MCC_LIKELY(x)   __builtin_expect(!!(x), 1)
#define MCC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define MCC_CACHELINE   64
#define MCC_ALIGNED     __attribute__((aligned(MCC_CACHELINE)))
#define MCC_UNUSED      __attribute__((unused))
#define MCC_INLINE      static inline __attribute__((always_inline))
#define MCC_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define MCC_MIN(a, b) ((a) < (b) ? (a) : (b))
#define MCC_MAX(a, b) ((a) > (b) ? (a) : (b))

void *mcc_xmalloc(size_t n);
void *mcc_xcalloc(size_t n, size_t sz);
void *mcc_xrealloc(void *p, size_t n);
char *mcc_xstrdup(const char *s);
void *mcc_xaligned(size_t align, size_t n);

/* 区块/区段坐标打包为 64 位键：高 32 位为 x，低 32 位为 z */
MCC_INLINE uint64_t mcc_key2(int32_t x, int32_t z) {
    return ((uint64_t)(uint32_t)x << 32) | (uint64_t)(uint32_t)z;
}
MCC_INLINE int32_t mcc_key2_x(uint64_t k) { return (int32_t)(uint32_t)(k >> 32); }
MCC_INLINE int32_t mcc_key2_z(uint64_t k) { return (int32_t)(uint32_t)k; }

/* 方块坐标打包，与原版 BlockPos.asLong 一致：x 26 位，z 26 位，y 12 位 */
MCC_INLINE uint64_t mcc_blockpos_pack(int32_t x, int32_t y, int32_t z) {
    return (((uint64_t)x & 0x3FFFFFFULL) << 38) | (((uint64_t)z & 0x3FFFFFFULL) << 12) | ((uint64_t)y & 0xFFFULL);
}
MCC_INLINE int32_t mcc_blockpos_x(uint64_t p) { return (int32_t)((int64_t)p >> 38); }
MCC_INLINE int32_t mcc_blockpos_y(uint64_t p) { return (int32_t)((int64_t)(p << 52) >> 52); }
MCC_INLINE int32_t mcc_blockpos_z(uint64_t p) { return (int32_t)((int64_t)(p << 26) >> 38); }

/* 64 位混合哈希（splitmix64 末段） */
MCC_INLINE uint64_t mcc_hash64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

uint64_t mcc_nanotime(void);

#include <stdio.h>
/* 读一行（含换行符），*line 按需扩大；文件结束返回 -1（各平台通用的 getline） */
long mcc_getline(char **line, size_t *cap, FILE *f);

/* 动态数组 */
#define MCC_VEC(T) struct { T *data; size_t len, cap; }
#define mcc_vec_push(v, x) do { \
        if ((v)->len == (v)->cap) { \
            (v)->cap = (v)->cap ? (v)->cap * 2 : 8; \
            (v)->data = (__typeof__((v)->data))mcc_xrealloc((v)->data, (v)->cap * sizeof(*(v)->data)); \
        } \
        (v)->data[(v)->len++] = (x); \
    } while (0)
#define mcc_vec_free(v) do { free((v)->data); (v)->data = NULL; (v)->len = (v)->cap = 0; } while (0)
#define mcc_vec_reserve(v, n) do { \
        if ((v)->cap < (n)) { (v)->cap = (n); \
            (v)->data = (__typeof__((v)->data))mcc_xrealloc((v)->data, (v)->cap * sizeof(*(v)->data)); } \
    } while (0)

#endif
