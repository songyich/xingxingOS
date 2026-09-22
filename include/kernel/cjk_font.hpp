// ============================================================================
//  include/kernel/cjk_font.hpp —— 中文字库接口
//  ---------------------------------------------------------------------------
//  这个字库是「按需生成」的：只包含源码里真正会显示出来的汉字，
//  由 tools/genfont.py 自动扫描生成。加一句中文诊断后重跑脚本即可，
//  不需要手工维护字表。
//
//  点阵规格：16x16，一个汉字横占 2 个英文字符格（16 = 8 x 2），纵深 1 格。
//
//  为什么汉字必须是 16x16？
//    8x16 的格子横向只有 8 像素，汉字笔画挤在这么窄的空间里会糊成一团，
//    根本认不出来。标准做法是给汉字两倍宽度。
// ============================================================================
#pragma once

#include <kernel/types.h>

namespace cjk {

// 字库里有多少个字
extern const int glyph_count;

// 单个字的像素尺寸
extern const int glyph_width;     // 16
extern const int glyph_height;    // 16

// 一个汉字横向占几个英文字符格
constexpr int CELLS_PER_GLYPH = 2;

// Unicode 码点表
extern const u32 codepoints[];

// 点阵数据，每字 32 字节（16 行 x 2 字节）
extern const u8 bitmaps[][32];

// 按 Unicode 码点查字模；字库里没有这个字时返回 nullptr
const u8* find(u32 codepoint);

}  // namespace cjk
