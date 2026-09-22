// ============================================================================
//  kernel/keyboard.cpp —— PS/2 键盘驱动（阶段 5：支持方向键与功能键）
//  ---------------------------------------------------------------------------
//  端口：
//    0x60  数据口：读 = 拿扫描码，写 = 发命令给键盘
//    0x64  状态口：读 = 状态寄存器，写 = 发命令给控制器
//
//  扫描码集 1 的规则（PC/AT 传统）：
//    按下 = make code（比如 A 键是 0x1E）
//    松开 = break code = make code | 0x80（A 键松开就是 0x9E）
//  所以判断最高位就能知道是按下还是松开。
//
//  扩展键的两字节序列：
//    方向键、Home/End、Insert/Delete、PageUp/PageDown 这些都是「扩展键」，
//    硬件会先发 0xE0 再发真正的扫描码。比如上箭头 = E0 48。
//    如果不处理 E0 前缀，48 会被当成小键盘的「8」——
//    表现就是按方向键会输出一堆数字，shell 的历史记录功能也就无从谈起。
// ============================================================================

#include <kernel/keyboard.hpp>
#include <kernel/io.h>
#include <kernel/isr.hpp>
#include <kernel/pic.hpp>
#include <kernel/thread.hpp>

namespace {

constexpr u16 KB_DATA_PORT   = 0x60;
constexpr u16 KB_STATUS_PORT = 0x64;

// ---------------------------------------------------------------------------
//  扫描码 -> 字符映射表（Set 1，普通键）
//  每行两个字符：[未按 Shift] [按住 Shift]
//  0 表示这个键不产生字符
// ---------------------------------------------------------------------------
const char kScanMap[128][2] = {
    {0,    0   },  // 0x00
    {0,    0   },  // 0x01 Esc（单独处理）
    {'1',  '!' },  // 0x02
    {'2',  '@' },  // 0x03
    {'3',  '#' },  // 0x04
    {'4',  '$' },  // 0x05
    {'5',  '%' },  // 0x06
    {'6',  '^' },  // 0x07
    {'7',  '&' },  // 0x08
    {'8',  '*' },  // 0x09
    {'9',  '(' },  // 0x0A
    {'0',  ')' },  // 0x0B
    {'-',  '_' },  // 0x0C
    {'=',  '+' },  // 0x0D
    {'\b', '\b'},  // 0x0E 退格
    {'\t', '\t'},  // 0x0F Tab
    {'q',  'Q' },  // 0x10
    {'w',  'W' },  // 0x11
    {'e',  'E' },  // 0x12
    {'r',  'R' },  // 0x13
    {'t',  'T' },  // 0x14
    {'y',  'Y' },  // 0x15
    {'u',  'U' },  // 0x16
    {'i',  'I' },  // 0x17
    {'o',  'O' },  // 0x18
    {'p',  'P' },  // 0x19
    {'[',  '{' },  // 0x1A
    {']',  '}' },  // 0x1B
    {'\n', '\n'},  // 0x1C 回车
    {0,    0   },  // 0x1D 左 Ctrl
    {'a',  'A' },  // 0x1E
    {'s',  'S' },  // 0x1F
    {'d',  'D' },  // 0x20
    {'f',  'F' },  // 0x21
    {'g',  'G' },  // 0x22
    {'h',  'H' },  // 0x23
    {'j',  'J' },  // 0x24
    {'k',  'K' },  // 0x25
    {'l',  'L' },  // 0x26
    {';',  ':' },  // 0x27
    {'\'', '"' },  // 0x28
    {'`',  '~' },  // 0x29
    {0,    0   },  // 0x2A 左 Shift
    {'\\', '|' },  // 0x2B
    {'z',  'Z' },  // 0x2C
    {'x',  'X' },  // 0x2D
    {'c',  'C' },  // 0x2E
    {'v',  'V' },  // 0x2F
    {'b',  'B' },  // 0x30
    {'n',  'N' },  // 0x31
    {'m',  'M' },  // 0x32
    {',',  '<' },  // 0x33
    {'.',  '>' },  // 0x34
    {'/',  '?' },  // 0x35
    {0,    0   },  // 0x36 右 Shift
    {'*',  '*' },  // 0x37 小键盘 *
    {0,    0   },  // 0x38 左 Alt
    {' ',  ' ' },  // 0x39 空格
    {0,    0   },  // 0x3A CapsLock
    {0,    0   },  // 0x3B F1
    {0,    0   },  // 0x3C F2
    {0,    0   },  // 0x3D F3
    {0,    0   },  // 0x3E F4
    {0,    0   },  // 0x3F F5
    {0,    0   },  // 0x40 F6
    {0,    0   },  // 0x41 F7
    {0,    0   },  // 0x42 F8
    {0,    0   },  // 0x43 F9
    {0,    0   },  // 0x44 F10
    {0,    0   },  // 0x45 NumLock
    {0,    0   },  // 0x46 ScrollLock
    {'7',  '7' },  // 0x47 小键盘 7（注意：E0 前缀时是 Home）
    {'8',  '8' },  // 0x48 小键盘 8（E0 前缀时是上箭头）
    {'9',  '9' },  // 0x49 小键盘 9（E0 前缀时是 PageUp）
    {'-',  '-' },  // 0x4A 小键盘 -
    {'4',  '4' },  // 0x4B 小键盘 4（E0 前缀时是左箭头）
    {'5',  '5' },  // 0x4C 小键盘 5
    {'6',  '6' },  // 0x4D 小键盘 6（E0 前缀时是右箭头）
    {'+',  '+' },  // 0x4E 小键盘 +
    {'1',  '1' },  // 0x4F 小键盘 1（E0 前缀时是 End）
    {'2',  '2' },  // 0x50 小键盘 2（E0 前缀时是下箭头）
    {'3',  '3' },  // 0x51 小键盘 3（E0 前缀时是 PageDown）
    {'0',  '0' },  // 0x52 小键盘 0（E0 前缀时是 Insert）
    {'.',  '.' },  // 0x53 小键盘 .（E0 前缀时是 Delete）
    // 0x54 ~ 0x7F 未使用
};

// ---------------------------------------------------------------------------
//  修饰键状态
// ---------------------------------------------------------------------------
bool g_shift = false;
bool g_ctrl  = false;
bool g_alt   = false;

// 上一个字节是不是 E0 前缀
bool g_extended = false;

// ---------------------------------------------------------------------------
//  环形缓冲区（元素改成 u16，因为功能键编码超过 255）
//
//  volatile 不能省！
//    head / tail 是「中断里写、主循环里读」的共享变量。
//    编译器看主循环时不知道会有中断来改它们，会理所当然地认为
//    head == tail 恒成立，然后把整个读取循环优化掉——
//    表现就是中断计数在涨，但一个字符都读不出来。
// ---------------------------------------------------------------------------
u16 g_buffer[keyboard::BUFFER_SIZE];

volatile int g_head = 0;
volatile int g_tail = 0;

volatile u32 g_interrupt_count = 0;
volatile u32 g_key_count = 0;

// 正在等按键的线程（没有就是 -1）。
// 有了它，等输入时可以真正「阻塞」，让键盘中断来唤醒，
// 而不是让线程反复被调度起来轮询（那样浪费大量 CPU，ps 里也很难看）。
int g_waiter_tid = -1;

void buffer_put(u16 k)
{
    int next = (g_head + 1) % keyboard::BUFFER_SIZE;
    if (next == g_tail) {
        return;                     // 缓冲区满了，丢弃（总比卡死好）
    }
    g_buffer[g_head] = k;
    g_head = next;
}

bool buffer_get(u16* out)
{
    if (g_head == g_tail) {
        return false;
    }
    *out = g_buffer[g_tail];
    g_tail = (g_tail + 1) % keyboard::BUFFER_SIZE;
    return true;
}

// 有键入队后，把等按键的线程叫醒
void wake_waiter()
{
    if (g_waiter_tid >= 0 && thread::enabled()) {
        thread::wake(g_waiter_tid);
        g_waiter_tid = -1;
    }
}

// ---------------------------------------------------------------------------
//  扩展键（E0 前缀之后）的扫描码 -> 功能键编码
//  返回 0 表示这个扫描码没有对应的功能键
// ---------------------------------------------------------------------------
u16 extended_key(u8 code)
{
    switch (code) {
        case 0x48: return keyboard::ARROW_UP;
        case 0x50: return keyboard::ARROW_DOWN;
        case 0x4B: return keyboard::ARROW_LEFT;
        case 0x4D: return keyboard::ARROW_RIGHT;
        case 0x47: return keyboard::KEY_HOME;
        case 0x4F: return keyboard::KEY_END;
        case 0x52: return keyboard::KEY_INSERT;
        case 0x53: return keyboard::KEY_DELETE;
        case 0x49: return keyboard::KEY_PAGEUP;
        case 0x51: return keyboard::KEY_PAGEDN;
        default:   return 0;
    }
}

// 普通功能键（F1~F12、Esc）
u16 function_key(u8 code)
{
    if (code >= 0x3B && code <= 0x44) {         // F1 ~ F10
        return static_cast<u16>(keyboard::KEY_F1 + (code - 0x3B));
    }
    if (code == 0x57) return static_cast<u16>(keyboard::KEY_F1 + 10);   // F11
    if (code == 0x58) return static_cast<u16>(keyboard::KEY_F1 + 11);   // F12
    if (code == 0x01) return keyboard::KEY_ESC;
    return 0;
}

// Ctrl 组合键 -> 控制字符
u16 ctrl_combo(u8 code)
{
    switch (code) {
        case 0x2E: return keyboard::KEY_CTRL_C;    // C
        case 0x26: return keyboard::KEY_CTRL_L;    // L
        case 0x16: return keyboard::KEY_CTRL_U;    // U
        case 0x20: return keyboard::KEY_CTRL_D;    // D
        default:   return 0;
    }
}

// IRQ1 的中断处理函数
void keyboard_handler(const Registers*)
{
    ++g_interrupt_count;

    // 读扫描码。这一步不能省——读数据口同时也在清中断，
    // 不读的话键盘会一直卡在这一次中断上。
    u8 scan_code = inb(KB_DATA_PORT);

    // --- E0 前缀处理 ---
    // E0 后面必须跟一个真正的扫描码，所以这里只置标志位然后返回
    if (scan_code == 0xE0) {
        g_extended = true;
        return;
    }

    u16 key = 0;

    if (g_extended) {
        g_extended = false;

        bool is_release = (scan_code & 0x80) != 0;
        u8 code = scan_code & 0x7F;

        // 扩展键里的修饰键（右 Ctrl / 右 Alt）
        if (code == 0x1D) { g_ctrl = !is_release; return; }
        if (code == 0x38) { g_alt  = !is_release; return; }

        if (is_release) {
            return;                     // 松开不产生按键
        }
        key = extended_key(code);
        if (key == 0) {
            return;                     // 不认识的扩展键，忽略
        }
        buffer_put(key);
        ++g_key_count;
        wake_waiter();
        return;
    }

    // --- 普通键 ---
    bool is_release = (scan_code & 0x80) != 0;
    u8 code = scan_code & 0x7F;

    switch (code) {
        case 0x1D:  g_ctrl  = !is_release;  return;
        case 0x2A:
        case 0x36:  g_shift = !is_release;  return;
        case 0x38:  g_alt   = !is_release;  return;
        default:    break;
    }

    if (is_release) {
        return;
    }

    // Ctrl 组合键优先：Ctrl+C / Ctrl+L 这些比字符本身更有用
    if (g_ctrl) {
        u16 combo = ctrl_combo(code);
        if (combo != 0) {
            buffer_put(combo);
            ++g_key_count;
            wake_waiter();
        }
        return;
    }

    // 功能键（F1~F12、Esc）
    u16 fn = function_key(code);
    if (fn != 0) {
        buffer_put(fn);
        ++g_key_count;
        wake_waiter();
        return;
    }

    // Tab：shell 可能要用它做补全，先作为字面按键送上去
    if (code == 0x0F) {
        buffer_put(keyboard::KEY_TAB_LIT);
        ++g_key_count;
        wake_waiter();
        return;
    }

    // 普通字符
    if (code < 128) {
        char c = kScanMap[code][g_shift ? 1 : 0];
        if (c != 0) {
            buffer_put(static_cast<u16>(static_cast<u8>(c)));
            ++g_key_count;
            wake_waiter();
        }
    }
}

}  // namespace

namespace keyboard {

void init()
{
    flush();
    g_interrupt_count = 0;
    g_key_count = 0;
    g_extended = false;

    // 【微内核改造：内核不再自己处理按键】
    //
    //   键盘驱动已经移到用户态服务进程（keyboard_service_entry）。
    //   如果这里还注册 keyboard_handler，内核会在 IRQ1 时**先把 0x60 读走**
    //   存进自己的缓冲区，等用户态服务再去读，端口已经空了 ——
    //   实测表现：0x64 的 OBF 位永远是 0，一个按键都收不到。
    //
    //   微内核的正确分工：
    //     内核   = 只负责"IRQ 来了通知谁"（deliver_irq）+ 放开中断线
    //     用户态 = 真正的驱动，自己读端口、自己做去抖和缓冲
    //
    //   所以这里**只 unmask，不注册 handler**。
    pic::unmask(pic::IRQ_KEYBOARD);
}

bool has_key()
{
    return g_head != g_tail;
}

u16 read_key()
{
    u16 k = 0;
    buffer_get(&k);
    return k;
}

u16 peek_key()
{
    if (g_head == g_tail) {
        return 0;
    }
    return g_buffer[g_tail];
}

u16 wait_key()
{
    // 阻塞等待按键（事件驱动）。
    //
    // 两种做法的区别：
    //   轮询版：反复被调度 → 检查 → 没键 → 让出。
    //           线程一直挂在就绪队列里，白白吃掉调度机会。
    //   阻塞版：把自己标成阻塞 → 让出 → 键盘中断来了再唤醒。
    //           等输入期间完全不参与调度，CPU 全留给真正干活的线程。
    //
    // 关中断是必须的：
    //   检查「有没有键」和登记「我在等」这两步之间不能被打断，
    //   否则键可能在这缝隙里到达，之后再也不会有人来唤醒我们——死锁。
    for (;;) {
        asm volatile("cli");
        if (g_head != g_tail) {
            asm volatile("sti");
            break;                              // 有键了，去读
        }
        if (thread::enabled()) {
            g_waiter_tid = thread::current_tid();
            thread::block();                    // 阻塞，等键盘中断唤醒
            asm volatile("sti");
        } else {
            asm volatile("sti; hlt");           // 调度器没起来，只能休眠
        }
    }
    return read_key();
}

u32 interrupt_count()
{
    return g_interrupt_count;
}

u32 key_count()
{
    return g_key_count;
}

void flush()
{
    g_head = 0;
    g_tail = 0;
}

}  // namespace keyboard
