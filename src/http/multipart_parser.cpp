#include "oklib/http/multipart_parser.h"

#include "oklib/http/url_encoding.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace oklib::http {
namespace {

bool is_ows(char ch) noexcept {
  return ch == ' ' || ch == '\t';
}

std::string_view trim_view(std::string_view value) {
  while (!value.empty() && is_ows(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_ows(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

std::string trim(std::string_view value) {
  value = trim_view(value);
  return std::string(value);
}

std::string lower(std::string_view value) {
  std::string result(value);
  for (char& ch : result) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return result;
}

bool starts_with(std::string_view value, std::string_view prefix) noexcept {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string unquote(std::string_view value) {
  value = trim_view(value);
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return std::string(value);
  }

  std::string result;
  result.reserve(value.size() - 2);
  for (std::size_t i = 1; i + 1 < value.size(); ++i) {
    if (value[i] == '\\' && i + 2 < value.size()) {
      result.push_back(value[++i]);
      continue;
    }
    result.push_back(value[i]);
  }
  return result;
}

std::vector<std::string_view> split_parameters(std::string_view value) {
  std::vector<std::string_view> parts;
  std::size_t begin = 0;
  bool quoted = false;
  bool escaped = false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (quoted && ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      quoted = !quoted;
      continue;
    }
    if (!quoted && ch == ';') {
      parts.push_back(value.substr(begin, i - begin));
      begin = i + 1;
    }
  }
  parts.push_back(value.substr(begin));
  return parts;
}

struct ParsedHeaderValue {
  std::string token;
  std::vector<std::pair<std::string, std::string>> params;
};

ParsedHeaderValue parse_header_value(std::string_view value) {
  ParsedHeaderValue parsed;
  const auto parts = split_parameters(value);
  if (parts.empty()) {
    return parsed;
  }
  parsed.token = lower(trim(parts.front()));
  for (std::size_t i = 1; i < parts.size(); ++i) {
    const auto part = parts[i];
    const auto equal = part.find('=');
    if (equal == std::string_view::npos) {
      continue;
    }
    auto key = lower(trim(part.substr(0, equal)));
    auto param_value = unquote(part.substr(equal + 1));
    if (!key.empty()) {
      parsed.params.push_back({std::move(key), std::move(param_value)});
    }
  }
  return parsed;
}

std::string parameter_value(const ParsedHeaderValue& parsed, std::string_view key) {
  for (const auto& [field, value] : parsed.params) {
    if (field == key) {
      return value;
    }
  }
  return {};
}

std::string decoded_parameter_value(const ParsedHeaderValue& parsed, std::string_view key) {
  return url_decode(parameter_value(parsed, key), UrlDecodeMode::percent);
}

std::string decoded_filename_value(const ParsedHeaderValue& parsed) {
  auto filename = decoded_parameter_value(parsed, "filename");
  const auto filename_star = parameter_value(parsed, "filename*");
  if (!filename_star.empty()) {
    if (auto decoded = decode_rfc5987_value(filename_star); decoded.has_value()) {
      filename = std::move(*decoded);
    }
  }
  return filename;
}

MultipartParseResult failure(MultipartParseError error, std::string message) {
  MultipartParseResult result;
  result.error = error;
  result.error_message = std::move(message);
  return result;
}

bool parse_part_headers(std::string_view block, HttpHeaders* headers) {
  std::size_t begin = 0;
  while (begin < block.size()) {
    const auto line_end = block.find("\r\n", begin);
    const auto line = line_end == std::string_view::npos
                          ? block.substr(begin)
                          : block.substr(begin, line_end - begin);
    begin = line_end == std::string_view::npos ? block.size() : line_end + 2;
    if (line.empty()) {
      continue;
    }
    if (line.front() == ' ' || line.front() == '\t') {
      return false;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      return false;
    }
    headers->add(line.substr(0, colon), line.substr(colon + 1));
  }
  return true;
}

}  // namespace

const MultipartPart* MultipartParseResult::find(std::string_view name) const noexcept {
  const auto it = std::find_if(parts.begin(), parts.end(), [name](const MultipartPart& part) {
    return part.name == name;
  });
  return it == parts.end() ? nullptr : &*it;
}

std::optional<std::string> multipart_boundary(std::string_view content_type) {
  const auto parsed = parse_header_value(content_type);
  if (parsed.token != "multipart/form-data") {
    return std::nullopt;
  }

  auto boundary = parameter_value(parsed, "boundary");
  if (boundary.empty()) {
    return std::nullopt;
  }
  return boundary;
}

MultipartParseResult parse_multipart_form_data(std::string_view content_type,
                                               std::string_view body,
                                               const MultipartParseOptions& options) {
  if (body.size() > options.max_body_bytes) {
    return failure(MultipartParseError::limits_exceeded, "multipart body exceeds max_body_bytes");
  }

  const auto boundary = multipart_boundary(content_type);
  if (!boundary.has_value()) {
    const auto parsed = parse_header_value(content_type);
    return failure(parsed.token == "multipart/form-data"
                       ? MultipartParseError::missing_boundary
                       : MultipartParseError::invalid_content_type,
                   "multipart/form-data boundary is missing");
  }

  const std::string marker = "--" + *boundary;
  if (!starts_with(body, marker)) {
    return failure(MultipartParseError::malformed_body, "multipart body does not start with boundary");
  }

  MultipartParseResult result;
  std::size_t position = marker.size();
  for (;;) {
    if (position + 2 <= body.size() && body.substr(position, 2) == "--") {
      result.error = MultipartParseError::none;
      return result;
    }
    if (position + 2 > body.size() || body.substr(position, 2) != "\r\n") {
      return failure(MultipartParseError::malformed_body, "boundary line is malformed");
    }
    position += 2;

    const auto header_end = body.find("\r\n\r\n", position);
    if (header_end == std::string_view::npos) {
      return failure(MultipartParseError::malformed_headers, "part headers are incomplete");
    }
    if (header_end - position > options.max_header_bytes) {
      return failure(MultipartParseError::limits_exceeded, "part headers exceed max_header_bytes");
    }

    MultipartPart part;
    if (!parse_part_headers(body.substr(position, header_end - position), &part.headers)) {
      return failure(MultipartParseError::malformed_headers, "part header line is malformed");
    }

    const auto disposition = parse_header_value(part.headers.get("Content-Disposition"));
    if (disposition.token != "form-data") {
      return failure(MultipartParseError::malformed_headers,
                     "part Content-Disposition must be form-data");
    }
    part.name = decoded_parameter_value(disposition, "name");
    part.filename = decoded_filename_value(disposition);
    part.content_type = part.headers.get("Content-Type");

    const std::size_t content_begin = header_end + 4;
    const std::string delimiter = "\r\n" + marker;
    const auto delimiter_position = body.find(delimiter, content_begin);
    if (delimiter_position == std::string_view::npos) {
      return failure(MultipartParseError::malformed_body, "next multipart boundary is missing");
    }

    part.body.assign(body.substr(content_begin, delimiter_position - content_begin));
    result.parts.push_back(std::move(part));
    if (result.parts.size() > options.max_parts) {
      return failure(MultipartParseError::limits_exceeded, "multipart part count exceeds max_parts");
    }

    position = delimiter_position + delimiter.size();
  }
}

}  // namespace oklib::http
