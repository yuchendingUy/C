/*
 * 世界层集成测试：真实多线程推进下的区域合并/拆分与物理迁移
 *
 *   1. 两块相距很远的地形 A、B，各放 20 个 3×3×3 结构：应是 2 个区域，结构各自落地静止
 *   2. 加载连接两地的区块走廊：合并成 1 个区域，40 个结构都在且位置不跳；
 *      在合并后的区域里挖坑，坑上的结构掉下去（地形格刚体随区块搬进了新场景）
 *   3. 卸载走廊：拆回 2 个区域，每边 20 个结构，存储位图正确
 *      （两地要隔得足够远，死区段超过 10% 才会拆分，与 Folia 规则一致）
 *   4. 反复合并/拆分多轮：结构不丢、不重复、不穿地
 */
#include "../src/world/level.h"
#include "../src/core/jobs.h"
#include "../src/core/ebr.h"
#include "testlib.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static bool g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { mcc_error(__VA_ARGS__); g_fail = true; } else { mcc_info("  通过：" __VA_ARGS__); } } while (0)

#define A_CX 0
#define B_CX 200
#define AREA 8          /* 每块地形 8×8 区块 */
#define CUBES 20

static mcc_level *g_lv;

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static mcc_state_t flat(int32_t x, int32_t y, int32_t z, void *ctx) {
    (void)x; (void)z; (void)ctx;
    return y < 61 ? STONE : 0;
}

static mcc_chunk *make_chunk(int32_t cx, int32_t cz) {
    mcc_chunk_store dims;
    mcc_store_init(&dims, TEST_MIN_Y, TEST_HEIGHT);
    mcc_chunk *c = mcc_chunk_new(&dims, cx, cz);
    mcc_store_free(&dims);
    for (int32_t y = 40; y < 61; y++)
        for (int lz = 0; lz < 16; lz++)
            for (int lx = 0; lx < 16; lx++) mcc_chunk_set_raw(c, lx, y, lz, flat(0, y, 0, NULL));
    return c;
}

/* 子世界内容：地块原点处 3×3×3 石头 */
static void fill_cube(void *arg, mcc_region_world *w, mcc_sublevel *sl) {
    (void)arg;
    int32_t bx = sl->plot_cx << 4, by = 64, bz = sl->plot_cz << 4;
    for (int32_t x = bx; x < bx + 3; x++)
        for (int32_t y = by; y < by + 3; y++)
            for (int32_t z = bz; z < bz + 3; z++) mcc_store_set(&sl->blocks, x, y, z, STONE);
    (void)w; /* 质心、包围盒、质量由世界层开启的质量追踪自动算出 */
}

/* 子世界存储里要有区块才能写方块：生成前先放好空区块 */
static void fill_cube_with_chunk(void *arg, mcc_region_world *w, mcc_sublevel *sl) {
    mcc_store_add_chunk(&sl->blocks, mcc_chunk_new(&sl->blocks, sl->plot_cx, sl->plot_cz));
    fill_cube(arg, w, sl);
}

/* ---- 探针：在拥有某区块的区域线程上读区域状态 ---- */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    bool done;
    uint64_t region;
    int sublevels;
    double min_y, max_y;
    int ids[256];
    double y[256];
    size_t chunks;
    bool verify_ok;
    bool has_phys;
} probe;

static void task_probe(mcc_region *r, int32_t cx, int32_t cz, void *arg) {
    (void)cx; (void)cz;
    probe *p = arg;
    pthread_mutex_lock(&p->m);
    if (r) {
        mcc_region_world *w = r->data;
        p->region = r->id;
        p->sublevels = 0;
        p->min_y = 1e9;
        p->max_y = -1e9;
        p->has_phys = w->phys != NULL;
        uint32_t it = 0;
        void *v;
        while (mcc_hmap_next(&w->sublevels, &it, NULL, &v)) {
            mcc_sublevel *sl = v;
            double pose[7] = { 0, NAN, 0, 0, 0, 0, 1 };
            if (w->phys && phys_body_get(w->phys, sl->id)) phys_body_get_pose(w->phys, sl->id, pose);
            if (p->sublevels < 256) { p->ids[p->sublevels] = sl->id; p->y[p->sublevels] = pose[1]; }
            p->sublevels++;
            if (!(pose[1] >= p->min_y)) p->min_y = pose[1];
            if (!(pose[1] <= p->max_y)) p->max_y = pose[1];
        }
        p->chunks = w->chunks->chunks.count;
        char err[256];
        p->verify_ok = getenv("NOVERIFY") ? true : mcc_store_verify(w->chunks, err, sizeof err);
        if (!p->verify_ok) mcc_error("区域 %llu 存储自检失败：%s", (unsigned long long)r->id, err);
    }
    p->done = true;
    pthread_cond_signal(&p->c);
    pthread_mutex_unlock(&p->m);
}

static bool probe_at(int32_t cx, int32_t cz, probe *p) {
    memset(p, 0, sizeof *p);
    pthread_mutex_init(&p->m, NULL);
    pthread_cond_init(&p->c, NULL);
    if (!mcc_regionizer_queue_chunk_task(&g_lv->rz, cx, cz, task_probe, p)) return false;
    pthread_mutex_lock(&p->m);
    while (!p->done) pthread_cond_wait(&p->c, &p->m);
    pthread_mutex_unlock(&p->m);
    return p->region != 0;
}

/* ---- 挖坑：在区域线程上改地形存储 ---- */
typedef struct { int32_t x0, z0, x1, z1, y0, y1; } dig_req;
static void task_dig(mcc_region *r, int32_t cx, int32_t cz, void *arg) {
    (void)cx; (void)cz;
    dig_req *d = arg;
    if (r) {
        mcc_region_world *w = r->data;
        for (int32_t x = d->x0; x <= d->x1; x++)
            for (int32_t z = d->z0; z <= d->z1; z++)
                for (int32_t y = d->y0; y <= d->y1; y++) mcc_store_set(w->chunks, x, y, z, 0);
    }
    free(d);
}

static size_t regions(void) {
    mcc_level_stats st;
    mcc_level_stats_get(g_lv, &st);
    return st.regions;
}

static bool wait_regions(size_t want, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 20) {
        if (regions() == want) return true;
        sleep_ms(20);
    }
    return regions() == want;
}

static void load_area(int32_t cx0) {
    for (int32_t cx = cx0; cx < cx0 + AREA; cx++)
        for (int32_t cz = 0; cz < AREA; cz++) mcc_level_load_chunk(g_lv, make_chunk(cx, cz));
}

static void corridor(bool load) {
    for (int32_t cx = A_CX + AREA; cx < B_CX; cx++) {
        if (load) mcc_level_load_chunk(g_lv, make_chunk(cx, 0));
        else mcc_level_unload_chunk(g_lv, cx, 0);
    }
}

static void spawn_cubes(int32_t cx0) {
    for (int i = 0; i < CUBES; i++) {
        double pose[7] = { (cx0 << 4) + 10.0 + (i % 5) * 20.0, 66.0, 10.0 + (i / 5) * 20.0, 0, 0, 0, 1 };
        if (mcc_level_spawn_sublevel(g_lv, pose, fill_cube_with_chunk, NULL) < 0) mcc_error("生成子世界失败");
    }
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* A、B 两个区域各有 CUBES 个结构、编号不重复、都在地面上 */
static bool check_split(const char *when, bool strict_rest) {
    probe pa, pb;
    bool ok = probe_at(A_CX, 0, &pa) && probe_at(B_CX, 0, &pb);
    if (!ok) { mcc_error("%s：探针失败", when); return false; }
    int all[512], n = 0;
    for (int i = 0; i < pa.sublevels && i < 256; i++) all[n++] = pa.ids[i];
    for (int i = 0; i < pb.sublevels && i < 256; i++) all[n++] = pb.ids[i];
    qsort(all, (size_t)n, sizeof(int), cmp_int);
    bool unique = true;
    for (int i = 1; i < n; i++) unique &= all[i] != all[i - 1];
    double lo = MCC_MIN(pa.min_y, pb.min_y), hi = MCC_MAX(pa.max_y, pb.max_y);
    bool rest = strict_rest ? (fabs(lo - 62.5) < 0.05 && fabs(hi - 62.5) < 0.05) : (lo > 56.0 && hi < 70.0);
    bool good = pa.region != pb.region && pa.sublevels == CUBES && pb.sublevels == CUBES && unique && rest && pa.verify_ok && pb.verify_ok;
    if (!good)
        mcc_error("%s：区域 %llu/%llu，子世界 %d/%d，编号不重复 %d，高度 %.3f~%.3f，自检 %d/%d", when, (unsigned long long)pa.region,
                  (unsigned long long)pb.region, pa.sublevels, pb.sublevels, unique, lo, hi, pa.verify_ok, pb.verify_ok);
    return good;
}

static bool check_merged(const char *when, probe *out) {
    probe pa, pb;
    if (!probe_at(A_CX, 0, &pa) || !probe_at(B_CX, 0, &pb)) return false;
    bool good = pa.region == pb.region && pa.sublevels == 2 * CUBES && pa.min_y > 56.0 && pa.max_y < 70.0 && pa.verify_ok;
    if (!good)
        mcc_error("%s：区域 %llu/%llu，子世界 %d，高度 %.3f~%.3f，自检 %d", when, (unsigned long long)pa.region,
                  (unsigned long long)pb.region, pa.sublevels, pa.min_y, pa.max_y, pa.verify_ok);
    if (out) *out = pa;
    return good;
}

int main(void) {
    mcc_jobs_start(2);
    test_load_blocks();
    mcc_level_config cfg = { 0 };
    cfg.min_y = TEST_MIN_Y;
    cfg.height = TEST_HEIGHT;
    cfg.tick_threads = 3;
    cfg.phys = (phys_scene_config){ { 0, -9.81, 0 }, 0.1 };
    g_lv = mcc_level_create(&cfg);
    mcc_level_start(g_lv);

    mcc_info("== 1. 两块分开的地形");
    load_area(A_CX);
    load_area(B_CX);
    CHECK(wait_regions(2, 2000), "两块地形相距 3200 格，是 %zu 个区域", regions());
    spawn_cubes(A_CX);
    spawn_cubes(B_CX);
    sleep_ms(3000);
    CHECK(check_split("分开时", true), "A、B 各 %d 个结构，各自落地静止在 62.5", CUBES);

    mcc_info("== 2. 加载走廊，两个区域合并");
    uint64_t t0 = mcc_nanotime();
    corridor(true);
    CHECK(wait_regions(1, 3000), "加载 %d 个走廊区块后合并成 %zu 个区域（%.0fms）", B_CX - A_CX - AREA, regions(),
          (mcc_nanotime() - t0) / 1e6);
    sleep_ms(500);
    probe pm;
    bool merged = check_merged("合并后", &pm);
    CHECK(merged && fabs(pm.min_y - 62.5) < 0.05 && fabs(pm.max_y - 62.5) < 0.05, "合并后 40 个结构都在同一区域，仍静止在 62.5（%.4f~%.4f）",
          pm.min_y, pm.max_y);

    /* 第 0 个结构在 A 的 (10, 10) 附近：挖坑，它应当掉下去 */
    dig_req *d = mcc_xmalloc(sizeof *d);
    *d = (dig_req){ 5, 5, 15, 15, 55, 60 };
    mcc_regionizer_queue_chunk_task(&g_lv->rz, 0, 0, task_dig, d);
    sleep_ms(2000);
    probe pd;
    probe_at(A_CX, 0, &pd);
    int fell = 0;
    for (int i = 0; i < pd.sublevels && i < 256; i++) fell += fabs(pd.y[i] - 56.5) < 0.1;
    CHECK(fell == 1, "合并后的区域里挖坑，坑上的结构落到坑底 56.5（%d 个）", fell);

    mcc_info("== 3. 卸载走廊，拆回两个区域");
    t0 = mcc_nanotime();
    corridor(false);
    CHECK(wait_regions(2, 5000), "卸载走廊后拆成 %zu 个区域（%.0fms）", regions(), (mcc_nanotime() - t0) / 1e6);
    sleep_ms(300);
    CHECK(check_split("拆分后", false), "拆分后每边 %d 个结构，编号不重复，都在地面上，存储自检通过", CUBES);

    mcc_info("== 4. 反复合并/拆分");
    bool ok = true;
    int rounds = 8;
    for (int r = 0; r < rounds && ok; r++) {
        corridor(true);
        ok &= wait_regions(1, 3000);
        sleep_ms(100 + (r % 3) * 150);
        ok &= check_merged("第 N 轮合并", NULL);
        corridor(false);
        ok &= wait_regions(2, 5000);
        sleep_ms(100 + (r % 2) * 200);
        ok &= check_split("第 N 轮拆分", false);
    }
    mcc_level_stats st;
    mcc_level_stats_get(g_lv, &st);
    CHECK(ok, "合并/拆分 %d 轮，结构不丢不重复不穿地；累计搬区块 %llu 次、迁移刚体 %llu 次", rounds,
          (unsigned long long)st.chunk_moves, (unsigned long long)st.body_moves);
    mcc_info("  推进 %llu 刻，最长一刻 %.2fms", (unsigned long long)st.tick.ticks, st.tick.max_tick_ns / 1e6);

    mcc_level_destroy(g_lv);
    test_free_blocks();
    mcc_ebr_drain();
    mcc_jobs_stop();
    if (g_fail) { mcc_error("世界层测试失败"); return 1; }
    mcc_info("世界层测试全部通过");
    return 0;
}
