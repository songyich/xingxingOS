// ===========================================================================
//  xingxing.h —— xingxingOS 对外程序接口（用户态）
//  ==========================================================================
//  这是「万物皆可程序」的契约：
//    系统里的一切能力都以**程序**的形态存在，
//    而所有程序都通过这一个头文件与系统打交道。
//
//  风格对标 windows.h：
//    - 类型全大写（U64 / I32 / BOOL ...）
//    - 函数带 Xing 前缀（XingPrint / XingSpawn ...）
//
//  用法（一个最小的 .xzs 程序）：
//      #include <xingxing.h>
//      int main(int argc, char** argv) {
//          XingPrint("你好，星星\\n");
//          return 0;
//      }
//
//  ⚠️ 本头文件必须能被 **-ffreestanding** 编译
//     （没有标准库，一切自己提供）
// ===========================================================================
#pragma once

// ---------------------------------------------------------------------------
//  基础类型（全大写，对标 windows.h 风格）
// ---------------------------------------------------------------------------
typedef unsigned char      U8;
typedef unsigned short     U16;
typedef unsigned int       U32;
typedef unsigned long long U64;
typedef signed char        I8;
typedef signed short       I16;
typedef signed int         I32;
typedef signed long long   I64;
typedef unsigned long long SIZE;
typedef int                BOOL;

#define XING_TRUE   1
#define XING_FALSE  0
#define XING_NULL   ((void*)0)

// ---------------------------------------------------------------------------
//  系统调用号（必须与内核 include/kernel/syscall.hpp 的枚举一致）
// ---------------------------------------------------------------------------
#define SYS_IPC_CALL     1
#define SYS_IPC_REPLY    2
#define SYS_IPC_SEND     3
#define SYS_IPC_RECV     4
#define SYS_YIELD        30
#define SYS_SLEEP        31
#define SYS_UPTIME       32
#define SYS_EXIT         33
#define SYS_PUTS         34
#define SYS_SHUTDOWN     35
#define SYS_REBOOT       36
#define SYS_THREAD_INFO  37
#define SYS_LOG_CONTROL  38
#define SYS_SPAWN        40     // 启动一个 .xzs 程序
#define SYS_SVC_REGISTER 41     // 注册服务：把当前线程登记为某个名字
#define SYS_SVC_LOOKUP   42     // 查找服务：按名字取 tid
#define SYS_SVC_LIST     43     // 列出所有服务（结果写入共享页）
#define SYS_KILL         47     // 终止进程（P2 崩溃自愈验收用）

// ---------------------------------------------------------------------------
//  IPC 消息结构
//  -------------------------------------------------------------------------
//  ⚠️ 布局必须与内核 struct IpcMsg **逐字节一致**。
//     内核是按固定偏移搬运整条消息的，对不上就会读到错位的数据。
//     （我们踩过这个坑：手写 copy_msg 漏同步字段，导致内容为空）
// ---------------------------------------------------------------------------
#define XING_MSG_TEXT 64

typedef struct XingMsg {
    U32 type;                   // 消息类型
    U32 len;                    // 有效数据长度
    U64 a, b, c, d;             // 四个通用字段
    U64 ptr;                    // 大数据地址（需双方已共享内存）
    U64 size;                   // 大数据长度
    char text[XING_MSG_TEXT];   // 内联小数据
} XingMsg;

// IPC 共享页与槽位
//
// 每个进程都把同一物理页映射到同一虚拟地址，
// 消息收发直接在这上面进行 —— 不碰用户栈，零拷贝。
// （用户栈地址在不同进程里是同一个虚拟值，
//   内核跨地址空间写会写到自己身上，这个坑我们踩过）
#define XING_IPC_PAGE   0x7ffe0000ull
#define XING_SLOT_RECV  (XING_IPC_PAGE + 0)
#define XING_SLOT_SEND  (XING_IPC_PAGE + 256)

// ---------------------------------------------------------------------------
//  常用服务名（用于 XingService 查找）
// ---------------------------------------------------------------------------
#define SVC_TERMINAL  "terminal"
#define SVC_KEYBOARD  "keyboard"
#define SVC_TIMER     "timer"
#define SVC_POWER     "power"
#define SVC_PROC      "proc"

// 终端服务消息
#define MSG_PUTS      100
#define MSG_PUTC      101
#define MSG_CLEAR     102
#define MSG_SET_COLOR 103
#define MSG_TERM_INFO 104

// 键盘服务消息
#define MSG_KEY_GET   200
#define MSG_KEY_PEEK  201

// 定时器服务消息
#define MSG_UPTIME    300
#define MSG_TICKS     301
#define MSG_SLEEP     302

// 电源服务消息
#define MSG_REBOOT    400
#define MSG_SHUTDOWN  401
#define MSG_HALT      402

// 进程服务消息
#define MSG_PS        500

// ---------------------------------------------------------------------------
//  系统调用封装
//  -------------------------------------------------------------------------
//  x86_64 syscall 指令约定：
//    调用号放 rax；参数用 rdi, rsi, rdx, **r10**, r8, r9
//    （第 4 个是 r10 不是 rcx —— rcx 被 syscall 用来存返回地址）
//    返回值在 rax
// ---------------------------------------------------------------------------
static inline U64 XingSyscall(U64 num, U64 a1, U64 a2, U64 a3,
                              U64 a4, U64 a5, U64 a6)
{
    U64 ret;
    register U64 r10 asm("r10") = a4;
    register U64 r8  asm("r8")  = a5;
    register U64 r9  asm("r9")  = a6;

    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}

// ---------------------------------------------------------------------------
//  基础输出
// ---------------------------------------------------------------------------

// 计算字符串长度（freestanding，没有 libc）
static inline SIZE XingStrLen(const char* s)
{
    SIZE n = 0;
    if (s == XING_NULL) return 0;
    while (s[n] != '\0') ++n;
    return n;
}

// 直接往内核调试通道输出（串口）。
// 任何时候都可用，即使终端服务还没起来 —— 适合排查启动期问题。
static inline void XingDebug(const char* s)
{
    XingSyscall(SYS_PUTS, (U64)s, XingStrLen(s), 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
//  终端输出
//  -------------------------------------------------------------------------
//  正常路径：发 IPC 给终端服务（这才是微内核该走的路）。
//  失败时自动回退到内核调试通道，保证输出不至于完全丢失。
// ---------------------------------------------------------------------------
int  XingPrint(const char* s);          // 输出字符串
void XingPrintLn(const char* s);        // 输出字符串 + 换行
void XingPutC(char c);                  // 输出单个字符
int  XingPrintU64(U64 v);               // 输出无符号整数
int  XingPrintI64(I64 v);               // 输出有符号整数
void XingClear(void);                   // 清屏

// ---------------------------------------------------------------------------
//  进程控制
// ---------------------------------------------------------------------------
void XingExit(int code);                // 退出本程序
void XingYield(void);                   // 让出 CPU
U64  XingUptime(void);                  // 系统运行毫秒数
void XingSleepMs(U64 ms);               // 睡眠

// ---------------------------------------------------------------------------
//  服务发现（万物皆可程序的"目录"）
//  -------------------------------------------------------------------------
//  对应 /system/services/<name>。
//  目前由内核维护一张名字→tid 的表；
//  将来有了文件系统，底层换成真实目录，接口不变。
// ---------------------------------------------------------------------------
int  XingServiceRegister(const char* name);   // 把本线程登记为某服务
int  XingServiceLookup(const char* name);     // 按名字查 tid（-1 = 没找到）

// ---------------------------------------------------------------------------
//  程序启动（万物皆可程序的核心）
//  -------------------------------------------------------------------------
//  启动 initrd 里名为 name 的 .xzs 程序。
//  返回新程序的 tid，失败返回负数。
// ---------------------------------------------------------------------------
int  XingSpawn(const char* name);

// ---------------------------------------------------------------------------
//  IPC（程序之间通信）
// ---------------------------------------------------------------------------
int  XingIpcCall(int tid, XingMsg* m);        // 发消息并等回复
int  XingIpcRecv(int* from, XingMsg* m);      // 接收一条消息
int  XingIpcReply(int tid, XingMsg* m);       // 回复给调用者
int  XingIpcSend(int tid, XingMsg* m);        // 异步发送，不等回复

// ---------------------------------------------------------------------------
//  电源
// ---------------------------------------------------------------------------
void XingReboot(void);
void XingShutdown(void);

// ---------------------------------------------------------------------------
//  日志控制（对应 shell 的 log on/off/flush）
// ---------------------------------------------------------------------------
void XingLogEnable(BOOL on);
void XingLogFlush(void);

// ---------------------------------------------------------------------------
//  程序入口约定
//  -------------------------------------------------------------------------
//  内核加载完程序后，会按 System V ABI 在栈上摆好 argc / argv，
//  然后跳到 _start（由 crt0 提供），crt0 再调用这里的 main。
// ---------------------------------------------------------------------------
int main(int argc, char** argv);
