/* ============================================================================
 *  arch/x86_64/linker.ld
 *  链接脚本：把内核放到高半区 0xffffffff80000000
 *  ---------------------------------------------------------------------------
 *  两个关键概念：
 *
 *    VMA（虚拟内存地址）= 程序运行时 CPU 访问的地址
 *    LMA（加载内存地址）= 加载器（这里是 GRUB）把数据放到的物理地址
 *
 *  内核最终运行在 0xffffffff80000000 之上（高半区），但 GRUB 是在「分页还没
 *  打开」的 32 位模式下把内核读进内存的，它只能往低物理地址写。
 *  所以链接脚本要做一次「地址分离」：
 *
 *      .boot*  段（32 位引导代码 + 64 位入口 + 页表）：VMA == LMA == 1MB
 *              —— 它们跑在「跳进高半区之前」，虚拟地址必须等于物理地址，
 *                 否则任何绝对地址引用（取符号地址、访问变量）都会算错。
 *
 *      内核正文（.text/.rodata/.data/.bss）：VMA = 0xffffffff80200000
 *                                           LMA = 0x200000
 *              —— 用 AT(...) 显式指定 LMA。boot32.asm 建的页表把
 *                 0xffffffff80000000 映射到物理 0，两块地址指向同一批物理页，
 *                 跳过去就完成了「升入高半区」。
 *
 *  为什么正文 LMA 从 2MB 开始而不是紧接 1MB 的 .boot？
 *      为了和 .boot 区域（引导代码 + 16KB 栈 + 36KB 页表）不重叠，
 *      省去一堆对齐计算；1MB 的空隙无所谓。
 * ==========================================================================*/

ENTRY(_start)                       /* 入口符号：boot32.asm 里的 _start */

KERNEL_PHYS_BASE = 0x0000000000100000;      /* 1MB   ：引导代码与页表的加载地址 */
KERNEL_VIRT_BASE = 0xffffffff80000000;      /* 高半区基址（-2GB） */
KERNEL_CODE_VMA  = 0xffffffff80200000;      /* 内核正文 VMA 起点（对应 LMA 2MB）*/
KERNEL_STACK_SIZE = 0x4000;                 /* 内核栈 16KB（阶段 6 多线程会调大）*/

SECTIONS
{
    /* ================= 低地址区：引导代码 / 页表 / 临时栈 ================= */
    /* 这里的所有东西 VMA == LMA，在开分页前后都能访问 */
    . = KERNEL_PHYS_BASE;

    .boot :
    {
        *(.multiboot2_header)       /* Multiboot2 头必须最先出现，保证在前 32KB 内 */
        *(.boot_text)
        *(.boot_rodata)
        *(.boot_data)
        *(.boot_text64)             /* 64 位入口：必须留在低地址，因为远跳转偏移是 32 位的 */
    }

    .boot_bss :
    {
        *(.boot_bss)                /* 16KB 临时栈 */
    }

    /* 页表必须 4KB 对齐：CR3 的低 12 位被硬件当作标志位使用 */
    . = ALIGN(4096);
    .boot_pgtable (NOLOAD) :
    {
        *(.boot_pgtable)            /* 9 个页表页 */
    }

    /* ================= 高半区：内核正文 ================= */
    . = KERNEL_CODE_VMA;

    /* ⚠️ 必须用 EXCLUDE_FILE 把 services.o 排除掉！
     *
     * 否则 `*(.text*)` 这个通配符会**先把 services.o 的代码收进来**，
     * 后面的 .user_text 段就什么都收不到了
     * （链接脚本是按顺序依次消费输入节的）。
     *
     * 这个错误的表现非常隐蔽：服务进程照样"创建成功"，
     * 但 _user_text_start/end 指向的段是空的，
     * 算出来的入口偏移成了巨大的无符号数，一跳过去就崩。
     */
    .text : AT(ADDR(.text) - KERNEL_VIRT_BASE)
    {
        EXCLUDE_FILE(build/kernel/services.o) *(.text .text.*)
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE)
    {
        EXCLUDE_FILE(build/kernel/services.o) *(.rodata .rodata.*)
    }

    /* .init_array：全局对象构造函数的指针数组。
       KEEP() 防止链接器因为「没人引用」而把它整段丢掉。 */
    .init_array ALIGN(8) : AT(ADDR(.init_array) - KERNEL_VIRT_BASE)
    {
        __init_array_start = .;
        KEEP(*(.init_array*))
        KEEP(*(.ctors*))
        __init_array_end = .;
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VIRT_BASE)
    {
        *(EXCLUDE_FILE(build/kernel/services.o) .data .data.*)

        /* 整个镜像的物理结束地址，存成变量而不是 ABS 符号。
         *
         * 为什么要存进内存？
         *   ABS 符号（_image_phys_end = ...）的值是物理地址 0x233000，
         *   而内核代码在 0xffffffff802xxxxx，相距远超 2GB。
         *   访问 ABS 符号时编译器生成 PC32 **相对**寻址，必然溢出：
         *     relocation truncated to fit: R_X86_64_PC32
         *   存成变量后变量自身在高半区，相对寻址就能覆盖。
         *
         * 为什么必须放在 .data（而不是 .user_text 之后）？
         *   链接脚本的 . 计数器在 .user_text 之后停在用户空间
         *   (0x402000)，放那里 VMA 同样离内核太远，一样溢出。
         */
        . = ALIGN(8);
        _image_phys_end_store = .;
        QUAD(LOADADDR(.user_data) + SIZEOF(.user_data));
    }
    .data_unused_placeholder_for_replace : AT(ADDR(.data) - KERNEL_VIRT_BASE)
    {
        *(EXCLUDE_FILE(build/kernel/services.o) .data*)
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VIRT_BASE)
    {
        _bss_start = .;             /* boot64.asm 会按这两个符号手动清零 .bss */
        *(EXCLUDE_FILE(build/kernel/services.o) COMMON)
        *(EXCLUDE_FILE(build/kernel/services.o) .bss*)
        . = ALIGN(16);
        _bss_end = .;
    }

    /* 内核栈：放在高半区，紧跟在 .bss 之后 */
    . = ALIGN(16);
    .stack (NOLOAD) : AT(ADDR(.stack) - KERNEL_VIRT_BASE)
    {
        _stack_bottom = .;
        . = . + KERNEL_STACK_SIZE;
        . = ALIGN(16);
        _stack_top = .;             /* boot64.asm 把 rsp 指向这里 */
    }

    /* 内核镜像的边界符号，阶段 4 做物理内存管理时要用来「避开内核占用的页」 */
    _kernel_virt_start = ADDR(.text);
    _kernel_virt_end   = _stack_top;
    _kernel_phys_start = LOADADDR(.text);
    /* ⚠️ 必须算到 **.stack 之后**，不能只算到 .bss！
     *
     * .stack 段紧跟在 .bss 后面。如果这里只算 .bss 的结束，
     * 后面 .user_text 的 LMA 就会落在 .stack 上 ——
     * GRUB 加载内核时会**用服务代码覆盖掉内核栈**，
     * 表现是系统一启动就黑屏（连第一条 kprintf 都打不出来）。
     *
     * 实测：只算 .bss 时 _kernel_phys_end=0x22f000，
     *      而内核正文段物理范围是 0x20a000~0x232a70，
     *      0x22f000 正好落在里面 —— 段重叠。
     */
    _kernel_phys_end   = LOADADDR(.stack) + SIZEOF(.stack);

    /* ================= 用户态服务代码 =================
     *
     * 【关键设计】VMA 就在用户地址空间（0x400000）。
     *
     * 为什么？
     *   services.o 的代码会引用字符串常量和函数地址，
     *   链接后都变成**绝对地址**。若链接在内核高半区，
     *   用户态一访问就 #PF（U/S=0 权限不足）——
     *   实测 CR2=0xffffffff8020af7a 正是这么来的。
     *   VMA 设在 0x400000 后，代码里的绝对引用天然是用户地址，
     *   映射进用户进程即可直接运行，无需重定位。
     *
     * LMA 紧跟内核正文之后（放在这里才能算出正确的物理位置）——
     * 注意不能直接写 AT(ALIGN(4096))，那会让 LMA 从头开始算，
     * 把内核正文的加载地址挤乱，导致大量 R_X86_64_PC32 重定位溢出。
     */
    .user_text 0x400000 : AT(ALIGN(_kernel_phys_end, 4096))
    {
        _user_text_start = .;
        build/kernel/services.o(.text .text.*)
        build/kernel/services.o(.rodata .rodata.*)
        . = ALIGN(4096);
        _user_text_end   = .;
    }

    /* ⚠️ _user_text_lma 必须定义在段**外面**！
     *
     * 写在段内的话，它是一个"段内符号"，
     * C++ 里 &_user_text_lma 取到的是它所在的**虚拟地址**
     * （也就是 0x400000 附近），而不是我们想要的物理加载地址。
     *
     * 定义在段外则成为 ABS 符号，其值就是 LOADADDR(...) 本身，
     * C++ 取地址才拿得到正确的物理地址。
     * （实测：写在段内时 phys 读成 0x400000，映射到了错误的地方）
     */
    _user_text_lma = LOADADDR(.user_text);

    /* ================= 用户态服务的可写数据段 =================
     *
     * 【为什么必须有这一段】
     *   services.o 是用 -UKERNEL_BUILD 编译的用户态代码，
     *   它的代码段被链接到 0x400000（用户空间，见上方 .user_text）。
     *   但它里的全局变量（鼠标服务的 g_mx/g_my/g_btn/g_pkt 等）
     *   默认会落进内核的 .data/.bss —— 那些在高半区 0xffffffff80xxxxxx。
     *
     *   于是代码（0x400000）访问变量（高半区）的 PC32 相对偏移约 2^63，
     *   链接器直接报：
     *     relocation truncated to fit: R_X86_64_PC32 against `.bss'
     *
     *   解决：给 services.o 的 .data/.bss 单独开一段，
     *   VMA 同样放在用户空间（0x500000），与代码段只差 1MB，
     *   PC32 轻松覆盖；LMA 紧跟 .user_text 的物理末尾。
     */
    .user_data 0x500000 : AT(ALIGN(_user_text_lma + SIZEOF(.user_text), 4096))
    {
        _user_data_start = .;
        build/kernel/services.o(.data .data.*)
        build/kernel/services.o(.bss .bss.*)
        . = ALIGN(4096);
        _user_data_end   = .;
    }
    _user_data_lma = LOADADDR(.user_data);


    /* ================= 整个镜像的物理结束 =================
     *
     * ⚠️ 这个符号**必须包含 .user_text**！
     *
     * PMM（物理页帧分配器）把位图放在 "内核结束之后"。
     * 如果它只算到 .stack，位图就会落在 .user_text 的加载地址上 ——
     * PMM 初始化清零位图时，会把用户态服务代码**擦成全 0**。
     *
     * 症状极具迷惑性：
     *   用户进程一切正常（页表对、映射对、CR3 切了、cpl=3），
     *   但取到的指令是全 0，CPU 把它解码成 add [rax],al，
     *   于是访问地址 0 —— 表现为 CR2=0x0 的页错误。
     *   查遍页表都找不到问题，因为页表确实没错。
     *
     * 实测：.user_text LMA=0x233000，位图范围 0x232a70~0x234a70，正好重叠。
     */


    /DISCARD/ :
    {
        *(.note.GNU-stack)
        *(.note.gnu.property)
        *(.comment)
        *(.eh_frame*)               /* 没有异常展开机制，调试信息用不上，丢掉 */
    }
}
