#pragma once

#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// TM-16：资产路径协议与"换目录不重新 configure"
//
// 现状（截至引入本文件）：
//   `cmake/AssetPaths.h.in` 在配置阶段把 shaders / Resources / config 的**绝对路径**
//   烧进可执行文件（TINY_SHADER_DIR / TINY_RESOURCE_DIR / TINY_CONFIG_PATH）。
//   好处是不依赖工作目录；坏处是仓库一换盘、一改目录名，所有路径全部失效，
//   而且报错发生在 Pipeline::readFile / stbi_load 里，离真正的原因很远。
//
// 本文件做两件事：
//   1. 启动时按**可执行文件位置**回溯出 shader / 资源 / 配置三个根目录，
//      取第一个真实存在的候选（`exe/shaders`、`exe/../shaders`……）。
//   2. 提供 `resolve()`：把"构建期烧死但现在不存在"的绝对路径，按锚点
//      （/shaders/、/Resources/、/config/）重新拼到运行时根目录上。
//
// 为什么不是"直接用协议路径替换所有调用点"：
//   调用点有上百处（`TINY_SHADER_DIR "bloom.frag.spv"` 这类字符串拼接），
//   一次性全改风险大且没有额外收益。现在的语义是"构建期路径优先，失效时回退"，
//   两条路都留着，回退只在真正找不到文件时发生 —— 默认路径的行为一字不变。
//
// 支持的显式协议（给新代码用）：
//   asset://Resources/...   资源根
//   shader://ssao.frag.spv  shader 根
//   config://engine.json    配置根
//   file://C:/abs/path      原样透传
// ---------------------------------------------------------------------------
namespace tiny::assetpath
{
    // 幂等；Application 构造的第一件事就调它（早于任何 Pipeline/Texture 读取）
    void initialize();

    // 三个运行时根目录（initialize 之后有效；初始化前返回空串）
    const std::string& shaderRoot();
    const std::string& resourceRoot();
    const std::string& configRoot();
    const std::string& executableDir();

    // 把可能是"构建期绝对路径"或"协议路径"的输入解析成真实存在的路径。
    //   - 原路径存在        -> 原样返回（默认行为完全不变）
    //   - 协议路径         -> 拼到对应根目录
    //   - 构建期路径失效    -> 按锚点重拼到运行时根目录
    //   - 都不成立         -> 返回原串（让调用方照常报"打不开 xxx"的错误，错误信息不丢）
    std::string resolve(const std::string& path);

    bool fileExists(const std::string& path);

    // 打印根目录与解析结果（--print-assets）
    void print(std::FILE* out);

    // 纯逻辑自检：协议前缀、锚点重拼、空串与斜杠归一化
    bool selfTest();
}