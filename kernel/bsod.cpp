// ============================================================================
//  kernel/bsod.cpp —— 启动蓝屏实现
//  ---------------------------------------------------------------------------
//  两个关键点：
//
//  1) 独立绘制路径
//     直接用 fb:: 画点阵，绕开 term（避免字符网格 / 光标状态冲突）。
//     支持 ASCII（8×16）和汉字（16×16），自己解 UTF-8。
//
//  2) 按键等待走内核直读 0x60
//     蓝屏在启动早期，用户态键盘服务还没起来，IPC 拿不到按键。
//     所以自己轮询 0x64 状态口 + 0x60 数据口，比对 Enter 的扫描码 0x1C。
//
//  另外：蓝屏总是画到**真屏幕**。
//     如果当前 fb 的渲染目标是离屏缓冲（加载中），
//     必须先切回真屏，否则用户什么都看不到。
// ============================================================================

#include <kernel/bsod.hpp>
#include <kernel/framebuffer.hpp>
#include <kernel/font.hpp>
#include <kernel/cjk_font.hpp>
#include <kernel/io.h>
#include <kernel/printf.hpp>
#include <kernel/bootanim.hpp>

namespace {

// ---------------------------------------------------------------------------
//  UTF-8 解码：从 s 位置取出一个字符的 Unicode 码点，返回消耗的字节数
// ---------------------------------------------------------------------------
int utf8_next(const char* s, u32* out)
{
    const u8* p = reinterpret_cast<const u8*>(s);
    u8 b0 = p[0];

    if (b0 < 0x80) {
        *out = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *out = ((static_cast<u32>(b0 & 0x1F)) << 6)
             |  static_cast<u32>(p[1] & 0x3F);
        return 2;
    }
    if ((b0 & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *out = ((static_cast<u32>(b0 & 0x0F)) << 12)
             | (static_cast<u32>(p[1] & 0x3F) << 6)
             |  static_cast<u32>(p[2] & 0x3F);
        return 3;
    }
    // 非法序列：当作单字节，避免死循环
    *out = b0;
    return 1;
}

// ---------------------------------------------------------------------------
//  画一个 ASCII 字符（8×16）
// ---------------------------------------------------------------------------
void draw_ascii(int x, int y, char c, u32 fg)
{
    const u8* bits = font::glyph(c);
    for (int row = 0; row < font::HEIGHT; ++row) {
        u8 line = bits[row];
        for (int col = 0; col < 8; ++col) {
            if (line & (1 << (7 - col))) {
                fb::put_pixel(static_cast<u32>(x + col),
                              static_cast<u32>(y + row), fg);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  画一个汉字（16×16）
// ---------------------------------------------------------------------------
void draw_cjk(int x, int y, u32 codepoint, u32 fg)
{
    const u8* bits = cjk::find(codepoint);
    if (bits == nullptr) return;              // 字库里没有，留空

    for (int row = 0; row < cjk::glyph_height; ++row) {
        u8 left  = bits[row * 2];
        u8 right = bits[row * 2 + 1];
        for (int col = 0; col < 8; ++col) {
            if (left & (1 << (7 - col))) {
                fb::put_pixel(static_cast<u32>(x + col),
                              static_cast<u32>(y + row), fg);
            }
            if (right & (1 << (7 - col))) {
                fb::put_pixel(static_cast<u32>(x + 8 + col),
                              static_cast<u32>(y + row), fg);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  画一行文字（自动处理中英混排）
// ---------------------------------------------------------------------------
//  返回绘制后光标的 x 偏移（用于居中计算）
// ---------------------------------------------------------------------------
int draw_line(int x, int y, const char* s, u32 fg)
{
    int cx = x;
    const char* p = s;
    while (*p != '\0') {
        u32 cp = 0;
        int n = utf8_next(p, &cp);
        p += n;

        if (cp >= 0x80) {
            draw_cjk(cx, y, cp, fg);
            cx += cjk::glyph_width;
        } else {
            draw_ascii(cx, y, static_cast<char>(cp), fg);
            cx += font::WIDTH;
        }
    }
    return cx - x;
}

// 文本宽度（像素），用于居中
int line_width(const char* s)
{
    int w = 0;
    const char* p = s;
    while (*p != '\0') {
        u32 cp = 0;
        int n = utf8_next(p, &cp);
        p += n;
        w += (cp >= 0x80) ? cjk::glyph_width : font::WIDTH;
    }
    return w;
}

void draw_line_centered(int y, const char* s, u32 fg, int screen_w)
{
    int w = line_width(s);
    int x = (screen_w - w) / 2;
    if (x < 0) x = 0;
    draw_line(x, y, s, fg);
}

// ---------------------------------------------------------------------------
//  Enter 键扫描码
// ---------------------------------------------------------------------------
constexpr u8 SC_ENTER      = 0x1C;    // 按下
constexpr u8 SC_ENTER_REL  = 0x9C;    // 松开

}  // namespace

namespace bsod {

// ---------------------------------------------------------------------------
//  等待 Enter 键（内核直读 0x60）
// ---------------------------------------------------------------------------
void wait_enter()
{
    for (;;) {
        u8 st = inb(0x64);
        if ((st & 0x01) == 0) continue;          // 输出缓冲空

        if (st & 0x20) {                         // bit5 = AUXB：鼠标数据
            inb(0x60);                           // 读掉丢弃，别当按键
            continue;
        }

        u8 sc = inb(0x60);
        if (sc == SC_ENTER) {
            return;                              // 按下 Enter → 放行
        }
        // 松开码(0x9C)和其他键一律忽略
    }
}

// ---------------------------------------------------------------------------
//  显示蓝屏
// ---------------------------------------------------------------------------
void show(const char* subsystem, const char* error, const char* detail, bool fatal)
{
    // 蓝屏事件记入串口（用户看不到串口，但排查时用得上）
    kprintf_serial("[BSOD] %s | %s | fatal=%d\n",
                   subsystem, error, fatal ? 1 : 0);

    const fb::Info& fi = fb::info();

    // 蓝屏必须画到真屏幕：可能当前渲染目标是离屏缓冲
    void* saved_target = fb::target();
    fb::set_target(nullptr);

    if (!fi.available) {
        // 连帧缓冲都没有：只能退化为停机（没法画）
        if (fatal) {
            asm volatile("cli");
            for (;;) asm volatile("hlt");
        }
        fb::set_target(saved_target);
        return;
    }

    int W = static_cast<int>(fi.width);
    int H = static_cast<int>(fi.height);

    // --- 底色 ---
    fb::clear(fb::color::BLUE);

    // --- 顶部装饰：三个横条（经典蓝屏观感）---
    int bar_w = 60, bar_h = 8, bar_gap = 12;
    int bar_total = bar_w * 3 + bar_gap * 2;
    int bar_x = (W - bar_total) / 2;
    int bar_y = 40;
    for (int i = 0; i < 3; ++i) {
        fb::fill_rect(static_cast<u32>(bar_x + i * (bar_w + bar_gap)),
                      static_cast<u32>(bar_y),
                      static_cast<u32>(bar_w), static_cast<u32>(bar_h),
                      fb::color::WHITE);
    }

    const u32 FG  = fb::color::WHITE;
    const u32 DIM = fb::color::LIGHT_GRAY;
    int LH = font::HEIGHT + 10;                  // 行高

    // --- 标题 ---
    int y = bar_y + bar_h + 40;
    draw_line_centered(y, "xingxingOS 启动失败", FG, W);
    y += LH + 24;

    // --- 错误内容 ---
    draw_line_centered(y, "子系统", DIM, W);
    y += LH;
    draw_line_centered(y, subsystem, FG, W);
    y += LH + 14;

    draw_line_centered(y, "错误", DIM, W);
    y += LH;
    draw_line_centered(y, error, FG, W);
    y += LH + 14;

    if (detail != nullptr && detail[0] != '\0') {
        draw_line_centered(y, "说明", DIM, W);
        y += LH;
        draw_line_centered(y, detail, FG, W);
        y += LH + 14;
    }

    // --- 排查提示 ---
    draw_line_centered(y, "请拍下此画面并反馈，以便定位问题", DIM, W);
    y += LH + 30;

    // --- 操作提示 ---
    if (fatal) {
        draw_line_centered(y, "系统已停止，请重启", FG, W);
        fb::set_target(saved_target);
        asm volatile("cli");
        for (;;) asm volatile("hlt");
    } else {
        draw_line_centered(y, ">> 按 Enter 继续加载 <<", fb::color::YELLOW, W);
        // 提示已画完再等按键（否则按键期间屏幕没提示，用户不知道要按）
        wait_enter();
        // 继续加载：清屏，恢复原渲染目标
        fb::clear(fb::color::BLACK);
        fb::set_target(saved_target);
    }
}

void fatal(const char* subsystem, const char* error, const char* detail)
{
    show(subsystem, error, detail, true);
}

void warn(const char* subsystem, const char* error, const char* detail)
{
    show(subsystem, error, detail, false);
}

}  // namespace bsod
