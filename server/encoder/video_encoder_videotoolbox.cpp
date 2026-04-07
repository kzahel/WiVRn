#include "video_encoder_videotoolbox.h"

#include "encoder_settings.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "utils/wivrn_vk_bundle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>

#include <CoreFoundation/CoreFoundation.h>

namespace wivrn
{
namespace
{
bool
log_apple_host_timing_enabled()
{
#if defined(__APPLE__)
	static const bool enabled = std::getenv("WIVRN_LOG_APPLE_HOST_TIMING") != nullptr;
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

const char *
source_format_name()
{
	return "BGRA";
}

const char *
source_conversion_name()
{
	return "cpu-readback-swizzle";
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
        frame_duration(make_frame_duration(settings.fps))
{
	if (settings.bit_depth != 8)
		throw std::runtime_error("VideoToolbox encoder only supports 8-bit encoding");
	if (settings.codec != h264)
		throw std::runtime_error("VideoToolbox encoder currently only supports H.264");
	if (!settings.rgba_input)
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

	const OSType pixel_format = kCVPixelFormatType_32BGRA;
	CFDictionaryRef source_attributes = create_source_attributes(extent.width, extent.height, pixel_format);

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

	if (create_status != noErr || session == nullptr)
		throw std::runtime_error("Failed to create VideoToolbox compression session: " + osstatus_string(create_status));

	// Keep one encoder-owned IOSurface-backed source buffer per slot so the
	// current CPU staging path and the later GPU handoff share the same lifetime.
	for (auto & slot: in)
		slot.pixel_buffer = create_source_pixel_buffer(pixel_format);

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
	U_LOG_W("VideoToolbox stream %u source pixel format=%s conversion=%s",
	        unsigned(stream_idx),
	        source_format_name(),
	        source_conversion_name());
}

CFDictionaryRef
video_encoder_videotoolbox::create_source_attributes(uint32_t width, uint32_t height, OSType pixel_format)
{
	CFNumberRef width_number = create_cf_number_s32(width);
	CFNumberRef height_number = create_cf_number_s32(height);
	CFNumberRef pixel_format_number = create_cf_number_s32(int32_t(pixel_format));
	CFDictionaryRef io_surface_properties = CFDictionaryCreate(
	        kCFAllocatorDefault,
	        nullptr,
	        nullptr,
	        0,
	        &kCFTypeDictionaryKeyCallBacks,
	        &kCFTypeDictionaryValueCallBacks);

	const void * source_keys[] = {
	        kCVPixelBufferPixelFormatTypeKey,
	        kCVPixelBufferWidthKey,
	        kCVPixelBufferHeightKey,
	        kCVPixelBufferIOSurfacePropertiesKey,
	        kCVPixelBufferMetalCompatibilityKey,
	};
	const void * source_values[] = {
	        pixel_format_number,
	        width_number,
	        height_number,
	        io_surface_properties,
	        kCFBooleanTrue,
	};
	CFDictionaryRef source_attributes = CFDictionaryCreate(
	        kCFAllocatorDefault,
	        source_keys,
	        source_values,
	        5,
	        &kCFTypeDictionaryKeyCallBacks,
	        &kCFTypeDictionaryValueCallBacks);

	CFRelease(io_surface_properties);
	CFRelease(pixel_format_number);
	CFRelease(height_number);
	CFRelease(width_number);
	return source_attributes;
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
	if (!external_alpha_input)
		return;

	const bool log_host_timing = log_apple_host_timing_enabled();
	const int64_t begin_ns = log_host_timing ? os_monotonic_get_ns() : 0;
	prepare_external_alpha_rgba(slot);
	if (log_host_timing)
	{
		const int64_t end_ns = os_monotonic_get_ns();
		fprintf(stderr,
		        "apple-vt frame=%llu stream=%u phase=prepare_external_alpha duration_us=%lld\n",
		        (unsigned long long)frame_index,
		        unsigned(stream_idx),
		        (long long)((end_ns - begin_ns) / 1000));
	}
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
		request->callback_ns = os_monotonic_get_ns();
		request->completed = true;
		request->cv.notify_all();
		return;
	}

	if (info_flags & kVTEncodeInfo_FrameDropped)
	{
		request->dropped = true;
		request->callback_ns = os_monotonic_get_ns();
		request->completed = true;
		request->cv.notify_all();
		return;
	}

	if (sample_buffer == nullptr || !CMSampleBufferDataIsReady(sample_buffer))
	{
		request->error = "VideoToolbox returned an empty sample buffer";
		request->callback_ns = os_monotonic_get_ns();
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

	request->callback_ns = os_monotonic_get_ns();
	request->completed = true;
	request->cv.notify_all();
}

void
video_encoder_videotoolbox::copy_rgba_to_bgra_pixel_buffer(uint8_t slot, CVPixelBufferRef pixel_buffer)
{
	auto * rgba = static_cast<uint8_t *>(in[slot].rgba.map());
	auto * dst = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(pixel_buffer));
	const size_t dst_stride = CVPixelBufferGetBytesPerRow(pixel_buffer);
	const size_t src_stride = size_t(extent.width) * 4;
	for (uint32_t y = 0; y < extent.height; ++y)
	{
		const uint8_t * src_row = rgba + y * src_stride;
		uint8_t * dst_row = dst + y * dst_stride;
		for (uint32_t x = 0; x < extent.width; ++x)
		{
			const uint8_t * src = src_row + x * 4;
			uint8_t * out = dst_row + x * 4;
			out[0] = src[2];
			out[1] = src[1];
			out[2] = src[0];
			out[3] = src[3];
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

CVPixelBufferRef
video_encoder_videotoolbox::create_source_pixel_buffer(OSType pixel_format)
{
	CFDictionaryRef attributes = create_source_attributes(extent.width, extent.height, pixel_format);
	CVPixelBufferRef pixel_buffer = nullptr;
	const OSStatus status =
	        CVPixelBufferCreate(kCFAllocatorDefault, extent.width, extent.height, pixel_format, attributes, &pixel_buffer);
	CFRelease(attributes);
	if (status != noErr || pixel_buffer == nullptr)
		throw std::runtime_error("Failed to create reusable VideoToolbox source pixel buffer: " + osstatus_string(status));
	return pixel_buffer;
}

std::optional<video_encoder::data>
video_encoder_videotoolbox::encode(uint8_t slot, uint64_t frame_index)
{
	const bool log_host_timing = log_apple_host_timing_enabled();
	const int64_t encode_begin_ns = log_host_timing ? os_monotonic_get_ns() : 0;
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

	const int64_t pixel_buffer_begin_ns = log_host_timing ? os_monotonic_get_ns() : 0;
	CVPixelBufferRef pixel_buffer = in[slot].pixel_buffer;
	if (pixel_buffer == nullptr)
		throw std::runtime_error("Missing reusable VideoToolbox source pixel buffer");
	const int64_t pixel_buffer_end_ns = log_host_timing ? os_monotonic_get_ns() : 0;

	OSStatus status = noErr;
	const int64_t source_copy_begin_ns = log_host_timing ? os_monotonic_get_ns() : 0;
	status = CVPixelBufferLockBaseAddress(pixel_buffer, 0);
	if (status != noErr)
		throw std::runtime_error("Failed to lock VideoToolbox pixel buffer: " + osstatus_string(status));
	copy_rgba_to_bgra_pixel_buffer(slot, pixel_buffer);
	const int64_t source_copy_end_ns = log_host_timing ? os_monotonic_get_ns() : 0;
	CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

	auto request = std::make_shared<encode_request>();
	request->frame_index = frame_index;
	request->stream_idx = stream_idx;

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
	const int64_t vt_encode_begin_ns = log_host_timing ? os_monotonic_get_ns() : 0;
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

	if (status != noErr)
		throw std::runtime_error("VTCompressionSessionEncodeFrame failed: " + osstatus_string(status));
	const int64_t vt_encode_end_ns = log_host_timing ? os_monotonic_get_ns() : 0;
	request->encode_submit_ns = vt_encode_end_ns;

	const int64_t complete_begin_ns = log_host_timing ? os_monotonic_get_ns() : 0;
	status = VTCompressionSessionCompleteFrames(session, pts);
	if (status != noErr)
		throw std::runtime_error("VTCompressionSessionCompleteFrames failed: " + osstatus_string(status));
	const int64_t complete_end_ns = log_host_timing ? os_monotonic_get_ns() : 0;

	const int64_t wait_begin_ns = log_host_timing ? os_monotonic_get_ns() : 0;
	{
		std::unique_lock lock(request->mutex);
		request->cv.wait(lock, [&] { return request->completed; });
	}
	const int64_t wait_end_ns = log_host_timing ? os_monotonic_get_ns() : 0;

	if (!request->error.empty())
		throw std::runtime_error(request->error);
	if (request->dropped || request->bitstream.empty())
	{
		if (log_host_timing)
		{
			fprintf(stderr,
			        "apple-vt frame=%llu stream=%u phase=encode_done source=%s dropped=%d bytes=%zu pool_us=%lld source_copy_us=%lld "
			        "encode_frame_us=%lld complete_frames_us=%lld wait_callback_us=%lld submit_to_callback_us=%lld total_us=%lld\n",
			        (unsigned long long)frame_index,
			        unsigned(stream_idx),
			        "bgra",
			        request->dropped ? 1 : 0,
			        request->bitstream.size(),
			        (long long)((pixel_buffer_end_ns - pixel_buffer_begin_ns) / 1000),
			        (long long)((source_copy_end_ns - source_copy_begin_ns) / 1000),
			        (long long)((vt_encode_end_ns - vt_encode_begin_ns) / 1000),
			        (long long)((complete_end_ns - complete_begin_ns) / 1000),
			        (long long)((wait_end_ns - wait_begin_ns) / 1000),
			        (long long)((request->callback_ns - request->encode_submit_ns) / 1000),
			        (long long)((wait_end_ns - encode_begin_ns) / 1000));
		}
		return {};
	}

	if (log_host_timing)
	{
		fprintf(stderr,
		        "apple-vt frame=%llu stream=%u phase=encode_done source=%s dropped=%d bytes=%zu control=%d pool_us=%lld source_copy_us=%lld "
		        "encode_frame_us=%lld complete_frames_us=%lld wait_callback_us=%lld submit_to_callback_us=%lld total_us=%lld\n",
		        (unsigned long long)frame_index,
		        unsigned(stream_idx),
		        "bgra",
		        request->dropped ? 1 : 0,
		        request->bitstream.size(),
		        request->control ? 1 : 0,
		        (long long)((pixel_buffer_end_ns - pixel_buffer_begin_ns) / 1000),
		        (long long)((source_copy_end_ns - source_copy_begin_ns) / 1000),
		        (long long)((vt_encode_end_ns - vt_encode_begin_ns) / 1000),
		        (long long)((complete_end_ns - complete_begin_ns) / 1000),
		        (long long)((wait_end_ns - wait_begin_ns) / 1000),
		        (long long)((request->callback_ns - request->encode_submit_ns) / 1000),
		        (long long)((wait_end_ns - encode_begin_ns) / 1000));
	}

	return data{
	        .encoder = this,
	        .span = std::span<uint8_t>(request->bitstream),
	        .mem = request,
	        .prefer_control = request->control,
	};
}

video_encoder_videotoolbox::~video_encoder_videotoolbox()
{
	for (auto & slot: in)
	{
		if (slot.pixel_buffer != nullptr)
		{
			CFRelease(slot.pixel_buffer);
			slot.pixel_buffer = nullptr;
		}
	}
	if (session != nullptr)
	{
		VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
		VTCompressionSessionInvalidate(session);
		CFRelease(session);
		session = nullptr;
	}
}

} // namespace wivrn
