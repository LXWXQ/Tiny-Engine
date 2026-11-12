#pragma once

#include "Editor/EditorPanel.h"
#include "Core/FrameInfo.h"
#include "Scene/ComponentTypes.h"
#include "Scene/ComponentRegistry.h"

#include <glm/glm.hpp>

#include <string>

class Scene;

namespace editor
{

// 组件目录 / 创建向导。
//
// 面板名仍然是「Render」（EditorLayer 的布局与快捷键都按这个名字找），但职责
// 已经从「渲染开关」升级为「按特效类型浏览并创建全部组件」：
//   - ComponentScope::Scene  = 单一性组件：每个场景一份，没有 Transform；
//   - ComponentScope::Entity = 对象组件：创建时新建一个带 Transform 的对象。
// 具体参数控件复用 Editor/ComponentWidgets.h，和 Inspector 是同一套行为。
class RenderSettingsPanel : public EditorPanel
{
public:
    const char* name() const override { return "Render"; }
    void draw(EditorState& state) override;

    // SSAO 核大小必须是 4..64 的偶数：场景 SSAO 组件共用这条规则，
    // 这里保留纯函数回归，避免夹取逻辑被改坏。
    static int clampKernelSize(int value);

    static bool selfTest();

private:
    void drawCatalog(EditorState& state, Scene& scene);
    void drawWizard(EditorState& state, Scene& scene);
    void openWizard(EditorState& state, Scene& scene,
                    const scene::ComponentDescriptor& descriptor);
    void drawAdvanced(EngineSettings& settings);

    // ---- 目录筛选 ----
    char search_[128] = {};

    // ---- 创建向导的临时状态 ----
    // 只保存类型 id，不保存 descriptor 指针：组件注册表理论上可以在运行期
    // 增删条目，向导跨帧打开时指针可能失效。
    std::string pendingType_;
    bool wizardRequested_ = false;

    scene::ComponentRecord draft_;
    scene::ComponentRecord defaults_;

    char nameBuffer_[128] = {};
    glm::vec3 spawnPosition_{ 0.0f };
    glm::vec3 spawnEuler_{ 0.0f };   // 度（UI），创建时转弧度
    glm::vec3 spawnScale_{ 1.0f };
};

} // namespace editor
