#pragma once

// ===========================================================================
// 编辑器与引擎之间的唯一接缝（Editor <-> Engine 边界）
//
// 设计原则（重要，加新功能前先读）：
//
//   1. **编辑器不拥有引擎**。EditorContext 只是一把"引用集合"，
//      由 Application 填好之后交给 EditorLayer。编辑器销毁不会带走任何 GPU 资源。
//
//   2. **编辑器不直接调 vkCmd***。要画东西必须走渲染图的 Pass 声明。
//      编辑器目前只读场景、改组件、调引擎已有的窗口/配置接口。
//
//   3. **这是唯一允许编辑器依赖 RHI/Core 的地方**。各面板（panels/）只依赖
//      EditorContext 和一个 entt::registry，不允许 include "RHI/Device.h"。
//      这样以后把编辑器拆成独立可执行文件时，只需要替换 EditorContext 的实现。
//
//   4. **回调而不是指针**：引擎能做什么，用 std::function 暴露出来，
//      而不是把 FirstApp* 递给编辑器。这样编辑器无法绕过引擎的内部约束
//      （例如"销毁资源必须走延迟回收队列"）。
// ===========================================================================

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <imgui.h>          // ImTextureID（离屏视口的句柄类型）
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

class Scene;
class DescriptorManager;
class Device;
struct EngineSettings;

namespace editor
{

// 编辑器的三种运行状态。
//
//   Editing —— 场景是"数据"，相机是编辑器自由相机，不推进游戏逻辑
//   Playing —— 场景是"运行时"，相机归游戏控制，逻辑每帧推进
//   Paused  —— 从 Playing 暂停，保留运行时状态
//
// 为什么需要这个状态机而不是一个 bool：
// "停止"要把场景恢复到播放前的快照，"暂停"不能恢复。两者语义不同，
// 用一个 bool 表达会导致"暂停后再停止"把场景恢复成错误的版本。
enum class PlayState
{
    Editing,
    Playing,
    Paused
};

// 一条日志。编辑器控制台面板用它显示引擎输出。
//
// 为什么不用 std::cout 直接写控制台：ImGui 面板需要"有边界的、可滚动、可过滤"的
// 结构化记录，而 stdout 是无界的线性流。两者用途不同，必须分开。
struct LogEntry
{
    enum class Level { Info, Warning, Error };
    Level level = Level::Info;
    std::string text;
};

// 选中状态。当前是单选，但接口按"集合"设计 —— 将来加多选不用改调用方。
struct Selection
{
    std::vector<entt::entity> entities;

    bool empty() const { return entities.empty(); }
    entt::entity primary() const { return entities.empty() ? entt::null : entities.front(); }

    void clear() { entities.clear(); }

    void select(entt::entity entity)
    {
        entities.clear();
        if (entity != entt::null)
            entities.push_back(entity);
    }

    bool contains(entt::entity entity) const
    {
        for (entt::entity e : entities)
            if (e == entity) return true;
        return false;
    }

    // E1：多选。层级面板的 Ctrl 点击、视口 Ctrl 点击都走这里。
    void add(entt::entity entity)
    {
        if (entity != entt::null && !contains(entity))
            entities.push_back(entity);
    }

    void remove(entt::entity entity)
    {
        entities.erase(std::remove(entities.begin(), entities.end(), entity), entities.end());
    }

    void toggle(entt::entity entity)
    {
        if (contains(entity)) remove(entity);
        else add(entity);
    }

    size_t size() const { return entities.size(); }
};

// 引擎侧能力的抽象。所有字段都在 Application 构造时填好，编辑器只管调用。
struct EngineServices
{
    // ---- 只读数据 ----
    Scene* scene = nullptr;
    entt::registry* registry = nullptr;
    EngineSettings* settings = nullptr;

    // ---- 编辑器需要但不该自己实现的动作 ----
    // 保存 / 重新加载 config/engine.json（沿用 W1 的 EngineConfig，不另开一套）
    std::function<void()> saveConfig;
    std::function<void()> reloadConfig;

    // 请求引擎重建渲染目标（例如面板尺寸变了、renderScale 改了）。
    // 编辑器不直接碰 swapchain / G-Buffer，只表达意图。
    std::function<void()> requestRenderTargetResize;

    // 把引擎日志推给编辑器控制台。
    std::function<void(const std::string&)> pushLog;

    // 打开一个原生文件对话框（返回空串表示取消）。
    // 由引擎侧实现，因为要用 Win32 API，编辑器层不应直接依赖 windows.h。
    std::function<std::string(const char* filter, const char* title)> openFileDialog;
    std::function<std::string(const char* filter, const char* title)> saveFileDialog;

    // ---- TD-09：离屏视口 ----
    // 编辑器"报"它想要的视口像素尺寸；引擎在下一帧开头统一重建离屏纹理。
    // 为什么不让编辑器直接建纹理：编辑器层不持有设备，而且重建需要
    // vkDeviceWaitIdle —— 那是引擎的职责。
    std::function<void(uint32_t width, uint32_t height)> requestViewportSize;

    // 取视口纹理的 ImGui 句柄。按 frame slot 区分：
    // 两帧在飞时第 N+1 帧写视口会覆盖 GPU 仍在采样的第 N 帧内容。
    std::function<ImTextureID(uint32_t frameSlot)> getViewportTextureId;

    // 窗口标题。编辑器要把"文档名 + 未保存标记"显示出来，但它不拥有 GLFW 窗口，
    // 所以只能请引擎代劳。
    std::function<void(const std::string& title)> setWindowTitle;

    // 请求退出。未保存提示点了"保存并继续"或"放弃修改"之后才会调到这里。
    std::function<void()> requestExit;
};

} // namespace editor
