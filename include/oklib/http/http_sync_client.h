#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "oklib/base/noncopyable.h"
#include "oklib/http/http_client.h"
#include "oklib/net/inet_address.h"

namespace oklib::http {

enum class HttpSyncClientError : int {
  ok = 0,
  null_argument = -1,
  invalid_url = -2,
  unsupported_scheme = -3,
  unsupported_host = -4,
  timeout = -5,
  request_failed = -6,
  called_from_event_loop = -7,
  tls_not_enabled = -8,
};

struct HttpSyncClientOptions {
  HttpClientOptions client_options;
  std::chrono::milliseconds timeout{std::chrono::seconds(10)};
};

class HttpSyncClient : private oklib::Noncopyable {
 public:
  explicit HttpSyncClient(HttpSyncClientOptions options = {});
  HttpSyncClient(const oklib::net::InetAddress& server_address,
                 std::string host,
                 HttpSyncClientOptions options = {});

  int send(HttpClientRequest* request, HttpResponseMessage* response);

  void set_timeout(std::chrono::milliseconds timeout) { options_.timeout = timeout; }
  [[nodiscard]] std::chrono::milliseconds timeout() const noexcept { return options_.timeout; }

 private:
  struct Endpoint {
    oklib::net::InetAddress address;
    std::string host_header;
    std::string host_name;
    std::string target;
    bool https{false};
  };

  static std::optional<Endpoint> parse_url(std::string_view url, int* error_code);
  [[nodiscard]] std::optional<Endpoint> resolve_endpoint(const HttpClientRequest& request,
                                                         int* error_code) const;

  HttpSyncClientOptions options_;
  std::optional<Endpoint> default_endpoint_;
};

[[nodiscard]] const char* http_sync_client_error_message(int code) noexcept;

}  // namespace oklib::http
