// ============================================================================
//  kernel/multiboot2.cpp —— 遍历 Multiboot2 标签
//  ---------------------------------------------------------------------------
//  遍历套路很固定：从 +8 开始，读一个标签，按 size 往后跳，直到遇到 type 0。
//  两个必须注意的细节：
//    1. size 可能含对齐补齐（规范要求每个标签按 8 字节对齐），所以跳的时候
//       要向上取整到 8 的倍数，否则会卡在半字节上读出一堆垃圾。
//    2. 地址必须转成高半区虚拟地址后再解引用。
// ============================================================================

#include <kernel/multiboot2.hpp>

// 高半区基址，与 linker.ld 和 boot32.asm 里的定义保持一致
constexpr u64 KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ull;

// 把物理地址翻译成内核可以访问的高半区虚拟地址
// 前提：物理地址 < 2GB（boot32.asm 只把 phys 0~2GB 映射到了高半区）
static inline const void* phys_to_virt(u64 phys)
{
    return reinterpret_cast<const void*>(phys + KERNEL_VIRT_BASE);
}

namespace mb2 {

const Mb2TagHeader* find_tag(u64 info_phys, u32 want_type)
{
    if (info_phys == 0) {
        return nullptr;
    }

    const u8* base = static_cast<const u8*>(phys_to_virt(info_phys));

    // 头 8 字节是 total_size + reserved，第一个标签从偏移 8 开始
    u32 offset = 8;

    for (;;) {
        const Mb2TagHeader* header =
            reinterpret_cast<const Mb2TagHeader*>(base + offset);

        // 结束标签：停止遍历
        if (header->type == MB2_TAG_END) {
            return nullptr;
        }

        if (header->type == want_type) {
            return header;
        }

        // size 为 0 是异常数据，直接退出以防死循环
        if (header->size == 0) {
            return nullptr;
        }

        // 向上取整到 8 的倍数： (x + 7) & ~7
        offset += (header->size + 7) & ~static_cast<u32>(7);
    }
}

const Mb2FramebufferTag* find_framebuffer(u64 info_phys)
{
    const Mb2TagHeader* tag = find_tag(info_phys, MB2_TAG_FRAMEBUFFER);
    if (tag == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<const Mb2FramebufferTag*>(tag);
}

}  // namespace mb2
