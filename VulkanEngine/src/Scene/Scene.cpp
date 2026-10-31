#include "Scene/Scene.h"
#include "AssetPaths.h"
#include "RHI/Texture.h"
#include "Scene/GLTFLoader.h"
#include <unordered_map>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace
{
    constexpr bool kLoadSponza = true;

    entt::entity createTransformEntity(entt::registry& registry)
    {
        const entt::entity entity = registry.create();
        registry.emplace<TransformComponent>(entity);
        registry.emplace<HierarchyComponent>(entity);
        return entity;
    }

    void setModelBounds(entt::registry& registry, entt::entity entity,
                        const std::shared_ptr<Model>& model)
    {
        if (!model)
            return;

        BoundsComponent& bounds = registry.get_or_emplace<BoundsComponent>(entity);
        bounds.local.min = model->getBoundsMin();
        bounds.local.max = model->getBoundsMax();
        bounds.local.valid = true;
    }

    void updateNodeRecursive(entt::registry& registry, entt::entity entity,
                             const glm::mat4& parentWorld)
    {
        const auto& transform = registry.get<TransformComponent>(entity);
        const glm::mat4 world = parentWorld * transform.localMatrix();

        WorldTransformComponent& worldTransform = registry.get_or_emplace<WorldTransformComponent>(entity);
        worldTransform.matrix = world;
        worldTransform.normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
        worldTransform.worldPosition = glm::vec3(world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

        if (const auto* hierarchy = registry.try_get<HierarchyComponent>(entity))
        {
            for (entt::entity child : hierarchy->children)
            {
                if (registry.valid(child))
                    updateNodeRecursive(registry, child, world);
            }
        }
    }

    void updateAllWorldTransforms(entt::registry& registry)
    {
        for (entt::entity entity : registry.view<TransformComponent>())
        {
            const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
            const bool isRoot = !hierarchy || hierarchy->parent == entt::null ||
                                !registry.valid(hierarchy->parent);
            if (isRoot)
                updateNodeRecursive(registry, entity, glm::mat4{ 1.0f });
        }
    }

    struct Plane
    {
        glm::vec3 normal{ 0.0f };
        float distance = 0.0f;
    };

    struct Frustum
    {
        Plane planes[6];
    };

    Plane makePlane(const glm::vec4& equation)
    {
        Plane plane;
        plane.normal = glm::vec3(equation);
        plane.distance = equation.w;
        const float length = glm::length(plane.normal);
        if (length > 0.00001f)
        {
            plane.normal /= length;
            plane.distance /= length;
        }
        return plane;
    }

    Frustum extractFrustum(const glm::mat4& viewProjection)
    {
        const glm::vec4 row0(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0], viewProjection[3][0]);
        const glm::vec4 row1(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1], viewProjection[3][1]);
        const glm::vec4 row2(viewProjection[0][2], viewProjection[1][2], viewProjection[2][2], viewProjection[3][2]);
        const glm::vec4 row3(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3], viewProjection[3][3]);

        Frustum frustum;
        // Vulkan clip space: -w <= x,y <= w and 0 <= z <= w.
        frustum.planes[0] = makePlane(row3 + row0); // left
        frustum.planes[1] = makePlane(row3 - row0); // right
        frustum.planes[2] = makePlane(row3 + row1); // bottom
        frustum.planes[3] = makePlane(row3 - row1); // top
        frustum.planes[4] = makePlane(row2);        // near
        frustum.planes[5] = makePlane(row3 - row2); // far
        return frustum;
    }

    bool intersectsAABB(const Frustum& frustum, const AABB& bounds)
    {
        if (!bounds.valid)
            return false;

        for (const Plane& plane : frustum.planes)
        {
            const glm::vec3 positive{
                plane.normal.x >= 0.0f ? bounds.max.x : bounds.min.x,
                plane.normal.y >= 0.0f ? bounds.max.y : bounds.min.y,
                plane.normal.z >= 0.0f ? bounds.max.z : bounds.min.z,
            };

            if (glm::dot(plane.normal, positive) + plane.distance < 0.0f)
                return false;
        }
        return true;
    }

    AABB frustumBroadBounds(const glm::mat4& viewProjection)
    {
        const glm::mat4 inverse = glm::inverse(viewProjection);
        AABB bounds;
        bounds.valid = true;

        bool first = true;
        for (int z = 0; z <= 1; ++z)
        {
            for (int y = -1; y <= 1; y += 2)
            {
                for (int x = -1; x <= 1; x += 2)
                {
                    glm::vec4 p = inverse * glm::vec4(static_cast<float>(x), static_cast<float>(y),
                                                      static_cast<float>(z), 1.0f);
                    p /= p.w;
                    const glm::vec3 world(p);
                    if (first)
                    {
                        bounds.min = bounds.max = world;
                        first = false;
                    }
                    else
                    {
                        bounds.min = glm::min(bounds.min, world);
                        bounds.max = glm::max(bounds.max, world);
                    }
                }
            }
        }
        return bounds;
    }

    bool isDescendant(entt::registry& registry, entt::entity candidate, entt::entity ancestor)
    {
        entt::entity current = candidate;
        while (current != entt::null && registry.valid(current))
        {
            if (current == ancestor)
                return true;
            const auto* hierarchy = registry.try_get<HierarchyComponent>(current);
            current = hierarchy ? hierarchy->parent : entt::null;
        }
        return false;
    }
}

void Scene::init(Device& deviceArg, VkCommandPool commandPoolArg, DescriptorManager& descriptorManagerArg,
                 bool loadDefaultContent)
{
    // 编辑器 / 场景重建需要这些依赖（创建网格实体、分配材质槽位）。
    // 存指针而不是让每个调用点都传参，是因为 Scene 已经是"场景所有者"，
    // 它天然应该记得自己是用哪个设备创建出来的。
    device = &deviceArg;
    commandPool = commandPoolArg;
    descriptorManager = &descriptorManagerArg;

    Device& device = deviceArg;
    VkCommandPool commandPool = commandPoolArg;
    DescriptorManager& descriptorManager = descriptorManagerArg;

    const std::string basePath = SENTINEL_RESOURCE_DIR;

    materialSystem.initDefaultTextures(device, commandPool);

    cameraEntity = createTransformEntity(registry);
    registry.emplace<CameraComponent>(cameraEntity);
    // 编辑器层级面板需要一个可读名字。没有 NameComponent 时面板只能显示
    // "Entity 2" 这样的数字 ID，完全看不出哪个是相机、哪个是天空盒。
    registry.emplace<NameComponent>(cameraEntity, NameComponent{ "Camera" });

    // loadDefaultContent=false 时跳过 Sponza / 兜底模型，只保留相机 + 天空盒。
    // 编辑器默认走这条路径：Sponza 整栋是一个实体、包围盒包住整个场景，
    // 会干扰"点选 / 包围盒 / 手柄"这类交互的观察。
    //
    // 天空盒仍然创建 —— 纯黑背景会让视口看起来像"没渲染出来"。
    std::shared_ptr<Model> heroModel;
    if (loadDefaultContent && kLoadSponza)
    {
        sentinel::GLTFLoadContext ctx;
        ctx.device = &device;
        ctx.commandPool = commandPool;
        ctx.materialSystem = &materialSystem;
        ctx.descriptorManager = &descriptorManager;

        heroModel = sentinel::loadGLTFModel(ctx, basePath + "Models/Sponza/glTF/Sponza.gltf");
        if (!heroModel)
            std::cerr << "[Scene] Sponza load failed, falling back to DamagedHelmet\n";
    }

    if (heroModel)
    {
        const entt::entity hero = createTransformEntity(registry);
        registry.emplace<MeshComponent>(hero, MeshComponent{ heroModel });
        registry.emplace<NameComponent>(hero, NameComponent{ "Sponza" });
        registry.emplace<RenderTagComponent>(hero);
        setModelBounds(registry, hero, heroModel);

        const glm::vec3 center = heroModel->getBoundsCenter();
        const float radius = heroModel->getBoundsRadius();
        std::cout << "[Scene] Sponza bounds min(" << heroModel->getBoundsMin().x << ", "
                  << heroModel->getBoundsMin().y << ", " << heroModel->getBoundsMin().z
                  << ") max(" << heroModel->getBoundsMax().x << ", "
                  << heroModel->getBoundsMax().y << ", " << heroModel->getBoundsMax().z
                  << ") center(" << center.x << ", " << center.y << ", " << center.z
                  << ") radius " << radius << "\n";

        // Default establishing shot for Sponza. The camera sits inside the
        // central aisle and looks down its long axis; config/engine.json can
        // still override this pose at runtime.
        TransformComponent& cameraTransform = getCameraTransform();
        cameraTransform.translation = { -4.6485233f, 5.2119632f, 0.0523994f };
        cameraTransform.setEuler({ 0.3507974f, -17.3302765f, 0.0f });
    }
    else if (loadDefaultContent)
    {
        const entt::entity helmet = createTransformEntity(registry);
        auto helmetModel = Model::createModelFromFile(device, commandPool,
            basePath + "Models/DamagedHelmet/DamagedHelmet.obj");
        registry.emplace<MeshComponent>(helmet, MeshComponent{ helmetModel });
        registry.emplace<NameComponent>(helmet, NameComponent{ "DamagedHelmet" });
        registry.emplace<RenderTagComponent>(helmet);
        setModelBounds(registry, helmet, helmetModel);
        getCameraTransform().translation = { 0.0f, 0.0f, -5.0f };
    }

    // Skybox: small sphere; the vertex shader performs depth/w translation.
    {
        const entt::entity skybox = createTransformEntity(registry);
        auto skyboxModel = Model::createModelFromFile(device, commandPool,
            basePath + "Models/sphere.obj");
        registry.emplace<MeshComponent>(skybox, MeshComponent{ skyboxModel });
        registry.emplace<NameComponent>(skybox, NameComponent{ "Skybox" });
        RenderTagComponent tag;
        tag.isSkybox = true;
        registry.emplace<RenderTagComponent>(skybox, tag);
        registry.get<TransformComponent>(skybox).scale = { 0.1f, 0.1f, 0.1f };
        setModelBounds(registry, skybox, skyboxModel);
    }

    updateAllWorldTransforms(registry);
    rebuildSpatialIndex();
}

entt::entity Scene::createEntity()
{
    return createTransformEntity(registry);
}

void Scene::destroyEntity(entt::entity entity)
{
    if (!registry.valid(entity))
        return;

    if (auto* hierarchy = registry.try_get<HierarchyComponent>(entity))
    {
        if (hierarchy->parent != entt::null && registry.valid(hierarchy->parent))
        {
            auto& siblings = registry.get<HierarchyComponent>(hierarchy->parent).children;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
        }

        for (entt::entity child : hierarchy->children)
        {
            if (registry.valid(child))
            {
                auto* childHierarchy = registry.try_get<HierarchyComponent>(child);
                if (childHierarchy)
                    childHierarchy->parent = entt::null;
            }
        }
    }

    registry.destroy(entity);
}

void Scene::setParent(entt::entity child, entt::entity parent)
{
    if (!registry.valid(child))
        throw std::runtime_error("Scene::setParent: invalid child entity");
    if (parent != entt::null && !registry.valid(parent))
        throw std::runtime_error("Scene::setParent: invalid parent entity");
    if (child == parent)
        throw std::runtime_error("Scene::setParent: entity cannot parent itself");
    if (parent != entt::null && isDescendant(registry, parent, child))
        throw std::runtime_error("Scene::setParent: cycle detected");

    auto& childHierarchy = registry.get_or_emplace<HierarchyComponent>(child);
    if (childHierarchy.parent != entt::null && registry.valid(childHierarchy.parent))
    {
        auto& oldChildren = registry.get<HierarchyComponent>(childHierarchy.parent).children;
        oldChildren.erase(std::remove(oldChildren.begin(), oldChildren.end(), child), oldChildren.end());
    }

    childHierarchy.parent = parent;
    if (parent != entt::null)
        registry.get_or_emplace<HierarchyComponent>(parent).children.push_back(child);
}

void Scene::update(float dt, GLFWwindow* window, float aspectRatio, bool allowCameraInput)
{
    TransformComponent& cameraTransform = getCameraTransform();
    cameraController.inputEnabled = allowCameraInput;
    cameraController.processPCInput(window, dt, cameraTransform);

    camera.setViewYXZ(cameraTransform.translation, cameraTransform.rotation);
    camera.setPerspectiveProjection(glm::radians(60.0f), aspectRatio, 0.1f, 5000.0f);

    updateWorldTransforms();
    rebuildSpatialIndex();
    updateVisibility(camera.getProjection() * camera.getView());
}

void Scene::updateWorldTransforms()
{
    updateAllWorldTransforms(registry);
}

void Scene::rebuildSpatialIndex()
{
    spatialIndex.clear();

    for (entt::entity entity : registry.view<MeshComponent>())
    {
        const MeshComponent& mesh = registry.get<MeshComponent>(entity);
        if (!mesh.model)
            continue;

        // Skybox is camera-following and always rendered. Its 1000-unit source
        // sphere would otherwise insert a huge number of grid cells every frame.
        const auto* tag = registry.try_get<RenderTagComponent>(entity);
        if (tag && tag->isSkybox)
            continue;

        auto& bounds = registry.get_or_emplace<BoundsComponent>(entity);
        if (!bounds.local.valid)
        {
            bounds.local.min = mesh.model->getBoundsMin();
            bounds.local.max = mesh.model->getBoundsMax();
            bounds.local.valid = true;
        }

        const WorldTransformComponent& world = registry.get_or_emplace<WorldTransformComponent>(entity);
        WorldBoundsComponent& worldBounds = registry.get_or_emplace<WorldBoundsComponent>(entity);
        worldBounds.world = transformAABB(bounds.local, world.matrix);
        spatialIndex.insert(entity, worldBounds.world);
    }
}

void Scene::updateVisibility(const glm::mat4& viewProjection)
{
    visibleEntities.clear();

    const Frustum frustum = extractFrustum(viewProjection);
    const AABB broadBounds = frustumBroadBounds(viewProjection);

    for (auto [entity, mesh, tag] : registry.view<MeshComponent, RenderTagComponent>().each())
    {
        if (tag.isSkybox)
            visibleEntities.push_back(entity);
    }

    std::vector<entt::entity> candidates;
    spatialIndex.query(broadBounds, candidates);

    for (entt::entity entity : candidates)
    {
        const auto* tag = registry.try_get<RenderTagComponent>(entity);
        const auto* worldBounds = registry.try_get<WorldBoundsComponent>(entity);
        if (!worldBounds)
            continue;

        // Skybox is camera-following and always needed as the background.
        if ((tag && tag->isSkybox) || intersectsAABB(frustum, worldBounds->world))
            visibleEntities.push_back(entity);
    }
}

void Scene::cleanup()
{
    materialSystem.cleanup();
    registry.clear();
    spatialIndex.clear();
    visibleEntities.clear();
    cameraEntity = entt::null;
}

bool Scene::selfTest()
{
    bool ok = true;

    entt::registry registry;
    const entt::entity parent = createTransformEntity(registry);
    const entt::entity child = createTransformEntity(registry);
    const entt::entity grandchild = createTransformEntity(registry);

    registry.get<TransformComponent>(parent).translation = { 1.0f, 2.0f, 3.0f };
    registry.get<TransformComponent>(child).translation = { 0.0f, 1.0f, 0.0f };
    registry.get<TransformComponent>(grandchild).translation = { 0.0f, 0.0f, 2.0f };

    registry.get<HierarchyComponent>(parent).children.push_back(child);
    registry.get<HierarchyComponent>(child).parent = parent;
    registry.get<HierarchyComponent>(child).children.push_back(grandchild);
    registry.get<HierarchyComponent>(grandchild).parent = child;

    updateAllWorldTransforms(registry);

    const glm::vec3 childPos = registry.get<WorldTransformComponent>(child).worldPosition;
    const glm::vec3 grandchildPos = registry.get<WorldTransformComponent>(grandchild).worldPosition;
    if (glm::length(childPos - glm::vec3(1.0f, 3.0f, 3.0f)) > 0.0001f ||
        glm::length(grandchildPos - glm::vec3(1.0f, 3.0f, 5.0f)) > 0.0001f)
    {
        std::cerr << "[Scene] selfTest FAIL: hierarchy world transform mismatch\n";
        ok = false;
    }

    SpatialIndex index(4.0f);
    const entt::entity nearEntity = registry.create();
    const entt::entity farEntity = registry.create();
    index.insert(nearEntity, AABB{ { 0.0f, 0.0f, 0.0f }, { 3.0f, 3.0f, 3.0f }, true });
    index.insert(farEntity, AABB{ { 100.0f, 100.0f, 100.0f }, { 104.0f, 104.0f, 104.0f }, true });

    std::vector<entt::entity> result;
    index.query(AABB{ { 1.0f, 1.0f, 1.0f }, { 2.0f, 2.0f, 2.0f }, true }, result);
    if (result.size() != 1 || result[0] != nearEntity)
    {
        std::cerr << "[Scene] selfTest FAIL: spatial index query mismatch\n";
        ok = false;
    }

    // 旋转现在以四元数为准，欧拉角只是显示值。这里钉住三件事：
    //   1. setEuler -> orientation -> eulerFromQuat 往返一致（非万向节区）；
    //   2. 四元数矩阵与旧的 T*Ry*Rx*Rz*S 完全等价（不能悄悄改变旋转约定）；
    //   3. 万向节附近 orientation 仍然有效，矩阵不是 NaN。
    {
        TransformComponent rotationTest;
        const glm::vec3 euler{ 0.31f, -0.72f, 1.13f };
        rotationTest.setEuler(euler);
        const glm::vec3 roundTrip = TransformComponent::eulerFromQuat(rotationTest.orientation);
        if (glm::length(roundTrip - euler) > 1e-4f)
        {
            std::cerr << "[Scene] selfTest FAIL: quaternion/euler round trip mismatch\n";
            ok = false;
        }

        glm::mat4 legacy = glm::translate(glm::mat4{ 1.0f }, rotationTest.translation);
        legacy = glm::rotate(legacy, euler.y, glm::vec3(0.0f, 1.0f, 0.0f));
        legacy = glm::rotate(legacy, euler.x, glm::vec3(1.0f, 0.0f, 0.0f));
        legacy = glm::rotate(legacy, euler.z, glm::vec3(0.0f, 0.0f, 1.0f));
        legacy = glm::scale(legacy, rotationTest.scale);
        const glm::mat4 fromQuat = rotationTest.localMatrix();
        float maxDiff = 0.0f;
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                maxDiff = std::max(maxDiff, std::abs(legacy[column][row] - fromQuat[column][row]));
        if (maxDiff > 1e-5f)
        {
            std::cerr << "[Scene] selfTest FAIL: quaternion matrix differs from legacy Euler order\n";
            ok = false;
        }

        // pitch 到 90° 是欧拉角的奇异点；四元数必须仍然能生成有限矩阵。
        TransformComponent gimbalTest;
        gimbalTest.setOrientation(glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)) *
                                  TransformComponent::quatFromEuler({ 0.2f, 0.3f, 0.4f }));
        const glm::mat4 gimbalMatrix = gimbalTest.localMatrix();
        bool finite = true;
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                finite = finite && std::isfinite(gimbalMatrix[column][row]);
        if (!finite)
        {
            std::cerr << "[Scene] selfTest FAIL: gimbal-lock orientation produced NaN\n";
            ok = false;
        }

        // 单轴旋转反解只能落在对应分量上；拖 X 手柄时 Inspector 不应该同时跳 Y/Z。
        const glm::vec3 axes[3] = {
            { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }
        };
        for (int axis = 0; axis < 3; ++axis)
        {
            const glm::vec3 e = TransformComponent::eulerFromQuat(
                glm::angleAxis(0.4f, axes[axis]));
            glm::vec3 expected(0.0f);
            expected[axis] = 0.4f;
            if (glm::length(e - expected) > 1e-4f)
            {
                std::cerr << "[Scene] selfTest FAIL: single-axis euler extraction mismatch\n";
                ok = false;
            }
        }
    }

    std::cout << "[Scene] ECS/spatial selfTest " << (ok ? "PASS" : "FAIL")
              << " | hierarchy=T*Ry*Rx*Rz*S | grid cells=" << index.cellCount() << "\n";
    return ok;
}

// ---------------------------------------------------------------------------
// 场景描述：导出 / 重建（编辑器与序列化共用）
// ---------------------------------------------------------------------------
scene::SceneDescription Scene::captureDescription() const
{
    scene::SceneDescription description;
    description.version = 1;

    // 建立 "实体 -> 下标" 映射。父子关系用下标表示，因为实体 ID 跨进程不稳定。
    std::unordered_map<uint32_t, int> idToIndex;
    int nextIndex = 0;
    for (entt::entity entity : registry.view<TransformComponent>())
        idToIndex[static_cast<uint32_t>(entity)] = nextIndex++;

    description.entities.reserve(idToIndex.size());

    for (entt::entity entity : registry.view<TransformComponent>())
    {
        scene::EntityDescription e;
        e.name = registry.try_get<NameComponent>(entity)
            ? registry.get<NameComponent>(entity).name
            : std::string("Entity");

        const TransformComponent& transform = registry.get<TransformComponent>(entity);
        e.translation = transform.translation;
        e.rotation = transform.rotation;
        e.scale = transform.scale;

        if (const auto* hierarchy = registry.try_get<HierarchyComponent>(entity))
        {
            if (hierarchy->parent != entt::null)
            {
                const auto it = idToIndex.find(static_cast<uint32_t>(hierarchy->parent));
                if (it != idToIndex.end())
                    e.parentIndex = it->second;
            }
        }

        if (const auto* mesh = registry.try_get<MeshComponent>(entity))
        {
            if (mesh->model)
                e.meshPath = mesh->model->getSourcePath();
        }

        if (const auto* tag = registry.try_get<RenderTagComponent>(entity))
        {
            e.isSkybox = tag->isSkybox;
            e.isGrid = tag->isGrid;
        }
        // CameraComponent 是空 tag 组件，用 any_of 判断存在性。
        e.isCamera = registry.any_of<CameraComponent>(entity);

        if (const auto* materialComp = registry.try_get<MaterialComponent>(entity))
        {
            if (materialComp->material)
            {
                const Material& m = *materialComp->material;
                e.material.present = true;
                e.material.name = m.name;
                e.material.baseColorFactor = m.baseColorFactor;
                e.material.metallicFactor = m.metallicFactor;
                e.material.roughnessFactor = m.roughnessFactor;
                e.material.alphaCutoff = m.alphaCutoff;
                e.material.alphaMode = static_cast<int>(m.alphaMode);
                if (m.albedoMap)            e.material.albedoMap = m.albedoMap->getPath();
                if (m.normalMap)            e.material.normalMap = m.normalMap->getPath();
                if (m.metallicRoughnessMap) e.material.metallicRoughnessMap = m.metallicRoughnessMap->getPath();
            }
        }

        // 注意：这里用 push_back，顺序必须与我上面 enum 的顺序一致。
        // 因为 idToIndex 也是按同一个 view 顺序建的，所以两边一致。
        description.entities.push_back(std::move(e));
    }

    return description;
}

void Scene::clearEntities()
{
    // 先归还材质槽位（延迟回收，不阻塞设备），再销毁实体。
    // 不归还的话，反复"新建场景"会慢慢吃满 256 个 bindless 槽位。
    for (entt::entity entity : registry.view<MaterialComponent>())
    {
        auto& materialComp = registry.get<MaterialComponent>(entity);
        if (materialComp.materialSetIndex != kInvalidMaterialSlot)
        {
            descriptorManager->releaseMaterialSet(materialComp.materialSetIndex);
            materialComp.materialSetIndex = kInvalidMaterialSlot;
        }
    }

    const entt::entity keepCamera = cameraEntity;

    std::vector<entt::entity> all;
    for (entt::entity entity : registry.view<TransformComponent>())
        all.push_back(entity);

    for (entt::entity entity : all)
    {
        if (entity == keepCamera)
            continue;
        if (registry.valid(entity))
            registry.destroy(entity);
    }

    // 相机实体本身也可能带有子节点，重新挂成根
    if (registry.valid(keepCamera))
    {
        auto& hierarchy = registry.get_or_emplace<HierarchyComponent>(keepCamera);
        hierarchy.parent = entt::null;
        hierarchy.children.clear();
    }

    visibleEntities.clear();
    updateWorldTransforms();
    rebuildSpatialIndex();
}

bool Scene::buildFromDescription(const scene::SceneDescription& description, std::string& errorMessage)
{
    if (!device || !descriptorManager)
    {
        errorMessage = "scene is not initialized";
        return false;
    }

    clearEntities();

    // 注意：不要写成 vector<entt::entity>(n, entt::null)。
    // MSVC 在 vector 的填充构造里会拿 allocator<entity> 去实例化 entt_traits，
    // 报 "entity_type 不是 std::allocator<Entity> 的成员"。
    // 先把 entt::null 转成具名 entt::entity，转换就发生在非模板推导上下文里。
    const entt::entity nullEntity = entt::null;
    std::vector<entt::entity> created(description.entities.size(), nullEntity);
    bool foundCamera = false;

    for (size_t i = 0; i < description.entities.size(); ++i)
    {
        const scene::EntityDescription& e = description.entities[i];

        const entt::entity entity = registry.create();
        created[i] = entity;

        TransformComponent transform;
        transform.translation = e.translation;
        transform.setEuler(e.rotation);
        transform.scale = e.scale;
        registry.emplace<TransformComponent>(entity, transform);
        registry.emplace<HierarchyComponent>(entity);
        registry.emplace<NameComponent>(entity, NameComponent{ e.name });

        if (e.isCamera && !foundCamera)
        {
            // 复用已有的相机实体，而不是新建 —— Scene 里 cameraEntity 是唯一句柄
            registry.destroy(entity);
            created[i] = cameraEntity;
            registry.get<NameComponent>(cameraEntity).name = e.name;
            registry.get<TransformComponent>(cameraEntity) = transform;
            foundCamera = true;
        }

        if (!e.meshPath.empty())
        {
            const entt::entity target = created[i];
            std::shared_ptr<Model> model;
            try
            {
                model = Model::createModelFromFile(*device, commandPool, e.meshPath);
            }
            catch (const std::exception& ex)
            {
                errorMessage = std::string("failed to load mesh '") + e.meshPath + "': " + ex.what();
                return false;
            }
            if (model)
            {
                registry.emplace<MeshComponent>(target, MeshComponent{ model });
                RenderTagComponent tag;
                tag.isSkybox = e.isSkybox;
                tag.isGrid = e.isGrid;
                registry.emplace<RenderTagComponent>(target, tag);
                setModelBounds(registry, target, model);
            }
        }
        else if (e.isSkybox || e.isGrid)
        {
            RenderTagComponent tag;
            tag.isSkybox = e.isSkybox;
            tag.isGrid = e.isGrid;
            registry.emplace<RenderTagComponent>(created[i], tag);
        }

        // 材质：默认用 defaultMat（MaterialSystem 已建好的），有描述则覆盖参数
        if (created[i] != entt::null && registry.valid(created[i]))
        {
            if (registry.try_get<MeshComponent>(created[i]))
            {
                MaterialComponent& materialComp = registry.get_or_emplace<MaterialComponent>(created[i]);
                materialComp.material = materialSystem.getOrCreateMaterial(
                    e.material.present ? e.material.name : std::string("scene_default"));
                materialComp.material->baseColorFactor = e.material.baseColorFactor;
                materialComp.material->metallicFactor = e.material.metallicFactor;
                materialComp.material->roughnessFactor = e.material.roughnessFactor;
                materialComp.material->alphaCutoff = e.material.alphaCutoff;
                materialComp.material->alphaMode = static_cast<AlphaMode>(e.material.alphaMode);

                // 贴图按需加载（走 MaterialSystem 的缓存）
                auto loadIfAny = [&](const std::string& path, bool srgb) -> std::shared_ptr<Texture>
                {
                    if (path.empty()) return nullptr;
                    return materialSystem.loadTexture(path, *device, commandPool, srgb);
                };
                if (!e.material.albedoMap.empty())
                    materialComp.material->albedoMap = loadIfAny(e.material.albedoMap, true);
                if (!e.material.normalMap.empty())
                    materialComp.material->normalMap = loadIfAny(e.material.normalMap, false);
                if (!e.material.metallicRoughnessMap.empty())
                    materialComp.material->metallicRoughnessMap = loadIfAny(e.material.metallicRoughnessMap, false);

                // 重新分配 bindless 槽位（序列化不存槽位号，因为它是运行期分发的）。
                // 槽位归实体所有，和 createMeshEntity 保持同一套规则。
                materialComp.materialSetIndex = descriptorManager->allocateMaterialSet(*materialComp.material);
            }
        }
    }

    // 第二遍：挂父子关系（父必须先存在，所以不能和创建放在一个循环里）
    for (size_t i = 0; i < description.entities.size(); ++i)
    {
        const int parentIndex = description.entities[i].parentIndex;
        if (parentIndex < 0 || parentIndex >= static_cast<int>(created.size()))
            continue;

        // 相机实体被复用时 created[i] 指向它，同样要参与挂接
        const entt::entity child = created[i];
        const entt::entity parent = created[parentIndex];
        if (child == entt::null || parent == entt::null)
            continue;
        if (!registry.valid(child) || !registry.valid(parent))
            continue;
        if (child == parent)
            continue;

        registry.get<HierarchyComponent>(child).parent = parent;
        registry.get<HierarchyComponent>(parent).children.push_back(child);
    }

    refreshTransformsAndVisibility();
    return true;
}

entt::entity Scene::createMeshEntity(const std::string& absolutePath, const std::string& name)
{
    if (!device || !descriptorManager)
        return entt::null;

    std::shared_ptr<Model> model;
    try
    {
        model = Model::createModelFromFile(*device, commandPool, absolutePath);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[Scene] createMeshEntity failed: " << ex.what() << "\n";
        return entt::null;
    }
    if (!model)
        return entt::null;

    const entt::entity entity = createTransformEntity(registry);
    registry.emplace<MeshComponent>(entity, MeshComponent{ model });
    registry.emplace<RenderTagComponent>(entity);
    registry.emplace<NameComponent>(entity, NameComponent{ name });
    setModelBounds(registry, entity, model);

    MaterialComponent& materialComp = registry.get_or_emplace<MaterialComponent>(entity);
    materialComp.material = materialSystem.getOrCreateMaterial(name + "_mat");

    // 实体级槽位归 MaterialComponent 所有：每个实体一个槽位，clearEntities()
    // 按实体归还。不要写 Material::descriptorSetIndex —— 那是材质对象的字段，
    // 同名的多个实体会共享同一个 Material，但各自持有独立的槽位。
    materialComp.materialSetIndex = descriptorManager->allocateMaterialSet(*materialComp.material);

    updateWorldTransforms();
    rebuildSpatialIndex();
    return entity;
}

void Scene::refreshTransformsAndVisibility()
{
    updateWorldTransforms();
    rebuildSpatialIndex();
    updateVisibility(camera.getProjection() * camera.getView());
}
