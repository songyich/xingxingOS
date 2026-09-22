; ============================================================================
;  arch/x86_64/isr_stubs.asm —— 中断服务程序（ISR）的汇编入口
;  ---------------------------------------------------------------------------
;  为什么这段必须是汇编？
;    中断发生时，CPU 只负责跳到 IDT 里登记的地址，别的什么都不做。
;    而 C++ 函数一进入就可能改动通用寄存器（编译器会拿它们当临时变量用），
;    如果不先把现场完整保存下来，中断返回后原来的程序就跑飞了。
;    保存/恢复寄存器必须精确控制每一条指令，用不了 C++，只能写汇编。
;
;  栈上发生了什么？
;    异常发生时 CPU 会自动压入这些（从高地址往低地址压）：
;      SS / RSP / RFLAGS / CS / RIP        <- 特权级切换时才有 SS:RSP
;      错误码 error_code                    <- 只有部分异常会压
;    关键陷阱：**有的异常压错误码，有的不压**。
;    为了让 C++ 那边拿到统一的栈结构，不压的那几个我们要手动补一个 0。
;
;  统一后的栈布局（从 rsp 指向的最低地址往高地址数）：
;      r15 r14 r13 r12 r11 r10 r9 r8
;      rbp rdi rsi rdx rcx rbx rax
;      int_no          <- 我们手动压的中断号
;      err_code        <- CPU 压的，或我们补的 0
;      rip cs rflags rsp ss
;    这正好对应 C++ 里 struct Registers 的字段顺序（见 include/kernel/isr.hpp）。
;
;  哪些异常带错误码（x86_64 手册规定）：
;      8  #DF 双重错误        10 #TS 无效TSS
;     11  #NP 段不存在        12 #SS 栈段错误
;     13  #GP 通用保护        14 #PF 页错误
;     17  #AC 对齐检查
;    其余 0~31 都不带，需要补 0。
; ============================================================================

BITS 64

; ---------------------------------------------------------------------------
;  宏：不带错误码的中断入口
;  手动压一个 0 占位，再压中断号，然后跳到公共处理部分
; ---------------------------------------------------------------------------
%macro ISR_NOERR 1
global isr%1
isr%1:
        push    qword 0                 ; 补一个假的错误码，对齐栈结构
        push    qword %1                ; 中断号
        jmp     isr_common_stub
%endmacro

; ---------------------------------------------------------------------------
;  宏：CPU 已经压了错误码的中断入口
;  只压中断号即可
; ---------------------------------------------------------------------------
%macro ISR_ERR 1
global isr%1
isr%1:
        push    qword %1                ; CPU 已压错误码，这里只压中断号
        jmp     isr_common_stub
%endmacro

; ---------------------------------------------------------------------------
;  0~31 号异常：前 32 个是 CPU 异常，后 224 个留给外部中断（IRQ）
; ---------------------------------------------------------------------------
ISR_NOERR  0      ; #DE 除法错误
ISR_NOERR  1      ; #DB 调试
ISR_NOERR  2      ; NMI 不可屏蔽中断
ISR_NOERR  3      ; #BP 断点
ISR_NOERR  4      ; #OF 溢出
ISR_NOERR  5      ; #BR 越界
ISR_NOERR  6      ; #UD 无效操作码
ISR_NOERR  7      ; #NM 设备不可用
ISR_ERR    8      ; #DF 双重错误（带错误码）
ISR_NOERR  9      ; 协处理器段越界（保留）
ISR_ERR   10      ; #TS 无效 TSS（带错误码）
ISR_ERR   11      ; #NP 段不存在（带错误码）
ISR_ERR   12      ; #SS 栈段错误（带错误码）
ISR_ERR   13      ; #GP 通用保护错误（带错误码）
ISR_ERR   14      ; #PF 页错误（带错误码）
ISR_NOERR 15      ; 保留
ISR_NOERR 16      ; #MF x87 浮点错误
ISR_ERR   17      ; #AC 对齐检查（带错误码）
ISR_NOERR 18      ; #MC 机器检查
ISR_NOERR 19      ; #XM SIMD 浮点异常
ISR_NOERR 20      ; #VE 虚拟化异常
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30      ; #SX 安全异常（带错误码）
ISR_NOERR 31

; ---------------------------------------------------------------------------
;  32~255：外部中断（IRQ0 映射到这里，所以 IRQ0 = 中断号 32）
;  硬件中断都不会压错误码，统一用 ISR_NOERR
; ---------------------------------------------------------------------------
%assign i 32
%rep    224
ISR_NOERR i
%assign i i+1
%endrep

; ---------------------------------------------------------------------------
;  公共处理部分
;  进入时：栈上已有 err_code + int_no（+ CPU 压的 rip/cs/rflags/...）
; ---------------------------------------------------------------------------
extern isr_handler                      ; C++ 侧的中断分发函数

isr_common_stub:
        ; --- 保存全部通用寄存器 ---
        ; 注意压栈顺序：先压的在**高**地址，最后压的在**低**地址。
        ; 下面的顺序配合 C++ 结构体，从 rsp 开始读依次是 r15,r14,...,rax。
        push    rax
        push    rbx
        push    rcx
        push    rdx
        push    rsi
        push    rdi
        push    rbp
        push    r8
        push    r9
        push    r10
        push    r11
        push    r12
        push    r13
        push    r14
        push    r15

        ; --- 调用 C++ 处理函数 ---
        ; SysV AMD64 ABI：第一个参数放 rdi。
        ; 此时 rsp 正好指向我们精心排列的 Registers 结构。
        ;
        ; 【阶段 6 的关键改造：线程切换统一在中断出口完成】
        ;   isr_handler 的返回值（rax）是「接下来该用哪个栈」：
        ;     - 没发生线程切换：返回的就是传进去的 rsp，一切照旧
        ;     - 发生了切换：返回**新线程的栈顶**，那里同样躺着一个
        ;       完整的 Registers 结构（线程创建时精心伪造的现场）
        ;   于是下面的 pop 序列弹的是新线程的寄存器，
        ;   最后的 iretq 直接跳进新线程的代码。
        ;   好处：不需要另写一套上下文切换机制，调度逻辑全在 C++ 里。
        mov     rdi, rsp
        call    isr_handler
        mov     rsp, rax                ; 加载（可能被换掉的）栈指针

; ============================================================================
;  统一出口：中断和系统调用**共用**这一段
;  ---------------------------------------------------------------------------
;  为什么 syscall 也要走 iretq，而不是用更快的 sysretq？
;
;    sysretq 靠 rcx/r11 恢复 rip/rflags，而 iretq 从栈上弹。
;    如果线程 A 通过 syscall 进入内核后阻塞，切到了线程 B，
;    B 是之前被**中断**打断的 —— 它的现场是给 iretq 准备的。
;    这时若用 sysretq 返回，rcx/r11 里根本不是 B 的返回地址，
;    会跳到莫名其妙的地方（实测跳到 RIP=0）。
;
;    统一用 iretq 之后，两条路径的现场格式完全一致，
;    线程切换就不再受"当初是怎么进内核的"影响。
;    代价是 sysretq 稍快一点，但正确性优先。
; ============================================================================
global isr_restore_exit
isr_restore_exit:

        ; --- 逆序恢复寄存器 ---
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     r11
        pop     r10
        pop     r9
        pop     r8
        pop     rbp
        pop     rdi
        pop     rsi
        pop     rdx
        pop     rcx
        pop     rbx
        pop     rax

        ; --- 跳过我们自己压的 int_no 和 err_code ---
        ; 这两个是给 C++ 看的，CPU 的 iretq 不认识它们
        add     rsp, 16

        ; --- 中断返回 ---
        iretq

; ============================================================================
;  诊断版出口：iretq 前把 CPU 实际要弹出的内容打到串口
;  ---------------------------------------------------------------------------
;  用于 C++ 打印的现场"看着正常"但 iretq 仍然 #GP 的情况：
;  直接读 rsp，绕开所有 C++ 结构体偏移的假设。
;
;  打印格式：每行 "I <rip低32位>" —— 只打低 4 字节，够定位了。
; ============================================================================
global isr_restore_exit_dbg
isr_restore_exit_dbg:
        push    rax
        push    rdx
        push    rcx

        ; --- 打印标记 'I' ---
        mov     dx, 0x3FD
.dw0:   in      al, dx
        test    al, 0x20
        jz      .dw0
        mov     dx, 0x3F8
        mov     al, 'I'
        out     dx, al

        ; --- 打印 rip 的低 4 字节（小端：先低后高）---
        ; 进本段时已 push 了 3 个寄存器(24 字节)，
        ; 所以原 rsp 位置 = rsp + 24
        mov     rcx, 8
.dl:
        mov     dx, 0x3FD
.dw1:   in      al, dx
        test    al, 0x20
        jz      .dw1
        mov     rax, [rsp + 24]
        mov     dx, 0x3F8
        mov     al, [rsp + 24 + rcx - 1]
        out     dx, al
        dec     rcx
        jnz     .dl

        ; --- 换行 ---
        mov     dx, 0x3FD
.dw2:   in      al, dx
        test    al, 0x20
        jz      .dw2
        mov     dx, 0x3F8
        mov     al, 10
        out     dx, al

        pop     rcx
        pop     rdx
        pop     rax
        iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
        dq isr%+i                       ; %+ 是 nasm 的标识符连接符，拼出 isr0/isr1/...
%assign i i+1
%endrep
