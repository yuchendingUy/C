#include "game.h"

namespace mcc {

void game_tick_region(mcc_region_world *w) {
    region_space(w).tick();
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&w->sublevels, &it, NULL, &v)) sublevel_space((mcc_sublevel *)v).tick();
}

} // namespace mcc
