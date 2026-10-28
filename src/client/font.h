/*
 * 中文字体：直接读系统字体（支持 .ttc 字体合集，例如 Windows 的微软雅黑 msyh.ttc），
 * 只烘焙用得到的字，生成 raylib 的 Font。
 */
#ifndef MCC_CLIENT_FONT_H
#define MCC_CLIENT_FONT_H

#include "raylib.h"

namespace mcc::client {

/* 读系统中文字体并先烘焙 chars 里的字；找不到字体返回 false（只能显示英文） */
bool font_init(int px, const char *chars);
/* 返回字体；text 里有没烘焙过的字会先补上 */
const Font &font_get(const char *text);

} // namespace mcc::client

#endif
