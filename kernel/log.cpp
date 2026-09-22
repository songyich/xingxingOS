// ===========================================================================
//  kernel/log.cpp —— 分批记录 + 周期清空（落盘可选）
//  ==========================================================================
//  【用户方案（2026-09-15 澄清）】
//
//    "所有日志都往内存里记，每隔两秒钟就清除。
//     如果日志开关是开的话，那就先记录（落盘），然后再清除。"
//
//  ---------------------------------------------------------------------------
//  ★ 核心语义：**清空是无条件的**
//  ---------------------------------------------------------------------------
//    每 2 秒一个周期。周期一到：
//        开关 开 → 先把这一批**落盘**，然后清空
//        开关 关 → **直接清空**（丢弃，什么都不留）
//
//    所以内存里**永远只有最近 2 秒**的日志，不会随时间累积。
//    内存占用恒定，不随时间增长 —— 这正是这个方案的价值。
//
//  ---------------------------------------------------------------------------
//  ★ 与前一版（环形缓冲保留历史）的区别
//  ---------------------------------------------------------------------------
//    前一版：开关关时内存持续累积（满了才覆盖），事后开开关能捞回 19 秒历史
//    这一版：每 2 秒必清空，事后开开关**只能拿到最近 2 秒**
//
//    取舍：换来的是**内存占用恒定**（不会因为忘记关开关而膨胀）。
//    用户选择了这一版。
//
//  ---------------------------------------------------------------------------
//  ★ 性能：记录路径极轻
//  ---------------------------------------------------------------------------
//    - 不看开关（开关只在周期结束时查一次）
//    - 不格式化字符串（内存存 32 字节二进制，落盘时才转文本）
//    - 线性写 + 计数递增，**无取模运算**（前一版环形缓冲需要取模）
//
//  【落盘目标：当前串口，阶段 8a 后换硬盘】
//    现在没有文件系统，先写 COM1，宿主侧 -serial file:xxx.log 落盘。
//    等 8a 之后把输出目标换成文件，上层逻辑一行不用改。
// ===========================================================================
#include <kernel/log.hpp>

#if LOG_MASTER

#include <kernel/serial.hpp>
#include <kernel/pit.hpp>
#include <kernel/thread.hpp>

namespace klog {

// ---------------------------------------------------------------------------
//  批次缓冲
//  -------------------------------------------------------------------------
//  容量 = 单个周期（2 秒）能产生的最大条数。
//    只开 IPC：845 条/秒 × 2 = 1690
//    临时开 SYSCALL：2222 条/秒 × 2 ≈ 4444
//  取 8192 = 256 KB，足够覆盖，且占用恒定。
//
//  线性使用（不是环形）：写满就丢弃后来的，等下个周期清空。
//  这样 record() 里没有取模，比环形更快。
// ---------------------------------------------------------------------------
static Entry  g_buf[LOG_CAPACITY];
static int    g_count = 0;      // 当前批次已记录条数
static int    g_dropped = 0;    // 本周期内因批次满而丢弃的条数

// 运行期落盘开关
static bool   g_flush_on = false;

// 周期长度
static constexpr u64 FLUSH_INTERVAL_MS = 2000;
static u64    g_last_period_ms = 0;

// ---------------------------------------------------------------------------
//  记录一条：只写内存，不查开关、不格式化
//  -------------------------------------------------------------------------
//  可能在中断上下文调用，所以**不加锁**（加锁会死锁）。
// ---------------------------------------------------------------------------
void record(LogMod mod, LogOp op, u64 addr, u64 ret, u64 extra)
{
    if (g_count >= LOG_CAPACITY) {
        // 本周期批次满了：丢弃（等下个周期清空后重新开始）
        ++g_dropped;
        return;
    }

    Entry* e = &g_buf[g_count++];
    e->tick  = pit::ticks();
    e->addr  = addr;
    e->ret   = ret;
    e->extra = extra;
    e->tid   = static_cast<u32>(thread::current_tid());
    e->mod   = static_cast<u16>(mod);
    e->op    = static_cast<u16>(op);
}

// ---------------------------------------------------------------------------
//  输出格式化（只在落盘时执行，热路径不做）
// ---------------------------------------------------------------------------
namespace {

void put_dec(u64 v)
{
    char buf[24];
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        while (v > 0 && n < 23) {
            buf[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
    }
    while (n > 0) serial::putc(buf[--n]);
}

void put_hex(u64 v)
{
    static const char* digits = "0123456789ABCDEF";
    char buf[20];
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        while (v > 0 && n < 19) {
            buf[n++] = digits[v & 0xF];
            v >>= 4;
        }
    }
    serial::putc('0');
    serial::putc('x');
    while (n > 0) serial::putc(buf[--n]);
}

void put_str(const char* s)
{
    while (*s != '\0') serial::putc(*s++);
}

const char* mod_name(u16 m)
{
    switch (static_cast<LogMod>(m)) {
    case LogMod::PMM:     return "PMM";
    case LogMod::VMM:     return "VMM";
    case LogMod::HEAP:    return "HEAP";
    case LogMod::IPC:     return "IPC";
    case LogMod::SYSCALL: return "SYSCALL";
    case LogMod::SCHED:   return "SCHED";
    case LogMod::ISR:     return "ISR";
    case LogMod::PORT:    return "PORT";
    case LogMod::ACPI:    return "ACPI";
    }
    return "?";
}

const char* op_name(u16 o)
{
    switch (static_cast<LogOp>(o)) {
    case LogOp::None:          return "none";
    case LogOp::IpcCall:       return "ipc_call";
    case LogOp::IpcRecv:       return "ipc_recv";
    case LogOp::IpcReply:      return "ipc_reply";
    case LogOp::IpcSend:       return "ipc_send";
    case LogOp::IrqDeliver:    return "irq_deliver";
    case LogOp::SyscallEnter:  return "syscall_enter";
    case LogOp::AcpiShutdown:  return "acpi_shutdown";
    case LogOp::AcpiReboot:    return "acpi_reboot";
    case LogOp::PmmAlloc:      return "pmm_alloc";
    case LogOp::PmmFree:       return "pmm_free";
    case LogOp::VmmMap:        return "vmm_map";
    case LogOp::VmmUnmap:      return "vmm_unmap";
    case LogOp::HeapAlloc:     return "heap_alloc";
    case LogOp::HeapFree:      return "heap_free";
    case LogOp::SchedSwitch:   return "sched_switch";
    }
    return "?";
}

}   // namespace

// 单次落盘上限：flush 在定时器中断里，不能一次输出太多
static constexpr int MAX_PER_FLUSH = 256;

// ---------------------------------------------------------------------------
//  清空当前批次（无条件 —— 不管有没有落盘过）
//  -------------------------------------------------------------------------
//  这就是用户说的"每隔两秒钟就清除"。
// ---------------------------------------------------------------------------
static void clear_batch()
{
    g_count = 0;
    g_dropped = 0;
}

// ---------------------------------------------------------------------------
//  ★ 落盘：把当前批次输出（最多 256 条），然后**无条件清空**
//  -------------------------------------------------------------------------
//  注意：
//    - 即使**没有落盘**（串口不可用），也会清空
//    - 超出 256 条的部分**丢弃**，不留到下个周期
//
//  清空是无条件的，这是用户方案的语义。
// ---------------------------------------------------------------------------
void flush_now()
{
    if (serial::ready() && g_count > 0) {
        int n = (g_count > MAX_PER_FLUSH) ? MAX_PER_FLUSH : g_count;

        put_str("==== 内核日志落盘（");
        put_dec(static_cast<u64>(n));
        put_str(" 条");
        if (g_dropped > 0) {
            put_str("，本周期丢弃 ");
            put_dec(static_cast<u64>(g_dropped));
            put_str(" 条");
        }
        put_str("）====\n");

        for (int i = 0; i < n; ++i) {
            const Entry* e = &g_buf[i];

            serial::putc('[');
            put_dec(e->tick);
            put_str("] TID=");
            put_dec(e->tid);
            put_str(" MOD=");
            put_str(mod_name(e->mod));
            put_str(" OP=");
            put_str(op_name(e->op));
            put_str(" ADDR=");
            put_hex(e->addr);
            put_str(" RET=");
            put_hex(e->ret);
            if (e->extra != 0) {
                put_str(" X=");
                put_hex(e->extra);
            }
            serial::putc('\n');
        }

        // ⚠️ 超出单批上限的部分**直接丢弃**，不留到下个周期。
        //
        //   曾经写成"剩余移前、留到下批"，但那违反了
        //   "每 2 秒无条件清空"的语义 —— 内存会一直留着旧数据不清。
        //   用户要的是：周期一到就清空。落不下的就是落不下（串口太慢）。
        if (n < g_count) {
            put_str("（本周期未落盘 ");
            put_dec(static_cast<u64>(g_count - n));
            put_str(" 条，已达单批上限，丢弃）\n");
        }
    }

    // ★ 无条件清空：不管有没有落盘、有没有落完
    clear_batch();
}

// ---------------------------------------------------------------------------
//  ★ 周期处理：每 2 秒执行一次，**清空是无条件的**
//  -------------------------------------------------------------------------
//    开关 开 → 先落盘，再清空
//    开关 关 → 直接清空（丢弃）
// ---------------------------------------------------------------------------
void periodic_flush()
{
    u64 now = pit::uptime_ms();
    if (now - g_last_period_ms < FLUSH_INTERVAL_MS) return;
    g_last_period_ms = now;

    if (g_flush_on) {
        flush_now();        // 先落盘（内部会清空）
    } else {
        clear_batch();      // 开关关：直接丢弃
    }
}

void set_flush_enabled(bool on)
{
    g_flush_on = on;
}

bool flush_enabled() { return g_flush_on; }

void init()
{
    g_count = 0;
    g_dropped = 0;
    g_flush_on = false;
    g_last_period_ms = pit::uptime_ms();
}

}   // namespace klog

#endif   // LOG_MASTER
