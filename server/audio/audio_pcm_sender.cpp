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

#include "audio_pcm_sender.h"

#include "driver/wivrn_session.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include <algorithm>
#include <cstring>

namespace
{
constexpr size_t packet_duration_ms = 5;
constexpr size_t max_buffered_packets = 20;
}

wivrn::audio_pcm_sender::audio_pcm_sender(wivrn_session & session, uint32_t sample_rate, uint8_t num_channels) :
        session(session)
{
	size_t packet_frames = std::max<size_t>(1, sample_rate * packet_duration_ms / 1000);
	packet_bytes = packet_frames * num_channels * sizeof(int16_t);
	max_buffer_bytes = packet_bytes * max_buffered_packets;
	thread = std::jthread([this](std::stop_token stop_token) { run(stop_token); });
}

wivrn::audio_pcm_sender::~audio_pcm_sender()
{
	thread.request_stop();
	cv.notify_all();
}

void wivrn::audio_pcm_sender::push_pcm_s16(std::span<const uint8_t> pcm)
{
	if (pcm.empty())
		return;

	std::lock_guard lock(mutex);
	if (paused)
		return;

	pending.insert(pending.end(), pcm.begin(), pcm.end());
	trim_locked();
	cv.notify_one();
}

void wivrn::audio_pcm_sender::pause()
{
	std::lock_guard lock(mutex);
	paused = true;
	pending.clear();
	read_offset = 0;
}

void wivrn::audio_pcm_sender::resume()
{
	std::lock_guard lock(mutex);
	paused = false;
	cv.notify_one();
}

void wivrn::audio_pcm_sender::run(std::stop_token stop_token)
{
	while (true)
	{
		std::vector<uint8_t> packet;
		{
			std::unique_lock lock(mutex);
			cv.wait(lock, [&]() {
				return stop_token.stop_requested() ||
				       (!paused && pending.size() - read_offset >= packet_bytes);
			});

			if (stop_token.stop_requested())
				return;

			packet.resize(packet_bytes);
			std::memcpy(packet.data(), pending.data() + read_offset, packet_bytes);
			read_offset += packet_bytes;
			trim_locked();
		}

		try
		{
			session.send_control(audio_data{
			        .timestamp = session.get_offset().to_headset(os_monotonic_get_ns()),
			        .payload = std::span(packet),
			});
		}
		catch (const std::exception & e)
		{
			U_LOG_D("Failed to send audio data: %s", e.what());
		}
	}
}

void wivrn::audio_pcm_sender::trim_locked()
{
	size_t buffered = pending.size() - read_offset;
	if (buffered > max_buffer_bytes)
	{
		size_t excess = buffered - max_buffer_bytes;
		size_t drop = excess - excess % packet_bytes;
		if (drop == 0)
			drop = packet_bytes;
		read_offset += drop;
		U_LOG_D("Audio sync: discard %zu bytes", drop);
	}

	if (read_offset == pending.size())
	{
		pending.clear();
		read_offset = 0;
		return;
	}

	if (read_offset >= packet_bytes * 4 && read_offset * 2 >= pending.size())
	{
		pending.erase(pending.begin(), pending.begin() + read_offset);
		read_offset = 0;
	}
}
