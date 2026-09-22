// ============================================================================
//  kernel/isr.cpp —— IDT 建立与中断分发
// ============================================================================

#include <kernel/isr.hpp>
#include <kernel/ipc.hpp>
#include <kernel/printf.hpp>
#include <kernel/thread.hpp>
#include <kernel/terminal.hpp>
#include <kernel/framebuffer.hpp>
#include <kernel/pic.hpp>
#include <kernel/printf.hpp>
#include <kernel/thread.hpp>

// 定义在 kernel/syscall.cpp。
// ⚠️ 必须声明在**全局**作用域：写在匿名 namespace 里会变成
//    (anonymous namespace)::g_syscall_count，链接时找不到符号。
//    （同一个坑：阶段 3 的 boot_pml4、阶段 6 的 mb2 变量）
extern u64 g_syscall_count;
extern u64 g_syscall_rsp;

namespace {

// ---------------------------------------------------------------------------
//  IDT 表项（x86_64 下 16 字节）
//  -------------------------------------------------------------------------
//  字段被硬件拆得七零八落是历史包袱：这个结构从 32 位时代一路扩展过来，
//  为了兼容只能把 64 位地址切成三段塞进不同位置。
// ---------------------------------------------------------------------------
struct IdtEntry {
    u16 offset_low;      // 处理函数地址 bit 0~15
    u16 selector;        // 目标代码段选择子（内核代码段 = 0x08）
    u8  ist;             // 中断栈表索引，0 表示不用
    u8  type_attr;       // P/DPL/类型位，见下面的常量
    u16 offset_mid;      // 地址 bit 16~31
    u32 offset_high;     // 地址 bit 32~63
    u32 zero;            // 保留，必须为 0
} __attribute__((packed));

// lidt 指令要的 6 字节指针：2 字节限长 + 8 字节基址
struct IdtPtr {
    u16 limit;
    u64 base;
} __attribute__((packed));

// type_attr 的取值
//   bit7   P    = 1（这一项有效）
//   bit6-5 DPL  = 00（只有内核能触发）
//   bit4   S    = 0（系统段）
//   bit3-0 Type = 1110（中断门，进入时自动关中断）
constexpr u8 IDT_ATTR_INTERRUPT_GATE = 0x8E;

// 内核代码段选择子：GDT 第 1 项，每项 8 字节，所以是 0x08
constexpr u16 KERNEL_CODE_SELECTOR = 0x08;

constexpr int IDT_ENTRIES = 256;

alignas(16) IdtEntry g_idt[IDT_ENTRIES];
IdtPtr g_idt_ptr;

// 中断处理函数表（IRQ 用；异常统一走内核自己的处理）
using handler_fn = void (*)(const Registers*);
handler_fn g_handlers[IDT_ENTRIES];

// ---------------------------------------------------------------------------
//  异常的中文名
//  这些字符串里的汉字会被 tools/genfont.py 自动扫描进字库，
//  所以写中文没有任何额外成本——加一句诊断，字库里就多那几个字。
// ---------------------------------------------------------------------------
const char* const kExceptionNames[] = {
    "除法错误",              //  0 #DE
    "调试异常",              //  1 #DB
    "不可屏蔽中断",          //  2 NMI
    "断点",                  //  3 #BP
    "溢出",                  //  4 #OF
    "越界",                  //  5 #BR
    "无效操作码",            //  6 #UD
    "设备不可用",            //  7 #NM
    "双重错误",              //  8 #DF
    "协处理器段越界",        //  9
    "无效任务状态段",        // 10 #TS
    "段不存在",              // 11 #NP
    "栈段错误",              // 12 #SS
    "通用保护错误",          // 13 #GP
    "页错误",                // 14 #PF
    "保留",                  // 15
    "浮点错误",              // 16 #MF
    "对齐检查",              // 17 #AC
    "机器检查",              // 18 #MC
    "单指令多数据浮点异常",  // 19 #XM
    "虚拟化异常",            // 20 #VE
    "保留",                  // 21
    "保留",                  // 22
    "保留",                  // 23
    "保留",                  // 24
    "保留",                  // 25
    "保留",                  // 26
    "保留",                  // 27
    "保留",                  // 28
    "保留",                  // 29
    "安全异常",              // 30 #SX
    "保留",                  // 31
};

// 读 CR2：页错误时里面存着「导致出错的那个线性地址」
inline u64 read_cr2()
{
    u64 value;
    asm volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

// 页错误错误码每一位的含义
void decode_page_fault(u64 err)
{
    kprintf("  页错误详情 : ");
    kprintf("%s ", (err & 0x01) ? "页存在但权限不足" : "页不在内存中");
    kprintf("%s ", (err & 0x02) ? "写操作" : "读操作");
    kprintf("%s ", (err & 0x04) ? "用户态触发" : "内核态触发");
    if (err & 0x08) kprintf("保留位被置位 ");
    // I/D 位**无论置位与否都要打印**。
    //   bit4=1 -> 取指令触发（RIP 指向的地址有问题）
    //   bit4=0 -> 数据访问（指令本身没问题，是它访问的地址有问题）
    // 这一个位就能把「代码被破坏」和「映射/栈有问题」分开，
    // 只在置位时打印的话，关键的信息反而是"没打印"。
    kprintf("%s ", (err & 0x10) ? "取指令触发" : "数据访问");
    kprintf("\n");
    // 注意：CR2 是 64 位的，必须用 %llx。
    // 用 %x 的话 kprintf 只会按 unsigned int 取 32 位，
    // 地址的高半截会被截掉，看起来就像「出错地址是 0」——极难排查。
    kprintf("  出错地址   : 0x%llx (CR2)\n", read_cr2());
}

// 打印全部寄存器。出事之后这就是唯一的线索，宁可多打也别漏。
void dump_registers(const Registers* regs)
{
    // 寄存器全是 64 位，一律用 %llx；用 %x 会只显示低 32 位
    kprintf("  RAX=0x%llx  RBX=0x%llx  RCX=0x%llx\n",
            regs->rax, regs->rbx, regs->rcx);
    kprintf("  RDX=0x%llx  RSI=0x%llx  RDI=0x%llx\n",
            regs->rdx, regs->rsi, regs->rdi);
    kprintf("  RBP=0x%llx  RSP=0x%llx  RIP=0x%llx\n",
            regs->rbp, regs->rsp, regs->rip);
    kprintf("  R8 =0x%llx  R9 =0x%llx  R10=0x%llx\n",
            regs->r8,  regs->r9,  regs->r10);
    kprintf("  R11=0x%llx  R12=0x%llx  R13=0x%llx\n",
            regs->r11, regs->r12, regs->r13);
    kprintf("  R14=0x%llx  R15=0x%llx\n", regs->r14, regs->r15);
    kprintf("  CS =0x%llx  SS =0x%llx  RFLAGS=0x%llx\n",
            regs->cs,  regs->ss,  regs->rflags);
}

// 异常处理：打印现场然后停机
// 注意：这里刻意不走 panic()——异常可能发生在任何时刻，
// 连堆都可能已经坏了，处理路径越短越安全。
[[noreturn]] void handle_exception(const Registers* regs)
{
    // 立即关中断，防止嵌套异常把栈打穿
    asm volatile("cli");

    term::set_color(fb::color::WHITE, fb::color::RED);
    term::clear();

    kprintf("\n");
    kprintf("  ==========================================================\n");
    kprintf("                     系统异常\n");
    kprintf("  ==========================================================\n");
    kprintf("\n");

    kprintf("  [debug] syscall 进入次数 = %llu\n", g_syscall_count);
    kprintf("  [debug] g_syscall_rsp = 0x%llx\n", g_syscall_rsp);
    {
        u64 cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        kprintf("  [debug] CR3 = 0x%llx\n", cr3);
    }

    const char* name = exception_name(static_cast<u8>(regs->int_no));
    kprintf("  异常类型   : %llu 号 - %s\n", regs->int_no, name);
    kprintf("  错误码     : 0x%llx\n", regs->err_code);

    if (regs->int_no == 14) {
        decode_page_fault(regs->err_code);
    }

    sched::dump_switch_log();

    kprintf("\n  【定性提示】\n");
    if (regs->int_no == 14) {
        u64 cr2 = read_cr2();
        if (cr2 == 0 && !(regs->err_code & 0x10) && (regs->err_code & 0x04)) {
            kprintf("  CR2=0 + 用户态 + 数据访问：\n");
            kprintf("  典型的「代码被擦成全 0」症状 ——\n");
            kprintf("  CPU 把 00 00 解码成 add [rax],al，RAX=0 于是访问地址 0。\n");
            kprintf("  重点检查：是不是有东西覆盖了这段代码的物理页\n");
            kprintf("  （比如 PMM 位图、BSS 清零范围算错）。\n");
        }
    }
    kprintf("\n  寄存器现场 :\n");
    dump_registers(regs);

    kprintf("\n  内核已停止运行。\n");

    for (;;) {
        asm volatile("hlt");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  汇编地址表：isr_stubs.asm 里造的 256 个入口
// ---------------------------------------------------------------------------
extern "C" const void* isr_stub_table[];

namespace isr {

void init()
{
    // 1) 清空整张表（.bss 已被 boot64.asm 清零，这里再保险一次）
    for (int i = 0; i < IDT_ENTRIES; ++i) {
        u8* raw = reinterpret_cast<u8*>(&g_idt[i]);
        for (usize j = 0; j < sizeof(IdtEntry); ++j) {
            raw[j] = 0;
        }
        g_handlers[i] = nullptr;
    }

    // 2) 逐项填入入口地址
    for (int i = 0; i < IDT_ENTRIES; ++i) {
        u64 addr = reinterpret_cast<u64>(isr_stub_table[i]);

        g_idt[i].offset_low  = static_cast<u16>(addr & 0xFFFF);
        g_idt[i].selector    = KERNEL_CODE_SELECTOR;
        g_idt[i].ist         = 0;
        g_idt[i].type_attr   = IDT_ATTR_INTERRUPT_GATE;
        g_idt[i].offset_mid  = static_cast<u16>((addr >> 16) & 0xFFFF);
        g_idt[i].offset_high = static_cast<u32>((addr >> 32) & 0xFFFFFFFF);
        g_idt[i].zero        = 0;
    }

    // 3) 用 lidt 把表告诉 CPU
    g_idt_ptr.limit = static_cast<u16>(sizeof(IdtEntry) * IDT_ENTRIES - 1);
    g_idt_ptr.base  = reinterpret_cast<u64>(&g_idt[0]);
    asm volatile("lidt %0" : : "m"(g_idt_ptr));
}

void register_handler(u8 int_no, void (*handler)(const Registers*))
{
    g_handlers[int_no] = handler;
}

}  // namespace isr

// ---------------------------------------------------------------------------
//  中断分发总入口（由 isr_stubs.asm 调用）
// ---------------------------------------------------------------------------
extern "C" u64 isr_handler(const Registers* regs)
{
    // 0~31 是 CPU 异常，内核自己处理
    if (regs->int_no < 32) {
        handle_exception(regs);
        // handle_exception 是 [[noreturn]]，不会走到这里
    }

    // 32 及以上是外部中断（IRQ）
    handler_fn handler = g_handlers[regs->int_no];
    if (handler != nullptr) {
        handler(regs);
    }

    // 必须发 EOI（End Of Interrupt）告诉 PIC「这个中断处理完了」。
    // 忘了发，PIC 就一直认为这个 IRQ 还在处理中，后续同级别中断全部阻塞——
    // 典型症状是定时器只响一次、键盘按一个键之后再无反应。
    // 只给真实的 IRQ（32~47）发 EOI。
    // 阶段 6 起多了 int 0x81 这种「软件触发的调度中断」，
    // 它不是硬件中断，往 PIC 发 EOI 会误清掉正在处理的 IRQ 状态。
    if (regs->int_no >= 32 && regs->int_no < 48) {
        // 【微内核】先把中断投递给"注册了这个 IRQ 的用户态驱动进程"。
        //
        //   deliver_irq 以前**从来没有被调用过** —— 写好了却忘了接上，
        //   结果用户态驱动永远收不到中断通知，只能靠轮询碰运气。
        //   内核不解释中断含义，只负责通知，这是微内核的本分。
        ipc::deliver_irq(static_cast<u8>(regs->int_no - pic::IRQ_BASE));
        pic::send_eoi(static_cast<u8>(regs->int_no));
    }

    // 【阶段 6】调度点：所有中断的**出口**都在这里。
    //   sched::on_interrupt 会把当前 rsp 存进「当前线程」，
    //   挑出下一个该跑的线程，返回它的栈顶。
    //   没有切换需求时原样返回传入的 rsp。
    u64 next_rsp = sched::on_interrupt(reinterpret_cast<u64>(regs));
    sched::record_switch(next_rsp);

    return next_rsp;
}

const char* exception_name(u8 int_no)
{
    if (int_no < 32) {
        return kExceptionNames[int_no];
    }
    return "外部中断";
}
