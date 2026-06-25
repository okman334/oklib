#include <oklib/http/http_response.h>
#include <oklib/http/http_client.h>
#include <oklib/net/event_loop.h>
#include <oklib/websocket/websocket_client.h>
#include <oklib/websocket/websocket_server.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

#if OKLIB_ENABLE_TLS
struct TestCertificate {
  std::filesystem::path directory;
  std::filesystem::path cert_file;
  std::filesystem::path key_file;
};

TestCertificate make_test_certificate() {
  char dir_template[] = "/tmp/oklib-wss-XXXXXX";
  char* dir = ::mkdtemp(dir_template);
  require(dir != nullptr, "temporary websocket certificate directory created");

  TestCertificate certificate;
  certificate.directory = dir;
  certificate.cert_file = certificate.directory / "server.crt";
  certificate.key_file = certificate.directory / "server.key";

  const std::string command =
      "openssl req -x509 -newkey rsa:2048 -nodes -keyout " + certificate.key_file.string() +
      " -out " + certificate.cert_file.string() +
      " -subj /CN=localhost -days 1 >/dev/null 2>&1";
  require(std::system(command.c_str()) == 0, "websocket self-signed certificate generated");
  return certificate;
}
#endif

void test_websocket_server_client_echo_and_http() {
  using namespace std::chrono_literals;

  oklib::net::EventLoop loop;
  oklib::websocket::WebSocketServer server(&loop,
                                           oklib::net::InetAddress::loopback(0),
                                           "websocket-integration-server");

  server.set_http_callback([](const oklib::http::HttpRequest& request,
                              oklib::http::HttpResponse* response) {
    if (request.path() == "/ping") {
      response->set_status_code(200);
      response->set_body("pong");
      return;
    }
    response->set_status_code(404);
    response->set_body("missing");
  });

  oklib::websocket::WebSocketService service;
  service.options.enable_compression = true;
  std::atomic<bool> opened{false};
  std::atomic<bool> closed{false};
  std::atomic<bool> server_compression{false};
  service.on_open = [&](const oklib::websocket::WebSocketChannelPtr& channel,
                        const oklib::http::HttpRequest&) {
    opened = true;
    server_compression = channel->compression_enabled();
    channel->send_text("welcome");
  };
  service.on_message = [](const oklib::websocket::WebSocketChannelPtr& channel,
                          std::string_view message,
                          oklib::websocket::WebSocketOpcode opcode) {
    if (opcode == oklib::websocket::WebSocketOpcode::text) {
      channel->send_text("echo:" + std::string(message));
    }
  };
  service.on_close = [&](const oklib::websocket::WebSocketChannelPtr&,
                         oklib::websocket::WebSocketCloseInfo) {
    closed = true;
  };
  server.register_websocket_service(service);
  server.start();

  oklib::websocket::WebSocketOptions client_options;
  client_options.enable_compression = true;
  oklib::websocket::WebSocketClient client(&loop,
                                           "websocket-integration-client",
                                           client_options);
  oklib::http::HttpClient http_client(&loop,
                                      oklib::net::InetAddress("127.0.0.1", server.port()),
                                      "127.0.0.1:" + std::to_string(server.port()),
                                      "websocket-integration-http-client");
  std::vector<std::string> messages;
  bool http_ok = false;
  bool client_open = false;
  bool client_compression = false;
  bool failed = false;
  client.set_open_callback([&](const oklib::websocket::WebSocketChannelPtr& channel) {
    client_open = true;
    client_compression = channel->compression_enabled();
    client.send_text("oklib");
  });
  client.set_message_callback([&](const oklib::websocket::WebSocketChannelPtr&,
                                  std::string_view message,
                                  oklib::websocket::WebSocketOpcode opcode) {
    if (opcode != oklib::websocket::WebSocketOpcode::text) {
      failed = true;
      loop.quit();
      return;
    }
    messages.emplace_back(message);
    if (messages.size() == 2) {
      client.close();
      loop.run_after(20ms, [&] { loop.quit(); });
    }
  });
  client.set_close_callback([&](const oklib::websocket::WebSocketChannelPtr&,
                                oklib::websocket::WebSocketCloseInfo) {
    closed = true;
  });
  client.set_error_callback([&](const oklib::websocket::WebSocketChannelPtr&,
                                oklib::websocket::WebSocketError) {
    failed = true;
    loop.quit();
  });

  const std::string url =
      "ws://127.0.0.1:" + std::to_string(server.port()) + "/chat";
  http_client.set_response_callback([&](oklib::http::HttpResponseMessage response) {
    if (response.status_code != 200 || response.body != "pong") {
      failed = true;
      loop.quit();
      return;
    }
    http_ok = true;
    require(client.open(url) == 0, "websocket client open starts");
  });
  http_client.set_error_callback([&] {
    failed = true;
    loop.quit();
  });
  http_client.send(oklib::http::HttpClientRequest("GET", "/ping"));

  loop.run_after(3s, [&] {
    failed = true;
    loop.quit();
  });
  loop.loop();

  require(!failed, "websocket integration did not fail");
  require(http_ok, "ordinary HTTP request shares websocket server port");
  require(opened.load(), "server on_open called");
  require(client_open, "client on_open called");
#if OKLIB_ENABLE_WEBSOCKET_COMPRESSION
  require(server_compression.load(), "server negotiated websocket compression");
  require(client_compression, "client negotiated websocket compression");
#else
  require(!server_compression.load(), "server does not negotiate compression when disabled");
  require(!client_compression, "client does not negotiate compression when disabled");
#endif
  require(messages.size() == 2, "client receives welcome and echo");
  require(messages[0] == "welcome", "client receives welcome");
  require(messages[1] == "echo:oklib", "client receives echo");
  require(closed.load(), "close callback observed");
}

#if OKLIB_ENABLE_TLS
void test_wss_client_server_round_trip() {
  using namespace std::chrono_literals;

  const auto certificate = make_test_certificate();
  oklib::net::EventLoop loop;
  oklib::websocket::WebSocketServer server(&loop,
                                           oklib::net::InetAddress::loopback(0),
                                           "wss-integration-server");
  oklib::http::TlsServerOptions server_tls;
  server_tls.enabled = true;
  server_tls.cert_file = certificate.cert_file.string();
  server_tls.key_file = certificate.key_file.string();
  server.set_tls_options(std::move(server_tls));

  oklib::websocket::WebSocketService service;
  service.on_open = [](const oklib::websocket::WebSocketChannelPtr& channel,
                       const oklib::http::HttpRequest&) {
    channel->send_text("secure-welcome");
  };
  service.on_message = [](const oklib::websocket::WebSocketChannelPtr& channel,
                          std::string_view message,
                          oklib::websocket::WebSocketOpcode opcode) {
    if (opcode == oklib::websocket::WebSocketOpcode::text) {
      channel->send_text("secure-echo:" + std::string(message));
    }
  };
  server.register_websocket_service(std::move(service));
  server.start();

  oklib::websocket::WebSocketClient client(&loop, "wss-integration-client");
  oklib::http::TlsClientOptions client_tls;
  client_tls.enabled = true;
  client_tls.verify_peer = false;
  client_tls.server_name = "localhost";
  client.set_tls_options(std::move(client_tls));

  std::vector<std::string> messages;
  bool opened = false;
  bool failed = false;
  client.set_open_callback([&](const oklib::websocket::WebSocketChannelPtr&) {
    opened = true;
    client.send_text("oklib");
  });
  client.set_message_callback([&](const oklib::websocket::WebSocketChannelPtr&,
                                  std::string_view message,
                                  oklib::websocket::WebSocketOpcode opcode) {
    if (opcode != oklib::websocket::WebSocketOpcode::text) {
      failed = true;
      loop.quit();
      return;
    }
    messages.emplace_back(message);
    if (messages.size() == 2) {
      client.close();
      loop.run_after(20ms, [&] { loop.quit(); });
    }
  });
  client.set_error_callback([&](const oklib::websocket::WebSocketChannelPtr&,
                                oklib::websocket::WebSocketError) {
    failed = true;
    loop.quit();
  });

  const std::string url =
      "wss://127.0.0.1:" + std::to_string(server.port()) + "/secure";
  require(client.open(url) == 0, "wss websocket client open starts");
  loop.run_after(3s, [&] {
    failed = true;
    loop.quit();
  });
  loop.loop();

  require(!failed, "wss integration did not fail");
  require(opened, "wss client opened");
  require(messages.size() == 2, "wss client receives welcome and echo");
  require(messages[0] == "secure-welcome", "wss welcome received");
  require(messages[1] == "secure-echo:oklib", "wss echo received");
}
#endif

}  // namespace

int main() {
  test_websocket_server_client_echo_and_http();
#if OKLIB_ENABLE_TLS
  test_wss_client_server_round_trip();
#endif
  return 0;
}
