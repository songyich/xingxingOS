// ============================================================================
//  kernel/ipc.cpp —— 进程间通信实现（微内核的核心）
//  ---------------------------------------------------------------------------
//  整套机制围绕「客户端-服务器」模式设计：
//    客户端（比如 shell）调用 ipc_call 发请求并阻塞，
//    服务进程（比如终端服务）用 ipc_recv 接收、处理、ipc_reply 回复。
//
//  性能优化点（用户要求最小开销）：
//    1. **直接切换**：call/reply 时明确知道下一个该跑谁，
//       直接 switch_to 过去，跳过调度器扫描候选（O(n) → O(1)）
//    2. **零动态分配**：消息缓冲是静态数组，按线程索引分配，
//       发送和接收只是 56 字节的 memcpy，没有 kmalloc 开销
//    3. **不经过队列**：同步 IPC 直接从发送方拷到接收方的用户缓冲区，
//       没有中间队列的入队出队
// ============================================================================

#include <kernel/ipc.hpp>
#include <kernel/log.hpp>
#include <kernel/thread.hpp>
#include <kernel/heap.hpp>
#include <kernel/printf.hpp>

// ---------------------------------------------------------------------------
//  IPC 链路统计（定位"消息消失在哪一段"）
//  -------------------------------------------------------------------------
//  ⚠️ 必须放在**全局作用域**。
//    放在匿名 namespace 里会和头文件的 extern 声明构成两个不同的实体，
//    编译器报 "reference to 'g_stat_call' is ambiguous"。
//  ---------------------------------------------------------------------------
static_assert(sizeof(IpcMsg) == 120, "内核 IpcMsg 尺寸不对");

u64 g_stat_call  = 0;
u64 g_stat_recv  = 0;
u64 g_stat_reply = 0;
u64 g_stat_puts  = 0;
u64 g_stat_syscall = 0;
u64 g_stat_call_done = 0;

// "我有一次未完成的 call"（do_reply 之后由发送方重试时清除）
bool g_call_pending[thread::MAX_THREADS];
// "回复已经写好了，发送方可以取走了"
bool g_reply_ready[thread::MAX_THREADS];

// ⚠️ 用**函数**递增，不要让外部直接 extern 全局变量。
//
//   全局变量的 extern 声明极易掉进调用方所在的 namespace
//   （比如 syscall.cpp 的 namespace syscall），
//   于是 syscall::g_stat_puts 和 ::g_stat_puts 成了两个符号，
//   一边递增、一边打印，永远是 0 —— 假象是"PUTS 没被调用"。
//
//   函数声明在全局，调用方写 ::ipc_inc_puts() 就一定落到同一实体。
void ipc_inc_syscall() { ++g_stat_syscall; }
void ipc_inc_puts()    { ++g_stat_puts; }

void ipc_dump_stats()
{
    // 统计信息保留在函数里，但默认不再打印（曾经刷屏淹没真实输出）
    if (false) kprintf("[STAT] sys=%llu call=%llu/done=%llu recv=%llu reply=%llu puts=%llu\n",
            g_stat_syscall, g_stat_call, g_stat_call_done, g_stat_recv,
            g_stat_reply, g_stat_puts);
}

namespace {

using thread::Thread;
using thread::State;
using thread::IpcState;

// ---------------------------------------------------------------------------
//  消息暂存区：每个线程一个槽位
//  -------------------------------------------------------------------------
//  为什么用静态数组而不是动态分配？
//    IPC 每秒可能上万次，每次 kmalloc 会引入分配器开销和内存碎片。
//    按线程索引直接定位，是 O(1) 且零分配的。
// ---------------------------------------------------------------------------
alignas(16) IpcMsg g_out_msg[thread::MAX_THREADS];   // 待发出/待取走的消息

// ---------------------------------------------------------------------------
//  g_delivered[i]：g_out_msg[i] 是否已经被接收方取走了
//  -------------------------------------------------------------------------
//  【踩过的坑：消息被重复消费】
//    do_call 发现对方正在等，会把消息**直接投递**进对方的接收缓冲区，
//    同时把发送方标记为 WAIT_REPLY（它确实在等回复）。
//
//    但 do_recv 的匹配条件只看 "ipc_state == WAIT_REPLY"，
//    于是接收方下次调用 do_recv 时，会**再次**匹配到这个发送方，
//    从 g_out_msg 里把同一条消息又取一遍 ——
//    表现是：第一条消息正常，之后接收方再也收不到新消息
//    （它一直在重复消费已经被取走的旧消息），
//    整个 IPC 链路在第一条消息之后就卡死。
//
//  加了标志后：直接投递时置 true，do_recv 只消费未投递过的。
// ---------------------------------------------------------------------------
bool g_delivered[thread::MAX_THREADS];



// 回复暂存区。
// do_reply 此刻运行在**接收方**上下文（CR3 = 接收方页表），
// 绝不能直接写发送方的用户缓冲区（会写坏接收方自己的栈，
// 因为两者用户栈虚拟地址相同）。
// 先存到内核，等发送方恢复后在**它自己**的上下文里取回。
alignas(16) IpcMsg g_reply_msg[thread::MAX_THREADS];

u64 g_total_messages = 0;

// 拷贝一条消息（56 字节，编译器会优化成几次 mov）
void copy_msg(IpcMsg* dst, const IpcMsg* src)
{
    // 整体拷贝，**不要逐字段手写**。
    //
    // 【踩过的坑】原来这里是逐字段赋值：
    //   dst->type = src->type; dst->len = src->len; ...
    //   后来给 IpcMsg 加了 text[64] 内联区，却忘了同步这里 ——
    //   结果 type/len 都正确，唯独**消息内容是空的**。
    //   表现是"IPC 明明通了、长度也对，就是没内容"，极难定位。
    //
    //   整体拷贝以后，再给 IpcMsg 加字段也不会漏。
    //   IpcMsg 是 POD，直接赋值即可；用 memcpy 表达"按字节复制"更明确。
    // 内核没有 libc，这里手写一个按 8 字节为主的拷贝。
    // IpcMsg 的大小是 8 的倍数（u32 x2 + u64 x6 + char[64] = 120），
    // 所以先按 u64 拷，剩余的按字节拷。
    {
        u64* d8 = reinterpret_cast<u64*>(dst);
        const u64* s8 = reinterpret_cast<const u64*>(src);
        usize n = sizeof(IpcMsg) / 8;
        for (usize i = 0; i < n; ++i) d8[i] = s8[i];
        usize rest = sizeof(IpcMsg) % 8;
        if (rest != 0) {
            u8* db = reinterpret_cast<u8*>(dst) + n * 8;
            const u8* sb = reinterpret_cast<const u8*>(src) + n * 8;
            for (usize i = 0; i < rest; ++i) db[i] = sb[i];
        }
    }
}

// 把当前线程阻塞下来，并触发一次调度。
// 用 int 0x81 走中断路径，因为切换统一在中断出口完成。
void block_and_switch()
{
    // 【事故记录】这个函数曾经被"清理诊断代码"时用正则误删成空壳。
    //
    //   后果：所有调用它的地方（do_recv / do_call 的阻塞分支）
    //   都变成了"设了 BLOCKED 状态但没真正让出 CPU"，
    //   代码继续往下跑完，把 ipc_state 改回 NONE ——
    //   线程变成「BLOCKED 但不在等任何东西」，永远醒不过来。
    //
    //   教训：**不要用正则批量删除带参数的 kprintf**，
    //   极易把紧邻的正常代码一起删掉。删除后必须检查函数体是否还完整。
    thread::block();
}

}  // namespace

namespace ipc {

void init()
{
    for (int i = 0; i < thread::MAX_THREADS; ++i) {
        IpcMsg* m = &g_out_msg[i];
        m->type = 0; m->len = 0;
        m->a = 0; m->b = 0; m->c = 0; m->d = 0;
        m->ptr = 0; m->size = 0;
    }
    g_total_messages = 0;
}

// ---------------------------------------------------------------------------
//  do_call：发送 + 等回复
//  -------------------------------------------------------------------------
//  两种情形：
//    a) 目标已经在等接收 → 直接把消息投递进它的缓冲区，然后切过去
//       （只发生一次切换，这是最快路径）
//    b) 目标正忙 → 自己阻塞，等目标稍后 recv 时来取
// ---------------------------------------------------------------------------
int do_call(int dest_tid, IpcMsg* msg)
{
    if (msg == nullptr) return IPC_ERR_INVAL;
    // 【统一日志】谁给谁发消息、消息类型是什么
    LOG_IPC2(LogOp::IpcCall, reinterpret_cast<u64>(msg),
             static_cast<u64>(dest_tid), msg->type);

    Thread* cur = thread::current();
    if (cur == nullptr) return IPC_ERR_INVAL;
    int cur_idx = thread::index_of_tid(cur->tid);
    if (cur_idx < 0) return IPC_ERR_INVAL;

    // --- 重试路径：之前发过，这次是来取回复的 ---
    if (g_call_pending[cur_idx]) {
        if (g_reply_ready[cur_idx]) {
            copy_msg(msg, &g_reply_msg[cur_idx]);
            g_reply_ready[cur_idx] = false;
            g_call_pending[cur_idx] = false;
            ++g_stat_call_done;
            return IPC_OK;
        }
        cur->state = State::BLOCKED;      // 回复还没来，继续等
        return IPC_WOULD_BLOCK;
    }

    int dest_idx = thread::index_of_tid(dest_tid);
    if (dest_idx < 0) return IPC_ERR_DEST;

    // --- 首次发送：消息存进内核暂存区 ---
    //
    // 为什么不直接写进接收方的缓冲区？
    //   此刻 CR3 是**发送方**的页表，而接收方的缓冲区在**它自己**的
    //   用户栈上。两者的虚拟地址常常相同（比如都是 0x7ffffe00），
    //   直接写会写到**发送方自己**的栈上 —— 消息根本没送出去。
    //   所以统一存内核，等接收方在自己上下文里来取。
    ++g_stat_call;
    copy_msg(&g_out_msg[cur_idx], msg);
    g_delivered[cur_idx] = false;
    g_reply_ready[cur_idx] = false;
    g_call_pending[cur_idx] = true;
    ++g_total_messages;

    cur->ipc_state = IpcState::WAIT_REPLY;
    cur->ipc_wait_target = dest_tid;

    // --- 目标是否正在等接收？是就唤醒它，让它自己来取 ---
    Thread* d = thread::get_by_index(dest_idx);
    if (d != nullptr && d->ipc_state == IpcState::WAIT_RECV
        && (d->ipc_wait_target == TID_ANY || d->ipc_wait_target == cur->tid)) {

        d->ipc_from = cur->tid;         // 告诉它：谁来消息了
        d->ipc_state = IpcState::NONE;
        d->ipc_recv_buf = nullptr;
        d->state = State::READY;        // 唤醒，让它自己来取
    }

    // 发送方阻塞等回复；切换由 syscall 出口统一完成
    cur->state = State::BLOCKED;
    return IPC_WOULD_BLOCK;
}

int do_reply(int dest_tid, IpcMsg* msg)
{
    ++g_stat_reply;
    if (msg == nullptr) return IPC_ERR_INVAL;

    int target_idx = thread::index_of_tid(dest_tid);
    if (target_idx < 0) return IPC_ERR_DEST;

    Thread* target = thread::get_by_index(target_idx);
    if (target == nullptr) return IPC_ERR_DEST;

    // 对方必须是在等我们回复，否则这个 reply 没有意义
    if (target->ipc_state != IpcState::WAIT_REPLY) {
        return IPC_ERR_INVAL;
    }
    if (target->ipc_wait_target != thread::current_tid()) {
        return IPC_ERR_PERM;        // 不是它等的那个人
    }

    ++g_total_messages;

    g_reply_ready[target_idx] = true;

    // 回复先存进内核暂存区。
    // 不能在这里写 target->ipc_recv_buf —— 那是**发送方**的用户地址，
    // 而此刻 CR3 是**我们（接收方）**的，直接写会写到自己的栈上。
    copy_msg(&g_reply_msg[target_idx], msg);


    // 唤醒对方（切换由调度出口统一完成）
    target->ipc_state = IpcState::NONE;
    target->ipc_wait_target = -1;
    target->ipc_recv_buf = nullptr;
    target->state = State::READY;
    return IPC_OK;
}

// ---------------------------------------------------------------------------
//  do_send：异步发送，不等回复
//  -------------------------------------------------------------------------
//  发出的消息会留在暂存区，等对方 recv 时来取。
//  因为不等回复，发送方**继续运行**。
// ---------------------------------------------------------------------------
int do_send(int dest_tid, IpcMsg* msg)
{
    if (msg == nullptr) return IPC_ERR_INVAL;
    if (thread::index_of_tid(dest_tid) < 0) return IPC_ERR_DEST;

    Thread* cur = thread::current();
    int cur_idx = thread::index_of_tid(cur->tid);
    if (cur_idx < 0) return IPC_ERR_INVAL;

    copy_msg(&g_out_msg[cur_idx], msg);
    ++g_total_messages;

    // 标记"这条消息是发给谁的"，目标 recv 时靠这个找到它
    cur->ipc_wait_target = dest_tid;
    cur->ipc_state = IpcState::WAIT_REPLY;   // 复用：表示"有消息待取"

    // 如果目标正等着，直接唤醒它
    int dest_idx = thread::index_of_tid(dest_tid);
    Thread* d = thread::get_by_index(dest_idx);
    if (d != nullptr && d->ipc_state == IpcState::WAIT_RECV
        && d->ipc_recv_buf != nullptr) {
        copy_msg(d->ipc_recv_buf, &g_out_msg[cur_idx]);
        g_delivered[cur_idx] = true;        // 已投递，防止被重复取走
        d->ipc_from = cur->tid;
        d->ipc_state = IpcState::NONE;
        d->ipc_recv_buf = nullptr;
        d->state = State::READY;
    }
    // 发送方继续运行，不阻塞
    return IPC_OK;
}

// ---------------------------------------------------------------------------
//  do_recv：接收一条消息
// ---------------------------------------------------------------------------
int do_recv(int* from_tid, IpcMsg* msg)
{
    if (msg == nullptr) return IPC_ERR_INVAL;
    LOG_IPC(LogOp::IpcRecv, reinterpret_cast<u64>(msg), 0);

    Thread* cur = thread::current();
    if (cur == nullptr) return IPC_ERR_INVAL;
    int cur_tid = cur->tid;

    // --- 被硬件中断唤醒：优先返回，让驱动进程去读硬件 ---
    //
    //   deliver_irq 会置位 irq_received。这里看到它，就**立刻返回**，
    //   并用 from_tid = -1 告诉调用方"这不是一条 IPC 消息，是 IRQ"。
    //
    //   若不给这个出口，do_recv 会因为没有 IPC 消息而继续阻塞，
    //   用户态的 ipc_recv 重试循环就永远出不来，
    //   外层的"读端口"代码再也执行不到 —— 表现为一个按键都收不到。
    if (cur->irq_received) {
        cur->irq_received = false;
        if (from_tid != nullptr) *from_tid = -1;
        return IPC_OK;
    }

    // --- 找有没有人在等我 ---
    for (int i = 0; i < thread::MAX_THREADS; ++i) {
        Thread* c = thread::get_by_index(i);
        if (c == nullptr) continue;
        if (c->state == State::UNUSED || c->state == State::DEAD) continue;

        // 有人在等回复，而且等的就是我
        if (c->ipc_state == IpcState::WAIT_REPLY
            && c->ipc_wait_target == cur_tid
            && !g_delivered[i]) {           // 只取"还没被直接投递过"的

            copy_msg(msg, &g_out_msg[i]);
            g_delivered[i] = true;      // 已消费，防止被反复取走

            if (from_tid != nullptr) *from_tid = c->tid;
            cur->ipc_from = c->tid;

            // 注意：这里不把 c 唤醒！
            // call 的语义是"我发完就等你回复"，所以 c 继续阻塞，
            // 等我 reply 时才醒。send 的话则已经醒着了。
            ++g_stat_recv;
            return IPC_OK;
        }
    }


    // --- 没人等我：标记阻塞，让用户态重试 ---
    //
    // 不再在内核里切走（那需要 int $0x81 嵌套，实测会导致用户态失联）。
    // 这里只设状态并**立即返回**，切换交给 syscall 出口的统一调度点。
    cur->ipc_state = IpcState::WAIT_RECV;
    cur->ipc_wait_target = TID_ANY;
    cur->ipc_recv_buf = msg;
    cur->ipc_from = -1;
    cur->state = State::BLOCKED;
    return IPC_WOULD_BLOCK;
}

// ---------------------------------------------------------------------------
//  deliver_irq：把硬件中断投递给注册了它的驱动进程
//  -------------------------------------------------------------------------
//  微内核的关键机制：内核**不处理**中断的具体含义，
//  它只知道"IRQ n 来了，该通知谁"。真正的处理在用户态驱动进程里。
// ---------------------------------------------------------------------------
void deliver_irq(u8 irq)
{
    // 【日志去噪】跳过**定时器中断(IRQ0)**。
    //
    //   定时器每 tick（约 100Hz）都会走到这里，
    //   实测它一个就占了全部日志的绝大多数，
    //   真正有价值的 ipc_call / 键盘中断 全被淹没在里面。
    //   而且"定时器又响了一次"这个事件本身没有信息量。
    //
    //   只记录**非定时器**的 IRQ（键盘、磁盘、网卡等），
    //   这些才是排查问题时真正想看的。
    if (irq != 0) {
        LOG_IPC(LogOp::IrqDeliver, static_cast<u64>(irq), 0);
    }
    for (int i = 0; i < thread::MAX_THREADS; ++i) {
        Thread* t = thread::get_by_index(i);
        if (t == nullptr) continue;
        // 条件放宽：驱动进程通常阻塞在 ipc_recv（WAIT_RECV）上等待请求，
        // 同时也想被 IRQ 唤醒。只认 WAIT_IRQ 的话它就永远醒不过来。
        bool waiting_irq = (t->ipc_state == IpcState::WAIT_IRQ)
                        || (t->ipc_state == IpcState::WAIT_RECV);
        if (waiting_irq && t->irq_waiting == static_cast<int>(irq)) {
            t->irq_received = true;
            t->ipc_state = IpcState::NONE;
            t->state = State::READY;
        }
    }
}

u64 total_messages() { return g_total_messages; }

void block_current() { block_and_switch(); }

void wake_thread(int tid)
{
    int idx = thread::index_of_tid(tid);
    if (idx < 0) return;
    Thread* t = thread::get_by_index(idx);
    if (t == nullptr) return;
    t->ipc_state = IpcState::NONE;
    t->irq_waiting = -1;
    t->state = State::READY;
}

}  // namespace ipc

// ---------------------------------------------------------------------------
//  供 ipc.hpp 的 inline 包装调用
// ---------------------------------------------------------------------------
int ipc_call_kernel(int dest, IpcMsg* msg)  { return ipc::do_call(dest, msg); }
int ipc_reply_kernel(int dest, IpcMsg* msg) { return ipc::do_reply(dest, msg); }
int ipc_send_kernel(int dest, IpcMsg* msg)  { return ipc::do_send(dest, msg); }
int ipc_recv_kernel(int* from, IpcMsg* msg) { return ipc::do_recv(from, msg); }
