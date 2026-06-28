#include "oklib/http/streaming_multipart_parser.h"

#include "oklib/http/url_encoding.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

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
  for (auto& ch : result) {
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
    const auto equal = parts[i].find('=');
    if (equal == std::string_view::npos) {
      continue;
    }
    auto key = lower(trim(parts[i].substr(0, equal)));
    auto parsed_value = unquote(parts[i].substr(equal + 1));
    if (!key.empty()) {
      parsed.params.push_back({std::move(key), std::move(parsed_value)});
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

StreamingMultipartParser::StreamingMultipartParser(
    std::string boundary,
    PartCallback part_callback,
    DataCallback data_callback,
    CompleteCallback complete_callback,
    StreamingMultipartParserOptions options)
    : boundary_(std::move(boundary)),
      marker_("--" + boundary_),
      delimiter_("\r\n" + marker_),
      part_callback_(std::move(part_callback)),
      data_callback_(std::move(data_callback)),
      complete_callback_(std::move(complete_callback)),
      options_(options) {
  if (boundary_.empty()) {
    fail("multipart boundary is empty");
  }
}

bool StreamingMultipartParser::append(std::string_view data) {
  if (state_ == State::done) {
    return data.empty();
  }
  if (state_ == State::error) {
    return false;
  }
  buffer_.append(data);
  return process();
}

bool StreamingMultipartParser::finish() {
  if (!process()) {
    return false;
  }
  if (state_ != State::done) {
    return fail("multipart body ended before final boundary");
  }
  complete_once();
  return true;
}

bool StreamingMultipartParser::complete() const noexcept {
  return state_ == State::done;
}

bool StreamingMultipartParser::process() {
  for (;;) {
    switch (state_) {
      case State::first_boundary:
        if (!process_first_boundary()) {
          return state_ != State::error;
        }
        break;
      case State::headers:
        if (!process_headers()) {
          return state_ != State::error;
        }
        break;
      case State::body:
        if (!process_body()) {
          return state_ != State::error;
        }
        break;
      case State::boundary_suffix:
        if (!process_boundary_suffix()) {
          return state_ != State::error;
        }
        break;
      case State::done:
        complete_once();
        return true;
      case State::error:
        return false;
    }
  }
}

bool StreamingMultipartParser::process_first_boundary() {
  if (buffer_.size() < marker_.size() + 2) {
    return false;
  }
  if (!starts_with(buffer_, marker_)) {
    return fail("multipart body does not start with boundary");
  }
  buffer_.erase(0, marker_.size());
  return process_boundary_suffix();
}

bool StreamingMultipartParser::process_headers() {
  const auto header_end = buffer_.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    if (buffer_.size() > options_.max_header_bytes) {
      return fail("multipart part headers exceed max_header_bytes");
    }
    return false;
  }
  if (header_end > options_.max_header_bytes) {
    return fail("multipart part headers exceed max_header_bytes");
  }

  StreamingMultipartPart part;
  if (!parse_part_headers(std::string_view(buffer_).substr(0, header_end), &part.headers)) {
    return fail("multipart part header line is malformed");
  }
  const auto disposition = parse_header_value(part.headers.get("Content-Disposition"));
  if (disposition.token != "form-data") {
    return fail("multipart part Content-Disposition must be form-data");
  }
  part.name = decoded_parameter_value(disposition, "name");
  part.filename = decoded_filename_value(disposition);
  part.content_type = part.headers.get("Content-Type");

  current_part_ = std::move(part);
  current_field_bytes_ = 0;
  if (part_callback_) {
    part_callback_(current_part_);
  }

  buffer_.erase(0, header_end + 4);
  state_ = State::body;
  return true;
}

bool StreamingMultipartParser::process_body() {
  const auto delimiter_position = buffer_.find(delimiter_);
  if (delimiter_position == std::string::npos) {
    const auto keep = std::min(buffer_.size(), delimiter_.size() - 1);
    if (buffer_.size() <= keep) {
      return false;
    }
    const auto emit_size = buffer_.size() - keep;
    emit_data(std::string_view(buffer_).substr(0, emit_size));
    buffer_.erase(0, emit_size);
    return false;
  }

  emit_data(std::string_view(buffer_).substr(0, delimiter_position));
  buffer_.erase(0, delimiter_position + delimiter_.size());
  state_ = State::boundary_suffix;
  return true;
}

bool StreamingMultipartParser::process_boundary_suffix() {
  if (buffer_.size() < 2) {
    return false;
  }
  if (starts_with(buffer_, "--")) {
    buffer_.erase(0, 2);
    if (starts_with(buffer_, "\r\n")) {
      buffer_.erase(0, 2);
    }
    state_ = State::done;
    complete_once();
    return true;
  }
  if (starts_with(buffer_, "\r\n")) {
    buffer_.erase(0, 2);
    state_ = State::headers;
    return true;
  }
  return fail("multipart boundary line is malformed");
}

bool StreamingMultipartParser::fail(std::string message) {
  state_ = State::error;
  error_message_ = std::move(message);
  return false;
}

void StreamingMultipartParser::complete_once() {
  if (completed_callback_called_) {
    return;
  }
  completed_callback_called_ = true;
  if (complete_callback_) {
    complete_callback_();
  }
}

void StreamingMultipartParser::emit_data(std::string_view data) {
  if (data.empty()) {
    return;
  }
  if (!current_part_.is_file()) {
    current_field_bytes_ += data.size();
    if (current_field_bytes_ > options_.max_field_bytes) {
      fail("multipart field exceeds max_field_bytes");
    }
    return;
  }
  if (data_callback_) {
    data_callback_(data);
  }
}

}  // namespace oklib::http
