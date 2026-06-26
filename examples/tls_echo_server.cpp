#include <cstdlib>
#include <iostream>
#include <string>

#include "oklib/base/logging.h"
#include "oklib/net/buffer.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"
#include "oklib/net/tcp_connection.h"
#include "oklib/net/tcp_server.h"
#include "oklib/net/tls_options.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0] << " <cert.pem> <key.pem> [port=9443] [io_threads=0]\n";
    return EXIT_FAILURE;
  }

  const uint16_t port = argc > 3 ? static_cast<uint16_t>(std::stoi(argv[3])) : 9443;
  const int threads = argc > 4 ? std::stoi(argv[4]) : 0;

  oklib::net::EventLoop loop;
  oklib::net::TcpServer server(&loop, oklib::net::InetAddress::any(port), "tls-echo");
  oklib::net::TlsServerOptions tls;
  tls.enabled = true;
  tls.cert_file = argv[1];
  tls.key_file = argv[2];
  server.set_tls_options(std::move(tls));
  server.set_thread_num(threads);
  server.set_connection_callback([](const oklib::net::TcpConnectionPtr& connection) {
    const char* state = connection->connected() ? "connected" : "disconnected";
    OKLIB_LOG_INFO << "TLS echo " << state << ' ' << connection->peer_address().to_ip_port();
    std::cout << state << ' ' << connection->peer_address().to_ip_port() << '\n';
  });
  server.set_message_callback([](const oklib::net::TcpConnectionPtr& connection,
                                 oklib::net::Buffer* buffer,
                                 oklib::Timestamp) {
    OKLIB_LOG_INFO << "TLS echo " << buffer->readable_bytes() << " byte(s)";
    connection->send(buffer->retrieve_all_as_string());
  });

  server.start();
  OKLIB_LOG_INFO << "TLS echo server listening on " << server.listen_address().to_ip_port();
  std::cout << "oklib TLS echo server listening on " << server.listen_address().to_ip_port() << '\n';
  loop.loop();
  return EXIT_SUCCESS;
}
