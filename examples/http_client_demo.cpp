#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "oklib/http/http_client.h"
#include "oklib/http/http_client_cache.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"

namespace {

struct Args {
  std::string ip{"127.0.0.1"};
  uint16_t port{8080};
  std::string host{"localhost:8080"};
  bool https{false};
  bool verify_peer{false};
  std::string ca_file;
  std::string ca_path;
};

uint16_t parse_port(const char* value, uint16_t fallback) {
  const int parsed = std::atoi(value);
  if (parsed <= 0 || parsed > 65535) {
    return fallback;
  }
  return static_cast<uint16_t>(parsed);
}

std::string default_host_header(std::string_view ip, uint16_t port) {
  std::string host;
  if (ip.find(':') != std::string_view::npos) {
    host = "[" + std::string(ip) + "]";
  } else {
    host = std::string(ip);
  }
  host += ":" + std::to_string(port);
  return host;
}

Args parse_args(int argc, char** argv) {
  Args args;
  std::vector<std::string> positionals;

  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--https") {
      args.https = true;
    } else if (option == "--verify-peer") {
      args.verify_peer = true;
    } else if (option == "--ca-file" && i + 1 < argc) {
      args.ca_file = argv[++i];
    } else if (option == "--ca-path" && i + 1 < argc) {
      args.ca_path = argv[++i];
    } else {
      positionals.push_back(option);
    }
  }

  if (!positionals.empty()) {
    args.ip = positionals[0];
  }
  if (positionals.size() > 1) {
    args.port = parse_port(positionals[1].c_str(), args.port);
  }
  args.host = default_host_header(args.ip, args.port);
  if (positionals.size() > 2) {
    args.host = positionals[2];
  }
  return args;
}

const char* source_name(oklib::http::HttpClientResponseSource source) {
  switch (source) {
    case oklib::http::HttpClientResponseSource::network:
      return "network";
    case oklib::http::HttpClientResponseSource::cache:
      return "cache";
    case oklib::http::HttpClientResponseSource::revalidated:
      return "revalidated";
  }
  return "unknown";
}

std::vector<oklib::http::HttpClientRequest> make_requests() {
  std::vector<oklib::http::HttpClientRequest> requests;
  requests.emplace_back("GET", "/");
  requests.emplace_back("GET", "/headers");

  oklib::http::HttpClientRequest post("POST", "/echo");
  post.set_header("Content-Type", "text/plain; charset=utf-8");
  post.set_body("hello from oklib HttpClient\n");
  requests.push_back(std::move(post));

  oklib::http::HttpClientRequest upload("POST", "/upload-file?name=client-demo.jpg");
  upload.set_header("Content-Type", "image/jpeg");
  upload.set_body("\xff\xd8\xff\xe0oklib-client-demo-jpg\xff\xd9");
  requests.push_back(std::move(upload));

  requests.emplace_back("GET", "/cache");
  requests.emplace_back("GET", "/cache");
  requests.emplace_back("GET", "/async?task=demo");
  requests.emplace_back("GET", "/chunks");
  return requests;
}

}  // namespace

int main(int argc, char** argv) {
  Args args = parse_args(argc, argv);
  if (args.https) {
#if !OKLIB_ENABLE_TLS
    std::cerr << "This binary was built without OKLIB_ENABLE_TLS=ON.\n";
    return 1;
#endif
  }

  oklib::net::EventLoop loop;
  auto cache = std::make_shared<oklib::http::HttpClientCache>();
  oklib::http::HttpClientOptions options;
  options.cache = cache;
#if OKLIB_ENABLE_TLS
  if (args.https) {
    options.tls.enabled = true;
    options.tls.verify_peer = args.verify_peer;
    options.tls.server_name = args.host;
    options.tls.ca_file = args.ca_file;
    options.tls.ca_path = args.ca_path;
  }
#endif

  oklib::http::HttpClient client(&loop,
                                 oklib::net::InetAddress(args.ip, args.port),
                                 args.host,
                                 "oklib-http-client-demo",
                                 std::move(options));

  auto requests = make_requests();
  std::size_t next_request = 0;
  bool failed = false;

  client.set_response_callback([&](oklib::http::HttpResponseMessage response) {
    std::cout << response.status_code << " " << response.reason_phrase
              << " source=" << source_name(response.source)
              << " bytes=" << response.body.size() << '\n';
    if (!response.body.empty()) {
      std::cout << response.body << '\n';
    }

    if (next_request < requests.size()) {
      client.send(requests[next_request++]);
      return;
    }
    client.disconnect();
    loop.quit();
  });
  client.set_error_callback([&] {
    failed = true;
    std::cerr << "HTTP client request failed\n";
    loop.quit();
  });

  std::cout << "connecting to " << (args.https ? "https://" : "http://")
            << args.host << " at " << args.ip << ':' << args.port << '\n';
  client.send(requests[next_request++]);
  loop.run_after(std::chrono::seconds(10), [&] {
    failed = true;
    std::cerr << "HTTP client demo timed out\n";
    loop.quit();
  });
  loop.loop();
  return failed ? 1 : 0;
}
