#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "oklib/base/timestamp.h"
#include "oklib/http/http_headers.h"
#include "oklib/http/http_request.h"

namespace oklib::net {
class Buffer;
}

namespace oklib::http {

enum class HttpParserMode {
  request,
  response,
};

enum class HttpParseStatus {
  incomplete,
  complete,
  error,
};

enum class HttpParseError {
  none,
  bad_start_line,
  bad_header,
  bad_message_framing,
  header_too_large,
  body_too_large,
};

struct HttpParserOptions {
  std::size_t max_start_line{8 * 1024};
  std::size_t max_headers{64 * 1024};
  std::size_t max_body{8 * 1024 * 1024};
};

struct HttpResponseMessage {
  HttpVersion version{HttpVersion::unknown};
  int status_code{0};
  std::string reason_phrase;
  HttpHeaders headers;
  HttpHeaders trailers;
  std::string body;
  std::optional<std::uint64_t> content_length;
  bool chunked{false};
};

class HttpParser {
 public:
  explicit HttpParser(HttpParserMode mode = HttpParserMode::request,
                      HttpParserOptions options = {});

  HttpParseStatus parse_request(oklib::net::Buffer* buffer, oklib::Timestamp receive_time);
  HttpParseStatus parse_request_head(oklib::net::Buffer* buffer, oklib::Timestamp receive_time);
  HttpParseStatus parse_response(oklib::net::Buffer* buffer);
  HttpParseStatus parse_response_head(oklib::net::Buffer* buffer);

  void reset();

  [[nodiscard]] HttpParserMode mode() const noexcept { return mode_; }
  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] HttpParseError error() const noexcept { return error_; }
  [[nodiscard]] const HttpRequest& request() const noexcept { return request_; }
  [[nodiscard]] HttpRequest& mutable_request() noexcept { return request_; }
  [[nodiscard]] HttpRequest take_request() { return std::move(request_); }
  [[nodiscard]] const HttpResponseMessage& response() const noexcept { return response_; }
  [[nodiscard]] HttpResponseMessage& mutable_response() noexcept { return response_; }

 private:
  enum class State {
    start_line,
    headers,
    fixed_body,
    chunk_size,
    chunk_data,
    trailers,
    complete,
    error,
  };

  HttpParseStatus parse(oklib::net::Buffer* buffer, oklib::Timestamp receive_time);
  bool parse_request_line(std::string_view line, oklib::Timestamp receive_time);
  bool parse_response_line(std::string_view line);
  bool parse_header_line(std::string_view line, HttpHeaders* headers);
  bool finish_headers();
  bool parse_fixed_body(oklib::net::Buffer* buffer);
  bool parse_chunk_size(oklib::net::Buffer* buffer);
  bool parse_chunk_data(oklib::net::Buffer* buffer);
  bool parse_trailers(oklib::net::Buffer* buffer);
  bool finish_message();
  bool set_error(HttpParseError error) noexcept;

  HttpParserMode mode_;
  HttpParserOptions options_;
  State state_{State::start_line};
  HttpParseError error_{HttpParseError::none};
  HttpRequest request_;
  HttpResponseMessage response_;
  std::size_t header_bytes_{0};
  std::uint64_t fixed_body_bytes_{0};
  std::uint64_t current_chunk_size_{0};
  std::uint64_t decoded_body_bytes_{0};
  bool chunked_{false};
};

}  // namespace oklib::http
