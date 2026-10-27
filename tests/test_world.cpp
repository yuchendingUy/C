/*
 * 世界区块存储测试：
 *   - 区段调色板 0/4/8/16 位扩宽与读写一致
 *   - 随机写方块（含负坐标、区块边界）、随机移出/加入区块后，物理位图与逐方块重算一致
 *   - 占用查询（有无 / 收集）与逐方块暴力判断一致，新游标第一次查任意区块都正确
 *   - 监听者收到的事件与写入一致
 */
#include "testlib.h"
#include <stdio.h>

static bool g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { mcc_error(__VA_ARGS__); g_fail = true; } else { mcc_info("  通过：" __VA_ARGS__); } } while (0)

static uint64_t g_rng = 0x2545F4914F6CDD1DULL;
static uint32_t rnd(uint32_t n) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng % n);
}

static void test_palette(void) {
    mcc_info("== 区段调色板");
    mcc_chunk_section *s = mcc_section_new(0);
    static mcc_state_t ref[MCC_SECTION_BLOCKS];
    memset(ref, 0, sizeof ref);
    bool ok = true;
    int widths[4] = { 0 }, nw = 0;
    /* 逐步引入更多不同状态：1 → 16 → 256 → 600 种 */
    for (int round = 0; round < 4 && ok; round++) {
        /* 加上状态 0：2 / 16 / 256 / 601 种 */
        uint32_t kinds = round == 0 ? 1 : round == 1 ? 15 : round == 2 ? 255 : 600;
        for (int k = 0; k < 20000; k++) {
            int idx = (int)rnd(MCC_SECTION_BLOCKS);
            mcc_state_t st = rnd(kinds + 1) == 0 ? 0 : 1 + rnd(kinds);
            mcc_state_t old = mcc_section_set(s, idx, st);
            if (old != ref[idx]) ok = false;
            ref[idx] = st;
        }
        widths[nw++] = s->bits;
        uint32_t non_air = 0, nonzero = 0;
        for (int i = 0; i < MCC_SECTION_BLOCKS; i++) {
            if (mcc_section_get(s, i) != ref[i]) ok = false;
            non_air += !mcc_state_is_air(ref[i]);
            nonzero += ref[i] != 0;
        }
        if (non_air != s->non_air || nonzero != s->nonzero) ok = false;
    }
    CHECK(ok, "随机读写 8 万次与参照数组一致，非空气计数正确");
    CHECK(widths[0] == 4 && widths[1] == 4 && widths[2] == 8 && widths[3] == 16, "位宽按状态种类扩宽：%d → %d → %d → %d", widths[0],
          widths[1], widths[2], widths[3]);
    mcc_section_free(s);
}

/* 判断定义：有碰撞箱且不是六面整块包围（未加载按空气） */
static bool def_full(const mcc_chunk_store *st, int32_t x, int32_t y, int32_t z) {
    return mcc_state_pflags(mcc_store_get(st, x, y, z)) & MCC_PHYS_FULL;
}
static bool def_solid(const mcc_chunk_store *st, int32_t x, int32_t y, int32_t z) {
    if (!mcc_store_chunk(st, x >> 4, z >> 4)) return false;
    if (!(mcc_state_pflags(mcc_store_get(st, x, y, z)) & MCC_PHYS_SOLID)) return false;
    return !(def_full(st, x - 1, y, z) && def_full(st, x + 1, y, z) && def_full(st, x, y - 1, z) && def_full(st, x, y + 1, z) &&
             def_full(st, x, y, z - 1) && def_full(st, x, y, z + 1));
}
static bool def_liquid(const mcc_chunk_store *st, int32_t x, int32_t y, int32_t z) {
    return mcc_store_chunk(st, x >> 4, z >> 4) && (mcc_state_pflags(mcc_store_get(st, x, y, z)) & MCC_PHYS_LIQUID);
}

typedef struct { int added, removed, changed; } events;
static void ev_added(void *c, mcc_chunk *ch) { (void)ch; ((events *)c)->added++; }
static void ev_removed(void *c, mcc_chunk *ch) { (void)ch; ((events *)c)->removed++; }
static void ev_changed(void *c, int32_t x, int32_t y, int32_t z, mcc_state_t o, mcc_state_t n) {
    (void)x; (void)y; (void)z; (void)o; (void)n;
    ((events *)c)->changed++;
}

static mcc_state_t gen(int32_t x, int32_t y, int32_t z, void *ctx) {
    (void)ctx;
    (void)x; (void)z;
    return y < 4 ? STONE : y < 6 ? WATER : 0;
}

static void test_store(void) {
    mcc_info("== 区块存储（区块 -2~1，含区块 (-1,-1)）");
    mcc_chunk_store st;
    mcc_store_init(&st, -16, 48);
    events ev = { 0 };
    mcc_store_listener l = { &ev, ev_added, ev_removed, ev_changed, NULL };
    mcc_store_listen(&st, &l);
    test_fill(&st, gen, NULL, -2, 1, -2, 1, -16, 31);
    char err[256];
    CHECK(mcc_store_verify(&st, err, sizeof err), "批量生成后位图正确%s%s", "", mcc_store_verify(&st, err, sizeof err) ? "" : err);

    mcc_state_t pool[] = {
        0, STONE, STONE, WATER, mcc_block_default_state("glass"), mcc_block_default_state("oak_slab"),
        mcc_block_default_state("oak_stairs"), mcc_state_with_str(mcc_block_default_state("oak_stairs"), "waterlogged", "true"),
        mcc_block_default_state("oak_fence"), mcc_block_default_state("short_grass"), mcc_block_default_state("cave_air"),
    };
    int sets = 0, changes = 0;
    bool verify_ok = true;
    for (int round = 0; round < 40 && verify_ok; round++) {
        for (int k = 0; k < 3000; k++) {
            /* 集中在区块边界附近与低处，覆盖跨区块邻居 */
            int32_t x = (int32_t)rnd(64) - 32, z = (int32_t)rnd(64) - 32, y = (int32_t)rnd(24) - 8;
            if (rnd(3) == 0) x = (int32_t)(rnd(4) * 16) - 32 + (int32_t)rnd(2) * 15;
            mcc_state_t s = pool[rnd(MCC_ARRAY_LEN(pool))];
            mcc_state_t before = mcc_store_get(&st, x, y, z);
            mcc_state_t old = mcc_store_set(&st, x, y, z, s);
            if (old == MCC_STATE_NONE) continue; /* 区块被移出了 */
            sets++;
            if (old != before) verify_ok = false;
            if (old != s) changes++;
        }
        /* 随机移出再加回一个区块（邻居边界要重算） */
        int32_t cx = (int32_t)rnd(4) - 2, cz = (int32_t)rnd(4) - 2;
        mcc_chunk *c = mcc_store_remove_chunk(&st, cx, cz);
        if (!mcc_store_verify(&st, err, sizeof err)) { mcc_error("移出区块后：%s", err); verify_ok = false; }
        if (c && rnd(2)) mcc_store_add_chunk(&st, c);
        else if (c) { mcc_chunk_free(c); test_fill(&st, gen, NULL, cx, cx, cz, cz, -16, 31); }
        if (!mcc_store_verify(&st, err, sizeof err)) { mcc_error("第 %d 轮：%s", round, err); verify_ok = false; }
    }
    CHECK(verify_ok, "随机写 %d 次、移出/加回区块 40 次，每轮位图与逐方块重算一致", sets);
    CHECK(ev.changed == changes, "方块变化事件 %d 次，与实际改变次数 %d 一致", ev.changed, changes);
    CHECK(ev.added == 16 + 40 && ev.removed == 40, "区块加入事件 %d、移出事件 %d", ev.added, ev.removed);

    /* 占用查询对拍：每次都用新游标（新游标第一次查询任意区块都必须正确） */
    bool q_ok = true;
    int hits_total = 0;
    for (int k = 0; k < 4000 && q_ok; k++) {
        int32_t mn[3], mx[3];
        for (int i = 0; i < 3; i++) {
            int32_t c = (int32_t)rnd(72) - 36, r = (int32_t)rnd(i == 1 ? 6 : 9);
            mn[i] = c - r;
            mx[i] = c + (int32_t)rnd(9);
        }
        mn[1] = MCC_MAX(mn[1], -20); mx[1] = MCC_MIN(mx[1], 20);
        for (int kind = 0; kind < 2; kind++) {
            int expect = 0;
            for (int32_t x = mn[0]; x <= mx[0]; x++)
                for (int32_t y = mn[1]; y <= mx[1]; y++)
                    for (int32_t z = mn[2]; z <= mx[2]; z++) expect += kind ? def_liquid(&st, x, y, z) : def_solid(&st, x, y, z);
            mcc_store_cursor cur;
            mcc_cursor_init(&cur, &st);
            bool any = mcc_cursor_box_any(&cur, kind, mn, mx);
            mcc_cursor_init(&cur, &st);
            mcc_i32vec out = { 0 };
            mcc_cursor_box_collect(&cur, kind, mn, mx, &out);
            for (size_t i = 0; i < out.len; i += 3) {
                int32_t x = out.data[i], y = out.data[i + 1], z = out.data[i + 2];
                bool in = x >= mn[0] && x <= mx[0] && y >= mn[1] && y <= mx[1] && z >= mn[2] && z <= mx[2];
                if (!in || !(kind ? def_liquid(&st, x, y, z) : def_solid(&st, x, y, z))) q_ok = false;
            }
            if (any != (expect > 0) || (int)(out.len / 3) != expect) {
                mcc_error("查询 (%d,%d,%d)-(%d,%d,%d) 类型 %d：期望 %d，有无 %d，收集 %zu", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], kind,
                          expect, any, out.len / 3);
                q_ok = false;
            }
            hits_total += expect;
            mcc_vec_free(&out);
        }
    }
    CHECK(q_ok, "4000 个随机包围盒的占用查询与逐方块暴力判断一致（命中 %d 个方块）", hits_total);

    /* 新游标逐个方块读取，与存储读取一致（覆盖每个区块的第一次查询） */
    bool g_ok = true;
    for (int32_t cx = -2; cx <= 1; cx++)
        for (int32_t cz = -2; cz <= 1; cz++) {
            mcc_store_cursor cur;
            mcc_cursor_init(&cur, &st);
            int32_t x = cx * 16 + 3, z = cz * 16 + 5;
            if (mcc_cursor_get(&cur, x, 2, z) != mcc_store_get(&st, x, 2, z)) g_ok = false;
            if (mcc_cursor_section(&cur, cx, 0, cz) != mcc_chunk_section_at(mcc_store_chunk(&st, cx, cz), 0)) g_ok = false;
        }
    CHECK(g_ok, "新游标第一次读取每个区块（含 (-1,-1)）都正确%s", "");

    mcc_store_stats ss;
    mcc_store_stats_get(&st, &ss);
    mcc_info("  区块 %zu，区段 %zu（带物理位图 %zu），内存 %.1fKB", ss.chunks, ss.sections, ss.phys_sections, ss.memory / 1024.0);
    mcc_store_unlisten(&st, &l);
    mcc_store_free(&st);
}

/* 区段全部改回空气后应当被释放 */
static void test_section_release(void) {
    mcc_info("== 区段释放");
    mcc_chunk_store st;
    mcc_store_init(&st, -16, 48);
    mcc_store_add_chunk(&st, mcc_chunk_new(&st, 0, 0));
    for (int i = 0; i < 100; i++) mcc_store_set(&st, i % 16, 20, i / 16, STONE);
    const mcc_chunk *c = mcc_store_chunk(&st, 0, 0);
    bool had = mcc_chunk_section_at(c, 1) != NULL;
    for (int i = 0; i < 100; i++) mcc_store_set(&st, i % 16, 20, i / 16, 0);
    CHECK(had && mcc_chunk_section_at(c, 1) == NULL, "写入方块时分配区段，全部清回空气后释放%s", "");
    mcc_store_free(&st);
}

/* 区块在两个存储之间搬迁：交界处的外露判断要随邻居的有无更新 */
static void test_move(void) {
    mcc_info("== 区块搬迁（区域拆分/合并）");
    mcc_chunk_store a, b;
    mcc_store_init(&a, -16, 48);
    mcc_store_init(&b, -16, 48);
    test_fill(&a, gen, NULL, -3, 2, -3, 2, -16, 31);
    char err[256];
    /* 把 x >= 0 的一半搬到 b：交界两侧原本是内部的石头变成外露 */
    uint64_t keys[64];
    size_t n = 0;
    for (int32_t cx = 0; cx <= 2; cx++)
        for (int32_t cz = -3; cz <= 2; cz++) keys[n++] = mcc_key2(cx, cz);
    uint32_t before = 0;
    for (int32_t cz = -3; cz <= 2; cz++) before += mcc_store_chunk(&a, -1, cz)->sec[1]->solid_count;
    mcc_store_move(&a, &b, keys, n);
    bool ok1 = mcc_store_verify(&a, err, sizeof err);
    if (!ok1) mcc_error("拆开后 a：%s", err);
    bool ok2 = mcc_store_verify(&b, err, sizeof err);
    if (!ok2) mcc_error("拆开后 b：%s", err);
    uint32_t after = 0;
    for (int32_t cz = -3; cz <= 2; cz++) after += mcc_store_chunk(&a, -1, cz)->sec[1]->solid_count;
    CHECK(ok1 && ok2 && a.chunks.count == 18 && b.chunks.count == 18 && after > before,
          "切成两半后两边位图都正确，交界处外露方块 %u → %u", before, after);
    /* 全部搬回 a：交界处重新变成内部 */
    n = 0;
    uint32_t it = 0;
    uint64_t k;
    while (mcc_hmap_next(&b.chunks, &it, &k, NULL)) keys[n++] = k;
    mcc_store_move(&b, &a, keys, n);
    uint32_t back = 0;
    for (int32_t cz = -3; cz <= 2; cz++) back += mcc_store_chunk(&a, -1, cz)->sec[1]->solid_count;
    bool ok3 = mcc_store_verify(&a, err, sizeof err);
    if (!ok3) mcc_error("合回后：%s", err);
    CHECK(ok3 && b.chunks.count == 0 && back == before, "合回后位图正确，交界外露方块恢复为 %u", back);
    mcc_store_free(&a);
    mcc_store_free(&b);
}

int main(void) {
    test_load_blocks();
    test_palette();
    test_store();
    test_section_release();
    test_move();
    test_free_blocks();
    if (g_fail) { mcc_error("世界存储测试失败"); return 1; }
    mcc_info("世界存储测试全部通过");
    return 0;
}
