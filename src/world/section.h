/*
 * 区段：16×16×16 方块的状态存储（对应原版 LevelChunkSection），世界方块数据的唯一存放处。
 *
 * 状态用调色板压缩，存储位宽只取 0 / 4 / 8 / 16，读取只需移位和查表：
 *   0   单值：整个区段同一个状态，不分配数据
 *   4   调色板 ≤ 16 项，每字节两个方块
 *   8   调色板 ≤ 256 项，每字节一个方块
 *   16  直接存状态编号
 * 网络和存档用的原版紧凑格式在发送/保存时另行转换。
 *
 * 区段里同时存放物理占用位图（物理直接读取，不再复制方块数据）：
 *   solid   外露实心：有碰撞箱，且不是六面都被整块包围的内部方块
 *   liquid  含流体
 * 位图由区块存储在写方块的同一次调用里更新（需要邻居信息），两个位图都为空时不分配。
 *
 * 线程：区段归属拥有其区块的区域，只由该区域线程写；物理步进期间该区域没有写入，
 * 物理工作线程可以并发只读。
 */
#ifndef MCC_SECTION_H
#define MCC_SECTION_H

#include "blocks.h"

#define MCC_SECTION_BLOCKS 4096

/* 区段内下标，与原版一致：x 最快，其次 z，最后 y */
MCC_INLINE int mcc_section_index(int x, int y, int z) { return x | (z << 4) | (y << 8); }

/* 物理占用位图：rows[y][z] 的第 x 位 */
typedef struct mcc_section_phys {
    uint16_t solid[16][16];
    uint16_t liquid[16][16];
} mcc_section_phys;

typedef struct mcc_chunk_section {
    uint8_t bits;              /* 0 / 4 / 8 / 16 */
    uint16_t palette_len;
    uint16_t non_air;          /* 非空气方块数 */
    uint16_t nonzero;          /* 状态编号非 0 的方块数，为 0 时区段可整个释放 */
    uint16_t solid_count;      /* solid 位图置位数 */
    uint16_t liquid_count;     /* liquid 位图置位数 */
    mcc_state_t single;        /* bits == 0 时的状态 */
    uint16_t *palette;         /* bits 4/8 */
    union {
        uint8_t *d8;           /* bits 4/8 */
        uint16_t *d16;         /* bits 16 */
        void *data;
    };
    mcc_section_phys *phys;    /* 两个计数都为 0 时为 NULL */
} mcc_chunk_section;

mcc_chunk_section *mcc_section_new(mcc_state_t fill);
void mcc_section_free(mcc_chunk_section *s);

MCC_INLINE mcc_state_t mcc_section_get(const mcc_chunk_section *s, int idx) {
    switch (s->bits) {
    case 0: return s->single;
    case 4: return s->palette[(s->d8[idx >> 1] >> ((idx & 1) << 2)) & 15];
    case 8: return s->palette[s->d8[idx]];
    default: return s->d16[idx];
    }
}

/* 写状态，返回旧状态。只维护 non_air，物理位图由区块存储维护 */
mcc_state_t mcc_section_set(mcc_chunk_section *s, int idx, mcc_state_t state);

/* 把每行（[y][z]，第 x 位）方块的物理标志展开成位图，任一输出可为 NULL */
void mcc_section_flag_rows(const mcc_chunk_section *s, uint16_t solid[16][16], uint16_t liquid[16][16], uint16_t full[16][16]);

/* 设置一个方块的物理位，维护计数并按需分配/释放位图 */
void mcc_section_set_phys(mcc_chunk_section *s, int x, int y, int z, bool solid, bool liquid);

MCC_INLINE bool mcc_section_solid_bit(const mcc_chunk_section *s, int x, int y, int z) {
    return s->phys && (s->phys->solid[y][z] >> x & 1);
}
MCC_INLINE bool mcc_section_liquid_bit(const mcc_chunk_section *s, int x, int y, int z) {
    return s->phys && (s->phys->liquid[y][z] >> x & 1);
}

size_t mcc_section_memory(const mcc_chunk_section *s);

#endif
