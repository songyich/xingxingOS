#!/usr/bin/env bash
# =============================================================================
#  scripts/run.sh —— 用 QEMU 启动 xingxingOS（BIOS 模式）
#  用法： bash scripts/run.sh [iso路径]
#  环境变量：
#    QEMU_DISPLAY=gtk|sdl|curses|none   强制指定显示后端
#    QEMU_EXTRA="..."                   追加额外 QEMU 参数
# ---------------------------------------------------------------------------
#  本脚本同时抓两个通道的日志，排查时很有用：
#    build/debugcon.log   boot32.asm 早期用 out 0xE9 打出来的引导日志
#    build/serial.log     内核起来后 COM1 串口的输出（kprintf 会同步发一份）
# =============================================================================
set -u

ISO="${1:-build/xingxingos.iso}"
LOG_DIR="$(dirname "$ISO")"
DEBUGCON_LOG="$LOG_DIR/debugcon.log"
SERIAL_LOG="$LOG_DIR/serial.log"

if [ ! -f "$ISO" ]; then
    echo "ERROR: $ISO not found. Run 'make iso' first."
    exit 1
fi

# 显示后端：
#   1. 显式指定了 QEMU_DISPLAY 就以它为准
#   2. 否则先让 QEMU 自己挑（通常弹 GTK 窗口）
#   3. 若启动失败（常见于 SSH / 无 X 权限 / DISPLAY 设了但连不上），
#      自动退回 -display none，靠 debugcon 和串口日志看输出
if [ -n "${QEMU_DISPLAY:-}" ]; then
    DISPLAY_OPT="-display $QEMU_DISPLAY"
else
    DISPLAY_OPT=""
fi

rm -f "$DEBUGCON_LOG" "$SERIAL_LOG"

# 不管 QEMU 是正常退出、被 Ctrl+C、还是被 timeout 杀掉，
# 都把抓到的日志打出来——否则排查时什么都看不到。
show_logs() {
    echo ""
    if [ -f "$DEBUGCON_LOG" ] && [ -s "$DEBUGCON_LOG" ]; then
        echo "=== debugcon (early boot) ==="
        cat "$DEBUGCON_LOG"
        echo "============================="
    fi
    if [ -f "$SERIAL_LOG" ] && [ -s "$SERIAL_LOG" ]; then
        echo "=== serial (kernel output) ==="
        cat "$SERIAL_LOG"
        echo "=============================="
    fi
}
trap show_logs EXIT INT TERM

echo "Booting QEMU: $ISO"
echo "  debugcon log : $DEBUGCON_LOG   (early boot, from boot32.asm)"
echo "  serial  log  : $SERIAL_LOG   (kernel output via COM1)"
echo "  (quit: close window / or press Ctrl-A then X in nographic mode)"
echo ""

qemu-system-x86_64 \
    -cdrom "$ISO" \
    -m 256M \
    -no-reboot \
    -debugcon "file:$DEBUGCON_LOG" \
    -serial "file:$SERIAL_LOG" \
    $DISPLAY_OPT \
    ${QEMU_EXTRA:-}

qemu_status=$?

# 启动失败且没有手动指定后端 -> 退回无窗口模式重试一次
if [ "$qemu_status" -ne 0 ] && [ -z "${QEMU_DISPLAY:-}" ] && [ -z "$DISPLAY_OPT" ]; then
    echo ""
    echo "GUI 启动失败（无可用图形环境），自动切换到 -display none"
    echo ""
    DISPLAY_OPT="-display none"
    qemu-system-x86_64 \
        -cdrom "$ISO" \
        -m 256M \
        -no-reboot \
        -debugcon "file:$DEBUGCON_LOG" \
        -serial "file:$SERIAL_LOG" \
        $DISPLAY_OPT \
        ${QEMU_EXTRA:-}
fi
