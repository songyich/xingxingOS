#!/usr/bin/env bash
# =============================================================================
#  scripts/run-uefi.sh —— 用 QEMU 以 UEFI 固件启动 xingxingOS
#  用法： bash scripts/run-uefi.sh <iso路径> <OVMF固件路径>
# ---------------------------------------------------------------------------
#  与 BIOS 启动的区别只有两点：
#    1. -bios <OVMF.fd>：让 QEMU 加载 UEFI 固件而不是 SeaBIOS
#    2. GRUB 会走 /EFI/BOOT/BOOTX64.EFI，而不是 ISO 的 El Torito 引导记录
#
#  UEFI 下的帧缓冲来自 GOP（Graphics Output Protocol），
#  GRUB 把它翻译成 Multiboot2 的帧缓冲标签交给内核——
#  所以内核里的代码路径和 BIOS/VBE 完全一致。
# =============================================================================
set -u

ISO="${1:-build/xingxingos.iso}"
OVMF="${2:-/usr/share/OVMF/OVMF_CODE.fd}"
LOG_DIR="$(dirname "$ISO")"
DEBUGCON_LOG="$LOG_DIR/debugcon.log"
SERIAL_LOG="$LOG_DIR/serial.log"

if [ ! -f "$ISO" ]; then
    echo "ERROR: $ISO not found. Run 'make iso' first."
    exit 1
fi
if [ ! -f "$OVMF" ]; then
    echo "ERROR: OVMF firmware not found at $OVMF"
    echo "       Ubuntu/Debian: sudo apt install ovmf"
    exit 1
fi

if [ -n "${QEMU_DISPLAY:-}" ]; then
    DISPLAY_OPT="-display $QEMU_DISPLAY"
elif [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
    DISPLAY_OPT=""
else
    DISPLAY_OPT="-display none"
fi

rm -f "$DEBUGCON_LOG" "$SERIAL_LOG"

show_logs() {
    echo ""
    if [ -f "$SERIAL_LOG" ] && [ -s "$SERIAL_LOG" ]; then
        echo "=== serial (kernel output) ==="
        cat "$SERIAL_LOG"
        echo "=============================="
    fi
}
trap show_logs EXIT INT TERM

echo "Booting QEMU in UEFI mode: $ISO"
echo "  firmware    : $OVMF"
echo "  debugcon log: $DEBUGCON_LOG"
echo "  serial  log : $SERIAL_LOG"
echo ""

qemu-system-x86_64 \
    -bios "$OVMF" \
    -cdrom "$ISO" \
    -m 256M \
    -no-reboot \
    -debugcon "file:$DEBUGCON_LOG" \
    -serial "file:$SERIAL_LOG" \
    $DISPLAY_OPT \
    ${QEMU_EXTRA:-}
