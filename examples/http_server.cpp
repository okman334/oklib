#include <csignal>
#include <iostream>

#include "oklib/http/http_request.h"
#include "oklib/http/http_response.h"
#include "oklib/http/http_server.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"

int main(int argc, char** argv) {
  const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 8080;
  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::any(port), "example-http");
  server.set_http_callback([](const oklib::http::HttpRequest& request,
                              oklib::http::HttpResponse* response) {
    if (request.path() == "/") {
      response->set_status_code(oklib::http::HttpStatusCode::ok);
      response->set_content_type("text/plain");
      response->set_body("oklib http server\n");
      return;
    }
    response->set_status_code(oklib::http::HttpStatusCode::not_found);
    response->set_content_type("text/plain");
    response->set_body("not found\n");
  });
  server.start();
  std::cout << "oklib HTTP server listening on " << server.listen_address().to_ip_port() << '\n';
  loop.loop();
  return 0;
}
