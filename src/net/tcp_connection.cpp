#include "oklib/net/tcp_connection.h"

#include <cerrno>
#include <cstring>
#include <utility>
#include <unistd.h>
#include <vector>

#include "oklib/base/logging.h"
#include "oklib/net/channel.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/socket.h"
#include "tls_engine.h"

namespace oklib::net {

struct TcpConnection::TlsContext {
#if OKLIB_ENABLE_TLS
  std::unique_ptr<TlsEngine> engine;
#endif
  Buffer plain_input_buffer;
  std::vector<std::string> pending_plain_output;
  std::string configuration_error;
};

TcpConnection::TcpConnection(EventLoop* loop, std::string name, int sockfd,
                             InetAddress local_address, InetAddress peer_address)
    : loop_(loop),
      name_(std::move(name)),
      socket_(std::make_unique<Socket>(sockfd)),
      channel_(std::make_unique<Channel>(loop, sockfd)),
      local_address_(std::move(local_address)),
      peer_address_(std::move(peer_address)) {
  channel_->set_read_callback([this](oklib::Timestamp time) { handle_read(time); });
  channel_->set_write_callback([this] { handle_write(); });
  channel_->set_close_callback([this] { handle_close(); });
  channel_->set_error_callback([this] { handle_error(); });
  socket_->set_keep_alive(true);
}

TcpConnection::~TcpConnection() = default;

bool TcpConnection::connected() const noexcept {
  return state_.load(std::memory_order_acquire) == State::connected;
}

bool TcpConnection::disconnected() const noexcept {
  return state_.load(std::memory_order_acquire) == State::disconnected;
}

bool TcpConnection::tls_enabled() const noexcept {
  return tls_ != nullptr;
}

bool TcpConnection::tls_established() const noexcept {
#if OKLIB_ENABLE_TLS
  return tls_ != nullptr && tls_->engine && tls_->engine->handshake_complete();
#else
  return false;
#endif
}

void TcpConnection::send(std::string_view message) {
  if (!connected()) {
    return;
  }
  send(std::string(message));
}

void TcpConnection::send(const char* message) {
  if (message == nullptr) {
    return;
  }
  send(std::string_view(message));
}

void TcpConnection::send(std::string&& message) {
  if (!connected()) {
    return;
  }
  if (loop_->is_in_loop_thread()) {
    send_in_loop(std::move(message));
  } else {
    auto self = shared_from_this();
    loop_->queue_in_loop([self, message = std::move(message)]() mutable { self->send_in_loop(std::move(message)); });
  }
}

void TcpConnection::send(const void* data, std::size_t len) {
  send(std::string_view(static_cast<const char*>(data), len));
}

void TcpConnection::send(Buffer* buffer) {
  send(buffer->retrieve_all_as_string());
}

void TcpConnection::send_raw(std::string_view message) {
  if (!connected()) {
    return;
  }
  std::string owned(message);
  if (loop_->is_in_loop_thread()) {
    send_raw_in_loop(std::move(owned));
  } else {
    auto self = shared_from_this();
    loop_->queue_in_loop([self, message = std::move(owned)]() mutable {
      self->send_raw_in_loop(std::move(message));
    });
  }
}

void TcpConnection::shutdown() {
  State expected = State::connected;
  if (state_.compare_exchange_strong(expected, State::disconnecting)) {
    auto self = shared_from_this();
    loop_->run_in_loop([self] { self->shutdown_in_loop(); });
  }
}

void TcpConnection::force_close() {
  if (connected() || state_.load(std::memory_order_acquire) == State::disconnecting) {
    set_state(State::disconnecting);
    auto self = shared_from_this();
    loop_->queue_in_loop([self] { self->force_close_in_loop(); });
  }
}

void TcpConnection::set_tcp_no_delay(bool on) {
  socket_->set_tcp_no_delay(on);
}

void TcpConnection::enable_client_tls(TlsClientOptions options, std::string server_name) {
  if (!options.enabled) {
    return;
  }
  tls_ = std::make_unique<TlsContext>();
#if OKLIB_ENABLE_TLS
  std::string error;
  tls_->engine = TlsEngine::create_client(options, server_name, &error);
  if (!tls_->engine) {
    tls_->configuration_error = error.empty() ? "failed to create client TLS engine" : std::move(error);
  }
#else
  (void)server_name;
  tls_->configuration_error = "oklib was built without OKLIB_ENABLE_TLS=ON";
#endif
}

void TcpConnection::enable_server_tls(TlsServerOptions options) {
  if (!options.enabled) {
    return;
  }
  tls_ = std::make_unique<TlsContext>();
#if OKLIB_ENABLE_TLS
  std::string error;
  tls_->engine = TlsEngine::create_server(options, &error);
  if (!tls_->engine) {
    tls_->configuration_error = error.empty() ? "failed to create server TLS engine" : std::move(error);
  }
#else
  tls_->configuration_error = "oklib was built without OKLIB_ENABLE_TLS=ON";
#endif
}

void TcpConnection::start_read() {
  auto self = shared_from_this();
  loop_->run_in_loop([self] { self->start_read_in_loop(); });
}

void TcpConnection::stop_read() {
  auto self = shared_from_this();
  loop_->run_in_loop([self] { self->stop_read_in_loop(); });
}

void TcpConnection::pause_reading() {
  stop_read();
}

void TcpConnection::resume_reading() {
  start_read();
}

void TcpConnection::connect_established() {
  loop_->assert_in_loop_thread();
  set_state(State::connected);
  channel_->enable_reading();
  if (tls_) {
    start_tls_handshake(oklib::Timestamp::now());
  }
  if (connected()) {
    connection_callback_(shared_from_this());
  }
}

void TcpConnection::connect_destroyed() {
  loop_->assert_in_loop_thread();
  if (connected()) {
    set_state(State::disconnected);
    channel_->disable_all();
    connection_callback_(shared_from_this());
  }
  channel_->remove();
}

void TcpConnection::handle_read(oklib::Timestamp receive_time) {
  loop_->assert_in_loop_thread();
  int saved_errno = 0;
  const auto n = input_buffer_.read_fd(channel_->fd(), &saved_errno);
  if (n > 0) {
    if (tls_) {
      const std::string encrypted = input_buffer_.retrieve_all_as_string();
#if OKLIB_ENABLE_TLS
      std::string error;
      if (!tls_->engine || !tls_->engine->receive_encrypted(encrypted, &error)) {
        fail_tls(error.empty() ? "failed to receive TLS data" : error);
        return;
      }
      process_tls(receive_time);
#else
      (void)encrypted;
      fail_tls(tls_->configuration_error);
#endif
    } else {
      message_callback_(shared_from_this(), &input_buffer_, receive_time);
    }
  } else if (n == 0) {
    handle_close();
  } else {
    errno = saved_errno;
    handle_error();
  }
}

void TcpConnection::handle_write() {
  loop_->assert_in_loop_thread();
  if (!channel_->is_writing()) {
    return;
  }
  const auto n = ::write(channel_->fd(), output_buffer_.peek(), output_buffer_.readable_bytes());
  if (n > 0) {
    output_buffer_.retrieve(static_cast<std::size_t>(n));
    if (output_buffer_.readable_bytes() == 0) {
      channel_->disable_writing();
      if (write_complete_callback_) {
        auto self = shared_from_this();
        loop_->queue_in_loop([self, callback = write_complete_callback_] { callback(self); });
      }
      if (state_.load(std::memory_order_acquire) == State::disconnecting) {
        shutdown_in_loop();
      }
    }
  } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    handle_error();
  }
}

void TcpConnection::handle_close() {
  loop_->assert_in_loop_thread();
  if (disconnected()) {
    return;
  }
  set_state(State::disconnected);
  channel_->disable_all();
  auto self = shared_from_this();
  connection_callback_(self);
  if (close_callback_) {
    close_callback_(self);
  }
}

void TcpConnection::handle_error() {
  OKLIB_LOG_ERROR << "TcpConnection error on " << name_ << ": " << std::strerror(errno);
}

void TcpConnection::send_in_loop(std::string message) {
  if (send_filter_) {
    message = send_filter_(message);
  }
  if (!message.empty()) {
    if (tls_) {
      if (!tls_established()) {
        tls_->pending_plain_output.push_back(std::move(message));
        start_tls_handshake(oklib::Timestamp::now());
        return;
      }
#if OKLIB_ENABLE_TLS
      std::string encrypted;
      std::string error;
      if (!tls_->engine || !tls_->engine->encrypt(message, &encrypted, &error)) {
        fail_tls(error.empty() ? "failed to encrypt TLS data" : error);
        return;
      }
      if (!encrypted.empty()) {
        send_raw_in_loop(encrypted.data(), encrypted.size());
      }
#else
      fail_tls(tls_->configuration_error);
#endif
      return;
    }
    send_raw_in_loop(message.data(), message.size());
  }
}

void TcpConnection::send_in_loop(const void* data, std::size_t len) {
  std::string message(static_cast<const char*>(data), len);
  send_in_loop(std::move(message));
}

void TcpConnection::send_raw_in_loop(std::string message) {
  send_raw_in_loop(message.data(), message.size());
}

void TcpConnection::send_raw_in_loop(const void* data, std::size_t len) {
  loop_->assert_in_loop_thread();
  if (disconnected()) {
    return;
  }

  ssize_t written = 0;
  std::size_t remaining = len;
  bool fault_error = false;

  if (!channel_->is_writing() && output_buffer_.readable_bytes() == 0) {
    written = ::write(channel_->fd(), data, len);
    if (written >= 0) {
      remaining = len - static_cast<std::size_t>(written);
      if (remaining == 0 && write_complete_callback_) {
        auto self = shared_from_this();
        loop_->queue_in_loop([self, callback = write_complete_callback_] { callback(self); });
      }
    } else {
      written = 0;
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        fault_error = errno == EPIPE || errno == ECONNRESET;
      }
    }
  }

  if (!fault_error && remaining > 0) {
    const auto old_len = output_buffer_.readable_bytes();
    if (old_len + remaining >= high_water_mark_ && old_len < high_water_mark_ &&
        high_water_mark_callback_) {
      auto self = shared_from_this();
      loop_->queue_in_loop([self, callback = high_water_mark_callback_, mark = old_len + remaining] {
        callback(self, mark);
      });
    }
    output_buffer_.append(static_cast<const char*>(data) + written, remaining);
    if (!channel_->is_writing()) {
      channel_->enable_writing();
    }
  }
}

void TcpConnection::start_tls_handshake(oklib::Timestamp receive_time) {
  loop_->assert_in_loop_thread();
  if (!tls_) {
    return;
  }
  if (!tls_->configuration_error.empty()) {
    fail_tls(tls_->configuration_error);
    return;
  }
  process_tls(receive_time);
}

void TcpConnection::process_tls(oklib::Timestamp receive_time) {
  loop_->assert_in_loop_thread();
  if (!tls_) {
    return;
  }
#if OKLIB_ENABLE_TLS
  if (!tls_->engine) {
    fail_tls("TLS engine is not available");
    return;
  }

  const bool was_established = tls_->engine->handshake_complete();
  std::string plain;
  std::string encrypted;
  std::string error;
  if (!tls_->engine->read_decrypted(&plain, &encrypted, &error)) {
    fail_tls(error.empty() ? "failed to decrypt TLS data" : error);
    return;
  }
  if (!encrypted.empty()) {
    send_raw_in_loop(encrypted.data(), encrypted.size());
  }

  if (!was_established && tls_->engine->handshake_complete()) {
    if (!flush_tls_pending_output()) {
      return;
    }
  }

  if (!plain.empty()) {
    tls_->plain_input_buffer.append(plain);
    message_callback_(shared_from_this(), &tls_->plain_input_buffer, receive_time);
  }
#else
  fail_tls(tls_->configuration_error);
#endif
}

bool TcpConnection::flush_tls_pending_output() {
  loop_->assert_in_loop_thread();
  if (!tls_ || tls_->pending_plain_output.empty()) {
    return true;
  }
#if OKLIB_ENABLE_TLS
  std::vector<std::string> pending;
  pending.swap(tls_->pending_plain_output);
  for (const auto& plain : pending) {
    std::string encrypted;
    std::string error;
    if (!tls_->engine || !tls_->engine->encrypt(plain, &encrypted, &error)) {
      fail_tls(error.empty() ? "failed to flush queued TLS data" : error);
      return false;
    }
    if (!encrypted.empty()) {
      send_raw_in_loop(encrypted.data(), encrypted.size());
    }
  }
  return true;
#else
  fail_tls(tls_->configuration_error);
  return false;
#endif
}

void TcpConnection::fail_tls(const std::string& error) {
  loop_->assert_in_loop_thread();
  OKLIB_LOG_ERROR << "TLS error on " << name_ << ": " << error;
  force_close_in_loop();
}

void TcpConnection::shutdown_in_loop() {
  loop_->assert_in_loop_thread();
  if (!channel_->is_writing()) {
    socket_->shutdown_write();
  }
}

void TcpConnection::force_close_in_loop() {
  loop_->assert_in_loop_thread();
  if (connected() || state_.load(std::memory_order_acquire) == State::disconnecting) {
    handle_close();
  }
}

void TcpConnection::start_read_in_loop() {
  loop_->assert_in_loop_thread();
  if (!reading_.load(std::memory_order_acquire)) {
    channel_->enable_reading();
    reading_.store(true, std::memory_order_release);
    dispatch_pending_read_in_loop(oklib::Timestamp::now());
  }
}

void TcpConnection::stop_read_in_loop() {
  loop_->assert_in_loop_thread();
  if (reading_.load(std::memory_order_acquire)) {
    channel_->disable_reading();
    reading_.store(false, std::memory_order_release);
  }
}

void TcpConnection::dispatch_pending_read_in_loop(oklib::Timestamp receive_time) {
  loop_->assert_in_loop_thread();
  if (!connected()) {
    return;
  }
#if OKLIB_ENABLE_TLS
  if (tls_ && tls_->plain_input_buffer.readable_bytes() > 0) {
    message_callback_(shared_from_this(), &tls_->plain_input_buffer, receive_time);
    return;
  }
#endif
  if (input_buffer_.readable_bytes() > 0) {
    message_callback_(shared_from_this(), &input_buffer_, receive_time);
  }
}

}  // namespace oklib::net
