/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Kyle Graehl
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "audio_coreaudio.h"

#include "audio_pcm_sender.h"
#include "driver/wivrn_session.h"
#include "util/u_logging.h"

#import <CoreGraphics/CGWindow.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <semaphore>
#include <vector>

namespace wivrn::coreaudio
{
class device;
}

@interface WIVRNSCAudioCapture : NSObject <SCStreamDelegate, SCStreamOutput>
- (instancetype)initWithDevice:(wivrn::coreaudio::device *)device
                    sampleRate:(NSInteger)sampleRate
                  channelCount:(NSInteger)channelCount;
- (BOOL)startWithError:(NSError **)error;
- (void)stop;
@end

namespace
{
int16_t float_to_pcm(float sample)
{
	sample = std::clamp(sample, -1.0f, 1.0f);
	return static_cast<int16_t>(std::lrintf(sample * 32767.0f));
}

std::string ns_error_string(NSError * error)
{
	if (!error)
		return "unknown error";
	return std::string([[error localizedDescription] UTF8String]);
}
} // namespace

namespace wivrn::coreaudio
{
class device : public audio_device
{
public:
	device(const wivrn::from_headset::headset_info_packet & info, wivrn_session & session) :
	        session(session),
	        speaker_sender(session, info.speaker->sample_rate, info.speaker->num_channels)
	{
		desc.speaker = {
		        .num_channels = info.speaker->num_channels,
		        .sample_rate = info.speaker->sample_rate,
		};
		capture = [[WIVRNSCAudioCapture alloc] initWithDevice:this
		                                          sampleRate:info.speaker->sample_rate
		                                        channelCount:info.speaker->num_channels];
	}

	~device() override
	{
		pause();
	}

	to_headset::audio_stream_description description() const override
	{
		return desc;
	}

	void process_mic_data(wivrn::audio_data &&) override
	{
	}

	void pause() override
	{
		std::lock_guard lock(state_mutex);
		speaker_sender.pause();
		if (running && capture)
		{
			[capture stop];
			running = false;
		}
	}

	void resume() override
	{
		std::lock_guard lock(state_mutex);
		if (!capture)
			return;
		if (!running)
		{
			NSError * error = nil;
			if (![capture startWithError:&error])
			{
				U_LOG_W("CoreAudio backend failed to start: %s", ns_error_string(error).c_str());
				return;
			}
			running = true;
		}
		speaker_sender.resume();
		session.send_control(description());
	}

	void handle_audio_sample(CMSampleBufferRef sample_buffer)
	{
		if (!sample_buffer || !CMSampleBufferIsValid(sample_buffer))
			return;

		auto format = CMSampleBufferGetFormatDescription(sample_buffer);
		if (!format)
			return;

		const auto * asbd = CMAudioFormatDescriptionGetStreamBasicDescription(format);
		if (!asbd || asbd->mFormatID != kAudioFormatLinearPCM)
			return;

		int32_t frames = CMSampleBufferGetNumSamples(sample_buffer);
		if (frames <= 0 || asbd->mChannelsPerFrame == 0)
			return;

		size_t buffer_list_size = 0;
		OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
		        sample_buffer,
		        &buffer_list_size,
		        nullptr,
		        0,
		        kCFAllocatorDefault,
		        kCFAllocatorDefault,
		        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
		        nullptr);
		if (status != noErr || buffer_list_size == 0)
			return;

		std::vector<uint8_t> audio_buffer_list_storage(buffer_list_size);
		auto * audio_buffer_list = reinterpret_cast<AudioBufferList *>(audio_buffer_list_storage.data());
		CMBlockBufferRef block_buffer = nullptr;
		status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
		        sample_buffer,
		        nullptr,
		        audio_buffer_list,
		        buffer_list_size,
		        kCFAllocatorDefault,
		        kCFAllocatorDefault,
		        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
		        &block_buffer);
		if (status != noErr)
			return;

		if (block_buffer)
			CFRelease(block_buffer);

		size_t samples = size_t(frames) * asbd->mChannelsPerFrame;
		thread_local std::vector<int16_t> converted;
		converted.resize(samples);

		bool is_float = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
		bool is_signed_int = (asbd->mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0;
		bool is_non_interleaved = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;

		if (is_float)
		{
			if (is_non_interleaved)
			{
				for (uint32_t channel = 0; channel < asbd->mChannelsPerFrame; ++channel)
				{
					const float * src = static_cast<const float *>(audio_buffer_list->mBuffers[channel].mData);
					for (int32_t frame = 0; frame < frames; ++frame)
						converted[size_t(frame) * asbd->mChannelsPerFrame + channel] = float_to_pcm(src[frame]);
				}
			}
			else
			{
				const float * src = static_cast<const float *>(audio_buffer_list->mBuffers[0].mData);
				for (size_t i = 0; i < samples; ++i)
					converted[i] = float_to_pcm(src[i]);
			}
		}
		else if (is_signed_int && asbd->mBitsPerChannel == 16)
		{
			if (is_non_interleaved)
			{
				for (uint32_t channel = 0; channel < asbd->mChannelsPerFrame; ++channel)
				{
					const int16_t * src = static_cast<const int16_t *>(audio_buffer_list->mBuffers[channel].mData);
					for (int32_t frame = 0; frame < frames; ++frame)
						converted[size_t(frame) * asbd->mChannelsPerFrame + channel] = src[frame];
				}
			}
			else
			{
				const auto * src = static_cast<const int16_t *>(audio_buffer_list->mBuffers[0].mData);
				std::copy_n(src, samples, converted.begin());
			}
		}
		else
		{
			static bool warned = false;
			if (!warned)
			{
				U_LOG_W("Unsupported ScreenCaptureKit audio format (format flags: 0x%x, bits: %u)",
				        unsigned(asbd->mFormatFlags),
				        unsigned(asbd->mBitsPerChannel));
				warned = true;
			}
			return;
		}

		speaker_sender.push_pcm_s16(std::span(
		        reinterpret_cast<const uint8_t *>(converted.data()),
		        converted.size() * sizeof(int16_t)));
	}

	void handle_capture_error(NSError * error)
	{
		U_LOG_W("CoreAudio capture stopped: %s", ns_error_string(error).c_str());
		std::lock_guard lock(state_mutex);
		running = false;
		speaker_sender.pause();
	}

private:
	to_headset::audio_stream_description desc;
	wivrn_session & session;
	audio_pcm_sender speaker_sender;
	std::mutex state_mutex;
	bool running = false;
	WIVRNSCAudioCapture * __strong capture = nil;
};
} // namespace wivrn::coreaudio

@implementation WIVRNSCAudioCapture
{
	wivrn::coreaudio::device * _device;
	NSInteger _sampleRate;
	NSInteger _channelCount;
	SCStream * _stream;
	dispatch_queue_t _queue;
}

- (instancetype)initWithDevice:(wivrn::coreaudio::device *)device
                    sampleRate:(NSInteger)sampleRate
                  channelCount:(NSInteger)channelCount
{
	self = [super init];
	if (self)
	{
		_device = device;
		_sampleRate = sampleRate;
		_channelCount = channelCount;
		dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
		_queue = dispatch_queue_create("io.github.wivrn.audio.capture", attr);
	}
	return self;
}

- (BOOL)startWithError:(NSError **)error
{
	if (_stream)
		return YES;

	if (@available(macOS 13.0, *))
	{
		if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess())
		{
			if (error)
			{
				*error = [NSError errorWithDomain:@"WiVRn"
				                             code:1
				                         userInfo:@{NSLocalizedDescriptionKey: @"Screen Recording permission is required to capture system audio."}];
			}
			return NO;
		}

		__block SCShareableContent * shareable_content = nil;
		__block NSError * shareable_error = nil;
		dispatch_semaphore_t shareable_semaphore = dispatch_semaphore_create(0);
		[SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent * _Nullable content, NSError * _Nullable content_error) {
			shareable_content = content;
			shareable_error = content_error;
			dispatch_semaphore_signal(shareable_semaphore);
		}];
		dispatch_semaphore_wait(shareable_semaphore, DISPATCH_TIME_FOREVER);

		if (!shareable_content)
		{
			if (error)
				*error = shareable_error;
			return NO;
		}

		SCDisplay * display = shareable_content.displays.firstObject;
		if (!display)
		{
			if (error)
			{
				*error = [NSError errorWithDomain:@"WiVRn"
				                             code:2
				                         userInfo:@{NSLocalizedDescriptionKey: @"No shareable display available for ScreenCaptureKit audio capture."}];
			}
			return NO;
		}

		SCContentFilter * filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
		SCStreamConfiguration * config = [[SCStreamConfiguration alloc] init];
		config.width = 64;
		config.height = 64;
		config.minimumFrameInterval = CMTimeMake(1, 1);
		config.queueDepth = 1;
		config.capturesAudio = YES;
		config.excludesCurrentProcessAudio = YES;
		config.sampleRate = _sampleRate;
		config.channelCount = _channelCount;
		config.showsCursor = NO;

		_stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:self];
		if (!_stream)
		{
			if (error)
			{
				*error = [NSError errorWithDomain:@"WiVRn"
				                             code:3
				                         userInfo:@{NSLocalizedDescriptionKey: @"Failed to create ScreenCaptureKit stream."}];
			}
			return NO;
		}

		NSError * output_error = nil;
		if (![_stream addStreamOutput:self type:SCStreamOutputTypeAudio sampleHandlerQueue:_queue error:&output_error])
		{
			if (error)
				*error = output_error;
			_stream = nil;
			return NO;
		}

		__block NSError * start_error = nil;
		dispatch_semaphore_t start_semaphore = dispatch_semaphore_create(0);
		[_stream startCaptureWithCompletionHandler:^(NSError * _Nullable capture_error) {
			start_error = capture_error;
			dispatch_semaphore_signal(start_semaphore);
		}];
		dispatch_semaphore_wait(start_semaphore, DISPATCH_TIME_FOREVER);

		if (start_error)
		{
			if (error)
				*error = start_error;
			_stream = nil;
			return NO;
		}

		return YES;
	}

	if (error)
	{
		*error = [NSError errorWithDomain:@"WiVRn"
		                             code:4
		                         userInfo:@{NSLocalizedDescriptionKey: @"System audio capture requires macOS 13.0 or newer."}];
	}
	return NO;
}

- (void)stop
{
	if (!_stream)
		return;

	SCStream * stream = _stream;
	_stream = nil;

	dispatch_semaphore_t stop_semaphore = dispatch_semaphore_create(0);
	[stream stopCaptureWithCompletionHandler:^(__unused NSError * _Nullable error) {
		dispatch_semaphore_signal(stop_semaphore);
	}];
	dispatch_semaphore_wait(stop_semaphore, DISPATCH_TIME_FOREVER);
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
	if (!_device || stream != _stream || type != SCStreamOutputTypeAudio)
		return;
	_device->handle_audio_sample(sampleBuffer);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
	if (stream == _stream)
		_stream = nil;
	if (_device)
		_device->handle_capture_error(error);
}
@end

std::unique_ptr<wivrn::audio_device> wivrn::create_coreaudio_handle(
        const std::string &,
        const std::string &,
        const std::string &,
        const std::string &,
        const wivrn::from_headset::headset_info_packet & info,
        wivrn::wivrn_session & session)
{
	if (!info.speaker)
		return nullptr;

	try
	{
		return std::make_unique<wivrn::coreaudio::device>(info, session);
	}
	catch (const std::exception & e)
	{
		U_LOG_I("CoreAudio backend creation failed: %s", e.what());
		return nullptr;
	}
}
