#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "oklib/http/http_headers.h"

namespace oklib::http {

enum class MultipartParseError {
  none,
  invalid_content_type,
  missing_boundary,
  malformed_body,
  malformed_headers,
  limits_exceeded,
};

struct MultipartPart {
  HttpHeaders headers;
  std::string name;
  std::string filename;
  std::string content_type;
  std::string body;

  [[nodiscard]] bool is_file() const noexcept { return !filename.empty(); }
};

struct MultipartParseOptions {
  std::size_t max_parts{128};
  std::size_t max_header_bytes{64 * 1024};
  std::size_t max_body_bytes{64 * 1024 * 1024};
};

struct MultipartParseResult {
  std::vector<MultipartPart> parts;
  MultipartParseError error{MultipartParseError::none};
  std::string error_message;

  [[nodiscard]] bool ok() const noexcept { return error == MultipartParseError::none; }
  [[nodiscard]] const MultipartPart* find(std::string_view name) const noexcept;
};

[[nodiscard]] std::optional<std::string> multipart_boundary(std::string_view content_type);

[[nodiscard]] MultipartParseResult parse_multipart_form_data(
    std::string_view content_type,
    std::string_view body,
    const MultipartParseOptions& options = {});

}  // namespace oklib::http
