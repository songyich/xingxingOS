; ============================================================================
;  arch/x86_64/switch.asm —— 线程入口蹦床
;  ---------------------------------------------------------------------------
;  新线程是「凭空造出来」的：它从未被中断过，栈上自然没有寄存器现场。
;  所以 thread.cpp 手工伪造了一个 Registers 结构，把 rip 指向这里。
;
;  第一次调度到新线程时，isr_stubs.asm 的 pop 序列会弹出我们填的寄存器，
;  最后 iretq 跳到 thread_trampoline，此时：
;      rdi = 线程函数指针
;      rsi = 传给线程函数的参数
;
;  蹦床要做的就是把参数摆到 SysV ABI 规定的位置上再调用，
;  函数返回后收尾退出线程。
; ============================================================================
BITS 64

section .text

extern thread_exit_trampoline      ; C++ 侧的收尾函数（[[noreturn]]）

global thread_trampoline
thread_trampoline:
        ; --- 把参数按 SysV ABI 摆好 ---
        ; 进来时: rdi = fn, rsi = arg
        ; 目标  : rdi = arg（第一个参数），fn 放一个能 call 的寄存器
        mov     rax, rdi                ; rax = 线程函数地址
        mov     rdi, rsi                ; rdi = 参数

        call    rax                     ; 调用 fn(arg)

        ; --- 函数返回了，收尾退出线程 ---
        ; 注意：走到这里说明线程函数自己 return 了，
        ; 不能让 CPU 继续往下跑（栈上没有返回地址），必须交给 exit。
        call    thread_exit_trampoline

        ; 理论上回不来；万一回来了就停机，避免乱跑
        cli
.hang:
        hlt
        jmp     .hang
