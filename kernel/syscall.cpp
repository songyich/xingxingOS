// ============================================================================
//  kernel/syscall.cpp —— 系统调用分发（微内核的「对外窗口」）
//  ---------------------------------------------------------------------------
//  微内核的 syscall 数量应该**极少**。判断标准：
//    这个功能能不能放到用户态做？能 → 就不该出现在内核里。
//
//  所以这里只有四类：
//    1. IPC          —— 一切服务的通道
//    2. 中断注册/应答 —— 驱动进程接管硬件的入口
//    3. 特权操作     —— 端口读写、设备内存映射（用户态做不了）
//    4. 基础         —— 让出 CPU、睡眠、退岀、调试输出
//
//  像 open/read/write 这种，属于文件系统服务，**不在这里**。
// ============================================================================

#include <kernel/syscall.hpp>
#include <kernel/terminal.hpp>
#include <kernel/ipc.hpp>
#include <kernel/isr.hpp>
#include <kernel/thread.hpp>
#include <kernel/pit.hpp>
#include <kernel/io.h>
#include <kernel/acpi.hpp>
#include <kernel/mouse.hpp>
#include <kernel/pic.hpp>
#include <kernel/log.hpp>
#include <kernel/svcdir.hpp>
#include <kernel/supervisor.hpp>
#include <kernel/usermode.hpp>
#include <kernel/vmm.hpp>
#include <kernel/pmm.hpp>
#include <kernel/serial.hpp>
#include <kernel/printf.hpp>
#include <kernel/terminal.hpp>
#include <kernel/gdt.hpp>

// 汇编函数（arch/x86_64/syscall_entry.asm）
// ⚠️ extern "C" 声明必须放在文件作用域，写在函数体内会导致语法错误
extern "C" void syscall_enable();
extern "C" void set_kernel_gs(u64 addr);

// syscall 入口要用的内核栈顶
//
// 定义必须在**全局**作用域（汇编里用 [rel g_syscall_rsp] 访问）：
//   写在匿名 namespace 里的话，链接器找的是带命名空间修饰的符号，
//   而汇编看到的是另一个符号 —— 报 undefined reference。
//   （这个坑在阶段 3 的 boot_pml4、阶段 6 的 mb2 变量上都踩过）
u64 g_syscall_rsp = 0;

// 进入 syscall 入口的次数（调试用，定义在汇编里引用）
u64 g_syscall_count = 0;



namespace {

// ---------------------------------------------------------------------------
//  用户态指针检查
//  -------------------------------------------------------------------------
//  用户程序可以传任意地址进来。如果不检查，用户传一个内核地址，
//  就能读写内核内存——整个隔离形同虚设。
//
//  这里做的是**粗粒度**检查：确保落在用户空间（低于内核起始地址）。
//  严格的检查还要验证页表项，那是后续完善安全（阶段 17）时的事。
// ---------------------------------------------------------------------------
constexpr u64 USER_SPACE_LIMIT = 0x0000800000000000ull;   // 128TB 以下

bool user_ptr_ok(u64 addr, u64 len)
{
    if (addr == 0) return false;
    // 检查是否越出用户空间（注意用减法避免 addr+len 溢出）
    if (addr > USER_SPACE_LIMIT) return false;
    if (len > USER_SPACE_LIMIT - addr) return false;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
//  分发入口：由 syscall_entry.asm 调用
//  ---------------------------------------------------------------------------
//  返回"接下来要恢复的现场指针"。
//    没发生切换  -> 就是传入的 regs
//    IPC 导致阻塞 -> 调度器会返回新线程的现场
//  这套机制和中断出口完全一致，两条路径共用一套切换逻辑。
// ---------------------------------------------------------------------------
#define IPC_PTR_OFFSET 0   /* 偏移方案无效，已改用共享页 */

// ---------------------------------------------------------------------------
//  从用户空间拷贝字符串到内核
//  -------------------------------------------------------------------------
//  ⚠️ 内核页表里**没有**用户进程的映射，直接解引用用户指针会 #PF。
//     进入 syscall 时 CR3 仍是调用进程的页表，
//     所以 vmm::get_phys 查到的正是调用者的映射 —— 逐页翻译即可。
// ---------------------------------------------------------------------------
static void copy_from_user_str(char* dst, u64 user_src, u64 n)
{
    for (u64 i = 0; i < n; ++i) {
        u64 va   = user_src + i;
        u64 off  = va & 0xFFF;
        u64 phys = vmm::get_phys_current(va & ~0xFFFull);
        if (phys == 0) { dst[i] = '\0'; return; }
        dst[i] = static_cast<char>(
            *reinterpret_cast<u8*>(phys + off + 0xFFFFFFFF80000000ull));
    }
}

extern "C" u64 syscall_handler(Registers* regs)
{

    // --- 取出系统调用号与参数 ---
    // 汇编里把 rax 存进了 int_no 字段，这里取出来
    const u64 num = regs->int_no;
    const u64 a1 = regs->rdi;
    const u64 a2 = regs->rsi;
    const u64 a3 = regs->rdx;
    const u64 a4 = regs->r10;      // 注意：第 4 参数是 r10 不是 rcx
    const u64 a5 = regs->r8;
    const u64 a6 = regs->r9;
    (void)a5;
    (void)a6;

    u64 ret = 0;

    // 【统一日志】三条核心信息之一：**哪个程序请求的内核**。
    //   X= 记录调用号（Sys 枚举），ADDR= 第一个参数（多为地址/端口）。
    LOG_SYSCALL2(LogOp::SyscallEnter, a1, num, a2);

    switch (static_cast<Sys>(num)) {

    // =======================  1. IPC  =======================
    case Sys::IPC_CALL:
        // a1 = 目标 tid，a2 = 消息结构地址（in-out 参数）
        if (!user_ptr_ok(a2, sizeof(IpcMsg))) {
            ret = IPC_ERR_INVAL;
        } else {
            ret = static_cast<u64>(
                ipc::do_call(static_cast<int>(a1),
                             reinterpret_cast<IpcMsg*>(a2 - IPC_PTR_OFFSET)));
        }
        break;

    case Sys::IPC_REPLY:
        if (!user_ptr_ok(a2, sizeof(IpcMsg))) {
            ret = IPC_ERR_INVAL;
        } else {
            ret = static_cast<u64>(
                ipc::do_reply(static_cast<int>(a1),
                              reinterpret_cast<IpcMsg*>(a2)));
        }
        break;

    case Sys::IPC_SEND:
        if (!user_ptr_ok(a2, sizeof(IpcMsg))) {
            ret = IPC_ERR_INVAL;
        } else {
            ret = static_cast<u64>(
                ipc::do_send(static_cast<int>(a1),
                             reinterpret_cast<IpcMsg*>(a2)));
        }
        break;

    case Sys::IPC_RECV:
        if (!user_ptr_ok(a2, sizeof(IpcMsg))) {
            ret = IPC_ERR_INVAL;
        } else {
            int from = 0;
            ret = static_cast<u64>(
                ipc::do_recv(reinterpret_cast<int*>(a1),
                             reinterpret_cast<IpcMsg*>(a2 - IPC_PTR_OFFSET)));
            (void)from;
        }
        break;

    // =======================  2. 中断  =======================
    case Sys::IRQ_REGISTER: {
        // a1 = IRQ 编号（0~15）
        // 驱动进程调用它，表示"这个 IRQ 来了通知我"
        thread::Thread* cur = thread::current();
        if (cur != nullptr && a1 < 16) {
            cur->irq_waiting = static_cast<int>(a1);

            // ---------------------------------------------------------
            //  【bug 修复】注册时**自动 unmask** 该 IRQ
            //  -------------------------------------------------------
            //  pic::init() 里把所有中断全屏蔽了（set_mask_all），
            //  原本靠内核各驱动自己调 unmask。但鼠标驱动是**用户态服务**，
            //  而 unmask 要写 0x21/0xA1 数据口 —— 端口 I/O 用户态做不了
            //  （虽然内核有 PORT_OUT，但让每个驱动自己管 PIC 屏蔽位
            //   会造成多个驱动互相覆盖）。
            //
            //  结果：IRQ12（鼠标）注册了却永远收不到中断。
            //
            //  现在：谁注册谁放行，由内核统一管屏蔽位，语义清晰。
            //  另外 IRQ12 在从片上，从片是经主片 IRQ2 级联的，
            //  所以 >= 8 的 IRQ 必须**同时放行 IRQ2**，否则从片根本传不上来。
            // ---------------------------------------------------------
            pic::unmask(static_cast<u8>(a1));
            if (a1 >= 8) {
                pic::unmask(2);      // 从片级联线
            }

            ret = 0;
        } else {
            ret = static_cast<u64>(-1);
        }
        break;
    }

    case Sys::IRQ_ACK:
        // 驱动处理完中断，告诉内核可以继续放行
        // （EOI 已经在中断处理里发过了，这里主要是清状态）
        ret = 0;
        break;

    // =======================  3. 特权操作  =======================
    case Sys::PORT_IN:
        // a1 = 端口号，a2 = 字节宽度(1/2/4)
        // 端口 I/O 是特权指令，用户态执行会 #GP，只能由内核代劳
        if (a2 == 1) {
            ret = inb(static_cast<u16>(a1));
        } else if (a2 == 2) {
            ret = inw(static_cast<u16>(a1));
        } else if (a2 == 4) {
            ret = inl(static_cast<u16>(a1));
        } else {
            ret = static_cast<u64>(-1);
        }
        break;

    case Sys::PORT_OUT:
        // a1 = 端口，a2 = 值，a3 = 宽度
        if (a3 == 1) {
            outb(static_cast<u16>(a1), static_cast<u8>(a2));
        } else if (a3 == 2) {
            outw(static_cast<u16>(a1), static_cast<u16>(a2));
        } else if (a3 == 4) {
            outl(static_cast<u16>(a1), static_cast<u32>(a2));
        } else {
            ret = static_cast<u64>(-1);
        }
        break;

    case Sys::MMIO_MAP:
        // a1 = 物理地址，a2 = 字节数，a3 = 建议虚拟地址
        // 把设备内存（比如帧缓冲）映射进调用者地址空间。
        // 必须带 PAGE_USER，否则用户态访问会 #PF。
        {
            u64 phys = a1 & ~(vmm::PAGE_SIZE - 1);
            u64 len  = a2;
            u64 virt = (a3 != 0) ? a3 : 0;
            if (virt == 0) {
                // 没指定就放在用户空间的一个固定区域
                static u64 next_mmio = 0x0000000400000000ull;   // 16GB 处起
                virt = next_mmio;
                next_mmio += (len + vmm::PAGE_SIZE - 1) & ~(vmm::PAGE_SIZE - 1);
            }
            u64 flags = vmm::PAGE_PRESENT | vmm::PAGE_WRITABLE
                      | vmm::PAGE_USER | vmm::PAGE_PCD | vmm::PAGE_NX;
            u64 pages = (len + vmm::PAGE_SIZE - 1) / vmm::PAGE_SIZE;
            if (pages == 0) pages = 1;
            bool ok = vmm::map_pages(virt, phys, pages, flags);
            ret = ok ? virt : 0;
        }
        break;

    // =======================  4. 基础  =======================
    case Sys::YIELD:
        thread::yield();
        ret = 0;
        break;

    case Sys::SLEEP:
        thread::sleep_ms(a1);
        ret = 0;
        break;

    case Sys::UPTIME:
        ret = pit::uptime_ms();
        break;

    case Sys::EXIT: {
        thread::Thread* cur = thread::current();
        if (cur != nullptr) {
            cur->state = thread::State::DEAD;
        }
        thread::yield();
        // yield 之后不会再回来（本线程已 DEAD）
        break;
    }

    case Sys::PUTS:
        // 调试输出：直接走串口 + 终端（应急通道）
        // 注意：正常输出应该走终端服务的 IPC，这里只用于内核调试
        if (user_ptr_ok(a1, a2)) {
            const char* s = reinterpret_cast<const char*>(a1);
            // 用户态输出（含单字符回显 MSG_PUTC）也要维护鼠标指针，
            // 否则敲一个键就把指针吃掉一块，要等下次输出才补回来。
            mouse::hide();
            for (u64 i = 0; i < a2 && s[i] != '\0'; ++i) {
                serial::putc(s[i]);
                // ⚠️ 必须同时写屏幕！
                //   之前只写串口，结果用户态服务的输出在串口日志里能看到，
                //   屏幕上却空空如也 —— 看起来就像"IPC 没通"，
                //   实际上消息早就到了终端服务。
                //   终端服务最终要自己映射帧缓冲绘制，
                //   在那之前先借内核的终端输出上屏。
                term::putc(s[i]);
            }
            mouse::show();
            ret = a2;
        } else {
            ret = static_cast<u64>(-1);
        }
        break;

    case Sys::MOUSE_CENTER:
        // 指针居中。返回中心坐标，用户态服务要用它同步自己的累加值
        // （否则服务从 0,0 开始累加，第一次移动就会把指针拽到角落）。
        if (!mouse::available()) {
            ret = static_cast<u64>(-1);
        } else {
            ret = static_cast<u64>(mouse::center());
        }
        break;

    case Sys::MOUSE_DRAW:
        // -------------------------------------------------------------
        //  画鼠标指针（机制在内核，策略在用户态 mouse 服务）
        //  -------------------------------------------------------------
        //  a1 = 模式：0 = 移到绝对坐标 (a2, a3)；1 = 相对移动 (a2, a3)
        //
        //  为什么必须由内核画？
        //    帧缓冲是内核管的资源，用户态服务不该直接写显存。
        //    它只负责"算出新坐标"，落笔交给内核 —— 和 IPC 的分工一致。
        // -------------------------------------------------------------
        if (!mouse::available()) {
            ret = static_cast<u64>(-1);     // 文本模式没有图形指针
        } else {
            i64 mx = static_cast<i64>(a2);
            i64 my = static_cast<i64>(a3);
            if (a1 == 0) {
                mouse::move_to(static_cast<int>(mx), static_cast<int>(my));
            } else {
                mouse::move_by(static_cast<int>(mx), static_cast<int>(my));
            }
            ret = 0;
        }
        break;

    case Sys::SHUTDOWN:
        // -------------------------------------------------------------
        //  【语义修正】关机失败**绝不能**退化成"重启"
        // -------------------------------------------------------------
        //  用户实测：执行 shutdown，机器却**重新启动**了。
        //
        //  原因：以前这里有个"传统兜底" —— 写键盘控制器 0x64/0xFE。
        //  但 0xFE 是 **Reset 键**的语义，效果是**重新启动**，
        //  跟"关机"完全相反。用户点关机看到重启，体验是错的。
        //
        //  现在：ACPI 真断电失败就老实 **halt（停机）** ——
        //  CPU 停止执行、屏幕冻结，语义正确，也不空转烧 CPU。
        //  键盘控制器复位只在 **REBOOT** 分支里用（那才是它的语义）。
        // -------------------------------------------------------------
        if (acpi::available()) {
            ret = 0;
            acpi::shutdown();
            // 能返回 = 写了端口但没断电。把探测过程打到屏幕上，
            // 方便用户反馈定位（VirtualBox 上看不到串口）。
            kprintf("\n  [ACPI] 已写端口但机器未断电，改为停机。\n");
            acpi::report_diag();
        } else {
            kprintf("\n  [ACPI] 不可用，改为停机。\n");
            acpi::report_diag();
        }

        kprintf("\n  系统已停机，可以关闭电源。\n");
        asm volatile("cli");
        for (;;) asm volatile("hlt");
        break;

    case Sys::REBOOT:
        // -----------------------------------------------------------------
        //  复位：通过键盘控制器的 0x64 端口写 0xFE
        //  ---------------------------------------------------------------
        //  这是 x86 上最通用的重启方式（比 triple fault 干净）。
        //
        //  为什么必须由内核做？
        //    端口 I/O 是特权指令，用户态执行会 #GP。
        //    但**决策权**在用户态的电源服务 —— 内核只提供这个动作。
        //
        //  0x64 是状态/命令端口，bit1=1 表示输入缓冲满，要等它清空。
        // -----------------------------------------------------------------
        {
            // 等输入缓冲空
            for (int i = 0; i < 10000; ++i) {
                if ((inb(0x64) & 0x02) == 0) break;
            }
            outb(0x64, 0xFE);           // 复位命令

            // 极少数机器不响应，兜底：三 faults 强制重启
            asm volatile("cli");
            for (;;) asm volatile("hlt");
        }
        break;

    case Sys::LOG_CONTROL:
        // -----------------------------------------------------------------
        //  日志落盘开关
        //  ---------------------------------------------------------------
        //  设计：日志**永远在内存里记录**，这里控制的只是"要不要落盘"。
        //
        //  关键性质：从"关"切到"开"时，会**立刻落盘一次**，
        //  把之前累积在环形缓冲里的历史全部倒出来 ——
        //  这正是这套设计的价值：出问题时往往来不及提前开开关，
        //  但事后打开就能看到最近发生了什么。
        //
        //  将来 UI 的设置界面就是调这个 syscall。
        // -----------------------------------------------------------------
        if (a1 == 0) {
            klog::set_flush_enabled(false);
            ret = 0;
        } else if (a1 == 1) {
            klog::set_flush_enabled(true);
            ret = 0;
        } else if (a1 == 2) {
            klog::flush_now();
            ret = 0;
        } else {
            ret = static_cast<u64>(-1);
        }
        break;

    case Sys::SPAWN:
        // -----------------------------------------------------------------
        //  启动一个 .xzs 程序（万物皆可程序的核心）
        //  ---------------------------------------------------------------
        //  内核只负责"加载并运行"，策略（要不要跑、跑哪个）
        //  完全由调用方（shell 或别的程序）决定。
        // -----------------------------------------------------------------
        ret = static_cast<u64>(
            usermode::spawn_program(a1, a2, a3, a4));
        break;

    case Sys::SVC_REGISTER:
        // 把**当前线程**登记为某个服务（对应 /system/services/<name>）
        {
            char nm[32];
            u64 nl = (a2 >= 31) ? 31 : a2;
            for (u64 i = 0; i < nl; ++i) nm[i] = '\0';
            {
                // 名字在用户空间，需逐字节翻译（内核页表没有用户映射）
                copy_from_user_str(nm, a1, nl);
            }
            nm[nl] = '\0';
            thread::Thread* me = thread::current();
            ret = static_cast<u64>(
                svcdir::register_service(nm, static_cast<int>(nl),
                                         me ? me->tid : -1));
        }
        break;

    case Sys::SVC_LOOKUP:
        // 按名字查服务 tid（对应 cat /system/services/<name>）
        {
            char nm[32];
            u64 nl = (a2 >= 31) ? 31 : a2;
            for (u64 i = 0; i < nl; ++i) nm[i] = '\0';
            {
                copy_from_user_str(nm, a1, nl);
            }
            nm[nl] = '\0';
            ret = static_cast<u64>(
                svcdir::lookup(nm, static_cast<int>(nl)));
        }
        break;

    case Sys::CLEAR:
        // -----------------------------------------------------------------
        //  清屏（终端服务收到 MSG_CLEAR 后转发进内核）
        //  ---------------------------------------------------------------
        //  为什么必须走 syscall？
        //    终端服务是**用户态进程**，不能直接碰帧缓冲。
        //    和 MSG_PUTS 转发给 Sys::PUTS 是同一个道理。
        // -----------------------------------------------------------------
        term::clear();
        ret = 0;
        break;

    case Sys::WAIT:
        // -----------------------------------------------------------------
        //  等待某个线程结束（shell 启动 .xzs 程序后要等它跑完）
        //  ---------------------------------------------------------------
        //  ⚠️ 用**轮询**而不是阻塞唤醒。
        //     原因：从 syscall 上下文调 thread::exit() 时，
        //     它内部的 int 0x81 调度在 syscall 栈上行为不可靠，
        //     实测会导致等待方永远醒不过来（shell 卡死、不再出提示符）。
        //     轮询虽然"土"，但在抢占式调度下完全可用：
        //     yield 会把 CPU 让给目标程序，它跑完我们就退出循环。
        // -----------------------------------------------------------------
        {
            int target = static_cast<int>(a1);
            u64 spins = 0;
            for (;;) {
                thread::Thread* tgt = thread::by_tid(target);
                if (tgt == nullptr || tgt->state == thread::State::DEAD) {
                    ret = 0;
                    break;
                }
                // 上限保护：万一目标永远不退出，不能把系统拖死
                if (++spins > 10 * 1000 * 1000) {
                    ret = static_cast<u64>(-1);
                    break;
                }
                thread::yield();
            }
        }
        break;
    case Sys::KILL:
        // -----------------------------------------------------------------
        //  【P2】终止一个进程
        //  ---------------------------------------------------------------
        //  存在的意义是**验证崩溃自愈**：用户可以 kill 掉键盘服务，
        //  观察"系统没挂 + 键盘几秒内恢复"。
        //
        //  微内核下这是安全的：被杀的只是用户态进程，
        //  内核和其他服务不受影响。
        //
        //  权限：只允许杀别的用户态进程，内核线程（idle/main）不允许。
        //  （真正的用户权限体系在 P5，这里只做最基本的保护）
        // -----------------------------------------------------------------
        {
            int target = static_cast<int>(a1);
            int caller = thread::current_tid();

            // 自杀：把状态改掉即可，返回后调度器自然带走
            if (target == caller) {
                thread::Thread* self = thread::current();
                if (self != nullptr) {
                    self->state = thread::State::DEAD;
                }
                thread::wake_waiters_of(caller);
                supervisor::notify_dead(caller);
                ret = 0;
            } else {
                ret = static_cast<u64>(supervisor::kill(target));
            }
        }
        break;
    case Sys::THREAD_INFO:
        // -----------------------------------------------------------------
        //  查询线程列表：结果写进调用方给的共享页
        //  ---------------------------------------------------------------
        //  布局（每条 32 字节）：
        //    +0  tid (u32)
        //    +4  state (u32)
        //    +8  ticks (u64)
        //    +16 name 指针放不了（跨地址空间无意义），
        //        改为直接把名字拷贝到 +16 起的 16 字节里
        //
        //  为什么不直接返回指针？
        //    内核和用户态地址空间不同，内核指针用户态解引用会 #PF。
        //    （阶段 4 就踩过：跨地址空间写用户缓冲区必须先切 CR3）
        // -----------------------------------------------------------------
        if (user_ptr_ok(a1, 32 * 32)) {
            ret = static_cast<u64>(
                thread::dump_info(reinterpret_cast<void*>(a1), 32));
        } else {
            ret = static_cast<u64>(-1);
        }
        break;

    default:
        ret = static_cast<u64>(-1);     // 未知系统调用
        break;
    }

    // --- 返回值写回 rax ---
    // 汇编里从结构的 rax 字段弹出到 rax 寄存器，用户态就能拿到返回值
    regs->rax = ret;

    // --- 调度点 ---
    u64 next_rsp = sched::on_interrupt(reinterpret_cast<u64>(regs));
    sched::record_switch(next_rsp);

    // 诊断：syscall 路径的出口现场
    {
        const Registers* r = reinterpret_cast<const Registers*>(next_rsp);
        if (r->rip < 0x400000ull) {
            kprintf("[BAD-SYSCALL] num=%llu rip=0x%llx cs=0x%llx rsp=0x%llx "
                    "rsp_ptr=0x%llx\n",
                    regs->rax, r->rip, r->cs, r->rsp, next_rsp);
        }
    }
    return next_rsp;
}

namespace syscall {

// 给调度器用：切换线程时更新 syscall 入口要用的内核栈顶
//
// 为什么必须单独维护一份？
//   syscall 指令**不像中断那样自动切栈**，
//   它靠我们在入口处 `mov rsp, [gs:24]` 手动切。
//   而 TSS.RSP0 是给**中断**用的，两者是两套机制。
//
//   之前只更新了 TSS.RSP0，漏了这个 ——
//   结果所有用户进程的 syscall 都压在同一个初始内核栈上，
//   线程一切换就把彼此的现场覆盖了，
//   表现是返回地址变成垃圾（比如 RIP=0x3f7fe0）。
void set_kernel_stack(u64 top)
{
    g_syscall_rsp = top;
}

void init()
{
    // 装载 syscall 相关 MSR（STAR / LSTAR / FMASK）
    syscall_enable();

    // 建立 per-cpu 数据区并加载内核 GS
    // 布局： [gs:0] 保留  [gs:8] 当前线程指针
    //        [gs:16] 用户 rsp 暂存   [gs:24] 内核栈顶
    static alignas(64) u64 g_percpu[64];
    for (int i = 0; i < 64; ++i) g_percpu[i] = 0;


    // [gs:24] 填内核栈顶
    //
    // 为什么需要它？
    //   syscall 进入时 CPU **不切栈**（这点和中断不同），
    //   所以内核必须自己准备一个栈，否则就跑在用户栈上了。
    //   每次线程切换时，gdt::set_kernel_stack 会把这里更新成
    //   **新线程自己的内核栈**，所以这只是一个初始值。
    static alignas(16) u8 g_boot_syscall_stack[16 * 1024];
    g_percpu[3] = reinterpret_cast<u64>(&g_boot_syscall_stack[sizeof(g_boot_syscall_stack)]);
    g_syscall_rsp = reinterpret_cast<u64>(&g_boot_syscall_stack[sizeof(g_boot_syscall_stack)]);

    set_kernel_gs(reinterpret_cast<u64>(&g_percpu[0]));


}

}  // namespace syscall
