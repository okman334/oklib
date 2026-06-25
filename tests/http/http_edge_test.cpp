#include <oklib/base/timestamp.h>
#include <oklib/http/http_client.h>
#include <oklib/http/http_parser.h>
#include <oklib/http/http_response.h>
#include <oklib/http/http_server.h>
#include <oklib/net/buffer.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>
#include <oklib/net/tcp_connection.h>
#include <oklib/net/tcp_server.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

oklib::Timestamp receive_time() {
  return oklib::Timestamp::from_unix_time(1);
}

void append(oklib::net::Buffer* buffer, std::string_view data) {
  buffer->append(data);
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
      if (response.find("Connection: Keep-Alive") != std::string::npos ||
          response.find("Connection: close") != std::string::npos) {
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

std::string run_server_case(const std::string& request) {
  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-edge-test");
  server.set_http_callback([](const oklib::http::HttpRequest& req,
                              oklib::http::HttpResponse* response) {
    if (req.path() == "/no-content") {
      response->set_status_code(204);
      response->set_body("must-not-send");
      return;
    }

    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->set_body("ok");
  });
  server.start();

  std::string response;
  std::thread client([&] { response = request_once(request, server.listen_address(), &loop); });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  return response;
}

void test_response_status_codes_without_body_ignore_content_length() {
  {
    oklib::net::Buffer buffer;
    append(&buffer,
           "HTTP/1.1 204 No Content\r\n"
           "Content-Length: 5\r\n"
           "\r\n"
           "NEXT");

    oklib::http::HttpParser parser(oklib::http::HttpParserMode::response);
    require(parser.parse_response(&buffer) == oklib::http::HttpParseStatus::complete,
            "204 response completes without waiting for body");
    require(parser.response().body.empty(), "204 response body stays empty");
    require(buffer.retrieve_all_as_string() == "NEXT", "204 leaves following bytes unread");
  }

  {
    oklib::net::Buffer buffer;
    append(&buffer,
           "HTTP/1.1 304 Not Modified\r\n"
           "Content-Length: 7\r\n"
           "\r\n"
           "PIPE");

    oklib::http::HttpParser parser(oklib::http::HttpParserMode::response);
    require(parser.parse_response(&buffer) == oklib::http::HttpParseStatus::complete,
            "304 response completes without waiting for body");
    require(parser.response().body.empty(), "304 response body stays empty");
    require(buffer.retrieve_all_as_string() == "PIPE", "304 leaves following bytes unread");
  }
}

void test_rejects_invalid_chunk_extensions_and_forbidden_trailers() {
  {
    oklib::net::Buffer buffer;
    append(&buffer,
           "POST / HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Transfer-Encoding: chunked\r\n"
           "\r\n"
           "5;=bad\r\n"
           "hello\r\n"
           "0\r\n\r\n");

    oklib::http::HttpParser parser;
    require(parser.parse_request(&buffer, receive_time()) == oklib::http::HttpParseStatus::error,
            "invalid chunk extension is rejected");
    require(parser.error() == oklib::http::HttpParseError::bad_message_framing,
            "invalid chunk extension reports framing error");
  }

  {
    oklib::net::Buffer buffer;
    append(&buffer,
           "POST / HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Transfer-Encoding: chunked\r\n"
           "\r\n"
           "0\r\n"
           "Content-Length: 1\r\n"
           "\r\n");

    oklib::http::HttpParser parser;
    require(parser.parse_request(&buffer, receive_time()) == oklib::http::HttpParseStatus::error,
            "forbidden trailer field is rejected");
    require(parser.error() == oklib::http::HttpParseError::bad_header,
            "forbidden trailer field reports bad header");
  }
}

void test_server_no_body_status_and_connection_token_list() {
  const auto no_content =
      run_server_case("GET /no-content HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  require(no_content.find("HTTP/1.1 204 No Content") != std::string::npos,
          "server returns 204");
  require(no_content.find("Content-Length: 0") != std::string::npos,
          "server does not advertise body for 204");
  require(no_content.find("must-not-send") == std::string::npos,
          "server does not send body for 204");

  const auto close_token =
      run_server_case("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade, close\r\n\r\n");
  require(close_token.find("Connection: close") != std::string::npos,
          "server recognizes close inside Connection token list");
}

void test_http_client_head_response_does_not_wait_for_body() {
  oklib::net::EventLoop loop;
  oklib::net::TcpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-edge-head-server");
  std::string received;
  server.set_message_callback(
      [&](const oklib::net::TcpConnectionPtr& connection,
          oklib::net::Buffer* buffer,
          oklib::Timestamp) {
        received += buffer->retrieve_all_as_string();
        if (received.find("\r\n\r\n") != std::string::npos) {
          connection->send("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 12\r\n\r\n");
          connection->shutdown();
        }
      });
  server.start();

  oklib::http::HttpClient client(&loop, server.listen_address(), "localhost", "http-edge-head-client");
  std::vector<oklib::http::HttpResponseMessage> responses;
  bool error = false;
  client.set_response_callback([&](oklib::http::HttpResponseMessage response) {
    responses.push_back(std::move(response));
    client.disconnect();
    loop.quit();
  });
  client.set_error_callback([&] {
    error = true;
    loop.quit();
  });

  client.send(oklib::http::HttpClientRequest("HEAD", "/head"));
  loop.run_after(std::chrono::milliseconds(500), [&] { loop.quit(); });
  loop.loop();

  require(received.find("HEAD /head HTTP/1.1") != std::string::npos, "HEAD request sent");
  require(!error, "HEAD response does not trigger client parse error");
  require(responses.size() == 1, "HEAD response callback fires");
  require(responses[0].status_code == 200, "HEAD response status parsed");
  require(responses[0].content_length == 12, "HEAD response content length exposed");
  require(responses[0].body.empty(), "HEAD response body is empty");
}

}  // namespace

int main() {
  test_response_status_codes_without_body_ignore_content_length();
  test_rejects_invalid_chunk_extensions_and_forbidden_trailers();
  test_server_no_body_status_and_connection_token_list();
  test_http_client_head_response_does_not_wait_for_body();
  return 0;
}
