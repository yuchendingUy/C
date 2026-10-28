/*
 * 转速传播：照 content/kinetics/RotationPropagator 逐条移植。
 * 原版 propagateNewSource 是递归的，这里用显式栈，进出顺序与递归一致：
 *   - 递归调用后 continue → 压入新帧，本帧之后接着处理下一个邻居
 *   - 递归调用后 return  → 弹出本帧，再压入新帧
 *   - destroyBlock 后 return → 弹出本帧再破坏方块（破坏会触发移除传播，那是独立的一次调用）
 * 邻居列表在进入一帧时算好，与原版在函数开头算好一致。
 */
#include "blocks.h"
#include <math.h>

namespace mcc::create {

static constexpr int MAX_FLICKER_SCORE = 128;

static bool is_speed_controller(mcc_state_t s) { return mcc_state_block(s) == g_kblocks.rotation_speed_controller; }

/* 原版 Axis.choose(diff) != 0 之外的两个轴都为 0 */
static bool aligned_axes(BlockPos diff, Dir dir) {
    for (Axis a : { AXIS_X, AXIS_Y, AXIS_Z })
        if (a != dir_axis(dir) && diff.along(a) != 0) return false;
    return true;
}

static float axis_modifier(KineticBlockEntity *be, Dir dir) {
    if (!(be->has_source || be->is_source()) || !be->interface(IFACE_SHAFT_HALVES_BE)) return 1;
    Dir source = ((ShaftHalvesBlockEntity *)be->interface(IFACE_SHAFT_HALVES_BE))->source_facing();
    if (be->interface(IFACE_GEARBOX_BE))
        return dir_axis(dir) == dir_axis(source) ? (dir == source ? 1 : -1)
                                                 : (dir_positive(dir) == dir_positive(source) ? -1 : 1);
    if (SplitShaftBlockEntity *ss = (SplitShaftBlockEntity *)be->interface(IFACE_SPLIT_SHAFT_BE))
        return ss->rotation_speed_modifier(dir);
    return 1;
}

static bool large_to_large_gear(mcc_state_t from, mcc_state_t to, BlockPos diff) {
    if (!is_large_cog(from) || !is_large_cog(to)) return false;
    Axis fa = kinetic_block(from)->rotation_axis(from), ta = kinetic_block(to)->rotation_axis(to);
    if (fa == ta) return false;
    for (Axis a : { AXIS_X, AXIS_Y, AXIS_Z }) {
        int d = diff.along(a);
        if (a == fa || a == ta) {
            if (d == 0) return false;
        } else if (d != 0) {
            return false;
        }
    }
    return true;
}

static bool large_to_small_cog(mcc_state_t from, mcc_state_t to, KineticBlock *def_to, BlockPos diff) {
    Axis af = kinetic_block(from)->rotation_axis(from);
    if (af != def_to->rotation_axis(to)) return false;
    if (diff.along(af) != 0) return false;
    for (Axis a : { AXIS_X, AXIS_Y, AXIS_Z }) {
        if (a == af) continue;
        if (std::abs(diff.along(a)) != 1) return false;
    }
    return true;
}

static bool large_cog_to_speed_controller(mcc_state_t from, mcc_state_t to, BlockPos diff) {
    if (!is_large_cog(from) || !is_speed_controller(to)) return false;
    if (diff != BlockPos{ 0, -1, 0 }) return false;
    Axis axis = kinetic_block(from)->rotation_axis(from);
    if (axis == AXIS_Y) return false;
    if (kinetic_block(to)->rotation_axis(to) == axis) return false;
    return true;
}

/* 两个相连部件之间的转速比：轴连轴 1，1:1 齿轮反向 -1 …… 不相连为 0 */
static float rotation_speed_modifier(KineticBlockEntity *from, KineticBlockEntity *to) {
    mcc_state_t sf = from->state, st = to->state;
    KineticBlock *df = kinetic_block(sf), *dt = kinetic_block(st);
    if (!df || !dt) return 0;
    BlockPos diff = to->pos.sub(from->pos);
    Dir dir = dir_nearest((float)diff.x, (float)diff.y, (float)diff.z);
    Space &sp = from->space();

    bool connected_by_axis = aligned_axes(diff, dir) && df->has_shaft_towards(sp, from->pos, sf, dir) &&
                             dt->has_shaft_towards(sp, to->pos, st, dir_opposite(dir));
    bool connected_by_gears = is_small_cog(sf) && is_small_cog(st);

    float custom = from->propagate_rotation_to(to, sf, st, diff, connected_by_axis, connected_by_gears);
    if (custom != 0) return custom;

    /* 轴 ↔ 轴 */
    if (connected_by_axis) {
        float m = axis_modifier(to, dir_opposite(dir));
        if (m != 0) m = 1 / m;
        return axis_modifier(from, dir) * m;
    }
    /* 链式传动 ↔ 链式传动 */
    if (chain_drive_block(sf) && chain_drive_block(st)) {
        bool connected = ChainDriveBlock::blocks_connected(sf, st, dir);
        return connected ? ChainDriveBlock::rotation_speed_modifier(from, to) : 0;
    }
    /* 大齿轮 ↔ 大齿轮 */
    if (large_to_large_gear(sf, st, diff)) {
        Axis sa = df->rotation_axis(sf), ta = dt->rotation_axis(st);
        int sd = diff.along(sa), td = diff.along(ta);
        return (sd > 0) ^ (td > 0) ? -1 : 1;
    }
    /* 齿轮 ↔ 大齿轮 */
    if (is_large_cog(sf) && is_small_cog(st))
        if (large_to_small_cog(sf, st, dt, diff)) return -2.f;
    if (is_large_cog(st) && is_small_cog(sf))
        if (large_to_small_cog(st, sf, df, diff)) return -.5f;
    /* 齿轮 ↔ 齿轮 */
    if (connected_by_gears) {
        if (diff.manhattan() != 1) return 0;
        if (is_large_cog(st)) return 0;
        if (dir_axis(dir) == df->rotation_axis(sf)) return 0;
        if (df->rotation_axis(sf) == dt->rotation_axis(st)) return -1;
    }
    return 0;
}

static float conveyed_speed(KineticBlockEntity *from, KineticBlockEntity *to) {
    /* 调速器 ↔ 大齿轮 */
    if (large_cog_to_speed_controller(from->state, to->state, to->pos.sub(from->pos)))
        return SpeedControllerBlockEntity::conveyed_speed(from, to, true);
    if (large_cog_to_speed_controller(to->state, from->state, from->pos.sub(to->pos)))
        return SpeedControllerBlockEntity::conveyed_speed(to, from, false);
    return from->theoretical_speed() * rotation_speed_modifier(from, to);
}

bool rotation_is_connected(KineticBlockEntity *from, KineticBlockEntity *to) {
    return large_cog_to_speed_controller(from->state, to->state, to->pos.sub(from->pos)) ||
           rotation_speed_modifier(from, to) != 0 || from->is_custom_connection(to, from->state, to->state);
}

static void potential_neighbour_locations(KineticBlockEntity *be, std::vector<BlockPos> &out) {
    out.clear();
    Space &sp = be->space();
    if (!sp.is_loaded(be->pos)) return;
    for (Dir d : ALL_DIRS) {
        BlockPos r = be->pos.offset(d);
        if (sp.is_loaded(r)) out.push_back(r);
    }
    KineticBlock *block = kinetic_block(be->state);
    if (!block) return;
    be->add_propagation_locations(block, be->state, out);
}

static KineticBlockEntity *find_connected_neighbour(KineticBlockEntity *cur, BlockPos np) {
    Space &sp = cur->space();
    mcc_state_t ns = sp.get(np);
    if (!kinetic_block(ns)) return nullptr;
    if (!state_has_block_entity(ns)) return nullptr;
    KineticBlockEntity *nb = as_kinetic(sp.block_entity(np));
    if (!nb) return nullptr;
    if (!kinetic_block(nb->state)) return nullptr;
    if (!rotation_is_connected(cur, nb) && !rotation_is_connected(nb, cur)) return nullptr;
    return nb;
}

static void connected_neighbours(KineticBlockEntity *be, std::vector<KineticBlockEntity *> &out) {
    out.clear();
    std::vector<BlockPos> locs;
    potential_neighbour_locations(be, locs);
    for (BlockPos p : locs)
        if (KineticBlockEntity *n = find_connected_neighbour(be, p)) out.push_back(n);
}

/* ---- propagateNewSource ---- */

struct Frame {
    KineticBlockEntity *be;
    std::vector<KineticBlockEntity *> nbrs;
    size_t i;
};

static void push_frame(std::vector<Frame> &st, KineticBlockEntity *be) {
    st.push_back({ be, {}, 0 });
    connected_neighbours(be, st.back().nbrs);
}

static void propagate_new_source(KineticBlockEntity *start) {
    std::vector<Frame> stack;
    push_frame(stack, start);
    while (!stack.empty()) {
        Frame &f = stack.back();
        if (f.i >= f.nbrs.size()) {
            stack.pop_back();
            continue;
        }
        KineticBlockEntity *cur = f.be;
        KineticBlockEntity *nb = f.nbrs[f.i++];
        BlockPos pos = cur->pos;
        Space &world = cur->space();

        float speed_of_current = cur->theoretical_speed();
        float speed_of_neighbour = nb->theoretical_speed();
        float new_speed = conveyed_speed(cur, nb);
        float opposite_speed = conveyed_speed(nb, cur);

        if (new_speed == 0 && opposite_speed == 0) continue;

        bool incompatible = ((new_speed > 0) - (new_speed < 0)) != ((speed_of_neighbour > 0) - (speed_of_neighbour < 0)) &&
                            (new_speed != 0 && speed_of_neighbour != 0);
        float max_speed = (float)g_kinetics.max_rotation_speed;
        bool too_fast = fabsf(new_speed) > max_speed || fabsf(opposite_speed) > max_speed;
        bool flickering = cur->flicker_score() > MAX_FLICKER_SCORE;
        if (too_fast || flickering || incompatible) {
            stack.pop_back();
            world.destroy_block(pos, true);
            continue;
        }

        /* 同向：快的一方覆盖慢的一方 */
        /* 邻居更快：反过来带动当前这一支 */
        if (fabsf(opposite_speed) > fabsf(speed_of_current)) {
            float prev = cur->get_speed();
            cur->set_source(nb->pos);
            cur->set_speed(conveyed_speed(nb, cur));
            cur->on_speed_changed(prev);
            cur->send_data();
            stack.pop_back();
            push_frame(stack, cur);
            continue;
        }
        /* 当前更快：覆盖邻居那一支 */
        if (fabsf(new_speed) >= fabsf(speed_of_neighbour)) {
            /* 不能覆盖自己所在的网络（成环） */
            if (!cur->has_network || (nb->has_network && cur->network == nb->network)) {
                float epsilon = fabsf(speed_of_neighbour) / 256.f / 256.f;
                /* 原版这里破坏方块后是 continue 不是 return：已移除的方块实体继续处理剩下的邻居 */
                if (fabsf(new_speed) > fabsf(speed_of_neighbour) + epsilon) world.destroy_block(pos, true);
                continue;
            }
            if (cur->has_source && cur->source == nb->pos) cur->remove_source();
            float prev = nb->get_speed();
            nb->set_source(cur->pos);
            nb->set_speed(conveyed_speed(cur, nb));
            nb->on_speed_changed(prev);
            nb->send_data();
            push_frame(stack, nb);
            continue;
        }

        if (fabsf(nb->theoretical_speed() - new_speed) <= 1e-4f) continue;

        float prev = nb->get_speed();
        nb->set_speed(new_speed);
        nb->set_source(cur->pos);
        nb->on_speed_changed(prev);
        nb->send_data();
        push_frame(stack, nb);
    }
}

void rotation_handle_added(Space &sp, BlockPos pos, KineticBlockEntity *added) {
    if (!sp.is_loaded(pos)) return;
    propagate_new_source(added);
}

/* ---- propagateMissingSource：清空依赖它的整片子网，再找新的源头 ---- */
static void propagate_missing_source(KineticBlockEntity *update) {
    Space &world = update->space();
    std::vector<KineticBlockEntity *> potential_new_sources;
    std::vector<BlockPos> frontier;
    size_t head = 0;
    frontier.push_back(update->pos);
    bool has_missing = update->has_source;
    BlockPos missing = update->source;
    std::vector<KineticBlockEntity *> nbrs;

    while (head < frontier.size()) {
        BlockPos pos = frontier[head++];
        KineticBlockEntity *cur = as_kinetic(world.block_entity(pos));
        if (!cur) continue;
        cur->remove_source();
        cur->send_data();
        connected_neighbours(cur, nbrs);
        for (KineticBlockEntity *nb : nbrs) {
            if (has_missing && nb->pos == missing) continue;
            if (!nb->has_source) continue;
            if (nb->source != pos) {
                potential_new_sources.push_back(nb);
                continue;
            }
            if (nb->is_source()) potential_new_sources.push_back(nb);
            frontier.push_back(nb->pos);
        }
    }
    for (KineticBlockEntity *ns : potential_new_sources) {
        if (ns->has_source || ns->is_source()) {
            propagate_new_source(ns);
            return;
        }
    }
}

void rotation_handle_removed(Space &sp, BlockPos pos, KineticBlockEntity *removed) {
    if (!removed) return;
    if (removed->theoretical_speed() == 0) return;
    std::vector<BlockPos> locs;
    potential_neighbour_locations(removed, locs);
    for (BlockPos np : locs) {
        mcc_state_t ns = sp.get(np);
        if (!kinetic_block(ns)) continue;
        KineticBlockEntity *nb = as_kinetic(sp.block_entity(np));
        if (!nb) continue;
        if (!nb->has_source || nb->source != pos) continue;
        propagate_missing_source(nb);
    }
}

} // namespace mcc::create
