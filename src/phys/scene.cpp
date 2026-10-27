/*
 * 物理场景实现：Sable sable_rapier/lib.rs + RapierPhysicsPipeline 的 C 移植，引擎换成 Box3D
 */
#include "scene.h"
#include "../core/jobs.h"
#include "../core/log.h"
#include <math.h>
#include <pthread.h>

static pthread_mutex_t g_world_lock = PTHREAD_MUTEX_INITIALIZER;

#define MAX_REPORTED_COLLISIONS 100
#define SUBLEVEL_BASE_FRICTION 0.525f   /* Sable 子世界碰撞体的基础摩擦 */
#define SLEEP_THRESHOLD 0.15f           /* Sable 休眠阈值 */

struct phys_scene {
    b3WorldId world;
    phys_scene_config cfg;
    phys_tuning tuning;
    mcc_chunk_store *terrain;            /* 地形方块（不归物理所有） */
    mcc_store_listener terrain_listener;
    mcc_hmap bodies;         /* 编号 → phys_body* */
    mcc_hmap cells;          /* 地形格键 → phys_cell* */
    MCC_VEC(phys_collision) collisions;
    MCC_VEC(b3BodyId) wake;              /* 方块变化时收集待唤醒的刚体 */
    b3ContactData *contact_buf;
    int contact_cap;
    _Atomic(uint64_t) stat_manifolds, stat_pairs, stat_collide_ns;
    uint64_t stat_step_ns, stat_tick_ns;
};

/* ------------------------------------------------------------------ */
/* Box3D 任务系统接入 mcc 通用任务线程池                                */
/* ------------------------------------------------------------------ */

typedef struct {
    b3TaskCallback *fn;
    void *ctx;
    mcc_job_group group;
} b3_job;

static void b3_job_run(void *p) {
    b3_job *j = (b3_job*)p;
    j->fn(j->ctx);
}

static void *enqueue_task(b3TaskCallback *task, void *task_ctx, void *user_ctx, const char *name) {
    (void)user_ctx;
    (void)name;
    if (mcc_jobs_thread_count() == 0) {
        task(task_ctx);
        return NULL;
    }
    b3_job *j = (b3_job*)mcc_xmalloc(sizeof *j);
    j->fn = task;
    j->ctx = task_ctx;
    mcc_job_group_init(&j->group);
    mcc_jobs_submit(&j->group, b3_job_run, j);
    return j;
}

static void finish_task(void *user_task, void *user_ctx) {
    (void)user_ctx;
    b3_job *j = (b3_job*)user_task;
    mcc_jobs_wait(&j->group);  /* 等待期间本线程帮忙执行其他任务 */
    free(j);
}

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

MCC_INLINE int32_t floor_div(int32_t a, int shift) { return a >> shift; }

static b3WorldTransform pose_to_xf(const double pose[7]) {
    b3WorldTransform xf;
    xf.p = (b3Pos){ pose[0], pose[1], pose[2] };
    xf.q = b3NormalizeQuat((b3Quat){ { (float)pose[3], (float)pose[4], (float)pose[5] }, (float)pose[6] });
    return xf;
}

phys_body *phys_body_get(phys_scene *s, int32_t id) {
    return (phys_body*)mcc_hmap_get(&s->bodies, (uint64_t)(uint32_t)id);
}

bool phys_scene_next_body(phys_scene *s, uint32_t *it, phys_body **out) {
    void *v;
    if (!mcc_hmap_next(&s->bodies, it, NULL, &v)) return false;
    *out = (phys_body*)v;
    return true;
}

static phys_body *body_req(phys_scene *s, int32_t id, const char *what) {
    phys_body *b = phys_body_get(s, id);
    if (!b) mcc_log_fatal("物理场景：%s 找不到刚体 %d", what, id);
    return b;
}

mcc_chunk_store *phys_scene_terrain(phys_scene *s) { return s->terrain; }

static void cell_acquire(phys_scene *s, int32_t cx, int32_t cz);
static void cell_release(phys_scene *s, int32_t cx, int32_t cz);

/* ------------------------------------------------------------------ */
/* 方块存储事件                                                        */
/* ------------------------------------------------------------------ */

static bool collect_body(b3ShapeId shape, void *ctx) {
    phys_scene *s = (phys_scene*)ctx;
    void *u = b3Shape_GetUserData(shape);
    if (u && *(int *)u == PHYS_KIND_BODY) mcc_vec_push(&s->wake, ((phys_body *)u)->body);
    return true;
}

/* 方块内容变了而刚体没动：Box3D 的接触回收会沿用旧接触，要作废缓存并唤醒 */
static void touch_body(b3BodyId body) {
    b3Body_InvalidateContacts(body);
    b3Body_SetAwake(body, true);
}

/* 包围盒碰到 [lo, hi] 的刚体全部 touch（查询期间不改动世界，先收集再处理）。
 * 宽相包围盒是单精度世界坐标，远离原点时有舍入，多留 2 格余量 */
static void touch_bodies_in(phys_scene *s, double lx, double ly, double lz, double hx, double hy, double hz) {
    b3AABB box = { { (float)lx - 2.0f, (float)ly - 2.0f, (float)lz - 2.0f }, { (float)hx + 2.0f, (float)hy + 2.0f, (float)hz + 2.0f } };
    s->wake.len = 0;
    b3World_OverlapAABB(s->world, box, b3DefaultQueryFilter(), collect_body, s);
    for (size_t i = 0; i < s->wake.len; i++) touch_body(s->wake.data[i]);
}

static void terrain_block_changed(void *ctx, int32_t x, int32_t y, int32_t z, mcc_state_t old, mcc_state_t now) {
    (void)old;
    (void)now;
    touch_bodies_in((phys_scene*)ctx, x, y, z, x + 1.0, y + 1.0, z + 1.0);
}

static void touch_chunk_column(phys_scene *s, const mcc_chunk *c) {
    double x = (double)c->cx * 16, z = (double)c->cz * 16;
    touch_bodies_in(s, x, (double)c->min_sy * 16, z, x + 16, (double)(c->min_sy + c->nsec) * 16, z + 16);
}

static void terrain_chunk_added(void *ctx, mcc_chunk *c) {
    cell_acquire((phys_scene*)ctx, c->cx, c->cz);
    touch_chunk_column((phys_scene*)ctx, c);
}

static void terrain_chunk_removed(void *ctx, mcc_chunk *c) {
    touch_chunk_column((phys_scene*)ctx, c);
    cell_release((phys_scene*)ctx, c->cx, c->cz);
}

/* 子世界方块变化：结构变了，静止的刚体该重新动起来；质量追踪同步更新 */
static void body_block_changed(void *ctx, int32_t x, int32_t y, int32_t z, mcc_state_t old, mcc_state_t now) {
    phys_body *b = (phys_body*)ctx;
    if (b->mass) {
        phys_mass_add(b->mass, x, y, z, old, -1);
        phys_mass_add(b->mass, x, y, z, now, +1);
    }
    touch_body(b->body);
}

static void body_chunk_added(void *ctx, mcc_chunk *c) {
    phys_body *b = (phys_body*)ctx;
    if (b->mass) phys_mass_add_chunk(b->mass, c, +1);
    touch_body(b->body);
}

static void body_chunk_removed(void *ctx, mcc_chunk *c) {
    phys_body *b = (phys_body*)ctx;
    if (b->mass) phys_mass_add_chunk(b->mass, c, -1);
    touch_body(b->body);
}

/* ------------------------------------------------------------------ */
/* 场景                                                                */
/* ------------------------------------------------------------------ */

phys_scene *phys_scene_create(const phys_scene_config *cfg, mcc_chunk_store *terrain) {
    phys_scene *s = (phys_scene*)mcc_xcalloc(1, sizeof *s);
    s->cfg = *cfg;
    /* Sable 默认：接触固有频率 30Hz、阻尼比 5；关节 550Hz、阻尼比 4 */
    s->tuning = (phys_tuning){ 30.0f, 5.0f, 4, 550.0f, 4.0f };
    mcc_hmap_init(&s->bodies, 16);
    mcc_hmap_init(&s->cells, 16);

    b3WorldDef def = b3DefaultWorldDef();
    def.gravity = (b3Vec3){ (float)cfg->gravity[0], (float)cfg->gravity[1], (float)cfg->gravity[2] };
    def.contactHertz = s->tuning.contact_hertz;
    def.contactDampingRatio = s->tuning.contact_damping;
    def.voxelManifoldFcn = phys_voxel_manifolds;
    def.voxelManifoldContext = s;
    int workers = mcc_jobs_thread_count();
    if (workers > 0) {
        def.workerCount = workers + 1;
        def.enqueueTask = enqueue_task;
        def.finishTask = finish_task;
        def.userTaskContext = s;
    } else {
        def.workerCount = 1;
    }
    /* Box3D 在全局数组里找空位建世界，没有加锁：各区域线程会同时建/销毁场景，这里串行化 */
    pthread_mutex_lock(&g_world_lock);
    s->world = b3CreateWorld(&def);
    pthread_mutex_unlock(&g_world_lock);
    if (!b3World_IsValid(s->world)) mcc_log_fatal("物理场景：Box3D 世界数量达到上限 %d", B3_MAX_WORLDS);

    s->terrain = terrain;
    if (terrain) {
        s->terrain_listener = (mcc_store_listener){ s, terrain_chunk_added, terrain_chunk_removed, terrain_block_changed, NULL };
        mcc_store_listen(terrain, &s->terrain_listener);
        uint32_t it = 0;
        void *v;
        while (mcc_hmap_next(&terrain->chunks, &it, NULL, &v)) cell_acquire(s, ((mcc_chunk *)v)->cx, ((mcc_chunk *)v)->cz);
    }
    return s;
}

void phys_scene_set_tuning(phys_scene *s, const phys_tuning *t) {
    s->tuning = *t;
    if (s->tuning.substeps < 1) s->tuning.substeps = 1;
    b3World_SetContactTuning(s->world, t->contact_hertz, t->contact_damping, 3.0f);
}

b3WorldId phys_scene_world(phys_scene *s) { return s->world; }

static void body_free(phys_body *b) {
    if (b->blocks) mcc_store_unlisten(b->blocks, &b->listener);
    if (b->mass) {
        phys_mass_free(b->mass);
        free(b->mass);
    }
    free(b);
}

void phys_scene_destroy(phys_scene *s) {
    if (s->terrain) mcc_store_unlisten(s->terrain, &s->terrain_listener);
    pthread_mutex_lock(&g_world_lock);
    b3DestroyWorld(s->world);
    pthread_mutex_unlock(&g_world_lock);
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&s->bodies, &it, NULL, &v)) body_free((phys_body*)v);
    it = 0;
    while (mcc_hmap_next(&s->cells, &it, NULL, &v)) free(v);
    mcc_hmap_free(&s->bodies);
    mcc_hmap_free(&s->cells);
    mcc_vec_free(&s->collisions);
    mcc_vec_free(&s->wake);
    free(s->contact_buf);
    free(s);
}

/* ------------------------------------------------------------------ */
/* 子世界                                                              */
/* ------------------------------------------------------------------ */

static void update_body_shape_bounds(phys_body *b) {
    if (!b->has_bounds || !b->has_com) return;
    b3AABB box = {
        { (float)(b->bmin[0] - b->com[0]), (float)(b->bmin[1] - b->com[1]), (float)(b->bmin[2] - b->com[2]) },
        { (float)(b->bmax[0] + 1 - b->com[0]), (float)(b->bmax[1] + 1 - b->com[1]), (float)(b->bmax[2] + 1 - b->com[2]) },
    };
    b3Shape_SetVoxelBounds(b->shape, box);
}

/* 在场景的 Box3D 世界里给 b 建刚体和体素形状 */
static void body_create_b3(phys_scene *s, phys_body *b, b3WorldTransform xf, b3BodyType type) {
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = type;
    bd.position = xf.p;
    bd.rotation = xf.q;
    bd.linearDamping = (float)s->cfg.universal_drag;
    bd.angularDamping = (float)s->cfg.universal_drag;
    bd.sleepThreshold = SLEEP_THRESHOLD;
    bd.userData = b;
    b->body = b3CreateBody(s->world, &bd);

    b3ShapeDef sd = b3DefaultShapeDef();
    sd.baseMaterial.friction = SUBLEVEL_BASE_FRICTION;
    sd.userData = b;
    b3VoxelShape vs = { b, { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } }, false };
    b->shape = b3CreateVoxelShape(b->body, &sd, &vs);
    update_body_shape_bounds(b);
}

bool phys_sublevel_create(phys_scene *s, int32_t id, const double pose[7], mcc_chunk_store *blocks) {
    if (phys_body_get(s, id)) {
        mcc_error("物理场景：刚体 %d 已存在", id);
        return false;
    }
    phys_body *b = (phys_body*)mcc_xcalloc(1, sizeof *b);
    b->kind = PHYS_KIND_BODY;
    b->scene = s;
    b->id = id;
    b->blocks = blocks;

    body_create_b3(s, b, pose_to_xf(pose), b3_dynamicBody);
    /* 体素形状不贡献质量；质量追踪算出真实值前先给单位质量，Box3D 要求步进前质量已设置 */
    b3MassData md = { 1.0f, b3Vec3_zero, { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
    b3Body_SetMassData(b->body, md);

    if (blocks) {
        b->listener = (mcc_store_listener){ b, body_chunk_added, body_chunk_removed, body_block_changed, NULL };
        mcc_store_listen(blocks, &b->listener);
    }
    mcc_hmap_put(&s->bodies, (uint64_t)(uint32_t)id, b);
    return true;
}

/* 区域合并/拆分：刚体换到另一个场景。Box3D 刚体不能跨世界，在目标世界按原状态重建：
 * 位姿、速度、质量、休眠状态、每刻力原样保留；接触缓存（暖启动）丢失，下一步重新生成 */
bool phys_body_migrate(phys_scene *src, phys_scene *dst, int32_t id) {
    if (src == dst) return true;
    phys_body *b = phys_body_get(src, id);
    if (!b) return false;
    if (phys_body_get(dst, id)) mcc_log_fatal("物理场景迁移：目标场景已有刚体 %d", id);
    mcc_hmap_remove(&src->bodies, (uint64_t)(uint32_t)id);
    b3WorldTransform xf = b3Body_GetTransform(b->body);
    b3Vec3 v = b3Body_GetLinearVelocity(b->body), w = b3Body_GetAngularVelocity(b->body);
    b3MassData md = b3Body_GetMassData(b->body);
    b3BodyType type = b3Body_GetType(b->body);
    bool awake = b3Body_IsAwake(b->body);
    b3DestroyBody(b->body);

    b->scene = dst;
    body_create_b3(dst, b, xf, type);
    b3Body_SetMassData(b->body, md);
    b3Body_SetLinearVelocity(b->body, v);
    b3Body_SetAngularVelocity(b->body, w);
    if (!awake) b3Body_SetAwake(b->body, false);
    mcc_hmap_put(&dst->bodies, (uint64_t)(uint32_t)id, b);
    return true;
}

size_t phys_scene_body_count(phys_scene *s) { return s->bodies.count; }

static void set_mass_data(phys_body *b, double mass, const double inertia[9]) {
    b3MassData md;
    md.mass = (float)MCC_MAX(mass, 1e-3);
    md.center = b3Vec3_zero;
    md.inertia.cx = (b3Vec3){ (float)inertia[0], (float)inertia[1], (float)inertia[2] };
    md.inertia.cy = (b3Vec3){ (float)inertia[3], (float)inertia[4], (float)inertia[5] };
    md.inertia.cz = (b3Vec3){ (float)inertia[6], (float)inertia[7], (float)inertia[8] };
    b3Body_SetMassData(b->body, md);
}

/* 把质量追踪的结果应用到刚体。刚体原点就是质心：质心在局部系里移动 d 时，
 * 原点在世界里跟着移动 R·d，方块世界位置不变；新质心的线速度 = v + ω × (R·d) */
static void apply_mass(phys_body *b) {
    phys_mass_tracker *t = b->mass;
    t->dirty = false;
    double com[3], mass, inertia[9];
    int32_t mn[3], mx[3];
    if (!phys_mass_result(t, com, &mass, inertia) || !phys_mass_bounds(t, mn, mx)) return; /* 空结构：保持原样，由上层移除 */
    if (b->has_com) {
        b3Vec3 d = { (float)(com[0] - b->com[0]), (float)(com[1] - b->com[1]), (float)(com[2] - b->com[2]) };
        if (d.x != 0 || d.y != 0 || d.z != 0) {
            b3WorldTransform xf = b3Body_GetTransform(b->body);
            b3Vec3 dw = b3RotateVector(xf.q, d);
            b3Vec3 v = b3Body_GetLinearVelocity(b->body), w = b3Body_GetAngularVelocity(b->body);
            xf.p.x += dw.x;
            xf.p.y += dw.y;
            xf.p.z += dw.z;
            bool awake = b3Body_IsAwake(b->body);
            b3Body_SetTransform(b->body, xf.p, xf.q);
            b3Body_SetLinearVelocity(b->body, b3Add(v, b3Cross(w, dw)));
            b3Body_SetAngularVelocity(b->body, w);
            if (!awake) b3Body_SetAwake(b->body, false);
        }
    }
    memcpy(b->com, com, sizeof b->com);
    b->has_com = true;
    memcpy(b->bmin, mn, sizeof b->bmin);
    memcpy(b->bmax, mx, sizeof b->bmax);
    b->has_bounds = true;
    update_body_shape_bounds(b);
    set_mass_data(b, mass, inertia);
}

void phys_body_track_mass(phys_scene *s, int32_t id) {
    phys_body *b = body_req(s, id, "开启质量追踪");
    if (b->mass || !b->blocks) return;
    b->mass = (phys_mass_tracker*)mcc_xmalloc(sizeof *b->mass);
    phys_mass_init(b->mass);
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&b->blocks->chunks, &it, NULL, &v)) phys_mass_add_chunk(b->mass, (const mcc_chunk*)v, +1);
    apply_mass(b);
}

void phys_sublevel_remove(phys_scene *s, int32_t id) {
    phys_body *b = (phys_body*)mcc_hmap_remove(&s->bodies, (uint64_t)(uint32_t)id);
    if (!b) return;
    b3DestroyBody(b->body);
    body_free(b);
}

void phys_body_set_center_of_mass(phys_scene *s, int32_t id, const double com[3]) {
    phys_body *b = body_req(s, id, "设置质心");
    b->has_com = true;
    memcpy(b->com, com, sizeof b->com);
    update_body_shape_bounds(b);
}

/* 对应 ActiveLevelColliderInfo::set_local_bounds。方块直接从子世界存储读，包围盒变化不需要重建任何结构 */
void phys_body_set_local_bounds(phys_scene *s, int32_t id, const int32_t min[3], const int32_t max[3]) {
    phys_body *b = body_req(s, id, "设置局部包围盒");
    memcpy(b->bmin, min, sizeof b->bmin);
    memcpy(b->bmax, max, sizeof b->bmax);
    b->has_bounds = true;
    update_body_shape_bounds(b);
}
void phys_body_set_mass(phys_scene *s, int32_t id, double mass, const double inertia[9]) {
    /* Sable 以列向量给出惯性张量；质心在刚体原点 */
    set_mass_data(body_req(s, id, "设置质量"), mass, inertia);
}

void phys_body_get_pose(phys_scene *s, int32_t id, double out[7]) {
    phys_body *b = body_req(s, id, "读位姿");
    b3WorldTransform xf = b3Body_GetTransform(b->body);
    out[0] = xf.p.x; out[1] = xf.p.y; out[2] = xf.p.z;
    out[3] = xf.q.v.x; out[4] = xf.q.v.y; out[5] = xf.q.v.z; out[6] = xf.q.s;
}

void phys_body_teleport(phys_scene *s, int32_t id, const double pose[7]) {
    phys_body *b = body_req(s, id, "传送");
    b3WorldTransform xf = pose_to_xf(pose);
    b3Body_SetTransform(b->body, xf.p, xf.q);
    b3Body_SetAwake(b->body, true);
}

void phys_body_wake(phys_scene *s, int32_t id) {
    b3Body_SetAwake(body_req(s, id, "唤醒")->body, true);
}

void phys_body_add_velocities(phys_scene *s, int32_t id, const double lin[3], const double ang[3], bool wake) {
    phys_body *b = body_req(s, id, "加速度");
    if (!wake && !b3Body_IsAwake(b->body)) return;
    b3Vec3 v = b3Body_GetLinearVelocity(b->body), w = b3Body_GetAngularVelocity(b->body);
    v = b3Add(v, (b3Vec3){ (float)lin[0], (float)lin[1], (float)lin[2] });
    w = b3Add(w, (b3Vec3){ (float)ang[0], (float)ang[1], (float)ang[2] });
    b3Body_SetLinearVelocity(b->body, v);
    b3Body_SetAngularVelocity(b->body, w);
}

void phys_body_apply_impulse(phys_scene *s, int32_t id, const double local_point[3], const double local_impulse[3], bool wake) {
    phys_body *b = body_req(s, id, "施加冲量");
    if (!wake && !b3Body_IsAwake(b->body)) return;
    b3WorldTransform xf = b3Body_GetTransform(b->body);
    b3Vec3 impulse = b3RotateVector(xf.q, (b3Vec3){ (float)local_impulse[0], (float)local_impulse[1], (float)local_impulse[2] });
    b3Pos point = b3TransformWorldPoint(xf, (b3Vec3){ (float)local_point[0], (float)local_point[1], (float)local_point[2] });
    b3Body_ApplyLinearImpulse(b->body, impulse, point, wake);
}

void phys_body_apply_world_impulse(phys_scene *s, int32_t id, const double point[3], const double impulse[3]) {
    phys_body *b = phys_body_get(s, id);
    if (!b) return;
    b3Body_ApplyLinearImpulse(b->body, (b3Vec3){ (float)impulse[0], (float)impulse[1], (float)impulse[2] },
                              (b3Pos){ point[0], point[1], point[2] }, true);
}

double phys_scene_gravity_scale(phys_scene *s) {
    const double *g = s->cfg.gravity;
    return sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]) / 9.81;
}

void phys_body_apply_impulse_torque(phys_scene *s, int32_t id, const double local_impulse[3], const double local_torque[3], bool wake) {
    phys_body *b = body_req(s, id, "施加冲量与角冲量");
    if (!wake && !b3Body_IsAwake(b->body)) return;
    b3WorldTransform xf = b3Body_GetTransform(b->body);
    b3Vec3 impulse = b3RotateVector(xf.q, (b3Vec3){ (float)local_impulse[0], (float)local_impulse[1], (float)local_impulse[2] });
    b3Vec3 torque = b3RotateVector(xf.q, (b3Vec3){ (float)local_torque[0], (float)local_torque[1], (float)local_torque[2] });
    b3Body_ApplyLinearImpulse(b->body, impulse, xf.p, wake);
    b3Body_ApplyAngularImpulse(b->body, torque, wake);
}

void phys_body_get_linear_velocity(phys_scene *s, int32_t id, double out[3]) {
    b3Vec3 v = b3Body_GetLinearVelocity(body_req(s, id, "读线速度")->body);
    out[0] = v.x; out[1] = v.y; out[2] = v.z;
}

void phys_body_get_angular_velocity(phys_scene *s, int32_t id, double out[3]) {
    b3Vec3 v = b3Body_GetAngularVelocity(body_req(s, id, "读角速度")->body);
    out[0] = v.x; out[1] = v.y; out[2] = v.z;
}

/* ------------------------------------------------------------------ */
/* 地形格                                                              */
/* ------------------------------------------------------------------ */

/* 地形格包围盒：水平覆盖整格，竖直覆盖存储的整个高度 */
static void cell_update_bounds(phys_scene *s, phys_cell *c) {
    const float half = (float)(1 << (PHYS_CELL_SHIFT - 1));
    b3AABB box = {
        { -half, (float)(s->terrain->min_sy * 16 - c->origin[1]), -half },
        { half, (float)((s->terrain->min_sy + s->terrain->nsec) * 16 - c->origin[1]), half },
    };
    b3Shape_SetVoxelBounds(c->shape, box);
}

static void cell_acquire(phys_scene *s, int32_t chx, int32_t chz) {
    int32_t cx = floor_div(chx, PHYS_CELL_SHIFT - 4), cz = floor_div(chz, PHYS_CELL_SHIFT - 4);
    uint64_t key = mcc_key2(cx, cz);
    phys_cell *c = (phys_cell*)mcc_hmap_get(&s->cells, key);
    if (!c) {
        c = (phys_cell*)mcc_xcalloc(1, sizeof *c);
        c->kind = PHYS_KIND_CELL;
        c->scene = s;
        c->cx = cx;
        c->cz = cz;
        const int32_t size = 1 << PHYS_CELL_SHIFT;
        c->origin[0] = (double)cx * size + size / 2;
        c->origin[1] = 0.0;
        c->origin[2] = (double)cz * size + size / 2;
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_staticBody;
        bd.position = (b3Pos){ c->origin[0], c->origin[1], c->origin[2] };
        bd.userData = c;
        c->body = b3CreateBody(s->world, &bd);
        b3ShapeDef sd = b3DefaultShapeDef();
        sd.baseMaterial.friction = SUBLEVEL_BASE_FRICTION;
        sd.userData = c;
        b3VoxelShape vs = { c, { { -1, -1, -1 }, { 1, 1, 1 } }, false };
        c->shape = b3CreateVoxelShape(c->body, &sd, &vs);
        mcc_hmap_put(&s->cells, key, c);
        cell_update_bounds(s, c);
    }
    c->chunks++;
}

static void cell_release(phys_scene *s, int32_t chx, int32_t chz) {
    int32_t cx = floor_div(chx, PHYS_CELL_SHIFT - 4), cz = floor_div(chz, PHYS_CELL_SHIFT - 4);
    uint64_t key = mcc_key2(cx, cz);
    phys_cell *c = (phys_cell*)mcc_hmap_get(&s->cells, key);
    if (!c) return;
    if (--c->chunks <= 0) {
        mcc_hmap_remove(&s->cells, key);
        b3DestroyBody(c->body);
        free(c);
    }
}

/* ------------------------------------------------------------------ */
/* 推进                                                                */
/* ------------------------------------------------------------------ */

void phys_buoyancy_compute(phys_scene *s); /* buoyancy.cpp */

void phys_scene_tick(phys_scene *s) {
    uint64_t t0 = mcc_nanotime();
    /* 上一刻里方块有增删的结构：质心、包围盒、质量更新后再算浮力 */
    uint32_t it = 0;
    phys_body *b;
    while (phys_scene_next_body(s, &it, &b))
        if (b->mass && b->mass->dirty) apply_mass(b);
    /* 关节帧更新（joints.cpp）与绳索在后续移植中接入 */
    phys_buoyancy_compute(s);
    s->stat_tick_ns += mcc_nanotime() - t0;
}

static void gather_collisions(phys_scene *s, double dt) {
    uint32_t it = 0;
    void *v;
    while (s->collisions.len < MAX_REPORTED_COLLISIONS && mcc_hmap_next(&s->bodies, &it, NULL, &v)) {
        phys_body *b = (phys_body*)v;
        if (!b3Body_IsAwake(b->body)) continue;
        int cap = b3Body_GetContactCapacity(b->body);
        if (cap > s->contact_cap) {
            s->contact_cap = cap * 2;
            s->contact_buf = (b3ContactData*)mcc_xrealloc(s->contact_buf, sizeof(b3ContactData) * (size_t)s->contact_cap);
        }
        int n = b3Body_GetContactData(b->body, s->contact_buf, s->contact_cap);
        for (int c = 0; c < n && s->collisions.len < MAX_REPORTED_COLLISIONS; c++) {
            b3ContactData *cd = &s->contact_buf[c];
            void *ua = b3Shape_GetUserData(cd->shapeIdA), *ub = b3Shape_GetUserData(cd->shapeIdB);
            if (!ua || !ub) continue;
            int ka = *(int *)ua, kb = *(int *)ub;
            /* 两个子世界之间的接触只从 A 侧上报一次 */
            if (ka == PHYS_KIND_BODY && kb == PHYS_KIND_BODY && ((phys_body *)ua)->id != b->id) continue;
            double impulse = 0;
            for (int m = 0; m < cd->manifoldCount; m++)
                for (int p = 0; p < cd->manifolds[m].pointCount; p++) impulse += cd->manifolds[m].points[p].normalImpulse;
            double force = impulse / dt;
            if (force < 0.1) continue;
            b3WorldTransform xa = b3Body_GetTransform(b3Shape_GetBody(cd->shapeIdA));
            b3WorldTransform xb = b3Body_GetTransform(b3Shape_GetBody(cd->shapeIdB));
            for (int m = 0; m < cd->manifoldCount && s->collisions.len < MAX_REPORTED_COLLISIONS; m++) {
                const b3Manifold *mf = &cd->manifolds[m];
                b3Vec3 na = b3InvRotateVector(xa.q, mf->normal);
                b3Vec3 nb = b3InvRotateVector(xb.q, b3Neg(mf->normal));
                for (int p = 0; p < mf->pointCount && s->collisions.len < MAX_REPORTED_COLLISIONS; p++) {
                    phys_collision col = {0};
                    col.body_a = ka == PHYS_KIND_BODY ? ((phys_body *)ua)->id : -1;
                    col.body_b = kb == PHYS_KIND_BODY ? ((phys_body *)ub)->id : -1;
                    col.force = force;
                    b3Vec3 pa = b3InvRotateVector(xa.q, mf->points[p].anchorA);
                    b3Vec3 pb = b3InvRotateVector(xb.q, mf->points[p].anchorB);
                    col.normal_a[0] = na.x; col.normal_a[1] = na.y; col.normal_a[2] = na.z;
                    col.normal_b[0] = nb.x; col.normal_b[1] = nb.y; col.normal_b[2] = nb.z;
                    /* 子世界给相对质心的局部点；地形给世界坐标 */
                    if (ka == PHYS_KIND_BODY) { col.point_a[0] = pa.x; col.point_a[1] = pa.y; col.point_a[2] = pa.z; }
                    else { col.point_a[0] = xa.p.x + pa.x; col.point_a[1] = xa.p.y + pa.y; col.point_a[2] = xa.p.z + pa.z; }
                    if (kb == PHYS_KIND_BODY) { col.point_b[0] = pb.x; col.point_b[1] = pb.y; col.point_b[2] = pb.z; }
                    else { col.point_b[0] = xb.p.x + pb.x; col.point_b[1] = xb.p.y + pb.y; col.point_b[2] = xb.p.z + pb.z; }
                    mcc_vec_push(&s->collisions, col);
                }
            }
        }
    }
}

void phys_scene_step(phys_scene *s, double dt) {
    /* 浮力/阻力每个游戏刻算一次，Box3D 每步清空外力，所以每个子步重新施加 */
    uint32_t it = 0;
    void *v;
    while (mcc_hmap_next(&s->bodies, &it, NULL, &v)) {
        phys_body *b = (phys_body*)v;
        if (!b->has_tick_force) continue;
        b3WorldTransform xf = b3Body_GetTransform(b->body);
        b3Body_ApplyForce(b->body, b->tick_force, xf.p, false);
        b3Body_ApplyTorque(b->body, b->tick_torque, false);
    }
    uint64_t t0 = mcc_nanotime();
    b3World_Step(s->world, (float)dt, s->tuning.substeps);
    s->stat_step_ns += mcc_nanotime() - t0;
    gather_collisions(s, dt);
}

size_t phys_scene_drain_collisions(phys_scene *s, phys_collision *out, size_t cap) {
    size_t n = MCC_MIN(cap, s->collisions.len);
    memcpy(out, s->collisions.data, n * sizeof *out);
    s->collisions.len = 0;
    return n;
}

void phys_scene_stats(phys_scene *s, phys_stats *out) {
    out->bodies = s->bodies.count;
    out->cells = s->cells.count;
    out->terrain_chunks = s->terrain ? s->terrain->chunks.count : 0;
    out->manifolds = atomic_load(&s->stat_manifolds);
    out->pairs = atomic_load(&s->stat_pairs);
    out->collide_ns = atomic_load(&s->stat_collide_ns);
    out->step_ns = s->stat_step_ns;
    out->tick_ns = s->stat_tick_ns;
}

/* 给 collide.cpp 用的统计入口 */
void phys_scene_count_time(phys_scene *s, uint64_t ns) {
    atomic_fetch_add_explicit(&s->stat_collide_ns, ns, memory_order_relaxed);
}

void phys_scene_count(phys_scene *s, uint64_t manifolds, uint64_t pairs) {
    atomic_fetch_add_explicit(&s->stat_manifolds, manifolds, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->stat_pairs, pairs, memory_order_relaxed);
}
