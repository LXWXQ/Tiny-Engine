#pragma once

// ===========================================================================
// 通用组件值模型
//
// 引擎原先的组件都是 C++ 结构体，Inspector / 序列化 / 命令各自硬编码一份字段
// 列表。要像 UE 那样“任意特性都能挂到任意实体上”，需要先把组件描述成
// **运行时数据**：类型名 + 属性列表 + 类型化取值。
//
// 这一层刻意不依赖 EnTT、不依赖 Vulkan：Scene 的序列化、编辑器命令、
// Inspector 都复用同一份 ComponentRecord，避免三处字段名漂移。
// ===========================================================================

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace scene
{

enum class PropertyType
{
    Bool,
    Int,
    Float,
    Vec3,
    Color,
    String,
};

struct PropertyValue
{
    PropertyType type = PropertyType::Float;

    bool boolValue = false;
    int intValue = 0;
    float floatValue = 0.0f;
    glm::vec3 vec3{ 0.0f };
    glm::vec4 color{ 1.0f };
    std::string stringValue;

    static PropertyValue fromBool(bool value)
    {
        PropertyValue result;
        result.type = PropertyType::Bool;
        result.boolValue = value;
        return result;
    }

    static PropertyValue fromInt(int value)
    {
        PropertyValue result;
        result.type = PropertyType::Int;
        result.intValue = value;
        return result;
    }

    static PropertyValue fromFloat(float value)
    {
        PropertyValue result;
        result.type = PropertyType::Float;
        result.floatValue = value;
        return result;
    }

    static PropertyValue fromVec3(const glm::vec3& value)
    {
        PropertyValue result;
        result.type = PropertyType::Vec3;
        result.vec3 = value;
        return result;
    }

    static PropertyValue fromColor(const glm::vec4& value)
    {
        PropertyValue result;
        result.type = PropertyType::Color;
        result.color = value;
        return result;
    }

    static PropertyValue fromString(const std::string& value)
    {
        PropertyValue result;
        result.type = PropertyType::String;
        result.stringValue = value;
        return result;
    }

    // 容错读取：类型不匹配时返回 0 / false，不抛异常。组件写入走这些函数，
    // 因此一个坏掉的场景文件最多把某个属性恢复成默认值，不会让编辑器崩掉。
    bool asBool() const
    {
        switch (type)
        {
        case PropertyType::Bool: return boolValue;
        case PropertyType::Int: return intValue != 0;
        case PropertyType::Float: return floatValue != 0.0f;
        default: return false;
        }
    }

    int asInt() const
    {
        switch (type)
        {
        case PropertyType::Bool: return boolValue ? 1 : 0;
        case PropertyType::Int: return intValue;
        case PropertyType::Float: return static_cast<int>(floatValue);
        default: return 0;
        }
    }

    float asFloat() const
    {
        switch (type)
        {
        case PropertyType::Bool: return boolValue ? 1.0f : 0.0f;
        case PropertyType::Int: return static_cast<float>(intValue);
        case PropertyType::Float: return floatValue;
        default: return 0.0f;
        }
    }

    glm::vec3 asVec3() const
    {
        switch (type)
        {
        case PropertyType::Vec3: return vec3;
        case PropertyType::Color: return glm::vec3(color);
        case PropertyType::Float: return glm::vec3(floatValue);
        case PropertyType::Int: return glm::vec3(static_cast<float>(intValue));
        default: return glm::vec3(0.0f);
        }
    }

    glm::vec4 asColor() const
    {
        switch (type)
        {
        case PropertyType::Color: return color;
        case PropertyType::Vec3: return glm::vec4(vec3, 1.0f);
        case PropertyType::Float: return glm::vec4(floatValue, floatValue, floatValue, 1.0f);
        default: return glm::vec4(1.0f);
        }
    }

    const std::string& asString() const { return stringValue; }

    bool operator==(const PropertyValue& other) const
    {
        return type == other.type &&
               boolValue == other.boolValue &&
               intValue == other.intValue &&
               floatValue == other.floatValue &&
               vec3 == other.vec3 &&
               color == other.color &&
               stringValue == other.stringValue;
    }

    bool operator!=(const PropertyValue& other) const { return !(*this == other); }
};

struct NamedProperty
{
    std::string name;
    std::string label;
    PropertyValue value;
};

// 一个已挂载组件的纯数据快照。type 对应 ComponentRegistry 里的稳定 id，
// properties 只包含注册时暴露出来的字段。
struct ComponentRecord
{
    std::string type;
    std::vector<NamedProperty> properties;

    const NamedProperty* find(const std::string& propertyName) const
    {
        for (const NamedProperty& property : properties)
        {
            if (property.name == propertyName)
                return &property;
        }
        return nullptr;
    }

    NamedProperty* find(const std::string& propertyName)
    {
        for (NamedProperty& property : properties)
        {
            if (property.name == propertyName)
                return &property;
        }
        return nullptr;
    }

    // 用于撤销合并 / 序列化自检：忽略 label（label 是 UI 元数据，
    // 从注册表重新生成，不参与数据比较）。
    bool equivalentTo(const ComponentRecord& other) const
    {
        if (type != other.type || properties.size() != other.properties.size())
            return false;
        for (const NamedProperty& property : properties)
        {
            const NamedProperty* otherProperty = other.find(property.name);
            if (otherProperty == nullptr || property.value != otherProperty->value)
                return false;
        }
        return true;
    }
};

} // namespace scene
