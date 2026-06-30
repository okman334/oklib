#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http_demo_routes.h"
#include "oklib/base/thread_pool.h"
#include "oklib/http/http_server.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"

namespace {

struct Target {
  std::string host{"127.0.0.1"};
  uint16_t port{0};
  std::string path{"/upload-file-worker"};
};

struct Options {
  std::size_t requests{20};
  std::size_t clients{4};
  std::size_t body_size{64 * 1024};
  std::size_t chunk_size{16 * 1024};
  bool multipart{false};
  Target target;
  bool use_external_url{false};
};

std::size_t parse_size(const char* value, std::size_t fallback) {
  const auto parsed = std::strtoull(value, nullptr, 10);
  return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
}

uint16_t parse_port(std::string_view value, uint16_t fallback) {
  const auto parsed = std::strtoul(std::string(value).c_str(), nullptr, 10);
  if (parsed == 0 || parsed > 65535) {
    return fallback;
  }
  return static_cast<uint16_t>(parsed);
}

bool parse_url(std::string_view url, Target* target) {
  constexpr std::string_view scheme = "http://";
  if (url.substr(0, scheme.size()) != scheme) {
    return false;
  }
  url.remove_prefix(scheme.size());
  const auto slash = url.find('/');
  const auto authority = slash == std::string_view::npos ? url : url.substr(0, slash);
  target->path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
  if (authority.empty()) {
    return false;
  }

  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || close == 1) {
      return false;
    }
    target->host = std::string(authority.substr(1, close - 1));
    if (close + 1 == authority.size()) {
      target->port = 80;
    } else if (authority[close + 1] == ':' && close + 2 < authority.size()) {
      target->port = parse_port(authority.substr(close + 2), 80);
    } else {
      return false;
    }
  } else {
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
      target->host = std::string(authority);
      target->port = 80;
    } else {
      if (authority.find(':') != colon) {
        return false;
      }
      target->host = std::string(authority.substr(0, colon));
      target->port = parse_port(authority.substr(colon + 1), 80);
    }
  }
  return !target->host.empty() && target->port != 0;
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--requests" && i + 1 < argc) {
      options.requests = parse_size(argv[++i], options.requests);
    } else if (option == "--clients" && i + 1 < argc) {
      options.clients = parse_size(argv[++i], options.clients);
    } else if (option == "--body-size" && i + 1 < argc) {
      options.body_size = parse_size(argv[++i], options.body_size);
    } else if (option == "--chunk-size" && i + 1 < argc) {
      options.chunk_size = parse_size(argv[++i], options.chunk_size);
    } else if (option == "--url" && i + 1 < argc) {
      options.use_external_url = parse_url(argv[++i], &options.target);
    } else if (option == "--multipart") {
      options.multipart = true;
    }
  }
  options.requests = std::max<std::size_t>(1, options.requests);
  options.clients = std::max<std::size_t>(1, std::min(options.clients, options.requests));
  options.chunk_size = std::max<std::size_t>(1, options.chunk_size);
  return options;
}

long long percentile(std::vector<long long>& values, double p) {
  if (values.empty()) {
    return 0;
  }
  const auto index = static_cast<std::size_t>(
      std::min<double>(values.size() - 1, std::ceil((p / 100.0) * values.size()) - 1));
  return values[index];
}

int connect_target(const Target& target) {
  oklib::net::InetAddress address;
  try {
    address = oklib::net::InetAddress(target.host, target.port);
  } catch (const std::exception&) {
    return -1;
  }
  int fd = ::socket(address.family(), SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  if (::connect(fd, address.sockaddr_ptr(), address.length()) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::string host_header_value(const Target& target) {
  if (target.host.find(':') != std::string::npos) {
    return "[" + target.host + "]:" + std::to_string(target.port);
  }
  return target.host + ":" + std::to_string(target.port);
}

bool write_all_chunked(int fd, std::string_view data, std::size_t chunk_size) {
  std::size_t written = 0;
  while (written < data.size()) {
    const auto nbytes = std::min(chunk_size, data.size() - written);
    const auto n = ::write(fd, data.data() + written, nbytes);
    if (n <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(n);
  }
  return true;
}

bool read_success_response(int fd) {
  std::string response;
  char buffer[4096];
  while (response.find("\r\n\r\n") == std::string::npos ||
         response.find("\"bytes\"") == std::string::npos) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n <= 0) {
      break;
    }
    response.append(buffer, static_cast<std::size_t>(n));
  }
  return response.find("HTTP/1.1 201 Created") != std::string::npos;
}

std::string path_with_name(const std::string& path, std::size_t id) {
  const std::string separator = path.find('?') == std::string::npos ? "?" : "&";
  return path + separator + "name=bench-upload-" + std::to_string(id) + ".bin";
}

bool send_upload_request(const Target& target,
                         const Options& options,
                         const std::string& raw_body,
                         std::size_t id) {
  const int fd = connect_target(target);
  if (fd < 0) {
    return false;
  }

  std::string path = path_with_name(target.path, id);
  std::string content_type = "application/octet-stream";
  std::string body;
  if (options.multipart) {
    const std::string boundary = "----oklib-upload-benchmark";
    content_type = "multipart/form-data; boundary=" + boundary;
    body = "--" + boundary + "\r\n"
           "Content-Disposition: form-data; name=\"file\"; filename=\"bench-upload-" +
           std::to_string(id) + ".bin\"\r\n"
           "Content-Type: application/octet-stream\r\n"
           "\r\n" +
           raw_body +
           "\r\n--" + boundary + "--\r\n";
    path = target.path;
  } else {
    body = raw_body;
  }

  const std::string headers =
      "POST " + path + " HTTP/1.1\r\n"
      "Host: " + host_header_value(target) + "\r\n"
      "Content-Type: " + content_type + "\r\n"
      "Content-Length: " + std::to_string(body.size()) + "\r\n"
      "Connection: close\r\n\r\n";

  const bool ok = write_all_chunked(fd, headers, options.chunk_size) &&
                  write_all_chunked(fd, body, options.chunk_size) &&
                  read_success_response(fd);
  ::close(fd);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  const std::string body(options.body_size, 'x');

  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("upload-benchmark-workers");
  std::unique_ptr<oklib::http::HttpServer> server;
  Target target = options.target;

  if (!options.use_external_url) {
    workers.start(4);
    server = std::make_unique<oklib::http::HttpServer>(
        &loop,
        oklib::net::InetAddress::loopback(0),
        "oklib-http-upload-benchmark-server");
    oklib::examples::install_http_demo_routes(*server, workers);
    server->start();
    target.port = server->port();
  }

  std::atomic_size_t next_request{0};
  std::atomic_size_t completed{0};
  std::atomic_bool failed{false};
  std::mutex latency_mutex;
  std::vector<long long> latencies_us;
  latencies_us.reserve(options.requests);

  const auto started_at = std::chrono::steady_clock::now();
  std::vector<std::thread> clients;
  clients.reserve(options.clients);
  for (std::size_t i = 0; i < options.clients; ++i) {
    clients.emplace_back([&] {
      for (;;) {
        const auto id = next_request.fetch_add(1, std::memory_order_relaxed);
        if (id >= options.requests || failed.load(std::memory_order_acquire)) {
          return;
        }
        const auto sent_at = std::chrono::steady_clock::now();
        if (!send_upload_request(target, options, body, id)) {
          failed.store(true, std::memory_order_release);
          if (server) {
            loop.queue_in_loop([&] { loop.quit(); });
          }
          return;
        }
        const auto finished_at = std::chrono::steady_clock::now();
        {
          std::lock_guard lock(latency_mutex);
          latencies_us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
              finished_at - sent_at).count());
        }
        if (completed.fetch_add(1, std::memory_order_relaxed) + 1 == options.requests &&
            server) {
          loop.queue_in_loop([&] { loop.quit(); });
        }
      }
    });
  }

  if (server) {
    loop.run_after(std::chrono::seconds(60), [&] {
      failed.store(true, std::memory_order_release);
      loop.quit();
    });
    loop.loop();
  }

  for (auto& client : clients) {
    client.join();
  }
  const auto finished_at = std::chrono::steady_clock::now();

  if (server) {
    workers.stop();
  }

  if (failed.load(std::memory_order_acquire) || completed.load() != options.requests) {
    std::cerr << "upload benchmark failed after " << completed.load() << "/"
              << options.requests << " uploads\n";
    return 1;
  }

  std::sort(latencies_us.begin(), latencies_us.end());
  long long sum = 0;
  for (const auto latency : latencies_us) {
    sum += latency;
  }

  const double seconds = std::chrono::duration<double>(finished_at - started_at).count();
  const double throughput_mib =
      static_cast<double>(completed.load() * options.body_size) / (1024.0 * 1024.0) / seconds;
  const double avg_ms =
      static_cast<double>(sum) / static_cast<double>(latencies_us.size()) / 1000.0;

  std::cout << std::fixed << std::setprecision(2)
            << "requests=" << completed.load()
            << " clients=" << options.clients
            << " body_size=" << options.body_size
            << " chunk_size=" << options.chunk_size
            << " multipart=" << (options.multipart ? "true" : "false")
            << " seconds=" << seconds
            << " throughput_mib_s=" << throughput_mib
            << " avg_ms=" << avg_ms
            << " p95_ms=" << static_cast<double>(percentile(latencies_us, 95.0)) / 1000.0
            << " p99_ms=" << static_cast<double>(percentile(latencies_us, 99.0)) / 1000.0
            << '\n';
  return 0;
}
