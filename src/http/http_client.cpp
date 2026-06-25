#include "oklib/http/http_client.h"

#include <cctype>
#include <utility>

#include "oklib/net/buffer.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/tcp_connection.h"

namespace oklib::http {
namespace {

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

}  // namespace

HttpClientRequest::HttpClientRequest(std::string method, std::string target)
    : method_(std::move(method)),
      target_(std::move(target)) {}

std::string HttpClientRequest::serialize(std::string_view host) const {
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
  output.append(body_);
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
  client_.disconnect();
}

void HttpClient::stop() {
  stopped_ = true;
  connecting_ = false;
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
  if (!stopped_ && !pending_requests_.empty()) {
    request_connect();
  }
}

void HttpClient::on_message(const oklib::net::TcpConnectionPtr&,
                            oklib::net::Buffer* buffer,
                            oklib::Timestamp) {
  loop_->assert_in_loop_thread();
  for (;;) {
    const auto status = parser_.parse_response(buffer);
    if (status == HttpParseStatus::incomplete) {
      return;
    }
    if (status == HttpParseStatus::error) {
      handle_parse_error();
      return;
    }

    HttpResponseMessage response = std::move(parser_.mutable_response());
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

  while (!pending_requests_.empty()) {
    HttpClientRequest request = std::move(pending_requests_.front());
    pending_requests_.pop_front();
    connection_->send(request.serialize(host_));
    in_flight_requests_.push_back(std::move(request));
  }
}

void HttpClient::handle_parse_error() {
  parser_.reset();
  pending_requests_.clear();
  in_flight_requests_.clear();
  if (error_callback_) {
    error_callback_();
  }
  client_.disconnect();
}

bool HttpClient::response_closes_connection(const HttpResponseMessage& response) const {
  const std::string connection = response.headers.get("Connection");
  return equals_ignore_case(connection, "close") ||
         (response.version == HttpVersion::http10 && !equals_ignore_case(connection, "Keep-Alive"));
}

}  // namespace oklib::http
