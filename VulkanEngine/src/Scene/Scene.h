#pragma once

#include "Scene/Components.h"
#include "Scene/SceneDescription.h"
#include "Scene/Camera.h"
#include "Scene/CameraController.h"
#include "Scene/SpatialIndex.h"
#include "Core/MaterialSystem.h"
#include "Core/DescriptorManager.h"
#include "Core/FrameInfo.h"
#include "RHI/Device.h"
#include "RHI/Texture.h"

#include <GLFW/glfw3.h>
#include <entt/entt.hpp>
#include <memory>
#include <vector>

class Scene
{
public:
    // DescriptorManager is required because glTF materials must reserve
    // bindless material slots while the scene is being loaded.
    // loadDefaultContent=false 时只创建相机实体，不加载 Sponza / 天空盒。
    void init(Device& device, VkCommandPool commandPool, DescriptorManager& descriptorManager,
              bool loadDefaultContent = true);
    // allowCameraInput=false 时相机不接受键鼠输入（编辑器在鼠标位于面板上、
    // 正在输入文字、或正在拖 gizmo 时用）。仍然会把内部状态写回 transform。
    void update(float dt, GLFWwindow* window, float aspectRatio, bool allowCameraInput = true);
    void cleanup();

    entt::entity createEntity();
    void destroyEntity(entt::entity entity);

    // Reparenting keeps HierarchyComponent::children consistent and rejects
    // cycles. Passing entt::null makes the entity a root.
    void setParent(entt::entity child, entt::entity parent);

    entt::registry& getRegistry() { return registry; }
    const entt::registry& getRegistry() const { return registry; }
    const std::vector<entt::entity>& getVisibleEntities() const { return visibleEntities; }

    entt::entity getCameraEntity() const { return cameraEntity; }
    TransformComponent& getCameraTransform() { return registry.get<TransformComponent>(cameraEntity); }
    const TransformComponent& getCameraTransform() const { return registry.get<TransformComponent>(cameraEntity); }

    VulkanCamera& getCamera() { return camera; }
    CameraController& getCameraController() { return cameraController; }
    EngineSettings& getSettings() { return engineSettings; }
    RenderTelemetry& getTelemetry() { return currentTelemetry; }
    const RenderTelemetry& getDisplayTelemetry() const { return displayTelemetry; }

    // Called once at the start of a render frame, before the UI is built.
    // The previous frame's counts are complete and safe to display while the
    // current frame is still being recorded.
    void beginFrameTelemetry()
    {
        displayTelemetry = currentTelemetry;
        currentTelemetry.reset();
    }

    MaterialSystem& getMaterialSystem() { return materialSystem; }

    // ---- 场景描述（编辑器 / 序列化用）----
    // captureDescription: 把当前 registry 导出成纯数据（不含任何 GPU 句柄）。
    // buildFromDescription: 清空当前场景并按描述重建（会重新加载网格、重新分配材质槽位）。
    // 两者都不抛异常，失败时通过 errorMessage 返回原因。
    scene::SceneDescription captureDescription() const;
    bool buildFromDescription(const scene::SceneDescription& description, std::string& errorMessage);

    // 清空场景（保留相机实体）。编辑器的"新建场景"走这里。
    void clearEntities();

    // 编辑器需要主动刷新世界变换 / 可见性（它改完组件后不能等下一帧）。
    void refreshTransformsAndVisibility();

    // D-7 regression test: hierarchy transform propagation + uniform-grid query.
    static bool selfTest();

private:
    // 编辑器加载场景时需要重新解析网格 / 材质，这两个指针由 init() 存下来。
    Device* device = nullptr;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    DescriptorManager* descriptorManager = nullptr;

public:
    // 供编辑器按路径实例化一个网格实体（会自动分配材质槽位并设置包围盒）。
    // 返回 entt::null 表示失败。
    entt::entity createMeshEntity(const std::string& absolutePath, const std::string& name);

private:
    void updateWorldTransforms();
    void rebuildSpatialIndex();
    void updateVisibility(const glm::mat4& viewProjection);

    entt::registry registry;
    entt::entity cameraEntity = entt::null;
    SpatialIndex spatialIndex{ 16.0f };
    std::vector<entt::entity> visibleEntities;

    VulkanCamera camera{};
    CameraController cameraController{};

    EngineSettings engineSettings;
    RenderTelemetry currentTelemetry;
    RenderTelemetry displayTelemetry;
    MaterialSystem materialSystem;
};