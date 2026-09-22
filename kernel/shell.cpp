// ============================================================================
//  kernel/shell.cpp —— 交互式命令行 shell
//  ---------------------------------------------------------------------------
//  整个 shell 就三件事：
//    1. read_line()  读一行，带行编辑（光标移动、退格、历史上下翻）
//    2. parse()      把一行按空格切成 argc/argv
//    3. dispatch()   在命令表里找名字，找到了就调用
//
//  行编辑为什么值得做？
//    只能「一路往前敲、退格只能删最后一个字符」的 shell 用起来非常难受。
//    支持左右方向键移动光标后，改命令中间的一个字母才不用删掉后半句重写。
//    这是命令行体验的分水岭，代码量却只多几十行。
// ============================================================================

#include <kernel/shell.hpp>
#include <kernel/terminal.hpp>
#include <kernel/framebuffer.hpp>
#include <kernel/printf.hpp>
#include <kernel/keyboard.hpp>
#include <kernel/pit.hpp>
#include <kernel/pmm.hpp>
#include <kernel/vmm.hpp>
#include <kernel/heap.hpp>
#include <kernel/panic.hpp>
#include <kernel/serial.hpp>
#include <kernel/multiboot2.hpp>
#include <kernel/io.h>
#include <kernel/thread.hpp>

// kmain.cpp 保存的 Multiboot2 信息结构地址（lsmem 命令要用）。
// 注意：必须在匿名 namespace **之外**声明。
// 写在里面的话，编译器会去找带命名空间修饰的符号，链接时报 undefined reference。
extern u64 g_mb2_info_phys;

namespace {

// ---------------------------------------------------------------------------
//  极简字符串工具
//  -------------------------------------------------------------------------
//  freestanding 环境下没有 <cstring>，这几个是 shell 需要的全部
// ---------------------------------------------------------------------------
int kstrlen(const char* s)
{
    int n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

bool kstreq(const char* a, const char* b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a; ++b;
    }
    return *a == *b;
}

// ---------------------------------------------------------------------------
//  shell 状态
// ---------------------------------------------------------------------------
char g_line[shell::MAX_LINE];        // 当前正在编辑的行
int  g_len = 0;                      // 当前长度
int  g_cur = 0;                      // 光标在行内的位置（0~g_len）

// 上一次**实际画到屏幕上**的内容长度（不含提示符）
//
// 为什么必须单独记一个？
//   redraw_line() 被调用时，g_len 往往已经变了——
//   比如按上箭头载入历史命令，g_len 在 history_load() 里就被改成了新命令的长度。
//   如果清理屏幕时用 g_len（新值）去擦除，而屏幕上显示的是**旧内容**，
//   两者长度不一致：新值短就擦不干净、留一截残字；新值长就擦过头、把提示符抹掉。
//   连续翻历史时误差累积，屏幕就彻底乱了。
//
//   所以「清理」必须用上一次真实渲染的长度，「重画」才用当前 g_len。
int  g_rendered_len = 0;

// 上一次渲染后**光标停在行内的哪个位置**
//
// 同理：redraw_line() 里「先把光标退回行首」这一步要用 g_cur，
// 但按上箭头时 g_cur 已经被 history_load() 改成了新命令的末尾，
// 而屏幕上的光标还停在**旧位置**。用新 g_cur 去退格就会退多或退少，
// 接下来的擦除自然错位——这就是疯狂翻历史时屏幕越刷越乱的根因。
//
// 一句话：g_rendered_len / g_rendered_cur 记录「屏幕上现在长什么样」，
// g_len / g_cur 记录「逻辑上应该是什么样」。清理用前者，重画用后者。
int  g_rendered_cur = 0;

char g_history[shell::HISTORY_COUNT][shell::MAX_LINE];
int  g_history_count = 0;            // 已存了多少条（最多 HISTORY_COUNT）

u32  g_command_count = 0;            // 累计执行了多少条命令

// 当前提示符（放在 .data，将来支持改提示符时可以改它）
const char* g_prompt = "xingxingos> ";

// ---------------------------------------------------------------------------
//  输出小工具
// ---------------------------------------------------------------------------
void puts_colored(const char* s, u32 color)
{
    // 用 kprintf 而不是 term::puts：
    // term::puts 只写屏幕，kprintf 会同时写屏幕和串口。
    // 串口是内核最可靠的调试通道，shell 的输出两边都应该看得到。
    term::set_color(color, fb::color::BLACK);
    kprintf("%s", s);
    term::set_color(fb::color::WHITE, fb::color::BLACK);
}

// 打印一条「名称 : 值」对齐的信息
void print_kv(const char* key, const char* fmt, u64 value)
{
    term::set_color(fb::color::LIGHT_CYAN, fb::color::BLACK);
    kprintf("  %-16s", key);
    term::set_color(fb::color::WHITE, fb::color::BLACK);
    kprintf(fmt, value);
    kprintf("\n");
}

// ---------------------------------------------------------------------------
//  行编辑：重画整行
//  -------------------------------------------------------------------------
//  最简单的做法是「退格 N 次 -> 重打整行 -> 光标回退到原位」。
//  终端是纯字符网格、没有光标定位指令，这样反而最可靠。
// ---------------------------------------------------------------------------
void redraw_line()
{
    // --- 阶段一：把上次画的内容擦干净 ---
    // 注意两处都用 g_rendered_len（上次渲染的长度），不是 g_len（当前长度）。
    // term::putc('\b') 现在只移动光标、不擦除，所以擦除靠显式打空格。

    // 1) 光标从**上次渲染后的位置**退回到本行内容的开头
    for (int i = 0; i < g_rendered_cur; ++i) {
        term::putc('\b');
    }
    // 2) 用空格覆盖掉上次显示的全部内容
    for (int i = 0; i < g_rendered_len; ++i) {
        term::putc(' ');
    }
    // 3) 光标再退回来，回到内容开头（此时屏幕上是 g_rendered_len 个空格）
    for (int i = 0; i < g_rendered_len; ++i) {
        term::putc('\b');
    }

    // --- 阶段二：只重画内容，**不画提示符** ---
    //
    // 这里曾经有个很蠢的 bug：每次重画都 puts 一遍提示符。
    // 但清理阶段只清了「内容」（g_rendered_len 格），提示符一直留在屏幕上没被擦，
    // 于是每重画一次就多摞一个提示符——疯狂翻历史（按 90 次上下键）
    // 屏幕上就会挤满 90 个 "xingxingos>"。
    //
    // 正确做法：提示符只在 read_line() 开头画一次（Ctrl+C 换行后另画一次），
    // redraw_line() 只管内容区，两者职责分开。
    for (int i = 0; i < g_len; ++i) {
        term::putc(g_line[i]);
    }

    // --- 阶段三：光标回到 g_cur 处 ---
    for (int i = g_len; i > g_cur; --i) {
        term::putc('\b');
    }

    // 记住这次画成了什么样，下次清理时要用
    g_rendered_len = g_len;
    g_rendered_cur = g_cur;
}

// ---------------------------------------------------------------------------
//  可输入的最大长度
//  -------------------------------------------------------------------------
//  为什么要限制到「屏幕宽度 - 提示符长度」？
//    终端是字符网格，内容一旦超过一行就会自动折到下一行。
//    而 redraw_line() 是靠「退格 N 次回到行首」来清理的，
//    这个逻辑**只在单行内成立**——内容跨行后退格会跑到上一行去，
//    重画必然错位。保证永远不折行，重画逻辑就永远正确。
//
//    代价是单条命令最长约 116 字符，对当前需求绰绰有余。
// ---------------------------------------------------------------------------
int max_input_len()
{
    int avail = term::columns() - static_cast<int>(kstrlen(g_prompt)) - 1;
    if (avail < 16) avail = 16;                 // 兜底，防止终端异常窄
    if (avail > shell::MAX_LINE - 1) avail = shell::MAX_LINE - 1;
    return avail;
}

void line_insert(char c)
{
    if (g_len >= max_input_len()) {
        return;                     // 到上限了，忽略（避免折行导致重画错位）
    }
    // 从光标处整体后移一格（相当于标准库的 memmove）
    for (int i = g_len; i > g_cur; --i) {
        g_line[i] = g_line[i - 1];
    }
    g_line[g_cur] = c;
    ++g_cur;
    ++g_len;
    g_line[g_len] = '\0';
}

void line_backspace()
{
    if (g_cur == 0) {
        return;
    }
    for (int i = g_cur - 1; i < g_len - 1; ++i) {
        g_line[i] = g_line[i + 1];
    }
    --g_cur;
    --g_len;
    g_line[g_len] = '\0';
}

void line_delete()
{
    if (g_cur >= g_len) {
        return;
    }
    for (int i = g_cur; i < g_len - 1; ++i) {
        g_line[i] = g_line[i + 1];
    }
    --g_len;
    g_line[g_len] = '\0';
}

void history_add(const char* line)
{
    if (line[0] == '\0') {
        return;                     // 空命令不进历史
    }
    // 和最近一条相同就不重复记录（连按两次回车不会塞满历史）
    if (g_history_count > 0) {
        int last = (g_history_count - 1) % shell::HISTORY_COUNT;
        if (kstreq(g_history[last], line)) {
            return;
        }
    }

    int slot = g_history_count % shell::HISTORY_COUNT;
    for (int i = 0; i < shell::MAX_LINE - 1 && line[i] != '\0'; ++i) {
        g_history[slot][i] = line[i];
    }
    g_history[slot][shell::MAX_LINE - 1] = '\0';
    ++g_history_count;
}

// 从历史里取一条填进当前行
void history_load(int index)
{
    const char* src = g_history[index];
    g_len = 0;
    g_cur = 0;
    while (src[g_len] != '\0' && g_len < shell::MAX_LINE - 1) {
        g_line[g_len] = src[g_len];
        ++g_len;
    }
    g_line[g_len] = '\0';
    g_cur = g_len;
}

// ---------------------------------------------------------------------------
//  read_line：读一行输入
//  返回 false 表示用户按了 Ctrl+D（退出）
// ---------------------------------------------------------------------------
bool read_line()
{
    g_len = 0;
    g_cur = 0;
    g_line[0] = '\0';
    // 新起一行：屏幕上这一行还没有内容，屏幕状态归零
    g_rendered_len = 0;
    g_rendered_cur = 0;

    // 历史浏览位置：初始指向「新行」（即 history_count 的位置），
    // 往上翻就减一，往下翻加一
    int browse = g_history_count;

    puts_colored(g_prompt, fb::color::LIGHT_GREEN);

    for (;;) {
        u16 key = keyboard::wait_key();

        // --- 回车：提交这一行 ---
        if (key == '\n' || key == '\r') {
            term::putc('\n');
            return true;
        }

        // --- Ctrl+C：放弃当前行 ---
        if (key == keyboard::KEY_CTRL_C) {
            term::putc('\n');
            g_line[0] = '\0';
            g_len = 0;
            g_cur = 0;
            g_rendered_len = 0;     // 已经换行重画提示符，屏幕状态清零
            g_rendered_cur  = 0;
            puts_colored("^C\n", fb::color::YELLOW);
            puts_colored(g_prompt, fb::color::LIGHT_GREEN);
            browse = g_history_count;
            continue;
        }

        // --- Ctrl+D：结束 ---
        if (key == keyboard::KEY_CTRL_D && g_len == 0) {
            term::putc('\n');
            return false;
        }

        // --- Ctrl+L：清屏 ---
        // 注意：清屏后当前正在编辑的行**不能丢**，要连同提示符一起重画。
        // 用户按 Ctrl+L 是为了看清屏幕，不是想放弃输入。
        // 另外 g_rendered_len/cur 要重置——屏幕被清空了，
        // 之前记的「屏幕上有什么」已经不成立，否则下次重画会去擦不存在的东西。
        if (key == keyboard::KEY_CTRL_L) {
            term::clear();
            g_rendered_len = 0;
            g_rendered_cur  = 0;
            puts_colored(g_prompt, fb::color::LIGHT_GREEN);
            for (int i = 0; i < g_len; ++i) {
                term::putc(g_line[i]);
            }
            g_rendered_len = g_len;
            g_rendered_cur  = g_cur;
            // 光标回到 g_cur（清屏后光标在刚画完的内容末尾）
            for (int i = g_len; i > g_cur; --i) {
                term::putc('\b');
            }
            continue;
        }

        // --- Ctrl+U：清空整行 ---
        if (key == keyboard::KEY_CTRL_U) {
            g_len = 0;
            g_cur = 0;
            g_line[0] = '\0';
            redraw_line();
            continue;
        }

        // --- 退格 ---
        if (key == '\b') {
            line_backspace();
            redraw_line();
            continue;
        }

        // --- 方向键上：上一条历史 ---
        if (key == keyboard::ARROW_UP) {
            int oldest = (g_history_count > shell::HISTORY_COUNT)
                       ? g_history_count - shell::HISTORY_COUNT : 0;
            if (browse > oldest) {
                --browse;
                history_load(browse % shell::HISTORY_COUNT);
                redraw_line();
            }
            continue;
        }

        // --- 方向键下：下一条历史 ---
        if (key == keyboard::ARROW_DOWN) {
            if (browse < g_history_count) {
                ++browse;
                if (browse == g_history_count) {
                    // 翻过了最近一条 -> 回到空白新行
                    g_len = 0; g_cur = 0; g_line[0] = '\0';
                } else {
                    history_load(browse % shell::HISTORY_COUNT);
                }
                redraw_line();
            }
            continue;
        }

        // --- 左方向键 ---
        if (key == keyboard::ARROW_LEFT) {
            if (g_cur > 0) {
                --g_cur;
                term::putc('\b');
                g_rendered_cur = g_cur;      // 只移了光标，同步记录
            }
            continue;
        }

        // --- 右方向键 ---
        if (key == keyboard::ARROW_RIGHT) {
            if (g_cur < g_len) {
                term::putc(g_line[g_cur]);
                ++g_cur;
                g_rendered_cur = g_cur;      // 同步记录
            }
            continue;
        }

        // --- Home：跳到行首 ---
        if (key == keyboard::KEY_HOME) {
            while (g_cur > 0) {
                --g_cur;
                term::putc('\b');
            }
            g_rendered_cur = g_cur;          // 同步记录
            continue;
        }

        // --- End：跳到行尾 ---
        if (key == keyboard::KEY_END) {
            while (g_cur < g_len) {
                term::putc(g_line[g_cur]);
                ++g_cur;
            }
            g_rendered_cur = g_cur;          // 同步记录
            continue;
        }

        // --- Delete：删除光标处的字符 ---
        if (key == keyboard::KEY_DELETE) {
            line_delete();
            redraw_line();
            continue;
        }

        // --- Tab：输出几个空格（此版本不做补全）---
        if (key == keyboard::KEY_TAB_LIT) {
            for (int i = 0; i < 4; ++i) {
                line_insert(' ');
            }
            redraw_line();
            continue;
        }

        // --- 普通可打印字符 ---
        if (key >= 0x20 && key < 0x7F) {
            int old_len = g_len;
            line_insert(static_cast<char>(key));
            if (g_len == old_len) {
                continue;                       // 没插进去（到上限），忽略
            }
            // 光标在行尾时的快速路径：直接打这个字符就行，不用重画整行
            if (g_cur == g_len) {
                term::putc(g_line[g_cur - 1]);
                // 快速路径绕过了 redraw_line，这里要手动同步屏幕状态
                g_rendered_len = g_len;
                g_rendered_cur  = g_cur;
            } else {
                redraw_line();
            }
            continue;
        }

        // 其余键（F1~F12、PageUp 等）暂时忽略
    }
}

// ---------------------------------------------------------------------------
//  parse：把一行按空格切成 argv
//  -------------------------------------------------------------------------
//  就地修改 line：把每个参数的结尾换成 '\0'，argv[i] 直接指向 line 里的位置。
//  这样不需要额外分配内存——内核里能少一次分配就少一次。
// ---------------------------------------------------------------------------
int parse(char* line, const char** argv, int max_args)
{
    int argc = 0;
    int i = 0;

    while (line[i] != '\0' && argc < max_args) {
        // 跳过空格和制表符
        while (line[i] == ' ' || line[i] == '\t') {
            ++i;
        }
        if (line[i] == '\0') {
            break;
        }

        argv[argc] = &line[i];
        ++argc;

        // 找到这个参数的结尾
        while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (line[i] != '\0') {
            line[i] = '\0';
            ++i;
        }
    }
    return argc;
}

// ===========================================================================
//  命令实现
//  ===========================================================================

// --- help ---
void cmd_help(int argc, const char** argv);

// --- clear ---
void cmd_clear(int, const char**)
{
    term::clear();
}

// --- echo ---
void cmd_echo(int argc, const char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (i > 1) kprintf(" ");
        kprintf("%s", argv[i]);
    }
    kprintf("\n");
}

// --- meminfo / free ---
void cmd_meminfo(int, const char**)
{
    u64 total = pmm::total_memory();
    u64 free_b = pmm::free_memory();
    u64 used_b = pmm::used_memory();

    puts_colored("    物理内存\n", fb::color::LIGHT_CYAN);
    print_kv("总量", "%llu MB\n", total / (1024 * 1024));
    print_kv("已用", "%llu KB\n", used_b / 1024);
    print_kv("剩余", "%llu KB\n", free_b / 1024);
    if (total > 0) {
        // 乘 1000 得到千分比，再拆成整数和小数部分，
        // 这样 0.5% 不会被整数除法抹成 0%
        u64 permille = (used_b * 1000) / total;
        term::set_color(fb::color::LIGHT_CYAN, fb::color::BLACK);
        kprintf("  %-16s", "使用率");
        term::set_color(fb::color::WHITE, fb::color::BLACK);
        kprintf("%llu.%llu%%\n", permille / 10, permille % 10);
    }
    print_kv("页帧", "%llu 个（每页 4KB）\n", pmm::frame_count());

    kprintf("\n");
    puts_colored("    内核堆\n", fb::color::LIGHT_CYAN);
    print_kv("总大小", "%llu KB\n", heap::total_bytes() / 1024);
    print_kv("已用", "%llu 字节\n", heap::used_bytes());
    print_kv("空闲", "%llu 字节\n", heap::free_bytes());
    print_kv("块数", "%llu\n", static_cast<u64>(heap::block_count()));
}

// --- lsmem：打印 BIOS 给的内存映射 ---
void cmd_lsmem(int, const char**)
{
    const Mb2MemoryMapTag* mmap =
        reinterpret_cast<const Mb2MemoryMapTag*>(
            mb2::find_tag(g_mb2_info_phys, MB2_TAG_MMAP));

    if (mmap == nullptr) {
        puts_colored("  未找到内存映射信息\n", fb::color::YELLOW);
        return;
    }

    const u8* base = reinterpret_cast<const u8*>(mmap) + sizeof(Mb2MemoryMapTag);
    const u8* end  = reinterpret_cast<const u8*>(mmap) + mmap->size;

    puts_colored("  起始地址          结束地址          长度        类型\n",
                 fb::color::LIGHT_CYAN);
    puts_colored("  ----------------------------------------------------------\n",
                 fb::color::LIGHT_CYAN);

    for (const u8* p = base; p + mmap->entry_size <= end; p += mmap->entry_size) {
        const Mb2MemoryMapEntry* e =
            reinterpret_cast<const Mb2MemoryMapEntry*>(p);

        u64 stop = e->base_addr + e->length;
        // 用 %016llx 零填充到 16 位十六进制，列自然对齐。
        // 之前靠手工补空格，地址位数一变就乱了。
        kprintf("  %016llx  %016llx  %6llu MB  %llu - %s\n",
                e->base_addr, stop,
                e->length / (1024 * 1024),
                static_cast<u64>(e->type), memory_type_name(e->type));
    }
}

// --- uptime ---
void cmd_uptime(int, const char**)
{
    u64 ms = pit::uptime_ms();
    u64 sec = ms / 1000;
    u64 min = sec / 60;
    u64 hour = min / 60;

    kprintf("  已运行 ");
    if (hour > 0) kprintf("%llu 小时 ", hour);
    if (min > 0)  kprintf("%llu 分 ", min % 60);
    kprintf("%llu.%llu 秒\n", sec % 60, (ms / 100) % 10);

    print_kv("定时器中断", "%llu 次\n", pit::ticks());
    print_kv("定时器频率", "%llu Hz\n", static_cast<u64>(pit::frequency()));
    print_kv("键盘中断", "%llu 次\n", static_cast<u64>(keyboard::interrupt_count()));
    print_kv("已执行命令", "%llu 条\n", static_cast<u64>(g_command_count));
}

// --- uname ---
void cmd_uname(int argc, const char** argv)
{
    // -a 显示全部，否则只显示系统名
    bool all = (argc > 1 && kstreq(argv[1], "-a"));

    if (all) {
        kprintf("  xingxingOS 0.5.0 x86_64\n");
    } else {
        kprintf("  xingxingOS\n");
    }
}

// --- version ---
void cmd_version(int, const char**)
{
    // 打印「键 : 字符串值」的辅助（print_kv 只接受数字，这里单独处理）
    auto kv_str = [](const char* key, const char* value) {
        term::set_color(fb::color::LIGHT_CYAN, fb::color::BLACK);
        kprintf("  %-16s", key);
        term::set_color(fb::color::WHITE, fb::color::BLACK);
        kprintf("%s\n", value);
    };

    puts_colored("  xingxingOS\n", fb::color::LIGHT_CYAN);
    kv_str("版本", "0.5.0（阶段 5 : 命令行 shell）");
    kv_str("架构", "x86_64");
    kv_str("内核基址", "0xffffffff80000000");
    kv_str("引导器", "GRUB + Multiboot2");
    kv_str("启动方式", "BIOS / UEFI 双启动");
#if defined(__GNUC__)
    kv_str("编译器", "freestanding C++17");
#else
    kv_str("编译器", "未知");
#endif
    kv_str("标准库", "无（自行实现全部基础函数）");
}

// --- cpuinfo ---
void cmd_cpuinfo(int, const char**)
{
    // CPUID 叶 0：厂商字符串
    u32 eax, ebx, ecx, edx;
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0u));

    char vendor[13];
    vendor[0]  = static_cast<char>(ebx & 0xFF);
    vendor[1]  = static_cast<char>((ebx >> 8) & 0xFF);
    vendor[2]  = static_cast<char>((ebx >> 16) & 0xFF);
    vendor[3]  = static_cast<char>((ebx >> 24) & 0xFF);
    vendor[4]  = static_cast<char>(edx & 0xFF);
    vendor[5]  = static_cast<char>((edx >> 8) & 0xFF);
    vendor[6]  = static_cast<char>((edx >> 16) & 0xFF);
    vendor[7]  = static_cast<char>((edx >> 24) & 0xFF);
    vendor[8]  = static_cast<char>(ecx & 0xFF);
    vendor[9]  = static_cast<char>((ecx >> 8) & 0xFF);
    vendor[10] = static_cast<char>((ecx >> 16) & 0xFF);
    vendor[11] = static_cast<char>((ecx >> 24) & 0xFF);
    vendor[12] = '\0';

    term::set_color(fb::color::LIGHT_CYAN, fb::color::BLACK);
    kprintf("  %-16s", "厂商");
    term::set_color(fb::color::WHITE, fb::color::BLACK);
    kprintf("%s\n", vendor);

    print_kv("最大功能号", "0x%llx\n", static_cast<u64>(eax));

    // CPUID 叶 1：型号与特性
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(1u));

    u32 family   = (eax >> 8) & 0xF;
    u32 model    = (eax >> 4) & 0xF;
    u32 stepping = eax & 0xF;
    if (family == 0xF) {
        family += (eax >> 20) & 0xFF;
    }
    if (family == 6 || family == 0xF) {
        model |= (eax >> 12) & 0xF0;
    }

    print_kv("系列", "%llu\n", static_cast<u64>(family));
    print_kv("型号", "%llu\n", static_cast<u64>(model));
    print_kv("步进", "%llu\n", static_cast<u64>(stepping));

    kprintf("\n");
    puts_colored("  支持的特性\n", fb::color::LIGHT_CYAN);
    kprintf("    ");
    if (edx & (1u << 0))  kprintf("FPU ");
    if (edx & (1u << 4))  kprintf("TSC ");
    if (edx & (1u << 5))  kprintf("MSR ");
    if (edx & (1u << 6))  kprintf("PAE ");
    if (edx & (1u << 11)) kprintf("SYSENTER ");
    if (edx & (1u << 23)) kprintf("MMX ");
    if (edx & (1u << 25)) kprintf("SSE ");
    if (edx & (1u << 26)) kprintf("SSE2 ");
    if (ecx & (1u << 0))  kprintf("SSE3 ");
    if (ecx & (1u << 19)) kprintf("SSE4.1 ");
    if (ecx & (1u << 20)) kprintf("SSE4.2 ");
    if (ecx & (1u << 28)) kprintf("AVX ");
    if (ecx & (1u << 5))  kprintf("VMX ");
    kprintf("\n");
}

// --- heap ---
void cmd_heap(int argc, const char** argv)
{
    if (argc > 1 && kstreq(argv[1], "dump")) {
        heap::dump();
        return;
    }
    print_kv("总大小", "%llu 字节\n", heap::total_bytes());
    print_kv("已用", "%llu 字节\n", heap::used_bytes());
    print_kv("空闲", "%llu 字节\n", heap::free_bytes());
    print_kv("块数", "%llu\n", static_cast<u64>(heap::block_count()));

    if (heap::total_bytes() > 0) {
        print_kv("使用率", "%llu%%\n",
                 (heap::used_bytes() * 100) / heap::total_bytes());
    }
    kprintf("\n  参数：dump —— 打印每个内存块的布局\n");
}

// --- history ---
void cmd_history(int, const char**)
{
    int oldest = (g_history_count > shell::HISTORY_COUNT)
               ? g_history_count - shell::HISTORY_COUNT : 0;
    for (int i = oldest; i < g_history_count; ++i) {
        kprintf("  %4d  %s\n", i - oldest + 1,
                g_history[i % shell::HISTORY_COUNT]);
    }
}

// --- date ---
void cmd_date(int, const char**)
{
    // 没有 RTC（实时时钟）驱动，只能报「开机以来」的时间。
    // 真机上的 date 要读 CMOS，那是另一个驱动的事。
    u64 ms = pit::uptime_ms();
    u64 sec = ms / 1000;
    u64 min = sec / 60;
    u64 hour = min / 60;

    puts_colored("  本系统尚未实现实时时钟（RTC）驱动\n", fb::color::YELLOW);
    kprintf("\n");
    kprintf("  开机时刻之后的时长：");
    kprintf("%02llu:%02llu:%02llu\n", hour % 24, min % 60, sec % 60);
    print_kv("定时器滴答", "%llu\n", pit::ticks());
}

// --- reboot ---
void cmd_reboot(int, const char**)
{
    puts_colored("  正在重启...\n", fb::color::YELLOW);

    // 8042 键盘控制器的系统复位命令。
    // 写 0xFE 到 0x64（控制器命令口）会让 CPU 的 RESET 线翻转，
    // 这是 PC 上最传统的重启方式，比「三重错误」温和得多。
    // 先等一下，确保输入缓冲区不是满的（否则命令会被丢掉）。
    for (int i = 0; i < 100; ++i) {
        // 状态口 bit1 = 输入缓冲满
        if ((inb(0x64) & 0x02) == 0) break;
        io_wait();
    }
    outb(0x64, 0xFE);
    io_wait();

    // 万一没重启成功（有些虚拟机不支持），退化为停机
    kprintf("  重启命令无效，改为停机\n");
    asm volatile("cli");
    for (;;) asm volatile("hlt");
}

// --- halt / shutdown ---
void cmd_halt(int, const char**)
{
    puts_colored("  系统已停机。\n", fb::color::YELLOW);
    kprintf("  可以关闭虚拟机电源了。\n");
    asm volatile("cli");
    for (;;) asm volatile("hlt");
}

// --- panic（调试用）---
void cmd_panic(int, const char**)
{
    PANIC("用户通过 panic 命令主动触发");
}

// --- test：跑一遍内核自检 ---
void cmd_test(int, const char**)
{
    puts_colored("  运行中...\n", fb::color::LIGHT_CYAN);

    bool ok = true;

    // 堆自检：分配 - 写入 - 校验 - 释放
    const int N = 6;
    void* ptrs[N];
    const usize sizes[N] = { 16, 64, 256, 1024, 4096, 8192 };
    for (int i = 0; i < N; ++i) {
        ptrs[i] = heap::kmalloc(sizes[i]);
        if (ptrs[i] == nullptr) { ok = false; break; }
        u8* d = static_cast<u8*>(ptrs[i]);
        for (usize j = 0; j < sizes[i]; ++j) d[j] = static_cast<u8>(i + 1);
    }
    for (int i = 0; i < N && ok; ++i) {
        u8* d = static_cast<u8*>(ptrs[i]);
        for (usize j = 0; j < sizes[i]; ++j) {
            if (d[j] != static_cast<u8>(i + 1)) { ok = false; break; }
        }
    }
    for (int i = 0; i < N; ++i) if (ptrs[i]) heap::kfree(ptrs[i]);

    term::set_color(ok ? fb::color::LIGHT_GREEN : fb::color::LIGHT_RED,
                    fb::color::BLACK);
    kprintf(ok ? "  [通过] " : "  [失败] ");
    term::set_color(fb::color::WHITE, fb::color::BLACK);
    kprintf("堆分配与数据完整性\n");

    // 物理内存自检
    u64 before = pmm::used_frames();
    u64 f1 = pmm::alloc_frame();
    u64 f2 = pmm::alloc_frame();
    bool pm_ok = (f1 != 0 && f2 != 0 && f1 != f2);
    pmm::free_frame(f1);
    pmm::free_frame(f2);
    pm_ok = pm_ok && (pmm::used_frames() == before);

    term::set_color(pm_ok ? fb::color::LIGHT_GREEN : fb::color::LIGHT_RED,
                    fb::color::BLACK);
    kprintf(pm_ok ? "  [通过] " : "  [失败] ");
    term::set_color(fb::color::WHITE, fb::color::BLACK);
    kprintf("物理页帧分配与回收\n");

    // 虚拟内存自检
    u64 vtest = 0xfffffe9000000000ull;
    u64 ptest = pmm::alloc_frame();
    bool vm_ok = false;
    if (ptest != 0) {
        vm_ok = vmm::map_page(vtest, ptest, vmm::FLAGS_KERNEL);
        if (vm_ok) {
            vm_ok = (vmm::get_phys(vtest) == ptest);
            vmm::unmap_page(vtest);
        }
        pmm::free_frame(ptest);
    }

    term::set_color(vm_ok ? fb::color::LIGHT_GREEN : fb::color::LIGHT_RED,
                    fb::color::BLACK);
    kprintf(vm_ok ? "  [通过] " : "  [失败] ");
    term::set_color(fb::color::WHITE, fb::color::BLACK);
    kprintf("虚拟地址映射与翻译\n");

    kprintf("\n");
}

// --- clear 的别名 cls ---
// --- help ---
void cmd_help(int, const char**)
{
    shell::print_commands();
    kprintf("\n");
    puts_colored("  键盘快捷键\n", fb::color::LIGHT_CYAN);
    kprintf("    上/下方向键  浏览历史命令\n");
    kprintf("    左/右方向键  移动光标\n");
    kprintf("    Home / End   跳到行首 / 行尾\n");
    kprintf("    Backspace    删除光标前一个字符\n");
    kprintf("    Delete       删除光标处字符\n");
    kprintf("    Ctrl+C       放弃当前输入\n");
    kprintf("    Ctrl+L       清屏\n");
    kprintf("    Ctrl+U       清空整行\n");
    kprintf("    Ctrl+D       退出 shell（空行时）\n");
    kprintf("\n");
    kprintf("  提示：命令名输入错误时，会尝试给出最接近的建议。\n");
}


// --- ps：列出所有内核线程 ---
void print_thread_row(const thread::Thread* t, void*)
{
    // 列：TID  名称  状态  优先级  时间片  累计 CPU 时间
    kprintf("  %3d  %-14s %-8s %4llu  %8llu  %6llu ms\n",
            t->tid, t->name, thread::state_name(t->state),
            static_cast<u64>(t->priority), t->ticks, t->cpu_time_ms);
}

void cmd_ps(int, const char**)
{
    puts_colored("   TID  名称            状态    优先级     时间片    CPU 时间\n",
                 fb::color::LIGHT_CYAN);
    puts_colored("  -------------------------------------------------------------\n",
                 fb::color::LIGHT_CYAN);
    thread::for_each(print_thread_row, nullptr);
    kprintf("\n");
    print_kv("线程总数", "%llu\n", static_cast<u64>(thread::count()));
    print_kv("累计切换", "%llu 次\n", thread::total_switches());
}

// --- 演示用的工作线程（在 kmain 里创建）---
// ---------------------------------------------------------------------------
//  命令表
//  -------------------------------------------------------------------------
//  加一个新命令只需要：写个函数 + 在这儿加一行
// ---------------------------------------------------------------------------
const shell::Command g_commands[] = {
    // 基础
    { "help",     "help",              "显示本帮助信息",              cmd_help },
    { "clear",    "clear",             "清空屏幕",                    cmd_clear },
    { "cls",      "cls",               "清空屏幕（clear 的别名）",     cmd_clear },
    { "echo",     "echo [文本...]",     "把参数原样显示出来",          cmd_echo },
    { "history",  "history",           "显示历史命令",                cmd_history },
    { "uname",    "uname [-a]",        "显示系统名称",                cmd_uname },
    { "version",  "version",           "显示版本信息",                cmd_version },

    // 信息查询
    { "meminfo",  "meminfo",           "显示内存使用情况",            cmd_meminfo },
    { "free",     "free",              "显示内存使用情况（别名）",     cmd_meminfo },
    { "lsmem",    "lsmem",             "显示 BIOS 内存映射表",        cmd_lsmem },
    { "heap",     "heap [dump]",       "显示内核堆状态",              cmd_heap },
    { "cpuinfo",  "cpuinfo",           "显示 CPU 信息",               cmd_cpuinfo },
    { "uptime",   "uptime",            "显示运行时长",                cmd_uptime },
    { "ps",       "ps",                "显示所有内核线程",            cmd_ps },
    { "date",     "date",              "显示时间",                    cmd_date },

    // 系统控制
    { "reboot",   "reboot",            "重启计算机",                  cmd_reboot },
    { "halt",     "halt",              "停机",                        cmd_halt },
    { "shutdown", "shutdown",          "停机（halt 的别名）",          cmd_halt },

    // 调试
    { "panic",    "panic",             "触发一次内核崩溃（调试用）",   cmd_panic },
    { "test",     "test",              "运行内核自检",                cmd_test },
};

constexpr int COMMAND_COUNT =
    static_cast<int>(sizeof(g_commands) / sizeof(g_commands[0]));

// ---------------------------------------------------------------------------
//  简单的最相似命令提示（编辑距离太长就放弃）
// ---------------------------------------------------------------------------
int edit_distance(const char* a, const char* b)
{
    // 只做「替换 + 增删」的简易距离，够提示用就行。
    // 用完整的 DP 会多占 256x256 的表，对内核启动阶段不划算。
    int la = kstrlen(a), lb = kstrlen(b);
    int diff = (la > lb) ? (la - lb) : (lb - la);
    if (diff > 3) return 99;

    int dist = diff;
    int n = (la < lb) ? la : lb;
    for (int i = 0; i < n; ++i) {
        if (a[i] != b[i]) ++dist;
        if (dist > 3) return 99;
    }
    return dist;
}

}  // namespace

namespace shell {

void print_commands()
{
    puts_colored("  可用命令\n", fb::color::LIGHT_CYAN);
    puts_colored("  --------------------------------------------------------------\n",
                 fb::color::LIGHT_CYAN);
    for (int i = 0; i < COMMAND_COUNT; ++i) {
        term::set_color(fb::color::YELLOW, fb::color::BLACK);
        kprintf("  %-10s", g_commands[i].name);
        term::set_color(fb::color::WHITE, fb::color::BLACK);
        kprintf("%s\n", g_commands[i].desc);
    }
}

void execute(const char* line)
{
    // 拷贝一份，因为 parse 会就地写入 '\0'
    char buf[MAX_LINE];
    int i = 0;
    for (; i < MAX_LINE - 1 && line[i] != '\0'; ++i) {
        buf[i] = line[i];
    }
    buf[i] = '\0';

    // 去掉首尾空白
    char* start = buf;
    while (*start == ' ' || *start == '\t') ++start;
    if (*start == '\0') {
        return;                         // 空行
    }

    const char* argv[16];
    int argc = parse(start, argv, 16);
    if (argc == 0) {
        return;
    }

    // 在命令表里查找
    for (int i = 0; i < COMMAND_COUNT; ++i) {
        if (kstreq(argv[0], g_commands[i].name)) {
            g_commands[i].fn(argc, argv);
            ++g_command_count;
            return;
        }
    }

    // 没找到：给出最接近的建议（比干巴巴一句"未知命令"友好得多）
    term::set_color(fb::color::LIGHT_RED, fb::color::BLACK);
    kprintf("  未知命令：%s\n", argv[0]);
    term::set_color(fb::color::WHITE, fb::color::BLACK);

    int best = -1;
    int best_dist = 99;
    for (int i = 0; i < COMMAND_COUNT; ++i) {
        int d = edit_distance(argv[0], g_commands[i].name);
        if (d < best_dist) {
            best_dist = d;
            best = i;
        }
    }
    if (best >= 0 && best_dist <= 3) {
        kprintf("  你是想输入 ");
        term::set_color(fb::color::LIGHT_GREEN, fb::color::BLACK);
        kprintf("%s", g_commands[best].name);
        term::set_color(fb::color::WHITE, fb::color::BLACK);
        kprintf(" 吗？\n");
        kprintf("  用法：%s\n", g_commands[best].usage);
    } else {
        kprintf("  输入 help 查看所有命令\n");
    }
}

u32 command_count()
{
    return g_command_count;
}

[[noreturn]] void run()
{
    kprintf("\n");
    puts_colored("  输入 help 查看可用命令，reboot 重启，halt 停机。\n",
                 fb::color::LIGHT_CYAN);
    kprintf("\n");

    for (;;) {
        if (!read_line()) {
            // Ctrl+D 退出到「停机」状态（没有别的程序可返回）
            kprintf("\n已退出 shell。\n");
            asm volatile("cli");
            for (;;) asm volatile("hlt");
        }

        // 非空行才进历史
        if (g_line[0] != '\0') {
            history_add(g_line);
        }
        execute(g_line);
    }
}

}  // namespace shell
