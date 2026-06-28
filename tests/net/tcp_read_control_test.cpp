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
#include <memory>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

void test_pause_and_resume_reading() {
  using namespace std::chrono_literals;

  oklib::net::EventLoop loop;
  oklib::net::TcpConnectionPtr server_connection;
  std::atomic<int> messages{0};

  oklib::net::TcpServer server(&loop,
                               oklib::net::InetAddress::loopback(0),
                               "tcp-read-control-test");
  server.set_connection_callback([&](const oklib::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
      server_connection = conn;
      conn->pause_reading();
      require(conn->reading_paused(), "connection reports paused reads");
    }
  });
  server.set_message_callback([&](const oklib::net::TcpConnectionPtr&,
                                  oklib::net::Buffer* buffer,
                                  oklib::Timestamp) {
    ++messages;
    buffer->retrieve_all();
  });
  server.start();

  oklib::net::TcpClient client(&loop,
                               server.listen_address(),
                               "tcp-read-control-client");
  client.set_connection_callback([&](const oklib::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
      conn->send("before-resume");
      loop.run_after(80ms, [&] {
        require(messages.load() == 0, "paused connection does not deliver reads");
        require(server_connection != nullptr, "server connection exists");
        server_connection->resume_reading();
      });
      loop.run_after(180ms, [&] {
        require(!server_connection->reading_paused(), "connection reports resumed reads");
        require(messages.load() == 1, "resumed connection delivers pending read");
        client.disconnect();
        loop.quit();
      });
    }
  });
  client.connect();
  loop.run_after(2s, [&] { loop.quit(); });
  loop.loop();
}

void test_pause_and_resume_from_another_thread() {
  using namespace std::chrono_literals;

  oklib::net::EventLoop loop;
  oklib::net::TcpConnectionPtr server_connection;
  std::atomic<int> messages{0};

  oklib::net::TcpServer server(&loop,
                               oklib::net::InetAddress::loopback(0),
                               "tcp-read-control-thread-test");
  server.set_connection_callback([&](const oklib::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
      server_connection = conn;
      std::thread([conn] { conn->pause_reading(); }).detach();
    }
  });
  server.set_message_callback([&](const oklib::net::TcpConnectionPtr&,
                                  oklib::net::Buffer* buffer,
                                  oklib::Timestamp) {
    ++messages;
    buffer->retrieve_all();
  });
  server.start();

  oklib::net::TcpClient client(&loop,
                               server.listen_address(),
                               "tcp-read-control-thread-client");
  client.set_connection_callback([&](const oklib::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
      conn->send("threaded-pause");
      loop.run_after(80ms, [&] {
        require(messages.load() == 0, "cross-thread pause suppresses reads");
        require(server_connection != nullptr, "server connection exists");
        std::thread([conn = server_connection] { conn->resume_reading(); }).detach();
      });
      loop.run_after(180ms, [&] {
        require(messages.load() == 1, "cross-thread resume restores reads");
        client.disconnect();
        loop.quit();
      });
    }
  });
  client.connect();
  loop.run_after(2s, [&] { loop.quit(); });
  loop.loop();
}

}  // namespace

int main() {
  test_pause_and_resume_reading();
  test_pause_and_resume_from_another_thread();
  return 0;
}
