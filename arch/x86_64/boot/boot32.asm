; ============================================================================
;  arch/x86_64/boot/boot32.asm
;  阶段 1：Multiboot2 头 -> 建页表 -> 开长模式 -> 跳进 64 位代码
;  ---------------------------------------------------------------------------
;  CPU 上电/GRUB 交接时的状态：
;    - 32 位保护模式，分页**关闭**，物理地址 == 线性地址
;    - eax = 0x36d76289（Multiboot2 魔法值）
;    - ebx = Multiboot2 信息结构的物理地址（阶段 4 解析内存映射要用）
;    - DS/ES/SS 是 GRUB 设好的平坦段（基址 0、限长 4GB）
;
;  要进入 64 位长模式，硬件规定了固定的五步（顺序不能乱）：
;    1. 用 CPUID 确认这颗 CPU 支持长模式，否则后面全是白搭
;    2. 建好 4 级页表（PML4 -> PDPT -> PD），长模式强制开启分页
;    3. 打开 CR4.PAE（物理地址扩展）；不开 PAE 就开不了长模式
;    4. 置 MSR 0xC0000080（EFER）的 LME 位（Long Mode Enable）
;    5. 把 PML4 物理地址写进 CR3，再把 CR0.PG 置 1 打开分页
;       这一步之后 CPU 进入「兼容模式」（还是 32 位，只是开了分页）
;    6. 用一条**远跳转**跳进 L 位为 1 的代码段描述符，才真正变成 64 位
;       远跳转是 x86 上唯一能改写 CS 可见部分（从而切换位宽）的手段
;
;  为什么是 elf64 容器 + BITS 32？
;    ld 不接受 elf32 与 elf64 目标文件混链。所以统一用 -f elf64，再用 BITS 32
;    让 nasm 生成 32 位机器码。符号重定位会落成 R_X86_64_32（32 位绝对地址），
;    我们的物理地址只有 1MB 出头，完全放得下。
;
;  本文件的产物：进入兼容模式并远跳转到 boot64.asm 的 long_mode_start。
; ============================================================================

BITS 32                             ; 关键：让 nasm 生成 32 位机器码

MB2_MAGIC        equ 0xE85250D6     ; Multiboot2 头魔数
MB2_ARCH_I386    equ 0              ; 0 = 保护模式 i386（32 位）
MB2_BOOT_MAGIC   equ 0x36D76289     ; GRUB 跳转时放在 eax 里的成功标志
VGA_TEXT_BUFFER  equ 0xB8000        ; 彩色文本模式显存物理地址
VGA_ATTR         equ 0x0F           ; 黑底(0)亮白字(F)
DEBUGCON_PORT    equ 0xE9           ; QEMU debugcon 端口（真实机器上是空操作）

PAGE_PRESENT     equ (1 << 0)       ; 页表项：存在
PAGE_WRITABLE    equ (1 << 1)       ; 页表项：可写
PAGE_PS          equ (1 << 7)       ; PD 项：PS=1 表示这一项直接映射 2MB 大页
PAGE_SIZE_2M     equ 0x200000       ; 2MB

CR4_PAE          equ (1 << 5)       ; CR4 的物理地址扩展位
CR0_PG           equ (1 << 31)      ; CR0 的分页开关位
EFER_MSR         equ 0xC0000080     ; EFER 的 MSR 编号
EFER_LME         equ (1 << 8)       ; EFER 的长模式使能位
EFER_NXE         equ (1 << 11)      ; EFER 的「不可执行」使能位
                                    ;
                                    ; 阶段 7 填的坑：用户栈、数据段都设了
                                    ; 页表项最高位（bit63 = NX，不可执行）。
                                    ; 但如果 NXE 没开，bit63 是**保留位**——
                                    ; CPU 一看到保留位被置 1 就直接 #PF，
                                    ; 错误码里 RSVD(bit3)=1。
                                    ; 症状极具迷惑性：页明明映射了，却报
                                    ; 「页不存在」式的错误，CR2 指向正常地址。

; 高半区基址。内核正文的 VMA = 物理地址 + 这个值（见 linker.ld）
KERNEL_VIRT_BASE equ 0xFFFFFFFF80000000

; 页表在 PML4 / PDPT 里的下标（64 位虚拟地址 4 级拆分：9+9+9+9+12）
;
; 这两个数字是**算出来的**，不是猜的。以 0xffffffff80000000 为例：
;   PML4 下标 = (VA >> 39) & 0x1FF = 511
;   PDPT 下标 = (VA >> 30) & 0x1FF = 510
; 验算一下 PDPT：PML4[511] 覆盖的 64 位地址区间是
; 0xffffff8000000000 ~ 0xffffffffffffffff，我们的内核基址
; 0xffffffff80000000 减去区间起点 = 0x7f80000000 = 510GB，
; 而一个 PDPT 项正好管 1GB，所以是第 510 项。
; （写成 480 是最经典的踩坑：映射落在 0xfffffff800000000，
;   内核一访问高半区就页错误 -> 三重错误，屏幕上只留最后一行 32 位输出）
PML4_IDX_HIGH    equ 511            ; 0xffffffff8xxxxxxx 落在 PML4 的最后一项
PDPT_IDX_HIGH    equ 510            ; 0xffffffff80000000 落在该 PDPT 的第 510 项

; ---------------------------------------------------------------------------
;  Multiboot2 头：8 字节对齐，必须位于镜像前 32KB 内
; ---------------------------------------------------------------------------
section .multiboot2_header
align 8
mb2_header_start:
        dd  MB2_MAGIC
        dd  MB2_ARCH_I386
        dd  mb2_header_end - mb2_header_start
        dd  -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header_start))
; --- 帧缓冲请求标签（type = 5）---
; 作用：告诉 GRUB「我要图形模式，别停在文本模式」。
; 没有这个标签，GRUB 会让我们停在 EGA 文本模式，
; Multiboot2 信息里也就不会有帧缓冲标签（type 8），
; 内核只能走 VGA 兜底路径。
;
; 字段：width / height / depth。这里请求 1024x768x32，
; GRUB 若做不到会自行挑一个接近的模式，并把**实际**值写进
; 信息结构的帧缓冲标签里 —— 所以内核永远以标签里的值为准，
; 不能假设一定拿到 1024x768。
align 8
        dw  5                                                ; type = 5（帧缓冲）
        dw  0                                                ; flags = 0
        dd  20                                               ; size = 20 字节
        dd  1024                                             ; preferred width
        dd  768                                              ; preferred height
        dd  32                                               ; preferred depth (bpp)

align 8
        dw  0                                                ; 结束标签 type=0
        dw  0                                                ;           flags=0
        dd  8                                                ;           size=8
mb2_header_end:

; ---------------------------------------------------------------------------
;  32 位代码
; ---------------------------------------------------------------------------
section .boot_text
global _start
extern long_mode_start               ; 64 位入口，定义在 arch/x86_64/boot/boot64.asm
_start:
        cli                           ; 关中断：现在还没有 IDT，来一个中断就是三重错误
        cld                           ; 清方向标志，让后面的 rep stos* 往高地址走

        mov     esp, boot_stack_top   ; 建临时栈：栈向低地址生长，栈顶在高地址

        ; 保存 GRUB 递过来的两个参数，后面传给 kmain（阶段 4 解析内存映射要用）
        mov     [multiboot_magic], eax
        mov     [multiboot_info_phys], ebx

        ; --- 第 0 步：确认真的是被 Multiboot2 加载的 ---
        cmp     eax, MB2_BOOT_MAGIC
        je      .magic_ok
        mov     esi, msg_bad_magic
        call    boot_print
        jmp     halt_loop
.magic_ok:
        mov     esi, msg_hello
        call    boot_print

        ; --- 第 1 步：确认 CPU 支持 CPUID，再确认支持长模式 ---
        call    check_cpuid
        call    check_long_mode
        mov     esi, msg_cpu_ok
        call    boot_print

        ; --- 第 2 步：建页表 ---
        call    setup_page_tables
        mov     esi, msg_pagetables_ok
        call    boot_print

        ; --- 第 3~5 步：开 PAE / EFER.LME / 开分页 ---
        call    enable_paging
        ; 注意：走到这里分页已经打开，但 CPU 还在「兼容模式」（32 位）。
        ; 现在往 VGA（物理 0xb8000）写东西依然有效，因为低地址是恒等映射的。
        mov     esi, msg_paging_ok
        call    boot_print

        ; --- 第 6 步：加载 64 位 GDT，然后远跳转进入真正的 64 位长模式 ---
        lgdt    [gdt64.pointer]       ; 32 位下 lgdt 读 6 字节：2 字节限长 + 4 字节基址
        jmp     gdt64.code:long_mode_start
        ;  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        ;  这条远跳转是进入长模式的最后一把钥匙：
        ;  目标段描述符的 L=1，CPU 把 CS 换掉的同时把位宽切到 64 位。
        ;  跳转偏移必须是 32 位能放下的低地址，所以 long_mode_start 放在 1MB 附近。

; ---------------------------------------------------------------------------
;  boot_print：以 esi 为指针，打印一个以 0 结尾的字符串
;  同时写 VGA 显存和 QEMU debugcon 端口（无窗口时靠 debugcon 抓输出）
;  VGA 文本模式每字符 2 字节：低字节 ASCII，高字节属性
; ---------------------------------------------------------------------------
boot_print:
        pusha                         ; 保护寄存器，方便在任意位置插桩调试
.loop:
        lodsb                         ; al = [esi]; esi++
        test    al, al
        jz      .done
        call    put_char
        jmp     .loop
.done:
        popa
        ret

; ---------------------------------------------------------------------------
;  put_char：输出 al 里的一个字符，自动维护光标位置
;  （旧版每次都从 0xB8000 重头写，多次打印会互相覆盖，看不到完整启动轨迹；
;    这里改成维护一个全局光标偏移，多行信息能依次显示）
;  入口 al = 字符；会改 eax/ecx/edx（调用者若需要请先保存）
; ---------------------------------------------------------------------------
put_char:
        cmp     al, 0x0A              ; '\n'：换到下一行行首
        je      .newline
        cmp     al, 0x0D              ; '\r'：回到本行行首
        je      .carriage
        push    edi
        push    edx
        mov     edi, VGA_TEXT_BUFFER
        add     edi, [vga_cursor]     ; 光标偏移（字节）
        mov     ah, VGA_ATTR
        mov     [edi], ax             ; 一次写 2 字节：ASCII + 属性
        add     dword [vga_cursor], 2
        out     DEBUGCON_PORT, al     ; 同样的字符送到 debugcon
        pop     edx
        pop     edi
        ret
.newline:
        push    eax
        push    ecx
        push    edx
        mov     eax, [vga_cursor]
        shr     eax, 1                ; 字节偏移 -> 字符索引
        xor     edx, edx              ; div 要求 edx:eax 为被除数，高位置 0
        mov     ecx, 80
        div     ecx                   ; eax = 当前行号，edx = 当前列（余数）
        inc     eax                   ; 行号 +1
        mov     ecx, 160              ; 每行 80 字符 × 2 字节
        mul     ecx                   ; eax = 新行的字节偏移
        mov     [vga_cursor], eax
        mov     al, 0x0A              ; 此时 eax 已被算成地址，不能直接发出去
        out     DEBUGCON_PORT, al     ; debugcon 侧补一个真正的换行符
        pop     edx
        pop     ecx
        pop     eax
        ret
.carriage:
        push    eax
        push    ecx
        push    edx
        mov     eax, [vga_cursor]
        shr     eax, 1                ; 字节偏移 -> 字符索引
        xor     edx, edx
        mov     ecx, 80
        div     ecx                   ; eax = 当前行号
        mov     ecx, 160              ; 每行 160 字节
        mul     ecx                   ; eax = 本行行首的字节偏移
        mov     [vga_cursor], eax
        mov     al, 0x0D
        out     DEBUGCON_PORT, al
        pop     edx
        pop     ecx
        pop     eax
        ret

; ---------------------------------------------------------------------------
;  put_hex32：以 8 位十六进制打印 eax 的值（排查 CPUID 问题时用）
;  做法：rol 循环左移 4 位，每次取最低 4 位查表转成字符
; ---------------------------------------------------------------------------
put_hex32:
        pusha
        mov     ecx, 8                ; 32 位 = 8 个十六进制字符
.loop:
        rol     eax, 4                ; 最高的 4 位转到最低 4 位
        push    eax                   ; 存一份（put_char 会改 eax）
        and     eax, 0x0F             ; 取最低 4 位
        mov     esi, hex_digits
        add     esi, eax              ; 查表得到字符地址
        mov     al, [esi]
        call    put_char
        pop     eax                   ; 还原，下一轮继续 rol
        loop    .loop
        popa
        ret

; ---------------------------------------------------------------------------
;  check_cpuid：通过翻转 EFLAGS 的 ID 位（bit 21）判断 CPUID 指令是否可用
;  能翻转说明 CPUID 存在，翻不动说明是颗太老的 CPU
; ---------------------------------------------------------------------------
check_cpuid:
        pushfd                        ; 把 EFLAGS 推到栈上
        pop     eax                   ; 取出来
        mov     ecx, eax              ; 存一份原值
        xor     eax, 1 << 21          ; 翻转 ID 位
        push    eax
        popfd                         ; 写回 EFLAGS
        pushfd
        pop     eax                   ; 再读出来
        push    ecx
        popfd                         ; 恢复原 EFLAGS
        xor     eax, ecx              ; 两次读数不同 => 该位可写 => 支持 CPUID
        jz      .no_cpuid
        ret
.no_cpuid:
        mov     esi, msg_no_cpuid
        call    boot_print
        jmp     halt_loop

; ---------------------------------------------------------------------------
;  check_long_mode：CPUID 的 0x80000001 号叶子，返回 EDX 的 bit29 = 长模式支持位
;  （0x8000xxxx 是 AMD 定义的扩展叶子，Intel 也照做了）
; ---------------------------------------------------------------------------
check_long_mode:
        mov     eax, 0x80000000       ; 先问：扩展功能号最大支持到多少？
        cpuid
        mov     [cpuid_max_ext], eax  ; 存下来，报错时打出来
        cmp     eax, 0x80000001       ; 至少得支持到 0x80000001 才行
        jb      .no_long_mode
        mov     eax, 0x80000001
        cpuid
        mov     [cpuid_ext_feat], edx ; 存下来，报错时打出来
        test    edx, 1 << 29          ; EDX bit29 = LM（Long Mode）
        jz      .no_long_mode
        ret
.no_long_mode:
        ; 打印原始 CPUID 数据。看到这些数字就能立刻区分两种情况：
        ;   max_ext < 0x80000001  -> 这颗 CPU 连扩展功能页都没有（极老或严重受限）
        ;   max_ext 正常但 ext_feat 的 bit29=0 -> 64 位能力被虚拟机/固件屏蔽了
        ;                                          （绝大多数情况是这个，改 VM 配置即可）
        mov     esi, msg_no_long_mode
        call    boot_print
        mov     esi, msg_diag_max
        call    boot_print
        mov     eax, [cpuid_max_ext]
        call    put_hex32
        mov     esi, msg_diag_feat
        call    boot_print
        mov     eax, [cpuid_ext_feat]
        call    put_hex32
        mov     esi, msg_diag_hint
        call    boot_print
        mov     esi, msg_diag_hint2
        call    boot_print
        jmp     halt_loop

; ---------------------------------------------------------------------------
;  setup_page_tables：建立 4 级页表
;  -------------------------------------------------------------------------
;  采用 2MB 大页（PD 项的 PS=1），这样只用一层 PD 就能覆盖大块地址，
;  省掉一层 PT，代码短很多，阶段 4 做页帧管理时会换成更精细的结构。
;
;  建立两张映射：
;    A) 恒等映射  phys 0 ~ 4GB  ->  virt 0 ~ 4GB
;       用途：进入长模式前后，低地址代码/VGA/GRUB 数据还能正常访问。
;       也方便阶段 2~3 直接访问显存、PIC、PIT 这些低于 4GB 的 MMIO。
;
;    B) 高半区映射 phys 0 ~ 2GB -> virt 0xffffffff80000000 ~ 0xffffffff80000000+2GB
;       用途：这就是「内核住的高半区」。0xffffffff80000000 是 -2GB，
;       往上 2GB 正好填满 64 位地址空间的最高 2GB，全是规范地址（不会踩
;       canonical hole）。
;
;  页表页放在 .boot_pgtable（nobits，由链接脚本安排在 1MB 之后的低地址）。
; ---------------------------------------------------------------------------
setup_page_tables:
        ; --- 先把 9 个页表页全部清零 ---
        ; 不能假设 GRUB 会帮我们清 bss，自己清零最保险（页表里有脏数据是经典的三重错误来源）
        mov     edi, boot_pml4
        mov     ecx, (9 * 4096) / 4   ; 按 dword 计数
        xor     eax, eax
        rep     stosd

        ; --- PML4：只填两项 ---
        mov     eax, boot_pdpt_low
        or      eax, PAGE_PRESENT | PAGE_WRITABLE
        mov     [boot_pml4 + 0 * 8], eax              ; PML4[0]   -> 恒等映射用

        mov     eax, boot_pdpt_high
        or      eax, PAGE_PRESENT | PAGE_WRITABLE
        mov     [boot_pml4 + PML4_IDX_HIGH * 8], eax  ; PML4[511] -> 高半区用

        ; --- PDPT_low：接 4 个 PD，每个覆盖 1GB，合起来 4GB ---
        mov     eax, boot_pd_low_0
        or      eax, PAGE_PRESENT | PAGE_WRITABLE
        mov     [boot_pdpt_low + 0 * 8], eax
        mov     eax, boot_pd_low_1
        or      eax, PAGE_PRESENT | PAGE_WRITABLE
        mov     [boot_pdpt_low + 1 * 8], eax
        mov     eax, boot_pd_low_2
        or      eax, PAGE_PRESENT | PAGE_WRITABLE
        mov     [boot_pdpt_low + 2 * 8], eax
        mov     eax, boot_pd_low_3
        or      eax, PAGE_PRESENT | PAGE_WRITABLE
        mov     [boot_pdpt_low + 3 * 8], eax

        ; --- PDPT_high：0xffffffff80000000 对应这一级的第 480 项 ---
        mov     eax, boot_pd_high_0
        or      eax, PAGE_PRESENT | PAGE_WRITABLE
        mov     [boot_pdpt_high + PDPT_IDX_HIGH * 8], eax        ; phys 0   ~ 1GB
        mov     eax, boot_pd_high_1
        or      eax, PAGE_PRESENT | PAGE_WRITABLE
        mov     [boot_pdpt_high + (PDPT_IDX_HIGH + 1) * 8], eax  ; phys 1GB ~ 2GB

        ; --- 填 6 个 PD：每个 512 项，每项 2MB ---
        mov     edi, boot_pd_low_0
        mov     ebx, 0x00000000
        call    fill_pd
        mov     edi, boot_pd_low_1
        mov     ebx, 0x40000000
        call    fill_pd
        mov     edi, boot_pd_low_2
        mov     ebx, 0x80000000
        call    fill_pd
        mov     edi, boot_pd_low_3
        mov     ebx, 0xC0000000
        call    fill_pd
        mov     edi, boot_pd_high_0
        mov     ebx, 0x00000000
        call    fill_pd
        mov     edi, boot_pd_high_1
        mov     ebx, 0x40000000
        call    fill_pd
        ret

; ---------------------------------------------------------------------------
;  fill_pd：填一个 PD 的 512 个 2MB 大页项
;    入口：edi = PD 的物理地址，ebx = 起始物理地址
;    出口：edi 已后移 512*8，ebx 已加上 1GB
; ---------------------------------------------------------------------------
fill_pd:
        mov     ecx, 512
.loop:
        mov     eax, ebx
        or      eax, PAGE_PRESENT | PAGE_WRITABLE | PAGE_PS   ; 0x83：存在+可写+2MB 大页
        mov     [edi], eax
        add     ebx, PAGE_SIZE_2M                             ; 下一个 2MB
        add     edi, 8                                        ; 下一个表项
        loop    .loop
        ret

; ---------------------------------------------------------------------------
;  enable_paging：开 PAE -> 置 EFER.LME -> 装 CR3 -> 开 CR0.PG
;  顺序是硬件硬性要求，写反了就是直接重启（三重错误）
; ---------------------------------------------------------------------------
enable_paging:
        ; 1) CR4.PAE = 1
        mov     eax, cr4
        or      eax, CR4_PAE
        mov     cr4, eax

        ; 2) EFER.LME = 1（MSR 0xC0000080，用 rdmsr/wrmsr 访问，edx:eax = 64 位值）
        mov     ecx, EFER_MSR
        rdmsr
        or      eax, EFER_LME
        or      eax, EFER_NXE                   ; 同时打开 NX 支持（见上方说明）
        wrmsr

        ; 3) CR3 = PML4 的**物理地址**
        mov     eax, boot_pml4
        mov     cr3, eax

        ; 4) CR0.PG = 1，分页正式生效
        mov     eax, cr0
        or      eax, CR0_PG
        mov     cr0, eax
        ret

; ---------------------------------------------------------------------------
;  halt_loop：停机死循环（出错时停在这里，屏幕上会留下最后一条错误信息）
; ---------------------------------------------------------------------------
halt_loop:
        hlt
        jmp     halt_loop

; ---------------------------------------------------------------------------
;  32 位阶段的只读数据
; ---------------------------------------------------------------------------
section .boot_rodata
msg_hello:
        db "P1: boot32 start", 0x0D, 0x0A, 0
msg_cpu_ok:
        db "P1: cpuid + long mode ok", 0x0D, 0x0A, 0
msg_pagetables_ok:
        db "P1: page tables built", 0x0D, 0x0A, 0
msg_paging_ok:
        db "P1: paging on (compat mode)", 0x0D, 0x0A, 0
msg_bad_magic:
        db "ERR: not loaded by multiboot2", 0x0D, 0x0A, 0
msg_no_cpuid:
        db "ERR: cpuid unsupported", 0x0D, 0x0A, 0
msg_no_long_mode:
        db "ERR: long mode unsupported", 0x0D, 0x0A, 0
msg_diag_max:
        db "  cpuid max ext  = 0x", 0
msg_diag_feat:
        db 0x0D, 0x0A, "  cpuid ext feat = 0x", 0
msg_diag_hint:
        db 0x0D, 0x0A, "  bit29 of ext feat is the LM (long mode) flag", 0x0D, 0x0A, 0
msg_diag_hint2:
        db "  => VM is hiding 64-bit. Check VM settings.", 0x0D, 0x0A, 0
hex_digits:
        db "0123456789ABCDEF"

; 64 位 GDT。放在低地址，因为 32 位下的 lgdt 只能接受 4 字节基址。
; 结构参考 AMD64 手册：8 字节一项，基址和限长在 64 位模式下被忽略，
; 真正起作用的是 type / DPL / P / L 这几个位。
align 16
global gdt64
gdt64:
        dq 0x0000000000000000        ; [0x00] 空描述符（CPU 要求第 0 项必须是空的）
.code:  equ $ - gdt64                ; [0x08]
        dq 0x00AF9A000000FFFF        ; 内核代码段：P=1 DPL=0 S=1 type=1010(执行/读)
                                     ;             G=1 D=0 **L=1**（L=1 才是 64 位段）
.data:  equ $ - gdt64                ; [0x10]
        dq 0x00AF92000000FFFF        ; 内核数据段：P=1 DPL=0 S=1 type=0010(读/写)
.pointer:                            ; lgdt 用的 6 字节描述符
        dw $ - gdt64 - 1             ; 限长（GDT 总长 - 1）
        dq gdt64                     ; 基址（低 4 字节有效；dq 是为了对齐省事）

; ---------------------------------------------------------------------------
;  32 位阶段的可写数据（GRUB 递过来的参数存在这里）
; ---------------------------------------------------------------------------
section .boot_data
global multiboot_magic
global multiboot_info_phys
multiboot_magic:      dd 0
multiboot_info_phys:  dd 0
vga_cursor:           dd 0        ; 当前光标字节偏移（每次 +2，从 0 开始）
cpuid_max_ext:        dd 0        ; CPUID 0x80000000 的返回值
cpuid_ext_feat:       dd 0        ; CPUID 0x80000001 返回的 EDX

; ---------------------------------------------------------------------------
;  临时栈 + 页表页（都是 nobits，不占文件空间）
;  链接脚本会把它们安排在 1MB 之后、内核正文（2MB）之前
; ---------------------------------------------------------------------------
section .boot_bss nobits
align 16
boot_stack_bottom:
        resb    16384                ; 16KB 临时栈（32 位阶段用）
boot_stack_top:

section .boot_pgtable nobits write alloc
align 4096                           ; 页表必须页对齐，否则 CR3 低 12 位不干净
global boot_pml4
boot_pml4:      resb 4096            ; 第 1 级：页映射表顶端
boot_pdpt_low:  resb 4096            ; 第 2 级：恒等映射用
boot_pd_low_0:  resb 4096            ; 第 3 级：phys 0GB   ~ 1GB
boot_pd_low_1:  resb 4096            ;        phys 1GB   ~ 2GB
boot_pd_low_2:  resb 4096            ;        phys 2GB   ~ 3GB
boot_pd_low_3:  resb 4096            ;        phys 3GB   ~ 4GB
boot_pdpt_high: resb 4096            ; 第 2 级：高半区用
boot_pd_high_0: resb 4096            ; 第 3 级：virt -2GB 起，对应 phys 0 ~ 1GB
boot_pd_high_1: resb 4096            ;        对应 phys 1GB ~ 2GB
