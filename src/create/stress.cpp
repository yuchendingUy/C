#include "stress.h"
#include "../core/log.h"
#include <stdio.h>
#include <vector>

namespace mcc::create {

static std::vector<double> g_impact, g_capacity;

bool stress_load(const char *path) {
    g_impact.assign(mcc_blocks.block_count, 0.0);
    g_capacity.assign(mcc_blocks.block_count, 0.0);
    FILE *f = fopen(path, "r");
    if (!f) { mcc_error("无法打开应力表 %s", path); return false; }
    char line[512], name[256], imp[64], cap[64];
    int n = 0, lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "S %255s %63s %63s", name, imp, cap) != 3) {
            mcc_error("应力表 %s 第 %d 行格式错误", path, lineno);
            fclose(f);
            return false;
        }
        int32_t b = mcc_block_by_name(name);
        if (b < 0) continue; /* 还没移植的方块 */
        if (imp[0] != '-') g_impact[b] = strtod(imp, NULL);
        if (cap[0] != '-') g_capacity[b] = strtod(cap, NULL);
        n++;
    }
    fclose(f);
    mcc_info("机械动力应力表载入：%d 种方块", n);
    return true;
}

double stress_impact(mcc_block_t block) { return block < g_impact.size() ? g_impact[block] : 0.0; }
double stress_capacity(mcc_block_t block) { return block < g_capacity.size() ? g_capacity[block] : 0.0; }

} // namespace mcc::create
