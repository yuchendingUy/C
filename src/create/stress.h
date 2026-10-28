/*
 * 方块应力值（对应 api/stress/BlockStressValues 与 CStress 的默认值）：
 * 每种方块的应力影响（消耗，每 RPM）与应力容量（提供，每 RPM），未设置为 0。
 * 数据来自 data/create/stress.txt（tools/extract_create_stress.py 从机械动力源码提取）。
 */
#ifndef MCC_CREATE_STRESS_H
#define MCC_CREATE_STRESS_H

#include "../world/blocks.h"

namespace mcc::create {

bool stress_load(const char *path);
double stress_impact(mcc_block_t block);
double stress_capacity(mcc_block_t block);

} // namespace mcc::create

#endif
