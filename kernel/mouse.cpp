// ===========================================================================
//  kernel/mouse.cpp —— 鼠标指针绘制（图形层）
//  ---------------------------------------------------------------------------
//  【微内核分工】
//    策略 + 数据解析在用户态（mouse 服务进程）：
//      初始化 PS/2、读 0x60 端口、把 3 字节包解成 (dx, dy, 按键)。
//    机制在内核（本文件）：
//      真正的像素读写要在帧缓冲上做，那是内核管的资源。
//    用户态服务算好新坐标，用 Sys::MOUSE_DRAW 让内核画。
//
//  为什么要"先存背景再画"？
//    帧缓冲写上去就覆盖了原来的像素，读不回来就没法擦。
//    所以指针移动前，先把指针覆盖区域的像素存进备份缓冲；
//    移动时先用备份恢复旧位置，再保存新位置的背景并画指针。
// ===========================================================================
#include <kernel/mouse.hpp>
#include <kernel/printf.hpp>
#include <kernel/framebuffer.hpp>

namespace mouse {

// --- 指针尺寸与形状 ---
// 经典箭头 12x19。'#' = 指针本体，'.' = 透明
static constexpr int CUR_W = 12;
static constexpr int CUR_H = 19;
static const char* const CURSOR[CUR_H] = {
    "#...........",
    "##..........",
    "###.........",
    "####........",
    "#####.......",
    "######......",
    "#######.....",
    "########....",
    "#########...",
    "##########..",
    "###########.",
    "############",
    "##########..",
    "##..#####...",
    "#....#####..",
    ".....######.",
    ".....######.",
    "......#####.",
    ".......###..",
};

static bool     g_visible   = false;   // 指针当前是否画在屏幕上
static int      g_x         = 0;       // 左上角坐标
static int      g_y         = 0;
static int      g_old_x     = 0;
static int      g_old_y     = 0;
static bool     g_has_old   = false;
static u32      g_backup[CUR_H][CUR_W];// 指针下面的原始像素
static void*    g_backup_target = nullptr;  // 备份时用的是哪个渲染目标

// 前景（指针）颜色：亮白；描边用黑色，在深色/浅色背景上都能看清
static constexpr u32 FG = 0x00FFFFFFu;
static constexpr u32 EDGE = 0x00000000u;

static bool in_bounds(int x, int y)
{
    const fb::Info& info = fb::info();
    if (!info.available) return false;
    return x >= 0 && y >= 0
        && x + CUR_W <= static_cast<int>(info.width)
        && y + CUR_H <= static_cast<int>(info.height);
}

void init()
{
    const fb::Info& info = fb::info();
    g_visible = false;
    g_has_old = false;
    if (info.available) {
        g_x = static_cast<int>(info.width) / 2;
        g_y = static_cast<int>(info.height) / 2;
        // ⚠️【修复】这里**不再直接画指针**。
        //  本函数曾在"离屏模式"下被调用（动画加载阶段），
        //  指针被画进离屏缓冲，随后随"背景上移"被推上屏幕成为**静态残影**，
        //  而真指针稍后又画在新位置 —— 于是屏幕上出现"两个鼠标"。
        //
        //  现在只初始化坐标，真正的绘制交给 kmain 在
        //  渲染目标切回真帧缓冲之后调用 mouse::show() 完成。
    }
}

bool available()
{
    return fb::info().available;
}

// 保存指针区域的原始像素
static void save_backup(int x, int y)
{
    // 记下这份备份属于哪个渲染目标（真屏 or 离屏）。
    // 目标换了之后备份就作废 —— 拿离屏的备份往真屏上恢复会画出一坨垃圾。
    g_backup_target = fb::target();
    for (int dy = 0; dy < CUR_H; ++dy) {
        for (int dx = 0; dx < CUR_W; ++dx) {
            g_backup[dy][dx] = fb::read_pixel(
                static_cast<u32>(x + dx), static_cast<u32>(y + dy));
        }
    }
}

// 用备份恢复该区域
static void restore_backup(int x, int y)
{
    for (int dy = 0; dy < CUR_H; ++dy) {
        for (int dx = 0; dx < CUR_W; ++dx) {
            fb::put_pixel(static_cast<u32>(x + dx),
                          static_cast<u32>(y + dy),
                          g_backup[dy][dx]);
        }
    }
}

// 画指针本体（带一圈描边，任何背景色上都可见）
static void draw_shape(int x, int y)
{
    for (int dy = 0; dy < CUR_H; ++dy) {
        for (int dx = 0; dx < CUR_W; ++dx) {
            if (CURSOR[dy][dx] != '#') continue;

            // 描边：相邻格是透明就画黑边
            bool edge = false;
            if (dx == 0 || CURSOR[dy][dx - 1] == '.') edge = true;
            if (dx == CUR_W - 1 || CURSOR[dy][dx + 1] == '.') edge = true;
            if (dy == 0 || CURSOR[dy - 1][dx] == '.') edge = true;
            if (dy == CUR_H - 1 || CURSOR[dy + 1][dx] == '.') edge = true;

            fb::put_pixel(static_cast<u32>(x + dx), static_cast<u32>(y + dy),
                          edge ? EDGE : FG);
        }
    }
}

void hide()
{
    if (!g_visible || !g_has_old) return;
    // 备份必须属于**当前**渲染目标，否则恢复出来是垃圾像素
    if (g_backup_target == fb::target()) {
        restore_backup(g_old_x, g_old_y);
    }
    g_visible = false;
    g_has_old = false;
}

void move_to(int x, int y)
{
    if (!available()) return;

    // 先擦掉旧位置的指针
    hide();

    // 夹到屏幕内
    const fb::Info& info = fb::info();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + CUR_W > static_cast<int>(info.width))  x = static_cast<int>(info.width) - CUR_W;
    if (y + CUR_H > static_cast<int>(info.height)) y = static_cast<int>(info.height) - CUR_H;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    g_x = x;
    g_y = y;

    if (!in_bounds(x, y)) return;

    save_backup(x, y);
    draw_shape(x, y);
    g_old_x = x;
    g_old_y = y;
    g_has_old = true;
    g_visible = true;
}

// 重新把指针画出来（用于终端清屏/重绘之后）
//
//  为什么需要它：
//    指针是直接画在帧缓冲上的，终端一清屏（fb::clear）就被抹掉了，
//    而清屏路径并不经过 mouse —— 于是开机画的指针在 shell 启动后消失，
//    用户看到的就是"鼠标没适配"。
//    清屏/重绘后调一次 show()，指针就回来了。
// 指针移到屏幕正中，并返回中心坐标给调用方同步
u32 center()
{
    const fb::Info& info = fb::info();
    int cx = 0, cy = 0;
    if (info.available) {
        cx = (static_cast<int>(info.width)  - CUR_W) / 2;
        cy = (static_cast<int>(info.height) - CUR_H) / 2;
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        move_to(cx, cy);
    }
    return (static_cast<u32>(cy) << 16) | static_cast<u32>(cx & 0xFFFF);
}

void invalidate()
{
    // 只丢状态，绝不 restore_backup —— 备份属于旧的渲染目标，
    // 写到新目标上就是一块垃圾像素。
    g_visible = false;
    g_has_old = false;
}


bool visible()
{
    return g_visible;
}

void show()
{
    if (!available()) return;
    int cx = g_x, cy = g_y;

    // ---------------------------------------------------------------
    //  【修复】原来这里无条件 invalidate() —— 只丢状态、**不擦除**。
    // ---------------------------------------------------------------
    //  后果链（这就是"回车一次多一个鼠标"的真因）：
    //    1. 屏幕上已经画着指针（g_visible=true）
    //    2. show() 把 g_visible 改成 false，但**像素还在**
    //    3. move_to → hide() 因为 g_visible=false 直接 return，也不擦
    //    4. save_backup 于是把"带指针的画面"存进备份
    //    5. 下次真正 hide() 时，恢复出来的还是指针 → 擦不掉
    //  终端每次 puts 结束都调 show()，于是每回车一次就多一个残影。
    //
    //  现在：只要备份还属于当前目标，就先**真正擦掉**旧指针再重画。
    //  目标已切换（清屏/离屏→真屏）时才只丢状态。
    // ---------------------------------------------------------------
    if (g_visible && g_has_old && g_backup_target == fb::target()) {
        restore_backup(g_old_x, g_old_y);
    }
    invalidate();
    move_to(cx, cy);
}

void move_by(int dx, int dy)
{
    move_to(g_x + dx, g_y + dy);
}

int x() { return g_x; }
int y() { return g_y; }

}   // namespace mouse
