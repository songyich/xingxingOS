// ===========================================================================
//  kernel/elf.cpp —— ELF64 加载器实现
//  ==========================================================================
//  【万物皆可程序】的地基：让系统能加载运行外部 .xzs 程序。
//
//  加载流程（每个 PT_LOAD 段）：
//    1. 把 vaddr 向下对齐到页，vaddr+memsz 向上对齐到页，算出需要几页
//    2. 用 PMM 分配物理页帧
//    3. 在**目标页表**里建立映射（权限由段的 flags 决定）
//    4. 先把整段清零（覆盖 .bss —— 文件里不存内容，但运行时要当 0）
//    5. 再把 filesz 字节从文件里拷进去
//
//  ⚠️ 关键：写入内容时**不能**用虚拟地址直接写。
//     我们此刻跑在内核页表下，目标页表里的用户虚拟地址在这里无效。
//     正确做法是通过「物理地址 + 高半区偏移」访问刚分配的页：
//        物理地址 + 0xFFFFFFFF80000000
//     （阶段 1 已把整个物理内存映射到了高半区）
// ===========================================================================
#include <kernel/elf.hpp>
#include <kernel/pmm.hpp>
#include <kernel/vmm.hpp>
#include <kernel/printf.hpp>

namespace elf {

// 高半区偏移：物理地址 + 这个偏移 = 内核可直接访问的虚拟地址
static constexpr u64 HHDM = 0xFFFFFFFF80000000ull;

// 向下/向上按页对齐
static inline u64 page_down(u64 addr) { return addr & ~(vmm::PAGE_SIZE - 1); }
static inline u64 page_up(u64 addr)
{
    return (addr + vmm::PAGE_SIZE - 1) & ~(vmm::PAGE_SIZE - 1);
}

bool validate(const void* image, u64 len, const char** out_reason)
{
    auto fail = [&](const char* why) -> bool {
        if (out_reason) *out_reason = why;
        return false;
    };

    if (image == nullptr) return fail("映像指针为空");
    if (len < sizeof(Header)) return fail("文件太小，连 ELF 头都不够");

    const Header* h = reinterpret_cast<const Header*>(image);

    // 魔数 \x7f E L F
    if (!(h->ident[0] == 0x7f && h->ident[1] == 'E'
          && h->ident[2] == 'L' && h->ident[3] == 'F')) {
        return fail("不是 ELF 文件（魔数不对）");
    }

    // EI_CLASS = 2 表示 64 位
    if (h->ident[4] != 2) return fail("不是 64 位 ELF");

    // EI_DATA = 1 表示小端
    if (h->ident[5] != 1) return fail("不是小端 ELF");

    // EM_X86_64 = 0x3E
    if (h->machine != 0x3E) return fail("不是 x86-64 架构");

    // ET_EXEC = 2（静态可执行文件）。ET_DYN=3 是 PIE/动态库，暂不支持
    if (h->type != 2) return fail("不是静态可执行文件（暂不支持 PIE / 动态库）");

    if (h->phnum == 0) return fail("没有程序头（无法加载任何段）");

    // 程序头表不能超出文件
    u64 pht_end = h->phoff + static_cast<u64>(h->phnum) * h->phentsize;
    if (pht_end > len) return fail("程序头表超出文件范围");

    return true;
}

bool load(const void* image, u64 len, u64 pml4, u64* out_entry, u64* out_end)
{
    const char* reason = nullptr;
    if (!validate(image, len, &reason)) {
        kprintf("[ELF] 校验失败：%s\n", reason ? reason : "未知");
        return false;
    }

    const Header* h = reinterpret_cast<const Header*>(image);
    const u8* base = reinterpret_cast<const u8*>(image);

    u64 max_end = 0;

    for (u16 i = 0; i < h->phnum; ++i) {
        const Phdr* ph = reinterpret_cast<const Phdr*>(
            base + h->phoff + static_cast<u64>(i) * h->phentsize);

        if (ph->type != PT_LOAD) continue;      // 只处理需要加载的段

        // 段在文件里的内容不能超出文件
        if (ph->offset + ph->filesz > len) {
            kprintf("[ELF] 段 %u 超出文件范围\n", i);
            return false;
        }

        // memsz 至少要能装下 filesz
        if (ph->memsz < ph->filesz) {
            kprintf("[ELF] 段 %u 的 memsz < filesz\n", i);
            return false;
        }

        if (ph->memsz == 0) continue;           // 空段，跳过

        // --- 算出要映射的页范围 ---
        u64 vstart = page_down(ph->vaddr);
        u64 vend   = page_up(ph->vaddr + ph->memsz);
        if (vend <= vstart) continue;

        u64 npages = (vend - vstart) / vmm::PAGE_SIZE;

        // 简单安全检查：必须落在用户空间（不能映射到内核高半区）
        if (vstart >= 0x0000800000000000ull) {
            kprintf("[ELF] 段 %u 的虚拟地址 0x%llx 不在用户空间\n", i, vstart);
            return false;
        }

        // --- 分配物理页 ---
        u64 phys = pmm::alloc_frames(npages);
        if (phys == 0) {
            kprintf("[ELF] 段 %u 分配 %llu 页失败\n", i, npages);
            return false;
        }

        // --- 权限：默认可读；可写看 PF_W；不可执行则加 NX ---
        u64 flags = vmm::PAGE_PRESENT | vmm::PAGE_USER;
        if (ph->flags & PF_W) flags |= vmm::PAGE_WRITABLE;
        if (!(ph->flags & PF_X)) flags |= vmm::PAGE_NX;

        if (ph->flags & PF_R) { /* 读权限在 x86 页表里隐含于 PRESENT */ }

        // --- 在目标页表里建立映射 ---
        //
        // ⚠️ 必须待在内核页表下操作（不能先切 CR3）。
        //    vmm 内部要通过高半区窗口访问页表页的物理地址，
        //    切到新页表后那个窗口未必还在，访问就直接 #PF。
        if (!vmm::map_pages_in(pml4, vstart, phys, npages, flags)) {
            kprintf("[ELF] 段 %u 映射失败 vaddr=0x%llx\n", i, vstart);
            return false;
        }

        // --- 通过高半区窗口访问刚分配的页 ---
        u8* dst = reinterpret_cast<u8*>(phys + HHDM);

        // 整段清零：这样 .bss（memsz > filesz 的那部分）自然是 0
        for (u64 b = 0; b < npages * vmm::PAGE_SIZE; ++b) dst[b] = 0;

        // 拷贝文件里的内容
        //
        // 注意目标偏移：段内容要放到 vaddr 对应的位置，
        // 而 vaddr 可能不是页对齐的（比如 0x400000 对齐，但 0x401234 不是）
        u64 off_in_page = ph->vaddr - vstart;
        for (u64 b = 0; b < ph->filesz; ++b) {
            dst[off_in_page + b] = base[ph->offset + b];
        }

        if (vend > max_end) max_end = vend;
    }

    if (out_entry) *out_entry = h->entry;
    if (out_end)   *out_end   = max_end;

    return true;
}

}   // namespace elf
