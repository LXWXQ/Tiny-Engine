#pragma once

#include "Editor/EditorPanel.h"
#include "Core/FrameInfo.h"

namespace editor
{

// 渲染设置面板（E5-b）：把 EngineSettings 里那些"改一下就能立刻看出差别"的
// 开关与滑杆摆到编辑器里。
//
// 之前这些字段只能改 config/engine.json 或命令行参数 —— 而 engine.json 要
// 重启才生效，调曝光/辉光/SSAO 时这条路径基本不可用（眼睛看到的和值是断开的）。
// 这里直接改 Scene 持有的 EngineSettings 实例，下一帧就生效。
//
// 注意：这些值属于"引擎默认预设"，不是场景文档的内容 ——
// 所以既不进撤销栈，也不让 documentDirty 变脏。要留到下次启动，按
// "Save to engine.json"（否则退出后回到磁盘上的值）。
class RenderSettingsPanel : public EditorPanel
{
public:
    const char* name() const override { return "Render"; }
    void draw(EditorState& state) override;

    // SSAO 核大小必须是 4..64 的偶数：slider 拖动时允许任意整数，
    // 真正写回设置前夹一手，避免 0 或 4096 直接打崩采样循环。
    static int clampKernelSize(int value);

    static bool selfTest();

private:
    void drawLight(EngineSettings& settings);
    void drawAdvanced(EngineSettings& settings);

    // 打开面板那一刻的会话基线：用户试了一堆值想回到"刚才的样子"，
    // 这时候该回的是本次启动时的快照，不是代码里的编译期默认值。
    EngineSettings baseline;
    bool baselineCaptured = false;
};

} // namespace editor
