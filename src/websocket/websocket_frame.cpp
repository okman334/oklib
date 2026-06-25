#include "oklib/websocket/websocket_frame.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

#include "oklib/net/buffer.h"

namespace oklib::websocket {
namespace {

bool valid_opcode(std::uint8_t opcode) {
  switch (opcode) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x8:
    case 0x9:
    case 0xA:
      return true;
    default:
      return false;
  }
}

std::array<unsigned char, 4> random_mask_key() {
  static thread_local std::mt19937 generator(std::random_device{}());
  std::array<unsigned char, 4> key{};
  for (auto& byte : key) {
    byte = static_cast<unsigned char>(generator() & 0xFF);
  }
  return key;
}

void append_u16(std::string* output, std::uint16_t value) {
  output->push_back(static_cast<char>((value >> 8) & 0xFF));
  output->push_back(static_cast<char>(value & 0xFF));
}

void append_u64(std::string* output, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    output->push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

std::uint16_t read_u16(const unsigned char* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

std::uint64_t read_u64(const unsigned char* data) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | data[i];
  }
  return value;
}

}  // namespace

bool is_control_opcode(WebSocketOpcode opcode) noexcept {
  const auto value = static_cast<std::uint8_t>(opcode);
  return value >= 0x8;
}

bool is_data_opcode(WebSocketOpcode opcode) noexcept {
  return opcode == WebSocketOpcode::text || opcode == WebSocketOpcode::binary;
}

bool valid_close_code(std::uint16_t code) noexcept {
  if (code < 1000) {
    return false;
  }
  if (code == 1004 || code == 1005 || code == 1006) {
    return false;
  }
  if (code >= 1016 && code <= 2999) {
    return false;
  }
  return code <= 4999;
}

bool valid_utf8(std::string_view value) noexcept {
  std::uint32_t codepoint = 0;
  int remaining = 0;
  std::uint32_t min_value = 0;
  for (unsigned char byte : value) {
    if (remaining == 0) {
      if (byte <= 0x7F) {
        continue;
      }
      if (byte >= 0xC2 && byte <= 0xDF) {
        remaining = 1;
        codepoint = byte & 0x1F;
        min_value = 0x80;
      } else if (byte >= 0xE0 && byte <= 0xEF) {
        remaining = 2;
        codepoint = byte & 0x0F;
        min_value = 0x800;
      } else if (byte >= 0xF0 && byte <= 0xF4) {
        remaining = 3;
        codepoint = byte & 0x07;
        min_value = 0x10000;
      } else {
        return false;
      }
      continue;
    }

    if ((byte & 0xC0) != 0x80) {
      return false;
    }
    codepoint = (codepoint << 6) | (byte & 0x3F);
    --remaining;
    if (remaining == 0) {
      if (codepoint < min_value || codepoint > 0x10FFFF ||
          (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return false;
      }
    }
  }
  return remaining == 0;
}

std::string encode_websocket_frame(const WebSocketFrame& frame,
                                   WebSocketMasking masking,
                                   std::optional<std::array<unsigned char, 4>> mask_key) {
  const bool masked = masking == WebSocketMasking::masked;
  std::string output;
  const auto len = frame.payload.size();
  output.reserve(len + 14);

  unsigned char first = static_cast<unsigned char>(frame.opcode);
  if (frame.fin) {
    first |= 0x80;
  }
  if (frame.rsv1) {
    first |= 0x40;
  }
  if (frame.rsv2) {
    first |= 0x20;
  }
  if (frame.rsv3) {
    first |= 0x10;
  }
  output.push_back(static_cast<char>(first));

  unsigned char second = masked ? 0x80 : 0x00;
  if (len <= 125) {
    second |= static_cast<unsigned char>(len);
    output.push_back(static_cast<char>(second));
  } else if (len <= 0xFFFF) {
    second |= 126;
    output.push_back(static_cast<char>(second));
    append_u16(&output, static_cast<std::uint16_t>(len));
  } else {
    second |= 127;
    output.push_back(static_cast<char>(second));
    append_u64(&output, static_cast<std::uint64_t>(len));
  }

  std::array<unsigned char, 4> key{};
  if (masked) {
    key = mask_key.value_or(random_mask_key());
    output.append(reinterpret_cast<const char*>(key.data()), key.size());
  }

  if (masked) {
    for (std::size_t i = 0; i < frame.payload.size(); ++i) {
      output.push_back(static_cast<char>(
          static_cast<unsigned char>(frame.payload[i]) ^ key[i % key.size()]));
    }
  } else {
    output.append(frame.payload);
  }
  return output;
}

WebSocketFrameParser::WebSocketFrameParser(WebSocketEndpointRole role,
                                           WebSocketOptions options)
    : role_(role), options_(options) {}

WebSocketParseStatus WebSocketFrameParser::parse(oklib::net::Buffer* buffer,
                                                 std::vector<WebSocketFrame>* frames) {
  bool parsed_any = false;
  while (buffer->readable_bytes() >= 2) {
    const auto* data = reinterpret_cast<const unsigned char*>(buffer->peek());
    const bool fin = (data[0] & 0x80) != 0;
    const bool rsv1 = (data[0] & 0x40) != 0;
    const bool rsv2 = (data[0] & 0x20) != 0;
    const bool rsv3 = (data[0] & 0x10) != 0;
    const std::uint8_t opcode_value = data[0] & 0x0F;
    const bool masked = (data[1] & 0x80) != 0;
    std::uint64_t payload_len = data[1] & 0x7F;
    std::size_t header_len = 2;

    if (!valid_opcode(opcode_value)) {
      return set_error(WebSocketError::protocol_error) ? WebSocketParseStatus::error
                                                       : WebSocketParseStatus::error;
    }
    const auto opcode = static_cast<WebSocketOpcode>(opcode_value);

    const bool expected_mask = role_ == WebSocketEndpointRole::server;
    if (masked != expected_mask) {
      return set_error(WebSocketError::protocol_error) ? WebSocketParseStatus::error
                                                       : WebSocketParseStatus::error;
    }

    if ((rsv1 && !options_.enable_compression) || rsv2 || rsv3) {
      return set_error(WebSocketError::protocol_error) ? WebSocketParseStatus::error
                                                       : WebSocketParseStatus::error;
    }
    if (rsv1 && (!is_data_opcode(opcode) || opcode == WebSocketOpcode::continuation)) {
      return set_error(WebSocketError::protocol_error) ? WebSocketParseStatus::error
                                                       : WebSocketParseStatus::error;
    }

    if (payload_len == 126) {
      if (buffer->readable_bytes() < header_len + 2) {
        return parsed_any ? WebSocketParseStatus::complete : WebSocketParseStatus::incomplete;
      }
      payload_len = read_u16(data + header_len);
      header_len += 2;
      if (payload_len < 126) {
        return set_error(WebSocketError::protocol_error) ? WebSocketParseStatus::error
                                                         : WebSocketParseStatus::error;
      }
    } else if (payload_len == 127) {
      if (buffer->readable_bytes() < header_len + 8) {
        return parsed_any ? WebSocketParseStatus::complete : WebSocketParseStatus::incomplete;
      }
      payload_len = read_u64(data + header_len);
      header_len += 8;
      if (payload_len < 65536 || (payload_len & (1ULL << 63)) != 0) {
        return set_error(WebSocketError::protocol_error) ? WebSocketParseStatus::error
                                                         : WebSocketParseStatus::error;
      }
    }

    std::array<unsigned char, 4> key{};
    if (masked) {
      if (buffer->readable_bytes() < header_len + key.size()) {
        return parsed_any ? WebSocketParseStatus::complete : WebSocketParseStatus::incomplete;
      }
      std::copy(data + header_len, data + header_len + key.size(), key.begin());
      header_len += key.size();
    }

    if (payload_len > options_.max_frame_payload) {
      return set_error(WebSocketError::message_too_big) ? WebSocketParseStatus::error
                                                        : WebSocketParseStatus::error;
    }
    if (payload_len > static_cast<std::uint64_t>(buffer->readable_bytes() - header_len)) {
      return parsed_any ? WebSocketParseStatus::complete : WebSocketParseStatus::incomplete;
    }

    if (is_control_opcode(opcode) && (!fin || payload_len > 125)) {
      return set_error(WebSocketError::protocol_error) ? WebSocketParseStatus::error
                                                       : WebSocketParseStatus::error;
    }

    WebSocketFrame frame;
    frame.fin = fin;
    frame.rsv1 = rsv1;
    frame.rsv2 = rsv2;
    frame.rsv3 = rsv3;
    frame.opcode = opcode;
    frame.payload.assign(reinterpret_cast<const char*>(data + header_len),
                         static_cast<std::size_t>(payload_len));
    if (masked) {
      for (std::size_t i = 0; i < frame.payload.size(); ++i) {
        frame.payload[i] = static_cast<char>(
            static_cast<unsigned char>(frame.payload[i]) ^ key[i % key.size()]);
      }
    }
    buffer->retrieve(header_len + static_cast<std::size_t>(payload_len));
    frames->push_back(std::move(frame));
    parsed_any = true;
  }
  return parsed_any ? WebSocketParseStatus::complete : WebSocketParseStatus::incomplete;
}

bool WebSocketFrameParser::set_error(WebSocketError error) noexcept {
  error_ = error;
  return true;
}

WebSocketMessageAssembler::WebSocketMessageAssembler(WebSocketOptions options)
    : options_(options) {}

WebSocketMessageStatus WebSocketMessageAssembler::consume(const WebSocketFrame& frame,
                                                          WebSocketMessage* message) {
  if (is_control_opcode(frame.opcode)) {
    if (frame.opcode == WebSocketOpcode::close && frame.payload.size() >= 2) {
      const auto code = static_cast<std::uint16_t>(
          (static_cast<unsigned char>(frame.payload[0]) << 8) |
          static_cast<unsigned char>(frame.payload[1]));
      if (!valid_close_code(code)) {
        return set_error(WebSocketError::protocol_error) ? WebSocketMessageStatus::error
                                                         : WebSocketMessageStatus::error;
      }
      if (frame.payload.size() > 2 && !valid_utf8(std::string_view(frame.payload).substr(2))) {
        return set_error(WebSocketError::invalid_utf8) ? WebSocketMessageStatus::error
                                                       : WebSocketMessageStatus::error;
      }
    }
    return WebSocketMessageStatus::control;
  }

  if (is_data_opcode(frame.opcode)) {
    if (fragmented_active_) {
      return set_error(WebSocketError::protocol_error) ? WebSocketMessageStatus::error
                                                       : WebSocketMessageStatus::error;
    }
    if (frame.payload.size() > options_.max_message_payload) {
      return set_error(WebSocketError::message_too_big) ? WebSocketMessageStatus::error
                                                        : WebSocketMessageStatus::error;
    }
    if (frame.fin) {
      if (frame.opcode == WebSocketOpcode::text && !frame.rsv1 && !valid_utf8(frame.payload)) {
        return set_error(WebSocketError::invalid_utf8) ? WebSocketMessageStatus::error
                                                       : WebSocketMessageStatus::error;
      }
      message->opcode = frame.opcode;
      message->payload = frame.payload;
      message->compressed = frame.rsv1;
      return WebSocketMessageStatus::complete;
    }
    fragmented_active_ = true;
    fragmented_opcode_ = frame.opcode;
    fragmented_payload_ = frame.payload;
    fragmented_compressed_ = frame.rsv1;
    return WebSocketMessageStatus::incomplete;
  }

  if (frame.opcode != WebSocketOpcode::continuation || !fragmented_active_) {
    return set_error(WebSocketError::protocol_error) ? WebSocketMessageStatus::error
                                                     : WebSocketMessageStatus::error;
  }
  if (fragmented_payload_.size() + frame.payload.size() > options_.max_message_payload) {
    return set_error(WebSocketError::message_too_big) ? WebSocketMessageStatus::error
                                                      : WebSocketMessageStatus::error;
  }
  fragmented_payload_.append(frame.payload);
  if (!frame.fin) {
    return WebSocketMessageStatus::incomplete;
  }

  if (fragmented_opcode_ == WebSocketOpcode::text && !fragmented_compressed_ &&
      !valid_utf8(fragmented_payload_)) {
    return set_error(WebSocketError::invalid_utf8) ? WebSocketMessageStatus::error
                                                   : WebSocketMessageStatus::error;
  }
  message->opcode = fragmented_opcode_;
  message->payload = std::move(fragmented_payload_);
  message->compressed = fragmented_compressed_;
  reset();
  return WebSocketMessageStatus::complete;
}

void WebSocketMessageAssembler::reset() {
  fragmented_opcode_ = WebSocketOpcode::continuation;
  fragmented_payload_.clear();
  fragmented_compressed_ = false;
  fragmented_active_ = false;
  error_ = WebSocketError::none;
}

bool WebSocketMessageAssembler::set_error(WebSocketError error) noexcept {
  error_ = error;
  return true;
}

}  // namespace oklib::websocket
