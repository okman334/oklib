#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "oklib/http/http_client.h"
#include "oklib/http/http_response.h"
#include "oklib/http/http_server.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"

namespace {

struct Options {
  std::size_t requests{10000};
  std::size_t clients{64};
  int server_threads{0};
  std::size_t body_size{2};
};

std::size_t parse_size(const char* value, std::size_t fallback) {
  const auto parsed = std::strtoull(value, nullptr, 10);
  return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--requests" && i + 1 < argc) {
      options.requests = parse_size(argv[++i], options.requests);
    } else if (option == "--clients" && i + 1 < argc) {
      options.clients = parse_size(argv[++i], options.clients);
    } else if (option == "--server-threads" && i + 1 < argc) {
      options.server_threads = static_cast<int>(parse_size(argv[++i], options.server_threads));
    } else if (option == "--body-size" && i + 1 < argc) {
      options.body_size = parse_size(argv[++i], options.body_size);
    }
  }
  options.clients = std::max<std::size_t>(1, std::min(options.clients, options.requests));
  return options;
}

struct ClientState {
  std::size_t id{0};
  std::unique_ptr<oklib::http::HttpClient> client;
  std::chrono::steady_clock::time_point sent_at;
};

long long percentile(std::vector<long long>& values, double p) {
  if (values.empty()) {
    return 0;
  }
  const auto index = static_cast<std::size_t>(
      std::min<double>(values.size() - 1, std::ceil((p / 100.0) * values.size()) - 1));
  return values[index];
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  const std::string response_body(options.body_size, 'x');

  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::loopback(0),
                                 "oklib-http-benchmark-server");
  server.set_thread_num(options.server_threads);
  server.set_http_callback([&](const oklib::http::HttpRequest&,
                               oklib::http::HttpResponse* response) {
    response->set_status_code(200);
    response->set_content_type("text/plain");
    response->set_body(response_body);
  });
  server.start();

  std::vector<std::unique_ptr<ClientState>> clients;
  clients.reserve(options.clients);
  std::vector<long long> latencies_us;
  latencies_us.reserve(options.requests);

  std::size_t issued = 0;
  std::size_t completed = 0;
  bool failed = false;

  auto send_next = [&](ClientState& state) {
    if (issued >= options.requests) {
      return;
    }
    const std::size_t request_id = ++issued;
    state.sent_at = std::chrono::steady_clock::now();
    state.client->send(oklib::http::HttpClientRequest("GET", "/bench?id=" + std::to_string(request_id)));
  };

  for (std::size_t i = 0; i < options.clients; ++i) {
    auto state = std::make_unique<ClientState>();
    state->id = i;
    state->client = std::make_unique<oklib::http::HttpClient>(
        &loop,
        server.listen_address(),
        "localhost:" + std::to_string(server.port()),
        "oklib-http-benchmark-client-" + std::to_string(i));
    ClientState* raw_state = state.get();
    state->client->set_response_callback([&, raw_state](oklib::http::HttpResponseMessage response) {
      if (response.status_code != 200) {
        failed = true;
        loop.quit();
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      latencies_us.push_back(
          std::chrono::duration_cast<std::chrono::microseconds>(now - raw_state->sent_at).count());
      ++completed;
      if (completed >= options.requests) {
        for (auto& client_state : clients) {
          client_state->client->disconnect();
        }
        loop.quit();
        return;
      }
      send_next(*raw_state);
    });
    state->client->set_error_callback([&] {
      failed = true;
      loop.quit();
    });
    clients.push_back(std::move(state));
  }

  const auto started_at = std::chrono::steady_clock::now();
  for (auto& state : clients) {
    send_next(*state);
  }
  loop.run_after(std::chrono::seconds(30), [&] {
    failed = true;
    loop.quit();
  });
  loop.loop();
  const auto finished_at = std::chrono::steady_clock::now();

  if (failed || completed != options.requests) {
    std::cerr << "benchmark failed after " << completed << "/" << options.requests << " responses\n";
    return 1;
  }

  std::sort(latencies_us.begin(), latencies_us.end());
  long long sum = 0;
  for (const auto latency : latencies_us) {
    sum += latency;
  }
  const double seconds =
      std::chrono::duration<double>(finished_at - started_at).count();
  const double qps = static_cast<double>(completed) / seconds;
  const double avg_us = static_cast<double>(sum) / static_cast<double>(latencies_us.size());

  std::cout << std::fixed << std::setprecision(2)
            << "requests=" << completed
            << " clients=" << options.clients
            << " server_threads=" << options.server_threads
            << " seconds=" << seconds
            << " qps=" << qps
            << " avg_us=" << avg_us
            << " p95_us=" << percentile(latencies_us, 95.0)
            << " p99_us=" << percentile(latencies_us, 99.0)
            << '\n';
  return 0;
}
