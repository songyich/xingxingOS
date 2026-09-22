// 显示系统运行时长
// 「万物皆可程序」：这是独立的 uptime.xzs 程序，
// 由 shell 通过 XingSpawn 启动，不是 shell 的内置命令。
#include <xingxing.h>

extern int XingProcTid(void);

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    U64 ms = XingUptime();
    XingPrint("  已运行 ");
    XingPrintU64(ms);
    XingPrintLn(" ms");
    return 0;
}
