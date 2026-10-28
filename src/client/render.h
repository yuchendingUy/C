/*
 * 画面：暗调的室内大厅。天花板上一排排灯（布局和 src/debug/main.cpp 的仓库一致），
 * 着色器按最近的几盏灯算聚光、加阴影和雾，远处没入黑暗；灯有光晕和光柱。
 *
 * 顶点的 texcoord：x = 环境光遮蔽（0~1），y = 材质样式（见 STYLE_*）
 */
#ifndef MCC_CLIENT_RENDER_H
#define MCC_CLIENT_RENDER_H

#include "raylib.h"

namespace mcc::client {

enum {
    STYLE_BLOCK = 0, STYLE_GRASS, STYLE_LEAVES, STYLE_PLANT, STYLE_LOG, STYLE_PLANKS, STYLE_GLASS, STYLE_SAND,
    STYLE_CONCRETE, STYLE_PAINTED, STYLE_LAMP, STYLE_BOX,
    STYLE_SMOOTH = 20, /* 机械零件、手里的东西：不加纹理 */
    STYLE_WATER = 21,
};

struct Render {
    Shader lit, depth;
    Material lit_mat, depth_mat;
    Matrix light_vp;
};
extern Render g_render;

void render_init();
/* 每帧开头：相机位置、时间 */
void render_frame(Vector3 cam, float time);
/* 阴影：在 begin/end 之间画会投影的东西（用 depth_mat 或深度着色器） */
void render_shadow_begin(Vector3 center);
void render_shadow_end();
unsigned render_shadow_texture();
/* 灯的光晕和光柱（半透明，最后画） */
void render_lamp_glow(Camera3D cam);
Color render_fog_color();
/* 着色器公共部分（实例化结构的着色器也要用） */
extern const char *LIGHT_GLSL;
void render_set_common(Shader s);

} // namespace mcc::client

#endif
