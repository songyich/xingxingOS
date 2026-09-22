// 清屏
// 「万物皆可程序」：这是独立的 clear.xzs 程序，
// 由 shell 通过 XingSpawn 启动，不是 shell 的内置命令。
#include <xingxing.h>

extern int XingProcTid(void);

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    XingClear();
    return 0;
}
