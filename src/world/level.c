#include "level.h"
#include "../core/log.h"
#include <math.h>

/* 地块：每个子世界 64×64 区块（1024×1024 方块），从 x、z = 2000 万格起排布（与 Sable 地块区一致） */
#define PLOT_BASE_CHUNK 1250000
#define PLOT_CHUNKS 64
#define PLOT_ROW 1024

/* ------------------------------------------------------------------ */
/* 区域数据                                                            */
/* ------------------------------------------------------------------ */

static mcc_region_world *rw_new(mcc_level *lv) {
    mcc_region_world *w = mcc_xcalloc(1, sizeof *w);
    w->level = lv;
    w->chunks = mcc_xmalloc(sizeof *w->chunks);
    mcc_store_init(w->chunks, lv->cfg.min_y, lv->cfg.height);
    mcc_hmap_init(&w->sublevels, 8);
    return w;
}

static void sublevel_free(mcc_sublevel *sl) {
    mcc_store_free(&sl->blocks);
    free(sl);
}

static phys_scene *ensure_phys(mcc_region_world *w) {
    if (!w->phys) w->phys = phys_scene_create(&w->level->cfg.phys, w->chunks);
    return w->phys;
}

/* 区域里没有子世界了：物理场景连同地形格刚体一起销毁，不再占用任何物理开销 */
static void maybe_drop_phys(mcc_region_world *w) {
    if (w->phys && w->sublevels.count == 0) {
        phys_scene_destroy(w->phys);
        w->phys = NULL;
    }
}

static void *cb_create(void *ctx, mcc_region *r) {
    (void)r;
    return rw_new(ctx);
}

static void cb_destroy(void *ctx, mcc_region *r) {
    mcc_level *lv = ctx;
    mcc_region_world *w = r->data;
    if (!w) return;
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&w->sublevels, &it, NULL, &v)) {
        mcc_sublevel *sl = v;
        if (w->phys) phys_sublevel_remove(w->phys, sl->id);
        if (lv->cfg.sublevel_orphan) lv->cfg.sublevel_orphan(lv->cfg.hook_ctx, sl);
        else sublevel_free(sl);
    }
    mcc_hmap_free(&w->sublevels);
    if (w->phys) phys_scene_destroy(w->phys);
    mcc_store_free(w->chunks);
    free(w->chunks);
    free(w);
}

static void cb_active(void *ctx, mcc_region *r) {
    mcc_level *lv = ctx;
    mcc_tick_scheduler_on_active(lv->sched, r);
}

static void move_sublevel(mcc_level *lv, mcc_region_world *from, mcc_region_world *to, mcc_sublevel *sl) {
    mcc_hmap_remove(&from->sublevels, (uint64_t)(uint32_t)sl->id);
    mcc_hmap_put(&to->sublevels, (uint64_t)(uint32_t)sl->id, sl);
    if (from->phys && phys_body_get(from->phys, sl->id)) {
        phys_body_migrate(from->phys, ensure_phys(to), sl->id);
        atomic_fetch_add_explicit(&lv->stat_body_moves, 1, memory_order_relaxed);
    }
}

/* 取出区域里全部子世界（遍历时不能改表） */
static size_t list_sublevels(mcc_region_world *w, mcc_sublevel ***out) {
    mcc_sublevel **v = mcc_xmalloc(sizeof *v * (w->sublevels.count + 1));
    size_t n = 0;
    uint32_t it = 0;
    void *p;
    while (mcc_hmap_next(&w->sublevels, &it, NULL, &p)) v[n++] = p;
    *out = v;
    return n;
}

static void move_all_chunks(mcc_level *lv, mcc_chunk_store *from, mcc_chunk_store *to) {
    size_t n = from->chunks.count;
    if (!n) return;
    uint64_t *keys = mcc_xmalloc(sizeof *keys * n);
    size_t k = 0;
    uint32_t it = 0;
    uint64_t key;
    while (mcc_hmap_next(&from->chunks, &it, &key, NULL)) keys[k++] = key;
    mcc_store_move(from, to, keys, k);
    atomic_fetch_add_explicit(&lv->stat_chunk_moves, k, memory_order_relaxed);
    free(keys);
}

/* 合并：from 的一切并入 into。先交换使搬运量最小（区块指针、刚体重建都按数量计） */
static void cb_merge(void *ctx, mcc_region *from, mcc_region *into) {
    mcc_level *lv = ctx;
    mcc_region_world *f = from->data, *t = into->data;
    size_t fs = f->chunks->chunks.count + f->sublevels.count * 64, ts = t->chunks->chunks.count + t->sublevels.count * 64;
    if (fs > ts) {
        /* 存储和场景成对交换：场景读的就是同一对里的存储 */
        mcc_chunk_store *cs = f->chunks; f->chunks = t->chunks; t->chunks = cs;
        phys_scene *ps = f->phys; f->phys = t->phys; t->phys = ps;
        mcc_hmap sv = f->sublevels; f->sublevels = t->sublevels; t->sublevels = sv;
    }
    move_all_chunks(lv, f->chunks, t->chunks);
    mcc_sublevel **sls;
    size_t n = list_sublevels(f, &sls);
    for (size_t i = 0; i < n; i++) move_sublevel(lv, f, t, sls[i]);
    free(sls);
    maybe_drop_phys(f);
    if (f->ticks > t->ticks) t->ticks = f->ticks;
}

/* 拆分：区块按所在区段的新归属分配；子世界按刚体当前位置所在区块的归属分配 */
static void cb_split(void *ctx, mcc_region *from, mcc_region **into, size_t n) {
    mcc_level *lv = ctx;
    mcc_region_world *f = from->data;
    MCC_VEC(uint64_t) *lists = mcc_xcalloc(n, sizeof *lists);
    uint32_t it = 0;
    uint64_t key;
    while (mcc_hmap_next(&f->chunks->chunks, &it, &key, NULL)) {
        mcc_region *dst = mcc_regionizer_region_of_chunk_locked(&lv->rz, mcc_key2_x(key), mcc_key2_z(key));
        if (!dst || dst == from) continue;
        for (size_t i = 0; i < n; i++)
            if (into[i] == dst) { mcc_vec_push(&lists[i], key); break; }
    }
    for (size_t i = 0; i < n; i++) {
        mcc_region_world *t = into[i]->data;
        mcc_store_move(f->chunks, t->chunks, lists[i].data, lists[i].len);
        atomic_fetch_add_explicit(&lv->stat_chunk_moves, lists[i].len, memory_order_relaxed);
        mcc_vec_free(&lists[i]);
    }
    free(lists);

    mcc_sublevel **sls;
    size_t m = list_sublevels(f, &sls);
    for (size_t i = 0; i < m; i++) {
        if (!f->phys || !phys_body_get(f->phys, sls[i]->id)) continue;
        double pose[7];
        phys_body_get_pose(f->phys, sls[i]->id, pose);
        int32_t cx = (int32_t)floor(pose[0]) >> 4, cz = (int32_t)floor(pose[2]) >> 4;
        mcc_region *dst = mcc_regionizer_region_of_chunk_locked(&lv->rz, cx, cz);
        if (!dst || dst == from) continue;
        for (size_t k = 0; k < n; k++)
            if (into[k] == dst) { move_sublevel(lv, f, dst->data, sls[i]); break; }
    }
    free(sls);
    maybe_drop_phys(f);
    for (size_t i = 0; i < n; i++) ((mcc_region_world *)into[i]->data)->ticks = f->ticks;
}

/* ------------------------------------------------------------------ */
/* 推进                                                                */
/* ------------------------------------------------------------------ */

static void level_tick(void *ctx, mcc_region *r, uint64_t start) {
    (void)start;
    mcc_level *lv = ctx;
    mcc_region_world *w = r->data;
    mcc_region_run_tasks(r);
    if (lv->cfg.tick_hook) lv->cfg.tick_hook(lv->cfg.hook_ctx, r, w);
    if (w->phys) {
        phys_scene_tick(w->phys);
        double dt = 0.05 / lv->cfg.phys_substeps;
        for (int k = 0; k < lv->cfg.phys_substeps; k++) phys_scene_step(w->phys, dt);
    }
    w->ticks++;
}

/* ------------------------------------------------------------------ */
/* 维度                                                                */
/* ------------------------------------------------------------------ */

mcc_level *mcc_level_create(const mcc_level_config *cfg) {
    mcc_level *lv = mcc_xcalloc(1, sizeof *lv);
    lv->cfg = *cfg;
    if (lv->cfg.phys_substeps < 1) lv->cfg.phys_substeps = 2;
    if (lv->cfg.tick_threads < 1) lv->cfg.tick_threads = 1;
    atomic_init(&lv->next_sublevel_id, 1);
    mcc_region_callbacks cb = { lv, cb_create, cb_destroy, cb_active, cb_merge, cb_split };
    mcc_regionizer_init(&lv->rz, &lv->cfg.region, &cb);
    mcc_tick_config tc = { .threads = lv->cfg.tick_threads, .tick = level_tick, .ctx = lv };
    lv->sched = mcc_tick_scheduler_create(&lv->rz, &tc);
    return lv;
}

void mcc_level_start(mcc_level *lv) { mcc_tick_scheduler_start(lv->sched); }
void mcc_level_stop(mcc_level *lv) { mcc_tick_scheduler_stop(lv->sched); }

void mcc_level_destroy(mcc_level *lv) {
    mcc_tick_scheduler_stop(lv->sched);
    mcc_regionizer_destroy(&lv->rz);
    mcc_tick_scheduler_destroy(lv->sched);
    free(lv);
}

/* ---- 区块 ---- */

static void task_load(mcc_region *r, int32_t cx, int32_t cz, void *arg) {
    mcc_chunk *c = arg;
    if (!r) { mcc_chunk_free(c); return; } /* 关服取消 */
    mcc_region_world *w = r->data;
    if (mcc_store_chunk(w->chunks, cx, cz)) {
        mcc_warn("区块 (%d,%d) 重复加载，丢弃新的", cx, cz);
        mcc_chunk_free(c);
        return;
    }
    mcc_store_add_chunk(w->chunks, c);
}

void mcc_level_load_chunk(mcc_level *lv, mcc_chunk *c) {
    if (c->min_sy != lv->cfg.min_y >> 4 || c->nsec != lv->cfg.height >> 4) mcc_log_fatal("区块高度与维度不符");
    mcc_regionizer_add_chunk(&lv->rz, c->cx, c->cz);
    if (!mcc_regionizer_queue_chunk_task(&lv->rz, c->cx, c->cz, task_load, c))
        mcc_log_fatal("区块 (%d,%d) 刚登记却投递失败", c->cx, c->cz);
}

static void task_unload(mcc_region *r, int32_t cx, int32_t cz, void *arg) {
    mcc_level *lv = arg;
    if (!r) return; /* 关服取消：区域器正在销毁 */
    mcc_region_world *w = r->data;
    mcc_chunk_free(mcc_store_remove_chunk(w->chunks, cx, cz));
    mcc_regionizer_remove_chunk(&lv->rz, cx, cz);
}

void mcc_level_unload_chunk(mcc_level *lv, int32_t cx, int32_t cz) {
    if (!mcc_regionizer_queue_chunk_task(&lv->rz, cx, cz, task_unload, lv)) mcc_warn("卸载未加载的区块 (%d,%d)", cx, cz);
}

/* ---- 子世界 ---- */

typedef struct {
    mcc_level *lv;
    int32_t id;
    double pose[7];
    mcc_sublevel_fill_fn fill;
    void *arg;
} spawn_req;

static void task_spawn(mcc_region *r, int32_t cx, int32_t cz, void *arg) {
    (void)cx;
    (void)cz;
    spawn_req *q = arg;
    if (!r) { free(q); return; }
    mcc_region_world *w = r->data;
    mcc_sublevel *sl = mcc_xcalloc(1, sizeof *sl);
    sl->id = q->id;
    sl->plot_cx = PLOT_BASE_CHUNK + (q->id % PLOT_ROW) * PLOT_CHUNKS;
    sl->plot_cz = PLOT_BASE_CHUNK + (q->id / PLOT_ROW) * PLOT_CHUNKS;
    mcc_store_init(&sl->blocks, q->lv->cfg.min_y, q->lv->cfg.height);
    mcc_hmap_put(&w->sublevels, (uint64_t)(uint32_t)sl->id, sl);
    phys_sublevel_create(ensure_phys(w), sl->id, q->pose, &sl->blocks);
    if (q->fill) q->fill(q->arg, w, sl);
    /* 质心、包围盒、质量由方块自动算出，之后随方块增删更新 */
    phys_body_track_mass(w->phys, sl->id);
    free(q);
}

int32_t mcc_level_spawn_sublevel(mcc_level *lv, const double pose[7], mcc_sublevel_fill_fn fill, void *arg) {
    spawn_req *q = mcc_xmalloc(sizeof *q);
    q->lv = lv;
    q->id = atomic_fetch_add(&lv->next_sublevel_id, 1);
    memcpy(q->pose, pose, sizeof q->pose);
    q->fill = fill;
    q->arg = arg;
    int32_t id = q->id; /* 投递之后 q 归区域线程，可能已被释放 */
    int32_t cx = (int32_t)floor(pose[0]) >> 4, cz = (int32_t)floor(pose[2]) >> 4;
    if (!mcc_regionizer_queue_chunk_task(&lv->rz, cx, cz, task_spawn, q)) {
        free(q);
        return -1;
    }
    return id;
}

mcc_sublevel *mcc_region_world_sublevel(mcc_region_world *w, int32_t id) {
    return mcc_hmap_get(&w->sublevels, (uint64_t)(uint32_t)id);
}

void mcc_region_world_remove_sublevel(mcc_region_world *w, int32_t id) {
    mcc_sublevel *sl = mcc_hmap_remove(&w->sublevels, (uint64_t)(uint32_t)id);
    if (!sl) return;
    if (w->phys) phys_sublevel_remove(w->phys, id);
    sublevel_free(sl);
    maybe_drop_phys(w);
}

void mcc_level_stats_get(mcc_level *lv, mcc_level_stats *out) {
    out->regions = atomic_load(&lv->rz.region_count);
    out->chunk_moves = atomic_load(&lv->stat_chunk_moves);
    out->body_moves = atomic_load(&lv->stat_body_moves);
    mcc_tick_scheduler_stats(lv->sched, &out->tick);
}
