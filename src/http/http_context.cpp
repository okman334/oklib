#include "oklib/http/http_context.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>
#include <utility>

#include "oklib/net/buffer.h"

namespace oklib::http {
namespace {

constexpr std::size_t k_max_streaming_trailers = 64 * 1024;
constexpr std::uint64_t k_max_streaming_body = 8 * 1024 * 1024;

bool is_ows(char c) noexcept {
  return c == ' ' || c == '\t';
}

std::string_view trim_ows(std::string_view value) {
  while (!value.empty() && is_ows(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_ows(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

bool parse_hex_u64(std::string_view value, std::uint64_t* output) {
  value = trim_ows(value);
  if (value.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed, 16);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  *output = parsed;
  return true;
}

bool valid_trailer_line(std::string_view line) {
  if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
    return false;
  }
  const auto colon = line.find(':');
  return colon != std::string_view::npos && colon != 0;
}

}  // namespace

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
  streaming_body_mode_ = StreamingBodyMode::fixed_length;
  streaming_body_remaining_ = remaining_bytes;
  streaming_body_active_ = true;
}

void HttpContext::start_streaming_chunked_body(HttpRequestBodyStream body_stream) {
  body_stream_ = std::move(body_stream);
  streaming_body_mode_ = StreamingBodyMode::chunked;
  streaming_chunk_state_ = StreamingChunkState::size;
  streaming_body_remaining_ = 0;
  streaming_decoded_body_bytes_ = 0;
  streaming_trailer_bytes_ = 0;
  streaming_body_active_ = true;
}

HttpParseStatus HttpContext::process_streaming_body(oklib::net::Buffer* buffer) {
  if (streaming_body_mode_ == StreamingBodyMode::chunked) {
    return process_chunked_streaming_body(buffer);
  }
  return process_fixed_streaming_body(buffer);
}

HttpParseStatus HttpContext::process_fixed_streaming_body(oklib::net::Buffer* buffer) {
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

HttpParseStatus HttpContext::process_chunked_streaming_body(oklib::net::Buffer* buffer) {
  for (;;) {
    switch (streaming_chunk_state_) {
      case StreamingChunkState::size: {
        const char* crlf = buffer->find_crlf();
        if (crlf == nullptr) {
          return HttpParseStatus::incomplete;
        }

        std::string_view line(buffer->peek(), static_cast<std::size_t>(crlf - buffer->peek()));
        const auto semicolon = line.find(';');
        const auto size_part = semicolon == std::string_view::npos ? line : line.substr(0, semicolon);
        std::uint64_t size = 0;
        if (!parse_hex_u64(size_part, &size)) {
          return HttpParseStatus::error;
        }
        if (size > k_max_streaming_body ||
            streaming_decoded_body_bytes_ > k_max_streaming_body - size) {
          return HttpParseStatus::error;
        }

        buffer->retrieve_until(crlf + 2);
        streaming_body_remaining_ = size;
        if (size == 0) {
          streaming_chunk_state_ = StreamingChunkState::trailers;
        } else {
          streaming_chunk_state_ = StreamingChunkState::data;
        }
        break;
      }
      case StreamingChunkState::data: {
        if (buffer->readable_bytes() == 0) {
          return HttpParseStatus::incomplete;
        }
        const auto chunk_size =
            std::min<std::uint64_t>(streaming_body_remaining_, buffer->readable_bytes());
        body_stream_.on_data(std::string_view(buffer->peek(), static_cast<std::size_t>(chunk_size)));
        buffer->retrieve(static_cast<std::size_t>(chunk_size));
        streaming_body_remaining_ -= chunk_size;
        streaming_decoded_body_bytes_ += chunk_size;
        if (streaming_body_remaining_ == 0) {
          streaming_chunk_state_ = StreamingChunkState::data_crlf;
        }
        break;
      }
      case StreamingChunkState::data_crlf: {
        if (buffer->readable_bytes() < 2) {
          return HttpParseStatus::incomplete;
        }
        if (buffer->peek()[0] != '\r' || buffer->peek()[1] != '\n') {
          return HttpParseStatus::error;
        }
        buffer->retrieve(2);
        streaming_chunk_state_ = StreamingChunkState::size;
        break;
      }
      case StreamingChunkState::trailers: {
        const char* crlf = buffer->find_crlf();
        if (crlf == nullptr) {
          return HttpParseStatus::incomplete;
        }

        const auto line_size = static_cast<std::size_t>(crlf - buffer->peek());
        streaming_trailer_bytes_ += line_size + 2;
        if (streaming_trailer_bytes_ > k_max_streaming_trailers) {
          return HttpParseStatus::error;
        }

        if (line_size == 0) {
          buffer->retrieve_until(crlf + 2);
          streaming_body_active_ = false;
          streaming_body_mode_ = StreamingBodyMode::none;
          body_stream_.on_complete();
          return HttpParseStatus::complete;
        }

        if (!valid_trailer_line(std::string_view(buffer->peek(), line_size))) {
          return HttpParseStatus::error;
        }
        buffer->retrieve_until(crlf + 2);
        break;
      }
    }
  }
}

void HttpContext::reset() {
  parser_.reset();
  streaming_body_mode_ = StreamingBodyMode::none;
  streaming_chunk_state_ = StreamingChunkState::size;
  streaming_body_remaining_ = 0;
  streaming_decoded_body_bytes_ = 0;
  streaming_trailer_bytes_ = 0;
  streaming_body_active_ = false;
  body_stream_ = HttpRequestBodyStream();
}

}  // namespace oklib::http
