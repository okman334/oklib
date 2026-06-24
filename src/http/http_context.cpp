#include "oklib/http/http_context.h"

#include <algorithm>
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

HttpParseStatus HttpContext::parse_request_head(oklib::net::Buffer* buffer,
                                                oklib::Timestamp receive_time) {
  const auto status = parser_.parse_request_head(buffer, receive_time);
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

void HttpContext::start_streaming_body(HttpRequestBodyStream body_stream, std::uint64_t remaining_bytes) {
  body_stream_ = std::move(body_stream);
  streaming_body_remaining_ = remaining_bytes;
  streaming_body_active_ = true;
}

HttpParseStatus HttpContext::process_streaming_body(oklib::net::Buffer* buffer) {
  while (streaming_body_remaining_ > 0 && buffer->readable_bytes() > 0) {
    const auto chunk_size =
        std::min<std::uint64_t>(streaming_body_remaining_, buffer->readable_bytes());
    body_stream_.on_data(std::string_view(buffer->peek(), static_cast<std::size_t>(chunk_size)));
    buffer->retrieve(static_cast<std::size_t>(chunk_size));
    streaming_body_remaining_ -= chunk_size;
  }

  if (streaming_body_remaining_ > 0) {
    return HttpParseStatus::incomplete;
  }

  streaming_body_active_ = false;
  body_stream_.on_complete();
  return HttpParseStatus::complete;
}

void HttpContext::reset() {
  parser_.reset();
  streaming_body_remaining_ = 0;
  streaming_body_active_ = false;
  body_stream_ = HttpRequestBodyStream();
}

}  // namespace oklib::http
