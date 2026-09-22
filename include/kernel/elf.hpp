// ===========================================================================
//  include/kernel/elf.hpp —— ELF64 加载器
//  ==========================================================================
//  这是「万物皆可程序」的技术地基。
//
//  有了它，系统才能**从外部加载并运行一个独立的 .xzs 程序**，
//  而不是像现在这样把服务代码编译进内核镜像。
//
//  当前支持：
//    - ELF64 小端，静态链接（非 PIE），链接地址在用户空间
//    - 只处理 PT_LOAD 段（程序真正需要被装进内存的部分）
//
//  暂不支持（将来需要时再加）：
//    - 动态链接（需要 ld.so 与重定位，工作量大）
//    - PIE（需要加载基址重定位）
// ===========================================================================
#pragma once

#include <kernel/types.h>

namespace elf {

// ELF64 文件头
struct [[gnu::packed]] Header {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};

// ELF64 程序头（描述一个要被加载的段）
struct [[gnu::packed]] Phdr {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};

constexpr u32 PT_LOAD = 1;      // 可加载段

constexpr u32 PF_X = 1;         // 可执行
constexpr u32 PF_W = 2;         // 可写
constexpr u32 PF_R = 4;         // 可读

// 校验 ELF 头部是否合法（魔数 / 位数 / 架构 / 类型）
// 失败时通过 out_reason 返回原因字符串，便于排查
bool validate(const void* image, u64 len, const char** out_reason = nullptr);

// ---------------------------------------------------------------------------
//  load：把 ELF 映像载入指定页表
//  -------------------------------------------------------------------------
//    image      ELF 文件在**内核虚拟地址**中的位置
//    len        文件长度
//    pml4       目标进程的页表（物理地址）
//    out_entry  返回程序入口地址
//    out_end    返回程序映像的最高虚拟地址（将来做堆/break 用）
//
//  返回 false 表示加载失败，调用方应清理已分配的页。
// ---------------------------------------------------------------------------
bool load(const void* image, u64 len, u64 pml4, u64* out_entry, u64* out_end);

}   // namespace elf
