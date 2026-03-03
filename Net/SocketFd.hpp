#ifndef NET_SOCKETFD_HPP
#define NET_SOCKETFD_HPP

#include<cstddef>
#include<cstdint>
#include<utility>
#include<vector>
#include"Net/Fd.hpp"
#include"Util/Compiler.hpp"

namespace Net {

/* RAII class for a network socket file descriptor.
 * See: http://ia800504.us.archive.org/3/items/TheUltimateSo_lingerPageOrWhyIsMyTcpNotReliable/the-ultimate-so_linger-page-or-why-is-my-tcp-not-reliable.html
 * This is most appropriate for sockets that represent a connection
 * between us and a remote computer, not appropriate for sockets
 * that are bound for listening, or which we are still setting up
 * for binding.
 */
class SocketFd {
private:
	Fd fd;

public:
	SocketFd(std::nullptr_t _ = nullptr) : fd() { }
	SocketFd(SocketFd&&) =default;
	SocketFd& operator=(SocketFd&&) =default;

	/* Promote from a plain fd (pre-connecting) to a
	 * connected socket.
	 */
	explicit SocketFd(Fd&& o) : fd(std::move(o)) { }

	~SocketFd();

	int get() const { return fd.get(); }
	int release() { return fd.release(); }
	void swap(SocketFd& o) { fd.swap(o.fd); }
	void reset(int fd_ = -1) { fd.reset(fd_); }

	operator bool() const { return (bool)fd; }
	bool operator!() const {return !((bool)fd); }

	void write(std::vector<std::uint8_t> const&);
	std::vector<std::uint8_t> read(std::size_t);
};

}

#endif /* NET_SOCKETFD_HPP */
