// ============================================================================
//  include/kernel/pic.hpp —— 8259 可编程中断控制器
//  ---------------------------------------------------------------------------
//  为什么必须重映射？
//    PC 刚开机时 BIOS 把 IRQ0~7 映射成中断号 8~15。
//    但中断号 0~31 是 CPU 保留给异常的（除零、页错误……都在这个区间）。
//    于是一个 IRQ 打进来，CPU 会以为发生了异常——直接崩。
//    解决办法：把 IRQ0~15 整体挪到中断号 32~47，避开前 32 个。
//
//  8259 的两片级联结构：
//    主片管 IRQ0~7，从片管 IRQ8~15，从片通过 IRQ2 挂在主片上。
//    所以给从片的中断发 EOI 时，主从两片都要发。
// ============================================================================
#pragma once

#include <kernel/types.h>

namespace pic {

// 重映射后 IRQ 对应的中断号
constexpr u8 IRQ_BASE    = 32;      // IRQ0 -> 中断号 32
constexpr u8 IRQ_TIMER   = 0;       // IRQ0：PIT 定时器
constexpr u8 IRQ_KEYBOARD= 1;       // IRQ1：PS/2 键盘
constexpr u8 IRQ_SLAVE   = 2;       // IRQ2：级联从片（不是真实设备）

// 初始化并重映射：IRQ0~15 -> 中断号 32~47
void init();

// 中断处理完必须调用，否则后续同级别中断会被永久阻塞
void send_eoi(u8 int_no);

// 屏蔽 / 放开某个 IRQ（编号 0~15，不是中断号）
void mask(u8 irq);
void unmask(u8 irq);

// 一次性设置 16 个 IRQ 的屏蔽位（1 = 屏蔽）
void set_mask_all(u16 mask_bits);

}  // namespace pic
