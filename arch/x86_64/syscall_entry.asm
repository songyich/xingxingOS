; ============================================================================
;  arch/x86_64/syscall_entry.asm —— 系统调用入口（Ring 3 -> Ring 0）
;  ---------------------------------------------------------------------------
;  为什么用 syscall 指令而不是 int 0x80？
;    int 0x80 要走完整中断门流程：查 IDT、权限检查、压栈……
;    syscall 是专门设计的**快速路径**，微内核里 IPC 每秒上万次，
;    这个差距会被放大。
;
;  syscall 硬件做的事（必须清楚它的规矩）：
;    - 返回地址放进 **rcx**（不是压栈！）
;    - RFLAGS 放进 **r11**
;    - CS/SS 从 IA32_STAR 加载（切到 Ring 0）
;    - **不切换栈** ← 最关键，必须自己切
;    - 用 IA32_FMASK 清掉指定 RFLAGS 位（我们清 IF，进内核先关中断）
;
;  因为硬件不切栈，第一件事必须是 swapgs + 手动换 rsp。
;  否则我们就跑在**用户栈**上——用户能随便改自己的栈，
;  给内核一个假栈指针就能完全劫持系统。
;
;  【重要设计】这里构造出和中断**完全一样的 Registers 结构**。
;  好处：线程切换可以复用中断出口那套逻辑（mov rsp, rax），
;  syscall 和中断两条路径共用一套切换代码，不容易出 bug。
; ============================================================================
BITS 64

section .text

extern syscall_handler              ; C++ 分发函数，返回"接下来要恢复的现场"
extern g_syscall_rsp                ; 内核栈顶（全局变量，由调度器在切线程时更新）
extern g_syscall_count              ; 进入次数计数器（调试用）
extern isr_restore_exit             ; 统一出口（在 isr_stubs.asm）
extern isr_restore_exit_dbg         ; 诊断版出口

; 段选择子（与 gdt.hpp 保持一致）
USER_CODE_RPL3  equ 0x2b            ; 用户代码段 | RPL 3
USER_DATA_RPL3  equ 0x23            ; 用户数据段 | RPL 3

global syscall_entry
syscall_entry:
        ; 入口状态：
        ;   rax = 系统调用号
        ;   rdi rsi rdx r10 r8 r9 = 参数
        ;   rcx = 返回地址（用户 RIP）
        ;   r11 = RFLAGS
        ;   rsp = 用户栈

        ; --- 0. 计数器（调试用：确认到底有没有进到这里）---
        ; inc 只影响 flags，不破坏任何参数寄存器，可以安全地放在最前面
        inc     qword [rel g_syscall_count]

        ; --- 1. 切换到内核栈 ---
        ; 需要暂存"用户 rsp"，但要**先保护好 rbx**。
        ;
        ; 【致命 bug 修复】原来直接写 mov rbx, rsp：
        ;   这一步把用户的 rbx **原值覆盖掉了**。
        ;   后面 push rbx 本意是"保存用户的 rbx"，
        ;   实际存进去的却是"用户 rsp"。
        ;   于是 iretq 返回时 pop rbx 弹回的是栈地址（如 0x7fffffc0），
        ;   而不是用户原本放在 rbx 里的值。
        ;
        ;   rbx 是 callee-saved 寄存器，编译器常把循环不变量
        ;   （比如 IPC 共享页地址）缓存在里面。它一旦被污染，
        ;   用户态重试 syscall 时就会把栈地址当成消息地址传进来 ——
        ;   表现为"前几次正常、被唤醒后参数就错"，极难定位。
        ;
        ; 解法：先把 rbx 原值和用户 rsp 存进临时槽，
        ;       切栈后再分别压入正确的位置。
        mov     [rel g_tmp_rbx], rbx        ; 保护用户 rbx 原值
        mov     rbx, rsp                    ; rbx = 用户 rsp
        mov     [rel g_tmp_rsp], rbx        ; 也存一份
        mov     rsp, [rel g_syscall_rsp]    ; 加载内核栈顶

        ; 到此已在内核栈上，可以安全压栈

        ; --- 3. 构造 Registers 结构 ---
        ; 结构从低地址到高地址：
        ;   r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rbx rax
        ;   int_no err_code rip cs rflags rsp ss
        ; 压栈是从高往低，所以**倒序**压：先压 ss，最后压 r15。

        push    USER_DATA_RPL3              ; ss
        push    qword [rel g_tmp_rsp]       ; rsp（用户栈，入口时暂存的）
        push    r11                         ; rflags
        push    USER_CODE_RPL3              ; cs
        push    rcx                         ; rip（syscall 放这里）
        push    0                           ; err_code（系统调用没有）
        push    rax                         ; int_no：这里存系统调用号
        push    rax                         ; rax
        push    qword [rel g_tmp_rbx]       ; rbx ← 用户真正的 rbx 原值
        push    rcx                         ; rcx（值已无意义，位置要占）
        push    rdx                         ; rdx
        push    rsi                         ; rsi
        push    rdi                         ; rdi
        push    rbp                         ; rbp
        push    r8                          ; r8
        push    r9                          ; r9
        push    r10                         ; r10（第 4 个参数在这里）
        push    r11                         ; r11
        push    r12
        push    r13
        push    r14
        push    r15
        ; 现在 rsp 指向 Registers 结构起始（r15）

        ; --- 4. 调用 C++ 分发 ---
        mov     rdi, rsp                    ; 参数：Registers*
        call    syscall_handler
        ; 返回值 rax = 接下来要恢复的现场地址
        ;         （没切换就是原值；IPC 导致切换则是新线程的现场）

        mov     rsp, rax                    ; 切换现场（和中断出口一样的套路）

        ; --- 5. 统一出口 ---
        ; 跳到 isr_stubs.asm 里中断共用的恢复序列，用 iretq 返回。
        ; 不在这里自己 pop + sysretq —— 那会和被切换进来的线程
        ; （它们是中断现场）不兼容，详见 isr_stubs.asm 里的说明。
        jmp     isr_restore_exit

; ---------------------------------------------------------------------------
;  装载 syscall 相关的 MSR
;  -------------------------------------------------------------------------
;  必须在第一次进入用户态之前调用，否则 syscall 指令会 #UD（无效指令）。
; ---------------------------------------------------------------------------
global syscall_enable
syscall_enable:
        ; --- 【最关键的一步】先开 EFER.SCE ---
        ;
        ; IA32_EFER (0xC0000080) 的 bit 0 是 SCE（System Call Extensions）。
        ; **不开这个位，syscall / sysret 就是非法指令**，
        ; 用户态一执行 syscall 直接 #UD（无效操作码，6 号异常）。
        ;
        ; 这个坑极具迷惑性：
        ;   STAR / LSTAR / FMASK 三个 MSR 全都装好了，GDT 用户段也建好了，
        ;   用户进程能正常进 Ring 3（CS=0x2b 说明特权级也对），
        ;   可就是不能发起系统调用。
        ;   排查时容易误以为是页表或栈的问题，
        ;   实际上是 CPU 压根不认这条指令。
        ;
        ; 注意：EFER 里已经有 LME/LMA（阶段 1 开长模式时设的），
        ; 必须用 or 置位，**不能整体覆盖**，否则关掉长模式直接崩。
        mov     ecx, 0xC0000080                 ; IA32_EFER
        rdmsr                                   ; edx:eax = 当前 EFER
        or      eax, 1                          ; bit 0 = SCE
        wrmsr

        ; --- IA32_STAR (0xC0000081) ---
        ;   [47:32] = 内核代码段 0x08    syscall 时加载 CS/SS
        ;   [63:48] = 用户段基址 0x18    sysret 时：
        ;       CS = 0x18 + 16 = 0x28（用户代码）
        ;       SS = 0x18 + 8  = 0x20（用户数据）
        ;   这个 +8/+16 是硬件写死的，所以 GDT 里用户数据段必须排在
        ;   用户代码段**前面**（已在 gdt.cpp 里排好）。
        mov     ecx, 0xC0000081
        mov     edx, 0x00180008
        xor     eax, eax
        wrmsr

        ; --- IA32_LSTAR (0xC0000082)：入口地址 ---
        mov     ecx, 0xC0000082
        lea     rax, [rel syscall_entry]
        mov     rdx, rax
        shr     rdx, 32
        wrmsr

        ; --- IA32_FMASK (0xC0000084)：进入时清掉哪些 RFLAGS 位 ---
        ; 清 IF(0x200)：进内核先关中断，防止换栈完成前被打断。
        mov     ecx, 0xC0000084
        mov     eax, 0x200
        xor     edx, edx
        wrmsr
        ret

; ---------------------------------------------------------------------------
;  设置内核 GS 基址（per-cpu 数据区）
;  入参 rdi = per-cpu 区地址
; ---------------------------------------------------------------------------
global set_kernel_gs
set_kernel_gs:
        mov     ecx, 0xC0000102             ; IA32_KERNEL_GS_BASE
        mov     rax, rdi
        mov     rdx, rax
        shr     rdx, 32
        wrmsr
        ret

section .bss
align 8
global g_tmp_rbx
g_tmp_rbx:      resq    1
global g_tmp_rsp
g_tmp_rsp:      resq    1
