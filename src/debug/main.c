/*
 * mcc_debug：带简易测试客户端的调试服务端
 *
 * 生成一块带水池的平地，区域多线程推进，浏览器打开 http://<地址>:<端口>/ 即可看到。
 * 网页上可以召唤物理结构（随机形状，从空中落下）、清空。
 *
 * 用法：mcc_debug [端口=8080] [推进线程=2] [任务线程=核数-推进线程]
 * 数据文件找法：程序所在目录下的 data/ 与 viewer/（发布包），找不到再用源码目录
 */
#include "viewer.h"
#include "../core/jobs.h"
#include "../core/log.h"
#include "../phys/colliders.h"
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define AREA 6 /* 地形 (2×AREA)² 个区块 */

static char g_data[1024], g_web[1024];

/* 先找程序所在目录（发布包布局：mcc_debug、data/、viewer/），再找源码目录 */
static void find_dirs(void) {
    char exe[900];
#ifdef _WIN32
    long n = (long)GetModuleFileNameA(NULL, exe, sizeof exe - 1);
#else
    long n = (long)readlink("/proc/self/exe", exe, sizeof exe - 1);
#endif
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/'), *bs = strrchr(exe, '\\');
        if (bs > slash) slash = bs;
        if (slash) *slash = 0;
        char probe[1100];
        snprintf(probe, sizeof probe, "%s/data/blocks.txt", exe);
        FILE *f = fopen(probe, "r");
        if (f) {
            fclose(f);
            snprintf(g_data, sizeof g_data, "%s/data", exe);
            snprintf(g_web, sizeof g_web, "%s/viewer", exe);
            return;
        }
    }
    snprintf(g_data, sizeof g_data, "%s/data", MCC_SOURCE_DIR);
    snprintf(g_web, sizeof g_web, "%s/tools/viewer", MCC_SOURCE_DIR);
}

static volatile sig_atomic_t g_quit;
static void on_signal(int s) { (void)s; g_quit = 1; }

static mcc_level *g_lv;
static mcc_state_t STONE, DIRT, GRASS, WATER, SAND, PLANKS, LOG, GLASS, WOOL, IRON;

/* 平地：草皮 y=64；中间偏东一个 24×24、深 10 的水池 */
static mcc_state_t terrain_at(int32_t x, int32_t y, int32_t z) {
    bool pool = x >= 8 && x < 32 && z >= -12 && z < 12;
    if (pool) {
        if (y < 52) return STONE;
        if (y < 54) return SAND;
        if (y < 64) return WATER;
        return 0;
    }
    if (y < 60) return STONE;
    if (y < 64) return DIRT;
    if (y == 64) return GRASS;
    return 0;
}

static mcc_chunk *make_chunk(const mcc_chunk_store *dims, int32_t cx, int32_t cz) {
    mcc_chunk *c = mcc_chunk_new(dims, cx, cz);
    for (int32_t y = 40; y <= 64; y++)
        for (int lz = 0; lz < 16; lz++)
            for (int lx = 0; lx < 16; lx++) {
                mcc_state_t s = terrain_at((cx << 4) + lx, y, (cz << 4) + lz);
                if (s) mcc_chunk_set_raw(c, lx, y, lz, s);
            }
    return c;
}

/* ---- 召唤：随机形状的结构 ---- */

static _Thread_local uint64_t t_rng = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(uint32_t n) {
    t_rng ^= t_rng << 13; t_rng ^= t_rng >> 7; t_rng ^= t_rng << 17;
    return (uint32_t)(t_rng % n);
}

typedef struct { int shape; } fill_arg;

static void fill_shape(void *arg, mcc_region_world *w, mcc_sublevel *sl) {
    (void)w;
    int shape = ((fill_arg *)arg)->shape;
    free(arg);
    mcc_store_add_chunk(&sl->blocks, mcc_chunk_new(&sl->blocks, sl->plot_cx, sl->plot_cz));
    int32_t bx = sl->plot_cx << 4, by = 64, bz = sl->plot_cz << 4;
    mcc_state_t mats[] = { STONE, PLANKS, LOG, GLASS, WOOL, IRON };
    mcc_state_t m = mats[rnd(MCC_ARRAY_LEN(mats))];
    switch (shape) {
    case 0: /* 3×3×3 方块 */
        for (int x = 0; x < 3; x++) for (int y = 0; y < 3; y++) for (int z = 0; z < 3; z++) mcc_store_set(&sl->blocks, bx + x, by + y, bz + z, m);
        break;
    case 1: /* L 形：底板 3×1×3 + 角上柱子 */
        for (int x = 0; x < 3; x++) for (int z = 0; z < 3; z++) mcc_store_set(&sl->blocks, bx + x, by, bz + z, m);
        for (int y = 1; y < 4; y++) mcc_store_set(&sl->blocks, bx, by + y, bz, m);
        break;
    case 2: /* 长条 6×1×2 */
        for (int x = 0; x < 6; x++) for (int z = 0; z < 2; z++) mcc_store_set(&sl->blocks, bx + x, by, bz + z, m);
        break;
    case 3: /* 小船：5×1×3 底 + 一圈边 */
        for (int x = 0; x < 5; x++) for (int z = 0; z < 3; z++) mcc_store_set(&sl->blocks, bx + x, by, bz + z, PLANKS);
        for (int x = 0; x < 5; x++) { mcc_store_set(&sl->blocks, bx + x, by + 1, bz, PLANKS); mcc_store_set(&sl->blocks, bx + x, by + 1, bz + 2, PLANKS); }
        mcc_store_set(&sl->blocks, bx, by + 1, bz + 1, PLANKS);
        mcc_store_set(&sl->blocks, bx + 4, by + 1, bz + 1, PLANKS);
        break;
    default: /* 单个方块 */
        mcc_store_set(&sl->blocks, bx, by, bz, m);
    }
}

static void on_spawn(void *ctx, double x, double z, int count) {
    (void)ctx;
    int side = (int)ceil(sqrt((double)count));
    for (int i = 0; i < count; i++) {
        double px = x + (i % side - side / 2) * 5.0 + rnd(100) / 100.0;
        double pz = z + (i / side - side / 2) * 5.0 + rnd(100) / 100.0;
        double py = 75.0 + rnd(10) + (count > 1 ? (i % 3) * 4 : 0);
        /* 随机朝向，落地时会翻滚 */
        double a = rnd(628) / 100.0, b = rnd(628) / 100.0;
        double q[4] = { sin(a / 2) * cos(b), sin(a / 2) * sin(b), 0, cos(a / 2) };
        double pose[7] = { px, py, pz, q[0], q[1], q[2], q[3] };
        fill_arg *fa = mcc_xmalloc(sizeof *fa);
        fa->shape = (int)rnd(5);
        if (mcc_level_spawn_sublevel(g_lv, pose, fill_shape, fa) < 0) free(fa); /* 落点不在已加载区块 */
    }
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    int tick_threads = argc > 2 ? atoi(argv[2]) : 2;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    long cores = (long)si.dwNumberOfProcessors;
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    int job_threads = argc > 3 ? atoi(argv[3]) : (int)MCC_MAX(1, cores - tick_threads);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    find_dirs();
    char p1[1100], p2[1100], p3[1100];
    snprintf(p1, sizeof p1, "%s/blocks.txt", g_data);
    snprintf(p2, sizeof p2, "%s/shapes.txt", g_data);
    snprintf(p3, sizeof p3, "%s/phys_props.txt", g_data);
    if (!mcc_blocks_load(p1)) return 1;
    if (!phys_block_colliders_load(p2, p3)) return 1;
    phys_colliders_finalize();
    STONE = mcc_block_default_state("stone");
    DIRT = mcc_block_default_state("dirt");
    GRASS = mcc_block_default_state("grass_block");
    WATER = mcc_block_default_state("water");
    SAND = mcc_block_default_state("sand");
    PLANKS = mcc_block_default_state("oak_planks");
    LOG = mcc_block_default_state("oak_log");
    GLASS = mcc_block_default_state("glass");
    WOOL = mcc_block_default_state("white_wool");
    IRON = mcc_block_default_state("iron_block");

    mcc_jobs_start(job_threads);
    mcc_level_config cfg = { 0 };
    cfg.min_y = -64;
    cfg.height = 384;
    cfg.tick_threads = tick_threads;
    /* 8.28 倍地球重力（3 × 2.76），调试时下落更利落 */
    cfg.phys = (phys_scene_config){ { 0, -9.81 * 3 * 2.76, 0 }, 0.1 };
    cfg.post_tick_hook = mcc_viewer_post_tick;
    g_lv = mcc_level_create(&cfg);
    mcc_viewer *v = mcc_viewer_start(g_lv, port, g_web, on_spawn, NULL);
    g_lv->cfg.hook_ctx = v;
    mcc_level_start(g_lv);

    mcc_chunk_store dims;
    mcc_store_init(&dims, cfg.min_y, cfg.height);
    for (int32_t cx = -AREA; cx < AREA; cx++)
        for (int32_t cz = -AREA; cz < AREA; cz++) {
            mcc_level_load_chunk(g_lv, make_chunk(&dims, cx, cz));
            mcc_viewer_add_chunk(v, cx, cz);
        }
    mcc_store_free(&dims);
    mcc_info("地形 %d×%d 区块已加载，Ctrl+C 退出", AREA * 2, AREA * 2);

    while (!g_quit) {
        struct timespec ts = { 0, 200 * 1000000L };
        nanosleep(&ts, NULL);
    }
    mcc_info("正在关闭");
    mcc_level_stop(g_lv);
    mcc_viewer_stop(v);
    mcc_level_destroy(g_lv);
    mcc_jobs_stop();
    return 0;
}
