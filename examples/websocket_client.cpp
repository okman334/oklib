#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include "oklib/http/tls_options.h"
#include "oklib/net/event_loop.h"
#include "oklib/websocket/websocket_client.h"

namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);

  std::string url = argc > 1 ? argv[1] : "ws://127.0.0.1:8081/ws";
  bool verify_peer = false;
  std::string ca_file;
  bool compression = false;
  for (int i = 2; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--verify-peer") {
      verify_peer = true;
    } else if (option == "--ca-file" && i + 1 < argc) {
      ca_file = argv[++i];
    } else if (option == "--compression") {
      compression = true;
    }
  }

  if (starts_with(url, "wss://")) {
#if !OKLIB_ENABLE_TLS
    std::cerr << "This binary was built without OKLIB_ENABLE_TLS=ON.\n";
    return 1;
#endif
  }

  oklib::net::EventLoop loop;
  oklib::websocket::WebSocketOptions options;
  options.enable_compression = compression;
  oklib::websocket::WebSocketClient client(&loop, "oklib-websocket-client", options);
#if OKLIB_ENABLE_TLS
  if (starts_with(url, "wss://")) {
    oklib::http::TlsClientOptions tls;
    tls.enabled = true;
    tls.verify_peer = verify_peer;
    tls.ca_file = ca_file;
    client.set_tls_options(std::move(tls));
  }
#endif

  std::atomic_bool done{false};
  bool failed = false;
  client.set_open_callback([&](const oklib::websocket::WebSocketChannelPtr&) {
    std::cout << "connected: " << url << '\n'
              << "type lines to send text frames; /close closes the session\n";
    std::thread([&] {
      std::string line;
      while (!done.load() && std::getline(std::cin, line)) {
        if (line == "/close") {
          client.close();
          return;
        }
        client.send_text(line);
      }
      client.close();
    }).detach();
  });
  client.set_message_callback([](const oklib::websocket::WebSocketChannelPtr&,
                                 std::string_view message,
                                 oklib::websocket::WebSocketOpcode opcode) {
    if (opcode == oklib::websocket::WebSocketOpcode::text) {
      std::cout << "text: " << message << '\n';
    } else {
      std::cout << "binary: " << message.size() << " bytes\n";
    }
  });
  client.set_close_callback([&](const oklib::websocket::WebSocketChannelPtr&,
                                oklib::websocket::WebSocketCloseInfo info) {
    done = true;
    std::cout << "closed code=" << info.code << " reason=" << info.reason << '\n';
    loop.quit();
  });
  client.set_error_callback([&](const oklib::websocket::WebSocketChannelPtr&,
                                oklib::websocket::WebSocketError) {
    done = true;
    failed = true;
    std::cerr << "websocket error\n";
    loop.quit();
  });

  const int rc = client.open(url);
  if (rc != 0) {
    std::cerr << "open failed: " << rc << '\n';
    return 1;
  }
  loop.loop();
  done = true;
  return failed ? 1 : 0;
}
