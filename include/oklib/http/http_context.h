#pragma once

#include <cstdint>
#include <string>

#include "oklib/http/http_request.h"

namespace oklib::net {
class Buffer;
}

namespace oklib::http {

class HttpContext {
 public:
  bool parse_request(oklib::net::Buffer* buffer, oklib::Timestamp receive_time);
  [[nodiscard]] bool got_all() const noexcept { return state_ == State::got_all; }
  [[nodiscard]] const HttpRequest& request() const noexcept { return request_; }
  [[nodiscard]] HttpRequest& mutable_request() noexcept { return request_; }
  void set_peer_address(std::string ip, uint16_t port);
  void reset();

 private:
  enum class State {
    expect_request_line,
    expect_headers,
    got_all,
  };

  bool process_request_line(const char* begin, const char* end);

  State state_{State::expect_request_line};
  HttpRequest request_;
  std::string peer_ip_;
  uint16_t peer_port_{0};
};

}  // namespace oklib::http
