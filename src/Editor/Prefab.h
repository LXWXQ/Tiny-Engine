#pragma once

// ===========================================================================
// 预制体（Prefab）资产
//
// 复用 v3 SceneDescription 作为文件格式，扩展名 .prefab.json：
//   - 只保存一棵子树（根实体 parentIndex=-1，其余下标指向子树内部）
//   - 不写 voxelWorlds / 地形 / 相机 / 天空盒 / 网格辅助等运行期实体
//   - 打开时可以直接当一个普通场景看，也可以在编辑器里实例化到现有场景
//
// 本文件只做“校验 + 读写”，实例化在 Scene::instantiatePrefab。
// ===========================================================================

#include "Scene/SceneDescription.h"

#include <entt/entt.hpp>
#include <string>

class Scene;

namespace editor
{
namespace prefab
{

// 校验描述是否可以作为预制体：单根、无运行期实体、无体素世界、父子下标合法。
bool validate(const scene::SceneDescription& description, std::string& errorMessage);

// 捕获 root 的子树并写成 .prefab.json。root 无效、含运行期实体或写盘失败返回 false。
bool save(Scene& scene, entt::entity root, const std::string& filepath,
          std::string& errorMessage);

// 读入并校验 .prefab.json。
bool load(const std::string& filepath, scene::SceneDescription& outDescription,
          std::string& errorMessage);

} // namespace prefab
} // namespace editor
