#pragma once

// T-06：图级黑板上的具名载荷。
//
// 每个结构体都是"一帧内从一侧交给另一侧"的东西，原先散落在 FrameInfo 上。
// 命名规则：名词短语，读方一眼知道自己拿的是什么。

#include <cstdint>
#include <functional>

#include <entt/entt.hpp>

namespace tiny::payload
{

// F6-02：自动曝光算出的本帧曝光覆盖值。0 = 不覆盖，回落 SceneSettings::exposure。
struct ExposureOverride
{
float value = 0.0f;
};

// 编辑器当前选中的实体（entt::null = 没选）。渲染侧用它画选中包围盒。
struct SelectionHighlight
{
entt::entity entity = entt::null;
};

// M1-C1c：模拟插值权重与刚结束的 tick 编号。
// alpha 取 [0,1)；tick 用来判断某个实体本 tick 是否真的动过。
struct RenderInterpolation
{
float alpha = 0.0f;
uint64_t tick = 0;
};

// W1：配置存取与编辑器接管 UI 的回调。
// ImGuiSystem 没有 RenderGraph，靠 FrameInfo 里的黑板句柄取到它们。
struct ConfigCallbacks
{
std::function<void()> save;
std::function<void()> load;
std::function<void()> buildEditorUI;
std::function<bool()> isEditorMode;
};

} // namespace tiny::payload