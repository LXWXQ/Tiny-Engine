#pragma once

#include "Editor/EditorPanel.h"

namespace editor
{

// 3D 视口：把引擎渲染出的离屏纹理当成一张 ImGui 图片画出来。
//
// 为什么要有这个面板（而不是继续让 3D 画面铺满整窗）：
//   1. 面板能真正"围绕"视口，而不是浮在 3D 画面上遮住内容；
//   2. 截图只包含视口内容，不会把 UI 一起拍进去；
//   3. 相机的纵横比可以按视口尺寸算，画面不会被横向拉伸。
//
// 这个面板不持有任何 GPU 资源：纹理由引擎创建，这里只拿一个 ImGui 句柄。
class ViewportPanel : public EditorPanel
{
public:
    const char* name() const override { return "Viewport"; }
    void draw(EditorState& state) override;
};

} // namespace editor
