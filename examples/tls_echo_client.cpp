#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "oklib/net/buffer.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"
#include "oklib/net/tcp_client.h"
#include "oklib/net/tcp_connection.h"
#include "oklib/net/tls_options.h"

int main(int argc, char** argv) {
  const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
  const uint16_t port = argc > 2 ? static_cast<uint16_t>(std::stoi(argv[2])) : 9443;
  const std::string message = argc > 3 ? argv[3] : "hello over tls\n";

  oklib::net::EventLoop loop;
  oklib::net::TcpClient client(&loop, oklib::net::InetAddress(host, port), "tls-echo-client");
  oklib::net::TlsClientOptions tls;
  tls.enabled = true;
  tls.verify_peer = false;
  tls.server_name = host;
  client.set_tls_options(std::move(tls));
  client.set_connection_callback([&](const oklib::net::TcpConnectionPtr& connection) {
    if (connection->connected()) {
      connection->send(message);
    }
  });
  client.set_message_callback([&](const oklib::net::TcpConnectionPtr& connection,
                                  oklib::net::Buffer* buffer,
                                  oklib::Timestamp) {
    std::cout << buffer->retrieve_all_as_string();
    connection->shutdown();
    loop.quit();
  });

  client.connect();
  loop.run_after(std::chrono::seconds(5), [&] {
    std::cerr << "timed out waiting for TLS echo response\n";
    loop.quit();
  });
  loop.loop();
  return EXIT_SUCCESS;
}
