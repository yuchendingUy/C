#include "blocks.h"
#include "../../game/pending.h"
#include <math.h>

namespace mcc::create {

KineticBlockIds g_kblocks;

/* ------------------------------------------------------------------ */
/* 方块实体                                                            */
/* ------------------------------------------------------------------ */

/* SimpleKineticBlockEntity：大齿轮向 12 个斜对角邻居传播 */
void SimpleKineticBlockEntity::add_propagation_locations(KineticBlock *block, mcc_state_t st, std::vector<BlockPos> &out) {
    if (!is_large_cog(st)) {
        KineticBlockEntity::add_propagation_locations(block, st, out);
        return;
    }
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (BlockPos{ dx, dy, dz }.dist_sqr() == 2) out.push_back(pos.offset(dx, dy, dz));
}

/* DirectionalShaftHalvesBlockEntity.getSourceFacing */
Dir ShaftHalvesBlockEntity::source_facing() const {
    BlockPos l = source.sub(pos);
    return dir_nearest((float)l.x, (float)l.y, (float)l.z);
}

static bool powered_of(mcc_state_t s) { return ((GearshiftBlock &)state_behaviour(s)).powered.get_bool(s); }

float ClutchBlockEntity::rotation_speed_modifier(Dir face) {
    if (has_source && face != source_facing() && powered_of(state)) return 0;
    return 1;
}

float GearshiftBlockEntity::rotation_speed_modifier(Dir face) {
    if (has_source && face != source_facing() && powered_of(state)) return -1;
    return 1;
}

void ChainGearshiftBlockEntity::neighbour_changed() {
    int power = redstone_best_neighbor_signal(space(), pos); /* TODO(红石) */
    if (power != signal) {
        signal_changed = true;
        wake();
    }
}

bool ChainGearshiftBlockEntity::tick_extra() {
    if (signal_changed) {
        signal_changed = false;
        analog_signal_changed(redstone_best_neighbor_signal(space(), pos)); /* TODO(红石) */
    }
    return signal_changed;
}

void ChainGearshiftBlockEntity::analog_signal_changed(int s) {
    detach_kinetics();
    remove_source();
    signal = s;
    attach_kinetics();
}

void CreativeMotorBlockEntity::initialize() {
    GeneratingKineticBlockEntity::initialize();
    if (!has_source || generated_speed() > theoretical_speed()) update_generated_rotation();
}

float CreativeMotorBlockEntity::generated_speed() {
    if (mcc_state_block(state) != g_kblocks.creative_motor) return 0;
    CreativeMotorBlock &b = (CreativeMotorBlock &)state_behaviour(state);
    return convert_to_direction((float)generated.value, b.facing_of(state));
}

void CreativeMotorBlockEntity::set_generated_speed(int v) {
    int before = generated.value;
    generated.set(v);
    if (generated.value == before) return;
    update_generated_rotation();
    set_changed();
    send_data();
}

void SpeedControllerBlockEntity::set_target_speed(int v) {
    int before = target.value;
    target.set(v);
    if (target.value == before) return;
    update_target_rotation();
    set_changed();
    send_data();
}

void SpeedControllerBlockEntity::update_target_rotation() {
    if (has_network) get_or_create_network()->remove(this);
    rotation_handle_removed(space(), pos, this);
    remove_source();
    attach_kinetics();
}

float SpeedControllerBlockEntity::conveyed_speed(KineticBlockEntity *cog, KineticBlockEntity *controller_in, bool targeting) {
    if (!controller_in->interface(IFACE_SPEED_CONTROLLER_BE)) return 0;
    float speed = controller_in->theoretical_speed();
    float wheel = cog->theoretical_speed();
    float desired = desired_output_speed(cog, controller_in, targeting);
    float compare = targeting ? speed : wheel;
    if (desired >= 0 && compare >= 0) return fmaxf(desired, compare);
    if (desired < 0 && compare < 0) return fminf(desired, compare);
    return desired;
}

float SpeedControllerBlockEntity::desired_output_speed(KineticBlockEntity *cog, KineticBlockEntity *controller_in, bool targeting) {
    SpeedControllerBlockEntity *sc = (SpeedControllerBlockEntity *)controller_in->interface(IFACE_SPEED_CONTROLLER_BE);
    float target_speed = (float)sc->target.value;
    float speed = controller_in->theoretical_speed();
    float wheel = cog->theoretical_speed();
    if (target_speed == 0) return 0;
    if (targeting && wheel == 0) return 0;
    if (!sc->has_source) return targeting ? target_speed : 0;
    bool wheel_powers_controller = sc->source == cog->pos;
    if (wheel_powers_controller) return targeting ? target_speed : wheel;
    return targeting ? speed : target_speed;
}

/* ------------------------------------------------------------------ */
/* 方块                                                                */
/* ------------------------------------------------------------------ */

ShaftBlock::ShaftBlock(mcc_block_t b, bool cog, bool large) : PillarKineticBlock(b), is_cog(cog), is_large(large) {
    waterlogged.bind(b, "waterlogged");
}

/* AbstractShaftBlock.updateShape → ProperWaterloggedBlock.updateWater */
mcc_state_t ShaftBlock::update_shape(Space &sp, BlockPos pos, mcc_state_t state, Dir dir, mcc_state_t neighbor, BlockPos npos) {
    (void)dir; (void)neighbor; (void)npos;
    if (waterlogged.get_bool(state)) fluid_schedule_tick(sp, pos); /* TODO(流体) */
    return state;
}

EncasedCogwheelBlock::EncasedCogwheelBlock(mcc_block_t b, bool large) : PillarKineticBlock(b), is_large(large) {
    top_shaft.bind(b, "top_shaft");
    bottom_shaft.bind(b, "bottom_shaft");
}

bool EncasedCogwheelBlock::has_shaft_towards(Space &, BlockPos, mcc_state_t s, Dir face) {
    return dir_axis(face) == rotation_axis(s) && (dir_positive(face) ? top_shaft : bottom_shaft).get_bool(s);
}

bool EncasedCogwheelBlock::states_kinetically_equivalent(mcc_state_t old, mcc_state_t now) {
    /* 原版：新旧都是机壳齿轮时比较两端的轴；种类不同时父类本来就返回 false */
    if (mcc_state_block(old) == mcc_state_block(now)) {
        if (top_shaft.get_bool(now) != top_shaft.get_bool(old)) return false;
        if (bottom_shaft.get_bool(now) != bottom_shaft.get_bool(old)) return false;
    }
    return PillarKineticBlock::states_kinetically_equivalent(old, now);
}

GearshiftBlock::GearshiftBlock(mcc_block_t b, bool is_clutch) : PillarKineticBlock(b), clutch(is_clutch) {
    powered.bind(b, "powered");
}

void GearshiftBlock::detach(Space &sp, BlockPos pos, bool reattach) {
    KineticBlockEntity *be = as_kinetic(sp.block_entity(pos));
    if (!be) return;
    rotation_handle_removed(sp, pos, be);
    if (reattach) sp.schedule_tick(pos, id, 1, PRIORITY_EXTREMELY_HIGH);
}

void GearshiftBlock::neighbor_changed(Space &sp, BlockPos pos, mcc_state_t state, mcc_block_t, BlockPos, bool) {
    bool was = powered.get_bool(state);
    bool now = redstone_has_neighbor_signal(sp, pos); /* TODO(红石) */
    if (was == now) return;
    if (clutch) {
        sp.set_block(pos, powered.cycle(state), UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE);
        detach(sp, pos, was);
    } else {
        detach(sp, pos, true);
        sp.set_block(pos, powered.cycle(state), UPDATE_CLIENTS);
    }
}

void GearshiftBlock::tick(Space &sp, BlockPos pos, mcc_state_t state) {
    (void)state;
    KineticBlockEntity *be = as_kinetic(sp.block_entity(pos));
    if (!be) return;
    rotation_handle_added(sp, pos, be);
}

ChainDriveBlock::ChainDriveBlock(mcc_block_t b, bool is_gearshift) : PillarKineticBlock(b), gearshift(is_gearshift) {
    part.bind(b, "part");
    along_first.bind(b, "axis_along_first");
    if (gearshift) powered.bind(b, "powered");
}

static Axis connection_axis_of(Axis axis, bool along_first) {
    return along_first ? (axis == AXIS_X ? AXIS_Y : AXIS_X) : (axis == AXIS_Z ? AXIS_Y : AXIS_Z);
}

Axis ChainDriveBlock::connection_axis(mcc_state_t s) { return connection_axis_of(rotation_axis(s), along_first.get_bool(s)); }

mcc_state_t ChainDriveBlock::update_shape(Space &sp, BlockPos pos, mcc_state_t state, Dir face, mcc_state_t neighbour, BlockPos npos) {
    (void)sp; (void)pos; (void)npos;
    int p = part.get(state);
    Axis axis = rotation_axis(state);
    bool conn_first = along_first.get_bool(state);
    Axis conn_axis = connection_axis_of(axis, conn_first);
    Axis face_axis = dir_axis(face);
    bool facing_first = axis == AXIS_X ? face_axis == AXIS_Y : face_axis == AXIS_X;
    bool positive = dir_positive(face);
    if (axis == face_axis) return state;

    ChainDriveBlock *other = chain_drive_block(neighbour);
    if (!other) {
        if (facing_first != conn_first || p == PART_NONE) return state;
        if (p == PART_MIDDLE) return part.with(state, positive ? PART_END : PART_START);
        if ((p == PART_START) == positive) return part.with(state, PART_NONE);
        return state;
    }
    int other_part = other->part.get(neighbour);
    Axis other_axis = other->rotation_axis(neighbour);
    Axis other_conn = connection_axis_of(other_axis, other->along_first.get_bool(neighbour));
    if (other_axis == face_axis) return state;
    if (other_part != PART_NONE && other_conn != face_axis) return state;
    if (p == PART_NONE) {
        p = positive ? PART_START : PART_END;
        conn_first = axis == AXIS_X ? face_axis == AXIS_Y : face_axis == AXIS_X;
    } else if (conn_axis != face_axis) {
        return state;
    }
    if ((p == PART_START) != positive) p = PART_MIDDLE;
    return along_first.with_bool(part.with(state, p), conn_first);
}

bool ChainDriveBlock::states_kinetically_equivalent(mcc_state_t old, mcc_state_t now) {
    bool eq = PillarKineticBlock::states_kinetically_equivalent(old, now);
    if (!gearshift) return eq;
    return eq && powered.get_bool(old) == powered.get_bool(now);
}

void ChainDriveBlock::on_place(Space &sp, BlockPos pos, mcc_state_t state, mcc_state_t old, bool moving) {
    PillarKineticBlock::on_place(sp, pos, state, old, moving);
    if (!gearshift || mcc_state_block(old) == mcc_state_block(state)) return;
    if (BlockEntity *be = sp.block_entity(pos))
        if (auto *cg = (ChainGearshiftBlockEntity *)be->interface(IFACE_CHAIN_GEARSHIFT_BE)) cg->neighbour_changed();
}

void ChainDriveBlock::neighbor_changed(Space &sp, BlockPos pos, mcc_state_t state, mcc_block_t, BlockPos, bool) {
    if (!gearshift) return;
    if (BlockEntity *be = sp.block_entity(pos))
        if (auto *cg = (ChainGearshiftBlockEntity *)be->interface(IFACE_CHAIN_GEARSHIFT_BE)) cg->neighbour_changed();
    bool was = powered.get_bool(state);
    if (was != redstone_has_neighbor_signal(sp, pos)) /* TODO(红石) */
        sp.set_block(pos, powered.cycle(state), UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE);
}

bool ChainDriveBlock::blocks_connected(mcc_state_t state, mcc_state_t other, Dir facing) {
    ChainDriveBlock *a = chain_drive_block(state), *b = chain_drive_block(other);
    int p = a->part.get(state);
    Axis ca = a->connection_axis(state), cb = b->connection_axis(other);
    if (cb != ca) return false;
    if (dir_axis(facing) != ca) return false;
    if (dir_positive(facing) && (p == PART_MIDDLE || p == PART_START)) return true;
    if (!dir_positive(facing) && (p == PART_MIDDLE || p == PART_END)) return true;
    return false;
}

float ChainDriveBlock::rotation_speed_modifier(KineticBlockEntity *from, KineticBlockEntity *to) {
    float fm = 1, tm = 1;
    if (auto *g = (ChainGearshiftBlockEntity *)from->interface(IFACE_CHAIN_GEARSHIFT_BE)) fm = g->modifier();
    if (auto *g = (ChainGearshiftBlockEntity *)to->interface(IFACE_CHAIN_GEARSHIFT_BE)) tm = g->modifier();
    return fm / tm;
}

/* FACING 取值顺序：north east south west up down */
Dir CreativeMotorBlock::facing_of(mcc_state_t s) {
    static const Dir map[6] = { NORTH, EAST, SOUTH, WEST, UP, DOWN };
    return map[facing.get(s)];
}

/* ------------------------------------------------------------------ */
/* 注册                                                                */
/* ------------------------------------------------------------------ */

void kinetic_blocks_register() {
    auto reg = [](const char *name, auto make) {
        mcc_block_t b = block_id(name);
        block_register(b, make(b));
        return b;
    };
    g_kblocks.shaft = reg("create:shaft", [](mcc_block_t b) { return new ShaftBlock(b, false, false); });
    g_kblocks.cogwheel = reg("create:cogwheel", [](mcc_block_t b) { return new ShaftBlock(b, true, false); });
    g_kblocks.large_cogwheel = reg("create:large_cogwheel", [](mcc_block_t b) { return new ShaftBlock(b, true, true); });
    for (const char *n : { "create:andesite_encased_shaft", "create:brass_encased_shaft" })
        reg(n, [](mcc_block_t b) { return new EncasedShaftBlock(b); });
    for (const char *n : { "create:andesite_encased_cogwheel", "create:brass_encased_cogwheel" })
        reg(n, [](mcc_block_t b) { return new EncasedCogwheelBlock(b, false); });
    for (const char *n : { "create:andesite_encased_large_cogwheel", "create:brass_encased_large_cogwheel" })
        reg(n, [](mcc_block_t b) { return new EncasedCogwheelBlock(b, true); });
    reg("create:gearbox", [](mcc_block_t b) { return new GearboxBlock(b); });
    reg("create:clutch", [](mcc_block_t b) { return new GearshiftBlock(b, true); });
    reg("create:gearshift", [](mcc_block_t b) { return new GearshiftBlock(b, false); });
    reg("create:encased_chain_drive", [](mcc_block_t b) { return new ChainDriveBlock(b, false); });
    reg("create:adjustable_chain_gearshift", [](mcc_block_t b) { return new ChainDriveBlock(b, true); });
    g_kblocks.creative_motor = reg("create:creative_motor", [](mcc_block_t b) { return new CreativeMotorBlock(b); });
    g_kblocks.rotation_speed_controller =
        reg("create:rotation_speed_controller", [](mcc_block_t b) { return new SpeedControllerBlock(b); });
}

} // namespace mcc::create
