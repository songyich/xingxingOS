// ===========================================================================
//  include/kernel/svcdir.hpp —— 服务目录（/system/services）
//  ---------------------------------------------------------------------------
//  「万物皆可程序」下的服务发现机制。
//
//  对应逻辑路径 /system/services/<name>：
//    服务启动时把自己的 tid 登记到某个名字下，
//    别人要用它就按名字查 tid。
//
//  ⚠️ 现在还没有文件系统，所以先由内核维护一张表。
//     等 P3 有了 FS，底层换成真实目录文件，接口不变，上层无感。
//
//  这样设计的好处：解决了"服务创建顺序一变 TID 就全乱"的坑
//  （我们踩过：硬编码 TID_TERMINAL=2，结果顺序变了全崩）。
// ===========================================================================
#pragma once
#include <kernel/types.h>

namespace svcdir {

constexpr int MAX_SERVICES = 32;
constexpr int NAME_MAX     = 32;

void init();

// 把 tid 登记为 name。成功返回 0
int register_service(const char* name, int len, int tid);

// 按名字查 tid。没找到返回 -1
int lookup(const char* name, int len);

// 已注册的服务数量
int count();

// 取第 index 项的名字与 tid
const char* name_at(int index, int* out_tid);

}   // namespace svcdir
