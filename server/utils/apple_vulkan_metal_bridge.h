#pragma once

#if defined(__APPLE__) && !defined(VK_USE_PLATFORM_METAL_EXT)
#define VK_USE_PLATFORM_METAL_EXT
#endif

#include <CoreVideo/CoreVideo.h>
#include <cstdint>
#include <memory>
#include <span>
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{

struct wivrn_vk_bundle;

class apple_vulkan_metal_bridge
{
public:
	static bool supported(const wivrn_vk_bundle & vk);

	apple_vulkan_metal_bridge(wivrn_vk_bundle & vk, vk::Extent2D extent, std::span<CVPixelBufferRef const> pixel_buffers);
	~apple_vulkan_metal_bridge();

	void copy_image_to_slot(VkImage image, uint8_t slot, uint32_t source_layer);

private:
	struct impl;
	std::unique_ptr<impl> impl_;
};

} // namespace wivrn
