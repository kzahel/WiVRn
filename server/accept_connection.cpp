/*
 * WiVRn VR streaming
 * Copyright (C) 2022-2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022-2024  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "accept_connection.h"

#include "util/u_logging.h"
#include "utils/overloaded.h"

#include "driver/configuration.h"
#include "driver/wivrn_session.h"
#include "wivrn_ipc.h"
#include "wivrn_sockets.h"

#if defined(_WIN32)
using accept_pollfd = WSAPOLLFD;

static int
poll_accept_fds(accept_pollfd * fds, size_t count, int timeout_ms)
{
	return WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
}
#else
#include <fcntl.h>
#include <sys/poll.h>
using accept_pollfd = pollfd;

static int
poll_accept_fds(accept_pollfd * fds, size_t count, int timeout_ms)
{
	return poll(fds, count, timeout_ms);
}
#endif

extern "C" int headset_listen_socket = -1;

std::unique_ptr<wivrn::TCP> wivrn::accept_connection(wivrn_session & cnx, std::stop_token stop, std::function<void(wivrn_session &)> tick)
{
	wivrn_ipc_socket_monado->send(from_monado::headset_disconnected{});

	wivrn::TCPListener listener;
	int listen_fd = headset_listen_socket;
	if (listen_fd < 0)
	{
		listener = wivrn::TCPListener(configuration().port);
		listen_fd = listener.get_fd();
	}

	accept_pollfd fds[2]{};
	fds[0].fd = static_cast<decltype(accept_pollfd{}.fd)>(listen_fd);
	fds[0].events = POLLIN;
	fds[1].fd = static_cast<decltype(accept_pollfd{}.fd)>(wivrn_ipc_socket_monado->get_fd());
	fds[1].events = POLLIN;

	while (not stop.stop_requested())
	{
		if (poll_accept_fds(fds, std::size(fds), 100) < 0)
		{
			perror("poll");
			return {};
		}

		if (fds[0].revents & POLLIN)
		{
			wivrn_ipc_socket_monado->send(from_monado::headset_connected{});
			sockaddr_in6 addr{};
			socklen_t addrlen = sizeof(addr);
			int fd = ::accept(listen_fd, (sockaddr *)&addr, &addrlen);
			if (fd < 0)
#if defined(_WIN32)
				throw std::system_error{WSAGetLastError(), std::system_category()};
#else
				throw std::system_error{errno, std::generic_category()};
			fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
			return std::make_unique<wivrn::TCP>(fd);
		}

		if (fds[1].revents & POLLIN)
		{
			auto packet = receive_from_main();
			if (packet)
				std::visit(utils::overloaded{
				                   [&cnx](to_monado::stop) {
					                   // gets handled in wivrn_session::reconnect since we return nullptr
					                   U_LOG_I("Received stop packet during reconnect, stopping");
					                   cnx.request_stop();
				                   },
				                   [](auto &&) {
					                   // Ignore request when no headset is connected
				                   },
				           },
				           *packet);
		}

		if (tick)
			tick(cnx);
	}

	return {};
}
