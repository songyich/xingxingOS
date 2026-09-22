// ============================================================================
//  include/kernel/syscall.hpp —— 系统调用号与调用封装
//  ---------------------------------------------------------------------------
//  微内核的「系统调用」只有极少数几类：
//    1. IPC        —— 一切服务都靠它，是内核对外的主要能力
//    2. 中断注册   —— 驱动进程告诉内核「这个 IRQ 归我管」
//    3. 特权操作   —— 端口读写、设备内存映射（驱动在用户态不能直接做）
//    4. 基础       —— 让出 CPU、睡眠、退岀
//
//  注意这里**没有** open/read/write/fork 这类东西：
//    它们属于文件系统服务和进程服务，要通过 IPC 去做，
//    而不是直接进内核。这就是微内核和宏内核的分界线。
// ============================================================================
#pragma once

#include <kernel/types.h>

// ---------------------------------------------------------------------------
//  系统调用号
// ---------------------------------------------------------------------------
enum class Sys : u64 {
    // --- IPC ---
    IPC_CALL    = 1,    // 发送 + 等回复（一次切换完成，最快路径）
    IPC_REPLY   = 2,    // 回复给调用者
    IPC_SEND    = 3,    // 异步发送，不等回复
    IPC_RECV    = 4,    // 接收一条消息

    // --- 中断（驱动进程用）---
    IRQ_REGISTER = 10,  // 注册：这个 IRQ 来了请通知我
    IRQ_ACK      = 11,  // 处理完了，内核可以继续放行了

    // --- 特权操作（驱动进程用）---
    PORT_IN      = 20,  // 读 I/O 端口
    PORT_OUT     = 21,  // 写 I/O 端口
    MMIO_MAP     = 22,  // 把设备物理内存映射进本进程地址空间

    // --- 基础 ---
    YIELD        = 30,
    SLEEP        = 31,
    UPTIME       = 32,
    EXIT         = 33,
    PUTS         = 34,  // 调试输出（应急用，正常走终端服务 IPC）
    SHUTDOWN     = 35,  // ACPI 关机
    REBOOT       = 36,  // 复位（写 0x64 端口，最小特权操作）
    THREAD_INFO  = 37,  // 查询线程列表：a1=共享页地址，返回线程数
    LOG_CONTROL  = 38,  // 日志落盘开关：a1=0关 1开 2立即落盘一次
    CLEAR        = 39,  // 清屏（供终端服务转发 MSG_CLEAR）

    // --- 程序与服务（万物皆可程序）---
    SPAWN        = 40,  // 启动 .xzs 程序：a1=名字指针 a2=名字长度 a3=argv a4=argc
    SVC_REGISTER = 41,  // 注册服务：a1=名字指针 a2=名字长度（把当前线程登记为该服务）
    SVC_LOOKUP   = 42,  // 查找服务：a1=名字指针 a2=名字长度，返回 tid
    WAIT         = 44,  // 等某个线程结束：a1=tid
    MOUSE_DRAW   = 45,  // 画鼠标指针：a1=模式(0绝对 1相对) a2=x/dx a3=y/dy
    MOUSE_CENTER = 46,  // 指针移到屏幕正中，返回 (y<<32)|x（给服务同步初始坐标）
};

// ---------------------------------------------------------------------------
//  用户态调用封装
//  -------------------------------------------------------------------------
//  用 inline 是因为它要同时被内核（测试用）和用户态服务进程使用，
//  而且必须是 inline：系统调用要尽可能少跳转。
//
//  约束（x86_64 syscall 指令的规矩）：
//    - 系统调用号放 rax
//    - 参数用 rdi, rsi, rdx, **r10**, r8, r9（第 4 个是 r10 不是 rcx！）
//      因为 rcx 被 syscall 用来存返回地址了
//    - 返回值在 rax
// ---------------------------------------------------------------------------
inline u64 syscall6(u64 num, u64 a1 = 0, u64 a2 = 0, u64 a3 = 0,
                    u64 a4 = 0, u64 a5 = 0, u64 a6 = 0)
{
    u64 ret;
    register u64 r10 asm("r10") = a4;
    register u64 r8  asm("r8")  = a5;
    register u64 r9  asm("r9")  = a6;

    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}

inline u64 syscall0(u64 num)
{
    return syscall6(num, 0, 0, 0, 0, 0, 0);
}

inline u64 syscall1(u64 num, u64 a1)
{
    return syscall6(num, a1, 0, 0, 0, 0, 0);
}
inline u64 syscall2(u64 num, u64 a1, u64 a2)
{
    return syscall6(num, a1, a2, 0, 0, 0, 0);
}
inline u64 syscall3(u64 num, u64 a1, u64 a2, u64 a3)
{
    return syscall6(num, a1, a2, a3, 0, 0, 0);
}

inline u64 syscall4(u64 num, u64 a1, u64 a2, u64 a3, u64 a4)
{
    return syscall6(num, a1, a2, a3, a4, 0, 0);
}

// ---------------------------------------------------------------------------
//  内核侧
// ---------------------------------------------------------------------------
#ifdef KERNEL_BUILD

// 装载 syscall 相关 MSR（由 gdt.cpp 或 kmain 调用）
extern "C" void syscall_enable();

// C++ 分发函数（由 syscall_entry.asm 调用）
//
// 参数是完整的寄存器现场，返回值是「接下来要恢复的现场」——
// 和中断出口 isr_handler 的机制完全一样，
// 这样 syscall 和中断两条路径能共用一套线程切换逻辑。
struct Registers;
extern "C" u64 syscall_handler(Registers* regs);

namespace syscall {
    void init();
    // 切换线程时更新 syscall 入口的内核栈顶
    void set_kernel_stack(u64 top);
}

#endif
