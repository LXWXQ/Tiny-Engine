// ============================================================================
// tinygltf 集成翻译单元
//
// tinygltf 是 header-only 的，实现必须、且只能在一个 .cpp 里展开。
// 单独放一个文件是为了不让它污染其他文件的编译时间。
//
// TINYGLTF_NO_STB_IMAGE / TINYGLTF_NO_STB_IMAGE_WRITE 由 CMake 统一注入，
// 原因见 GLTFLoader.h 的说明。
// ============================================================================

// 注意包含顺序 —— 这是 tinygltf 的一个坑：
// tiny_gltf.h 的「声明部分」在 include guard 内（L37~L1639），
// 而「实现部分」#if defined(TINYGLTF_IMPLEMENTATION) 在 include guard 之外（L1641~L8744）。
//
// 因此必须：先包含一次拿声明（此时宏还没定义），再定义宏包含一次 —— 实现恰好展开一次。
// 如果反过来（先定义宏再包含），实现会被展开两次，直接重定义编译失败。
#include "Scene/GLTFLoader.h"

#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#include <stb_image.h>
#include <cstring>

namespace tiny
{

bool loadImageData(tinygltf::Image* image, int image_idx,
    std::string* err, std::string* warn,
    int req_width, int req_height,
    const unsigned char* bytes, int size, void* user_data)
{
    (void)image_idx; (void)warn; (void)req_width; (void)req_height; (void)user_data;

    if (bytes == nullptr || size <= 0)
    {
        if (err) *err = "glTF 图片数据为空";
        return false;
    }

    // 统一解成 RGBA8。glTF 规范只允许 PNG / JPEG，不涉及 HDR，所以这里够用。
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes, size, &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr)
    {
        if (err) *err = std::string("stb_image 解码失败: ") + stbi_failure_reason();
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    image->width = width;
    image->height = height;
    image->component = 4;
    image->image.resize(pixelCount);
    std::memcpy(image->image.data(), pixels, pixelCount);

    stbi_image_free(pixels);
    return true;
}

void setupGLTFLoader(tinygltf::TinyGLTF& loader)
{
    loader.SetImageLoader(&loadImageData, nullptr);
}

} // namespace tiny
// ============================================================================
// glTF 2.0 加载实现
//
// 设计要点：
//   * 遍历节点时累积变换，几何直接烘焙到世界空间
//     （当前渲染路径没有逐物体变换的需求，SubMesh 也不携带节点信息）
//   * 材质解析成 DescriptorManager 的槽位，逐个子网格写进 SubMesh::materialSlot，
//     于是 GeometrySystem 无需区分 OBJ / glTF
//   * 贴图严格按「语义」选色彩空间：颜色贴图 sRGB，数据贴图 UNORM
// ============================================================================

#include "Scene/Model.h"
#include "Core/MaterialSystem.h"
#include "Core/DescriptorManager.h"
#include "Core/Material.h"
#include "RHI/Texture.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <iostream>

namespace tiny
{
namespace
{
    // glTF accessor 的「视图」：基址 + 步长 + 元素个数
    struct AccessorView
    {
        const unsigned char* base = nullptr;
        size_t stride = 0;
        size_t count = 0;
    };

    AccessorView makeAccessorView(const tinygltf::Model& model, int accessorIndex, size_t elementSize)
    {
        AccessorView view{};
        if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
            return view;

        const tinygltf::Accessor& acc = model.accessors[accessorIndex];
        if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(model.bufferViews.size()))
            return view;

        const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
        if (bv.buffer < 0 || bv.buffer >= static_cast<int>(model.buffers.size()))
            return view;

        const tinygltf::Buffer& buf = model.buffers[bv.buffer];
        if (buf.data.empty())
            return view;

        view.base = buf.data.data() + bv.byteOffset + acc.byteOffset;
        // byteStride 为 0 表示元素紧密排列
        view.stride = bv.byteStride != 0 ? bv.byteStride : elementSize;
        view.count = acc.count;
        return view;
    }

    glm::vec2 readVec2(const AccessorView& v, size_t i)
    {
        const float* p = reinterpret_cast<const float*>(v.base + i * v.stride);
        return glm::vec2(p[0], p[1]);
    }

    glm::vec3 readVec3(const AccessorView& v, size_t i)
    {
        const float* p = reinterpret_cast<const float*>(v.base + i * v.stride);
        return glm::vec3(p[0], p[1], p[2]);
    }

    uint32_t readIndex(const tinygltf::Model& model, int accessorIndex, size_t i)
    {
        const tinygltf::Accessor& acc = model.accessors[accessorIndex];
        const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
        const tinygltf::Buffer& buf = model.buffers[bv.buffer];

        size_t componentSize = 4;
        if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) componentSize = 2;
        else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) componentSize = 1;

        const size_t stride = bv.byteStride != 0 ? bv.byteStride : componentSize;
        const unsigned char* p = buf.data.data() + bv.byteOffset + acc.byteOffset + i * stride;

        switch (acc.componentType)
        {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   return *reinterpret_cast<const uint32_t*>(p);
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return *reinterpret_cast<const uint16_t*>(p);
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return *reinterpret_cast<const uint8_t*>(p);
        default: return 0;
        }
    }
} // anonymous namespace

std::shared_ptr<Model> loadGLTFModel(const GLTFLoadContext& ctx, const std::string& filepath)
{
    if (!ctx.isValid())
    {
        std::cerr << "[GLTF] 上下文无效：device / materialSystem / descriptorManager 有空指针\n";
        return nullptr;
    }

    tinygltf::TinyGLTF loader;
    setupGLTFLoader(loader);

    tinygltf::Model gltf;
    std::string err, warn;

    const bool isBinary = filepath.size() >= 4 &&
        filepath.compare(filepath.size() - 4, 4, ".glb") == 0;

    const bool ok = isBinary
        ? loader.LoadBinaryFromFile(&gltf, &err, &warn, filepath)
        : loader.LoadASCIIFromFile(&gltf, &err, &warn, filepath);

    if (!warn.empty())
        std::cout << "[GLTF] 警告: " << warn << "\n";
    if (!ok)
    {
        std::cerr << "[GLTF] 加载失败: " << filepath << "\n        " << err << "\n";
        return nullptr;
    }

    std::cout << "[GLTF] " << filepath << "\n"
              << "       网格 " << gltf.meshes.size()
              << "  节点 " << gltf.nodes.size()
              << "  材质 " << gltf.materials.size()
              << "  贴图 " << gltf.textures.size()
              << "  图片 " << gltf.images.size() << "\n";

    // ---------------- 贴图 ----------------
    // 按 (imageIndex, 是否 sRGB) 缓存：同一张图可能既当颜色用又当数据用
    std::vector<std::shared_ptr<Texture>> texSRGB(gltf.images.size());
    std::vector<std::shared_ptr<Texture>> texLinear(gltf.images.size());

    auto fetchTexture = [&](int textureIndex, bool srgb) -> std::shared_ptr<Texture>
    {
        if (textureIndex < 0 || textureIndex >= static_cast<int>(gltf.textures.size()))
            return nullptr;

        const int imageIndex = gltf.textures[textureIndex].source;
        if (imageIndex < 0 || imageIndex >= static_cast<int>(gltf.images.size()))
            return nullptr;

        std::shared_ptr<Texture>& slot = srgb ? texSRGB[imageIndex] : texLinear[imageIndex];
        if (slot) return slot;

        const tinygltf::Image& img = gltf.images[imageIndex];
        if (img.image.empty() || img.width <= 0 || img.height <= 0)
        {
            std::cerr << "[GLTF] 图片 " << imageIndex << " 没有解码数据，跳过\n";
            return nullptr;
        }

        const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        slot = std::make_shared<Texture>(*ctx.device, img.image.data(),
            static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height),
            format, ctx.commandPool, false);
        return slot;
    };

    // ---------------- 材质 ----------------
    std::vector<uint32_t> materialSlots(gltf.materials.size(), kInvalidMaterialSlot);
    std::vector<uint8_t> materialAlphaMasked(gltf.materials.size(), 0);
    std::vector<float> materialAlphaCutoff(gltf.materials.size(), 0.5f);
    std::vector<uint8_t> materialAlbedoFallback(gltf.materials.size(), 0);

    for (size_t i = 0; i < gltf.materials.size(); ++i)
    {
        const tinygltf::Material& src = gltf.materials[i];

        // 材质必须交给 MaterialSystem 托管，绝不能用局部 make_shared：
        // 否则函数返回后 Material 立即析构，它持有的 shared_ptr<Texture> 一并释放，
        // VkImage / VkImageView / VkSampler 随之销毁，而描述符集里还留着旧句柄 ——
        // 验证层会报 "imageView ... is invalid or has been destroyed"。
        const std::string matKey = filepath + "#material[" + std::to_string(i) + "]";
        auto mat = ctx.materialSystem->getOrCreateMaterial(matKey);
        mat->name = src.name.empty() ? matKey : (src.name + "  @" + filepath);

        const tinygltf::PbrMetallicRoughness& pbr = src.pbrMetallicRoughness;
        mat->baseColorFactor = glm::vec4(
            static_cast<float>(pbr.baseColorFactor[0]),
            static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]),
            static_cast<float>(pbr.baseColorFactor[3]));
        mat->metallicFactor  = static_cast<float>(pbr.metallicFactor);
        mat->roughnessFactor = static_cast<float>(pbr.roughnessFactor);
        mat->alphaCutoff     = static_cast<float>(src.alphaCutoff);

        if (src.alphaMode == "MASK")       mat->alphaMode = AlphaMode::MASK_MODE;
        else if (src.alphaMode == "BLEND") mat->alphaMode = AlphaMode::BLEND_MODE;
        else                               mat->alphaMode = AlphaMode::OPAQUE_MODE;

        // 贴图按语义选色彩空间：
        //   baseColor / emissive 是「颜色」 - sRGB（硬件采样时转线性）
        //   normal / metallicRoughness / occlusion 是「数据」 - UNORM，绝不能被转换
        mat->albedoMap            = fetchTexture(pbr.baseColorTexture.index, true);
        mat->emissiveMap          = fetchTexture(src.emissiveTexture.index, true);
        mat->normalMap            = fetchTexture(src.normalTexture.index, false);
        mat->metallicRoughnessMap = fetchTexture(pbr.metallicRoughnessTexture.index, false);
        mat->aoMap                = fetchTexture(src.occlusionTexture.index, false);

        ctx.materialSystem->buildMaterialDescriptor(mat, *ctx.descriptorManager);
        materialSlots[i] = mat->descriptorSetIndex;
        materialAlphaMasked[i] = (mat->alphaMode == AlphaMode::MASK_MODE) ? 1u : 0u;
        materialAlphaCutoff[i] = mat->alphaCutoff;
        // 没有 baseColorTexture 的 glTF 材质也会采样到默认白纹理；标记出来，
        // 让 GeometrySystem 对"没有纹理的网格"统一走棋盘格占位着色。
        materialAlbedoFallback[i] = mat->albedoMap ? 0u : 1u;
    }

    // ---------------- 几何 ----------------
    Model::Builder builder;
    size_t missingNormals = 0;
    size_t skippedPrimitives = 0;

    std::function<void(int, const glm::mat4&)> processNode =
        [&](int nodeIndex, const glm::mat4& parentMatrix)
    {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(gltf.nodes.size()))
            return;

        const tinygltf::Node& node = gltf.nodes[nodeIndex];

        glm::mat4 local(1.0f);
        if (node.matrix.size() == 16)
        {
            // glTF 的 matrix 是列主序，GLM 的 mat4 也是列主序，逐元素照抄
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    local[c][r] = static_cast<float>(node.matrix[c * 4 + r]);
        }
        else
        {
            glm::vec3 translation(0.0f);
            glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
            glm::vec3 scale(1.0f);

            if (node.translation.size() == 3)
                translation = glm::vec3(static_cast<float>(node.translation[0]),
                                        static_cast<float>(node.translation[1]),
                                        static_cast<float>(node.translation[2]));

            if (node.rotation.size() == 4)
                // glTF 的 quaternion 分量顺序是 (x, y, z, w)；GLM 构造函数是 (w, x, y, z)
                rotation = glm::quat(static_cast<float>(node.rotation[3]),
                                     static_cast<float>(node.rotation[0]),
                                     static_cast<float>(node.rotation[1]),
                                     static_cast<float>(node.rotation[2]));

            if (node.scale.size() == 3)
                scale = glm::vec3(static_cast<float>(node.scale[0]),
                                  static_cast<float>(node.scale[1]),
                                  static_cast<float>(node.scale[2]));

            local = glm::translate(glm::mat4(1.0f), translation)
                  * glm::mat4_cast(rotation)
                  * glm::scale(glm::mat4(1.0f), scale);
        }

        const glm::mat4 world = parentMatrix * local;

        if (node.mesh >= 0 && node.mesh < static_cast<int>(gltf.meshes.size()))
        {
            const tinygltf::Mesh& mesh = gltf.meshes[node.mesh];
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));

            for (const tinygltf::Primitive& prim : mesh.primitives)
            {
                if (prim.mode != TINYGLTF_MODE_TRIANGLES)
                {
                    ++skippedPrimitives;
                    continue;
                }

                auto posIt = prim.attributes.find("POSITION");
                if (posIt == prim.attributes.end())
                {
                    ++skippedPrimitives;
                    continue;
                }

                const AccessorView posView = makeAccessorView(gltf, posIt->second, sizeof(float) * 3);
                if (posView.base == nullptr || posView.count == 0)
                {
                    ++skippedPrimitives;
                    continue;
                }

                AccessorView normView{};
                auto nIt = prim.attributes.find("NORMAL");
                if (nIt != prim.attributes.end())
                    normView = makeAccessorView(gltf, nIt->second, sizeof(float) * 3);

                AccessorView uvView{};
                auto uIt = prim.attributes.find("TEXCOORD_0");
                if (uIt != prim.attributes.end())
                    uvView = makeAccessorView(gltf, uIt->second, sizeof(float) * 2);

                const uint32_t firstVertex = static_cast<uint32_t>(builder.vertices.size());
                const uint32_t firstIndex  = static_cast<uint32_t>(builder.indices.size());

                for (size_t v = 0; v < posView.count; ++v)
                {
                    Vertex vertex{};

                    const glm::vec3 localPos = readVec3(posView, v);
                    vertex.position = glm::vec3(world * glm::vec4(localPos, 1.0f));

                    if (normView.base != nullptr)
                        vertex.normal = glm::normalize(normalMatrix * readVec3(normView, v));
                    else
                    {
                        // 零法线：片元着色器会用屏幕导数重建几何法线。
                        // 不要伪造成 (0,1,0)，否则占位棋盘格会在侧面退化成条纹。
                        vertex.normal = glm::vec3(0.0f);
                        ++missingNormals;
                    }

                    // 关键：glTF 的 UV 原点在「左上」，与 Vulkan 一致，
                    // 所以这里「不能」像 OBJ 路径那样做 1.0 - v 翻转，否则贴图会上下颠倒。
                    vertex.uv = (uvView.base != nullptr) ? readVec2(uvView, v) : glm::vec2(0.0f);

                    // glTF 顶点格式没有顶点色，统一填白
                    vertex.color = glm::vec3(1.0f);

                    builder.vertices.push_back(vertex);
                }

                if (prim.indices >= 0 && prim.indices < static_cast<int>(gltf.accessors.size()))
                {
                    const size_t indexCount = gltf.accessors[prim.indices].count;
                    for (size_t k = 0; k < indexCount; ++k)
                        builder.indices.push_back(firstVertex + readIndex(gltf, prim.indices, k));
                }
                else
                {
                    for (uint32_t k = 0; k < static_cast<uint32_t>(posView.count); ++k)
                        builder.indices.push_back(firstVertex + k);
                }

                SubMesh sub{};
                sub.firstIndex = firstIndex;
                sub.indexCount = static_cast<uint32_t>(builder.indices.size()) - firstIndex;
                const bool hasMaterial = prim.material >= 0 &&
                    prim.material < static_cast<int>(materialSlots.size());
                sub.materialSlot = hasMaterial ? materialSlots[prim.material] : kInvalidMaterialSlot;
                if (hasMaterial)
                {
                    sub.alphaMasked = materialAlphaMasked[prim.material] != 0;
                    sub.alphaCutoff = materialAlphaCutoff[prim.material];
                    sub.albedoFallback = materialAlbedoFallback[prim.material] != 0;
                }

                if (sub.indexCount > 0)
                    builder.subMeshes.push_back(sub);
            }
        }

        for (int child : node.children)
            processNode(child, world);
    };

    if (!gltf.scenes.empty())
    {
        int sceneIndex = gltf.defaultScene;
        if (sceneIndex < 0 || sceneIndex >= static_cast<int>(gltf.scenes.size()))
            sceneIndex = 0;

        for (int rootNode : gltf.scenes[sceneIndex].nodes)
            processNode(rootNode, glm::mat4(1.0f));
    }
    else
    {
        // 容错：没有 scene 时把顶层节点都当作根
        for (size_t i = 0; i < gltf.nodes.size(); ++i)
            processNode(static_cast<int>(i), glm::mat4(1.0f));
    }

    if (missingNormals > 0)
        std::cout << "[GLTF] 提示: " << missingNormals
                  << " 个顶点缺少法线，已填零向量（渲染时用几何法线）\n";
    if (skippedPrimitives > 0)
        std::cerr << "[GLTF] 警告: 跳过 " << skippedPrimitives << " 个非三角形/无效图元\n";

    if (builder.subMeshes.empty())
    {
        std::cerr << "[GLTF] 没有解析出任何可用图元\n";
        return nullptr;
    }

    std::cout << "[GLTF] 几何合并完成: 顶点 " << builder.vertices.size()
              << "  索引 " << builder.indices.size()
              << "  子网格 " << builder.subMeshes.size() << "\n";

    return std::make_shared<Model>(*ctx.device, ctx.commandPool, builder);
}

} // namespace tiny
