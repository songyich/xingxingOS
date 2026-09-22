// 列出系统中的所有线程
// 「万物皆可程序」：这是独立的 ps.xzs 程序，
// 由 shell 通过 XingSpawn 启动，不是 shell 的内置命令。
#include <xingxing.h>

extern int XingProcTid(void);

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    int tid = XingProcTid();
    if (tid < 0) {
        XingPrintLn("  进程服务不可用");
        return 1;
    }

    XingMsg* m = reinterpret_cast<XingMsg*>(XING_SLOT_SEND);
    m->type = MSG_PS;
    m->len = 0;
    m->a = 0; m->b = 0; m->c = 0; m->d = 0;
    m->ptr = 0; m->size = 0;

    if (XingIpcCall(tid, m) != 0) {
        XingPrintLn("  无法获取线程列表");
        return 1;
    }

    int n = static_cast<int>(m->a);
    if (n <= 0) {
        XingPrintLn("  没有线程");
        return 0;
    }

    // 结果在共享页偏移 1024 处，每条 32 字节：
    //   +0 tid  +4 state  +8 ticks  +16 name[16]
    const U8* buf = reinterpret_cast<const U8*>(XING_IPC_PAGE + 1024);

    XingPrintLn(" TID  状态    时间片    名称");
    XingPrintLn("----  ------  --------  ----------------");

    for (int i = 0; i < n && i < 32; ++i) {
        const U8* e = buf + i * 32;

        U32 tidv  = *reinterpret_cast<const U32*>(e + 0);
        U32 state = *reinterpret_cast<const U32*>(e + 4);
        U64 ticks = *reinterpret_cast<const U64*>(e + 8);
        const char* name = reinterpret_cast<const char*>(e + 16);

        XingPrint(" ");
        XingPrintU64(tidv);
        XingPrint("    ");

        const char* st = "?";
        if (state == 1)      st = "运行";
        else if (state == 2) st = "就绪";
        else if (state == 3) st = "阻塞";
        XingPrint(st);
        XingPrint("  ");

        XingPrintU64(ticks);
        XingPrint("    ");
        XingPrintLn(name);
    }
    return 0;
}
