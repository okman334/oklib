#include "oklib/http/url_encoding.h"

#include <algorithm>
#include <cctype>

namespace oklib::http {
namespace {

int hex_digit_value(char ch) noexcept {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

std::string lower_ascii(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return result;
}

}  // namespace

std::string url_decode(std::string_view input, UrlDecodeMode mode) {
  std::string decoded;
  decoded.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      const int high = hex_digit_value(input[i + 1]);
      const int low = hex_digit_value(input[i + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    if (mode == UrlDecodeMode::form && input[i] == '+') {
      decoded.push_back(' ');
      continue;
    }
    decoded.push_back(input[i]);
  }
  return decoded;
}

std::optional<std::string> decode_rfc5987_value(std::string_view input) {
  const auto charset_end = input.find('\'');
  if (charset_end == std::string_view::npos) {
    return std::nullopt;
  }
  const auto language_end = input.find('\'', charset_end + 1);
  if (language_end == std::string_view::npos) {
    return std::nullopt;
  }

  const auto charset = lower_ascii(input.substr(0, charset_end));
  if (charset != "utf-8" && charset != "us-ascii") {
    return std::nullopt;
  }

  return url_decode(input.substr(language_end + 1), UrlDecodeMode::percent);
}

}  // namespace oklib::http
