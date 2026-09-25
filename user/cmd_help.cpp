// 显示可用命令列表
// 「万物皆可程序」：这是独立的 help.xzs 程序，
// 由 shell 通过 XingSpawn 启动，不是 shell 的内置命令。
#include <xingxing.h>

extern int XingProcTid(void);

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    XingPrintLn("  可用命令：");
    XingPrintLn("    help      显示帮助");
    XingPrintLn("    uptime    运行时长");
    XingPrintLn("    clear     清屏");
    XingPrintLn("    echo      回显");
    XingPrintLn("    ps        线程列表");
    XingPrintLn("    reboot    重启");
    XingPrintLn("    shutdown  关机（ACPI 真断电）");
    XingPrintLn("    log       on/off/flush 日志落盘");
    XingPrintLn("    kill      终止进程（验证崩溃自愈：kill keyboard）");
    return 0;
}
