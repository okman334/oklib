#pragma once

#include <cstdint>
#include <string>

#include "oklib/http/http_parser.h"

namespace oklib::net {
class Buffer;
}

namespace oklib::http {

class HttpContext {
 public:
  HttpParseStatus parse_request(oklib::net::Buffer* buffer, oklib::Timestamp receive_time);
  [[nodiscard]] bool got_all() const noexcept { return parser_.complete(); }
  [[nodiscard]] HttpParseError error() const noexcept { return parser_.error(); }
  [[nodiscard]] const HttpRequest& request() const noexcept { return parser_.request(); }
  [[nodiscard]] HttpRequest& mutable_request() noexcept { return parser_.mutable_request(); }
  void set_peer_address(std::string ip, uint16_t port);
  void reset();

 private:
  HttpParser parser_;
  std::string peer_ip_;
  uint16_t peer_port_{0};
};

}  // namespace oklib::http
