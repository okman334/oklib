#include <oklib/net/buffer.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>
#include <oklib/net/tcp_client.h>
#include <oklib/net/tcp_connection.h>
#include <oklib/net/tcp_server.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  using namespace std::chrono_literals;

  {
    oklib::net::EventLoop loop;
    oklib::net::TcpServer server(&loop, oklib::net::InetAddress::loopback(0), "echo");
    server.set_message_callback([](const oklib::net::TcpConnectionPtr& conn,
                                   oklib::net::Buffer* buffer,
                                   oklib::Timestamp) {
      conn->send(buffer->retrieve_all_as_string());
    });
    server.start();

    std::atomic<bool> echoed{false};
    oklib::net::TcpClient client(&loop, oklib::net::InetAddress::loopback(server.port()), "client");
    client.set_connection_callback([](const oklib::net::TcpConnectionPtr& conn) {
      if (conn->connected()) {
        conn->send("ping");
      }
    });
    client.set_message_callback([&](const oklib::net::TcpConnectionPtr&,
                                    oklib::net::Buffer* buffer,
                                    oklib::Timestamp) {
      echoed = buffer->retrieve_all_as_string() == "ping";
      loop.quit();
    });
    client.connect();
    loop.run_after(2s, [&] { loop.quit(); });
    loop.loop();
    require(echoed.load(), "single-thread TcpServer/TcpClient echo");
  }

  {
    oklib::net::EventLoop loop;
    oklib::net::TcpServer server(&loop, oklib::net::InetAddress::loopback(0), "threaded");
    server.set_thread_num(2);
    server.set_connection_callback([](const oklib::net::TcpConnectionPtr& conn) {
      if (conn->connected()) {
        std::thread([conn] { conn->send("hello-from-worker"); }).detach();
      }
    });
    server.start();

    std::atomic<bool> received{false};
    oklib::net::TcpClient client(&loop, oklib::net::InetAddress::loopback(server.port()), "threaded-client");
    client.set_message_callback([&](const oklib::net::TcpConnectionPtr&,
                                    oklib::net::Buffer* buffer,
                                    oklib::Timestamp) {
      received = buffer->retrieve_all_as_string() == "hello-from-worker";
      loop.quit();
    });
    client.connect();
    loop.run_after(2s, [&] { loop.quit(); });
    loop.loop();
    require(received.load(), "TcpConnection::send is safe from another thread");
  }

  return 0;
}
