#pragma once
#include <string>
#include <memory>
#include <glm/glm.hpp>
#include "RHI/Texture.h"
#include "Core/MaterialSlot.h"
enum class AlphaMode {
    OPAQUE_MODE,  // 不透明 (绝大部分物体)
    MASK_MODE,    // 遮罩裁剪 (树叶、铁丝网，需要 discard)
    BLEND_MODE    // 半透明混合 (玻璃，延迟管线暂不支持)
};

class Material {
public:
    std::string name;
    // 在 DescriptorManager 中分配的材质描述符集槽位；未分配时为 kInvalidMaterialSlot
    uint32_t descriptorSetIndex = kInvalidMaterialSlot;
    // --- PBR 基础属性 ---
    glm::vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    float alphaCutoff{0.5f}; // 仅在 MASK 模式下生效
    AlphaMode alphaMode{AlphaMode::OPAQUE_MODE};

    // --- 贴图指针 ---
    std::shared_ptr<Texture> albedoMap;
    std::shared_ptr<Texture> normalMap;
    std::shared_ptr<Texture> metallicRoughnessMap;
    std::shared_ptr<Texture> emissiveMap;
    std::shared_ptr<Texture> aoMap;

    // --- 全局默认替身 (由 MaterialSystem 在引擎启动时填充) ---
    static VkDescriptorImageInfo defaultWhiteTextureInfo;
    static VkDescriptorImageInfo defaultBlackTextureInfo;
    static VkDescriptorImageInfo defaultNormalTextureInfo; 

    // --- 安全的 Descriptor 获取接口 ---
    VkDescriptorImageInfo getAlbedoInfo() const 
    {
        if (albedoMap) return {albedoMap->getSampler(), albedoMap->getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        return defaultWhiteTextureInfo; // 没有颜色图？用纯白替身，依靠 baseColorFactor
    }

    VkDescriptorImageInfo getNormalInfo() const
    {
        if (normalMap) return {normalMap->getSampler(), normalMap->getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        return defaultNormalTextureInfo; // 没有法线图？用朝向正 Z 的平坦法线替身 (128, 128, 255)
    }

    VkDescriptorImageInfo getMetallicRoughnessInfo() const 
    {
        if (metallicRoughnessMap) return {metallicRoughnessMap->getSampler(), metallicRoughnessMap->getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        return defaultWhiteTextureInfo; // glTF中，没贴图默认用基础系数
    }

    VkDescriptorImageInfo getEmissiveInfo() const
     {
        if (emissiveMap) return {emissiveMap->getSampler(), emissiveMap->getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        return defaultBlackTextureInfo; // 没有自发光图？用纯黑替身，不发光
    }

    VkDescriptorImageInfo getAoInfo() const 
    {
        if (aoMap) return {aoMap->getSampler(), aoMap->getView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        return defaultWhiteTextureInfo; // 没有 AO 图？用纯白替身 (1.0 表示没有遮蔽)
    }
};

// 静态成员变量需要在 .cpp 中或者这里做内联定义 (C++17 起可以使用 inline static)
inline VkDescriptorImageInfo Material::defaultWhiteTextureInfo{};
inline VkDescriptorImageInfo Material::defaultBlackTextureInfo{};
inline VkDescriptorImageInfo Material::defaultNormalTextureInfo{};