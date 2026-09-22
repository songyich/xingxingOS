// ===========================================================================
//  kernel/initrd.cpp —— initrd 解析
//  ==========================================================================
//  文件格式（由 tools/mkinitrd.py 生成）：
//    magic 'XZS1' | count
//    Entry[name(32) + offset(4) + size(4)] * count
//    Data...
// ===========================================================================
#include <kernel/initrd.hpp>
#include <kernel/printf.hpp>

// 链接脚本嵌入的 initrd 边界符号
extern const u8 _initrd_start[];
extern const u8 _initrd_end[];

namespace initrd {

struct Entry {
    char name[32];
    u32  offset;
    u32  size;
};

static const u8*  g_base  = nullptr;
static u32        g_count = 0;
static bool       g_ready = false;

// 比较名字，允许带或不带 ".xzs" 后缀
static bool name_eq(const char* a, const char* b)
{
    for (int i = 0; i < 31; ++i) {
        char ca = a[i];
        char cb = b[i];
        // 忽略 .xzs 后缀
        if (ca == '.' ) ca = '\0';
        if (cb == '.' ) cb = '\0';
        if (ca == '\0' && cb == '\0') return true;
        if (ca != cb) return false;
    }
    return true;
}

void init()
{
    g_base = _initrd_start;
    u64 total = static_cast<u64>(_initrd_end - _initrd_start);

    if (total < 8) {
        kprintf("[initrd] 为空或未嵌入\n");
        return;
    }

    if (!(g_base[0] == 'X' && g_base[1] == 'Z'
          && g_base[2] == 'S' && g_base[3] == '1')) {
        kprintf("[initrd] 魔数不对\n");
        return;
    }

    g_count = *reinterpret_cast<const u32*>(g_base + 4);
    if (g_count > 64) g_count = 64;     // 防御

    g_ready = true;
    kprintf("[initrd] 就绪：%u 个程序\n", g_count);
}

bool ready() { return g_ready; }

int count() { return static_cast<int>(g_count); }

const Entry* entry_at(int i)
{
    if (!g_ready || i < 0 || i >= static_cast<int>(g_count)) return nullptr;
    return reinterpret_cast<const Entry*>(g_base + 8 + 40 * static_cast<u64>(i));
}

const char* name_of(int index)
{
    const Entry* e = entry_at(index);
    return (e != nullptr) ? e->name : nullptr;
}

bool find(const char* name, const void** out_data, u64* out_size)
{
    if (!g_ready || name == nullptr) return false;

    for (u32 i = 0; i < g_count; ++i) {
        const Entry* e = entry_at(static_cast<int>(i));
        if (e == nullptr) continue;

        if (name_eq(e->name, name)) {
            if (out_data) *out_data = g_base + e->offset;
            if (out_size) *out_size = e->size;
            return true;
        }
    }
    return false;
}

}   // namespace initrd
