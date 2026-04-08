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

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace wivrn
{
class wivrn_session;

// Packetizes captured PCM into fixed-size low-latency audio_data packets so
// platform backends only need to provide interleaved S16 samples.
class audio_pcm_sender
{
public:
	audio_pcm_sender(wivrn_session & session, uint32_t sample_rate, uint8_t num_channels);
	~audio_pcm_sender();

	void push_pcm_s16(std::span<const uint8_t> pcm);
	void pause();
	void resume();

private:
	void run(std::stop_token stop_token);
	void trim_locked();

	wivrn_session & session;
	size_t packet_bytes = 0;
	size_t max_buffer_bytes = 0;

	std::mutex mutex;
	std::condition_variable cv;
	std::vector<uint8_t> pending;
	size_t read_offset = 0;
	bool paused = true;
	std::jthread thread;
};
} // namespace wivrn
