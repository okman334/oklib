#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace oklib::http {

enum class UrlDecodeMode {
  percent,
  form,
};

[[nodiscard]] std::string url_decode(
    std::string_view input,
    UrlDecodeMode mode = UrlDecodeMode::percent);

[[nodiscard]] std::optional<std::string> decode_rfc5987_value(std::string_view input);

}  // namespace oklib::http
