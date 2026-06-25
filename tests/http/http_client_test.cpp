#include <oklib/http/http_client.h>
#include <oklib/http/http_response.h>
#include <oklib/http/http_response_writer.h>
#include <oklib/http/http_server.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>
#include <oklib/net/buffer.h>
#include <oklib/net/tcp_connection.h>
#include <oklib/net/tcp_server.h>

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

void run_fixed_streaming_response_case() {
  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-client-fixed-stream-server");
  server.set_http_callback([](const oklib::http::HttpRequest&,
                              oklib::http::HttpResponse* response) {
    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->add_header("X-Stream", "fixed");
    response->set_content_type("text/plain");
    response->set_body("fixed response stream");
  });
  server.start();

  oklib::http::HttpClient client(&loop, server.listen_address(), "localhost", "http-client-fixed-stream");
  std::vector<oklib::http::HttpResponseMessage> responses;
  std::string streamed_body;
  bool completed = false;
  client.set_streaming_response_callback(
      [&](oklib::http::HttpResponseMessage response,
          oklib::http::HttpClientResponseStream stream) {
        responses.push_back(std::move(response));
        require(responses.back().body.empty(), "fixed streaming response body is not buffered");
        stream.set_data_callback([&](std::string_view chunk) {
          streamed_body.append(chunk);
        });
        stream.set_complete_callback([&] {
          completed = true;
          client.disconnect();
          loop.quit();
        });
      });

  client.send(oklib::http::HttpClientRequest("GET", "/fixed-stream"));
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();

  require(responses.size() == 1, "fixed streaming client receives headers");
  require(responses[0].status_code == 200, "fixed streaming client status parsed");
  require(responses[0].headers.get("X-Stream") == "fixed", "fixed streaming client headers parsed");
  require(completed, "fixed streaming client completes body");
  require(streamed_body == "fixed response stream", "fixed streaming client body delivered");
}

void run_chunked_streaming_response_case() {
  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-client-chunked-stream-server");
  server.set_async_http_callback([](oklib::http::HttpRequest,
                                    oklib::http::HttpResponseWriter writer) {
    auto response = writer.make_response();
    response.set_status_code(oklib::http::HttpStatusCode::ok);
    response.add_header("X-Stream", "chunked");
    require(writer.start_chunked(std::move(response)), "streaming client chunked response starts");
    require(writer.write_chunk("hello "), "streaming client chunked first chunk");
    require(writer.write_chunk("stream"), "streaming client chunked second chunk");
    require(writer.finish(), "streaming client chunked response finishes");
  });
  server.start();

  oklib::http::HttpClient client(&loop, server.listen_address(), "localhost", "http-client-chunked-stream");
  std::vector<oklib::http::HttpResponseMessage> responses;
  std::vector<std::string> chunks;
  bool completed = false;
  client.set_streaming_response_callback(
      [&](oklib::http::HttpResponseMessage response,
          oklib::http::HttpClientResponseStream stream) {
        responses.push_back(std::move(response));
        stream.set_data_callback([&](std::string_view chunk) {
          chunks.emplace_back(chunk);
        });
        stream.set_complete_callback([&] {
          completed = true;
          client.disconnect();
          loop.quit();
        });
      });

  client.send(oklib::http::HttpClientRequest("GET", "/chunked-stream"));
  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();

  require(responses.size() == 1, "chunked streaming client receives headers");
  require(responses[0].status_code == 200, "chunked streaming client status parsed");
  require(responses[0].chunked, "chunked streaming client marks response chunked");
  require(responses[0].headers.get("X-Stream") == "chunked", "chunked streaming client headers parsed");
  require(completed, "chunked streaming client completes body");
  require(chunks.size() == 2, "chunked streaming client preserves response chunk callbacks");
  require(chunks[0] == "hello ", "chunked streaming client first chunk delivered");
  require(chunks[1] == "stream", "chunked streaming client second chunk delivered");
}

void run_expect_continue_case() {
  oklib::net::EventLoop loop;
  oklib::net::TcpServer server(&loop, oklib::net::InetAddress::loopback(0), "http-client-expect-server");
  std::string received;
  bool sent_continue = false;
  bool sent_final = false;
  bool body_before_continue = false;
  server.set_message_callback(
      [&](const oklib::net::TcpConnectionPtr& connection,
          oklib::net::Buffer* buffer,
          oklib::Timestamp) {
        received += buffer->retrieve_all_as_string();
        const auto header_end = received.find("\r\n\r\n");
        if (!sent_continue && header_end != std::string::npos) {
          body_before_continue = received.find("upload-body", header_end + 4) != std::string::npos;
          connection->send("HTTP/1.1 100 Continue\r\n\r\n");
          sent_continue = true;
        }
        if (sent_continue && !sent_final &&
            received.find("upload-body", header_end == std::string::npos ? 0 : header_end + 4) !=
                std::string::npos) {
          connection->send("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                           "Content-Length: 8\r\nX-Expect: ok\r\n\r\naccepted");
          sent_final = true;
        }
      });
  server.start();

  oklib::http::HttpClient client(&loop, server.listen_address(), "localhost", "http-client-expect");
  std::vector<oklib::http::HttpResponseMessage> responses;
  client.set_response_callback([&](oklib::http::HttpResponseMessage response) {
    responses.push_back(std::move(response));
    if (responses.back().status_code == 200) {
      client.disconnect();
      loop.quit();
    }
  });

  oklib::http::HttpClientRequest request("POST", "/expect");
  request.add_header("Expect", "100-continue");
  request.set_body("upload-body");
  client.send(std::move(request));

  loop.run_after(std::chrono::seconds(2), [&] { loop.quit(); });
  loop.loop();

  require(sent_continue, "expect server sends 100 Continue");
  require(sent_final, "expect server sends final response after body");
  require(!body_before_continue, "expect client waits for 100 Continue before body");
  require(responses.size() == 1, "expect client hides interim 100 response");
  require(responses[0].status_code == 200, "expect client final status parsed");
  require(responses[0].headers.get("X-Expect") == "ok", "expect client final headers parsed");
  require(responses[0].body == "accepted", "expect client final body parsed");
}

}  // namespace

int main() {
  run_buffered_client_case();
  run_keep_alive_reuse_case();
  run_queued_before_connect_case();
  run_server_close_reconnect_case();
  run_chunked_response_case();
  run_fixed_streaming_response_case();
  run_chunked_streaming_response_case();
  run_expect_continue_case();
  return 0;
}
