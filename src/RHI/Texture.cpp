#include "Texture.h"
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <vector>
#include <stb_image.h>

namespace
{
// Tileable textures must wrap when their mip chain is built. vkCmdBlitImage
// clamps at the image edge, so every texture repeat keeps a darker/lighter
// 1px seam that turns into a visible square quilt at distance. This CPU box
// filter wraps the 2x2 taps, which keeps the mip chain periodic.
std::vector<unsigned char> buildWrappedMipChain(const unsigned char* base,
                                                uint32_t width, uint32_t height,
                                                uint32_t levels)
{
	std::vector<unsigned char> chain;
	size_t totalBytes = 0;
	for (uint32_t level = 0; level < levels; ++level)
	{
		const uint32_t w = std::max(1u, width >> level);
		const uint32_t h = std::max(1u, height >> level);
		totalBytes += static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
	}
	chain.resize(totalBytes);
	std::memcpy(chain.data(), base, static_cast<size_t>(width) * height * 4u);

	size_t previousOffset = 0;
	size_t destinationOffset = static_cast<size_t>(width) * height * 4u;
	uint32_t previousWidth = width;
	uint32_t previousHeight = height;
	for (uint32_t level = 1; level < levels; ++level)
	{
		const uint32_t w = std::max(1u, width >> level);
		const uint32_t h = std::max(1u, height >> level);
		const unsigned char* src = chain.data() + previousOffset;
		unsigned char* dst = chain.data() + destinationOffset;
		for (uint32_t y = 0; y < h; ++y)
		{
			const uint32_t sy0 = (y * 2u) % previousHeight;
			const uint32_t sy1 = (y * 2u + 1u) % previousHeight;
			for (uint32_t x = 0; x < w; ++x)
			{
				const uint32_t sx0 = (x * 2u) % previousWidth;
				const uint32_t sx1 = (x * 2u + 1u) % previousWidth;
				const unsigned char* p00 = src + (static_cast<size_t>(sy0) * previousWidth + sx0) * 4u;
				const unsigned char* p10 = src + (static_cast<size_t>(sy0) * previousWidth + sx1) * 4u;
				const unsigned char* p01 = src + (static_cast<size_t>(sy1) * previousWidth + sx0) * 4u;
				const unsigned char* p11 = src + (static_cast<size_t>(sy1) * previousWidth + sx1) * 4u;
				unsigned char* out = dst + (static_cast<size_t>(y) * w + x) * 4u;
				for (uint32_t c = 0; c < 4u; ++c)
				{
					out[c] = static_cast<unsigned char>(
						(static_cast<uint32_t>(p00[c]) + p10[c] + p01[c] + p11[c] + 2u) / 4u);
				}
			}
		}
		previousOffset = destinationOffset;
		destinationOffset += static_cast<size_t>(w) * h * 4u;
		previousWidth = w;
		previousHeight = h;
	}
	return chain;
}
} // namespace

Texture::Texture(Device& device, const std::string& filepath, VkFormat format, VkCommandPool commandPool, bool isHDR, bool tileableMips)
	: vulkanDevice(device)
{
	// 记录来源路径：场景序列化时要用它写回文件。
	sourcePath = filepath;
	if (isHDR)
		loadHDR(filepath, commandPool);
	else
		loadLDR(filepath, format, commandPool, tileableMips);
}

Texture::Texture(Device& device, const void* pixelData, uint32_t width, uint32_t height,
	VkFormat format, VkCommandPool commandPool, bool isHDR, bool tileableMips)
	: vulkanDevice(device)
{
	VkDeviceSize pixelSize = isHDR ? sizeof(float) : 1;
	createFromPixels(pixelData, width, height, pixelSize, format, commandPool, isHDR, tileableMips);
}

Texture::~Texture()
{
	if (sampler != VK_NULL_HANDLE) vkDestroySampler(vulkanDevice.getDevice(), sampler, nullptr);
	if (view != VK_NULL_HANDLE) vkDestroyImageView(vulkanDevice.getDevice(), view, nullptr);
	if (image != VK_NULL_HANDLE) vmaDestroyImage(vulkanDevice.getVmaAllocator(), image, allocation);
}

Texture::Texture(Texture&& other) noexcept
	: vulkanDevice(other.vulkanDevice), image(other.image), allocation(other.allocation), view(other.view), sampler(other.sampler)
{
	other.image = VK_NULL_HANDLE;
	other.allocation = VK_NULL_HANDLE;
	other.view = VK_NULL_HANDLE;
	other.sampler = VK_NULL_HANDLE;
}

Texture& Texture::operator=(Texture&& other) noexcept
{
	if (this != &other)
	{
		if (sampler) vkDestroySampler(vulkanDevice.getDevice(), sampler, nullptr);
		if (view) vkDestroyImageView(vulkanDevice.getDevice(), view, nullptr);
		if (image) vmaDestroyImage(vulkanDevice.getVmaAllocator(), image, allocation);

		image = other.image;
		allocation = other.allocation;
		view = other.view;
		sampler = other.sampler;

		other.image = VK_NULL_HANDLE;
		other.allocation = VK_NULL_HANDLE;
		other.view = VK_NULL_HANDLE;
		other.sampler = VK_NULL_HANDLE;
	}
	return *this;
}

void Texture::loadLDR(const std::string& filepath, VkFormat format, VkCommandPool commandPool, bool tileableMips)
{
	int texWidth, texHeight, texChannels;
	stbi_uc* pixels = stbi_load(filepath.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

	if (!pixels)
		throw std::runtime_error("Texture: failed to load " + filepath);

	createFromPixels(pixels, static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight),
		1, format, commandPool, false, tileableMips);

	stbi_image_free(pixels);

	std::cout << "[Texture] " << filepath << " (" << texWidth << "x" << texHeight << ")\n";
}

void Texture::loadHDR(const std::string& filepath, VkCommandPool commandPool)
{
	int texWidth, texHeight, texChannels;
	float* pixels = stbi_loadf(filepath.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

	if (!pixels)
		throw std::runtime_error("Texture: failed to load HDR " + filepath);

	createFromPixels(pixels, static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight),
		sizeof(float), VK_FORMAT_R32G32B32A32_SFLOAT, commandPool, true, false);

	stbi_image_free(pixels);

	std::cout << "[Texture] HDR " << filepath << " (" << texWidth << "x" << texHeight << ")\n";
}

void Texture::createFromPixels(const void* pixels, uint32_t width, uint32_t height,
VkDeviceSize pixelSize, VkFormat format, VkCommandPool commandPool,
bool isFloat, bool tileableMips)
{
VkDeviceSize imageSize = width * height * 4 * pixelSize;

// Material textures are sampled at a very oblique angle in Sponza. Without
// a mip chain, minification aliases into obvious moiré patterns in the
// distance, even with anisotropic filtering enabled.
VkFormatProperties formatProperties{};
vkGetPhysicalDeviceFormatProperties(vulkanDevice.getPhysicalDevice(), format, &formatProperties);
const bool canBlit = (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
const bool wrappedMipRequested = tileableMips && !isFloat && pixelSize == 1;
uint32_t mipLevels = 1;
if (canBlit || wrappedMipRequested)
{
mipLevels = static_cast<uint32_t>(
std::floor(std::log2(static_cast<float>(std::max(width, height))))) + 1;
}
const bool useWrappedMips = wrappedMipRequested && mipLevels > 1;

std::vector<unsigned char> wrappedChain;
VkDeviceSize uploadSize = imageSize;
if (useWrappedMips)
{
wrappedChain = buildWrappedMipChain(static_cast<const unsigned char*>(pixels),
width, height, mipLevels);
uploadSize = static_cast<VkDeviceSize>(wrappedChain.size());
}

VkBuffer stagingBuffer = VK_NULL_HANDLE;
VmaAllocation stagingAllocation = VK_NULL_HANDLE;

VkBufferCreateInfo bufferInfo{};
bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
bufferInfo.size = uploadSize;
bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

VmaAllocationCreateInfo stagingAllocInfo{};
stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
if (vmaCreateBuffer(vulkanDevice.getVmaAllocator(), &bufferInfo, &stagingAllocInfo,
&stagingBuffer, &stagingAllocation, nullptr) != VK_SUCCESS)
{
throw std::runtime_error("Texture: staging buffer creation failed");
}

void* data = nullptr;
vmaMapMemory(vulkanDevice.getVmaAllocator(), stagingAllocation, &data);
memcpy(data, useWrappedMips ? wrappedChain.data() : pixels, static_cast<size_t>(uploadSize));
vmaUnmapMemory(vulkanDevice.getVmaAllocator(), stagingAllocation);

VkImageCreateInfo imageInfo{};
imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
imageInfo.imageType = VK_IMAGE_TYPE_2D;
imageInfo.extent.width = width;
imageInfo.extent.height = height;
imageInfo.extent.depth = 1;
imageInfo.mipLevels = mipLevels;
imageInfo.arrayLayers = 1;
imageInfo.format = format;
imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

VmaAllocationCreateInfo imageAllocInfo{};
imageAllocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
if (vmaCreateImage(vulkanDevice.getVmaAllocator(), &imageInfo, &imageAllocInfo,
&this->image, &this->allocation, nullptr) != VK_SUCCESS)
{
vmaDestroyBuffer(vulkanDevice.getVmaAllocator(), stagingBuffer, stagingAllocation);
throw std::runtime_error("Texture: image creation failed");
}

VkCommandBufferAllocateInfo allocInfoCmd{};
allocInfoCmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
allocInfoCmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
allocInfoCmd.commandPool = commandPool;
allocInfoCmd.commandBufferCount = 1;
VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
vkAllocateCommandBuffers(vulkanDevice.getDevice(), &allocInfoCmd, &commandBuffer);

VkCommandBufferBeginInfo beginInfo{};
beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
vkBeginCommandBuffer(commandBuffer, &beginInfo);

auto transitionMip = [&](uint32_t mipLevel, uint32_t levelCount,
VkImageLayout oldLayout, VkImageLayout newLayout,
VkAccessFlags srcAccess, VkAccessFlags dstAccess,
VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
VkImageMemoryBarrier barrier{};
barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
barrier.oldLayout = oldLayout;
barrier.newLayout = newLayout;
barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
barrier.image = this->image;
barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
barrier.subresourceRange.baseMipLevel = mipLevel;
barrier.subresourceRange.levelCount = levelCount;
barrier.subresourceRange.baseArrayLayer = 0;
barrier.subresourceRange.layerCount = 1;
barrier.srcAccessMask = srcAccess;
barrier.dstAccessMask = dstAccess;
vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0,
0, nullptr, 0, nullptr, 1, &barrier);
};

if (useWrappedMips)
{
transitionMip(0, mipLevels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
0, VK_ACCESS_TRANSFER_WRITE_BIT,
VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

VkBufferImageCopy wrappedRegion{};
wrappedRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
wrappedRegion.imageSubresource.layerCount = 1;
VkDeviceSize mipOffset = 0;
for (uint32_t level = 0; level < mipLevels; ++level)
{
const uint32_t levelWidth = std::max(1u, width >> level);
const uint32_t levelHeight = std::max(1u, height >> level);
wrappedRegion.bufferOffset = mipOffset;
wrappedRegion.imageSubresource.mipLevel = level;
wrappedRegion.imageExtent = { levelWidth, levelHeight, 1 };
vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, this->image,
VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &wrappedRegion);
mipOffset += static_cast<VkDeviceSize>(levelWidth) * levelHeight * 4u;
}

transitionMip(0, mipLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}
else
{
transitionMip(0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
0, VK_ACCESS_TRANSFER_WRITE_BIT,
VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

VkBufferImageCopy region{};
region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
region.imageSubresource.mipLevel = 0;
region.imageSubresource.layerCount = 1;
region.imageExtent = { width, height, 1 };
vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, this->image,
VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

if (mipLevels > 1)
{
transitionMip(0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

const VkFilter blitFilter =
(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

for (uint32_t level = 1; level < mipLevels; ++level)
{
transitionMip(level, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
0, VK_ACCESS_TRANSFER_WRITE_BIT,
VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

VkImageBlit blit{};
blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
blit.srcSubresource.mipLevel = level - 1;
blit.srcSubresource.layerCount = 1;
blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
blit.dstSubresource.mipLevel = level;
blit.dstSubresource.layerCount = 1;
blit.srcOffsets[1] = {
static_cast<int32_t>(std::max(1u, width >> (level - 1))),
static_cast<int32_t>(std::max(1u, height >> (level - 1))), 1 };
blit.dstOffsets[1] = {
static_cast<int32_t>(std::max(1u, width >> level)),
static_cast<int32_t>(std::max(1u, height >> level)), 1 };

vkCmdBlitImage(commandBuffer,
this->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
this->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
1, &blit, blitFilter);

if (level + 1 < mipLevels)
{
transitionMip(level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}
}

// The final mip was just written, so move it to TRANSFER_SRC before
// transitioning the whole chain to the sampled layout.
transitionMip(mipLevels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

transitionMip(0, mipLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}
else
{
transitionMip(0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

}

vkEndCommandBuffer(commandBuffer);

VkSubmitInfo submitInfo{};
submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
submitInfo.commandBufferCount = 1;
submitInfo.pCommandBuffers = &commandBuffer;
vkQueueSubmit(vulkanDevice.getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
vkQueueWaitIdle(vulkanDevice.getGraphicsQueue());
vkFreeCommandBuffers(vulkanDevice.getDevice(), commandPool, 1, &commandBuffer);

vmaDestroyBuffer(vulkanDevice.getVmaAllocator(), stagingBuffer, stagingAllocation);

VkImageViewCreateInfo viewInfo{};
viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
viewInfo.image = this->image;
viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
viewInfo.format = format;
viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
viewInfo.subresourceRange.baseMipLevel = 0;
viewInfo.subresourceRange.levelCount = mipLevels;
viewInfo.subresourceRange.baseArrayLayer = 0;
viewInfo.subresourceRange.layerCount = 1;
vkCreateImageView(vulkanDevice.getDevice(), &viewInfo, nullptr, &this->view);

VkSamplerCreateInfo samplerInfo{};
samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
samplerInfo.magFilter = VK_FILTER_LINEAR;
samplerInfo.minFilter = VK_FILTER_LINEAR;
samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
samplerInfo.anisotropyEnable = VK_TRUE;
samplerInfo.maxAnisotropy = 16.0f;
samplerInfo.borderColor = isFloat ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_INT_OPAQUE_BLACK;
samplerInfo.unnormalizedCoordinates = VK_FALSE;
samplerInfo.compareEnable = VK_FALSE;
samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
samplerInfo.minLod = 0.0f;
samplerInfo.maxLod = static_cast<float>(mipLevels - 1);
vkCreateSampler(vulkanDevice.getDevice(), &samplerInfo, nullptr, &this->sampler);
}
