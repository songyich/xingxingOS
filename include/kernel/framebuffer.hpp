// ============================================================================
//  include/kernel/framebuffer.hpp —— 图形帧缓冲（framebuffer）
//  ---------------------------------------------------------------------------
//  什么是帧缓冲？
//    图形模式下，屏幕上的每一个像素对应内存里的若干字节。这块内存就叫帧缓冲。
//    想让屏幕上出现东西，只要往这块内存的对应位置写颜色值即可。
//
//  为什么要「映射」它？
//    GRUB 给我们的是帧缓冲的**物理地址**（比如 0xFD000000，接近 4GB）。
//    但内核只把物理内存的前 2GB 映射到了高半区（见 boot32.asm），
//    高半区总共只有 2GB 空间，塞不下 4GB 的物理地址。
//    所以这里单独给它开一片虚拟地址区间，临时建几张页表把它映射进来。
//
//  MMIO 映射区：0xffffff0000000000
//    PML4 的第 510 项（第 511 项是内核高半区，第 509 项及以上也空闲），
//    一整项覆盖 512GB，用来映射显存、PCI 设备寄存器这类「内存映射 I/O」正合适。
// ============================================================================
#pragma once

#include <kernel/types.h>

namespace fb {

// MMIO 映射区的起始虚拟地址（PML4[510]）
constexpr u64 MMIO_VIRT_BASE = 0xffffff0000000000ull;

// ---------------------------------------------------------------------------
//  帧缓冲状态
// ---------------------------------------------------------------------------
struct Info {
    bool     available;      // 是否拿到了可用的图形帧缓冲
    u32*     addr;           // 映射后的**虚拟**地址（可写）
    u64      phys_addr;      // GRUB 给的原始物理地址
    u32      width;          // 像素宽
    u32      height;         // 像素高
    u32      pitch;          // 一行占多少字节
    u8       bpp;            // 每像素位数
};

// 初始化：从 Multiboot2 信息里找帧缓冲标签，并把它映射进 MMIO 区
// 参数 info_phys = GRUB 递过来的 Multiboot2 信息结构物理地址
void init(u64 info_phys);

// 取当前帧缓冲信息（未初始化完时 available == false）
const Info& info();

// 在 (x, y) 画一个像素，color 是 0xRRGGBB
void put_pixel(u32 x, u32 y, u32 color);

// 读回 (x, y) 的像素颜色（鼠标指针要保存/恢复下面的内容）
u32 read_pixel(u32 x, u32 y);

// 用颜色填满一个矩形（画字符背景、清屏都要用）
void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);

// 清屏（填背景色）
void clear(u32 color);

// ---------------------------------------------------------------------------
//  渲染目标切换（开机动画用）
// -------------------------------------------------------------------------
//  set_target(addr) 后，所有绘制（含终端/汉字/鼠标）都写到 addr；
//  set_target(nullptr) 切回真帧缓冲。
//
//  这是"背景上移"动画的关键：加载期间把界面画到离屏缓冲，
//  加载完成后再逐帧把离屏内容推上屏幕。
// ---------------------------------------------------------------------------
void set_target(void* addr);
void* target();

// ---------------------------------------------------------------------------
//  常用颜色（0xRRGGBB）
//  后面 panic 蓝屏、终端配色都用这些常量
// ---------------------------------------------------------------------------
namespace color {
    constexpr u32 BLACK        = 0x000000;
    constexpr u32 BLUE         = 0x0000AA;   // 经典蓝屏底色
    constexpr u32 GREEN        = 0x00AA00;
    constexpr u32 CYAN         = 0x00AAAA;
    constexpr u32 RED          = 0xAA0000;
    constexpr u32 MAGENTA      = 0xAA00AA;
    constexpr u32 BROWN        = 0xAA5500;
    constexpr u32 LIGHT_GRAY   = 0xAAAAAA;
    constexpr u32 DARK_GRAY    = 0x555555;
    constexpr u32 LIGHT_BLUE   = 0x5555FF;
    constexpr u32 LIGHT_GREEN  = 0x55FF55;
    constexpr u32 LIGHT_CYAN   = 0x55FFFF;
    constexpr u32 LIGHT_RED    = 0xFF5555;
    constexpr u32 LIGHT_MAGENTA= 0xFF55FF;
    constexpr u32 YELLOW       = 0xFFFF55;
    constexpr u32 WHITE        = 0xFFFFFF;
}

}  // namespace fb
