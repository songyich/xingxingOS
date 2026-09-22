// ===========================================================================
//  user/xingcrt.cpp —— 用户程序运行时（C runtime + 系统接口实现）
//  ==========================================================================
//  每个 .xzs 程序都链接这一份。它提供：
//    1. _start 入口（从栈上取 argc/argv，调用 main，然后退出）
//    2. xingxing.h 里声明的各个 Xing* 函数的实现
//
//  ⚠️ 这里跑在 **Ring 3 用户态**，没有标准库，一切自己实现。
// ===========================================================================
#include <xingxing.h>

// ---------------------------------------------------------------------------
//  _start：程序真正的入口
//  -------------------------------------------------------------------------
//  内核加载完程序、建好栈后跳到这里。栈上按 System V ABI 摆着：
//      rsp -> argc
//             argv[0]
//             argv[1]
//             ...
//             argv[argc] = NULL
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  _start：程序真正的入口
//  -------------------------------------------------------------------------
//  【传参方式：寄存器，不是栈】
//
//    内核 create_elf_process 会把参数按 System V ABI 放进寄存器后跳转过来：
//        rdi = argc
//        rsi = argv（指向新程序栈上的 argv[] 数组）
//
//    为什么不从栈上读？那条路踩了两个坑：
//      1) 普通函数有 prologue（push rbp），
//         执行到函数体时 rsp 已经**不再指向 argc**（实测读到 argc=0）
//      2) 改用 naked + 汇编 `jmp _xstart` 会产生**重定位**，
//         而内核 ELF 加载器不做重定位 → 跳到错误地址崩溃（CR2=0x2）
//
//    用寄存器传参最稳：写成普通函数，编译器自动生成正确的读取代码，
//    既不需要 naked，也不产生重定位。
//
//    栈上依然摆好了 argc/argv（保持标准布局，方便调试与将来兼容）。
// ---------------------------------------------------------------------------
extern "C" void _start(int argc, char** argv)
{
    int ret = main(argc, argv);
    XingExit(ret);

    // 理论上回不来
    for (;;) { asm volatile("hlt"); }
}

// ---------------------------------------------------------------------------
//  服务 tid 缓存
//  -------------------------------------------------------------------------
//  每次输出都去查服务名太慢（要进内核），所以查一次就记住。
//  -2 表示"还没查过"，-1 表示"查了但没有这个服务"。
// ---------------------------------------------------------------------------
#define NOT_QUERIED (-2)

static int g_tid_terminal = NOT_QUERIED;
static int g_tid_timer    = NOT_QUERIED;
static int g_tid_power    = NOT_QUERIED;
static int g_tid_proc     = NOT_QUERIED;

static int service_tid(const char* name, int* cache)
{
    if (*cache == NOT_QUERIED) {
        *cache = static_cast<int>(
            XingSyscall(SYS_SVC_LOOKUP, (U64)name, XingStrLen(name), 0, 0, 0, 0));
    }
    return *cache;
}

int XingServiceLookup(const char* name)
{
    return static_cast<int>(
        XingSyscall(SYS_SVC_LOOKUP, (U64)name, XingStrLen(name), 0, 0, 0, 0));
}

int XingServiceRegister(const char* name)
{
    return static_cast<int>(
        XingSyscall(SYS_SVC_REGISTER, (U64)name, XingStrLen(name), 0, 0, 0, 0));
}

// ---------------------------------------------------------------------------
//  IPC 原语
//  -------------------------------------------------------------------------
//  IPC_WOULD_BLOCK(-4) 表示目标正忙，重试即可（不是错误）。
// ---------------------------------------------------------------------------
#define IPC_WOULD_BLOCK (-4)

int XingIpcCall(int tid, XingMsg* m)
{
    for (;;) {
        I64 r = static_cast<I64>(
            XingSyscall(SYS_IPC_CALL, (U64)tid, (U64)m, 0, 0, 0, 0));
        if (r != IPC_WOULD_BLOCK) return static_cast<int>(r);
    }
}

int XingIpcSend(int tid, XingMsg* m)
{
    return static_cast<int>(
        XingSyscall(SYS_IPC_SEND, (U64)tid, (U64)m, 0, 0, 0, 0));
}

int XingIpcRecv(int* from, XingMsg* m)
{
    return static_cast<int>(
        XingSyscall(SYS_IPC_RECV, (U64)from, (U64)m, 0, 0, 0, 0));
}

int XingIpcReply(int tid, XingMsg* m)
{
    return static_cast<int>(
        XingSyscall(SYS_IPC_REPLY, (U64)tid, (U64)m, 0, 0, 0, 0));
}

// ---------------------------------------------------------------------------
//  输出
// ---------------------------------------------------------------------------
int XingPrint(const char* s)
{
    if (s == XING_NULL) return -1;
    SIZE len = XingStrLen(s);
    if (len == 0) return 0;

    int tid = service_tid(SVC_TERMINAL, &g_tid_terminal);
    if (tid < 0) {
        // 终端服务不可用：回退到内核调试通道，至少能看到东西
        XingDebug(s);
        return -1;
    }

    // 分块发送：消息的内联区只有 64 字节，长字符串要拆开
    SIZE off = 0;
    while (off < len) {
        XingMsg* m = reinterpret_cast<XingMsg*>(XING_SLOT_SEND);
        m->type = MSG_PUTS;
        m->ptr  = 0;
        m->size = 0;
        m->a = 0; m->b = 0; m->c = 0; m->d = 0;

        SIZE n = len - off;
        if (n > XING_MSG_TEXT - 1) n = XING_MSG_TEXT - 1;

        // -------------------------------------------------------------
        //  【健壮性修复】不要从 UTF-8 多字节序列**中间**切开
        //  -----------------------------------------------------------
        //  一个汉字是 3 字节（E4 B8 AD）。按固定 63 字节切块，
        //  完全可能切在中间：块1 末尾是 E4 B8，块2 开头是 AD。
        //
        //  内核侧 term::putc 靠全局状态机跨块重组，
        //  只要两次 Sys::PUTS 之间没人插入别的输出就还能拼回来；
        //  但一旦中间插了 ASCII（比如内核日志），序列会被判非法 →
        //  输出 '?'。这是个隐患。
        //
        //  现在：块尾若是续字节（10xxxxxx）就回退，
        //  保证切在字符起点，从根上避免跨块。
        // -------------------------------------------------------------
        if (off + n < len) {
            while (n > 0 &&
                   (static_cast<unsigned char>(s[off + n]) & 0xC0) == 0x80) {
                --n;
            }
        }
        for (SIZE i = 0; i < n; ++i) m->text[i] = s[off + i];
        m->text[n] = '\0';
        m->len = static_cast<U32>(n);

        int r = XingIpcCall(tid, m);
        if (r != 0) {
            XingDebug(s + off);     // 失败也要把内容吐出来
            return -1;
        }
        off += n;
    }
    return 0;
}

void XingPrintLn(const char* s)
{
    if (s != XING_NULL) XingPrint(s);
    XingPrint("\n");
}

void XingPutC(char c)
{
    int tid = service_tid(SVC_TERMINAL, &g_tid_terminal);
    XingMsg* m = reinterpret_cast<XingMsg*>(XING_SLOT_SEND);
    m->type = MSG_PUTC;
    m->len  = 1;
    m->a    = static_cast<U64>(static_cast<U8>(c));
    m->ptr = 0; m->size = 0; m->b = 0; m->c = 0; m->d = 0;

    if (tid < 0) {
        char tmp[2] = { c, '\0' };
        XingDebug(tmp);
        return;
    }
    XingIpcCall(tid, m);
}

int XingPrintU64(U64 v)
{
    char buf[24];
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        while (v > 0 && n < 23) {
            buf[n++] = static_cast<char>('0' + static_cast<U32>(v % 10));
            v /= 10;
        }
    }
    // 反转
    char out[25];
    for (int i = 0; i < n; ++i) out[i] = buf[n - 1 - i];
    out[n] = '\0';
    return XingPrint(out);
}

int XingPrintI64(I64 v)
{
    if (v < 0) {
        XingPrint("-");
        return XingPrintU64(static_cast<U64>(0 - static_cast<U64>(-(v + 1)) - 1));
    }
    return XingPrintU64(static_cast<U64>(v));
}

void XingClear(void)
{
    int tid = service_tid(SVC_TERMINAL, &g_tid_terminal);
    if (tid < 0) return;

    XingMsg* m = reinterpret_cast<XingMsg*>(XING_SLOT_SEND);
    m->type = MSG_CLEAR;
    m->len = 0;
    m->a = 0; m->b = 0; m->c = 0; m->d = 0;
    m->ptr = 0; m->size = 0;
    XingIpcCall(tid, m);
}

// ---------------------------------------------------------------------------
//  进程控制
// ---------------------------------------------------------------------------
void XingExit(int code)
{
    XingSyscall(SYS_EXIT, (U64)code, 0, 0, 0, 0, 0);
    for (;;) { asm volatile("hlt"); }
}

void XingYield(void)
{
    XingSyscall(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}

U64 XingUptime(void)
{
    // 优先走定时器服务（微内核正路）
    int tid = service_tid(SVC_TIMER, &g_tid_timer);
    if (tid >= 0) {
        XingMsg* m = reinterpret_cast<XingMsg*>(XING_SLOT_SEND);
        m->type = MSG_UPTIME;
        m->len = 0;
        m->a = 0; m->b = 0; m->c = 0; m->d = 0;
        m->ptr = 0; m->size = 0;
        if (XingIpcCall(tid, m) == 0) return m->a;
    }
    // 回退：直接问内核
    return XingSyscall(SYS_UPTIME, 0, 0, 0, 0, 0, 0);
}

void XingSleepMs(U64 ms)
{
    XingSyscall(SYS_SLEEP, ms, 0, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
//  启动程序
// ---------------------------------------------------------------------------
int XingSpawn(const char* name)
{
    return static_cast<int>(
        XingSyscall(SYS_SPAWN, (U64)name, XingStrLen(name), 0, 0, 0, 0));
}

// ---------------------------------------------------------------------------
//  电源
// ---------------------------------------------------------------------------
static void power_op(U32 which)
{
    int tid = service_tid(SVC_POWER, &g_tid_power);
    if (tid < 0) return;

    XingMsg* m = reinterpret_cast<XingMsg*>(XING_SLOT_SEND);
    m->type = which;
    m->len = 0;
    m->a = 0; m->b = 0; m->c = 0; m->d = 0;
    m->ptr = 0; m->size = 0;
    XingIpcCall(tid, m);
}

void XingReboot(void)   { power_op(MSG_REBOOT); }
void XingShutdown(void) { power_op(MSG_SHUTDOWN); }

// ---------------------------------------------------------------------------
//  日志
// ---------------------------------------------------------------------------
void XingLogEnable(BOOL on)
{
    XingSyscall(SYS_LOG_CONTROL, on ? 1 : 0, 0, 0, 0, 0, 0);
}

void XingLogFlush(void)
{
    XingSyscall(SYS_LOG_CONTROL, 2, 0, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
//  取终端服务 / 进程服务 tid（供 ps 这类程序直接用）
// ---------------------------------------------------------------------------
int XingTerminalTid(void) { return service_tid(SVC_TERMINAL, &g_tid_terminal); }
int XingProcTid(void)     { return service_tid(SVC_PROC,     &g_tid_proc); }
