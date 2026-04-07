#include "apple_vulkan_metal_bridge.h"

#include "wivrn_vk_bundle.h"
#include "vk/vk_helpers.h"

#include <CoreVideo/CVMetalTextureCache.h>
#include <Foundation/Foundation.h>
#include <Metal/Metal.h>

#include <array>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace wivrn
{
namespace
{
std::string
ns_error_string(NSError * error, const char * fallback)
{
	if (error == nil || error.localizedDescription == nil)
		return fallback;
	return error.localizedDescription.UTF8String;
}

class mtl_texture_handle
{
public:
	mtl_texture_handle() = default;

	explicit mtl_texture_handle(id<MTLTexture> texture) :
	        texture_([texture retain])
	{}

	mtl_texture_handle(mtl_texture_handle && other) noexcept :
	        texture_(other.texture_)
	{
		other.texture_ = nil;
	}

	mtl_texture_handle & operator=(mtl_texture_handle && other) noexcept
	{
		if (this == &other)
			return *this;
		reset();
		texture_ = other.texture_;
		other.texture_ = nil;
		return *this;
	}

	mtl_texture_handle(const mtl_texture_handle &) = delete;
	mtl_texture_handle & operator=(const mtl_texture_handle &) = delete;

	~mtl_texture_handle()
	{
		reset();
	}

	id<MTLTexture> get() const
	{
		return texture_;
	}

private:
	void reset()
	{
		if (texture_ != nil)
		{
			[texture_ release];
			texture_ = nil;
		}
	}

	id<MTLTexture> texture_ = nil;
};

class cv_metal_texture_handle
{
public:
	cv_metal_texture_handle() = default;

	explicit cv_metal_texture_handle(CVMetalTextureRef texture) :
	        texture_(texture)
	{}

	cv_metal_texture_handle(cv_metal_texture_handle && other) noexcept :
	        texture_(other.texture_)
	{
		other.texture_ = nullptr;
	}

	cv_metal_texture_handle & operator=(cv_metal_texture_handle && other) noexcept
	{
		if (this == &other)
			return *this;
		reset();
		texture_ = other.texture_;
		other.texture_ = nullptr;
		return *this;
	}

	cv_metal_texture_handle(const cv_metal_texture_handle &) = delete;
	cv_metal_texture_handle & operator=(const cv_metal_texture_handle &) = delete;

	~cv_metal_texture_handle()
	{
		reset();
	}

	id<MTLTexture> get() const
	{
		return texture_ ? CVMetalTextureGetTexture(texture_) : nil;
	}

private:
	void reset()
	{
		if (texture_ != nullptr)
		{
			CFRelease(texture_);
			texture_ = nullptr;
		}
	}

	CVMetalTextureRef texture_ = nullptr;
};
} // namespace

struct apple_vulkan_metal_bridge::impl
{
	struct slot_texture
	{
		cv_metal_texture_handle cv_texture;
	};

	struct source_texture
	{
		vk::raii::ImageView image_view = nullptr;
		mtl_texture_handle texture;
	};

	wivrn_vk_bundle & vk;
	vk::Extent2D extent;
	id<MTLDevice> device = nil;
	id<MTLCommandQueue> queue = nil;
	id<MTLLibrary> library = nil;
	id<MTLComputePipelineState> pipeline = nil;
	CVMetalTextureCacheRef texture_cache = nullptr;
	std::array<slot_texture, 2> slot_textures = {};
	std::unordered_map<uint64_t, source_texture> source_textures;

	impl(wivrn_vk_bundle & vk, vk::Extent2D extent, std::span<CVPixelBufferRef const> pixel_buffers) :
	        vk(vk),
	        extent(extent)
	{
		if (pixel_buffers.size() != slot_textures.size())
			throw std::runtime_error("Apple VT GPU bridge received an unexpected slot count");

		@autoreleasepool
		{
			device = [MTLCreateSystemDefaultDevice() retain];
			if (device == nil)
				throw std::runtime_error("Failed to create Metal device for Apple VT GPU bridge");

			queue = [device newCommandQueue];
			if (queue == nil)
				throw std::runtime_error("Failed to create Metal command queue for Apple VT GPU bridge");

			NSString * source = @"#include <metal_stdlib>\n"
			                     "using namespace metal;\n"
			                     "kernel void rgba_to_bgra(texture2d<half, access::read> src [[texture(0)]],\n"
			                     "                         texture2d<half, access::write> dst [[texture(1)]],\n"
			                     "                         uint2 gid [[thread_position_in_grid]]) {\n"
			                     "  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;\n"
			                     "  dst.write(src.read(gid).bgra, gid);\n"
			                     "}\n";

			NSError * error = nil;
			library = [device newLibraryWithSource:source options:nil error:&error];
			if (library == nil)
				throw std::runtime_error("Failed to compile Metal shader library for Apple VT GPU bridge: " +
				                         ns_error_string(error, "unknown"));

			id<MTLFunction> function = [library newFunctionWithName:@"rgba_to_bgra"];
			if (function == nil)
				throw std::runtime_error("Failed to create Metal function rgba_to_bgra for Apple VT GPU bridge");

			pipeline = [device newComputePipelineStateWithFunction:function error:&error];
			[function release];
			if (pipeline == nil)
				throw std::runtime_error("Failed to create Metal compute pipeline for Apple VT GPU bridge: " +
				                         ns_error_string(error, "unknown"));

			const CVReturn cache_status = CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &texture_cache);
			if (cache_status != kCVReturnSuccess || texture_cache == nullptr)
				throw std::runtime_error("Failed to create CVMetalTextureCache for Apple VT GPU bridge: " +
				                         std::to_string(int(cache_status)));

			for (size_t slot = 0; slot < slot_textures.size(); ++slot)
			{
				CVMetalTextureRef cv_texture = nullptr;
				const CVReturn status = CVMetalTextureCacheCreateTextureFromImage(
				        kCFAllocatorDefault,
				        texture_cache,
				        pixel_buffers[slot],
				        nullptr,
				        MTLPixelFormatBGRA8Unorm,
				        extent.width,
				        extent.height,
				        0,
				        &cv_texture);
				if (status != kCVReturnSuccess || cv_texture == nullptr || CVMetalTextureGetTexture(cv_texture) == nil)
					throw std::runtime_error("Failed to create destination Metal texture for Apple VT GPU bridge slot " +
					                         std::to_string(slot) + ": " + std::to_string(int(status)));
				slot_textures[slot].cv_texture = cv_metal_texture_handle(cv_texture);
			}
		}
	}

	~impl()
	{
		source_textures.clear();
		if (texture_cache != nullptr)
		{
			CFRelease(texture_cache);
			texture_cache = nullptr;
		}
		if (pipeline != nil)
		{
			[pipeline release];
			pipeline = nil;
		}
		if (library != nil)
		{
			[library release];
			library = nil;
		}
		if (queue != nil)
		{
			[queue release];
			queue = nil;
		}
		if (device != nil)
		{
			[device release];
			device = nil;
		}
	}

	source_texture &
	get_source_texture(VkImage image, uint32_t source_layer)
	{
		const uint64_t key = wivrn::vk_handle(vk::Image(image));
		auto [it, inserted] = source_textures.try_emplace(key);
		if (!inserted)
			return it->second;

		auto & entry = it->second;
		entry.image_view = vk::raii::ImageView(
		        vk.device,
		        vk::ImageViewCreateInfo{
		                .image = image,
		                .viewType = vk::ImageViewType::e2D,
		                .format = vk::Format::eR8G8B8A8Unorm,
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .baseMipLevel = 0,
		                        .levelCount = 1,
		                        .baseArrayLayer = source_layer,
		                        .layerCount = 1,
		                },
		        });

		VkExportMetalTextureInfoEXT texture_info = {
		        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
		        .image = image,
		        .imageView = *entry.image_view,
		        .plane = VK_IMAGE_ASPECT_COLOR_BIT,
		        .mtlTexture = nil,
		};
		VkExportMetalObjectsInfoEXT export_info = {
		        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
		        .pNext = &texture_info,
		};
		vk.vk.vkExportMetalObjectsEXT(vk.vk.device, &export_info);
		if (texture_info.mtlTexture == nil)
			throw std::runtime_error("vkExportMetalObjectsEXT did not return a source Metal texture");

		entry.texture = mtl_texture_handle(texture_info.mtlTexture);
		return entry;
	}

	void
	copy_image_to_slot(VkImage image, uint8_t slot, uint32_t source_layer)
	{
		if (image == VK_NULL_HANDLE)
			throw std::runtime_error("Apple VT GPU bridge is missing a source image");

		source_texture & source = get_source_texture(image, source_layer);
		id<MTLTexture> src = source.texture.get();
		id<MTLTexture> dst = slot_textures[slot].cv_texture.get();
		if (src == nil || dst == nil)
			throw std::runtime_error("Apple VT GPU bridge is missing a Metal texture");

		@autoreleasepool
		{
			id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
			if (command_buffer == nil)
				throw std::runtime_error("Failed to create Metal command buffer for Apple VT GPU bridge");

			id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
			if (encoder == nil)
				throw std::runtime_error("Failed to create Metal compute encoder for Apple VT GPU bridge");

			[encoder setComputePipelineState:pipeline];
			[encoder setTexture:src atIndex:0];
			[encoder setTexture:dst atIndex:1];

			const MTLSize threads = MTLSizeMake(16, 16, 1);
			const MTLSize groups = MTLSizeMake(
			        (extent.width + threads.width - 1) / threads.width,
			        (extent.height + threads.height - 1) / threads.height,
			        1);
			[encoder dispatchThreadgroups:groups threadsPerThreadgroup:threads];
			[encoder endEncoding];

			[command_buffer commit];
			[command_buffer waitUntilCompleted];

			if (command_buffer.status != MTLCommandBufferStatusCompleted)
				throw std::runtime_error("Apple VT GPU bridge Metal command buffer failed: " +
				                         ns_error_string(command_buffer.error, "unknown"));
		}
	}
};

bool
apple_vulkan_metal_bridge::supported(const wivrn_vk_bundle & vk)
{
	return vk.vk.has_EXT_metal_objects && vk.vk.vkExportMetalObjectsEXT != nullptr;
}

apple_vulkan_metal_bridge::apple_vulkan_metal_bridge(
        wivrn_vk_bundle & vk, vk::Extent2D extent, std::span<CVPixelBufferRef const> pixel_buffers) :
        impl_(std::make_unique<impl>(vk, extent, pixel_buffers))
{}

apple_vulkan_metal_bridge::~apple_vulkan_metal_bridge() = default;

void
apple_vulkan_metal_bridge::copy_image_to_slot(VkImage image, uint8_t slot, uint32_t source_layer)
{
	impl_->copy_image_to_slot(image, slot, source_layer);
}

} // namespace wivrn
