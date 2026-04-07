#pragma once

#include "video_encoder.h"
#include "vk/allocation.h"

#include <Accelerate/Accelerate.h>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

namespace wivrn
{

class apple_vulkan_metal_bridge;

class video_encoder_videotoolbox : public video_encoder
{
	struct encode_request
	{
		std::mutex mutex;
		std::condition_variable cv;
		bool completed = false;
		bool dropped = false;
		bool control = false;
		uint64_t frame_index = 0;
		uint8_t stream_idx = 0;
		int64_t encode_submit_ns = 0;
		int64_t callback_ns = 0;
		std::string error;
		std::vector<uint8_t> bitstream;
	};

	struct in_t
	{
		buffer_allocation rgba;
		CVPixelBufferRef pixel_buffer = nullptr;
		VkImage source_image = VK_NULL_HANDLE;
		int64_t source_copy_ns = 0;
	};

	VTCompressionSessionRef session = nullptr;
	std::array<in_t, num_slots> in;
	bool rgba_input = false;
	bool external_alpha_input = false;
	bool bgra_input = false;
	bool direct_rgba_input = false;
	bool gpu_bridge_enabled = false;
	bool vimage_nv12_conversion = false;
	uint64_t rgba_debug_log_count = 0;
	std::array<buffer_allocation *, 2> external_alpha_sources = {};
	vImage_ARGBToYpCbCr vimage_argb_to_ycbcr = {};
	CMTime next_pts = kCMTimeZero;
	CMTime frame_duration = kCMTimeInvalid;
	std::unique_ptr<apple_vulkan_metal_bridge> gpu_bridge;

public:
	static bool supports(video_codec codec);

	video_encoder_videotoolbox(wivrn_vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx);

	std::pair<bool, vk::Semaphore> present_image(vk::Image y_cbcr, bool transferred, vk::raii::CommandBuffer & cmd_buf, uint8_t slot, uint64_t frame_index) override;
	void prepare_for_encode(uint8_t slot, uint64_t frame_index) override;
	std::optional<data> encode(uint8_t slot, uint64_t frame_index) override;
	void set_external_alpha_sources(buffer_allocation * left, buffer_allocation * right);

	~video_encoder_videotoolbox() override;

private:
	static void output_callback(void * output_callback_refcon,
	                            void * source_frame_refcon,
	                            OSStatus status,
	                            VTEncodeInfoFlags info_flags,
	                            CMSampleBufferRef sample_buffer);

	static CMTime make_frame_duration(float fps);
	static void append_annexb_nal(std::vector<uint8_t> & out, const uint8_t * data, size_t size);
	static void append_h264_parameter_sets(std::vector<uint8_t> & out, CMFormatDescriptionRef format_description);
	static void append_h264_sample(std::vector<uint8_t> & out, CMSampleBufferRef sample_buffer);
	static bool is_sync_sample(CMSampleBufferRef sample_buffer);
	static CFDictionaryRef create_source_attributes(uint32_t width, uint32_t height, OSType pixel_format);

	void configure_session(const encoder_settings & settings);
	void update_frame_rate(float fps);
	void update_bitrate(uint32_t bitrate_bps);
	void copy_rgba_to_pixel_buffer(uint8_t slot, CVPixelBufferRef pixel_buffer);
	void convert_rgba_to_nv12(uint8_t slot, CVPixelBufferRef pixel_buffer);
	void prepare_external_alpha_rgba(uint8_t slot);
	CVPixelBufferRef create_source_pixel_buffer(OSType pixel_format);
};

} // namespace wivrn
