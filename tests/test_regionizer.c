/*
 * 区域化 + 推进调度器 多线程压力测试
 *
 * 模拟玩家随机走动加载/卸载区块，驱动区域频繁合并、延迟合并、拆分、销毁。
 * 区块进出区域数据全部通过"区块任务"在所属区域线程上完成（Folia 的做法），
 * 每次推进都校验：
 *   - 同一区域不会被两个线程同时推进
 *   - 区域数据里的每个区块，此刻确实归属这个区域
 * 另有校验线程周期性运行全量一致性自检。
 */
#include "../src/region/regionizer.h"
#include "../src/region/tick_scheduler.h"
#include "../src/core/ebr.h"
#include "../src/core/jobs.h"
#include "../src/core/log.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    mcc_cmap chunks;           /* 区块键 → 引用计数 */
    _Atomic int ticking_now;
} rdata;

static mcc_regionizer g_rz;
static mcc_tick_scheduler *g_sched;
static _Atomic bool g_fail;
static _Atomic uint64_t g_task_runs, g_inserts, g_removes;

/* ---------- 每区块事件记录（定位并发时序错误） ---------- */
enum { EV_PUSH_INS = 1, EV_PUSH_REM, EV_MOVE_MERGE, EV_MOVE_SPLIT, EV_EXEC_INS, EV_EXEC_REM, EV_DATA_MERGE, EV_DATA_SPLIT };
static const char *EV_NAMES[] = { "", "投递插入", "投递卸载", "任务随合并搬家", "任务随拆分搬家", "执行插入", "执行卸载", "数据随合并搬家", "数据随拆分搬家" };
typedef struct { int ev; uint64_t a, b; uint64_t seq; } evrec;
typedef struct { evrec r[48]; int n; } evlog;
static pthread_mutex_t g_ev_lock = PTHREAD_MUTEX_INITIALIZER;
static mcc_cmap g_ev;
static uint64_t g_ev_seq;
static void task_insert(mcc_region *r, int32_t cx, int32_t cz, void *arg);
static void ev(uint64_t key, int e, uint64_t a, uint64_t b) {
    pthread_mutex_lock(&g_ev_lock);
    evlog *l = mcc_cmap_get(&g_ev, key);
    if (!l) { l = mcc_xcalloc(1, sizeof *l); mcc_cmap_put(&g_ev, key, l); }
    evrec *x = &l->r[l->n++ % 48];
    x->ev = e; x->a = a; x->b = b; x->seq = ++g_ev_seq;
    pthread_mutex_unlock(&g_ev_lock);
}
static void dump_ev(int32_t cx, int32_t cz) {
    pthread_mutex_lock(&g_ev_lock);
    evlog *l = mcc_cmap_get(&g_ev, mcc_key2(cx, cz));
    if (l) {
        int start = l->n > 48 ? l->n - 48 : 0;
        for (int i = start; i < l->n; i++) {
            evrec *x = &l->r[i % 48];
            fprintf(stderr, "  #%llu %s  %llu -> %llu\n", (unsigned long long)x->seq, EV_NAMES[x->ev],
                    (unsigned long long)x->a, (unsigned long long)x->b);
        }
    }
    pthread_mutex_unlock(&g_ev_lock);
}
static void rz_hook(int e, uint64_t from, uint64_t to, const mcc_chunk_task *t) {
    uint64_t k = mcc_key2(t->cx, t->cz);
    if (e == MCC_RZ_EV_PUSH) ev(k, t->fn == task_insert ? EV_PUSH_INS : EV_PUSH_REM, 0, to);
    else ev(k, e == MCC_RZ_EV_MOVE_MERGE ? EV_MOVE_MERGE : EV_MOVE_SPLIT, from, to);
}

#define CHECK(cond, ...) do { if (!(cond)) { mcc_error(__VA_ARGS__); atomic_store(&g_fail, true); } } while (0)
#define CHECKC(cond, cx, cz, ...) do { if (!(cond)) { mcc_error(__VA_ARGS__); dump_ev(cx, cz); atomic_store(&g_fail, true); } } while (0)

static size_t cnt_get(mcc_cmap *m, uint64_t k) { return (size_t)(uintptr_t)mcc_cmap_get(m, k); }
static void cnt_add(mcc_cmap *m, uint64_t k, long d) {
    long v = (long)cnt_get(m, k) + d;
    mcc_cmap_put(m, k, v > 0 ? (void *)(uintptr_t)v : NULL);
}

/* ---------- 区域数据回调 ---------- */

static void *cb_create(void *ctx, mcc_region *r) {
    (void)ctx; (void)r;
    rdata *d = mcc_xcalloc(1, sizeof *d);
    mcc_cmap_init(&d->chunks, 64);
    return d;
}

static void cb_destroy(void *ctx, mcc_region *r) {
    (void)ctx;
    rdata *d = r->data;
    CHECK(mcc_cmap_size(&d->chunks) == 0, "区域 %llu 销毁时仍有 %zu 个区块",
          (unsigned long long)r->id, mcc_cmap_size(&d->chunks));
    mcc_cmap_destroy(&d->chunks);
    free(d);
}

static void cb_active(void *ctx, mcc_region *r) {
    (void)ctx;
    mcc_tick_scheduler_on_active(g_sched, r);
}

static void cb_merge(void *ctx, mcc_region *from, mcc_region *into) {
    (void)ctx;
    rdata *f = from->data, *t = into->data;
    MCC_CMAP_FOREACH(&f->chunks, k, v, {
        ev(k, EV_DATA_MERGE, from->id, into->id);
        cnt_add(&t->chunks, k, (long)(uintptr_t)v);
        mcc_cmap_put(&f->chunks, k, NULL);
    });
}

static void cb_split(void *ctx, mcc_region *from, mcc_region **into, size_t n) {
    (void)ctx; (void)into; (void)n;
    rdata *f = from->data;
    MCC_CMAP_FOREACH(&f->chunks, k, v, {
        mcc_region *owner = mcc_regionizer_region_of_chunk_locked(&g_rz, mcc_key2_x(k), mcc_key2_z(k));
        if (owner && owner != from) {
            ev(k, EV_DATA_SPLIT, from->id, owner->id);
            cnt_add(&((rdata *)owner->data)->chunks, k, (long)(uintptr_t)v);
            mcc_cmap_put(&f->chunks, k, NULL);
        }
    });
}

/* ---------- 区块任务 ---------- */

static void task_insert(mcc_region *r, int32_t cx, int32_t cz, void *arg) {
    (void)arg;
    CHECK(r != NULL, "插入任务被取消：区块 (%d,%d) 仍被引用", cx, cz);
    if (!r) return;
    ev(mcc_key2(cx, cz), EV_EXEC_INS, 0, r->id);
    cnt_add(&((rdata *)r->data)->chunks, mcc_key2(cx, cz), 1);
    atomic_fetch_add(&g_task_runs, 1);
}

static void task_remove(mcc_region *r, int32_t cx, int32_t cz, void *arg) {
    (void)arg;
    CHECK(r != NULL, "卸载任务被取消：区块 (%d,%d)", cx, cz);
    if (!r) return;
    rdata *d = r->data;
    ev(mcc_key2(cx, cz), EV_EXEC_REM, 0, r->id);
    CHECKC(cnt_get(&d->chunks, mcc_key2(cx, cz)) > 0, cx, cz, "卸载时区域 %llu 中没有区块 (%d,%d)",
          (unsigned long long)r->id, cx, cz);
    cnt_add(&d->chunks, mcc_key2(cx, cz), -1);
    mcc_regionizer_remove_chunk(&g_rz, cx, cz);
    atomic_fetch_add(&g_task_runs, 1);
}

/* ---------- 推进 ---------- */

static void tick(void *ctx, mcc_region *r, uint64_t start) {
    (void)ctx; (void)start;
    rdata *d = r->data;
    CHECK(atomic_fetch_add(&d->ticking_now, 1) == 0, "区域 %llu 被两个线程同时推进", (unsigned long long)r->id);
    mcc_region_run_tasks(r);
    mcc_ebr_enter();
    MCC_CMAP_FOREACH(&d->chunks, k, v, {
        (void)v;
        mcc_region *owner = mcc_regionizer_region_of_chunk(&g_rz, mcc_key2_x(k), mcc_key2_z(k));
        CHECK(owner == r, "区域 %llu 持有不归它的区块 (%d,%d)，实际归属 %llu", (unsigned long long)r->id,
              mcc_key2_x(k), mcc_key2_z(k), owner ? (unsigned long long)owner->id : 0ULL);
    });
    mcc_ebr_exit();
    /* 模拟 0.2~1ms 的推进负载 */
    uint64_t until = mcc_nanotime() + 200000 + (mcc_hash64(r->id ^ r->tick_count) % 800000);
    while (mcc_nanotime() < until) {}
    atomic_fetch_sub(&d->ticking_now, 1);
}

/* ---------- 玩家 ---------- */

static pthread_mutex_t g_load_lock = PTHREAD_MUTEX_INITIALIZER;
static mcc_cmap g_refs; /* 区块 → 被多少玩家视野引用，受 g_load_lock 保护 */
static _Atomic bool g_stop_players;

#define VIEW 3

static void ref_chunk(int32_t cx, int32_t cz) {
    uint64_t k = mcc_key2(cx, cz);
    size_t c = cnt_get(&g_refs, k);
    cnt_add(&g_refs, k, 1);
    if (c == 0) {
        mcc_regionizer_add_chunk(&g_rz, cx, cz);
        bool ok = mcc_regionizer_queue_chunk_task(&g_rz, cx, cz, task_insert, NULL);
        CHECK(ok, "刚加载的区块 (%d,%d) 无法投递任务", cx, cz);
        atomic_fetch_add(&g_inserts, 1);
    }
}

static void unref_chunk(int32_t cx, int32_t cz) {
    uint64_t k = mcc_key2(cx, cz);
    cnt_add(&g_refs, k, -1);
    if (cnt_get(&g_refs, k) == 0) {
        bool ok = mcc_regionizer_queue_chunk_task(&g_rz, cx, cz, task_remove, NULL);
        CHECK(ok, "待卸载区块 (%d,%d) 无法投递任务", cx, cz);
        atomic_fetch_add(&g_removes, 1);
    }
}

static void view(int32_t px, int32_t pz, bool add) {
    for (int dx = -VIEW; dx <= VIEW; dx++)
        for (int dz = -VIEW; dz <= VIEW; dz++) {
            if (add) ref_chunk(px + dx, pz + dz);
            else unref_chunk(px + dx, pz + dz);
        }
}

static int g_spread = 200; /* 聚集区间距（区块），越大区域越多 */

static void *player_thread(void *p) {
    uint64_t seed = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ULL + 1;
    /* 玩家按编号分到 8×6 个聚集区，各聚集区相距 g_spread 区块 */
    int idx = (int)(uintptr_t)p;
    int32_t cxs = (idx % 8) * g_spread - 4 * g_spread;
    int32_t czs = ((idx / 8) % 6) * g_spread - 3 * g_spread;
    int32_t px = cxs, pz = czs;
    pthread_mutex_lock(&g_load_lock);
    view(px, pz, true);
    pthread_mutex_unlock(&g_load_lock);
    while (!atomic_load(&g_stop_players)) {
        seed = mcc_hash64(seed);
        int32_t nx = px, nz = pz;
        int step = (int)(seed % 8) + 1;
        switch ((seed >> 8) % 5) {
            case 0: nx += step; break;
            case 1: nx -= step; break;
            case 2: nz += step; break;
            case 3: nz -= step; break;
            default:
                /* 偶尔传送到另一个聚集区，制造远距离加载、整片卸载、区域销毁 */
                if ((seed >> 16) % 25 == 0) {
                    int k = (int)((seed >> 24) % 48);
                    cxs = (k % 8) * g_spread - 4 * g_spread;
                    czs = ((k / 8) % 6) * g_spread - 3 * g_spread;
                    nx = cxs; nz = czs;
                }
        }
        /* 在聚集区附近游荡：时而靠近别人（合并），时而走远（拆分） */
        if (nx < cxs - 60) nx = cxs - 60; if (nx > cxs + 60) nx = cxs + 60;
        if (nz < czs - 60) nz = czs - 60; if (nz > czs + 60) nz = czs + 60;
        pthread_mutex_lock(&g_load_lock);
        view(nx, nz, true);
        view(px, pz, false);
        pthread_mutex_unlock(&g_load_lock);
        px = nx; pz = nz;
        usleep(2000 + (useconds_t)((seed >> 32) % 3000));
    }
    pthread_mutex_lock(&g_load_lock);
    view(px, pz, false);
    pthread_mutex_unlock(&g_load_lock);
    return NULL;
}

/* ---------- 校验线程 ---------- */

static _Atomic bool g_stop_verify;
static _Atomic uint64_t g_verifies;

static void *verify_thread(void *p) {
    (void)p;
    char err[256];
    while (!atomic_load(&g_stop_verify)) {
        if (!mcc_regionizer_verify(&g_rz, err, sizeof err)) {
            mcc_error("一致性自检失败：%s", err);
            atomic_store(&g_fail, true);
        }
        atomic_fetch_add(&g_verifies, 1);
        usleep(20000);
    }
    return NULL;
}

/* ---------- 任务线程池正确性 ---------- */

static _Atomic uint64_t g_sum;
static void sum_range(void *arg, size_t b, size_t e) {
    (void)arg;
    uint64_t s = 0;
    for (size_t i = b; i < e; i++) s += i;
    atomic_fetch_add(&g_sum, s);
}

static bool test_jobs(void) {
    for (int round = 0; round < 50; round++) {
        atomic_store(&g_sum, 0);
        size_t n = 1000000 + (size_t)round * 7919;
        mcc_jobs_parallel_for(n, 4096, sum_range, NULL);
        uint64_t expect = (uint64_t)n * (n - 1) / 2;
        if (atomic_load(&g_sum) != expect) {
            mcc_error("并行循环结果错误：%llu != %llu", (unsigned long long)g_sum, (unsigned long long)expect);
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 8;
    int tick_threads = argc > 2 ? atoi(argv[2]) : 8;
    int players = argc > 3 ? atoi(argv[3]) : 48;
    if (argc > 4) g_spread = atoi(argv[4]);

    mcc_jobs_start(4);
    if (!test_jobs()) return 1;
    mcc_info("任务线程池并行循环校验通过");

    mcc_cmap_init(&g_refs, 4096);
    mcc_cmap_init(&g_ev, 65536);
    mcc_regionizer_debug_hook = rz_hook;
    mcc_region_callbacks cb = {
        .create_data = cb_create, .destroy_data = cb_destroy, .on_active = cb_active,
        .merge = cb_merge, .split = cb_split,
    };
    mcc_regionizer_init(&g_rz, NULL, &cb);
    mcc_tick_config tc = { .threads = tick_threads, .tick = tick };
    g_sched = mcc_tick_scheduler_create(&g_rz, &tc);
    mcc_tick_scheduler_start(g_sched);

    pthread_t vt, *pt = mcc_xmalloc(sizeof(pthread_t) * (size_t)players);
    pthread_create(&vt, NULL, verify_thread, NULL);
    for (int i = 0; i < players; i++) pthread_create(&pt[i], NULL, player_thread, (void *)(uintptr_t)(i + 1));

    for (int s = 0; s < seconds && !atomic_load(&g_fail); s++) {
        sleep(1);
        mcc_tick_stats st;
        mcc_tick_scheduler_stats(g_sched, &st);
        mcc_info("第 %d 秒：区域 %zu，推进 %llu 次，合并 %llu（延迟 %llu），拆分 %llu，销毁 %llu，任务 %llu，最长一刻 %.2fms",
                 s + 1, atomic_load(&g_rz.region_count), (unsigned long long)st.ticks,
                 (unsigned long long)g_rz.stat_merges, (unsigned long long)g_rz.stat_deferred_merges,
                 (unsigned long long)g_rz.stat_splits, (unsigned long long)g_rz.stat_kills,
                 (unsigned long long)g_task_runs, st.max_tick_ns / 1e6);
    }

    atomic_store(&g_stop_players, true);
    for (int i = 0; i < players; i++) pthread_join(pt[i], NULL);
    /* 等待卸载任务全部执行完、区域全部销毁 */
    for (int i = 0; i < 300 && atomic_load(&g_rz.region_count) > 0; i++) usleep(20000);
    atomic_store(&g_stop_verify, true);
    pthread_join(vt, NULL);

    char err[256];
    if (!mcc_regionizer_verify(&g_rz, err, sizeof err)) { mcc_error("最终自检失败：%s", err); g_fail = true; }
    size_t left = atomic_load(&g_rz.region_count);
    if (left != 0) { mcc_error("所有区块卸载后仍残留 %zu 个区域", left); g_fail = true; }
    if (atomic_load(&g_inserts) != atomic_load(&g_removes)) {
        mcc_error("加载 %llu 次，卸载 %llu 次，不相等", (unsigned long long)g_inserts, (unsigned long long)g_removes);
        g_fail = true;
    }

    mcc_tick_scheduler_stop(g_sched);
    mcc_tick_scheduler_destroy(g_sched);
    mcc_regionizer_destroy(&g_rz);
    mcc_ebr_drain();
    mcc_jobs_stop();
    free(pt);
    mcc_cmap_destroy(&g_refs);

    if (atomic_load(&g_fail)) { mcc_error("测试失败"); return 1; }
    mcc_info("测试通过：自检 %llu 次全部一致，加载/卸载 %llu 次配平", (unsigned long long)g_verifies,
             (unsigned long long)g_inserts);
    return 0;
}
