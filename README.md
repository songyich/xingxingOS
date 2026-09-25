# xingxingOS —— 星星系统

从零手写的 x86_64 微内核操作系统，中文界面。

## 核心理念：万物皆可程序

命令、驱动、服务全是独立的 `.xzs` 程序，由 ELF 加载器运行。
内核只保留调度、内存、IPC、中断分发。

## 现状（P2 崩溃自愈）

- 微内核：7 个用户态服务进程，独立页表 + Ring 3
- **崩溃自愈**：服务崩溃只杀它一个，系统不挂，自动重启并沿用原 tid
- 开机动画：五角星螺旋入场 → 收缩 → 加载 → 放出 → 背景上移
- 启动蓝屏：非致命错误可按 Enter 继续加载
- 已适配 PS/2 鼠标、ACPI 真关机
- 命令：help uptime echo ps clear kill reboot shutdown log

验收崩溃自愈：`kill keyboard` → 系统不挂，键盘几秒内恢复

## 编译运行

```bash
make iso          # 生成 build/xingxingos.iso
make qemu         # QEMU 启动
```

依赖：`build-essential nasm grub-pc-bin xorriso mtools qemu-system-x86`

## 路线

P2 崩溃自愈 → P3 存储 → P4 网络 → P5 用户与运行环境 → P6 双版本与 UI

## 交流

QQ 交流群：**689036241**
