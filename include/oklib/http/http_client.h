#pragma once

#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "oklib/base/noncopyable.h"
#include "oklib/http/http_headers.h"
#include "oklib/http/http_parser.h"
#include "oklib/net/tcp_client.h"

namespace oklib::http {

struct HttpClientOptions {
  bool retry{false};
};

class HttpClientRequest {
 public:
  HttpClientRequest(std::string method = "GET", std::string target = "/");

  [[nodiscard]] const std::string& method() const noexcept { return method_; }
  [[nodiscard]] const std::string& target() const noexcept { return target_; }
  [[nodiscard]] const std::string& body() const noexcept { return body_; }
  [[nodiscard]] const HttpHeaders& headers() const noexcept { return headers_; }

  void set_method(std::string method) { method_ = std::move(method); }
  void set_target(std::string target) { target_ = std::move(target); }
  void set_body(std::string body) { body_ = std::move(body); }
  void add_header(std::string_view field, std::string_view value) { headers_.add(field, value); }
  void set_header(std::string_view field, std::string_view value) { headers_.set(field, value); }

  [[nodiscard]] std::string serialize(std::string_view host) const;

 private:
  std::string method_;
  std::string target_;
  HttpHeaders headers_;
  std::string body_;
};

class HttpClient : private oklib::Noncopyable {
 public:
  using ResponseCallback = std::function<void(HttpResponseMessage)>;
  using ErrorCallback = std::function<void()>;

  HttpClient(oklib::net::EventLoop* loop,
             const oklib::net::InetAddress& server_address,
             std::string host,
             std::string name,
             HttpClientOptions options = {});
  ~HttpClient();

  void connect();
  void disconnect();
  void stop();
  void send(HttpClientRequest request);

  void set_response_callback(ResponseCallback callback) { response_callback_ = std::move(callback); }
  void set_error_callback(ErrorCallback callback) { error_callback_ = std::move(callback); }

 private:
  void on_connection(const oklib::net::TcpConnectionPtr& connection);
  void on_message(const oklib::net::TcpConnectionPtr& connection,
                  oklib::net::Buffer* buffer,
                  oklib::Timestamp receive_time);
  void send_in_loop(HttpClientRequest request);
  void request_connect();
  void flush_requests();
  void handle_parse_error();
  [[nodiscard]] bool response_closes_connection(const HttpResponseMessage& response) const;

  oklib::net::EventLoop* loop_;
  std::string host_;
  oklib::net::TcpClient client_;
  ResponseCallback response_callback_;
  ErrorCallback error_callback_;
  HttpParser parser_{HttpParserMode::response};
  oklib::net::TcpConnectionPtr connection_;
  std::deque<HttpClientRequest> pending_requests_;
  std::deque<HttpClientRequest> in_flight_requests_;
  bool connecting_{false};
  bool stopped_{false};
};

}  // namespace oklib::http
