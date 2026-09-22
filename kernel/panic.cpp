// ============================================================================
//  kernel/panic.cpp —— 蓝屏的实现
//  ---------------------------------------------------------------------------
//  为什么 panic 里不直接用 kprintf？
//    panic 是在「系统已经不正常」时被调用的，此时堆可能坏了、格式化逻辑依赖的
//    状态也可能坏了。所以这里刻意走最朴素的路子：直接调用终端和串口的字符级
//    输出，不经过任何可能出问题的中间层。这是内核调试的通用原则——
//    错误处理路径要尽可能短、尽可能少依赖。
//
//  蓝屏长什么样：
//    整屏刷成蓝色，白字逐行打印标题、错误信息、出错位置、停机提示。
//    用图形后端时能真正看到蓝色背景；VGA 文本后端下用蓝底白字模拟。
// ============================================================================

#include <kernel/panic.hpp>
#include <kernel/terminal.hpp>
#include <kernel/framebuffer.hpp>
#include <kernel/serial.hpp>
#include <kernel/font.hpp>

namespace {

// 同时写屏幕和串口（panic 专用，不走 kprintf）
void raw_putc(char c)
{
    term::putc(c);
    serial::putc(c);
}

void raw_puts(const char* str)
{
    if (str == nullptr) {
        return;
    }
    while (*str != '\0') {
        raw_putc(*str);
        ++str;
    }
}

// 十进制输出，用来打印行号
void raw_put_dec(int value)
{
    char buf[16];
    int pos = 15;
    buf[pos] = '\0';

    if (value == 0) {
        raw_putc('0');
        return;
    }
    bool negative = value < 0;
    unsigned int v = negative ? static_cast<unsigned int>(-value)
                              : static_cast<unsigned int>(value);
    while (v > 0) {
        --pos;
        buf[pos] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    if (negative) {
        raw_putc('-');
    }
    raw_puts(&buf[pos]);
}

}  // namespace

void panic(const char* message, const char* file, int line)
{
    // 1) 关中断：不让任何中断处理器再跑起来，避免二次破坏
    asm volatile("cli");

    // 2) 整屏刷蓝
    //    统一走 term::clear()：它按当前的前景色/背景色清屏，
    //    图形后端会真的填成蓝色，VGA 后端会填成蓝底白字（属性 0x1F）。
    //    顺带把光标归位到左上角。
    term::set_color(fb::color::WHITE, fb::color::BLUE);
    term::clear();

    // 3) 开始打印
    raw_puts("\n");
    raw_puts("  ==========================================================\n");
    raw_puts("                     KERNEL  PANIC\n");
    raw_puts("  ==========================================================\n");
    raw_puts("\n");

    raw_puts("  xingxingOS hit an unrecoverable error and has stopped.\n");
    raw_puts("\n");

    raw_puts("  reason : ");
    raw_puts(message != nullptr ? message : "(无)");
    raw_puts("\n");

    raw_puts("  source : ");
    raw_puts(file != nullptr ? file : "(未知)");
    raw_putc(':');
    raw_put_dec(line);
    raw_puts("\n");

    raw_puts("\n");
    raw_puts("  ----------------------------------------------------------\n");
    raw_puts("  Write down the info above. Under QEMU use: make debug + gdb.\n");
    raw_puts("  ----------------------------------------------------------\n");
    raw_puts("\n");

    // 4) 停机。hlt 之后如果被 NMI 唤醒，再 hlt，始终保持停机状态。
    for (;;) {
        asm volatile("hlt");
    }
}
