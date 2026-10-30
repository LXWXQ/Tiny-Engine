#include "Editor/panels/AssetBrowserPanel.h"
#include "Editor/EditorState.h"
#include "Editor/Commands.h"
#include "Scene/Scene.h"
#include <glm/glm.hpp>
#include "AssetPaths.h"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

namespace editor
{

namespace
{
std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool isModelExtension(const std::string& ext)
{
    return ext == ".obj" || ext == ".gltf" || ext == ".glb";
}

// 相机前向。必须与 CameraController::cameraForward 用同一个公式：
// 这套相机的投影把 view-space +Z 当作前方，所以世界前向是 +w 而不是 -w。
glm::vec3 cameraForward(const glm::vec3& rotation)
{
    const float yaw = rotation.y;
    const float pitch = rotation.x;
    const float cosPitch = std::cos(pitch);
    return glm::vec3{
        std::sin(yaw) * cosPitch,
        -std::sin(pitch),
        std::cos(yaw) * cosPitch
    };
}

// 文件名（不含扩展名）作为默认实体名。
std::string stemOf(const std::filesystem::path& path)
{
    return path.stem().string();
}
} // namespace

void AssetBrowserPanel::onAttach(EditorState& /*state*/)
{
    refresh();
}

void AssetBrowserPanel::refresh()
{
    models.clear();
    scanned = true;

    const std::filesystem::path root = std::filesystem::path(SENTINEL_RESOURCE_DIR) / "Models";

    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
    {
        std::cerr << "[Assets] models directory not found: " << root.string() << "\n";
        return;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;

        const std::string ext = toLower(entry.path().extension().string());
        if (!isModelExtension(ext))
            continue;

        AssetEntry asset;
        asset.absolutePath = entry.path().string();
        asset.extension = ext;

        // 显示名用相对 Models/ 的路径，这样 Sponza.gltf 和某个子目录里的
        // 同名文件不会看起来完全一样。
        std::error_code relEc;
        const auto rel = std::filesystem::relative(entry.path(), root, relEc);
        asset.displayName = relEc ? entry.path().filename().string() : rel.string();

        models.push_back(std::move(asset));
    }

    std::sort(models.begin(), models.end(),
        [](const AssetEntry& a, const AssetEntry& b) { return a.displayName < b.displayName; });

    std::cout << "[Assets] scanned " << models.size() << " model file(s) under "
              << root.string() << "\n";
}

void AssetBrowserPanel::draw(EditorState& state)
{
    if (!ImGui::Begin("Assets", &visible))
    {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Rescan"))
        refresh();
    ImGui::SameLine();
    ImGui::TextDisabled("%d models", (int)models.size());

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "filter...", filter, sizeof(filter));

    ImGui::Separator();

    const std::string filterText = toLower(std::string(filter));

    ImGui::BeginChild("asset_list", ImVec2(0, 0), ImGuiChildFlags_None);

    for (const AssetEntry& asset : models)
    {
        if (!filterText.empty() && toLower(asset.displayName).find(filterText) == std::string::npos)
            continue;

        ImGui::PushID(asset.absolutePath.c_str());

        // 用 Selectable 而不是 Button：整行可点，长路径也不会把面板撑宽。
        const bool clicked = ImGui::Selectable(asset.displayName.c_str(), false,
            ImGuiSelectableFlags_AllowDoubleClick);

        if (clicked)
        {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                // 双击 = 加入场景。走命令栈，所以 Ctrl+Z 能撤销。
                if (state.services.scene != nullptr)
                {
                    // 生成在相机前方 4 个单位处，而不是世界原点。
                    // 相机离原点可能很远，落在原点会让用户以为"没生成"。
                    const TransformComponent& cameraTransform =
                        state.services.scene->getCameraTransform();
                    const glm::vec3 forward = cameraForward(cameraTransform.rotation);
                    const glm::vec3 spawn = cameraTransform.translation + forward * 4.0f;

                    auto command = std::make_unique<CreateEntityCommand>(
                        *state.services.registry, *state.services.scene,
                        asset.absolutePath, stemOf(std::filesystem::path(asset.absolutePath)),
                        spawn);

                    // 记下实体以便选中：命令 apply 之后才拿得到 entity。
                    CreateEntityCommand* raw = command.get();
                    state.commands.execute(std::move(command));

                    if (raw->currentEntity() != entt::null)
                        state.selection.select(raw->currentEntity());

                    state.markDirty();
                }
            }
            else
            {
                state.pushLog(LogEntry::Level::Info,
                    "[Assets] double-click to add: " + asset.displayName);
            }
        }

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", asset.absolutePath.c_str());

        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace editor
