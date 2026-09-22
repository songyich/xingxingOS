// ============================================================================
//  kernel/thread.cpp —— 内核线程与抢占式调度
//  ---------------------------------------------------------------------------
//  上下文切换的全部秘密只有一句话：
//    **切换线程 = 切换 rsp**。
//
//  为什么？因为中断发生时，CPU 和我们的 stub 已经把所有寄存器
//  按顺序压进了当前线程的栈，形成一个 Registers 结构。
//  只要我们把这个结构所在的地址（也就是当时的 rsp）记下来，
//  下次把 rsp 指回这里，再按相反顺序弹出，寄存器就全恢复了。
//  栈是 per-thread 的，所以换栈就等于换了整个上下文。
//
//  切换点在**中断出口**（isr_stubs.asm 的 mov rsp, rax），
//  由 sched::on_interrupt() 返回新栈指针完成。
//  这样设计的好处是不需要额外写一套 switch_to 汇编。
// ============================================================================

#include <kernel/thread.hpp>
#include <kernel/isr.hpp>
#include <kernel/heap.hpp>
#include <kernel/pit.hpp>
#include <kernel/printf.hpp>
#include <kernel/terminal.hpp>
#include <kernel/framebuffer.hpp>
#include <kernel/vmm.hpp>
#include <kernel/gdt.hpp>
#include <kernel/syscall.hpp>

// 线程入口的汇编蹦床（arch/x86_64/switch.asm）
extern "C" void thread_trampoline();
// 线程函数返回后由蹦床调用（[[noreturn]]）
extern "C" void thread_exit_trampoline();

// 前向声明：完整的切换实现在 sched 命名空间里
// （含 CR3 地址空间切换与 TSS.RSP0 更新）。
// ⚠️ 必须放在**所有命名空间之外**。
//    写在匿名 namespace 里会形成嵌套的 sched，
//    与全局的 ::sched 冲突，编译器报 "reference to 'sched' is ambiguous"。
namespace sched {

u64 do_switch_idx(int next_idx, u64 current_rsp);

// 切换日志（调试用）：记录最近若干次"返回哪个线程、它的 rip 是什么"。
// 用于排查 "iretq 时 rip=0" —— 单点打印只能看到当前这一次，
// 而日志记录的是**崩溃前的完整序列**，能看出从哪一步开始变坏。
struct SwitchRec {
    int  tid;
    u64  rip;
    u64  rsp;
};

constexpr int SWITCH_LOG_MAX = 48;
extern SwitchRec g_switch_log[SWITCH_LOG_MAX];
extern int       g_switch_n;
void record_switch(u64 ret_rsp);
void dump_switch_log();

}

namespace {

using thread::State;
using thread::Thread;

// ---------------------------------------------------------------------------
//  全局状态
// ---------------------------------------------------------------------------
Thread g_threads[thread::MAX_THREADS];
int    g_current_idx = 0;
bool   g_started = false;
u64    g_switches = 0;
u16    g_next_tid = 1;

// 当前线程剩余的时间片（单位：定时器中断次数）
u32    g_slice_left = 0;

// 每次调度给多少时间片。定时器是 100Hz（10ms 一次），
// 2 个 tick = 20ms 切一次，肉眼能看出多线程在跑又不会切得太频繁。
constexpr u32 SLICE_TICKS = 2;

// 自旋锁计数：> 0 表示禁止抢占
u32 g_preempt_disable = 0;

// 主动切换请求（yield / 阻塞时用）
bool g_need_switch = false;

// 简单清零
void zero(void* p, usize n)
{
    u8* b = static_cast<u8*>(p);
    for (usize i = 0; i < n; ++i) b[i] = 0;
}

// ---------------------------------------------------------------------------
//  构造新线程的初始栈
//  -------------------------------------------------------------------------
//  新线程从来没有运行过，所以没有人给它压过寄存器。
//  我们要手工伪造一个「它刚被中断过」的假现场：
//  从栈顶往下摆一个 Registers 结构，把 rip 指向蹦床函数。
//
//  这样第一次调度到它时，中断出口的 pop 序列会弹出我们填的值，
//  最后的 iretq 就跳进 thread_trampoline —— 线程正式开始执行。
//
//  内存布局（低地址 -> 高地址，与 isr.hpp 的 Registers 完全一致）：
//    r15 r14 r13 r12 r11 r10 r9 r8
//    rbp rdi rsi rdx rcx rbx rax
//    int_no err_code rip cs rflags rsp ss
// ---------------------------------------------------------------------------
u64 build_initial_stack(u64 stack_top, thread::entry_fn fn, void* arg)
{
    constexpr u64 FRAME = sizeof(Registers);   // 160 字节

    Registers* r = reinterpret_cast<Registers*>(stack_top - FRAME);
    zero(r, FRAME);

    // --- CPU 自动压的部分 ---
    r->ss     = 0x10;                       // 内核数据段
    r->rsp    = stack_top;                  // 同特权级 iret 不会真弹，占位即可
    r->rflags = 0x202;                      // IF=1，让新线程一上来就开着中断
    r->cs     = 0x08;                       // 内核代码段
    r->rip    = reinterpret_cast<u64>(thread_trampoline);

    // --- stub 自己压的部分 ---
    r->int_no   = 0;
    r->err_code = 0;

    // --- 通用寄存器 ---
    // SysV ABI：第一个参数 rdi，第二个 rsi。
    // 蹦床会做 mov rax,rdi / mov rdi,rsi，所以这里 rdi=函数指针、rsi=参数。
    r->rdi = reinterpret_cast<u64>(fn);
    r->rsi = reinterpret_cast<u64>(arg);

    return reinterpret_cast<u64>(r);
}

// ---------------------------------------------------------------------------
//  挑下一个该运行的线程
//  -------------------------------------------------------------------------
//  从当前位置往后找一圈（保证公平），遇到优先级更高的就替换。
//  这样既做到轮转，又让高优先级线程优先。
// ---------------------------------------------------------------------------
// allow_idle = false 时把 idle 排除在候选之外。
// 返回 -1 表示没有合适的人选。
int pick_next(bool allow_idle)
{
    int best = -1;

    for (int i = 1; i <= thread::MAX_THREADS; ++i) {
        int idx = (g_current_idx + i) % thread::MAX_THREADS;
        Thread* t = &g_threads[idx];

        if (t->state != State::READY) {
            continue;
        }
        if (!allow_idle && t->priority <= thread::PRIO_IDLE) {
            continue;               // 当前线程还能跑，就别让 idle 插队
        }

        if (best < 0) {
            best = idx;
            continue;
        }
        if (t->priority > g_threads[best].priority) {
            best = idx;
        }
    }
    return best;
}

// 真正执行切换

// ---------------------------------------------------------------------------
//  真正落地的切换动作：地址空间 + TSS.RSP0
//  -------------------------------------------------------------------------
//  ⚠️ 为什么必须抽成独立函数？
//    调度器有**两条**切换路径：
//      1. do_switch      —— 定时器抢占、阻塞唤醒，由 pick_next 选目标
//      2. do_switch_idx  —— IPC 明确指定目标时直接切
//    两条都必须做下面这两件事。
//
//    【踩过的坑】CR3 切换一开始只写在了 do_switch_idx 里，
//    do_switch（调度器实际走的那条）漏了。
//    结果用户进程跑在**内核页表**上：cpl 明明是 3，
//    但访问 0x400000 命中的是内核页表里 U/S=0 的恒等映射，
//    直接权限违规 #PF（错误码 0x5，而不是缺页的 0x4）。
//
//    这个 bug 极具迷惑性：用户页表建得**完全正确**
//    （PT[0]=0x20a007，U/S=1、物理页对、代码首字节也读得到），
//    查了半天才发现是压根没切过去。
// ---------------------------------------------------------------------------
void apply_switch(int next_idx)
{
    Thread* nxt = &g_threads[next_idx];

    // --- 地址空间切换 ---
    //
    // 为什么要先比较再写？
    //   写 CR3 会让整个 TLB 失效，代价很贵。
    //   同一进程的多个线程共享页表，它们之间切换**不需要**重载。
    //   只在确实要用不同页表时才写——这是重要的性能优化。
    u64 target_cr3 = (nxt->cr3 != 0) ? nxt->cr3 : vmm::pml4_phys();

    u64 cur_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    if (cur_cr3 != target_cr3) {
        asm volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");
    }

    // --- 更新 TSS.RSP0 ---
    // 用户态线程发生中断/系统调用时，CPU 从这里取 Ring 0 栈。
    // 切了线程却忘了改，新线程一陷入内核就会用**上一个线程的栈**，
    // 现场互相覆盖，必崩。
    if (nxt->is_user && nxt->kernel_stack_top != 0) {
        gdt::set_kernel_stack(nxt->kernel_stack_top);        // 中断用
        syscall::set_kernel_stack(nxt->kernel_stack_top);    // syscall 用
    } else if (nxt->kernel_stack_top != 0) {
        syscall::set_kernel_stack(nxt->stack_top);
    }


}

u64 do_switch(u64 current_rsp)
{
    Thread* cur = &g_threads[g_current_idx];

    // 当前线程是因为「时间片用完」还是「真的跑不动了」被换下？
    //   - 时间片用完（RUNNING）：它还想跑，只是被强制让位
    //   - 阻塞/退出（BLOCKED/DEAD）：它自己不想跑了
    bool cur_runnable = (cur->state == State::RUNNING);
    if (cur_runnable) {
        cur->state = State::READY;
    }

    // 当前线程还想跑时，不让 idle 参选。
    //
    // 为什么要这么一条？
    //   不加的话，「唯一在干活的线程」用完时间片后会和 idle 轮流——
    //   因为候选里只有 idle 就绪，于是 CPU 一半时间在做 hlt。
    //   实测 worker-b 和 idle 各占 210/232 个时间片，浪费了一半算力。
    int next = pick_next(!cur_runnable);

    if (next < 0 || next == g_current_idx) {
        // 没有更合适的人选，让当前线程继续跑
        if (cur_runnable) {
            cur->state = State::RUNNING;
        }
        g_slice_left = SLICE_TICKS;
        return current_rsp;
    }

    Thread* nxt = &g_threads[next];

    cur->rsp = current_rsp;

    // 地址空间 + TSS.RSP0 **必须**在返回新 rsp 之前做完：
    // 中断出口会按新 rsp 弹出现场，而 iretq 之后就要用新页表跑用户代码了。
    // 这两件事都在 do_switch_idx 里完成（切换 CR3 + 更新 TSS.RSP0），
    // 这里直接复用，避免两处逻辑不一致。
    return sched::do_switch_idx(next, current_rsp);
}

}  // namespace

namespace sched {

// --- 切换日志实体 ---
SwitchRec g_switch_log[SWITCH_LOG_MAX];
int       g_switch_n = 0;

// 记录一次"即将返回哪个现场"。
// 必须在**所有**出口都调用（包括"不切换"的提前 return）——
// 之前只记了 do_switch 路径，结果 6 次 iretq 只留下 2 条记录，
// 崩溃那次恰好是"未切换"路径，日志里根本看不到。
void record_switch(u64 ret_rsp)
{
    const Registers* rr = reinterpret_cast<const Registers*>(ret_rsp);
    SwitchRec rec;
    rec.tid = (g_switch_n >= 0 && g_current_idx >= 0)
            ? g_threads[g_current_idx].tid : -1;
    rec.rip = rr->rip;
    rec.rsp = ret_rsp;
    g_switch_log[g_switch_n % SWITCH_LOG_MAX] = rec;
    ++g_switch_n;
}

void dump_switch_log()
{
    int total = g_switch_n;
    int start = 0;
    if (total > SWITCH_LOG_MAX) start = total - SWITCH_LOG_MAX;
    kprintf("\n  --- 最近 %d 次切换 ---\n", total - start);
    for (int i = start; i < total; ++i) {
        const SwitchRec& r = g_switch_log[i % SWITCH_LOG_MAX];
        // 直接从 rsp 重新读完整现场（rsp 指向 Registers 结构）
        const Registers* rr = reinterpret_cast<const Registers*>(r.rsp);
        bool kernel_ret = (rr->cs == 0x08);
        kprintf("   #%d tid=%-3d rip=0x%llx cs=0x%llx ss=0x%llx "
                "rf=0x%llx rsp3=0x%llx %s%s\n",
                i, r.tid, rr->rip, rr->cs, rr->ss, rr->rflags, rr->rsp,
                kernel_ret ? "[内核态]" : "",
                (rr->cs != 0x08 && rr->cs != 0x2b) ? "  <<<< CS 异常" : "");
    }
    kprintf("\n");
}


// ---------------------------------------------------------------------------
//  中断出口的调度点（由 isr_handler 调用）
// ---------------------------------------------------------------------------
u64 on_interrupt(u64 current_rsp)
{
    if (!g_started) {
        return current_rsp;             // 调度器还没起来，什么都不做
    }

    Thread* cur = &g_threads[g_current_idx];

    // 把当前现场记进 TCB。
    // 注意：如果线程已经不是 RUNNING（比如刚被 block() 或 exit()），
    // 它的 rsp 已经没意义了，但先存着也无妨，反切回来前状态会先被改。
    cur->rsp = current_rsp;

    // --- 判断是否需要切换 ---
    bool need = g_need_switch || (cur->state != State::RUNNING);

    // 抢占被关（持有自旋锁）时不能切，否则持锁线程被换下会死锁
    if (g_preempt_disable > 0) {
        need = false;
    }

    g_need_switch = false;

    if (!need) {
        return current_rsp;
    }
    return do_switch(current_rsp);
}

// ---------------------------------------------------------------------------
//  定时器滴答：推进时间片、唤醒睡眠线程
// ---------------------------------------------------------------------------
void tick()
{
    if (!g_started) return;

    Thread* cur = &g_threads[g_current_idx];
    if (cur->state == State::RUNNING) {
        ++cur->ticks;
        cur->cpu_time_ms += 10;             // 定时器 100Hz，一次 10ms

        if (g_slice_left > 0) {
            --g_slice_left;
        }
        if (g_slice_left == 0) {
            g_need_switch = true;
            g_slice_left = SLICE_TICKS;
        }
    }

    // 唤醒时间到了的睡眠线程
    u64 now = pit::uptime_ms();
    for (int i = 0; i < thread::MAX_THREADS; ++i) {
        Thread* t = &g_threads[i];
        if (t->state == State::BLOCKED && t->wake_time_ms != 0
            && now >= t->wake_time_ms) {
            t->wake_time_ms = 0;
            t->state = State::READY;
        }
    }
}

void disable_preempt() { ++g_preempt_disable; }

void enable_preempt()
{
    if (g_preempt_disable > 0) {
        --g_preempt_disable;
    }
}

bool preempt_enabled() { return g_preempt_disable == 0; }

u64 do_switch_idx(int next_idx, u64 current_rsp)
{
    if (next_idx < 0 || next_idx >= thread::MAX_THREADS) {
        return current_rsp;
    }
    if (next_idx == g_current_idx) {
        return current_rsp;
    }

    Thread* cur = &g_threads[g_current_idx];
    Thread* nxt = &g_threads[next_idx];

    if (nxt->state != State::READY && nxt->state != State::RUNNING) {
        return current_rsp;             // 目标不可运行，放弃
    }

    if (cur->state == State::RUNNING) {
        cur->state = State::READY;
    }
    cur->rsp = current_rsp;

    // 地址空间 + TSS.RSP0 + syscall 内核栈，统一交给 apply_switch。
    //
    // 这**三件事必须一起做**，少一件都会崩：
    //   - CR3：不切的话用户进程跑在别人（或内核）的页表上
    //   - TSS.RSP0：中断陷入时 CPU 从这里取 Ring 0 栈
    //   - g_syscall_rsp：syscall 不自动切栈，靠入口手动换 rsp
    //
    // 【本次修复的 bug】之前只做了前两件，漏了 syscall 栈。
    //  于是所有用户线程发起 syscall 时都从同一个固定栈顶压现场，
    //  线程 A 在 syscall 中途阻塞后，线程 B 发起 syscall 就从
    //  同一个栈顶开始用，直接把 A 尚未返回的现场覆盖。
    //  表现：A 被唤醒后 rip/ss/rflags 全是垃圾，iretq 时 #GP。
    //  微内核里 IPC 频繁阻塞，这个 bug 几乎必然触发。
    apply_switch(next_idx);

    nxt->state = State::RUNNING;
    g_current_idx = next_idx;
    ++g_switches;
    g_slice_left = SLICE_TICKS;

    return nxt->rsp;
}

}  // namespace sched

// ---------------------------------------------------------------------------
//  自旋锁
//  -------------------------------------------------------------------------
//  lock xchg：把 1 写进去，同时拿到旧值。
//    旧值 0 → 原来没锁，我抢到了
//    旧值 1 → 已经有人持锁，继续转圈等
//  lock 前缀保证「读-改-写」三步不可分割。
// ---------------------------------------------------------------------------
void Spinlock::lock()
{
    // 先关抢占：否则单核上持锁线程被切走，等锁的线程会永远空转
    sched::disable_preempt();

    while (__atomic_exchange_n(&locked_, 1u, __ATOMIC_ACQUIRE) != 0u) {
        // 让 CPU 歇一下，降低功耗也减轻总线争用
        asm volatile("pause");
    }
}

void Spinlock::unlock()
{
    __atomic_store_n(&locked_, 0u, __ATOMIC_RELEASE);
    sched::enable_preempt();
}

bool Spinlock::try_lock()
{
    if (__atomic_exchange_n(&locked_, 1u, __ATOMIC_ACQUIRE) == 0u) {
        sched::disable_preempt();
        return true;
    }
    return false;
}

namespace thread {

void init()
{
    for (int i = 0; i < MAX_THREADS; ++i) {
        g_threads[i].state = State::UNUSED;
        g_threads[i].rsp = 0;
        g_threads[i].stack_top = 0;
        g_threads[i].tid = 0;
        g_threads[i].ticks = 0;
        g_threads[i].cpu_time_ms = 0;
        g_threads[i].wake_time_ms = 0;
        g_threads[i].priority = PRIO_NORMAL;
        zero(g_threads[i].name, sizeof(g_threads[i].name));

        // 微内核新增字段
        g_threads[i].cr3 = 0;
        g_threads[i].is_user = false;
        g_threads[i].user_stack_top = 0;
        g_threads[i].kernel_stack_top = 0;
        g_threads[i].ipc_state = IpcState::NONE;
        g_threads[i].ipc_recv_buf = nullptr;
        g_threads[i].ipc_wait_target = -1;
        g_threads[i].ipc_from = -1;
        g_threads[i].irq_waiting = -1;
        g_threads[i].irq_received = false;
    }

    // 0 号线程 = 当前执行流（kmain 后面会变成 shell）
    Thread* main = &g_threads[0];
    main->tid = 0;
    main->state = State::RUNNING;
    main->priority = PRIO_NORMAL;
    main->rsp = 0;                  // 第一次中断时会被填上
    const char* nm = "main";
    for (int i = 0; nm[i]; ++i) main->name[i] = nm[i];

    // 0 号是内核主线程，不是用户态
    main->cr3 = 0;              // 0 = 用当前页表（内核页表）
    main->is_user = false;

    g_current_idx = 0;
    g_next_tid = 1;
    g_slice_left = SLICE_TICKS;
    g_started = true;

    // 建立 idle 线程：没人可跑时它上，只做 hlt 省电
    create("idle", [](void*) {
        for (;;) {
            asm volatile("sti; hlt");
        }
    }, nullptr, PRIO_IDLE);
}

int create(const char* name, entry_fn fn, void* arg, int priority)
{
    if (!g_started) {
        return -1;
    }
    // fn 允许为 nullptr：表示"调用者会自己构造初始现场"。
    // 用户态进程就是这种情况——它需要的现场（cs/ss 带 RPL3、
    // rip 指向用户空间）和内核线程的蹦床现场完全不同，
    // 只能由 usermode.cpp 自己来摆。

    int slot = -1;
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (g_threads[i].state == State::UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;                      // 线程表满了
    }

    void* stack = heap::kmalloc(STACK_SIZE);
    if (stack == nullptr) {
        return -1;
    }

    Thread* t = &g_threads[slot];
    t->stack_top = reinterpret_cast<u64>(stack) + STACK_SIZE;
    // fn 为空时不构造现场，留给调用者（用户态进程会自己摆一个
    // "从 Ring 3 陷入"的现场，让 iret 直接进用户态）
    t->rsp = (fn != nullptr) ? build_initial_stack(t->stack_top, fn, arg) : 0;
    t->state = State::READY;
    t->priority = static_cast<u8>(priority);
    t->tid = g_next_tid++;
    t->ticks = 0;
    t->cpu_time_ms = 0;
    t->wake_time_ms = 0;
    t->entry = fn;
    t->arg = arg;

    // 微内核新增字段默认值（创建后再由 usermode 层改成用户进程）
    t->cr3 = 0;                 // 0 = 沿用内核页表（内核线程）
    t->is_user = false;
    t->user_stack_top = 0;
    t->kernel_stack_top = t->stack_top;
    t->ipc_state = IpcState::NONE;
    t->ipc_recv_buf = nullptr;
    t->ipc_wait_target = -1;
    t->ipc_from = -1;
    t->irq_waiting = -1;
    t->wait_tid       = -1;
    t->irq_received = false;

    if (name != nullptr) {
        int i = 0;
        for (; name[i] != '\0' && i < 31; ++i) {
            t->name[i] = name[i];
        }
        t->name[i] = '\0';
    }

    return t->tid;
}

// 调度器是否已运行（keyboard::wait_key 用它决定忙等还是让出）
bool enabled() { return g_started; }

// ---------------------------------------------------------------------------
//  dump_info：把线程表导出到用户态共享页
//  -------------------------------------------------------------------------
//  每条 32 字节：
//    +0   tid      (u32)
//    +4   state    (u32)
//    +8   ticks    (u64)
//    +16  name     16 字节（不足补 0）
//
//  为什么不能直接返回内核指针？
//    用户态页表和内核不同，把内核地址交给用户态去解引用会 #PF。
//    必须**拷贝**数据，而不是共享指针。
// ---------------------------------------------------------------------------
int dump_info(void* user_buf, int max_count)
{
    if (user_buf == nullptr) return -1;

    u8* p = reinterpret_cast<u8*>(user_buf);
    int n = 0;

    for (int i = 0; i < MAX_THREADS && n < max_count; ++i) {
        Thread* t = &g_threads[i];
        if (t->state == State::UNUSED || t->state == State::DEAD) continue;

        u32* p32 = reinterpret_cast<u32*>(p);
        p32[0] = static_cast<u32>(t->tid);
        p32[1] = static_cast<u32>(t->state);

        u64* p64 = reinterpret_cast<u64*>(p + 8);
        p64[0] = t->ticks;

        // 拷贝名字（最多 15 字符 + 结尾 0）
        char* dst = reinterpret_cast<char*>(p + 16);
        for (int k = 0; k < 15; ++k) {
            dst[k] = t->name[k];
            if (t->name[k] == '\0') break;
        }
        dst[15] = '\0';

        p += 32;
        ++n;
    }
    return n;
}

void yield()
{
    if (!g_started) return;
    g_need_switch = true;
    // 用一条软中断触发调度。
    // 走中断路径才能复用「中断出口切栈」这套机制。
    asm volatile("int $0x81");
}

void sleep_ms(u64 ms)
{
    if (!g_started) return;
    Thread* cur = &g_threads[g_current_idx];
    cur->wake_time_ms = pit::uptime_ms() + ms;
    cur->state = State::BLOCKED;
    yield();
}

void block()
{
    if (!g_started) return;
    g_threads[g_current_idx].state = State::BLOCKED;
    yield();
}

void wake(int tid)
{
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (g_threads[i].tid == static_cast<u16>(tid)
            && g_threads[i].state == State::BLOCKED) {
            g_threads[i].state = State::READY;
            return;
        }
    }
}

// 唤醒所有正在等 tid 结束的线程（wait 的实现基础）
static void wake_waiters(int tid)
{
    for (int i = 0; i < MAX_THREADS; ++i) {
        Thread* w = &g_threads[i];
        if (w->state == State::UNUSED) continue;
        if (w->wait_tid == tid) {
            w->wait_tid = -1;
            if (w->state == State::BLOCKED) w->state = State::READY;
        }
    }
}

void wake_waiters_of(int tid) { wake_waiters(tid); }

[[noreturn]] void exit()
{
    if (!g_started) {
        for (;;) asm volatile("cli; hlt");
    }
    Thread* cur = &g_threads[g_current_idx];
    cur->state = State::DEAD;

    // 唤醒所有"在等我结束"的线程（比如等子程序的 shell）
    wake_waiters(cur->tid);

    g_need_switch = true;
    asm volatile("int $0x81");

    // 理论上回不来（DEAD 不会再被调度）；万一回来了就停机待命
    for (;;) asm volatile("sti; hlt");
}

// --- 通过 tid 找索引 ---
int index_of_tid(int tid)
{
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (g_threads[i].tid == static_cast<u16>(tid)
            && g_threads[i].state != State::UNUSED) {
            return i;
        }
    }
    return -1;
}

Thread* get_by_index(int idx)
{
    if (idx < 0 || idx >= MAX_THREADS) {
        return nullptr;
    }
    return &g_threads[idx];
}

// --- 直接切换（IPC 快路径）---
u64 switch_to_idx(int idx, u64 current_rsp)
{
    return sched::do_switch_idx(idx, current_rsp);
}

u64 switch_to_tid(int tid, u64 current_rsp)
{
    int idx = index_of_tid(tid);
    if (idx < 0) {
        return current_rsp;
    }
    return sched::do_switch_idx(idx, current_rsp);
}

int current_tid() { return g_threads[g_current_idx].tid; }

Thread* current() { return &g_threads[g_current_idx]; }

Thread* by_tid(int tid)
{
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (g_threads[i].tid == static_cast<u16>(tid)
            && g_threads[i].state != State::UNUSED) {
            return &g_threads[i];
        }
    }
    return nullptr;
}

int count()
{
    int n = 0;
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (g_threads[i].state != State::UNUSED) ++n;
    }
    return n;
}

u64 total_switches() { return g_switches; }

void for_each(void (*cb)(const Thread* t, void* ctx), void* ctx)
{
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (g_threads[i].state != State::UNUSED) {
            cb(&g_threads[i], ctx);
        }
    }
}

const char* state_name(State s)
{
    switch (s) {
        case State::UNUSED:  return "未使用";
        case State::READY:   return "就绪";
        case State::RUNNING: return "运行中";
        case State::BLOCKED: return "阻塞";
        case State::DEAD:    return "已结束";
    }
    return "未知";
}

}  // namespace thread

// 蹦床在线程函数返回后跳来这里
extern "C" void thread_exit_trampoline()
{
    thread::exit();
}
