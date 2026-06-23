#include "oklib/net/acceptor.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

#include "oklib/base/logging.h"
#include "oklib/net/event_loop.h"

namespace oklib::net {

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listen_address, bool reuse_port)
    : loop_(loop),
      accept_socket_(Socket::create_nonblocking()),
      accept_channel_(loop, accept_socket_.fd()) {
  accept_socket_.set_reuse_addr(true);
  accept_socket_.set_reuse_port(reuse_port);
  accept_socket_.bind(listen_address);
  accept_channel_.set_read_callback([this](oklib::Timestamp time) { handle_read(time); });
}

void Acceptor::listen() {
  loop_->assert_in_loop_thread();
  listening_ = true;
  accept_socket_.listen();
  accept_channel_.enable_reading();
}

void Acceptor::handle_read(oklib::Timestamp) {
  loop_->assert_in_loop_thread();
  for (;;) {
    InetAddress peer_address;
    const int connfd = accept_socket_.accept(&peer_address);
    if (connfd >= 0) {
      if (new_connection_callback_) {
        new_connection_callback_(connfd, peer_address);
      } else {
        ::close(connfd);
      }
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    OKLIB_LOG_ERROR << "accept failed: " << std::strerror(errno);
    break;
  }
}

}  // namespace oklib::net
