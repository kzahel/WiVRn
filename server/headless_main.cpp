/*
 * WiVRn VR streaming
 * Copyright (C) 2026  OpenAI
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "driver/configuration.h"
#include "driver/wivrn_connection.h"
#include "ipc_server_cb.h"
#include "utils/overloaded.h"
#include "util/u_trace_marker.h"
#include "version.h"
#include "wivrn_ipc.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include "server/ipc_server_interface.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <poll.h>
#include <random>
#include <stop_token>
#include <string_view>
#include <sys/socket.h>
#include <thread>

extern "C"
{
	int listen_socket = -1;
	extern int headset_listen_socket;
}

std::optional<wivrn::typed_socket<wivrn::UnixDatagram, to_monado::packets, from_monado::packets>> wivrn_ipc_socket_monado;

namespace
{
std::optional<wivrn::typed_socket<wivrn::UnixDatagram, from_monado::packets, to_monado::packets>> wivrn_ipc_socket_headless;
int control_pipe_fds[2] = {-1, -1};
std::unique_ptr<wivrn::TCPListener> headset_listener;

void
print_usage(const char * argv0)
{
	std::cerr << "Usage: " << argv0 << " [--config FILE] [--no-encrypt] [--help]\n";
}

bool
parse_args(int argc, char * argv[], std::optional<std::filesystem::path> & config_file, bool & no_encrypt)
{
	for (int i = 1; i < argc; ++i)
	{
		std::string_view arg = argv[i];
		if (arg == "--help" || arg == "-h")
		{
			print_usage(argv[0]);
			return false;
		}

		if (arg == "--no-encrypt")
		{
			no_encrypt = true;
			continue;
		}

		if ((arg == "--config" || arg == "-f") && i + 1 < argc)
		{
			config_file = argv[++i];
			continue;
		}

		std::cerr << "Unknown argument: " << arg << "\n";
		print_usage(argv[0]);
		return false;
	}

	return true;
}

std::string
generate_pairing_pin()
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> distrib(0, 999999);

	char buffer[7];
	snprintf(buffer, sizeof(buffer), "%06d", distrib(gen));
	return buffer;
}

wivrn::wivrn_connection::encryption_state
choose_encryption_state(bool no_encrypt)
{
	if (no_encrypt)
		return wivrn::wivrn_connection::encryption_state::disabled;
	if (wivrn::known_keys().empty())
		return wivrn::wivrn_connection::encryption_state::pairing;
	return wivrn::wivrn_connection::encryption_state::enabled;
}

void
log_control_packet(const from_monado::packets & packet)
{
	std::visit(
	        utils::overloaded{
	                [](const wivrn::from_headset::headset_info_packet & info) {
		                std::cerr << "Headset connected: system='" << info.system_name << "' refresh-rates=" << info.available_refresh_rates.size() << "\n";
	                },
	                [](const wivrn::from_headset::settings_changed & settings) {
		                std::cerr << "Settings changed: fps=" << settings.preferred_refresh_rate << " bitrate=" << settings.bitrate_bps << "\n";
	                },
	                [](const wivrn::from_headset::start_app & request) {
		                std::cerr << "Headset requested app launch: " << request.app_id << " (ignored by headless host)\n";
	                },
	                [](const wivrn::from_headset::stream_tab_changed &) {
		                std::cerr << "Client stream tab changed\n";
	                },
	                [](const from_monado::headset_connected &) {
		                std::cerr << "Monado-side session reported headset connected\n";
	                },
	                [](const from_monado::headset_disconnected &) {
		                std::cerr << "Monado-side session reported headset disconnected\n";
	                },
	                [](const from_monado::server_error & err) {
		                std::cerr << "Server error from session: " << err.where << ": " << err.message << "\n";
	                },
	        },
	        packet);
}

void
drain_control_packets(std::stop_token stop)
{
	pollfd fd{
	        .fd = wivrn_ipc_socket_headless->get_fd(),
	        .events = POLLIN,
	        .revents = 0,
	};

	while (!stop.stop_requested())
	{
		if (poll(&fd, 1, 100) < 0)
		{
			perror("poll");
			return;
		}

		if (fd.revents & POLLIN)
		{
			auto packet = wivrn_ipc_socket_headless->receive();
			if (packet)
				log_control_packet(*packet);
		}
	}
}

void
configure_server_environment()
{
	setenv("XRT_COMPOSITOR_SCALE_PERCENTAGE", "100", true);
	setenv("XRT_COMPOSITOR_COMPUTE", "1", true);
	setenv("AMD_DEBUG", "lowlatencyenc", false);
	setenv("INTEL_DEBUG", "noccs", false);
}

int
wait_for_initial_connection(std::stop_token stop_token, wivrn::wivrn_connection::encryption_state state, const std::string & pin)
{
	wivrn::configuration config;
	headset_listener = std::make_unique<wivrn::TCPListener>(config.port);
	headset_listen_socket = headset_listener->get_fd();

	while (!stop_token.stop_requested())
	{
		try
		{
			std::cerr << "Waiting for initial headset connection on TCP port " << config.port << "\n";
			auto tcp = headset_listener->accept().first;
			connection = std::make_unique<wivrn::wivrn_connection>(stop_token, state, pin, std::move(tcp));
			std::cerr << "Initial headset handshake completed\n";
			return EXIT_SUCCESS;
		}
		catch (const wivrn::incorrect_pin &)
		{
			std::cerr << "Incorrect pairing PIN, waiting for another connection attempt\n";
		}
		catch (const std::exception & e)
		{
			std::cerr << "Initial headset connection failed: " << e.what() << "\n";
		}
	}

	return EXIT_FAILURE;
}
} // namespace

int
main(int argc, char * argv[])
{
	std::optional<std::filesystem::path> config_file;
	bool no_encrypt = false;
	if (!parse_args(argc, argv, config_file, no_encrypt))
		return EXIT_SUCCESS;

	if (config_file)
		wivrn::configuration::set_config_file(*config_file);

	std::cerr << "WiVRn " << wivrn::display_version() << " headless host starting\n";

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, control_pipe_fds) < 0)
	{
		perror("socketpair");
		return EXIT_FAILURE;
	}

	wivrn_ipc_socket_headless.emplace(control_pipe_fds[0]);
	wivrn_ipc_socket_monado.emplace(control_pipe_fds[1]);

	std::jthread control_thread(drain_control_packets);

	u_trace_marker_init();
	init_cleanup_functions();

	auto state = choose_encryption_state(no_encrypt);
	std::string pin;
	if (state == wivrn::wivrn_connection::encryption_state::pairing)
	{
		pin = generate_pairing_pin();
		std::cerr << "Headset pairing enabled. PIN: " << pin << "\n";
	}
	else if (state == wivrn::wivrn_connection::encryption_state::disabled)
	{
		std::cerr << "Encryption disabled for headless host\n";
	}

	std::stop_source stop_source;
	if (wait_for_initial_connection(stop_source.get_token(), state, pin) != EXIT_SUCCESS)
		return EXIT_FAILURE;

	configure_server_environment();

	wivrn::ipc_server_cb server_cb;
	ipc_server_main_info server_info{
	        .udgci =
	                {
	                        .window_title = "WiVRn Headless",
	                        .open = U_DEBUG_GUI_OPEN_NEVER,
	                },
	        .exit_on_disconnect = false,
	        .no_stdin = true,
	};

	try
	{
		return ipc_server_main_common(&server_info, &server_cb, nullptr);
	}
	catch (const std::exception & e)
	{
		std::cerr << "Headless host failed: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
