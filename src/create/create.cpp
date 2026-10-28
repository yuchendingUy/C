#include "create.h"
#include "stress.h"
#include "kinetics/blocks.h"
#include <stdio.h>

namespace mcc::create {

bool create_init(const char *data_dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/create/stress.txt", data_dir);
    if (!stress_load(path)) return false;
    kinetics_init();
    kinetic_blocks_register();
    return true;
}

} // namespace mcc::create
