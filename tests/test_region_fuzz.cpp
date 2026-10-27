/*
 * 区域器单线程模糊测试：随机加载/卸载区块，随机让区域进入/结束推进，
 * 每一步后检查区域数据与区段归属完全一致。出错时可按种子精确复现。
 */
#include "../src/region/regionizer.h"
#include "../src/core/ebr.h"
#include "../src/core/log.h"
#include <stdio.h>

typedef struct { mcc_cmap chunks; } rdata;

static mcc_regionizer g_rz;
static MCC_VEC(mcc_region *) g_active;    /* 就绪区域（模拟调度器） */
static MCC_VEC(mcc_region *) g_ticking;   /* 正在"推进"的区域 */
static mcc_cmap g_loaded;                 /* 区块 → 1 */
static bool g_fail;

static void *cb_create(void *ctx, mcc_region *r) { (void)ctx; (void)r; rdata *d = (rdata*)mcc_xcalloc(1, sizeof *d); mcc_cmap_init(&d->chunks, 16); return d; }
static void cb_destroy(void *ctx, mcc_region *r) {
    (void)ctx; rdata *d = (rdata*)r->data;
    if (mcc_cmap_size(&d->chunks)) { mcc_error("区域 %llu 销毁时仍有区块", (unsigned long long)r->id); g_fail = true; }
    mcc_cmap_destroy(&d->chunks); free(d);
}
static void cb_active(void *ctx, mcc_region *r) {
    (void)ctx;
    for (size_t i = 0; i < g_active.len; i++) if (g_active.data[i] == r) return;
    mcc_region_ref(r);
    mcc_vec_push(&g_active, r);
}
static void cb_merge(void *ctx, mcc_region *from, mcc_region *into) {
    (void)ctx; rdata *f = (rdata*)from->data, *t = (rdata*)into->data;
    MCC_CMAP_FOREACH(&f->chunks, k, v, { mcc_cmap_put(&t->chunks, k, v); mcc_cmap_put(&f->chunks, k, NULL); });
}
static void cb_split(void *ctx, mcc_region *from, mcc_region **into, size_t n) {
    (void)ctx; (void)into; (void)n; rdata *f = (rdata*)from->data;
    MCC_CMAP_FOREACH(&f->chunks, k, v, {
        mcc_region *o = mcc_regionizer_region_of_chunk_locked(&g_rz, mcc_key2_x(k), mcc_key2_z(k));
        if (o && o != from) { mcc_cmap_put(&((rdata *)o->data)->chunks, k, v); mcc_cmap_put(&f->chunks, k, NULL); }
    });
}

/* 区块直接写进所属区域数据（单线程下等价于任务立即执行） */
static void check_all(int step) {
    char err[256];
    if (!mcc_regionizer_verify(&g_rz, err, sizeof err)) { mcc_error("第 %d 步自检失败：%s", step, err); g_fail = true; return; }
    MCC_CMAP_FOREACH(&g_loaded, k, v, {
        (void)v;
        mcc_region *o = mcc_regionizer_region_of_chunk(&g_rz, mcc_key2_x(k), mcc_key2_z(k));
        if (!o) { mcc_error("第 %d 步：已加载区块 (%d,%d) 无区域", step, mcc_key2_x(k), mcc_key2_z(k)); g_fail = true; return; }
        /* 区块数据必须在区段所属区域，或者在其延迟合并链上 */
        rdata *d = (rdata*)o->data;
        if (!mcc_cmap_get(&d->chunks, k)) {
            mcc_error("第 %d 步：区块 (%d,%d) 的数据不在其所属区域 %llu（状态 %d）", step,
                      mcc_key2_x(k), mcc_key2_z(k), (unsigned long long)o->id, atomic_load(&o->state));
            g_fail = true; return;
        }
    });
}

int main(int argc, char **argv) {
    uint64_t seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 1;
    int steps = argc > 2 ? atoi(argv[2]) : 200000;
    mcc_region_callbacks cb = { .create_data = cb_create, .destroy_data = cb_destroy, .on_active = cb_active,
                                .merge = cb_merge, .split = cb_split };
    mcc_regionizer_init(&g_rz, NULL, &cb);
    mcc_cmap_init(&g_loaded, 1024);
    MCC_VEC(uint64_t) loaded_list = {0};

    for (int step = 0; step < steps && !g_fail; step++) {
        seed = mcc_hash64(seed + 0x9E3779B97F4A7C15ULL);
        int op = (int)(seed % 100);
        if (op < 45) {
            /* 加载：在已有区块附近或随机远处 */
            int32_t cx, cz;
            if (loaded_list.len && (seed >> 8) % 4) {
                uint64_t base = loaded_list.data[(seed >> 16) % loaded_list.len];
                cx = mcc_key2_x(base) + (int32_t)((seed >> 24) % 41) - 20;
                cz = mcc_key2_z(base) + (int32_t)((seed >> 32) % 41) - 20;
            } else {
                cx = (int32_t)((seed >> 24) % 2000) - 1000;
                cz = (int32_t)((seed >> 40) % 2000) - 1000;
            }
            uint64_t k = mcc_key2(cx, cz);
            if (!mcc_cmap_get(&g_loaded, k)) {
                mcc_regionizer_add_chunk(&g_rz, cx, cz);
                mcc_cmap_put(&g_loaded, k, (void *)1);
                mcc_vec_push(&loaded_list, k);
                mcc_region *o = mcc_regionizer_region_of_chunk(&g_rz, cx, cz);
                mcc_cmap_put(&((rdata *)o->data)->chunks, k, (void *)1);
            }
        } else if (op < 85) {
            if (!loaded_list.len) continue;
            size_t idx = (seed >> 8) % loaded_list.len;
            uint64_t k = loaded_list.data[idx];
            loaded_list.data[idx] = loaded_list.data[--loaded_list.len];
            mcc_region *o = mcc_regionizer_region_of_chunk(&g_rz, mcc_key2_x(k), mcc_key2_z(k));
            rdata *d = (rdata*)o->data;
            if (!mcc_cmap_get(&d->chunks, k)) { mcc_error("第 %d 步卸载：数据不在所属区域", step); g_fail = true; break; }
            mcc_cmap_put(&d->chunks, k, NULL);
            mcc_cmap_put(&g_loaded, k, NULL);
            mcc_regionizer_remove_chunk(&g_rz, mcc_key2_x(k), mcc_key2_z(k));
        } else if (op < 93) {
            /* 开始推进一个就绪区域 */
            if (!g_active.len) continue;
            size_t idx = (seed >> 8) % g_active.len;
            mcc_region *r = g_active.data[idx];
            g_active.data[idx] = g_active.data[--g_active.len];
            if (mcc_regionizer_try_mark_ticking(r)) mcc_vec_push(&g_ticking, r);
            else {
                if (atomic_load(&r->state) == MCC_REGION_READY) cb_active(NULL, r);
                mcc_region_unref(r);
            }
        } else {
            /* 结束推进 */
            if (!g_ticking.len) continue;
            size_t idx = (seed >> 8) % g_ticking.len;
            mcc_region *r = g_ticking.data[idx];
            g_ticking.data[idx] = g_ticking.data[--g_ticking.len];
            if (mcc_regionizer_release(r)) cb_active(NULL, r);
            mcc_region_unref(r);
        }
        /* 推进中的区域，其区块在推进期间不会被别人改动：此处每步全量校验 */
        if (step % 1 == 0) check_all(step);
    }
    if (g_fail) { mcc_error("种子 %llu 失败", (unsigned long long)seed); return 1; }
    mcc_info("模糊测试 %d 步通过，区域 %zu，合并 %llu（延迟 %llu），拆分 %llu，销毁 %llu", steps,
             atomic_load(&g_rz.region_count), (unsigned long long)g_rz.stat_merges,
             (unsigned long long)g_rz.stat_deferred_merges, (unsigned long long)g_rz.stat_splits,
             (unsigned long long)g_rz.stat_kills);
    return 0;
}
