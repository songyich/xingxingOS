// ===========================================================================
//  include/kernel/log.hpp —— 统一内核日志（环形缓冲 + 按需落盘）
//  ==========================================================================
//  【需求来源】用户明确要求（2026-09-13），并在 2026-09-15 改进方案：
//
//    "默认都记日志，但是隔几秒有一个判断：
//     如果是开的，就从内存里把它读到硬盘，清空那部分的内存。"
//
//  ---------------------------------------------------------------------------
//  ★ 核心设计：分批记录 + 周期无条件清空
//  ---------------------------------------------------------------------------
//    用户原话（2026-09-15）：
//      "所有日志都往内存里记，每隔两秒钟就清除。
//       如果日志开关是开的话，那就先记录（落盘），然后再清除。"
//
//    每 2 秒一个周期：
//        开关 开 → 先把这一批**落盘**，然后清空
//        开关 关 → **直接清空**（丢弃）
//
//    ★ 关键：**清空是无条件的**，开关只决定"清空前要不要先落盘"。
//
//    好处：
//      1. 热路径**零 if**（记录时完全不看开关）
//      2. **内存占用恒定** 256 KB，不会随时间累积膨胀
//      3. 记录开销极小（见下面"性能优化 1"）
//
//  ---------------------------------------------------------------------------
//  ★ 性能优化 1：内存里只存**二进制**，不存文本
//  ---------------------------------------------------------------------------
//    如果每条日志都格式化字符串（数字转文本），开销依然可观。
//    所以环形缓冲里存的是固定 32 字节的结构体：
//        { tick, tid, mod_id, op_id, addr, ret, extra }
//    模块名/操作名用**枚举 ID**，不是字符串。
//    只有**落盘那一刻**才格式化成人类可读的文本。
//    → "记录"退化成几次内存写 + 索引递增。
//
//  ---------------------------------------------------------------------------
//  ★ 性能优化 2：编译期仍可整体剔除
//  ---------------------------------------------------------------------------
//    LOG_MASTER=0 时整个日志系统不编译（零开销，用于 Release）。
//    LOG_ENABLE_XXX=0 时该模块的记录点被剔除（用于极热路径）。
//
//  ---------------------------------------------------------------------------
//  ★ 落盘目标：当前是串口，阶段 8a 后换成硬盘
//  ---------------------------------------------------------------------------
//    现在还没有文件系统 / 磁盘驱动（阶段 8a 才有），
//    所以 flush 先写**串口 COM1**，宿主侧 -serial file:xxx.log 落盘。
//    等 8a 之后把 flush_to_storage() 的落点换成文件即可，
//    上层逻辑与调用点**一行都不用改**。
// ===========================================================================
#pragma once

#include <kernel/types.h>

// ---------------------------------------------------------------------------
//  ★ 总开关（编译期）
//    1 = 日志系统参与编译；0 = 完全剔除（Release）
// ---------------------------------------------------------------------------
#ifndef LOG_MASTER
#define LOG_MASTER   1
#endif

// ---------------------------------------------------------------------------
//  ★ 每模块记录开关（编译期）
//
//    注意：这里控制的是"要不要**记录到内存**"，
//    不是"要不要输出"。输出由运行时的 g_log_flush_enabled 控制。
//
//    默认全开 —— 因为写内存开销极小。
//    只有极热路径（SCHED / ISR / PORT）建议关掉。
// ---------------------------------------------------------------------------
#ifndef LOG_ENABLE_PMM
#define LOG_ENABLE_PMM      1   // 物理内存分配/释放
#endif
#ifndef LOG_ENABLE_VMM
#define LOG_ENABLE_VMM      1   // 页表映射/取消映射
#endif
#ifndef LOG_ENABLE_HEAP
#define LOG_ENABLE_HEAP     1   // 内核堆
#endif
#ifndef LOG_ENABLE_IPC
#define LOG_ENABLE_IPC      1   // 微内核 IPC
#endif
#ifndef LOG_ENABLE_SYSCALL
#define LOG_ENABLE_SYSCALL  0   // 系统调用
// ⚠️ 默认关：实测它占全部日志的 **62%**（约 1370 条/秒），
//    会把缓冲在 2 秒内冲光，其他模块的日志全被覆盖。
//    需要排查"谁请求的"时手动打开，并尽快关闭。
#endif
#ifndef LOG_ENABLE_SCHED
#define LOG_ENABLE_SCHED    0   // 线程调度（极热，默认关）
#endif
#ifndef LOG_ENABLE_ISR
#define LOG_ENABLE_ISR      0   // 中断分发（极热，默认关）
#endif
#ifndef LOG_ENABLE_PORT
#define LOG_ENABLE_PORT     0   // 端口 I/O（极热，默认关）
#endif
#ifndef LOG_ENABLE_ACPI
#define LOG_ENABLE_ACPI     1   // ACPI / 电源
#endif

// ---------------------------------------------------------------------------
//  单周期（2 秒）批次容量：8192 条 × 32 字节 = 256 KB
//
//    【语义】这不是"能存多久的历史"，而是**一个周期内的上限**。
//      每 2 秒无条件清空，所以内存占用**恒定 256 KB，不随时间增长**。
//
//    【为什么是 8192】按实测产生速率估算：
//      只开 IPC：845 条/秒 × 2 秒 = 1690
//      临时开 SYSCALL：2222 条/秒 × 2 秒 ≈ 4444
//      8192 留了充足余量，周期内的日志基本不会丢。
//
//    放在 .bss，不占内核镜像体积。
//
//    ⚠️ 硬限制：串口 115200 ≈ 11.5 KB/s ≈ 143 条/秒，
//       而日志产生 845 条/秒。所以即使开关开着，
//       一个周期内的日志也**只能落盘其中一小部分**（单批上限 256 条），
//       其余在清空时被丢弃。
//       等阶段 8a 有硬盘后，落盘带宽会大得多，此限制自然解除。
// ---------------------------------------------------------------------------
constexpr int LOG_CAPACITY = 8192;

// ---------------------------------------------------------------------------
//  模块 ID 与操作 ID（**枚举，不是字符串** —— 省内存也省时间）
// ---------------------------------------------------------------------------
enum class LogMod : u16 {
    PMM, VMM, HEAP, IPC, SYSCALL, SCHED, ISR, PORT, ACPI,
};

enum class LogOp : u16 {
    // 通用
    None = 0,

    // IPC
    IpcCall, IpcRecv, IpcReply, IpcSend, IrqDeliver,

    // Syscall
    SyscallEnter,

    // ACPI / 电源
    AcpiShutdown, AcpiReboot,

    // 内存
    PmmAlloc, PmmFree, VmmMap, VmmUnmap, HeapAlloc, HeapFree,

    // 调度
    SchedSwitch,
};

namespace klog {

#if LOG_MASTER

// ---------------------------------------------------------------------------
//  环形缓冲中的一条记录（**二进制**，32 字节）
// ---------------------------------------------------------------------------
struct [[gnu::packed]] Entry {
    u64   tick;     // 时间戳（PIT ticks）
    u64   addr;     // 访问了哪个地址
    u64   ret;      // 返回值 / 结果
    u64   extra;    // 附加数值
    u32   tid;      // 哪个程序请求的
    u16   mod;      // LogMod
    u16   op;       // LogOp
};

// 初始化：串口之后、线程子系统之前调用
void init();

// ---------------------------------------------------------------------------
//  ★ 记录一条日志（热路径调用，极轻量）
//  -------------------------------------------------------------------------
//  不检查任何开关、不格式化任何字符串 —— 只是写内存 + 递增索引。
// ---------------------------------------------------------------------------
void record(LogMod mod, LogOp op, u64 addr, u64 ret, u64 extra = 0);

// ---------------------------------------------------------------------------
//  ★ 运行期落盘开关
//  -------------------------------------------------------------------------
//  这就是"隔 2 秒判断一次"的那个开关。
//  由 UI 设置 / 命令行（log on / log off）控制。
// ---------------------------------------------------------------------------
void set_flush_enabled(bool on);
bool flush_enabled();

// ---------------------------------------------------------------------------
//  ★ 周期处理（由定时器中断驱动，每 2 秒）
//  -------------------------------------------------------------------------
//  逻辑（**清空无条件**）：
//    - 开关开 → 先把这一批落盘，然后清空
//    - 开关关 → 直接清空（丢弃）
//
//  用户原话："每隔两秒钟就清除；如果开关是开的，那就先落盘，然后再清除"
// ---------------------------------------------------------------------------
void periodic_flush();

// 手动立即落盘（比如崩溃前抢救日志）
void flush_now();

#else   // !LOG_MASTER —— 全部变成空内联，零开销

inline void init() {}
inline void record(LogMod, LogOp, u64, u64, u64 = 0) {}
inline void set_flush_enabled(bool) {}
inline bool flush_enabled() { return false; }
inline void periodic_flush() {}
inline void flush_now() {}

#endif  // LOG_MASTER

}   // namespace klog

// ---------------------------------------------------------------------------
//  ★ 每模块记录宏
//  -------------------------------------------------------------------------
//  用法：LOG_IPC(LogOp::IpcCall, addr, ret)
//        LOG_IPC2(LogOp::IpcCall, addr, ret, extra)
// ---------------------------------------------------------------------------
#if LOG_MASTER && LOG_ENABLE_PMM
  #define LOG_PMM(op, addr, ret)         klog::record(LogMod::PMM, op, addr, ret)
  #define LOG_PMM2(op, addr, ret, x)     klog::record(LogMod::PMM, op, addr, ret, x)
#else
  #define LOG_PMM(op, addr, ret)         ((void)0)
  #define LOG_PMM2(op, addr, ret, x)     ((void)0)
#endif

#if LOG_MASTER && LOG_ENABLE_VMM
  #define LOG_VMM(op, addr, ret)         klog::record(LogMod::VMM, op, addr, ret)
  #define LOG_VMM2(op, addr, ret, x)     klog::record(LogMod::VMM, op, addr, ret, x)
#else
  #define LOG_VMM(op, addr, ret)         ((void)0)
  #define LOG_VMM2(op, addr, ret, x)     ((void)0)
#endif

#if LOG_MASTER && LOG_ENABLE_HEAP
  #define LOG_HEAP(op, addr, ret)        klog::record(LogMod::HEAP, op, addr, ret)
  #define LOG_HEAP2(op, addr, ret, x)    klog::record(LogMod::HEAP, op, addr, ret, x)
#else
  #define LOG_HEAP(op, addr, ret)        ((void)0)
  #define LOG_HEAP2(op, addr, ret, x)    ((void)0)
#endif

#if LOG_MASTER && LOG_ENABLE_IPC
  #define LOG_IPC(op, addr, ret)         klog::record(LogMod::IPC, op, addr, ret)
  #define LOG_IPC2(op, addr, ret, x)     klog::record(LogMod::IPC, op, addr, ret, x)
#else
  #define LOG_IPC(op, addr, ret)         ((void)0)
  #define LOG_IPC2(op, addr, ret, x)     ((void)0)
#endif

#if LOG_MASTER && LOG_ENABLE_SYSCALL
  #define LOG_SYSCALL(op, addr, ret)     klog::record(LogMod::SYSCALL, op, addr, ret)
  #define LOG_SYSCALL2(op, addr, ret, x) klog::record(LogMod::SYSCALL, op, addr, ret, x)
#else
  #define LOG_SYSCALL(op, addr, ret)     ((void)0)
  #define LOG_SYSCALL2(op, addr, ret, x) ((void)0)
#endif

#if LOG_MASTER && LOG_ENABLE_SCHED
  #define LOG_SCHED(op, addr, ret)       klog::record(LogMod::SCHED, op, addr, ret)
  #define LOG_SCHED2(op, addr, ret, x)   klog::record(LogMod::SCHED, op, addr, ret, x)
#else
  #define LOG_SCHED(op, addr, ret)       ((void)0)
  #define LOG_SCHED2(op, addr, ret, x)   ((void)0)
#endif

#if LOG_MASTER && LOG_ENABLE_ISR
  #define LOG_ISR(op, addr, ret)         klog::record(LogMod::ISR, op, addr, ret)
  #define LOG_ISR2(op, addr, ret, x)     klog::record(LogMod::ISR, op, addr, ret, x)
#else
  #define LOG_ISR(op, addr, ret)         ((void)0)
  #define LOG_ISR2(op, addr, ret, x)     ((void)0)
#endif

#if LOG_MASTER && LOG_ENABLE_PORT
  #define LOG_PORT(op, addr, ret)        klog::record(LogMod::PORT, op, addr, ret)
  #define LOG_PORT2(op, addr, ret, x)    klog::record(LogMod::PORT, op, addr, ret, x)
#else
  #define LOG_PORT(op, addr, ret)        ((void)0)
  #define LOG_PORT2(op, addr, ret, x)    ((void)0)
#endif

#if LOG_MASTER && LOG_ENABLE_ACPI
  #define LOG_ACPI(op, addr, ret)        klog::record(LogMod::ACPI, op, addr, ret)
  #define LOG_ACPI2(op, addr, ret, x)    klog::record(LogMod::ACPI, op, addr, ret, x)
#else
  #define LOG_ACPI(op, addr, ret)        ((void)0)
  #define LOG_ACPI2(op, addr, ret, x)    ((void)0)
#endif
