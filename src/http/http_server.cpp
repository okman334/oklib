#include "oklib/http/http_server.h"

#include <any>
#include <cctype>
#include <utility>

#include "oklib/http/http_context.h"
#include "oklib/http/http_request.h"
#include "oklib/http/http_response.h"
#include "oklib/net/buffer.h"
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
    HttpContext context;
    context.set_peer_address(connection->peer_address().to_ip(), connection->peer_address().port());
    connection->set_context(std::move(context));
  }
}

void HttpServer::on_message(const oklib::net::TcpConnectionPtr& connection,
                            oklib::net::Buffer* buffer,
                            oklib::Timestamp receive_time) {
  auto* context = std::any_cast<HttpContext>(&connection->mutable_context());
  if (context == nullptr) {
    connection->send("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    connection->shutdown();
    return;
  }

  while (connection->connected()) {
    const auto status = context->parse_request(buffer, receive_time);
    if (status == HttpParseStatus::error) {
      connection->send("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
      connection->shutdown();
      return;
    }
    if (status == HttpParseStatus::incomplete) {
      return;
    }

    HttpRequest request = context->take_request();
    const bool close = async_http_callback_ ? on_async_request(connection, context, std::move(request))
                                            : on_request(connection, request);
    context->reset();
    if (close || buffer->readable_bytes() == 0) {
      return;
    }
  }
}

bool HttpServer::on_request(const oklib::net::TcpConnectionPtr& connection, const HttpRequest& request) {
  const bool close = should_close(request);

  HttpResponse response(close);
  http_callback_(request, &response);
  oklib::net::Buffer output;
  response.append_to_buffer(&output, request.method() != HttpMethod::head);
  connection->send(&output);
  if (response.close_connection()) {
    connection->shutdown();
  }
  return response.close_connection();
}

bool HttpServer::on_async_request(const oklib::net::TcpConnectionPtr& connection,
                                  HttpContext* context,
                                  HttpRequest request) {
  const bool close = should_close(request);
  const bool include_body = request.method() != HttpMethod::head;
  auto writer = context->make_response_writer(connection, close, include_body);
  async_http_callback_(std::move(request), std::move(writer));
  return close;
}

bool HttpServer::should_close(const HttpRequest& request) const {
  const std::string connection_header = request.header("Connection");
  return equals_ignore_case(connection_header, "close") ||
         (request.version() == HttpVersion::http10 &&
          !equals_ignore_case(connection_header, "Keep-Alive"));
}

}  // namespace oklib::http
