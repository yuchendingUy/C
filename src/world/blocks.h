/*
 * 方块与方块状态注册表。启动时从 blocks.txt 载入，之后只读，任意线程无锁访问。
 */
#ifndef MCC_BLOCKS_H
#define MCC_BLOCKS_H

#include "../core/common.h"

typedef uint32_t mcc_state_t;  /* 方块状态编号（与网络编号一致） */
typedef uint16_t mcc_block_t;  /* 方块编号（注册表顺序） */

#define MCC_BLOCK_TRANSPARENT 1u
#define MCC_BLOCK_FULL_BOX    2u
#define MCC_BLOCK_DIGGABLE    4u

typedef struct mcc_block_prop {
    char *name;
    int nvalues;
    char **values;
    uint32_t stride;   /* 该属性取值加 1 时状态编号增加多少 */
} mcc_block_prop;

typedef struct mcc_block_def {
    char *name;            /* 命名空间:名称 */
    mcc_state_t first_state;
    uint32_t state_count;
    mcc_state_t default_state;
    float hardness, resistance;
    uint8_t flags, emit_light, filter_light;
    uint8_t nprops;
    mcc_block_prop *props;
} mcc_block_def;

/* 每个状态的紧凑信息，热路径只读这一张表 */
typedef struct mcc_state_info {
    mcc_block_t block;
    uint8_t flags;
    uint8_t light;         /* 低 4 位发光，高 4 位滤光 */
} mcc_state_info;

typedef struct mcc_block_registry {
    mcc_block_def *blocks;
    uint32_t block_count;
    mcc_state_info *states;
    uint32_t state_count;
    int direct_bits;       /* 区段直接模式所需位数 = ceil(log2(状态总数)) */
    void *name_index;
} mcc_block_registry;

extern mcc_block_registry mcc_blocks;

bool mcc_blocks_load(const char *path);
void mcc_blocks_free(void);

/* 名称查找，返回 -1 表示不存在 */
int32_t mcc_block_by_name(const char *name);
mcc_state_t mcc_block_default_state(const char *name);  /* 不存在时致命错误 */

MCC_INLINE mcc_block_t mcc_state_block(mcc_state_t s) { return mcc_blocks.states[s].block; }
MCC_INLINE const mcc_block_def *mcc_state_def(mcc_state_t s) { return &mcc_blocks.blocks[mcc_blocks.states[s].block]; }
MCC_INLINE bool mcc_state_is_air(mcc_state_t s) { return mcc_blocks.states[s].flags & 0x80; }
MCC_INLINE uint8_t mcc_state_emit_light(mcc_state_t s) { return mcc_blocks.states[s].light & 15; }
MCC_INLINE uint8_t mcc_state_filter_light(mcc_state_t s) { return mcc_blocks.states[s].light >> 4; }
MCC_INLINE bool mcc_state_full_box(mcc_state_t s) { return mcc_blocks.states[s].flags & MCC_BLOCK_FULL_BOX; }

/* 属性读写：prop 为属性下标，返回/传入取值下标 */
int mcc_state_prop_index(mcc_state_t s, const char *prop);  /* 属性下标，-1 不存在 */
int mcc_state_get(mcc_state_t s, int prop);
mcc_state_t mcc_state_with(mcc_state_t s, int prop, int value);
/* 按名字读写，取值不存在返回原状态 */
const char *mcc_state_get_str(mcc_state_t s, const char *prop);
mcc_state_t mcc_state_with_str(mcc_state_t s, const char *prop, const char *value);
/* 调试输出，如 minecraft:oak_stairs[facing=north,half=bottom,...] */
void mcc_state_describe(mcc_state_t s, char *buf, size_t len);

#endif
