// ============================================================================
//  kernel/bootanim.cpp —— 开机动画实现
//  ---------------------------------------------------------------------------
//  设计要点：
//
//  1) 计时用 **PIT 通道 2**，不动通道 0。
//     通道 0 是调度器用的（pit::init 配成 100Hz），
//     动画若去改它，调度就乱了。通道 2 平时没人用，专门借来计时。
//
//  2) 螺旋轨迹用「向量旋转 + 长度收缩」，**不需要 sqrt / atan2**。
//     直接把「起点相对中心的向量」绕中心旋转 θ(t) 并乘以 (1-t)：
//         t=0  在起点（左上角）
//         t=1  在中心
//        中间  旋转 + 收缩 = 螺旋
//     比极坐标写法简单得多，还避开了内核里没有的浮点库函数。
//
//  3) sin/cos 用整数泰勒展开，象限归约到 0..90 度，精度足够画画。
//
//  4) 星形填充用扫描线算法（对凹多边形即五角星同样正确）。
// ============================================================================

#include <kernel/bootanim.hpp>
#include <kernel/framebuffer.hpp>
#include <kernel/pmm.hpp>
#include <kernel/vmm.hpp>
#include <kernel/io.h>
#include <kernel/mouse.hpp>
#include <kernel/printf.hpp>

// ===========================================================================
//  常量
// ===========================================================================
namespace {

// 离屏缓冲映射的虚拟地址。
// 内核镜像在 0xFFFFFFFF80000000，堆在 0xfffffe8000000000，
// 这里取 0xFFFFFFFF90000000，与两者都隔得很开。
constexpr u64 OFFSCREEN_VA = 0xFFFFFFFF90000000ull;

// 五角星的内接比：标准五角星内径 / 外径 = (3-√5)/2 ≈ 0.382
constexpr int STAR_INNER_PER_MILLE = 382;

// 光晕三层：相对半径(%) 与 亮度(%)
constexpr int GLOW_LAYERS   = 3;
constexpr int GLOW_SCALE[3] = { 100, 62, 34 };
constexpr int GLOW_BRIGHT[3]= { 18, 45, 100 };

// 各阶段时长（毫秒）
constexpr u32 MS_SPIRAL   = 1500;   // ① 螺旋入场
constexpr u32 MS_SHRINK   = 300;    // ② 收缩消失
constexpr u32 MS_BURST    = 400;    // ④ 突然放出
constexpr u32 MS_FADE     = 200;    // ④ 之后星星淡出
constexpr u32 MS_SCROLL   = 600;    // ⑤ 背景上移

// 螺旋圈数
constexpr int SPIRAL_TURNS = 2;      // 转 2 圈（约 720 度）

// ---------------------------------------------------------------------------
//  整数三角函数（返回 sin × 1024）
// ---------------------------------------------------------------------------
//  内核没有 libm，浮点也不宜引入。用泰勒展开 + 象限归约：
//    sin(x) ≈ x - x³/6 + x⁵/120 - x⁷/5040
//  x 用 Q16 定点弧度。先归约到 0..90 度再算，精度足够。
// ---------------------------------------------------------------------------
constexpr long long PI_HALF_Q16 = 102944LL;      // π/2 × 65536

// 计算 0..π/2 区间的 sin，输入输出均为 Q16 定点
long long sin_q16_first_quadrant(long long x)
{
    long long x2 = (x * x) >> 16;
    long long x3 = (x2 * x) >> 16;
    long long x5 = (x3 * x2) >> 16;
    long long x7 = (x5 * x2) >> 16;
    return x - x3 / 6 + x5 / 120 - x7 / 5040;
}

// sin(deg) × 1024
int isin_deg(int deg)
{
    // 归一化到 0..359
    deg %= 360;
    if (deg < 0) deg += 360;

    int q = deg / 90;
    int r = deg % 90;

    long long x = (PI_HALF_Q16 * r) / 90;      // 0..90 度 → 0..π/2
    long long s;
    switch (q) {
        case 0:  s =  sin_q16_first_quadrant(x);             break;  // sin(r)
        case 1:  s =  sin_q16_first_quadrant(PI_HALF_Q16 - x); break;  // cos(r)
        case 2:  s = -sin_q16_first_quadrant(x);             break;
        default: s = -sin_q16_first_quadrant(PI_HALF_Q16 - x); break;
    }
    // Q16 → Q10（×1024）
    return static_cast<int>((s * 1024) / 65536);
}

int icos_deg(int deg) { return isin_deg(deg + 90); }

// ---------------------------------------------------------------------------
//  缓动函数（输入 0..1000，输出 0..1000）
// ---------------------------------------------------------------------------
int ease_in(int tp)  { return (tp * tp) / 1000; }

int ease_out(int tp)
{
    int u = 1000 - tp;
    return 1000 - (u * u) / 1000;
}

// 带回弹的缓出（放出时用，末段会略微过冲再回落）
//   f(t) = 1 + c3·(t-1)³ + c1·(t-1)²
//   c1 = 1.70158, c3 = 2.70158
int ease_out_back(int tp)
{
    long long u  = tp - 1000;                  // 负
    long long u2 = (u * u) / 1000;
    long long u3 = (u2 * u) / 1000;
    long long f  = 1000 + (2702 * u3) / 1000 + (1702 * u2) / 1000;
    if (f < 0) f = 0;
    return static_cast<int>(f);
}

// ---------------------------------------------------------------------------
//  计时：借用 PIT 通道 2
// ---------------------------------------------------------------------------
static inline u64 rdtsc()
{
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<u64>(hi) << 32) | lo;
}

//  为什么不用通道 0？那是调度器的（pit::init 配成 100Hz），
//  动画改它会把调度搞乱。通道 2 平时闲置，借来计时最安全。
// ---------------------------------------------------------------------------
bool     g_timer_ready = false;
u64      g_tsc_base     = 0;
u64      g_tsc_per_us   = 2000;      // TSC 每微秒计数（启动时校准）

void timer_init()
{
    // ===========================================================
    //  【性能修复】计时方式从 PIT 改为 **TSC（rdtsc）**
    // -----------------------------------------------------------
    //  原实现每次 now_us() 都要 outb(0x43) + 两次 inb(0x42)。
    //  PIT 端口访问在 QEMU 下**极慢**（每次约微秒级），
    //  而 wait_until_us 是忙等循环、每轮都调 now_us() ——
    //  于是每一帧都在疯狂做慢速端口 I/O，动画被拖到几乎不动，
    //  开机卡在动画阶段进不了系统（实测复现）。
    //
    //  TSC 是 CPU 内部计数器，一条 rdtsc 指令即可读取（几十纳秒），
    //  完全没有端口开销。只在启动时用 PIT **校准一次**得到频率，
    //  之后全程用 TSC。
    // ===========================================================

    // --- 用 PIT 通道 2 校准 TSC 频率 ---
    //  只在校准时短暂打开 gate，用完立刻恢复，避免扬声器被误触发。
    u8 port61_orig = inb(0x61);
    outb(0x61, static_cast<u8>(port61_orig | 0x01));    // 只开 gate(bit0)，不动 speaker(bit1)

    outb(0x43, 0xB4);                    // 通道2, LSB/MSB, mode2, binary
    outb(0x42, 0xFF);
    outb(0x42, 0xFF);

    // 累计 PIT 计数直到攒够约 1/8 秒的计数（14915 ≈ 12.5ms）
    //  用**累计**而非单次读，规避单次读数抖动。
    auto pit_read = []() -> u16 {
        outb(0x43, 0x80);                // 锁存通道 2
        u8 lo = inb(0x42);
        u8 hi = inb(0x42);
        return static_cast<u16>(lo | (hi << 8));
    };

    const u32 WANT = 14915u;             // ≈ 12.5 ms
    u32 acc = 0;
    u16 last = pit_read();
    u64 t0 = rdtsc();
    u64 guard = 0;
    while (acc < WANT) {
        u16 cur = pit_read();
        acc += ((static_cast<u32>(last) - cur) & 0xFFFFu);
        last = cur;
        if (++guard > 5000000ull) break;    // 超时保护：别在校准里卡死
    }
    u64 t1 = rdtsc();

    outb(0x61, port61_orig);             // 恢复 0x61（关 gate，避免噪音）

    // 采到足够计数才采信校准结果，否则用保守默认值（按 2 GHz 估）
    if (acc >= WANT / 2 && t1 > t0) {
        u64 us = (static_cast<u64>(acc) * 1000000ull) / 1193182ull;
        if (us > 0) g_tsc_per_us = (t1 - t0) / us;
    }
    if (g_tsc_per_us == 0) g_tsc_per_us = 2000;   // 兜底：2 GHz

    g_tsc_base     = rdtsc();
    g_timer_ready  = true;
}

u64 now_us()
{
    if (!g_timer_ready) return 0;
    u64 d = rdtsc() - g_tsc_base;
    return d / g_tsc_per_us;
}

// ---------------------------------------------------------------------------
//  内核是 freestanding，没有 libc 的 memcpy/memset。
//  __builtin_memcpy 在长度是**运行时变量**时会退化成函数调用，
//  链接时找不到符号（实测报 undefined reference to `memcpy'）。
//
//  所以自己写：按 8 字节块拷贝，比逐字节快一个量级。
// ---------------------------------------------------------------------------
static void block_copy(u8* dst, const u8* src, u64 n)
{
    u64 i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 v;
        __builtin_memcpy(&v, src + i, 8);      // 定长 8，会被内联成一条 mov
        __builtin_memcpy(dst + i, &v, 8);
    }
    for (; i < n; ++i) dst[i] = src[i];
}

static void block_zero(u8* dst, u64 n)
{
    u64 i = 0;
    const u64 z = 0;
    for (; i + 8 <= n; i += 8) {
        __builtin_memcpy(dst + i, &z, 8);      // 定长，内联
    }
    for (; i < n; ++i) dst[i] = 0;
}

// 忙等到累计时间达到 target_us（动画阶段没有调度器，只能忙等）
void wait_until_us(u64 target_us)
{
    // ---------------------------------------------------------------
    //  基于 TSC 的忙等。
    //  仍保留超时保护：万一 TSC 异常（比如某些虚拟机不提供），
    //  也不能让开机被永久卡死 —— 宁可动画节奏不准，也要保证能进系统。
    // ---------------------------------------------------------------
    if (!g_timer_ready) return;

    u64 deadline = g_tsc_base + target_us * g_tsc_per_us;
    u64 guard = 0;
    const u64 LIMIT = 500000000ull;      // 兜底上限
    while (rdtsc() < deadline) {
        if (++guard >= LIMIT) break;
    }
}

// ---------------------------------------------------------------------------
//  颜色工具
// ---------------------------------------------------------------------------
u32 scale_color(u32 c, int percent)
{
    u32 r = ((c >> 16) & 0xFF) * static_cast<u32>(percent) / 100;
    u32 g = ((c >> 8)  & 0xFF) * static_cast<u32>(percent) / 100;
    u32 b = (c         & 0xFF) * static_cast<u32>(percent) / 100;
    return (r << 16) | (g << 8) | b;
}

// ---------------------------------------------------------------------------
//  五角星填充（扫描线算法，对凹多边形正确）
// ---------------------------------------------------------------------------
void fill_star_poly(int cx, int cy, int radius, int rot_deg, u32 color)
{
    const fb::Info& fi = fb::info();
    if (!fi.available || radius < 1) return;

    int px[10], py[10];
    int inner = radius * STAR_INNER_PER_MILLE / 1000;

    for (int i = 0; i < 10; ++i) {
        int r = (i & 1) ? inner : radius;
        int a = rot_deg - 90 + i * 36;          // -90 让尖端朝上
        px[i] = cx + (r * icos_deg(a)) / 1024;
        py[i] = cy + (r * isin_deg(a)) / 1024;
    }

    int miny = py[0], maxy = py[0];
    for (int i = 1; i < 10; ++i) {
        if (py[i] < miny) miny = py[i];
        if (py[i] > maxy) maxy = py[i];
    }
    if (miny < 0) miny = 0;
    if (maxy >= static_cast<int>(fi.height)) maxy = static_cast<int>(fi.height) - 1;
    if (miny > maxy) return;

    for (int y = miny; y <= maxy; ++y) {
        int xs[10];
        int n = 0;

        // 求每条边与扫描线 y 的交点
        for (int i = 0; i < 10; ++i) {
            int j = (i + 1) % 10;
            int y0 = py[i], y1 = py[j];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                int dx = px[j] - px[i];
                int dy = y1 - y0;
                xs[n++] = px[i] + (dx * (y - y0)) / dy;
            }
        }
        if (n < 2) continue;

        // 冒泡排序（n ≤ 10，无所谓效率）
        for (int i = 0; i < n - 1; ++i) {
            for (int j = i + 1; j < n; ++j) {
                if (xs[j] < xs[i]) { int t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
            }
        }

        // 成对填充
        for (int i = 0; i + 1 < n; i += 2) {
            int x0 = xs[i];
            int x1 = xs[i + 1] + 1;
            if (x0 < 0) x0 = 0;
            if (x1 > static_cast<int>(fi.width)) x1 = static_cast<int>(fi.width);
            if (x1 > x0) {
                fb::fill_rect(static_cast<u32>(x0), static_cast<u32>(y),
                              static_cast<u32>(x1 - x0), 1, color);
            }
        }
    }
}

}  // namespace

// ===========================================================================
//  公开接口
// ===========================================================================
namespace bootanim {

// ---------------------------------------------------------------------------
//  系统官方图标：五角星 + 光晕
// ---------------------------------------------------------------------------
void draw_star(int cx, int cy, int radius, int rot_deg, u32 color)
{
    if (radius < 1) return;

    for (int l = 0; l < GLOW_LAYERS; ++l) {
        int r = radius * GLOW_SCALE[l] / 100;
        if (r < 1) continue;
        fill_star_poly(cx, cy, r, rot_deg, scale_color(color, GLOW_BRIGHT[l]));
    }
}

// ---------------------------------------------------------------------------
//  离屏缓冲
// ---------------------------------------------------------------------------
u8*  g_off      = nullptr;
u64  g_off_phys = 0;
u64  g_off_pages= 0;
bool g_off_ok   = false;

bool offscreen_active() { return g_off_ok; }

bool begin_offscreen()
{
    const fb::Info& fi = fb::info();
    if (!fi.available) return false;
    if (!g_timer_ready) timer_init();

    u64 size  = static_cast<u64>(fi.pitch) * fi.height;
    u64 pages = (size + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE;

    // ⚠️ 不能用堆：堆只有 192 KB，而整屏缓冲要 3 MB 左右。
    //    直接找 pmm 要连续物理页，再映射到固定的内核虚拟地址。
    u64 phys = pmm::alloc_frames(pages);
    if (phys == 0) {
        // 分配失败 → 降级：界面直接画到屏幕，跳过上移动画
        return false;
    }

    if (!vmm::map_pages(OFFSCREEN_VA, phys, pages, vmm::FLAGS_KERNEL)) {
        pmm::free_frames(phys, pages);
        return false;
    }

    g_off       = reinterpret_cast<u8*>(OFFSCREEN_VA);
    g_off_phys  = phys;
    g_off_pages = pages;

    // ---------------------------------------------------------------
    //  【修复】不要清零，而是先把真屏当前内容拷进离屏
    //  ---------------------------------------------------------------
    //  pmm / vmm / heap 的初始化日志是用 kprintf 画在**真屏**上的
    //  （它们必须早于 begin_offscreen，因为离屏要靠 pmm 分配）。
    //  原来这里清零 + 清真屏，那几行就被抹掉了 —— 用户看到
    //  "最上面的诊断少了几行"。
    //  现在先把真屏内容搬进离屏，后续输出接着往下画，内容就连续了。
    // ---------------------------------------------------------------
    block_copy(g_off, reinterpret_cast<u8*>(fi.addr), size);

    // 真屏幕清黑（加载期间屏幕保持黑），然后切到离屏
    fb::set_target(nullptr);
    fb::clear(fb::color::BLACK);

    // ⚠️ 渲染目标即将从真屏切到离屏：鼠标指针的像素备份属于真屏，
    //    切过去之后就失效了，必须丢弃（否则后面 restore 会把真屏像素
    //    写进离屏，再随上移推到屏幕上 = 莫名其妙的色块）。
    mouse::invalidate();

    fb::set_target(g_off);

    g_off_ok = true;
    return true;
}

// ---------------------------------------------------------------------------
//  阶段 ①②：螺旋入场 + 收缩消失
// ---------------------------------------------------------------------------
bool play_intro()
{
    const fb::Info& fi = fb::info();
    if (!fi.available) return false;

    timer_init();

    int W = static_cast<int>(fi.width);
    int H = static_cast<int>(fi.height);
    int ccx = W / 2;
    int ccy = H / 2;

    // 星星最终大小：取屏幕短边的 1/8，下限 12 像素
    int R = (W < H ? W : H) / 8;
    if (R < 12) R = 12;

    // 起点：左上角（留 8% 边距，免得星星一开始就出屏）
    int sx = W / 10;
    int sy = H / 10;

    // 起点相对中心的向量 —— 螺旋就是"旋转这个向量 + 缩短它"
    int vx = sx - ccx;
    int vy = sy - ccy;

    const u32 color = fb::color::YELLOW;   // 星星是黄色的（用户要求）
    // ---- ① 螺旋入场 ----
    {
        u64 t0 = now_us();
        const int frames = 90;
        for (int f = 0; f <= frames; ++f) {
            int tp = f * 1000 / frames;              // 0..1000
            int e  = ease_out(tp);                   // 收拢时减速

            fb::clear(fb::color::BLACK);

            // 旋转角度：e 越大转越多
            int ang = e * (360 * SPIRAL_TURNS) / 1000;
            int c = icos_deg(ang);
            int s = isin_deg(ang);

            // 剩余比例
            int remain = 1000 - e;

            int x = ccx + ((vx * c - vy * s) / 1024) * remain / 1000;
            int y = ccy + ((vx * s + vy * c) / 1024) * remain / 1000;

            // 星星自身也在转（转 2 圈）
            int rot = e * 720 / 1000;

            // 尺寸由小渐大
            int r = R * (30 + 70 * e / 1000) / 100;

            draw_star(x, y, r, rot, color);

            u64 target = t0 + (static_cast<u64>(MS_SPIRAL) * 1000ull * f) / frames;
            wait_until_us(target);
        }
    }
    // ---- ② 收缩消失 ----
    {
        u64 t0 = now_us();
        const int frames = 24;
        for (int f = 0; f <= frames; ++f) {
            int tp = f * 1000 / frames;
            int e  = ease_in(tp);                    // 加速收缩

            fb::clear(fb::color::BLACK);

            int r = R * (1000 - e) / 1000;
            if (r > 0) {
                // 亮度同时衰减，收成一个点后彻底不见
                int bright = 100 - (e * 100) / 1000;
                if (bright < 0) bright = 0;
                draw_star(ccx, ccy, r, 0, scale_color(color, bright));
            }

            u64 target = t0 + (static_cast<u64>(MS_SHRINK) * 1000ull * f) / frames;
            wait_until_us(target);
        }
    }

    // 收尾：确保全黑
    fb::clear(fb::color::BLACK);
    return true;
}

// ---------------------------------------------------------------------------
//  阶段 ④⑤：星星放出 + 背景上移
// ---------------------------------------------------------------------------
void play_outro()
{
    if (!g_timer_ready) timer_init();

    const fb::Info& fi = fb::info();
    if (!fi.available) return;

    // ---------------------------------------------------------------
    //  【修复】"两个鼠标"：先把离屏里残留的指针擦掉
    //  ---------------------------------------------------------------
    //  加载期间若画过指针（初始化或早期移动事件），它会留在离屏里。
    //  上移时整个离屏被拷到屏幕 —— 那个旧指针就**固化进背景**了；
    //  动画结束后 mouse::show() 又画一个真的，于是屏幕上出现两个。
    //
    //  必须在**切回真屏之前** hide，否则会擦到真屏上、离屏里的还在。
    // ---------------------------------------------------------------
    mouse::hide();

    int W = static_cast<int>(fi.width);
    int H = static_cast<int>(fi.height);
    int ccx = W / 2;
    int ccy = H / 2;

    int R = (W < H ? W : H) / 8;
    if (R < 12) R = 12;

    const u32 color = fb::color::YELLOW;   // 星星是黄色的（用户要求）

    // 先切回真帧缓冲：放出阶段要画在屏幕上
    fb::set_target(nullptr);

    // ⚠️ 目标从离屏切回真屏：离屏里存的指针备份同样作废
    //    （这段备份若被 restore，会把离屏内容画到真屏上，
    //     表现为每次回车多一块残影 —— 实测复现过）。
    mouse::invalidate();

    fb::clear(fb::color::BLACK);
    // ---- ④ 突然放出（带回弹） ----
    {
        u64 t0 = now_us();
        const int frames = 30;
        for (int f = 0; f <= frames; ++f) {
            int tp = f * 1000 / frames;
            int e  = ease_out_back(tp);              // 末段略过冲

            fb::clear(fb::color::BLACK);
            int r = R * e / 1000;
            if (r > 0) draw_star(ccx, ccy, r, 0, color);

            u64 target = t0 + (static_cast<u64>(MS_BURST) * 1000ull * f) / frames;
            wait_until_us(target);
        }
    }

    // ---- ④b 星星淡出（给背景上移让位） ----
    {
        u64 t0 = now_us();
        const int frames = 12;
        for (int f = 0; f <= frames; ++f) {
            int tp = f * 1000 / frames;

            fb::clear(fb::color::BLACK);
            int bright = 100 - (tp * 100) / 1000;
            if (bright > 0) {
                draw_star(ccx, ccy, R, 0, scale_color(color, bright));
            }

            u64 target = t0 + (static_cast<u64>(MS_FADE) * 1000ull * f) / frames;
            wait_until_us(target);
        }
        fb::clear(fb::color::BLACK);
    }
    // ---- ⑤ 背景上移揭示界面 ----
    if (g_off_ok && g_off != nullptr) {
        u64   pitch = fi.pitch;
        u64   row_bytes = (fi.bpp == 32)
                        ? static_cast<u64>(fi.width) * 4
                        : static_cast<u64>(fi.width) * 3;
        if (row_bytes > pitch) row_bytes = pitch;

        u8* screen = reinterpret_cast<u8*>(fi.addr);

        u64 t0 = now_us();
        const int frames = 36;
        for (int f = 0; f <= frames; ++f) {
            int tp = f * 1000 / frames;
            int e  = ease_out(tp);

            // offset 从 H 递减到 0：界面从下方升上来
            int off = H - (H * e / 1000);
            if (off < 0) off = 0;

            for (int y = 0; y < H; ++y) {
                int sy = y - off;                    // 离屏源行
                u8* dst = screen + static_cast<u64>(y) * pitch;
                if (sy < 0) {
                    // 还没进入视野的区域填黑
                    block_zero(dst, pitch);
                } else {
                    u8* src = g_off + static_cast<u64>(sy) * pitch;
                    block_copy(dst, src, row_bytes);
                    if (row_bytes < pitch) block_zero(dst + row_bytes, pitch - row_bytes);
                }
            }

            u64 target = t0 + (static_cast<u64>(MS_SCROLL) * 1000ull * f) / frames;
            wait_until_us(target);
        }

        // 最终再整屏拷一次，确保与离屏完全一致
        for (int y = 0; y < H; ++y) {
            u8* dst = screen + static_cast<u64>(y) * pitch;
            u8* src = g_off + static_cast<u64>(y) * pitch;
            block_copy(dst, src, row_bytes);
            if (row_bytes < pitch) block_zero(dst + row_bytes, pitch - row_bytes);
        }
    } else {
        // 降级：没有离屏缓冲，界面本来就在屏幕上，直接保持即可
    }
}

// ---------------------------------------------------------------------------
//  收尾
// ---------------------------------------------------------------------------
void finish()
{
    fb::set_target(nullptr);

    if (g_off_ok && g_off != nullptr) {
        vmm::unmap_page(OFFSCREEN_VA);       // 只需解第一页即可断掉整段访问
        // 更稳妥：逐页解映射
        for (u64 i = 0; i < g_off_pages; ++i) {
            vmm::unmap_page(OFFSCREEN_VA + i * pmm::PAGE_SIZE);
        }
        pmm::free_frames(g_off_phys, g_off_pages);
    }
    g_off      = nullptr;
    g_off_phys = 0;
    g_off_pages= 0;
    g_off_ok   = false;
}

}  // namespace bootanim
