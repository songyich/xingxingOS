// ============================================================================
//  kernel/cxx_support.cpp —— C++ 运行期支撑
//  ---------------------------------------------------------------------------
//  为什么内核必须自己写这些？
//    平时写 C++ 时，「new 一个对象」「全局对象的构造函数被自动调用」「纯虚函数
//    被误调用时报错退出」这些看起来是天经地义的，其实全靠 C++ 运行期（libstdc++
//    和 crtbegin/crtend）在背后兜底。内核是 freestanding 的，没有这层兜底，
//    所以这些桩（stub）必须自己实现，否则：
//      - 没写 operator new        -> 链接报 undefined reference to `operator new'
//      - 没写 __cxa_pure_virtual  -> 误用纯虚函数时跳到一个不存在的地址
//      - 没调用 .init_array       -> 全局对象的构造函数永远不执行（最阴险的一种）
//
//  .init_array 是什么？
//    编译器把「需要在 main 之前执行的全局对象构造函数指针」收集到一个叫
//    .init_array 的段里。宿主程序由 glibc 的 __libc_start_main 负责遍历调用，
//    内核没人帮我们做，必须在 kmain() 开头自己走一遍。
// ============================================================================

#include <kernel/types.h>
#include <kernel/panic.hpp>
#include <kernel/heap.hpp>

// ---------------------------------------------------------------------------
//  早期堆：一个极简的「撞针式（bump）分配器」
//  ---------------------------------------------------------------------------
//  阶段 4 才会实现真正的 kmalloc/kfree。在那之前，为了让 operator new 可用，
//  先用一块静态数组顶着：分配就是往后挪指针，不支持释放（释放直接忽略）。
//  够用的原因是：早期内核几乎不动态分配，new 主要用于验证这条链路是通的。
// ---------------------------------------------------------------------------
namespace {

constexpr usize EARLY_HEAP_SIZE = 64 * 1024;   // 64KB

// alignas(16)：保证任何对象放进来都不会破坏对齐要求
alignas(16) u8 g_early_heap[EARLY_HEAP_SIZE];
usize g_early_heap_used = 0;

}  // namespace

// ---------------------------------------------------------------------------
//  operator new / delete
//  注意：这些函数是 C++ 的「可替换全局分配函数」，必须带 extern "C++" 语义
//  （默认就是），且不能声明为 static。
//
//  两阶段策略：
//    - 堆（heap::kmalloc）还没初始化时，用上面的早期 bump 分配器顶着
//    - 堆就绪之后，全部转给 kmalloc，享受真正的分配与回收
//  这样 C++ 的 new/delete 和 C 风格的 kmalloc/kfree 共用同一套内存，
//  不会出现「两套分配器各管一块、互不知情」的割裂局面。
// ---------------------------------------------------------------------------
void* operator new(usize size)
{
    if (heap::ready()) {
        return heap::kmalloc(size);
    }

    // 早期堆：只增不减的撞针式分配
    usize aligned = (g_early_heap_used + 15) & ~static_cast<usize>(15);
    if (aligned + size > EARLY_HEAP_SIZE) {
        // 早期堆耗尽：这属于「无法恢复」的情况，直接 panic 蓝屏
        PANIC("early heap exhausted (operator new)");
    }
    g_early_heap_used = aligned + size;
    return &g_early_heap[aligned];
}

void* operator new[](usize size)
{
    return operator new(size);
}

// noexcept 必须写：delete 的声明就是 noexcept，签名不匹配会导致链接错误
void operator delete(void* ptr) noexcept
{
    // 早期堆分配的内存不回收（撞针式分配器没有回收能力）。
    // 堆就绪后交给 kfree 正常回收。
    if (heap::ready()) {
        heap::kfree(ptr);
    }
}

void operator delete[](void* ptr) noexcept
{
    if (heap::ready()) {
        heap::kfree(ptr);
    }
}

// C++14 起的「带大小」版本：编译器知道对象大小时会优先调用它，能省一次查表
void operator delete(void* ptr, usize size) noexcept
{
    (void)size;
    if (heap::ready()) {
        heap::kfree(ptr);
    }
}

void operator delete[](void* ptr, usize size) noexcept
{
    (void)size;
    if (heap::ready()) {
        heap::kfree(ptr);
    }
}

// ---------------------------------------------------------------------------
//  __cxa_pure_virtual
//  当代码调用了一个纯虚函数（比如构造函数里调用虚函数、或者对象已被析构），
//  编译器会生成对它的调用。没有实现的话，出错时 CPU 会跳到非法地址。
// ---------------------------------------------------------------------------
extern "C" void __cxa_pure_virtual()
{
    PANIC("pure virtual function called");
}

// ---------------------------------------------------------------------------
//  __cxa_atexit / __cxa_finalize
//  全局对象析构函数的登记接口。宿主程序在 exit 时回调，内核永不 exit，
//  所以登记直接丢弃、返回 0 表示成功即可。
// ---------------------------------------------------------------------------
extern "C" int __cxa_atexit(void (*destructor)(void*), void* arg, void* dso)
{
    (void)destructor;
    (void)arg;
    (void)dso;
    return 0;                 // 0 = 成功
}

extern "C" void __cxa_finalize(void* dso)
{
    (void)dso;
}

// ---------------------------------------------------------------------------
//  __dso_handle
//  「动态共享对象句柄」，给 __cxa_atexit 区分不同的模块用。
//  内核只有一个模块，给个全局变量放着就行，但它必须真的存在（会被取地址）。
// ---------------------------------------------------------------------------
void* __dso_handle = nullptr;

// ---------------------------------------------------------------------------
//  call_global_constructors —— 遍历 .init_array，调用所有全局对象构造函数
//  符号 __init_array_start / __init_array_end 由链接脚本定义
// ---------------------------------------------------------------------------
using constructor_fn = void (*)();

extern constructor_fn __init_array_start[];
extern constructor_fn __init_array_end[];

extern "C" void call_global_constructors()
{
    for (constructor_fn* fn = __init_array_start; fn < __init_array_end; ++fn) {
        if (*fn != nullptr) {
            (*fn)();
        }
    }
}
