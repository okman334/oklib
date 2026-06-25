#include "oklib/websocket/websocket_handshake.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

#include "oklib/http/http_request.h"

namespace oklib::websocket {
namespace {

constexpr std::string_view k_websocket_magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr char k_base64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char* data, std::size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (std::size_t i = 0; i < len; i += 3) {
    const std::uint32_t octet_a = data[i];
    const std::uint32_t octet_b = i + 1 < len ? data[i + 1] : 0;
    const std::uint32_t octet_c = i + 2 < len ? data[i + 2] : 0;
    const std::uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
    out.push_back(k_base64[(triple >> 18) & 0x3F]);
    out.push_back(k_base64[(triple >> 12) & 0x3F]);
    out.push_back(i + 1 < len ? k_base64[(triple >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < len ? k_base64[triple & 0x3F] : '=');
  }
  return out;
}

struct Sha1 {
  std::array<std::uint32_t, 5> h{0x67452301U,
                                0xEFCDAB89U,
                                0x98BADCFEU,
                                0x10325476U,
                                0xC3D2E1F0U};
  std::array<unsigned char, 64> block{};
  std::uint64_t total_bytes{0};
  std::size_t block_size{0};

  static std::uint32_t left_rotate(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
  }

  void process_block(const unsigned char* data) {
    std::array<std::uint32_t, 80> w{};
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24) |
             (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(data[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = left_rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = h[0];
    std::uint32_t b = h[1];
    std::uint32_t c = h[2];
    std::uint32_t d = h[3];
    std::uint32_t e = h[4];

    for (int i = 0; i < 80; ++i) {
      std::uint32_t f = 0;
      std::uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999U;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1U;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCU;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6U;
      }
      const std::uint32_t temp = left_rotate(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = left_rotate(b, 30);
      b = a;
      a = temp;
    }

    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  void update(std::string_view data) {
    total_bytes += data.size();
    const auto* input = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t remaining = data.size();
    while (remaining > 0) {
      const std::size_t copy = std::min<std::size_t>(remaining, block.size() - block_size);
      std::memcpy(block.data() + block_size, input, copy);
      block_size += copy;
      input += copy;
      remaining -= copy;
      if (block_size == block.size()) {
        process_block(block.data());
        block_size = 0;
      }
    }
  }

  std::array<unsigned char, 20> final() {
    const std::uint64_t total_bits = total_bytes * 8;
    block[block_size++] = 0x80;
    if (block_size > 56) {
      while (block_size < 64) {
        block[block_size++] = 0;
      }
      process_block(block.data());
      block_size = 0;
    }
    while (block_size < 56) {
      block[block_size++] = 0;
    }
    for (int i = 7; i >= 0; --i) {
      block[block_size++] = static_cast<unsigned char>((total_bits >> (i * 8)) & 0xFF);
    }
    process_block(block.data());

    std::array<unsigned char, 20> digest{};
    for (std::size_t i = 0; i < h.size(); ++i) {
      digest[i * 4] = static_cast<unsigned char>((h[i] >> 24) & 0xFF);
      digest[i * 4 + 1] = static_cast<unsigned char>((h[i] >> 16) & 0xFF);
      digest[i * 4 + 2] = static_cast<unsigned char>((h[i] >> 8) & 0xFF);
      digest[i * 4 + 3] = static_cast<unsigned char>(h[i] & 0xFF);
    }
    return digest;
  }
};

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
        std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

std::string_view trim_ows(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

}  // namespace

std::string websocket_accept_key(std::string_view key) {
  Sha1 sha1;
  std::string input(key);
  input.append(k_websocket_magic);
  sha1.update(input);
  const auto digest = sha1.final();
  return base64_encode(digest.data(), digest.size());
}

std::string websocket_generate_key() {
  std::array<unsigned char, 16> key{};
  std::random_device rd;
  for (auto& byte : key) {
    byte = static_cast<unsigned char>(rd());
  }
  return base64_encode(key.data(), key.size());
}

std::vector<std::string> websocket_split_tokens(std::string_view value) {
  std::vector<std::string> result;
  while (!value.empty()) {
    const auto comma = value.find(',');
    const auto part = trim_ows(comma == std::string_view::npos ? value : value.substr(0, comma));
    if (!part.empty()) {
      result.emplace_back(part);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    value.remove_prefix(comma + 1);
  }
  return result;
}

bool websocket_header_has_token(std::string_view value, std::string_view token) {
  for (const auto& part : websocket_split_tokens(value)) {
    if (equals_ignore_case(part, token)) {
      return true;
    }
  }
  return false;
}

bool websocket_is_upgrade_request(const oklib::http::HttpRequest& request) {
  return request.method_string() == "GET" &&
         request.version() == oklib::http::HttpVersion::http11 &&
         websocket_header_has_token(request.header("Connection"), "Upgrade") &&
         equals_ignore_case(request.header("Upgrade"), "websocket") &&
         request.header("Sec-WebSocket-Version") == "13" &&
         !request.header("Sec-WebSocket-Key").empty();
}

std::vector<std::string> websocket_requested_subprotocols(
    const oklib::http::HttpRequest& request) {
  return websocket_split_tokens(request.header("Sec-WebSocket-Protocol"));
}

std::optional<std::string> websocket_validate_client_key(std::string_view key) {
  key = trim_ows(key);
  if (key.empty()) {
    return std::nullopt;
  }
  return websocket_accept_key(key);
}

}  // namespace oklib::websocket
