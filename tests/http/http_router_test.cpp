#include <oklib/http/http_router.h>
#include <oklib/http/http_server.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
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

std::string request_once(const std::string& request,
                         const oklib::net::InetAddress& address,
                         oklib::net::EventLoop* loop) {
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
      if (response.find("Connection: close") != std::string::npos) {
        break;
      }
      if (response.find("\r\n\r\n") != std::string::npos &&
          response.find("Content-Length: 0") != std::string::npos) {
        break;
      }
      if (response.find("get-ok") != std::string::npos ||
          response.find("body=hello") != std::string::npos ||
          response.find("post=hello") != std::string::npos ||
          response.find("custom missing") != std::string::npos) {
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

std::string run_router_case(const std::string& request) {
  oklib::net::EventLoop loop;
  oklib::http::HttpRouter router;

  router.get("/resource", [](const oklib::http::HttpRequest&,
                             oklib::http::HttpResponseWriter writer) {
    auto response = writer.make_response();
    response.set_status_code(200);
    response.set_content_type("text/plain");
    response.set_body("get-ok");
    require(writer.send(std::move(response)), "GET route sends response");
  });

  router.post_streaming("/resource",
                        [](oklib::http::HttpRequest,
                           oklib::http::HttpRequestBodyStream body,
                           oklib::http::HttpResponseWriter writer) {
                          auto payload = std::make_shared<std::string>();
                          body.set_data_callback([payload](std::string_view chunk) {
                            payload->append(chunk);
                          });
                          body.set_complete_callback([payload, writer]() mutable {
                            auto response = writer.make_response();
                            response.set_status_code(200);
                            response.set_content_type("text/plain");
                            response.set_body("post=" + *payload);
                            require(writer.send(std::move(response)), "POST route sends response");
                          });
                        });

  router.post("/buffered", [](const oklib::http::HttpRequest& request,
                              oklib::http::HttpResponseWriter writer) {
    auto response = writer.make_response();
    response.set_status_code(200);
    response.set_content_type("text/plain");
    response.set_body("body=" + request.body());
    require(writer.send(std::move(response)), "buffered POST route sends response");
  });

  router.set_not_found_handler([](const oklib::http::HttpRequest&,
                                  oklib::http::HttpResponseWriter writer) {
    auto response = writer.make_response();
    response.set_status_code(404);
    response.set_content_type("text/plain");
    response.set_body("custom missing");
    writer.send(std::move(response));
  });

  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-router-test");
  server.set_router(router);
  server.start();

  std::string response;
  std::jthread client([&] { response = request_once(request, server.listen_address(), &loop); });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  return response;
}

void test_router_dispatches_by_method_and_path() {
  const std::string get_response = run_router_case(
      "GET /resource HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  require(get_response.find("HTTP/1.1 200 OK") != std::string::npos, "GET route returns 200");
  require(get_response.find("get-ok") != std::string::npos, "GET route body returned");

  const std::string post_response = run_router_case(
      "POST /resource HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n"
      "Connection: close\r\n\r\nhello");
  require(post_response.find("HTTP/1.1 200 OK") != std::string::npos, "POST route returns 200");
  require(post_response.find("post=hello") != std::string::npos, "POST route body returned");
}

void test_router_buffers_body_for_plain_post_handler() {
  const std::string response = run_router_case(
      "POST /buffered HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n"
      "Connection: close\r\n\r\nhello");
  require(response.find("HTTP/1.1 200 OK") != std::string::npos,
          "buffered POST route returns 200");
  require(response.find("body=hello") != std::string::npos,
          "buffered POST route receives request body");
}

void test_router_returns_405_with_allow_for_known_path_wrong_method() {
  const std::string response = run_router_case(
      "DELETE /resource HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  require(response.find("HTTP/1.1 405 Method Not Allowed") != std::string::npos,
          "wrong method returns 405");
  require(response.find("Allow: GET, POST") != std::string::npos,
          "405 response includes Allow header");
}

void test_router_returns_404_for_missing_path() {
  const std::string response = run_router_case(
      "GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  require(response.find("HTTP/1.1 404 Not Found") != std::string::npos,
          "missing path returns 404");
  require(response.find("custom missing") != std::string::npos,
          "custom not found handler is used");
}

}  // namespace

int main() {
  test_router_dispatches_by_method_and_path();
  test_router_buffers_body_for_plain_post_handler();
  test_router_returns_405_with_allow_for_known_path_wrong_method();
  test_router_returns_404_for_missing_path();
  return 0;
}
