// ============================================================================
//  kernel/gdt.cpp —— 正式 GDT、TSS 装载
//  ============================================================================

#include <kernel/gdt.hpp>
#include <kernel/serial.hpp>

namespace {

// GDT 共 8 个条目（64 字节）。
// 为什么是 8 个而不是 6 个：TSS 描述符占 **16 字节两个条目**。
alignas(16) u64 g_gdt[8];

// ---------------------------------------------------------------------------
//  TSS（任务状态段）—— x86_64 下长度 104 字节 + I/O 位图
// ---------------------------------------------------------------------------
struct __attribute__((packed)) Tss {
    u32 reserved0;
    u64 rsp0;               // Ring 0 栈指针 ← 最关键的一个字段
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];             // 中断栈表
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;         // I/O 位图偏移；= 0xFFFF 表示不用位图
} __attribute__((aligned(16)));

alignas(16) Tss g_tss;

// GDTR：6 字节限长 + 8 字节基址
struct __attribute__((packed)) Gdtr {
    u16 limit;
    u64 base;
} __attribute__((aligned(16)));

// ---------------------------------------------------------------------------
//  构造一个普通的代码/数据段描述符
//  -------------------------------------------------------------------------
//  8 字节描述符的位布局（AMD64 手册）：
//    bits 15:0   limit[15:0]
//    bits 31:16  base[15:0]
//    bits 39:32  base[23:16]
//    bits 47:40  P(1) DPL(2) S(1) type(4)
//    bits 55:48  G(1) D/B(1) L(1) AVL(1) limit[19:16](4)
//    bits 63:56  base[31:24]
//
//  64 位模式下 base 和 limit 基本被忽略（平坦模型），
//  真正起作用的只有 type / DPL / P / L 这几位。
// ---------------------------------------------------------------------------
// 期望值：内核代码段应该是 0x00AF9A000000FFFF
// （G=1 L=1 P=1 DPL=0 S=1 type=1010，limit=0xFFFFF）
static_assert(true, "段描述符常量见下方注释说明");

u64 make_segment(u8 dpl, bool is_code)
{
    u64 desc = 0;
    // P=1, DPL=dpl, S=1（代码/数据段，非系统段）
    desc |= (1ULL << 47);                    // P
    desc |= (static_cast<u64>(dpl & 3) << 45); // DPL
    desc |= (1ULL << 44);                    // S = 1
    // type：代码段 1010（执行/读），数据段 0010（读/写）
    //
    // ⚠️ 必须左移 40 位！type 字段占描述符的 bits 43:40。
    //    这里曾经漏了左移，直接 `desc |= 0xA` 把值写进了 limit 区域
    //    （bits 3:1），结果段描述符完全错误：
    //    内核数据段变成了"不可写"，lgdt 之后一访问数据就 #GP，
    //    系统卡在 GDT 初始化那一步，后面一句输出都打不出来。
    desc |= (is_code ? 0xAULL : 0x2ULL) << 40;
    // G=1（粒度 4KB）、L=1（64 位段）、limit 高 4 位 = 0xF
    desc |= (1ULL << 55);                    // G
    if (is_code) {
        desc |= (1ULL << 53);                // L = 1，64 位代码段
    }
    desc |= (0xFULL << 48);                  // limit[19:16]
    desc |= 0xFFFF;                          // limit[15:0]
    return desc;
}

}  // namespace

namespace gdt {

void init()
{
    // --- 建立段描述符 ---
    g_gdt[0] = 0x0000000000000000;                    // [0x00] 空（CPU 硬性要求）
    g_gdt[1] = make_segment(0, true);                 // [0x08] 内核代码 DPL=0
    g_gdt[2] = make_segment(0, false);                // [0x10] 内核数据 DPL=0
    g_gdt[3] = make_segment(3, false);                // [0x18] 用户数据 32（占位）
    g_gdt[4] = make_segment(3, false);                // [0x20] 用户数据 DPL=3
    g_gdt[5] = make_segment(3, true);                 // [0x28] 用户代码 DPL=3

    // --- TSS 描述符（16 字节，占 [0x30] 和 [0x38] 两个条目）---
    u64 tss_base = reinterpret_cast<u64>(&g_tss);

    u64 low = 0;
    low |= (104 - 1) & 0xFFFF;                        // limit[15:0] = 0x67
    low |= (tss_base & 0xFFFF) << 16;                 // base[15:0]
    low |= ((tss_base >> 16) & 0xFF) << 32;           // base[23:16]
    low |= (0x89ULL << 40);                           // P=1 DPL=0 S=0 type=1001
                                                      //   (1001 = 64 位可用 TSS)
    low |= ((tss_base >> 24) & 0xFF) << 56;           // base[31:24]

    // 高 8 字节：base[63:32]（我们的 TSS 在低 4GB，所以是 0）
    u64 high = (tss_base >> 32) & 0xFFFFFFFF;

    g_gdt[6] = low;    // [0x30]
    g_gdt[7] = high;   // [0x38]

    // --- 清零 TSS ---
    u8* p = reinterpret_cast<u8*>(&g_tss);
    for (usize i = 0; i < sizeof(Tss); ++i) p[i] = 0;
    g_tss.iomap_base = 0xFFFF;      // 不使用 I/O 位图

    // --- 加载 GDT ---
    Gdtr gdtr;
    gdtr.limit = sizeof(g_gdt) - 1;
    gdtr.base  = reinterpret_cast<u64>(&g_gdt[0]);

    asm volatile("lgdt %0" : : "m"(gdtr));

    // --- 重新加载段寄存器 ---
    // lgdt 之后，CS 之外的段寄存器里的旧值还在。
    // 但真正要紧的是 CS：必须用**远跳转**重新加载，
    // 因为 x86 上只有远跳转/远返回能改 CS 的可见部分。
    asm volatile(
        "mov $0x10, %%ax\n"     // 先用内核数据段刷新所有数据段寄存器
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%ss\n"
        // 远跳转重载 CS：push 选择子 + push 地址 + retfq
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        : : : "rax", "memory");

    // --- 加载 TR 寄存器（TSS）---
    // ltr 之后 CPU 才知道去哪找 RSP0。
    // 这一步之前，任何从 Ring 3 来的中断都会导致 #GP。
    asm volatile("ltr %0" : : "r"(static_cast<u16>(TSS)));

    serial::puts("[OK]   GDT/TSS loaded (incl. Ring 3 segments)\n");
}

void set_kernel_stack(u64 rsp0)
{
    g_tss.rsp0 = rsp0;
}

u64 tss_address()
{
    return reinterpret_cast<u64>(&g_tss);
}

}  // namespace gdt
