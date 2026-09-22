// ============================================================================
//  include/kernel/thread.hpp —— 内核线程与抢占式调度（阶段 6）
//  ---------------------------------------------------------------------------
//  线程是什么？
//    一段能独立执行的控制流。它和进程的区别是：进程独占地址空间，
//    线程共享地址空间。本阶段在内核态，所有线程共享同一个页表，
//    所以做的是「内核线程」。
//
//  线程靠什么保持独立？
//    **栈**。每个线程有自己的栈，切换线程 = 切换 rsp。
//    CPU 的「上下文」= 所有通用寄存器 + 栈指针 + 指令指针。
//    切走时把寄存器存进旧线程的栈，切回时从新线程的栈弹出来。
//
//  什么时候切换？
//    定时器每隔 10ms 打一次中断，中断出口处调度器决定换不换。
//    这叫**抢占式**——线程自己不用管，时间片到了就被强制换下。
//
//  和阶段 5 shell 的关系：
//    shell 自己也是一个线程。它按 wait_key() 阻塞时会让出 CPU，
//    于是后台线程（比如计数器）能持续跑。
// ============================================================================
#pragma once

#include <kernel/types.h>

// 前向声明：IpcMsg 定义在 ipc.hpp，这里只需要指针。
// ⚠️ 必须放在 **全局** 命名空间。写在 namespace thread 里的话，
//    会声明出一个全新的 thread::IpcMsg 类型，
//    和真正的 ::IpcMsg 是两个不同类型，赋值时编译报错。
struct IpcMsg;

namespace thread {

// 每个线程的栈大小（16KB，够深调用）
constexpr u64 STACK_SIZE = 16 * 1024;

// 最多线程数
constexpr int MAX_THREADS = 32;

// 优先级：数字越大越先跑
constexpr int PRIO_IDLE = 0;      // 空闲线程，只在没别人可跑时上
constexpr int PRIO_LOW  = 1;
constexpr int PRIO_NORMAL = 5;    // 默认
constexpr int PRIO_HIGH = 10;

// ---------------------------------------------------------------------------
//  线程状态
// ---------------------------------------------------------------------------
enum class State : u8 {
    UNUSED,      // 槽位空闲
    READY,       // 就绪，等待被调度
    RUNNING,     // 正在运行
    BLOCKED,     // 阻塞，等待某个事件
    DEAD,        // 已退出，等待回收
};

// ---------------------------------------------------------------------------
//  IPC 子状态
//  -------------------------------------------------------------------------
//  为什么要把 IPC 状态单独分出来，而不是复用 State::BLOCKED？
//    因为「阻塞」的原因不同，唤醒方式完全不同：
//      - 等接收：有人发消息给我才能醒
//      - 等回复：对方 reply 才能醒
//      - 等中断：硬件中断来了才能醒
//    混在一起的话，唤醒时就分不清该由谁来触发，容易漏唤醒导致死锁。
// ---------------------------------------------------------------------------
enum class IpcState : u8 {
    NONE,        // 不参与 IPC
    WAIT_RECV,   // 阻塞，等别人发消息过来（服务进程等待请求）
    WAIT_REPLY,  // 阻塞，等对方回复（客户端发出 call 之后）
    WAIT_IRQ,    // 阻塞，等硬件中断（驱动进程）
};

// 线程入口函数
using entry_fn = void (*)(void* arg);

// ---------------------------------------------------------------------------
//  线程控制块（TCB）
// ---------------------------------------------------------------------------
struct Thread {
    u64      rsp;                        // 被切走时的栈指针（上下文存在栈里）
    u64      stack_top;                  // 栈顶（高地址），分配的就是这段
    State    state;
    u8       priority;
    u16      tid;
    u64      ticks;                      // 已运行的时间片数
    u64      cpu_time_ms;                // 累计运行毫秒
    u64      wake_time_ms;               // sleep 到什么时候醒
    char     name[32];
    entry_fn entry;
    void*    arg;

    // --- 用户态支持（阶段 7 微内核新增）---
    u64      cr3;                        // 页表物理地址（用户进程各有各的页表）
    bool     is_user;                    // 是否是用户态线程
    u64      user_stack_top;             // 用户栈顶
    u64      kernel_stack_top;           // 内核栈顶（从用户态陷入时用）

    // --- IPC ---
    IpcState ipc_state;
    IpcMsg*  ipc_recv_buf;               // 用户态缓冲区：recv 时是接收区，
                                         //               call 时是回复接收区
    int      ipc_wait_target;            // 我在等谁（WAIT_REPLY 时是目标 tid，
                                         //            WAIT_RECV 时是 TID_ANY）
    int      ipc_from;                   // 最近一次消息来自谁
    int      irq_waiting;                // 等哪个 IRQ（-1 = 不等）
    bool     irq_received;               // 中断已到达

    // --- 等待子进程（shell 启动 .xzs 程序后要等它跑完）---
    int      wait_tid;                   // 我在等哪个线程结束（-1 = 不等）
};

// ---------------------------------------------------------------------------
//  初始化：建立 idle 线程，把「当前执行流」包装成主线程
//  必须在中断开启前调用
// ---------------------------------------------------------------------------
void init();

// 调度器是否已经启动（未启动时不能调用 yield / sleep）
bool enabled();

// ---------------------------------------------------------------------------
//  直接切换到指定线程（IPC 快路径用）
//  -------------------------------------------------------------------------
//  为什么不走普通调度？
//    普通调度要扫描一遍候选、比较优先级，是 O(n)。
//    而 IPC 时我们已经**明确知道**下一个该跑谁（就是通信的另一方），
//    直接切过去，省掉扫描开销。这是微内核 IPC 最核心的优化之一。
//
//  返回新线程的栈顶（给中断出口的 mov rsp, rax 用）
// ---------------------------------------------------------------------------
u64 switch_to_tid(int tid, u64 current_rsp);

// 内核态栈切换：从当前线程（可能在用户态）切到目标线程
u64 switch_to_idx(int idx, u64 current_rsp);

// 通过 tid 找线程索引，找不到返回 -1
int index_of_tid(int tid);

// 通过索引直接取 TCB（IPC 内部遍历用，idx 必须合法）
Thread* get_by_index(int idx);

// ---------------------------------------------------------------------------
//  创建线程。返回 tid，失败返回 -1
// ---------------------------------------------------------------------------
int create(const char* name, entry_fn entry, void* arg,
           int priority = PRIO_NORMAL);

// 当前线程主动让出 CPU
// 把线程表导出到用户态共享页，返回导出的条数
int dump_info(void* user_buf, int max_count);

void yield();

// 睡眠若干毫秒
void sleep_ms(u64 ms);

// 阻塞/唤醒（配合锁使用）
void block();
void wake(int tid);

// 退出当前线程（[[noreturn]]）
[[noreturn]] void exit();

// ---------------------------------------------------------------------------
//  查询
// ---------------------------------------------------------------------------
int  current_tid();

// 唤醒所有正在等 tid 结束的线程
void wake_waiters_of(int tid);
Thread* current();
Thread* by_tid(int tid);
int  count();                  // 活跃线程数
u64  total_switches();         // 累计切换次数

// 遍历所有线程（ps 命令用）
void for_each(void (*cb)(const Thread* t, void* ctx), void* ctx);

// 转状态名为中文
const char* state_name(State s);

}  // namespace thread

// ---------------------------------------------------------------------------
//  调度器：只暴露给中断出口调用
// ---------------------------------------------------------------------------
namespace sched {
    // 调试：崩溃时打印最近若干次切换记录
    void dump_switch_log();
    void record_switch(u64 ret_rsp);

// 传入当前 rsp，返回接下来要用的 rsp
u64 on_interrupt(u64 current_rsp);

// 内部：真正执行切换到某个索引（thread.cpp 用）
u64 do_switch_idx(int next_idx, u64 current_rsp);

// 定时器每次滴答时调用：推进时间片、唤醒睡眠中的线程
void tick();

// 打开/关闭抢占（临界区用，配合自旋锁）
void disable_preempt();
void enable_preempt();
bool preempt_enabled();

}  // namespace sched

// ---------------------------------------------------------------------------
//  自旋锁（Spinlock）
//  -------------------------------------------------------------------------
//  为什么不用睡眠锁？
//    内核里持锁时间极短（改几个变量），睡眠/唤醒的开销反而更大。
//    而且中断上下文里不能睡眠。
//
//  为什么必须关抢占？
//    单核上，如果线程 A 拿了锁、时间片到了被切走，
//    线程 B 进来 spin 等待——A 永远没机会释放锁，死锁。
//    所以持锁期间禁止被切走。
//
//  用 lock xchg 保证原子性：
//    普通 "读-改-写" 三步可能被中断打断，
//    lock 前缀锁总线，让整个交换不可分割。
// ---------------------------------------------------------------------------
class Spinlock {
public:
    Spinlock() : locked_(0) {}

    void lock();
    void unlock();
    bool try_lock();
    bool is_locked() const { return locked_ != 0; }

    Spinlock(const Spinlock&) = delete;
    Spinlock& operator=(const Spinlock&) = delete;

private:
    volatile u32 locked_;
};

// RAII 守卫：构造时加锁，析构时解锁。
// 有了它就不用到处记着配对 lock/unlock，
// 中途 return 或者抛异常（虽然我们没开异常）都不会漏解锁。
class LockGuard {
public:
    explicit LockGuard(Spinlock& lk) : lk_(lk) { lk_.lock(); }
    ~LockGuard() { lk_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private:
    Spinlock& lk_;
};
