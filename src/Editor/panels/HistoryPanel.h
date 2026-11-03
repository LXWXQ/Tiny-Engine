#pragma once

#include "Editor/EditorPanel.h"

namespace editor
{

// 撤销历史（E4-d）：把 CommandStack 里的每一层摊开给用户看。
//
// 只读展示 + 点击跳转：面板自己绝不直接改数据，跳转统一走 CommandStack::jumpTo()，
// 这样 replaying 标记、合并窗口、GPU 副作用都还是同一条路径。
class HistoryPanel : public EditorPanel
{
public:
    const char* name() const override { return "History"; }
    void draw(EditorState& state) override;
};

} // namespace editor
