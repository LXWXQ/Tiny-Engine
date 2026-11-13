#pragma once

// 组件属性的通用 ImGui 控件。
//
// Inspector 与 Render 的「组件创建向导」必须画同一套控件：范围、拖动速度、
// 提示、右键重置都来自 ComponentRegistry 的元数据。把这里做成头文件而不是
// 各面板各写一份 switch，避免两个入口对同一个字段给出不同的编辑行为。

#include "Scene/ComponentRegistry.h"
#include "Scene/ComponentTypes.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace editor
{

// 通用属性控件。defaults 是组件默认值快照：右键菜单用它做「重置为默认值」，
// 没有默认值时不弹菜单。属性元数据（范围/拖动速度/提示）来自 ComponentRegistry。
inline bool drawPropertyWidgets(scene::ComponentRecord& record,
                                const scene::ComponentDescriptor& descriptor,
                                const scene::ComponentRecord* defaults = nullptr)
{
    bool changed = false;
    for (const scene::PropertyDescriptor& property : descriptor.properties)
    {
        scene::NamedProperty* value = record.find(property.name);
        if (value == nullptr)
            continue;
        // 贴图等资源字段由组件自己的专用控件绘制，通用控件不重复出现，
        // 也不允许手输绝对路径。
        if (property.hidden)
            continue;

        ImGui::PushID(property.name.c_str());
        const scene::NamedProperty* defaultValue =
            defaults != nullptr ? defaults->find(property.name) : nullptr;
        const bool hasRange = property.hasRange && property.maximum > property.minimum;
        const float fallbackSpeed =
            property.type == scene::PropertyType::Int ? 1.0f : 0.01f;
        const float dragSpeed = property.dragSpeed > 0.0f ? property.dragSpeed : fallbackSpeed;

        switch (property.type)
        {
        case scene::PropertyType::Bool:
        {
            bool v = value->value.asBool();
            if (ImGui::Checkbox(property.label.c_str(), &v))
            {
                value->value = scene::PropertyValue::fromBool(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::Int:
        {
            int v = value->value.asInt();
            bool edited = false;
            if (!property.enumLabels.empty())
            {
                const int count = static_cast<int>(property.enumLabels.size());
                if (v < 0 || v >= count)
                    v = 0;
                std::vector<const char*> items;
                items.reserve(property.enumLabels.size());
                for (const std::string& enumLabel : property.enumLabels)
                    items.push_back(enumLabel.c_str());
                edited = ImGui::Combo(property.label.c_str(), &v,
                                      items.data(), count);
            }
            else
            {
                edited = hasRange
                    ? ImGui::DragInt(property.label.c_str(), &v, dragSpeed,
                                     static_cast<int>(property.minimum),
                                     static_cast<int>(property.maximum), "%d")
                    : ImGui::DragInt(property.label.c_str(), &v, dragSpeed);
            }
            if (edited)
            {
                value->value = scene::PropertyValue::fromInt(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::Float:
        {
            float v = value->value.asFloat();
            const bool edited = hasRange
                ? ImGui::DragFloat(property.label.c_str(), &v, dragSpeed,
                                   property.minimum, property.maximum, "%.3f")
                : ImGui::DragFloat(property.label.c_str(), &v, dragSpeed, 0.0f, 0.0f, "%.3f");
            if (edited)
            {
                value->value = scene::PropertyValue::fromFloat(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::Vec3:
        {
            glm::vec3 v = value->value.asVec3();
            const bool edited = hasRange
                ? ImGui::DragFloat3(property.label.c_str(), glm::value_ptr(v), dragSpeed,
                                    property.minimum, property.maximum, "%.3f")
                : ImGui::DragFloat3(property.label.c_str(), glm::value_ptr(v),
                                    dragSpeed, 0.0f, 0.0f, "%.3f");
            if (edited)
            {
                value->value = scene::PropertyValue::fromVec3(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::Color:
        {
            glm::vec4 v = value->value.asColor();
            if (ImGui::ColorEdit4(property.label.c_str(), glm::value_ptr(v)))
            {
                value->value = scene::PropertyValue::fromColor(v);
                changed = true;
            }
            break;
        }
        case scene::PropertyType::String:
        {
            char buffer[256] = {};
            std::snprintf(buffer, sizeof(buffer), "%s", value->value.asString().c_str());
            if (ImGui::InputText(property.label.c_str(), buffer, sizeof(buffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                value->value = scene::PropertyValue::fromString(buffer);
                changed = true;
            }
            break;
        }
        }

        // 提示里同时给「这个字段是什么」和「怎么操作」，避免只有一句泛泛的
        // 「拖动修改」。范围由注册表提供，UI 不再自己猜。
        if (ImGui::IsItemHovered())
        {
            std::string tip = property.tooltip;
            if (hasRange)
            {
                char range[96] = {};
                std::snprintf(range, sizeof(range), "范围 [%.3f, %.3f]",
                              property.minimum, property.maximum);
                if (!tip.empty())
                    tip += "\n";
                tip += range;
            }
            if (!tip.empty())
                tip += "\n";
            tip += property.type == scene::PropertyType::Bool
                ? "点击切换，右键重置"
                : (!property.enumLabels.empty()
                    ? "点击选择，右键重置"
                    : "拖动修改，Ctrl+点击输入数值，右键重置");
            ImGui::SetTooltip("%s", tip.c_str());
        }

        if (defaultValue != nullptr && ImGui::BeginPopupContextItem("##property_menu"))
        {
            if (ImGui::MenuItem("重置为默认值"))
            {
                value->value = defaultValue->value;
                changed = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    return changed;
}

// 从完整组件描述里裁出一个分组（只保留指定属性），复用同一套控件与默认值
// 重置逻辑。体积雾的专用面板用它把 14 个字段分成 Placement/Appearance/...
inline scene::ComponentDescriptor makePropertyGroup(
    const scene::ComponentDescriptor& descriptor,
    std::initializer_list<const char*> names)
{
    scene::ComponentDescriptor group = descriptor;
    group.properties.clear();
    for (const char* name : names)
    {
        for (const scene::PropertyDescriptor& property : descriptor.properties)
        {
            if (property.name == name)
            {
                group.properties.push_back(property);
                break;
            }
        }
    }
    return group;
}

inline void setVec3Property(scene::ComponentRecord& record, const char* name,
                            const glm::vec3& value)
{
    if (scene::NamedProperty* property = record.find(name))
        property->value = scene::PropertyValue::fromVec3(value);
}

} // namespace editor
