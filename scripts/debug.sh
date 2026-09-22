#!/usr/bin/env bash
# =============================================================================
#  scripts/debug.sh —— QEMU 停在启动处，等待 GDB 连接
#  用法：
#    终端 1： bash scripts/debug.sh
#    终端 2： gdb build/myos.elf
#            (gdb) target remote :1234
#            (gdb) break *0x100000        # 32 位入口的物理地址
#            (gdb) continue
#            (gdb) layout asm / info registers
# =============================================================================
set -u

ISO="${1:-build/myos.iso}"
ELF="build/myos.elf"

if [ ! -f "$ISO" ]; then
    echo "错误：找不到 $ISO，请先执行 make iso"
    exit 1
fi

echo "QEMU 已冻结在启动瞬间，等待 GDB 连接 localhost:1234"
echo "另开终端执行： gdb $ELF -ex 'target remote :1234'"
echo ""

qemu-system-x86_64 \
    -cdrom "$ISO" \
    -m 256M \
    -no-reboot \
    -S \
    -gdb tcp::1234 \
    -debugcon file:build/debugcon.log \
    -display none
