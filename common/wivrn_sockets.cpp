/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "wivrn_sockets.h"

#include "crypto.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <memory>
#include <string.h>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <cstdlib>
#include <mstcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace
{

int
last_socket_error()
{
#if defined(_WIN32)
	return WSAGetLastError();
#else
	return errno;
#endif
}

[[noreturn]] void
throw_last_socket_error()
{
	throw std::system_error(last_socket_error(), std::system_category());
}

void
close_socket(int fd)
{
#if defined(_WIN32)
	closesocket(static_cast<SOCKET>(fd));
#else
	::close(fd);
#endif
}

void
set_close_on_exec(int fd)
{
#if !defined(_WIN32)
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#else
	(void)fd;
#endif
}

template <typename T>
int
set_socket_option(int fd, int level, int option, const T & value)
{
#if defined(_WIN32)
	return setsockopt(fd, level, option, reinterpret_cast<const char *>(&value), sizeof(value));
#else
	return setsockopt(fd, level, option, &value, sizeof(value));
#endif
}

#if defined(_WIN32)

int
map_wsa_error_to_errno(int error)
{
	switch (error)
	{
		case WSAEWOULDBLOCK:
			return EWOULDBLOCK;
		case WSAECONNRESET:
			return ECONNRESET;
		case WSAECONNABORTED:
			return ECONNABORTED;
		case WSAEINTR:
			return EINTR;
		case WSAEINVAL:
			return EINVAL;
		case WSAEMSGSIZE:
			return EMSGSIZE;
		case WSAENOTCONN:
			return ENOTCONN;
		default:
			return EIO;
	}
}

void
set_errno_from_wsa(int error)
{
	errno = map_wsa_error_to_errno(error);
}

void
ensure_socket_runtime()
{
	static std::once_flag once;
	std::call_once(once, []() {
		WSADATA data{};
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
			throw_last_socket_error();
	});
}

class nonblocking_socket_guard
{
public:
	nonblocking_socket_guard(int fd, int flags) :
	        fd(fd),
	        active((flags & MSG_DONTWAIT) != 0)
	{
		if (!active)
			return;

		u_long enabled = 1;
		if (ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &enabled) == SOCKET_ERROR)
		{
			active = false;
			throw_last_socket_error();
		}
	}

	~nonblocking_socket_guard()
	{
		if (!active)
			return;

		u_long disabled = 0;
		ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &disabled);
	}

private:
	int fd;
	bool active;
};

int
recvmsg(int fd, msghdr * msg, int flags)
{
	nonblocking_socket_guard guard(fd, flags);

	std::vector<WSABUF> buffers(msg->msg_iovlen);
	for (size_t i = 0; i < msg->msg_iovlen; ++i)
	{
		buffers[i].buf = static_cast<char *>(msg->msg_iov[i].iov_base);
		buffers[i].len = static_cast<ULONG>(msg->msg_iov[i].iov_len);
	}

	DWORD bytes_received = 0;
	DWORD recv_flags = 0;
	if ((flags & MSG_PEEK) != 0)
		recv_flags |= MSG_PEEK;

	int result = 0;
	if (msg->msg_name)
	{
		int name_length = msg->msg_namelen;
		result = WSARecvFrom(static_cast<SOCKET>(fd),
		                     buffers.data(),
		                     static_cast<DWORD>(buffers.size()),
		                     &bytes_received,
		                     &recv_flags,
		                     static_cast<sockaddr *>(msg->msg_name),
		                     &name_length,
		                     nullptr,
		                     nullptr);
		msg->msg_namelen = name_length;
	}
	else
	{
		result = WSARecv(static_cast<SOCKET>(fd),
		                 buffers.data(),
		                 static_cast<DWORD>(buffers.size()),
		                 &bytes_received,
		                 &recv_flags,
		                 nullptr,
		                 nullptr);
	}

	if (result == SOCKET_ERROR)
	{
		set_errno_from_wsa(WSAGetLastError());
		return -1;
	}

	msg->msg_flags = static_cast<int>(recv_flags);
	return static_cast<int>(bytes_received);
}

int
sendmsg(int fd, const msghdr * msg, int)
{
	std::vector<WSABUF> buffers(msg->msg_iovlen);
	for (size_t i = 0; i < msg->msg_iovlen; ++i)
	{
		buffers[i].buf = static_cast<char *>(msg->msg_iov[i].iov_base);
		buffers[i].len = static_cast<ULONG>(msg->msg_iov[i].iov_len);
	}

	DWORD bytes_sent = 0;
	int result = 0;
	if (msg->msg_name)
	{
		result = WSASendTo(static_cast<SOCKET>(fd),
		                   buffers.data(),
		                   static_cast<DWORD>(buffers.size()),
		                   &bytes_sent,
		                   0,
		                   static_cast<const sockaddr *>(msg->msg_name),
		                   msg->msg_namelen,
		                   nullptr,
		                   nullptr);
	}
	else
	{
		result = WSASend(static_cast<SOCKET>(fd),
		                 buffers.data(),
		                 static_cast<DWORD>(buffers.size()),
		                 &bytes_sent,
		                 0,
		                 nullptr,
		                 nullptr);
	}

	if (result == SOCKET_ERROR)
	{
		set_errno_from_wsa(WSAGetLastError());
		return -1;
	}

	return static_cast<int>(bytes_sent);
}

ssize_t
writev(int fd, const iovec * iov, int iovcnt)
{
	msghdr header{
	        .msg_name = nullptr,
	        .msg_namelen = 0,
	        .msg_iov = const_cast<iovec *>(iov),
	        .msg_iovlen = static_cast<size_t>(iovcnt),
	        .msg_control = nullptr,
	        .msg_controllen = 0,
	        .msg_flags = 0,
	};

	return sendmsg(fd, &header, 0);
}

size_t
peek_next_datagram_size(int fd)
{
	u_long bytes_available = 0;
	if (ioctlsocket(static_cast<SOCKET>(fd), FIONREAD, &bytes_available) == SOCKET_ERROR)
		throw_last_socket_error();
	return static_cast<size_t>(bytes_available);
}

#endif

void
mark_unreachable()
{
#if defined(_MSC_VER)
	__assume(0);
#else
	__builtin_unreachable();
#endif
}

} // namespace

#if defined(__APPLE__) || defined(_WIN32)
#ifndef IPV6_ADD_MEMBERSHIP
#define IPV6_ADD_MEMBERSHIP IPV6_JOIN_GROUP
#endif
#ifndef IPV6_DROP_MEMBERSHIP
#define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP
#endif

static int
recvmmsg(int fd, struct mmsghdr * msgvec, unsigned int vlen, int flags, struct timespec *)
{
	unsigned int received = 0;

	for (; received < vlen; ++received)
	{
		ssize_t result = recvmsg(fd, &msgvec[received].msg_hdr, flags);
		if (result < 0)
		{
			if (received > 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				return static_cast<int>(received);
			return -1;
		}

		if (result == 0)
			break;

		msgvec[received].msg_len = static_cast<unsigned int>(result);
	}

	return static_cast<int>(received);
}

static int
sendmmsg(int fd, struct mmsghdr * msgvec, unsigned int vlen, int flags)
{
	unsigned int sent = 0;

	for (; sent < vlen; ++sent)
	{
		ssize_t result = sendmsg(fd, &msgvec[sent].msg_hdr, flags);
		if (result < 0)
		{
			if (sent > 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				return static_cast<int>(sent);
			return -1;
		}

		msgvec[sent].msg_len = static_cast<unsigned int>(result);
	}

	return static_cast<int>(sent);
}
#endif

thread_local crypto::encrypt_context wivrn::UDP::encrypter{EVP_aes_128_ctr()};
std::atomic<uint64_t> wivrn::UDP::iv_counter;

const char * wivrn::invalid_packet::what() const noexcept
{
	return "Invalid packet";
}

const char * wivrn::socket_shutdown::what() const noexcept
{
	return "Socket shutdown";
}

wivrn::fd_base::fd_base(wivrn::fd_base && other) :
        fd(other.fd)
{
	other.fd = -1;
}

wivrn::fd_base & wivrn::fd_base::operator=(wivrn::fd_base && other)
{
	std::swap(fd, other.fd);
	return *this;
}

wivrn::fd_base::~fd_base()
{
	if (fd >= 0)
		close_socket(fd);
}

wivrn::UDP::UDP()
{
#if defined(_WIN32)
	ensure_socket_runtime();
#endif
	fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0)
		throw_last_socket_error();
	set_close_on_exec(fd);
}

wivrn::UDP::UDP(int fd)
{
	this->fd = fd;
}

void wivrn::UDP::bind(sockaddr_in6 address)
{
	if (::bind(fd, (sockaddr *)&address, sizeof(address)) < 0)
		throw_last_socket_error();
}

void wivrn::UDP::connect(in6_addr address, int port)
{
	sockaddr_in6 sa;
	sa.sin6_family = AF_INET6;
	sa.sin6_addr = address;
	sa.sin6_port = htons(port);

	if (::connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
		throw_last_socket_error();
}

void wivrn::UDP::connect(in_addr address, int port)
{
	sockaddr_in sa;
	sa.sin_family = AF_INET;
	sa.sin_addr = address;
	sa.sin_port = htons(port);

	if (::connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
		throw_last_socket_error();
}

void wivrn::UDP::subscribe_multicast(in6_addr address)
{
	assert(IN6_IS_ADDR_MULTICAST(&address));

	ipv6_mreq subscribe{};
	subscribe.ipv6mr_multiaddr = address;

	if (set_socket_option(fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, subscribe) < 0)
	{
		close_socket(fd);
		throw_last_socket_error();
	}
}

void wivrn::UDP::unsubscribe_multicast(in6_addr address)
{
	assert(IN6_IS_ADDR_MULTICAST(&address));

	ipv6_mreq subscribe{};
	subscribe.ipv6mr_multiaddr = address;

	if (set_socket_option(fd, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, subscribe) < 0)
	{
		close_socket(fd);
		throw_last_socket_error();
	}
}

void wivrn::UDP::set_receive_buffer_size(int size)
{
	set_socket_option(fd, SOL_SOCKET, SO_RCVBUF, size);
}

void wivrn::UDP::set_send_buffer_size(int size)
{
	set_socket_option(fd, SOL_SOCKET, SO_SNDBUF, size);
}

void wivrn::UDP::set_tos(int tos)
{
	int err = set_socket_option(fd, IPPROTO_IP, IP_TOS, tos);
	if (err == -1)
		throw_last_socket_error();
}

void wivrn::TCP::init()
{
	int nodelay = 1;
	if (set_socket_option(fd, IPPROTO_TCP, TCP_NODELAY, nodelay) < 0)
	{
		close_socket(fd);
		throw_last_socket_error();
	}

	mutex = std::make_unique<std::mutex>();
}

wivrn::TCP::TCP(int fd)
{
	this->fd = fd;

	init();
}

wivrn::TCP::TCP(in6_addr address, int port)
{
#if defined(_WIN32)
	ensure_socket_runtime();
#endif
	fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (fd < 0)
		throw_last_socket_error();
	set_close_on_exec(fd);

	sockaddr_in6 sa;
	sa.sin6_family = AF_INET6;
	sa.sin6_addr = address;
	sa.sin6_port = htons(port);

	if (connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
	{
		close_socket(fd);
		throw_last_socket_error();
	}

	init();
}

wivrn::TCP::TCP(in_addr address, int port)
{
#if defined(_WIN32)
	ensure_socket_runtime();
#endif
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		throw_last_socket_error();
	set_close_on_exec(fd);

	sockaddr_in sa;
	sa.sin_family = AF_INET;
	sa.sin_addr = address;
	sa.sin_port = htons(port);

	if (connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
	{
		close_socket(fd);
		throw_last_socket_error();
	}

	init();
}

wivrn::TCPListener::TCPListener(int port)
{
#if defined(_WIN32)
	ensure_socket_runtime();
#endif
	fd = socket(AF_INET6, SOCK_STREAM, 0);

	if (fd < 0)
	{
		throw_last_socket_error();
	}

	int reuse_addr = 1;
	if (set_socket_option(fd, SOL_SOCKET, SO_REUSEADDR, reuse_addr) < 0)
	{
		close_socket(fd);
		throw_last_socket_error();
	}

	sockaddr_in6 addr{};
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(port);
	addr.sin6_addr = in6addr_any;

	if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
	{
		close_socket(fd);
		throw_last_socket_error();
	}

	int backlog = 1;
	if (listen(fd, backlog) < 0)
	{
		close_socket(fd);
		throw_last_socket_error();
	}
}

std::pair<wivrn::deserialization_packet, sockaddr_in6> wivrn::UDP::receive_from_raw()
{
	sockaddr_in6 addr;
	socklen_t addrlen = sizeof(addr);

#if defined(_WIN32)
	size_t size = peek_next_datagram_size(fd);
#else
	size_t size = recvfrom(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC, (sockaddr *)&addr, &addrlen);
#endif

#if defined(__cpp_lib_smart_ptr_for_overwrite) && __cpp_lib_smart_ptr_for_overwrite >= 202002L
	auto buffer = std::make_shared_for_overwrite<uint8_t[]>(size);
#else
	std::shared_ptr<uint8_t[]> buffer(new uint8_t[size]);
#endif
	ssize_t received = recvfrom(fd, reinterpret_cast<char *>(buffer.get()), static_cast<int>(size), 0, (sockaddr *)&addr, &addrlen);
	if (received < 0)
		throw_last_socket_error();

	std::span message{buffer.get(), (size_t)received};

	if (encrypted)
	{
		// Not big enough for the IV: drop the packet
		if (received < sizeof(uint64_t))
			return {};

		std::array<uint8_t, 16> full_iv;
		memcpy(full_iv.data(), buffer.get(), sizeof(uint64_t)); // TODO: endianness?
		memcpy(full_iv.data() + sizeof(uint64_t), recv_iv_header.data(), recv_iv_header.size());

		message = message.subspan(sizeof(uint64_t));

		decrypter.set_iv(full_iv);
		decrypter.decrypt_in_place(message);
	}

	return {deserialization_packet{std::move(buffer), message}, addr};
}

wivrn::deserialization_packet wivrn::UDP::receive_pending()
{
	if (messages.empty())
		return {};

	auto span = messages.back();
	messages.pop_back();
	return deserialization_packet{buffer, span};
}

wivrn::deserialization_packet wivrn::UDP::receive_raw()
{
	if (not messages.empty())
	{
		auto span = messages.back();
		messages.pop_back();
		return deserialization_packet{buffer, span};
	}

	static const size_t message_size = 2048;
	static const size_t num_messages = 20;
	if ((not buffer) or buffer.use_count() > 1)
	{
#if defined(__cpp_lib_smart_ptr_for_overwrite) && __cpp_lib_smart_ptr_for_overwrite >= 202002L
		buffer = std::make_shared_for_overwrite<uint8_t[]>(message_size * num_messages);
#else
		buffer.reset(new uint8_t[message_size * num_messages]);
#endif
	}
	std::array<iovec, num_messages> iovecs;
	std::array<mmsghdr, num_messages> mmsgs;
	for (size_t i = 0; i < num_messages; ++i)
	{
		iovecs[i] = {
		        .iov_base = buffer.get() + message_size * i,
		        .iov_len = message_size,
		};

		mmsgs[i] = {
		        .msg_hdr = {
		                .msg_iov = &iovecs[i],
		                .msg_iovlen = static_cast<decltype(msghdr{}.msg_iovlen)>(1),
		        },
		};
	}

	int received = recvmmsg(fd, mmsgs.data(), num_messages, MSG_DONTWAIT, nullptr);

	if (received < 0)
		throw_last_socket_error();
	if (received == 0)
		throw socket_shutdown();

	messages.reserve(received);

	for (int i = received - 1; i >= 0; --i)
	{
		std::span<uint8_t> message{(uint8_t *)iovecs[i].iov_base, mmsgs[i].msg_len};
		assert(message.data() != nullptr);

		if (encrypted)
		{
			// Not big enough for the IV: drop the packet
			if (message.size() < sizeof(uint64_t))
				throw std::runtime_error("Packet too small: " + std::to_string(message.size()));

			std::array<uint8_t, 16> full_iv;
			memcpy(full_iv.data(), message.data(), sizeof(uint64_t)); // TODO: endianness?
			memcpy(full_iv.data() + sizeof(uint64_t), recv_iv_header.data(), recv_iv_header.size());

			message = message.subspan(sizeof(uint64_t));

			decrypter.set_iv(full_iv);
			decrypter.decrypt_in_place(message);
		}

		if (i == 0)
			return deserialization_packet{buffer, message};

		messages.push_back(message);
	}

	mark_unreachable();
	return {};
}

size_t wivrn::UDP::send_raw(serialization_packet && packet)
{
	thread_local std::vector<iovec> iovecs;
	iovecs.clear();

	std::vector<std::span<uint8_t>> & data = packet;

	uint64_t counter;
	if (encrypted)
	{
		counter = iv_counter.fetch_add(1);

		std::array<uint8_t, 16> full_iv;
		memcpy(full_iv.data(), &counter, sizeof(uint64_t)); // TODO: endianness?
		memcpy(full_iv.data() + sizeof(uint64_t), send_iv_header.data(), send_iv_header.size());

		iovecs.emplace_back(&counter, sizeof(uint64_t));

		encrypter.set_key_and_iv(key, full_iv);
		encrypter.encrypt_in_place(data);
	}

	for (const auto & span: data)
		iovecs.emplace_back(span.data(), span.size());

	if (ssize_t sent = ::writev(fd, iovecs.data(), iovecs.size()); sent >= 0)
		return sent;
	throw_last_socket_error();
}

size_t wivrn::UDP::send_many_raw(std::span<serialization_packet> packets)
{
	thread_local std::vector<iovec> iovecs;
	thread_local std::vector<mmsghdr> mmsgs;
	thread_local std::vector<uint64_t> iv_counters;

	if (packets.empty())
		return 0;

	iovecs.clear();
	mmsgs.clear();
	iv_counters.clear();

	iv_counters.reserve(packets.size());

	size_t sent = 0;
	for (serialization_packet & packet: packets)
	{
		std::vector<std::span<uint8_t>> & data = packet;

		if (encrypted)
		{
			iv_counters.push_back(iv_counter.fetch_add(1));

			std::array<uint8_t, 16> full_iv;
			memcpy(full_iv.data(), &iv_counters.back(), sizeof(uint64_t)); // TODO: endianness?
			memcpy(full_iv.data() + sizeof(uint64_t), send_iv_header.data(), send_iv_header.size());

			iovecs.emplace_back(&iv_counters.back(), sizeof(uint64_t));

			encrypter.set_key_and_iv(key, full_iv);
			encrypter.encrypt_in_place(data);
		}

		for (const auto & span: data)
		{
			iovecs.emplace_back(span.data(), span.size_bytes());
			sent += span.size();
		}

		if (encrypted)
			mmsgs.push_back({.msg_hdr = {.msg_iovlen = static_cast<decltype(msghdr{}.msg_iovlen)>(data.size() + 1)}});
		else
			mmsgs.push_back({.msg_hdr = {.msg_iovlen = static_cast<decltype(msghdr{}.msg_iovlen)>(data.size())}});
	}

	for (size_t i = 0, j = 0; i < packets.size(); ++i)
	{
		mmsgs[i].msg_hdr.msg_iov = &iovecs[j];
		j += mmsgs[i].msg_hdr.msg_iovlen;
	}

	// sendmmsg may not send all messages, just consider them as lost for UDP
	if (sendmmsg(fd, mmsgs.data(), mmsgs.size(), 0) < 0)
		throw_last_socket_error();
	return sent;
}

wivrn::deserialization_packet wivrn::TCP::receive_raw()
{
	ssize_t expected_size;

	if (data.size_bytes() < sizeof(uint32_t))
	{
		expected_size = sizeof(uint32_t) - data.size_bytes();
	}
	else
	{
		uint32_t payload_size = *reinterpret_cast<uint32_t *>(data.data());
		expected_size = payload_size + sizeof(uint32_t) - data.size_bytes();
	}

	if (expected_size > capacity_left)
	{
		size_t new_size = std::max<size_t>(data.size_bytes() + expected_size,
		                                   4096);
		auto old = std::move(buffer);
#if defined(__cpp_lib_smart_ptr_for_overwrite) && __cpp_lib_smart_ptr_for_overwrite >= 202002L
		buffer = std::make_shared_for_overwrite<uint8_t[]>(new_size);
#else
		buffer.reset(new uint8_t[new_size]);
#endif
		memcpy(buffer.get(), data.data(), data.size_bytes());
		data = std::span(buffer.get(), data.size());
		capacity_left = new_size - data.size_bytes();
	}

	if (capacity_left > 0)
	{
		ssize_t received_size = recv(fd, reinterpret_cast<char *>(&*data.end()), static_cast<int>(capacity_left),
#if defined(_WIN32)
		                             0
#else
		                             MSG_DONTWAIT
#endif
		);

		if (received_size < 0)
			throw_last_socket_error();

		if (received_size == 0)
			throw socket_shutdown{};

		if (decrypter)
		{
			std::span<uint8_t> received_data{&*data.end(), (size_t)received_size};
			decrypter.decrypt_in_place(received_data);
		}

		data = std::span(data.data(), data.size() + received_size);
		capacity_left -= received_size;
	}

	if (data.size_bytes() < sizeof(uint32_t))
		return {};

	uint32_t payload_size = *reinterpret_cast<uint32_t *>(data.data());
	if (payload_size == 0)
		throw std::runtime_error("Invalid packet: 0 size");

	if (data.size_bytes() < sizeof(uint32_t) + payload_size)
		return {};

	auto span = data.subspan(sizeof(uint32_t), payload_size);
	data = data.subspan(sizeof(uint32_t) + payload_size);
	return deserialization_packet{buffer, span};
}

wivrn::deserialization_packet wivrn::TCP::receive_pending()
{
	if (data.size_bytes() < sizeof(uint32_t))
		return {};

	uint32_t payload_size = *reinterpret_cast<uint32_t *>(data.data());
	if (payload_size == 0)
		throw std::runtime_error("Invalid packet: 0 size");

	if (data.size_bytes() < sizeof(uint32_t) + payload_size)
		return {};

	auto span = data.subspan(sizeof(uint32_t), payload_size);
	data = data.subspan(sizeof(uint32_t) + payload_size);
	return deserialization_packet{buffer, span};
}

size_t wivrn::TCP::send_raw(serialization_packet && packet)
{
	size_t total_sent = 0;
	thread_local std::vector<iovec> iovecs;
	iovecs.clear();

	std::vector<std::span<uint8_t>> & data = packet;

	uint32_t size = 0;
	iovecs.emplace_back(&size, sizeof(size));
	for (const auto & span: data)
	{
		size += span.size_bytes();
		iovecs.emplace_back(span.data(), span.size_bytes());
	}

	msghdr hdr{
	        .msg_name = nullptr,
	        .msg_namelen = 0,
	        .msg_iov = iovecs.data(),
	        .msg_iovlen = static_cast<decltype(msghdr{}.msg_iovlen)>(iovecs.size()),
	        .msg_control = nullptr,
	        .msg_controllen = 0,
	        .msg_flags = 0,
	};

	std::lock_guard lock(*mutex);
	if (encrypter)
	{
		data.insert(data.begin(), {(uint8_t *)&size, sizeof(size)});
		encrypter.encrypt_in_place(data);
	}

	while (true)
	{
		ssize_t sent = ::sendmsg(fd, &hdr, MSG_NOSIGNAL);

		if (sent == 0)
			throw socket_shutdown{};

		if (sent < 0)
			throw_last_socket_error();

		total_sent += sent;

		// iov fully consumed
		while (hdr.msg_iovlen > 0 and sent >= hdr.msg_iov[0].iov_len)
		{
			sent -= hdr.msg_iov[0].iov_len;
			++hdr.msg_iov;
			--hdr.msg_iovlen;
		}
		if (hdr.msg_iovlen == 0)
			return total_sent;
		hdr.msg_iov[0].iov_base = (void *)((uintptr_t)hdr.msg_iov[0].iov_base + sent);
		hdr.msg_iov[0].iov_len -= sent;
	}
}

size_t wivrn::TCP::send_many_raw(std::span<serialization_packet> packets)
{
	thread_local std::vector<iovec> iovecs;
	thread_local std::vector<uint32_t> sizes;
	thread_local std::vector<std::span<uint8_t>> spans;

	if (packets.empty())
		return 0;

	iovecs.clear();
	sizes.clear();
	spans.clear();

	sizes.reserve(packets.size());

	for (serialization_packet & packet: packets)
	{
		std::vector<std::span<uint8_t>> & data = packet;

		auto & size = sizes.emplace_back(0);
		iovecs.emplace_back(&size, sizeof(size));
		spans.emplace_back((uint8_t *)&size, sizeof(size));

		for (const auto & span: data)
		{
			size += span.size_bytes();
			iovecs.emplace_back(span.data(), span.size_bytes());
			spans.emplace_back(span.data(), span.size_bytes());
		}
	}

	msghdr hdr{
	        .msg_name = nullptr,
	        .msg_namelen = 0,
	        .msg_iov = iovecs.data(),
	        .msg_iovlen = static_cast<decltype(msghdr{}.msg_iovlen)>(iovecs.size()),
	        .msg_control = nullptr,
	        .msg_controllen = 0,
	        .msg_flags = 0,
	};

	std::lock_guard lock(*mutex);
	if (encrypter)
	{
		encrypter.encrypt_in_place(spans);
	}

	size_t total_sent = 0;
	while (true)
	{
		ssize_t sent = ::sendmsg(fd, &hdr, MSG_NOSIGNAL);

		if (sent == 0)
			throw socket_shutdown{};

		if (sent < 0)
			throw_last_socket_error();

		total_sent += sent;

		// iov fully consumed
		while (hdr.msg_iovlen > 0 and sent >= hdr.msg_iov[0].iov_len)
		{
			sent -= hdr.msg_iov[0].iov_len;
			++hdr.msg_iov;
			--hdr.msg_iovlen;
		}
		if (hdr.msg_iovlen == 0)
			return total_sent;
		hdr.msg_iov[0].iov_base = (void *)((uintptr_t)hdr.msg_iov[0].iov_base + sent);
		hdr.msg_iov[0].iov_len -= sent;
	}
}

void wivrn::UDP::set_aes_key_and_ivs(std::span<std::uint8_t, 16> key_, std::span<std::uint8_t, 8> recv_iv_header_, std::span<std::uint8_t, 8> send_iv_header_)
{
	decrypter = crypto::decrypt_context{EVP_aes_128_ctr()};
	decrypter.set_key(key_);

	std::ranges::copy(key_, key.begin());
	std::ranges::copy(recv_iv_header_, recv_iv_header.begin());
	std::ranges::copy(send_iv_header_, send_iv_header.begin());
	encrypted = true;
}

void wivrn::TCP::set_aes_key_and_ivs(std::span<std::uint8_t, 16> key, std::span<std::uint8_t, 16> recv_iv, std::span<std::uint8_t, 16> send_iv)
{
	encrypter = crypto::encrypt_context{EVP_aes_128_ctr()};
	encrypter.set_key(key);
	encrypter.set_iv(send_iv);

	decrypter = crypto::decrypt_context{EVP_aes_128_ctr()};
	decrypter.set_key(key);
	decrypter.set_iv(recv_iv);
}

std::pair<wivrn::UDP, wivrn::UDP> wivrn::make_local_datagram_pair()
{
#if !defined(_WIN32)
	int fds[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) < 0)
		throw_last_socket_error();

	set_close_on_exec(fds[0]);
	set_close_on_exec(fds[1]);
	return {UDP{fds[0]}, UDP{fds[1]}};
#else
	ensure_socket_runtime();

	UDP first;
	UDP second;

	sockaddr_in6 first_addr{};
	first_addr.sin6_family = AF_INET6;
	first_addr.sin6_addr = in6addr_loopback;
	first_addr.sin6_port = 0;
	first.bind(first_addr);

	sockaddr_in6 second_addr{};
	second_addr.sin6_family = AF_INET6;
	second_addr.sin6_addr = in6addr_loopback;
	second_addr.sin6_port = 0;
	second.bind(second_addr);

	sockaddr_in6 first_bound{};
	sockaddr_in6 second_bound{};
	int first_len = sizeof(first_bound);
	int second_len = sizeof(second_bound);
	if (getsockname(first.get_fd(), reinterpret_cast<sockaddr *>(&first_bound), &first_len) < 0)
		throw_last_socket_error();
	if (getsockname(second.get_fd(), reinterpret_cast<sockaddr *>(&second_bound), &second_len) < 0)
		throw_last_socket_error();

	first.connect(second_bound.sin6_addr, ntohs(second_bound.sin6_port));
	second.connect(first_bound.sin6_addr, ntohs(first_bound.sin6_port));

	return {std::move(first), std::move(second)};
#endif
}
