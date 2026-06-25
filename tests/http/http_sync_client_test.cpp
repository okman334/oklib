#include <oklib/http/http_response.h>
#include <oklib/http/http_server.h>
#include <oklib/http/http_sync_client.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
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

class TestHttpServer {
 public:
  TestHttpServer() {
    std::promise<oklib::net::InetAddress> address_ready;
    std::promise<oklib::net::EventLoop*> loop_ready;
    auto address_future = address_ready.get_future();
    auto loop_future = loop_ready.get_future();

    thread_ = std::thread([this,
                           address_ready = std::move(address_ready),
                           loop_ready = std::move(loop_ready)]() mutable {
      oklib::net::EventLoop loop;
      loop_.store(&loop, std::memory_order_release);
      oklib::http::HttpServer server(&loop,
                                     oklib::net::InetAddress::loopback(0),
                                     "http-sync-client-test-server");
      server.set_http_callback([this](const oklib::http::HttpRequest& request,
                                      oklib::http::HttpResponse* response) {
        requests_.fetch_add(1, std::memory_order_relaxed);
        response->set_status_code(oklib::http::HttpStatusCode::ok);
        response->set_content_type("text/plain");
        response->add_header("X-Sync-Test", request.method_string());
        response->set_body(request.method_string() + " " + request.path() + "?" +
                           request.query() + " " + request.body());
      });
      server.start();
      address_ready.set_value(server.listen_address());
      loop_ready.set_value(&loop);
      loop.run_after(std::chrono::seconds(5), [&loop] { loop.quit(); });
      loop.loop();
      loop_.store(nullptr, std::memory_order_release);
    });

    address_ = address_future.get();
    loop_future.get();
  }

  ~TestHttpServer() {
    if (auto* loop = loop_.load(std::memory_order_acquire); loop != nullptr) {
      loop->quit();
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] const oklib::net::InetAddress& address() const noexcept { return address_; }
  [[nodiscard]] int requests() const noexcept { return requests_.load(std::memory_order_relaxed); }

 private:
  oklib::net::InetAddress address_;
  std::atomic<int> requests_{0};
  std::thread thread_;
  std::atomic<oklib::net::EventLoop*> loop_{nullptr};
};

void test_sync_client_posts_url_request() {
  TestHttpServer server;
  oklib::http::HttpSyncClient client;

  oklib::http::HttpClientRequest request("POST", "/unused");
  request.set_url("http://127.0.0.1:" + std::to_string(server.address().port()) +
                  "/echo?name=sync");
  request.set_header("Content-Type", "text/plain");
  request.set_body("sync-body");
  request.set_timeout(std::chrono::seconds(2));

  oklib::http::HttpResponseMessage response;
  const int ret = client.send(&request, &response);

  require(ret == static_cast<int>(oklib::http::HttpSyncClientError::ok),
          "sync client returns ok");
  require(response.status_code == 200, "sync client receives status");
  require(response.headers.get("X-Sync-Test") == "POST", "sync client receives headers");
  require(response.body == "POST /echo?name=sync sync-body", "sync client receives body");
  require(server.requests() == 1, "server receives exactly one sync request");
}

void test_sync_client_rejects_bad_url() {
  oklib::http::HttpSyncClient client;
  oklib::http::HttpClientRequest request("GET", "/");
  request.set_url("ftp://127.0.0.1/");

  oklib::http::HttpResponseMessage response;
  const int ret = client.send(&request, &response);
  require(ret == static_cast<int>(oklib::http::HttpSyncClientError::unsupported_scheme),
          "sync client rejects unsupported scheme");
}

void test_sync_client_rejects_event_loop_thread() {
  oklib::net::EventLoop loop;
  oklib::http::HttpSyncClient client;
  oklib::http::HttpClientRequest request("GET", "/");
  request.set_url("http://127.0.0.1/");

  oklib::http::HttpResponseMessage response;
  const int ret = client.send(&request, &response);
  require(ret == static_cast<int>(oklib::http::HttpSyncClientError::called_from_event_loop),
          "sync client rejects calls from an EventLoop thread");
}

void test_sync_client_times_out() {
  oklib::http::HttpSyncClient client;
  oklib::http::HttpClientRequest request("GET", "/");
  request.set_url("http://127.0.0.1:9/timeout");
  request.set_timeout(std::chrono::milliseconds(50));

  oklib::http::HttpResponseMessage response;
  const int ret = client.send(&request, &response);
  require(ret == static_cast<int>(oklib::http::HttpSyncClientError::timeout),
          "sync client reports timeout");
}

}  // namespace

int main() {
  test_sync_client_posts_url_request();
  test_sync_client_rejects_bad_url();
  test_sync_client_rejects_event_loop_thread();
  test_sync_client_times_out();
  return 0;
}
