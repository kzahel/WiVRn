/*
 * WiVRn VR streaming
 * Copyright (C) 2026  OpenAI
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "wivrn_foveation.h"

#include "driver/clock_offset.h"

namespace wivrn
{

wivrn_foveation::wivrn_foveation(wivrn_vk_bundle &, const xrt_hmd_parts & hmd) :
        foveated_width(hmd.views[0].display.w_pixels),
        foveated_height(hmd.views[0].display.h_pixels),
        angle_offset(0.0f),
        convergence_distance(0.0f),
        command_pool(nullptr),
        cmd(nullptr)
{
	for (size_t eye = 0; eye < params.size(); ++eye)
	{
		const size_t view_index = eye < hmd.view_count ? eye : 0;
		params[eye].x = {static_cast<uint16_t>(hmd.views[view_index].display.w_pixels)};
		params[eye].y = {static_cast<uint16_t>(hmd.views[view_index].display.h_pixels)};
	}
}

void
wivrn_foveation::update_tracking(const from_headset::tracking &, const clock_offset &)
{}

void
wivrn_foveation::update_foveation_center_override(const from_headset::override_foveation_center & center)
{
	manual_foveation = center;
}

std::array<to_headset::foveation_parameter, 2>
wivrn_foveation::get_parameters()
{
	return params;
}

vk::Buffer
wivrn_foveation::get_gpu_buffer()
{
	return VK_NULL_HANDLE;
}

vk::CommandBuffer
wivrn_foveation::update_foveation_buffer(vk::Buffer, bool, xrt_rect[2], xrt_fov[2])
{
	return nullptr;
}

} // namespace wivrn
