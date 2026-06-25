#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "http_demo_routes.h"
#include "oklib/base/logging.h"
#include "oklib/base/thread_pool.h"
#include "oklib/http/http_server.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"

namespace {

uint16_t parse_port(const char* value, uint16_t fallback) {
  if (value == nullptr) {
    return fallback;
  }
  const int parsed = std::atoi(value);
  if (parsed <= 0 || parsed > 65535) {
    return fallback;
  }
  return static_cast<uint16_t>(parsed);
}

int parse_positive(const char* value, int fallback) {
  if (value == nullptr) {
    return fallback;
  }
  const int parsed = std::atoi(value);
  return parsed >= 0 ? parsed : fallback;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);

  const uint16_t port = argc > 1 ? parse_port(argv[1], 8080) : 8080;
  const int io_threads = argc > 2 ? parse_positive(argv[2], 0) : 0;
  const int worker_threads = argc > 3 ? parse_positive(argv[3], 4) : 4;

  oklib::Logger::set_file_basename("oklib_http_full_server");
  OKLIB_LOG_INFO << "starting HTTP demo server on port " << port
                 << ", io_threads=" << io_threads
                 << ", worker_threads=" << worker_threads;

  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("http-demo-workers");
  workers.start(worker_threads);

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::any(port),
                                 "oklib-http-full-server");
  server.set_thread_num(io_threads);
  oklib::examples::install_http_demo_routes(server, workers);
  server.start();

  std::cout << "oklib HTTP demo server listening on "
            << server.listen_address().to_ip_port() << '\n'
            << "usage: " << argv[0] << " [port=8080] [io_threads=0] [worker_threads=4]\n";
  loop.loop();
  return 0;
}
