#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "oklib/base/noncopyable.h"
#include "oklib/http/http_request_body_stream.h"
#include "oklib/http/http_response_writer.h"
#include "oklib/http/tls_options.h"
#include "oklib/net/tcp_server.h"

namespace oklib::http {

class HttpRequest;
class HttpResponse;

class HttpServer : private oklib::Noncopyable {
 public:
  using HttpCallback = std::function<void(const HttpRequest&, HttpResponse*)>;
  using AsyncHttpCallback = std::function<void(HttpRequest, HttpResponseWriter)>;
  using StreamingHttpCallback =
      std::function<void(HttpRequest, HttpRequestBodyStream, HttpResponseWriter)>;

  HttpServer(oklib::net::EventLoop* loop, const oklib::net::InetAddress& listen_address,
             std::string name,
             oklib::net::TcpServer::Option option = oklib::net::TcpServer::Option::no_reuse_port);

  void set_http_callback(HttpCallback callback) { http_callback_ = std::move(callback); }
  void set_async_http_callback(AsyncHttpCallback callback) {
    async_http_callback_ = std::move(callback);
  }
  void set_streaming_http_callback(StreamingHttpCallback callback) {
    streaming_http_callback_ = std::move(callback);
  }
  void set_tls_options(TlsServerOptions options) { tls_options_ = std::move(options); }
  void set_allowed_methods(std::vector<std::string> methods) {
    allowed_methods_ = std::move(methods);
  }
  void set_high_water_mark_callback(oklib::net::HighWaterMarkCallback callback, std::size_t mark) {
    server_.set_high_water_mark_callback(std::move(callback), mark);
  }
  void set_thread_num(int num_threads) { server_.set_thread_num(num_threads); }
  void start();

  [[nodiscard]] oklib::net::InetAddress listen_address() const { return server_.listen_address(); }
  [[nodiscard]] uint16_t port() const { return server_.port(); }

 private:
  void on_connection(const oklib::net::TcpConnectionPtr& connection);
  void on_message(const oklib::net::TcpConnectionPtr& connection, oklib::net::Buffer* buffer,
                  oklib::Timestamp receive_time);
  void on_plain_message(const oklib::net::TcpConnectionPtr& connection,
                        oklib::net::Buffer* buffer,
                        oklib::Timestamp receive_time,
                        HttpContext* context);
  bool on_request(const oklib::net::TcpConnectionPtr& connection, const HttpRequest& request);
  bool on_options_star(const oklib::net::TcpConnectionPtr& connection, const HttpRequest& request);
  bool on_async_request(const oklib::net::TcpConnectionPtr& connection,
                        HttpContext* context,
                        HttpRequest request);
  bool on_streaming_request(const oklib::net::TcpConnectionPtr& connection,
                            HttpContext* context,
                            HttpRequest request);
  [[nodiscard]] bool should_close(const HttpRequest& request) const;

  oklib::net::TcpServer server_;
  HttpCallback http_callback_;
  AsyncHttpCallback async_http_callback_;
  StreamingHttpCallback streaming_http_callback_;
  TlsServerOptions tls_options_;
  std::vector<std::string> allowed_methods_{"GET", "HEAD", "POST", "PUT", "DELETE",
                                            "OPTIONS", "TRACE", "CONNECT", "PATCH"};
};

}  // namespace oklib::http
