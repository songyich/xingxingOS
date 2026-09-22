// ============================================================================
//  include/kernel/io.h —— x86 端口 I/O 内联封装
//  ---------------------------------------------------------------------------
//  x86 上除了内存，还有一套独立的「I/O 端口」地址空间，用 in/out 指令访问。
//  串口、PIC、PIT、PS/2 键盘控制器都挂在这上面。
//  注意 volatile 是必须的：编译器不知道读端口有副作用（比如读一次就清中断），
//  不加 volatile 会被优化掉。
// ============================================================================
#pragma once

#include <kernel/types.h>

// 向 port 写一个字节
static inline void outb(u16 port, u8 value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

// 从 port 读一个字节
static inline u8 inb(u16 port)
{
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

// 向 port 写一个 16 位字（阶段 2 初始化串口分频器时会用到）
static inline void outw(u16 port, u16 value)
{
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

// 短暂的 I/O 延时：往 0x80（POST 诊断口，写了没人管）写一个字节，
// 给老设备留出反应时间。没有它，某些机器上串口初始化会失败。
static inline u16 inw(u16 port)
{
    u16 value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(u16 port, u32 value)
{
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline u32 inl(u16 port)
{
    u32 value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait()
{
    outb(0x80, 0);
}
