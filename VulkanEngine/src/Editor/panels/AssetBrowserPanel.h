#pragma once

#include "Editor/EditorPanel.h"

#include <string>
#include <vector>

namespace editor
{

// 资产浏览器：列出 Resources/Models 下可实例化的网格文件。
//
// 定位：这是"往场景里加东西"的入口。真正的资产管线（导入、依赖、缩略图）
// 是独立的大工程，这里只做"发现文件 + 创建实体"这条最短路径。
//
// 为什么不用资源数据库：当前项目规模下，扫描目录只要几毫秒，
// 而资源数据库需要一套导入/变更检测/序列化格式 —— 收益不成比例。
class AssetBrowserPanel : public EditorPanel
{
public:
    const char* name() const override { return "Assets"; }
    void draw(EditorState& state) override;
    void onAttach(EditorState& state) override;

private:
    void refresh();

    struct AssetEntry
    {
        std::string absolutePath;   // 传给 createMeshEntity
        std::string displayName;    // 相对 Models/ 的路径，带子目录便于区分同名文件
        std::string extension;
    };

    std::vector<AssetEntry> models;
    bool scanned = false;
    char filter[64] = { 0 };
};

} // namespace editor
