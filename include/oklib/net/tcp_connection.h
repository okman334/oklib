#pragma once

#include <any>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "oklib/base/noncopyable.h"
#include "oklib/net/buffer.h"
#include "oklib/net/callbacks.h"
#include "oklib/net/inet_address.h"
#include "oklib/net/tls_options.h"

namespace oklib::net {

class Channel;
class EventLoop;
class Socket;

class TcpConnection : private oklib::Noncopyable, public std::enable_shared_from_this<TcpConnection> {
 public:
  using SendFilter = std::function<std::string(std::string_view)>;

  TcpConnection(EventLoop* loop, std::string name, int sockfd, InetAddress local_address, InetAddress peer_address);
  ~TcpConnection();

  [[nodiscard]] EventLoop* loop() const noexcept { return loop_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] const InetAddress& local_address() const noexcept { return local_address_; }
  [[nodiscard]] const InetAddress& peer_address() const noexcept { return peer_address_; }
  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] bool disconnected() const noexcept;
  [[nodiscard]] bool tls_enabled() const noexcept;
  [[nodiscard]] bool tls_established() const noexcept;

  void send(std::string_view message);
  void send(const void* data, std::size_t len);
  void send(Buffer* buffer);
  void send_raw(std::string_view message);
  void shutdown();
  void force_close();
  void set_tcp_no_delay(bool on);
  void start_read();
  void stop_read();

  void set_context(std::any context) { context_ = std::move(context); }
  [[nodiscard]] const std::any& context() const noexcept { return context_; }
  [[nodiscard]] std::any& mutable_context() noexcept { return context_; }

  void set_connection_callback(ConnectionCallback callback) { connection_callback_ = std::move(callback); }
  void set_send_filter(SendFilter filter) { send_filter_ = std::move(filter); }
  void set_message_callback(MessageCallback callback) { message_callback_ = std::move(callback); }
  void set_write_complete_callback(WriteCompleteCallback callback) { write_complete_callback_ = std::move(callback); }
  void set_high_water_mark_callback(HighWaterMarkCallback callback, std::size_t mark) {
    high_water_mark_callback_ = std::move(callback);
    high_water_mark_ = mark;
  }
  void set_close_callback(CloseCallback callback) { close_callback_ = std::move(callback); }
  void enable_client_tls(TlsClientOptions options, std::string server_name);
  void enable_server_tls(TlsServerOptions options);

  void connect_established();
  void connect_destroyed();

 private:
  struct TlsContext;
  enum class State { disconnected, connecting, connected, disconnecting };

  void handle_read(oklib::Timestamp receive_time);
  void handle_write();
  void handle_close();
  void handle_error();
  void send_in_loop(std::string message);
  void send_in_loop(const void* data, std::size_t len);
  void send_raw_in_loop(std::string message);
  void send_raw_in_loop(const void* data, std::size_t len);
  void start_tls_handshake(oklib::Timestamp receive_time);
  void process_tls(oklib::Timestamp receive_time);
  bool flush_tls_pending_output();
  void fail_tls(const std::string& error);
  void shutdown_in_loop();
  void force_close_in_loop();
  void start_read_in_loop();
  void stop_read_in_loop();
  void set_state(State state) noexcept { state_.store(state, std::memory_order_release); }

  EventLoop* loop_;
  std::string name_;
  std::atomic<State> state_{State::connecting};
  bool reading_{true};
  std::unique_ptr<Socket> socket_;
  std::unique_ptr<Channel> channel_;
  InetAddress local_address_;
  InetAddress peer_address_;
  ConnectionCallback connection_callback_{default_connection_callback};
  MessageCallback message_callback_{default_message_callback};
  WriteCompleteCallback write_complete_callback_;
  HighWaterMarkCallback high_water_mark_callback_;
  CloseCallback close_callback_;
  SendFilter send_filter_;
  std::size_t high_water_mark_{64 * 1024 * 1024};
  Buffer input_buffer_;
  Buffer output_buffer_;
  std::unique_ptr<TlsContext> tls_;
  std::any context_;
};

}  // namespace oklib::net
