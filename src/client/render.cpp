#include "render.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <string>

namespace mcc::client {

Render g_render;
Vector4 g_flash;

/* ------------------------------------------------------------------ */
/* 灯光布局：灯在 y=99、x 和 z 都是 16 的倍数，柱子所在的格子（x、z 除以 16 都是奇数）没有灯 */
/* ------------------------------------------------------------------ */

static const float LAMP_Y = 99, LAMP_STEP = 16, FLOOR_Y = 65;
static const Vector3 LAMP_COL{ 1.5f, 1.35f, 1.12f }, AMB_UP{ 0.05f, 0.055f, 0.065f }, AMB_DOWN{ 0.07f, 0.064f, 0.056f };
static const Vector3 FOG_COL{ 0.018f, 0.02f, 0.025f };
static const float FOG_NEAR = 18, FOG_FAR = 150;
/* 阴影用一个近乎竖直的方向光代替头顶的灯 */
static const Vector3 SHADOW_DIR{ 0.08f, 1.0f, 0.13f };

const char *LIGHT_GLSL = R"(
uniform vec3 viewPos, lightDir, lampCol, ambUp, ambDown, fogCol;
uniform vec2 fogRange;
uniform mat4 lightVP;
uniform sampler2D texture1;
uniform float shadowTexel, time;
uniform vec4 flash;

const float LAMP_Y = 99.0, LAMP_STEP = 16.0;

float hash3(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}
float noise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash3(vec3(i, 1.0)), b = hash3(vec3(i + vec2(1, 0), 1.0));
    float c = hash3(vec3(i + vec2(0, 1), 1.0)), d = hash3(vec3(i + vec2(1, 1), 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float shadowAt(vec3 wp, vec3 n) {
    vec4 lp = lightVP * vec4(wp + n * 0.06, 1.0);
    vec3 c = lp.xyz / lp.w * 0.5 + 0.5;
    if (c.x < 0.0 || c.x > 1.0 || c.y < 0.0 || c.y > 1.0 || c.z > 1.0) return 1.0;
    float s = 0.0;
    for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++)
            s += c.z - 0.0005 > texture(texture1, c.xy + vec2(i, j) * shadowTexel).r ? 0.0 : 1.0;
    float edge = max(abs(c.x - 0.5), abs(c.y - 0.5)) * 2.0;
    return mix(s / 9.0, 1.0, smoothstep(0.85, 1.0, edge));
}
/* 附近 3×3 盏灯的聚光 */
vec3 lamps(vec3 wp, vec3 n) {
    vec2 g = floor(wp.xz / LAMP_STEP + 0.5);
    vec3 sum = vec3(0.0);
    for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++) {
            vec2 c = g + vec2(i, j);
            if (mod(c.x, 2.0) == 1.0 && mod(c.y, 2.0) == 1.0) continue;   /* 柱子 */
            vec3 lp = vec3(c.x * LAMP_STEP + 0.5, LAMP_Y - 0.1, c.y * LAMP_STEP + 0.5);
            vec3 d = lp - wp;
            float dist = length(d);
            vec3 l = d / dist;
            float cone = smoothstep(0.945, 0.995, l.y);             /* 光锥约 19° */
            float soft = 0.16 * smoothstep(0.35, 0.95, l.y);         /* 光锥外一圈散光，照亮侧面 */
            float atten = 1300.0 / (dist * dist + 1300.0);
            sum += lampCol * (cone + soft) * atten * max(dot(n, l), 0.0);
        }
    return sum;
}
vec3 lighting(vec3 base, vec3 wp, vec3 n, float ao, bool shadow) {
    vec3 direct = lamps(wp, n);
    float sh = shadow && dot(n, lightDir) > -0.2 ? shadowAt(wp, n) : 1.0;
    vec3 amb = mix(ambDown, ambUp, n.y * 0.5 + 0.5);
    if (flash.w > 0.0) {                                  /* 爆炸火光 */
        vec3 d = flash.xyz - wp;
        float dist2 = dot(d, d);
        direct += vec3(1.0, 0.55, 0.2) * flash.w * max(dot(n, d * inversesqrt(dist2 + 1e-4)), 0.0) * 30.0 / (dist2 + 30.0);
    }
    return base * (amb * ao + direct * mix(0.15, 1.0, sh) * (0.6 + 0.4 * ao));
}
vec3 applyFog(vec3 c, vec3 wp, float k) {
    float f = smoothstep(fogRange.x, fogRange.y, length(wp - viewPos)) * k;
    return mix(c, fogCol, f);
}
/* 方块的像素纹理：每格 16×16 像素，按样式变化 */
vec3 blockTexture(vec3 base, vec3 wp, vec3 n, float style) {
    vec3 a = abs(n);
    vec3 inner = wp - n * 0.002;
    vec2 uv = a.x > 0.5 ? inner.zy : a.y > 0.5 ? inner.xz : inner.xy;
    vec2 f = fract(uv);
    vec2 px = floor(f * 16.0);
    vec3 cell = floor(inner);
    float side = a.y > 0.5 ? 0.0 : 1.0;
    float h = hash3(vec3(px, side * 7.0 + style * 13.0));
    float h4 = hash3(vec3(floor(px / 4.0), 91.0 + style));
    float tint = hash3(cell) * 0.06 - 0.03;
    vec3 c = base * (0.86 + 0.16 * h + 0.06 * (h4 - 0.5) + tint);
    int st = int(style + 0.5);
    float e = min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y));
    if (st == 8) {                       /* 混凝土：大块的污渍、竖直的水渍；地面画测量网格 */
        float stain = noise2(uv * 0.15) * 0.6 + noise2(uv * 0.6) * 0.4;
        c = base * (0.82 + 0.25 * stain + 0.05 * h);
        if (side > 0.5) c *= 0.92 + 0.12 * noise2(vec2(uv.x * 3.0, uv.y * 0.12));
        else {
            vec2 g4 = abs(fract(uv / 4.0 + 0.5) - 0.5) * 4.0;
            float line = min(g4.x, g4.y);
            if (e < 0.03) c *= 1.1;                          /* 每格一条细线 */
            if (line < 0.05) c *= 1.18;                      /* 每 4 格一条粗线 */
        }
        return c;
    }
    if (st == 9) return base * (0.93 + 0.07 * h) * (e < 0.04 ? 0.8 : 1.0);   /* 刷漆的钢材、标线 */
    if (st == 11) {                      /* 纸箱：中间一道胶带 */
        c = base * (0.9 + 0.1 * h);
        if (abs(f.x - 0.5) < 0.09 && side < 0.5) c = base * 1.12;
        return c * (e < 0.04 ? 0.75 : 1.0);
    }
    if (st == 4) {                       /* 原木：侧面竖纹，截面年轮 */
        if (side > 0.5) c = base * (0.75 + 0.3 * hash3(vec3(px.x, 1.0, 2.0)) + 0.08 * h);
        else {
            float r = length(f - 0.5) * 16.0;
            c = (r > 6.5 ? base : vec3(0.72, 0.58, 0.36)) * (0.85 + 0.15 * step(0.5, fract(r * 0.5)) + 0.05 * h);
        }
    } else if (st == 5) {                /* 木板：横向四块木条 */
        float row = floor(px.y / 4.0);
        c = base * (0.9 + 0.1 * hash3(vec3(row, cell.x + cell.z, 4.0)) + 0.06 * h);
        if (mod(px.y, 4.0) == 0.0 || (mod(px.x + row * 5.0, 16.0) == 0.0)) c *= 0.72;
    } else if (st == 6) {                /* 玻璃：亮框 + 两道反光 */
        bool frame = px.x == 0.0 || px.y == 0.0 || px.x == 15.0 || px.y == 15.0;
        bool glint = abs(px.x - px.y - 3.0) < 1.0 || abs(px.x - px.y + 4.0) < 0.6;
        c = base * (frame ? 1.05 : 0.82) + (glint && !frame ? vec3(0.12) : vec3(0.0));
    }
    if (st != 6 && e < 1.0 / 16.0) c *= 0.9;
    return c;
}
)";

static const char *LIT_VS = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec4 vertexColor;
in vec2 vertexTexCoord;
uniform mat4 mvp, matModel, matNormal;
uniform float time;
out vec3 vPos;
out vec3 vNor;
out vec4 vCol;
out vec2 vTex;
void main() {
    vec3 p = vertexPosition;
    if (vertexTexCoord.y > 20.5)   /* 水面起伏 */
        p.y += 0.025 * sin(p.x * 1.3 + time * 1.1) + 0.02 * sin(p.z * 1.7 + time * 0.9) - 0.05;
    vPos = (matModel * vec4(p, 1.0)).xyz;
    vNor = normalize(mat3(matNormal) * vertexNormal);
    vCol = vertexColor;
    vTex = vertexTexCoord;
    gl_Position = mvp * vec4(p, 1.0);
}
)";

static const char *LIT_FS_MAIN = R"(
in vec3 vPos;
in vec3 vNor;
in vec4 vCol;
in vec2 vTex;
uniform vec4 colDiffuse;
out vec4 fragColor;
void main() {
    vec3 n = normalize(vNor);
    float style = vTex.y, ao = vTex.x;
    vec3 base = vCol.rgb * colDiffuse.rgb;
    if (style > 20.5) {                      /* 水：暗的，反着灯光 */
        vec3 wn = normalize(vec3(-0.04 * cos(vPos.x * 1.3 + time * 1.1) - 0.02 * cos(vPos.x * 3.1 - time * 1.7), 1.0,
                                 -0.035 * cos(vPos.z * 1.7 + time * 0.9) - 0.02 * cos(vPos.z * 2.7 + time * 1.3)));
        vec3 v = normalize(viewPos - vPos);
        float fres = pow(1.0 - max(dot(wn, v), 0.0), 4.0);
        vec3 body = lighting(base * 0.35, vPos, vec3(0.0, 1.0, 0.0), 1.0, true);
        /* 倒影里的灯：反射光线打到天花板的位置离灯多近 */
        vec3 r = reflect(-v, wn);
        vec2 hit = vPos.xz + r.xz / max(r.y, 0.02) * (LAMP_Y - vPos.y);
        vec2 g = floor(hit / LAMP_STEP + 0.5);
        float lampHere = mod(g.x, 2.0) == 1.0 && mod(g.y, 2.0) == 1.0 ? 0.0 : 1.0;
        float d = length(hit - (g * LAMP_STEP + 0.5));
        float glint = lampHere * (smoothstep(1.0, 0.4, d) * 2.5 + exp(-d * d / 10.0) * 0.25) * step(0.0, r.y);
        vec3 c = body + lampCol * glint + fogCol * fres;
        fragColor = vec4(applyFog(c, vPos, 1.0), mix(0.75, 0.95, fres));
        return;
    }
    if (int(style + 0.5) == 10) {            /* 灯：自己发光 */
        fragColor = vec4(applyFog(base * 1.6, vPos, 0.35), 1.0);
        return;
    }
    if (style < 19.5) {
        if (int(style + 0.5) == 3) {         /* 草、花：十字面片，按像素挖空成一根根草叶 */
            vec2 uv = vec2(fract(vPos.x), fract(vPos.y));
            vec2 px = floor(uv * 16.0);
            if (px.y / 16.0 > hash3(vec3(px.x, 1.0, 9.0)) * 0.9 + 0.1) discard;
        } else base = blockTexture(base, vPos, n, style);
    }
    vec3 c = lighting(base, vPos, n, 0.35 + 0.65 * ao, true);
    fragColor = vec4(applyFog(c, vPos, 1.0), 1.0);
}
)";

/* 深度：天花板一层不投影，不然全场都在阴影里 */
static const char *DEPTH_VS = R"(#version 330
in vec3 vertexPosition;
uniform mat4 mvp, matModel;
out float vY;
void main() { vY = (matModel * vec4(vertexPosition, 1.0)).y; gl_Position = mvp * vec4(vertexPosition, 1.0); }
)";
static const char *DEPTH_FS = R"(#version 330
in float vY;
void main() { if (vY > 97.0) discard; }
)";

/* 光柱：开口的锥面，正对镜头的地方亮、边缘和底部淡出，没有硬边 */
static const char *SHAFT_VS = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec2 vertexTexCoord;
uniform mat4 mvp, matModel, matNormal;
out vec3 vPos;
out vec3 vNor;
out float vV;
void main() {
    vPos = (matModel * vec4(vertexPosition, 1.0)).xyz;
    vNor = normalize(mat3(matNormal) * vertexNormal);
    vV = vertexTexCoord.y;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";
static const char *SHAFT_FS = R"(#version 330
in vec3 vPos;
in vec3 vNor;
in float vV;
uniform vec3 viewPos;
uniform vec4 colDiffuse;
out vec4 fragColor;
void main() {
    vec3 v = normalize(viewPos - vPos);
    float face = abs(dot(normalize(vNor), v));
    float a = face * face;                                          /* 掠射的边缘几乎看不见 */
    a *= smoothstep(0.0, 0.08, vV) * pow(1.0 - vV, 1.6);             /* 灯口和地面都淡出 */
    a *= smoothstep(1.5, 6.0, length(viewPos - vPos));               /* 镜头穿过光柱时不糊一脸 */
    fragColor = vec4(colDiffuse.rgb * a * colDiffuse.a, 1.0);
}
)";

/* 光晕：朝着镜头的方片，径向渐变 */
static const char *GLOW_VS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 vUv;
out vec4 vCol;
void main() { vUv = vertexTexCoord; vCol = vertexColor; gl_Position = mvp * vec4(vertexPosition, 1.0); }
)";
static const char *GLOW_FS = R"(#version 330
in vec2 vUv;
in vec4 vCol;
out vec4 fragColor;
void main() {
    float d = length(vUv - 0.5) * 2.0;
    float g = exp(-d * d * 5.0) * (1.0 - smoothstep(0.85, 1.0, d));
    fragColor = vec4(vCol.rgb * g * vCol.a, 1.0);
}
)";

/* ------------------------------------------------------------------ */

static const int SHADOW_SIZE = 4096;
static const float SHADOW_R = 64;
static unsigned g_shadow_fbo, g_shadow_tex;
static Shader g_glow, g_shaft;
static Mesh g_cone;
static Material g_shaft_mat;

/* 开口锥面：顶点 y 从 0（灯口，半径 r0）到 -1（地面，半径 r1），texcoord.y 0→1 */
static Mesh cone_mesh(float r0, float r1, int seg) {
    Mesh m = {};
    m.vertexCount = (seg + 1) * 2;
    m.triangleCount = seg * 2;
    m.vertices = (float *)MemAlloc((unsigned)(m.vertexCount * 3 * sizeof(float)));
    m.normals = (float *)MemAlloc((unsigned)(m.vertexCount * 3 * sizeof(float)));
    m.texcoords = (float *)MemAlloc((unsigned)(m.vertexCount * 2 * sizeof(float)));
    m.indices = (unsigned short *)MemAlloc((unsigned)(m.triangleCount * 3 * sizeof(unsigned short)));
    float slope = r1 - r0; /* 法线要考虑锥面的斜度 */
    for (int i = 0; i <= seg; i++) {
        float a = (float)i / seg * 2 * PI, c = cosf(a), s = sinf(a);
        Vector3 n = Vector3Normalize(Vector3{ c, slope, s });
        float *v = m.vertices + i * 6, *nn = m.normals + i * 6, *t = m.texcoords + i * 4;
        v[0] = c * r0; v[1] = 0; v[2] = s * r0;
        v[3] = c * r1; v[4] = -1; v[5] = s * r1;
        nn[0] = nn[3] = n.x; nn[1] = nn[4] = n.y; nn[2] = nn[5] = n.z;
        t[0] = t[2] = (float)i / seg; t[1] = 0; t[3] = 1;
    }
    for (int i = 0; i < seg; i++) {
        unsigned short a = (unsigned short)(i * 2), b = (unsigned short)(i * 2 + 1), c = (unsigned short)(i * 2 + 2), d = (unsigned short)(i * 2 + 3);
        unsigned short *ix = m.indices + i * 6;
        ix[0] = a; ix[1] = b; ix[2] = d; ix[3] = a; ix[4] = d; ix[5] = c;
    }
    UploadMesh(&m, false);
    return m;
}

static void set_v3(Shader s, const char *name, Vector3 v) { SetShaderValue(s, GetShaderLocation(s, name), &v, SHADER_UNIFORM_VEC3); }

void render_set_common(Shader s) {
    set_v3(s, "lightDir", Vector3Normalize(SHADOW_DIR));
    set_v3(s, "lampCol", LAMP_COL);
    set_v3(s, "ambUp", AMB_UP);
    set_v3(s, "ambDown", AMB_DOWN);
    set_v3(s, "fogCol", FOG_COL);
    Vector2 fr{ FOG_NEAR, FOG_FAR };
    SetShaderValue(s, GetShaderLocation(s, "fogRange"), &fr, SHADER_UNIFORM_VEC2);
    float tx = 1.f / SHADOW_SIZE;
    SetShaderValue(s, GetShaderLocation(s, "shadowTexel"), &tx, SHADER_UNIFORM_FLOAT);
}

void render_init() {
    std::string fs = std::string("#version 330\n") + LIGHT_GLSL + LIT_FS_MAIN;
    g_render.lit = LoadShaderFromMemory(LIT_VS, fs.c_str());
    g_render.depth = LoadShaderFromMemory(DEPTH_VS, DEPTH_FS);
    g_glow = LoadShaderFromMemory(GLOW_VS, GLOW_FS);
    g_shaft = LoadShaderFromMemory(SHAFT_VS, SHAFT_FS);
    g_shaft_mat = LoadMaterialDefault();
    g_shaft_mat.shader = g_shaft;
    g_cone = cone_mesh(0.6f, 9.0f, 40);
    render_set_common(g_render.lit);

    g_shadow_fbo = rlLoadFramebuffer();
    g_shadow_tex = rlLoadTextureDepth(SHADOW_SIZE, SHADOW_SIZE, false);
    rlFramebufferAttach(g_shadow_fbo, g_shadow_tex, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(g_shadow_fbo)) TraceLog(LOG_WARNING, "阴影帧缓冲不完整");

    /* 注意 Material 的 maps 是指针：拷贝 Material 会共用同一张表 */
    g_render.lit_mat = LoadMaterialDefault();
    g_render.lit_mat.shader = g_render.lit;
    g_render.lit_mat.maps[MATERIAL_MAP_SPECULAR].texture = Texture2D{ g_shadow_tex, SHADOW_SIZE, SHADOW_SIZE, 1, 0 };
    g_render.depth_mat = LoadMaterialDefault();
    g_render.depth_mat.shader = g_render.depth;
}

void render_frame(Vector3 cam, float time) {
    SetShaderValue(g_render.lit, GetShaderLocation(g_render.lit, "viewPos"), &cam, SHADER_UNIFORM_VEC3);
    SetShaderValue(g_render.lit, GetShaderLocation(g_render.lit, "time"), &time, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shaft, GetShaderLocation(g_shaft, "viewPos"), &cam, SHADER_UNIFORM_VEC3);
    SetShaderValue(g_render.lit, GetShaderLocation(g_render.lit, "flash"), &g_flash, SHADER_UNIFORM_VEC4);
}

Color render_fog_color() { return Color{ (unsigned char)(FOG_COL.x * 255), (unsigned char)(FOG_COL.y * 255), (unsigned char)(FOG_COL.z * 255), 255 }; }
unsigned render_shadow_texture() { return g_shadow_tex; }

void render_shadow_begin(Vector3 center) {
    /* 光源视角按阴影贴图的像素对齐，走动时阴影边不闪 */
    Vector3 dir = Vector3Normalize(SHADOW_DIR);
    Vector3 fwd = Vector3Negate(dir), up{ 0, 0, 1 };
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, up));
    Vector3 vup = Vector3CrossProduct(right, fwd);
    float texel = 2 * SHADOW_R / SHADOW_SIZE;
    center.y = FLOOR_Y;
    float r = Vector3DotProduct(center, right), u = Vector3DotProduct(center, vup);
    center = Vector3Add(center, Vector3Add(Vector3Scale(right, floorf(r / texel) * texel - r), Vector3Scale(vup, floorf(u / texel) * texel - u)));
    Matrix view = MatrixLookAt(Vector3Add(center, Vector3Scale(dir, 60)), center, up);
    Matrix proj = MatrixOrtho(-SHADOW_R, SHADOW_R, -SHADOW_R, SHADOW_R, 1, 120);
    g_render.light_vp = MatrixMultiply(view, proj);
    SetShaderValueMatrix(g_render.lit, GetShaderLocation(g_render.lit, "lightVP"), g_render.light_vp);

    /* 和 BeginMode3D 一样：投影矩阵压栈，模型视图矩阵直接换 */
    rlDrawRenderBatchActive();
    rlEnableFramebuffer(g_shadow_fbo);
    rlViewport(0, 0, SHADOW_SIZE, SHADOW_SIZE);
    rlClearScreenBuffers();
    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(proj));
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(view));
    rlEnableDepthTest();
    rlSetCullFace(RL_CULL_FACE_FRONT);
}

void render_shadow_end() {
    rlDrawRenderBatchActive();
    rlSetCullFace(RL_CULL_FACE_BACK);
    rlMatrixMode(RL_PROJECTION);
    rlPopMatrix();
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlDisableFramebuffer();
    rlViewport(0, 0, GetRenderWidth(), GetRenderHeight());
}

/* 柔边光团（叠加混合，不写深度）：爆炸火球用 */
void render_glow(Camera3D cam, Vector3 c, float size, Color col) {
    Vector3 f = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, cam.up)), u = Vector3CrossProduct(r, f);
    rlDrawRenderBatchActive();
    BeginBlendMode(BLEND_ADDITIVE);
    rlDisableDepthMask();
    rlDisableBackfaceCulling();
    BeginShaderMode(g_glow);
    rlBegin(RL_QUADS);
    rlColor4ub(col.r, col.g, col.b, col.a);
    static const float UV[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int v = 0; v < 4; v++) {
        Vector3 p = Vector3Add(c, Vector3Add(Vector3Scale(r, (UV[v][0] * 2 - 1) * size), Vector3Scale(u, (UV[v][1] * 2 - 1) * size)));
        rlTexCoord2f(UV[v][0], UV[v][1]);
        rlVertex3f(p.x, p.y, p.z);
    }
    rlEnd();
    EndShaderMode();
    rlEnableBackfaceCulling();
    rlEnableDepthMask();
    EndBlendMode();
}

/* 每盏灯：一根往下散开的光柱 + 一圈光晕（叠加混合，不写深度） */
void render_lamp_glow(Camera3D cam) {
    Vector3 f = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, cam.up)), u = Vector3CrossProduct(r, f);
    rlDrawRenderBatchActive();
    BeginBlendMode(BLEND_ADDITIVE);
    rlDisableDepthMask();
    rlDisableBackfaceCulling();
    int cx = (int)floorf(cam.position.x / LAMP_STEP), cz = (int)floorf(cam.position.z / LAMP_STEP);
    const int R = 8;
    auto lamp = [](int i, int j) { return !((i & 1) && (j & 1)); };
    for (int i = cx - R; i <= cx + R + 1; i++)
        for (int j = cz - R; j <= cz + R + 1; j++) {
            if (!lamp(i, j)) continue;
            Vector3 top{ i * LAMP_STEP + 0.5f, LAMP_Y - 0.05f, j * LAMP_STEP + 0.5f };
            float k = 1 - fminf(1, Vector3Distance(top, cam.position) / 140.f);
            if (k <= 0) continue;
            g_shaft_mat.maps[MATERIAL_MAP_DIFFUSE].color = Color{ 255, 238, 205, (unsigned char)(80 * k * k) };
            DrawMesh(g_cone, g_shaft_mat, MatrixMultiply(MatrixScale(1, LAMP_Y - FLOOR_Y, 1), MatrixTranslate(top.x, top.y, top.z)));
        }
    rlDrawRenderBatchActive();
    BeginShaderMode(g_glow);
    rlBegin(RL_QUADS);
    for (int i = cx - R; i <= cx + R + 1; i++)
        for (int j = cz - R; j <= cz + R + 1; j++) {
            if (!lamp(i, j)) continue;
            Vector3 c{ i * LAMP_STEP + 0.5f, LAMP_Y - 0.6f, j * LAMP_STEP + 0.5f };
            float k = 1 - fminf(1, Vector3Distance(c, cam.position) / 170.f);
            if (k <= 0) continue;
            const float s = 3.2f;
            Vector3 q[4] = {
                Vector3Add(c, Vector3Add(Vector3Scale(r, -s), Vector3Scale(u, -s))), Vector3Add(c, Vector3Add(Vector3Scale(r, s), Vector3Scale(u, -s))),
                Vector3Add(c, Vector3Add(Vector3Scale(r, s), Vector3Scale(u, s))), Vector3Add(c, Vector3Add(Vector3Scale(r, -s), Vector3Scale(u, s))),
            };
            static const float UV[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
            rlColor4ub(255, 236, 200, (unsigned char)(150 * k));
            for (int v = 0; v < 4; v++) {
                rlTexCoord2f(UV[v][0], UV[v][1]);
                rlVertex3f(q[v].x, q[v].y, q[v].z);
            }
        }
    rlEnd();
    EndShaderMode();
    rlEnableBackfaceCulling();
    rlEnableDepthMask();
    EndBlendMode();
}

} // namespace mcc::client
