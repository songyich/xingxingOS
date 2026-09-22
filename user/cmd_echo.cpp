// 回显参数
// 「万物皆可程序」：这是独立的 echo.xzs 程序，
// 由 shell 通过 XingSpawn 启动，不是 shell 的内置命令。
#include <xingxing.h>

extern int XingProcTid(void);

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (i > 1) XingPrint(" ");
        XingPrint(argv[i]);
    }
    XingPrint("\n");
    return 0;
}
