#pragma once

#include <any>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "oklib/net/callbacks.h"
#include "oklib/websocket/websocket_frame.h"

namespace oklib::websocket {

class WebSocketChannel : public std::enable_shared_from_this<WebSocketChannel> {
 public:
  WebSocketChannel(oklib::net::TcpConnectionPtr connection,
                   WebSocketEndpointRole role,
                   WebSocketOptions options = {},
                   bool compression_enabled = false);

  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] const oklib::net::TcpConnectionPtr& tcp_connection() const noexcept {
    return connection_;
  }
  [[nodiscard]] bool compression_enabled() const noexcept { return compression_enabled_; }

  bool send_text(std::string_view message);
  bool send_binary(std::string_view message);
  bool send_ping(std::string_view payload = {});
  bool send_pong(std::string_view payload = {});
  bool close(std::uint16_t code = 1000, std::string_view reason = {});
  bool send_frame(WebSocketFrame frame);

  void mark_close_received() noexcept { close_received_ = true; }
  [[nodiscard]] bool close_sent() const noexcept { return close_sent_; }

  void set_context(std::any context);
  [[nodiscard]] const std::any& context() const noexcept { return context_; }
  [[nodiscard]] std::any& mutable_context() noexcept { return context_; }

 private:
  oklib::net::TcpConnectionPtr connection_;
  WebSocketEndpointRole role_;
  WebSocketOptions options_;
  bool compression_enabled_{false};
  std::atomic<bool> close_sent_{false};
  std::atomic<bool> close_received_{false};
  mutable std::mutex context_mutex_;
  std::any context_;
};

using WebSocketChannelPtr = std::shared_ptr<WebSocketChannel>;

}  // namespace oklib::websocket
