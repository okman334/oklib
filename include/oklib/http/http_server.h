#pragma once

#include <functional>
#include <string>

#include "oklib/base/noncopyable.h"
#include "oklib/net/tcp_server.h"

namespace oklib::http {

class HttpRequest;
class HttpResponse;

class HttpServer : private oklib::Noncopyable {
 public:
  using HttpCallback = std::function<void(const HttpRequest&, HttpResponse*)>;

  HttpServer(oklib::net::EventLoop* loop, const oklib::net::InetAddress& listen_address,
             std::string name,
             oklib::net::TcpServer::Option option = oklib::net::TcpServer::Option::no_reuse_port);

  void set_http_callback(HttpCallback callback) { http_callback_ = std::move(callback); }
  void set_thread_num(int num_threads) { server_.set_thread_num(num_threads); }
  void start();

  [[nodiscard]] oklib::net::InetAddress listen_address() const { return server_.listen_address(); }
  [[nodiscard]] uint16_t port() const { return server_.port(); }

 private:
  void on_connection(const oklib::net::TcpConnectionPtr& connection);
  void on_message(const oklib::net::TcpConnectionPtr& connection, oklib::net::Buffer* buffer,
                  oklib::Timestamp receive_time);
  void on_request(const oklib::net::TcpConnectionPtr& connection, const HttpRequest& request);

  oklib::net::TcpServer server_;
  HttpCallback http_callback_;
};

}  // namespace oklib::http
