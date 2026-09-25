// ============================================================================
//  kernel/supervisor.cpp —— 服务监管（崩溃自愈）
//  ---------------------------------------------------------------------------
//  微内核的兑现点：服务崩了，系统不跟着死，而且服务能自动回来。
//
//  三条职责：
//    1. 登记服务（tid + 名字 + 入口 + 优先级）
//    2. 收到"某服务死了"的通知 → 标记待重启
//    3. 在安全的上下文里把它重新拉起来
// ============================================================================

#include <kernel/supervisor.hpp>
#include <kernel/usermode.hpp>
#include <kernel/thread.hpp>
#include <kernel/ipc.hpp>
#include <kernel/printf.hpp>
#include <kernel/serial.hpp>
#include <kernel/pit.hpp>

namespace {

// ---------------------------------------------------------------------------
//  被看护服务的登记表
// ---------------------------------------------------------------------------
struct Watched {
    int      tid;                       // 服务的 tid（重启时复用）
    char     name[32];
    u64      entry_virt;                // 重启用的入口地址
    int      priority;
    bool     pending;                   // 待重启
    bool     used;
    int      restarts;                  // 已重启次数
    u64      dead_at_ms;                // 死亡时刻（延迟重启用）
};

Watched g_watched[supervisor::MAX_WATCHED];
int     g_count = 0;

// 重启延迟：死掉之后稍等一下再拉起。
// 立刻拉起的话，如果服务是"一启动就崩"的死循环，
// 会在极短时间内反复崩，日志瞬间刷满。延迟让节奏可控。
constexpr u64 RESTART_DELAY_MS = 200;

Watched* find_by_tid(int tid)
{
    for (int i = 0; i < supervisor::MAX_WATCHED; ++i) {
        if (g_watched[i].used && g_watched[i].tid == tid) {
            return &g_watched[i];
        }
    }
    return nullptr;
}

// 复制字符串（内核里没有 strcpy）
void copy_name(char* dst, const char* src)
{
    int i = 0;
    while (src != nullptr && src[i] != '\0' && i < 31) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

bool name_eq(const char* a, const char* b)
{
    int i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return false;
        ++i;
    }
    return a[i] == b[i];
}

// ---------------------------------------------------------------------------
//  清掉服务"上辈子"留在 IPC 共享页里的状态
//  -------------------------------------------------------------------------
//  共享页是所有进程共用的，服务死了它写过的东西还在。
//  不清的话重启后的服务会读到旧状态：
//    键盘服务：环形队列 head/tail 不一致 → 上来就吐出一堆陈年按键
//              PEND 里存着已消失的请求方 tid → 回复给空气
// ---------------------------------------------------------------------------
void reset_service_shared_state(const char* name)
{
    u64 page = usermode::ipc_page_virt();
    if (page == 0) return;

    if (name_eq(name, "keyboard")) {
        u64* head = reinterpret_cast<u64*>(page + (IPC_KBD_HEAD - USER_IPC_PAGE));
        u64* tail = reinterpret_cast<u64*>(page + (IPC_KBD_TAIL - USER_IPC_PAGE));
        u64* pend = reinterpret_cast<u64*>(page + (IPC_KBD_PEND - USER_IPC_PAGE));
        u64* st   = reinterpret_cast<u64*>(page + (IPC_KBD_STATE - USER_IPC_PAGE));
        u64* ext  = reinterpret_cast<u64*>(page + (IPC_KBD_EXT   - USER_IPC_PAGE));
        *head = 0;
        *tail = 0;
        *pend = 0;
        *st   = 0;
        *ext  = 0;
    }
}

}  // namespace

namespace supervisor {

void init()
{
    for (int i = 0; i < MAX_WATCHED; ++i) {
        g_watched[i].used     = false;
        g_watched[i].pending  = false;
        g_watched[i].tid      = -1;
        g_watched[i].restarts = 0;
        g_watched[i].dead_at_ms = 0;
        g_watched[i].name[0]  = '\0';
    }
    g_count = 0;
}

void watch(int tid, const char* name, u64 entry_virt, int priority)
{
    if (tid <= 0 || name == nullptr) return;

    // 已经登记过就更新（正常不会发生）
    Watched* w = find_by_tid(tid);
    if (w == nullptr) {
        if (g_count >= MAX_WATCHED) return;
        w = &g_watched[g_count++];
        w->used     = true;
        w->tid      = tid;
        w->restarts = 0;
    }
    copy_name(w->name, name);
    w->entry_virt = entry_virt;
    w->priority   = priority;
    w->pending    = false;

    kprintf_serial("[sup] 看护服务 %s (tid=%d)\n", name, tid);
}

bool notify_dead(int tid)
{
    Watched* w = find_by_tid(tid);
    if (w == nullptr) return false;

    if (w->restarts >= MAX_RESTART) {
        // 崩太多次，放弃 —— 否则会无限重启刷屏
        kprintf_serial("[sup] %s (tid=%d) 已崩溃 %d 次，放弃重启\n",
                       w->name, tid, w->restarts);
        return false;
    }

    w->pending     = true;
    w->dead_at_ms  = pit::uptime_ms();
    kprintf_serial("[sup] 服务 %s (tid=%d) 崩溃，准备重启\n", w->name, tid);
    return true;
}

void tick()
{
    for (int i = 0; i < g_count; ++i) {
        Watched* w = &g_watched[i];
        if (!w->used || !w->pending) continue;

        // 延迟一下再拉起，避免"一启动就崩"时疯狂刷屏
        if (pit::uptime_ms() - w->dead_at_ms < RESTART_DELAY_MS) continue;

        w->pending = false;

        if (usermode::respawn_user_process(w->tid, w->entry_virt) != 0) {
            kprintf_serial("[sup] %s 重启失败（respawn 返回错误）\n", w->name);
            continue;
        }

        // 清掉共享页里的残留状态
        reset_service_shared_state(w->name);

        ++w->restarts;
        kprintf_serial("[sup] 服务 %s 已重启（第 %d 次）\n", w->name, w->restarts);
    }
}

int kill(int tid)
{
    if (tid <= 0) return -1;

    thread::Thread* t = thread::by_tid(tid);
    if (t == nullptr) return -1;

    // 不允许杀内核线程（idle / 主线程），那是系统本身
    if (!t->is_user) return -2;

    int cur = thread::current_tid();
    if (tid == cur) {
        // 自杀：标记后触发调度，由调度器带走
        t->state = thread::State::DEAD;
        thread::wake_waiters_of(tid);
        notify_dead(tid);
        return 0;
    }

    t->state = thread::State::DEAD;
    // 关键：唤醒所有等它回复的线程，否则它们永远卡住
    thread::wake_waiters_of(tid);
    notify_dead(tid);
    return 0;
}

int restart_count(int tid)
{
    Watched* w = find_by_tid(tid);
    return (w != nullptr) ? w->restarts : -1;
}

int watched_count()
{
    return g_count;
}

}  // namespace supervisor
