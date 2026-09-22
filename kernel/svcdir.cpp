// ===========================================================================
//  kernel/svcdir.cpp —— 服务目录实现
//  ==========================================================================
//  一张简单的 名字→tid 表。将来换成文件系统里的 /system/services 目录。
// ===========================================================================
#include <kernel/svcdir.hpp>
#include <kernel/printf.hpp>

namespace svcdir {

struct Slot {
    char name[NAME_MAX];
    int  tid;
    bool used;
};

static Slot g_slots[MAX_SERVICES];

void init()
{
    for (int i = 0; i < MAX_SERVICES; ++i) {
        g_slots[i].used = false;
        g_slots[i].tid = -1;
        for (int k = 0; k < NAME_MAX; ++k) g_slots[i].name[k] = '\0';
    }
}

static bool name_match(const char* a, const char* b, int len)
{
    for (int i = 0; i < len && i < NAME_MAX - 1; ++i) {
        char cb = b[i];
        if (cb == '\0') break;
        if (a[i] != cb) return false;
    }
    return true;
}

int register_service(const char* name, int len, int tid)
{
    if (name == nullptr || len <= 0) return -1;
    if (len >= NAME_MAX) len = NAME_MAX - 1;

    // 已存在则更新 tid（服务重启后 tid 会变）
    for (int i = 0; i < MAX_SERVICES; ++i) {
        if (g_slots[i].used && name_match(g_slots[i].name, name, len)) {
            g_slots[i].tid = tid;
            return 0;
        }
    }

    // 否则找空位
    for (int i = 0; i < MAX_SERVICES; ++i) {
        if (!g_slots[i].used) {
            for (int k = 0; k < len; ++k) g_slots[i].name[k] = name[k];
            g_slots[i].name[len] = '\0';
            g_slots[i].tid = tid;
            g_slots[i].used = true;
            // 【改走串口】6 个服务启动时各刷一行，放屏幕上很吵
            kprintf_serial("[svc] 注册 %s -> tid=%d\n", g_slots[i].name, tid);
            return 0;
        }
    }
    return -1;
}

int lookup(const char* name, int len)
{
    if (name == nullptr || len <= 0) return -1;

    for (int i = 0; i < MAX_SERVICES; ++i) {
        if (g_slots[i].used && name_match(g_slots[i].name, name, len)) {
            return g_slots[i].tid;
        }
    }
    return -1;
}

int count()
{
    int n = 0;
    for (int i = 0; i < MAX_SERVICES; ++i) if (g_slots[i].used) ++n;
    return n;
}

const char* name_at(int index, int* out_tid)
{
    int n = 0;
    for (int i = 0; i < MAX_SERVICES; ++i) {
        if (!g_slots[i].used) continue;
        if (n == index) {
            if (out_tid) *out_tid = g_slots[i].tid;
            return g_slots[i].name;
        }
        ++n;
    }
    return nullptr;
}

}   // namespace svcdir
