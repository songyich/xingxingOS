// ============================================================================
//  kernel/services.cpp —— 用户态服务进程（微内核改造的核心部分）
//  ---------------------------------------------------------------------------
//  ⚠️ 重要：这个文件里的服务函数运行在 **Ring 3 用户态**，
//     它们**绝对不能**调用内核函数（kprintf / term::* / heap::* 等）。
//
//     为什么？内核代码映射在高半区，页表项 U/S=0（用户不可访问）。
//     用户态执行内核函数会立刻 #PF 页错误，进程崩溃。
//
//     所以这里的一切都要靠**系统调用**完成：
//       - 输出      -> IPC 发给终端服务
//       - 读端口    -> SYS_PORT_IN
//       - 映射设备  -> SYS_MMIO_MAP
//       - 等中断    -> SYS_IRQ_REGISTER + 阻塞
//
//  这正是微内核的样子：驱动和服务都是普通用户进程，
//  崩了只影响自己，不会拖垮整个内核。
// ============================================================================

#include <kernel/syscall.hpp>
#include <kernel/ipc.hpp>
#include <kernel/types.h>

// ===========================================================================
//  用户态基础库（不能依赖内核，全部自己实现）
// ===========================================================================
// 验证：用户态看到的 IpcMsg 必须是完整版（含 text[64]，共 120 字节）
static_assert(sizeof(IpcMsg) == 120, "用户态 IpcMsg 尺寸不对（可能用了旧定义编译）");

namespace ulib {

inline usize strlen(const char* s)
{
    usize n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

// --- 通过 IPC 向终端服务输出字符串 ---
void puts(const char* s)
{
    if (s == nullptr) return;
    usize len = strlen(s);
    if (len == 0) return;

    // 消息写到**共享页发送槽**（所有进程共用的物理页）
    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
    m->type = MSG_PUTS;
    m->ptr  = 0;
    m->size = 0;
    m->a = 0; m->b = 0; m->c = 0; m->d = 0;

    usize n = len < sizeof(m->text) - 1 ? len : sizeof(m->text) - 1;
    for (usize i = 0; i < n; ++i) m->text[i] = s[i];
    m->text[n] = '\0';
    m->len = static_cast<u32>(n);

    ipc_call(TID_TERMINAL, m);
}

void putc(char c)
{
    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
    m->type = MSG_PUTC;
    m->len  = 1;
    m->a    = static_cast<u64>(static_cast<u8>(c));
    m->ptr = 0; m->size = 0; m->b = 0; m->c = 0; m->d = 0;
    ipc_call(TID_TERMINAL, m);
}

// --- 数字转字符串 ---
void put_uint(u64 v, u32 base, bool upper)
{
    char buf[24];
    int i = 0;
    if (v == 0) {
        putc('0');
        return;
    }
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v > 0 && i < 23) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (i > 0) putc(buf[--i]);
}

void put_int(i64 v)
{
    if (v < 0) {
        putc('-');
        v = -v;
    }
    put_uint(static_cast<u64>(v), 10, false);
}

// --- 简化 printf：支持 %s %d %u %x %p %c %% ---
void printf(const char* fmt, ...)
{
    if (fmt == nullptr) return;

    // 变参：SysV ABI 下前 6 个参数走寄存器，
    // 这里用 __builtin_va_list 让编译器正确处理
    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    for (usize i = 0; fmt[i] != '\0'; ++i) {
        if (fmt[i] != '%') {
            putc(fmt[i]);
            continue;
        }
        ++i;
        if (fmt[i] == '\0') break;

        switch (fmt[i]) {
        case 's':
            puts(__builtin_va_arg(args, const char*));
            break;
        case 'd':
            put_int(__builtin_va_arg(args, int));
            break;
        case 'u':
            put_uint(__builtin_va_arg(args, u64), 10, false);
            break;
        case 'x':
            put_uint(__builtin_va_arg(args, u64), 16, false);
            break;
        case 'X':
            put_uint(__builtin_va_arg(args, u64), 16, true);
            break;
        case 'p':
            puts("0x");
            put_uint(__builtin_va_arg(args, u64), 16, true);
            break;
        case 'c':
            putc(static_cast<char>(__builtin_va_arg(args, int)));
            break;
        case '%':
            putc('%');
            break;
        default:
            putc('%');
            putc(fmt[i]);
            break;
        }
    }
    __builtin_va_end(args);
}

// --- 睡眠 ---
void sleep(u64 ms)
{
    syscall1(static_cast<u64>(Sys::SLEEP), ms);
}

// --- 运行时长 ---
u64 uptime()
{
    return syscall1(static_cast<u64>(Sys::UPTIME), 0);
}

}  // namespace ulib

// ===========================================================================
//  终端服务进程（TID_TERMINAL）
//  ---------------------------------------------------------------------------
//  职责：管理屏幕输出。其他进程要打印东西，就发 IPC 给它。
//
//  它在用户态直接写帧缓冲：
//    通过 SYS_MMIO_MAP 把显存映射到自己的地址空间，然后自己画像素。
//  这是真正的内核外驱动——内核完全不知道"屏幕"是什么。
// ============================================================================
namespace {

// 帧缓冲状态（服务进程自己维护）
u32* g_fb = nullptr;
u64  g_fb_width = 0;
u64  g_fb_height = 0;
u64  g_fb_pitch = 0;

// 光标位置
u64 g_col = 0;
u64 g_row = 0;

constexpr u64 CHAR_W = 8;
constexpr u64 CHAR_H = 16;

u32 g_fg = 0xFFFFFF;      // 前景色（白）
u32 g_bg = 0x000000;      // 背景色（黑）

// 8x16 字库：从内核映射过来的数据
extern const u8 g_font8x16[];
const u8* g_font = nullptr;

// 扫描码 -> ASCII 转换表（仅可打印字符，其余填 0）
const char k_keymap[128] = {
    0,   0,   '1','2','3','4','5','6','7','8','9','0','-','=','\b', 0,
    'q', 'w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a', 's','d','f','g','h','j','k','l',';','\'', '`',
    0, '\\','z','x','c','v','b','n','m',',','.','/',
    0, '*', 0, ' ',
};

}  // namespace

// 终端服务主循环
extern "C" void terminal_service_entry()
{
    // 服务的标准结构：循环等待请求
    //
    // 微内核下终端不再是内核的一部分，而是一个**用户态服务进程**。
    // 谁想往屏幕上写东西，就给它发一条 IPC 消息，
    // 它收到后自己去操作帧缓冲。
    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_RECV);
    int from = 0;

    for (;;) {
        // 等待任何进程发来的请求
        from = 0;
        ipc_recv(&from, m);

        // 同上：from 无效时跳过，避免 reply 失败死循环刷屏
        if (from <= 0) {
            continue;
        }

        // 目前先用内核的调试输出通道（Sys::PUTS）把内容送到屏幕。
        // 完整版会由终端服务自己映射帧缓冲并绘制 —— 那需要
        // MMIO_MAP 系统调用把显存物理地址映射进本进程地址空间。
        // 诊断：把用户态看到的 type 编码成一个字符输出
        //   1 -> type == 100（正确）
        //   0 -> type == 0（copy_msg 没写进去）
        //   ? -> 其他值

        if (m->type == MSG_PUTS) {
            syscall2(static_cast<u64>(Sys::PUTS),
                     reinterpret_cast<u64>(m->text), m->len);
        } else if (m->type == MSG_PUTC) {
            char c = static_cast<char>(m->a & 0xFF);
            syscall2(static_cast<u64>(Sys::PUTS),
                     reinterpret_cast<u64>(&c), 1);
        } else if (m->type == MSG_CLEAR) {
            // ---------------------------------------------------------
            //  【bug 修复】原来**根本没有处理 MSG_CLEAR**！
            //    所以 clear 命令发出消息后终端服务什么都不做，
            //    屏幕纹丝不动 —— 命令看起来"执行了"但没效果。
            //    现在转发给内核的 Sys::CLEAR。
            // ---------------------------------------------------------
            syscall0(static_cast<u64>(Sys::CLEAR));
        }

        // 回包，让发送方可以继续跑
        ipc_reply(from, m);

    }
}

// ---------------------------------------------------------------------------
//  键盘队列（全部状态放在共享页 —— 用户态不能用 static 变量）
//  ---------------------------------------------------------------------------
namespace kbdq {

inline u8 inb(u16 port)
{
    return static_cast<u8>(
        syscall2(static_cast<u64>(Sys::PORT_IN), port, 1));
}

inline void push(char c)
{
    volatile u32* head = reinterpret_cast<volatile u32*>(IPC_KBD_HEAD);
    volatile u32* tail = reinterpret_cast<volatile u32*>(IPC_KBD_TAIL);
    volatile u8*  buf  = reinterpret_cast<volatile u8*>(IPC_KBD_BUF);

    u32 t = *tail;
    u32 nt = (t + 1) % KBQ_CAP;
    if (nt == *head) return;        // 队列满，丢弃（避免覆盖未读数据）
    buf[t] = static_cast<u8>(c);
    *tail = nt;
}

inline char pop()
{
    volatile u32* head = reinterpret_cast<volatile u32*>(IPC_KBD_HEAD);
    volatile u32* tail = reinterpret_cast<volatile u32*>(IPC_KBD_TAIL);
    volatile u8*  buf  = reinterpret_cast<volatile u8*>(IPC_KBD_BUF);

    u32 h = *head;
    if (h == *tail) return 0;       // 队列空
    char c = static_cast<char>(buf[h]);
    *head = (h + 1) % KBQ_CAP;
    return c;
}

inline bool empty()
{
    volatile u32* head = reinterpret_cast<volatile u32*>(IPC_KBD_HEAD);
    volatile u32* tail = reinterpret_cast<volatile u32*>(IPC_KBD_TAIL);
    return *head == *tail;
}

// ---------------------------------------------------------------------------
//  排空键盘端口：把当前所有待处理的扫描码读完
//  ---------------------------------------------------------------------------
//  【核心 bug 修复】原来只在 shell 请求时**读一个字节**。
//    但一次按键会产生**两个**字节：按下码 + 松开码。
//    只读一个 → 松开码滞留在端口里，状态机认为"键还按着"，
//    后续所有按键都被当成"自动重复"而忽略。
//    实测：按 3 次 k 只输入 1 个字符，就是这个原因。
//
//  现在循环读 0x64 状态寄存器的 OBF 位，把数据全部取走。
// ---------------------------------------------------------------------------
inline void drain()
{
    volatile u8* state = reinterpret_cast<volatile u8*>(IPC_KBD_STATE);
    volatile u8* ext   = reinterpret_cast<volatile u8*>(IPC_KBD_EXT);

    // OBF（bit0）= 输出缓冲区有数据
    //
    // ⚠️【重大 bug 修复】必须同时检查 **bit5（AUXB）** 判断数据来源：
    //      bit5 = 0 → 键盘数据     bit5 = 1 → **鼠标数据**
    //
    //   PS/2 控制器里键盘和鼠标**共用 0x60 数据口**，
    //   OBF(bit0) 只表示"缓冲里有东西"，不表示是谁的。
    //   原来只看 bit0，于是鼠标移动产生的 3 字节数据包
    //   被键盘服务当成扫描码读走 → 译成字符送进 shell，
    //   表现为"一动鼠标就往命令行里打乱码"。
    //
    //   现在：bit5=1 说明是鼠标的，**break 留给鼠标服务**，键盘不抢。
    //   （break 而非 continue：数据留给别人读，自己不能一直空转。）
    while (true) {
        u8 st = inb(0x64);
        if ((st & 0x01) == 0) break;      // 缓冲空
        if (st & 0x20) break;             // 是鼠标数据，不归键盘管

        u8 sc = inb(0x60);

        if (sc == 0xE0) {
            // 扩展键前缀（方向键、Delete 等），等下一个字节
            *ext = 1;
            continue;
        }

        if (sc & 0x80) {
            // 松开码：清除"按住"标记，才允许下一次输入
            *state = 0;
            *ext = 0;
            continue;
        }

        if (*state == sc) {
            // 同一个键的自动重复（typematic）—— 忽略
            // 这正是"按住 0.3 秒输出 kkk"的成因
            *ext = 0;
            continue;
        }

        // 新按下的键
        *state = sc;
        u8 e = *ext;
        *ext = 0;

        if (!e && sc < 128) {
            char c = k_keymap[sc];
            if (c != 0) push(c);
        }
    }
}

}   // namespace kbdq

extern "C" void keyboard_service_entry()
{
    // --- 1. 告诉内核：IRQ1（键盘）归我管 ---
    syscall1(static_cast<u64>(Sys::IRQ_REGISTER), 1);


    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_RECV);
    volatile u32* pending = reinterpret_cast<volatile u32*>(IPC_KBD_PEND);
    *pending = 0;
    int from = 0;

    for (;;) {
        // 每轮先把端口排空（IRQ 唤醒后数据可能已经堆了好几个字节）
        kbdq::drain();

        // --- 有挂起请求且已有按键：立刻回复它 ---
        if (*pending != 0 && !kbdq::empty()) {
            IpcMsg* reply = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
            reply->type = MSG_KEY_GET;
            reply->len = 1;
            reply->a = static_cast<u64>(static_cast<u8>(kbdq::pop()));
            reply->ptr = 0; reply->size = 0;
            reply->b = 0; reply->c = 0; reply->d = 0;
            ipc_reply(static_cast<int>(*pending), reply);
            *pending = 0;
            continue;
        }

        // --- 等待请求 / 被 IRQ 唤醒 ---
        from = 0;
        ipc_recv(&from, m);

        if (from <= 0) {
            continue;       // 是被 IRQ 唤醒的，回去排空端口
        }

        if (m->type == MSG_KEY_GET || m->type == MSG_KEY_PEEK) {
            kbdq::drain();
            // 【死锁教训】这里**必须总是回复**。
            //   曾经改成"队列空就不回复、记下 pending 等 IRQ 再答"，
            //   结果死锁：ipc_recv 内部是重试循环（WOULD_BLOCK 就重试），
            //   IRQ 唤醒后若仍返回 WOULD_BLOCK，就永远出不了那个内层循环，
            //   外层 drain() 再也执行不到 —— 一个按键都收不到。
            //
            //   所以保持"总回复"，队列空就返回 0，由调用方（shell）重试。
            IpcMsg* reply = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
            reply->type = m->type;
            reply->len = 1;
            reply->a = static_cast<u64>(static_cast<u8>(kbdq::pop()));
            reply->ptr = 0; reply->size = 0;
            reply->b = 0; reply->c = 0; reply->d = 0;
            ipc_reply(from, reply);
        } else {
            IpcMsg* reply = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
            reply->type = 0; reply->len = 0;
            reply->a = 0; reply->b = 0; reply->c = 0; reply->d = 0;
            reply->ptr = 0; reply->size = 0;
            ipc_reply(from, reply);
        }
    }
}

// ===========================================================================
//  定时器服务进程（TID_TIMER）
// ============================================================================
// ===========================================================================
//  鼠标服务进程（用户态驱动）
//  ---------------------------------------------------------------------------
//  【万物皆可程序】鼠标就是一个普通的用户态服务进程，和键盘平级。
//    内核只提供：端口 I/O（特权指令）+ IRQ 通知 + 画指针。
//    初始化时序、数据包解析、坐标累加 —— 全在本进程里做。
//
//  PS/2 鼠标初始化序列（通过键盘控制器 0x64/0x60）：
//    0xA8 → 0x64   启用辅助端口（鼠标口）
//    0xD4 → 0x64   下一字节转发给鼠标
//    0xF4 → 0x60   使能数据流，鼠标回 0xFA
//    之后每次移动，鼠标主动发 3 字节包，并拉起 IRQ12。
//
//  数据包格式（3 字节）：
//    b0: bit3 恒为 1（用来同步） bit0/1/2 = 左/右/中键
//        bit4 = X 符号位  bit5 = Y 符号位  bit6/7 = 溢出
//    b1: X 位移（9 位有符号）
//    b2: Y 位移 —— 注意鼠标 Y 向上为正，屏幕向下为正，要取反
// ===========================================================================
namespace {

// 端口读写走 syscall（用户态不能直接用 in/out）
// ⚠️【重大 bug 修复】这两个辅助**漏传了"宽度"参数**！
//
//   Sys::PORT_IN  的约定是 a1=端口, **a2=字节宽度**
//   Sys::PORT_OUT 的约定是 a1=端口, a2=值, **a3=字节宽度**
//   内核里是 `if (a2 == 1)` / `if (a3 == 1)` 才真正执行 inb/outb，
//   否则直接返回 -1（宽度不匹配，什么都不做）。
//
//   原来这里：
//     port_in  → syscall1(PORT_IN, p)         缺 a2 → 宽度=0 → 永远返回 0xFF
//     port_out → syscall2(PORT_OUT, p, v)     缺 a3 → 宽度=0 → **端口根本没写**
//
//   结果：鼠标服务的所有端口读写**全部无效** ——
//   PS/2 初始化序列白写、配置字节没设、IRQ12 没使能、数据包永远读不到。
//   表现为"鼠标完全不动"（指针画得出来，因为那是内核画的，
//   但坐标从来不更新）。
//
//   对照：键盘服务在别处用的是 syscall2(PORT_IN, port, 1)，
//   传了宽度所以一直正常 —— 这也解释了为什么键盘能用、鼠标不能。
inline u8  port_in(u16 p) {
    return static_cast<u8>(syscall2(static_cast<u64>(Sys::PORT_IN), p, 1));
}
inline void port_out(u16 p, u8 v) {
    syscall3(static_cast<u64>(Sys::PORT_OUT), p, v, 1);
}

// 等输入缓冲空（bit1=1 表示还有数据没被控制器取走）
inline void wait_in_ready()
{
    for (int i = 0; i < 100000; ++i) {
        if ((port_in(0x64) & 0x02) == 0) return;
    }
}

// 等输出缓冲满（bit0=1 表示有数据可读）
inline bool wait_out_full()
{
    for (int i = 0; i < 100000; ++i) {
        if ((port_in(0x64) & 0x01) != 0) return true;
    }
    return false;
}

// 鼠标状态（本服务自己累加）
u64  g_mx = 0, g_my = 0;
u64  g_btn = 0;
int  g_pkt[3];
int  g_pkt_n = 0;

}

// 给鼠标发一个命令并等 0xFA 应答。
// PS/2 控制器：写 0xD4 表示"下一字节转发给鼠标（辅助端口）"。
static bool mouse_cmd(u8 cmd)
{
    wait_in_ready();
    port_out(0x64, 0xD4);              // 下一字节写给鼠标
    wait_in_ready();
    port_out(0x60, cmd);
    if (!wait_out_full()) return false;
    u8 ack = port_in(0x60);
    return ack == 0xFA;                 // 0xFA = ACK
}

extern "C" void mouse_service_entry()
{
    // --- 1. 完整初始化 PS/2 鼠标 ---
    //
    //  ⚠️ 原来的序列只做了 "0xA8 + 0xD4/0xF4"，**漏了最关键的一步：
    //  设置控制器配置字节的 bit1（IRQ12 使能）**。
    //  没有它，鼠标的数据包不会触发 IRQ12 —— 在 VirtualBox 上表现为
    //  "鼠标完全不动"（指针画出来了，但坐标永远不更新）。
    //
    //  标准序列：
    //    1) 0xA8  启用辅助端口（鼠标）
    //    2) 0x20  读配置字节
    //       → 置 bit1(IRQ12 使能) / bit0(IRQ1 使能)，清 bit5/bit4(时钟不禁用)
    //    3) 0x60  写回配置字节
    //    4) 0xD4+0xF6 鼠标设置默认值
    //    5) 0xD4+0xF4 使能数据流
    wait_in_ready();
    port_out(0x64, 0xA8);              // 启用辅助端口
    wait_in_ready();
    port_out(0x64, 0x20);              // 读配置字节命令
    wait_out_full();
    u8 cfg = port_in(0x60);
    cfg |= 0x02;                       // bit1 = IRQ12 使能 ← **关键一步**
    cfg |= 0x01;                       // bit0 = IRQ1 使能（键盘）
    cfg &= static_cast<u8>(~0x20);     // bit5 = 0，启用鼠标时钟（不禁用）
    cfg &= static_cast<u8>(~0x10);     // bit4 = 0，启用键盘时钟
    wait_in_ready();
    port_out(0x64, 0x60);              // 写配置字节命令
    wait_in_ready();
    port_out(0x60, cfg);               // 写回

    mouse_cmd(0xF6);                   // 鼠标：设置默认值
    mouse_cmd(0xF4);                   // 鼠标：使能数据流

    // --- 2. 告诉内核：IRQ12（鼠标）归我管 ---
    syscall1(static_cast<u64>(Sys::IRQ_REGISTER), 12);

    // --- 3. 初始坐标放屏幕中央 ---
    //
    //  ⚠️ 以前是传一个极大坐标 (1<<30) 让内核"夹到边界"，
    //  结果指针被夹到了屏幕**右下角**（不是中心），
    //  加上这里又把共享页清零 —— 指针看起来像没适配。
    //  现在用 MOUSE_CENTER 明确居中，并拿返回值同步本地累加值。
    volatile u64* sh_x   = reinterpret_cast<volatile u64*>(IPC_MOUSE_X);
    volatile u64* sh_y   = reinterpret_cast<volatile u64*>(IPC_MOUSE_Y);
    volatile u64* sh_btn = reinterpret_cast<volatile u64*>(IPC_MOUSE_BTN);

    {
        u64 c = syscall0(static_cast<u64>(Sys::MOUSE_CENTER));
        if (c != static_cast<u64>(-1)) {
            g_mx = static_cast<u64>(c & 0xFFFF);
            g_my = static_cast<u64>((c >> 16) & 0xFFFF);
        }
    }
    *sh_x = g_mx; *sh_y = g_my; *sh_btn = 0;

    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_RECV);
    int from = 0;

    for (;;) {
        // --- 每轮先把端口里堆积的字节全部读完 ---
        for (;;) {
            // 与键盘服务对称：bit5=1 才表示"这是我的数据"。
            //  bit0=0 → 缓冲空；bit5=0 → 键盘的数据，留给键盘服务读。
            u8 st = port_in(0x64);
            if ((st & 0x01) == 0) break;                 // 缓冲空
            if ((st & 0x20) == 0) break;                 // 是键盘数据，不抢

            u8 b = port_in(0x60);

            // 同步：包首字节 bit3 恒为 1
            if (g_pkt_n == 0 && (b & 0x08) == 0) {
                continue;                                 // 丢掉，重新同步
            }
            g_pkt[g_pkt_n++] = b;
            if (g_pkt_n < 3) continue;

            g_pkt_n = 0;
            int b0 = g_pkt[0], dx = g_pkt[1], dy = g_pkt[2];

            // 9 位有符号扩展
            if (b0 & 0x10) dx -= 256;      // X 符号位
            if (b0 & 0x20) dy -= 256;      // Y 符号位

            // Y 轴：鼠标向上为正，屏幕向下为正 → 取反
            dy = -dy;

            // 溢出位：忽略（真溢出时这一包不移动即可）
            if ((b0 & 0xC0) != 0) {
                dx = 0; dy = 0;
            }

            g_btn = static_cast<u64>(b0 & 0x07);

            // 累加坐标（用有符号中间量，避免 u64 下溢）
            long long nx = static_cast<long long>(g_mx) + dx;
            long long ny = static_cast<long long>(g_my) + dy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            g_mx = static_cast<u64>(nx);
            g_my = static_cast<u64>(ny);

            // 更新共享页 + 让内核重画指针
            *sh_x = g_mx;
            *sh_y = g_my;
            *sh_btn = g_btn;

            syscall3(static_cast<u64>(Sys::MOUSE_DRAW), 1,
                     static_cast<u64>(dx), static_cast<u64>(dy));
        }

        // --- 处理查询请求 ---
        from = 0;
        ipc_recv(&from, m);
        if (from <= 0) continue;         // 被 IRQ 唤醒，回去读端口

        IpcMsg* reply = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
        reply->type = m->type;
        reply->len = 0;
        reply->ptr = 0; reply->size = 0;
        if (m->type == MSG_MOUSE_GET) {
            reply->a = *sh_x;
            reply->b = *sh_y;
            reply->c = *sh_btn;
        } else {
            reply->a = 0; reply->b = 0; reply->c = 0;
        }
        reply->d = 0;
        ipc_reply(from, reply);
    }
}

extern "C" void timer_service_entry()
{
    syscall1(static_cast<u64>(Sys::IRQ_REGISTER), 0);

    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_RECV);
    int from = 0;

    for (;;) {
        ipc_recv(&from, m);

        IpcMsg* reply = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
        reply->type = m->type;
        reply->len = 0;
        reply->ptr = 0; reply->size = 0;
        reply->a = 0; reply->b = 0; reply->c = 0; reply->d = 0;

        switch (m->type) {
        case MSG_UPTIME:
            reply->a = ulib::uptime();
            break;
        case MSG_TICKS:
            reply->a = ulib::uptime() / 10;
            break;
        case MSG_SLEEP:
            ulib::sleep(m->a);
            break;
        default:
            break;
        }
        ipc_reply(from, reply);
    }
}

// ===========================================================================
//  电源管理服务进程（TID_POWER）
//  ===========================================================================
//
//  微内核分工：
//    策略（什么时候重启、要不要提示用户）→ 用户态服务，就是这里
//    机制（真正执行复位的特权指令）    → 内核的最小 syscall Sys::REBOOT
//
//  这样即使电源管理逻辑写得再复杂，崩了也只是一个服务进程挂掉，
//  不会拖垮整个内核 —— 这正是微内核相对宏内核的核心价值。
// ===========================================================================
extern "C" void power_service_entry()
{
    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_RECV);
    int from = 0;

    for (;;) {
        from = 0;
        ipc_recv(&from, m);
        if (from <= 0) continue;

        IpcMsg* reply = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
        reply->type = m->type;
        reply->len = 0;
        reply->ptr = 0; reply->size = 0;
        reply->a = 0; reply->b = 0; reply->c = 0; reply->d = 0;

        switch (m->type) {
        case MSG_REBOOT:
            // 先给请求方回包（否则它一直阻塞），再执行复位
            ipc_reply(from, reply);
            syscall0(static_cast<u64>(Sys::REBOOT));
            break;

        case MSG_SHUTDOWN:
            ipc_reply(from, reply);
            syscall0(static_cast<u64>(Sys::SHUTDOWN));
            break;

        case MSG_HALT:
            ipc_reply(from, reply);
            syscall0(static_cast<u64>(Sys::EXIT));
            break;

        default:
            reply->a = static_cast<u64>(-1);    // 不支持的操作
            ipc_reply(from, reply);
            break;
        }
    }
}

// ===========================================================================
//  进程信息服务进程（TID_PROC）
//  ===========================================================================
//  提供 ps 的底层数据来源。
//
//  为什么不直接让 shell 调 syscall 查？
//    微内核下"信息也应该由服务提供"，shell 只管交互和显示。
//    将来要做权限控制（A 用户不能看 B 用户的进程），
//    策略就加在这里，不用动内核。
// ===========================================================================
extern "C" void proc_service_entry()
{
    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_RECV);
    int from = 0;

    for (;;) {
        from = 0;
        ipc_recv(&from, m);
        if (from <= 0) continue;

        IpcMsg* reply = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
        reply->type = m->type;
        reply->len = 0;
        reply->ptr = 0; reply->size = 0;
        reply->a = 0; reply->b = 0; reply->c = 0; reply->d = 0;

        if (m->type == MSG_PS) {
            // 线程表写进共享页的 PS 区域（偏移 1024）
            // 布局见 thread::dump_info：每条 32 字节
            u8* buf = reinterpret_cast<u8*>(USER_IPC_PAGE + 1024);
            int n = static_cast<int>(
                syscall2(static_cast<u64>(Sys::THREAD_INFO),
                         reinterpret_cast<u64>(buf), 32));
            reply->a = static_cast<u64>(n);     // 返回条数
            ipc_reply(from, reply);
        } else {
            reply->a = static_cast<u64>(-1);
            ipc_reply(from, reply);
        }
    }
}

// ===========================================================================
//  Shell 进程（TID_SHELL）—— 也是用户态！
//  ---------------------------------------------------------------------------
//  连 shell 都不在内核里了。它只是一个普通用户进程，
//  通过 IPC 向终端服务要输出、向键盘服务要输入。
// ============================================================================
namespace {

// ---------------------------------------------------------------------------
//  do_reboot：向电源服务发重启请求
//  -------------------------------------------------------------------------
//  shell 不碰任何硬件，只发一条 IPC —— 这就是微内核的样子。
// ---------------------------------------------------------------------------
void do_reboot()
{
    // ⚠️【共享页陷阱 —— 极其隐蔽的 bug】
    //
    //   所有进程共享**同一物理页**的 SEND 槽（偏移 256）。
    //   而 ulib::puts() 内部也要用这个槽（它发 MSG_PUTS 给终端服务）。
    //
    //   原来先写 m->type = MSG_REBOOT，再调 ulib::puts() ——
    //   puts 立刻把同一个槽覆盖成 MSG_PUTS(100)，
    //   等 ipc_call 时读到的 type 已经是 100，
    //   电源服务收到后走 default 分支，重启永远不执行。
    //
    //   规则：**任何 puts/printf 之后，必须重新填写消息再发**。
    ulib::puts("  正在重启...\n");

    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
    m->type = MSG_REBOOT;
    m->len = 0;
    m->a = 0; m->b = 0; m->c = 0; m->d = 0;
    m->ptr = 0; m->size = 0;

    ipc_call(TID_POWER, m);
    // 正常不会走到这里（电源服务已经复位了）
    ulib::puts("  重启失败\n");
}

// ---------------------------------------------------------------------------
//  do_shutdown：向电源服务发关机请求
//  -------------------------------------------------------------------------
//  同样受"共享页陷阱"约束：puts 之后再填消息。
//
//  与 reboot 的区别：
//    reboot   = 复位（写 0x64 端口 0xFE），机器会重新起来
//    shutdown = ACPI 断电（写 PM1a_CNT），机器真正关机
// ---------------------------------------------------------------------------
void do_shutdown()
{
    ulib::puts("  正在关机...\n");

    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
    m->type = MSG_SHUTDOWN;
    m->len = 0;
    m->a = 0; m->b = 0; m->c = 0; m->d = 0;
    m->ptr = 0; m->size = 0;

    ipc_call(TID_POWER, m);
    // 正常不会走到这里
    ulib::puts("  关机失败\n");
}

// ---------------------------------------------------------------------------
//  do_ps：向进程服务要线程列表并显示
//  -------------------------------------------------------------------------
//  数据在共享页的 PS 区域（USER_IPC_PAGE + 1024），
//  每条 32 字节：+0 tid  +4 state  +8 ticks  +16 name[16]
// ---------------------------------------------------------------------------
// --- 把无符号数格式化成定宽左对齐字符串（手写，不依赖 printf 宽度支持）---
// 为什么不直接用 ulib::printf("%-4u")？
//   ulib::printf 只认 %s %d %u %x %c %%，**不支持宽度和对齐**。
//   写 "%-4u" 时 '-' 走 default 输出字面量，'4' 当普通字符，
//   只有 'u' 被识别 —— 结果格式串被原样打印出来。
//   更糟的是 %u 按 **u64** 取参数，传 u32 进去会读错，
//   实测直接 #PF（CR2=0x1）。所以这里手写，可控且安全。
static void fmt_pad(char* dst, u64 v, int width)
{
    char tmp[24];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < 23) {
            tmp[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
    }
    // 反转成正常顺序
    int k = 0;
    while (n > 0) dst[k++] = tmp[--n];
    // 右侧补空格到指定宽度
    while (k < width) dst[k++] = ' ';
    dst[k] = '\0';
}

void do_ps()
{
    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
    m->type = MSG_PS;
    m->len = 0;
    m->a = 0; m->b = 0; m->c = 0; m->d = 0;
    m->ptr = 0; m->size = 0;

    if (ipc_call(TID_PROC, m) != 0) {
        ulib::puts("  无法获取线程列表\n");
        return;
    }

    int n = static_cast<int>(m->a);
    if (n <= 0) {
        ulib::puts("  没有线程\n");
        return;
    }

    const u8* buf = reinterpret_cast<const u8*>(USER_IPC_PAGE + 1024);

    ulib::puts(" TID  状态    时间片    名称\n");
    ulib::puts("----  ------  --------  ----------------\n");

    char col[24];
    for (int i = 0; i < n && i < 32; ++i) {
        const u8* e = buf + i * 32;

        u32 tid   = *reinterpret_cast<const u32*>(e + 0);
        u32 state = *reinterpret_cast<const u32*>(e + 4);
        u64 ticks = *reinterpret_cast<const u64*>(e + 8);
        const char* name = reinterpret_cast<const char*>(e + 16);

        const char* st = "?";
        if (state == 1)      st = "运行";
        else if (state == 2) st = "就绪";
        else if (state == 3) st = "阻塞";

        ulib::puts(" ");
        fmt_pad(col, tid, 4);
        ulib::puts(col);
        ulib::puts(" ");
        ulib::puts(st);
        // 中文状态是 2 个汉字（4 字节 UTF-8，占 2 个字符格），补 2 空格对齐
        ulib::puts("  ");
        fmt_pad(col, ticks, 8);
        ulib::puts(col);
        ulib::puts("  ");
        ulib::puts(name);
        ulib::puts("\n");
    }
}

// 读一个按键（通过键盘服务）
char get_key()
{
    IpcMsg* m = reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND);
    m->type = MSG_KEY_GET;
    m->len = 0;
    m->a = 0; m->b = 0; m->c = 0; m->d = 0;
    m->ptr = 0; m->size = 0;

    ipc_call(TID_KEYBOARD, m);
    return static_cast<char>(m->a & 0xFF);
}

// 读一行
void read_line(char* buf, int max)
{
    int len = 0;
    for (;;) {
        char c = get_key();
        if (c == 0) continue;

        if (c == '\n' || c == '\r') {
            ulib::puts("\n");
            break;
        }
        if (c == '\b') {
            if (len > 0) {
                --len;
                ulib::puts("\b \b");
            }
            continue;
        }
        if (len < max - 1) {
            buf[len++] = c;
            char s[2] = { c, 0 };
            ulib::puts(s);
        }
    }
    buf[len] = '\0';
}

int strcmp_simple(const char* a, const char* b)
{
    while (*a && *b) {
        if (*a != *b) return 1;
        ++a; ++b;
    }
    return (*a == *b) ? 0 : 1;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Shell —— 「万物皆可程序」下的极薄外壳
//  -------------------------------------------------------------------------
//  它**不再内置任何命令**，只做三件事：
//    1. 读一行输入
//    2. 切分成 argv
//    3. 启动对应的 .xzs 程序，等它跑完
//
//  命令全部是独立的 .xzs 程序（放在 initrd 里），
//  由内核的 ELF 加载器装载运行。
//
//  这就是「万物皆可程序」的核心：
//    系统功能的增加 = 往里放一个新程序，而不是改 shell 或内核。
// ---------------------------------------------------------------------------
static void split_args(char* line, char** argv, int* out_argc, int max_args)
{
    int argc = 0;
    char* p = line;

    while (*p != '\0' && argc < max_args) {
        while (*p == ' ') ++p;              // 跳过前导空格
        if (*p == '\0') break;

        argv[argc++] = p;                   // 本参数起点

        while (*p != '\0' && *p != ' ') ++p;
        if (*p == ' ') {
            *p = '\0';                      // 就地截断成独立字符串
            ++p;
        }
    }
    argv[argc] = nullptr;
    *out_argc = argc;
}

extern "C" void shell_service_entry()
{
    ulib::puts("\n");
    ulib::puts("  xingxingOS 微内核 Shell（用户态）\n");
    ulib::puts("  【万物皆可程序】每条命令都是一个独立的 .xzs 程序\n");
    ulib::puts("  输入 help 查看命令\n\n");

    char  line[128];
    char* argv[16];

    for (;;) {
        ulib::puts("xingxingos> ");
        read_line(line, 128);

        if (line[0] == '\0') continue;

        int argc = 0;
        split_args(line, argv, &argc, 15);
        if (argc == 0) continue;

        // --- 启动程序 ---
        //
        // argv[] 指向 line 里被截断的各个片段，全在 shell 自己的栈上。
        // 内核在 syscall 期间仍用 shell 的页表，能正确翻译这些地址。
        u64 tid = syscall4(static_cast<u64>(Sys::SPAWN),
                           reinterpret_cast<u64>(argv[0]),
                           static_cast<u64>(ulib::strlen(argv[0])),
                           reinterpret_cast<u64>(argv),
                           static_cast<u64>(argc));

        if (tid == static_cast<u64>(-1) || (static_cast<i64>(tid)) < 0) {
            ulib::puts("  未知命令：");
            ulib::puts(argv[0]);
            ulib::puts("\n");
            continue;
        }

        // --- 等它跑完再显示提示符 ---
        // 不然多个程序的输出会交错在一起
        syscall1(static_cast<u64>(Sys::WAIT), tid);
    }
}
