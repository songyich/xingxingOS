// 关机（ACPI 真断电）
// 「万物皆可程序」：这是独立的 shutdown.xzs 程序，
// 由 shell 通过 XingSpawn 启动，不是 shell 的内置命令。
#include <xingxing.h>

extern int XingProcTid(void);

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    XingPrintLn("  正在关机...");
    XingShutdown();
    XingPrintLn("  关机失败");
    return 1;
}
