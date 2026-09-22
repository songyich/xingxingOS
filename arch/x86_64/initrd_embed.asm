; ============================================================================
;  arch/x86_64/initrd_embed.asm —— 把 initrd 二进制嵌进内核
;  ---------------------------------------------------------------------------
;  为什么用 incbin 而不是 objcopy + 链接脚本？
;    objcopy --rename-section 的段名在增量构建时会失效
;    （实测 initrd.o 段名仍是 .data，链接脚本收不到内容，段大小变 0，
;     启动后 initrd 是空的 —— 表现为"找不到程序"）。
;    incbin 直接在汇编里把文件字节引进来，段归属一目了然，最可靠。
;
;  放在 .rodata：链接脚本已有成熟的 LMA 规则，不会和别的段打架。
; ============================================================================
section .rodata

global _initrd_start
global _initrd_end

align 8
_initrd_start:
    incbin "build/initrd.bin"
align 8
_initrd_end:
