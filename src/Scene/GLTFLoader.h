#pragma once

#include <tiny_gltf.h>
#include <vulkan/vulkan.h>
#include <string>
#include <memory>
#include <vector>

class Model;
class Device;
class MaterialSystem;
class DescriptorManager;

namespace sentinel
{

// 自定义 glTF 图片解码器（注册给 tinygltf）。
//
// 为什么不用 tinygltf 自带的：它内部会展开一份 stb_image 实现，
// 与 Application.cpp 里的 STB_IMAGE_IMPLEMENTATION 撞符号，链接会失败。
//
// 作用是把 glTF 里编码后的 PNG/JPEG 字节解成 RGBA8，填进 image->image /
// width / height / component。随后由我们按「语义」决定 VkFormat：
// baseColor / emissive 走 sRGB，normal / metallicRoughness 走 UNORM。
bool loadImageData(tinygltf::Image* image, int image_idx,
    std::string* err, std::string* warn,
    int req_width, int req_height,
    const unsigned char* bytes, int size, void* user_data);

// 注册解码器。加载任何 glTF 之前调用一次。
void setupGLTFLoader(tinygltf::TinyGLTF& loader);

// glTF 加载所需的上下文。
// 之所以打包成一个结构体，是为了不让 Model 直接依赖 Core 层的材质系统 ——
// 几何数据（Model）与材质/描述符（MaterialSystem / DescriptorManager）解耦。
struct GLTFLoadContext
{
    Device* device = nullptr;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    MaterialSystem* materialSystem = nullptr;
    DescriptorManager* descriptorManager = nullptr;

    bool isValid() const
    {
        return device != nullptr && materialSystem != nullptr && descriptorManager != nullptr;
    }
};

// 加载 glTF 2.0（.gltf 与 .glb 都支持）。
// 几何数据填入 Model；材质会被解析成 DescriptorManager 的槽位，
// 逐个子网格写进 SubMesh::materialSlot —— 渲染侧无需区分 OBJ / glTF。
// 失败时返回 nullptr 并打印原因。
std::shared_ptr<Model> loadGLTFModel(const GLTFLoadContext& ctx, const std::string& filepath);

} // namespace sentinel
