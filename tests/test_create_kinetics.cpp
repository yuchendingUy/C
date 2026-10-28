/*
 * 机械动力动力核心测试：转速传播、拆除后的重新传播、各种传动比、冲突时破坏方块、应力网络
 * 期望值都按机械动力源码（RotationPropagator / KineticNetwork）手算
 */
#include "../src/game/space.h"
#include "../src/create/create.h"
#include "../src/create/kinetics/blocks.h"
#include "../src/create/stress.h"
#include "../src/game/game.h"
#include "../src/core/jobs.h"
#include "testlib.h"
#include <math.h>
#include <atomic>
#include <time.h>

using namespace mcc;
using namespace mcc::create;

static bool g_fail;
#define CHECK(cond, ...) do { if (cond) mcc_info("  通过：" __VA_ARGS__); else { mcc_error("  失败：" __VA_ARGS__); g_fail = true; } } while (0)

static mcc_state_t st(const char *block, const char *p1 = nullptr, const char *v1 = nullptr, const char *p2 = nullptr,
                      const char *v2 = nullptr) {
    mcc_state_t s = mcc_block_default_state(block);
    if (p1) s = mcc_state_with_str(s, p1, v1);
    if (p2) s = mcc_state_with_str(s, p2, v2);
    return s;
}

static mcc_state_t flat(int32_t, int32_t y, int32_t, void *) { return y < 60 ? STONE : 0; }

static KineticBlockEntity *kbe(Space &sp, BlockPos p) { return as_kinetic(sp.block_entity(p)); }
static float speed_at(Space &sp, BlockPos p) {
    KineticBlockEntity *k = kbe(sp, p);
    return k ? k->get_speed() : NAN;
}
static void run(Space &sp, int ticks) { for (int i = 0; i < ticks; i++) sp.tick(); }

/* 新建一块平地空间 */
struct World {
    mcc_chunk_store store;
    Space *sp;
    World() {
        mcc_store_init(&store, TEST_MIN_Y, TEST_HEIGHT);
        test_fill(&store, flat, nullptr, -2, 2, -2, 2, 50, 70);
        sp = &space_of(&store, 0);
    }
    ~World() { mcc_store_free(&store); }
};

static void test_shaft_line() {
    mcc_info("创造马达 + 一排传动杆");
    World w;
    Space &sp = *w.sp;
    BlockPos motor{ 0, 64, 0 };
    sp.set_block(motor, st("create:creative_motor", "facing", "east"));
    for (int x = 1; x <= 10; x++) sp.set_block({ x, 64, 0 }, st("create:shaft", "axis", "x"));
    run(sp, 3);
    bool all = true;
    for (int x = 1; x <= 10; x++) all &= speed_at(sp, { x, 64, 0 }) == 16;
    CHECK((speed_at(sp, motor) == 16 && all), "马达朝东 16 RPM，10 根传动杆全部 16 RPM");
    KineticBlockEntity *last = kbe(sp, { 10, 64, 0 });
    CHECK((last->has_network && last->network == motor.as_long()), "网络编号是马达位置（BlockPos.asLong）");
    KineticNetwork *n = network_get(sp, motor.as_long());
    CHECK((n && n->members.size() == 11 && n->sources.size() == 1), "网络 11 个成员、1 个动力源（实际 %zu / %zu）",
          n ? n->members.size() : 0, n ? n->sources.size() : 0);
    CHECK((n && n->current_capacity == 16384.f * 16), "网络容量 = 16384 × 16 RPM = %.0f", n ? n->current_capacity : 0);
    CHECK((kbe(sp, { 5, 64, 0 })->has_source && kbe(sp, { 5, 64, 0 })->source == BlockPos{ 4, 64, 0 }), "第 5 根的动力来自第 4 根");

    sp.set_block({ 5, 64, 0 }, 0);
    run(sp, 2);
    bool before = true, after = true;
    for (int x = 1; x <= 4; x++) before &= speed_at(sp, { x, 64, 0 }) == 16;
    for (int x = 6; x <= 10; x++) after &= speed_at(sp, { x, 64, 0 }) == 0 && !kbe(sp, { x, 64, 0 })->has_network;
    CHECK((before && after), "拆掉第 5 根：前 4 根仍转，后 5 根停转并离开网络");
    CHECK((n->members.size() == 5), "网络剩 5 个成员（实际 %zu）", n->members.size());

    sp.set_block({ 5, 64, 0 }, st("create:shaft", "axis", "x"));
    run(sp, 2);
    after = true;
    for (int x = 5; x <= 10; x++) after &= speed_at(sp, { x, 64, 0 }) == 16;
    CHECK((after && n->members.size() == 11), "放回去：后半段重新转起来，网络恢复 11 个成员");

    /* 马达调到 -32：整条线反转、转速翻倍 */
    ((CreativeMotorBlockEntity *)sp.block_entity(motor))->set_generated_speed(-32);
    run(sp, 2);
    all = true;
    for (int x = 1; x <= 10; x++) all &= speed_at(sp, { x, 64, 0 }) == -32;
    CHECK((all), "马达调到 -32：全线 -32 RPM");
}

static void test_cogs() {
    mcc_info("齿轮传动");
    World w;
    Space &sp = *w.sp;
    /* 马达朝上带一个竖轴小齿轮 A；A 东边的小齿轮 B 反向；A 斜对角的大齿轮 L 半速反向 */
    sp.set_block({ 0, 63, 0 }, st("create:creative_motor", "facing", "up"));
    BlockPos a{ 0, 64, 0 }, b{ 1, 64, 0 }, l{ -1, 64, 1 }, s2{ -2, 64, 2 };
    sp.set_block(a, st("create:cogwheel", "axis", "y"));
    sp.set_block(b, st("create:cogwheel", "axis", "y"));
    sp.set_block(l, st("create:large_cogwheel", "axis", "y"));
    sp.set_block(s2, st("create:cogwheel", "axis", "y"));
    run(sp, 3);
    CHECK((speed_at(sp, a) == 16), "小齿轮 A 16 RPM（实际 %g）", speed_at(sp, a));
    CHECK((speed_at(sp, b) == -16), "相邻小齿轮 B 反向 -16 RPM（实际 %g）", speed_at(sp, b));
    CHECK((speed_at(sp, l) == -8), "斜对角大齿轮 -8 RPM（小带大 ×-0.5，实际 %g）", speed_at(sp, l));
    CHECK((speed_at(sp, s2) == 16), "大齿轮再带斜对角小齿轮 ×-2 → 16 RPM（实际 %g）", speed_at(sp, s2));
}

static void test_gearbox() {
    mcc_info("齿轮箱、离合器、换向器");
    World w;
    Space &sp = *w.sp;
    sp.set_block({ 0, 64, 0 }, st("create:creative_motor", "facing", "east"));
    sp.set_block({ 1, 64, 0 }, st("create:shaft", "axis", "x"));
    sp.set_block({ 2, 64, 0 }, st("create:gearbox", "axis", "y"));
    sp.set_block({ 3, 64, 0 }, st("create:shaft", "axis", "x"));
    sp.set_block({ 2, 64, 1 }, st("create:shaft", "axis", "z"));
    sp.set_block({ 2, 64, -1 }, st("create:shaft", "axis", "z"));
    run(sp, 3);
    /* 齿轮箱：源头在西。东边同轴反向 → -16；南（正方向，与西方向相反）→ 1；北（负方向，与西相同）→ -1 */
    CHECK((speed_at(sp, { 3, 64, 0 }) == -16), "齿轮箱东出口 -16（实际 %g）", speed_at(sp, { 3, 64, 0 }));
    CHECK((speed_at(sp, { 2, 64, 1 }) == 16), "齿轮箱南出口 16（实际 %g）", speed_at(sp, { 2, 64, 1 }));
    CHECK((speed_at(sp, { 2, 64, -1 }) == -16), "齿轮箱北出口 -16（实际 %g）", speed_at(sp, { 2, 64, -1 }));

    /* 没有红石时离合器、换向器不起作用，就是一根轴 */
    sp.set_block({ 3, 64, 0 }, st("create:clutch", "axis", "x"));
    sp.set_block({ 4, 64, 0 }, st("create:gearshift", "axis", "x"));
    sp.set_block({ 5, 64, 0 }, st("create:shaft", "axis", "x"));
    run(sp, 3);
    CHECK((speed_at(sp, { 3, 64, 0 }) == -16 && speed_at(sp, { 4, 64, 0 }) == -16 && speed_at(sp, { 5, 64, 0 }) == -16),
          "未充能的离合器、换向器直通（%g %g %g）", speed_at(sp, { 3, 64, 0 }), speed_at(sp, { 4, 64, 0 }), speed_at(sp, { 5, 64, 0 }));
}

static void test_conflict() {
    mcc_info("两个动力源的传动线接到一起");
    World w;
    Space &sp = *w.sp;
    sp.set_block({ 0, 64, 0 }, st("create:creative_motor", "facing", "east"));
    for (int x = 1; x <= 5; x++) sp.set_block({ x, 64, 0 }, st("create:shaft", "axis", "x"));
    /* 另一台朝西的马达单独转：发电转速 -16（方向与左边那条线相反） */
    sp.set_block({ 7, 64, 0 }, st("create:creative_motor", "facing", "west"));
    run(sp, 3);
    CHECK((speed_at(sp, { 7, 64, 0 }) == -16), "朝西的马达单独转 -16（实际 %g）", speed_at(sp, { 7, 64, 0 }));
    /* 用一根传动杆接通：两边方向相反 → 原版打掉正在传播的这根 */
    sp.set_block({ 6, 64, 0 }, st("create:shaft", "axis", "x"));
    run(sp, 3);
    CHECK((mcc_state_is_air(sp.get({ 6, 64, 0 }))), "接通两条反向的线：中间那根传动杆被破坏");
    bool all = true;
    for (int x = 1; x <= 5; x++) all &= speed_at(sp, { x, 64, 0 }) == 16;
    CHECK((all && speed_at(sp, { 7, 64, 0 }) == -16), "两边都不受影响");

    /* 右边马达改成同方向、更快（-64 朝西 = +64）：接通后它接管整条线 */
    ((CreativeMotorBlockEntity *)sp.block_entity({ 7, 64, 0 }))->set_generated_speed(-64);
    run(sp, 2);
    CHECK((speed_at(sp, { 7, 64, 0 }) == 64), "右边马达调到 +64（实际 %g）", speed_at(sp, { 7, 64, 0 }));
    sp.set_block({ 6, 64, 0 }, st("create:shaft", "axis", "x"));
    run(sp, 3);
    all = true;
    for (int x = 1; x <= 6; x++) all &= speed_at(sp, { x, 64, 0 }) == 64;
    KineticBlockEntity *m0 = kbe(sp, { 0, 64, 0 });
    CHECK((all), "接通同向更快的线：左边整条线变成 64 RPM（第 3 根 %g）", speed_at(sp, { 3, 64, 0 }));
    CHECK((m0 && m0->has_source && m0->source == BlockPos{ 1, 64, 0 } && m0->get_speed() == 64),
          "左边马达被带着转：64 RPM，动力来自旁边的传动杆");
    KineticNetwork *n = network_get(sp, BlockPos{ 7, 64, 0 }.as_long());
    CHECK((n && n->members.size() == 8), "全部 8 个部件在右边马达的网络里（实际 %zu）", n ? n->members.size() : 0);
}

static void test_long_line() {
    mcc_info("长传动线（原版递归传播在这里容易爆栈）");
    World w;
    Space &sp = *w.sp;
    sp.set_block({ -31, 64, 0 }, st("create:creative_motor", "facing", "east"));
    /* 先铺好传动杆，最后再放马达所在那一格的下一根，让一次传播走完整条线 */
    for (int x = -29; x <= 47; x++) sp.set_block({ x, 64, 0 }, st("create:shaft", "axis", "x"));
    run(sp, 2);
    sp.set_block({ -30, 64, 0 }, st("create:shaft", "axis", "x"));
    run(sp, 2);
    bool all = true;
    for (int x = -30; x <= 47; x++) all &= speed_at(sp, { x, 64, 0 }) == 16;
    CHECK((all), "78 根传动杆一次传通，全部 16 RPM");
}

/* ---------------- 多线程：区域合并、拆分时动力网络跟着搬 ---------------- */

static mcc_level *g_lv;
static void tick_hook(void *ctx, mcc_region *r, mcc_region_world *w) { (void)ctx; (void)r; game_tick_region(w); }

static mcc_chunk *flat_chunk(int32_t cx, int32_t cz) {
    static mcc_chunk_store dims;
    static bool inited;
    if (!inited) { mcc_store_init(&dims, TEST_MIN_Y, TEST_HEIGHT); inited = true; }
    mcc_chunk *c = mcc_chunk_new(&dims, cx, cz);
    for (int32_t y = 50; y < 60; y++)
        for (int lz = 0; lz < 16; lz++)
            for (int lx = 0; lx < 16; lx++) mcc_chunk_set_raw(c, lx, y, lz, STONE);
    return c;
}

struct Job {
    int32_t x0;
    int op;               /* 0 放马达 + 10 根传动杆；1 检查；2 拆掉第 5 根 */
    std::atomic<int> done{ 0 };
    float speeds[11];
    size_t members;
};

static void job_task(mcc_region *r, int32_t, int32_t, void *arg) {
    Job *j = (Job *)arg;
    if (!r) { j->done = 2; return; }
    Space &sp = region_space((mcc_region_world *)r->data);
    if (j->op == 0) {
        sp.set_block({ j->x0, 64, 8 }, st("create:creative_motor", "facing", "east"));
        for (int x = 1; x <= 10; x++) sp.set_block({ j->x0 + x, 64, 8 }, st("create:shaft", "axis", "x"));
    } else if (j->op == 1) {
        for (int x = 0; x <= 10; x++) j->speeds[x] = speed_at(sp, { j->x0 + x, 64, 8 });
        KineticNetwork *n = network_get(sp, BlockPos{ j->x0, 64, 8 }.as_long());
        j->members = n ? n->members.size() : 0;
    } else {
        sp.set_block({ j->x0 + 5, 64, 8 }, 0);
    }
    j->done = 1;
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void job(int32_t x0, int op, Job *out) {
    out->x0 = x0;
    out->op = op;
    out->done = 0;
    if (!mcc_regionizer_queue_chunk_task(&g_lv->rz, x0 >> 4, 0, job_task, out)) mcc_log_fatal("投递失败");
    while (!out->done) sleep_ms(5);
}

static bool line_ok(Job &j, int from, int to, float v) {
    for (int x = from; x <= to; x++)
        if (j.speeds[x] != v) return false;
    return true;
}

static void test_regions() {
    mcc_info("多线程：两个区域各有一套动力，合并后、拆分后都正常");
    mcc_level_config cfg{};
    cfg.min_y = TEST_MIN_Y;
    cfg.height = TEST_HEIGHT;
    cfg.tick_threads = 2;
    cfg.phys = phys_scene_config{ { 0, -9.81, 0 }, 0.1 };
    cfg.tick_hook = tick_hook;
    g_lv = mcc_level_create(&cfg);
    mcc_level_start(g_lv);
    const int32_t A = 0, B = 200; /* 区块 x：两处相距 3200 格 */
    for (int32_t cz = 0; cz <= 1; cz++)
        for (int32_t cx : { A, A + 1, B, B + 1 }) mcc_level_load_chunk(g_lv, flat_chunk(cx, cz));
    sleep_ms(200);
    mcc_level_stats ls;
    mcc_level_stats_get(g_lv, &ls);
    CHECK((ls.regions == 2), "两处各自一个区域（%zu 个）", ls.regions);

    Job ja, jb;
    job(A * 16, 0, &ja);
    job(B * 16, 0, &jb);
    sleep_ms(300);
    job(A * 16, 1, &ja);
    job(B * 16, 1, &jb);
    CHECK((line_ok(ja, 0, 10, 16) && ja.members == 11 && line_ok(jb, 0, 10, 16) && jb.members == 11),
          "两个区域各自转起来：16 RPM、网络 11 个成员（%zu / %zu）", ja.members, jb.members);

    /* 中间铺一条区块，两个区域合并 */
    for (int32_t cx = A + 2; cx < B; cx++) mcc_level_load_chunk(g_lv, flat_chunk(cx, 0));
    sleep_ms(500);
    mcc_level_stats_get(g_lv, &ls);
    job(A * 16, 1, &ja);
    job(B * 16, 1, &jb);
    CHECK((ls.regions == 1), "铺通后合并成一个区域（%zu 个）", ls.regions);
    CHECK((line_ok(ja, 0, 10, 16) && ja.members == 11 && line_ok(jb, 0, 10, 16) && jb.members == 11),
          "合并后两套动力照常：16 RPM、网络 11 个成员（%zu / %zu）", ja.members, jb.members);

    /* 合并后网络照常工作：拆掉 A 的第 5 根 */
    Job jx;
    job(A * 16, 2, &jx);
    sleep_ms(200);
    job(A * 16, 1, &ja);
    CHECK((line_ok(ja, 0, 4, 16) && line_ok(ja, 6, 10, 0) && ja.members == 5),
          "合并后拆掉 A 的第 5 根：前半段转、后半段停，网络剩 5 个成员（%zu）", ja.members);

    /* 拆掉中间那条，重新分成两个区域 */
    for (int32_t cx = A + 2; cx < B; cx++) mcc_level_unload_chunk(g_lv, cx, 0);
    sleep_ms(800);
    mcc_level_stats_get(g_lv, &ls);
    job(A * 16, 1, &ja);
    job(B * 16, 1, &jb);
    CHECK((ls.regions == 2), "拆掉中间后又分成两个区域（%zu 个）", ls.regions);
    CHECK((line_ok(jb, 0, 10, 16) && jb.members == 11 && line_ok(ja, 0, 4, 16) && ja.members == 5),
          "拆分后两边状态保持（A %zu 个成员，B %zu 个）", ja.members, jb.members);

    mcc_level_stop(g_lv);
    mcc_level_destroy(g_lv);
}

int main() {
    test_load_blocks_with_create();
    game_init();
    if (!create_init(MCC_SOURCE_DIR "/data")) mcc_log_fatal("机械动力初始化失败");
    test_shaft_line();
    test_cogs();
    test_gearbox();
    test_conflict();
    test_long_line();
    mcc_jobs_start(2);
    test_regions();
    mcc_jobs_stop();
    block_registry_free();
    test_free_blocks();
    if (g_fail) {
        mcc_error("机械动力动力测试有失败项");
        return 1;
    }
    mcc_info("机械动力动力测试全部通过");
    return 0;
}
