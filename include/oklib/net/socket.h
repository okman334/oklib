#pragma once

#include "oklib/base/noncopyable.h"
#include "oklib/net/inet_address.h"

namespace oklib::net {

class Socket : private oklib::Noncopyable {
 public:
  explicit Socket(int fd) noexcept : fd_(fd) {}
  ~Socket();

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  static Socket create_nonblocking();

  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] int release() noexcept;

  void bind(const InetAddress& address) const;
  void listen() const;
  [[nodiscard]] int accept(InetAddress* peer_address) const;
  void shutdown_write() const;

  void set_reuse_addr(bool on) const;
  void set_reuse_port(bool on) const;
  void set_tcp_no_delay(bool on) const;
  void set_keep_alive(bool on) const;

  [[nodiscard]] InetAddress local_address() const;
  [[nodiscard]] InetAddress peer_address() const;

 private:
  int fd_{-1};
};

void set_nonblocking_and_close_on_exec(int fd);
int get_socket_error(int fd);

}  // namespace oklib::net
