#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "oklib/http/http_parser.h"
#include "oklib/http/http_response_writer.h"
#include "oklib/net/callbacks.h"

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
  [[nodiscard]] HttpRequest take_request() { return parser_.take_request(); }
  [[nodiscard]] HttpResponseWriter make_response_writer(const oklib::net::TcpConnectionPtr& connection,
                                                        bool close_connection,
                                                        bool include_body);
  void set_peer_address(std::string ip, uint16_t port);
  void reset();

 private:
  HttpParser parser_;
  HttpResponseWriter::StatePtr response_writer_state_;
  std::size_t next_response_sequence_{0};
  std::string peer_ip_;
  uint16_t peer_port_{0};
};

}  // namespace oklib::http
