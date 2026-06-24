#include "oklib/http/http_context.h"

#include <utility>

#include "oklib/net/buffer.h"

namespace oklib::http {

HttpParseStatus HttpContext::parse_request(oklib::net::Buffer* buffer, oklib::Timestamp receive_time) {
  const auto status = parser_.parse_request(buffer, receive_time);
  if (status == HttpParseStatus::complete) {
    parser_.mutable_request().set_peer_address(peer_ip_, peer_port_);
  }
  return status;
}

void HttpContext::set_peer_address(std::string ip, uint16_t port) {
  peer_ip_ = std::move(ip);
  peer_port_ = port;
}

void HttpContext::reset() {
  parser_.reset();
}

}  // namespace oklib::http
