#include <oklib/net/buffer.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>
#include <oklib/net/tcp_client.h>
#include <oklib/net/tcp_connection.h>
#include <oklib/net/tcp_server.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
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

bool ipv6_dual_stack_available() {
  const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  int off = 0;
  const bool option_ok =
      ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) == 0;
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_any;
  address.sin6_port = 0;
  const bool bind_ok =
      option_ok && ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  ::close(fd);
  return bind_ok;
}

}  // namespace

int main() {
  using namespace std::chrono_literals;

  {
    auto send_owned_string =
        static_cast<void (oklib::net::TcpConnection::*)(std::string&&)>(&oklib::net::TcpConnection::send);
    (void)send_owned_string;
  }

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

  if (ipv6_loopback_available()) {
    oklib::net::EventLoop loop;
    oklib::net::TcpServer server(&loop,
                                 oklib::net::InetAddress::loopback_ipv6(0),
                                 "ipv6-echo");
    server.set_message_callback([](const oklib::net::TcpConnectionPtr& conn,
                                   oklib::net::Buffer* buffer,
                                   oklib::Timestamp) {
      conn->send(buffer->retrieve_all_as_string());
    });
    server.start();

    std::atomic<bool> echoed{false};
    oklib::net::TcpClient client(&loop,
                                 oklib::net::InetAddress::loopback_ipv6(server.port()),
                                 "ipv6-client");
    client.set_connection_callback([](const oklib::net::TcpConnectionPtr& conn) {
      if (conn->connected()) {
        conn->send("ipv6-ping");
      }
    });
    client.set_message_callback([&](const oklib::net::TcpConnectionPtr&,
                                    oklib::net::Buffer* buffer,
                                    oklib::Timestamp) {
      echoed = buffer->retrieve_all_as_string() == "ipv6-ping";
      loop.quit();
    });
    client.connect();
    loop.run_after(2s, [&] { loop.quit(); });
    loop.loop();
    require(echoed.load(), "IPv6 loopback TcpServer/TcpClient echo");
  } else {
    std::cout << "skip IPv6 loopback TCP test: ::1 unavailable\n";
  }

  if (ipv6_dual_stack_available()) {
    oklib::net::EventLoop loop;
    oklib::net::TcpServer::ListenOptions listen_options;
    listen_options.ipv6_only = false;
    oklib::net::TcpServer server(&loop,
                                 oklib::net::InetAddress::any_ipv6(0),
                                 "dual-stack-echo",
                                 listen_options);
    server.set_message_callback([](const oklib::net::TcpConnectionPtr& conn,
                                   oklib::net::Buffer* buffer,
                                   oklib::Timestamp) {
      conn->send(buffer->retrieve_all_as_string());
    });
    server.start();

    std::atomic<int> echoed{0};
    oklib::net::TcpClient ipv6_client(&loop,
                                      oklib::net::InetAddress::loopback_ipv6(server.port()),
                                      "dual-stack-ipv6-client");
    oklib::net::TcpClient ipv4_client(&loop,
                                      oklib::net::InetAddress::loopback(server.port()),
                                      "dual-stack-ipv4-client");
    auto on_message = [&](const oklib::net::TcpConnectionPtr&,
                          oklib::net::Buffer* buffer,
                          oklib::Timestamp) {
      const auto body = buffer->retrieve_all_as_string();
      if (body == "v4" || body == "v6") {
        if (++echoed == 2) {
          loop.quit();
        }
      }
    };
    ipv6_client.set_connection_callback([](const oklib::net::TcpConnectionPtr& conn) {
      if (conn->connected()) {
        conn->send("v6");
      }
    });
    ipv4_client.set_connection_callback([](const oklib::net::TcpConnectionPtr& conn) {
      if (conn->connected()) {
        conn->send("v4");
      }
    });
    ipv6_client.set_message_callback(on_message);
    ipv4_client.set_message_callback(on_message);
    ipv6_client.connect();
    ipv4_client.connect();
    loop.run_after(2s, [&] { loop.quit(); });
    loop.loop();
    require(echoed.load() == 2, "dual-stack IPv6 listener accepts IPv4 and IPv6 clients");
  } else {
    std::cout << "skip dual-stack TCP test: IPv6 dual-stack unavailable\n";
  }

  if (ipv6_loopback_available()) {
    oklib::net::EventLoop loop;
    oklib::net::TcpServer server(&loop,
                                 oklib::net::InetAddress::loopback_ipv6(0),
                                 "multi-address-client-server");
    server.set_connection_callback([](const oklib::net::TcpConnectionPtr& conn) {
      if (conn->connected()) {
        conn->send("fallback-ok");
      }
    });
    server.start();

    std::atomic<bool> received{false};
    std::vector<oklib::net::InetAddress> candidates;
    candidates.push_back(oklib::net::InetAddress::loopback_ipv6(9));
    candidates.push_back(oklib::net::InetAddress::loopback_ipv6(server.port()));
    oklib::net::TcpClient client(&loop, std::move(candidates), "multi-address-client");
    client.set_message_callback([&](const oklib::net::TcpConnectionPtr&,
                                    oklib::net::Buffer* buffer,
                                    oklib::Timestamp) {
      received = buffer->retrieve_all_as_string() == "fallback-ok";
      loop.quit();
    });
    client.connect();
    loop.run_after(3s, [&] { loop.quit(); });
    loop.loop();
    require(received.load(), "TcpClient tries next address candidate after connection failure");
  }

  return 0;
}
