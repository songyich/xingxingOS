// 终止一个进程
//
// 「万物皆可程序」：这是独立的 kill.xzs 程序，
// 由 shell 通过 XingSpawn 启动，不是 shell 的内置命令。
//
// 【P2 崩溃自愈的验收工具】
//   用法：kill keyboard       —— 杀掉键盘服务
//   预期：系统**不挂**，键盘在几百毫秒内自动恢复可用
//
//   也可以直接给 tid：kill 3
//
//   这演示了微内核的核心价值：
//   驱动只是普通的用户态进程，杀掉它就像杀掉一个普通程序，
//   内核毫发无伤，监管者还会把它重新拉起来。
#include <xingxing.h>

extern int XingProcTid(void);

// 把十进制字符串转成整数；遇到非数字返回 -1
static int parse_num(const char* s)
{
    if (s == nullptr || s[0] == '\0') return -1;
    int v = 0;
    for (int i = 0; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        XingPrintLn("  用法: kill <服务名 或 tid>");
        XingPrintLn("  例如: kill keyboard");
        XingPrintLn("        kill 3");
        return 1;
    }

    int target = parse_num(argv[1]);

    // 不是纯数字 → 按服务名查
    if (target < 0) {
        target = XingServiceLookup(argv[1]);
        if (target < 0) {
            XingPrint("  找不到服务: ");
            XingPrintLn(argv[1]);
            XingPrintLn("  可用服务: terminal keyboard timer shell power proc mouse");
            return 1;
        }
    }

    // 调用 KILL 系统调用
    I64 r = static_cast<I64>(XingSyscall(SYS_KILL,
                                         static_cast<U64>(target),
                                         0, 0, 0, 0, 0));

    if (r == 0) {
        XingPrint("  已终止进程 ");
        XingPrintU64(static_cast<U64>(target));
        XingPrintLn("（若是服务，监管者会自动重启它）");
        return 0;
    }

    if (r == -2) {
        XingPrintLn("  不允许：那是内核线程，不能杀");
        return 1;
    }

    XingPrintLn("  终止失败（进程不存在？）");
    return 1;
}
