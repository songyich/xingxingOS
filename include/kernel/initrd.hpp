// ===========================================================================
//  include/kernel/initrd.hpp —— 初始内存盘（存放 .xzs 程序）
//  ---------------------------------------------------------------------------
//  现在还没有文件系统，程序先打包进内核镜像里的 initrd。
//  等阶段 P3 有了真正的 FS，程序就放到 /bin 下，这里换成从磁盘读。
// ===========================================================================
#pragma once
#include <kernel/types.h>

namespace initrd {

// 解析内核镜像里的 initrd（由链接脚本嵌入）
void init();

bool ready();

// 程序总个数
int count();

// 按名字查找程序，返回内容指针与长度。找不到返回 false。
// 名字形如 "help" 或 "help.xzs"，两种都接受。
bool find(const char* name, const void** out_data, u64* out_size);

// 取第 index 个程序的名字（供 ls /bin 这类命令用）
const char* name_of(int index);

}   // namespace initrd
