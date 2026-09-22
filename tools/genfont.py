#!/usr/bin/env python3
# =============================================================================
#  tools/genfont.py —— 生成「按需」中文字库
#  ---------------------------------------------------------------------------
#  为什么叫「按需」？
#    一套完整中文字库有上万个字，16x16 点阵也要 300KB+，对早期内核太重了。
#    而实际上内核要显示的汉字就那么几十上百个（诊断信息、提示语）。
#    所以这里扫描源码里**会被显示出来的字符串**，把里面的汉字挑出来，
#    只为这些字生成字模。加一句中文诊断，字库里就多那几个字，成本极低。
#
#  用法：  python3 tools/genfont.py
#  产出：  kernel/cjk_font.cpp
#
#  点阵格式：16x16，每个字 32 字节（一行 2 字节，bit7 是最左像素）
# =============================================================================

import os
import re
import glob
from PIL import Image, ImageDraw, ImageFont

# --- 配置 ---
FONT_PATH = '/usr/share/fonts/opentype/source-han-sans/SourceHanSansSC-Regular.otf'
GLYPH_W, GLYPH_H = 16, 16
FONT_SIZE = 13                     # 13pt 时墨迹高约 14px，正好装进 16 行不裁切
BYTES_PER_GLYPH = GLYPH_H * 2      # 每行 2 字节

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(ROOT, 'kernel', 'cjk_font.cpp')

# 需要显示中文的源码目录
#
# ⚠️【重要】必须包含 'user'！
#    P1「万物皆可程序」之后，所有命令（help/uptime/echo/ps/...）
#    都从 shell 内置移到了 user/ 下的独立 .xzs 程序。
#    这里如果漏了 user，那些命令输出里的中文（"可用命令"、"显示帮助"…）
#    统统不会进字库 —— 屏幕上就显示成 '?'。
#    这是 2026-09-17 实测"字库缺字"的真正根因。
SCAN_DIRS = ['kernel', 'include', 'user']


def collect_chars():
    """扫描源码，提取会被显示出来的字符串里的非 ASCII 字符。"""
    chars = set()

    for d in SCAN_DIRS:
        pattern = os.path.join(ROOT, d, '**', '*.cpp')
        for path in glob.glob(pattern, recursive=True) + glob.glob(
                os.path.join(ROOT, d, '**', '*.hpp'), recursive=True) + glob.glob(
                os.path.join(ROOT, d, '**', '*.h'), recursive=True):
            with open(path, encoding='utf-8') as f:
                src = f.read()

            # 先去掉注释，避免把注释里的中文也算进去
            src = re.sub(r'//[^\n]*', '', src)
            src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)

            # 提取双引号字符串字面量
            for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', src):
                for ch in m.group(1):
                    if ord(ch) > 0x2000:   # 中文、中文标点、全角字符都算
                        chars.add(ch)

    return sorted(chars)


def make_offsets(font):
    """
    算出让汉字正确落位的 (x, y) 偏移。

    为什么不能写死偏移？
      中文字体的 ascent + descent 通常大于字号（思源黑体约 1.2 em），
      直接按字号渲染会撑出格子被裁掉。而且标点（比如句号）必须落在底部，
      不能简单居中——否则「。」会飘到正中，看着很怪。

    做法：让 descent 贴着格子底边，反推出 y；再按字身宽度算水平居中。
    """
    ascent, descent = font.getmetrics()
    y = GLYPH_H - ascent - descent
    x = (GLYPH_W - FONT_SIZE) // 2       # 汉字字身是正方形，边长约等于字号
    return x, y


def render(ch, font, offsets):
    """把单个字符渲染成 16x16 的 0/1 位图。"""
    x_off, y_off = offsets
    img = Image.new('1', (GLYPH_W, GLYPH_H), 0)
    d = ImageDraw.Draw(img)
    d.text((x_off, y_off), ch, fill=1, font=font)

    data = []
    for y in range(GLYPH_H):
        b0 = 0
        b1 = 0
        for x in range(8):
            if img.getpixel((x, y)):
                b0 |= (1 << (7 - x))
        for x in range(8, 16):
            if img.getpixel((x, y)):
                b1 |= (1 << (7 - (x - 8)))
        data.append(b0)
        data.append(b1)
    return data


def main():
    chars = collect_chars()
    if not chars:
        print('没有找到需要生成的汉字')
        return

    font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
    offsets = make_offsets(font)
    print('渲染偏移: x=%d, y=%d' % offsets)

    print('需要生成的汉字（%d 个）:' % len(chars))
    print('  ' + ''.join(chars))
    print()

    L = []
    L.append('// ============================================================================')
    L.append('//  kernel/cjk_font.cpp —— 按需生成的中文字库')
    L.append('//  ---------------------------------------------------------------------------')
    L.append('//  【本文件由 tools/genfont.py 自动生成，请勿手工修改】')
    L.append('//')
    L.append('//  为什么是「按需」的？')
    L.append('//    完整中文字库上万字，16x16 点阵要 300KB+，早期内核背不动。')
    L.append('//    而这个字库只包含源码里**真正会显示出来**的汉字。')
    L.append('//    想加一句中文诊断？直接写就是了，重新跑一次脚本，')
    L.append('//    用到的新字会自动进字库，成本只有几十字节。')
    L.append('//')
    L.append('//  点阵格式：')
    L.append('//    每字 16x16 像素，共 32 字节。')
    L.append('//    一行 2 字节：第 1 字节管左半边 8 像素，第 2 字节管右半边 8 像素。')
    L.append('//    每个字节里 bit7 是最左边的像素，1 = 点亮。')
    L.append('//')
    L.append('//  显示规则：一个汉字横占 2 个英文字符格（16 = 8 x 2），纵深 1 格。')
    L.append('//  本文件包含 %d 个汉字 / 全角字符。' % len(chars))
    L.append('// ============================================================================')
    L.append('')
    L.append('#include <kernel/cjk_font.hpp>')
    L.append('')
    L.append('namespace cjk {')
    L.append('')
    L.append('const int glyph_count = %d;' % len(chars))
    L.append('const int glyph_width  = %d;' % GLYPH_W)
    L.append('const int glyph_height = %d;' % GLYPH_H)
    L.append('')
    L.append('// Unicode 码点表（升序，配合二分/顺序查找）')
    L.append('const u32 codepoints[%d] = {' % len(chars))
    for i in range(0, len(chars), 8):
        batch = chars[i:i + 8]
        items = ' '.join('0x%04X,' % ord(c) for c in batch)
        tail = '  // ' + ''.join(batch) if batch else ''
        L.append('    %s%s' % (items, tail))
    L.append('};')
    L.append('')
    L.append('// 点阵数据：每字 %d 字节，顺序与 codepoints 一一对应' % BYTES_PER_GLYPH)
    L.append('const u8 bitmaps[%d][%d] = {' % (len(chars), BYTES_PER_GLYPH))

    for ch in chars:
        data = render(ch, font, offsets)
        hexs = ' '.join('0x%02X,' % b for b in data)
        L.append('    { %s },  // U+%04X %s' % (hexs, ord(ch), ch))

    L.append('};')
    L.append('')
    L.append('const u8* find(u32 codepoint)')
    L.append('{')
    L.append('    // 字数很少（几十到一两百），顺序查找完全够用，不必上二分')
    L.append('    for (int i = 0; i < glyph_count; ++i) {')
    L.append('        if (codepoints[i] == codepoint) {')
    L.append('            return bitmaps[i];')
    L.append('        }')
    L.append('    }')
    L.append('    return nullptr;')
    L.append('}')
    L.append('')
    L.append('}  // namespace cjk')

    with open(OUTPUT, 'w', encoding='utf-8') as f:
        f.write('\n'.join(L) + '\n')

    print('已生成: %s' % OUTPUT)
    print('字数: %d, 占用: %d 字节' % (len(chars), len(chars) * BYTES_PER_GLYPH))

    # 顺便预览第一个字，确认渲染没跑偏
    if chars:
        for pch in chars[:1] + (['。'] if '。' in chars else []):
            data = render(pch, font, offsets)
            print('\n预览 "%s":' % pch)
            for y in range(GLYPH_H):
                b0, b1 = data[y * 2], data[y * 2 + 1]
                row = ''
                for x in range(8):
                    row += '#' if b0 & (1 << (7 - x)) else '.'
                for x in range(8):
                    row += '#' if b1 & (1 << (7 - x)) else '.'
                print('  ' + row)
        for y in range(GLYPH_H):
            b0, b1 = data[y * 2], data[y * 2 + 1]
            row = ''
            for x in range(8):
                row += '#' if b0 & (1 << (7 - x)) else '.'
            for x in range(8):
                row += '#' if b1 & (1 << (7 - x)) else '.'
            print('  ' + row)


if __name__ == '__main__':
    main()
