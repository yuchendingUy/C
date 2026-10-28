#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <vector>

/* raylib 自带的 stb_truetype 是 static 的，这里单独编一份 */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "external/stb_truetype.h"
#pragma GCC diagnostic pop
#include "rlgl.h"

namespace mcc::client {

static const char *FONT_PATHS[] = {
#ifdef _WIN32
    "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/msyh.ttf", "C:/Windows/Fonts/Deng.ttf",
    "C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/simsun.ttc",
#else
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
#endif
};

static unsigned char *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc((size_t)*len);
    if (fread(d, 1, (size_t)*len, f) != (size_t)*len) { free(d); d = nullptr; }
    fclose(f);
    return d;
}

/* 字体文件常驻内存，遇到没烘焙过的字就重建图集（只在界面出现新字时发生） */
static unsigned char *g_data;
static stbtt_fontinfo g_info;
static int g_px;
static std::set<int> g_cps;
static Font g_font;
static bool g_has_font;

static void bake() {
    std::vector<int> v(g_cps.begin(), g_cps.end());
    float scale = stbtt_ScaleForPixelHeight(&g_info, (float)g_px);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_info, &ascent, &descent, &gap);

    int W = 1024, H = 64;
    std::vector<stbtt_packedchar> pc(v.size());
    std::vector<unsigned char> atlas;
    for (;;) {
        atlas.assign((size_t)W * H, 0);
        stbtt_pack_context ctx = {};
        stbtt_PackBegin(&ctx, atlas.data(), W, H, 0, 1, nullptr);
        stbtt_PackSetOversampling(&ctx, 1, 1);
        stbtt_pack_range r = {};
        r.font_size = (float)g_px;
        r.array_of_unicode_codepoints = v.data();
        r.num_chars = (int)v.size();
        r.chardata_for_range = pc.data();
        std::vector<stbrp_rect> rects(v.size());
        int k = stbtt_PackFontRangesGatherRects(&ctx, &g_info, &r, 1, rects.data());
        stbtt_PackFontRangesPackRects(&ctx, rects.data(), k);
        bool all = true;
        for (int i = 0; i < k; i++) all &= rects[i].was_packed != 0;
        if (all) stbtt_PackFontRangesRenderIntoRects(&ctx, &g_info, &r, 1, rects.data());
        stbtt_PackEnd(&ctx);
        if (all) break;
        H *= 2;
        if (H > 8192) return;
    }

    /* 灰度 → 白色带透明度 */
    Image img = GenImageColor(W, H, BLANK);
    Color *px = (Color *)img.data;
    for (int i = 0; i < W * H; i++) px[i] = Color{ 255, 255, 255, atlas[i] };
    if (g_has_font) {
        rlDrawRenderBatchActive(); /* 旧图集可能还在批里没画 */
        UnloadFont(g_font);
    }
    Font f = {};
    f.baseSize = g_px;
    f.glyphCount = (int)v.size();
    f.glyphPadding = 0;
    f.texture = LoadTextureFromImage(img);
    SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
    UnloadImage(img);
    f.recs = (Rectangle *)MemAlloc((unsigned)(sizeof(Rectangle) * v.size()));
    f.glyphs = (GlyphInfo *)MemAlloc((unsigned)(sizeof(GlyphInfo) * v.size()));
    for (size_t i = 0; i < v.size(); i++) {
        const stbtt_packedchar &c = pc[i];
        f.recs[i] = Rectangle{ (float)c.x0, (float)c.y0, (float)(c.x1 - c.x0), (float)(c.y1 - c.y0) };
        f.glyphs[i].value = v[i];
        f.glyphs[i].offsetX = (int)c.xoff;
        f.glyphs[i].offsetY = (int)(c.yoff + ascent * scale);
        f.glyphs[i].advanceX = (int)(c.xadvance + 0.5f);
        f.glyphs[i].image = Image{};
    }
    g_font = f;
    g_has_font = true;
}

bool font_init(int px, const char *chars) {
    long len = 0;
    for (const char *p : FONT_PATHS)
        if ((g_data = read_file(p, &len))) break;
    if (!g_data) return false;
    int offset = stbtt_GetFontOffsetForIndex(g_data, 0); /* .ttc 取第一个字体 */
    if (offset < 0 || !stbtt_InitFont(&g_info, g_data, offset)) {
        free(g_data);
        g_data = nullptr;
        return false;
    }
    g_px = px;
    for (int c = 32; c < 127; c++) g_cps.insert(c);
    font_get(chars);
    if (!g_has_font) bake();
    return g_has_font;
}

const Font &font_get(const char *text) {
    if (!g_data) {
        static Font def = GetFontDefault();
        return def;
    }
    bool add = false;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        int n = 0;
        int cp = GetCodepointNext((const char *)p, &n);
        p += n > 0 ? n : 1;
        if (cp >= 32 && g_cps.insert(cp).second) add = true;
    }
    if (add) bake();
    return g_font;
}

} // namespace mcc::client
