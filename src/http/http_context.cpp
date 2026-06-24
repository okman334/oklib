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

HttpResponseWriter HttpContext::make_response_writer(const oklib::net::TcpConnectionPtr& connection,
                                                     bool close_connection,
                                                     bool include_body) {
  if (!response_writer_state_) {
    response_writer_state_ = HttpResponseWriter::make_state(connection);
  }
  return HttpResponseWriter(response_writer_state_, next_response_sequence_++, close_connection,
                            include_body);
}

void HttpContext::reset() {
  parser_.reset();
}

}  // namespace oklib::http
