#!/usr/bin/env bash
# =============================================================================
#  scripts/env-check.sh —— 工具链自检
#  用法： bash scripts/env-check.sh
#  作用： 逐个检查必需程序是否存在、版本是多少，缺哪个一眼看出来。
#        （阶段 0 最常用的命令，装完工具先跑它）
# =============================================================================
set -u

pass=0
fail=0

# check <显示名> <命令名> <版本命令>              —— 必需，缺失算失败
# check_opt <显示名> <命令名> <版本命令>          —— 可选，缺失只提示
check_raw() {
    local name="$1" cmd="$2" ver_cmd="$3" required="$4"
    if command -v "$cmd" >/dev/null 2>&1; then
        local ver
        ver="$($ver_cmd 2>&1 | head -1)"
        printf "  [OK]      %-22s %s\n" "$name" "$ver"
        pass=$((pass + 1))
    elif [ "$required" = "yes" ]; then
        printf "  [MISSING] %-22s 需要安装（见 README.md 的安装命令）\n" "$name"
        fail=$((fail + 1))
    else
        printf "  [可选]    %-22s 未安装（不影响阶段 0~6）\n" "$name"
    fi
}
check()     { check_raw "$1" "$2" "$3" yes; }
check_opt() { check_raw "$1" "$2" "$3" no;  }

echo "=== myos 工具链自检 ==="
echo ""
echo "--- 必需 ---"
check "g++ (x86_64)"       g++               "g++ --version"
check "nasm"               nasm              "nasm -v"
check "make"               make              "make --version"
check "qemu-system-x86_64" qemu-system-x86_64 "qemu-system-x86_64 --version"
check "xorriso"            xorriso           "xorriso -version"
check "grub-mkrescue"      grub-mkrescue     "grub-mkrescue --version"
check "grub-file"          grub-file         "grub-file --help"

echo ""
echo "--- 可选 ---"
check_opt "gdb"            gdb               "gdb --version"
check_opt "git"            git               "git --version"

echo ""
echo "--- 架构检查 ---"
arch="$(uname -m)"
if [ "$arch" = "x86_64" ] || [ "$arch" = "amd64" ]; then
    printf "  [OK]      开发机架构 %s（目标架构 x86_64，匹配）\n" "$arch"
    pass=$((pass + 1))
else
    printf "  [WARN]    开发机架构 %s，目标是 x86_64，需要交叉编译器\n" "$arch"
    fail=$((fail + 1))
fi

# 检查有没有装真正的 x86_64-elf 交叉编译器（可选）
if command -v x86_64-elf-g++ >/dev/null 2>&1; then
    printf "  [OK]      检测到交叉编译器 x86_64-elf-g++，可用 make CROSS=x86_64-elf-\n"
else
    printf "  [INFO]    未检测到 x86_64-elf-g++（可选），默认使用宿主 g++\n"
fi

echo ""
echo "=== 结果：通过 $pass 项，缺失 $fail 项 ==="
if [ "$fail" -gt 0 ]; then
    echo "请先安装缺失的工具，命令见 README.md 的『工具链安装』一节。"
    exit 1
fi
echo "工具链完整，可以执行 make iso。"
exit 0
