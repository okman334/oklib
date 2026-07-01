/*
 * SOCKS5 proxy server example
 *
 * Build:
 *   cmake --build build --target oklib_socks5_proxy_server
 *
 * Start without authentication:
 *   ./build/examples/oklib_socks5_proxy_server
 *   ./build/examples/oklib_socks5_proxy_server 1080
 *
 * Start with username/password authentication:
 *   ./build/examples/oklib_socks5_proxy_server alice secret
 *   ./build/examples/oklib_socks5_proxy_server 1080 alice secret
 *
 * Start with I/O threads:
 *   ./build/examples/oklib_socks5_proxy_server 1080 --threads 4
 *
 * Start with IPv6 or dual-stack listening:
 *   ./build/examples/oklib_socks5_proxy_server 1080 --ipv6
 *   ./build/examples/oklib_socks5_proxy_server 1080 --dual-stack
 *
 * Start with a TLS-wrapped SOCKS5 listener. This requires a build configured
 * with OKLIB_ENABLE_TLS=ON:
 *   ./build-tls/examples/oklib_socks5_proxy_server 1082 --tls examples/certs/oklib_demo_server.crt examples/certs/oklib_demo_server.key
 *   ./build-tls/examples/oklib_socks5_proxy_server 1082 alice secret --tls examples/certs/oklib_demo_server.crt examples/certs/oklib_demo_server.key
 *
 * Test with curl:
 *   curl -v --proxy socks5h://127.0.0.1:1080 http://example.com/
 *   curl -v --proxy 'socks5h://[::1]:1080' http://example.com/
 *   curl -v --proxy socks5h://alice:secret@127.0.0.1:1080 http://example.com/
 *
 * Note: --tls wraps the proxy listener itself in TLS. A regular
 * socks5h:// curl proxy URL does not perform TLS to the proxy.
 */

#include <algorithm>
#include <any>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "oklib/base/logging.h"
#include "oklib/net/buffer.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"
#include "oklib/net/resolver.h"
#include "oklib/net/tcp_client.h"
#include "oklib/net/tcp_connection.h"
#include "oklib/net/tcp_server.h"
#include "oklib/net/timer_id.h"
#include "oklib/net/tls_options.h"
#include "socks5_protocol.h"

namespace {

using SocksAuthConfig = oklib::examples::socks5::AuthConfig;
using SocksAuthMethod = oklib::examples::socks5::AuthMethod;
using SocksAddressType = oklib::examples::socks5::AddressType;
using SocksParseStatus = oklib::examples::socks5::ParseStatus;
using SocksReplyCode = oklib::examples::socks5::ReplyCode;

constexpr auto kDefaultConnectTimeout = std::chrono::seconds(10);
constexpr auto kDefaultHandshakeTimeout = std::chrono::seconds(15);
constexpr auto kDefaultIdleTimeout = std::chrono::milliseconds(0);

enum class ListenMode {
  ipv4,
  ipv6,
  dual_stack,
};

struct Options {
  std::uint16_t port{1080};
  int io_threads{0};
  ListenMode listen_mode{ListenMode::ipv4};
  SocksAuthConfig auth;
  std::chrono::milliseconds connect_timeout{kDefaultConnectTimeout};
  std::chrono::milliseconds handshake_timeout{kDefaultHandshakeTimeout};
  std::chrono::milliseconds idle_timeout{kDefaultIdleTimeout};
  bool tls{false};
  std::string cert_file;
  std::string key_file;
};

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

bool parse_milliseconds(std::string_view value,
                        bool allow_zero,
                        std::chrono::milliseconds* timeout) {
  if (value.empty()) {
    return false;
  }
  unsigned long long parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end ||
      (!allow_zero && parsed == 0) ||
      parsed > static_cast<unsigned long long>(
                   std::chrono::milliseconds::max().count())) {
    return false;
  }
  *timeout = std::chrono::milliseconds(parsed);
  return true;
}

void print_usage(const char* program) {
  std::cerr << "usage: " << program
            << " <port=1080> [username password] [--threads N]"
               " [--ipv6|--dual-stack]"
               " [--connect-timeout-ms N]"
               " [--handshake-timeout-ms N]"
               " [--idle-timeout-ms N]"
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
    } else if (arg == "--ipv6") {
      if (options.listen_mode == ListenMode::dual_stack) {
        return std::nullopt;
      }
      options.listen_mode = ListenMode::ipv6;
    } else if (arg == "--dual-stack") {
      if (options.listen_mode == ListenMode::ipv6) {
        return std::nullopt;
      }
      options.listen_mode = ListenMode::dual_stack;
    } else if (arg == "--connect-timeout-ms") {
      if (i + 1 >= argc ||
          !parse_milliseconds(argv[++i], false, &options.connect_timeout)) {
        return std::nullopt;
      }
    } else if (arg == "--handshake-timeout-ms") {
      if (i + 1 >= argc ||
          !parse_milliseconds(argv[++i], false, &options.handshake_timeout)) {
        return std::nullopt;
      }
    } else if (arg == "--idle-timeout-ms") {
      if (i + 1 >= argc ||
          !parse_milliseconds(argv[++i], true, &options.idle_timeout)) {
        return std::nullopt;
      }
    } else if (arg == "--tls") {
      if (i + 2 >= argc || starts_with(argv[i + 1], "--") ||
          starts_with(argv[i + 2], "--")) {
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

std::string bytes_to_string(const std::vector<std::uint8_t>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

const char* address_type_name(SocksAddressType type);

class Socks5ProxySession
    : public std::enable_shared_from_this<Socks5ProxySession> {
 public:
  Socks5ProxySession(oklib::net::TcpConnectionPtr downstream,
                     SocksAuthConfig auth,
                     std::chrono::milliseconds connect_timeout,
                     std::chrono::milliseconds handshake_timeout,
                     std::chrono::milliseconds idle_timeout)
      : downstream_(std::move(downstream)),
        auth_(std::move(auth)),
        connect_timeout_(connect_timeout),
        handshake_timeout_(handshake_timeout),
        idle_timeout_(idle_timeout) {}

  void start() {
    if (handshake_timeout_.count() <= 0) {
      return;
    }
    auto weak = weak_from_this();
    handshake_timer_ = downstream_->loop()->run_after(handshake_timeout_, [weak] {
      if (auto session = weak.lock()) {
        session->on_handshake_timeout();
      }
    });
  }

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
    cancel_timers();
    if (upstream_) {
      if (upstream_->connected()) {
        upstream_->force_close();
      }
      schedule_upstream_cleanup();
      return;
    }
    if (client_) {
      client_->stop();
      client_.reset();
    }
    self_hold_.reset();
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
    auto targets =
        oklib::net::resolve_tcp_addresses(result.target.host, result.target.port);
    if (targets.empty()) {
      send_reply_and_shutdown(SocksReplyCode::general_failure);
      return;
    }

    preserved_downstream_ = buffer->retrieve_all_as_string();
    downstream_->pause_reading();
    phase_ = Phase::connecting;
    cancel_handshake_timer();
    connect_upstream(std::move(targets),
                     result.target.type,
                     result.target.host,
                     result.target.port);
  }

  void connect_upstream(std::vector<oklib::net::InetAddress> targets,
                        SocksAddressType target_type,
                        std::string_view host,
                        std::uint16_t port) {
    auto weak = weak_from_this();
    self_hold_ = shared_from_this();
    const auto first_target = targets.front().to_ip_port();
    client_ = std::make_unique<oklib::net::TcpClient>(
        downstream_->loop(), std::move(targets), "socks5-upstream");
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

    client_->set_connect_failed_callback([weak] {
      if (auto session = weak.lock()) {
        session->on_upstream_connect_failed();
      }
    });

    connect_timer_ = downstream_->loop()->run_after(connect_timeout_, [weak] {
      if (auto session = weak.lock()) {
        session->on_connect_timeout();
      }
    });

    OKLIB_LOG_INFO << "SOCKS5 connecting "
                   << downstream_->peer_address().to_ip_port() << " to "
                   << host << ':' << port
                   << " target-type=" << address_type_name(target_type)
                   << " via " << first_target;
    client_->connect();
  }

  void on_upstream_connection(const oklib::net::TcpConnectionPtr& connection) {
    if (phase_ == Phase::closed) {
      if (connection->connected()) {
        connection->force_close();
      } else {
        schedule_upstream_cleanup();
      }
      return;
    }

    if (connection->connected()) {
      if (phase_ != Phase::connecting) {
        connection->force_close();
        return;
      }
      cancel_connect_timer();
      cancel_handshake_timer();
      upstream_ = connection;
      phase_ = Phase::relay;
      send_bytes(oklib::examples::socks5::build_bound_reply(
          SocksReplyCode::success, connection->local_address()));
      if (!preserved_downstream_.empty()) {
        upstream_->send(std::move(preserved_downstream_));
        preserved_downstream_.clear();
      }
      downstream_->resume_reading();
      reset_idle_timer();
      OKLIB_LOG_INFO << "SOCKS5 relay established for "
                     << downstream_->peer_address().to_ip_port();
      return;
    }

    if (phase_ == Phase::connecting) {
      send_reply_and_shutdown(SocksReplyCode::general_failure);
      schedule_upstream_cleanup();
      return;
    }

    close_downstream_now();
    phase_ = Phase::closed;
    schedule_upstream_cleanup();
  }

  void on_upstream_message(oklib::net::Buffer* buffer) {
    if (phase_ != Phase::relay || !downstream_->connected()) {
      buffer->retrieve_all();
      close();
      return;
    }
    downstream_->send(buffer->retrieve_all_as_string());
    reset_idle_timer();
  }

  void on_handshake_timeout() {
    if (phase_ == Phase::connecting || phase_ == Phase::relay ||
        phase_ == Phase::closed) {
      return;
    }
    OKLIB_LOG_WARN << "SOCKS5 handshake timeout for "
                   << downstream_->peer_address().to_ip_port();
    if (phase_ == Phase::greeting || phase_ == Phase::user_password) {
      close_downstream_now();
      phase_ = Phase::closed;
    } else {
      send_reply_and_shutdown(SocksReplyCode::general_failure);
    }
    self_hold_.reset();
  }

  void on_upstream_connect_failed() {
    if (phase_ != Phase::connecting) {
      return;
    }
    OKLIB_LOG_WARN << "SOCKS5 upstream connect failed for "
                   << downstream_->peer_address().to_ip_port();
    send_reply_and_shutdown(SocksReplyCode::general_failure);
    schedule_upstream_cleanup();
  }

  void on_connect_timeout() {
    if (phase_ != Phase::connecting) {
      return;
    }
    OKLIB_LOG_WARN << "SOCKS5 upstream connect timeout for "
                   << downstream_->peer_address().to_ip_port();
    if (client_) {
      client_->stop();
      client_.reset();
    }
    send_reply_and_shutdown(SocksReplyCode::general_failure);
    self_hold_.reset();
  }

  void relay_downstream(oklib::net::Buffer* buffer) {
    if (!upstream_ || !upstream_->connected()) {
      buffer->retrieve_all();
      close_downstream_now();
      phase_ = Phase::closed;
      return;
    }
    upstream_->send(buffer->retrieve_all_as_string());
    reset_idle_timer();
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
    cancel_timers();
    if (downstream_ && downstream_->connected()) {
      downstream_->shutdown();
    }
  }

  void close_downstream_now() {
    cancel_timers();
    if (downstream_ && downstream_->connected()) {
      downstream_->force_close();
    }
  }

  void cancel_timers() {
    cancel_connect_timer();
    cancel_handshake_timer();
    cancel_idle_timer();
  }

  void cancel_connect_timer() {
    if (connect_timer_.valid()) {
      downstream_->loop()->cancel(connect_timer_);
      connect_timer_ = oklib::net::TimerId();
    }
  }

  void cancel_handshake_timer() {
    if (handshake_timer_.valid()) {
      downstream_->loop()->cancel(handshake_timer_);
      handshake_timer_ = oklib::net::TimerId();
    }
  }

  void cancel_idle_timer() {
    if (idle_timer_.valid()) {
      downstream_->loop()->cancel(idle_timer_);
      idle_timer_ = oklib::net::TimerId();
    }
  }

  void reset_idle_timer() {
    if (idle_timeout_.count() <= 0 || phase_ != Phase::relay) {
      return;
    }
    cancel_idle_timer();
    auto weak = weak_from_this();
    idle_timer_ = downstream_->loop()->run_after(idle_timeout_, [weak] {
      if (auto session = weak.lock()) {
        session->on_idle_timeout();
      }
    });
  }

  void on_idle_timeout() {
    if (phase_ != Phase::relay) {
      return;
    }
    OKLIB_LOG_INFO << "SOCKS5 relay idle timeout for "
                   << downstream_->peer_address().to_ip_port();
    phase_ = Phase::closed;
    cancel_timers();
    if (downstream_ && downstream_->connected()) {
      downstream_->force_close();
    }
    if (upstream_ && upstream_->connected()) {
      upstream_->force_close();
    }
    schedule_upstream_cleanup();
  }

  void schedule_upstream_cleanup() {
    if (upstream_cleanup_scheduled_) {
      return;
    }
    upstream_cleanup_scheduled_ = true;
    auto self = shared_from_this();
    downstream_->loop()->queue_in_loop([self] {
      self->upstream_.reset();
      self->client_.reset();
      self->self_hold_.reset();
    });
  }

  oklib::net::TcpConnectionPtr downstream_;
  oklib::net::TcpConnectionPtr upstream_;
  std::unique_ptr<oklib::net::TcpClient> client_;
  std::shared_ptr<Socks5ProxySession> self_hold_;
  SocksAuthConfig auth_;
  std::chrono::milliseconds connect_timeout_;
  std::chrono::milliseconds handshake_timeout_;
  std::chrono::milliseconds idle_timeout_;
  Phase phase_{Phase::greeting};
  std::string preserved_downstream_;
  oklib::net::TimerId connect_timer_;
  oklib::net::TimerId handshake_timer_;
  oklib::net::TimerId idle_timer_;
  bool upstream_cleanup_scheduled_{false};
};

std::shared_ptr<Socks5ProxySession>* session_from(
    const oklib::net::TcpConnectionPtr& connection) {
  return std::any_cast<std::shared_ptr<Socks5ProxySession>>(
      &connection->mutable_context());
}

const char* auth_mode(const SocksAuthConfig& auth) {
  return auth.enabled() ? "username/password" : "none";
}

const char* address_type_name(SocksAddressType type) {
  switch (type) {
    case SocksAddressType::ipv4:
      return "ipv4";
    case SocksAddressType::domain:
      return "domain";
    case SocksAddressType::ipv6:
      return "ipv6";
  }
  return "unknown";
}

const char* listen_mode_name(ListenMode mode) {
  switch (mode) {
    case ListenMode::ipv4:
      return "ipv4";
    case ListenMode::ipv6:
      return "ipv6";
    case ListenMode::dual_stack:
      return "dual-stack";
  }
  return "unknown";
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
  const bool use_ipv6_socket = options.listen_mode != ListenMode::ipv4;
  auto listen_address = use_ipv6_socket
                            ? oklib::net::InetAddress::any_ipv6(options.port)
                            : oklib::net::InetAddress::any(options.port);
  oklib::net::TcpServer::ListenOptions listen_options;
  listen_options.ipv6_only = options.listen_mode != ListenMode::dual_stack;
  oklib::net::TcpServer server(&loop, listen_address, "socks5-proxy",
                               listen_options);
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
      [options](const oklib::net::TcpConnectionPtr& connection) {
        if (connection->connected()) {
          auto session = std::make_shared<Socks5ProxySession>(
              connection,
              options.auth,
              options.connect_timeout,
              options.handshake_timeout,
              options.idle_timeout);
          session->start();
          connection->set_context(session);
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
                 << options.io_threads << " listen-mode="
                 << listen_mode_name(options.listen_mode)
                 << " connect-timeout-ms="
                 << options.connect_timeout.count()
                 << " handshake-timeout-ms="
                 << options.handshake_timeout.count()
                 << " idle-timeout-ms=" << options.idle_timeout.count();
  std::cout << "oklib SOCKS5 proxy listening on "
            << server.listen_address().to_ip_port() << " auth="
            << auth_mode(options.auth) << " TLS="
            << (options.tls ? "on" : "off") << " I/O threads="
            << options.io_threads << " listen-mode="
            << listen_mode_name(options.listen_mode)
            << " connect-timeout-ms=" << options.connect_timeout.count()
            << " handshake-timeout-ms=" << options.handshake_timeout.count()
            << " idle-timeout-ms=" << options.idle_timeout.count() << '\n';
  loop.loop();
  return EXIT_SUCCESS;
}
