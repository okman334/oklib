#include <oklib/net/buffer.h>
#include <oklib/websocket/websocket_frame.h>
#include <oklib/websocket/websocket_handshake.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

void test_rfc_accept_key() {
  const std::string accept =
      oklib::websocket::websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
  require(accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "RFC6455 accept key matches");
}

void test_server_text_frame_encoding() {
  oklib::websocket::WebSocketFrame frame;
  frame.fin = true;
  frame.opcode = oklib::websocket::WebSocketOpcode::text;
  frame.payload = "hi";

  const std::string encoded =
      oklib::websocket::encode_websocket_frame(frame, oklib::websocket::WebSocketMasking::unmasked);
  require(encoded.size() == 4, "server short text frame has 2 byte header");
  require(static_cast<unsigned char>(encoded[0]) == 0x81, "server text frame first byte");
  require(static_cast<unsigned char>(encoded[1]) == 0x02, "server text frame payload len");
  require(encoded.substr(2) == "hi", "server text frame payload");
}

void test_masked_client_frame_decodes_for_server() {
  oklib::websocket::WebSocketFrame frame;
  frame.fin = true;
  frame.opcode = oklib::websocket::WebSocketOpcode::text;
  frame.payload = "hello";

  const std::string encoded = oklib::websocket::encode_websocket_frame(
      frame,
      oklib::websocket::WebSocketMasking::masked,
      std::array<unsigned char, 4>{1, 2, 3, 4});

  oklib::net::Buffer buffer;
  buffer.append(encoded);
  oklib::websocket::WebSocketFrameParser parser(oklib::websocket::WebSocketEndpointRole::server);
  std::vector<oklib::websocket::WebSocketFrame> frames;
  const auto status = parser.parse(&buffer, &frames);

  require(status == oklib::websocket::WebSocketParseStatus::complete,
          "masked client frame parses");
  require(frames.size() == 1, "one masked frame parsed");
  require(frames[0].opcode == oklib::websocket::WebSocketOpcode::text,
          "masked frame opcode parsed");
  require(frames[0].payload == "hello", "masked frame payload decoded");
}

void test_fragmented_message_reassembles() {
  oklib::websocket::WebSocketOptions options;
  oklib::websocket::WebSocketMessageAssembler assembler(options);

  oklib::websocket::WebSocketFrame first;
  first.fin = false;
  first.opcode = oklib::websocket::WebSocketOpcode::text;
  first.payload = "hel";

  oklib::websocket::WebSocketFrame second;
  second.fin = true;
  second.opcode = oklib::websocket::WebSocketOpcode::continuation;
  second.payload = "lo";

  oklib::websocket::WebSocketMessage message;
  require(assembler.consume(first, &message) == oklib::websocket::WebSocketMessageStatus::incomplete,
          "first fragment is incomplete");
  require(assembler.consume(second, &message) == oklib::websocket::WebSocketMessageStatus::complete,
          "second fragment completes message");
  require(message.opcode == oklib::websocket::WebSocketOpcode::text,
          "fragmented message opcode preserved");
  require(message.payload == "hello", "fragmented message payload reassembled");
}

void test_extended_payload_frame_decodes() {
  oklib::websocket::WebSocketFrame frame;
  frame.opcode = oklib::websocket::WebSocketOpcode::binary;
  frame.payload.assign(130, 'x');

  const std::string encoded =
      oklib::websocket::encode_websocket_frame(frame, oklib::websocket::WebSocketMasking::unmasked);
  require(static_cast<unsigned char>(encoded[1]) == 126, "extended 16-bit length marker");
  require(static_cast<unsigned char>(encoded[2]) == 0, "extended length high byte");
  require(static_cast<unsigned char>(encoded[3]) == 130, "extended length low byte");

  oklib::net::Buffer buffer;
  buffer.append(encoded);
  oklib::websocket::WebSocketFrameParser parser(oklib::websocket::WebSocketEndpointRole::client);
  std::vector<oklib::websocket::WebSocketFrame> frames;
  require(parser.parse(&buffer, &frames) == oklib::websocket::WebSocketParseStatus::complete,
          "extended payload frame parses");
  require(frames.size() == 1, "one extended payload frame parsed");
  require(frames[0].payload == frame.payload, "extended payload preserved");
}

void test_server_rejects_unmasked_client_frame() {
  oklib::websocket::WebSocketFrame frame;
  frame.opcode = oklib::websocket::WebSocketOpcode::text;
  frame.payload = "bad";

  oklib::net::Buffer buffer;
  buffer.append(oklib::websocket::encode_websocket_frame(
      frame, oklib::websocket::WebSocketMasking::unmasked));
  oklib::websocket::WebSocketFrameParser parser(oklib::websocket::WebSocketEndpointRole::server);
  std::vector<oklib::websocket::WebSocketFrame> frames;
  require(parser.parse(&buffer, &frames) == oklib::websocket::WebSocketParseStatus::error,
          "server rejects unmasked client frame");
  require(parser.error() == oklib::websocket::WebSocketError::protocol_error,
          "unmasked client frame is protocol error");
}

void test_fragmented_control_frame_rejected() {
  oklib::websocket::WebSocketFrame frame;
  frame.fin = false;
  frame.opcode = oklib::websocket::WebSocketOpcode::ping;
  frame.payload = "x";

  oklib::net::Buffer buffer;
  buffer.append(oklib::websocket::encode_websocket_frame(
      frame,
      oklib::websocket::WebSocketMasking::masked,
      std::array<unsigned char, 4>{0, 0, 0, 0}));
  oklib::websocket::WebSocketFrameParser parser(oklib::websocket::WebSocketEndpointRole::server);
  std::vector<oklib::websocket::WebSocketFrame> frames;
  require(parser.parse(&buffer, &frames) == oklib::websocket::WebSocketParseStatus::error,
          "fragmented control frame rejected");
}

void test_invalid_utf8_text_rejected() {
  oklib::websocket::WebSocketMessageAssembler assembler;
  oklib::websocket::WebSocketFrame frame;
  frame.opcode = oklib::websocket::WebSocketOpcode::text;
  frame.payload.assign("\xC3\x28", 2);

  oklib::websocket::WebSocketMessage message;
  require(assembler.consume(frame, &message) == oklib::websocket::WebSocketMessageStatus::error,
          "invalid UTF-8 text rejected");
  require(assembler.error() == oklib::websocket::WebSocketError::invalid_utf8,
          "invalid UTF-8 maps to invalid_utf8");
}

}  // namespace

int main() {
  test_rfc_accept_key();
  test_server_text_frame_encoding();
  test_masked_client_frame_decodes_for_server();
  test_fragmented_message_reassembles();
  test_extended_payload_frame_decodes();
  test_server_rejects_unmasked_client_frame();
  test_fragmented_control_frame_rejected();
  test_invalid_utf8_text_rejected();
  return 0;
}
