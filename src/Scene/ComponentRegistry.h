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
#include <vector>

namespace scene
{

struct PropertyDescriptor
{
    std::string name;
    std::string label;
    PropertyType type = PropertyType::Float;

    // void* 指向组件实例；由 makeProperty<T>() 做类型擦除。
    std::function<PropertyValue(const void*)> get;
    std::function<void(void*, const PropertyValue&)> set;
};

struct ComponentDescriptor
{
    std::string type;
    std::string label;
    std::string category;
    // false 表示不在 + Add Component 菜单里出现（仍可显示已挂载实例）。
    bool exposed = true;

    std::vector<PropertyDescriptor> properties;

    std::function<void(entt::registry&, entt::entity)> addDefault;
    std::function<bool(const entt::registry&, entt::entity)> has;
    std::function<void(entt::registry&, entt::entity)> remove;
    std::function<ComponentRecord(const entt::registry&, entt::entity)> read;
    std::function<void(entt::registry&, entt::entity, const ComponentRecord&)> write;
    std::function<ComponentRecord()> defaults;
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
                                             bool exposed = true)
{
    ComponentDescriptor descriptor;
    descriptor.type = std::move(type);
    descriptor.label = std::move(label);
    descriptor.category = std::move(category);
    descriptor.exposed = exposed;
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

    return descriptor;
}

} // namespace scene
