#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "http_demo_routes.h"
#include "oklib/base/logging.h"
#include "oklib/base/thread_pool.h"
#include "oklib/http/http_server.h"
#include "oklib/http/tls_options.h"
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

  if (argc < 3) {
    std::cerr << "usage: " << argv[0]
              << " <cert.pem> <key.pem> [port=8443] [io_threads=0] [worker_threads=4]\n";
    return 1;
  }

  const uint16_t port = argc > 3 ? parse_port(argv[3], 8443) : 8443;
  const int io_threads = argc > 4 ? parse_positive(argv[4], 0) : 0;
  const int worker_threads = argc > 5 ? parse_positive(argv[5], 4) : 4;

  oklib::Logger::set_file_basename("oklib_https_server");
  OKLIB_LOG_INFO << "starting HTTPS demo server on port " << port
                 << ", cert=" << argv[1]
                 << ", key=" << argv[2];

  oklib::net::EventLoop loop;
  oklib::ThreadPool workers("https-demo-workers");
  workers.start(worker_threads);

  oklib::http::HttpServer server(&loop,
                                 oklib::net::InetAddress::any(port),
                                 "oklib-https-demo-server");
  oklib::http::TlsServerOptions tls;
  tls.enabled = true;
  tls.cert_file = argv[1];
  tls.key_file = argv[2];
  server.set_tls_options(std::move(tls));
  server.set_thread_num(io_threads);
  oklib::examples::install_http_demo_routes(server, workers);
  server.start();

  std::cout << "oklib HTTPS demo server listening on "
            << server.listen_address().to_ip_port() << '\n'
            << "upload raw jpg bytes with:\n"
            << "  curl -k -H 'Content-Type: image/jpeg' --data-binary @photo.jpg "
            << "https://127.0.0.1:" << port << "/upload-file?name=photo.jpg\n";
  loop.loop();
  return 0;
}
