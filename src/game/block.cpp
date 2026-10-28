#include "block.h"
#include "space.h"
#include "../core/log.h"
#include <vector>

namespace mcc {

static BlockBehaviour g_default;
static std::vector<BlockBehaviour *> g_table;

void block_register(mcc_block_t block, BlockBehaviour *b) {
    if (g_table.size() < mcc_blocks.block_count) g_table.resize(mcc_blocks.block_count, &g_default);
    if (block >= g_table.size()) mcc_log_fatal("方块行为注册：方块编号 %u 越界", block);
    g_table[block] = b;
}

void block_registry_free() {
    for (BlockBehaviour *&b : g_table) {
        if (b != &g_default) delete b;
        b = &g_default;
    }
    g_table.clear();
}

BlockBehaviour &block_behaviour(mcc_block_t block) {
    return block < g_table.size() ? *g_table[block] : g_default;
}

mcc_block_t block_id(const char *name) {
    int32_t b = mcc_block_by_name(name);
    if (b < 0) mcc_log_fatal("方块 %s 不在注册表里", name);
    return (mcc_block_t)b;
}

void Prop::bind(mcc_block_t block, const char *name) {
    index = mcc_state_prop_index(mcc_blocks.blocks[block].default_state, name);
    if (index < 0) mcc_log_fatal("方块 %s 没有属性 %s", mcc_blocks.blocks[block].name, name);
}

/* 原版 BlockState.cycle：取下一个值，最后一个之后回到第一个 */
mcc_state_t Prop::cycle(mcc_state_t s) const {
    int n = mcc_state_def(s)->props[index].nvalues;
    return mcc_state_with(s, index, (get(s) + 1) % n);
}

/* 原版 BlockBehaviour.onRemove：方块种类变了，移除方块实体 */
void BlockBehaviour::on_remove(Space &sp, BlockPos pos, mcc_state_t state, mcc_state_t now, bool moving) {
    (void)moving;
    if (has_block_entity() && mcc_state_block(state) != mcc_state_block(now)) sp.remove_block_entity(pos);
}

} // namespace mcc
