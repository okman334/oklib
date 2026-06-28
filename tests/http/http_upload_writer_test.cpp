#include <oklib/http/http_request_body_stream.h>
#include <oklib/http/http_request.h>
#include <oklib/http/http_response.h>
#include <oklib/http/http_response_writer.h>
#include <oklib/http/http_server.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

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

int connect_socket(const oklib::net::InetAddress& address) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0, "socket succeeds");
  require(::connect(fd, address.sockaddr_ptr(), address.length()) == 0, "connect succeeds");
  return fd;
}

void write_all(int fd, const std::string& data) {
  std::size_t written = 0;
  while (written < data.size()) {
    const auto n = ::write(fd, data.data() + written, data.size() - written);
    require(n > 0, "write succeeds");
    written += static_cast<std::size_t>(n);
  }
}

std::string read_response_until_close(int fd) {
  std::string response;
  char buffer[4096];
  for (;;) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n > 0) {
      response.append(buffer, static_cast<std::size_t>(n));
      continue;
    }
    break;
  }
  return response;
}

void test_stream_pause_resume_and_cancel() {
  using namespace std::chrono_literals;

  oklib::net::EventLoop loop;
  std::atomic<bool> saw_body{false};
  std::atomic<bool> checked_paused{false};
  std::atomic<bool> canceled{false};

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "http-stream-control-test");
  server.set_streaming_http_callback(
      [&](oklib::http::HttpRequest request,
          oklib::http::HttpRequestBodyStream body,
          oklib::http::HttpResponseWriter writer) mutable {
        if (request.path() == "/cancel") {
          body.set_cancel_callback([&] {
            canceled = true;
            loop.quit();
          });
          return;
        }

        body.pause_reading();
        loop.run_after(40ms, [&, body] {
          require(!saw_body.load(), "paused body stream does not deliver buffered body");
          checked_paused = true;
          body.resume_reading();
        });
        body.set_data_callback([&](std::string_view) {
          saw_body = true;
        });
        body.set_complete_callback([writer = std::move(writer)]() mutable {
          auto response = writer.make_response();
          response.set_status_code(200);
          response.set_body("ok");
          writer.send(std::move(response));
        });
      });
  server.start();

  std::string response;
  std::thread client([&] {
    const int fd = connect_socket(server.listen_address());
    const std::string body = "abcdef";
    write_all(fd,
              std::string("POST /pause HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Length: ") +
                  std::to_string(body.size()) + "\r\n"
                  "Connection: close\r\n\r\n" +
                  body);
    response = read_response_until_close(fd);
    ::close(fd);

    const int cancel_fd = connect_socket(server.listen_address());
    write_all(cancel_fd,
              "POST /cancel HTTP/1.1\r\n"
              "Host: localhost\r\n"
              "Content-Length: 64\r\n"
              "Connection: close\r\n\r\n"
              "partial");
    ::close(cancel_fd);
  });
  loop.run_after(2s, [&] { loop.quit(); });
  loop.loop();
  client.join();

  require(checked_paused.load(), "pause checkpoint ran");
  require(saw_body.load(), "body was delivered after resume");
  require(response.find("HTTP/1.1 200 OK") != std::string::npos, "response sent after body completion");
  require(canceled.load(), "stream cancel callback runs on client disconnect");
}

}  // namespace

int main() {
  test_stream_pause_resume_and_cancel();
  return 0;
}
