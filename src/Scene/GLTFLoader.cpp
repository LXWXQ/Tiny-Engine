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
//   * 节点树原样保留在 Model::Node 里，几何只存「节点局部空间」，
//     节点矩阵在渲染 / 拾取 / 包围盒处现乘（F4：改 node 变换不必重导资产）
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
#include <limits>

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

    glm::mat4 readMat4(const AccessorView& v, size_t i)
    {
        glm::mat4 result(1.0f);
        const float* p = reinterpret_cast<const float*>(v.base + i * v.stride);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                result[c][r] = p[c * 4 + r];
        return result;
    }

    glm::mat4 nodeLocalMatrix(const tinygltf::Node& node)
    {
        if (node.matrix.size() == 16)
        {
            glm::mat4 local(1.0f);
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    local[c][r] = static_cast<float>(node.matrix[c * 4 + r]);
            return local;
        }

        glm::vec3 translation(0.0f);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale(1.0f);
        if (node.translation.size() == 3)
            translation = glm::vec3(node.translation[0], node.translation[1], node.translation[2]);
        if (node.rotation.size() == 4)
            rotation = glm::quat(static_cast<float>(node.rotation[3]),
                                 static_cast<float>(node.rotation[0]),
                                 static_cast<float>(node.rotation[1]),
                                 static_cast<float>(node.rotation[2]));
        if (node.scale.size() == 3)
            scale = glm::vec3(node.scale[0], node.scale[1], node.scale[2]);

        return glm::translate(glm::mat4(1.0f), translation)
             * glm::mat4_cast(rotation)
             * glm::scale(glm::mat4(1.0f), scale);
    }

    void decomposeTRS(const glm::mat4& matrix, glm::vec3& outTranslation,
                      glm::quat& outRotation, glm::vec3& outScale)
    {
        outTranslation = glm::vec3(matrix[3]);
        const glm::vec3 c0(matrix[0]);
        const glm::vec3 c1(matrix[1]);
        const glm::vec3 c2(matrix[2]);
        outScale = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));
        if (outScale.x < 1e-8f || outScale.y < 1e-8f || outScale.z < 1e-8f)
        {
            outRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            outScale = glm::max(outScale, glm::vec3(1e-8f));
            return;
        }
        const glm::mat3 rotation(
            c0 / outScale.x,
            c1 / outScale.y,
            c2 / outScale.z);
        outRotation = glm::normalize(glm::quat_cast(rotation));
    }

    void readJointIndices(const tinygltf::Model& model, int accessorIndex, size_t i, glm::uvec4& out)
    {
        out = glm::uvec4(0u);
        if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
            return;
        const tinygltf::Accessor& acc = model.accessors[accessorIndex];
        if (acc.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
            && acc.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            return;

        const size_t componentSize = acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ? 1u : 2u;
        const AccessorView view = makeAccessorView(model, accessorIndex, componentSize * 4u);
        if (view.base == nullptr || i >= view.count)
            return;
        const unsigned char* p = view.base + i * view.stride;
        for (int k = 0; k < 4; ++k)
        {
            out[k] = componentSize == 1u
                ? static_cast<uint32_t>(p[k])
                : static_cast<uint32_t>(reinterpret_cast<const uint16_t*>(p)[k]);
        }
    }

    void readWeights(const tinygltf::Model& model, int accessorIndex, size_t i, glm::vec4& out)
    {
        out = glm::vec4(0.0f);
        if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
            return;
        const tinygltf::Accessor& acc = model.accessors[accessorIndex];
        const size_t componentSize =
            acc.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT ? 4u
            : (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ? 2u
            : (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ? 1u : 0u));
        if (componentSize == 0u)
            return;

        const AccessorView view = makeAccessorView(model, accessorIndex, componentSize * 4u);
        if (view.base == nullptr || i >= view.count)
            return;
        const unsigned char* p = view.base + i * view.stride;
        if (componentSize == 4u)
        {
            const float* f = reinterpret_cast<const float*>(p);
            out = glm::vec4(f[0], f[1], f[2], f[3]);
        }
        else if (componentSize == 2u)
        {
            const uint16_t* u = reinterpret_cast<const uint16_t*>(p);
            const float inv = acc.normalized ? 1.0f / 65535.0f : 1.0f;
            out = glm::vec4(u[0] * inv, u[1] * inv, u[2] * inv, u[3] * inv);
        }
        else
        {
            const float inv = acc.normalized ? 1.0f / 255.0f : 1.0f;
            out = glm::vec4(p[0] * inv, p[1] * inv, p[2] * inv, p[3] * inv);
        }
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

namespace
{
    // ---------------- 贴图 ----------------
    // 按 (imageIndex, 是否 sRGB) 缓存：同一张图可能既当颜色用又当数据用。
    struct GLTFTextureCache
    {
        const GLTFLoadContext& ctx;
        const tinygltf::Model& gltf;
        std::vector<std::shared_ptr<Texture>> srgb;
        std::vector<std::shared_ptr<Texture>> linear;

        GLTFTextureCache(const GLTFLoadContext& context, const tinygltf::Model& model)
            : ctx(context), gltf(model),
              srgb(model.images.size()), linear(model.images.size())
        {
        }

        const tinygltf::Image* imageForTexture(int textureIndex, int& imageIndex) const
        {
            if (textureIndex < 0 || textureIndex >= static_cast<int>(gltf.textures.size()))
                return nullptr;

            imageIndex = gltf.textures[textureIndex].source;
            if (imageIndex < 0 || imageIndex >= static_cast<int>(gltf.images.size()))
                return nullptr;
            return &gltf.images[imageIndex];
        }

        std::shared_ptr<Texture> fetch(int textureIndex, bool srgbSpace)
        {
            int imageIndex = 0;
            const tinygltf::Image* image = imageForTexture(textureIndex, imageIndex);
            if (image == nullptr)
                return nullptr;

            std::shared_ptr<Texture>& slot = srgbSpace ? srgb[imageIndex] : linear[imageIndex];
            if (slot)
                return slot;

            if (image->image.empty() || image->width <= 0 || image->height <= 0)
            {
                std::cerr << "[GLTF] 图片 " << imageIndex << " 没有解码数据，跳过\n";
                return nullptr;
            }

            const VkFormat format = srgbSpace
                ? VK_FORMAT_R8G8B8A8_SRGB
                : VK_FORMAT_R8G8B8A8_UNORM;
            slot = std::make_shared<Texture>(*ctx.device, image->image.data(),
                static_cast<uint32_t>(image->width), static_cast<uint32_t>(image->height),
                format, ctx.commandPool, false);
            return slot;
        }
    };

    // ---------------- 材质 ----------------
    struct GLTFMaterialTable
    {
        std::vector<uint32_t> slots;
        std::vector<uint8_t> alphaMasked;
        std::vector<float> alphaCutoff;
        std::vector<uint8_t> albedoFallback;
    };

    void buildMaterialTable(const GLTFLoadContext& ctx, const tinygltf::Model& gltf,
                            GLTFTextureCache& textures, const std::string& filepath,
                            GLTFMaterialTable& out)
    {
        out.slots.assign(gltf.materials.size(), kInvalidMaterialSlot);
        out.alphaMasked.assign(gltf.materials.size(), 0);
        out.alphaCutoff.assign(gltf.materials.size(), 0.5f);
        out.albedoFallback.assign(gltf.materials.size(), 0);

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
            mat->albedoMap            = textures.fetch(pbr.baseColorTexture.index, true);
            mat->emissiveMap          = textures.fetch(src.emissiveTexture.index, true);
            mat->normalMap            = textures.fetch(src.normalTexture.index, false);
            mat->metallicRoughnessMap = textures.fetch(pbr.metallicRoughnessTexture.index, false);
            mat->aoMap                = textures.fetch(src.occlusionTexture.index, false);

            ctx.materialSystem->buildMaterialDescriptor(mat, *ctx.descriptorManager);
            out.slots[i] = mat->descriptorSetIndex;
            out.alphaMasked[i] = (mat->alphaMode == AlphaMode::MASK_MODE) ? 1u : 0u;
            out.alphaCutoff[i] = mat->alphaCutoff;
            // glTF 里没有 baseColorTexture 是常态（纯色材质），不是"缺材质"。
            // 这里必须保留 baseColorFactor，让它乘默认白纹理会出正确纯色；
            // 只有完全没有材质槽的图元才交给棋盘格占位着色。
            out.albedoFallback[i] = 0u;
        }
    }

    // ---------------- 骨架 ----------------
    struct GLTFSkeletonData
    {
        std::vector<std::vector<uint32_t>> skinJointSlots;
        bool hasSkinData = false;
    };

    std::vector<int32_t> buildNodeParents(const tinygltf::Model& gltf)
    {
        std::vector<int32_t> nodeParents(gltf.nodes.size(), -1);
        for (size_t i = 0; i < gltf.nodes.size(); ++i)
        {
            for (int child : gltf.nodes[i].children)
            {
                if (child >= 0 && child < static_cast<int>(gltf.nodes.size()))
                    nodeParents[static_cast<size_t>(child)] = static_cast<int32_t>(i);
            }
        }
        return nodeParents;
    }

    void buildSkeletonNodes(const tinygltf::Model& gltf,
                            const std::vector<int32_t>& nodeParents,
                            Model::Builder& builder)
    {
        builder.skeleton.resize(gltf.nodes.size());
        for (size_t i = 0; i < gltf.nodes.size(); ++i)
        {
            const glm::mat4 local = nodeLocalMatrix(gltf.nodes[i]);
            SkeletonNode& skeletonNode = builder.skeleton[i];
            skeletonNode.parent = nodeParents[i];
            decomposeTRS(local, skeletonNode.translation, skeletonNode.rotation, skeletonNode.scale);
        }
    }

    // F4：把 glTF 节点树原样搬进 Model。几何之后只存节点局部空间，
    // 所以节点自身的 TRS 必须完整保留，不能再像以前那样只用来烘焙顶点。
    void buildModelNodes(const tinygltf::Model& gltf,
                         const std::vector<int32_t>& nodeParents,
                         Model::Builder& builder)
    {
        const float big = std::numeric_limits<float>::max();
        builder.nodes.resize(gltf.nodes.size());
        builder.nodeBoundsMin.assign(gltf.nodes.size(), glm::vec3(big));
        builder.nodeBoundsMax.assign(gltf.nodes.size(), glm::vec3(-big));

        for (size_t i = 0; i < gltf.nodes.size(); ++i)
        {
            const tinygltf::Node& node = gltf.nodes[i];
            Model::Node& modelNode = builder.nodes[i];
            modelNode.name = node.name;
            modelNode.parent = nodeParents[i];
            modelNode.mesh = node.mesh;
            decomposeTRS(nodeLocalMatrix(node), modelNode.translation,
                         modelNode.rotation, modelNode.scale);
        }
    }

    void buildJointSlots(const tinygltf::Model& gltf, Model::Builder& builder,
                         GLTFSkeletonData& out)
    {
        std::vector<int> nodeToJoint(gltf.nodes.size(), -1);
        out.skinJointSlots.resize(gltf.skins.size());

        for (size_t skinIndex = 0; skinIndex < gltf.skins.size(); ++skinIndex)
        {
            const tinygltf::Skin& skin = gltf.skins[skinIndex];
            AccessorView ibmView{};
            if (skin.inverseBindMatrices >= 0)
                ibmView = makeAccessorView(gltf, skin.inverseBindMatrices, sizeof(float) * 16);

            for (size_t j = 0; j < skin.joints.size(); ++j)
            {
                const int nodeIndex = skin.joints[j];
                if (nodeIndex < 0 || nodeIndex >= static_cast<int>(gltf.nodes.size()))
                    continue;

                int slot = nodeToJoint[static_cast<size_t>(nodeIndex)];
                if (slot < 0)
                {
                    slot = static_cast<int>(builder.jointNodes.size());
                    nodeToJoint[static_cast<size_t>(nodeIndex)] = slot;
                    builder.jointNodes.push_back(static_cast<uint32_t>(nodeIndex));
                    builder.inverseBindMatrices.push_back(glm::mat4(1.0f));
                }
                out.skinJointSlots[skinIndex].push_back(static_cast<uint32_t>(slot));

                if (ibmView.base != nullptr && j < ibmView.count)
                    builder.inverseBindMatrices[static_cast<size_t>(slot)] = readMat4(ibmView, j);
            }
        }
    }

    void buildSkeletonData(const tinygltf::Model& gltf, Model::Builder& builder,
                           GLTFSkeletonData& out)
    {
        buildSkeletonNodes(gltf, buildNodeParents(gltf), builder);
        buildJointSlots(gltf, builder, out);
        out.hasSkinData = !builder.jointNodes.empty();
    }

    // ---------------- 几何 ----------------
    struct GLTFGeometryState
    {
        const tinygltf::Model& gltf;
        Model::Builder& builder;
        const GLTFSkeletonData& skeleton;
        const GLTFMaterialTable& materials;
        size_t missingNormals = 0;
        size_t skippedPrimitives = 0;
    };

    struct GLTFPrimitiveSkin
    {
        int index = -1;
        AccessorView joints{};
        AccessorView weights{};
    };

    struct GLTFPrimitiveViews
    {
        AccessorView positions{};
        AccessorView normals{};
        AccessorView uvs{};
        GLTFPrimitiveSkin skin;
    };

    GLTFPrimitiveSkin resolvePrimitiveSkin(const GLTFGeometryState& state,
                                           const tinygltf::Node& node,
                                           const tinygltf::Primitive& prim)
    {
        GLTFPrimitiveSkin result;
        if (!state.skeleton.hasSkinData || node.skin < 0)
            return result;
        if (node.skin >= static_cast<int>(state.gltf.skins.size()))
            return result;
        if (state.skeleton.skinJointSlots[static_cast<size_t>(node.skin)].empty())
            return result;

        auto jointsIt = prim.attributes.find("JOINTS_0");
        auto weightsIt = prim.attributes.find("WEIGHTS_0");
        if (jointsIt == prim.attributes.end() || weightsIt == prim.attributes.end())
            return result;

        result.joints = makeAccessorView(state.gltf, jointsIt->second, sizeof(uint32_t) * 4);
        result.weights = makeAccessorView(state.gltf, weightsIt->second, sizeof(float) * 4);
        if (result.joints.base != nullptr && result.weights.base != nullptr)
            result.index = node.skin;
        return result;
    }

    // F4：顶点直接存节点局部空间的坐标。节点矩阵由 Model::Node 承担，
    // 渲染 / 拾取 / 包围盒在消费端相乘，改 node 变换不需要重新导资产。
    Vertex makeVertex(const AccessorView& positions, const AccessorView& normals,
                      const AccessorView& uvs, size_t vertexIndex,
                      size_t& missingNormals)
    {
        Vertex vertex{};

        vertex.position = readVec3(positions, vertexIndex);

        if (normals.base != nullptr)
        {
            vertex.normal = readVec3(normals, vertexIndex);
        }
        else
        {
            vertex.normal = glm::vec3(0.0f);
            ++missingNormals;
        }

        vertex.uv = (uvs.base != nullptr) ? readVec2(uvs, vertexIndex) : glm::vec2(0.0f);
        vertex.color = glm::vec3(1.0f);
        return vertex;
    }

    void appendPrimitiveSkinData(const tinygltf::Model& gltf,
                                 const tinygltf::Primitive& prim,
                                 size_t vertexIndex,
                                 const GLTFPrimitiveSkin& skin,
                                 const std::vector<std::vector<uint32_t>>& skinJointSlots,
                                 Model::Builder& builder)
    {
        glm::uvec4 joints(0u);
        glm::vec4 weights(0.0f);
        if (skin.index >= 0)
        {
            glm::uvec4 rawJoints(0u);
            readJointIndices(gltf, prim.attributes.at("JOINTS_0"), vertexIndex, rawJoints);
            readWeights(gltf, prim.attributes.at("WEIGHTS_0"), vertexIndex, weights);

            const std::vector<uint32_t>& slotMap =
                skinJointSlots[static_cast<size_t>(skin.index)];
            for (int k = 0; k < 4; ++k)
            {
                if (rawJoints[k] < slotMap.size() && weights[k] > 0.0f)
                    joints[k] = slotMap[rawJoints[k]];
                else
                    weights[k] = 0.0f;
            }
        }
        builder.skinJoints.push_back(joints);
        builder.skinWeights.push_back(weights);
    }

    uint32_t appendPrimitiveVertices(GLTFGeometryState& state, const tinygltf::Primitive& prim,
                                     const GLTFPrimitiveViews& views)
    {
        const uint32_t firstVertex = static_cast<uint32_t>(state.builder.vertices.size());
        const bool skinned = views.skin.index >= 0;

        for (size_t v = 0; v < views.positions.count; ++v)
        {
            state.builder.vertices.push_back(makeVertex(
                views.positions, views.normals, views.uvs, v, state.missingNormals));

            if (state.skeleton.hasSkinData)
            {
                appendPrimitiveSkinData(state.gltf, prim, v, views.skin,
                    state.skeleton.skinJointSlots, state.builder);
            }
        }
        return firstVertex;
    }

    void appendPrimitiveIndices(const tinygltf::Model& gltf, const tinygltf::Primitive& prim,
                                uint32_t firstVertex, size_t vertexCount,
                                Model::Builder& builder)
    {
        if (prim.indices >= 0 && prim.indices < static_cast<int>(gltf.accessors.size()))
        {
            const size_t indexCount = gltf.accessors[prim.indices].count;
            for (size_t k = 0; k < indexCount; ++k)
                builder.indices.push_back(firstVertex + readIndex(gltf, prim.indices, k));
        }
        else
        {
            for (uint32_t k = 0; k < static_cast<uint32_t>(vertexCount); ++k)
                builder.indices.push_back(firstVertex + k);
        }
    }

    void appendPrimitiveSubMesh(const GLTFMaterialTable& materials,
                                const tinygltf::Primitive& prim,
                                uint32_t firstIndex, uint32_t indexCount,
                                uint32_t nodeIndex,
                                Model::Builder& builder)
    {
        SubMesh sub{};
        sub.firstIndex = firstIndex;
        sub.indexCount = indexCount;
        sub.node = nodeIndex;

        const bool hasMaterial = prim.material >= 0 &&
            prim.material < static_cast<int>(materials.slots.size());
        sub.materialSlot = hasMaterial
            ? materials.slots[prim.material]
            : kInvalidMaterialSlot;
        if (hasMaterial)
        {
            sub.alphaMasked = materials.alphaMasked[prim.material] != 0;
            sub.alphaCutoff = materials.alphaCutoff[prim.material];
            sub.albedoFallback = materials.albedoFallback[prim.material] != 0;
        }

        if (sub.indexCount > 0)
            builder.subMeshes.push_back(sub);
    }

    void appendPrimitive(GLTFGeometryState& state, const tinygltf::Node& node,
                         int nodeIndex, const tinygltf::Primitive& prim)
    {
        if (prim.mode != TINYGLTF_MODE_TRIANGLES)
        {
            ++state.skippedPrimitives;
            return;
        }

        auto posIt = prim.attributes.find("POSITION");
        if (posIt == prim.attributes.end())
        {
            ++state.skippedPrimitives;
            return;
        }

        GLTFPrimitiveViews views;
        views.positions = makeAccessorView(state.gltf, posIt->second, sizeof(float) * 3);
        if (views.positions.base == nullptr || views.positions.count == 0)
        {
            ++state.skippedPrimitives;
            return;
        }

        auto nIt = prim.attributes.find("NORMAL");
        if (nIt != prim.attributes.end())
            views.normals = makeAccessorView(state.gltf, nIt->second, sizeof(float) * 3);

        auto uvIt = prim.attributes.find("TEXCOORD_0");
        if (uvIt != prim.attributes.end())
            views.uvs = makeAccessorView(state.gltf, uvIt->second, sizeof(float) * 2);

        views.skin = resolvePrimitiveSkin(state, node, prim);

        const uint32_t firstIndex = static_cast<uint32_t>(state.builder.indices.size());
        const uint32_t firstVertex = appendPrimitiveVertices(state, prim, views);
        appendPrimitiveIndices(state.gltf, prim, firstVertex, views.positions.count, state.builder);

        const uint32_t indexCount =
            static_cast<uint32_t>(state.builder.indices.size()) - firstIndex;

        // 蒙皮顶点本来就在绑定姿态的局部空间，再乘一次节点矩阵就是双重变换。
        const bool skinned = views.skin.index >= 0;
        const uint32_t subMeshNode = skinned ? kNoModelNode : static_cast<uint32_t>(nodeIndex);
        appendPrimitiveSubMesh(state.materials, prim, firstIndex, indexCount, subMeshNode, state.builder);

        if (!skinned && nodeIndex >= 0 &&
            static_cast<size_t>(nodeIndex) < state.builder.nodeBoundsMin.size())
        {
            glm::vec3& lo = state.builder.nodeBoundsMin[static_cast<size_t>(nodeIndex)];
            glm::vec3& hi = state.builder.nodeBoundsMax[static_cast<size_t>(nodeIndex)];
            for (size_t v = firstVertex; v < state.builder.vertices.size(); ++v)
            {
                const glm::vec3& position = state.builder.vertices[v].position;
                lo = glm::min(lo, position);
                hi = glm::max(hi, position);
            }
        }
    }

    // F4：这里只负责遍历拓扑，不再累积世界矩阵——节点变换已经存进 builder.nodes。
    void processSceneNode(GLTFGeometryState& state, int nodeIndex)
    {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(state.gltf.nodes.size()))
            return;

        const tinygltf::Node& node = state.gltf.nodes[nodeIndex];

        if (node.mesh >= 0 && node.mesh < static_cast<int>(state.gltf.meshes.size()))
        {
            const tinygltf::Mesh& mesh = state.gltf.meshes[node.mesh];
            for (const tinygltf::Primitive& prim : mesh.primitives)
                appendPrimitive(state, node, nodeIndex, prim);
        }

        for (int child : node.children)
            processSceneNode(state, child);
    }

    void processSceneGeometry(GLTFGeometryState& state)
    {
        buildModelNodes(state.gltf, buildNodeParents(state.gltf), state.builder);

        if (!state.gltf.scenes.empty())
        {
            int sceneIndex = state.gltf.defaultScene;
            if (sceneIndex < 0 || sceneIndex >= static_cast<int>(state.gltf.scenes.size()))
                sceneIndex = 0;

            for (int rootNode : state.gltf.scenes[sceneIndex].nodes)
                processSceneNode(state, rootNode);
            return;
        }

        for (size_t i = 0; i < state.gltf.nodes.size(); ++i)
            processSceneNode(state, static_cast<int>(i));
    }

    // ---------------- 动画 ----------------
    std::string animationClipName(const std::string& rawName, size_t index)
    {
        // Blender/Quaternius 导出常带 "CharacterArmature|Idle" 前缀；
        // 引擎按 Idle/Walk/Run/Jump 查找，去掉 "|" 之前的部分。
        std::string clipName = rawName;
        const size_t separator = clipName.rfind('|');
        if (separator != std::string::npos && separator + 1 < clipName.size())
            clipName = clipName.substr(separator + 1);
        return clipName.empty()
            ? ("Anim" + std::to_string(index))
            : clipName;
    }

    bool resolveAnimationPath(const std::string& targetPath, AnimationPath& out)
    {
        if (targetPath == "translation") out = AnimationPath::Translation;
        else if (targetPath == "rotation") out = AnimationPath::Rotation;
        else if (targetPath == "scale") out = AnimationPath::Scale;
        else return false;
        return true;
    }

    int animationOutputComponents(const tinygltf::Accessor& accessor)
    {
        if (accessor.type == TINYGLTF_TYPE_VEC4)
            return 4;
        if (accessor.type == TINYGLTF_TYPE_VEC3)
            return 3;
        return 0;
    }

    bool readAnimationSamples(const tinygltf::Model& gltf,
                              const tinygltf::AnimationSampler& sampler,
                              AnimationChannel& out)
    {
        const AccessorView inputView = makeAccessorView(gltf, sampler.input, sizeof(float));
        const tinygltf::Accessor& inputAccessor = gltf.accessors[sampler.input];
        if (inputView.base == nullptr ||
            inputAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
            return false;

        const int components = animationOutputComponents(gltf.accessors[sampler.output]);
        if (components == 0)
            return false;

        const AccessorView outputView = makeAccessorView(
            gltf, sampler.output, sizeof(float) * static_cast<size_t>(components));
        if (outputView.base == nullptr)
            return false;

        const bool cubic = sampler.interpolation == "CUBICSPLINE";
        const size_t sampleCount = std::min(inputView.count, outputView.count);
        if (sampleCount == 0)
            return false;

        out.times.resize(sampleCount);
        out.values.resize(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i)
        {
            const float* input = reinterpret_cast<const float*>(
                inputView.base + i * inputView.stride);
            out.times[i] = input[0];

            // CUBICSPLINE 每个关键帧是 (inTangent, value, outTangent)。
            // 这里取 value 当线性关键帧，至少保证动作方向与节奏正确。
            const size_t valueIndex = cubic ? (i * 3 + 1) : i;
            if (valueIndex >= outputView.count)
                break;
            const float* output = reinterpret_cast<const float*>(
                outputView.base + valueIndex * outputView.stride);
            out.values[i] = glm::vec4(
                output[0], output[1], output[2],
                components == 4 ? output[3] : 0.0f);
        }
        out.values.resize(out.times.size());
        return !out.times.empty();
    }

    bool parseAnimationChannel(const tinygltf::Model& gltf,
                               const tinygltf::Animation& animation,
                               const tinygltf::AnimationChannel& channel,
                               AnimationChannel& out)
    {
        if (channel.target_node < 0 || channel.sampler < 0)
            return false;
        if (channel.sampler >= static_cast<int>(animation.samplers.size()))
            return false;

        AnimationPath path;
        if (!resolveAnimationPath(channel.target_path, path))
            return false;

        const tinygltf::AnimationSampler& sampler =
            animation.samplers[static_cast<size_t>(channel.sampler)];
        if (sampler.input < 0 || sampler.output < 0)
            return false;
        if (sampler.input >= static_cast<int>(gltf.accessors.size()) ||
            sampler.output >= static_cast<int>(gltf.accessors.size()))
            return false;

        out.node = static_cast<uint32_t>(channel.target_node);
        out.path = path;
        out.step = sampler.interpolation == "STEP";
        return readAnimationSamples(gltf, sampler, out);
    }

    void appendAnimationClip(const tinygltf::Model& gltf,
                             const tinygltf::Animation& animation,
                             size_t clipIndex, Model::Builder& builder)
    {
        AnimationClip clip;
        clip.name = animationClipName(animation.name, builder.animations.size());

        for (const tinygltf::AnimationChannel& channel : animation.channels)
        {
            AnimationChannel parsed;
            if (!parseAnimationChannel(gltf, animation, channel, parsed))
                continue;
            if (parsed.times.empty())
                continue;
            clip.duration = std::max(clip.duration, parsed.times.back());
            clip.channels.push_back(std::move(parsed));
        }

        if (!clip.channels.empty())
            builder.animations.push_back(std::move(clip));
        (void)clipIndex;
    }

    void parseAnimations(const tinygltf::Model& gltf, Model::Builder& builder)
    {
        for (size_t i = 0; i < gltf.animations.size(); ++i)
            appendAnimationClip(gltf, gltf.animations[i], i, builder);
    }
} // namespace

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

    GLTFTextureCache textures(ctx, gltf);
    GLTFMaterialTable materials;
    buildMaterialTable(ctx, gltf, textures, filepath, materials);

    // ---------------- 几何 / 骨架 ----------------
    Model::Builder builder;
    GLTFSkeletonData skeleton;
    buildSkeletonData(gltf, builder, skeleton);

    GLTFGeometryState geometry{gltf, builder, skeleton, materials};
    processSceneGeometry(geometry);

    parseAnimations(gltf, builder);

    if (skeleton.hasSkinData)
    {
        std::cout << "[GLTF] 蒙皮: joints=" << builder.jointNodes.size()
                  << "  animations=" << builder.animations.size()
                  << "  skinVertices=" << builder.skinJoints.size() << "\n";
    }

    if (geometry.missingNormals > 0)
        std::cout << "[GLTF] 提示: " << geometry.missingNormals
                  << " 个顶点缺少法线，已填零向量（渲染时用几何法线）\n";
    if (geometry.skippedPrimitives > 0)
        std::cerr << "[GLTF] 警告: 跳过 " << geometry.skippedPrimitives << " 个非三角形/无效图元\n";

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
