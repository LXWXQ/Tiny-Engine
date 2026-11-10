#pragma once

// ===========================================================================
// ComponentRegistry：UE 风格的组件元数据注册表
//
// 每个可挂载组件注册一次：
//   - 稳定 id（存盘用，不能随 C++ 类型名改）
//   - 显示名 / 分类（Inspector 的 + Add Component 菜单）
//   - 暴露给 UI / 序列化的属性（name + type + getter/setter）
//   - 类型擦除的 addDefault / has / remove / read / write
//
// 属性 getter/setter 用 std::function 而不是 offsetof：组件可以有私有成员、
// 非 POD 字段和自定义 setter（例如 TransformComponent 的 dirty 通知），
// 注册表只要求“给我一个 T*，我读写这些字段”。
// ===========================================================================

#include "Scene/ComponentTypes.h"

#include <entt/entt.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace scene
{

struct PropertyDescriptor
{
    std::string name;
    std::string label;
    PropertyType type = PropertyType::Float;

    // ---- Inspector 元数据（不参与序列化） ----
    // 有范围时 DragFloat/DragInt 会夹取，并在 tooltip 里显示区间。
    bool hasRange = false;
    float minimum = 0.0f;
    float maximum = 0.0f;
    // <= 0 时按属性类型选默认拖动速度。
    float dragSpeed = 0.0f;
    std::string tooltip;

    // void* 指向组件实例；由 makeProperty<T>() 做类型擦除。
    std::function<PropertyValue(const void*)> get;
    std::function<void(void*, const PropertyValue&)> set;
};

// 组件挂在谁身上：
//   Entity = 普通实体（Transform / Character / Spin…）
//   Scene  = 场景本身（阴影 / Bloom / 雾 / 水面…），由 Scene 的隐藏设置实体持有。
// UI 用同一个 ComponentRegistry 列出两套组件，但 Add 菜单按 scope 过滤，
// 这样“场景功能目录”和“对象组件”不会互相污染。
enum class ComponentScope
{
    Entity,
    Scene,
};

// 注册选项。原生组件要同时满足两件事：既出现在统一的 + Add Component
// 菜单里，又不能重复写进 v3 场景的通用 components 数组。
struct ComponentOptions
{
    ComponentScope scope = ComponentScope::Entity;
    bool exposed = true;
    bool addable = true;
    bool removable = true;
    bool drawProperties = true;
    bool serialized = true;
    bool required = false;
    std::string unavailableReason;
};

struct ComponentDescriptor
{
    std::string type;
    std::string label;
    std::string category;
    // 场景级组件只能挂在 Scene 的隐藏设置实体上，普通实体不允许添加。
    ComponentScope scope = ComponentScope::Entity;
    // false 表示不在 + Add Component 菜单里出现（仍可显示已挂载实例）。
    bool exposed = true;
    // false 时菜单里仍显示该项，但置灰并提示 unavailableReason。
    bool addable = true;
    bool removable = true;
    // false 表示由固定 Inspector 面板绘制，通用列表不再重复画一遍。
    bool drawProperties = true;
    // 是否进入 v3 EntityDescription.components。原生组件走专用字段，必须为 false。
    bool serialized = true;
    // Name / Transform / Hierarchy 这类场景必需组件：不可加、不可删。
    bool required = false;
    std::string unavailableReason;

    std::vector<PropertyDescriptor> properties;

    std::function<void(entt::registry&, entt::entity)> addDefault;
    std::function<bool(const entt::registry&, entt::entity)> has;
    std::function<void(entt::registry&, entt::entity)> remove;
    std::function<ComponentRecord(const entt::registry&, entt::entity)> read;
    std::function<void(entt::registry&, entt::entity, const ComponentRecord&)> write;
    std::function<ComponentRecord()> defaults;
    // 额外的移除条件（例如地形的 RenderTag 不允许被摘掉）。可空。
    std::function<bool(const entt::registry&, entt::entity)> canRemove;
};

class ComponentRegistry
{
public:
    // 拒绝空 type、重复 type 或缺少回调的 descriptor。
    bool add(ComponentDescriptor descriptor);

    const ComponentDescriptor* find(const std::string& type) const;

    const std::vector<ComponentDescriptor>& descriptors() const { return registered; }
    bool empty() const { return registered.empty(); }
    void clear() { registered.clear(); }

private:
    std::vector<ComponentDescriptor> registered;
};

// 生成一个属性描述。T 是组件类型。
template <typename T>
PropertyDescriptor makeProperty(
    std::string name,
    std::string label,
    PropertyType type,
    std::function<PropertyValue(const T&)> getter,
    std::function<void(T&, const PropertyValue&)> setter)
{
    PropertyDescriptor property;
    property.name = std::move(name);
    property.label = std::move(label);
    property.type = type;
    property.get = [getter = std::move(getter)](const void* component) -> PropertyValue
    {
        return getter(*static_cast<const T*>(component));
    };
    property.set = [setter = std::move(setter)](void* component,
                                                const PropertyValue& value)
    {
        setter(*static_cast<T*>(component), value);
    };
    return property;
}

// 给属性补 UI 元数据。按值返回，方便在注册表初始化里串起来写。
inline PropertyDescriptor withRange(PropertyDescriptor property, float minimum, float maximum,
                                    float dragSpeed = 0.0f)
{
    property.hasRange = true;
    property.minimum = minimum;
    property.maximum = maximum;
    property.dragSpeed = dragSpeed;
    return property;
}

inline PropertyDescriptor withTooltip(PropertyDescriptor property, std::string tooltip)
{
    property.tooltip = std::move(tooltip);
    return property;
}

namespace detail
{

template <typename T>
ComponentRecord readRecord(const std::string& type,
                           const std::vector<PropertyDescriptor>& properties,
                           const T& component)
{
    ComponentRecord record;
    record.type = type;
    record.properties.reserve(properties.size());
    for (const PropertyDescriptor& property : properties)
    {
        NamedProperty named;
        named.name = property.name;
        named.label = property.label;
        named.value = property.get(&component);
        record.properties.push_back(std::move(named));
    }
    return record;
}

} // namespace detail

// 生成一个组件的类型擦除描述。要求 T 可默认构造。
template <typename T>
ComponentDescriptor makeComponentDescriptor(std::string type,
                                             std::string label,
                                             std::string category,
                                             std::vector<PropertyDescriptor> properties,
                                             ComponentOptions options = {})
{
    ComponentDescriptor descriptor;
    descriptor.type = std::move(type);
    descriptor.label = std::move(label);
    descriptor.category = std::move(category);
    descriptor.scope = options.scope;
    descriptor.exposed = options.exposed;
    descriptor.addable = options.addable;
    descriptor.removable = options.removable;
    descriptor.drawProperties = options.drawProperties;
    descriptor.serialized = options.serialized;
    descriptor.required = options.required;
    descriptor.unavailableReason = std::move(options.unavailableReason);
    descriptor.properties = std::move(properties);

    const std::string recordType = descriptor.type;
    const std::vector<PropertyDescriptor> recordProperties = descriptor.properties;

    descriptor.addDefault = [](entt::registry& registry, entt::entity entity)
    {
        if (!registry.all_of<T>(entity))
            registry.emplace<T>(entity);
    };
    descriptor.has = [](const entt::registry& registry, entt::entity entity)
    {
        return registry.try_get<T>(entity) != nullptr;
    };
    descriptor.remove = [](entt::registry& registry, entt::entity entity)
    {
        if (registry.all_of<T>(entity))
            registry.remove<T>(entity);
    };
    descriptor.read = [recordType, recordProperties](const entt::registry& registry,
                                                     entt::entity entity)
    {
        const T* component = registry.try_get<T>(entity);
        if (component == nullptr)
            return ComponentRecord{};
        return detail::readRecord(recordType, recordProperties, *component);
    };
    descriptor.write = [recordProperties](entt::registry& registry, entt::entity entity,
                                          const ComponentRecord& record)
    {
        T* component = registry.try_get<T>(entity);
        if (component == nullptr)
            component = &registry.emplace<T>(entity);
        for (const PropertyDescriptor& property : recordProperties)
        {
            if (const NamedProperty* value = record.find(property.name))
                property.set(component, value->value);
        }
    };
    descriptor.defaults = [recordType, recordProperties]()
    {
        return detail::readRecord(recordType, recordProperties, T{});
    };
    descriptor.canRemove = [](const entt::registry&, entt::entity)
    {
        return true;
    };

    return descriptor;
}

} // namespace scene
