// ============================================================================
//  kernel/pmm.cpp —— 位图式物理页帧分配器
//  ---------------------------------------------------------------------------
//  位图放哪里？
//    位图本身也要占内存，而它的大小取决于「探测到多少物理内存」——
//    这是个鸡生蛋问题：不知道内存多大，就不知道位图多大。
//
//  解法：位图放在**内核镜像结束之后**的紧邻位置。
//    内核的结束地址是链接脚本算好的（_kernel_virt_end），
//    那之后的虚拟地址一定还没被使用，且这段虚拟地址对应的物理页
//    在 boot32.asm 建的映射里已经可访问了。
//    先探测内存总量 -> 算出位图大小 -> 就地占用从 _kernel_virt_end 开始的一段。
// ============================================================================

#include <kernel/pmm.hpp>
#include <kernel/multiboot2.hpp>
#include <kernel/printf.hpp>

constexpr u64 KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ull;

// 链接脚本导出的内核结束虚拟地址
extern u8 _kernel_virt_end[];

// 整个镜像（含 .user_text）的物理结束地址。
// ⚠️ 必须用它来算"已占用到哪"，不能用 _kernel_virt_end 换算 ——
//    那会漏掉 .user_text，导致位图盖住用户态服务代码。
// 注意：这是**变量**（值存在内存里），不是 ABS 符号。
// 直接 extern u64 _image_phys_end 会让编译器生成 PC32 相对寻址，
// 物理地址(0x233000) 与内核代码(0xffffffff802xxxxx) 相距过远，链接失败。
extern u64 _image_phys_end_store;

namespace {

// ---------------------------------------------------------------------------
//  PMM 状态
// ---------------------------------------------------------------------------
u64* g_bitmap = nullptr;        // 位图（每个 bit 对应一个页帧）
u64  g_frame_count = 0;         // 管理的页帧总数（= 最高物理地址 / 4KB）
u64  g_bitmap_words = 0;        // 位图的 u64 字数
u64  g_used_frames = 0;         // 已占用计数（用于快速算剩余，不必每次扫位图）
u64  g_managed_frames = 0;      // 「真正可分配」的页帧数（只统计 type=1 区域）
                                // 注意与 g_frame_count 区分：后者是位图覆盖范围，
                                // 包含了 MMIO 空洞、ACPI 保留区等不能碰的地址。
                                // 用 g_frame_count 算剩余会得出「已用 3.8GB」这种荒谬数字。

u64  g_total_bytes = 0;         // 探测到的可用 RAM 总量

// 物理地址 <-> 虚拟地址
inline void* phys_to_virt(u64 phys)
{
    return reinterpret_cast<void*>(phys + KERNEL_VIRT_BASE);
}
inline u64 virt_to_phys(const void* virt)
{
    return reinterpret_cast<u64>(virt) - KERNEL_VIRT_BASE;
}

// 位图操作：bit = 1 表示已占用
inline void set_bit(u64 index)   { g_bitmap[index / 64] |=  (1ull << (index % 64)); }
inline void clear_bit(u64 index) { g_bitmap[index / 64] &= ~(1ull << (index % 64)); }
inline bool test_bit(u64 index)  { return (g_bitmap[index / 64] >> (index % 64)) & 1; }

// 向上取整到页边界
inline u64 align_up(u64 addr)   { return (addr + pmm::PAGE_SIZE - 1) & ~(pmm::PAGE_SIZE - 1); }

// 通用对齐（align_up 是**按页**对齐的，位图只需要 16 字节对齐，
// 按页对齐会白白浪费空间，所以单独写一个）
inline u64 align_up_to(u64 addr, u64 align) {
    return (addr + align - 1) & ~(align - 1);
}
inline u64 align_down(u64 addr) { return addr & ~(pmm::PAGE_SIZE - 1); }

}  // namespace

namespace pmm {

void init(u64 info_phys)
{
    // --- 1. 先扫一遍内存映射，统计可用内存和最高物理地址 ---
    const Mb2MemoryMapTag* mmap =
        reinterpret_cast<const Mb2MemoryMapTag*>(
            mb2::find_tag(info_phys, MB2_TAG_MMAP));

    if (mmap == nullptr) {
        // 拿不到内存映射就等于瞎了眼，后面全没法做
        kprintf("[PMM] 致命错误：GRUB 未提供内存映射\n");
        return;
    }

    const u8* base = reinterpret_cast<const u8*>(mmap) + sizeof(Mb2MemoryMapTag);
    const u8* end  = reinterpret_cast<const u8*>(mmap) + mmap->size;

    u64 highest_addr = 0;
    g_total_bytes = 0;

    for (const u8* p = base; p + mmap->entry_size <= end; p += mmap->entry_size) {
        const Mb2MemoryMapEntry* e =
            reinterpret_cast<const Mb2MemoryMapEntry*>(p);

        if (e->type == 1) {                       // 可用 RAM
            g_total_bytes += e->length;
        }
        // 位图要覆盖到的最高地址：只取「可用 RAM」的顶端就够了。
        // 把 ACPI 保留区、MMIO 空洞也算进来会让位图无谓地膨胀
        // （实测 256MB 机器会算出 4GB 的覆盖范围，位图白白占 128KB）。
        if (e->type == 1) {
            u64 region_end = e->base_addr + e->length;
            if (region_end > highest_addr) {
                highest_addr = region_end;
            }
        }
    }

    // --- 2. 位图放在**整个镜像**之后 ---
    //
    // ⚠️ 关键：不能用 _kernel_virt_end！
    //   .user_text（用户态服务代码）的 LMA 紧跟在内核之后，
    //   从 _kernel_virt_end 开始放位图会把服务代码**擦成 0**。
    //   必须从 _image_phys_end（含 .user_text 的物理结束）之后开始。
    //
    //   【踩过的坑】症状是用户进程一运行就 #PF，CR2=0x0。
    //   页表、映射、CR3、特权级全都对，查了半天 ——
    //   实际是取到的指令字节被位图清零了，
    //   CPU 把 00 00 解码成 add [rax],al，于是访问地址 0。
    u64 bitmap_addr = align_up_to(_image_phys_end_store + KERNEL_VIRT_BASE, 16);

    // 需要管理多少个页帧 = 最高物理地址 / 4KB
    g_frame_count = highest_addr / PAGE_SIZE;
    g_bitmap_words = (g_frame_count + 63) / 64;
    u64 bitmap_bytes = g_bitmap_words * 8;

    g_bitmap = reinterpret_cast<u64*>(bitmap_addr);

    // 位图占用的物理区间，稍后要标记为已用
    u64 bitmap_phys_start = virt_to_phys(g_bitmap);
    u64 bitmap_phys_end   = bitmap_phys_start + bitmap_bytes;

    // --- 3. 初始状态：全部标记为「已占用」---
    // 保守起步更安全：只把 mmap 明确说可用的区间放开，
    // 其它（设备内存、保留区、ACPI）一律不允许分配。
    for (u64 i = 0; i < g_bitmap_words; ++i) {
        g_bitmap[i] = 0xFFFFFFFFFFFFFFFFull;
    }
    g_used_frames = 0;
    g_managed_frames = 0;

    // --- 4. 把 mmap 里 type=1 的区间标记为空闲 ---
    for (const u8* p = base; p + mmap->entry_size <= end; p += mmap->entry_size) {
        const Mb2MemoryMapEntry* e =
            reinterpret_cast<const Mb2MemoryMapEntry*>(p);

        if (e->type != 1) {
            continue;
        }

        // 第一页（物理地址 0）永远不放开：
        // 空指针解引用在 C++ 里是未定义行为，留着它能让这类 bug 立刻触发页错误
        u64 start = e->base_addr;
        if (start == 0) {
            start = PAGE_SIZE;
        }
        start = align_up(start);

        u64 stop = align_down(e->base_addr + e->length);

        for (u64 addr = start; addr < stop && addr / PAGE_SIZE < g_frame_count;
             addr += PAGE_SIZE) {
            u64 index = addr / PAGE_SIZE;
            if (test_bit(index)) {       // 防止重复计数
                clear_bit(index);
                ++g_managed_frames;
            }
        }
    }

    // --- 5. 把内核自己 + 位图占用的页标记为已用 ---
    // GRUB 把内核加载进了某个可用区间，但 mmap 不知道这件事，
    // 不手动保护的话，分配器会把内核自己的代码页分出去——灾难。
    // 内核物理范围：从 1MB（KERNEL_PHYS_BASE）到内核结束物理地址。
    // 保护范围要覆盖：内核正文 + .user_text + 位图自身
    u64 kernel_phys_end = align_up(bitmap_phys_end);

    for (u64 addr = align_down(0x100000);
         addr < kernel_phys_end && addr / PAGE_SIZE < g_frame_count;
         addr += PAGE_SIZE) {
        u64 index = addr / PAGE_SIZE;
        if (!test_bit(index)) {
            set_bit(index);
            ++g_used_frames;
        }
    }

    // 位图本身占用的页也要保护（它就在内核结束紧后面，通常已被上面覆盖，
    // 这里显式再做一次，逻辑更清楚）
    for (u64 addr = align_down(bitmap_phys_start);
         addr < align_up(bitmap_phys_end) && addr / PAGE_SIZE < g_frame_count;
         addr += PAGE_SIZE) {
        u64 index = addr / PAGE_SIZE;
        if (!test_bit(index)) {
            set_bit(index);
            ++g_used_frames;
        }
    }

    kprintf("[PMM] 初始化完成：可分配 %llu 个页帧（%llu MB）\n",
            g_managed_frames, g_total_bytes / (1024 * 1024));
    kprintf("[PMM] 位图位于 0x%llx，占用 %llu 字节\n",
            reinterpret_cast<u64>(g_bitmap), bitmap_bytes);
}

u64 alloc_frame()
{
    if (g_bitmap == nullptr) {
        return 0;
    }
    // 逐字扫描，找一个含 0 位的 u64
    for (u64 w = 0; w < g_bitmap_words; ++w) {
        if (g_bitmap[w] == 0xFFFFFFFFFFFFFFFFull) {
            continue;                       // 这一字全占满，跳过
        }
        // 找第一个为 0 的位
        for (u64 b = 0; b < 64; ++b) {
            u64 index = w * 64 + b;
            if (index >= g_frame_count) {
                return 0;
            }
            if (!test_bit(index)) {
                set_bit(index);
                ++g_used_frames;
                return index * PAGE_SIZE;
            }
        }
    }
    return 0;                               // 内存耗尽
}

u64 alloc_frames(u64 count)
{
    if (count == 0) {
        return 0;
    }
    if (count == 1) {
        return alloc_frame();
    }

    // 找连续 count 个空闲位。教学实现用朴素扫描，够清楚也够快
    // （真要优化可以做空闲链表或伙伴系统）
    u64 run_start = 0;
    u64 run_len = 0;

    for (u64 index = 0; index < g_frame_count; ++index) {
        if (!test_bit(index)) {
            if (run_len == 0) {
                run_start = index;
            }
            ++run_len;
            if (run_len == count) {
                for (u64 i = 0; i < count; ++i) {
                    set_bit(run_start + i);
                }
                g_used_frames += count;
                return run_start * PAGE_SIZE;
            }
        } else {
            run_len = 0;
        }
    }
    return 0;                               // 找不到这么长的连续区域
}

void free_frame(u64 addr)
{
    if (g_bitmap == nullptr || addr == 0) {
        return;
    }
    u64 index = addr / PAGE_SIZE;
    if (index >= g_frame_count) {
        return;
    }
    if (test_bit(index)) {                  // 只释放确实被占用的，避免重复计数
        clear_bit(index);
        --g_used_frames;
    }
}

void free_frames(u64 addr, u64 count)
{
    for (u64 i = 0; i < count; ++i) {
        free_frame(addr + i * PAGE_SIZE);
    }
}

void mark_used(u64 addr)
{
    u64 index = addr / PAGE_SIZE;
    if (index >= g_frame_count) {
        return;
    }
    if (!test_bit(index)) {
        set_bit(index);
        ++g_used_frames;
    }
}

u64 alloc_frame_below(u64 max_addr)
{
    u64 max_index = max_addr / PAGE_SIZE;
    if (max_index > g_frame_count) {
        max_index = g_frame_count;
    }
    for (u64 index = 0; index < max_index; ++index) {
        if (!test_bit(index)) {
            set_bit(index);
            ++g_used_frames;
            return index * PAGE_SIZE;
        }
    }
    return 0;
}

u64 total_memory() { return g_total_bytes; }
u64 frame_count()  { return g_managed_frames; }
u64 used_frames()  { return g_used_frames; }

u64 used_memory()  { return g_used_frames * PAGE_SIZE; }

u64 free_memory()
{
    if (g_managed_frames < g_used_frames) {
        return 0;
    }
    return (g_managed_frames - g_used_frames) * PAGE_SIZE;
}

}  // namespace pmm

const char* memory_type_name(u32 type)
{
    switch (type) {
        case 1:  return "可用内存";
        case 2:  return "ACPI 保留";
        case 3:  return "休眠保留";
        case 4:  return "坏内存";
        case 5:  return "ACPI NVS";
        default: return "未知类型";
    }
}
