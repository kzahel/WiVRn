#pragma once

#include "video_encoder.h"
#include "vk/allocation.h"

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

class video_encoder_videotoolbox : public video_encoder
{
	struct encode_request
	{
		std::mutex mutex;
		std::condition_variable cv;
		bool completed = false;
		bool dropped = false;
		bool control = false;
		std::string error;
		std::vector<uint8_t> bitstream;
	};

	struct in_t
	{
		buffer_allocation rgba;
	};

	VTCompressionSessionRef session = nullptr;
	std::array<in_t, num_slots> in;
	bool rgba_input = false;
	bool external_alpha_input = false;
	uint64_t rgba_debug_log_count = 0;
	std::array<buffer_allocation *, 2> external_alpha_sources = {};
	CMTime next_pts = kCMTimeZero;
	CMTime frame_duration = kCMTimeInvalid;

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

	void configure_session(const encoder_settings & settings);
	void update_frame_rate(float fps);
	void update_bitrate(uint32_t bitrate_bps);
	void convert_rgba_to_nv12(uint8_t slot, CVPixelBufferRef pixel_buffer);
	void prepare_external_alpha_rgba(uint8_t slot);
};

} // namespace wivrn
