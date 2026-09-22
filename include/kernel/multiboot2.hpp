// ============================================================================
//  include/kernel/multiboot2.hpp —— 解析 GRUB 递过来的 Multiboot2 信息结构
//  ---------------------------------------------------------------------------
//  什么是 Multiboot2 信息结构？
//    GRUB 把「内核需要了解的机器信息」打包成一块内存，地址放在 ebx 里交给我们。
//    它是一串**标签（tag）**首尾相接组成的，每个标签自带类型和长度，
//    类型不认识就跳过——这样协议可以向后兼容，新版本加新标签老内核也不怕。
//
//  结构总览：
//    +0   u32 total_size    整块信息的总字节数
//    +4   u32 reserved      保留（恒为 0）
//    +8   第一个 tag ...
//    每个 tag：u32 type / u32 size / 数据（size 含头部，且按 8 字节对齐补齐）
//
//  本阶段关心的两个标签：
//    type 8  = framebuffer（帧缓冲：图形模式下屏幕显存的地址、宽高、每行字节数）
//    type 6  = memory map（阶段 4 才用，这里先认个门牌号）
//
//  重要：这些信息是**物理地址**，内核跑在高半区，用之前要转成高半区虚拟地址
//  （低 2GB 的物理地址可以直接 +0xffffffff80000000，因为 boot32.asm 已经
//  把 phys 0~2GB 映射到那里了）。
// ============================================================================
#pragma once

#include <kernel/types.h>

// --- 标签类型常量 ---
constexpr u32 MB2_TAG_END         = 0;    // 结束标签，遇到它就要停止遍历
constexpr u32 MB2_TAG_MMAP        = 6;    // 内存映射
constexpr u32 MB2_TAG_FRAMEBUFFER = 8;    // 帧缓冲

// --- 帧缓冲的类型（framebuffer_type 字段）---
constexpr u8  MB2_FB_TYPE_INDEXED = 0;    // 调色板模式
constexpr u8  MB2_FB_TYPE_RGB     = 1;    // 直接 RGB 颜色（绝大多数情况）
constexpr u8  MB2_FB_TYPE_TEXT    = 2;    // EGA 文本模式（没有图形帧缓冲）

// ---------------------------------------------------------------------------
//  所有标签共用的头部
//  packed 是必须的：编译器默认会为了对齐在字段之间插空隙，
//  而这些结构体是 GRUB 按字节紧凑写好的，插了空隙就读错位了。
// ---------------------------------------------------------------------------
struct Mb2TagHeader {
    u32 type;
    u32 size;
} __attribute__((packed));

// ---------------------------------------------------------------------------
//  type 6：内存映射标签
//  -------------------------------------------------------------------------
//  BIOS/UEFI 告诉 GRUB「哪些物理地址区间能当内存用」，GRUB 转手交给我们。
//  这块信息是**唯一可信的物理内存信息来源**——不能靠猜，也不能用
//  「假设有 4GB」这种写法：不同机器差别极大，而且可用区间往往是**不连续**的
//  （设备内存、ACPI 表、保留区会把地址空间切得七零八落）。
//
//  每个 entry 描述一段区间：
//    type 1 = 可用 RAM（我们只能分配这种）
//    type 2 = ACPI 保留
//    type 3 = 需要保存供休眠用
//    type 4 = 坏内存
//    type 5 = ACPI NVS
//    其余保留
//
//  注意 entry_size 的作用：
//    协议允许每个 entry 后面带扩展字段，所以不能按 sizeof 硬算偏移，
//    必须用标签给出的 entry_size 跳到下一个。这是协议向后兼容的常规做法。
// ---------------------------------------------------------------------------
struct Mb2MemoryMapTag {
    u32 type;
    u32 size;
    u32 entry_size;                 // 单个 entry 的字节数（>= 24）
    u32 entry_version;              // 恒为 0
    // 后面紧跟若干个 entry
} __attribute__((packed));

// 单个内存区间描述（前 24 字节是协议保证的，后面可能有扩展字段）
struct Mb2MemoryMapEntry {
    u64 base_addr;                  // 区间起始物理地址
    u64 length;                     // 区间长度（字节）
    u32 type;                       // 1 = 可用 RAM
    u32 reserved;                   // 恒为 0
} __attribute__((packed));

// 内存区间类型的中文名，诊断输出用
const char* memory_type_name(u32 type);

// ---------------------------------------------------------------------------
//  type 8：帧缓冲标签
//  后面还有一段颜色通道描述（红/绿/蓝各占几位、在第几位），
//  32bpp 下通常是 R:16 G:8 B:0，这里先不解析，按 0xRRGGBB 直接画。
// ---------------------------------------------------------------------------
struct Mb2FramebufferTag {
    u32 type;
    u32 size;
    u64 addr;                       // 帧缓冲的**物理地址**
    u32 pitch;                      // 一行占多少字节（>= width * bpp/8）
    u32 width;                      // 像素宽
    u32 height;                     // 像素高
    u8  bpp;                        // 每像素位数（32 / 24 / 16...）
    u8  fb_type;                    // 见上面的 MB2_FB_TYPE_*
    u16 reserved;
} __attribute__((packed));

namespace mb2 {

// 在信息结构里找指定类型的标签；找不到返回 nullptr
const Mb2TagHeader* find_tag(u64 info_phys, u32 want_type);

// 便捷封装：取帧缓冲标签
const Mb2FramebufferTag* find_framebuffer(u64 info_phys);

}  // namespace mb2
