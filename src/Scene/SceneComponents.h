#pragma once

// ===========================================================================
// 场景级组件（Scene Components）
//
// 阴影、Bloom、SSAO、雾、水面、体积云这些“整个场景一份”的渲染功能，过去硬编码在
// EngineSettings 与 RenderSettingsPanel 里：用户只能看到一堆固定滑杆，
// 不能添加/移除，也无法在场景文件里保存“这套场景启用了哪些功能”。
//
// 这里把它们注册成普通的元数据组件，但 scope = Scene。Scene 持有一个不带
// Transform 的隐藏实体专门挂它们，因此：
//   - Inspector 的 + Add Component 菜单可以直接添加；
//   - 属性编辑、复制/粘贴、重置、撤销全部复用现有组件工作流；
//   - SceneSerializer 把它们写进场景文档，打开场景就恢复同一套功能。
//
// EngineSettings 仍是渲染器每帧读取的缓存。组件变化后 Scene 调
// syncSceneSettings() 把值写回去；组件缺失 = 功能关闭。
// ===========================================================================

#include "Core/FrameInfo.h"
#include "Scene/ComponentRegistry.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace scene
{

// 环境：曝光、动态分辨率、方向光。它不控制某个 pass，而是所有 pass 的公共输入。
struct SceneEnvironmentComponent
{
    bool enabled = true;
    float exposure = 1.0f;
    float renderScale = 1.0f;
    glm::vec3 lightDirection{ -1.0f, -1.0f, 1.0f };
    glm::vec4 lightColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float lightIntensity = 1.0f;
};

// 三级联阴影。
struct SceneShadowsComponent
{
    bool enabled = true;
    float distance = 90.0f;
    float strength = 0.65f;
};

struct SceneBloomComponent
{
    bool enabled = true;
    float threshold = 2.2f;
    float knee = 0.5f;
    float intensity = 0.12f;
};

struct SceneSsaoComponent
{
    bool enabled = true;
    float radius = 0.5f;
    float bias = 0.025f;
    int kernelSize = 16;
};

struct SceneSsrComponent
{
    bool enabled = true;
    float maxDistance = 200.0f;
};

struct SceneMotionBlurComponent
{
    bool enabled = true;
    float strength = 1.0f;
    float maxRadius = 0.035f;
};

struct SceneTaaComponent
{
    bool enabled = true;
    float blend = 0.9f;
    float jitterScale = 1.0f;
};

struct SceneWaterComponent
{
    bool enabled = true;
    float level = 2.0f;
    float waveStrength = 1.0f;
    float waveSpeed = 1.0f;
    bool planarReflection = true;
};

struct SceneFogComponent
{
    bool enabled = true;
    float start = 80.0f;
    float density = 0.00045f;
};

// F6-05：体积云。参数对应 CloudPass 的全屏 raymarch：
// coverage 控制云量，density 控制消光，baseHeight/thickness 定义云层高度带，
// speed 是风移速度，absorption/ambient 控制自阴影与天光，steps 是步进预算。
struct SceneCloudsComponent
{
    bool enabled = true;
    float coverage = 0.45f;
    float density = 0.9f;
    float baseHeight = 120.0f;
    float thickness = 90.0f;
    float speed = 4.0f;
    float absorption = 0.9f;
    float ambient = 0.35f;
    int steps = 48;
    glm::vec4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

// F6-06：太阳盘与镜头光晕。方向/光色不在这里，跟随 scene.environment
// 的方向光；本组件只控制太阳自身的视觉强度和光晕风格。
struct SceneSunComponent
{
    bool enabled = true;
    float intensity = 12.0f;
    float angularRadius = 0.8f;
    float glow = 0.35f;
    float flareIntensity = 0.8f;
    int flareGhosts = 7;
    bool occlusion = true;
    glm::vec4 color{ 1.0f, 0.96f, 0.88f, 1.0f };
};

// 注册 11 个 Scene scope 组件。Scene 构造时调用一次。
void registerSceneComponents(ComponentRegistry& registry);

// 把当前挂载的场景组件写回渲染器缓存的 EngineSettings。
// 先重置成默认值，因此“组件被移除”会等价于“关闭该功能”，不会残留上一帧。
void syncSceneSettings(entt::registry& registry, entt::entity settingsEntity,
                       EngineSettings& settings);

// 从 engine.json 载入的 EngineSettings 初始化整套组件。
// 旧场景文件没有 sceneComponents 字段时也走这里：画面保持和旧版本一致。
void resetSceneComponentsFromSettings(entt::registry& registry, entt::entity settingsEntity,
                                      const EngineSettings& settings);

// 移除全部场景组件（v4 文件中 sceneComponents 为空数组时使用）。
void clearSceneComponents(entt::registry& registry, entt::entity settingsEntity);

} // namespace scene
