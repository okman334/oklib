#pragma once

#include <functional>

#include "oklib/base/noncopyable.h"
#include "oklib/net/channel.h"
#include "oklib/net/inet_address.h"
#include "oklib/net/socket.h"

namespace oklib::net {

class EventLoop;

class Acceptor : private oklib::Noncopyable {
 public:
  using NewConnectionCallback = std::function<void(int sockfd, const InetAddress& peer_address)>;

  Acceptor(EventLoop* loop,
           const InetAddress& listen_address,
           bool reuse_port,
           bool ipv6_only = true);

  void set_new_connection_callback(NewConnectionCallback callback) {
    new_connection_callback_ = std::move(callback);
  }

  void listen();
  [[nodiscard]] bool listening() const noexcept { return listening_; }
  [[nodiscard]] InetAddress listen_address() const { return accept_socket_.local_address(); }

 private:
  void handle_read(oklib::Timestamp receive_time);

  EventLoop* loop_;
  Socket accept_socket_;
  Channel accept_channel_;
  NewConnectionCallback new_connection_callback_;
  bool listening_{false};
};

}  // namespace oklib::net
