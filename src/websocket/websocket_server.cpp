#include "oklib/websocket/websocket_server.h"

#include <algorithm>
#include <any>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "oklib/http/http_context.h"
#include "oklib/http/http_request.h"
#include "oklib/http/http_response.h"
#include "oklib/http/http_router.h"
#include "oklib/net/buffer.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/tcp_connection.h"
#include "oklib/net/timer_id.h"
#include "oklib/websocket/websocket_handshake.h"
#include "websocket_compression.h"

namespace oklib::websocket {
namespace {

#if OKLIB_ENABLE_WEBSOCKET_COMPRESSION
constexpr bool k_compression_available = true;
#else
constexpr bool k_compression_available = false;
#endif

struct ServerConnectionContext {
  enum class Protocol {
    http,
    websocket,
  };

  oklib::http::HttpContext http;
  Protocol protocol{Protocol::http};
  WebSocketOptions options;
  WebSocketFrameParser frame_parser{WebSocketEndpointRole::server, options};
  WebSocketMessageAssembler message_assembler{options};
  WebSocketChannelPtr channel;
  oklib::net::TimerId ping_timer;
  bool compression_enabled{false};
  bool close_notified{false};
};

std::string lower(std::string_view value) {
  std::string result(value);
  for (char& ch : result) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return result;
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

std::string_view trim_ows(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
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

void default_http_callback(const oklib::http::HttpRequest&, oklib::http::HttpResponse* response) {
  response->set_status_code(oklib::http::HttpStatusCode::not_found);
  response->set_body("404 Not Found");
  response->set_close_connection(true);
}

bool has_transfer_encoding(const oklib::http::HttpRequest& request) {
  return !request.header("Transfer-Encoding").empty();
}

bool looks_like_websocket_upgrade(const oklib::http::HttpRequest& request) {
  return equals_ignore_case(request.header("Upgrade"), "websocket") ||
         !request.header("Sec-WebSocket-Key").empty() ||
         !request.header("Sec-WebSocket-Version").empty();
}

bool request_offers_permessage_deflate(const oklib::http::HttpRequest& request) {
  for (const auto& value : request.headers().values("Sec-WebSocket-Extensions")) {
    if (lower(value).find("permessage-deflate") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool contains_value(const std::vector<std::string>& values, std::string_view needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

std::string encode_switching_protocols(std::string_view accept_key,
                                       std::string_view selected_subprotocol,
                                       bool compression_enabled) {
  std::string response;
  response.reserve(256);
  response.append("HTTP/1.1 101 Switching Protocols\r\n");
  response.append("Upgrade: websocket\r\n");
  response.append("Connection: Upgrade\r\n");
  response.append("Sec-WebSocket-Accept: ");
  response.append(accept_key);
  response.append("\r\n");
  if (!selected_subprotocol.empty()) {
    response.append("Sec-WebSocket-Protocol: ");
    response.append(selected_subprotocol);
    response.append("\r\n");
  }
  if (compression_enabled) {
    response.append("Sec-WebSocket-Extensions: permessage-deflate; "
                    "server_no_context_takeover; client_no_context_takeover\r\n");
  }
  response.append("\r\n");
  return response;
}

void send_http_error(const oklib::net::TcpConnectionPtr& connection,
                     int status,
                     std::string_view reason,
                     std::string_view extra_headers = {}) {
  std::string response = "HTTP/1.1 " + std::to_string(status) + " ";
  response.append(reason);
  response.append("\r\nConnection: close\r\n");
  if (!extra_headers.empty()) {
    response.append(extra_headers);
  }
  response.append("Content-Length: 0\r\n\r\n");
  connection->send(response);
  connection->shutdown();
}

WebSocketCloseInfo close_info_from_payload(std::string_view payload, bool* ok) {
  *ok = true;
  WebSocketCloseInfo info;
  if (payload.empty()) {
    return info;
  }
  if (payload.size() == 1) {
    *ok = false;
    return info;
  }
  info.code = static_cast<std::uint16_t>(
      (static_cast<unsigned char>(payload[0]) << 8) |
      static_cast<unsigned char>(payload[1]));
  if (!valid_close_code(info.code)) {
    *ok = false;
    return info;
  }
  info.reason.assign(payload.substr(2));
  if (!info.reason.empty() && !valid_utf8(info.reason)) {
    *ok = false;
  }
  return info;
}

std::uint16_t close_code_for_error(WebSocketError error) {
  switch (error) {
    case WebSocketError::message_too_big:
      return 1009;
    case WebSocketError::invalid_utf8:
      return 1007;
    default:
      return 1002;
  }
}

void notify_error(const WebSocketService& service,
                  const WebSocketChannelPtr& channel,
                  WebSocketError error) {
  if (service.on_error) {
    service.on_error(channel, error);
  }
}

void notify_close(const WebSocketService& service,
                  const oklib::net::TcpConnectionPtr& connection,
                  ServerConnectionContext* context,
                  WebSocketCloseInfo info) {
  if (context->ping_timer.valid()) {
    connection->loop()->cancel(context->ping_timer);
    context->ping_timer = oklib::net::TimerId();
  }
  if (context->close_notified) {
    return;
  }
  context->close_notified = true;
  if (service.on_close) {
    service.on_close(context->channel, std::move(info));
  }
}

void close_for_error(const WebSocketService& service,
                     const oklib::net::TcpConnectionPtr& connection,
                     ServerConnectionContext* context,
                     WebSocketError error,
                     std::string reason) {
  const auto code = close_code_for_error(error);
  notify_error(service, context->channel, error);
  context->channel->close(code, reason);
  notify_close(service, connection, context, WebSocketCloseInfo{code, std::move(reason)});
}

}  // namespace

WebSocketServer::WebSocketServer(oklib::net::EventLoop* loop,
                                 const oklib::net::InetAddress& listen_address,
                                 std::string name,
                                 oklib::net::TcpServer::Option option)
    : server_(loop, listen_address, std::move(name), option),
      http_callback_(default_http_callback) {
  server_.set_connection_callback([this](const oklib::net::TcpConnectionPtr& conn) {
    on_connection(conn);
  });
  server_.set_message_callback([this](const oklib::net::TcpConnectionPtr& conn,
                                      oklib::net::Buffer* buffer,
                                      oklib::Timestamp receive_time) {
    on_message(conn, buffer, receive_time);
  });
}

void WebSocketServer::start() {
  server_.start();
}

void WebSocketServer::set_router(const oklib::http::HttpRouter& router) {
  set_streaming_http_callback(router.streaming_callback());
}

void WebSocketServer::on_connection(const oklib::net::TcpConnectionPtr& connection) {
  if (connection->connected()) {
    ServerConnectionContext context;
    context.http.set_peer_address(connection->peer_address().to_ip(), connection->peer_address().port());
    connection->set_context(std::move(context));
    return;
  }

  auto* context = std::any_cast<ServerConnectionContext>(&connection->mutable_context());
  if (context != nullptr && context->protocol == ServerConnectionContext::Protocol::websocket &&
      context->channel) {
    notify_close(websocket_service_, connection, context, {});
  }
}

void WebSocketServer::on_message(const oklib::net::TcpConnectionPtr& connection,
                                 oklib::net::Buffer* buffer,
                                 oklib::Timestamp receive_time) {
  auto* context = std::any_cast<ServerConnectionContext>(&connection->mutable_context());
  if (context == nullptr) {
    send_http_error(connection, 400, "Bad Request");
    return;
  }

  on_plain_message(connection, buffer, receive_time);
}

void WebSocketServer::on_plain_message(const oklib::net::TcpConnectionPtr& connection,
                                       oklib::net::Buffer* buffer,
                                       oklib::Timestamp receive_time) {
  auto* context = std::any_cast<ServerConnectionContext>(&connection->mutable_context());
  if (context == nullptr) {
    send_http_error(connection, 400, "Bad Request");
    return;
  }

  if (context->protocol == ServerConnectionContext::Protocol::websocket) {
    on_websocket_message(connection, buffer);
    return;
  }

  while (connection->connected()) {
    if (context->http.streaming_body_active()) {
      const auto status = context->http.process_streaming_body(buffer);
      if (status == oklib::http::HttpParseStatus::error) {
        send_http_error(connection, 400, "Bad Request");
        return;
      }
      if (status == oklib::http::HttpParseStatus::incomplete) {
        return;
      }
      context->http.reset();
      if (buffer->readable_bytes() == 0) {
        return;
      }
      continue;
    }

    const auto status =
        streaming_http_callback_ ? context->http.parse_request_head(buffer, receive_time)
                                 : context->http.parse_request(buffer, receive_time);
    if (status == oklib::http::HttpParseStatus::error) {
      send_http_error(connection, 400, "Bad Request");
      return;
    }
    if (status == oklib::http::HttpParseStatus::incomplete) {
      return;
    }

    oklib::http::HttpRequest request = context->http.take_request();
    if (looks_like_websocket_upgrade(request)) {
      if (!try_websocket_upgrade(connection, &context->http, request, buffer)) {
        return;
      }
      context->http.reset();
      if (buffer->readable_bytes() > 0) {
        on_websocket_message(connection, buffer);
      }
      return;
    }

    bool close = false;
    if (streaming_http_callback_) {
      close = on_streaming_http_request(connection, &context->http, std::move(request));
    } else if (async_http_callback_) {
      close = on_async_http_request(connection, &context->http, std::move(request));
    } else {
      close = on_http_request(connection, request);
    }
    if (!context->http.streaming_body_active()) {
      context->http.reset();
    }
    if (context->http.streaming_body_active()) {
      const auto body_status = context->http.process_streaming_body(buffer);
      if (body_status == oklib::http::HttpParseStatus::error) {
        send_http_error(connection, 400, "Bad Request");
        return;
      }
      if (body_status == oklib::http::HttpParseStatus::incomplete) {
        return;
      }
      context->http.reset();
    }
    if (close || buffer->readable_bytes() == 0) {
      return;
    }
  }
}

bool WebSocketServer::on_http_request(const oklib::net::TcpConnectionPtr& connection,
                                      const oklib::http::HttpRequest& request) {
  const bool close = should_close(request);
  oklib::http::HttpResponse response(close);
  http_callback_(request, &response);
  oklib::net::Buffer output;
  response.append_to_buffer(&output, request.method() != oklib::http::HttpMethod::head);
  connection->send(&output);
  if (response.close_connection()) {
    connection->shutdown();
  }
  return response.close_connection();
}

bool WebSocketServer::on_async_http_request(const oklib::net::TcpConnectionPtr& connection,
                                            oklib::http::HttpContext* context,
                                            oklib::http::HttpRequest request) {
  const bool close = should_close(request);
  const bool include_body = request.method() != oklib::http::HttpMethod::head;
  auto writer = context->make_response_writer(connection, close, include_body);
  async_http_callback_(std::move(request), std::move(writer));
  return close;
}

bool WebSocketServer::on_streaming_http_request(const oklib::net::TcpConnectionPtr& connection,
                                                oklib::http::HttpContext* context,
                                                oklib::http::HttpRequest request) {
  const bool close = should_close(request);
  const bool include_body = request.method() != oklib::http::HttpMethod::head;
  const bool chunked_body = has_transfer_encoding(request);
  const auto body_length = request.content_length();
  auto writer = context->make_response_writer(connection, close, include_body);
  oklib::http::HttpRequestBodyStream body_stream;
  streaming_http_callback_(std::move(request), body_stream, std::move(writer));
  if (chunked_body) {
    context->start_streaming_chunked_body(std::move(body_stream));
  } else {
    context->start_streaming_body(std::move(body_stream), body_length);
  }
  return close;
}

bool WebSocketServer::try_websocket_upgrade(const oklib::net::TcpConnectionPtr& connection,
                                            oklib::http::HttpContext* context,
                                            const oklib::http::HttpRequest& request,
                                            oklib::net::Buffer* leftover) {
  (void)context;
  (void)leftover;
  auto* server_context =
      std::any_cast<ServerConnectionContext>(&connection->mutable_context());
  if (server_context == nullptr) {
    send_http_error(connection, 400, "Bad Request");
    return false;
  }
  if (!websocket_is_upgrade_request(request)) {
    if (request.header("Sec-WebSocket-Version") != "13") {
      send_http_error(connection, 426, "Upgrade Required", "Sec-WebSocket-Version: 13\r\n");
    } else {
      send_http_error(connection, 400, "Bad Request");
    }
    return false;
  }

  const auto accept = websocket_validate_client_key(request.header("Sec-WebSocket-Key"));
  if (!accept) {
    send_http_error(connection, 400, "Bad Request");
    return false;
  }

  std::string selected_subprotocol;
  const auto requested_subprotocols = websocket_requested_subprotocols(request);
  if (websocket_service_.select_subprotocol) {
    auto selected = websocket_service_.select_subprotocol(request, requested_subprotocols);
    if (selected && !selected->empty()) {
      if (!contains_value(requested_subprotocols, *selected)) {
        send_http_error(connection, 400, "Bad Request");
        return false;
      }
      selected_subprotocol = std::move(*selected);
    }
  }

  const bool compression_enabled =
      websocket_service_.options.enable_compression && k_compression_available &&
      request_offers_permessage_deflate(request);
  WebSocketOptions connection_options = websocket_service_.options;
  connection_options.enable_compression = compression_enabled;

  connection->send(encode_switching_protocols(*accept, selected_subprotocol, compression_enabled));
  server_context->protocol = ServerConnectionContext::Protocol::websocket;
  server_context->options = connection_options;
  server_context->compression_enabled = compression_enabled;
  server_context->frame_parser =
      WebSocketFrameParser(WebSocketEndpointRole::server, connection_options);
  server_context->message_assembler = WebSocketMessageAssembler(connection_options);
  server_context->channel = std::make_shared<WebSocketChannel>(
      connection, WebSocketEndpointRole::server, connection_options, compression_enabled);

  if (websocket_service_.ping_interval.count() > 0) {
    std::weak_ptr<WebSocketChannel> weak = server_context->channel;
    server_context->ping_timer = connection->loop()->run_every(
        websocket_service_.ping_interval, [weak] {
          if (auto channel = weak.lock(); channel && channel->connected()) {
            channel->send_ping();
          }
        });
  }

  if (websocket_service_.on_open) {
    websocket_service_.on_open(server_context->channel, request);
  }
  return true;
}

void WebSocketServer::on_websocket_message(const oklib::net::TcpConnectionPtr& connection,
                                           oklib::net::Buffer* buffer) {
  auto* context = std::any_cast<ServerConnectionContext>(&connection->mutable_context());
  if (context == nullptr || !context->channel) {
    connection->force_close();
    return;
  }

  std::vector<WebSocketFrame> frames;
  const auto parse_status = context->frame_parser.parse(buffer, &frames);
  if (parse_status == WebSocketParseStatus::error) {
    const auto error = context->frame_parser.error();
    close_for_error(websocket_service_, connection, context, error, "protocol error");
    return;
  }

  for (const auto& frame : frames) {
    WebSocketMessage message;
    const auto message_status = context->message_assembler.consume(frame, &message);
    if (message_status == WebSocketMessageStatus::error) {
      const auto error = context->message_assembler.error();
      close_for_error(websocket_service_, connection, context, error, "protocol error");
      return;
    }

    if (message_status == WebSocketMessageStatus::control) {
      if (frame.opcode == WebSocketOpcode::ping) {
        context->channel->send_pong(frame.payload);
        continue;
      }
      if (frame.opcode == WebSocketOpcode::pong) {
        continue;
      }
      if (frame.opcode == WebSocketOpcode::close) {
        bool ok = false;
        WebSocketCloseInfo info = close_info_from_payload(frame.payload, &ok);
        if (!ok) {
          close_for_error(websocket_service_,
                          connection,
                          context,
                          WebSocketError::protocol_error,
                          "bad close frame");
          return;
        }
        context->channel->mark_close_received();
        if (!context->channel->close_sent()) {
          const std::uint16_t code = info.code == 1005 ? 1000 : info.code;
          context->channel->close(code, info.reason);
        }
        notify_close(websocket_service_, connection, context, std::move(info));
        return;
      }
      continue;
    }

    if (message_status != WebSocketMessageStatus::complete) {
      continue;
    }

    if (message.compressed) {
      std::string inflated;
      if (!context->compression_enabled || !websocket_inflate(message.payload, &inflated)) {
        close_for_error(websocket_service_,
                        connection,
                        context,
                        WebSocketError::compression_error,
                        "compression error");
        return;
      }
      message.payload = std::move(inflated);
      if (message.opcode == WebSocketOpcode::text && !valid_utf8(message.payload)) {
        close_for_error(websocket_service_,
                        connection,
                        context,
                        WebSocketError::invalid_utf8,
                        "invalid utf-8");
        return;
      }
    }

    if (websocket_service_.on_message) {
      websocket_service_.on_message(context->channel, message.payload, message.opcode);
    }
  }
}

bool WebSocketServer::should_close(const oklib::http::HttpRequest& request) const {
  const std::string connection_header = request.header("Connection");
  return header_has_token(connection_header, "close") ||
         (request.version() == oklib::http::HttpVersion::http10 &&
          !header_has_token(connection_header, "Keep-Alive"));
}

}  // namespace oklib::websocket
