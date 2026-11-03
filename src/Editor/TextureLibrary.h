#pragma once

#include <string>
#include <vector>

namespace editor
{

// E6：材质贴图选择用的贴图库。
//
// 为什么不复用 Assets 面板的扫描结果：模型与贴图是两类资产，扫描根、
// 扩展名过滤、显示名（相对 Texture/ 的路径）都不一样；混在一起会让
// "双击模型 = 加进场景" 与 "选贴图" 两种操作抢同一个列表。
struct TextureEntry
{
    std::string absolutePath;
    std::string displayName;   // 相对 Resources/Texture/ 的路径
};

class TextureLibrary
{
public:
    // 重新扫描 Resources/Texture（递归）。扫描失败不清空已有结果。
    void refresh();

    const std::vector<TextureEntry>& entries() const { return items; }
    const std::string& scannedRoot() const { return rootPath; }

    // 只认 LDR 图片格式：Texture 的加载路径走 stb_image，HDR 需要另一个构造函数重载。
    static bool isTextureExtension(const std::string& lowerExtension);
    static bool selfTest();

private:
    std::vector<TextureEntry> items;
    std::string rootPath;
};

} // namespace editor
