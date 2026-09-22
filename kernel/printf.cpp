// ============================================================================
//  kernel/printf.cpp —— kprintf 的实现
//  ---------------------------------------------------------------------------
//  整体流程就是「扫描格式串 + 状态机」：
//    1. 逐字符扫描，普通字符直接输出；
//    2. 遇到 '%' 进入解析状态：依次读标志、宽度、长度修饰符、转换符；
//    3. 按 va_arg 取出对应类型的参数，转成字符串，套用宽度和对齐后输出。
//
//  va_list 从哪来？
//    <stdarg.h> 是 freestanding 环境允许使用的少数标准头之一——它不依赖 libc，
//    只是编译器内置的一组宏（x86_64 上靠寄存器和栈溢出区取参数）。
// ============================================================================

#include <kernel/printf.hpp>
#include <kernel/terminal.hpp>
#include <kernel/serial.hpp>

#include <stdarg.h>

namespace {

// ---------------------------------------------------------------------------
//  数字转字符串（自己实现 itoa）
//  base 支持 8 / 10 / 16；uppercase 控制十六进制字母大小写
//  返回写入的字符数
// ---------------------------------------------------------------------------
int format_unsigned(u64 value, char* buf, int base, bool uppercase)
{
    const char* lower = "0123456789abcdef";
    const char* upper = "0123456789ABCDEF";
    const char* digits = uppercase ? upper : lower;

    char tmp[32];                 // u64 转二进制最多 64 位，32 够装 8 进制了
    int pos = 0;

    if (value == 0) {
        tmp[pos++] = '0';
    }
    while (value > 0) {
        tmp[pos++] = digits[value % base];
        value /= base;
    }

    // tmp 里是倒着的（先低位后高位），翻转到 buf
    int len = pos;
    for (int i = 0; i < len; ++i) {
        buf[i] = tmp[len - 1 - i];
    }
    buf[len] = '\0';
    return len;
}

// ---------------------------------------------------------------------------
//  把一段文本按「最小宽度 + 对齐方式 + 填充字符」输出
// ---------------------------------------------------------------------------
// out：输出一个字符的回调。
//   console_only 时传 serial::putc，普通时传 console::putc。
//
//  【为什么需要它】原来这里硬写 console::putc，
//   而 console::putc → term::putc → 若 term 内部再用 kprintf_serial 做诊断
//   → 又回到 printf → emit_padded → console::putc …… 无限递归。
//   实测串口刷出 "[A] c=[A] c=[A] c=..." 就是这个环。
//   把"往哪输出"变成参数，就能彻底断开这个环。
// --- 输出目标开关：让 emit_padded 也遵守 serial_only ---
static bool g_serial_only = false;

static void out_console(char c) { console::putc(c); }
static void out_serial(char c)  { serial::putc(c); }

void emit_padded(void (*out)(char), const char* text, int text_len,
                 int width, bool left_align, char pad)
{
    int spaces = width - text_len;
    if (spaces < 0) {
        spaces = 0;
    }

    if (!left_align) {
        for (int i = 0; i < spaces; ++i) {
            out(pad);
        }
    }
    for (int i = 0; i < text_len; ++i) {
        out(text[i]);
    }
    if (left_align) {
        for (int i = 0; i < spaces; ++i) {
            out(' ');   // 左对齐时右侧一律补空格，不补 0
        }
    }
}

// ---------------------------------------------------------------------------
//  核心：带 va_list 的格式化
// ---------------------------------------------------------------------------
int vsnprintf_impl(const char* fmt, va_list args, bool serial_only)
{
    // -----------------------------------------------------------------
    //  【bug 修复】serial_only 以前被完全忽略（函数末尾只有 (void)serial_only;）
    //  ---------------------------------------------------------------
    //  后果：kprintf_serial 声称"只写串口"，实际仍走 console::putc，
    //  而 console::putc 会调 term::putc → 形成**无限递归**：
    //      term::putc → kprintf_serial → console::putc → term::putc → ...
    //
    //  这带来两个实际问题：
    //    1. 想用 kprintf_serial 做"不上屏"的调试输出，结果照样刷屏
    //    2. 在 term::putc 里加任何 kprintf_serial 诊断都会栈溢出
    //
    //  现在按 serial_only 真正分流。
    // -----------------------------------------------------------------
    auto emit = [serial_only](char c) {
        if (serial_only) {
            serial::putc(c);
        } else {
            console::putc(c);
        }
    };
    // emit_padded 需要一个**普通函数指针**，不能直接吃 lambda，
    // 所以这里再包一层静态函数，用文件级开关决定落到哪里。
    g_serial_only = serial_only;
    void (*emit_out)(char) = serial_only ? &out_serial : &out_console;
    int written = 0;
    char buf[32];

    for (const char* p = fmt; *p != '\0'; ++p) {
        if (*p != '%') {
            emit(*p);
            ++written;
            continue;
        }

        ++p;                                  // 跳过 '%'
        if (*p == '\0') {
            break;
        }
        if (*p == '%') {                      // "%%" -> 输出一个 '%'
            emit('%');
            ++written;
            continue;
        }

        // --- 1. 解析标志 ---
        bool left_align  = false;
        bool plus_sign   = false;
        bool space_sign  = false;
        bool zero_pad    = false;
        bool alt_form    = false;

        for (;;) {
            if      (*p == '-') { left_align = true;  ++p; }
            else if (*p == '+') { plus_sign  = true;  ++p; }
            else if (*p == ' ') { space_sign = true;  ++p; }
            else if (*p == '0') { zero_pad   = true;  ++p; }
            else if (*p == '#') { alt_form   = true;  ++p; }
            else break;
        }

        // --- 2. 解析宽度 ---
        int width = 0;
        if (*p == '*') {
            ++p;
            width = va_arg(args, int);
            if (width < 0) {
                left_align = true;
                width = -width;
            }
        } else {
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                ++p;
            }
        }

        // --- 3. 解析长度修饰符 ---
        enum { LEN_DEFAULT, LEN_HH, LEN_H, LEN_L, LEN_LL } length = LEN_DEFAULT;
        if (p[0] == 'h' && p[1] == 'h') { length = LEN_HH; p += 2; }
        else if (p[0] == 'h')           { length = LEN_H;  p += 1; }
        else if (p[0] == 'l' && p[1] == 'l') { length = LEN_LL; p += 2; }
        else if (p[0] == 'l')           { length = LEN_L;  p += 1; }

        // --- 4. 解析转换符并输出 ---
        char conv = *p;

        // 按长度修饰符取出参数，统一扩成 64 位再处理
        auto take_unsigned = [&]() -> u64 {
            switch (length) {
                case LEN_HH: return static_cast<u64>(va_arg(args, int));
                case LEN_H:  return static_cast<u64>(va_arg(args, int));
                case LEN_L:  return static_cast<u64>(va_arg(args, unsigned long));
                case LEN_LL: return static_cast<u64>(va_arg(args, unsigned long long));
                default:     return static_cast<u64>(va_arg(args, unsigned int));
            }
        };
        auto take_signed = [&]() -> i64 {
            switch (length) {
                case LEN_HH: return static_cast<i64>(va_arg(args, int));
                case LEN_H:  return static_cast<i64>(va_arg(args, int));
                case LEN_L:  return static_cast<i64>(va_arg(args, long));
                case LEN_LL: return static_cast<i64>(va_arg(args, long long));
                default:     return static_cast<i64>(va_arg(args, int));
            }
        };

        if (conv == 's') {
            const char* str = va_arg(args, const char*);
            if (str == nullptr) {
                str = "(null)";
            }
            int len = 0;
            while (str[len] != '\0') {
                ++len;
            }
            emit_padded(emit_out, str, len, width, left_align, zero_pad ? '0' : ' ');
            written += (len > width) ? len : width;
        }
        else if (conv == 'c') {
            char c = static_cast<char>(va_arg(args, int));
            emit_padded(emit_out, &c, 1, width, left_align, ' ');
            written += (width > 1) ? width : 1;
        }
        else if (conv == 'd' || conv == 'i') {
            i64 value = take_signed();
            char prefix[2] = {0, 0};
            int prefix_len = 0;
            if (value < 0) {
                prefix[0] = '-';
                prefix_len = 1;
            } else if (plus_sign) {
                prefix[0] = '+';
                prefix_len = 1;
            } else if (space_sign) {
                prefix[0] = ' ';
                prefix_len = 1;
            }
            int len = format_unsigned(value < 0 ? static_cast<u64>(-value)
                                                : static_cast<u64>(value),
                                      buf, 10, false);
            // 符号要紧跟填充字符之后（比如 %+05d -> "+0007"）
            if (zero_pad && !left_align) {
                if (prefix_len > 0) emit(prefix[0]);
                emit_padded(emit_out, buf, len, width - prefix_len, left_align, '0');
            } else {
                char staged[34];
                int slen = 0;
                if (prefix_len > 0) staged[slen++] = prefix[0];
                for (int i = 0; i < len; ++i) staged[slen++] = buf[i];
                emit_padded(emit_out, staged, slen, width, left_align, ' ');
            }
            written += (len + prefix_len > width) ? (len + prefix_len) : width;
        }
        else if (conv == 'u') {
            int len = format_unsigned(take_unsigned(), buf, 10, false);
            emit_padded(emit_out, buf, len, width, left_align, zero_pad ? '0' : ' ');
            written += (len > width) ? len : width;
        }
        else if (conv == 'x' || conv == 'X') {
            int len = format_unsigned(take_unsigned(), buf, 16, conv == 'X');
            char staged[34];
            int slen = 0;
            if (alt_form) {
                staged[slen++] = '0';
                staged[slen++] = conv;         // 'x' 或 'X'
            }
            for (int i = 0; i < len; ++i) staged[slen++] = buf[i];
            emit_padded(emit_out, staged, slen, width, left_align, zero_pad ? '0' : ' ');
            written += (slen > width) ? slen : width;
        }
        else if (conv == 'p') {
            u64 value = reinterpret_cast<u64>(va_arg(args, void*));
            int len = format_unsigned(value, buf, 16, false);
            char staged[34];
            int slen = 0;
            staged[slen++] = '0';
            staged[slen++] = 'x';
            // 指针固定显示 16 位（补前导 0），方便对齐查看
            while (slen - 2 + len < 16) staged[slen++] = '0';
            for (int i = 0; i < len; ++i) staged[slen++] = buf[i];
            emit_padded(emit_out, staged, slen, width, left_align, ' ');
            written += (slen > width) ? slen : width;
        }
        else {
            // 不认识的转换符：原样输出 '%' 和该字符
            emit('%');
            emit(conv);
            written += 2;
        }
    }

    (void)serial_only;
    return written;
}

}  // namespace

// ---------------------------------------------------------------------------
//  console 层：同时往屏幕和串口写
// ---------------------------------------------------------------------------
namespace console {

void putc(char c)
{
    term::putc(c);
    serial::putc(c);
}

void puts(const char* str)
{
    term::puts(str);
    serial::puts(str);
}

void puts_n(const char* str, int len)
{
    for (int i = 0; i < len && str[i] != '\0'; ++i) {
        console::putc(str[i]);
    }
    // 注意：这里不能依赖 str 有 '\0' 结尾（emit_padded 传的是定长片段）
}

}  // namespace console

extern "C" int kprintf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf_impl(fmt, args, false);
    va_end(args);
    return n;
}

int kprintf_serial(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf_impl(fmt, args, true);
    va_end(args);
    return n;
}
