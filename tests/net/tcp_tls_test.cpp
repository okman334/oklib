#include <oklib/net/buffer.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>
#include <oklib/net/tcp_client.h>
#include <oklib/net/tcp_connection.h>
#include <oklib/net/tcp_server.h>
#include <oklib/net/tls_options.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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

struct TestCertificate {
  std::filesystem::path directory;
  std::filesystem::path cert_file;
  std::filesystem::path key_file;
};

TestCertificate make_test_certificate() {
  char dir_template[] = "/tmp/oklib-net-tls-XXXXXX";
  char* dir = ::mkdtemp(dir_template);
  require(dir != nullptr, "temporary certificate directory created");

  TestCertificate certificate;
  certificate.directory = dir;
  certificate.cert_file = certificate.directory / "server.crt";
  certificate.key_file = certificate.directory / "server.key";

  const std::string command =
      "openssl req -x509 -newkey rsa:2048 -nodes -keyout " + certificate.key_file.string() +
      " -out " + certificate.cert_file.string() +
      " -subj /CN=localhost -days 1 >/dev/null 2>&1";
  require(std::system(command.c_str()) == 0, "self-signed certificate generated");
  return certificate;
}

oklib::net::TlsServerOptions server_tls_options(const TestCertificate& certificate) {
  oklib::net::TlsServerOptions options;
  options.enabled = true;
  options.cert_file = certificate.cert_file.string();
  options.key_file = certificate.key_file.string();
  return options;
}

oklib::net::TlsClientOptions client_tls_options() {
  oklib::net::TlsClientOptions options;
  options.enabled = true;
  options.verify_peer = false;
  options.server_name = "localhost";
  return options;
}

}  // namespace

int main() {
  using namespace std::chrono_literals;

  {
    const auto certificate = make_test_certificate();
    oklib::net::EventLoop loop;
    oklib::net::TcpServer server(&loop, oklib::net::InetAddress::loopback(0), "tls-echo");
    server.set_tls_options(server_tls_options(certificate));
    server.set_message_callback([](const oklib::net::TcpConnectionPtr& conn,
                                   oklib::net::Buffer* buffer,
                                   oklib::Timestamp) {
      require(conn->tls_enabled(), "server connection reports TLS enabled");
      require(conn->tls_established(), "server message callback runs after TLS handshake");
      conn->send(buffer->retrieve_all_as_string());
    });
    server.start();

    std::atomic<bool> echoed{false};
    oklib::net::TcpClient client(&loop, oklib::net::InetAddress::loopback(server.port()), "tls-client");
    client.set_tls_options(client_tls_options());
    client.set_connection_callback([](const oklib::net::TcpConnectionPtr& conn) {
      if (conn->connected()) {
        require(conn->tls_enabled(), "client connection reports TLS enabled");
        conn->send("hello-before-handshake-completes");
      }
    });
    client.set_message_callback([&](const oklib::net::TcpConnectionPtr& conn,
                                    oklib::net::Buffer* buffer,
                                    oklib::Timestamp) {
      require(conn->tls_established(), "client message callback runs after TLS handshake");
      echoed = buffer->retrieve_all_as_string() == "hello-before-handshake-completes";
      client.disconnect();
      loop.quit();
    });
    client.connect();
    loop.run_after(3s, [&] { loop.quit(); });
    loop.loop();
    require(echoed.load(), "TLS TcpServer/TcpClient echo preserves queued send");
  }

  {
    const auto certificate = make_test_certificate();
    oklib::net::EventLoop loop;
    oklib::net::TcpServer server(&loop, oklib::net::InetAddress::loopback(0), "tls-threaded");
    server.set_thread_num(2);
    server.set_tls_options(server_tls_options(certificate));
    server.set_message_callback([](const oklib::net::TcpConnectionPtr& conn,
                                   oklib::net::Buffer* buffer,
                                   oklib::Timestamp) {
      buffer->retrieve_all();
      std::thread([conn] { conn->send("from-worker"); }).detach();
    });
    server.start();

    std::atomic<bool> received{false};
    oklib::net::TcpClient client(&loop, oklib::net::InetAddress::loopback(server.port()), "tls-threaded-client");
    client.set_tls_options(client_tls_options());
    client.set_connection_callback([](const oklib::net::TcpConnectionPtr& conn) {
      if (conn->connected()) {
        conn->send("trigger");
      }
    });
    client.set_message_callback([&](const oklib::net::TcpConnectionPtr&,
                                    oklib::net::Buffer* buffer,
                                    oklib::Timestamp) {
      received = buffer->retrieve_all_as_string() == "from-worker";
      client.disconnect();
      loop.quit();
    });
    client.connect();
    loop.run_after(3s, [&] { loop.quit(); });
    loop.loop();
    require(received.load(), "TLS TcpConnection::send remains cross-thread safe");
  }

  return 0;
}
