#pragma once

#include <cstdio>
#include <vector>

namespace tiny::cli
{

// 命令行选项表（单一事实来源）。
//
// main.cpp 的 --help / --list-options 与编辑器的「帮助 -> 命令行选项」面板
// 都从这张表渲染：以前它只存在于 main.cpp 的静态数组里，编辑器看不到，
// 加完参数容易忘记同步文档。新增参数时只改这里一处。
struct UsageLine
{
    const char* group;
    const char* flags;
    const char* text;
};

const std::vector<UsageLine>& catalog();

// 按分组打印整张表（含 exe 名与结尾说明），供 --help / --list-options 使用。
void printCatalog(std::FILE* out, const char* exeName);

// 纯逻辑自检：分组非空、flag 不重复、说明非空、总数没有意外缩水。
bool selfTest();

} // namespace tiny::cli
