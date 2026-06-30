#include <oklib/http/http_response.h>
#include <oklib/http/http_client.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/buffer.h>
#include <oklib/net/tcp_connection.h>
#include <oklib/net/tcp_server.h>
#include <oklib/websocket/websocket_client.h>
#include <oklib/websocket/websocket_frame.h>
#include <oklib/websocket/websocket_handshake.h>
#include <oklib/websocket/websocket_server.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

std::string raw_request_once(const oklib::net::InetAddress& address,
                             const std::string& request) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0, "raw socket succeeds");
  require(::connect(fd, address.sockaddr_ptr(), address.length()) == 0, "raw connect succeeds");
  require(::write(fd, request.data(), request.size()) == static_cast<ssize_t>(request.size()),
          "raw request write succeeds");

  std::string response;
  char buffer[4096];
  for (;;) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n > 0) {
      response.append(buffer, static_cast<std::size_t>(n));
      if (response.find("\r\n\r\n") != std::string::npos) {
        break;
      }
      continue;
    }
    break;
  }
  ::close(fd);
  return response;
}

std::string websocket_upgrade_request(std::string_view key,
                                      std::string_view path = "/ws",
                                      std::string_view version = "13") {
  std::string request;
  request.append("GET ");
  request.append(path);
  request.append(" HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                 "Connection: Upgrade\r\nSec-WebSocket-Key: ");
  request.append(key);
  request.append("\r\nSec-WebSocket-Version: ");
  request.append(version);
  request.append("\r\n\r\n");
  return request;
}

std::string raw_header_value(std::string_view request, std::string_view field) {
  const std::string prefix = std::string(field) + ": ";
  const auto pos = request.find(prefix);
  if (pos == std::string_view::npos) {
    return {};
  }
  const auto value_start = pos + prefix.size();
  const auto value_end = request.find("\r\n", value_start);
  if (value_end == std::string_view::npos) {
    return {};
  }
  return std::string(request.substr(value_start, value_end - value_start));
}

bool read_one_frame(int fd,
                    oklib::websocket::WebSocketEndpointRole role,
                    oklib::websocket::WebSocketFrame* frame) {
  oklib::net::Buffer input;
  oklib::websocket::WebSocketFrameParser parser(role);
  std::vector<oklib::websocket::WebSocketFrame> frames;
  char buffer[4096];
  for (;;) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n <= 0) {
      return false;
    }
    input.append(buffer, static_cast<std::size_t>(n));
    const auto status = parser.parse(&input, &frames);
    if (status == oklib::websocket::WebSocketParseStatus::error) {
      return false;
    }
    if (!frames.empty()) {
      *frame = std::move(frames.front());
      return true;
    }
  }
}

bool ipv6_loopback_available() {
  const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_loopback;
  address.sin6_port = 0;
  const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  ::close(fd);
  return ok;
}

void test_websocket_client_replies_to_ping() {
  using namespace std::chrono_literals;

  oklib::net::EventLoop loop;
  oklib::net::TcpServer raw_server(&loop,
                                   oklib::net::InetAddress::loopback(0),
                                   "websocket-client-ping-raw-server");
  bool handshake_sent = false;
  bool pong_received = false;
  bool failed = false;
  std::string request_bytes;
  oklib::websocket::WebSocketFrameParser parser(oklib::websocket::WebSocketEndpointRole::server);
  raw_server.set_message_callback([&](const oklib::net::TcpConnectionPtr& connection,
                                      oklib::net::Buffer* buffer,
                                      oklib::Timestamp) {
    if (!handshake_sent) {
      request_bytes.append(buffer->retrieve_all_as_string());
      if (request_bytes.find("\r\n\r\n") == std::string::npos) {
        return;
      }
      const auto key = raw_header_value(request_bytes, "Sec-WebSocket-Key");
      if (key.empty()) {
        failed = true;
        loop.quit();
        return;
      }
      std::string response =
          "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
          "Connection: Upgrade\r\nSec-WebSocket-Accept: ";
      response.append(oklib::websocket::websocket_accept_key(key));
      response.append("\r\n\r\n");
      connection->send(response);

      oklib::websocket::WebSocketFrame ping;
      ping.opcode = oklib::websocket::WebSocketOpcode::ping;
      ping.payload = "from-server";
      connection->send(oklib::websocket::encode_websocket_frame(
          ping, oklib::websocket::WebSocketMasking::unmasked));
      handshake_sent = true;
      return;
    }

    std::vector<oklib::websocket::WebSocketFrame> frames;
    const auto status = parser.parse(buffer, &frames);
    if (status == oklib::websocket::WebSocketParseStatus::error) {
      failed = true;
      loop.quit();
      return;
    }
    for (const auto& frame : frames) {
      if (frame.opcode == oklib::websocket::WebSocketOpcode::pong &&
          frame.payload == "from-server") {
        pong_received = true;
        loop.quit();
        return;
      }
    }
  });
  raw_server.start();

  oklib::websocket::WebSocketClient client(&loop, "websocket-client-ping-test");
  client.set_error_callback([&](const oklib::websocket::WebSocketChannelPtr&,
                                oklib::websocket::WebSocketError) {
    failed = true;
    loop.quit();
  });
  const std::string url =
      "ws://127.0.0.1:" + std::to_string(raw_server.port()) + "/ping";
  require(client.open(url) == 0, "client ping test open starts");
  loop.run_after(3s, [&] {
    failed = true;
    loop.quit();
  });
  loop.loop();

  require(!failed, "client ping test did not fail");
  require(handshake_sent, "raw websocket server sent handshake");
  require(pong_received, "client automatically replies pong to server ping");
}

void test_websocket_client_reports_protocol_close() {
  using namespace std::chrono_literals;

  oklib::net::EventLoop loop;
  oklib::net::TcpServer raw_server(&loop,
                                   oklib::net::InetAddress::loopback(0),
                                   "websocket-client-protocol-raw-server");
  bool handshake_sent = false;
  bool failed = false;
  bool saw_protocol_error = false;
  bool saw_protocol_close = false;
  std::string request_bytes;
  raw_server.set_message_callback([&](const oklib::net::TcpConnectionPtr& connection,
                                      oklib::net::Buffer* buffer,
                                      oklib::Timestamp) {
    if (handshake_sent) {
      buffer->retrieve_all();
      return;
    }
    request_bytes.append(buffer->retrieve_all_as_string());
    if (request_bytes.find("\r\n\r\n") == std::string::npos) {
      return;
    }
    const auto key = raw_header_value(request_bytes, "Sec-WebSocket-Key");
    if (key.empty()) {
      failed = true;
      loop.quit();
      return;
    }
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: ";
    response.append(oklib::websocket::websocket_accept_key(key));
    response.append("\r\n\r\n");
    connection->send(response);

    oklib::websocket::WebSocketFrame bad_server_frame;
    bad_server_frame.opcode = oklib::websocket::WebSocketOpcode::text;
    bad_server_frame.payload = "masked-from-server";
    connection->send(oklib::websocket::encode_websocket_frame(
        bad_server_frame,
        oklib::websocket::WebSocketMasking::masked,
        std::array<unsigned char, 4>{9, 8, 7, 6}));
    handshake_sent = true;
  });
  raw_server.start();

  oklib::websocket::WebSocketClient client(&loop, "websocket-client-protocol-test");
  client.set_error_callback([&](const oklib::websocket::WebSocketChannelPtr&,
                                oklib::websocket::WebSocketError error) {
    saw_protocol_error = error == oklib::websocket::WebSocketError::protocol_error;
  });
  client.set_close_callback([&](const oklib::websocket::WebSocketChannelPtr&,
                                oklib::websocket::WebSocketCloseInfo info) {
    saw_protocol_close = info.code == 1002;
    loop.quit();
  });
  const std::string url =
      "ws://127.0.0.1:" + std::to_string(raw_server.port()) + "/protocol";
  require(client.open(url) == 0, "client protocol test open starts");
  loop.run_after(3s, [&] {
    failed = true;
    loop.quit();
  });
  loop.loop();

  require(!failed, "client protocol close test did not fail");
  require(handshake_sent, "raw websocket server sent protocol test handshake");
  require(saw_protocol_error, "client reports protocol error");
  require(saw_protocol_close, "client on_close sees protocol close code");
}

std::uint16_t close_code_from_payload(std::string_view payload) {
  if (payload.size() < 2) {
    return 1005;
  }
  return static_cast<std::uint16_t>(
      (static_cast<unsigned char>(payload[0]) << 8) |
      static_cast<unsigned char>(payload[1]));
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

void test_malformed_websocket_handshake_responses() {
  oklib::net::EventLoop loop;
  oklib::websocket::WebSocketServer server(&loop,
                                           oklib::net::InetAddress::loopback(0),
                                           "websocket-bad-handshake-server");
  server.register_websocket_service({});
  server.start();

  std::vector<std::string> responses;
  std::thread client([&] {
    const auto address = server.listen_address();
    responses.push_back(raw_request_once(
        address,
        "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n"));
    responses.push_back(raw_request_once(
        address,
        websocket_upgrade_request("dGhlIHNhbXBsZSBub25jZQ==", "/ws", "12")));
    responses.push_back(raw_request_once(
        address,
        "POST /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"));
    loop.queue_in_loop([&] { loop.quit(); });
  });
  loop.run_after(std::chrono::seconds(3), [&] { loop.quit(); });
  loop.loop();
  client.join();

  require(responses.size() == 3, "malformed handshake responses collected");
  require(responses[0].find("HTTP/1.1 400 Bad Request") == 0,
          "missing websocket key returns 400");
  require(responses[1].find("HTTP/1.1 426 Upgrade Required") == 0,
          "wrong websocket version returns 426");
  require(responses[1].find("Sec-WebSocket-Version: 13\r\n") != std::string::npos,
          "426 advertises supported websocket version");
  require(responses[2].find("HTTP/1.1 400 Bad Request") == 0,
          "non-GET websocket upgrade returns 400");
}

void test_raw_ping_pong_and_protocol_close() {
  using namespace std::chrono_literals;

  oklib::net::EventLoop loop;
  oklib::websocket::WebSocketServer server(&loop,
                                           oklib::net::InetAddress::loopback(0),
                                           "websocket-raw-control-server");
  std::vector<oklib::websocket::WebSocketCloseInfo> close_infos;
  oklib::websocket::WebSocketService service;
  service.on_close = [&](const oklib::websocket::WebSocketChannelPtr&,
                         oklib::websocket::WebSocketCloseInfo info) {
    close_infos.push_back(std::move(info));
  };
  server.register_websocket_service(std::move(service));
  server.start();

  bool ping_pong_ok = false;
  bool close_echo_ok = false;
  bool protocol_close_ok = false;
  std::thread client([&] {
    const auto address = server.listen_address();
    {
      int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      require(fd >= 0, "ping raw socket succeeds");
      require(::connect(fd, address.sockaddr_ptr(), address.length()) == 0,
              "ping raw connect succeeds");
      const auto request = websocket_upgrade_request("dGhlIHNhbXBsZSBub25jZQ==");
      require(::write(fd, request.data(), request.size()) == static_cast<ssize_t>(request.size()),
              "ping upgrade write succeeds");
      std::string response;
      char buffer[512];
      while (response.find("\r\n\r\n") == std::string::npos) {
        const auto n = ::read(fd, buffer, sizeof(buffer));
        require(n > 0, "ping upgrade response read succeeds");
        response.append(buffer, static_cast<std::size_t>(n));
      }
      require(response.find("HTTP/1.1 101 Switching Protocols") == 0,
              "raw ping client upgraded");

      oklib::websocket::WebSocketFrame ping;
      ping.opcode = oklib::websocket::WebSocketOpcode::ping;
      ping.payload = "abc";
      const std::string encoded_ping = oklib::websocket::encode_websocket_frame(
          ping,
          oklib::websocket::WebSocketMasking::masked,
          std::array<unsigned char, 4>{1, 2, 3, 4});
      require(::write(fd, encoded_ping.data(), encoded_ping.size()) ==
                  static_cast<ssize_t>(encoded_ping.size()),
              "masked ping write succeeds");
      oklib::websocket::WebSocketFrame pong;
      require(read_one_frame(fd, oklib::websocket::WebSocketEndpointRole::client, &pong),
              "pong frame received");
      ping_pong_ok = pong.opcode == oklib::websocket::WebSocketOpcode::pong &&
                     pong.payload == "abc";

      oklib::websocket::WebSocketFrame close;
      close.opcode = oklib::websocket::WebSocketOpcode::close;
      close.payload.push_back(static_cast<char>(1000 >> 8));
      close.payload.push_back(static_cast<char>(1000 & 0xff));
      close.payload.append("bye");
      const std::string encoded_close = oklib::websocket::encode_websocket_frame(
          close,
          oklib::websocket::WebSocketMasking::masked,
          std::array<unsigned char, 4>{4, 3, 2, 1});
      require(::write(fd, encoded_close.data(), encoded_close.size()) ==
                  static_cast<ssize_t>(encoded_close.size()),
              "masked close write succeeds");
      oklib::websocket::WebSocketFrame close_echo;
      require(read_one_frame(fd, oklib::websocket::WebSocketEndpointRole::client, &close_echo),
              "close echo frame received");
      close_echo_ok = close_echo.opcode == oklib::websocket::WebSocketOpcode::close &&
                      close_code_from_payload(close_echo.payload) == 1000;
      ::close(fd);
    }

    {
      int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      require(fd >= 0, "protocol raw socket succeeds");
      require(::connect(fd, address.sockaddr_ptr(), address.length()) == 0,
              "protocol raw connect succeeds");
      const auto request = websocket_upgrade_request("x3JJHMbDL1EzLkh9GBhXDw==");
      require(::write(fd, request.data(), request.size()) == static_cast<ssize_t>(request.size()),
              "protocol upgrade write succeeds");
      std::string response;
      char buffer[512];
      while (response.find("\r\n\r\n") == std::string::npos) {
        const auto n = ::read(fd, buffer, sizeof(buffer));
        require(n > 0, "protocol upgrade response read succeeds");
        response.append(buffer, static_cast<std::size_t>(n));
      }
      oklib::websocket::WebSocketFrame unmasked_text;
      unmasked_text.opcode = oklib::websocket::WebSocketOpcode::text;
      unmasked_text.payload = "bad";
      const std::string encoded_bad = oklib::websocket::encode_websocket_frame(
          unmasked_text, oklib::websocket::WebSocketMasking::unmasked);
      require(::write(fd, encoded_bad.data(), encoded_bad.size()) ==
                  static_cast<ssize_t>(encoded_bad.size()),
              "unmasked frame write succeeds");
      oklib::websocket::WebSocketFrame protocol_close;
      require(read_one_frame(fd, oklib::websocket::WebSocketEndpointRole::client, &protocol_close),
              "protocol close frame received");
      protocol_close_ok =
          protocol_close.opcode == oklib::websocket::WebSocketOpcode::close &&
          close_code_from_payload(protocol_close.payload) == 1002;
      ::close(fd);
    }

    std::this_thread::sleep_for(20ms);
    loop.queue_in_loop([&] { loop.quit(); });
  });

  loop.run_after(3s, [&] { loop.quit(); });
  loop.loop();
  client.join();

  require(ping_pong_ok, "server automatically replies pong to masked ping");
  require(close_echo_ok, "server echoes close frame");
  const bool saw_normal_close = std::any_of(
      close_infos.begin(),
      close_infos.end(),
      [](const oklib::websocket::WebSocketCloseInfo& info) {
        return info.code == 1000 && info.reason == "bye";
      });
  const bool saw_protocol_close = std::any_of(
      close_infos.begin(),
      close_infos.end(),
      [](const oklib::websocket::WebSocketCloseInfo& info) {
        return info.code == 1002;
      });
  require(saw_normal_close, "server on_close sees normal close code and reason");
  require(protocol_close_ok, "unmasked client frame closes with 1002");
  require(saw_protocol_close, "server on_close sees protocol close code");
}

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

void test_websocket_client_supports_ipv6_literal_url() {
  using namespace std::chrono_literals;

  if (!ipv6_loopback_available()) {
    std::cout << "skip WebSocket IPv6 URL test: ::1 unavailable\n";
    return;
  }

  oklib::net::EventLoop loop;
  oklib::websocket::WebSocketServer server(&loop,
                                           oklib::net::InetAddress::loopback_ipv6(0),
                                           "websocket-ipv6-integration-server");

  oklib::websocket::WebSocketService service;
  service.on_open = [](const oklib::websocket::WebSocketChannelPtr& channel,
                       const oklib::http::HttpRequest&) {
    channel->send_text("ipv6-welcome");
  };
  service.on_message = [](const oklib::websocket::WebSocketChannelPtr& channel,
                          std::string_view message,
                          oklib::websocket::WebSocketOpcode opcode) {
    if (opcode == oklib::websocket::WebSocketOpcode::text) {
      channel->send_text("ipv6-echo:" + std::string(message));
    }
  };
  server.register_websocket_service(service);
  server.start();

  oklib::websocket::WebSocketClient client(&loop, "websocket-ipv6-client");
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

  const std::string url = "ws://[::1]:" + std::to_string(server.port()) + "/ws";
  require(client.open(url) == 0, "IPv6 websocket client open starts");
  loop.run_after(3s, [&] {
    failed = true;
    loop.quit();
  });
  loop.loop();

  require(!failed, "IPv6 websocket integration did not fail");
  require(opened, "IPv6 websocket client opened");
  require(messages.size() == 2, "IPv6 websocket client receives welcome and echo");
  require(messages[0] == "ipv6-welcome", "IPv6 websocket welcome received");
  require(messages[1] == "ipv6-echo:oklib", "IPv6 websocket echo received");
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
  test_malformed_websocket_handshake_responses();
  test_raw_ping_pong_and_protocol_close();
  test_websocket_client_replies_to_ping();
  test_websocket_client_reports_protocol_close();
  test_websocket_server_client_echo_and_http();
  test_websocket_client_supports_ipv6_literal_url();
#if OKLIB_ENABLE_TLS
  test_wss_client_server_round_trip();
#endif
  return 0;
}
