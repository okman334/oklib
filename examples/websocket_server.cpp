#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#include "oklib/base/logging.h"
#include "oklib/http/http_request.h"
#include "oklib/http/http_response.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"
#include "oklib/websocket/websocket_server.h"

namespace {

uint16_t parse_port(const char* value, uint16_t fallback) {
  if (value == nullptr) {
    return fallback;
  }
  const int parsed = std::atoi(value);
  if (parsed <= 0 || parsed > 65535) {
    return fallback;
  }
  return static_cast<uint16_t>(parsed);
}

int parse_threads(const char* value, int fallback) {
  if (value == nullptr) {
    return fallback;
  }
  const int parsed = std::atoi(value);
  return parsed >= 0 ? parsed : fallback;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);

  const uint16_t port = argc > 1 ? parse_port(argv[1], 8081) : 8081;
  const int io_threads = argc > 2 ? parse_threads(argv[2], 0) : 0;

  oklib::Logger::set_file_basename("oklib_websocket_server");
  OKLIB_LOG_INFO << "starting WebSocket demo server on port " << port
                 << ", io_threads=" << io_threads;

  oklib::net::EventLoop loop;
  oklib::websocket::WebSocketServer server(&loop,
                                           oklib::net::InetAddress::any(port),
                                           "oklib-websocket-server");
  server.set_thread_num(io_threads);
  server.set_http_callback([](const oklib::http::HttpRequest& request,
                              oklib::http::HttpResponse* response) {
    if (request.path() == "/ping") {
      response->set_status_code(200);
      response->set_content_type("text/plain; charset=utf-8");
      response->set_body("pong\n");
      return;
    }
    response->set_status_code(404);
    response->set_content_type("text/plain; charset=utf-8");
    response->set_body("not found\n");
  });

  oklib::websocket::WebSocketService service;
  service.options.enable_compression = true;
  service.ping_interval = std::chrono::seconds(15);
  service.on_open = [](const oklib::websocket::WebSocketChannelPtr& channel,
                       const oklib::http::HttpRequest& request) {
    OKLIB_LOG_INFO << "websocket open from " << request.peer_address()
                   << " target=" << request.target();
    channel->send_text("welcome to oklib websocket");
  };
  service.on_message = [](const oklib::websocket::WebSocketChannelPtr& channel,
                          std::string_view message,
                          oklib::websocket::WebSocketOpcode opcode) {
    if (opcode == oklib::websocket::WebSocketOpcode::text) {
      OKLIB_LOG_INFO << "websocket text message bytes=" << message.size();
      channel->send_text("echo: " + std::string(message));
      return;
    }
    if (opcode == oklib::websocket::WebSocketOpcode::binary) {
      OKLIB_LOG_INFO << "websocket binary message bytes=" << message.size();
      channel->send_binary(message);
    }
  };
  service.on_close = [](const oklib::websocket::WebSocketChannelPtr&,
                        oklib::websocket::WebSocketCloseInfo info) {
    OKLIB_LOG_INFO << "websocket close code=" << info.code
                   << " reason=" << info.reason;
  };
  service.on_error = [](const oklib::websocket::WebSocketChannelPtr&,
                        oklib::websocket::WebSocketError) {
    OKLIB_LOG_WARN << "websocket protocol error";
  };
  server.register_websocket_service(std::move(service));
  server.start();

  std::cout << "oklib WebSocket server listening on port " << server.port() << '\n'
            << "HTTP: curl http://127.0.0.1:" << server.port() << "/ping\n"
            << "WS:   " << argv[0] << " accepts ws://127.0.0.1:"
            << server.port() << "/ws\n"
            << "usage: " << argv[0] << " [port=8081] [io_threads=0]\n";
  loop.loop();
  return 0;
}
