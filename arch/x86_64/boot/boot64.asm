; ============================================================================
;  arch/x86_64/boot/boot64.asm
;  阶段 1：真正的 64 位入口
;  ---------------------------------------------------------------------------
;  从 boot32.asm 那条远跳转过来时，CPU 的状态：
;    - 已在 64 位长模式（CS.L = 1），但还没干任何 64 位的事
;    - 分页已开，恒等映射 + 高半区映射都建好了
;    - 还在低地址（1MB 附近）执行，因为远跳转的偏移是 32 位的
;
;  本文件只做三件事：
;    1. 重装数据段寄存器（CS 由远跳转换了，DS/ES/SS/FS/GS 还是老的 32 位值）
;    2. 手动清零 .bss（不指望 GRUB 帮忙，全局变量初值才有保证）
;    3. 把栈搬到高半区，然后跳转到 C++ 的 kmain()
;
;  为什么栈要搬到高半区？
;    低地址（0~4GB 恒等映射）将来是给用户态进程用的，内核不该长期赖在那儿。
;    从这一刻起，内核的所有代码/数据/栈都只在高半区活动，这是后续做
;    用户态（Ring 3）和系统调用的前提。
; ============================================================================

BITS 64                              ; 与 boot32.asm 的区别就在这：生成 64 位机器码

section .boot_text64
global long_mode_start               ; boot32.asm 会远跳转到这里

extern kmain                         ; C++ 内核入口（在 kernel/kmain.cpp）
extern _bss_start                    ; 链接脚本导出的 .bss 起止地址
extern _bss_end
extern _stack_top                    ; 链接脚本导出的高半区栈顶
extern multiboot_magic               ; boot32.asm 存在低地址的两个参数
extern multiboot_info_phys

long_mode_start:
        ; --- 1. 重装数据段寄存器 ---
        ; 64 位模式下 DS/ES/SS 的内容实际上被 CPU 忽略，但 FS/GS 和
        ; 一些遗留机制还会用到，所以统一装成内核数据段（选择子 0x10）最省心。
        mov     ax, 0x10              ; 0x10 = gdt64.data（第 2 项，每项 8 字节）
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     fs, ax
        mov     gs, ax

        ; --- 2. 手动清零 .bss ---
        ; .bss 里放的是「初值为 0 的全局/静态变量」。ELF 里它不占文件空间，
        ; 谁负责把它清零由加载器决定 —— Multiboot 规范没强制要求 GRUB 做这件事，
        ; 所以自己清最稳。忘了清的典型症状：某个全局变量随机非 0，行为飘忽。
        mov     rdi, _bss_start       ; R_X86_64_32S：0xffffffff8020xxxx 可用 32 位有符号表示
        mov     rcx, _bss_end
        sub     rcx, rdi              ; rcx = .bss 字节数
        xor     rax, rax              ; 填 0
        rep     stosb                 ; 按字节填，慢一点但 .bss 只有几 KB，无所谓

        ; --- 3. 切到高半区的内核栈 ---
        mov     rsp, _stack_top       ; 栈顶在高地址（栈向低地址生长）
        and     rsp, -16              ; 16 字节对齐：SysV ABI 要求，SSE 指令会用到

        ; --- 4. 进入 C++ 世界 ---
        ; 按 SysV AMD64 ABI，前两个整型参数用 rdi / rsi 传递
        mov     edi, [rel multiboot_magic]      ; 第 1 个参数：Multiboot2 魔法值
        mov     esi, [rel multiboot_info_phys]  ; 第 2 个参数：信息结构物理地址
        ; 注意 [rel ...] 是 RIP 相对寻址。这两个变量在 1MB 附近、我们也在 1MB 附近，
        ; 相对偏移在 ±2GB 内，安全。

        ; 显式写 qword：强制生成 64 位立即数（imm64），kmain 在
        ; 0xffffffff8020xxxx，只有完整的 64 位地址才能表示
        mov     rax, qword kmain
        call    rax                   ; 间接调用：此时 RIP 还在低地址，直接 call 相对跳转够不着

        ; kmain 正常不该返回；真返回了就停机，屏幕会保留最后的输出
.hang:
        hlt
        jmp     .hang
