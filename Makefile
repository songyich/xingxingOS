# =============================================================================
#  xingxingOS —— x86_64 命令行操作系统   阶段 2：屏幕输出
#  ---------------------------------------------------------------------------
#  用法：
#    make          # 等价于 make iso
#    make iso      # 编译内核并打包成 ISO（BIOS + UEFI 双启动）
#    make run      # 在 QEMU 中启动（BIOS 模式）
#    make run-uefi # 在 QEMU 中启动（UEFI 模式，需要 OVMF）
#    make debug    # QEMU + GDB（另开终端 gdb build/xingxingos.elf -ex 'target remote :1234'）
#    make check    # 工具链自检
#    make clean    # 清理全部生成物
# =============================================================================

# ---- 交叉编译器前缀 --------------------------------------------------------
# 留空 = 使用宿主的 x86_64 gcc/g++。
# 因为开发机和目标机同为 x86_64，-ffreestanding -nostdlib 下生成的代码与
# x86_64-elf-* 交叉编译器等价，省去两小时的交叉工具链编译。
# 若将来装了真正的交叉编译器： make CROSS=x86_64-elf- ...
CROSS ?=

CXX      := $(CROSS)g++
AS       := nasm
LD       := $(CROSS)g++            # 用 g++ 当链接器驱动，它会自动带上 libgcc
OBJCOPY  := $(CROSS)objcopy
READELF  := $(CROSS)readelf
GRUBFILE := grub-file
MKRESCUE := grub-mkrescue
QEMU     := qemu-system-x86_64

TARGET     := xingxingos
BUILD_DIR  := build
ISO_DIR    := $(BUILD_DIR)/isodir
KERNEL_ELF := $(BUILD_DIR)/$(TARGET).elf
ISO_IMAGE  := $(BUILD_DIR)/$(TARGET).iso

# ---- 编译参数 --------------------------------------------------------------
# -ffreestanding  告诉编译器：没有宿主 OS，别假设标准库存在
# -fno-exceptions/-fno-rtti  内核里没有异常和 RTTI 的运行期支持
# -fno-pic/-fno-pie 内核是绝对定位的，不要生成位置无关代码
# -mno-red-zone   x86_64 的 red zone（栈顶下 128 字节）会被中断踩到，内核必须关掉
# -mcmodel=kernel 代码默认放在高半区 0xffffffff80000000 附近
# -fno-stack-protector  没有 canary 所需的 %fs:0x28 运行期支持
# -nostdinc++     禁用宿主 libstdc++ 头文件，防止误 include
# -MMD -MP：编译时顺带生成 .d 依赖文件，把「这个 .cpp 包含了哪些 .h」记录下来。
# 不加这两个参数的话，改了头文件 make 不会重新编译引用它的 .cpp——
# 于是你改了常量、改了结构体，编译出来的还是旧代码，
# 症状是「明明改了却没效果」，非常难查（本阶段就踩到了）。
CXXFLAGS := -std=c++17 -O2 -g \
            -ffreestanding -fno-exceptions -fno-rtti -fno-pic -fno-pie \
            -mno-red-zone -mcmodel=kernel -fno-stack-protector \
            -fno-threadsafe-statics -nostdinc++ \
            -Wall -Wextra -Wno-unused-parameter -Iinclude \
            -DKERNEL_BUILD \
            -MMD -MP

LDFLAGS  := -T arch/x86_64/linker.ld -nostdlib -static -no-pie \
            -z max-page-size=0x1000 -Wl,--build-id=none

# 统一用 -f elf64：ld 不接受 elf32 与 elf64 目标文件混链。
# 32 位还是 64 位由每个 .asm 源文件顶部的 BITS 指令决定：
#   BITS 32 -> 32 位机器码（arch/x86_64/boot/boot32.asm，开分页之前）
#   BITS 64 -> 64 位机器码（arch/x86_64/boot/boot64.asm 及以后所有汇编）
ASFLAGS := -f elf64 -g -F dwarf

# ---- 源文件 ----------------------------------------------------------------
ASM_SRCS := $(shell find arch -name '*.asm' 2>/dev/null)
CPP_SRCS := $(shell find kernel -name '*.cpp' 2>/dev/null)

ASM_OBJS := $(ASM_SRCS:%.asm=$(BUILD_DIR)/%.o)
CPP_OBJS := $(CPP_SRCS:%.cpp=$(BUILD_DIR)/%.o)

# ---- 用户态 .xzs 程序（万物皆可程序）--------------------------------------
# 每个命令是一个独立程序，编译后打包进 initrd，由内核 ELF 加载器运行。
USER_DIR     := user
USER_SRCS    := $(wildcard $(USER_DIR)/cmd_*.cpp)
USER_XZS     := $(USER_SRCS:$(USER_DIR)/cmd_%.cpp=$(BUILD_DIR)/%.xzs)
INITRD_BIN   := $(BUILD_DIR)/initrd.bin
INITRD_OBJ   := $(BUILD_DIR)/initrd.o

# 用户程序编译参数：
#   不定义 KERNEL_BUILD（走用户态分支）
#   不加 -mcmodel=kernel（程序跑在用户空间低地址）
#   -fno-pie -no-pie：静态定位在 0x400000，内核加载器无需重定位
USER_CXXFLAGS := -std=c++17 -O2 -g \
                 -ffreestanding -fno-exceptions -fno-rtti \
                 -fno-pic -fno-pie -mno-red-zone -fno-stack-protector \
                 -nostdinc++ -Wall -Iinclude

OBJS     := $(ASM_OBJS) $(CPP_OBJS)
DEPS     := $(CPP_OBJS:.o=.d)

# ---- UEFI 相关 -------------------------------------------------------------
# OVMF 是 QEMU 用的 UEFI 固件实现。有了它才能测试 UEFI 启动路径。
# 发行版常见路径都列一遍，取第一个存在的。
OVMF := $(firstword $(wildcard \
            /usr/share/OVMF/OVMF_CODE.fd \
            /usr/share/OVMF/OVMF_CODE_4M.ms.fd \
            /usr/share/ovmf/OVMF.fd \
            /usr/share/qemu/OVMF.fd))

.PHONY: all iso run run-uefi debug check clean

all: iso

# ---- 默认目标：生成 ISO ----------------------------------------------------
# grub-mkrescue 只要系统里同时装了 grub-pc-bin（BIOS）和
# grub-efi-amd64-bin（UEFI），就会自动打出「双启动」ISO：
#   - BIOS  走 i386-pc 的 El Torito 引导记录
#   - UEFI  走 ISO 里的 /EFI/BOOT/BOOTX64.EFI
# 同一个 ISO 两种固件都能起，不需要分别编译。
iso: $(ISO_IMAGE)

# ---- 编译 C++ --------------------------------------------------------------
# 用户态服务代码的特殊编译规则
#
# 为什么要 -U KERNEL_BUILD？
#   services.cpp 里的服务进程运行在 **Ring 3 用户态**，
#   它调用 ipc_call() 时必须走 **syscall** 版本，
#   不能走内核内部函数版本（ipc_call_kernel）——
#   内核函数在高半区、页表项 U/S=0，用户态一调用就 #PF。
#
#   ipc.hpp 就是靠 KERNEL_BUILD 宏来区分这两条路径的，
#   所以这里必须把全局定义的宏**取消掉**。
$(BUILD_DIR)/kernel/services.o: CPPFLAGS_LOCAL := -UKERNEL_BUILD

$(BUILD_DIR)/kernel/services.o: kernel/services.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS_LOCAL) -c $< -o $@

# services.o 有自己专用的规则（要 -U KERNEL_BUILD），
# 必须从通用模式的源文件列表里排除掉。
# 否则两条规则匹配同一个目标，make 报
# "warning: overriding recipe" 并**丢掉**专用规则，
# 结果 services.cpp 仍带着 KERNEL_BUILD 编译——
# 表现为用户态服务一调 ipc_call 就越权访问内核地址而崩溃。
CPP_OBJS_FILTERED := $(filter-out $(BUILD_DIR)/kernel/services.o,$(CPP_OBJS))

$(CPP_OBJS_FILTERED): $(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  [CXX]  $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ---- 汇编：统一规则，位宽由源文件的 BITS 指令决定 -------------------------
$(ASM_OBJS): $(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  [ASM]  $<"
	$(AS) $(ASFLAGS) $< -o $@

# ---- 链接 ------------------------------------------------------------------
# --- 用户程序：crt + 命令源码 -> 链接成 .xzs ---
$(BUILD_DIR)/user/%.o: $(USER_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(USER_CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.xzs: $(BUILD_DIR)/user/cmd_%.o $(BUILD_DIR)/user/xingcrt.o
	@mkdir -p $(dir $@)
	ld -T $(USER_DIR)/link.ld -o $@ $^
	@echo "  [XZS]  $@"

# --- initrd：把所有 .xzs 打包 ---
$(INITRD_BIN): $(USER_XZS)
	@mkdir -p $(dir $@)
	python3 tools/mkinitrd.py $@ $(USER_XZS)

.PHONY: user-programs
user-programs: $(USER_XZS)

$(KERNEL_ELF): $(OBJS) arch/x86_64/linker.ld
	@mkdir -p $(dir $@)
	@echo "  [LD]   $@"
	$(LD) $(LDFLAGS) $(OBJS) -o $@ -lgcc
	@$(GRUBFILE) --is-x86-multiboot2 $@ || \
	  { echo "ERROR: $@ is not a valid multiboot2 image"; exit 1; }
	@echo "  [OK]   multiboot2 check passed"

# ---- 打包 ISO --------------------------------------------------------------
$(ISO_IMAGE): $(KERNEL_ELF) scripts/grub.cfg
	@echo "  [ISO]  $@"
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL_ELF) $(ISO_DIR)/boot/$(TARGET).elf
	@cp scripts/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(MKRESCUE) -o $@ $(ISO_DIR)
	@echo "  [OK]   ISO built: $@"

# ---- 运行 ------------------------------------------------------------------
run: iso
	@bash scripts/run.sh $(ISO_IMAGE)

# UEFI 模式启动。校验一下 OVMF 存不存在，不存在就给出安装提示。
run-uefi: iso
ifeq ($(OVMF),)
	@echo "ERROR: 找不到 OVMF 固件，无法测试 UEFI 启动。"
	@echo "       Ubuntu/Debian: sudo apt install ovmf"
	@echo "       Arch:          sudo pacman -S edk2-ovmf"
	@exit 1
else
	@echo "  [UEFI] 使用固件 $(OVMF)"
	@bash scripts/run-uefi.sh $(ISO_IMAGE) $(OVMF)
endif

debug: iso
	@bash scripts/debug.sh $(ISO_IMAGE)

check:
	@bash scripts/env-check.sh

clean:
	@rm -rf $(BUILD_DIR)
	@echo "  [OK]   cleaned"

# 把自动生成的依赖文件包含进来。
# 前面的 - 表示「文件不存在也不报错」（首次编译时尚未生成）。
-include $(DEPS)
