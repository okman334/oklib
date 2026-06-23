#include <cstdlib>
#include <iostream>
#include <string>

#include "oklib/base/logging.h"
#include "oklib/net/buffer.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"
#include "oklib/net/tcp_connection.h"
#include "oklib/net/tcp_server.h"

int main(int argc, char** argv) {
  const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 9000;
  const int threads = argc > 2 ? std::stoi(argv[2]) : 0;

  oklib::net::EventLoop loop;
  oklib::net::TcpServer server(&loop, oklib::net::InetAddress::any(port), "tcp-echo");
  server.set_thread_num(threads);
  server.set_connection_callback([](const oklib::net::TcpConnectionPtr& connection) {
    const char* state = connection->connected() ? "connected" : "disconnected";
    OKLIB_LOG_INFO << state << ' ' << connection->peer_address().to_ip_port();
    std::cout << state << ' ' << connection->peer_address().to_ip_port() << '\n';
  });
  server.set_message_callback([](const oklib::net::TcpConnectionPtr& connection,
                                 oklib::net::Buffer* buffer,
                                 oklib::Timestamp) {
    const auto bytes = buffer->readable_bytes();
    auto message = buffer->retrieve_all_as_string();
    OKLIB_LOG_INFO << "echo " << bytes << " byte(s) to "
                   << connection->peer_address().to_ip_port();
    connection->send(message);
  });

  server.start();
  OKLIB_LOG_INFO << "tcp echo server listening on "
                 << server.listen_address().to_ip_port()
                 << " with " << threads << " I/O thread(s)";
  std::cout << "oklib TCP echo server listening on "
            << server.listen_address().to_ip_port()
            << " with " << threads << " I/O thread(s)\n";
  loop.loop();
  return EXIT_SUCCESS;
}
