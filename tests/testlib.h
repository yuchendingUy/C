/*
 * 测试共用：载入真实方块数据，用函数描述方块分布来生成区块存储
 */
#ifndef MCC_TESTLIB_H
#define MCC_TESTLIB_H

#include "../src/world/chunk.h"
#include "../src/phys/colliders.h"
#include "../src/core/log.h"

#define TEST_MIN_Y (-64)
#define TEST_HEIGHT 384

static mcc_state_t STONE, WATER;

static void test_load_blocks(void) {
    if (!mcc_blocks_load(MCC_SOURCE_DIR "/data/blocks.txt")) mcc_log_fatal("载入方块注册表失败");
    if (!phys_block_colliders_load(MCC_SOURCE_DIR "/data/shapes.txt", MCC_SOURCE_DIR "/data/phys_props.txt")) mcc_log_fatal("载入碰撞形状失败");
    phys_colliders_finalize();
    STONE = mcc_block_default_state("stone");
    WATER = mcc_block_default_state("water");
}

/* 原版 + 机械动力方块 */
static void test_load_blocks_with_create(void) {
    const char *blocks[] = { MCC_SOURCE_DIR "/data/blocks.txt", MCC_SOURCE_DIR "/data/create/blocks.txt" };
    const char *shapes[] = { MCC_SOURCE_DIR "/data/shapes.txt", MCC_SOURCE_DIR "/data/create/shapes.txt" };
    if (!mcc_blocks_load_files(blocks, 2)) mcc_log_fatal("载入方块注册表失败");
    if (!phys_block_colliders_load_files(shapes, 2, MCC_SOURCE_DIR "/data/phys_props.txt")) mcc_log_fatal("载入碰撞形状失败");
    phys_colliders_finalize();
    STONE = mcc_block_default_state("stone");
    WATER = mcc_block_default_state("water");
}

static void test_free_blocks(void) {
    phys_colliders_free();
    mcc_blocks_free();
}

typedef mcc_state_t (*block_fn)(int32_t x, int32_t y, int32_t z, void *ctx);

/* 生成区块 [cx0, cx1] × [cz0, cz1]，方块 y ∈ [y0, y1] 由 f 决定，加入存储 */
static void test_fill(mcc_chunk_store *st, block_fn f, void *ctx, int32_t cx0, int32_t cx1, int32_t cz0, int32_t cz1, int32_t y0,
                      int32_t y1) {
    for (int32_t cx = cx0; cx <= cx1; cx++)
        for (int32_t cz = cz0; cz <= cz1; cz++) {
            mcc_chunk *c = mcc_chunk_new(st, cx, cz);
            for (int32_t y = y0; y <= y1; y++)
                for (int lz = 0; lz < 16; lz++)
                    for (int lx = 0; lx < 16; lx++) {
                        mcc_state_t s = f((cx << 4) + lx, y, (cz << 4) + lz, ctx);
                        if (s) mcc_chunk_set_raw(c, lx, y, lz, s);
                    }
            mcc_store_add_chunk(st, c);
        }
}

/* 地形：y < 61 全是石头（顶面 y=61）；相对 (ox, oz) x∈[64,96)、z∈[-16,16) 挖出水池，水面顶 y=71 */
typedef struct { int32_t ox, oz; } terrain_ctx;
static mcc_state_t terrain(int32_t x, int32_t y, int32_t z, void *p) {
    terrain_ctx *t = (terrain_ctx *)p;
    int32_t lx = x - t->ox, lz = z - t->oz;
    bool pool = lx >= 64 && lx < 96 && lz >= -16 && lz < 16;
    if (pool) {
        if (y < 50) return STONE;
        if (y < 71) return WATER;
        return 0;
    }
    return y < 61 ? STONE : 0;
}

/* 子世界：地块坐标里的 3×3×3 实心立方体 */
typedef struct { int32_t bx, by, bz; } cube_ctx;
static mcc_state_t cube(int32_t x, int32_t y, int32_t z, void *p) {
    cube_ctx *c = (cube_ctx *)p;
    return (x >= c->bx && x < c->bx + 3 && y >= c->by && y < c->by + 3 && z >= c->bz && z < c->bz + 3) ? STONE : 0;
}

static void test_cube_store(mcc_chunk_store *st, cube_ctx *c) {
    mcc_store_init(st, TEST_MIN_Y, TEST_HEIGHT);
    test_fill(st, cube, c, c->bx >> 4, (c->bx + 2) >> 4, c->bz >> 4, (c->bz + 2) >> 4, c->by, c->by + 2);
}

#endif
