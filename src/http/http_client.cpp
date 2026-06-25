#include "oklib/http/http_client.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <mutex>
#include <string_view>
#include <utility>

#include "oklib/http/http_semantics.h"
#include "oklib/net/buffer.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/tcp_connection.h"

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

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
        std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

bool header_has_token(std::string_view value, std::string_view token) {
  while (!value.empty()) {
    const auto comma = value.find(',');
    const auto part = trim_ows(comma == std::string_view::npos ? value : value.substr(0, comma));
    if (equals_ignore_case(part, token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      return false;
    }
    value.remove_prefix(comma + 1);
  }
  return false;
}

bool expects_continue(const HttpClientRequest& request) {
  return !request.body().empty() && equals_ignore_case(request.headers().get("Expect"), "100-continue");
}

bool expects_no_response_body(const HttpClientRequest& request) {
  return equals_ignore_case(request.method(), "HEAD");
}

}  // namespace

struct HttpClientResponseStream::State {
  std::mutex mutex;
  DataCallback data_callback;
  CompleteCallback complete_callback;
  std::atomic_bool completed{false};
};

HttpClientResponseStream::HttpClientResponseStream()
    : state_(std::make_shared<State>()) {}

void HttpClientResponseStream::set_data_callback(DataCallback callback) const {
  if (!state_) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  state_->data_callback = std::move(callback);
}

void HttpClientResponseStream::set_complete_callback(CompleteCallback callback) const {
  if (!state_) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  state_->complete_callback = std::move(callback);
}

void HttpClientResponseStream::on_data(std::string_view chunk) const {
  if (!state_ || chunk.empty()) {
    return;
  }

  DataCallback callback;
  {
    std::lock_guard lock(state_->mutex);
    callback = state_->data_callback;
  }
  if (callback) {
    callback(chunk);
  }
}

void HttpClientResponseStream::on_complete() const {
  if (!state_) {
    return;
  }

  bool expected = false;
  if (!state_->completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }

  CompleteCallback callback;
  {
    std::lock_guard lock(state_->mutex);
    callback = state_->complete_callback;
  }
  if (callback) {
    callback();
  }
}

HttpClientRequest::HttpClientRequest(std::string method, std::string target)
    : method_(std::move(method)),
      target_(std::move(target)) {}

std::string HttpClientRequest::serialize(std::string_view host, bool include_body) const {
  std::string output;
  output.reserve(method_.size() + target_.size() + body_.size() + 128);
  output.append(method_);
  output.push_back(' ');
  output.append(target_);
  output.append(" HTTP/1.1\r\n");

  if (!headers_.contains("Host")) {
    output.append("Host: ");
    output.append(host);
    output.append("\r\n");
  }
  if (!headers_.contains("Connection")) {
    output.append("Connection: Keep-Alive\r\n");
  }
  if (!body_.empty() && !headers_.contains("Content-Length") &&
      !headers_.contains("Transfer-Encoding")) {
    output.append("Content-Length: ");
    output.append(std::to_string(body_.size()));
    output.append("\r\n");
  }

  for (const auto& [field, value] : headers_.entries()) {
    output.append(field);
    output.append(": ");
    output.append(value);
    output.append("\r\n");
  }
  output.append("\r\n");
  if (include_body) {
    output.append(body_);
  }
  return output;
}

HttpClient::HttpClient(oklib::net::EventLoop* loop,
                       const oklib::net::InetAddress& server_address,
                       std::string host,
                       std::string name,
                       HttpClientOptions options)
    : loop_(loop),
      host_(std::move(host)),
      client_(loop, server_address, std::move(name)) {
  if (options.retry) {
    client_.enable_retry();
  }
  client_.set_connection_callback([this](const oklib::net::TcpConnectionPtr& connection) {
    on_connection(connection);
  });
  client_.set_message_callback([this](const oklib::net::TcpConnectionPtr& connection,
                                      oklib::net::Buffer* buffer,
                                      oklib::Timestamp receive_time) {
    on_message(connection, buffer, receive_time);
  });
}

HttpClient::~HttpClient() {
  stop();
}

void HttpClient::connect() {
  loop_->run_in_loop([this] {
    stopped_ = false;
    request_connect();
  });
}

void HttpClient::disconnect() {
  stopped_ = true;
  connecting_ = false;
  awaiting_continue_ = false;
  client_.disconnect();
}

void HttpClient::stop() {
  stopped_ = true;
  connecting_ = false;
  awaiting_continue_ = false;
  client_.stop();
}

void HttpClient::send(HttpClientRequest request) {
  loop_->run_in_loop([this, request = std::move(request)]() mutable {
    send_in_loop(std::move(request));
  });
}

void HttpClient::on_connection(const oklib::net::TcpConnectionPtr& connection) {
  loop_->assert_in_loop_thread();
  if (connection->connected()) {
    connecting_ = false;
    connection_ = connection;
    parser_.reset();
    flush_requests();
    return;
  }

  if (connection_ == connection) {
    connection_.reset();
  }
  connecting_ = false;
  parser_.reset();
  in_flight_requests_.clear();
  streaming_response_active_ = false;
  streaming_response_close_ = false;
  streaming_body_mode_ = StreamingBodyMode::none;
  awaiting_continue_ = false;
  if (!stopped_ && !pending_requests_.empty()) {
    request_connect();
  }
}

void HttpClient::on_message(const oklib::net::TcpConnectionPtr&,
                            oklib::net::Buffer* buffer,
                            oklib::Timestamp) {
  loop_->assert_in_loop_thread();
  for (;;) {
    if (streaming_response_active_) {
      const auto body_status = process_streaming_body(buffer);
      if (body_status == HttpParseStatus::error) {
        handle_parse_error();
        return;
      }
      if (body_status == HttpParseStatus::incomplete) {
        return;
      }
      if (!connection_ || !connection_->connected() || buffer->readable_bytes() == 0) {
        return;
      }
      continue;
    }

    const bool request_expects_no_body =
        !in_flight_requests_.empty() && expects_no_response_body(in_flight_requests_.front());
    const auto status = streaming_response_callback_ || request_expects_no_body
                            ? parser_.parse_response_head(buffer)
                            : parser_.parse_response(buffer);
    if (status == HttpParseStatus::incomplete) {
      return;
    }
    if (status == HttpParseStatus::error) {
      handle_parse_error();
      return;
    }

    HttpResponseMessage response = std::move(parser_.mutable_response());
    if (response.status_code == 100) {
      parser_.reset();
      handle_continue_response();
      if (buffer->readable_bytes() == 0) {
        return;
      }
      continue;
    }

    awaiting_continue_ = false;

    if (streaming_response_callback_) {
      streaming_response_close_ = response_closes_connection(response);
      const auto content_length = response.content_length.value_or(0);
      const bool response_has_body =
          !request_expects_no_body && status_code_allows_body(response.status_code);
      response.body.clear();
      parser_.reset();

      response_stream_ = HttpClientResponseStream();
      if (response_has_body && response.chunked) {
        streaming_body_mode_ = StreamingBodyMode::chunked;
        streaming_chunk_state_ = StreamingChunkState::size;
        streaming_body_remaining_ = 0;
        streaming_decoded_body_bytes_ = 0;
        streaming_trailer_bytes_ = 0;
        streaming_response_active_ = true;
      } else if (response_has_body && content_length > 0) {
        streaming_body_mode_ = StreamingBodyMode::fixed_length;
        streaming_body_remaining_ = content_length;
        streaming_response_active_ = true;
      }

      streaming_response_callback_(std::move(response), response_stream_);

      if (!streaming_response_active_) {
        finish_streaming_response();
      } else {
        const auto body_status = process_streaming_body(buffer);
        if (body_status == HttpParseStatus::error) {
          handle_parse_error();
          return;
        }
        if (body_status == HttpParseStatus::incomplete) {
          return;
        }
      }
      if (!connection_ || !connection_->connected() || buffer->readable_bytes() == 0) {
        return;
      }
      continue;
    }

    const bool close_after_response = response_closes_connection(response);
    parser_.reset();
    if (!in_flight_requests_.empty()) {
      in_flight_requests_.pop_front();
    }
    if (response_callback_) {
      response_callback_(std::move(response));
    }
    if (close_after_response) {
      client_.disconnect();
      return;
    }
    flush_requests();
    if (buffer->readable_bytes() == 0) {
      return;
    }
  }
}

HttpParseStatus HttpClient::process_streaming_body(oklib::net::Buffer* buffer) {
  if (streaming_body_mode_ == StreamingBodyMode::chunked) {
    return process_chunked_streaming_body(buffer);
  }
  return process_fixed_streaming_body(buffer);
}

HttpParseStatus HttpClient::process_fixed_streaming_body(oklib::net::Buffer* buffer) {
  while (streaming_body_remaining_ > 0 && buffer->readable_bytes() > 0) {
    const auto chunk_size =
        std::min<std::uint64_t>(streaming_body_remaining_, buffer->readable_bytes());
    response_stream_.on_data(std::string_view(buffer->peek(), static_cast<std::size_t>(chunk_size)));
    buffer->retrieve(static_cast<std::size_t>(chunk_size));
    streaming_body_remaining_ -= chunk_size;
  }

  if (streaming_body_remaining_ > 0) {
    return HttpParseStatus::incomplete;
  }

  finish_streaming_response();
  return HttpParseStatus::complete;
}

HttpParseStatus HttpClient::process_chunked_streaming_body(oklib::net::Buffer* buffer) {
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
        streaming_chunk_state_ = size == 0 ? StreamingChunkState::trailers : StreamingChunkState::data;
        break;
      }
      case StreamingChunkState::data: {
        if (buffer->readable_bytes() == 0) {
          return HttpParseStatus::incomplete;
        }
        const auto chunk_size =
            std::min<std::uint64_t>(streaming_body_remaining_, buffer->readable_bytes());
        response_stream_.on_data(std::string_view(buffer->peek(), static_cast<std::size_t>(chunk_size)));
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
          finish_streaming_response();
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

void HttpClient::finish_streaming_response() {
  const bool close_after_response = streaming_response_close_;
  streaming_response_active_ = false;
  streaming_response_close_ = false;
  streaming_body_mode_ = StreamingBodyMode::none;
  streaming_chunk_state_ = StreamingChunkState::size;
  streaming_body_remaining_ = 0;
  streaming_decoded_body_bytes_ = 0;
  streaming_trailer_bytes_ = 0;
  if (!in_flight_requests_.empty()) {
    in_flight_requests_.pop_front();
  }

  response_stream_.on_complete();
  response_stream_ = HttpClientResponseStream();

  if (close_after_response) {
    client_.disconnect();
  } else if (!stopped_) {
    flush_requests();
  }
}

void HttpClient::send_in_loop(HttpClientRequest request) {
  loop_->assert_in_loop_thread();
  stopped_ = false;
  pending_requests_.push_back(std::move(request));
  if (!connection_ || !connection_->connected()) {
    request_connect();
    return;
  }
  flush_requests();
}

void HttpClient::request_connect() {
  loop_->assert_in_loop_thread();
  if (stopped_ || connecting_ || (connection_ && connection_->connected())) {
    return;
  }
  connecting_ = true;
  client_.connect();
}

void HttpClient::flush_requests() {
  loop_->assert_in_loop_thread();
  if (!connection_ || !connection_->connected()) {
    return;
  }
  if (awaiting_continue_) {
    return;
  }

  while (!pending_requests_.empty()) {
    HttpClientRequest request = std::move(pending_requests_.front());
    pending_requests_.pop_front();
    const bool wait_for_continue = expects_continue(request);
    connection_->send(request.serialize(host_, !wait_for_continue));
    in_flight_requests_.push_back(std::move(request));
    if (wait_for_continue) {
      awaiting_continue_ = true;
      break;
    }
  }
}

void HttpClient::handle_continue_response() {
  loop_->assert_in_loop_thread();
  if (!awaiting_continue_ || !connection_ || !connection_->connected() || in_flight_requests_.empty()) {
    return;
  }
  connection_->send(in_flight_requests_.front().body());
  awaiting_continue_ = false;
}

void HttpClient::handle_parse_error() {
  parser_.reset();
  pending_requests_.clear();
  in_flight_requests_.clear();
  awaiting_continue_ = false;
  streaming_response_active_ = false;
  streaming_response_close_ = false;
  streaming_body_mode_ = StreamingBodyMode::none;
  streaming_chunk_state_ = StreamingChunkState::size;
  streaming_body_remaining_ = 0;
  streaming_decoded_body_bytes_ = 0;
  streaming_trailer_bytes_ = 0;
  if (error_callback_) {
    error_callback_();
  }
  client_.disconnect();
}

bool HttpClient::response_closes_connection(const HttpResponseMessage& response) const {
  const std::string connection = response.headers.get("Connection");
  return header_has_token(connection, "close") ||
         (response.version == HttpVersion::http10 && !header_has_token(connection, "Keep-Alive"));
}

}  // namespace oklib::http
