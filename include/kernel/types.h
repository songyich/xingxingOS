// ============================================================================
//  include/kernel/types.h —— 内核自己的整数类型
//  ---------------------------------------------------------------------------
//  为什么不能用 <cstdint>？
//    那些头文件来自宿主系统的 libstdc++ / glibc，它们背后假设「有一个操作系统
//    在给你提供系统调用、堆、线程局部存储」。内核里这些全不存在，混入宿主头文件
//    轻则链接报错，重则生成调用 libc 的代码，一跑就崩。所以内核必须自带类型定义。
// ============================================================================
#pragma once

using u8  = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using i8  = signed char;
using i16 = signed short;
using i32 = signed int;
using i64 = signed long long;

// __SIZE_TYPE__ 是 GCC/Clang 的内置宏，就等于目标平台的 size_t。
// 用它而不是手写 unsigned long，是为了让 operator new(size_t) 这类
// 「签名必须精确匹配」的函数不出 -fpermissive 报错。
using usize = __SIZE_TYPE__;
using uptr  = unsigned long long;
using phys_addr_t = unsigned long long;  // 物理地址
using virt_addr_t = unsigned long long;  // 虚拟地址

using bool_ = bool;                 // 保持 C++ 原生 bool 即可，这里只是占位

// 禁止拷贝：内核里很多对象（驱动、锁）天生不该被复制
#define MYOS_NONCOPYABLE(ClassName)         \
    ClassName(const ClassName&) = delete;   \
    ClassName& operator=(const ClassName&) = delete
