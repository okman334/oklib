#include <oklib/http/http_client.h>
#include <oklib/http/http_response.h>
#include <oklib/http/http_response_writer.h>
#include <oklib/http/http_server.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

std::string make_body(std::string prefix, const oklib::http::HttpRequest& request) {
  return prefix + " " + request.path() + "?" + request.query();
}

void run_buffered_client_case() {
  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-client-buffered-server");
  server.set_http_callback([](const oklib::http::HttpRequest& request,
                              oklib::http::HttpResponse* response) {
    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->add_header("X-Echo-Method", request.method_string());
    response->set_content_type("text/plain");
    response->set_body(make_body("buffered", request));
  });
  server.start();

  oklib::http::HttpClient client(&loop, server.listen_address(), "localhost", "http-client-buffered");
  std::vector<oklib::http::HttpResponseMessage> responses;
  client.set_response_callback([&](oklib::http::HttpResponseMessage response) {
    responses.push_back(std::move(response));
    client.disconnect();
    loop.quit();
  });

  oklib::http::HttpClientRequest request("GET", "/hello?name=oklib");
  request.add_header("X-Test", "client");
  client.send(std::move(request));
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();

  require(responses.size() == 1, "buffered client receives one response");
  require(responses[0].status_code == 200, "buffered client status code parsed");
  require(responses[0].headers.get("X-Echo-Method") == "GET", "buffered client response headers parsed");
  require(responses[0].body == "buffered /hello?name=oklib", "buffered client response body parsed");
}

void run_keep_alive_reuse_case() {
  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-client-keepalive-server");
  server.set_http_callback([](const oklib::http::HttpRequest& request,
                              oklib::http::HttpResponse* response) {
    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->add_header("X-Peer-Port", std::to_string(request.peer_port()));
    response->set_body(request.query());
  });
  server.start();

  oklib::http::HttpClient client(&loop, server.listen_address(), "localhost", "http-client-keepalive");
  std::vector<oklib::http::HttpResponseMessage> responses;
  client.set_response_callback([&](oklib::http::HttpResponseMessage response) {
    responses.push_back(std::move(response));
    if (responses.size() == 1) {
      client.send(oklib::http::HttpClientRequest("GET", "/reuse?second"));
    } else {
      client.disconnect();
      loop.quit();
    }
  });

  client.send(oklib::http::HttpClientRequest("GET", "/reuse?first"));
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();

  require(responses.size() == 2, "keep-alive client receives two responses");
  require(responses[0].body == "first", "first keep-alive response parsed");
  require(responses[1].body == "second", "second keep-alive response parsed");
  require(responses[0].headers.get("X-Peer-Port") == responses[1].headers.get("X-Peer-Port"),
          "keep-alive client reuses same TCP connection");
}

void run_queued_before_connect_case() {
  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-client-queued-server");
  server.set_http_callback([](const oklib::http::HttpRequest& request,
                              oklib::http::HttpResponse* response) {
    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->add_header("X-Peer-Port", std::to_string(request.peer_port()));
    response->set_body(request.query());
  });
  server.start();

  oklib::http::HttpClient client(&loop, server.listen_address(), "localhost", "http-client-queued");
  std::vector<oklib::http::HttpResponseMessage> responses;
  client.set_response_callback([&](oklib::http::HttpResponseMessage response) {
    responses.push_back(std::move(response));
    if (responses.size() == 2) {
      client.disconnect();
      loop.quit();
    }
  });

  client.send(oklib::http::HttpClientRequest("GET", "/queued?one"));
  client.send(oklib::http::HttpClientRequest("GET", "/queued?two"));
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();

  require(responses.size() == 2, "queued-before-connect client receives two responses");
  require(responses[0].body == "one", "first queued response parsed");
  require(responses[1].body == "two", "second queued response parsed");
  require(responses[0].headers.get("X-Peer-Port") == responses[1].headers.get("X-Peer-Port"),
          "queued-before-connect requests share one TCP connection");
}

void run_server_close_reconnect_case() {
  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-client-reconnect-server");
  server.set_http_callback([](const oklib::http::HttpRequest& request,
                              oklib::http::HttpResponse* response) {
    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->add_header("X-Peer-Port", std::to_string(request.peer_port()));
    response->set_body(request.path());
    if (request.path() == "/close") {
      response->set_close_connection(true);
    }
  });
  server.start();

  oklib::http::HttpClient client(&loop, server.listen_address(), "localhost", "http-client-reconnect");
  std::vector<oklib::http::HttpResponseMessage> responses;
  client.set_response_callback([&](oklib::http::HttpResponseMessage response) {
    responses.push_back(std::move(response));
    if (responses.size() == 1) {
      loop.run_after(std::chrono::milliseconds(50), [&] {
        client.send(oklib::http::HttpClientRequest("GET", "/again"));
      });
    } else {
      client.disconnect();
      loop.quit();
    }
  });

  client.send(oklib::http::HttpClientRequest("GET", "/close"));
  loop.run_after(std::chrono::seconds(3), [&] { loop.quit(); });
  loop.loop();

  require(responses.size() == 2, "client reconnect receives second response");
  require(responses[0].body == "/close", "first reconnect response parsed");
  require(responses[1].body == "/again", "second reconnect response parsed");
  require(responses[0].headers.get("X-Peer-Port") != responses[1].headers.get("X-Peer-Port"),
          "client reconnect opens a new TCP connection after server close");
}

void run_chunked_response_case() {
  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-client-chunked-server");
  server.set_async_http_callback([](oklib::http::HttpRequest,
                                    oklib::http::HttpResponseWriter writer) {
    auto response = writer.make_response();
    response.set_status_code(oklib::http::HttpStatusCode::ok);
    response.add_header("X-Chunked", "yes");
    require(writer.start_chunked(std::move(response)), "client chunked response starts");
    require(writer.write_chunk("hello "), "client chunked first chunk");
    require(writer.write_chunk("world"), "client chunked second chunk");
    require(writer.finish(), "client chunked response finishes");
  });
  server.start();

  oklib::http::HttpClient client(&loop, server.listen_address(), "localhost", "http-client-chunked");
  std::vector<oklib::http::HttpResponseMessage> responses;
  client.set_response_callback([&](oklib::http::HttpResponseMessage response) {
    responses.push_back(std::move(response));
    client.disconnect();
    loop.quit();
  });

  client.send(oklib::http::HttpClientRequest("GET", "/chunked"));
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();

  require(responses.size() == 1, "chunked client receives one response");
  require(responses[0].status_code == 200, "chunked client status parsed");
  require(responses[0].chunked, "chunked client flag parsed");
  require(responses[0].headers.get("X-Chunked") == "yes", "chunked client headers parsed");
  require(responses[0].body == "hello world", "chunked client body decoded");
}

}  // namespace

int main() {
  run_buffered_client_case();
  run_keep_alive_reuse_case();
  run_queued_before_connect_case();
  run_server_close_reconnect_case();
  run_chunked_response_case();
  return 0;
}
