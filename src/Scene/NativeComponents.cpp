#include "Scene/NativeComponents.h"

#include "Scene/ComponentRegistry.h"
#include "Scene/Components.h"

#include <algorithm>
#include <string>

namespace scene
{
namespace
{

PropertyDescriptor makeFloatProperty(std::string name, std::string label, float minimum,
                                     float maximum,
                                     std::function<float(const CharacterControllerComponent&)> getter,
                                     std::function<void(CharacterControllerComponent&, float)> setter,
                                     const char* tooltip = nullptr, float dragSpeed = 0.0f)
{
    PropertyDescriptor property = makeProperty<CharacterControllerComponent>(
        std::move(name), std::move(label), PropertyType::Float,
        [getter = std::move(getter)](const CharacterControllerComponent& component)
        {
            return PropertyValue::fromFloat(getter(component));
        },
        [setter = std::move(setter), minimum, maximum](CharacterControllerComponent& component,
                                                       const PropertyValue& value)
        {
            setter(component, std::clamp(value.asFloat(), minimum, maximum));
        });
    property.hasRange = true;
    property.minimum = minimum;
    property.maximum = maximum;
    property.dragSpeed = dragSpeed;
    if (tooltip != nullptr)
        property.tooltip = tooltip;
    return property;
}

PropertyDescriptor makeBoolProperty(std::string name, std::string label,
                                    std::function<bool(const RenderTagComponent&)> getter,
                                    std::function<void(RenderTagComponent&, bool)> setter,
                                    const char* tooltip = nullptr)
{
    PropertyDescriptor property = makeProperty<RenderTagComponent>(
        std::move(name), std::move(label), PropertyType::Bool,
        [getter = std::move(getter)](const RenderTagComponent& component)
        {
            return PropertyValue::fromBool(getter(component));
        },
        [setter = std::move(setter)](RenderTagComponent& component, const PropertyValue& value)
        {
            setter(component, value.asBool());
        });
    if (tooltip != nullptr)
        property.tooltip = tooltip;
    return property;
}

// 场景必需组件：不出现在菜单里，也不能被移除。属性面板由固定面板负责。
ComponentDescriptor makeRequiredDescriptor(std::string type, std::string label)
{
    ComponentDescriptor descriptor;
    descriptor.type = std::move(type);
    descriptor.label = std::move(label);
    descriptor.category = "核心";
    descriptor.exposed = false;
    descriptor.addable = false;
    descriptor.removable = false;
    descriptor.drawProperties = false;
    descriptor.serialized = false;
    descriptor.required = true;
    descriptor.addDefault = [](entt::registry&, entt::entity) {};
    descriptor.has = [](const entt::registry&, entt::entity) { return true; };
    descriptor.remove = [](entt::registry&, entt::entity) {};
    descriptor.read = [](const entt::registry&, entt::entity) { return ComponentRecord{}; };
    descriptor.write = [](entt::registry&, entt::entity, const ComponentRecord&) {};
    descriptor.defaults = []() { return ComponentRecord{}; };
    descriptor.canRemove = [](const entt::registry&, entt::entity) { return false; };
    return descriptor;
}

ComponentDescriptor makeCameraDescriptor()
{
    ComponentOptions options;
    options.addable = false;
    options.removable = false;
    options.drawProperties = false;
    options.serialized = false;
    options.unavailableReason = "Camera is unique; it is created with the scene.";

    ComponentDescriptor descriptor;
    descriptor.type = "native.camera";
    descriptor.label = "Camera";
    descriptor.category = "托管";
    descriptor.exposed = options.exposed;
    descriptor.addable = options.addable;
    descriptor.removable = options.removable;
    descriptor.drawProperties = options.drawProperties;
    descriptor.serialized = options.serialized;
    descriptor.unavailableReason = options.unavailableReason;
    // CameraComponent 是空 tag：EnTT 不存对象，只能查存在性。
    descriptor.addDefault = [](entt::registry&, entt::entity) {};
    descriptor.has = [](const entt::registry& registry, entt::entity entity)
    {
        return registry.any_of<CameraComponent>(entity);
    };
    descriptor.remove = [](entt::registry&, entt::entity) {};
    descriptor.read = [](const entt::registry&, entt::entity) { return ComponentRecord{}; };
    descriptor.write = [](entt::registry&, entt::entity, const ComponentRecord&) {};
    descriptor.defaults = []() { return ComponentRecord{}; };
    descriptor.canRemove = [](const entt::registry&, entt::entity) { return false; };
    return descriptor;
}

template <typename T>
ComponentDescriptor makeManagedDescriptor(std::string type, std::string label,
                                          std::string reason)
{
    ComponentOptions options;
    options.addable = false;
    options.removable = false;
    options.drawProperties = false;
    options.serialized = false;
    options.unavailableReason = std::move(reason);

    ComponentDescriptor descriptor = makeComponentDescriptor<T>(
        std::move(type), std::move(label), "托管", {}, options);
    // 托管组件的生命周期由创建流程负责，通用删除入口不应碰它。
    descriptor.canRemove = [](const entt::registry&, entt::entity) { return false; };
    return descriptor;
}

} // namespace

void registerNativeComponents(ComponentRegistry& registry)
{
    registry.add(makeRequiredDescriptor("native.name", "Name"));
    registry.add(makeRequiredDescriptor("native.transform", "Transform"));
    registry.add(makeRequiredDescriptor("native.hierarchy", "Hierarchy"));

    ComponentOptions characterOptions;
    characterOptions.drawProperties = false;
    characterOptions.serialized = false;

    std::vector<PropertyDescriptor> characterProperties;
    characterProperties.push_back(makeFloatProperty(
        "radius", "Radius", 0.05f, 5.0f,
        [](const CharacterControllerComponent& c) { return c.radius; },
        [](CharacterControllerComponent& c, float v) { c.radius = v; },
        "角色胶囊半径，单位米", 0.005f));
    characterProperties.push_back(makeFloatProperty(
        "height", "Height", 0.2f, 10.0f,
        [](const CharacterControllerComponent& c) { return c.height; },
        [](CharacterControllerComponent& c, float v) { c.height = v; },
        "角色总高度，从脚底到头顶", 0.01f));
    characterProperties.push_back(makeFloatProperty(
        "moveSpeed", "Move Speed", 0.0f, 50.0f,
        [](const CharacterControllerComponent& c) { return c.moveSpeed; },
        [](CharacterControllerComponent& c, float v) { c.moveSpeed = v; },
        "地面最大移动速度，单位/秒", 0.05f));
    characterProperties.push_back(makeFloatProperty(
        "jumpSpeed", "Jump Speed", 0.0f, 50.0f,
        [](const CharacterControllerComponent& c) { return c.jumpSpeed; },
        [](CharacterControllerComponent& c, float v) { c.jumpSpeed = v; },
        "起跳初速度，单位/秒", 0.05f));
    characterProperties.push_back(makeFloatProperty(
        "gravity", "Gravity", 0.0f, 100.0f,
        [](const CharacterControllerComponent& c) { return c.gravity; },
        [](CharacterControllerComponent& c, float v) { c.gravity = v; },
        "重力加速度；越大下落越快", 0.05f));
    characterProperties.push_back(makeFloatProperty(
        "maxSlopeAngle", "Max Slope", 0.0f, 89.0f,
        [](const CharacterControllerComponent& c) { return c.maxSlopeAngle; },
        [](CharacterControllerComponent& c, float v) { c.maxSlopeAngle = v; },
        "可站立的最大坡度，单位度", 0.5f));
    characterProperties.push_back(makeFloatProperty(
        "stepHeight", "Step Height", 0.0f, 3.0f,
        [](const CharacterControllerComponent& c) { return c.stepHeight; },
        [](CharacterControllerComponent& c, float v) { c.stepHeight = v; },
        "可自动跨上的台阶高度，单位米", 0.01f));
    characterProperties.push_back(makeFloatProperty(
        "groundSnapDistance", "Ground Snap", 0.0f, 5.0f,
        [](const CharacterControllerComponent& c) { return c.groundSnapDistance; },
        [](CharacterControllerComponent& c, float v) { c.groundSnapDistance = v; },
        "贴地检测距离；防止下坡时反复离地", 0.01f));

    registry.add(makeComponentDescriptor<CharacterControllerComponent>(
        "native.characterController", "Character Controller", "角色",
        std::move(characterProperties), characterOptions));

    ComponentOptions renderTagOptions;
    renderTagOptions.drawProperties = false;
    renderTagOptions.serialized = false;

    std::vector<PropertyDescriptor> renderTagProperties;
    renderTagProperties.push_back(makeBoolProperty(
        "skybox", "Skybox",
        [](const RenderTagComponent& c) { return c.isSkybox; },
        [](RenderTagComponent& c, bool v) { c.isSkybox = v; },
        "把该实体当作天空盒渲染"));
    renderTagProperties.push_back(makeBoolProperty(
        "grid", "Grid",
        [](const RenderTagComponent& c) { return c.isGrid; },
        [](RenderTagComponent& c, bool v) { c.isGrid = v; },
        "把该实体当作世界网格渲染（编辑辅助）"));

    ComponentDescriptor renderTag = makeComponentDescriptor<RenderTagComponent>(
        "native.renderTag", "Render Tag", "渲染", std::move(renderTagProperties),
        renderTagOptions);
    renderTag.canRemove = [](const entt::registry& registry, entt::entity entity)
    {
        const auto* tag = registry.try_get<RenderTagComponent>(entity);
        return tag == nullptr || !tag->isTerrain;
    };
    registry.add(std::move(renderTag));

    registry.add(makeCameraDescriptor());
    registry.add(makeManagedDescriptor<TerrainComponent>(
        "native.terrain", "Terrain",
        "Create a terrain from the Create menu instead."));
    registry.add(makeManagedDescriptor<TerrainStreamingComponent>(
        "native.terrainStreaming", "Streamed Terrain",
        "Create streamed terrain from the Create menu instead."));
    registry.add(makeManagedDescriptor<VoxelWorldComponent>(
        "native.voxelWorld", "Voxel World",
        "Create a voxel world from the Create menu instead."));
}

} // namespace scene
