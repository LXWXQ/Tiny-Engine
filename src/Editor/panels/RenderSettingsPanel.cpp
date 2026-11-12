#include "Editor/panels/RenderSettingsPanel.h"
#include "Editor/EditorState.h"
#include "Editor/Commands.h"
#include "Editor/ComponentWidgets.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/ComponentRegistry.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace editor
{

namespace
{

// 与 CameraController::cameraForward 同一个公式：这套相机的 view-space +Z
// 是前方，所以世界前向由 yaw/pitch 直接算出，不是四元数乘 -Z。
glm::vec3 cameraForward(const glm::vec3& euler)
{
    const float yaw = euler.y;
    const float pitch = euler.x;
    const float cosPitch = std::cos(pitch);
    return glm::vec3{
        std::sin(yaw) * cosPitch,
        -std::sin(pitch),
        std::cos(yaw) * cosPitch
    };
}

std::string lowercaseCopy(const char* text)
{
    std::string result = text != nullptr ? text : "";
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool matchesFilter(const scene::ComponentDescriptor& descriptor, const std::string& needle)
{
    if (needle.empty())
        return true;
    return lowercaseCopy(descriptor.label.c_str()).find(needle) != std::string::npos ||
           lowercaseCopy(descriptor.category.c_str()).find(needle) != std::string::npos ||
           lowercaseCopy(descriptor.type.c_str()).find(needle) != std::string::npos ||
           lowercaseCopy(descriptor.description.c_str()).find(needle) != std::string::npos;
}

void setFloatProperty(scene::ComponentRecord& record, const char* name, float value)
{
    if (scene::NamedProperty* property = record.find(name))
        property->value = scene::PropertyValue::fromFloat(value);
}


bool matchesSkyboxFilter(const std::string& needle)
{
    if (needle.empty())
        return true;
    return std::string("天空盒").find(needle) != std::string::npos ||
           std::string("skybox").find(needle) != std::string::npos ||
           std::string("系统对象").find(needle) != std::string::npos;
}

entt::entity findSkyboxEntity(Scene& scene)
{
    entt::registry& registry = scene.getRegistry();
    for (entt::entity entity : registry.view<RenderTagComponent>())
    {
        const RenderTagComponent* tag = registry.try_get<RenderTagComponent>(entity);
        if (tag != nullptr && tag->isSkybox)
            return entity;
    }
    return entt::null;
}
} // namespace

int RenderSettingsPanel::clampKernelSize(int value)
{
    if (value < 4)
        value = 4;
    if (value > 64)
        value = 64;
    return (value % 2 == 0) ? value : value + 1;
}

bool RenderSettingsPanel::selfTest()
{
    if (clampKernelSize(3) != 4 || clampKernelSize(65) != 64 ||
        clampKernelSize(16) != 16 || clampKernelSize(17) != 18)
    {
        std::fprintf(stderr, "[Editor] render settings selfTest FAIL: kernel clamp\n");
        return false;
    }
    std::printf("[Editor] render settings selfTest PASS | kernel clamped to 4..64 even\n");
    return true;
}

void RenderSettingsPanel::draw(EditorState& state)
{
    if (!visible)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 workPos = viewport->WorkPos;
    const ImVec2 workSize = viewport->WorkSize;
    ImGui::SetNextWindowPos(ImVec2(workPos.x + workSize.x - 600.0f, workPos.y + 40.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 700.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, 300.0f), ImVec2(FLT_MAX, FLT_MAX));

    if (!ImGui::Begin("Render", &visible))
    {
        ImGui::End();
        return;
    }

    Scene* scene = state.services.scene;
    if (scene == nullptr)
    {
        ImGui::TextDisabled("场景不可用（编辑器没有拿到 Scene）。");
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("组件目录：按特效类型分组。单一性组件每个场景只有一份，没有位置、旋转、缩放；对象组件会在场景中创建一个带 Transform 的新对象。");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 64.0f);
    ImGui::InputTextWithHint("##component_search", "搜索组件 / 分类 / id", search_, IM_ARRAYSIZE(search_));
    if (search_[0] != '\0')
    {
        ImGui::SameLine();
        if (ImGui::Button("清除", ImVec2(56.0f, 0.0f)))
            search_[0] = '\0';
    }

    ImGui::Separator();
    drawCatalog(state, *scene);
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::CollapsingHeader("引擎默认值（config/engine.json）"))
    {
        ImGui::TextDisabled("这些是渲染器的全局默认值；单个场景的开关请在组件目录里创建对应的单一性组件。");
        const bool canSave = static_cast<bool>(state.services.saveConfig);
        if (!canSave)
            ImGui::BeginDisabled();
        if (ImGui::Button("保存到 engine.json"))
        {
            state.services.saveConfig();
            state.notifySuccess("engine.json 已保存");
        }
        if (!canSave)
            ImGui::EndDisabled();

        ImGui::SameLine();
        const bool canReload = static_cast<bool>(state.services.reloadConfig);
        if (!canReload)
            ImGui::BeginDisabled();
        if (ImGui::Button("重新载入 engine.json"))
        {
            state.services.reloadConfig();
            state.notifySuccess("engine.json 已重新载入");
        }
        if (!canReload)
            ImGui::EndDisabled();

        if (state.services.settings != nullptr)
            drawAdvanced(*state.services.settings);
        else
            ImGui::TextDisabled("引擎设置不可用（编辑器没有拿到 EngineSettings）。");
    }

    if (wizardRequested_)
    {
        ImGui::OpenPopup("创建组件");
        wizardRequested_ = false;
    }
    drawWizard(state, *scene);

    ImGui::End();
}

void RenderSettingsPanel::drawCatalog(EditorState& state, Scene& scene)
{
    const scene::ComponentRegistry& registry = scene.getComponentRegistry();
    const std::string needle = lowercaseCopy(search_);

    int total = 0;
    for (const scene::ComponentDescriptor& descriptor : registry.descriptors())
    {
        if (descriptor.exposed && matchesFilter(descriptor, needle))
            ++total;
    }
    const bool showSkybox = matchesSkyboxFilter(needle);
    if (showSkybox)
        ++total;
    if (total == 0)
    {
        ImGui::TextDisabled("没有匹配的组件。");
        return;
    }

    const auto drawScope = [&](scene::ComponentScope scope, const char* title, const char* hint)
    {
        std::vector<std::string> categories;
        int count = 0;
        for (const scene::ComponentDescriptor& descriptor : registry.descriptors())
        {
            if (!descriptor.exposed || descriptor.scope != scope ||
                !matchesFilter(descriptor, needle))
                continue;
            ++count;
            if (std::find(categories.begin(), categories.end(), descriptor.category) == categories.end())
                categories.push_back(descriptor.category);
        }
        if (count == 0)
            return;

        ImGui::PushID(scope == scene::ComponentScope::Scene ? 1 : 2);
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.65f, 0.80f, 1.00f, 1.0f), "%s", title);
        ImGui::SameLine();
        ImGui::TextDisabled("(%d)", count);
        ImGui::TextDisabled("%s", hint);

        for (const std::string& category : categories)
        {
            ImGui::SeparatorText(category.c_str());
            ImGui::PushID(category.c_str());
            if (ImGui::BeginTable("##catalog", 2,
                                  ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX))
            {
                for (const scene::ComponentDescriptor& descriptor : registry.descriptors())
                {
                    if (!descriptor.exposed || descriptor.scope != scope ||
                        descriptor.category != category || !matchesFilter(descriptor, needle))
                        continue;

                    ImGui::TableNextColumn();
                    ImGui::PushID(descriptor.type.c_str());

                    const bool mounted = scope == scene::ComponentScope::Scene &&
                                         descriptor.has(scene.getRegistry(),
                                                        scene.getSceneSettingsEntity());

                    if (!descriptor.addable)
                        ImGui::BeginDisabled();
                    if (ImGui::Button(descriptor.label.c_str(), ImVec2(-FLT_MIN, 0.0f)))
                        openWizard(state, scene, descriptor);
                    if (!descriptor.addable)
                        ImGui::EndDisabled();

                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        if (!descriptor.addable)
                        {
                            ImGui::SetTooltip("%s", descriptor.unavailableReason.empty()
                                                       ? "该组件由系统托管，不能手动创建。"
                                                       : descriptor.unavailableReason.c_str());
                        }
                        else if (!descriptor.description.empty())
                        {
                            ImGui::SetTooltip("%s", descriptor.description.c_str());
                        }
                    }

                    if (mounted)
                        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f),
                                           "已挂载 · 点击编辑");
                    else if (!descriptor.addable)
                        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f),
                                           "系统托管，不能创建");

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    };

    drawScope(scene::ComponentScope::Scene, "单一性组件",
              "每个场景一份，没有位置、旋转、缩放；创建后写入场景设置。");
    drawScope(scene::ComponentScope::Entity, "对象组件",
              "创建时新建一个带 Transform 的对象；灰色项由系统托管，不能手动创建。");

    if (showSkybox)
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.65f, 0.80f, 1.00f, 1.0f), "系统对象");
        ImGui::SameLine();
        ImGui::TextDisabled("(1)");
        ImGui::TextDisabled("天空盒不是注册表组件：它由场景启动时创建，每个场景一个。");
        ImGui::SeparatorText("大气");
        ImGui::PushID("system.skybox");
        if (ImGui::BeginTable("##catalog", 2,
                              ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX))
        {
            ImGui::TableNextColumn();
            if (ImGui::Button("天空盒", ImVec2(-FLT_MIN, 0.0f)))
            {
                pendingType_ = "system.skybox";
                wizardRequested_ = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "系统对象：球体 + RenderTag.isSkybox；不是组件注册表条目，"
                    "但同样每个场景只有一个。");
            }

            const entt::entity skybox = findSkyboxEntity(scene);
            if (skybox != entt::null)
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f),
                                   "场景中已存在 · 点击查看");
            else
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f),
                                   "当前场景没有天空盒对象");
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
}

void RenderSettingsPanel::openWizard(EditorState& state, Scene& scene,
                                     const scene::ComponentDescriptor& descriptor)
{
    (void)state;
    pendingType_ = descriptor.type;

    defaults_ = scene.defaultComponentRecord(descriptor.type)
                    .value_or(scene::ComponentRecord{});
    if (defaults_.type.empty())
        defaults_.type = descriptor.type;

    const std::optional<scene::ComponentRecord> existing =
        scene.readComponent(scene.getSceneSettingsEntity(), descriptor.type);
    draft_ = existing.value_or(defaults_);

    std::snprintf(nameBuffer_, sizeof(nameBuffer_), "%s", descriptor.label.c_str());
    spawnEuler_ = glm::vec3(0.0f);
    spawnScale_ = glm::vec3(1.0f);

    float distance = 10.0f;
    if (descriptor.type == "effects.fogVolume")
    {
        distance = 18.0f;
        setVec3Property(draft_, "size", glm::vec3(30.0f, 12.0f, 30.0f));
        setFloatProperty(draft_, "density", 0.035f);
    }

    const TransformComponent& camera = scene.getCameraTransform();
    spawnPosition_ = camera.getTranslation() + cameraForward(camera.getEuler()) * distance;
    spawnPosition_.y = std::max(spawnPosition_.y, 0.5f);

    wizardRequested_ = true;
}

void RenderSettingsPanel::drawWizard(EditorState& state, Scene& scene)
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("创建组件", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    if (pendingType_ == "system.skybox")
    {
        ImGui::TextDisabled("大气 · 系统对象");
        ImGui::SeparatorText("天空盒");
        ImGui::PushTextWrapPos(520.0f);
        ImGui::TextWrapped(
            "天空盒不是组件注册表条目：它由场景启动时创建一个球体对象，"
            "并用 RenderTag.isSkybox 标记；每个场景只有一个。");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        const entt::entity skybox = findSkyboxEntity(scene);
        if (skybox != entt::null)
        {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f),
                               "场景中已存在天空盒对象。");
            ImGui::Spacing();
            if (ImGui::Button("在 Inspector 中编辑", ImVec2(200.0f, 0.0f)))
            {
                state.selection.select(skybox);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
        }
        else
        {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f),
                               "当前场景没有天空盒对象。");
            ImGui::Spacing();
        }

        if (ImGui::Button("关闭", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const scene::ComponentRegistry& registry = scene.getComponentRegistry();
    const scene::ComponentDescriptor* descriptor = registry.find(pendingType_);
    if (descriptor == nullptr)
    {
        ImGui::TextDisabled("组件类型已失效：%s", pendingType_.c_str());
        if (ImGui::Button("关闭", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextDisabled("%s", descriptor->category.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("· %s", descriptor->scope == scene::ComponentScope::Scene
                                        ? "单一性组件"
                                        : "对象组件");
    ImGui::SeparatorText(descriptor->label.c_str());

    if (!descriptor->description.empty())
    {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 520.0f);
        ImGui::TextWrapped("%s", descriptor->description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }

    if (!descriptor->addable)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.25f, 1.0f), "该组件不能手动创建。");
        if (!descriptor->unavailableReason.empty())
            ImGui::TextWrapped("%s", descriptor->unavailableReason.c_str());
        ImGui::Spacing();
        if (ImGui::Button("关闭", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const entt::entity settingsEntity = scene.getSceneSettingsEntity();

    if (descriptor->scope == scene::ComponentScope::Scene)
    {
        const bool mounted = descriptor->has(scene.getRegistry(), settingsEntity);
        if (mounted)
        {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f),
                               "该组件已经在场景中。每个场景只能有一个单一性组件。");
            ImGui::TextDisabled("移除后可以从目录重新创建。");
            ImGui::Spacing();
            ImGui::SeparatorText("当前参数");
            drawPropertyWidgets(draft_, *descriptor, &defaults_);

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Button("应用修改", ImVec2(140.0f, 0.0f)))
            {
                const std::optional<scene::ComponentRecord> before =
                    scene.readComponent(settingsEntity, descriptor->type);
                if (before)
                {
                    if (!before->equivalentTo(draft_))
                    {
                        state.commands.execute(std::make_unique<SetComponentRecordCommand>(
                            scene, settingsEntity, descriptor->type, before, draft_,
                            "Modify Scene Component"));
                        state.markDirty();
                        state.notifySuccess("已更新场景组件：" + descriptor->label);
                    }
                    else
                    {
                        state.notifySuccess("场景组件参数没有变化");
                    }
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("移除组件", ImVec2(120.0f, 0.0f)))
            {
                const std::optional<scene::ComponentRecord> before =
                    scene.readComponent(settingsEntity, descriptor->type);
                if (before)
                {
                    state.commands.execute(std::make_unique<SetComponentRecordCommand>(
                        scene, settingsEntity, descriptor->type, before, std::nullopt,
                        "Remove Scene Component"));
                    state.markDirty();
                    state.notifySuccess("已移除场景组件：" + descriptor->label);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(100.0f, 0.0f)))
                ImGui::CloseCurrentPopup();
        }
        else
        {
            ImGui::TextDisabled("创建后写入场景设置；这个组件没有位置、旋转、缩放。");
            ImGui::SeparatorText("初始参数");
            drawPropertyWidgets(draft_, *descriptor, &defaults_);

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Button("创建并挂到场景", ImVec2(180.0f, 0.0f)))
            {
                if (draft_.type.empty())
                    draft_.type = descriptor->type;
                state.commands.execute(std::make_unique<SetComponentRecordCommand>(
                    scene, settingsEntity, descriptor->type, std::nullopt, draft_,
                    "Add Scene Component"));
                state.markDirty();
                state.notifySuccess("已创建单一性组件：" + descriptor->label);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(100.0f, 0.0f)))
                ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
        return;
    }

    ImGui::TextDisabled("创建时会新建一个带 Transform 的对象，并把组件挂到它上面。");
    ImGui::SeparatorText("对象");
    ImGui::PushItemWidth(300.0f);
    ImGui::InputText("名称", nameBuffer_, IM_ARRAYSIZE(nameBuffer_));
    ImGui::DragFloat3("位置", &spawnPosition_.x, 0.1f);
    ImGui::DragFloat3("旋转（度）", &spawnEuler_.x, 0.5f);
    ImGui::DragFloat3("缩放", &spawnScale_.x, 0.01f, 0.001f, 1000.0f);
    ImGui::PopItemWidth();

    ImGui::SeparatorText("组件参数");
    drawPropertyWidgets(draft_, *descriptor, &defaults_);

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("创建对象并挂载", ImVec2(180.0f, 0.0f)))
    {
        if (draft_.type.empty())
            draft_.type = descriptor->type;

        const std::string objectName =
            nameBuffer_[0] != '\0' ? std::string(nameBuffer_) : descriptor->label;

        std::vector<scene::ComponentRecord> components;
        components.push_back(draft_);

        auto command = std::make_unique<CreateComponentEntityCommand>(
            scene, objectName, spawnPosition_, glm::radians(spawnEuler_), spawnScale_,
            std::move(components));
        CreateComponentEntityCommand* raw = command.get();
        state.commands.execute(std::move(command));

        const entt::entity created = raw->currentEntity();
        if (created != entt::null)
        {
            state.selection.select(created);
            state.markDirty();
            state.notifySuccess("已创建对象：" + objectName);
        }
        else
        {
            state.notify(LogEntry::Level::Error, "创建对象失败：场景没有返回有效实体");
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(100.0f, 0.0f)))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

void RenderSettingsPanel::drawAdvanced(EngineSettings& settings)
{
    if (!ImGui::CollapsingHeader("提交路径（调试）"))
        return;

    ImGui::PushItemWidth(180.0f);
    ImGui::Checkbox("GPU 场景提交", &settings.gpuSceneEnabled);
    ImGui::Checkbox("GPU 合并缓冲", &settings.gpuMergedBuffers);
    ImGui::Checkbox("Hi-Z 遮挡剔除", &settings.hizEnabled);
    ImGui::Checkbox("地形批次合并", &settings.terrainBatchEnabled);

    int minInstances = static_cast<int>(settings.gpuSceneMinInstances);
    if (ImGui::DragInt("GPU 场景最小实例数", &minInstances, 1.0f, 1, 2048))
        settings.gpuSceneMinInstances = static_cast<std::uint32_t>(std::max(minInstances, 1));

    ImGui::DragFloat("GPU 场景最小像素", &settings.gpuSceneMinScreenPixels, 0.1f, 0.0f, 64.0f);
    ImGui::PopItemWidth();
}

} // namespace editor
