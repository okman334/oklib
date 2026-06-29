# SOCKS5 Proxy Example Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an `oklib_socks5_proxy_server` example that supports normal SOCKS5 CONNECT proxying with no-auth or username/password auth, plus an optional TLS listener path.

**Architecture:** Keep protocol parsing in a small example-local header so it can be unit tested without opening sockets. The executable owns one `Socks5ProxySession` per downstream `TcpConnection`, uses `TcpClient` for the upstream connection, pauses downstream reads while connecting upstream, then relays bytes in both directions after a SOCKS5 success reply. IPv4 and domain targets are supported; IPv6 target requests receive SOCKS5 `AddrTypeNotSupported` because the current `InetAddress`, `Socket`, and `Connector` stack is IPv4-only.

**Tech Stack:** C++20, `oklib::net::TcpServer`, `oklib::net::TcpClient`, `oklib::net::Buffer`, optional `oklib::net::TlsServerOptions`, CMake, existing simple test style.

---

## File Structure

- Create `examples/socks5_protocol.h`
  - Defines SOCKS5 constants, auth config, parse results, reply builders, and IPv4/domain request parsing.
  - Does not depend on `TcpServer` or `TcpClient`.
- Create `examples/socks5_proxy_server.cpp`
  - Parses command line arguments.
  - Configures optional TLS listener.
  - Owns per-client `Socks5ProxySession` objects that drive the SOCKS5 state machine and bridge downstream/upstream connections.
- Create `tests/examples/socks5_protocol_test.cpp`
  - Unit tests for auth method selection, username/password sub-negotiation, IPv4 request parsing, domain request parsing, and IPv6 rejection.
- Modify `examples/CMakeLists.txt`
  - Adds `oklib_socks5_proxy_server`.
- Modify `tests/CMakeLists.txt`
  - Adds `oklib_socks5_protocol_test` and includes the `examples` directory for the header.

---

### Task 1: Add Test Coverage For SOCKS5 Protocol Helpers

**Files:**
- Create: `tests/examples/socks5_protocol_test.cpp`
- Modify: `tests/CMakeLists.txt`
- Create in Task 2: `examples/socks5_protocol.h`

- [ ] **Step 1: Write the failing protocol tests**

Create `tests/examples/socks5_protocol_test.cpp`:

```cpp
#include "socks5_protocol.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> values) {
  return std::vector<std::uint8_t>(values);
}

}  // namespace

int main() {
  using namespace oklib::examples::socks5;

  AuthConfig no_auth;
  auto method = choose_auth_method(bytes({0x00, 0x02}), no_auth);
  require(method == AuthMethod::no_auth, "no-auth server selects no-auth when offered");

  AuthConfig password_auth;
  password_auth.username = "alice";
  password_auth.password = "secret";
  method = choose_auth_method(bytes({0x00, 0x02}), password_auth);
  require(method == AuthMethod::username_password,
          "password server selects username/password when offered");
  method = choose_auth_method(bytes({0x00}), password_auth);
  require(method == AuthMethod::no_acceptable_methods,
          "password server rejects clients that do not offer username/password");

  auto auth_ok = parse_user_password_auth(bytes({0x01, 0x05, 'a', 'l', 'i', 'c', 'e',
                                                0x06, 's', 'e', 'c', 'r', 'e', 't'}),
                                          password_auth);
  require(auth_ok.ok, "valid username/password auth succeeds");
  require(auth_ok.consumed == 14, "valid auth consumes all auth bytes");

  auto auth_bad = parse_user_password_auth(bytes({0x01, 0x05, 'a', 'l', 'i', 'c', 'e',
                                                 0x05, 'w', 'r', 'o', 'n', 'g'}),
                                           password_auth);
  require(!auth_bad.ok, "wrong password fails");
  require(auth_bad.consumed == 13, "failed auth consumes the complete auth packet");

  auto ipv4 = parse_connect_request(bytes({0x05, 0x01, 0x00, 0x01, 127, 0, 0, 1, 0x1F, 0x90}));
  require(ipv4.status == ParseStatus::complete, "IPv4 connect request parses");
  require(ipv4.target.host == "127.0.0.1", "IPv4 host is formatted");
  require(ipv4.target.port == 8080, "IPv4 port is parsed");
  require(ipv4.consumed == 10, "IPv4 request consumes exact request length");

  auto domain = parse_connect_request(bytes({0x05, 0x01, 0x00, 0x03, 11, 'e', 'x', 'a', 'm',
                                            'p', 'l', 'e', '.', 'c', 'o', 'm', 0x00, 0x50}));
  require(domain.status == ParseStatus::complete, "domain connect request parses");
  require(domain.target.host == "example.com", "domain host is parsed");
  require(domain.target.port == 80, "domain port is parsed");
  require(domain.consumed == 18, "domain request consumes exact request length");

  auto short_request = parse_connect_request(bytes({0x05, 0x01, 0x00, 0x03, 11, 'e'}));
  require(short_request.status == ParseStatus::need_more, "short domain request waits for more bytes");

  auto ipv6 = parse_connect_request(bytes({0x05, 0x01, 0x00, 0x04, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 80}));
  require(ipv6.status == ParseStatus::error, "IPv6 request is rejected");
  require(ipv6.reply == ReplyCode::addr_type_not_supported, "IPv6 rejection maps to addr type reply");

  auto success = build_reply(ReplyCode::success);
  require(success == bytes({0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0}),
          "success reply uses IPv4 wildcard bind address");

  return EXIT_SUCCESS;
}
```

- [ ] **Step 2: Register the failing test target**

Modify `tests/CMakeLists.txt` after the existing `oklib_http_demo_routes_test` target:

```cmake
add_executable(oklib_socks5_protocol_test examples/socks5_protocol_test.cpp)
target_include_directories(oklib_socks5_protocol_test PRIVATE ${PROJECT_SOURCE_DIR}/examples)
target_link_libraries(oklib_socks5_protocol_test PRIVATE oklib::net)
add_test(NAME oklib.example.socks5_protocol COMMAND oklib_socks5_protocol_test)
```

- [ ] **Step 3: Run the new test and confirm the expected failure**

Run:

```bash
cmake --build build --target oklib_socks5_protocol_test
```

Expected: the build fails because `examples/socks5_protocol.h` does not exist yet.

---

### Task 2: Implement SOCKS5 Protocol Helpers

**Files:**
- Create: `examples/socks5_protocol.h`
- Test: `tests/examples/socks5_protocol_test.cpp`

- [ ] **Step 1: Add the protocol helper header**

Create `examples/socks5_protocol.h`:

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oklib::examples::socks5 {

inline constexpr std::uint8_t kVersion = 0x05;
inline constexpr std::uint8_t kAuthVersion = 0x01;

enum class AuthMethod : std::uint8_t {
  no_auth = 0x00,
  username_password = 0x02,
  no_acceptable_methods = 0xff,
};

enum class ReplyCode : std::uint8_t {
  success = 0x00,
  general_failure = 0x01,
  command_not_supported = 0x07,
  addr_type_not_supported = 0x08,
};

enum class ParseStatus {
  need_more,
  complete,
  error,
};

struct AuthConfig {
  std::string username;
  std::string password;

  [[nodiscard]] bool enabled() const noexcept {
    return !username.empty() || !password.empty();
  }
};

struct AuthParseResult {
  ParseStatus status{ParseStatus::need_more};
  bool ok{false};
  std::size_t consumed{0};
};

struct TargetAddress {
  std::string host;
  std::uint16_t port{0};
};

struct RequestParseResult {
  ParseStatus status{ParseStatus::need_more};
  ReplyCode reply{ReplyCode::success};
  TargetAddress target;
  std::size_t consumed{0};
};

inline bool contains_method(std::string_view methods, AuthMethod method) {
  const auto value = static_cast<char>(static_cast<std::uint8_t>(method));
  return std::find(methods.begin(), methods.end(), value) != methods.end();
}

inline AuthMethod choose_auth_method(std::string_view methods, const AuthConfig& auth) {
  if (auth.enabled()) {
    return contains_method(methods, AuthMethod::username_password)
               ? AuthMethod::username_password
               : AuthMethod::no_acceptable_methods;
  }
  return contains_method(methods, AuthMethod::no_auth)
             ? AuthMethod::no_auth
             : AuthMethod::no_acceptable_methods;
}

inline AuthMethod choose_auth_method(const std::vector<std::uint8_t>& methods,
                                     const AuthConfig& auth) {
  return choose_auth_method(std::string_view(reinterpret_cast<const char*>(methods.data()),
                                             methods.size()),
                            auth);
}

inline std::vector<std::uint8_t> build_method_selection(AuthMethod method) {
  return {kVersion, static_cast<std::uint8_t>(method)};
}

inline std::vector<std::uint8_t> build_auth_status(bool ok) {
  return {kAuthVersion, static_cast<std::uint8_t>(ok ? 0x00 : 0x01)};
}

inline std::vector<std::uint8_t> build_reply(ReplyCode reply) {
  return {kVersion, static_cast<std::uint8_t>(reply), 0x00, 0x01, 0, 0, 0, 0, 0, 0};
}

inline AuthParseResult parse_user_password_auth(std::string_view data,
                                                const AuthConfig& auth) {
  if (data.size() < 2) {
    return {};
  }
  const auto version = static_cast<std::uint8_t>(data[0]);
  const auto username_len = static_cast<std::uint8_t>(data[1]);
  if (version != kAuthVersion) {
    return AuthParseResult{ParseStatus::error, false, 2};
  }
  const std::size_t password_len_index = 2 + username_len;
  if (data.size() <= password_len_index) {
    return {};
  }
  const auto password_len = static_cast<std::uint8_t>(data[password_len_index]);
  const std::size_t total = password_len_index + 1 + password_len;
  if (data.size() < total) {
    return {};
  }
  const std::string_view username(data.data() + 2, username_len);
  const std::string_view password(data.data() + password_len_index + 1, password_len);
  const bool ok = username == auth.username && password == auth.password;
  return AuthParseResult{ParseStatus::complete, ok, total};
}

inline AuthParseResult parse_user_password_auth(const std::vector<std::uint8_t>& data,
                                                const AuthConfig& auth) {
  return parse_user_password_auth(std::string_view(reinterpret_cast<const char*>(data.data()),
                                                   data.size()),
                                  auth);
}

inline std::uint16_t read_port(std::string_view data, std::size_t offset) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[offset])) << 8) |
      static_cast<std::uint8_t>(data[offset + 1]));
}

inline RequestParseResult parse_connect_request(std::string_view data) {
  if (data.size() < 4) {
    return {};
  }
  const auto version = static_cast<std::uint8_t>(data[0]);
  const auto command = static_cast<std::uint8_t>(data[1]);
  const auto address_type = static_cast<std::uint8_t>(data[3]);
  if (version != kVersion) {
    return RequestParseResult{ParseStatus::error, ReplyCode::general_failure, {}, 1};
  }
  if (command != 0x01) {
    return RequestParseResult{ParseStatus::error, ReplyCode::command_not_supported, {}, 2};
  }
  if (address_type == 0x01) {
    if (data.size() < 10) {
      return {};
    }
    std::string host = std::to_string(static_cast<std::uint8_t>(data[4])) + "." +
                       std::to_string(static_cast<std::uint8_t>(data[5])) + "." +
                       std::to_string(static_cast<std::uint8_t>(data[6])) + "." +
                       std::to_string(static_cast<std::uint8_t>(data[7]));
    return RequestParseResult{
        ParseStatus::complete,
        ReplyCode::success,
        TargetAddress{std::move(host), read_port(data, 8)},
        10};
  }
  if (address_type == 0x03) {
    const auto domain_len = static_cast<std::uint8_t>(data[4]);
    if (domain_len == 0) {
      return RequestParseResult{ParseStatus::error, ReplyCode::general_failure, {}, 5};
    }
    const std::size_t total = 5 + domain_len + 2;
    if (data.size() < total) {
      return {};
    }
    std::string host(data.data() + 5, domain_len);
    return RequestParseResult{
        ParseStatus::complete,
        ReplyCode::success,
        TargetAddress{std::move(host), read_port(data, 5 + domain_len)},
        total};
  }
  return RequestParseResult{ParseStatus::error, ReplyCode::addr_type_not_supported, {}, 4};
}

inline RequestParseResult parse_connect_request(const std::vector<std::uint8_t>& data) {
  return parse_connect_request(std::string_view(reinterpret_cast<const char*>(data.data()),
                                                data.size()));
}

}  // namespace oklib::examples::socks5
```

- [ ] **Step 2: Run the protocol helper test**

Run:

```bash
cmake --build build --target oklib_socks5_protocol_test
ctest --test-dir build -R oklib.example.socks5_protocol --output-on-failure
```

Expected: the test builds and passes.

- [ ] **Step 3: Commit the protocol helpers**

Run:

```bash
git add examples/socks5_protocol.h tests/examples/socks5_protocol_test.cpp tests/CMakeLists.txt
git commit -m "test: add socks5 protocol helper coverage"
```

---

### Task 3: Add The SOCKS5 Proxy Server Example

**Files:**
- Create: `examples/socks5_proxy_server.cpp`
- Modify: `examples/CMakeLists.txt`
- Use: `examples/socks5_protocol.h`

- [ ] **Step 1: Register the example target**

Modify `examples/CMakeLists.txt` after the TCP echo targets:

```cmake
add_executable(oklib_socks5_proxy_server socks5_proxy_server.cpp)
target_link_libraries(oklib_socks5_proxy_server PRIVATE oklib::net)
```

- [ ] **Step 2: Add the executable with option parsing and session skeleton**

Create `examples/socks5_proxy_server.cpp` with these top-level pieces:

```cpp
#include <arpa/inet.h>
#include <netdb.h>

#include <any>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "oklib/base/logging.h"
#include "oklib/net/buffer.h"
#include "oklib/net/event_loop.h"
#include "oklib/net/inet_address.h"
#include "oklib/net/tcp_client.h"
#include "oklib/net/tcp_connection.h"
#include "oklib/net/tcp_server.h"
#include "oklib/net/tls_options.h"
#include "socks5_protocol.h"

namespace {

struct Options {
  std::uint16_t port{1080};
  int io_threads{0};
  oklib::examples::socks5::AuthConfig auth;
  bool tls{false};
  std::string cert_file;
  std::string key_file;
};

void print_usage(const char* program) {
  std::cerr << "usage: " << program
            << " <port=1080> [username password] [--threads N] [--tls cert.pem key.pem]\n";
}

std::optional<Options> parse_options(int argc, char** argv) {
  Options options;
  int index = 1;
  if (index < argc && std::string_view(argv[index]).rfind("--", 0) != 0) {
    options.port = static_cast<std::uint16_t>(std::stoi(argv[index++]));
  }
  if (index + 1 < argc && std::string_view(argv[index]).rfind("--", 0) != 0 &&
      std::string_view(argv[index + 1]).rfind("--", 0) != 0) {
    options.auth.username = argv[index++];
    options.auth.password = argv[index++];
  }
  while (index < argc) {
    const std::string_view option(argv[index++]);
    if (option == "--threads" && index < argc) {
      options.io_threads = std::stoi(argv[index++]);
    } else if (option == "--tls" && index + 1 < argc) {
      options.tls = true;
      options.cert_file = argv[index++];
      options.key_file = argv[index++];
    } else {
      return std::nullopt;
    }
  }
  return options;
}
```

- [ ] **Step 3: Add IPv4/domain resolver for upstream targets**

In `examples/socks5_proxy_server.cpp`, add this helper inside the anonymous namespace:

```cpp
std::optional<oklib::net::InetAddress> resolve_ipv4(std::string_view host, std::uint16_t port) {
  std::string host_string(host);
  if (host_string == "localhost") {
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
  if (::getaddrinfo(host_string.c_str(), service.c_str(), &hints, &result) != 0) {
    return std::nullopt;
  }
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(result, ::freeaddrinfo);
  for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
    if (ai->ai_family == AF_INET && ai->ai_addrlen >= sizeof(sockaddr_in)) {
      return oklib::net::InetAddress(*reinterpret_cast<sockaddr_in*>(ai->ai_addr));
    }
  }
  return std::nullopt;
}
```

- [ ] **Step 4: Implement `Socks5ProxySession`**

Add this class inside the anonymous namespace. Use `force_close` after sending failure replies because the SOCKS5 exchange is complete for invalid requests.

```cpp
class Socks5ProxySession : public std::enable_shared_from_this<Socks5ProxySession> {
 public:
  Socks5ProxySession(oklib::net::EventLoop* loop,
                     oklib::net::TcpConnectionPtr downstream,
                     oklib::examples::socks5::AuthConfig auth)
      : loop_(loop), downstream_(std::move(downstream)), auth_(std::move(auth)) {}

  void on_downstream_message(oklib::net::Buffer* buffer) {
    if (phase_ == Phase::relay) {
      if (upstream_) {
        upstream_->send(buffer);
      } else {
        buffer->retrieve_all();
      }
      return;
    }

    while (buffer->readable_bytes() > 0 && phase_ != Phase::connecting && phase_ != Phase::closed) {
      if (phase_ == Phase::greeting && !handle_greeting(buffer)) {
        return;
      }
      if (phase_ == Phase::user_password && !handle_user_password(buffer)) {
        return;
      }
      if (phase_ == Phase::request && !handle_request(buffer)) {
        return;
      }
    }
  }

  void close() {
    phase_ = Phase::closed;
    if (upstream_client_) {
      upstream_client_->disconnect();
    }
    upstream_.reset();
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

  bool handle_greeting(oklib::net::Buffer* buffer) {
    if (buffer->readable_bytes() < 2) {
      return false;
    }
    const char* data = buffer->peek();
    if (static_cast<std::uint8_t>(data[0]) != oklib::examples::socks5::kVersion) {
      close_with_method(oklib::examples::socks5::AuthMethod::no_acceptable_methods);
      return false;
    }
    const auto method_count = static_cast<std::uint8_t>(data[1]);
    if (method_count == 0 || buffer->readable_bytes() < 2 + method_count) {
      return false;
    }
    buffer->retrieve(2);
    const std::string methods = buffer->retrieve_as_string(method_count);
    const auto method = oklib::examples::socks5::choose_auth_method(methods, auth_);
    send_bytes(oklib::examples::socks5::build_method_selection(method));
    if (method == oklib::examples::socks5::AuthMethod::no_acceptable_methods) {
      downstream_->force_close();
      phase_ = Phase::closed;
      return false;
    }
    phase_ = method == oklib::examples::socks5::AuthMethod::username_password
                 ? Phase::user_password
                 : Phase::request;
    return true;
  }

  bool handle_user_password(oklib::net::Buffer* buffer) {
    const std::string_view data(buffer->peek(), buffer->readable_bytes());
    const auto result = oklib::examples::socks5::parse_user_password_auth(data, auth_);
    if (result.status == oklib::examples::socks5::ParseStatus::need_more) {
      return false;
    }
    buffer->retrieve(result.consumed);
    send_bytes(oklib::examples::socks5::build_auth_status(result.ok));
    if (!result.ok) {
      downstream_->force_close();
      phase_ = Phase::closed;
      return false;
    }
    phase_ = Phase::request;
    return true;
  }

  bool handle_request(oklib::net::Buffer* buffer) {
    const std::string_view data(buffer->peek(), buffer->readable_bytes());
    const auto result = oklib::examples::socks5::parse_connect_request(data);
    if (result.status == oklib::examples::socks5::ParseStatus::need_more) {
      return false;
    }
    buffer->retrieve(result.consumed);
    if (result.status == oklib::examples::socks5::ParseStatus::error) {
      send_reply(result.reply);
      downstream_->force_close();
      phase_ = Phase::closed;
      return false;
    }

    auto address = resolve_ipv4(result.target.host, result.target.port);
    if (!address) {
      send_reply(oklib::examples::socks5::ReplyCode::general_failure);
      downstream_->force_close();
      phase_ = Phase::closed;
      return false;
    }

    pending_downstream_ = buffer->retrieve_all_as_string();
    downstream_->pause_reading();
    connect_upstream(*address, result.target.host, result.target.port);
    phase_ = Phase::connecting;
    return false;
  }

  void connect_upstream(const oklib::net::InetAddress& address,
                        std::string_view host,
                        std::uint16_t port) {
    auto self = shared_from_this();
    upstream_client_ = std::make_unique<oklib::net::TcpClient>(
        loop_, address, "socks5-upstream-" + std::string(host) + ":" + std::to_string(port));
    upstream_client_->set_connection_callback([self](const oklib::net::TcpConnectionPtr& connection) {
      self->on_upstream_connection(connection);
    });
    upstream_client_->set_message_callback([self](const oklib::net::TcpConnectionPtr&,
                                                  oklib::net::Buffer* buffer,
                                                  oklib::Timestamp) {
      self->on_upstream_message(buffer);
    });
    upstream_client_->connect();
  }

  void on_upstream_connection(const oklib::net::TcpConnectionPtr& connection) {
    if (phase_ == Phase::closed) {
      return;
    }
    if (connection->connected()) {
      upstream_ = connection;
      phase_ = Phase::relay;
      send_reply(oklib::examples::socks5::ReplyCode::success);
      if (!pending_downstream_.empty()) {
        upstream_->send(std::exchange(pending_downstream_, {}));
      }
      downstream_->resume_reading();
      return;
    }
    if (phase_ != Phase::closed) {
      phase_ = Phase::closed;
      if (downstream_ && downstream_->connected()) {
        downstream_->force_close();
      }
    }
  }

  void on_upstream_message(oklib::net::Buffer* buffer) {
    if (phase_ != Phase::relay || !downstream_ || !downstream_->connected()) {
      buffer->retrieve_all();
      return;
    }
    downstream_->send(buffer);
  }

  void close_with_method(oklib::examples::socks5::AuthMethod method) {
    send_bytes(oklib::examples::socks5::build_method_selection(method));
    downstream_->force_close();
    phase_ = Phase::closed;
  }

  void send_reply(oklib::examples::socks5::ReplyCode reply) {
    send_bytes(oklib::examples::socks5::build_reply(reply));
  }

  void send_bytes(const std::vector<std::uint8_t>& bytes) {
    downstream_->send(bytes.data(), bytes.size());
  }

  oklib::net::EventLoop* loop_;
  oklib::net::TcpConnectionPtr downstream_;
  oklib::examples::socks5::AuthConfig auth_;
  std::unique_ptr<oklib::net::TcpClient> upstream_client_;
  oklib::net::TcpConnectionPtr upstream_;
  std::string pending_downstream_;
  Phase phase_{Phase::greeting};
};
```

- [ ] **Step 5: Wire `TcpServer` callbacks and TLS option**

Add `main` below the class:

```cpp
int main(int argc, char** argv) {
  auto parsed = parse_options(argc, argv);
  if (!parsed) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }
  Options options = std::move(*parsed);

  if (options.tls) {
#if !OKLIB_ENABLE_TLS
    std::cerr << "TLS was requested, but oklib was built without OKLIB_ENABLE_TLS=ON\n";
    return EXIT_FAILURE;
#endif
  }

  oklib::net::EventLoop loop;
  oklib::net::TcpServer server(&loop, oklib::net::InetAddress::any(options.port), "socks5-proxy");
  server.set_thread_num(options.io_threads);
  if (options.tls) {
    oklib::net::TlsServerOptions tls;
    tls.enabled = true;
    tls.cert_file = options.cert_file;
    tls.key_file = options.key_file;
    server.set_tls_options(std::move(tls));
  }

  server.set_connection_callback([auth = options.auth, &loop](const oklib::net::TcpConnectionPtr& connection) {
    if (connection->connected()) {
      auto session = std::make_shared<Socks5ProxySession>(&loop, connection, auth);
      connection->set_context(session);
      OKLIB_LOG_INFO << "SOCKS5 downstream connected " << connection->peer_address().to_ip_port();
      return;
    }
    if (auto* session = std::any_cast<std::shared_ptr<Socks5ProxySession>>(
            &connection->mutable_context())) {
      if (*session) {
        (*session)->close();
      }
    }
    connection->set_context(std::any{});
    OKLIB_LOG_INFO << "SOCKS5 downstream disconnected " << connection->peer_address().to_ip_port();
  });

  server.set_message_callback([](const oklib::net::TcpConnectionPtr& connection,
                                 oklib::net::Buffer* buffer,
                                 oklib::Timestamp) {
    auto* session = std::any_cast<std::shared_ptr<Socks5ProxySession>>(
        &connection->mutable_context());
    if (session == nullptr || !*session) {
      buffer->retrieve_all();
      connection->force_close();
      return;
    }
    (*session)->on_downstream_message(buffer);
  });

  server.start();
  std::cout << "oklib SOCKS5 proxy listening on " << server.listen_address().to_ip_port()
            << " auth=" << (options.auth.enabled() ? "username/password" : "none")
            << " tls=" << (options.tls ? "on" : "off")
            << " io_threads=" << options.io_threads << '\n';
  loop.loop();
  return EXIT_SUCCESS;
}
```

- [ ] **Step 6: Build the example**

Run:

```bash
cmake --build build --target oklib_socks5_proxy_server
```

Expected: the target builds successfully.

- [ ] **Step 7: Commit the example**

Run:

```bash
git add examples/socks5_proxy_server.cpp examples/CMakeLists.txt
git commit -m "feat: add socks5 proxy server example"
```

---

### Task 4: Verify Normal SOCKS5 Behavior

**Files:**
- Use: `examples/socks5_proxy_server.cpp`
- Use: `examples/socks5_protocol.h`

- [ ] **Step 1: Run unit tests**

Run:

```bash
ctest --test-dir build -R oklib.example.socks5_protocol --output-on-failure
```

Expected: `oklib.example.socks5_protocol` passes.

- [ ] **Step 2: Build all examples**

Run:

```bash
cmake --build build --target oklib_socks5_proxy_server
```

Expected: `oklib_socks5_proxy_server` builds successfully.

- [ ] **Step 3: Manual no-auth proxy test**

Terminal A:

```bash
./build/examples/oklib_socks5_proxy_server 1080
```

Terminal B:

```bash
curl -v --proxy socks5h://127.0.0.1:1080 http://example.com/
```

Expected: curl receives an HTTP response from `example.com`. The `socks5h` scheme keeps domain resolution on the proxy side and verifies domain target parsing.

- [ ] **Step 4: Manual username/password proxy test**

Terminal A:

```bash
./build/examples/oklib_socks5_proxy_server 1081 alice secret
```

Terminal B:

```bash
curl -v --proxy socks5h://alice:secret@127.0.0.1:1081 http://example.com/
```

Expected: curl receives an HTTP response from `example.com`.

Terminal B wrong password check:

```bash
curl -v --proxy socks5h://alice:wrong@127.0.0.1:1081 http://example.com/
```

Expected: curl fails during SOCKS5 authentication.

- [ ] **Step 5: Manual IPv4 target test**

Run against a known IPv4 address:

```bash
curl -v --proxy socks5://127.0.0.1:1080 http://93.184.216.34/
```

Expected: the proxy parses an IPv4 SOCKS5 target and attempts the upstream connection. The exact HTTP response may vary because the request Host header is the IP address.

---

### Task 5: Verify Optional TLS Build Path

**Files:**
- Use: `examples/socks5_proxy_server.cpp`
- Use: `examples/certs/oklib_demo_server.crt`
- Use: `examples/certs/oklib_demo_server.key`

- [ ] **Step 1: Configure a TLS-enabled build**

Run:

```bash
cmake -S . -B build-tls -DOKLIB_ENABLE_TLS=ON -DOKLIB_BUILD_EXAMPLES=ON -DOKLIB_BUILD_TESTS=ON
```

Expected: CMake configures with OpenSSL found.

- [ ] **Step 2: Build the TLS-enabled SOCKS5 example**

Run:

```bash
cmake --build build-tls --target oklib_socks5_proxy_server
```

Expected: the target builds successfully.

- [ ] **Step 3: Start the TLS listener**

Run:

```bash
./build-tls/examples/oklib_socks5_proxy_server 1082 --tls examples/certs/oklib_demo_server.crt examples/certs/oklib_demo_server.key
```

Expected: the server prints `tls=on` and continues running.

- [ ] **Step 4: Check that the listener performs a TLS handshake**

Run from another terminal:

```bash
openssl s_client -connect 127.0.0.1:1082 -servername localhost -quiet
```

Expected: the TLS handshake succeeds and the process waits for SOCKS5 bytes. Full SOCKS5-over-TLS functional testing can be done with a client that sends SOCKS5 frames over an established TLS stream.

---

### Task 6: Final Verification

**Files:**
- Use: `examples/CMakeLists.txt`
- Use: `tests/CMakeLists.txt`
- Use: `examples/socks5_protocol.h`
- Use: `examples/socks5_proxy_server.cpp`
- Use: `tests/examples/socks5_protocol_test.cpp`

- [ ] **Step 1: Run focused build and test**

Run:

```bash
cmake --build build --target oklib_socks5_protocol_test oklib_socks5_proxy_server
ctest --test-dir build -R oklib.example.socks5_protocol --output-on-failure
```

Expected: both targets build and the SOCKS5 protocol test passes.

- [ ] **Step 2: Inspect git diff**

Run:

```bash
git diff -- examples/CMakeLists.txt tests/CMakeLists.txt examples/socks5_protocol.h examples/socks5_proxy_server.cpp tests/examples/socks5_protocol_test.cpp
```

Expected: the diff contains only the SOCKS5 protocol helper, SOCKS5 proxy example, and the two CMake registrations.

- [ ] **Step 3: Commit final verification updates if any**

Run only if Task 4 or Task 5 required small fixes:

```bash
git add examples/CMakeLists.txt tests/CMakeLists.txt examples/socks5_protocol.h examples/socks5_proxy_server.cpp tests/examples/socks5_protocol_test.cpp
git commit -m "fix: verify socks5 proxy example"
```

Expected: no commit is needed if Tasks 2 and 3 already passed without follow-up fixes.

---

## Self-Review

- Spec coverage: the plan covers IPv4 SOCKS5 CONNECT, domain SOCKS5 CONNECT, no-auth, username/password auth, CMake example registration, protocol unit testing, manual curl verification, and a TLS listener build path.
- Scope boundary: IPv6 is intentionally rejected with `AddrTypeNotSupported` because the current core networking layer is IPv4-only.
- Placeholder scan: the plan contains concrete file paths, commands, expected results, and code blocks for the new files and CMake changes.
- Type consistency: `AuthConfig`, `AuthMethod`, `ReplyCode`, `ParseStatus`, `TargetAddress`, and parser function names match across the helper header, test, and executable plan.
