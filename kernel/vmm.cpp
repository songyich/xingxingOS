// ============================================================================
//  kernel/vmm.cpp —— 4 级页表的建立与维护
//  ---------------------------------------------------------------------------
//  关键设计：怎么读写页表？
//    页表项里存的是**物理地址**，但 CPU 开了分页之后我们只能用虚拟地址访问内存。
//    所以拿到页表项的物理地址后，必须翻译成虚拟地址才能改它的内容。
//
//  本内核的做法：物理地址 + 高半区基址 = 虚拟地址。
//    boot32.asm 把 phys 0~2GB 映射到了 0xffffffff80000000 起，
//    所以只要页表页的物理地址 < 2GB，就能用这个公式访问。
//    为此，申请页表层时统一用 pmm::alloc_frame_below(2GB)。
//
//  （更通用的做法是「递归映射」：让 PML4 的某一项指向 PML4 自己，
//    这样固定一段虚拟地址就能访问全部页表，不受 2GB 限制。
//    教学阶段先用简单方案，DEVLOG 里记了这个备选。）
// ============================================================================

#include <kernel/vmm.hpp>
#include <kernel/pmm.hpp>
#include <kernel/printf.hpp>

constexpr u64 KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ull;

// 页表层能用的最高物理地址（见文件头说明）
constexpr u64 PAGETABLE_MAX_PHYS = 0x80000000ull;   // 2GB

// PML4 由 boot32.asm 建立，链接在 .boot_pgtable 段（低地址）
extern u8 boot_pml4[];

namespace {

constexpr u64 ENTRIES_PER_TABLE = 512;
constexpr u64 ADDR_MASK = 0x000FFFFFFFFFF000ull;    // 页表项里存地址的位

// 取页表项里存的物理地址（清掉低 12 位的标志）
inline u64 entry_addr(u64 entry) { return entry & ADDR_MASK; }

// 物理地址 -> 可访问的虚拟地址
inline u64* table_virt(u64 phys)
{
    return reinterpret_cast<u64*>(phys + KERNEL_VIRT_BASE);
}

// 虚拟地址拆成 4 级下标
inline u64 pml4_index(u64 virt) { return (virt >> 39) & 0x1FF; }
inline u64 pdpt_index(u64 virt) { return (virt >> 30) & 0x1FF; }
inline u64 pd_index  (u64 virt) { return (virt >> 21) & 0x1FF; }
inline u64 pt_index  (u64 virt) { return (virt >> 12) & 0x1FF; }

u64 g_pml4_phys = 0;

// ---------------------------------------------------------------------------
//  get_or_create_table：取下一级表；不存在就申请一页建出来
//  -------------------------------------------------------------------------
//  entry_ptr 是当前层里那一「项」的地址（虚拟地址）。
//  返回下一级表的虚拟地址；失败返回 nullptr。
// ---------------------------------------------------------------------------
u64* get_or_create_table(u64* entry_ptr, u64 flags)
{
    u64 entry = *entry_ptr;

    // 已经存在：直接返回
    if (entry & vmm::PAGE_PRESENT) {
        return table_virt(entry_addr(entry));
    }

    // 不存在：申请一页物理内存当新表
    u64 new_phys = pmm::alloc_frame_below(PAGETABLE_MAX_PHYS);
    if (new_phys == 0) {
        kprintf("[VMM] 错误：无法为页表层分配物理页\n");
        return nullptr;
    }

    // 新表必须清零，否则里面的随机数据会被当成有效页表项
    u64* new_table = table_virt(new_phys);
    for (u64 i = 0; i < ENTRIES_PER_TABLE; ++i) {
        new_table[i] = 0;
    }

    // 填回上一层。注意：中间层一律给「可写 + 用户可访问」，
    // 权限的精细控制交给最末一级（PT）去做——
    // 因为 CPU 是逐层检查权限的，中间层不放开，末级再放开也没用。
    *entry_ptr = new_phys | vmm::PAGE_PRESENT | vmm::PAGE_WRITABLE | flags;

    return new_table;
}

}  // namespace

namespace vmm {

void init()
{
    // boot32.asm 已经建好了初始页表，我们接管它，不重建
    g_pml4_phys = reinterpret_cast<u64>(boot_pml4);
    kprintf("[VMM] 接管 boot 页表，PML4 物理地址 = 0x%llx\n", g_pml4_phys);
}

u64 pml4_phys()
{
    return g_pml4_phys;
}

bool map_page_in(u64 pml4_phys_in, u64 virt, u64 phys, u64 flags)
{
    if (pml4_phys_in == 0) {
        return false;
    }

    u64* pml4 = table_virt(pml4_phys_in);
    u64* pdpt = get_or_create_table(&pml4[pml4_index(virt)], PAGE_USER);
    if (pdpt == nullptr) return false;

    u64* pd = get_or_create_table(&pdpt[pdpt_index(virt)], PAGE_USER);
    if (pd == nullptr) return false;

    // 2MB 大页拆分（阶段 7 微内核改造补上）
    //
    // 为什么必须支持：boot32.asm 给内核正文建的是 2MB 大页映射
    // （0xffffffff80200000 起）。而用户态进程需要 4KB 粒度的页——
    // 每个进程独立的栈、代码段都要按 4KB 精细控制权限。
    // 阶段 4 时靠「把堆放到完全空闲的 PML4[509]」绕开了这个问题，
    // 但用户进程地址绕不开，必须正视。
    //
    // PD 项里 PS=1 时，存的是「2MB 页的物理基址」而不是下一级页表地址。
    // 直接拿它当页表用，会写到完全不相干的内存里去——这是灾难性的。
    //
    // 拆分步骤：
    //   1. 取出 2MB 大页的物理基址（2MB 对齐）和原有权限位
    //   2. 申请一个新 PT（4KB），填 512 项，每项指向基址 + i*4KB
    //   3. PD 项改指向新 PT，清掉 PS 位
    //   4. 刷新 TLB（旧的大页映射还缓存在 TLB 里）
    if (pd[pd_index(virt)] & PAGE_PS) {
        u64 old_entry = pd[pd_index(virt)];

        // 2MB 大页的物理基址：低 21 位是页内偏移，要清掉
        u64 huge_base = old_entry & ADDR_MASK & ~(u64)0x1FFFFFull;
        // 保留原有权限位，但去掉 PS（页大小）标志
        u64 old_flags = old_entry & 0xFFFull & ~PAGE_PS;

        u64 pt_phys = pmm::alloc_frame_below(PAGETABLE_MAX_PHYS);
        if (pt_phys == 0) {
            kprintf("[VMM] 错误：拆分大页时无法分配页表\n");
            return false;
        }

        u64* new_pt = table_virt(pt_phys);

        // 把原本一整块 2MB 的映射，拆成 512 个连续的 4KB 映射
        for (u64 i = 0; i < ENTRIES_PER_TABLE; ++i) {
            new_pt[i] = (huge_base + i * PAGE_SIZE) | old_flags;
        }

        // PD 项改为指向新页表，清 PS 位，并且要保证可写
        // （中间层不放开写权限，末级再放开也没用——CPU 逐层检查）
        pd[pd_index(virt)] = (pt_phys & ADDR_MASK)
                           | (old_flags | PAGE_WRITABLE | PAGE_PRESENT);

        // 关键：整块 2MB 的 TLB 都要刷。
        // 只刷单页的话，其余 511 个页的旧大页映射还在 TLB 里，
        // 之后访问会命中错误的旧映射。
        u64 base = virt & ~(u64)0x1FFFFFull;
        for (u64 i = 0; i < ENTRIES_PER_TABLE; ++i) {
            flush_tlb(base + i * PAGE_SIZE);
        }
        // 注意：这里**不能** return，要继续往下走，
        // 在新 PT 里填调用者真正要映射的那一页
    }

    u64* pt = get_or_create_table(&pd[pd_index(virt)], PAGE_USER);
    if (pt == nullptr) return false;

    // 最末一级：真正放上物理地址和调用者要的权限
    pt[pt_index(virt)] = (phys & ADDR_MASK) | flags;

    flush_tlb(virt);
    return true;
}

bool map_page(u64 virt, u64 phys, u64 flags)
{
    return map_page_in(g_pml4_phys, virt, phys, flags);
}

bool map_pages_in(u64 pml4_phys_in, u64 virt, u64 phys, u64 count, u64 flags)
{
    for (u64 i = 0; i < count; ++i) {
        if (!map_page_in(pml4_phys_in,
                         virt + i * PAGE_SIZE,
                         phys + i * PAGE_SIZE, flags)) {
            return false;
        }
    }
    return true;
}

bool map_pages(u64 virt, u64 phys, u64 count, u64 flags)
{
    for (u64 i = 0; i < count; ++i) {
        if (!map_page(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags)) {
            return false;
        }
    }
    return true;
}

void unmap_page(u64 virt)
{
    if (g_pml4_phys == 0) {
        return;
    }

    u64* pml4 = table_virt(g_pml4_phys);
    u64 pml4e = pml4[pml4_index(virt)];
    if (!(pml4e & PAGE_PRESENT)) return;

    u64* pdpt = table_virt(entry_addr(pml4e));
    u64 pdpte = pdpt[pdpt_index(virt)];
    if (!(pdpte & PAGE_PRESENT)) return;

    // 1GB 大页：清 PDPT 项
    if (pdpte & PAGE_PS) {
        pdpt[pdpt_index(virt)] = 0;
        flush_tlb(virt);
        return;
    }

    u64* pd = table_virt(entry_addr(pdpte));
    u64 pde = pd[pd_index(virt)];
    if (!(pde & PAGE_PRESENT)) return;

    // 2MB 大页：清 PD 项（PS 位在 PD 项上）
    if (pde & PAGE_PS) {
        pd[pd_index(virt)] = 0;
        flush_tlb(virt);
        return;
    }

    u64* pt = table_virt(entry_addr(pde));
    pt[pt_index(virt)] = 0;

    flush_tlb(virt);
}

// ---------------------------------------------------------------------------
//  按**指定页表**翻译虚拟地址
//  -------------------------------------------------------------------------
//  为什么需要它？
//    get_phys() 固定用内核自己的 PML4（g_pml4_phys），
//    而用户进程各有各的页表 —— 内核页表里根本没有用户进程的映射。
//    所以翻译用户指针（比如 syscall 传进来的字符串地址）
//    必须用**当前进程的**页表，也就是 CR3。
// ---------------------------------------------------------------------------
u64 get_phys_in(u64 pml4_phys, u64 virt)
{
    if (pml4_phys == 0) {
        return 0;
    }

    u64* pml4 = table_virt(pml4_phys);
    u64 pml4e = pml4[pml4_index(virt)];
    if (!(pml4e & PAGE_PRESENT)) return 0;

    u64* pdpt = table_virt(entry_addr(pml4e));
    u64 pdpte = pdpt[pdpt_index(virt)];
    if (!(pdpte & PAGE_PRESENT)) return 0;

    // 1GB 大页：PS 位在 PDPT 项上，页内偏移是低 30 位
    if (pdpte & PAGE_PS) {
        return entry_addr(pdpte) + (virt & 0x3FFFFFFFull);
    }

    u64* pd = table_virt(entry_addr(pdpte));
    u64 pde = pd[pd_index(virt)];
    if (!(pde & PAGE_PRESENT)) return 0;

    // 2MB 大页：PS 位在 **PD 项** 上（不是 PDPT 项！这是最容易搞混的地方）。
    // boot32.asm 给内核建的初始映射用的就是 2MB 大页，
    // 如果这里漏判，会把 2MB 页的物理基址当成「下一级页表的地址」去解引用，
    // 于是翻译结果完全是垃圾（实测表现为 get_phys 返回 0）。
    if (pde & PAGE_PS) {
        return entry_addr(pde) + (virt & 0x1FFFFFull);
    }

    u64* pt = table_virt(entry_addr(pde));
    u64 pte = pt[pt_index(virt)];
    if (!(pte & PAGE_PRESENT)) return 0;

    return entry_addr(pte) + (virt & (PAGE_SIZE - 1));
}

// 用内核自己的页表翻译（老行为，保持不变）
u64 get_phys(u64 virt)
{
    return get_phys_in(g_pml4_phys, virt);
}

// ---------------------------------------------------------------------------
//  用**当前 CPU 的 CR3** 翻译
//  -------------------------------------------------------------------------
//  进入 syscall / 中断时，CR3 仍是**被抢占的那个进程**的页表，
//  所以用它翻译用户指针才是正确的。
// ---------------------------------------------------------------------------
u64 get_phys_current(u64 virt)
{
    u64 cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return get_phys_in(cr3, virt);
}

bool is_mapped(u64 virt)
{
    return get_phys(virt) != 0;
}

bool set_flags(u64 virt, u64 flags)
{
    if (g_pml4_phys == 0) {
        return false;
    }

    u64* pml4 = table_virt(g_pml4_phys);
    u64 pml4e = pml4[pml4_index(virt)];
    if (!(pml4e & PAGE_PRESENT)) return false;

    u64* pdpt = table_virt(entry_addr(pml4e));
    u64 pdpte = pdpt[pdpt_index(virt)];
    if (!(pdpte & PAGE_PRESENT)) return false;

    u64* pd = table_virt(entry_addr(pdpte));
    u64 pde = pd[pd_index(virt)];
    if (!(pde & PAGE_PRESENT)) return false;

    u64* pt = table_virt(entry_addr(pde));
    u64 index = pt_index(virt);
    if (!(pt[index] & PAGE_PRESENT)) return false;

    // 保留物理地址，只改标志位
    pt[index] = entry_addr(pt[index]) | flags;

    flush_tlb(virt);
    return true;
}

void flush_tlb(u64 virt)
{
    // invlpg：让 CPU 丢弃这个虚拟地址的缓存翻译
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void flush_tlb_all()
{
    // 重新加载 CR3 会清空整个 TLB（除了标记为 global 的项）
    u64 cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

}  // namespace vmm
