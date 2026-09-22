// ============================================================================
//  kernel/terminal.cpp —— 文本终端实现（图形后端 + VGA 兜底）
//  ---------------------------------------------------------------------------
//  核心逻辑其实就三件事：
//    1. 维护一个「光标位置」（第几列、第几行）
//    2. 把字符画到光标处，然后光标右移
//    3. 光标走到边界就换行 / 滚屏
//
//  两个后端共用第 1、3 步的游标逻辑，只有第 2 步「怎么画」不同。
// ============================================================================

#include <kernel/printf.hpp>
#include <kernel/terminal.hpp>
#include <kernel/serial.hpp>
#include <kernel/framebuffer.hpp>
#include <kernel/mouse.hpp>
#include <kernel/font.hpp>
#include <kernel/cjk_font.hpp>

namespace {

// ---------------------------------------------------------------------------
//  VGA 文本模式相关
//  物理显存 0xB8000，每字符 2 字节（低字节 ASCII，高字节属性）。
//  属性字节：低 4 位前景色，高 4 位背景色，bit7 是闪烁位（这里关掉）。
// ---------------------------------------------------------------------------
constexpr u64 VGA_PHYS_BASE = 0xB8000;
constexpr u64 KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ull;
constexpr u64 VGA_VIRT_BASE = KERNEL_VIRT_BASE + VGA_PHYS_BASE;
constexpr int VGA_WIDTH  = 80;
constexpr int VGA_HEIGHT = 25;

// 16 色 VGA 调色板（顺序与 VGA 硬件的色号一致，0~15）
constexpr u32 VGA_PALETTE[16] = {
    0x000000,  // 0 黑
    0x0000AA,  // 1 蓝
    0x00AA00,  // 2 绿
    0x00AAAA,  // 3 青
    0xAA0000,  // 4 红
    0xAA00AA,  // 5 洋红
    0xAA5500,  // 6 棕
    0xAAAAAA,  // 7 浅灰
    0x555555,  // 8 深灰
    0x5555FF,  // 9 亮蓝
    0x55FF55,  // 10 亮绿
    0x55FFFF,  // 11 亮青
    0xFF5555,  // 12 亮红
    0xFF55FF,  // 13 亮洋红
    0xFFFF55,  // 14 黄
    0xFFFFFF,  // 15 白
};

volatile u16* vga_buffer()
{
    return reinterpret_cast<volatile u16*>(VGA_VIRT_BASE);
}

// 把一个 0xRRGGBB 颜色映射到最接近的 VGA 16 色号
// 做法很土但够用：算到 16 个调色板颜色的距离，取最近的那个
u8 nearest_vga_color(u32 rgb)
{
    u32 r = (rgb >> 16) & 0xFF;
    u32 g = (rgb >> 8)  & 0xFF;
    u32 b = rgb & 0xFF;

    int best = 0;
    u32 best_dist = 0xFFFFFFFF;

    for (int i = 0; i < 16; ++i) {
        u32 pr = (VGA_PALETTE[i] >> 16) & 0xFF;
        u32 pg = (VGA_PALETTE[i] >> 8)  & 0xFF;
        u32 pb = VGA_PALETTE[i] & 0xFF;
        // 平方距离，不用开根号，比较大小就够了
        u32 dist = (r - pr) * (r - pr) + (g - pg) * (g - pg) + (b - pb) * (b - pb);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return static_cast<u8>(best);
}

// ---------------------------------------------------------------------------
//  终端状态
// ---------------------------------------------------------------------------
term::Backend g_backend = term::Backend::None;

int  g_columns = 0;
int  g_rows    = 0;
int  g_col     = 0;      // 光标列
int  g_row     = 0;      // 光标行

u32  g_fg = 0xFFFFFF;    // 前景色（默认白）
u32  g_bg = 0x000000;    // 背景色（默认黑）

// ---------------------------------------------------------------------------
//  字符网格：记录屏幕上每个格子当前显示的是什么
//  -------------------------------------------------------------------------
//  为什么需要它？
//    光标闪烁要"反色显示光标所在格"，闪回去时还得**恢复原样**。
//    帧缓冲是像素设备，写上去就覆盖了原来的像素，读不回来，
//    所以必须额外记住每格的字符，才能正确擦除光标。
//
//    （VGA 文本模式本身能从显存读回，但为了统一，也一起记录）
// ---------------------------------------------------------------------------
constexpr int MAX_ROWS = 128;
constexpr int MAX_COLS = 256;
// ⚠️【关键】这里存的**不是 char，是 Unicode 码点**。
//
//  为什么？原来存 char，汉字（>= 0x80）根本存不进去，
//  于是汉字格子在网格里永远是"空格"。
//  而光标闪烁擦除靠网格恢复原字符 ——
//  结果每画完一个汉字，光标一移走就把它的**起始格擦成空格**，
//  只剩右半边。这就是用户看到的「中文只显示一半」。
//
//  约定：
//    0         = 空格/空
//    >= 0x80   = 该格是某个汉字的**起始格**（左半）
//    CONTINUE  = 该格是左边汉字占用的**右半格**
static constexpr u32 CELL_CONTINUE = 0xFFFFFFFEu;
static u32 g_screen_code[MAX_ROWS][MAX_COLS];

// ---------------------------------------------------------------------------
//  光标闪烁状态
//  -------------------------------------------------------------------------
//  g_cursor_visible  ：当前是否"显示"光标（亮）
//  g_cursor_ticks    ：计时，累计到 BLINK_PERIOD 就翻转一次
//  g_cursor_drawn    ：光标**当前是否已画在屏幕上**（用于正确擦除）
//
//  闪烁周期：BLINK_PERIOD 个 tick 翻转一次。
//    PIT 频率 100Hz（10ms/tick），30 tick = 300ms，
//    即亮 300ms、灭 300ms，一个完整周期 600ms。
//    这是终端里比较舒服的速度（Windows 默认约 530ms）。
// ---------------------------------------------------------------------------
constexpr u32 BLINK_PERIOD = 30;
static bool g_cursor_visible = true;
static u32  g_cursor_ticks   = 0;
static bool g_cursor_drawn   = false;
static int  g_cursor_last_col = -1;
static int  g_cursor_last_row = -1;

u8   g_vga_attr = 0x0F;  // VGA 属性字节（黑底白字）

// ---------------------------------------------------------------------------
//  UTF-8 解码状态
//  -------------------------------------------------------------------------
//  kprintf 是逐字节往外吐的，而一个汉字在 UTF-8 里是 3 个字节。
//  所以终端必须自己攒字节：收到起始字节就知道还要再等几个，
//  攒够了再合成一个 Unicode 码点去查字库。
// ---------------------------------------------------------------------------
u8   g_utf8_buf[4];      // 攒下来的字节
int  g_utf8_len  = 0;    // 已经攒了几个
int  g_utf8_need = 0;    // 这个字符总共需要几个字节

// ---------------------------------------------------------------------------
//  帧缓冲后端：在 (col,row) 这个字符格子里画一个字符
// ---------------------------------------------------------------------------
void fb_draw_cell(int col, int row, char c)
{
    int px = col * font::WIDTH;
    int py = row * font::HEIGHT;

    // 先填背景色（这样字符周围没有残留的旧像素）
    fb::fill_rect(px, py, font::WIDTH, font::HEIGHT, g_bg);

    // 再按点阵数据逐像素点亮前景色
    const u8* bits = font::glyph(c);
    for (int y = 0; y < font::HEIGHT; ++y) {
        u8 line = bits[y];
        for (int x = 0; x < font::WIDTH; ++x) {
            // bit7 对应最左边的像素
            if (line & (1 << (7 - x))) {
                fb::put_pixel(px + x, py + y, g_fg);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  帧缓冲后端：画一个 16x16 的汉字
//  它横向盖住 2 个英文字符格（col 和 col+1）
// ---------------------------------------------------------------------------
void fb_draw_cjk(int col, int row, const u8* bits)
{
    int px = col * font::WIDTH;      // 起始像素 X（1 格 = 8 像素）
    int py = row * font::HEIGHT;

    // 先铺背景，盖掉上一次的内容
    fb::fill_rect(px, py, cjk::glyph_width, cjk::glyph_height, g_bg);

    // 逐行点亮：每行 2 字节，第 1 字节是左半边、第 2 字节是右半边
    for (int y = 0; y < cjk::glyph_height; ++y) {
        u8 left  = bits[y * 2];
        u8 right = bits[y * 2 + 1];
        for (int x = 0; x < 8; ++x) {
            if (left & (1 << (7 - x))) {
                fb::put_pixel(px + x, py + y, g_fg);
            }
            if (right & (1 << (7 - x))) {
                fb::put_pixel(px + 8 + x, py + y, g_fg);
            }
        }
    }

}

// ---------------------------------------------------------------------------
//  VGA 后端：往文本显存写「字符 + 属性」
// ---------------------------------------------------------------------------
void vga_draw_cell(int col, int row, char c)
{
    if (col < 0 || col >= VGA_WIDTH || row < 0 || row >= VGA_HEIGHT) {
        return;
    }
    vga_buffer()[row * VGA_WIDTH + col] =
        static_cast<u16>((static_cast<u16>(g_vga_attr) << 8) | static_cast<u8>(c));
}

// 分派到具体后端
void draw_cell(int col, int row, char c)
{
    // 记录到字符网格（供光标闪烁恢复用）
    if (row >= 0 && row < MAX_ROWS && col >= 0 && col < MAX_COLS) {
        g_screen_code[row][col] = (c == 0) ? ' ' : static_cast<u32>(static_cast<u8>(c));
    }

    if (g_backend == term::Backend::Framebuffer) {
        fb_draw_cell(col, row, c);
    } else if (g_backend == term::Backend::VgaText) {
        vga_draw_cell(col, row, c);
    }
}

// ---------------------------------------------------------------------------
//  画一个"反色"的格子（用于光标）
//  -------------------------------------------------------------------------
//  反色 = 前景色和背景色互换。
//  光标亮时把这一格反色显示，灭时再正常画回去 ——
//  只要字符网格里记得原字符，就能完美恢复。
// ---------------------------------------------------------------------------
static void draw_cell_inverted(int col, int row, char c)
{
    if (col < 0 || col >= g_columns || row < 0 || row >= g_rows) return;

    if (g_backend == term::Backend::VgaText) {
        // VGA：属性字节高 4 位是背景、低 4 位是前景，互换即可
        u8 attr = g_vga_attr;
        u8 fg   = attr & 0x0F;
        u8 bg   = (attr >> 4) & 0x0F;
        u8 inv  = static_cast<u8>((fg << 4) | bg);
        vga_buffer()[row * VGA_WIDTH + col] =
            static_cast<u16>((static_cast<u16>(inv) << 8) | static_cast<u8>(c));
        return;
    }

    if (g_backend != term::Backend::Framebuffer) return;

    // 帧缓冲：整格填前景色，再用背景色画字形
    int px = col * font::WIDTH;
    int py = row * font::HEIGHT;
    fb::fill_rect(px, py, font::WIDTH, font::HEIGHT, g_fg);

    if (c != ' ' && c != 0) {
        const u8* bits = font::glyph(c);
        for (int y = 0; y < font::HEIGHT; ++y) {
            u8 line = bits[y];
            for (int x = 0; x < font::WIDTH; ++x) {
                if (line & (1 << (7 - x))) {
                    fb::put_pixel(px + x, py + y, g_bg);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  擦除光标：把该格恢复成正常显示
// ---------------------------------------------------------------------------
static void erase_cursor_at(int col, int row)
{
    if (col < 0 || col >= g_columns || row < 0 || row >= g_rows) return;
    if (row >= MAX_ROWS || col >= MAX_COLS) return;

    u32 cp = g_screen_code[row][col];

    // 这一格是某个汉字的右半：往前找起始格，重画整个汉字
    if (cp == CELL_CONTINUE) {
        if (col > 0) {
            u32 head = g_screen_code[row][col - 1];
            if (head >= 0x80) {
                const u8* bits = cjk::find(head);
                if (bits != nullptr) {
                    fb_draw_cjk(col - 1, row, bits);
                    return;
                }
            }
        }
        draw_cell(col, row, ' ');
        return;
    }

    // 这一格是汉字的起始格（左半）：重画整个汉字
    if (cp >= 0x80) {
        const u8* bits = cjk::find(cp);
        if (bits != nullptr) {
            fb_draw_cjk(col, row, bits);
            return;
        }
    }

    draw_cell(col, row, static_cast<char>(cp ? cp : ' '));
}

// ---------------------------------------------------------------------------
//  set_cursor_visible：更新光标显示状态
//  -------------------------------------------------------------------------
//  由定时器中断周期性调用（见 term::tick_cursor）。
//
//  【关键】如果光标位置变了（比如用户按了左右方向键），
//  必须先把**旧位置**的光标擦掉，再在新位置画，
//  否则旧位置会留下一个永远反色的块。
// ---------------------------------------------------------------------------
void set_cursor_visible(bool visible)
{
    // 位置变了：先擦掉旧的
    if (g_cursor_last_col != g_col || g_cursor_last_row != g_row) {
        if (g_cursor_last_col >= 0 && g_cursor_last_row >= 0) {
            erase_cursor_at(g_cursor_last_col, g_cursor_last_row);
        }
        g_cursor_last_col = g_col;
        g_cursor_last_row = g_row;
        g_cursor_drawn = false;
    }

    if (visible) {
        // 光标落在汉字上时，网格里存的是码点（>= 0x80）或 CELL_CONTINUE。
        // 这两种情况统一按"空格"处理 —— 结果是一个纯色反相块，
        // 视觉上和真实终端里光标压在汉字上是一致的。
        char c = ' ';
        if (g_row < MAX_ROWS && g_col < MAX_COLS) {
            u32 cp = g_screen_code[g_row][g_col];
            if (cp < 0x80) {
                c = static_cast<char>(cp ? cp : ' ');
            }
        }
        draw_cell_inverted(g_col, g_row, c);
        g_cursor_drawn = true;
    } else {
        erase_cursor_at(g_col, g_row);
        g_cursor_drawn = false;
    }
}



// 把屏幕内容整体上移一行（滚屏）
// ---------------------------------------------------------------------------
//  字符网格同步上滚一行（供 scroll_one_line 调用）
//  -------------------------------------------------------------------------
//  网格是光标闪烁"恢复原字符"的唯一依据，
//  屏幕滚动时它必须跟着滚，否则恢复出的就是错位内容。
// ---------------------------------------------------------------------------
static void scroll_grid_one_line()
{
    int rows = (g_rows < MAX_ROWS) ? g_rows : MAX_ROWS;
    int cols = (g_columns < MAX_COLS) ? g_columns : MAX_COLS;

    for (int y = 1; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            g_screen_code[y - 1][x] = g_screen_code[y][x];
        }
    }
    // 最后一行清空
    for (int x = 0; x < cols; ++x) {
        g_screen_code[rows - 1][x] = ' ';
    }
}

void scroll_one_line()
{
    if (g_backend == term::Backend::VgaText) {
        volatile u16* buf = vga_buffer();
        for (int y = 1; y < VGA_HEIGHT; ++y) {
            for (int x = 0; x < VGA_WIDTH; ++x) {
                buf[(y - 1) * VGA_WIDTH + x] = buf[y * VGA_WIDTH + x];
            }
        }
        u16 blank = static_cast<u16>((static_cast<u16>(g_vga_attr) << 8) | ' ');
        for (int x = 0; x < VGA_WIDTH; ++x) {
            buf[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = blank;
        }
        return;
    }

    if (g_backend != term::Backend::Framebuffer) {
        return;
    }

    // 图形模式：整屏像素往上搬一行字符的高度
    // 直接按像素搬运比「重新画一遍所有字符」简单，也更快
    const fb::Info& info = fb::info();
    u8* base = reinterpret_cast<u8*>(info.addr);
    int line_bytes = static_cast<int>(info.pitch) * font::HEIGHT;

    for (int y = 0; y < (g_rows - 1) * font::HEIGHT; ++y) {
        u8* dst = base + static_cast<u64>(y) * info.pitch;
        u8* src = base + static_cast<u64>(y + font::HEIGHT) * info.pitch;
        for (u32 x = 0; x < info.pitch; ++x) {
            dst[x] = src[x];
        }
    }
    // 最后一行用背景色清掉
    (void)line_bytes;
    fb::fill_rect(0, (g_rows - 1) * font::HEIGHT,
                  info.width, font::HEIGHT, g_bg);

    // -----------------------------------------------------------------
    //  【关键 bug 修复】字符网格必须跟着一起滚！
    //  ---------------------------------------------------------------
    //  原来这里只搬了**像素**，字符网格完全没动。
    //  于是网格里记的字符和屏幕实际内容**错位一行**，
    //  光标闪烁擦除时（erase_cursor_at 从网格取字符恢复）
    //  就会把**上一行/上一次的陈旧字符**画到当前光标位置。
    //
    //  用户实测现象：底部莫名冒出 "l"，按空格又冒出 "og on"
    //  —— 正是之前输入过的 "log on" 残留在网格里被"恢复"出来了。
    //
    //  必须在搬完像素后同步滚动网格，否则光标闪烁会画出幽灵字符。
    // -----------------------------------------------------------------
    scroll_grid_one_line();
}

// ---------------------------------------------------------------------------
//  光标推进的公共收尾：处理「列越界换行」和「行越界滚屏」
// ---------------------------------------------------------------------------
void advance_after_draw()
{
    if (g_col >= g_columns) {
        g_col = 0;
        ++g_row;
    }
    if (g_row >= g_rows) {
        scroll_one_line();
        g_row = g_rows - 1;
    }
}

// ---------------------------------------------------------------------------
//  emit_ascii：画一个 ASCII 字符（含换行、制表、退格这些控制字符）
// ---------------------------------------------------------------------------
void emit_ascii(char c)
{
    if (c == '\n') {
        g_col = 0;
        ++g_row;
    } else if (c == '\r') {
        g_col = 0;
    } else if (c == '\t') {
        g_col = (g_col + 8) & ~7;
        if (g_col >= g_columns) {
            g_col = 0;
            ++g_row;
        }
    } else if (c == '\b') {
        // 退格：只把光标左移一格，**不擦除**字符。
        //
        // 为什么改成这样？
        //   之前的实现是「左移 + 把那一格擦成空格」，
        //   但 shell 的行编辑需要「只移动光标、不改动内容」这个能力
        //   （左右方向键、Home/End、重画整行时的光标归位都要用到）。
        //   把擦除耦合进 '\b' 之后，shell 每次重画都会把沿途字符顺手抹掉，
        //   连提示符也被擦掉又重画——疯狂按上下键翻历史时，
        //   屏幕上就会堆积出一串粘连的提示符（实测出现过 "helpxingxingos>"）。
        //
        //   标准做法是 '\b' 只移动光标，要删除就显式写 "\b \b"
        //   （左移 -> 打空格覆盖 -> 再左移），职责分明。
        if (g_col > 0) {
            --g_col;
        } else if (g_row > 0) {
            --g_row;
            g_col = g_columns - 1;
        }
    } else {
        draw_cell(g_col, g_row, c);
        ++g_col;
    }

    advance_after_draw();
}

// ---------------------------------------------------------------------------
//  emit_codepoint：画一个 Unicode 码点
//    汉字 -> 查 16x16 字库，横占 2 个字符格
//    查不到 / VGA 文本模式 -> 退化成问号（VGA 字模是硬件固化的，没有汉字）
// ---------------------------------------------------------------------------
void emit_codepoint(u32 cp)
{
    if (cp < 0x80) {
        emit_ascii(static_cast<char>(cp));
        return;
    }

    const u8* bits = cjk::find(cp);
    if (bits == nullptr || g_backend != term::Backend::Framebuffer) {
        emit_ascii('?');
        return;
    }

    // 汉字要占两格，本行剩下的位置不够就先换行
    if (g_col + cjk::CELLS_PER_GLYPH > g_columns) {
        g_col = 0;
        ++g_row;
    }

    fb_draw_cjk(g_col, g_row, bits);

    // 【关键】把汉字记进网格，光标闪烁擦除时才不会把它抹掉。
    //   起始格存码点，右半格存 CELL_CONTINUE 标记。
    if (g_row < MAX_ROWS) {
        g_screen_code[g_row][g_col] = cp;
        if (g_col + 1 < MAX_COLS) {
            g_screen_code[g_row][g_col + 1] = CELL_CONTINUE;
        }
    }

    g_col += cjk::CELLS_PER_GLYPH;

    advance_after_draw();
}

// ---------------------------------------------------------------------------
//  decode_utf8：把攒下来的字节序列合成 Unicode 码点
//  UTF-8 的规则很简单：起始字节的高位 1 的个数 = 总字节数，
//  后续字节一律是 10xxxxxx，各贡献 6 个有效位。
// ---------------------------------------------------------------------------
u32 decode_utf8(const u8* buf, int len)
{
    if (len == 2) {
        return ((buf[0] & 0x1Fu) << 6) | (buf[1] & 0x3Fu);
    }
    if (len == 3) {
        return ((buf[0] & 0x0Fu) << 12)
             | ((buf[1] & 0x3Fu) << 6)
             |  (buf[2] & 0x3Fu);
    }
    if (len == 4) {
        return ((buf[0] & 0x07u) << 18)
             | ((buf[1] & 0x3Fu) << 12)
             | ((buf[2] & 0x3Fu) << 6)
             |  (buf[3] & 0x3Fu);
    }
    return 0xFFFD;      // 替换字符：表示这个码点解不出来
}

}  // namespace

namespace term {

// ---------------------------------------------------------------------------
//  tick_cursor：定时器中断每次调用一次（光标闪烁的心跳）
//  -------------------------------------------------------------------------
//  累计 tick，到周期就翻转光标亮/灭。
// ---------------------------------------------------------------------------
void tick_cursor()
{
    ++g_cursor_ticks;
    if (g_cursor_ticks >= BLINK_PERIOD) {
        g_cursor_ticks = 0;
        g_cursor_visible = !g_cursor_visible;
        set_cursor_visible(g_cursor_visible);
    }
}

// ---------------------------------------------------------------------------
//  cursor_moved：光标位置被程序改过（输入、退格、方向键等）
//  -------------------------------------------------------------------------
//  让光标**立刻**重新显示（并擦掉旧位置），不等下一个闪烁周期。
//  否则快速输入时光标会"跟不上手"，体验很差。
// ---------------------------------------------------------------------------
void cursor_moved()
{
    g_cursor_ticks = 0;
    g_cursor_visible = true;
    set_cursor_visible(true);
}

void init(u64 info_phys)
{
    // 先尝试图形帧缓冲（BIOS 的 VBE 模式、UEFI 的 GOP 都走这条路）
    fb::init(info_phys);

    if (fb::info().available) {
        g_backend = Backend::Framebuffer;
        g_columns = static_cast<int>(fb::info().width) / font::WIDTH;
        g_rows    = static_cast<int>(fb::info().height) / font::HEIGHT;
        fb::clear(g_bg);
    } else {
        // 兜底：VGA 文本模式
        g_backend = Backend::VgaText;
        g_columns = VGA_WIDTH;
        g_rows    = VGA_HEIGHT;
        volatile u16* buf = vga_buffer();
        u16 blank = static_cast<u16>((static_cast<u16>(g_vga_attr) << 8) | ' ');
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i) {
            buf[i] = blank;
        }
    }

    // -----------------------------------------------------------------
    //  【关键 bug 修复】清屏必须同时清空字符网格。
    //    否则网格里还留着清屏前的内容，
    //    光标闪烁擦除时会把那些"幽灵字符"重新画出来。
    // -----------------------------------------------------------------
    for (int y = 0; y < MAX_ROWS; ++y) {
        for (int x = 0; x < MAX_COLS; ++x) {
            g_screen_code[y][x] = ' ';
        }
    }

    g_col = 0;
    g_row = 0;
    g_cursor_drawn = false;
    g_cursor_last_col = -1;
    g_cursor_last_row = -1;
}

Backend backend()
{
    return g_backend;
}

int columns() { return g_columns; }
int rows()    { return g_rows;    }

void clear()
{
    if (g_backend == Backend::Framebuffer) {
        fb::clear(g_bg);
    } else if (g_backend == Backend::VgaText) {
        volatile u16* buf = vga_buffer();
        u16 blank = static_cast<u16>((static_cast<u16>(g_vga_attr) << 8) | ' ');
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i) {
            buf[i] = blank;
        }
    }
    g_col = 0;
    g_row = 0;

    // 清屏会把指针和它的背景备份一起抹掉 —— 先擦（作废旧备份），再重画。
    mouse::hide();
    // 清屏把鼠标指针一起抹掉了 —— 重新画回来。
    // （指针直接画在帧缓冲上，不走字符网格，清屏路径不知道它的存在）
    mouse::show();
}

void set_color(u32 fg, u32 bg)
{
    g_fg = fg;
    g_bg = bg;
    g_vga_attr = static_cast<u8>(
        (nearest_vga_color(bg) << 4) | nearest_vga_color(fg));
}

void putc(char c)
{
    if (g_backend == Backend::None) {
        return;
    }

    u8 byte = static_cast<u8>(c);

    // 情况一：不在多字节序列中，且是普通 ASCII -> 直接输出
    if (g_utf8_need == 0 && byte < 0x80) {
        emit_ascii(c);
        cursor_moved();
        return;
    }

    if (g_utf8_need == 0) {
        // 情况二：多字节序列的起始字节。
        // 起始字节高位连续 1 的个数就是总字节数：
        //   110xxxxx = 2 字节，1110xxxx = 3 字节（中文都是这种），11110xxx = 4 字节
        if      ((byte & 0xE0) == 0xC0) { g_utf8_need = 2; }
        else if ((byte & 0xF0) == 0xE0) { g_utf8_need = 3; }
        else if ((byte & 0xF8) == 0xF0) { g_utf8_need = 4; }
        else {
            emit_ascii('?');            // 非法起始字节
            return;
        }
        g_utf8_buf[0] = byte;
        g_utf8_len = 1;
    } else {
        // 情况三：序列的后续字节，必须是 10xxxxxx 形式
        if ((byte & 0xC0) != 0x80) {
            // 序列被打断（传输出错或指针错位），丢掉重来，避免一直错下去
            g_utf8_need = 0;
            g_utf8_len = 0;
            emit_ascii('?');
            return;
        }
        g_utf8_buf[g_utf8_len++] = byte;
    }

    // 攒够了就解码输出
    if (g_utf8_len >= g_utf8_need) {
        u32 cp = decode_utf8(g_utf8_buf, g_utf8_need);
        g_utf8_need = 0;
        g_utf8_len = 0;
        emit_codepoint(cp);
    }

    // 光标跟着刚输出的内容走：打字时**常亮**，停手后才开始闪烁。
    // 这和真实终端的手感一致 —— 一敲键光标就亮，不动了才慢慢闪。
    cursor_moved();
}

void puts(const char* str)
{
    if (str == nullptr) {
        return;
    }
    // ---------------------------------------------------------------
    //  【鼠标与终端的协作】指针直接画在帧缓冲上，不走字符网格，
    //  终端输出时不知道它的存在 → 文字会把指针盖掉一块。
    //
    //  做法：输出**前**擦掉指针（用备份还原背景），
    //        输出**后**重新保存背景并画回指针。
    //  这样指针永远浮在文字上层，且不会残留残影。
    //
    //  只在 puts（字符串）这一层做，不在 putc 里做 ——
    //  否则每输出一个字符都要存取 12x19 像素，开销没必要。
    // ---------------------------------------------------------------
    mouse::hide();
    while (*str != '\0') {
        putc(*str);
        ++str;
    }
    mouse::show();
}

void puts_n(const char* str, int len)
{
    if (str == nullptr) {
        return;
    }
    for (int i = 0; i < len && str[i] != '\0'; ++i) {
        putc(str[i]);
    }
}

}  // namespace term
