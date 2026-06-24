#include "oklib/http/http_server.h"

#include <any>

#include "oklib/http/http_context.h"
#include "oklib/http/http_request.h"
#include "oklib/http/http_response.h"
#include "oklib/net/buffer.h"
#include "oklib/net/tcp_connection.h"

namespace oklib::http {
namespace {

void default_http_callback(const HttpRequest&, HttpResponse* response) {
  response->set_status_code(HttpStatusCode::not_found);
  response->set_body("404 Not Found");
  response->set_close_connection(true);
}

}  // namespace

HttpServer::HttpServer(oklib::net::EventLoop* loop, const oklib::net::InetAddress& listen_address,
                       std::string name, oklib::net::TcpServer::Option option)
    : server_(loop, listen_address, std::move(name), option),
      http_callback_(default_http_callback) {
  server_.set_connection_callback([this](const oklib::net::TcpConnectionPtr& conn) { on_connection(conn); });
  server_.set_message_callback([this](const oklib::net::TcpConnectionPtr& conn,
                                      oklib::net::Buffer* buffer,
                                      oklib::Timestamp receive_time) {
    on_message(conn, buffer, receive_time);
  });
}

void HttpServer::start() {
  server_.start();
}

void HttpServer::on_connection(const oklib::net::TcpConnectionPtr& connection) {
  if (connection->connected()) {
    connection->set_context(HttpContext());
  }
}

void HttpServer::on_message(const oklib::net::TcpConnectionPtr& connection,
                            oklib::net::Buffer* buffer,
                            oklib::Timestamp receive_time) {
  auto* context = std::any_cast<HttpContext>(&connection->mutable_context());
  if (context == nullptr || !context->parse_request(buffer, receive_time)) {
    connection->send("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    connection->shutdown();
    return;
  }

  if (context->got_all()) {
    on_request(connection, context->request());
    context->reset();
  }
}

void HttpServer::on_request(const oklib::net::TcpConnectionPtr& connection, const HttpRequest& request) {
  HttpRequest request_with_connection = request;
  request_with_connection.set_peer_address(connection->peer_address().to_ip(), connection->peer_address().port());

  const std::string connection_header = request_with_connection.header("Connection");
  const bool close = connection_header == "close" ||
                     (request_with_connection.version() == HttpVersion::http10 && connection_header != "Keep-Alive");

  HttpResponse response(close);
  http_callback_(request_with_connection, &response);
  oklib::net::Buffer output;
  response.append_to_buffer(&output, request_with_connection.method() != HttpMethod::head);
  connection->send(&output);
  if (response.close_connection()) {
    connection->shutdown();
  }
}

}  // namespace oklib::http
