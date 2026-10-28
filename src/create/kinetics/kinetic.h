/*
 * 机械动力：动力（转速传递与应力）核心
 *
 * 对应源码 content/kinetics：
 *   IRotate / KineticBlock / ICogWheel        → KineticBlock
 *   KineticBlockEntity / GeneratingKinetic…   → KineticBlockEntity / GeneratingKineticBlockEntity
 *   KineticNetwork / TorquePropagator          → KineticNetwork / 每空间一张网络表
 *   RotationPropagator                         → rotation_* 函数
 *
 * 规则与原版逐条一致；内部做法的不同：
 *   - 方块实体平时不推进，只在有事时（刚放下、转速待更新、闪烁计数、网络待重算）
 *     或到了原版倒计时的时间点（校验、慢推进）才推进
 *   - 转速传播原版是递归，长传动线会爆栈；这里改成显式栈，访问顺序与递归完全相同
 *   - 应力网络的总和只在有变化时才重算（原版每次都把全网遍历一遍）
 *   - 网络表每个方块空间一张（原版每个维度一张静态表，不能多线程）
 */
#ifndef MCC_CREATE_KINETIC_H
#define MCC_CREATE_KINETIC_H

#include "../../game/space.h"
#include "../config.h"
#include <vector>

namespace mcc::create {

/* 接口编号（代替 instanceof） */
enum : int {
    IFACE_KINETIC_BLOCK = 0x43520001,  /* IRotate / KineticBlock */
    IFACE_KINETIC_BE,                  /* KineticBlockEntity */
    IFACE_SHAFT_HALVES_BE,             /* DirectionalShaftHalvesBlockEntity */
    IFACE_GEARBOX_BE,                  /* GearboxBlockEntity */
    IFACE_SPLIT_SHAFT_BE,              /* SplitShaftBlockEntity */
    IFACE_CHAIN_GEARSHIFT_BE,          /* ChainGearshiftBlockEntity */
    IFACE_SPEED_CONTROLLER_BE,         /* SpeedControllerBlockEntity */
    IFACE_CHAIN_DRIVE_BLOCK,           /* ChainDriveBlock */
    IFACE_GENERATING_BE,               /* GeneratingKineticBlockEntity */
};

/* IRotate.SpeedLevel */
enum class SpeedLevel { NONE, SLOW, MEDIUM, FAST };
float speed_level_value(SpeedLevel l);
SpeedLevel speed_level_of(float speed);
/* IRotate.StressImpact.isEnabled */
MCC_INLINE bool stress_enabled() { return !g_kinetics.disable_stress; }

struct KineticBlockEntity;

/* ---------------- 方块：IRotate + KineticBlock + ICogWheel ---------------- */

struct KineticBlock : BlockBehaviour {
    mcc_block_t id;
    explicit KineticBlock(mcc_block_t b) : id(b) {}

    /* IRotate */
    virtual bool has_shaft_towards(Space &sp, BlockPos pos, mcc_state_t state, Dir face) {
        (void)sp; (void)pos; (void)state; (void)face;
        return false;
    }
    virtual Axis rotation_axis(mcc_state_t state) = 0;
    virtual SpeedLevel min_required_speed_level() { return SpeedLevel::NONE; }

    /* ICogWheel：是否实现了这个接口；大小齿轮（原版 isSmallCog 默认 = !isLargeCog） */
    virtual bool cogwheel() { return false; }
    virtual bool large_cog() { return false; }
    virtual bool dedicated_cogwheel() { return false; }

    /* KineticBlock.areStatesKineticallyEquivalent */
    virtual bool states_kinetically_equivalent(mcc_state_t old, mcc_state_t now);

    /* KineticBlock 的方块回调 */
    void on_place(Space &sp, BlockPos pos, mcc_state_t state, mcc_state_t old, bool moving) override;
    void on_remove(Space &sp, BlockPos pos, mcc_state_t state, mcc_state_t now, bool moving) override;
    void update_indirect_neighbour_shapes(Space &sp, BlockPos pos, mcc_state_t state, int flags, int recursion) override;
    bool has_block_entity() const override { return true; }
    void *interface(int iid) override { return iid == IFACE_KINETIC_BLOCK ? this : nullptr; }
};

MCC_INLINE KineticBlock *kinetic_block(mcc_state_t s) {
    return (KineticBlock *)state_behaviour(s).interface(IFACE_KINETIC_BLOCK);
}
/* ICogWheel.isSmallCog / isLargeCog / isDedicatedCogWheel */
MCC_INLINE bool is_small_cog(mcc_state_t s) { KineticBlock *k = kinetic_block(s); return k && k->cogwheel() && !k->large_cog(); }
MCC_INLINE bool is_large_cog(mcc_state_t s) { KineticBlock *k = kinetic_block(s); return k && k->cogwheel() && k->large_cog(); }
MCC_INLINE bool is_dedicated_cogwheel(mcc_state_t s) { KineticBlock *k = kinetic_block(s); return k && k->cogwheel() && k->dedicated_cogwheel(); }

/* ---------------- 应力网络 ---------------- */

struct KineticNetwork {
    int64_t id = 0;
    bool initialized = false;
    std::vector<KineticBlockEntity *> members;   /* 原版 members（成员 → 每 RPM 应力） */
    std::vector<KineticBlockEntity *> sources;   /* 原版 sources（动力源 → 每 RPM 容量） */
    float current_capacity = 0, current_stress = 0;
    float unloaded_capacity = 0, unloaded_stress = 0;
    int unloaded_members = 0;
    /* 总和缓存：成员、取值、转速有变化时作废（原版每次都重算，结果相同） */
    bool sums_dirty = true;
    float cached_capacity = 0, cached_stress = 0;
    uint64_t cache_time = NEVER;
    void recompute();

    void init_from_te(float max_stress, float cur_stress, int members);
    void add_silently(KineticBlockEntity *be, float last_capacity, float last_stress);
    void add(KineticBlockEntity *be);
    void update_capacity_for(KineticBlockEntity *be, float capacity);
    void update_stress_for(KineticBlockEntity *be, float stress);
    void remove(KineticBlockEntity *be);
    void sync();
    void update_capacity();
    void update_stress();
    void update_network();
    float calculate_capacity();
    float calculate_stress();
    float actual_capacity_of(KineticBlockEntity *be);
    float actual_stress_of(KineticBlockEntity *be);
    int size() const { return unloaded_members + (int)members.size(); }

    /* 内部 */
    Space *space = nullptr;
    void unlink(KineticBlockEntity *be);         /* 只从表里摘掉，不触发任何更新 */
    void link_member(KineticBlockEntity *be, float stress);
    void link_source(KineticBlockEntity *be, float capacity);
};

/* 空间里的网络表（原版 TorquePropagator.networks 的一个维度） */
KineticNetwork *network_get(Space &sp, int64_t id);
KineticNetwork *network_get_or_create(Space &sp, int64_t id);
void network_drop(Space &sp, int64_t id);

/* ---------------- 方块实体 ---------------- */

struct KineticBlockEntity : BlockEntity {
    /* 原版字段 */
    bool has_network = false;
    int64_t network = 0;
    bool has_source = false;
    BlockPos source{};
    bool network_dirty = false;
    bool update_speed = true;
    int prevent_speed_update = 0;
    float speed = 0, capacity = 0, stress = 0;
    bool over_stressed = false;
    bool was_moved = false;
    int flicker_tally = 0;
    int network_size = 0;
    float last_stress_applied = 0, last_capacity_provided = 0;
    /* TODO(顺序齿轮箱)：sequenceContext */

    /* SmartBlockEntity：首刻初始化、慢推进（原版倒计时换成到期时间） */
    bool initialized = false;
    int lazy_tick_rate = 10;
    bool has_lazy_tick = false;     /* 子类有 lazyTick 逻辑才需要定时推进 */
    uint64_t lazy_due = NEVER;
    uint64_t validation_due = 0;    /* 原版 validationCountdown 初值 0：第一刻就校验 */

    /* 所在网络表里的位置（O(1) 摘除） */
    KineticNetwork *net_obj = nullptr;
    int net_member_idx = -1, net_source_idx = -1;
    float net_stress = 0, net_capacity = 0;  /* 原版 members / sources 两张表里存的值 */

    KineticBlockEntity(BlockPos p, mcc_state_t s) : BlockEntity(p, s) {}
    ~KineticBlockEntity() override;

    /* ---- 推进 ---- */
    bool tick() override;
    virtual void initialize();
    virtual void lazy_tick() {}
    /* 子类的每刻逻辑（原版 tick() 里 super.tick() 之后的部分），返回是否还要继续推进 */
    virtual bool tick_extra() { return false; }
    void set_lazy_tick_rate(int rate) { lazy_tick_rate = rate; has_lazy_tick = true; }
    void schedule_next();

    /* ---- 原版方法 ---- */
    void validate_kinetics();
    void update_from_network(float max_stress, float current_stress, int size);
    virtual float calculate_stress_applied();
    virtual float calculate_added_stress_capacity();
    virtual void on_speed_changed(float previous_speed);
    virtual float generated_speed() { return 0; }
    bool is_source() { return generated_speed() != 0; }
    float get_speed();
    virtual float theoretical_speed() { return speed; }
    void set_speed(float s);
    virtual void set_source(BlockPos src);
    virtual void remove_source();
    void set_network(bool has, int64_t id);
    KineticNetwork *get_or_create_network();
    void attach_kinetics();
    void detach_kinetics();
    bool speed_requirement_fulfilled();
    void clear_kinetic_information();
    void warn_of_movement() { was_moved = true; }
    int flicker_score() const { return flicker_tally; }
    void send_data();
    void set_changed() { /* TODO(存档)：标记区块需要保存 */ }
    /* KineticEffectHandler.queueRotationIndicators：只有粒子效果 */
    void queue_rotation_indicators() { /* TODO(网络)：旋转指示粒子 */ }

    /* ---- 自定义传播（原版 KineticBlockEntity 的可覆盖方法） ---- */
    virtual float propagate_rotation_to(KineticBlockEntity *target, mcc_state_t from, mcc_state_t to, BlockPos diff,
                                        bool via_axes, bool via_cogs) {
        (void)target; (void)from; (void)to; (void)diff; (void)via_axes; (void)via_cogs;
        return 0;
    }
    virtual void add_propagation_locations(KineticBlock *block, mcc_state_t state, std::vector<BlockPos> &out);
    virtual bool is_custom_connection(KineticBlockEntity *other, mcc_state_t state, mcc_state_t other_state) {
        (void)other; (void)state; (void)other_state;
        return false;
    }
    virtual bool can_propagate_diagonally(KineticBlock *block, mcc_state_t state);

    /* ---- 方块实体生命周期 ---- */
    void on_removed() override;            /* SmartBlockEntity.setRemoved → remove() */
    void on_chunk_unloaded() override;
    void on_space_changed(Space &from) override;
    void *interface(int iid) override { return iid == IFACE_KINETIC_BE ? this : nullptr; }
    /* SmartBlockEntity.destroy：行为（Behaviour）的销毁回调 */
    virtual void destroy() {}
};

MCC_INLINE KineticBlockEntity *as_kinetic(BlockEntity *be) {
    return be ? (KineticBlockEntity *)be->interface(IFACE_KINETIC_BE) : nullptr;
}

/* GeneratingKineticBlockEntity */
struct GeneratingKineticBlockEntity : KineticBlockEntity {
    bool reactivate_source = false;
    using KineticBlockEntity::KineticBlockEntity;

    void notify_stress_capacity_change(float capacity);
    void remove_source() override;
    void set_source(BlockPos src) override;
    bool tick_extra() override;
    void update_generated_rotation();
    void apply_new_speed(float prev_speed, float speed);
    int64_t create_network_id() { return pos.as_long(); }
    void *interface(int iid) override {
        return iid == IFACE_GENERATING_BE ? this : KineticBlockEntity::interface(iid);
    }
};

/* KineticBlockEntity.convertToDirection */
MCC_INLINE float convert_to_direction(float axis_speed, Dir d) { return dir_positive(d) ? axis_speed : -axis_speed; }
/* KineticBlockEntity.switchToBlockState */
void switch_to_block_state(Space &sp, BlockPos pos, mcc_state_t state);

/* ---------------- 转速传播（RotationPropagator） ---------------- */

void rotation_handle_added(Space &sp, BlockPos pos, KineticBlockEntity *added);
void rotation_handle_removed(Space &sp, BlockPos pos, KineticBlockEntity *removed);
bool rotation_is_connected(KineticBlockEntity *from, KineticBlockEntity *to);

/* 网络表附加数据（init 时注册） */
void kinetics_init();

} // namespace mcc::create

#endif
