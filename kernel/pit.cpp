// ============================================================================
//  kernel/pit.cpp —— 定时器实现
//  ==========================================================================
//  端口：
//    0x40  通道 0 的数据口（我们用的就是这个，它连着 IRQ0）
//    0x41  通道 1（现代机器上已废弃）
//    0x42  通道 2（连到 PC 扬声器）
//    0x43  控制口
//
//  控制字格式（8 位）：
//    bit7-6  选通道       00 = 通道 0
//    bit5-4  读写方式     11 = 先写低字节、再写高字节
//    bit3-1  工作模式     011 = 模式 3（方波发生器，最常用）
//    bit0    计数进制     0 = 二进制，1 = BCD
//    合起来 = 0b00110110 = 0x36
// ============================================================================

#include <kernel/pit.hpp>
#include <kernel/log.hpp>
#include <kernel/terminal.hpp>
#include <kernel/supervisor.hpp>
#include <kernel/thread.hpp>
#include <kernel/io.h>
#include <kernel/isr.hpp>
#include <kernel/pic.hpp>

namespace {

constexpr u16 PIT_CHANNEL0 = 0x40;
constexpr u16 PIT_COMMAND  = 0x43;

constexpr u32 PIT_BASE_FREQ = 1193182;      // 基准频率，硬件固定

// volatile 同理：g_ticks 在定时器中断里自增，主循环里读。
volatile u64 g_ticks = 0;                   // 累计中断次数
u32 g_frequency = 0;                        // 当前频率

// 定时器中断处理函数：唯一要做的就是给计数器 +1
// 中断处理函数要尽可能短——它抢占了别的代码，干太久会拖慢整个系统
void timer_handler(const Registers*)
{
    ++g_ticks;

    // 【统一日志】定期检查落盘开关。
    //   内部自己判断间隔（默认每 2 秒），且开关关时立即返回 ——
    //   所以这里每 tick 调用的开销只是一次函数调用 + 一个 bool 判断。
    klog::periodic_flush();

    // 光标闪烁的心跳：累计 tick 到周期就翻转光标亮/灭。
    // 放在调度之后，避免影响调度时机的精确性。
    term::tick_cursor();

    // 【阶段 6】推进调度：扣减时间片、唤醒睡眠到期的线程。
    // 真正的切栈不在这里做，而是统一放到中断出口（sched::on_interrupt）。
    sched::tick();

    // 【P2 崩溃自愈】把崩溃的服务重新拉起来。
    //   放在这里而不是异常上下文里，是因为重启要构造栈、写现场，
    //   在中断栈上做这些事风险太大。定时器中断是稳定的普通上下文。
    supervisor::tick();
}

}  // namespace

namespace pit {

void init(u32 frequency)
{
    g_frequency = frequency;
    g_ticks = 0;

    // 算出分频值。注意 16 位计数器最大 65535，
    // 频率低于 1193182/65536 ≈ 18.2 Hz 就装不下了（实践中不会这么低）
    u32 divisor = PIT_BASE_FREQ / frequency;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }

    // 写控制字：通道 0，先低后高，模式 3，二进制
    outb(PIT_COMMAND, 0x36);
    io_wait();

    // 分频值要分两次写：先低 8 位，再高 8 位
    outb(PIT_CHANNEL0, static_cast<u8>(divisor & 0xFF));
    io_wait();
    outb(PIT_CHANNEL0, static_cast<u8>((divisor >> 8) & 0xFF));
    io_wait();

    // 登记中断处理函数，然后放开 IRQ0
    isr::register_handler(pic::IRQ_BASE + pic::IRQ_TIMER, timer_handler);
    pic::unmask(pic::IRQ_TIMER);
}

u32 frequency()
{
    return g_frequency;
}

u64 ticks()
{
    return g_ticks;
}

u64 uptime_ms()
{
    if (g_frequency == 0) {
        return 0;
    }
    return g_ticks * 1000 / g_frequency;
}

u64 uptime_seconds()
{
    if (g_frequency == 0) {
        return 0;
    }
    return g_ticks / g_frequency;
}

void busy_wait_ms(u32 ms)
{
    // 空转等待。没有调度器之前只能这么干——
    // 等阶段 6 有了内核线程，这里就能换成「让出 CPU」
    u64 target = g_ticks + (static_cast<u64>(ms) * g_frequency / 1000);
    while (g_ticks < target) {
        asm volatile("hlt");        // hlt 让 CPU 休眠到下一个中断，省电
    }
}

}  // namespace pit
