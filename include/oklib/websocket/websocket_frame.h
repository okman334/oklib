#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oklib::net {
class Buffer;
}

namespace oklib::websocket {

enum class WebSocketOpcode : std::uint8_t {
  continuation = 0x0,
  text = 0x1,
  binary = 0x2,
  close = 0x8,
  ping = 0x9,
  pong = 0xA,
};

enum class WebSocketEndpointRole {
  client,
  server,
};

enum class WebSocketMasking {
  unmasked,
  masked,
};

enum class WebSocketParseStatus {
  incomplete,
  complete,
  error,
};

enum class WebSocketMessageStatus {
  incomplete,
  complete,
  control,
  error,
};

enum class WebSocketError {
  none,
  bad_handshake,
  unsupported_url,
  tls_not_enabled,
  connection_error,
  protocol_error,
  message_too_big,
  invalid_utf8,
  compression_error,
};

struct WebSocketOptions {
  std::size_t max_frame_payload{16 * 1024 * 1024};
  std::size_t max_message_payload{64 * 1024 * 1024};
  bool enable_compression{false};
};

struct WebSocketCloseInfo {
  std::uint16_t code{1005};
  std::string reason;
};

struct WebSocketFrame {
  bool fin{true};
  bool rsv1{false};
  bool rsv2{false};
  bool rsv3{false};
  WebSocketOpcode opcode{WebSocketOpcode::text};
  std::string payload;
};

struct WebSocketMessage {
  WebSocketOpcode opcode{WebSocketOpcode::text};
  std::string payload;
  bool compressed{false};
};

[[nodiscard]] bool is_control_opcode(WebSocketOpcode opcode) noexcept;
[[nodiscard]] bool is_data_opcode(WebSocketOpcode opcode) noexcept;
[[nodiscard]] bool valid_close_code(std::uint16_t code) noexcept;
[[nodiscard]] bool valid_utf8(std::string_view value) noexcept;

[[nodiscard]] std::string encode_websocket_frame(
    const WebSocketFrame& frame,
    WebSocketMasking masking,
    std::optional<std::array<unsigned char, 4>> mask_key = std::nullopt);

class WebSocketFrameParser {
 public:
  explicit WebSocketFrameParser(WebSocketEndpointRole role,
                                WebSocketOptions options = {});

  WebSocketParseStatus parse(oklib::net::Buffer* buffer, std::vector<WebSocketFrame>* frames);
  [[nodiscard]] WebSocketError error() const noexcept { return error_; }

 private:
  bool set_error(WebSocketError error) noexcept;

  WebSocketEndpointRole role_;
  WebSocketOptions options_;
  WebSocketError error_{WebSocketError::none};
};

class WebSocketMessageAssembler {
 public:
  explicit WebSocketMessageAssembler(WebSocketOptions options = {});

  WebSocketMessageStatus consume(const WebSocketFrame& frame, WebSocketMessage* message);
  void reset();
  [[nodiscard]] WebSocketError error() const noexcept { return error_; }

 private:
  bool set_error(WebSocketError error) noexcept;

  WebSocketOptions options_;
  WebSocketOpcode fragmented_opcode_{WebSocketOpcode::continuation};
  std::string fragmented_payload_;
  bool fragmented_compressed_{false};
  bool fragmented_active_{false};
  WebSocketError error_{WebSocketError::none};
};

}  // namespace oklib::websocket
