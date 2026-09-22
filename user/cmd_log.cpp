// 控制内核日志落盘
// 「万物皆可程序」：这是独立的 log.xzs 程序，
// 由 shell 通过 XingSpawn 启动，不是 shell 的内置命令。
#include <xingxing.h>

extern int XingProcTid(void);

int main(int argc, char** argv)
{
    if (argc < 2) {
        XingPrintLn("  用法：log on | log off | log flush");
        XingPrintLn("  说明：日志**始终在内存里记录**，");
        XingPrintLn("        这里控制的只是要不要输出到串口。");
        return 0;
    }

    // 简单的字符串比较（没有 libc）
    auto eq = [](const char* a, const char* b) -> BOOL {
        while (*a != '\0' && *b != '\0') {
            if (*a != *b) return XING_FALSE;
            ++a; ++b;
        }
        return (*a == '\0' && *b == '\0') ? XING_TRUE : XING_FALSE;
    };

    if (eq(argv[1], "on")) {
        XingLogEnable(XING_TRUE);
        XingPrintLn("  日志落盘：开（每 2 秒一次）");
    } else if (eq(argv[1], "off")) {
        XingLogEnable(XING_FALSE);
        XingPrintLn("  日志落盘：关（内存仍在记录）");
    } else if (eq(argv[1], "flush")) {
        XingLogFlush();
        XingPrintLn("  已落盘");
    } else {
        XingPrint("  未知参数：");
        XingPrintLn(argv[1]);
        return 1;
    }
    return 0;
}
