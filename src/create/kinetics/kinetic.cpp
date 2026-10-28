#include "kinetic.h"
#include "../stress.h"
#include "../../game/pending.h"
#include "../../core/log.h"
#include <math.h>

namespace mcc::create {

KineticsConfig g_kinetics;

/* Java Math.signum（0 → 0） */
static int signum(float v) { return (v > 0) - (v < 0); }

/* ------------------------------------------------------------------ */
/* IRotate.SpeedLevel                                                  */
/* ------------------------------------------------------------------ */

float speed_level_value(SpeedLevel l) {
    switch (l) {
    case SpeedLevel::FAST: return g_kinetics.fast_speed;
    case SpeedLevel::MEDIUM: return g_kinetics.medium_speed;
    case SpeedLevel::SLOW: return 1;
    default: return 0;
    }
}

SpeedLevel speed_level_of(float speed) {
    speed = fabsf(speed);
    if (speed >= g_kinetics.fast_speed) return SpeedLevel::FAST;
    if (speed >= g_kinetics.medium_speed) return SpeedLevel::MEDIUM;
    if (speed >= 1) return SpeedLevel::SLOW;
    return SpeedLevel::NONE;
}

/* ------------------------------------------------------------------ */
/* KineticBlock                                                        */
/* ------------------------------------------------------------------ */

bool KineticBlock::states_kinetically_equivalent(mcc_state_t old, mcc_state_t now) {
    if (mcc_state_block(old) != mcc_state_block(now)) return false;
    return rotation_axis(now) == rotation_axis(old);
}

void KineticBlock::on_place(Space &sp, BlockPos pos, mcc_state_t state, mcc_state_t old, bool moving) {
    (void)moving;
    KineticBlockEntity *kbe = as_kinetic(sp.block_entity(pos));
    if (!kbe) return;
    kbe->prevent_speed_update = 0;
    if (mcc_state_block(old) != mcc_state_block(state)) return;
    if (state_has_block_entity(state) != state_has_block_entity(old)) return;
    if (!states_kinetically_equivalent(old, state)) return;
    kbe->prevent_speed_update = 2;
    kbe->wake(); /* 原版下一刻推进时清零 */
}

/* IBE.onRemove */
void KineticBlock::on_remove(Space &sp, BlockPos pos, mcc_state_t state, mcc_state_t now, bool moving) {
    (void)moving;
    if (!state_has_block_entity(state)) return;
    if (mcc_state_block(state) == mcc_state_block(now) && state_has_block_entity(now)) return;
    KineticBlockEntity *kbe = as_kinetic(sp.block_entity(pos));
    if (kbe) kbe->destroy();
    sp.remove_block_entity(pos);
}

void KineticBlock::update_indirect_neighbour_shapes(Space &sp, BlockPos pos, mcc_state_t state, int flags, int recursion) {
    (void)state; (void)flags; (void)recursion;
    KineticBlockEntity *kbe = as_kinetic(sp.block_entity(pos));
    if (!kbe) return;
    if (kbe->prevent_speed_update > 0) return;
    kbe->warn_of_movement();
    kbe->clear_kinetic_information();
    kbe->update_speed = true;
    kbe->wake();
}

/* ------------------------------------------------------------------ */
/* 网络表                                                              */
/* ------------------------------------------------------------------ */

static int g_net_slot = -1;

struct NetworkTable {
    mcc_hmap map; /* 编号 → KineticNetwork* */
};

static void table_destroy(void *p) {
    NetworkTable *t = (NetworkTable *)p;
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&t->map, &it, NULL, &v)) delete (KineticNetwork *)v;
    mcc_hmap_free(&t->map);
    delete t;
}

static const SpaceAttachmentType g_table_type = { table_destroy };

static NetworkTable &table_of(Space &sp) {
    if (!sp.attach[g_net_slot]) {
        NetworkTable *t = new NetworkTable();
        mcc_hmap_init(&t->map, 16);
        sp.attach[g_net_slot] = t;
    }
    return *(NetworkTable *)sp.attach[g_net_slot];
}

void kinetics_init() {
    if (g_net_slot < 0) g_net_slot = space_attachment_register(&g_table_type);
}

KineticNetwork *network_get(Space &sp, int64_t id) {
    return (KineticNetwork *)mcc_hmap_get(&table_of(sp).map, (uint64_t)id);
}

KineticNetwork *network_get_or_create(Space &sp, int64_t id) {
    NetworkTable &t = table_of(sp);
    KineticNetwork *n = (KineticNetwork *)mcc_hmap_get(&t.map, (uint64_t)id);
    if (!n) {
        n = new KineticNetwork();
        n->id = id;
        n->space = &sp;
        mcc_hmap_put(&t.map, (uint64_t)id, n);
    }
    return n;
}

void network_drop(Space &sp, int64_t id) {
    KineticNetwork *n = (KineticNetwork *)mcc_hmap_remove(&table_of(sp).map, (uint64_t)id);
    if (!n) return;
    for (KineticBlockEntity *be : n->members) { be->net_obj = nullptr; be->net_member_idx = -1; }
    for (KineticBlockEntity *be : n->sources) { be->net_obj = nullptr; be->net_source_idx = -1; }
    delete n;
}

/* ------------------------------------------------------------------ */
/* KineticNetwork                                                      */
/* ------------------------------------------------------------------ */

/* 已移除的方块实体不进表：原版里它在本刻剩下的推进中可能把自己加回网络，
 * 网络下次重算时发现它已不在世界里就会剔除，这里直接不加，结果相同 */
void KineticNetwork::link_member(KineticBlockEntity *be, float st) {
    if (be->removed) return;
    if (be->net_obj && be->net_obj != this) be->net_obj->unlink(be);
    if (be->net_member_idx < 0) {
        be->net_member_idx = (int)members.size();
        members.push_back(be);
    }
    be->net_obj = this;
    be->net_stress = st;
    sums_dirty = true;
}

void KineticNetwork::link_source(KineticBlockEntity *be, float cap) {
    if (be->removed) return;
    if (be->net_obj && be->net_obj != this) be->net_obj->unlink(be);
    if (be->net_source_idx < 0) {
        be->net_source_idx = (int)sources.size();
        sources.push_back(be);
    }
    be->net_obj = this;
    be->net_capacity = cap;
    sums_dirty = true;
}

void KineticNetwork::unlink(KineticBlockEntity *be) {
    if (be->net_member_idx >= 0) {
        KineticBlockEntity *last = members.back();
        members[be->net_member_idx] = last;
        last->net_member_idx = be->net_member_idx;
        members.pop_back();
        be->net_member_idx = -1;
    }
    if (be->net_source_idx >= 0) {
        KineticBlockEntity *last = sources.back();
        sources[be->net_source_idx] = last;
        last->net_source_idx = be->net_source_idx;
        sources.pop_back();
        be->net_source_idx = -1;
    }
    if (be->net_obj == this) be->net_obj = nullptr;
    sums_dirty = true;
}

void KineticNetwork::init_from_te(float max_stress, float cur_stress, int n) {
    unloaded_capacity = max_stress;
    unloaded_stress = cur_stress;
    unloaded_members = n;
    initialized = true;
    sums_dirty = true;
    update_stress();
    update_capacity();
}

void KineticNetwork::add_silently(KineticBlockEntity *be, float last_capacity, float last_stress) {
    if (be->net_member_idx >= 0 && be->net_obj == this) return;
    if (be->is_source()) {
        unloaded_capacity -= last_capacity * fabsf(be->generated_speed());
        link_source(be, be->calculate_added_stress_capacity());
    }
    unloaded_stress -= last_stress * fabsf(be->theoretical_speed());
    link_member(be, be->calculate_stress_applied());
    unloaded_members--;
    if (unloaded_members < 0) unloaded_members = 0;
    if (unloaded_capacity < 0) unloaded_capacity = 0;
    if (unloaded_stress < 0) unloaded_stress = 0;
}

void KineticNetwork::add(KineticBlockEntity *be) {
    if (be->net_member_idx >= 0 && be->net_obj == this) return;
    if (be->is_source()) link_source(be, be->calculate_added_stress_capacity());
    link_member(be, be->calculate_stress_applied());
    be->update_from_network(current_capacity, current_stress, size());
    be->network_dirty = true;
    be->wake();
}

void KineticNetwork::update_capacity_for(KineticBlockEntity *be, float cap) {
    link_source(be, cap);
    update_capacity();
}

void KineticNetwork::update_stress_for(KineticBlockEntity *be, float st) {
    link_member(be, st);
    update_stress();
}

void KineticNetwork::remove(KineticBlockEntity *be) {
    if (be->net_member_idx < 0 || be->net_obj != this) return;
    /* 原版只在 isSource() 时从 sources 移除：动力源停转后再拆掉会在旧网络里残留一条，
     * 之后它重新转起来会给旧网络凭空加容量。这里成员和动力源一起摘掉（修正） */
    unlink(be);
    be->update_from_network(0, 0, 0);
    if (members.empty()) {
        network_drop(*space, id);
        return;
    }
    KineticBlockEntity *first = members.front();
    first->network_dirty = true;
    first->wake();
}

void KineticNetwork::sync() {
    std::vector<KineticBlockEntity *> list = members; /* 回调里可能改动成员表 */
    for (KineticBlockEntity *be : list) be->update_from_network(current_capacity, current_stress, size());
}

void KineticNetwork::update_capacity() {
    float v = calculate_capacity();
    if (current_capacity != v) {
        current_capacity = v;
        sync();
    }
}

void KineticNetwork::update_stress() {
    float v = calculate_stress();
    if (current_stress != v) {
        current_stress = v;
        sync();
    }
}

void KineticNetwork::update_network() {
    float ns = calculate_stress(), nc = calculate_capacity();
    if (current_stress != ns || current_capacity != nc) {
        current_stress = ns;
        current_capacity = nc;
        sync();
    }
}

float KineticNetwork::actual_capacity_of(KineticBlockEntity *be) { return be->net_capacity * fabsf(be->generated_speed()); }
float KineticNetwork::actual_stress_of(KineticBlockEntity *be) { return be->net_stress * fabsf(be->theoretical_speed()); }

/* 原版每次都把整张表遍历一遍。刚连上一长串部件时，每个新成员下一刻都要求重算一次，
 * 全网 n 个成员就是 n 次 O(n)。同一刻里表、取值、转速都没变时结果一样，直接用缓存；
 * 动力源的发电转速理论上可能不经通知就变化，所以跨刻一定重算 */
void KineticNetwork::recompute() {
    float cap = 0, st = 0;
    for (KineticBlockEntity *be : sources) cap += actual_capacity_of(be);
    for (KineticBlockEntity *be : members) st += actual_stress_of(be);
    cached_capacity = cap;
    cached_stress = st;
    cache_time = space->time;
    sums_dirty = false;
}

float KineticNetwork::calculate_capacity() {
    if (sums_dirty || cache_time != space->time) recompute();
    return cached_capacity + unloaded_capacity;
}

float KineticNetwork::calculate_stress() {
    if (sums_dirty || cache_time != space->time) recompute();
    return cached_stress + unloaded_stress;
}

/* ------------------------------------------------------------------ */
/* KineticBlockEntity                                                  */
/* ------------------------------------------------------------------ */

KineticBlockEntity::~KineticBlockEntity() {
    if (net_obj) net_obj->unlink(this);
}

void KineticBlockEntity::initialize() {
    if (has_network) {
        KineticNetwork *n = get_or_create_network();
        if (!n->initialized) n->init_from_te(capacity, stress, network_size);
        n->add_silently(this, last_capacity_provided, last_stress_applied);
    }
    /* SmartBlockEntity.initialize：行为初始化 + 一次慢推进 */
    if (has_lazy_tick) lazy_tick();
}

bool KineticBlockEntity::tick() {
    uint64_t t = now();
    if (update_speed) attach_kinetics();

    /* SmartBlockEntity.tick */
    if (!initialized) {
        initialize();
        initialized = true;
        if (has_lazy_tick) lazy_due = t + (uint64_t)lazy_tick_rate;
    } else if (has_lazy_tick && t >= lazy_due) {
        lazy_due = t + (uint64_t)lazy_tick_rate + 1;
        lazy_tick();
    }
    /* KineticEffectHandler.tick：服务端只有旋转指示粒子（TODO(网络)） */

    prevent_speed_update = 0;
    if (t >= validation_due) {
        validation_due = t + (uint64_t)g_kinetics.kinetic_validation_frequency + 1;
        validate_kinetics();
    }
    if (flicker_score() > 0) flicker_tally = flicker_score() - 1;
    if (network_dirty) {
        if (has_network) get_or_create_network()->update_network();
        network_dirty = false;
    }
    bool more = tick_extra();
    schedule_next();
    return more || update_speed || flicker_tally > 0 || network_dirty || prevent_speed_update > 0;
}

void KineticBlockEntity::schedule_next() {
    if (removed) return;
    uint64_t t = validation_due;
    if (has_lazy_tick && lazy_due < t) t = lazy_due;
    wake_at(t);
}

void KineticBlockEntity::validate_kinetics() {
    Space &sp = space();
    if (has_source) {
        if (!has_network) {
            remove_source();
            return;
        }
        if (!sp.is_loaded(source)) return;
        KineticBlockEntity *src = as_kinetic(sp.block_entity(source));
        if (!src || src->speed == 0) {
            remove_source();
            detach_kinetics();
        }
        return;
    }
    if (speed != 0 && generated_speed() == 0) set_speed(0);
}

void KineticBlockEntity::update_from_network(float max_stress, float current_stress, int size) {
    network_dirty = false;
    capacity = max_stress;
    stress = current_stress;
    network_size = size;
    bool os = max_stress < current_stress && stress_enabled();
    set_changed();
    if (os != over_stressed) {
        float prev = get_speed();
        over_stressed = os;
        on_speed_changed(prev);
        send_data();
    }
}

float KineticBlockEntity::calculate_stress_applied() {
    float impact = (float)stress_impact(mcc_state_block(state));
    last_stress_applied = impact;
    return impact;
}

float KineticBlockEntity::calculate_added_stress_capacity() {
    float cap = (float)stress_capacity(mcc_state_block(state));
    last_capacity_provided = cap;
    return cap;
}

void KineticBlockEntity::on_speed_changed(float previous_speed) {
    bool from_or_to_zero = (previous_speed == 0) != (get_speed() == 0);
    bool direction_swap = !from_or_to_zero && signum(previous_speed) != signum(get_speed());
    if (from_or_to_zero || direction_swap) {
        flicker_tally = flicker_score() + 5;
        wake();
    }
    set_changed();
}

float KineticBlockEntity::get_speed() {
    if (over_stressed) return 0; /* 原版另有 tickRateManager().isFrozen()（/tick freeze） */
    return theoretical_speed();
}

void KineticBlockEntity::set_speed(float s) {
    speed = s;
    if (net_obj) net_obj->sums_dirty = true;
}

void KineticBlockEntity::set_source(BlockPos src) {
    has_source = true;
    source = src;
    KineticBlockEntity *sbe = as_kinetic(space().block_entity(src));
    if (!sbe) {
        remove_source();
        return;
    }
    set_network(sbe->has_network, sbe->network);
    /* TODO(顺序齿轮箱)：copySequenceContextFrom */
}

void KineticBlockEntity::remove_source() {
    float prev = get_speed();
    set_speed(0);
    has_source = false;
    set_network(false, 0);
    on_speed_changed(prev);
}

void KineticBlockEntity::set_network(bool has, int64_t id) {
    if (has_network == has && (!has || network == id)) return;
    if (has_network) get_or_create_network()->remove(this);
    has_network = has;
    network = id;
    set_changed();
    if (!has) return;
    KineticNetwork *n = get_or_create_network();
    n->initialized = true;
    n->add(this);
}

KineticNetwork *KineticBlockEntity::get_or_create_network() {
    if (!has_network) return nullptr;
    return network_get_or_create(space(), network);
}

void KineticBlockEntity::attach_kinetics() {
    update_speed = false;
    rotation_handle_added(space(), pos, this);
}

void KineticBlockEntity::detach_kinetics() { rotation_handle_removed(space(), pos, this); }

bool KineticBlockEntity::speed_requirement_fulfilled() {
    KineticBlock *def = kinetic_block(state);
    if (!def) return true;
    return fabsf(get_speed()) >= speed_level_value(def->min_required_speed_level());
}

void KineticBlockEntity::clear_kinetic_information() {
    set_speed(0);
    has_source = false;
    /* 原版只把 network 置空，不从网络对象里移除：它会同时留在旧网络的成员表里，
     * 之后接到别的网络时旧网络还按它的转速算应力（幽灵应力），接回同一网络时又不刷新自己的
     * 超载状态。这里直接从旧网络摘掉（修正） */
    if (net_obj) net_obj->unlink(this);
    has_network = false;
    network = 0;
    over_stressed = false;
    stress = 0;
    capacity = 0;
    last_stress_applied = 0;
    last_capacity_provided = 0;
}

void KineticBlockEntity::send_data() { net_block_entity_data(this); }

void KineticBlockEntity::add_propagation_locations(KineticBlock *block, mcc_state_t st, std::vector<BlockPos> &out) {
    if (!can_propagate_diagonally(block, st)) return;
    Axis axis = block->rotation_axis(st);
    /* BlockPos.betweenClosedStream(-1,-1,-1 → 1,1,1)：x 最快，其次 y，最后 z */
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                BlockPos o{ dx, dy, dz };
                if (o.along(axis) != 0) continue;
                if (o.dist_sqr() != 2) continue;
                out.push_back(pos.offset(dx, dy, dz));
            }
}

bool KineticBlockEntity::can_propagate_diagonally(KineticBlock *block, mcc_state_t st) {
    (void)block;
    return is_small_cog(st);
}

/* SmartBlockEntity.setRemoved：方块被移除时 remove()（区块卸载不走这里） */
void KineticBlockEntity::on_removed() {
    if (has_network) get_or_create_network()->remove(this);
    detach_kinetics();
}

void KineticBlockEntity::on_chunk_unloaded() {
    /* 原版：区块卸载不调用 remove()，网络在下次重算时发现它已经不在世界里，把它从表里去掉 */
    if (net_obj) {
        KineticNetwork *n = net_obj;
        n->unlink(this);
        if (n->members.empty() && n->sources.empty()) network_drop(*home, n->id);
    }
}

/* 区块随区域合并/拆分搬到另一个空间：网络成员身份跟着搬到新空间的同号网络 */
void KineticBlockEntity::on_space_changed(Space &from) {
    KineticNetwork *old = net_obj;
    if (!old) return;
    bool member = net_member_idx >= 0, src = net_source_idx >= 0;
    float st = net_stress, cap = net_capacity;
    old->unlink(this);
    bool fresh = network_get(space(), old->id) == nullptr;
    KineticNetwork *n = network_get_or_create(space(), old->id);
    if (fresh) {
        n->initialized = old->initialized;
        n->current_capacity = old->current_capacity;
        n->current_stress = old->current_stress;
        n->unloaded_capacity = old->unloaded_capacity;
        n->unloaded_stress = old->unloaded_stress;
        n->unloaded_members = old->unloaded_members;
    }
    if (member) n->link_member(this, st);
    if (src) n->link_source(this, cap);
    if (old->members.empty() && old->sources.empty()) network_drop(from, old->id);
}

/* ------------------------------------------------------------------ */
/* GeneratingKineticBlockEntity                                        */
/* ------------------------------------------------------------------ */

void GeneratingKineticBlockEntity::notify_stress_capacity_change(float cap) { get_or_create_network()->update_capacity_for(this, cap); }

void GeneratingKineticBlockEntity::remove_source() {
    if (has_source && is_source()) {
        reactivate_source = true;
        wake();
    }
    KineticBlockEntity::remove_source();
}

void GeneratingKineticBlockEntity::set_source(BlockPos src) {
    KineticBlockEntity::set_source(src);
    KineticBlockEntity *sbe = as_kinetic(space().block_entity(src));
    if (!sbe) return;
    if (reactivate_source && fabsf(sbe->get_speed()) >= fabsf(generated_speed())) reactivate_source = false;
}

bool GeneratingKineticBlockEntity::tick_extra() {
    if (reactivate_source) {
        update_generated_rotation();
        reactivate_source = false;
    }
    return false;
}

void GeneratingKineticBlockEntity::update_generated_rotation() {
    float s = generated_speed();
    float prev = speed;
    if (prev != s) {
        if (!has_source) {
            if (speed_level_of(speed) != speed_level_of(s)) queue_rotation_indicators();
        }
        apply_new_speed(prev, s);
    }
    if (has_network && s != 0) {
        KineticNetwork *n = get_or_create_network();
        notify_stress_capacity_change(calculate_added_stress_capacity());
        get_or_create_network()->update_stress_for(this, calculate_stress_applied());
        n->update_stress();
    }
    on_speed_changed(prev);
    send_data();
}

void GeneratingKineticBlockEntity::apply_new_speed(float prev, float s) {
    /* 转速变成 0 */
    if (s == 0) {
        if (has_source) {
            notify_stress_capacity_change(0);
            get_or_create_network()->update_stress_for(this, calculate_stress_applied());
            return;
        }
        detach_kinetics();
        set_speed(0);
        set_network(false, 0);
        return;
    }
    /* 开始转动：新建网络 */
    if (prev == 0) {
        set_speed(s);
        set_network(true, create_network_id());
        attach_kinetics();
        return;
    }
    /* 被别的动力源带着转 */
    if (has_source) {
        if (fabsf(prev) >= fabsf(s)) {
            if (signum(prev) != signum(s)) space().destroy_block(pos, true);
            return;
        }
        /* 比所连网络快：自己成为新的源头 */
        detach_kinetics();
        set_speed(s);
        has_source = false;
        set_network(true, create_network_id());
        attach_kinetics();
        return;
    }
    /* 重新施加 */
    detach_kinetics();
    set_speed(s);
    attach_kinetics();
}

/* KineticBlockEntity.switchToBlockState */
void switch_to_block_state(Space &sp, BlockPos pos, mcc_state_t st) {
    BlockEntity *be = sp.block_entity(pos);
    mcc_state_t cur = sp.get(pos);
    KineticBlockEntity *kbe = as_kinetic(be);
    if (cur == st) return;
    if (!be || !kbe) {
        sp.set_block(pos, st, UPDATE_ALL);
        return;
    }
    KineticBlock *kb = kinetic_block(st);
    if (kb && !kb->states_kinetically_equivalent(cur, st)) {
        if (kbe->has_network) kbe->get_or_create_network()->remove(kbe);
        kbe->detach_kinetics();
        kbe->remove_source();
    }
    if (GeneratingKineticBlockEntity *g = (GeneratingKineticBlockEntity *)be->interface(IFACE_GENERATING_BE)) {
        g->reactivate_source = true;
        g->wake();
    }
    sp.set_block(pos, st, UPDATE_ALL);
}

} // namespace mcc::create
