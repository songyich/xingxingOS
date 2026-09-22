// ============================================================================
//  kernel/framebuffer.cpp —— 帧缓冲的获取、映射与绘图
// ============================================================================

#include <kernel/framebuffer.hpp>
#include <kernel/multiboot2.hpp>

constexpr u64 KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ull;

// ---------------------------------------------------------------------------
//  boot_pml4 是 boot32.asm 里用 `global boot_pml4` 导出的符号，
//  链接在 .boot_pgtable 段（低地址 1MB 附近）。
//  注意：这条 extern 必须写在匿名 namespace **外面**——
//  写进去的话编译器会去找一个带命名空间修饰的符号，链接必然失败。
// ---------------------------------------------------------------------------
extern u8 boot_pml4[];

// 页表项的标志位（与 boot32.asm 里的定义一致）
constexpr u64 PAGE_PRESENT  = (1ull << 0);
constexpr u64 PAGE_WRITABLE = (1ull << 1);
constexpr u64 PAGE_PS       = (1ull << 7);   // PD 项：PS=1 表示 2MB 大页
constexpr u64 PAGE_SIZE_2M  = 0x200000;

// ---------------------------------------------------------------------------
//  页表需要的辅助页
//  映射一段 MMIO 需要：1 个 PDPT + 1 个 PD（PD 用 2MB 大页，一张就够覆盖 1GB）
//  放在 .bss 里由 boot64.asm 清零，alignas(4096) 保证页对齐（CR3 要求低 12 位干净）
// ---------------------------------------------------------------------------
namespace {
    alignas(4096) u64 g_mmio_pdpt[512];
    alignas(4096) u64 g_mmio_pd[512];

    fb::Info g_info;

    // 取高半区里的 PML4 数组地址（boot_pml4 的声明在文件顶部）
    u64* pml4_table()
    {
        return reinterpret_cast<u64*>(
            reinterpret_cast<u64>(boot_pml4) + KERNEL_VIRT_BASE);
    }

    // 内核数据都在高半区，虚拟地址减掉基址就是物理地址
    u64 virt_to_phys(const void* virt)
    {
        return reinterpret_cast<u64>(virt) - KERNEL_VIRT_BASE;
    }

    // PML4 下标 = (虚拟地址 >> 39) & 511
    constexpr u64 PML4_INDEX = 510;         // 对应 0xffffff0000000000

    // -----------------------------------------------------------------------
    //  map_mmio：把一段物理内存映射到 MMIO 虚拟区
    //  做法（2MB 大页，一级 PD 足够）：
    //    PML4[510] -> g_mmio_pdpt
    //    PDPT[0]   -> g_mmio_pd
    //    PD[i]     -> 物理地址第 i 个 2MB 页
    // -----------------------------------------------------------------------
    void* map_mmio(u64 phys, u64 size)
    {
        // 需要多少个 2MB 页（向上取整）
        u64 pages = (size + PAGE_SIZE_2M - 1) / PAGE_SIZE_2M;
        if (pages == 0)  pages = 1;
        if (pages > 512) pages = 512;      // 一张 PD 最多 512 项 = 1GB

        // 物理地址按 2MB 向下对齐（大页映射要求页首地址对齐）
        u64 phys_aligned = phys & ~(PAGE_SIZE_2M - 1);

        u64* pml4 = pml4_table();
        pml4[PML4_INDEX] = virt_to_phys(g_mmio_pdpt) | PAGE_PRESENT | PAGE_WRITABLE;
        g_mmio_pdpt[0]   = virt_to_phys(g_mmio_pd)   | PAGE_PRESENT | PAGE_WRITABLE;

        for (u64 i = 0; i < pages; ++i) {
            g_mmio_pd[i] = (phys_aligned + i * PAGE_SIZE_2M)
                         | PAGE_PRESENT | PAGE_WRITABLE | PAGE_PS;
        }

        // 映射后的虚拟地址 = MMIO 基址 + 物理地址在页内的偏移
        u64 offset_in_page = phys - phys_aligned;
        return reinterpret_cast<void*>(fb::MMIO_VIRT_BASE + offset_in_page);
    }
}  // namespace

// ---------------------------------------------------------------------------
//  渲染目标（离屏缓冲 / 真帧缓冲）
//  -------------------------------------------------------------------------
//  开机动画的"背景上移"需要先把界面画到一个**离屏缓冲**里，
//  加载完成后再把整块内容推上屏幕。
//
//  【为什么不用改其他模块】
//  全内核所有绘制（ASCII 字体、汉字、终端、鼠标指针）
//  最终都调用 fb::put_pixel / fill_rect / read_pixel，
//  没有任何一处直接访问 g_info.addr（已全局确认）。
//  所以只要这三个函数改读 g_target，切换渲染目标只需改一个指针，
//  终端 / 汉字 / 鼠标**一行都不用动**。
//
//  g_target == nullptr 表示渲染到真帧缓冲（g_info.addr）。
// ---------------------------------------------------------------------------
static u8* g_target = nullptr;

namespace fb {

void set_target(void* addr)
{
    g_target = static_cast<u8*>(addr);
}

void* target()
{
    return g_target;
}

// 实际要写入的基址
static inline u8* base()
{
    return (g_target != nullptr) ? g_target
                                 : reinterpret_cast<u8*>(g_info.addr);
}

void init(u64 info_phys)
{
    g_info.available  = false;
    g_info.addr       = nullptr;
    g_info.phys_addr  = 0;
    g_info.width      = 0;
    g_info.height     = 0;
    g_info.pitch      = 0;
    g_info.bpp        = 0;

    const Mb2FramebufferTag* tag = mb2::find_framebuffer(info_phys);
    if (tag == nullptr) {
        return;                                  // GRUB 没给帧缓冲（少见）
    }

    // type 2 是 EGA 文本模式：GRUB 没进入图形模式，没有帧缓冲可用
    if (tag->fb_type != MB2_FB_TYPE_RGB) {
        return;
    }

    // 只支持 32 / 24 位色深；16 位及以下先不处理（需要格式转换表）
    if (tag->bpp != 32 && tag->bpp != 24) {
        return;
    }

    u64 size = static_cast<u64>(tag->pitch) * tag->height;

    void* virt = map_mmio(tag->addr, size);

    g_info.available = true;
    g_info.addr      = static_cast<u32*>(virt);
    g_info.phys_addr = tag->addr;
    g_info.width     = tag->width;
    g_info.height    = tag->height;
    g_info.pitch     = tag->pitch;
    g_info.bpp       = tag->bpp;
}

const Info& info()
{
    return g_info;
}

void put_pixel(u32 x, u32 y, u32 color)
{
    if (!g_info.available || x >= g_info.width || y >= g_info.height) {
        return;                                  // 越界保护，别写到显存外面去
    }

    if (g_info.bpp == 32) {
        // 32bpp：一个像素正好 4 字节，按字节算偏移后写 u32
        u8* row = base() + static_cast<u64>(y) * g_info.pitch;
        reinterpret_cast<u32*>(row)[x] = color;
    } else {
        // 24bpp：一个像素 3 字节，内存中是 B、G、R 的顺序（小端）
        u8* pixel = base() + static_cast<u64>(y) * g_info.pitch
                  + static_cast<u64>(x) * 3;
        pixel[0] = static_cast<u8>(color & 0xFF);          // B
        pixel[1] = static_cast<u8>((color >> 8) & 0xFF);   // G
        pixel[2] = static_cast<u8>((color >> 16) & 0xFF);  // R
    }
}

u32 read_pixel(u32 x, u32 y)
{
    if (!g_info.available || x >= g_info.width || y >= g_info.height) {
        return 0;
    }

    const u8* b = base();
    if (g_info.bpp == 32) {
        const u8* row = b + static_cast<u64>(y) * g_info.pitch;
        return reinterpret_cast<const u32*>(row)[x];
    }

    // 24bpp：B、G、R 三字节拼回 0xRRGGBB
    const u8* p = b + static_cast<u64>(y) * g_info.pitch
                + static_cast<u64>(x) * 3;
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8)
         | (static_cast<u32>(p[2]) << 16);
}

void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color)
{
    // 逐像素填充。教学阶段不追求性能，清晰第一；
    // 真要优化可以按 32bpp 走 memset32，或者一次算好整行。
    for (u32 dy = 0; dy < h; ++dy) {
        for (u32 dx = 0; dx < w; ++dx) {
            put_pixel(x + dx, y + dy, color);
        }
    }
}

void clear(u32 color)
{
    if (!g_info.available) {
        return;
    }
    fill_rect(0, 0, g_info.width, g_info.height, color);
}

}  // namespace fb
