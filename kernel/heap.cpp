// ============================================================================
//  kernel/heap.cpp —— 链表式内核堆
//  ---------------------------------------------------------------------------
//  内存布局（堆是一段连续的虚拟地址区间）：
//
//    HEAP_START
//    +----------------+---------------------------+--------------------+----
//    | BlockHeader    | 数据区（给用户用）        | BlockHeader        | ...
//    | size / free /  |                           |                    |
//    | prev / next    |                           |                    |
//    +----------------+---------------------------+--------------------+----
//    ^                ^                           ^
//    块头              kmalloc 返回的指针          下一个块头
//
//  kmalloc(p) 返回的是「块头之后」的地址；
//  kfree(p) 用 p - sizeof(BlockHeader) 反推回块头。
// ============================================================================

#include <kernel/heap.hpp>
#include <kernel/vmm.hpp>
#include <kernel/pmm.hpp>
#include <kernel/printf.hpp>

namespace {

// ---------------------------------------------------------------------------
//  块头
//  alignas(16) 保证 sizeof 是 16 的倍数，
//  这样「块头之后」的数据区天然 16 字节对齐——SSE 指令和不少结构体都要求这个。
// ---------------------------------------------------------------------------
constexpr u64 BLOCK_MAGIC = 0xDEADBEEFCAFEBABEull;

struct alignas(16) BlockHeader {
    u64         magic;      // 魔数，用来识别「这个指针到底是不是我们发的」
    u64         size;       // 数据区大小（不含块头）
    bool        free;       // 是否空闲
    BlockHeader* prev;      // 双向链表，方便释放时向前合并
    BlockHeader* next;
};

// 单个块占用的总空间（块头 + 数据区）
inline u64 block_total(u64 data_size)
{
    return sizeof(BlockHeader) + data_size;
}

// 数据区最小粒度：16 字节。
// 小于这个值的请求也按 16 给，避免切出无法再利用的碎渣。
constexpr u64 MIN_DATA_SIZE = 16;

inline u64 align_up_16(u64 n) { return (n + 15) & ~15ull; }

BlockHeader* g_first = nullptr;     // 链表头（地址最低的块）
BlockHeader* g_last  = nullptr;     // 链表尾（方便追加新块）
bool  g_ready = false;

u64 g_heap_end = 0;                 // 堆当前的虚拟地址末尾
u64 g_used = 0;                     // 已分配字节数（不含块头）
u64 g_total = 0;                    // 堆总大小
u32 g_blocks = 0;

// ---------------------------------------------------------------------------
//  expand_heap：向页分配器要更多内存，接在堆末尾
// ---------------------------------------------------------------------------
bool expand_heap(u64 pages)
{
    if (g_total + pages * vmm::PAGE_SIZE > heap::HEAP_MAX_SIZE) {
        kprintf("[HEAP] 已达上限，无法继续扩张\n");
        return false;
    }

    // 逐页申请物理内存并映射过去。
    // 注意：这里不要求物理页连续——虚拟地址连续就够了，
    // 这正是虚拟内存的价值所在。
    for (u64 i = 0; i < pages; ++i) {
        u64 phys = pmm::alloc_frame();
        if (phys == 0) {
            kprintf("[HEAP] 物理内存不足，扩张失败\n");
            return false;
        }
        u64 virt = g_heap_end + i * vmm::PAGE_SIZE;
        if (!vmm::map_page(virt, phys, vmm::FLAGS_KERNEL)) {
            kprintf("[HEAP] 映射失败\n");
            pmm::free_frame(phys);
            return false;
        }
    }

    u64 grow_bytes = pages * vmm::PAGE_SIZE;
    u64 old_end = g_heap_end;
    g_heap_end += grow_bytes;
    g_total += grow_bytes;

    // 把新增的这整段做成一个空闲块，挂到链表尾
    BlockHeader* nb = reinterpret_cast<BlockHeader*>(old_end);
    nb->magic = BLOCK_MAGIC;
    nb->size  = grow_bytes - sizeof(BlockHeader);
    nb->free  = true;
    nb->prev  = g_last;
    nb->next  = nullptr;

    if (g_last != nullptr) {
        g_last->next = nb;
    } else {
        g_first = nb;
    }
    g_last = nb;
    ++g_blocks;

    return true;
}

// 把块 b 切成「data_size」和「剩余」两块；剩余部分留在链表中
void split_block(BlockHeader* b, u64 data_size)
{
    // 剩余空间够不够再放一个块 + 最小数据区？不够就不切，整块给出
    u64 remaining = b->size - data_size;
    if (remaining < sizeof(BlockHeader) + MIN_DATA_SIZE) {
        return;
    }

    BlockHeader* nb = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<u64>(b) + sizeof(BlockHeader) + data_size);

    nb->magic = BLOCK_MAGIC;
    nb->size  = remaining - sizeof(BlockHeader);
    nb->free  = true;
    nb->prev  = b;
    nb->next  = b->next;

    if (b->next != nullptr) {
        b->next->prev = nb;
    } else {
        g_last = nb;
    }
    b->next = nb;
    b->size = data_size;

    ++g_blocks;
}

// 尝试把 b 和它后面的块合并（要求后块空闲且地址紧邻）
void merge_with_next(BlockHeader* b)
{
    BlockHeader* n = b->next;
    if (n == nullptr || !n->free) {
        return;
    }
    // 地址必须紧邻：b 的数据区结束处就是 n 的开头
    u64 expected = reinterpret_cast<u64>(b) + sizeof(BlockHeader) + b->size;
    if (reinterpret_cast<u64>(n) != expected) {
        return;                 // 中间有空洞（理论上不会），不合并
    }

    b->size += sizeof(BlockHeader) + n->size;
    b->next = n->next;
    if (n->next != nullptr) {
        n->next->prev = b;
    } else {
        g_last = b;
    }
    --g_blocks;
}

}  // namespace

namespace heap {

void init()
{
    g_first = nullptr;
    g_last = nullptr;
    g_heap_end = HEAP_START;
    g_used = 0;
    g_total = 0;
    g_blocks = 0;

    // 先要一小块作为起点
    if (!expand_heap(HEAP_EXPAND_PAGES)) {
        kprintf("[HEAP] 初始化失败\n");
        return;
    }
    g_ready = true;
    kprintf("[HEAP] 初始化完成：起始 0x%llx，初始 %llu KB\n",
            HEAP_START, g_total / 1024);
}

bool ready() { return g_ready; }

void* kmalloc(usize size)
{
    if (!g_ready || size == 0) {
        return nullptr;
    }

    u64 need = align_up_16(size);
    if (need < MIN_DATA_SIZE) {
        need = MIN_DATA_SIZE;
    }

    // 首次适配：从头扫，第一个够大的就用
    BlockHeader* cur = g_first;
    while (cur != nullptr) {
        if (cur->free && cur->size >= need) {
            split_block(cur, need);
            cur->free = false;
            g_used += cur->size;
            return reinterpret_cast<void*>(
                reinterpret_cast<u64>(cur) + sizeof(BlockHeader));
        }

        // 扫到尾还没找到 -> 扩堆，然后再从头找
        if (cur->next == nullptr) {
            if (!expand_heap(HEAP_EXPAND_PAGES)) {
                return nullptr;
            }
            cur = g_first;              // 重新扫描（新增的块在尾部）
            continue;
        }
        cur = cur->next;
    }

    return nullptr;
}

void kfree(void* ptr)
{
    if (ptr == nullptr || !g_ready) {
        return;
    }

    BlockHeader* b = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<u64>(ptr) - sizeof(BlockHeader));

    // 魔数校验：能挡住「重复释放」和「释放了不是我们发的指针」这两类常见错误
    if (b->magic != BLOCK_MAGIC) {
        kprintf("[HEAP] 警告：kfree 收到非法指针 0x%llx\n",
                reinterpret_cast<u64>(ptr));
        return;
    }
    if (b->free) {
        kprintf("[HEAP] 警告：重复释放 0x%llx\n", reinterpret_cast<u64>(ptr));
        return;
    }

    b->free = true;
    g_used -= b->size;

    // 先向后合并，再向前合并。
    // 顺序无所谓，两次合并能处理「前后都空闲」的情况。
    merge_with_next(b);
    if (b->prev != nullptr && b->prev->free) {
        merge_with_next(b->prev);
    }
}

void* kzalloc(usize size)
{
    void* p = kmalloc(size);
    if (p == nullptr) {
        return nullptr;
    }
    // 逐字节清零。堆内存来源混杂（可能是回收的旧块），不清零会读到脏数据
    u8* bytes = static_cast<u8*>(p);
    for (usize i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
    return p;
}

void* krealloc(void* ptr, usize new_size)
{
    if (ptr == nullptr) {
        return kmalloc(new_size);
    }
    if (new_size == 0) {
        kfree(ptr);
        return nullptr;
    }

    BlockHeader* b = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<u64>(ptr) - sizeof(BlockHeader));
    if (b->magic != BLOCK_MAGIC) {
        return nullptr;
    }

    u64 need = align_up_16(new_size);
    if (need < MIN_DATA_SIZE) {
        need = MIN_DATA_SIZE;
    }

    // 原地够用：直接改大小（可能顺手切一刀回收多余部分）
    if (b->size >= need) {
        g_used -= b->size;
        split_block(b, need);
        b->free = false;
        g_used += b->size;
        return ptr;
    }

    // 不够：开新块 + 拷数据 + 释放旧的
    void* np = kmalloc(new_size);
    if (np == nullptr) {
        return nullptr;
    }
    u8* dst = static_cast<u8*>(np);
    u8* src = static_cast<u8*>(ptr);
    for (u64 i = 0; i < b->size; ++i) {
        dst[i] = src[i];
    }
    kfree(ptr);
    return np;
}

u64 used_bytes()  { return g_used; }
u64 total_bytes() { return g_total; }
u64 free_bytes()  { return g_total - g_used; }
u32 block_count() { return g_blocks; }

void dump()
{
    kprintf("[HEAP] 总大小 %llu 字节，已用 %llu，空闲 %llu，块数 %llu\n",
            g_total, g_used, free_bytes(), static_cast<u64>(g_blocks));

    u64 index = 0;
    for (BlockHeader* b = g_first; b != nullptr; b = b->next) {
        kprintf("   块 %llu : 头 0x%llx  数据 %llu 字节  %s\n",
                index, reinterpret_cast<u64>(b), b->size,
                b->free ? "空闲" : "已用");
        ++index;
        if (index > 20) {           // 别刷屏
            kprintf("   ...（已省略）\n");
            break;
        }
    }
}

}  // namespace heap
