// ============================================================================
//  kernel/pic.cpp —— 8259 初始化与操作
//  ---------------------------------------------------------------------------
//  8259 的编程模型：每片只有两个端口
//    主片：0x20 命令口 / 0x21 数据口
//    从片：0xA0 命令口 / 0xA1 数据口
//
//  初始化要按顺序写 4 个 ICW（Initialization Command Word）：
//    ICW1  告诉它「我要开始初始化了」，以及是否级联
//    ICW2  中断号基址——我们要的 32 和 40 就是在这里设的
//    ICW3  两片之间怎么连线
//    ICW4  工作模式（8086 模式）
//  写完 ICW 之后，同一个数据口再写就变成 OCW（操作命令），
//  用来屏蔽中断线或者发 EOI。硬件靠「当前处于什么阶段」区分这两种含义。
// ============================================================================

#include <kernel/pic.hpp>
#include <kernel/io.h>

namespace {

constexpr u16 PIC1_COMMAND = 0x20;      // 主片命令口
constexpr u16 PIC1_DATA    = 0x21;      // 主片数据口
constexpr u16 PIC2_COMMAND = 0xA0;      // 从片命令口
constexpr u16 PIC2_DATA    = 0xA1;      // 从片数据口

constexpr u8 ICW1_INIT     = 0x10;      // 初始化标志
constexpr u8 ICW1_ICW4     = 0x01;      // 还需要写 ICW4
constexpr u8 ICW4_8086     = 0x01;      // 8086/88 模式

constexpr u8 OCW2_EOI      = 0x20;      // 普通的「中断结束」命令

// 当前 16 条 IRQ 线的屏蔽状态（1 = 屏蔽）。读改写时需要先知道旧值。
u16 g_mask = 0xFFFF;                    // 默认全屏蔽，等驱动逐个打开

}  // namespace

namespace pic {

void init()
{
    // --- ICW1：开始初始化，两片都要 ---
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    // --- ICW2：中断号基址 ---
    // 主片 IRQ0~7  -> 32~39
    outb(PIC1_DATA, IRQ_BASE);
    io_wait();
    // 从片 IRQ8~15 -> 40~47
    outb(PIC2_DATA, IRQ_BASE + 8);
    io_wait();

    // --- ICW3：级联关系 ---
    outb(PIC1_DATA, 0x04);              // 主片：从片接在第 2 条线上（bit2）
    io_wait();
    outb(PIC2_DATA, 0x02);              // 从片：我的级联标识是 2
    io_wait();

    // --- ICW4：8086 模式 ---
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    // --- 先把所有中断都屏蔽掉 ---
    // 驱动初始化时再逐个 unmask，避免还没装好处理函数就被打断
    set_mask_all(0xFFFF);
}

void send_eoi(u8 int_no)
{
    // 中断号 40~47 说明中断来自从片，必须给从片也发一次 EOI
    if (int_no >= IRQ_BASE + 8) {
        outb(PIC2_COMMAND, OCW2_EOI);
    }
    // 无论主片还是从片来的，主片都要发
    outb(PIC1_COMMAND, OCW2_EOI);
}

void mask(u8 irq)
{
    if (irq > 15) {
        return;
    }
    g_mask |= static_cast<u16>(1u << irq);

    if (irq < 8) {
        outb(PIC1_DATA, static_cast<u8>(g_mask & 0xFF));
    } else {
        outb(PIC2_DATA, static_cast<u8>((g_mask >> 8) & 0xFF));
    }
}

void unmask(u8 irq)
{
    if (irq > 15) {
        return;
    }
    g_mask &= static_cast<u16>(~(1u << irq));

    if (irq < 8) {
        outb(PIC1_DATA, static_cast<u8>(g_mask & 0xFF));
    } else {
        outb(PIC2_DATA, static_cast<u8>((g_mask >> 8) & 0xFF));
    }
}

void set_mask_all(u16 mask_bits)
{
    g_mask = mask_bits;
    outb(PIC1_DATA, static_cast<u8>(g_mask & 0xFF));
    io_wait();
    outb(PIC2_DATA, static_cast<u8>((g_mask >> 8) & 0xFF));
    io_wait();
}

}  // namespace pic
