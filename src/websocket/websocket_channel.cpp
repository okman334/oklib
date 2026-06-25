#include "oklib/websocket/websocket_channel.h"

#include <utility>

#include "oklib/net/tcp_connection.h"
#include "websocket_compression.h"

namespace oklib::websocket {

WebSocketChannel::WebSocketChannel(oklib::net::TcpConnectionPtr connection,
                                   WebSocketEndpointRole role,
                                   WebSocketOptions options,
                                   bool compression_enabled)
    : connection_(std::move(connection)),
      role_(role),
      options_(options),
      compression_enabled_(compression_enabled) {}

bool WebSocketChannel::connected() const noexcept {
  return connection_ && connection_->connected();
}

bool WebSocketChannel::send_text(std::string_view message) {
  WebSocketFrame frame;
  frame.opcode = WebSocketOpcode::text;
  frame.payload.assign(message);
  return send_frame(std::move(frame));
}

bool WebSocketChannel::send_binary(std::string_view message) {
  WebSocketFrame frame;
  frame.opcode = WebSocketOpcode::binary;
  frame.payload.assign(message);
  return send_frame(std::move(frame));
}

bool WebSocketChannel::send_ping(std::string_view payload) {
  if (payload.size() > 125) {
    return false;
  }
  WebSocketFrame frame;
  frame.opcode = WebSocketOpcode::ping;
  frame.payload.assign(payload);
  return send_frame(std::move(frame));
}

bool WebSocketChannel::send_pong(std::string_view payload) {
  if (payload.size() > 125) {
    return false;
  }
  WebSocketFrame frame;
  frame.opcode = WebSocketOpcode::pong;
  frame.payload.assign(payload);
  return send_frame(std::move(frame));
}

bool WebSocketChannel::close(std::uint16_t code, std::string_view reason) {
  if (!valid_close_code(code) || reason.size() > 123) {
    return false;
  }
  bool expected = false;
  if (!close_sent_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return true;
  }
  WebSocketFrame frame;
  frame.opcode = WebSocketOpcode::close;
  frame.payload.push_back(static_cast<char>((code >> 8) & 0xFF));
  frame.payload.push_back(static_cast<char>(code & 0xFF));
  frame.payload.append(reason);
  const bool ok = send_frame(std::move(frame));
  if (connection_) {
    connection_->shutdown();
  }
  return ok;
}

bool WebSocketChannel::send_frame(WebSocketFrame frame) {
  if (!connected()) {
    return false;
  }
  WebSocketMasking masking = role_ == WebSocketEndpointRole::client
                                 ? WebSocketMasking::masked
                                 : WebSocketMasking::unmasked;
  if (compression_enabled_ && is_data_opcode(frame.opcode) && !frame.payload.empty()) {
    std::string compressed;
    if (websocket_deflate(frame.payload, &compressed)) {
      frame.payload = std::move(compressed);
      frame.rsv1 = true;
    }
  }
  connection_->send(encode_websocket_frame(frame, masking));
  return true;
}

void WebSocketChannel::set_context(std::any context) {
  std::lock_guard lock(context_mutex_);
  context_ = std::move(context);
}

}  // namespace oklib::websocket
