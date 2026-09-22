// ============================================================================
//  include/kernel/panic.hpp —— 内核恐慌（蓝屏）
//  ---------------------------------------------------------------------------
//  什么是 panic？
//    内核遇到「继续跑下去只会造成更大破坏」的情况（比如页表坏了、内存分配器
//    状态不一致、断言失败），就没有「弹个错误框让用户点确定」这条退路了——
//    内核是最后一层，只能把所有状态打印出来然后停机，等待人来看。
//
//  为什么叫「不可恢复」？
//    用户程序崩了，内核可以杀掉它、回收资源、继续跑别的程序。
//    内核自己崩了就没人来收尸了，所以 panic = 停机，不做恢复尝试。
//
//  用法：
//    PANIC("页表项意外为空");
//    PANIC_FMT 目前还没实现变参版本，需要带数字时用 kprintf 打完再 PANIC。
// ============================================================================
#pragma once

#include <kernel/types.h>

// 触发一次内核恐慌，永不返回
// 参数 message = 错误说明，file/line = 出错位置（用下面的宏自动填）
[[noreturn]] void panic(const char* message, const char* file, int line);

// 便捷宏：自动补上文件名和行号
#define PANIC(msg) panic((msg), __FILE__, __LINE__)

// 断言宏：条件不成立就 panic。NDEBUG 时不参与编译（与标准 assert 行为一致）
#ifndef NDEBUG
#define KASSERT(cond, msg)                       \
    do {                                         \
        if (!(cond)) {                           \
            PANIC((msg));                        \
        }                                        \
    } while (0)
#else
#define KASSERT(cond, msg) ((void)0)
#endif
