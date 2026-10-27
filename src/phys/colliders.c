#include "colliders.h"
#include "../core/log.h"

static MCC_VEC(phys_collider) g_colliders;
static bool g_finalized;

uint32_t phys_collider_new(float friction, float volume, float restitution, bool fluid, bool dynamic, uint16_t callback) {
    if (g_finalized) mcc_log_fatal("碰撞体注册表已冻结，不能再注册");
    phys_collider c = {0};
    c.friction = friction;
    c.volume = volume;
    c.restitution = restitution;
    c.fluid = fluid;
    c.dynamic = dynamic;
    c.callback = callback;
    c.needs_hooks = callback != 0 || friction != 1.0f || restitution != 0.0f;
    mcc_vec_push(&g_colliders, c);
    return (uint32_t)g_colliders.len;
}

void phys_collider_add_box(uint32_t id, const float min[3], const float max[3]) {
    if (g_finalized || id == 0 || id > g_colliders.len) mcc_log_fatal("非法碰撞体编号 %u", id);
    phys_collider *c = &g_colliders.data[id - 1];
    c->boxes = mcc_xrealloc(c->boxes, sizeof(phys_box) * (size_t)(c->nboxes + 1));
    phys_box *b = &c->boxes[c->nboxes++];
    for (int i = 0; i < 3; i++) {
        /* 与 Sable 一致：碰撞箱裁剪到单位方块内，避免栅栏/墙越界 */
        b->min[i] = MCC_MAX(min[i], 0.0f);
        b->max[i] = MCC_MIN(max[i], 1.0f);
    }
}

void phys_collider_clear_boxes(uint32_t id) {
    if (g_finalized || id == 0 || id > g_colliders.len) mcc_log_fatal("非法碰撞体编号 %u", id);
    phys_collider *c = &g_colliders.data[id - 1];
    free(c->boxes);
    c->boxes = NULL;
    c->nboxes = 0;
}

void phys_colliders_finalize(void) {
    for (size_t i = 0; i < g_colliders.len; i++) {
        phys_collider *c = &g_colliders.data[i];
        if (!c->nboxes) continue;
        c->hulls = mcc_xaligned(16, sizeof(b3BoxHull) * (size_t)c->nboxes);
        c->centers = mcc_xmalloc(sizeof(b3Vec3) * (size_t)c->nboxes);
        c->halves = mcc_xmalloc(sizeof(b3Vec3) * (size_t)c->nboxes);
        for (int b = 0; b < c->nboxes; b++) {
            const phys_box *bx = &c->boxes[b];
            b3Vec3 h = { (bx->max[0] - bx->min[0]) * 0.5f, (bx->max[1] - bx->min[1]) * 0.5f, (bx->max[2] - bx->min[2]) * 0.5f };
            /* 极薄的碰撞箱（地毯、压力板）给一个最小厚度，凸包构造要求非退化 */
            const float minh = 0.005f;
            if (h.x < minh) h.x = minh;
            if (h.y < minh) h.y = minh;
            if (h.z < minh) h.z = minh;
            c->halves[b] = h;
            c->centers[b] = (b3Vec3){ (bx->max[0] + bx->min[0]) * 0.5f, (bx->max[1] + bx->min[1]) * 0.5f,
                                       (bx->max[2] + bx->min[2]) * 0.5f };
            c->hulls[b] = b3MakeBoxHull(h.x, h.y, h.z);
        }
    }
    g_finalized = true;
    mcc_info("物理碰撞体注册表冻结：%zu 个碰撞体", g_colliders.len);
}

void phys_colliders_free(void) {
    for (size_t i = 0; i < g_colliders.len; i++) {
        phys_collider *c = &g_colliders.data[i];
        free(c->boxes);
        free(c->hulls);
        free(c->centers);
        free(c->halves);
    }
    mcc_vec_free(&g_colliders);
    g_finalized = false;
    free(phys_state_mass);
    phys_state_mass = NULL;
}

uint32_t phys_collider_count(void) { return (uint32_t)g_colliders.len; }

const phys_collider *phys_collider_get(uint32_t id) {
    if (id == 0 || id > g_colliders.len) return NULL;
    return &g_colliders.data[id - 1];
}
