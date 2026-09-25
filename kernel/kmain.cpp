// ============================================================================
//  kernel/kmain.cpp —— C++ 内核入口（阶段 5：命令行 shell）
//  ---------------------------------------------------------------------------
//  到这一阶段，内核已经有了：屏幕、键盘、定时器、物理内存、虚拟内存、内核堆。
//  于是终于可以做一个真正能用的交互环境了。
//
//  初始化顺序（依赖关系决定，不能乱）：
//    串口 -> 终端 -> 全局构造 -> PMM -> VMM -> 堆 -> 中断 -> 定时器 -> 键盘 -> shell
//    堆依赖 VMM（扩张要映射新页），VMM 依赖 PMM（页表层要申请物理页），
//    shell 依赖中断（要读键盘）和堆（要缓存命令行）。
// ============================================================================

#include <kernel/types.h>
#include <kernel/terminal.hpp>
#include <kernel/framebuffer.hpp>
#include <kernel/serial.hpp>
#include <kernel/printf.hpp>
#include <kernel/panic.hpp>
#include <kernel/multiboot2.hpp>
#include <kernel/pmm.hpp>
#include <kernel/vmm.hpp>
#include <kernel/heap.hpp>
#include <kernel/isr.hpp>
#include <kernel/pic.hpp>
#include <kernel/acpi.hpp>
#include <kernel/mouse.hpp>
#include <kernel/log.hpp>
#include <kernel/initrd.hpp>
#include <kernel/svcdir.hpp>
#include <kernel/pit.hpp>
#include <kernel/keyboard.hpp>
#include <kernel/shell.hpp>
#include <kernel/supervisor.hpp>
#include <kernel/thread.hpp>
#include <kernel/gdt.hpp>
#include <kernel/ipc.hpp>
#include <kernel/syscall.hpp>
#include <kernel/usermode.hpp>
#include <kernel/io.h>
#include <kernel/bootanim.hpp>
#include <kernel/bsod.hpp>

extern "C" void call_global_constructors();

bool g_global_ctor_ran = false;

// shell 的 lsmem 命令要用，保存下来供查询
u64 g_mb2_info_phys = 0;

namespace {

struct ConstructorProbe {
    ConstructorProbe() { g_global_ctor_ran = true; }
};
ConstructorProbe g_constructor_probe;

constexpr int LINE_WIDTH = 62;

void print_line()
{
    for (int i = 0; i < LINE_WIDTH; ++i) kprintf("=");
    kprintf("\n");
}

void banner()
{
    // 这个 ASCII art 是用真实字体（DejaVuSansMono-Bold 14pt）渲染后
    // 逐像素转成 '#' 生成的，不是手工拼的。
    //
    // 为什么强调这点：前一版是手写的，形状根本没拼对，
    // 屏幕上显示成了谁也不认识的 "luahx OS"。
    // 手工拼多字母 art 极易出错，尤其 x / i / n / g 这类笔画密集的字母。
    //
    // 改文字时重跑生成脚本即可：PIL 渲染 -> getbbox 裁空白 -> 转 '#' 点阵。
    term::set_color(fb::color::LIGHT_CYAN, fb::color::BLACK);
    kprintf("\n");
    kprintf("               ###                               ###\n");
    kprintf("               ###                               ###                     ####     ####\n");
    kprintf("                                                                        ### ##   ##   #\n");
    kprintf("     ##  ### #####    ######   ######  ##  ### #####    ######   ###### ##  ###  ##\n");
    kprintf("      ## ##    ###    ### ##  ##  ###   ## ##    ###    ### ##  ##  ### ##   ##  ###\n");
    kprintf("      ####     ###    ##  ##  ##  ###   ####     ###    ##  ##  ##  ### ##   ##  #####\n");
    kprintf("       ###     ###    ##  ##  ##  ###    ###     ###    ##  ##  ##  ### ##   ##    ####\n");
    kprintf("       ###     ###    ##  ##  ##  ###    ###     ###    ##  ##  ##  ### ##   ##      ###\n");
    kprintf("      ####     ###    ##  ##  ##  ###   ####     ###    ##  ##  ##  ### ##  ###      ###\n");
    kprintf("     ### ##    ###    ##  ##  ##  ###  ### ##    ###    ##  ##  ##  ### ### ##   #   ##\n");
    kprintf("     ##  ### #######  ##  ##   ######  ##  ### #######  ##  ##   ######  ####     ####\n");
    kprintf("                                  ##                                ##\n");
    kprintf("                              #   ##                            #   ##\n");
    kprintf("                               ####                              ####\n");
    term::set_color(fb::color::WHITE, fb::color::BLACK);
    kprintf("\n");
}

}  // namespace


// 用户态服务进程的入口（定义在 kernel/services.cpp，运行在 Ring 3）
extern "C" void terminal_service_entry();
extern "C" void keyboard_service_entry();
extern "C" void timer_service_entry();
extern "C" void power_service_entry();
extern "C" void proc_service_entry();
extern "C" void shell_service_entry();
extern "C" void mouse_service_entry();

// 极早期串口探针：直接写端口，不依赖任何初始化
// （排查"完全无输出"时用，确认 CPU 到底有没有走到 kmain）
static void probe(char c)
{
    // 等待发送缓冲区空
    for (int i = 0; i < 100000; ++i) {
        if ((inb(0x3F8 + 5) & 0x20) != 0) break;
    }
    outb(0x3F8, static_cast<u8>(c));
}

extern "C" void kmain(u32 multiboot_magic, u64 multiboot_info_phys)
{
    probe('K');            // 探针：进 kmain 了

    g_mb2_info_phys = multiboot_info_phys;

    // --- 1. 串口优先：它不依赖屏幕，是最早可用的输出通道 ---
    serial::init();
    // 统一日志系统（串口输出）：必须在 serial 之后。
    // 之后所有模块的 LOG_XXX() 才会真正输出。
    klog::init();
    probe('1');            // 探针：串口 OK

    // --- 2. 文本终端 ---
    term::init(multiboot_info_phys);

    // --- 3. 全局构造函数 ---
    call_global_constructors();

    term::set_color(fb::color::WHITE, fb::color::BLACK);

    // --- 3.5 开机动画 ①②：星星螺旋入场 + 收缩消失 ---
    //   必须在 term::init 之后（需要帧缓冲），
    //   且在 pmm::init 之前（此时还没有内存管理器，只能画在真帧缓冲上）。
    //   动画期间屏幕被清空，加载过程紧接着在离屏里进行。
    bootanim::play_intro();

    // --- 4~6. 内存三件套：PMM -> VMM -> 堆 ---
    pmm::init(multiboot_info_phys);

    // 【检测点 1：内存 / PMM】致命 —— 没有内存管理器后面全完蛋
    if (pmm::frame_count() == 0 || pmm::total_memory() == 0) {
        bsod::fatal("内存 / PMM 初始化",
                    "没有可用物理内存（帧数为 0）",
                    "无法建立内存管理，系统无法继续运行");
    }

    vmm::init();
    heap::init();

    // --- 6.5 开机动画 ③：切到离屏缓冲 ---
    //   之后所有界面绘制（横幅、自检、服务就绪）都进离屏，
    //   屏幕保持全黑，直到加载完成后再上移揭示。
    //   失败则降级：界面直接画在屏幕上，跳过上移动画。
    bootanim::begin_offscreen();

    // -------------------------------------------------------------------
    //  【修复】补回被清屏抹掉的早期诊断行
    // -------------------------------------------------------------------
    //  PMM / VMM / HEAP 的初始化信息是在 begin_offscreen() **之前**
    //  用 kprintf 输出的 —— 那时还画在真帧缓冲上，
    //  而 begin_offscreen() 会把真屏 clear 成黑色（加载期间保持黑屏），
    //  于是这几行**从最终界面上消失了**（你反馈的"最上面的诊断少了几行"）。
    //
    //  它们已经写进串口，所以这里在离屏缓冲里**重画一遍**：
    //  内容一样，但这次会随"背景上移"出现在屏幕上。
    // -------------------------------------------------------------------
    kprintf("[PMM] 初始化完成：可分配 %llu 个页帧（%llu MB）\n",
            pmm::frame_count(), pmm::total_memory() / (1024 * 1024));
    kprintf("[VMM] 接管 boot 页表，PML4 物理地址 = 0x%llx\n", vmm::pml4_phys());
    kprintf("[HEAP] 初始化完成：起始 0x%llx，初始 %llu KB\n",
            heap::HEAP_START, heap::total_bytes() / 1024);

    // --- 7~9. 中断三件套：IDT -> 8259 -> 设备 ---
    isr::init();
    pic::init();
    pit::init(pit::DEFAULT_FREQUENCY);
    keyboard::init();

    // --- 9.2 initrd：解析嵌入的 .xzs 程序（万物皆可程序）---
    initrd::init();

    // 【检测点 2：initrd / 程序加载】非致命 —— 按 Enter 可继续
    //   （系统能起来，只是命令不可用，属于可降级运行）
    if (!initrd::ready() || initrd::count() == 0) {
        // 【诊断】文案里带上**真实**检测值，而不是写死"共 0 个" ——
        //   上次就是这样，日志明明显示"8 个程序"却报"共 0 个"，
        //   写死的文案掩盖了真实原因，无法定位。
        char detail[64];
        const char* dig = "0123456789";
        int n = initrd::count();
        int p2 = 0;
        const char* pre = "initrd 检测失败：ready=";
        for (const char* q = pre; *q; ++q) detail[p2++] = *q;
        detail[p2++] = initrd::ready() ? '1' : '0';
        detail[p2++] = ' ';
        detail[p2++] = 'c'; detail[p2++] = 'o'; detail[p2++] = 'u';
        detail[p2++] = 'n'; detail[p2++] = 't'; detail[p2++] = '=';
        if (n == 0) { detail[p2++] = '0'; }
        else {
            char tmp[12]; int t = 0;
            while (n > 0) { tmp[t++] = dig[n % 10]; n /= 10; }
            while (t > 0) detail[p2++] = tmp[--t];
        }
        detail[p2] = '\0';

        kprintf_serial("[BSOD-REAL] ready=%d count=%d\n",
                       static_cast<int>(initrd::ready()), initrd::count());

        bsod::warn("initrd / 程序加载", detail,
                   "命令程序无法运行，命令将不可用，但系统可继续启动");
    }

    // --- 9.3 服务目录 /system/services ---
    svcdir::init();

    // --- 9.4 鼠标指针层 ---
    //   ⚠️【位置调整】原来 mouse::init() 在这里（离屏模式期间）初始化，
    //   会把指针画进**离屏缓冲**，随后被"背景上移"推上屏幕变成静态残影，
    //   与之后画的真指针同时显示 —— 用户看到"两个鼠标"。
    //
    //   现在这里只做**服务层面**的准备，指针的绘制推迟到
    //   bootanim::finish() 之后（渲染目标已切回真帧缓冲）再执行。
    //   真正的 PS/2 驱动在用户态 mouse 服务里。

    // --- 9.5 ACPI：解析电源管理表，让 shutdown 能真断电 ---
    // 放在 heap/vmm 之后（要用高半区访问 1MB 以下的 ACPI 表）
    acpi::init();

    // --- 10. 线程子系统：把「当前执行流」包装成 0 号线程，并建 idle ---
    // 必须在开中断**之前**完成，否则第一次中断进来时还没有当前线程可保存。
    thread::init();

    // --- 11. 微内核地基：GDT（含 Ring 3 段 + TSS）与系统调用 ---
    // 顺序有讲究：
    //   gdt::init() 必须在 syscall::init() 之前——
    //   syscall 的 sysret 要用到 GDT 里的用户段，段还没建好就返回用户态会 #GP。
    gdt::init();
    syscall::init();
    ipc::init();
    usermode::init();

    // --- 12. 创建用户态服务进程 ---
    // 这就是微内核和宏内核的分界线：
    // 终端、键盘、定时器、shell 全部作为**用户态进程**运行，
    // 内核只留下调度、内存、IPC 和中断分发。
    // 微内核的核心：终端、键盘、定时器、shell 全是**用户态进程**，
    // 内核只保留调度、内存、IPC 和中断分发。
    int tid_term = usermode::create_user_process(
        "terminal", reinterpret_cast<u64>(&terminal_service_entry), thread::PRIO_HIGH);
    int tid_kbd = usermode::create_user_process(
        "keyboard", reinterpret_cast<u64>(&keyboard_service_entry), thread::PRIO_HIGH);
    int tid_timer = usermode::create_user_process(
        "timer", reinterpret_cast<u64>(&timer_service_entry), thread::PRIO_NORMAL);
    int tid_shell = usermode::create_user_process(
        "shell", reinterpret_cast<u64>(&shell_service_entry), thread::PRIO_NORMAL);
    // ⚠️ 创建**顺序**决定 tid，必须与 ipc.hpp 里的 TID_* 常量一致：
    //   terminal=2 keyboard=3 timer=4 shell=5 power=6 proc=7
    // 曾经把 power/proc 插在 shell 前面，导致 shell 拿到 tid=7，
    // 而代码里 TID_SHELL=5 —— 服务之间互相找不到对方，命令全部无响应。
    int tid_power = usermode::create_user_process(
        "power", reinterpret_cast<u64>(&power_service_entry), thread::PRIO_NORMAL);
    int tid_proc = usermode::create_user_process(
        "proc", reinterpret_cast<u64>(&proc_service_entry), thread::PRIO_NORMAL);
    // 鼠标服务（用户态驱动）—— 与键盘平级，符合「万物皆可程序」：
    // 设备驱动就是一个普通的用户态进程，崩了不影响内核。
    int tid_mouse = usermode::create_user_process(
        "mouse", reinterpret_cast<u64>(&mouse_service_entry), thread::PRIO_NORMAL);
    // 【改走串口】这行会打印一堆 tid 数字，放屏幕上像调试信息。
    //   原来用 kprintf（同时写屏幕和串口），现在只写串口。
    //   屏幕上保留一行干净的"服务已就绪"即可（见下方）。
    kprintf_serial("[OK] 用户态服务进程已创建: 终端=%d 键盘=%d 定时器=%d\n",
                   tid_term, tid_kbd, tid_timer);

    // 【检测点 3：服务进程创建】
    //   核心服务（终端/键盘/shell）失败 = 致命，系统没法用；
    //   非核心（定时器/电源/进程/鼠标）失败 = 非致命，按 Enter 继续。
    if (tid_term <= 0 || tid_kbd <= 0 || tid_shell <= 0) {
        bsod::fatal("服务进程创建",
                    "核心服务（终端 / 键盘 / shell）创建失败",
                    "无法显示、无法输入，系统无法继续运行");
    }
    if (tid_timer <= 0 || tid_power <= 0 || tid_proc <= 0) {
        bsod::warn("服务进程创建",
                   "非核心服务（定时器 / 电源 / 进程）创建失败",
                   "部分功能不可用，但系统可继续启动");
    }
    // 鼠标服务失败不算环境问题（QEMU 里就没有真实鼠标），只在串口记一笔
    if (tid_mouse <= 0) {
        kprintf_serial("[warn] 鼠标服务未创建（无 PS/2 鼠标设备？）\n");
    }

    // --- 12.5 【P2 崩溃自愈】把服务交给监管者看护 ---
    //
    //   登记之后，任何一个服务崩溃都只会被这一个进程"带走"，
    //   内核继续跑，监管者会在 200ms 后把它重新拉起来（复用同一个 tid）。
    //
    //   这就是微内核隔离收益真正兑现的地方：
    //   kill 掉键盘服务 → 系统不挂 → 键盘几秒内恢复可用。
    supervisor::init();
    if (tid_term  > 0) supervisor::watch(tid_term,  "terminal",
            reinterpret_cast<u64>(&terminal_service_entry),  thread::PRIO_HIGH);
    if (tid_kbd   > 0) supervisor::watch(tid_kbd,   "keyboard",
            reinterpret_cast<u64>(&keyboard_service_entry),  thread::PRIO_HIGH);
    if (tid_timer > 0) supervisor::watch(tid_timer, "timer",
            reinterpret_cast<u64>(&timer_service_entry),     thread::PRIO_NORMAL);
    if (tid_shell > 0) supervisor::watch(tid_shell, "shell",
            reinterpret_cast<u64>(&shell_service_entry),     thread::PRIO_NORMAL);
    if (tid_power > 0) supervisor::watch(tid_power, "power",
            reinterpret_cast<u64>(&power_service_entry),     thread::PRIO_NORMAL);
    if (tid_proc  > 0) supervisor::watch(tid_proc,  "proc",
            reinterpret_cast<u64>(&proc_service_entry),      thread::PRIO_NORMAL);
    if (tid_mouse > 0) supervisor::watch(tid_mouse, "mouse",
            reinterpret_cast<u64>(&mouse_service_entry),     thread::PRIO_NORMAL);
    // --- 服务注册：把各服务登记到 /system/services/<name> ---
    //
    //   这样别的程序可以按**名字**找服务，不用记 TID。
    //   解决了"服务创建顺序一变 TID 就全乱"的坑。
    svcdir::register_service("terminal", 8, TID_TERMINAL);
    svcdir::register_service("keyboard", 8, TID_KEYBOARD);
    svcdir::register_service("timer",    5, TID_TIMER);
    svcdir::register_service("shell",    5, TID_SHELL);
    svcdir::register_service("power",    5, TID_POWER);
    svcdir::register_service("proc",     4, TID_PROC);
    svcdir::register_service("mouse",    5, TID_MOUSE);

    kprintf_serial(" 电源=%d 进程=%d Shell=%d\n", tid_power, tid_proc, tid_shell);
    // 屏幕上只留这一行（不含 tid 数字，干净）
    //
    // 【补充】之前把启动诊断全改走串口，屏幕上方变得太空，
    //   用户反馈"最上面的诊断少了几行"。这里补回几行**格式整齐**的，
    //   既保留信息量，又不会像原始调试输出那样像乱码。
    kprintf("  [OK] initrd 程序 : %d 个\n", initrd::count());
    kprintf("  [OK] 服务进程    : 终端 键盘 定时器 Shell 电源 进程 鼠标\n");
    kprintf("  [OK] ACPI 电源管理\n");
    kprintf("  [OK] 6 个用户态服务已就绪\n");

    // --- 13. 开中断 ---
    //  ⚠️【修复】开中断必须放在开机动画**之后**
    //  ---------------------------------------------------------------
    //  原来 sti 在动画之前，shell 服务进程立刻开始跑并输出提示符。
    //  而 play_outro 的上移循环每帧都把离屏整屏拷到真屏 ——
    //  shell 画到真屏上的提示符会被下一帧**覆盖掉**，
    //  且 shell 只输出一次、不会重画，于是用户看到"没有提示符"。
    //
    //  现在推迟到动画结束后再开中断：shell 此时才启动，
    //  提示符直接画在真屏上，不会被任何东西覆盖。
    //  动画期间是忙等、不依赖调度，推迟 sti 没有副作用。
    // ---------------------------------------------------------------

    // 开机自检（只留一行汇总，不刷屏；详细测试用 test 命令）
    const bool selfcheck = g_global_ctor_ran
                        && (multiboot_magic == 0x36D76289u)
                        && heap::ready()
                        && (pmm::frame_count() > 0);

    banner();
    kprintf("  ");
    term::set_color(fb::color::LIGHT_CYAN, fb::color::BLACK);
    // 【更新】原来写"第六阶段 : 多任务与调度"，早已过时。
    //   现在是微内核 + 万物皆可程序，版本信息要如实反映。
    kprintf("微内核操作系统   P1 : 万物皆可程序\n");
    term::set_color(fb::color::WHITE, fb::color::BLACK);
    print_line();

    kprintf("  内存 ");
    term::set_color(fb::color::LIGHT_GREEN, fb::color::BLACK);
    kprintf("%llu MB", pmm::total_memory() / (1024 * 1024));
    term::set_color(fb::color::WHITE, fb::color::BLACK);
    kprintf("  |  堆 ");
    term::set_color(fb::color::LIGHT_GREEN, fb::color::BLACK);
    kprintf("%llu KB", heap::total_bytes() / 1024);
    term::set_color(fb::color::WHITE, fb::color::BLACK);
    kprintf("  |  自检 ");
    if (selfcheck) {
        term::set_color(fb::color::LIGHT_GREEN, fb::color::BLACK);
        kprintf("通过");
    } else {
        term::set_color(fb::color::LIGHT_RED, fb::color::BLACK);
        kprintf("失败");
    }
    term::set_color(fb::color::WHITE, fb::color::BLACK);
    kprintf("\n");
    print_line();

    // --- 内核初始化完成，让位给用户态服务进程 ---
    // kmain 所在的 0 号线程就此退居二线，变成 idle：
    // 真正的工作都在用户态的 shell / 终端 / 键盘服务里完成。
    kprintf("\n  内核初始化完成，已切换到用户态服务进程。\n");
    kprintf("  内核现在只保留：调度、内存、IPC、中断分发。\n");

    // --- 14. 开机动画 ④⑤：星星放出 + 背景上移揭示界面 ---
    //   加载已完成，此刻把界面推上屏幕。
    bootanim::play_outro();
    bootanim::finish();

    // 渲染目标已切回真帧缓冲 —— 此刻才初始化并绘制鼠标指针，
    // 保证指针画在真屏上、且只有这一个。
    mouse::init();
    mouse::show();

    // 动画结束后才开中断 → shell 与鼠标服务开始运行。
    // 此时渲染目标已是真屏，shell 的提示符不会被动画覆盖。
    asm volatile("sti");

    for (;;) {
        asm volatile("sti; hlt");
    }
}
