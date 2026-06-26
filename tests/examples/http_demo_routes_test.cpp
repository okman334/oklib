#include <http_demo_routes.h>

#include <oklib/base/thread_pool.h>
#include <oklib/http/http_server.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

std::string request_once(const std::string& request,
                         const oklib::net::InetAddress& address,
                         oklib::net::EventLoop* loop,
                         const std::function<bool(const std::string&)>& done = {}) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0, "socket succeeds");
  require(::connect(fd, address.sockaddr_ptr(), address.length()) == 0, "connect succeeds");
  require(::write(fd, request.data(), request.size()) == static_cast<ssize_t>(request.size()),
          "write request succeeds");

  std::string response;
  char buffer[4096];
  for (;;) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n > 0) {
      response.append(buffer, static_cast<std::size_t>(n));
      if (done && done(response)) {
        break;
      }
      if (!done && response.find("\r\n\r\n") != std::string::npos &&
          response.find("\"bytes\"") != std::string::npos) {
        break;
      }
    } else {
      break;
    }
  }
  ::close(fd);
  loop->queue_in_loop([loop] { loop->quit(); });
  return response;
}

void test_ping_route_returns_pong() {
  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("http-demo-routes-ping-test-workers");
  workers.start(1);

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-demo-routes-ping-test");
  oklib::examples::install_http_demo_routes(server, workers);
  server.start();

  std::string response;
  std::thread client([&] {
    response = request_once(
        "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        server.listen_address(),
        &loop,
        [](const std::string& value) { return value.find("pong\n") != std::string::npos; });
  });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  workers.stop();

  require(response.find("HTTP/1.1 200 OK") != std::string::npos,
          "ping route returns 200");
  require(response.find("pong\n") != std::string::npos, "ping route returns pong body");
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  require(file.is_open(), "uploaded file opens");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void test_upload_file_route_saves_raw_jpeg_body() {
  const std::string file_name = "oklib-demo-routes-upload-test.jpg";
  const std::filesystem::path upload_path = std::filesystem::current_path() / "uploads" / file_name;
  std::filesystem::remove(upload_path);

  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("http-demo-routes-test-workers");
  workers.start(1);

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-demo-routes-upload-test");
  oklib::examples::install_http_demo_routes(server, workers);
  server.start();

  const std::string jpg_body = "\xff\xd8\xff\xe0oklib-test-jpg\xff\xd9";
  const std::string request =
      "POST /upload-file?name=" + file_name + " HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: " + std::to_string(jpg_body.size()) + "\r\n"
      "Connection: close\r\n\r\n" +
      jpg_body;

  std::string response;
  std::thread client([&] {
    response = request_once(request, server.listen_address(), &loop);
  });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  workers.stop();

  require(response.find("HTTP/1.1 201 Created") != std::string::npos,
          "upload route returns 201");
  require(response.find("\"file\":\"" + file_name + "\"") != std::string::npos,
          "upload response includes file name");
  require(response.find("\"bytes\":" + std::to_string(jpg_body.size())) != std::string::npos,
          "upload response includes byte count");
  require(read_file(upload_path) == jpg_body, "uploaded jpg body is saved byte-for-byte");

  std::filesystem::remove(upload_path);
}

}  // namespace

int main() {
  test_ping_route_returns_pong();
  test_upload_file_route_saves_raw_jpeg_body();
  return 0;
}
