/*
 * 机械动力：动力传递方块（content/kinetics 下的 simpleRelays、gearbox、transmission、
 * chainDrive、motor、speedController）
 *
 * 传播规则里要按具体类型判断的部分（齿轮箱、离合器/换向器、链式传动、调速器）
 * 通过接口编号查询，见 kinetic.h。
 */
#ifndef MCC_CREATE_KINETIC_BLOCKS_H
#define MCC_CREATE_KINETIC_BLOCKS_H

#include "kinetic.h"

namespace mcc::create {

/* ---------------- 方块实体 ---------------- */

/* SimpleKineticBlockEntity（含 BracketedKineticBlockEntity：支架是装饰，TODO(玩家交互)） */
struct SimpleKineticBlockEntity : KineticBlockEntity {
    using KineticBlockEntity::KineticBlockEntity;
    void add_propagation_locations(KineticBlock *block, mcc_state_t st, std::vector<BlockPos> &out) override;
};

/* DirectionalShaftHalvesBlockEntity */
struct ShaftHalvesBlockEntity : KineticBlockEntity {
    using KineticBlockEntity::KineticBlockEntity;
    Dir source_facing() const;
    void *interface(int iid) override {
        return iid == IFACE_SHAFT_HALVES_BE ? this : KineticBlockEntity::interface(iid);
    }
};

/* GearboxBlockEntity */
struct GearboxBlockEntity : ShaftHalvesBlockEntity {
    using ShaftHalvesBlockEntity::ShaftHalvesBlockEntity;
    void *interface(int iid) override {
        return iid == IFACE_GEARBOX_BE ? this : ShaftHalvesBlockEntity::interface(iid);
    }
};

/* SplitShaftBlockEntity：离合器、换向器 */
struct SplitShaftBlockEntity : ShaftHalvesBlockEntity {
    using ShaftHalvesBlockEntity::ShaftHalvesBlockEntity;
    virtual float rotation_speed_modifier(Dir face) = 0;
    void *interface(int iid) override {
        return iid == IFACE_SPLIT_SHAFT_BE ? this : ShaftHalvesBlockEntity::interface(iid);
    }
};

struct ClutchBlockEntity : SplitShaftBlockEntity {
    using SplitShaftBlockEntity::SplitShaftBlockEntity;
    float rotation_speed_modifier(Dir face) override;
};

struct GearshiftBlockEntity : SplitShaftBlockEntity {
    using SplitShaftBlockEntity::SplitShaftBlockEntity;
    float rotation_speed_modifier(Dir face) override;
};

/* ChainGearshiftBlockEntity（可调链式变速箱） */
struct ChainGearshiftBlockEntity : KineticBlockEntity {
    int signal = 0;
    bool signal_changed = false;
    ChainGearshiftBlockEntity(BlockPos p, mcc_state_t s) : KineticBlockEntity(p, s) { set_lazy_tick_rate(40); }
    float modifier() const { return modifier_for_signal(signal); }
    static float modifier_for_signal(int power) { return power == 0 ? 1 : 1 + ((power + 1) / 16.f); }
    void neighbour_changed();
    void lazy_tick() override { neighbour_changed(); }
    bool tick_extra() override;
    void analog_signal_changed(int s);
    void *interface(int iid) override {
        return iid == IFACE_CHAIN_GEARSHIFT_BE ? this : KineticBlockEntity::interface(iid);
    }
};

/* ScrollValueBehaviour 的服务端部分：一个有上下限的整数，改了就回调。
 * 玩家用扳手面板调值是 TODO(玩家交互)，现在由代码直接 set */
struct ScrollValue {
    int value = 0, min = 0, max = 0;
    void set(int v) { value = v < min ? min : v > max ? max : v; }
};

/* CreativeMotorBlockEntity */
struct CreativeMotorBlockEntity : GeneratingKineticBlockEntity {
    static constexpr int DEFAULT_SPEED = 16, MAX_SPEED = 256;
    ScrollValue generated{ DEFAULT_SPEED, -MAX_SPEED, MAX_SPEED };
    using GeneratingKineticBlockEntity::GeneratingKineticBlockEntity;
    void initialize() override;
    float generated_speed() override;
    /* 原版 ScrollValueBehaviour.setValue → withCallback(updateGeneratedRotation) */
    void set_generated_speed(int v);
};

/* SpeedControllerBlockEntity（转速控制器） */
struct SpeedControllerBlockEntity : KineticBlockEntity {
    static constexpr int DEFAULT_SPEED = 16;
    ScrollValue target{ DEFAULT_SPEED, -256, 256 };
    SpeedControllerBlockEntity(BlockPos p, mcc_state_t s) : KineticBlockEntity(p, s) {
        target.min = -g_kinetics.max_rotation_speed;
        target.max = g_kinetics.max_rotation_speed;
    }
    void set_target_speed(int v);
    void update_target_rotation();
    static float conveyed_speed(KineticBlockEntity *cog, KineticBlockEntity *controller, bool targeting_controller);
    static float desired_output_speed(KineticBlockEntity *cog, KineticBlockEntity *controller, bool targeting_controller);
    void *interface(int iid) override {
        return iid == IFACE_SPEED_CONTROLLER_BE ? this : KineticBlockEntity::interface(iid);
    }
};

/* ---------------- 方块 ---------------- */

/* 一根轴的方块（RotatedPillarKineticBlock：AXIS 属性） */
struct PillarKineticBlock : KineticBlock {
    Prop axis;
    explicit PillarKineticBlock(mcc_block_t b) : KineticBlock(b) { axis.bind(b, "axis"); }
    Axis rotation_axis(mcc_state_t s) override { return (Axis)axis.get(s); }
};

/* AbstractShaftBlock：传动杆、齿轮、大齿轮（AXIS + WATERLOGGED） */
struct ShaftBlock : PillarKineticBlock {
    bool is_cog, is_large;
    Prop waterlogged;
    ShaftBlock(mcc_block_t b, bool cog, bool large);
    bool has_shaft_towards(Space &, BlockPos, mcc_state_t s, Dir face) override { return dir_axis(face) == rotation_axis(s); }
    bool cogwheel() override { return is_cog; }
    bool large_cog() override { return is_large; }
    bool dedicated_cogwheel() override { return is_cog; }
    mcc_state_t update_shape(Space &sp, BlockPos pos, mcc_state_t state, Dir dir, mcc_state_t neighbor, BlockPos npos) override;
    BlockEntity *new_block_entity(BlockPos p, mcc_state_t s) override { return new SimpleKineticBlockEntity(p, s); }
};

/* EncasedShaftBlock：安山岩 / 黄铜机壳传动杆 */
struct EncasedShaftBlock : PillarKineticBlock {
    using PillarKineticBlock::PillarKineticBlock;
    bool has_shaft_towards(Space &, BlockPos, mcc_state_t s, Dir face) override { return dir_axis(face) == rotation_axis(s); }
    BlockEntity *new_block_entity(BlockPos p, mcc_state_t s) override { return new KineticBlockEntity(p, s); }
};

/* EncasedCogwheelBlock：机壳齿轮（AXIS + TOP_SHAFT + BOTTOM_SHAFT） */
struct EncasedCogwheelBlock : PillarKineticBlock {
    bool is_large;
    Prop top_shaft, bottom_shaft;
    EncasedCogwheelBlock(mcc_block_t b, bool large);
    bool has_shaft_towards(Space &, BlockPos, mcc_state_t s, Dir face) override;
    bool states_kinetically_equivalent(mcc_state_t old, mcc_state_t now) override;
    bool cogwheel() override { return true; }
    bool large_cog() override { return is_large; }
    BlockEntity *new_block_entity(BlockPos p, mcc_state_t s) override { return new SimpleKineticBlockEntity(p, s); }
};

/* GearboxBlock */
struct GearboxBlock : PillarKineticBlock {
    using PillarKineticBlock::PillarKineticBlock;
    bool has_shaft_towards(Space &, BlockPos, mcc_state_t s, Dir face) override { return dir_axis(face) != rotation_axis(s); }
    BlockEntity *new_block_entity(BlockPos p, mcc_state_t s) override { return new GearboxBlockEntity(p, s); }
};

/* GearshiftBlock / ClutchBlock（AbstractEncasedShaftBlock + POWERED） */
struct GearshiftBlock : PillarKineticBlock {
    bool clutch;
    Prop powered;
    GearshiftBlock(mcc_block_t b, bool is_clutch);
    bool has_shaft_towards(Space &, BlockPos, mcc_state_t s, Dir face) override { return dir_axis(face) == rotation_axis(s); }
    void neighbor_changed(Space &sp, BlockPos pos, mcc_state_t state, mcc_block_t from_block, BlockPos from, bool moving) override;
    void tick(Space &sp, BlockPos pos, mcc_state_t state) override;
    void detach(Space &sp, BlockPos pos, bool reattach_next_tick);
    BlockEntity *new_block_entity(BlockPos p, mcc_state_t s) override {
        if (clutch) return new ClutchBlockEntity(p, s);
        return new GearshiftBlockEntity(p, s);
    }
};

/* ChainDriveBlock / ChainGearshiftBlock（AXIS + PART + AXIS_ALONG_FIRST_COORDINATE [+ POWERED]） */
enum ChainPart { PART_START, PART_MIDDLE, PART_END, PART_NONE };
struct ChainDriveBlock : PillarKineticBlock {
    bool gearshift;
    Prop part, along_first, powered;
    ChainDriveBlock(mcc_block_t b, bool is_gearshift);
    bool has_shaft_towards(Space &, BlockPos, mcc_state_t s, Dir face) override { return dir_axis(face) == rotation_axis(s); }
    mcc_state_t update_shape(Space &sp, BlockPos pos, mcc_state_t state, Dir dir, mcc_state_t neighbor, BlockPos npos) override;
    bool states_kinetically_equivalent(mcc_state_t old, mcc_state_t now) override;
    void on_place(Space &sp, BlockPos pos, mcc_state_t state, mcc_state_t old, bool moving) override;
    void neighbor_changed(Space &sp, BlockPos pos, mcc_state_t state, mcc_block_t from_block, BlockPos from, bool moving) override;
    Axis connection_axis(mcc_state_t s);
    static bool blocks_connected(mcc_state_t state, mcc_state_t other, Dir facing);
    static float rotation_speed_modifier(KineticBlockEntity *from, KineticBlockEntity *to);
    void *interface(int iid) override { return iid == IFACE_CHAIN_DRIVE_BLOCK ? this : PillarKineticBlock::interface(iid); }
    BlockEntity *new_block_entity(BlockPos p, mcc_state_t s) override {
        if (gearshift) return new ChainGearshiftBlockEntity(p, s);
        return new KineticBlockEntity(p, s);
    }
};
MCC_INLINE ChainDriveBlock *chain_drive_block(mcc_state_t s) {
    return (ChainDriveBlock *)state_behaviour(s).interface(IFACE_CHAIN_DRIVE_BLOCK);
}

/* CreativeMotorBlock（DirectionalKineticBlock：FACING） */
struct CreativeMotorBlock : KineticBlock {
    Prop facing;
    explicit CreativeMotorBlock(mcc_block_t b) : KineticBlock(b) { facing.bind(b, "facing"); }
    Dir facing_of(mcc_state_t s);
    bool has_shaft_towards(Space &, BlockPos, mcc_state_t s, Dir face) override { return face == facing_of(s); }
    Axis rotation_axis(mcc_state_t s) override { return dir_axis(facing_of(s)); }
    BlockEntity *new_block_entity(BlockPos p, mcc_state_t s) override { return new CreativeMotorBlockEntity(p, s); }
};

/* SpeedControllerBlock（HorizontalAxisKineticBlock：HORIZONTAL_AXIS） */
struct SpeedControllerBlock : KineticBlock {
    Prop haxis;
    explicit SpeedControllerBlock(mcc_block_t b) : KineticBlock(b) { haxis.bind(b, "axis"); }
    Axis rotation_axis(mcc_state_t s) override { return haxis.get(s) == 0 ? AXIS_X : AXIS_Z; }
    bool has_shaft_towards(Space &, BlockPos, mcc_state_t s, Dir face) override { return dir_axis(face) == rotation_axis(s); }
    BlockEntity *new_block_entity(BlockPos p, mcc_state_t s) override { return new SpeedControllerBlockEntity(p, s); }
};

/* 方块编号（create_init 之后有效） */
struct KineticBlockIds {
    mcc_block_t shaft, cogwheel, large_cogwheel, creative_motor, rotation_speed_controller;
};
extern KineticBlockIds g_kblocks;

void kinetic_blocks_register();

} // namespace mcc::create

#endif
