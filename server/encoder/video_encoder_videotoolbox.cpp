#include "video_encoder_videotoolbox.h"

#include "encoder_settings.h"
#include "util/u_logging.h"
#include "utils/wivrn_vk_bundle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>

#include <CoreFoundation/CoreFoundation.h>

namespace wivrn
{
namespace
{
bool
log_apple_rgba_samples_enabled()
{
#if defined(__APPLE__)
	static const bool enabled = std::getenv("WIVRN_LOG_APPLE_RGBA_SAMPLES") != nullptr;
	return enabled;
#else
	return false;
#endif
}

CFNumberRef
create_cf_number_s32(int32_t value)
{
	return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
}

CFNumberRef
create_cf_number_s64(int64_t value)
{
	return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
}

std::string
osstatus_string(OSStatus status)
{
	return std::to_string(static_cast<int32_t>(status));
}

void
set_session_property(VTCompressionSessionRef session, CFStringRef key, CFTypeRef value)
{
	OSStatus status = VTSessionSetProperty(session, key, value);
	if (status != noErr)
		throw std::runtime_error("VTSessionSetProperty failed: " + osstatus_string(status));
}
} // namespace

bool
video_encoder_videotoolbox::supports(video_codec codec)
{
#if defined(__APPLE__)
	return codec == video_codec::h264;
#else
	(void)codec;
	return false;
#endif
}

CMTime
video_encoder_videotoolbox::make_frame_duration(float fps)
{
	const int32_t timescale = 1000;
	const int32_t value = std::max<int32_t>(1, static_cast<int32_t>(std::lround(timescale / std::max(fps, 1.0f))));
	return CMTimeMake(value, timescale);
}

video_encoder_videotoolbox::video_encoder_videotoolbox(
        wivrn_vk_bundle & vk,
        const encoder_settings & settings,
        uint8_t stream_idx) :
        video_encoder(stream_idx, settings, std::make_unique<default_idr_handler>(), true),
        rgba_input(settings.rgba_input),
        frame_duration(make_frame_duration(settings.fps))
{
	if (settings.bit_depth != 8)
		throw std::runtime_error("VideoToolbox encoder only supports 8-bit encoding");
	if (settings.codec != h264)
		throw std::runtime_error("VideoToolbox encoder currently only supports H.264");
	if (!rgba_input)
		throw std::runtime_error("VideoToolbox encoder currently requires Apple RGBA input");

#if defined(__APPLE__)
	external_alpha_input = stream_idx == 2;
#endif

	for (auto & slot: in)
	{
		slot.rgba = buffer_allocation(
		        vk.device,
		        {
		                .size = vk::DeviceSize(extent.width * extent.height * 4),
		                .usage = vk::BufferUsageFlagBits::eTransferDst,
		        },
		        {
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO,
		        },
		        "videotoolbox rgba buffer");
	}

	CFNumberRef width = create_cf_number_s32(extent.width);
	CFNumberRef height = create_cf_number_s32(extent.height);
	const int32_t pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	CFNumberRef pixel_format_number = create_cf_number_s32(pixel_format);

	const void * source_keys[] = {
	        kCVPixelBufferPixelFormatTypeKey,
	        kCVPixelBufferWidthKey,
	        kCVPixelBufferHeightKey,
	};
	const void * source_values[] = {
	        pixel_format_number,
	        width,
	        height,
	};
	CFDictionaryRef source_attributes = CFDictionaryCreate(
	        kCFAllocatorDefault,
	        source_keys,
	        source_values,
	        3,
	        &kCFTypeDictionaryKeyCallBacks,
	        &kCFTypeDictionaryValueCallBacks);

	const void * encoder_keys[] = {
	        kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
	};
	const void * encoder_values[] = {
	        kCFBooleanTrue,
	};
	CFDictionaryRef encoder_specification = CFDictionaryCreate(
	        kCFAllocatorDefault,
	        encoder_keys,
	        encoder_values,
	        1,
	        &kCFTypeDictionaryKeyCallBacks,
	        &kCFTypeDictionaryValueCallBacks);

	const OSStatus create_status = VTCompressionSessionCreate(
	        kCFAllocatorDefault,
	        extent.width,
	        extent.height,
	        kCMVideoCodecType_H264,
	        encoder_specification,
	        source_attributes,
	        nullptr,
	        &video_encoder_videotoolbox::output_callback,
	        this,
	        &session);

	CFRelease(encoder_specification);
	CFRelease(source_attributes);
	CFRelease(pixel_format_number);
	CFRelease(height);
	CFRelease(width);

	if (create_status != noErr || session == nullptr)
		throw std::runtime_error("Failed to create VideoToolbox compression session: " + osstatus_string(create_status));

	configure_session(settings);
}

void
video_encoder_videotoolbox::configure_session(const encoder_settings & settings)
{
	set_session_property(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
	set_session_property(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
	set_session_property(session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Main_AutoLevel);

	CFNumberRef max_keyframe_interval = create_cf_number_s32(std::numeric_limits<int32_t>::max() / 2);
	set_session_property(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, max_keyframe_interval);
	CFRelease(max_keyframe_interval);

	update_frame_rate(settings.fps);
	update_bitrate(settings.bitrate);

	const OSStatus prepare_status = VTCompressionSessionPrepareToEncodeFrames(session);
	if (prepare_status != noErr)
		throw std::runtime_error("VTCompressionSessionPrepareToEncodeFrames failed: " + osstatus_string(prepare_status));

	CFTypeRef using_hardware = nullptr;
	if (VTSessionCopyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, nullptr, &using_hardware) == noErr && using_hardware)
	{
		const bool hardware = using_hardware == kCFBooleanTrue;
		U_LOG_I("VideoToolbox stream %u hardware encoder=%d", unsigned(stream_idx), int(hardware));
		CFRelease(using_hardware);
	}
}

void
video_encoder_videotoolbox::update_frame_rate(float fps)
{
	frame_duration = make_frame_duration(fps);
	const int32_t fps_rounded = std::max<int32_t>(1, static_cast<int32_t>(std::lround(fps)));
	CFNumberRef value = create_cf_number_s32(fps_rounded);
	set_session_property(session, kVTCompressionPropertyKey_ExpectedFrameRate, value);
	CFRelease(value);
}

void
video_encoder_videotoolbox::update_bitrate(uint32_t bitrate_bps)
{
	const int32_t clamped = static_cast<int32_t>(std::min<uint32_t>(bitrate_bps, std::numeric_limits<int32_t>::max()));
	CFNumberRef bitrate = create_cf_number_s32(clamped);
	set_session_property(session, kVTCompressionPropertyKey_AverageBitRate, bitrate);
	CFRelease(bitrate);

	const int64_t bytes_per_second = std::max<int64_t>(1, bitrate_bps / 8);
	CFNumberRef bytes = create_cf_number_s64(bytes_per_second);
	CFNumberRef seconds = create_cf_number_s64(1);
	const void * limits[] = {bytes, seconds};
	CFArrayRef data_rate_limits = CFArrayCreate(kCFAllocatorDefault, limits, 2, &kCFTypeArrayCallBacks);
	set_session_property(session, kVTCompressionPropertyKey_DataRateLimits, data_rate_limits);
	CFRelease(data_rate_limits);
	CFRelease(seconds);
	CFRelease(bytes);
}

std::pair<bool, vk::Semaphore>
video_encoder_videotoolbox::present_image(vk::Image y_cbcr, bool transferred, vk::raii::CommandBuffer & cmd_buf, uint8_t slot, uint64_t)
{
	(void)transferred;
	if (external_alpha_input)
		return {false, nullptr};

	cmd_buf.copyImageToBuffer(
	        y_cbcr,
	        vk::ImageLayout::eTransferSrcOptimal,
	        in[slot].rgba,
	        vk::BufferImageCopy{
	                .bufferRowLength = extent.width,
	                .imageSubresource = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .baseArrayLayer = stream_idx,
	                        .layerCount = 1,
	                },
	                .imageExtent = {
	                        .width = extent.width,
	                        .height = extent.height,
	                        .depth = 1,
	                }});
	return {false, nullptr};
}

void
video_encoder_videotoolbox::set_external_alpha_sources(buffer_allocation * left, buffer_allocation * right)
{
	if (!external_alpha_input)
		return;

	external_alpha_sources = {left, right};
}

void
video_encoder_videotoolbox::prepare_for_encode(uint8_t slot, uint64_t frame_index)
{
	(void)frame_index;
	if (!external_alpha_input)
		return;

	prepare_external_alpha_rgba(slot);
}

void
video_encoder_videotoolbox::append_annexb_nal(std::vector<uint8_t> & out, const uint8_t * data, size_t size)
{
	static constexpr uint8_t start_code[] = {0, 0, 0, 1};
	out.insert(out.end(), std::begin(start_code), std::end(start_code));
	out.insert(out.end(), data, data + size);
}

void
video_encoder_videotoolbox::append_h264_parameter_sets(std::vector<uint8_t> & out, CMFormatDescriptionRef format_description)
{
	size_t parameter_set_count = 0;
	int nal_length = 0;
	const uint8_t * parameter_set = nullptr;
	size_t parameter_set_size = 0;
	OSStatus status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
	        format_description,
	        0,
	        &parameter_set,
	        &parameter_set_size,
	        &parameter_set_count,
	        &nal_length);
	if (status != noErr)
		throw std::runtime_error("Failed to read H.264 parameter sets: " + osstatus_string(status));

	append_annexb_nal(out, parameter_set, parameter_set_size);
	for (size_t index = 1; index < parameter_set_count; ++index)
	{
		status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
		        format_description,
		        index,
		        &parameter_set,
		        &parameter_set_size,
		        nullptr,
		        nullptr);
		if (status != noErr)
			throw std::runtime_error("Failed to read H.264 parameter set: " + osstatus_string(status));
		append_annexb_nal(out, parameter_set, parameter_set_size);
	}
}

bool
video_encoder_videotoolbox::is_sync_sample(CMSampleBufferRef sample_buffer)
{
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample_buffer, false);
	if (attachments == nullptr || CFArrayGetCount(attachments) == 0)
		return true;

	auto * attachment = static_cast<CFDictionaryRef>(const_cast<void *>(CFArrayGetValueAtIndex(attachments, 0)));
	CFTypeRef not_sync = CFDictionaryGetValue(attachment, kCMSampleAttachmentKey_NotSync);
	return not_sync == nullptr || not_sync == kCFBooleanFalse;
}

void
video_encoder_videotoolbox::append_h264_sample(std::vector<uint8_t> & out, CMSampleBufferRef sample_buffer)
{
	CMFormatDescriptionRef format_description = CMSampleBufferGetFormatDescription(sample_buffer);
	if (is_sync_sample(sample_buffer))
		append_h264_parameter_sets(out, format_description);

	int nal_length = 0;
	OSStatus status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
	        format_description,
	        0,
	        nullptr,
	        nullptr,
	        nullptr,
	        &nal_length);
	if (status != noErr)
		throw std::runtime_error("Failed to query H.264 NAL length: " + osstatus_string(status));
	if (nal_length <= 0)
		throw std::runtime_error("Invalid H.264 NAL length from VideoToolbox");

	CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buffer);
	if (block == nullptr)
		throw std::runtime_error("VideoToolbox sample is missing block buffer");

	const size_t total_size = CMBlockBufferGetDataLength(block);
	std::vector<uint8_t> avcc(total_size);
	status = CMBlockBufferCopyDataBytes(block, 0, total_size, avcc.data());
	if (status != noErr)
		throw std::runtime_error("Failed to copy H.264 block buffer: " + osstatus_string(status));

	size_t offset = 0;
	while (offset + nal_length <= avcc.size())
	{
		size_t unit_size = 0;
		for (size_t i = 0; i < nal_length; ++i)
			unit_size = (unit_size << 8) | avcc[offset + i];
		offset += nal_length;
		if (offset + unit_size > avcc.size())
			throw std::runtime_error("Invalid AVCC NAL length from VideoToolbox");
		append_annexb_nal(out, avcc.data() + offset, unit_size);
		offset += unit_size;
	}
	if (offset != avcc.size())
		throw std::runtime_error("Trailing AVCC bytes from VideoToolbox sample");
}

void
video_encoder_videotoolbox::output_callback(void * output_callback_refcon,
                                            void * source_frame_refcon,
                                            OSStatus status,
                                            VTEncodeInfoFlags info_flags,
                                            CMSampleBufferRef sample_buffer)
{
	auto * self = static_cast<video_encoder_videotoolbox *>(output_callback_refcon);
	auto * request = static_cast<encode_request *>(source_frame_refcon);
	std::unique_lock lock(request->mutex);

	if (status != noErr)
	{
		request->error = "VideoToolbox callback error: " + osstatus_string(status);
		request->completed = true;
		request->cv.notify_all();
		return;
	}

	if (info_flags & kVTEncodeInfo_FrameDropped)
	{
		request->dropped = true;
		request->completed = true;
		request->cv.notify_all();
		return;
	}

	if (sample_buffer == nullptr || !CMSampleBufferDataIsReady(sample_buffer))
	{
		request->error = "VideoToolbox returned an empty sample buffer";
		request->completed = true;
		request->cv.notify_all();
		return;
	}

	try
	{
		request->control = self->is_sync_sample(sample_buffer);
		self->append_h264_sample(request->bitstream, sample_buffer);
	}
	catch (const std::exception & e)
	{
		request->error = e.what();
	}

	request->completed = true;
	request->cv.notify_all();
}

void
video_encoder_videotoolbox::convert_rgba_to_nv12(uint8_t slot, CVPixelBufferRef pixel_buffer)
{
	auto * rgba = static_cast<uint8_t *>(in[slot].rgba.map());
	auto * y_plane = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
	auto * uv_plane = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
	const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
	const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);

	for (uint32_t y = 0; y < extent.height; ++y)
	{
		auto * y_row = y_plane + y * y_stride;
		for (uint32_t x = 0; x < extent.width; ++x)
		{
			const uint8_t * src = rgba + (y * extent.width + x) * 4;
			int r = src[0];
			int g = src[1];
			int b = src[2];
			int y_value = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
			y_row[x] = std::clamp(y_value, 0, 255);
		}
	}

	for (uint32_t y = 0; y < extent.height; y += 2)
	{
		auto * uv_row = uv_plane + (y / 2) * uv_stride;
		for (uint32_t x = 0; x < extent.width; x += 2)
		{
			int r_sum = 0;
			int g_sum = 0;
			int b_sum = 0;
			for (uint32_t dy = 0; dy < 2; ++dy)
			{
				for (uint32_t dx = 0; dx < 2; ++dx)
				{
					const uint8_t * src = rgba + ((y + dy) * extent.width + (x + dx)) * 4;
					r_sum += src[0];
					g_sum += src[1];
					b_sum += src[2];
				}
			}

			int r = r_sum / 4;
			int g = g_sum / 4;
			int b = b_sum / 4;
			int u_value = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
			int v_value = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
			uv_row[x + 0] = std::clamp(u_value, 0, 255);
			uv_row[x + 1] = std::clamp(v_value, 0, 255);
		}
	}

	if (log_apple_rgba_samples_enabled())
	{
		++rgba_debug_log_count;
		if (rgba_debug_log_count <= 5 || rgba_debug_log_count % 120 == 0)
		{
			const size_t center_index = ((extent.height / 2) * extent.width + (extent.width / 2)) * 4;
			const size_t center_y_index = (extent.height / 2) * y_stride + (extent.width / 2);
			const size_t center_uv_index = (extent.height / 4) * uv_stride + (extent.width / 2 & ~1u);
			fprintf(stderr,
			        "apple-rgba-vt stream=%u rgba0=(%u,%u,%u,%u) rgbaC=(%u,%u,%u,%u) y0=%u yC=%u uvC=(%u,%u)\n",
			        unsigned(stream_idx),
			        unsigned(rgba[0]),
			        unsigned(rgba[1]),
			        unsigned(rgba[2]),
			        unsigned(rgba[3]),
			        unsigned(rgba[center_index + 0]),
			        unsigned(rgba[center_index + 1]),
			        unsigned(rgba[center_index + 2]),
			        unsigned(rgba[center_index + 3]),
			        unsigned(y_plane[0]),
			        unsigned(y_plane[center_y_index]),
			        unsigned(uv_plane[center_uv_index + 0]),
			        unsigned(uv_plane[center_uv_index + 1]));
		}
	}
}

void
video_encoder_videotoolbox::prepare_external_alpha_rgba(uint8_t slot)
{
	auto * dst = static_cast<uint8_t *>(in[slot].rgba.map());
	const uint32_t output_width = extent.width;
	const uint32_t output_height = extent.height;
	const uint32_t eye_output_width = output_width / 2;
	const uint32_t source_width = output_width;
	const uint32_t source_height = output_height * 2;

	if (!external_alpha_sources[0] || !external_alpha_sources[1])
		throw std::runtime_error("Apple alpha stream missing RGBA sources");

	auto sample_alpha = [&](const uint8_t * src, uint32_t x, uint32_t y) {
		const size_t pixel_index = (size_t(y) * source_width + x) * 4 + 3;
		return src[pixel_index];
	};

	std::array<const uint8_t *, 2> sources = {
	        external_alpha_sources[0]->data<uint8_t>(),
	        external_alpha_sources[1]->data<uint8_t>(),
	};

	for (uint32_t y = 0; y < output_height; ++y)
	{
		for (uint32_t x = 0; x < output_width; ++x)
		{
			const uint32_t eye = x >= eye_output_width ? 1u : 0u;
			const uint32_t eye_x = eye == 0 ? x : x - eye_output_width;
			const uint32_t src_x = std::min(source_width - 1, eye_x * 2);
			const uint32_t src_y = std::min(source_height - 1, y * 2);

			uint32_t alpha_sum = 0;
			for (uint32_t dy = 0; dy < 2; ++dy)
			{
				for (uint32_t dx = 0; dx < 2; ++dx)
				{
					alpha_sum += sample_alpha(
					        sources[eye],
					        std::min(source_width - 1, src_x + dx),
					        std::min(source_height - 1, src_y + dy));
				}
			}

			const uint8_t alpha = uint8_t(alpha_sum / 4);
			const size_t dst_index = (size_t(y) * output_width + x) * 4;
			dst[dst_index + 0] = alpha;
			dst[dst_index + 1] = alpha;
			dst[dst_index + 2] = alpha;
			dst[dst_index + 3] = 255;
		}
	}
}

std::optional<video_encoder::data>
video_encoder_videotoolbox::encode(uint8_t slot, uint64_t frame_index)
{
	bool reconfigure = false;
	if (auto framerate = pending_framerate.exchange(0))
	{
		reconfigure = true;
		update_frame_rate(framerate);
	}
	if (auto bitrate = pending_bitrate.exchange(0))
	{
		reconfigure = true;
		update_bitrate(bitrate);
	}
	if (reconfigure)
		idr->reset();

	auto & idr_handler = ((default_idr_handler &)*idr);
	auto frame_type = idr_handler.get_type(frame_index);

	CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
	if (pool == nullptr)
		throw std::runtime_error("VideoToolbox did not provide a pixel buffer pool");

	CVPixelBufferRef pixel_buffer = nullptr;
	OSStatus status = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixel_buffer);
	if (status != noErr || pixel_buffer == nullptr)
		throw std::runtime_error("Failed to allocate VideoToolbox source pixel buffer: " + osstatus_string(status));

	status = CVPixelBufferLockBaseAddress(pixel_buffer, 0);
	if (status != noErr)
	{
		CFRelease(pixel_buffer);
		throw std::runtime_error("Failed to lock VideoToolbox pixel buffer: " + osstatus_string(status));
	}
	convert_rgba_to_nv12(slot, pixel_buffer);
	CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

	auto request = std::make_shared<encode_request>();

	CFDictionaryRef frame_properties = nullptr;
	if (frame_type == default_idr_handler::frame_type::i)
	{
		const void * keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
		const void * values[] = {kCFBooleanTrue};
		frame_properties = CFDictionaryCreate(
		        kCFAllocatorDefault,
		        keys,
		        values,
		        1,
		        &kCFTypeDictionaryKeyCallBacks,
		        &kCFTypeDictionaryValueCallBacks);
	}

	VTEncodeInfoFlags info_flags = 0;
	const CMTime pts = next_pts;
	next_pts = CMTimeAdd(next_pts, frame_duration);
	status = VTCompressionSessionEncodeFrame(
	        session,
	        pixel_buffer,
	        pts,
	        frame_duration,
	        frame_properties,
	        request.get(),
	        &info_flags);

	if (frame_properties)
		CFRelease(frame_properties);
	CFRelease(pixel_buffer);

	if (status != noErr)
		throw std::runtime_error("VTCompressionSessionEncodeFrame failed: " + osstatus_string(status));

	status = VTCompressionSessionCompleteFrames(session, pts);
	if (status != noErr)
		throw std::runtime_error("VTCompressionSessionCompleteFrames failed: " + osstatus_string(status));

	{
		std::unique_lock lock(request->mutex);
		request->cv.wait(lock, [&] { return request->completed; });
	}

	if (!request->error.empty())
		throw std::runtime_error(request->error);
	if (request->dropped || request->bitstream.empty())
		return {};

	return data{
	        .encoder = this,
	        .span = std::span<uint8_t>(request->bitstream),
	        .mem = request,
	        .prefer_control = request->control,
	};
}

video_encoder_videotoolbox::~video_encoder_videotoolbox()
{
	if (session != nullptr)
	{
		VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
		VTCompressionSessionInvalidate(session);
		CFRelease(session);
		session = nullptr;
	}
}

} // namespace wivrn
