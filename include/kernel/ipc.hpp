// ============================================================================
//  include/kernel/ipc.hpp —— 进程间通信（微内核的心脏）
//  ---------------------------------------------------------------------------
//  为什么 IPC 是微内核的心脏？
//    微内核把驱动、文件系统都挪到用户态了，它们之间不能再直接函数调用，
//    唯一的沟通方式就是**发消息**。IPC 的性能直接决定了整个系统的性能，
//    所以用户要求「最小性能开销」，优化重点全在这里。
//
//  性能优化（三条，都是 L4 家族的经典手法）：
//    1. **短消息走结构体**：消息固定 56 字节，写完就在缓存里，
//       传一个指针比在寄存器里拼来拼去更省事，也够快。
//    2. **大数据走共享内存**：超过 56 字节的数据（比如终端输出大段文字）
//       不塞进消息，而是把内存**映射给对方**，消息里只带地址和长度。
//    3. **call/reply 合并切换**（最关键）：
//       普通的 send + recv 需要两次上下文切换（A→B，B→A）。
//       ipc_call 直接把调用者挂起、**立刻切到目标线程**，
//       目标 reply 时又直接切回来——省掉了经过调度器重选的开销。
// ============================================================================
#pragma once

#include <kernel/types.h>

// ---------------------------------------------------------------------------
//  消息结构（56 字节，固定大小）
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  IPC 共享页：所有用户进程共用的消息传递区
//  -------------------------------------------------------------------------
//  为什么需要它？
//    原来的做法是内核直接写用户态的栈变量（&m），但实测发现
//    **用户态栈指针在 syscall 返回后会错位 8 字节** ——
//    传参时算出的 &m 与返回后读的 &m 不是同一个地址，
//    于是内核写对了地方，用户态却读到 0。
//    （内核侧读回验证写入成功，用户态仍读到 0，就是这个原因）
//
//  改为固定地址的共享页后：
//    - 所有用户进程把**同一个物理页**映射到同一个虚拟地址
//    - 消息一律放在这里，不依赖任何栈变量地址
//    - 栈错位完全不影响，而且是零拷贝
// ---------------------------------------------------------------------------
// ⚠️ 地址必须避开用户栈！
//   用户栈范围是 [USER_STACK_TOP - 64KB, USER_STACK_TOP) = [0x7fff0000, 0x80000000)
//   共享页最初放在 0x7fff0000，正好与栈**重叠**，
//   映射把栈内容冲掉，导致写 0x80000000 时 #PF（错误码 0x2，内核态写）。
//   改到 0x7ffe0000（栈下方一页处），与代码区(0x400000)和栈都不冲突。
constexpr u64 USER_IPC_PAGE = 0x7ffe0000ull;

// 共享页内的槽位布局（每个 IpcMsg 120 字节，留足余量按 256 对齐）
//   偏移 0   : 接收槽 —— 内核写入，用户态读取
//   偏移 256 : 发送槽 —— 用户态写入，内核读取
constexpr u64 IPC_SLOT_RECV = USER_IPC_PAGE + 0;
constexpr u64 IPC_SLOT_SEND = USER_IPC_PAGE + 256;

// 键盘状态字节（偏移 512）
//
// 为什么放在共享页而不是服务的 static 变量？
//   用户态服务**不能**用 static/全局变量 —— 它们会链接到内核 .bss
//   （高半区），PC32 相对寻址必然溢出（relocation truncated to fit）。
//   共享页是固定地址，读写都安全。
//
// 值含义：0 = 没有键被按住；非 0 = 当前按住的扫描码。
constexpr u64 IPC_KBD_STATE = USER_IPC_PAGE + 512;
constexpr u64 IPC_KBD_EXT   = USER_IPC_PAGE + 516;   // E0 扩展前缀标志
constexpr u64 IPC_KBD_HEAD  = USER_IPC_PAGE + 520;   // 环形队列头
constexpr u64 IPC_KBD_TAIL  = USER_IPC_PAGE + 524;   // 环形队列尾
constexpr u64 IPC_KBD_BUF   = USER_IPC_PAGE + 528;   // 环形队列缓冲
constexpr u64 IPC_KBD_PEND  = USER_IPC_PAGE + 660;   // 挂起中的请求方 tid
// --- 鼠标状态（鼠标服务写，其他进程读）---
constexpr u64 IPC_MOUSE_X   = USER_IPC_PAGE + 668;   // 当前 X（有符号，存 u64）
constexpr u64 IPC_MOUSE_Y   = USER_IPC_PAGE + 676;   // 当前 Y
constexpr u64 IPC_MOUSE_BTN = USER_IPC_PAGE + 684;   // 按键位：bit0左 bit1右 bit2中
constexpr u32 KBQ_CAP       = 128;                   // 队列容量

struct IpcMsg {
    u32 type;           // 消息类型（各服务自己定义协议）
    u32 len;            // 有效数据长度
    u64 a, b, c, d;     // 四个通用字段，够绝大多数请求用
    u64 ptr;            // 大数据：共享内存地址（可选）
    u64 size;           // 大数据长度

    // -----------------------------------------------------------------------
    //  内联小数据区
    //  ---------------------------------------------------------------------
    //  为什么需要它？
    //    每个用户进程有**独立的地址空间**，
    //    A 的指针在 B 里指向完全不同的物理内存（甚至无效）。
    //    所以 IPC 不能只传指针 —— 接收方解引用会直接 #PF。
    //
    //    ptr/size 只在**双方已建立共享内存**时才可用。
    //    常规的小消息一律走内联区，内核原样搬运整条 IpcMsg 即可，
    //    既避开了地址空间问题，也省掉一次数据拷贝（零拷贝）。
    //
    //    64 字节足够覆盖绝大多数请求（路径、短文本、请求参数）。
    // -----------------------------------------------------------------------
    char text[64];
};

constexpr int MSG_SIZE = sizeof(IpcMsg);   // 56

// ---------------------------------------------------------------------------
//  固定服务号（简化设计：不做名字服务，直接用约定好的编号）
// ---------------------------------------------------------------------------
// ⚠️ 这些数字必须和**实际创建的顺序**一致。
//
// thread::init() 会先建两个内核线程：
//   0 = main（当前执行流，即 kmain 自己）
//   1 = idle（空闲线程）
// 所以 kmain 里创建的第一个用户进程 TID 从 2 开始。
//
// 【踩过的坑】之前这里写的是 1/2/3/4，
// 于是 shell 发的消息全送到了 **idle 线程**（TID=1），
// 而 idle 永远在 hlt，从不收消息 ——
// 表现是 shell 卡死、屏幕上连提示符都不出，
// 但系统"看起来很正常"（没崩、没报错），极难定位。
constexpr int TID_TERMINAL = 2;     // 终端服务
constexpr int TID_KEYBOARD = 3;     // 键盘服务
constexpr int TID_TIMER    = 4;     // 定时器服务
constexpr int TID_SHELL    = 5;     // shell（也是用户态！）
constexpr int TID_POWER    = 6;     // 电源管理服务
constexpr int TID_PROC     = 7;     // 进程信息服务
constexpr int TID_MOUSE    = 8;     // 鼠标服务（用户态 PS/2 驱动）
constexpr int TID_ANY      = -1;    // 接收任意来源

// ---------------------------------------------------------------------------
//  消息协议：终端服务
// ---------------------------------------------------------------------------
constexpr u32 MSG_PUTS      = 100;  // a=字符串地址（共享内存） b=长度
constexpr u32 MSG_PUTC      = 101;  // a=字符
constexpr u32 MSG_CLEAR     = 102;  // 清屏
constexpr u32 MSG_SET_COLOR = 103;  // a=前景色
constexpr u32 MSG_TERM_INFO = 104;  // 返回 b=列数 c=行数
constexpr u32 MSG_SCROLL    = 105;  // 滚屏

// ---------------------------------------------------------------------------
//  消息协议：键盘服务
// ---------------------------------------------------------------------------
constexpr u32 MSG_KEY_GET   = 200;  // 取一个按键，返回 a=按键
constexpr u32 MSG_KEY_PEEK  = 201;  // 看一眼，返回 a=是否有键

// ---------------------------------------------------------------------------
//  消息协议：定时器服务
// ---------------------------------------------------------------------------
constexpr u32 MSG_UPTIME    = 300;  // 返回 a=毫秒数
constexpr u32 MSG_TICKS     = 301;  // 返回 a=滴答数
constexpr u32 MSG_SLEEP     = 302;  // a=毫秒

// --- 电源管理服务 ---
constexpr u32 MSG_REBOOT    = 400;  // 重启
constexpr u32 MSG_SHUTDOWN  = 401;
constexpr u32 MSG_MOUSE_GET = 500;  // 取鼠标状态：a=X b=Y c=按键  // ACPI 关机（断电）
constexpr u32 MSG_HALT      = 402;  // 停机（不断电）

// --- 进程信息服务 ---
constexpr u32 MSG_PS        = 500;  // 返回线程列表（结果放共享页）

// ---------------------------------------------------------------------------
//  返回码
// ---------------------------------------------------------------------------
constexpr int IPC_OK        = 0;
constexpr int IPC_ERR_INVAL = -1;   // 参数无效
constexpr int IPC_ERR_DEST  = -2;   // 目标不存在
constexpr int IPC_ERR_PERM  = -3;   // 没权限
constexpr int IPC_WOULD_BLOCK = -4; // 本线程已阻塞，调用方应重试

// ---------------------------------------------------------------------------
//  为什么要用「重试」而不是「内核内部切换」？
//  -------------------------------------------------------------------------
//  原实现在 do_recv/do_call 里调用 thread::block()，
//  它内部用 int $0x81 **嵌套在 syscall 处理中**触发切换。
//  这条路极其脆弱：嵌套中断的栈布局、CR3 切换时机、
//  恢复后继续执行 do_recv 剩余代码 —— 任何一环出错，
//  用户态就再也回不去（实测：终端服务第一次 ipc_recv 后彻底失联）。
//
//  改为：
//    内核只负责「标记阻塞 + 立即返回 IPC_WOULD_BLOCK」，
//    切换交给 syscall 出口的统一调度点（那时现场干净）。
//    用户态看到 WOULD_BLOCK 就重试，
//    而这期间线程是 BLOCKED 的，不会忙等 —— 调度器根本不会调度它。
// ---------------------------------------------------------------------------

// ===========================================================================
//  用户态调用接口（inline，直接走 syscall）
//  ===========================================================================

#ifndef KERNEL_BUILD
#include <kernel/syscall.hpp>
#endif

// --- 发送 + 等回复（带重试，用户态主接口）---
inline int ipc_call(int dest_tid, IpcMsg* /*msg 已废弃*/)
{
#ifdef KERNEL_BUILD
    extern int ipc_call_kernel(int, IpcMsg*);
    return ipc_call_kernel(dest_tid, reinterpret_cast<IpcMsg*>(IPC_SLOT_SEND));
#else
    // ⚠️ 不要先存进局部变量再传！
    //   实测：slot 存在用户栈上会被**破坏**（变成 0x7fffffc0 这类栈地址），
    //   导致内核把消息写到了错误的地址。
    //   直接用常量，编译器会生成立即数，不经过栈。
    for (;;) {
        int r = static_cast<int>(
            syscall2(static_cast<u64>(Sys::IPC_CALL),
                     static_cast<u64>(dest_tid),
                     IPC_SLOT_SEND));
        if (r != IPC_WOULD_BLOCK) return r;
        syscall1(static_cast<u64>(Sys::YIELD), 0);
    }
#endif
}

inline int ipc_reply(int dest_tid, IpcMsg* msg)
{
#ifdef KERNEL_BUILD
    extern int ipc_reply_kernel(int, IpcMsg*);
    return ipc_reply_kernel(dest_tid, msg);
#else
    return static_cast<int>(
        syscall2(static_cast<u64>(Sys::IPC_REPLY),
                 static_cast<u64>(dest_tid),
                 reinterpret_cast<u64>(msg)));
#endif
}

// --- 异步发送（不等回复）---
inline int ipc_send(int dest_tid, IpcMsg* msg)
{
#ifdef KERNEL_BUILD
    extern int ipc_send_kernel(int, IpcMsg*);
    return ipc_send_kernel(dest_tid, msg);
#else
    return static_cast<int>(
        syscall2(static_cast<u64>(Sys::IPC_SEND),
                 static_cast<u64>(dest_tid),
                 reinterpret_cast<u64>(msg)));
#endif
}

// --- 接收一条消息（带重试，用户态主接口）---
inline int ipc_recv(int* from_tid, IpcMsg* /*msg 已废弃：改用共享页*/)
{
#ifdef KERNEL_BUILD
    extern int ipc_recv_kernel(int*, IpcMsg*);
    return ipc_recv_kernel(from_tid, reinterpret_cast<IpcMsg*>(IPC_SLOT_RECV));
#else
    // 消息一律走共享页接收槽，**不再使用调用者的栈变量**。
    // 原因：用户态栈指针在 syscall 返回后会错位，
    //       导致 &m 在传出与读回时不是同一个地址（实测差 8 字节）。
    for (;;) {
        int r = static_cast<int>(
            syscall2(static_cast<u64>(Sys::IPC_RECV),
                     reinterpret_cast<u64>(from_tid),
                     IPC_SLOT_RECV));
        if (r != IPC_WOULD_BLOCK) return r;
        syscall1(static_cast<u64>(Sys::YIELD), 0);
    }
#endif
}

// ===========================================================================
//  内核侧实现（定义在 kernel/ipc.cpp 的 namespace ipc 内）
//  ===========================================================================
namespace ipc {
    void init();
    int  do_call(int dest_tid, IpcMsg* msg);
    int  do_reply(int dest_tid, IpcMsg* msg);
    int  do_send(int dest_tid, IpcMsg* msg);
    int  do_recv(int* from_tid, IpcMsg* msg);
    // 把硬件中断投递给注册了它的驱动进程
    void deliver_irq(u8 irq);
}   // namespace ipc

// 内核入口包装（供汇编/C 调用）
int ipc_call_kernel(int dest, IpcMsg* msg);
int ipc_reply_kernel(int dest, IpcMsg* msg);
int ipc_send_kernel(int dest, IpcMsg* msg);
int ipc_recv_kernel(int* from, IpcMsg* msg);
