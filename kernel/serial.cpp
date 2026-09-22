// ============================================================================
//  kernel/serial.cpp —— 8250/16550 UART 串口驱动
//  ---------------------------------------------------------------------------
//  COM1 的 8 个寄存器（基址 0x3F8，用「基址 + 偏移」寻址）：
//    偏移 0  数据寄存器（写=发送，读=接收）；把 DLAB 置 1 后变成波特率除数低字节
//    偏移 1  中断使能；DLAB=1 时变成波特率除数高字节
//    偏移 2  FIFO 控制
//    偏移 3  线路控制（LCR）：数据位/校验/停止位，bit7 = DLAB 开关
//    偏移 4  调制解调器控制（MCR）
//    偏移 5  线路状态（LSR）：bit5 = 发送缓冲空，可以写下一个字节
//    偏移 6  调制解调器状态
//    偏移 7  暂存
//
//  波特率怎么设：
//    芯片的基准时钟是 115200，除数 divisor = 115200 / 想要波特率。
//    想要 115200 就填 1。写除数前必须先把 LCR 的 bit7（DLAB）置 1，
//    写完再清掉——这是硬件规定的步骤。
// ============================================================================

#include <kernel/serial.hpp>
#include <kernel/io.h>

namespace {

bool g_ready = false;

// 等待发送缓冲区空
// LSR 的 bit5 = THR empty，表示可以写下一个字节
// 加个超时上限，防止串口芯片不存在时死循环卡死整个内核
bool wait_transmit_empty()
{
    const int kMaxSpin = 100000;
    for (int i = 0; i < kMaxSpin; ++i) {
        if (inb(serial::COM1 + 5) & 0x20) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace serial {

bool init()
{
    // 1) 先关掉串口中断：现在还没装 IDT，来个中断就是三重错误
    outb(serial::COM1 + 1, 0x00);

    // 2) 打开 DLAB，准备写波特率除数
    outb(serial::COM1 + 3, 0x80);
    io_wait();

    // 3) 除数 = 1 -> 波特率 115200
    outb(serial::COM1 + 0, 0x01);           // 低字节
    outb(serial::COM1 + 1, 0x00);           // 高字节
    io_wait();

    // 4) 8N1：8 位数据、无校验、1 位停止位，同时关掉 DLAB
    outb(serial::COM1 + 3, 0x03);
    io_wait();

    // 5) 打开 FIFO 并清空，阈值设成 14 字节
    outb(serial::COM1 + 2, 0xC7);
    io_wait();

    // 6) 让 RTS/DTR 有效，并打开中断允许位（我们不用中断，但让芯片进入工作状态）
    outb(serial::COM1 + 4, 0x0B);
    io_wait();

    // 7) 自检：切到回环模式，发一个字节看能不能原样读回来
    outb(serial::COM1 + 4, 0x1E);           // 回环模式
    io_wait();
    outb(serial::COM1 + 0, 0xAE);           // 发个特征值
    io_wait();

    u8 echoed = inb(serial::COM1 + 0);
    if (echoed != 0xAE) {
        g_ready = false;
        return false;               // 芯片不存在或坏了
    }

    // 8) 自检通过，切回正常工作模式
    outb(serial::COM1 + 4, 0x0F);
    io_wait();

    g_ready = true;
    return true;
}

bool ready()
{
    return g_ready;
}

void putc(char c)
{
    if (!g_ready) {
        return;
    }
    // 串口只认 '\n' 不认 '\r'，所以换行时补一个 '\r'，
    // 否则在真实终端上会看到阶梯状错位
    if (c == '\n') {
        if (wait_transmit_empty()) outb(serial::COM1, '\r');
    }
    if (wait_transmit_empty()) {
        outb(serial::COM1, static_cast<u8>(c));
    }
}

void puts(const char* str)
{
    if (str == nullptr) {
        return;
    }
    while (*str != '\0') {
        putc(*str);
        ++str;
    }
}

}  // namespace serial
