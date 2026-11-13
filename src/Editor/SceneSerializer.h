#pragma once

// ===========================================================================
// 场景描述 <-> JSON
//
// 分层：SceneSerializer 只做"纯数据 <-> 文本"，不知道 Vulkan、不知道 Scene。
// 真正的"registry <-> 描述"转换在 Scene::captureDescription / buildFromDescription 里。
//
// 这样切开的好处：序列化逻辑可以单独测（给定描述对象，验证 JSON 往返一致），
// 不需要创建 Vulkan 设备。
// ===========================================================================

#include "Scene/SceneDescription.h"

#include <string>

namespace editor
{

class SceneSerializer
{
public:
    // 把场景描述写成 JSON 文件。路径按"相对资源根目录"存，换机器仍可用。
    static bool saveToFile(const scene::SceneDescription& description,
                           const std::string& filepath,
                           std::string& errorMessage);

    static bool loadFromFile(const std::string& filepath,
                             scene::SceneDescription& outDescription,
                             std::string& errorMessage);

    // 纯内存往返（自检用，不碰文件系统）。
    static bool roundTrip(const scene::SceneDescription& input,
                          scene::SceneDescription& output,
                          std::string& errorMessage);

    // B3：材质只存非默认字段的契约自检（不需要 Vulkan / 文件系统）。
    static bool materialJsonSelfTest();
};

} // namespace editor
