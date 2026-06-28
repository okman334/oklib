#include <algorithm>
#include <any>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <iostream>
#include <limits>
#include <memory>
#include <netdb.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include "oklib/base/logging.h"
#include "oklib/net/buffer.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"
#include "oklib/net/tcp_client.h"
#include "oklib/net/tcp_connection.h"
#include "oklib/net/tcp_server.h"
#include "oklib/net/timer_id.h"
#include "oklib/net/tls_options.h"
#include "socks5_protocol.h"

namespace {

using SocksAuthConfig = oklib::examples::socks5::AuthConfig;
using SocksAuthMethod = oklib::examples::socks5::AuthMethod;
using SocksParseStatus = oklib::examples::socks5::ParseStatus;
using SocksReplyCode = oklib::examples::socks5::ReplyCode;

struct Options {
  std::uint16_t port{1080};
  int io_threads{0};
  SocksAuthConfig auth;
  bool tls{false};
  std::string cert_file;
  std::string key_file;
};

std::string lower(std::string_view value) {
  std::string result(value);
  for (char& ch : result) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return result;
}

bool starts_with(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool parse_port(std::string_view value, std::uint16_t* port) {
  if (value.empty()) {
    return false;
  }
  unsigned parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed == 0 ||
      parsed > 65535) {
    return false;
  }
  *port = static_cast<std::uint16_t>(parsed);
  return true;
}

bool parse_threads(std::string_view value, int* threads) {
  if (value.empty()) {
    return false;
  }
  unsigned parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end ||
      parsed > static_cast<unsigned>(std::numeric_limits<int>::max())) {
    return false;
  }
  *threads = static_cast<int>(parsed);
  return true;
}

void print_usage(const char* program) {
  std::cerr << "usage: " << program
            << " <port=1080> [username password] [--threads N]"
               " [--tls cert.pem key.pem]\n";
}

std::optional<Options> parse_options(int argc, char** argv) {
  Options options;
  std::vector<std::string_view> positional;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--threads") {
      if (i + 1 >= argc || !parse_threads(argv[++i], &options.io_threads)) {
        return std::nullopt;
      }
    } else if (arg == "--tls") {
      if (i + 2 >= argc) {
        return std::nullopt;
      }
      options.tls = true;
      options.cert_file = argv[++i];
      options.key_file = argv[++i];
    } else if (starts_with(arg, "--")) {
      return std::nullopt;
    } else {
      positional.push_back(arg);
      if (positional.size() > 3) {
        return std::nullopt;
      }
    }
  }

  if (positional.size() == 1) {
    if (!parse_port(positional[0], &options.port)) {
      return std::nullopt;
    }
  } else if (positional.size() == 2) {
    options.auth.username = positional[0];
    options.auth.password = positional[1];
  } else if (positional.size() == 3) {
    if (!parse_port(positional[0], &options.port)) {
      return std::nullopt;
    }
    options.auth.username = positional[1];
    options.auth.password = positional[2];
  }

  return options;
}

std::optional<oklib::net::InetAddress> resolve_ipv4(std::string_view host,
                                                    std::uint16_t port) {
  std::string host_string(host);
  if (lower(host_string) == "localhost") {
    host_string = "127.0.0.1";
  }

  try {
    return oklib::net::InetAddress(host_string, port);
  } catch (const std::invalid_argument&) {
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host_string.c_str(), service.c_str(), &hints, &result) !=
      0) {
    return std::nullopt;
  }

  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(result,
                                                             ::freeaddrinfo);
  for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
    if (ai->ai_family == AF_INET && ai->ai_addrlen >= sizeof(sockaddr_in)) {
      return oklib::net::InetAddress(
          *reinterpret_cast<sockaddr_in*>(ai->ai_addr));
    }
  }
  return std::nullopt;
}

std::string bytes_to_string(const std::vector<std::uint8_t>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

class Socks5ProxySession
    : public std::enable_shared_from_this<Socks5ProxySession> {
 public:
  Socks5ProxySession(oklib::net::TcpConnectionPtr downstream,
                     SocksAuthConfig auth)
      : downstream_(std::move(downstream)), auth_(std::move(auth)) {}

  void on_downstream_message(oklib::net::Buffer* buffer) {
    if (phase_ == Phase::closed) {
      buffer->retrieve_all();
      return;
    }

    if (phase_ == Phase::relay) {
      relay_downstream(buffer);
      return;
    }

    while (phase_ != Phase::closed && phase_ != Phase::connecting &&
           phase_ != Phase::relay && buffer->readable_bytes() > 0) {
      const auto before_phase = phase_;
      const auto before_bytes = buffer->readable_bytes();
      if (phase_ == Phase::greeting) {
        handle_greeting(buffer);
      } else if (phase_ == Phase::user_password) {
        handle_user_password(buffer);
      } else if (phase_ == Phase::request) {
        handle_request(buffer);
      }
      if (phase_ == before_phase && buffer->readable_bytes() == before_bytes) {
        break;
      }
    }
  }

  void close() {
    if (phase_ == Phase::closed) {
      return;
    }
    phase_ = Phase::closed;
    cancel_connect_timer();
    if (upstream_ && upstream_->connected()) {
      upstream_->force_close();
    }
    if (client_) {
      client_->stop();
    }
  }

 private:
  enum class Phase {
    greeting,
    user_password,
    request,
    connecting,
    relay,
    closed,
  };

  void handle_greeting(oklib::net::Buffer* buffer) {
    if (buffer->readable_bytes() < 2) {
      return;
    }
    const auto* data = buffer->peek();
    const auto version = static_cast<std::uint8_t>(data[0]);
    const auto method_count = static_cast<std::uint8_t>(data[1]);
    if (version != oklib::examples::socks5::kVersion || method_count == 0) {
      send_method_and_shutdown(SocksAuthMethod::no_acceptable_methods);
      buffer->retrieve_all();
      return;
    }
    const std::size_t total = 2 + method_count;
    if (buffer->readable_bytes() < total) {
      return;
    }

    const std::string_view methods(data + 2, method_count);
    const auto method = oklib::examples::socks5::choose_auth_method(methods,
                                                                    auth_);
    buffer->retrieve(total);
    send_bytes(oklib::examples::socks5::build_method_selection(method));

    if (method == SocksAuthMethod::no_acceptable_methods) {
      shutdown_downstream();
      phase_ = Phase::closed;
      return;
    }
    phase_ = method == SocksAuthMethod::username_password
                 ? Phase::user_password
                 : Phase::request;
  }

  void handle_user_password(oklib::net::Buffer* buffer) {
    const std::string_view data(buffer->peek(), buffer->readable_bytes());
    const auto result =
        oklib::examples::socks5::parse_user_password_auth(data, auth_);
    if (result.status == SocksParseStatus::need_more) {
      return;
    }
    if (result.consumed > 0) {
      buffer->retrieve(result.consumed);
    }
    send_bytes(oklib::examples::socks5::build_auth_status(result.ok));
    if (result.status != SocksParseStatus::complete || !result.ok) {
      shutdown_downstream();
      phase_ = Phase::closed;
      return;
    }
    phase_ = Phase::request;
  }

  void handle_request(oklib::net::Buffer* buffer) {
    const std::string_view data(buffer->peek(), buffer->readable_bytes());
    const auto result = oklib::examples::socks5::parse_connect_request(data);
    if (result.status == SocksParseStatus::need_more) {
      return;
    }
    if (result.status != SocksParseStatus::complete) {
      if (result.consumed > 0) {
        buffer->retrieve(result.consumed);
      }
      send_reply_and_shutdown(result.reply);
      return;
    }

    buffer->retrieve(result.consumed);
    auto target = resolve_ipv4(result.target.host, result.target.port);
    if (!target) {
      send_reply_and_shutdown(SocksReplyCode::general_failure);
      return;
    }

    preserved_downstream_ = buffer->retrieve_all_as_string();
    downstream_->pause_reading();
    phase_ = Phase::connecting;
    connect_upstream(*target, result.target.host, result.target.port);
  }

  void connect_upstream(const oklib::net::InetAddress& target,
                        std::string_view host,
                        std::uint16_t port) {
    auto weak = weak_from_this();
    client_ = std::make_unique<oklib::net::TcpClient>(
        downstream_->loop(), target, "socks5-upstream");
    client_->set_connection_callback([weak](const auto& connection) {
      if (auto session = weak.lock()) {
        session->on_upstream_connection(connection);
      }
    });
    client_->set_message_callback(
        [weak](const auto&, oklib::net::Buffer* buffer, oklib::Timestamp) {
          if (auto session = weak.lock()) {
            session->on_upstream_message(buffer);
          } else {
            buffer->retrieve_all();
          }
        });

    connect_timer_ = downstream_->loop()->run_after(std::chrono::seconds(10),
                                                    [weak] {
      if (auto session = weak.lock()) {
        session->on_connect_timeout();
      }
    });

    OKLIB_LOG_INFO << "SOCKS5 connecting "
                   << downstream_->peer_address().to_ip_port() << " to "
                   << host << ':' << port << " via " << target.to_ip_port();
    client_->connect();
  }

  void on_upstream_connection(const oklib::net::TcpConnectionPtr& connection) {
    if (phase_ == Phase::closed) {
      if (connection->connected()) {
        connection->force_close();
      }
      return;
    }

    if (connection->connected()) {
      if (phase_ != Phase::connecting) {
        connection->force_close();
        return;
      }
      cancel_connect_timer();
      upstream_ = connection;
      phase_ = Phase::relay;
      send_bytes(oklib::examples::socks5::build_reply(SocksReplyCode::success));
      if (!preserved_downstream_.empty()) {
        upstream_->send(std::move(preserved_downstream_));
        preserved_downstream_.clear();
      }
      downstream_->resume_reading();
      OKLIB_LOG_INFO << "SOCKS5 relay established for "
                     << downstream_->peer_address().to_ip_port();
      return;
    }

    if (phase_ == Phase::connecting) {
      send_reply_and_shutdown(SocksReplyCode::general_failure);
      return;
    }

    close_downstream_now();
    phase_ = Phase::closed;
  }

  void on_upstream_message(oklib::net::Buffer* buffer) {
    if (phase_ != Phase::relay || !downstream_->connected()) {
      buffer->retrieve_all();
      close();
      return;
    }
    downstream_->send(buffer->retrieve_all_as_string());
  }

  void on_connect_timeout() {
    if (phase_ != Phase::connecting) {
      return;
    }
    OKLIB_LOG_WARN << "SOCKS5 upstream connect timeout for "
                   << downstream_->peer_address().to_ip_port();
    if (client_) {
      client_->stop();
    }
    send_reply_and_shutdown(SocksReplyCode::general_failure);
  }

  void relay_downstream(oklib::net::Buffer* buffer) {
    if (!upstream_ || !upstream_->connected()) {
      buffer->retrieve_all();
      close_downstream_now();
      phase_ = Phase::closed;
      return;
    }
    upstream_->send(buffer->retrieve_all_as_string());
  }

  void send_method_and_shutdown(SocksAuthMethod method) {
    send_bytes(oklib::examples::socks5::build_method_selection(method));
    shutdown_downstream();
    phase_ = Phase::closed;
  }

  void send_reply_and_shutdown(SocksReplyCode reply) {
    send_bytes(oklib::examples::socks5::build_reply(reply));
    shutdown_downstream();
    phase_ = Phase::closed;
  }

  void send_bytes(const std::vector<std::uint8_t>& bytes) {
    if (downstream_ && downstream_->connected()) {
      downstream_->send(bytes_to_string(bytes));
    }
  }

  void shutdown_downstream() {
    cancel_connect_timer();
    if (downstream_ && downstream_->connected()) {
      downstream_->shutdown();
    }
  }

  void close_downstream_now() {
    cancel_connect_timer();
    if (downstream_ && downstream_->connected()) {
      downstream_->force_close();
    }
  }

  void cancel_connect_timer() {
    if (connect_timer_.valid()) {
      downstream_->loop()->cancel(connect_timer_);
      connect_timer_ = oklib::net::TimerId();
    }
  }

  oklib::net::TcpConnectionPtr downstream_;
  oklib::net::TcpConnectionPtr upstream_;
  std::unique_ptr<oklib::net::TcpClient> client_;
  SocksAuthConfig auth_;
  Phase phase_{Phase::greeting};
  std::string preserved_downstream_;
  oklib::net::TimerId connect_timer_;
};

std::shared_ptr<Socks5ProxySession>* session_from(
    const oklib::net::TcpConnectionPtr& connection) {
  return std::any_cast<std::shared_ptr<Socks5ProxySession>>(
      &connection->mutable_context());
}

const char* auth_mode(const SocksAuthConfig& auth) {
  return auth.enabled() ? "username/password" : "none";
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);

  auto parsed = parse_options(argc, argv);
  if (!parsed) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }
  Options options = std::move(*parsed);

  if (options.tls) {
#if !OKLIB_ENABLE_TLS
    std::cerr << "This binary was built without OKLIB_ENABLE_TLS=ON.\n";
    return EXIT_FAILURE;
#endif
  }

  oklib::net::EventLoop loop;
  oklib::net::TcpServer server(&loop, oklib::net::InetAddress::any(options.port),
                               "socks5-proxy");
  server.set_thread_num(options.io_threads);
#if OKLIB_ENABLE_TLS
  if (options.tls) {
    oklib::net::TlsServerOptions tls;
    tls.enabled = true;
    tls.cert_file = options.cert_file;
    tls.key_file = options.key_file;
    server.set_tls_options(std::move(tls));
  }
#endif

  server.set_connection_callback(
      [auth = options.auth](const oklib::net::TcpConnectionPtr& connection) {
        if (connection->connected()) {
          auto session =
              std::make_shared<Socks5ProxySession>(connection, auth);
          connection->set_context(std::move(session));
          OKLIB_LOG_INFO << "SOCKS5 downstream connected "
                         << connection->peer_address().to_ip_port();
          return;
        }

        if (auto* session = session_from(connection); session != nullptr &&
                                                 *session) {
          (*session)->close();
        }
        connection->set_context(std::any{});
        OKLIB_LOG_INFO << "SOCKS5 downstream disconnected "
                       << connection->peer_address().to_ip_port();
      });
  server.set_message_callback(
      [](const oklib::net::TcpConnectionPtr& connection,
         oklib::net::Buffer* buffer,
         oklib::Timestamp) {
        if (auto* session = session_from(connection); session != nullptr &&
                                                 *session) {
          (*session)->on_downstream_message(buffer);
          return;
        }
        buffer->retrieve_all();
        connection->force_close();
      });

  server.start();
  OKLIB_LOG_INFO << "SOCKS5 proxy listening on "
                 << server.listen_address().to_ip_port() << " auth="
                 << auth_mode(options.auth) << " TLS="
                 << (options.tls ? "on" : "off") << " I/O threads="
                 << options.io_threads;
  std::cout << "oklib SOCKS5 proxy listening on "
            << server.listen_address().to_ip_port() << " auth="
            << auth_mode(options.auth) << " TLS="
            << (options.tls ? "on" : "off") << " I/O threads="
            << options.io_threads << '\n';
  loop.loop();
  return EXIT_SUCCESS;
}
