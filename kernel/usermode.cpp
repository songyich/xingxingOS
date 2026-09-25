// ============================================================================
//  kernel/usermode.cpp —— 用户态进程创建
//  ---------------------------------------------------------------------------
//  宏内核里"创建一个程序"就是分配栈、跑起来，大家共用一个页表。
//  微内核/现代 OS 里，每个用户进程必须有**独立的地址空间**：
//    - 进程 A 的指针，在进程 B 里毫无意义（指向不同的物理内存）
//    - 用户进程看不到内核内存（页表项 U/S=0，用户态访问直接 #PF）
//    - 一个进程崩了，不会踩坏别的进程
//
//  创建步骤：
//    1. 建新页表（拷内核映射，用户空间留空）
//    2. 映射代码（内核镜像里的服务函数 → 用户空间，U/S=1 可执行）
//    3. 映射用户栈（U/S=1 可读写）
//    4. 分配内核栈（用户态陷入内核时用）
//    5. 伪造一个"刚从 Ring 3 陷入"的现场，让调度器切过去时直接进用户态
// ============================================================================

#include <kernel/usermode.hpp>
#include <kernel/elf.hpp>
#include <kernel/initrd.hpp>
#include <kernel/ipc.hpp>
#include <kernel/thread.hpp>
#include <kernel/vmm.hpp>
#include <kernel/pmm.hpp>
#include <kernel/heap.hpp>
#include <kernel/gdt.hpp>
#include <kernel/printf.hpp>
#include <kernel/serial.hpp>
#include <kernel/isr.hpp>

// 链接脚本定义的 .user_text 段边界（全局符号，必须在这里声明）
extern u64 _user_text_start;
extern u64 _user_text_end;
extern u64 _user_text_lma;
extern u64 _user_data_start;
extern u64 _user_data_end;
extern u64 _user_data_lma;

namespace {

// 内核临时映射窗口：用来读写刚分配的页表页
// （页表页是物理地址，CPU 只能用虚拟地址访问，得先映射一下）
//
// ⚠️ 地址不能随便选！
//   内核高半区 0xffffffff80000000 起是 **2MB 大页** 映射的，
//   vmm::map_page 遇到大页不会自动拆分（会直接报错）。
//   所以窗口必须放在用 4KB 页管理的区域——也就是 PML4[509]
//   （0xfffffe8000000000 起，堆所在的那个 512GB），
//   并且要避开堆本身。这里放在堆上方 256GB 处。
constexpr u64 TEMP_WINDOW = 0xfffffe9000000000ull;

// 把一页物理内存临时映射到内核虚拟地址，返回可写指针
u64* map_temp(u64 phys)
{
    vmm::map_page(TEMP_WINDOW, phys,
                  vmm::PAGE_PRESENT | vmm::PAGE_WRITABLE);
    return reinterpret_cast<u64*>(TEMP_WINDOW);
}

void unmap_temp()
{
    vmm::unmap_page(TEMP_WINDOW);
}

}  // namespace

namespace usermode {

void init()
{
    serial::puts("[OK]   usermode infra ready\n");
}

// ---------------------------------------------------------------------------
//  创建用户页表
// ---------------------------------------------------------------------------
u64 create_user_pml4()
{
    u64 pml4_phys = pmm::alloc_frame();
    if (pml4_phys == 0) {
        return 0;
    }

    // --- 清零 ---
    u64* pml4 = map_temp(pml4_phys);
    for (int i = 0; i < 512; ++i) {
        pml4[i] = 0;
    }

    // --- 拷贝内核映射（整段内核空间）---
    //
    // 为什么必须拷内核映射？
    //   用户进程陷入内核（syscall / 中断）时，CPU 切到 Ring 0 但**不换页表**。
    //   如果新页表里没有内核的映射，内核代码一执行就 #PF —— 系统直接崩。
    //
    // ⚠️ 拷哪些条目？这里踩过一个很隐蔽的坑：
    //   最早只拷了 511/510/509 三项（以为内核正文+MMIO+堆就这三个），
    //   结果一切到用户进程就崩。
    //   原因：堆的虚拟地址是 0xfffffe8000000000，
    //   它的 PML4 index 是 **464**（不是 509！）。
    //   PML4 index 的算法是 (addr >> 39) & 0x1FF：
    //     0xfffffe8000000000 >> 39 = 0x1D0 = 464
    //   而内核线程栈、现场结构都分配在堆上，
    //   没拷 464 就等于切页表后现场所在的内存凭空消失。
    //
    // 正确做法：x86_64 惯例是 PML4 的高半（index >= 256，
    // 即 0xffff800000000000 以上）全归内核，整段拷过去。
    // 低半（0~0x00007fffffffffff）是用户空间，一个都不拷——
    // 拷了就等于让用户进程直读全部物理内存，隔离形同虚设。
    u64* kpml4 = map_temp(vmm::pml4_phys());
    // map_temp 只有一个窗口，第二次调用会覆盖第一次。
    // 所以先把内核 PML4 的高半读到栈上暂存（256 项 = 2KB，内核栈够用）。
    u64 kernel_half[256];
    for (int i = 0; i < 256; ++i) {
        kernel_half[i] = kpml4[256 + i];
    }
    unmap_temp();

    pml4 = map_temp(pml4_phys);
    for (int i = 0; i < 256; ++i) {
        pml4[256 + i] = kernel_half[i];
    }
    unmap_temp();

    return pml4_phys;
}

// ---------------------------------------------------------------------------
//  映射用户页
// ---------------------------------------------------------------------------
bool map_user_pages(u64 virt, u64 phys, u64 count, bool executable)
{
    u64 flags = vmm::PAGE_PRESENT | vmm::PAGE_WRITABLE | vmm::PAGE_USER;
    if (!executable) {
        flags |= vmm::PAGE_NX;      // 数据页禁止执行（防代码注入）
    }
    return vmm::map_pages(virt, phys, count, flags);
}

// ---------------------------------------------------------------------------
//  创建用户进程
// ---------------------------------------------------------------------------
// IPC 共享页的**物理**地址。
// 所有用户进程的页表都把这一页映射到 USER_IPC_PAGE，
// 于是它们看到的是同一块物理内存 —— 消息天然跨进程可见。
static u64 g_ipc_page_phys = 0;

// 前向声明：create_user_process 会用到（完整定义在它后面）
void setup_user_frame(thread::Thread* t, u64 entry_virt);

int create_user_process(const char* name, u64 entry_virt, int priority)
{
    // --- 1. 独立页表 ---
    u64 pml4 = create_user_pml4();
    if (pml4 == 0) {
        kprintf("[usermode] 失败：无法创建用户页表\n");
        return -1;
    }

    // --- 2. 映射代码 ---
    // 入口函数在内核镜像里，先把它转成物理地址，
    // 再映射到用户空间的 USER_CODE_BASE。
    u64 entry_phys = vmm::get_phys(entry_virt);
    if (entry_phys == 0) {
        kprintf("[usermode] 失败：入口地址无法翻译 entry_virt=0x%llx\n", entry_virt);
        return -1;
    }

    // 映射**整个** .user_text 段（不是只映射入口那几页）
    //
    // 为什么要整段映射？
    //   服务函数之间会互相调用（shell 调 ulib::puts、put_uint 等）。
    //   只映射入口所在的 64KB 的话，调用到段外面的函数就 #PF，
    //   而且这种错误极难排查——看起来是"随机崩在某个函数"。
    //   链接脚本已经把 services.o 整个放进 .user_text 段，
    //   这里一次性映射 段起始~段结束，保证所有服务代码都在。
    //
    // ⚠️ 关键：不能先切 CR3 再映射！
    //   vmm 内部要通过内核窗口访问页表页的物理地址，
    //   切到新页表后那个窗口未必还在，访问就直接 #PF。
    //   所以全程待在内核页表下，用 map_pages_in 直接改**新页表**。
    u64 utext_start = reinterpret_cast<u64>(&_user_text_start);
    u64 utext_end   = reinterpret_cast<u64>(&_user_text_end);
    u64 utext_size  = (utext_end - utext_start + vmm::PAGE_SIZE - 1)
                      & ~(vmm::PAGE_SIZE - 1);
    u64 utext_pages = utext_size / vmm::PAGE_SIZE;
    if (utext_pages == 0) utext_pages = 1;

    // 直接用链接脚本给出的 LMA（物理加载地址）。
    //
    // ⚠️ 不能用 vmm::get_phys(utext_start)：
    //    utext_start 现在是**用户虚拟地址** 0x400000，
    //    内核页表里根本没有这一段的映射，翻译必然失败。
    u64 utext_phys = reinterpret_cast<u64>(&_user_text_lma);

    bool ok = vmm::map_pages_in(pml4,
                                USER_CODE_BASE,
                                utext_phys,
                                utext_pages,
                                vmm::PAGE_PRESENT | vmm::PAGE_WRITABLE
                                    | vmm::PAGE_USER);   // 代码段可执行

    if (!ok) {
        kprintf("[usermode] 失败：服务代码段映射失败\n");
        return -1;
    }


    // --- 2.4 映射用户态服务的数据段 ---
    //
    // 与代码段同理：services.o 的全局变量（鼠标坐标、按键状态等）
    // 必须落在用户空间，否则用户态一访问就 #PF。
    {
        u64 ud_start = reinterpret_cast<u64>(&_user_data_start);
        u64 ud_end   = reinterpret_cast<u64>(&_user_data_end);
        u64 ud_size  = (ud_end - ud_start + vmm::PAGE_SIZE - 1)
                       & ~(vmm::PAGE_SIZE - 1);
        u64 ud_pages = ud_size / vmm::PAGE_SIZE;
        u64 ud_phys  = reinterpret_cast<u64>(&_user_data_lma);

        if (ud_pages > 0) {
            if (!vmm::map_pages_in(pml4, USER_DATA_BASE, ud_phys, ud_pages,
                                   vmm::PAGE_PRESENT | vmm::PAGE_WRITABLE
                                       | vmm::PAGE_USER | vmm::PAGE_NX)) {
                kprintf("[usermode] 失败：服务数据段映射失败\n");
                return -1;
            }
        }
    }

    // --- 2.5 映射 IPC 共享页 ---
    //
    // 这是微内核 IPC 的关键设施：所有进程共享同一个物理页，
    // 消息放在固定虚拟地址上，**不依赖任何栈变量地址**。
    if (g_ipc_page_phys == 0) {
        g_ipc_page_phys = pmm::alloc_frames(1);
        if (g_ipc_page_phys == 0) {
            kprintf("[usermode] 失败：无法分配 IPC 共享页\n");
            return -1;
        }
        // 清零，避免残留数据被当成消息
        u8* zero = reinterpret_cast<u8*>(g_ipc_page_phys + 0xFFFFFFFF80000000ull);
        for (u64 i = 0; i < 4096; ++i) zero[i] = 0;
    }
    ok = vmm::map_pages_in(pml4, USER_IPC_PAGE, g_ipc_page_phys, 1,
                           vmm::PAGE_PRESENT | vmm::PAGE_WRITABLE
                               | vmm::PAGE_USER | vmm::PAGE_NX);
    if (!ok) {
        kprintf("[usermode] 失败：IPC 共享页映射失败\n");
        return -1;
    }

    // --- 3. 映射用户栈 ---
    u64 stack_phys = pmm::alloc_frames(USER_STACK_SIZE / vmm::PAGE_SIZE);
    if (stack_phys != 0 && ok) {
        u64 stack_base = USER_STACK_TOP - USER_STACK_SIZE;
        ok = vmm::map_pages_in(pml4, stack_base, stack_phys,
                               USER_STACK_SIZE / vmm::PAGE_SIZE,
                               vmm::PAGE_PRESENT | vmm::PAGE_WRITABLE
                                   | vmm::PAGE_USER | vmm::PAGE_NX);
    } else {
        ok = false;
    }

    if (!ok || stack_phys == 0) {
        kprintf("[usermode] 失败：映射失败 ok=%d stack=0x%llx\n",
                ok ? 1 : 0, stack_phys);
        return -1;
    }

    // --- 4. 创建线程 ---
    // 线程自带的内核栈（16KB）就够了，不用再单独分配。
    // 注意：create 传 nullptr 入口——我们不用它的蹦床机制，
    // 而是自己构造完整的用户态现场（见下方第 6 步）
    int tid = thread::create(name, nullptr, nullptr, priority);
    if (tid < 0) {
        kprintf("[usermode] 失败：thread::create 返回 -1（线程表可能已满）\n");
        return -1;
    }

    thread::Thread* t = thread::by_tid(tid);
    if (t == nullptr) {
        return -1;
    }

    // 标记为用户态进程，并记录页表与栈
    // kernel_stack_top 指向线程自带的内核栈：
    // 用户态陷入内核（syscall / 中断）时，CPU 会切到这个栈，
    // TSS.RSP0 也要指向它（在线程切换时由 gdt::set_kernel_stack 设置）
    t->is_user = true;
    t->cr3 = pml4;
    t->user_stack_top = USER_STACK_TOP;
    t->kernel_stack_top = t->stack_top;

    // --- 5. 伪造"从 Ring 3 陷入内核"的现场 ---
    setup_user_frame(t, entry_virt);
    return tid;
}

// ---------------------------------------------------------------------------
//  构造用户态现场（create 与 respawn 共用）
//  -------------------------------------------------------------------------
//  新进程（或刚被重启的进程）从未运行过，栈上没有现场。
//  手工摆一个 Registers，让调度器切过去、中断出口弹完寄存器后，
//  iret/sysret 直接进用户态。
//
//  关键字段：
//    cs     = 用户代码段 | RPL3（0x2b）—— iret 靠它决定返回哪个特权级
//    ss     = 用户数据段 | RPL3（0x23）
//    rip    = 用户代码入口
//    rsp    = 用户栈顶
//    rflags = 0x202（IF=1，让用户进程一上来就开着中断）
//
//  为什么 cs/ss 的最低 2 位必须是 3？
//    RPL=3 表示"以用户态身份访问"。iret 会根据它把 CPL 设成 3，
//    进程这才真正跑在 Ring 3。
// ---------------------------------------------------------------------------
void setup_user_frame(thread::Thread* t, u64 entry_virt)
{
    constexpr u64 FRAME = sizeof(Registers);

    Registers* r = reinterpret_cast<Registers*>(t->stack_top - FRAME);
    u8* p = reinterpret_cast<u8*>(r);
    for (usize i = 0; i < FRAME; ++i) p[i] = 0;

    r->ss     = gdt::USER_DATA_RPL3;
    r->rsp    = USER_STACK_TOP;
    r->rflags = 0x202;
    r->cs     = gdt::USER_CODE_RPL3;

    // 入口地址：服务代码现在就链接在用户地址空间，
    // 所以 entry_virt 本身就是用户态可直接执行的地址，无需换算。
    r->rip = entry_virt;

    r->int_no   = 0;
    r->err_code = 0;

    t->rsp = reinterpret_cast<u64>(r);

    // 【清理】原来这里有段"回读校验 rip/cs"的 kprintf，
    //   它会在**屏幕上**给每个服务进程刷一行调试信息（6 个服务 = 6 行），
    //   用户看到的是满屏 [usermode] rip=... cs=0x2b，非常干扰。
    //   纯开发期残留，已删除。需要时走 kprintf_serial。
    t->state = thread::State::READY;
}

// ---------------------------------------------------------------------------
//  重启一个已死亡的用户态服务（复用同一个 tid 槽位）
//  -------------------------------------------------------------------------
//  【为什么复用 tid，而不是新建进程？】
//    服务目录（svcdir）登记的正是 tid，各服务之间也直接持有 tid 常量
//    （TID_KEYBOARD=3 之类）。若重建时分配新 tid，
//    所有持有旧 tid 的引用立刻失效 —— 服务"复活了却没人找得到它"。
//    复用槽位则 tid 不变，其他服务完全无感。
//
//  【为什么不用重新建页表？】
//    页表还在（线程死了只是 state=DEAD，页表没释放），
//    代码段/数据段/IPC 共享页/用户栈的映射全都完好。
//    只要把栈指针重置回栈顶、现场重摆一遍即可 —— 零分配、无泄漏。
// ---------------------------------------------------------------------------
int respawn_user_process(int tid, u64 entry_virt)
{
    thread::Thread* t = thread::by_tid(tid);
    if (t == nullptr) return -1;
    if (!t->is_user)  return -1;

    // --- 清掉所有"上辈子"残留的等待状态 ---
    // 不清的话，重启后的服务一上来就以为自己在等消息/等中断，
    // 直接阻塞 —— 表现是"服务重启了但依旧没反应"。
    t->ipc_state       = thread::IpcState::NONE;
    t->ipc_recv_buf    = nullptr;
    t->ipc_wait_target = -1;
    t->ipc_from        = -1;
    t->irq_waiting     = -1;
    t->irq_received    = false;
    t->wait_tid        = -1;
    t->wake_time_ms    = 0;

    setup_user_frame(t, entry_virt);
    return 0;
}

// IPC 共享页的内核虚拟地址（supervisor 清残留状态时用）
u64 ipc_page_virt()
{
    if (g_ipc_page_phys == 0) return 0;
    return g_ipc_page_phys + 0xFFFFFFFF80000000ull;
}

// ---------------------------------------------------------------------------
//  从用户空间安全地拷贝一个字符串到内核
//  -------------------------------------------------------------------------
//  ⚠️ 为什么不能直接解引用用户指针？
//     每个进程有独立的页表，内核页表里**没有**用户进程的映射。
//     直接解引用会 #PF。必须先查当前进程的页表拿到物理地址，
//     再通过「物理地址 + 高半区偏移」访问。
//
//     进入 syscall 时 CR3 还是**调用进程**的页表，
//     所以 vmm::get_phys 查到的正是调用者的映射 —— 正确。
// ---------------------------------------------------------------------------
static bool copy_str_from_user(char* dst, u64 user_src, u64 maxlen)
{
    for (u64 i = 0; i < maxlen; ++i) {
        u64 va  = user_src + i;
        u64 off = va & 0xFFF;
        u64 phys = vmm::get_phys_current(va & ~0xFFFull);
        if (phys == 0) {
            dst[i] = '\0';
            return false;
        }
        u8 c = *reinterpret_cast<u8*>(phys + off + 0xFFFFFFFF80000000ull);
        dst[i] = static_cast<char>(c);
        if (c == '\0') return true;
    }
    dst[maxlen - 1] = '\0';
    return true;
}

static usize kstrlen(const char* s)
{
    usize n = 0;
    while (s != nullptr && s[n] != '\0') ++n;
    return n;
}

// ---------------------------------------------------------------------------
//  在用户栈上按 System V ABI 摆好 argc / argv
//  -------------------------------------------------------------------------
//  返回新的用户栈顶（rsp 初值）。
//
//  ⚠️ 写入必须通过物理地址 + 高半区：此刻我们在内核页表下，
//     目标进程的虚拟地址在这里无效。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  从用户空间读一个 8 字节指针值
//  -------------------------------------------------------------------------
//  ⚠️ 不能写 argv_user[i] 直接解引用！
//     argv_user 指向 shell **用户栈**上的数组，内核页表没有这块映射。
// ---------------------------------------------------------------------------
static bool read_user_ptr(u64 user_addr, u64* out)
{
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        u64 va   = user_addr + i;
        u64 off  = va & 0xFFF;
        u64 phys = vmm::get_phys_current(va & ~0xFFFull);
        if (phys == 0) return false;
        u8 b = *reinterpret_cast<u8*>(phys + off + 0xFFFFFFFF80000000ull);
        v |= (static_cast<u64>(b) << (i * 8));
    }
    *out = v;
    return true;
}

static u64 setup_user_stack(u64 stack_phys, u64 stack_virt_base, u64 stack_size,
                            int argc, char** argv_user,
                            int* out_argc, u64* out_argv)
{
    auto write8 = [&](u64 va, u8 v) {
        u64 p = stack_phys + (va - stack_virt_base);
        *reinterpret_cast<u8*>(p + 0xFFFFFFFF80000000ull) = v;
    };
    auto write64 = [&](u64 va, u64 v) {
        u64 p = stack_phys + (va - stack_virt_base);
        u8* q = reinterpret_cast<u8*>(p + 0xFFFFFFFF80000000ull);
        for (int i = 0; i < 8; ++i) q[i] = static_cast<u8>((v >> (i * 8)) & 0xFF);
    };

    constexpr int MAX_ARGS = 15;
    int n = (argc > MAX_ARGS) ? MAX_ARGS : argc;
    u64 arg_ptrs[MAX_ARGS];

    u64 sp = stack_virt_base + stack_size;      // = USER_STACK_TOP

    for (int i = n - 1; i >= 0; --i) {
        char buf[128];
        buf[0] = '\0';

        if (argv_user != nullptr) {
            // argv[i] 这个**指针本身**也存在用户空间，不能直接解引用
            u64 elem_addr = reinterpret_cast<u64>(&argv_user[i]);
            u64 str_addr = 0;
            if (read_user_ptr(elem_addr, &str_addr) && str_addr != 0) {
                copy_str_from_user(buf, str_addr, 127);
            }
        }
        u64 len = kstrlen(buf);
        sp -= (len + 1);
        sp &= ~0xFull;
        for (u64 k = 0; k <= len; ++k) {
            write8(sp + k, static_cast<u8>(buf[k]));
        }
        arg_ptrs[i] = sp;
    }

    sp -= static_cast<u64>(n + 1) * 8;
    sp &= ~0xFull;
    u64 argv_base = sp;
    for (int i = 0; i < n; ++i) write64(argv_base + static_cast<u64>(i) * 8, arg_ptrs[i]);
    write64(argv_base + static_cast<u64>(n) * 8, 0);

    sp -= 8;
    sp &= ~0xFull;
    write64(sp, static_cast<u64>(n));

    // 同时把 argc / argv 通过**寄存器**传给 _start。
    //
    //  为什么不用"让 _start 自己从栈上读"？
    //    那条路踩了两个坑：
    //      1) 普通函数有 prologue（push rbp），进到函数体时 rsp 已不指向 argc
    //      2) 改用 naked + `jmp _xstart` 又会产生**重定位**，
    //         而内核 ELF 加载器（故意）不做重定位 → 跳到错误地址崩溃
    //         （实测 CR2=0x2 取指令异常）
    //
    //  用寄存器传参是最稳的：遵循 System V ABI（rdi=第1参, rsi=第2参），
    //  _start 写成普通函数即可，编译器自动正确读取，无需 naked、无重定位。
    if (out_argc) *out_argc = n;
    if (out_argv) *out_argv = argv_base;

    return sp;
}

// ---------------------------------------------------------------------------
//  create_elf_process：从 initrd 里的 ELF 映像创建一个用户进程
//  -------------------------------------------------------------------------
//  **真正的外部程序**：从 ELF 文件加载，有独立的 argc/argv。
//  这是「万物皆可程序」的核心能力。
// ---------------------------------------------------------------------------
int create_elf_process(const char* prog_name, int argc, char** argv_user)
{
    const void* img = nullptr;
    u64 img_size = 0;
    if (!initrd::find(prog_name, &img, &img_size)) {
        kprintf("[usermode] 找不到程序：%s\n", prog_name);
        return -1;
    }

    u64 pml4 = create_user_pml4();
    if (pml4 == 0) {
        kprintf("[usermode] 无法创建页表\n");
        return -1;
    }

    if (g_ipc_page_phys == 0) {
        g_ipc_page_phys = pmm::alloc_frames(1);
        if (g_ipc_page_phys == 0) return -1;
        u8* zero = reinterpret_cast<u8*>(g_ipc_page_phys + 0xFFFFFFFF80000000ull);
        for (u64 i = 0; i < 4096; ++i) zero[i] = 0;
    }
    if (!vmm::map_pages_in(pml4, USER_IPC_PAGE, g_ipc_page_phys, 1,
                           vmm::PAGE_PRESENT | vmm::PAGE_WRITABLE
                               | vmm::PAGE_USER | vmm::PAGE_NX)) {
        kprintf("[usermode] IPC 共享页映射失败\n");
        return -1;
    }

    u64 stack_phys = pmm::alloc_frames(USER_STACK_SIZE / vmm::PAGE_SIZE);
    if (stack_phys == 0) {
        kprintf("[usermode] 用户栈分配失败\n");
        return -1;
    }
    u64 stack_base = USER_STACK_TOP - USER_STACK_SIZE;
    if (!vmm::map_pages_in(pml4, stack_base, stack_phys,
                           USER_STACK_SIZE / vmm::PAGE_SIZE,
                           vmm::PAGE_PRESENT | vmm::PAGE_WRITABLE
                               | vmm::PAGE_USER | vmm::PAGE_NX)) {
        kprintf("[usermode] 用户栈映射失败\n");
        return -1;
    }

    u64 entry = 0, img_end = 0;
    if (!elf::load(img, img_size, pml4, &entry, &img_end)) {
        kprintf("[usermode] ELF 加载失败：%s\n", prog_name);
        return -1;
    }

    int  n_argc = 0;
    u64  u_argv = 0;
    u64 user_rsp = setup_user_stack(stack_phys, stack_base, USER_STACK_SIZE,
                                    argc, argv_user, &n_argc, &u_argv);

    int tid = thread::create(prog_name, nullptr, nullptr, thread::PRIO_NORMAL);
    if (tid < 0) {
        kprintf("[usermode] 线程创建失败\n");
        return -1;
    }

    thread::Thread* t = thread::by_tid(tid);
    if (t == nullptr) return -1;

    t->is_user = true;
    t->cr3 = pml4;
    t->user_stack_top = USER_STACK_TOP;
    t->kernel_stack_top = t->stack_top;

    constexpr u64 FRAME = sizeof(Registers);
    Registers* r = reinterpret_cast<Registers*>(t->stack_top - FRAME);
    u8* p = reinterpret_cast<u8*>(r);
    for (usize i = 0; i < FRAME; ++i) p[i] = 0;

    r->ss     = gdt::USER_DATA_RPL3;
    r->rsp    = user_rsp;
    r->rflags = 0x202;
    r->cs     = gdt::USER_CODE_RPL3;
    r->rip    = entry;

    // 按 System V ABI 传参给 _start(int argc, char** argv)
    r->rdi    = static_cast<u64>(n_argc);
    r->rsi    = u_argv;

    r->int_no   = 0;
    r->err_code = 0;

    t->rsp = reinterpret_cast<u64>(r);
    t->state = thread::State::READY;

    // 【改走串口】启动程序是**每次执行命令**都打的，
    //   放屏幕上会夹在命令输出之间，看起来像乱码。
    //   调试需要时用 -serial file:xxx.log 抓取即可。
    kprintf_serial("[usermode] 启动程序 %s -> tid=%d entry=0x%llx\n",
                   prog_name, tid, entry);
    return tid;
}

// ---------------------------------------------------------------------------
//  spawn_program：系统调用入口
// ---------------------------------------------------------------------------
int spawn_program(u64 name_ptr, u64 name_len, u64 argv_ptr, u64 argc)
{
    char name[32];
    u64 nl = name_len;
    if (nl >= 31) nl = 31;

    for (u64 i = 0; i < nl; ++i) {
        u64 va  = name_ptr + i;
        u64 off = va & 0xFFF;
        u64 phys = vmm::get_phys_current(va & ~0xFFFull);
        if (phys == 0) { name[i] = '\0'; break; }
        name[i] = static_cast<char>(
            *reinterpret_cast<u8*>(phys + off + 0xFFFFFFFF80000000ull));
    }
    name[nl] = '\0';

    char** argv = reinterpret_cast<char**>(argv_ptr);
    return create_elf_process(name, static_cast<int>(argc), argv);
}

}  // namespace usermode
