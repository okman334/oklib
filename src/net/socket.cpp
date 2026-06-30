#include "oklib/net/socket.h"

#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <system_error>
#include <unistd.h>

namespace oklib::net {

void set_nonblocking_and_close_on_exec(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  const int fd_flags = ::fcntl(fd, F_GETFD, 0);
  if (fd_flags >= 0) {
    ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }
}

int get_socket_error(int fd) {
  int option = 0;
  socklen_t len = sizeof(option);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &option, &len) < 0) {
    return errno;
  }
  return option;
}

Socket::~Socket() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

Socket::Socket(Socket&& other) noexcept : fd_(other.release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.release();
  }
  return *this;
}

Socket Socket::create_nonblocking(sa_family_t family) {
  int fd = ::socket(family, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(), "socket");
  }
  set_nonblocking_and_close_on_exec(fd);
  return Socket(fd);
}

int Socket::release() noexcept {
  const int fd = fd_;
  fd_ = -1;
  return fd;
}

void Socket::bind(const InetAddress& address) const {
  if (::bind(fd_, address.sockaddr_ptr(), address.length()) != 0) {
    throw std::system_error(errno, std::generic_category(), "bind");
  }
}

void Socket::listen() const {
  if (::listen(fd_, SOMAXCONN) != 0) {
    throw std::system_error(errno, std::generic_category(), "listen");
  }
}

int Socket::accept(InetAddress* peer_address) const {
  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);
#if defined(__linux__)
  const int connfd = ::accept4(fd_, reinterpret_cast<sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
  const int connfd = ::accept(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  if (connfd >= 0) {
    set_nonblocking_and_close_on_exec(connfd);
  }
#endif
  if (connfd >= 0 && peer_address != nullptr) {
    *peer_address = InetAddress(reinterpret_cast<const sockaddr*>(&addr), len);
  }
  return connfd;
}

void Socket::shutdown_write() const {
  ::shutdown(fd_, SHUT_WR);
}

void Socket::set_reuse_addr(bool on) const {
  int optval = on ? 1 : 0;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &optval, static_cast<socklen_t>(sizeof(optval)));
}

void Socket::set_reuse_port(bool on) const {
#if defined(SO_REUSEPORT)
  int optval = on ? 1 : 0;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &optval, static_cast<socklen_t>(sizeof(optval)));
#else
  (void)on;
#endif
}

void Socket::set_ipv6_only(bool on) const {
  int optval = on ? 1 : 0;
  if (::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &optval,
                   static_cast<socklen_t>(sizeof(optval))) != 0) {
    throw std::system_error(errno, std::generic_category(), "setsockopt IPV6_V6ONLY");
  }
}

void Socket::set_tcp_no_delay(bool on) const {
  int optval = on ? 1 : 0;
  ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &optval, static_cast<socklen_t>(sizeof(optval)));
}

void Socket::set_keep_alive(bool on) const {
  int optval = on ? 1 : 0;
  ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &optval, static_cast<socklen_t>(sizeof(optval)));
}

InetAddress Socket::local_address() const {
  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    throw std::system_error(errno, std::generic_category(), "getsockname");
  }
  return InetAddress(reinterpret_cast<const sockaddr*>(&addr), len);
}

InetAddress Socket::peer_address() const {
  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);
  if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    throw std::system_error(errno, std::generic_category(), "getpeername");
  }
  return InetAddress(reinterpret_cast<const sockaddr*>(&addr), len);
}

}  // namespace oklib::net
