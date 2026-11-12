#pragma once

// ===========================================================================
// 具体的可撤销操作。
//
// 每个命令都遵守同一个模式：**保存旧值 + 保存新值，apply 写新值，revert 写旧值**。
// 不要在命令里做"增量"（例如 translation += delta）：撤销时浮点误差会累积，
// 十几次撤销之后物体就回不到原位。
// ===========================================================================

#include "Editor/CommandStack.h"
#include "Scene/Components.h"   // MeshComponent / MaterialComponent 快照需要完整定义
#include "Scene/ComponentTypes.h"
#include "Scene/SceneDescription.h"

#include <entt/entt.hpp>
#include <optional>

class Scene;
#include <glm/glm.hpp>
#include <functional>
#include <string>
#include <vector>

namespace editor
{

// 修改一个实体的局部变换。拖拽滑块产生的连续命令会合并成一条。
class SetTransformCommand : public Command
{
public:
    SetTransformCommand(entt::registry& registry, entt::entity entity,
                        const glm::vec3& oldTranslation, const glm::vec3& newTranslation,
                        const glm::vec3& oldRotation, const glm::vec3& newRotation,
                        const glm::vec3& oldScale, const glm::vec3& newScale,
                        Scene* scene = nullptr);

    // 四元数版本：Gizmo 拖拽时旋转不能只存欧拉角（万向节处会丢信息）。
    SetTransformCommand(entt::registry& registry, entt::entity entity,
                        const glm::vec3& oldTranslation, const glm::vec3& newTranslation,
                        const glm::vec3& oldRotation, const glm::vec3& newRotation,
                        const glm::vec3& oldScale, const glm::vec3& newScale,
                        const glm::quat& oldOrientation, const glm::quat& newOrientation,
                        Scene* scene = nullptr);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Modify Transform"; }
    bool mergeWith(const Command& previous) const override;
    void absorb(const Command& newer) override;

private:
    entt::registry* registry;
    Scene* scene;
    entt::entity entity;
    glm::vec3 oldTranslation, newTranslation;
    glm::vec3 oldRotation, newRotation;
    glm::vec3 oldScale, newScale;
    glm::quat oldOrientation, newOrientation;
};

// E2：一次改多个实体的变换（多选拖拽 gizmo）。
//
// 为什么不让 Gizmo 每帧压 N 条 SetTransformCommand：那样一次拖拽会在栈里留下
// N 条命令，撤销要按 N 次。这里一条命令持有整组 pose，并支持合并窗口内的吸收。
struct TransformPose
{
    entt::entity entity = entt::null;
    glm::vec3 oldTranslation{ 0.0f };
    glm::vec3 newTranslation{ 0.0f };
    glm::vec3 oldRotation{ 0.0f };
    glm::vec3 newRotation{ 0.0f };
    glm::vec3 oldScale{ 1.0f };
    glm::vec3 newScale{ 1.0f };
    glm::quat oldOrientation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::quat newOrientation{ 1.0f, 0.0f, 0.0f, 0.0f };
};

class SetTransformsCommand : public Command
{
public:
    SetTransformsCommand(entt::registry& registry, std::vector<TransformPose> poses,
                         Scene* scene = nullptr);

    void apply() override;
    void revert() override;
    const char* name() const override
    {
        return poses.size() > 1 ? "Modify Transforms" : "Modify Transform";
    }
    bool mergeWith(const Command& previous) const override;
    void absorb(const Command& newer) override;

private:
    entt::registry* registry;
    Scene* scene = nullptr;
    std::vector<TransformPose> poses;
};

// 重命名实体。
class RenameEntityCommand : public Command
{
public:
    RenameEntityCommand(entt::registry& registry, entt::entity entity,
                        const std::string& oldName, const std::string& newName);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Rename Entity"; }
    bool mergeWith(const Command& previous) const override;
    void absorb(const Command& newer) override;

private:
    entt::registry* registry;
    entt::entity entity;
    std::string oldName, newName;
};

// E3：材质参数快照 + 一条命令。
//
// 为什么需要快照：材质参数分散在 Material 的多个字段里，撤销要整体回到旧值，
// 而"只存改动的那个字段"会让合并窗口（拖滑杆）把中间态混进来。
struct MaterialSnapshot
{
    glm::vec4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float metallic = 1.0f;
    float roughness = 1.0f;
    float alphaCutoff = 0.5f;
    int alphaMode = 0;
};

class SetMaterialCommand : public Command
{
public:
    SetMaterialCommand(entt::registry& registry, entt::entity entity, MaterialSnapshot before,
                       MaterialSnapshot after, Scene* scene = nullptr);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Modify Material"; }
    bool mergeWith(const Command& previous) const override;
    void absorb(const Command& newer) override;

private:
    void writeState(const MaterialSnapshot& snapshot);

    entt::registry* registry;
    entt::entity entity;
    MaterialSnapshot before, after;
    Scene* scene = nullptr;
};

// E6：材质贴图替换。贴图对象由 Scene::loadMaterialTexture 加载并缓存，
// 命令只负责"换指针 + 刷新描述符"，这样撤销就是把指针换回去。
class SetMaterialTextureCommand : public Command
{
public:
    enum class Slot
    {
        Albedo,
        Normal,
        MetallicRoughness,
        Emissive,
        Ao
    };

    SetMaterialTextureCommand(entt::registry& registry, entt::entity entity, Slot slot,
                              std::shared_ptr<Texture> texture, Scene* scene = nullptr);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Modify Material Texture"; }

    // 槽位对应的贴图指针（apply / revert / UI 显示共用，避免三处各写一份 switch）。
    static std::shared_ptr<Texture>& slotTexture(Material& material, Slot slot);

private:
    void writeState(const std::shared_ptr<Texture>& texture);

    entt::registry* registry;
    entt::entity entity;
    Slot slot;
    std::shared_ptr<Texture> before, after;
    Scene* scene = nullptr;
};

// E3：Render Tag（Skybox / Grid）开关，同样要能撤销。
class SetRenderTagCommand : public Command
{
public:
    SetRenderTagCommand(entt::registry& registry, entt::entity entity, bool oldSkybox,
                        bool oldGrid, bool newSkybox, bool newGrid);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Modify Render Tag"; }
    bool mergeWith(const Command& previous) const override;
    void absorb(const Command& newer) override;

private:
    entt::registry* registry;
    entt::entity entity;
    bool oldSkybox, oldGrid, newSkybox, newGrid;
};

// 通用组件的增 / 删 / 改。记录组件前后的完整 ComponentRecord，
// 因此同一个命令既能处理“添加组件”（before 为空），也能处理“移除组件”
// （after 为空）。拖拽属性时由 mergeWith/absorb 合并成一条。
class SetComponentRecordCommand : public Command
{
public:
    SetComponentRecordCommand(Scene& scene, entt::entity entity, std::string type,
                               std::optional<scene::ComponentRecord> before,
                               std::optional<scene::ComponentRecord> after,
                               const char* action = "Modify Component");

    void apply() override;
    void revert() override;
    const char* name() const override { return commandName.c_str(); }
    bool mergeWith(const Command& previous) const override;
    void absorb(const Command& newer) override;

private:
    Scene& scene;
    entt::entity entity;
    std::string type;
    std::optional<scene::ComponentRecord> before;
    std::optional<scene::ComponentRecord> after;
    std::string commandName;
};

// 删除实体（连同整棵子树），可撤销。
//
// 关键设计：撤销**不能**指望实体 ID 保持不变 —— EnTT 销毁后重建会拿到新 ID。
// 所以构造时必须把
//   1) 每个实体的组件内容（值拷贝）
//   2) 整棵子树的父子关系（用"在快照里的下标"表示，不用实体 ID）
// 都存下来，revert 时按父先子后的顺序重建再重新挂接。
class DeleteEntityCommand : public Command
{
public:
    DeleteEntityCommand(entt::registry& registry, entt::entity entity,
                        Scene* scene = nullptr);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Delete Entity"; }

    // 撤销后重建出来的根实体（ID 与原实体不同），供上层更新选中状态。
    entt::entity restoredRoot() const { return restoredRootEntity; }

private:
    struct Node
    {
        // 原实体 ID。只在 apply() 里用来按父先子后的反序销毁 ——
        // revert() 之前它就已经失效，不能再当句柄用。
        entt::entity original = entt::null;
        // 重建该实体的组件（值拷贝进闭包）
        std::function<void(entt::entity)> rebuild;
        // 元数据注册的扩展组件（Spin / Orbit…），重建后逐条 apply。
        std::vector<scene::ComponentRecord> components;
        // 父节点在 snapshot 里的下标；-1 表示它是子树根
        int parentIndex = -1;
    };

    entt::registry* registry;
    Scene* scene;
    entt::entity rootEntity;
    std::vector<Node> snapshot;              // 父一定排在子之前
    std::vector<entt::entity> createdEntities;
    entt::entity restoredParent = entt::null;
    entt::entity restoredRootEntity = entt::null;
    bool captured = false;
};

// 从网格文件创建一个实体，可撤销。
//
// 关键设计：**撤销后重做不能重新加载模型**。
// Model::createModelFromFile 会真的读盘 + 上传顶点/索引缓冲，Sponza 那种规模
// 一次要几百毫秒。如果每次 redo 都重载，撤销/重做会卡到不可用。
// 所以首次 apply 之后把 MeshComponent（内部是 shared_ptr<Model>）快照下来，
// 之后 revert 只销毁实体（模型仍被快照持有），redo 直接复用同一个 Model。
class CreateEntityCommand : public Command
{
public:
    // scene 用于 createMeshEntity（它需要 device / commandPool / 材质系统）。
    // 只在这里用 Scene* 而不是 registry：创建网格实体本质上需要引擎能力。
    // spawnPosition 是**父节点局部空间**的落点。
    // 编辑器传入"相机前方"的位置，避免新建的物体永远叠在世界原点。
    CreateEntityCommand(entt::registry& registry, Scene& scene,
                        std::string meshPath, std::string name,
                        const glm::vec3& spawnPosition = glm::vec3(0.0f));

    void apply() override;
    void revert() override;
    const char* name() const override { return "Create Entity"; }

    // 当前实体（revert 之后为 entt::null）。供上层更新选中态。
    entt::entity currentEntity() const { return entity; }

private:
    entt::registry* registry;
    Scene* scene;
    std::string meshPath;
    std::string entityName;
    glm::vec3 spawnPosition{ 0.0f };

    entt::entity entity = entt::null;

    // 首次 apply 后填充：用于 redo 时免去重新加载。
    std::optional<MeshComponent> meshSnapshot;
    std::optional<MaterialComponent> materialSnapshot;
};


// 从「组件创建向导」创建的对象：空实体 + Name/Transform + 一组组件。
//
// 与 CreateEntityCommand 的区别：这里不加载任何网格，创建的是承载效果组件的
// 逻辑对象（粒子发射器、体积雾、Spin/Orbit…）。撤销时销毁实体，重做时按同样
// 的位姿与组件记录重建 —— EnTT 的实体 ID 会变，所以外部只能通过
// currentEntity() 重新取句柄。
class CreateComponentEntityCommand : public Command
{
public:
    CreateComponentEntityCommand(Scene& scene, std::string name,
                                 const glm::vec3& translation,
                                 const glm::vec3& eulerRadians,
                                 const glm::vec3& scale,
                                 std::vector<scene::ComponentRecord> components);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Create Component Object"; }

    // 当前实体（revert 之后为 entt::null）。
    entt::entity currentEntity() const { return entity; }

private:
    Scene& scene;
    std::string entityName;
    glm::vec3 translation{ 0.0f };
    glm::vec3 euler{ 0.0f };   // 弧度
    glm::vec3 scale{ 1.0f };
    std::vector<scene::ComponentRecord> components;
    entt::entity entity = entt::null;
};

// Creates a terrain through Scene::createTerrainEntity. Redo reuses the
// MeshComponent/TerrainComponent snapshots instead of regenerating noise or
// re-uploading GPU buffers.
class CreateTerrainCommand : public Command
{
public:
    // deferBuild = true 时走 Scene::createTerrainEntityAsync（tech-debt #19）：
    // 实体当帧出现、几何后台生成；此时不拍 mesh/terrain 快照，redo 会重新提交一次异步构建。
    CreateTerrainCommand(entt::registry& registry, Scene& scene,
                         terrain::TerrainConfig config,
                         std::string name,
                         const glm::vec3& spawnPosition = glm::vec3(0.0f),
                         bool deferBuild = false);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Create Terrain"; }

    entt::entity currentEntity() const { return entity; }

private:
    entt::registry* registry;
    Scene* scene;
    terrain::TerrainConfig config;
    std::string entityName;
    glm::vec3 spawnPosition{ 0.0f };
    entt::entity entity = entt::null;
    bool deferBuild = false;

    std::optional<MeshComponent> meshSnapshot;
    std::optional<TerrainComponent> terrainSnapshot;
    std::optional<RenderTagComponent> tagSnapshot;
    std::optional<BoundsComponent> boundsSnapshot;
};

class CreateCharacterCommand : public Command
{
public:
    CreateCharacterCommand(entt::registry& registry, Scene& scene,
                           std::string name, const glm::vec3& spawnPosition);
    void apply() override;
    void revert() override;
    const char* name() const override { return "Create Character"; }
    entt::entity currentEntity() const { return entity; }

private:
    entt::registry* registry;
    Scene* scene;
    std::string entityName;
    glm::vec3 spawnPosition{ 0.0f };
    entt::entity entity = entt::null;
};

// Edits only TerrainComponent::config. Mesh regeneration is a separate,
// explicit command so dragging a slider never uploads a new terrain mesh.
class SetTerrainConfigCommand : public Command
{
public:
    SetTerrainConfigCommand(entt::registry& registry, entt::entity entity,
                            const terrain::TerrainConfig& oldConfig,
                            const terrain::TerrainConfig& newConfig);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Modify Terrain Config"; }
    bool mergeWith(const Command& previous) const override;
    void absorb(const Command& newer) override;

private:
    entt::registry* registry;
    entt::entity entity;
    terrain::TerrainConfig oldConfig;
    terrain::TerrainConfig newConfig;
};

// F3：体素编辑（挖 / 放一格）。一次点击 = 一条命令，撤销时把方块放回去。
// 只记录世界坐标与前后方块 ID，重放走 Scene::setVoxelBlock，chunk 重建由它负责。
class VoxelEditCommand : public Command
{
public:
    VoxelEditCommand(Scene& scene, entt::entity world,
                     int32_t x, int32_t y, int32_t z,
                     uint8_t before, uint8_t after);

    void apply() override;
    void revert() override;
    const char* name() const override
    {
        return after == 0u ? "Dig Voxel" : "Place Voxel";
    }

private:
    Scene* scene;
    entt::entity world;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint8_t before = 0;
    uint8_t after = 0;
};

// Swaps two already-generated terrain states. The old/new MeshComponents hold
// shared_ptr<Model>, so undo history keeps the previous GPU buffers alive.
class RegenerateTerrainCommand : public Command
{
public:
    RegenerateTerrainCommand(entt::registry& registry, Scene& scene,
                             entt::entity entity,
                             const TerrainComponent& oldTerrain,
                             const TerrainComponent& newTerrain,
                             const MeshComponent& oldMesh,
                             const MeshComponent& newMesh,
                             const BoundsComponent& oldBounds,
                             const BoundsComponent& newBounds,
                             const RenderTagComponent& oldTag,
                             const RenderTagComponent& newTag);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Regenerate Terrain"; }

private:
    void restore(const TerrainComponent& terrain,
                 const MeshComponent& mesh,
                 const BoundsComponent& bounds,
                 const RenderTagComponent& tag);

    entt::registry* registry;
    Scene* scene;
    entt::entity entity;

    TerrainComponent oldTerrain;
    TerrainComponent newTerrain;
    MeshComponent oldMesh;
    MeshComponent newMesh;
    BoundsComponent oldBounds;
    BoundsComponent newBounds;
    RenderTagComponent oldTag;
    RenderTagComponent newTag;
};

// E1：把多条命令当成一条（多选删除/多选复制）。
//
// 为什么需要：撤销栈的粒度必须等于"用户的一次操作"。批量删除 5 个实体如果
// 压 5 条命令，用户就要按 5 次 Ctrl+Z 才回到操作前。
class CompositeCommand : public Command
{
public:
    CompositeCommand(std::string name, std::vector<std::unique_ptr<Command>> children);

    void apply() override;
    void revert() override;
    const char* name() const override { return commandName.c_str(); }

private:
    std::string commandName;
    std::vector<std::unique_ptr<Command>> children;
};

// E1：复制一个实体（含子树）。
//
// 只复制"数据型"组件：名字（追加 " Copy"）、局部变换、网格、材质、渲染标记、
// 角色控制器、局部包围盒。地形/流送/相机不复制（复制地形等于复制一整块高度场，
// 复制相机会让场景出现两个相机）。
//
// 撤销 = 销毁这次新建的副本；重做会重新建一遍（新 ID，和 DeleteEntityCommand 的
// 恢复语义一致）。
class DuplicateEntityCommand : public Command
{
public:
    DuplicateEntityCommand(entt::registry& registry, Scene* scene, entt::entity entity);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Duplicate Entity"; }

    // apply() 之后可查：副本的根实体（撤销后失效）。
    entt::entity duplicatedRoot() const { return createdRoot; }

    // 静默检查：地形/流送 chunk 这类实体不允许复制。
    static bool canDuplicate(entt::registry& registry, entt::entity entity);

private:
    entt::registry* registry;
    Scene* scene;
    entt::entity source;
    entt::entity createdRoot = entt::null;
    std::vector<entt::entity> createdEntities;
};

// F5-02：实例化预制体（一棵子树）到当前场景。
//
// 失败语义：CommandStack::execute 没有返回值，所以构造函数里先完成一次实例化；
// 成功才 adopted=true，第一次 apply() 只负责“接管”这次结果。撤销逆序销毁，
// 重做重新实例化（新 ID，和 DuplicateEntityCommand 一致）。
class InstantiatePrefabCommand : public Command
{
public:
    InstantiatePrefabCommand(Scene& scene,
                             const scene::SceneDescription& description,
                             entt::entity parent);

    void apply() override;
    void revert() override;
    const char* name() const override { return "Instantiate Prefab"; }

    bool succeeded() const { return error.empty(); }
    const std::string& errorMessage() const { return error; }
    entt::entity currentRoot() const { return createdRoot; }
    const std::vector<entt::entity>& createdEntities() const { return created; }

private:
    Scene* scene;
    scene::SceneDescription description;
    entt::entity parent;
    entt::entity createdRoot = entt::null;
    std::vector<entt::entity> created;
    std::string error;
    bool adopted = false;
};

// UI helper shared by the Create menu and the Hierarchy button. Returns the
// created entity (or entt::null on failure) after executing the command.
entt::entity createTerrainThroughCommand(entt::registry& registry,
                                         Scene& scene,
                                         CommandStack& commands,
                                         const terrain::TerrainConfig& config,
                                         const std::string& name,
                                         bool deferBuild = false);

// Places the terrain centre in front of the camera, flattened to world Y=0 so
// baseHeight remains the visible ground elevation.
glm::vec3 terrainSpawnPosition(const Scene& scene,
                               const terrain::TerrainConfig& config);

} // namespace editor
