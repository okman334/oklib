#pragma once

#include <map>
#include <string>
#include <string_view>

namespace oklib::net {
class Buffer;
}

namespace oklib::http {

enum class HttpStatusCode {
  unknown = 0,
  ok = 200,
  bad_request = 400,
  not_found = 404,
  not_implemented = 501,
};

class HttpResponse {
 public:
  explicit HttpResponse(bool close_connection = false) : close_connection_(close_connection) {}

  void set_status_code(HttpStatusCode code) noexcept { status_code_ = static_cast<int>(code); }
  void set_status_code(int code) noexcept { status_code_ = code; }
  void set_status_message(std::string message) { status_message_ = std::move(message); }
  void set_close_connection(bool on) noexcept { close_connection_ = on; }
  [[nodiscard]] bool close_connection() const noexcept { return close_connection_; }

  void set_content_type(std::string content_type) { add_header("Content-Type", content_type); }
  void add_header(std::string field, std::string value) { headers_[std::move(field)] = std::move(value); }
  void set_body(std::string body) { body_ = std::move(body); }
  [[nodiscard]] const std::string& body() const noexcept { return body_; }

  void append_headers_to_buffer(oklib::net::Buffer* output, bool chunked = false) const;
  void append_to_buffer(oklib::net::Buffer* output, bool include_body = true) const;

 private:
  [[nodiscard]] std::string status_message() const;

  int status_code_{static_cast<int>(HttpStatusCode::unknown)};
  std::string status_message_;
  bool close_connection_;
  std::map<std::string, std::string> headers_;
  std::string body_;
};

}  // namespace oklib::http
