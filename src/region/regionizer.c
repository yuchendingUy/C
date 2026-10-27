#include "regionizer.h"
#include "../core/ebr.h"
#include "../core/log.h"
#include <stdio.h>

void (*mcc_regionizer_debug_hook)(int ev, uint64_t from_id, uint64_t to_id, const mcc_chunk_task *t);
#define DBG(ev, f, to, t) do { if (MCC_UNLIKELY(mcc_regionizer_debug_hook)) mcc_regionizer_debug_hook(ev, f, to, t); } while (0)

/* ---------- 区域与区段的创建/释放 ---------- */

static void region_free(void *p) {
    mcc_region *r = p;
    pthread_mutex_destroy(&r->task_lock);
    mcc_vec_free(&r->sections);
    mcc_vec_free(&r->expecting_from);
    mcc_vec_free(&r->tasks);
    free(r);
}

void mcc_region_ref(mcc_region *r) {
    atomic_fetch_add_explicit(&r->refs, 1, memory_order_relaxed);
}

void mcc_region_unref(mcc_region *r) {
    if (atomic_fetch_sub_explicit(&r->refs, 1, memory_order_acq_rel) == 1)
        mcc_ebr_retire(r, region_free);
}

static mcc_region *region_new(mcc_regionizer *rz, int state) {
    mcc_region *r = mcc_xcalloc(1, sizeof *r);
    r->id = atomic_fetch_add(&rz->next_region_id, 1);
    atomic_init(&r->state, state);
    atomic_init(&r->refs, 1);
    r->rz = rz;
    pthread_mutex_init(&r->task_lock, NULL);
    atomic_init(&r->in_schedule, false);
    r->next_tick_start = mcc_nanotime();
    r->data = rz->cb.create_data ? rz->cb.create_data(rz->cb.ctx, r) : NULL;
    atomic_fetch_add(&rz->region_count, 1);
    return r;
}

static void region_retire(mcc_regionizer *rz, mcc_region *r) {
    if (rz->cb.destroy_data) rz->cb.destroy_data(rz->cb.ctx, r);
    r->data = NULL;
    atomic_fetch_sub(&rz->region_count, 1);
    mcc_region_unref(r);
}

MCC_INLINE mcc_section *sec_get(mcc_regionizer *rz, int32_t sx, int32_t sz) {
    return mcc_cmap_get(&rz->sections, mcc_key2(sx, sz));
}

static mcc_section *sec_get_or_create(mcc_regionizer *rz, int32_t sx, int32_t sz, bool *created) {
    mcc_section *s = sec_get(rz, sx, sz);
    if (s) { *created = false; return s; }
    s = mcc_xcalloc(1, sizeof *s);
    s->sx = sx;
    s->sz = sz;
    atomic_init(&s->region, NULL);
    atomic_init(&s->removed, false);
    mcc_cmap_put(&rz->sections, mcc_key2(sx, sz), s);
    *created = true;
    return s;
}

/* ---------- 初始化 ---------- */

void mcc_regionizer_init(mcc_regionizer *rz, const mcc_regionizer_config *cfg, const mcc_region_callbacks *cb) {
    memset(rz, 0, sizeof *rz);
    mcc_regionizer_config d = { 4, 2, 1, 10, 4 };
    if (cfg) {
        d = *cfg;
        if (d.section_shift <= 0) d.section_shift = 4;
        if (d.empty_radius < 1) d.empty_radius = 2;
        if (d.merge_radius < 1) d.merge_radius = 1;
        if (d.max_dead_percent <= 0) d.max_dead_percent = 10;
        if (d.min_dead_to_recalc <= 0) d.min_dead_to_recalc = 4;
    }
    rz->cfg = d;
    if (cb) rz->cb = *cb;
    pthread_mutex_init(&rz->lock, NULL);
    mcc_cmap_init(&rz->sections, 1024);
    atomic_init(&rz->next_region_id, 1);
}

void mcc_regionizer_destroy(mcc_regionizer *rz) {
    /* 收集全部区域（去重）后释放 */
    mcc_region_vec regions = {0};
    MCC_CMAP_FOREACH(&rz->sections, k, v, {
        (void)k;
        mcc_section *s = v;
        mcc_region *r = atomic_load(&s->region);
        if (r && atomic_load(&r->state) != MCC_REGION_DEAD) {
            atomic_store(&r->state, MCC_REGION_DEAD);
            mcc_vec_push(&regions, r);
        }
        free(s);
    });
    for (size_t i = 0; i < regions.len; i++) {
        mcc_region *r = regions.data[i];
        for (size_t t = 0; t < r->tasks.len; t++)
            r->tasks.data[t].fn(NULL, r->tasks.data[t].cx, r->tasks.data[t].cz, r->tasks.data[t].arg);
        r->tasks.len = 0;
        if (rz->cb.destroy_data) rz->cb.destroy_data(rz->cb.ctx, r);
        region_free(r);
    }
    mcc_vec_free(&regions);
    mcc_cmap_destroy(&rz->sections);
    pthread_mutex_destroy(&rz->lock);
}

/* ---------- 合并 ---------- */

/* 把 from 立即并入 into。二者都不在推进中（或 from 是正在释放的本线程区域），持区域器锁 */
static void merge_now(mcc_regionizer *rz, mcc_region *from, mcc_region *into) {
    /* 1. 同时持有双方任务锁，再改区段归属、搬任务。
     *    若先改归属后搬任务，中间空档里新任务会直接进 into，
     *    插到 from 队列里更早的同区块任务前面，执行顺序颠倒。
     *    持锁期间看到新归属的投递者会阻塞在 into 的锁上，直到旧任务全部搬完。 */
    pthread_mutex_lock(&from->task_lock);
    pthread_mutex_lock(&into->task_lock);
    for (size_t i = 0; i < from->sections.len; i++) {
        mcc_section *s = from->sections.data[i];
        atomic_store_explicit(&s->region, into, memory_order_release);
        mcc_vec_push(&into->sections, s);
    }
    into->dead_sections += from->dead_sections;
    from->sections.len = 0;
    from->dead_sections = 0;

    /* 2. 原本等着并入 from 的区域，改为等着并入 into */
    for (size_t i = 0; i < from->expecting_from.len; i++) {
        mcc_region *x = from->expecting_from.data[i];
        x->merge_into_later = into;
        mcc_vec_push(&into->expecting_from, x);
    }
    from->expecting_from.len = 0;

    /* 3. 搬运任务队列，并在持有 from 任务锁时宣告死亡 */
    for (size_t i = 0; i < from->tasks.len; i++) {
        DBG(MCC_RZ_EV_MOVE_MERGE, from->id, into->id, &from->tasks.data[i]);
        mcc_vec_push(&into->tasks, from->tasks.data[i]);
    }
    from->tasks.len = 0;
    atomic_store(&from->state, MCC_REGION_DEAD);
    pthread_mutex_unlock(&into->task_lock);
    pthread_mutex_unlock(&from->task_lock);

    /* 4. 世界数据合并 */
    if (rz->cb.merge) rz->cb.merge(rz->cb.ctx, from, into);
    if (into->next_tick_start > from->next_tick_start) into->next_tick_start = from->next_tick_start;
    region_retire(rz, from);
    atomic_fetch_add(&rz->stat_merges, 1);
}

static void vec_remove_region(mcc_region_vec *v, mcc_region *r) {
    for (size_t i = 0; i < v->len; i++) {
        if (v->data[i] == r) {
            v->data[i] = v->data[--v->len];
            return;
        }
    }
}

static bool vec_has_region(mcc_region_vec *v, mcc_region *r) {
    for (size_t i = 0; i < v->len; i++)
        if (v->data[i] == r) return true;
    return false;
}

static void activate(mcc_regionizer *rz, mcc_region *r) {
    atomic_store(&r->state, MCC_REGION_READY);
    if (rz->cb.on_active) rz->cb.on_active(rz->cb.ctx, r);
}

/* ---------- 区块登记 ---------- */

static void mark_section_alive(mcc_regionizer *rz, mcc_section *center) {
    const int E = rz->cfg.empty_radius, M = rz->cfg.merge_radius;
    mcc_section_vec created = {0};

    /* 1. 建立/引用缓冲区段 */
    for (int dx = -E; dx <= E; dx++) {
        for (int dz = -E; dz <= E; dz++) {
            bool c;
            mcc_section *s = sec_get_or_create(rz, center->sx + dx, center->sz + dz, &c);
            /* 新建的区段（包括调用方刚建的中心区段）尚无区域 */
            if (c || !atomic_load_explicit(&s->region, memory_order_relaxed)) mcc_vec_push(&created, s);
            if (++s->buffer_refs == 1) {
                mcc_region *r = atomic_load_explicit(&s->region, memory_order_relaxed);
                if (r) r->dead_sections--;
            }
        }
    }

    /* 2. 收集合并半径内的全部区域 */
    mcc_region_vec found = {0};
    const int R = E + M;
    for (int dx = -R; dx <= R; dx++) {
        for (int dz = -R; dz <= R; dz++) {
            mcc_section *s = sec_get(rz, center->sx + dx, center->sz + dz);
            if (!s) continue;
            mcc_region *r = atomic_load_explicit(&s->region, memory_order_relaxed);
            if (r && !vec_has_region(&found, r)) mcc_vec_push(&found, r);
        }
    }

    /* 3. 分类：可立即合并的（就绪且抢占成功 / 过渡态）与正在推进的 */
    mcc_region_vec idle = {0}, ticking = {0};
    for (size_t i = 0; i < found.len; i++) {
        mcc_region *r = found.data[i];
        int st = atomic_load(&r->state);
        if (st == MCC_REGION_READY) {
            int expect = MCC_REGION_READY;
            if (atomic_compare_exchange_strong(&r->state, &expect, MCC_REGION_TRANSIENT)) {
                if (!vec_has_region(&idle, r)) mcc_vec_push(&idle, r);
                continue;
            }
            st = expect; /* 抢占失败：被调度线程抢先标为推进中 */
        }
        if (st == MCC_REGION_TICKING) {
            if (r->merge_into_later) {
                /* 已有延迟合并目标，该目标必为过渡态，一并合并 */
                if (!vec_has_region(&idle, r->merge_into_later)) mcc_vec_push(&idle, r->merge_into_later);
            } else if (!vec_has_region(&ticking, r)) {
                mcc_vec_push(&ticking, r);
            }
        } else if (st == MCC_REGION_TRANSIENT) {
            if (!vec_has_region(&idle, r)) mcc_vec_push(&idle, r);
        } else {
            mcc_log_fatal("区段指向已死亡区域 %llu", (unsigned long long)r->id);
        }
    }

    /* 4. 选目标：区段最多的空闲区域，减少搬运量；没有则新建 */
    mcc_region *target = NULL;
    for (size_t i = 0; i < idle.len; i++)
        if (!target || idle.data[i]->sections.len > target->sections.len) target = idle.data[i];
    if (!target) target = region_new(rz, MCC_REGION_TRANSIENT);

    for (size_t i = 0; i < idle.len; i++)
        if (idle.data[i] != target) merge_now(rz, idle.data[i], target);

    for (size_t i = 0; i < created.len; i++) {
        atomic_store_explicit(&created.data[i]->region, target, memory_order_release);
        mcc_vec_push(&target->sections, created.data[i]);
    }

    for (size_t i = 0; i < ticking.len; i++) {
        ticking.data[i]->merge_into_later = target;
        mcc_vec_push(&target->expecting_from, ticking.data[i]);
        atomic_fetch_add(&rz->stat_deferred_merges, 1);
    }

    if (target->expecting_from.len == 0) activate(rz, target);

    mcc_vec_free(&created);
    mcc_vec_free(&found);
    mcc_vec_free(&idle);
    mcc_vec_free(&ticking);
}

void mcc_regionizer_add_chunk(mcc_regionizer *rz, int32_t cx, int32_t cz) {
    const int sh = rz->cfg.section_shift;
    pthread_mutex_lock(&rz->lock);
    bool created;
    mcc_section *s = sec_get_or_create(rz, cx >> sh, cz >> sh, &created);
    if (++s->chunk_count == 1) mark_section_alive(rz, s);
    pthread_mutex_unlock(&rz->lock);
}

void mcc_regionizer_remove_chunk(mcc_regionizer *rz, int32_t cx, int32_t cz) {
    const int sh = rz->cfg.section_shift, E = rz->cfg.empty_radius;
    pthread_mutex_lock(&rz->lock);
    mcc_section *center = sec_get(rz, cx >> sh, cz >> sh);
    if (!center || center->chunk_count <= 0) {
        pthread_mutex_unlock(&rz->lock);
        mcc_warn("卸载未登记的区块 (%d, %d)", cx, cz);
        return;
    }
    if (--center->chunk_count == 0) {
        for (int dx = -E; dx <= E; dx++) {
            for (int dz = -E; dz <= E; dz++) {
                mcc_section *s = sec_get(rz, center->sx + dx, center->sz + dz);
                if (--s->buffer_refs == 0) {
                    mcc_region *r = atomic_load_explicit(&s->region, memory_order_relaxed);
                    r->dead_sections++;
                }
            }
        }
    }
    pthread_mutex_unlock(&rz->lock);
}

/* ---------- 查询 ---------- */

mcc_region *mcc_regionizer_region_of_chunk(mcc_regionizer *rz, int32_t cx, int32_t cz) {
    const int sh = rz->cfg.section_shift;
    mcc_section *s = mcc_cmap_get(&rz->sections, mcc_key2(cx >> sh, cz >> sh));
    return s ? atomic_load_explicit(&s->region, memory_order_acquire) : NULL;
}

mcc_region *mcc_regionizer_region_of_chunk_locked(mcc_regionizer *rz, int32_t cx, int32_t cz) {
    return mcc_regionizer_region_of_chunk(rz, cx, cz);
}

/* ---------- 推进与释放 ---------- */

bool mcc_regionizer_try_mark_ticking(mcc_region *r) {
    int expect = MCC_REGION_READY;
    return atomic_compare_exchange_strong(&r->state, &expect, MCC_REGION_TICKING);
}

static void kill_region(mcc_regionizer *rz, mcc_region *r) {
    for (size_t i = 0; i < r->sections.len; i++) {
        mcc_section *s = r->sections.data[i];
        mcc_cmap_remove(&rz->sections, mcc_key2(s->sx, s->sz));
        atomic_store(&s->removed, true);
        mcc_ebr_retire(s, free);
    }
    r->sections.len = 0;
    pthread_mutex_lock(&r->task_lock);
    atomic_store(&r->state, MCC_REGION_DEAD);
    mcc_task_vec orphan = r->tasks;
    memset(&r->tasks, 0, sizeof r->tasks);
    pthread_mutex_unlock(&r->task_lock);
    /* 区域已无任何区块：通知任务被取消（区域参数为 NULL），让其释放资源 */
    for (size_t i = 0; i < orphan.len; i++) orphan.data[i].fn(NULL, orphan.data[i].cx, orphan.data[i].cz, orphan.data[i].arg);
    mcc_vec_free(&orphan);
    region_retire(rz, r);
    atomic_fetch_add(&rz->stat_kills, 1);
}

/* 返回 false 表示区域已销毁。new_regions 收集拆出的新区域 */
static bool cleanup_and_split(mcc_regionizer *rz, mcc_region *r, mcc_region_vec *new_regions) {
    if (r->dead_sections == 0) return true;
    if (r->dead_sections == r->sections.len) {
        kill_region(rz, r);
        return false;
    }
    if (r->dead_sections < (size_t)rz->cfg.min_dead_to_recalc ||
        r->dead_sections * 100 < r->sections.len * (size_t)rz->cfg.max_dead_percent)
        return true;

    /* 1. 删除死区段 */
    size_t w = 0;
    for (size_t i = 0; i < r->sections.len; i++) {
        mcc_section *s = r->sections.data[i];
        if (s->buffer_refs == 0) {
            mcc_cmap_remove(&rz->sections, mcc_key2(s->sx, s->sz));
            atomic_store(&s->removed, true);
            mcc_ebr_retire(s, free);
        } else {
            r->sections.data[w++] = s;
        }
    }
    r->sections.len = w;
    r->dead_sections = 0;

    /* 2. 按合并半径求连通分量 */
    const int M = rz->cfg.merge_radius;
    uint32_t gen = ++rz->visit_gen;
    mcc_section_vec stack = {0};
    MCC_VEC(mcc_section_vec) comps = {0};
    for (size_t i = 0; i < r->sections.len; i++) {
        mcc_section *seed = r->sections.data[i];
        if (seed->visit_mark == gen) continue;
        mcc_section_vec comp = {0};
        seed->visit_mark = gen;
        stack.len = 0;
        mcc_vec_push(&stack, seed);
        while (stack.len) {
            mcc_section *s = stack.data[--stack.len];
            mcc_vec_push(&comp, s);
            for (int dx = -M; dx <= M; dx++) {
                for (int dz = -M; dz <= M; dz++) {
                    if (!dx && !dz) continue;
                    mcc_section *n = sec_get(rz, s->sx + dx, s->sz + dz);
                    if (!n || n->visit_mark == gen) continue;
                    if (atomic_load_explicit(&n->region, memory_order_relaxed) != r) continue;
                    n->visit_mark = gen;
                    mcc_vec_push(&stack, n);
                }
            }
        }
        mcc_vec_push(&comps, comp);
    }
    mcc_vec_free(&stack);

    if (comps.len <= 1) {
        for (size_t i = 0; i < comps.len; i++) mcc_vec_free(&comps.data[i]);
        mcc_vec_free(&comps);
        return true;
    }

    /* 3. 第一个分量留给原区域，其余各建新区域。
     *    原区域和全部新区域的任务锁在"改归属 + 重分任务"全程持有，
     *    保证同一区块的任务不会因归属切换的空档而乱序。
     *    只有持区域器锁的线程会同时持多把任务锁，投递者一次只持一把，不会死锁。 */
    size_t base = new_regions->len;
    pthread_mutex_lock(&r->task_lock);
    mcc_vec_free(&r->sections);
    r->sections = comps.data[0];
    for (size_t c = 1; c < comps.len; c++) {
        mcc_region *nr = region_new(rz, MCC_REGION_TRANSIENT);
        pthread_mutex_lock(&nr->task_lock);
        nr->next_tick_start = r->next_tick_start;
        nr->tick_count = r->tick_count;
        nr->sections = comps.data[c];
        for (size_t i = 0; i < nr->sections.len; i++)
            atomic_store_explicit(&nr->sections.data[i]->region, nr, memory_order_release);
        mcc_vec_push(new_regions, nr);
    }
    mcc_vec_free(&comps);

    /* 4. 按区块归属重分任务（保持各自相对顺序） */
    size_t keep = 0;
    for (size_t i = 0; i < r->tasks.len; i++) {
        mcc_chunk_task t = r->tasks.data[i];
        mcc_region *dst = mcc_regionizer_region_of_chunk_locked(rz, t.cx, t.cz);
        if (dst && dst != r) {
            DBG(MCC_RZ_EV_MOVE_SPLIT, r->id, dst->id, &t);
            mcc_vec_push(&dst->tasks, t);
        } else {
            r->tasks.data[keep++] = t;
        }
    }
    r->tasks.len = keep;
    for (size_t i = base; i < new_regions->len; i++) pthread_mutex_unlock(&new_regions->data[i]->task_lock);
    pthread_mutex_unlock(&r->task_lock);

    /* 5. 世界数据拆分 */
    if (rz->cb.split) rz->cb.split(rz->cb.ctx, r, new_regions->data + base, new_regions->len - base);
    atomic_fetch_add(&rz->stat_splits, 1);
    return true;
}

bool mcc_regionizer_release(mcc_region *r) {
    mcc_regionizer *rz = r->rz;
    pthread_mutex_lock(&rz->lock);

    if (r->merge_into_later) {
        mcc_region *x = r->merge_into_later;
        r->merge_into_later = NULL;
        vec_remove_region(&x->expecting_from, r);
        merge_now(rz, r, x);
        if (x->expecting_from.len == 0) activate(rz, x);
        pthread_mutex_unlock(&rz->lock);
        return false;
    }

    mcc_region_vec news = {0};
    bool alive = cleanup_and_split(rz, r, &news);
    if (alive) atomic_store(&r->state, MCC_REGION_READY);
    for (size_t i = 0; i < news.len; i++) activate(rz, news.data[i]);
    mcc_vec_free(&news);
    pthread_mutex_unlock(&rz->lock);
    return alive;
}

/* ---------- 区块任务 ---------- */

bool mcc_regionizer_queue_chunk_task(mcc_regionizer *rz, int32_t cx, int32_t cz, mcc_chunk_task_fn fn, void *arg) {
    const int sh = rz->cfg.section_shift;
    const uint64_t key = mcc_key2(cx >> sh, cz >> sh);
    mcc_ebr_enter();
    for (;;) {
        mcc_section *s = mcc_cmap_get(&rz->sections, key);
        if (!s) { mcc_ebr_exit(); return false; }
        mcc_region *r = atomic_load_explicit(&s->region, memory_order_acquire);
        if (!r) { mcc_ebr_exit(); return false; }
        pthread_mutex_lock(&r->task_lock);
        if (atomic_load(&r->state) != MCC_REGION_DEAD &&
            atomic_load_explicit(&s->region, memory_order_acquire) == r &&
            !atomic_load(&s->removed)) {
            mcc_chunk_task t = { cx, cz, fn, arg };
            DBG(MCC_RZ_EV_PUSH, 0, r->id, &t);
            mcc_vec_push(&r->tasks, t);
            pthread_mutex_unlock(&r->task_lock);
            mcc_ebr_exit();
            return true;
        }
        pthread_mutex_unlock(&r->task_lock);
        /* 区域在合并/拆分中变化，重新查询 */
    }
}

size_t mcc_region_run_tasks(mcc_region *r) {
    pthread_mutex_lock(&r->task_lock);
    mcc_task_vec batch = r->tasks;
    memset(&r->tasks, 0, sizeof r->tasks);
    pthread_mutex_unlock(&r->task_lock);
    for (size_t i = 0; i < batch.len; i++) batch.data[i].fn(r, batch.data[i].cx, batch.data[i].cz, batch.data[i].arg);
    size_t n = batch.len;
    mcc_vec_free(&batch);
    return n;
}

/* ---------- 一致性自检 ---------- */

#define VFAIL(...) do { snprintf(err, errlen, __VA_ARGS__); ok = false; goto out; } while (0)

static mcc_region *root_of(mcc_region *r) {
    return r->merge_into_later ? r->merge_into_later : r;
}

bool mcc_regionizer_verify(mcc_regionizer *rz, char *err, size_t errlen) {
    bool ok = true;
    const int E = rz->cfg.empty_radius, M = rz->cfg.merge_radius;
    pthread_mutex_lock(&rz->lock);
    mcc_region_vec regions = {0};
    MCC_CMAP_FOREACH(&rz->sections, k, v, {
        mcc_section *s = v;
        if (mcc_key2(s->sx, s->sz) != k) VFAIL("区段键不符");
        mcc_region *r = atomic_load(&s->region);
        if (!r) VFAIL("区段 (%d,%d) 无区域", s->sx, s->sz);
        if (atomic_load(&r->state) == MCC_REGION_DEAD) VFAIL("区段 (%d,%d) 指向死区域", s->sx, s->sz);
        if (s->chunk_count < 0) VFAIL("区块计数为负");
        int refs = 0;
        for (int dx = -E; dx <= E; dx++)
            for (int dz = -E; dz <= E; dz++) {
                mcc_section *n = sec_get(rz, s->sx + dx, s->sz + dz);
                if (n && n->chunk_count > 0) refs++;
            }
        if (refs != s->buffer_refs) VFAIL("区段 (%d,%d) 缓冲引用 %d，实际 %d", s->sx, s->sz, s->buffer_refs, refs);
        if (s->chunk_count > 0) {
            for (int dx = -E; dx <= E; dx++)
                for (int dz = -E; dz <= E; dz++)
                    if (!sec_get(rz, s->sx + dx, s->sz + dz)) VFAIL("活区段 (%d,%d) 缺少缓冲区段", s->sx, s->sz);
        }
        for (int dx = -M; dx <= M; dx++)
            for (int dz = -M; dz <= M; dz++) {
                mcc_section *n = sec_get(rz, s->sx + dx, s->sz + dz);
                if (n && root_of(atomic_load(&n->region)) != root_of(r))
                    VFAIL("区段 (%d,%d) 与 (%d,%d) 在合并半径内却分属两个区域", s->sx, s->sz, n->sx, n->sz);
            }
        if (!vec_has_region(&regions, r)) mcc_vec_push(&regions, r);
    });
    for (size_t i = 0; i < regions.len; i++) {
        mcc_region *r = regions.data[i];
        size_t dead = 0;
        for (size_t j = 0; j < r->sections.len; j++) {
            mcc_section *s = r->sections.data[j];
            if (atomic_load(&s->region) != r) VFAIL("区域 %llu 的区段归属错误", (unsigned long long)r->id);
            if (s->buffer_refs == 0) dead++;
        }
        if (dead != r->dead_sections) VFAIL("区域 %llu 死区段计数 %zu，实际 %zu", (unsigned long long)r->id, r->dead_sections, dead);
        int st = atomic_load(&r->state);
        if (r->expecting_from.len && st != MCC_REGION_TRANSIENT) VFAIL("等待合并的区域不是过渡态");
        if (r->merge_into_later && st != MCC_REGION_TICKING) VFAIL("延迟合并的区域不在推进中");
    }
out:
    mcc_vec_free(&regions);
    pthread_mutex_unlock(&rz->lock);
    return ok;
}
