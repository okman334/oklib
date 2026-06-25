#include <oklib/http/http_client.h>
#include <oklib/http/http_client_cache.h>
#include <oklib/http/http_response.h>
#include <oklib/http/http_server.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
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

struct CacheClientCase {
  oklib::net::EventLoop loop;
  oklib::http::HttpServer server;
  std::shared_ptr<oklib::http::HttpClientCache> cache;
  oklib::http::HttpClient client;
  std::vector<oklib::http::HttpResponseMessage> responses;
  std::function<void(std::size_t)> on_response;
  std::size_t expected_responses{2};

  explicit CacheClientCase(std::string name,
                           oklib::http::HttpClientCacheOptions cache_options = {})
      : server(&loop, oklib::net::InetAddress::loopback(0), name + "-server"),
        cache(std::make_shared<oklib::http::HttpClientCache>(cache_options)),
        client(&loop,
               server.listen_address(),
               "localhost",
               name + "-client",
               oklib::http::HttpClientOptions{.cache = cache}) {
    client.set_response_callback([this](oklib::http::HttpResponseMessage response) {
      responses.push_back(std::move(response));
      if (on_response) {
        on_response(responses.size());
      }
      if (responses.size() >= expected_responses) {
        client.disconnect();
        loop.quit();
      }
    });
  }

  void run_for(std::chrono::milliseconds timeout = std::chrono::milliseconds(1500)) {
    loop.run_after(timeout, [this] { loop.quit(); });
    loop.loop();
  }
};

void test_fresh_get_hit_uses_cache_without_network() {
  CacheClientCase fixture("http-client-cache-fresh");
  int requests = 0;
  fixture.server.set_http_callback([&](const oklib::http::HttpRequest&,
                                       oklib::http::HttpResponse* response) {
    ++requests;
    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->add_header("Cache-Control", "max-age=60");
    response->set_body("fresh-body");
  });
  fixture.server.start();

  fixture.client.send(oklib::http::HttpClientRequest("GET", "/fresh"));
  fixture.on_response = [&](std::size_t count) {
    if (count == 1) {
      fixture.client.send(oklib::http::HttpClientRequest("GET", "/fresh"));
    }
  };
  fixture.run_for();

  require(fixture.responses.size() == 2, "fresh cache case returns two responses");
  require(requests == 1, "fresh cache hit does not reach server twice");
  require(fixture.responses[0].source == oklib::http::HttpClientResponseSource::network,
          "first fresh response comes from network");
  require(fixture.responses[1].source == oklib::http::HttpClientResponseSource::cache,
          "second fresh response comes from cache");
  require(fixture.responses[1].body == "fresh-body", "fresh cached body returned");
}

void test_stale_etag_response_revalidates_with_304() {
  CacheClientCase fixture("http-client-cache-revalidate");
  int requests = 0;
  bool saw_validator = false;
  fixture.server.set_http_callback([&](const oklib::http::HttpRequest& request,
                                       oklib::http::HttpResponse* response) {
    ++requests;
    if (request.header("If-None-Match") == "\"v1\"") {
      saw_validator = true;
      response->set_status_code(304);
      response->add_header("Cache-Control", "max-age=60");
      response->add_header("ETag", "\"v1\"");
      return;
    }

    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->add_header("Cache-Control", "max-age=0");
    response->add_header("ETag", "\"v1\"");
    response->set_body("etag-body");
  });
  fixture.server.start();

  fixture.client.send(oklib::http::HttpClientRequest("GET", "/etag"));
  fixture.on_response = [&](std::size_t count) {
    if (count == 1) {
      fixture.client.send(oklib::http::HttpClientRequest("GET", "/etag"));
    }
  };
  fixture.run_for();

  require(fixture.responses.size() == 2, "revalidation returns two responses");
  require(requests == 2, "stale cache revalidates over network");
  require(saw_validator, "revalidation sends If-None-Match");
  require(fixture.responses[1].source == oklib::http::HttpClientResponseSource::revalidated,
          "304 response is surfaced as revalidated cache response");
  require(fixture.responses[1].status_code == 200, "revalidated response keeps cached status");
  require(fixture.responses[1].body == "etag-body", "revalidated response keeps cached body");
}

void test_vary_mismatch_uses_network_and_matching_vary_hits_cache() {
  CacheClientCase fixture("http-client-cache-vary");
  int requests = 0;
  fixture.server.set_http_callback([&](const oklib::http::HttpRequest& request,
                                       oklib::http::HttpResponse* response) {
    ++requests;
    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->add_header("Cache-Control", "max-age=60");
    response->add_header("Vary", "X-Mode");
    response->set_body("mode=" + request.header("X-Mode"));
  });
  fixture.server.start();

  oklib::http::HttpClientRequest first("GET", "/vary");
  first.add_header("X-Mode", "a");
  fixture.client.send(std::move(first));
  fixture.expected_responses = 3;
  fixture.on_response = [&](std::size_t count) {
    if (count == 1) {
      oklib::http::HttpClientRequest second("GET", "/vary");
      second.add_header("X-Mode", "b");
      fixture.client.send(std::move(second));
    } else if (count == 2) {
      oklib::http::HttpClientRequest third("GET", "/vary");
      third.add_header("X-Mode", "a");
      fixture.client.send(std::move(third));
    }
  };
  fixture.run_for();

  require(fixture.responses.size() == 3, "vary case returns three responses");
  require(requests == 2, "vary mismatch misses but later matching request hits cache");
  require(fixture.responses[0].source == oklib::http::HttpClientResponseSource::network,
          "first vary response is network");
  require(fixture.responses[1].source == oklib::http::HttpClientResponseSource::network,
          "vary mismatch is network");
  require(fixture.responses[2].source == oklib::http::HttpClientResponseSource::cache,
          "matching vary request hits cache");
  require(fixture.responses[2].body == "mode=a", "vary hit returns matching representation");
}

void test_no_store_and_body_limit_are_not_cached() {
  oklib::http::HttpClientCacheOptions options;
  options.max_body_bytes = 4;
  CacheClientCase fixture("http-client-cache-limits", options);
  int requests = 0;
  fixture.server.set_http_callback([&](const oklib::http::HttpRequest& request,
                                       oklib::http::HttpResponse* response) {
    ++requests;
    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->add_header("Cache-Control", request.path() == "/no-store" ? "no-store" : "max-age=60");
    response->set_body(request.path() == "/no-store" ? "no-store-body" : "too-large");
  });
  fixture.server.start();

  fixture.client.send(oklib::http::HttpClientRequest("GET", "/no-store"));
  fixture.on_response = [&](std::size_t count) {
    if (count == 1) {
      fixture.client.send(oklib::http::HttpClientRequest("GET", "/no-store"));
    }
  };
  fixture.run_for();

  require(fixture.responses.size() == 2, "no-store case returns two responses");
  require(requests == 2, "no-store responses are not cached");
  require(fixture.responses[1].source == oklib::http::HttpClientResponseSource::network,
          "no-store second response is network");

  fixture.responses.clear();
  fixture.on_response = [&](std::size_t count) {
    if (count == 1) {
      fixture.client.send(oklib::http::HttpClientRequest("GET", "/large"));
    }
  };
  fixture.client.send(oklib::http::HttpClientRequest("GET", "/large"));
  fixture.run_for();

  require(fixture.responses.size() == 2, "body limit case returns two responses");
  require(requests == 4, "oversized responses are not cached");
  require(fixture.responses[1].source == oklib::http::HttpClientResponseSource::network,
          "oversized second response is network");
}

}  // namespace

int main() {
  test_fresh_get_hit_uses_cache_without_network();
  test_stale_etag_response_revalidates_with_304();
  test_vary_mismatch_uses_network_and_matching_vary_hits_cache();
  test_no_store_and_body_limit_are_not_cached();
  return 0;
}
