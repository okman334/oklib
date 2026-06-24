#include <oklib/http/http_request.h>
#include <oklib/http/http_response.h>
#include <oklib/http/http_server.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <functional>
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
          (response.find("Connection: Keep-Alive") != std::string::npos ||
           response.find("Connection: close") != std::string::npos)) {
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

std::string run_http_case(const std::string& request,
                          const std::function<bool(const std::string&)>& done = {}) {
  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-test");
  server.set_http_callback([](const oklib::http::HttpRequest& req, oklib::http::HttpResponse* response) {
    if (req.path() == "/") {
      response->set_status_code(oklib::http::HttpStatusCode::ok);
      response->set_body(req.method_string() + " ok");
      response->set_content_type("text/plain");
    } else if (req.path() == "/query") {
      response->set_status_code(oklib::http::HttpStatusCode::ok);
      response->add_header("X-Query", req.query());
      response->set_body(req.header("X-Test"));
    } else if (req.path() == "/peer") {
      response->set_status_code(oklib::http::HttpStatusCode::ok);
      response->add_header("X-Peer-IP", req.peer_ip());
      response->set_body(req.peer_address());
    } else if (req.path() == "/body") {
      response->set_status_code(oklib::http::HttpStatusCode::ok);
      response->add_header("X-Body-Length", std::to_string(req.content_length()));
      response->set_body(req.body());
    } else if (req.path() == "/chunked") {
      response->set_status_code(oklib::http::HttpStatusCode::ok);
      response->add_header("X-Trailer", req.trailers().get("X-Trailer"));
      response->set_body(req.body());
    } else if (req.path() == "/pipeline") {
      response->set_status_code(oklib::http::HttpStatusCode::ok);
      response->set_body(req.query());
    } else {
      response->set_status_code(oklib::http::HttpStatusCode::not_found);
      response->set_body("missing");
    }
  });
  server.start();

  std::string response;
  std::jthread client([&] { response = request_once(request, server.listen_address(), &loop, done); });
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();
  client.join();
  return response;
}

}  // namespace

int main() {
  const auto get_response = run_http_case("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
  require(get_response.find("HTTP/1.1 200 OK") != std::string::npos, "GET returns 200");
  require(get_response.find("GET ok") != std::string::npos, "GET includes body");
  require(get_response.find("Connection: Keep-Alive") != std::string::npos, "HTTP/1.1 defaults keep-alive");

  const auto head_response = run_http_case("HEAD / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  require(head_response.find("HTTP/1.1 200 OK") != std::string::npos, "HEAD returns 200");
  require(head_response.find("GET ok") == std::string::npos, "HEAD omits body");
  require(head_response.find("Connection: close") != std::string::npos, "Connection close respected");

  const auto query_response =
      run_http_case("GET /query?name=oklib HTTP/1.1\r\nHost: localhost\r\nX-Test: value\r\n\r\n");
  require(query_response.find("X-Query: name=oklib") != std::string::npos, "query string parsed");
  require(query_response.find("value") != std::string::npos, "headers parsed");

  const auto peer_response = run_http_case("GET /peer HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  require(peer_response.find("X-Peer-IP: 127.0.0.1") != std::string::npos, "peer ip exposed");
  require(peer_response.find("127.0.0.1:") != std::string::npos, "peer address exposed");

  const auto body_response =
      run_http_case("POST /body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\nConnection: close\r\n\r\n"
                    "hello world");
  require(body_response.find("X-Body-Length: 11") != std::string::npos, "content-length exposed to callback");
  require(body_response.find("hello world") != std::string::npos, "content-length body exposed to callback");

  const auto chunked_response =
      run_http_case("POST /chunked HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n"
                    "Trailer: X-Trailer\r\nConnection: close\r\n\r\n"
                    "5\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: done\r\n\r\n");
  require(chunked_response.find("X-Trailer: done") != std::string::npos, "chunked trailers exposed");
  require(chunked_response.find("hello world") != std::string::npos, "chunked body exposed to callback");

  const auto pipeline_response =
      run_http_case("GET /pipeline?one HTTP/1.1\r\nHost: localhost\r\n\r\n"
                    "GET /pipeline?two HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
                    [](const std::string& response) {
                      return response.find("one") != std::string::npos &&
                             response.find("two") != std::string::npos;
                    });
  require(pipeline_response.find("one") != std::string::npos, "first pipelined request responded");
  require(pipeline_response.find("two") != std::string::npos, "second pipelined request responded");

  const auto not_found = run_http_case("GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  require(not_found.find("HTTP/1.1 404 Not Found") != std::string::npos, "unknown path returns 404");

  const auto bad_request = run_http_case("BAD REQUEST\r\n\r\n");
  require(bad_request.find("HTTP/1.1 400 Bad Request") != std::string::npos, "malformed request returns 400");

  const auto missing_host = run_http_case("GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
  require(missing_host.find("HTTP/1.1 400 Bad Request") != std::string::npos, "HTTP/1.1 requires Host");

  const auto ambiguous_framing =
      run_http_case("POST /body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n"
                    "Content-Length: 5\r\nConnection: close\r\n\r\n0\r\n\r\n");
  require(ambiguous_framing.find("HTTP/1.1 400 Bad Request") != std::string::npos,
          "ambiguous message framing rejected");

  return 0;
}
