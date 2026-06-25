#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "oklib/base/noncopyable.h"
#include "oklib/http/http_request_body_stream.h"
#include "oklib/http/http_response_writer.h"
#include "oklib/http/http_server.h"
#include "oklib/http/tls_options.h"
#include "oklib/net/tcp_server.h"
#include "oklib/websocket/websocket_channel.h"

namespace oklib::http {
class HttpContext;
class HttpRequest;
class HttpResponse;
class HttpRouter;
}

namespace oklib::websocket {

struct WebSocketService {
  using OpenCallback =
      std::function<void(const WebSocketChannelPtr&, const oklib::http::HttpRequest&)>;
  using MessageCallback =
      std::function<void(const WebSocketChannelPtr&, std::string_view, WebSocketOpcode)>;
  using CloseCallback = std::function<void(const WebSocketChannelPtr&, WebSocketCloseInfo)>;
  using ErrorCallback = std::function<void(const WebSocketChannelPtr&, WebSocketError)>;
  using SubprotocolSelector =
      std::function<std::optional<std::string>(const oklib::http::HttpRequest&,
                                               const std::vector<std::string>&)>;

  OpenCallback on_open;
  MessageCallback on_message;
  CloseCallback on_close;
  ErrorCallback on_error;
  SubprotocolSelector select_subprotocol;
  WebSocketOptions options;
  std::chrono::milliseconds ping_interval{0};
};

class WebSocketServer : private oklib::Noncopyable {
 public:
  using HttpCallback = oklib::http::HttpServer::HttpCallback;
  using AsyncHttpCallback = oklib::http::HttpServer::AsyncHttpCallback;
  using StreamingHttpCallback = oklib::http::HttpServer::StreamingHttpCallback;

  WebSocketServer(oklib::net::EventLoop* loop,
                  const oklib::net::InetAddress& listen_address,
                  std::string name,
                  oklib::net::TcpServer::Option option =
                      oklib::net::TcpServer::Option::no_reuse_port);

  void set_http_callback(HttpCallback callback) { http_callback_ = std::move(callback); }
  void set_async_http_callback(AsyncHttpCallback callback) {
    async_http_callback_ = std::move(callback);
  }
  void set_streaming_http_callback(StreamingHttpCallback callback) {
    streaming_http_callback_ = std::move(callback);
  }
  void set_router(const oklib::http::HttpRouter& router);
  void set_websocket_service(WebSocketService service) {
    websocket_service_ = std::move(service);
  }
  void register_websocket_service(WebSocketService service) {
    set_websocket_service(std::move(service));
  }
  void set_tls_options(oklib::http::TlsServerOptions options) {
    tls_options_ = std::move(options);
  }
  void set_high_water_mark_callback(oklib::net::HighWaterMarkCallback callback,
                                    std::size_t mark) {
    server_.set_high_water_mark_callback(std::move(callback), mark);
  }
  void set_thread_num(int num_threads) { server_.set_thread_num(num_threads); }
  void start();

  [[nodiscard]] oklib::net::InetAddress listen_address() const { return server_.listen_address(); }
  [[nodiscard]] uint16_t port() const { return server_.port(); }

 private:
  void on_connection(const oklib::net::TcpConnectionPtr& connection);
  void on_message(const oklib::net::TcpConnectionPtr& connection,
                  oklib::net::Buffer* buffer,
                  oklib::Timestamp receive_time);
  void on_plain_message(const oklib::net::TcpConnectionPtr& connection,
                        oklib::net::Buffer* buffer,
                        oklib::Timestamp receive_time);
  bool on_http_request(const oklib::net::TcpConnectionPtr& connection,
                       const oklib::http::HttpRequest& request);
  bool on_async_http_request(const oklib::net::TcpConnectionPtr& connection,
                             oklib::http::HttpContext* context,
                             oklib::http::HttpRequest request);
  bool on_streaming_http_request(const oklib::net::TcpConnectionPtr& connection,
                                 oklib::http::HttpContext* context,
                                 oklib::http::HttpRequest request);
  bool try_websocket_upgrade(const oklib::net::TcpConnectionPtr& connection,
                             oklib::http::HttpContext* context,
                             const oklib::http::HttpRequest& request,
                             oklib::net::Buffer* leftover);
  void on_websocket_message(const oklib::net::TcpConnectionPtr& connection,
                            oklib::net::Buffer* buffer);
  [[nodiscard]] bool should_close(const oklib::http::HttpRequest& request) const;

  oklib::net::TcpServer server_;
  HttpCallback http_callback_;
  AsyncHttpCallback async_http_callback_;
  StreamingHttpCallback streaming_http_callback_;
  oklib::http::TlsServerOptions tls_options_;
  WebSocketService websocket_service_;
};

}  // namespace oklib::websocket
