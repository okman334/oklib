#include "oklib/net/connector.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <unistd.h>

#include "oklib/base/logging.h"
#include "oklib/net/channel.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/socket.h"

namespace oklib::net {
namespace {

bool is_self_connect(int sockfd) {
  sockaddr_in local{};
  sockaddr_in peer{};
  socklen_t len = sizeof(local);
  if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
    return false;
  }
  len = sizeof(peer);
  if (::getpeername(sockfd, reinterpret_cast<sockaddr*>(&peer), &len) != 0) {
    return false;
  }
  return local.sin_port == peer.sin_port && local.sin_addr.s_addr == peer.sin_addr.s_addr;
}

}  // namespace

Connector::Connector(EventLoop* loop, InetAddress server_address)
    : loop_(loop), server_address_(std::move(server_address)) {}

Connector::~Connector() = default;

void Connector::start() {
  connect_ = true;
  loop_->run_in_loop([self = shared_from_this()] { self->start_in_loop(); });
}

void Connector::restart() {
  loop_->assert_in_loop_thread();
  set_state(State::disconnected);
  retry_delay_ms_ = 500;
  connect_ = true;
  start_in_loop();
}

void Connector::stop() {
  connect_ = false;
  loop_->queue_in_loop([self = shared_from_this()] { self->stop_in_loop(); });
}

void Connector::start_in_loop() {
  loop_->assert_in_loop_thread();
  if (connect_) {
    connect();
  }
}

void Connector::stop_in_loop() {
  loop_->assert_in_loop_thread();
  if (state_ == State::connecting) {
    const int sockfd = remove_and_reset_channel();
    ::close(sockfd);
    set_state(State::disconnected);
  }
}

void Connector::connect() {
  Socket socket = Socket::create_nonblocking();
  const int sockfd = socket.fd();
  const int ret = ::connect(sockfd, server_address_.sockaddr_ptr(), server_address_.length());
  const int saved_errno = ret == 0 ? 0 : errno;
  if (ret == 0 || saved_errno == EINPROGRESS || saved_errno == EINTR || saved_errno == EISCONN) {
    connecting(socket.release());
  } else {
    retry(socket.release());
  }
}

void Connector::connecting(int sockfd) {
  set_state(State::connecting);
  channel_ = std::make_unique<Channel>(loop_, sockfd);
  channel_->set_write_callback([self = shared_from_this()] { self->handle_write(); });
  channel_->set_error_callback([self = shared_from_this()] { self->handle_error(); });
  channel_->enable_writing();
}

void Connector::handle_write() {
  if (state_ != State::connecting) {
    return;
  }
  const int sockfd = remove_and_reset_channel();
  const int error = get_socket_error(sockfd);
  if (error != 0) {
    errno = error;
    retry(sockfd);
  } else if (is_self_connect(sockfd)) {
    retry(sockfd);
  } else {
    set_state(State::connected);
    if (connect_ && new_connection_callback_) {
      new_connection_callback_(sockfd);
    } else {
      ::close(sockfd);
    }
  }
}

void Connector::handle_error() {
  if (state_ == State::connecting) {
    const int sockfd = remove_and_reset_channel();
    retry(sockfd);
  }
}

void Connector::retry(int sockfd) {
  ::close(sockfd);
  set_state(State::disconnected);
  if (connect_ && retry_) {
    const auto delay = std::chrono::milliseconds(retry_delay_ms_);
    retry_delay_ms_ = std::min(retry_delay_ms_ * 2, 30000);
    loop_->run_after(delay, [self = shared_from_this()] { self->start_in_loop(); });
  }
}

int Connector::remove_and_reset_channel() {
  channel_->disable_all();
  channel_->remove();
  const int sockfd = channel_->fd();
  loop_->queue_in_loop([self = shared_from_this()] { self->reset_channel(); });
  return sockfd;
}

void Connector::reset_channel() {
  channel_.reset();
}

}  // namespace oklib::net
