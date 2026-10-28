/*
 * 方块行为（对应原版 Block / BlockBehaviour 的服务端回调）
 *
 * 每种方块一个行为对象，按方块编号查表；没有注册的方块用默认行为（什么都不做）。
 * 回调的调用时机与顺序照原版 Level.setBlock / LevelChunk.setBlockState / NeighborUpdater。
 * 所有回调都在拥有该方块空间的区域线程上执行。
 */
#ifndef MCC_GAME_BLOCK_H
#define MCC_GAME_BLOCK_H

#include "types.h"
#include "../world/blocks.h"

namespace mcc {

struct Space;
struct BlockEntity;

/* 原版 Block.UPDATE_* 标志 */
enum : int {
    UPDATE_NEIGHBORS = 1,
    UPDATE_CLIENTS = 2,
    UPDATE_INVISIBLE = 4,
    UPDATE_IMMEDIATE = 8,
    UPDATE_KNOWN_SHAPE = 16,
    UPDATE_SUPPRESS_DROPS = 32,
    UPDATE_MOVE_BY_PISTON = 64,
    UPDATE_ALL = UPDATE_NEIGHBORS | UPDATE_CLIENTS,
};

/* 原版 TickPriority */
enum TickPriority : int8_t {
    PRIORITY_EXTREMELY_HIGH = -3,
    PRIORITY_VERY_HIGH = -2,
    PRIORITY_HIGH = -1,
    PRIORITY_NORMAL = 0,
    PRIORITY_LOW = 1,
    PRIORITY_VERY_LOW = 2,
    PRIORITY_EXTREMELY_LOW = 3,
};

struct BlockBehaviour {
    virtual ~BlockBehaviour() = default;

    /* 原版 onPlace：新状态已写入存储，方块实体还没创建（新放置时）或已存在（状态变化时） */
    virtual void on_place(Space &, BlockPos, mcc_state_t state, mcc_state_t old, bool moving) {
        (void)state; (void)old; (void)moving;
    }
    /* 原版 onRemove：此时存储里已经是新状态。默认：方块种类变了就移除方块实体 */
    virtual void on_remove(Space &sp, BlockPos pos, mcc_state_t state, mcc_state_t now, bool moving);
    /* 原版 neighborChanged */
    virtual void neighbor_changed(Space &, BlockPos, mcc_state_t state, mcc_block_t from_block, BlockPos from, bool moving) {
        (void)state; (void)from_block; (void)from; (void)moving;
    }
    /* 原版 updateShape：dir 方向的邻居变成 neighbor，返回本方块的新状态 */
    virtual mcc_state_t update_shape(Space &, BlockPos pos, mcc_state_t state, Dir dir, mcc_state_t neighbor, BlockPos npos) {
        (void)pos; (void)dir; (void)neighbor; (void)npos;
        return state;
    }
    /* 原版 updateIndirectNeighbourShapes（setBlock 时对新旧状态各调一次） */
    virtual void update_indirect_neighbour_shapes(Space &, BlockPos, mcc_state_t state, int flags, int recursion) {
        (void)state; (void)flags; (void)recursion;
    }
    /* 原版 tick：计划刻 */
    virtual void tick(Space &, BlockPos, mcc_state_t state) { (void)state; }

    /* 原版 EntityBlock：有没有方块实体、怎么创建 */
    virtual bool has_block_entity() const { return false; }
    virtual BlockEntity *new_block_entity(BlockPos, mcc_state_t) { return nullptr; }

    /* 代替 Java 的 instanceof 接口判断（编译关掉了 RTTI）：
     * 实现了编号为 id 的接口就返回对应指针，否则 NULL。接口编号由各模组定义 */
    virtual void *interface(int id) { (void)id; return nullptr; }
};

/* 方块状态属性的快速读写：注册时按名字查一次下标，之后按下标取值 */
struct Prop {
    int index = -1;
    void bind(mcc_block_t block, const char *name);
    bool valid() const { return index >= 0; }
    int get(mcc_state_t s) const { return mcc_state_get(s, index); }
    mcc_state_t with(mcc_state_t s, int v) const { return mcc_state_with(s, index, v); }
    /* 布尔属性：原版取值顺序 true、false */
    bool get_bool(mcc_state_t s) const { return mcc_state_get(s, index) == 0; }
    mcc_state_t with_bool(mcc_state_t s, bool v) const { return mcc_state_with(s, index, v ? 0 : 1); }
    mcc_state_t cycle(mcc_state_t s) const;
};

/* 注册：启动时调用，之后只读 */
void block_register(mcc_block_t block, BlockBehaviour *b);
BlockBehaviour &block_behaviour(mcc_block_t block);
/* 关服：释放注册的行为对象（注册表接管所有权） */
void block_registry_free();
MCC_INLINE BlockBehaviour &state_behaviour(mcc_state_t s) { return block_behaviour(mcc_state_block(s)); }
MCC_INLINE bool state_has_block_entity(mcc_state_t s) { return state_behaviour(s).has_block_entity(); }

/* 按名字取方块编号，不存在时致命错误（注册机械动力方块时用） */
mcc_block_t block_id(const char *name);

} // namespace mcc

#endif
