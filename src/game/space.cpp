#include "space.h"
#include "pending.h"
#include "../core/log.h"
#include <algorithm>
#include <atomic>

namespace mcc {

static std::atomic<uint64_t> g_be_seq{ 1 };
static const SpaceAttachmentType *g_attach_types[MAX_SPACE_ATTACHMENTS];
static int g_attach_count;
static mcc_state_t g_water, g_lava;
static mcc_block_t g_water_block, g_lava_block;

int space_attachment_register(const SpaceAttachmentType *t) {
    if (g_attach_count >= MAX_SPACE_ATTACHMENTS) mcc_log_fatal("空间附加数据种类超过 %d", MAX_SPACE_ATTACHMENTS);
    g_attach_types[g_attach_count] = t;
    return g_attach_count++;
}

/* 区块内坐标键 */
MCC_INLINE uint64_t local_key(BlockPos p) {
    return (uint64_t)(p.x & 15) | ((uint64_t)(p.z & 15) << 4) | ((uint64_t)(uint32_t)(p.y + (1 << 24)) << 8);
}

static ChunkData *chunk_data(mcc_chunk *c) {
    if (!c->ext) {
        ChunkData *d = new ChunkData();
        mcc_hmap_init(&d->bes, 8);
        c->ext = d;
    }
    return (ChunkData *)c->ext;
}

static bool be_less(const BlockEntity *a, const BlockEntity *b) { return a->seq > b->seq; } /* 小顶堆 */

/* ------------------------------------------------------------------ */
/* 方块实体                                                            */
/* ------------------------------------------------------------------ */

uint64_t BlockEntity::now() const { return home->time; }
void BlockEntity::wake() { home->be_wake(this); }
void BlockEntity::wake_at(uint64_t t) { home->be_wake_at(this, t); }

BlockEntity *Space::block_entity(BlockPos p) const {
    mcc_chunk *c = mcc_store_chunk(store, p.x >> 4, p.z >> 4);
    if (!c || !c->ext) return nullptr;
    return (BlockEntity *)mcc_hmap_get(&((ChunkData *)c->ext)->bes, local_key(p));
}

void Space::add_block_entity(BlockEntity *be, mcc_chunk *c) {
    ChunkData *d = chunk_data(c);
    /* 原版 setBlockEntity：同位置已有的被替换并移除 */
    BlockEntity *old = (BlockEntity *)mcc_hmap_remove(&d->bes, local_key(be->pos));
    if (old) {
        old->removed = true;
        old->on_removed();
        graveyard.push_back(old);
    }
    be->seq = g_be_seq.fetch_add(1, std::memory_order_relaxed);
    be->chunk = c;
    be->home = this;
    be->born = time;
    be->born_in_phase = ticking_bes;
    mcc_hmap_put(&d->bes, local_key(be->pos), be);
    /* 原版新方块实体加入推进表：先推进一次，没事做就停下 */
    be_wake(be);
}

void Space::remove_block_entity(BlockPos p) {
    mcc_chunk *c = mcc_store_chunk(store, p.x >> 4, p.z >> 4);
    if (!c || !c->ext) return;
    BlockEntity *be = (BlockEntity *)mcc_hmap_remove(&((ChunkData *)c->ext)->bes, local_key(p));
    if (!be) return;
    /* 原版：先从区块移除，再 setRemoved（此时同位置查不到方块实体） */
    be->removed = true;
    be->on_removed();
    graveyard.push_back(be);
}

void Space::be_wake(BlockEntity *be) {
    if (be->removed || be->active) return;
    be->active = true;
    /* 推进阶段中：排在当前位置之后的本刻就推进（原版每刻都推进它）；
     * 之前的、或本阶段刚创建的，下一刻推进 */
    bool defer_new = be->born_in_phase && be->born == time;
    if (ticking_bes && be->seq > cur_seq && !defer_new) {
        current.push_back(be);
        std::push_heap(current.begin(), current.end(), be_less);
    } else {
        active.push_back(be);
    }
}

void Space::be_wake_at(BlockEntity *be, uint64_t t) {
    if (be->removed) return;
    if (t <= time) {
        be_wake(be);
        return;
    }
    if (t >= be->wake_time) return;
    be->wake_time = t;
    wakes.push_back({ t, be->seq, be->pos });
    std::push_heap(wakes.begin(), wakes.end(), std::greater<WakeEntry>());
}

void Space::run_block_entities() {
    current.clear();
    /* 到时间的 wake_at（按位置重新找方块实体，区块搬走或方块实体已换的条目自然作废） */
    while (!wakes.empty() && wakes.front().due <= time) {
        std::pop_heap(wakes.begin(), wakes.end(), std::greater<WakeEntry>());
        WakeEntry e = wakes.back();
        wakes.pop_back();
        BlockEntity *be = block_entity(e.pos);
        if (!be || be->seq != e.seq || be->wake_time != e.due) continue;
        be->wake_time = NEVER;
        if (!be->active) {
            be->active = true;
            current.push_back(be);
        }
    }
    for (BlockEntity *be : active) {
        if (be->removed) be->active = false;
        else if (be->home == this) current.push_back(be);
    }
    active.clear();
    std::make_heap(current.begin(), current.end(), be_less);

    ticking_bes = true;
    while (!current.empty()) {
        std::pop_heap(current.begin(), current.end(), be_less);
        BlockEntity *be = current.back();
        current.pop_back();
        be->active = false;
        if (be->removed || be->home != this || be->ticked_at == time) continue;
        cur_seq = be->seq;
        be->ticked_at = time;
        if (be->tick() && !be->removed && !be->active) {
            be->active = true;
            active.push_back(be);
        }
    }
    ticking_bes = false;
    cur_seq = 0;
}

/* ------------------------------------------------------------------ */
/* 写方块：原版 Level.setBlock + LevelChunk.setBlockState               */
/* ------------------------------------------------------------------ */

bool Space::set_block(BlockPos p, mcc_state_t state, int flags, int recursion) {
    mcc_chunk *c = mcc_store_chunk(store, p.x >> 4, p.z >> 4);
    if (!c) return false;
    int32_t sy = p.y >> 4;
    if (sy < c->min_sy || sy >= c->min_sy + c->nsec) return false;
    bool moving = flags & UPDATE_MOVE_BY_PISTON;

    /* LevelChunk.setBlockState */
    mcc_state_t old = mcc_store_set(store, p.x, p.y, p.z, state);
    if (old == state) return false;
    mcc_block_t nb = mcc_state_block(state);
    state_behaviour(old).on_remove(*this, p, old, state, moving);
    if (mcc_state_block(get(p)) != nb) return false; /* onRemove 里方块又被换掉 */
    state_behaviour(state).on_place(*this, p, state, old, moving);
    if (state_has_block_entity(state)) {
        BlockEntity *be = block_entity(p);
        if (!be) {
            be = state_behaviour(state).new_block_entity(p, state);
            if (be) add_block_entity(be, c);
        } else {
            be->set_block_state(state);
        }
    }

    /* Level.setBlock 的通知部分：只有状态没被中途改掉才通知 */
    if (get(p) == state) {
        if (flags & UPDATE_CLIENTS) net_block_updated(*this, p, old, state, flags);
        if (flags & UPDATE_NEIGHBORS) {
            update_neighbors_at(p, mcc_state_block(old));
            redstone_update_output_signal(*this, p, nb);
        }
        if (!(flags & UPDATE_KNOWN_SHAPE) && recursion > 0) {
            int f2 = flags & ~(UPDATE_NEIGHBORS | UPDATE_SUPPRESS_DROPS);
            state_behaviour(old).update_indirect_neighbour_shapes(*this, p, old, f2, recursion - 1);
            update_neighbour_shapes(p, state, f2, recursion - 1);
            state_behaviour(state).update_indirect_neighbour_shapes(*this, p, state, f2, recursion - 1);
        }
    }
    return true;
}

/* 原版 FluidState.createLegacyBlock：含水方块破坏后留下水源 */
static mcc_state_t legacy_fluid_block(mcc_state_t s) {
    switch (mcc_state_fluid(s)) {
    case MCC_FLUID_WATER: return mcc_state_block(s) == g_water_block ? s : g_water;
    case MCC_FLUID_LAVA: return mcc_state_block(s) == g_lava_block ? s : g_lava;
    default: return 0;
    }
}

bool Space::destroy_block(BlockPos p, bool drop, int recursion) {
    mcc_state_t s = get(p);
    if (mcc_state_is_air(s)) return false;
    net_level_event(*this, 2001, p, (int)s);
    if (drop) drops_drop_resources(*this, p, s, state_has_block_entity(s) ? block_entity(p) : nullptr);
    return set_block(p, legacy_fluid_block(s), UPDATE_ALL, recursion);
}

/* ------------------------------------------------------------------ */
/* 原版 CollectingNeighborUpdater                                      */
/* ------------------------------------------------------------------ */

#define MAX_CHAINED_NEIGHBOR_UPDATES 1000000

/* 原版 Block.updateOrDestroy */
static void update_or_destroy(Space &sp, mcc_state_t old, mcc_state_t now, BlockPos p, int flags, int recursion) {
    if (now == old) return;
    if (mcc_state_is_air(now)) sp.destroy_block(p, !(flags & UPDATE_SUPPRESS_DROPS), recursion);
    else sp.set_block(p, now, flags & ~UPDATE_SUPPRESS_DROPS, recursion);
}

/* 原版 NeighborUpdater.executeUpdate */
static void execute_update(Space &sp, BlockPos p, mcc_block_t block, BlockPos from) {
    if (!sp.is_loaded(p)) return;
    mcc_state_t s = sp.get(p);
    state_behaviour(s).neighbor_changed(sp, p, s, block, from, false);
}

/* 执行一步；返回这条待办是否还有下一步（只有 MULTI 有多步） */
static bool run_next(Space &sp, NeighborUpdate &u) {
    switch (u.kind) {
    case NeighborUpdate::SHAPE: {
        if (!sp.is_loaded(u.pos)) return false;
        mcc_state_t cur = sp.get(u.pos);
        mcc_state_t now = state_behaviour(cur).update_shape(sp, u.pos, cur, u.dir, u.state, u.from);
        update_or_destroy(sp, cur, now, u.pos, u.flags, u.recursion);
        return false;
    }
    case NeighborUpdate::SIMPLE:
        execute_update(sp, u.pos, u.block, u.from);
        return false;
    case NeighborUpdate::MULTI: {
        BlockPos np = u.pos.offset(UPDATE_ORDER[u.idx++]);
        execute_update(sp, np, u.block, u.pos);
        if (u.idx < 6 && u.has_skip && UPDATE_ORDER[u.idx] == u.dir) u.idx++;
        return u.idx < 6;
    }
    }
    return false;
}

void Space::nu_add_and_run(const NeighborUpdate &u) {
    bool running = nu_count > 0;
    bool over = nu_count >= MAX_CHAINED_NEIGHBOR_UPDATES;
    nu_count++;
    if (!over) {
        if (running) nu_added.push_back(u);
        else nu_stack.push_back(u);
    } else if (nu_count - 1 == MAX_CHAINED_NEIGHBOR_UPDATES) {
        mcc_error("连锁方块更新过多，跳过其余（第一个跳过的位置 %d,%d,%d）", u.pos.x, u.pos.y, u.pos.z);
    }
    if (running) return;
    while (!nu_stack.empty() || !nu_added.empty()) {
        for (size_t i = nu_added.size(); i-- > 0;) nu_stack.push_back(nu_added[i]);
        nu_added.clear();
        /* 执行期间只会往 nu_added 里加，栈顶不会移动 */
        size_t top = nu_stack.size() - 1;
        while (nu_added.empty()) {
            if (!run_next(*this, nu_stack[top])) {
                nu_stack.pop_back();
                break;
            }
        }
    }
    nu_stack.clear();
    nu_added.clear();
    nu_count = 0;
}

void Space::update_neighbors_at(BlockPos p, mcc_block_t block) {
    NeighborUpdate u{};
    u.kind = NeighborUpdate::MULTI;
    u.pos = p;
    u.block = block;
    nu_add_and_run(u);
}

void Space::update_neighbors_at_except(BlockPos p, mcc_block_t block, Dir skip) {
    NeighborUpdate u{};
    u.kind = NeighborUpdate::MULTI;
    u.pos = p;
    u.block = block;
    u.has_skip = true;
    u.dir = skip;
    u.idx = UPDATE_ORDER[0] == skip ? 1 : 0;
    nu_add_and_run(u);
}

void Space::neighbor_changed(BlockPos p, mcc_block_t block, BlockPos from) {
    NeighborUpdate u{};
    u.kind = NeighborUpdate::SIMPLE;
    u.pos = p;
    u.from = from;
    u.block = block;
    nu_add_and_run(u);
}

void Space::update_neighbour_shapes(BlockPos p, mcc_state_t state, int flags, int recursion) {
    for (Dir d : UPDATE_SHAPE_ORDER) {
        NeighborUpdate u{};
        u.kind = NeighborUpdate::SHAPE;
        u.dir = dir_opposite(d);
        u.state = state;
        u.pos = p.offset(d);
        u.from = p;
        u.flags = flags;
        u.recursion = recursion;
        nu_add_and_run(u);
    }
}

/* ------------------------------------------------------------------ */
/* 计划刻                                                              */
/* ------------------------------------------------------------------ */

void Space::schedule_tick(BlockPos p, mcc_block_t block, int delay, TickPriority prio) {
    mcc_chunk *c = mcc_store_chunk(store, p.x >> 4, p.z >> 4);
    if (!c) return;
    ChunkData *d = chunk_data(c);
    for (const ScheduledTick &t : d->ticks)
        if (t.pos == p && t.block == block) return;
    d->ticks.push_back({ time + (uint64_t)delay, next_sub++, p, block, prio });
    mcc_hmap_put(&tick_chunks, mcc_key2(c->cx, c->cz), c);
}

bool Space::has_scheduled_tick(BlockPos p, mcc_block_t block) const {
    mcc_chunk *c = mcc_store_chunk(store, p.x >> 4, p.z >> 4);
    if (!c || !c->ext) return false;
    for (const ScheduledTick &t : ((ChunkData *)c->ext)->ticks)
        if (t.pos == p && t.block == block) return true;
    return false;
}

void Space::run_scheduled_ticks() {
    if (!tick_chunks.count) return;
    std::vector<ScheduledTick> due;
    std::vector<uint64_t> empty;
    uint32_t it = 0;
    uint64_t key;
    void *v;
    while (mcc_hmap_next(&tick_chunks, &it, &key, &v)) {
        ChunkData *d = (ChunkData *)((mcc_chunk *)v)->ext;
        size_t w = 0;
        for (size_t i = 0; i < d->ticks.size(); i++) {
            if (d->ticks[i].due <= time) due.push_back(d->ticks[i]);
            else d->ticks[w++] = d->ticks[i];
        }
        d->ticks.resize(w);
        if (!w) empty.push_back(key);
    }
    for (uint64_t k : empty) mcc_hmap_remove(&tick_chunks, k);
    /* 原版 ScheduledTick.DRAIN_ORDER：时间、优先级、同刻先后 */
    std::sort(due.begin(), due.end(), [](const ScheduledTick &a, const ScheduledTick &b) {
        if (a.due != b.due) return a.due < b.due;
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.sub < b.sub;
    });
    for (const ScheduledTick &t : due) {
        if (!is_loaded(t.pos)) continue;
        mcc_state_t s = get(t.pos);
        if (mcc_state_block(s) == t.block) state_behaviour(s).tick(*this, t.pos, s);
    }
}

/* ------------------------------------------------------------------ */
/* 推进                                                                */
/* ------------------------------------------------------------------ */

void Space::tick() {
    time++;
    run_scheduled_ticks();
    run_block_entities();
    if (!graveyard.empty()) {
        /* 已移除的从推进表里清掉，再释放 */
        active.erase(std::remove_if(active.begin(), active.end(), [](BlockEntity *be) { return be->removed; }), active.end());
        for (BlockEntity *be : graveyard) delete be;
        graveyard.clear();
    }
}

/* ------------------------------------------------------------------ */
/* 区块进出空间（加载、卸载、区域合并拆分时搬家）                        */
/* ------------------------------------------------------------------ */

void Space::chunk_removed(mcc_chunk *c) {
    ChunkData *d = (ChunkData *)c->ext;
    mcc_hmap_remove(&tick_chunks, mcc_key2(c->cx, c->cz));
    if (!d) return;
    bool any_active = false;
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&d->bes, &it, NULL, &v)) {
        BlockEntity *be = (BlockEntity *)v;
        any_active |= be->active;
        /* 时间换成相对值，到新空间再换回来 */
        if (be->wake_time != NEVER) be->wake_time = be->wake_time > time ? be->wake_time - time : 0;
    }
    if (any_active)
        active.erase(std::remove_if(active.begin(), active.end(), [c](BlockEntity *be) { return be->chunk == c; }), active.end());
    for (ScheduledTick &t : d->ticks) t.due = t.due > time ? t.due - time : 0;
    d->relative = true;
}

void Space::chunk_added(mcc_chunk *c) {
    ChunkData *d = (ChunkData *)c->ext;
    if (!d) return;
    uint32_t it = 0;
    void *v;
    std::vector<std::pair<BlockEntity *, Space *>> moved;
    while (mcc_hmap_next(&d->bes, &it, NULL, &v)) {
        BlockEntity *be = (BlockEntity *)v;
        Space *old = be->home;
        be->home = this;
        if (d->relative && be->wake_time != NEVER) {
            uint64_t t = time + be->wake_time;
            be->wake_time = NEVER;
            be_wake_at(be, t);
        }
        if (be->active) active.push_back(be);
        if (old && old != this) moved.push_back({ be, old });
    }
    if (d->relative)
        for (ScheduledTick &t : d->ticks) t.due += time;
    d->relative = false;
    if (!d->ticks.empty()) mcc_hmap_put(&tick_chunks, mcc_key2(c->cx, c->cz), c);
    for (auto &m : moved) m.first->on_space_changed(*m.second);
}

static void on_chunk_added(void *ctx, mcc_chunk *c) { ((Space *)ctx)->chunk_added(c); }
static void on_chunk_removed(void *ctx, mcc_chunk *c) { ((Space *)ctx)->chunk_removed(c); }

Space &space_of(mcc_chunk_store *store, int32_t sublevel) {
    if (store->ext) return *(Space *)store->ext;
    Space *sp = new Space();
    sp->store = store;
    sp->sublevel = sublevel;
    mcc_hmap_init(&sp->tick_chunks, 16);
    sp->listener.ctx = sp;
    sp->listener.chunk_added = on_chunk_added;
    sp->listener.chunk_removed = on_chunk_removed;
    mcc_store_listen(store, &sp->listener);
    store->ext = sp;
    /* 接管已经在存储里的区块 */
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&store->chunks, &it, NULL, &v)) sp->chunk_added((mcc_chunk *)v);
    return *sp;
}

static void chunk_ext_free(mcc_chunk *c) {
    ChunkData *d = (ChunkData *)c->ext;
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&d->bes, &it, NULL, &v)) {
        BlockEntity *be = (BlockEntity *)v;
        be->on_chunk_unloaded();
        delete be;
    }
    mcc_hmap_free(&d->bes);
    delete d;
    c->ext = nullptr;
}

static void store_ext_free(mcc_chunk_store *st) {
    Space *sp = (Space *)st->ext;
    for (BlockEntity *be : sp->graveyard) delete be;
    for (int i = 0; i < g_attach_count; i++)
        if (sp->attach[i] && g_attach_types[i]->destroy) g_attach_types[i]->destroy(sp->attach[i]);
    mcc_hmap_free(&sp->tick_chunks);
    delete sp;
}

void game_init() {
    mcc_chunk_ext_free = chunk_ext_free;
    mcc_store_ext_free = store_ext_free;
    g_water = mcc_block_default_state("minecraft:water");
    g_lava = mcc_block_default_state("minecraft:lava");
    g_water_block = mcc_state_block(g_water);
    g_lava_block = mcc_state_block(g_lava);
}

} // namespace mcc
