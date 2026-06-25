#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "oklib/base/noncopyable.h"
#include "oklib/http/http_headers.h"
#include "oklib/http/http_parser.h"
#include "oklib/http/tls_options.h"
#include "oklib/net/tcp_client.h"
#include "oklib/websocket/websocket_channel.h"

namespace oklib::websocket {

enum class WebSocketClientErrorCode {
  ok = 0,
  invalid_url = -1,
  unsupported_scheme = -2,
  unsupported_host = -3,
  tls_not_enabled = -4,
};

class WebSocketClient : private oklib::Noncopyable {
 public:
  using OpenCallback = std::function<void(const WebSocketChannelPtr&)>;
  using MessageCallback =
      std::function<void(const WebSocketChannelPtr&, std::string_view, WebSocketOpcode)>;
  using CloseCallback = std::function<void(const WebSocketChannelPtr&, WebSocketCloseInfo)>;
  using ErrorCallback = std::function<void(const WebSocketChannelPtr&, WebSocketError)>;

  WebSocketClient(oklib::net::EventLoop* loop,
                  std::string name,
                  WebSocketOptions options = {});
  ~WebSocketClient();

  int open(std::string url, oklib::http::HttpHeaders headers = {});
  bool send_text(std::string_view message);
  bool send_binary(std::string_view message);
  bool send_ping(std::string_view payload = {});
  bool close(std::uint16_t code = 1000, std::string_view reason = {});
  void stop();

  void set_tls_options(oklib::http::TlsClientOptions options) {
    tls_options_ = std::move(options);
  }
  void set_open_callback(OpenCallback callback) { open_callback_ = std::move(callback); }
  void set_message_callback(MessageCallback callback) {
    message_callback_ = std::move(callback);
  }
  void set_close_callback(CloseCallback callback) { close_callback_ = std::move(callback); }
  void set_error_callback(ErrorCallback callback) { error_callback_ = std::move(callback); }

  [[nodiscard]] WebSocketChannelPtr channel() const { return channel_; }

 private:
  void on_connection(const oklib::net::TcpConnectionPtr& connection);
  void on_message(const oklib::net::TcpConnectionPtr& connection,
                  oklib::net::Buffer* buffer,
                  oklib::Timestamp receive_time);
  void send_handshake_request();
  bool process_tls_input(oklib::net::Buffer* buffer, oklib::net::Buffer* plain);
  bool start_tls_handshake();
  void on_plain_message(oklib::net::Buffer* buffer);
  void on_websocket_message(oklib::net::Buffer* buffer);
  void report_error(WebSocketError error);
  void close_for_error(WebSocketError error, std::string reason);
  void notify_close(WebSocketCloseInfo info);
  void reset_connection_state();

  oklib::net::EventLoop* loop_;
  std::string name_;
  WebSocketOptions options_;
  oklib::http::TlsClientOptions tls_options_;
  std::unique_ptr<oklib::net::TcpClient> client_;
  oklib::net::TcpConnectionPtr connection_;
  WebSocketChannelPtr channel_;
  OpenCallback open_callback_;
  MessageCallback message_callback_;
  CloseCallback close_callback_;
  ErrorCallback error_callback_;
  oklib::http::HttpParser parser_{oklib::http::HttpParserMode::response};
  WebSocketFrameParser frame_parser_;
  WebSocketMessageAssembler message_assembler_;
  std::string host_header_;
  std::string host_name_;
  std::string target_;
  std::string websocket_key_;
  oklib::http::HttpHeaders headers_;
  bool handshake_complete_{false};
  bool compression_enabled_{false};
  bool tls_ready_{false};
  bool close_notified_{false};
#if OKLIB_ENABLE_TLS
  class TlsState;
  std::unique_ptr<TlsState> tls_state_;
#endif
};

}  // namespace oklib::websocket
