#include "oklib/http/http_sync_client.h"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <exception>
#include <string_view>
#include <system_error>
#include <utility>

#include "oklib/net/event_loop.h"

namespace oklib::http {
namespace {

std::string to_lower(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (char c : value) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return lowered;
}

bool parse_port(std::string_view value, std::uint16_t* port) {
  if (value.empty()) {
    return false;
  }

  std::uint32_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed == 0 || parsed > 65535) {
    return false;
  }
  *port = static_cast<std::uint16_t>(parsed);
  return true;
}

std::string strip_fragment(std::string_view target) {
  const auto fragment = target.find('#');
  if (fragment != std::string_view::npos) {
    target = target.substr(0, fragment);
  }
  return std::string(target.empty() ? std::string_view("/") : target);
}

std::string host_for_connect(std::string_view host) {
  if (to_lower(host) == "localhost") {
    return "127.0.0.1";
  }
  return std::string(host);
}

}  // namespace

HttpSyncClient::HttpSyncClient(HttpSyncClientOptions options)
    : options_(std::move(options)) {}

HttpSyncClient::HttpSyncClient(const oklib::net::InetAddress& server_address,
                               std::string host,
                               HttpSyncClientOptions options)
    : options_(std::move(options)) {
  default_endpoint_ = Endpoint{server_address, std::move(host), {}, {}, false};
}

std::optional<HttpSyncClient::Endpoint> HttpSyncClient::parse_url(std::string_view url,
                                                                  int* error_code) {
  if (error_code != nullptr) {
    *error_code = static_cast<int>(HttpSyncClientError::invalid_url);
  }

  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos || scheme_end == 0) {
    return std::nullopt;
  }

  const auto scheme = to_lower(url.substr(0, scheme_end));
  const bool https = scheme == "https";
  if (scheme != "http" && !https) {
    if (error_code != nullptr) {
      *error_code = static_cast<int>(HttpSyncClientError::unsupported_scheme);
    }
    return std::nullopt;
  }

  std::string_view remainder = url.substr(scheme_end + 3);
  if (remainder.empty()) {
    return std::nullopt;
  }

  const auto target_start = remainder.find_first_of("/?#");
  const auto authority =
      target_start == std::string_view::npos ? remainder : remainder.substr(0, target_start);
  if (authority.empty() || authority.find('@') != std::string_view::npos ||
      authority.front() == '[') {
    if (error_code != nullptr) {
      *error_code = static_cast<int>(HttpSyncClientError::unsupported_host);
    }
    return std::nullopt;
  }

  std::string_view host = authority;
  std::uint16_t port = https ? 443 : 80;
  if (const auto colon = authority.find(':'); colon != std::string_view::npos) {
    if (authority.find(':', colon + 1) != std::string_view::npos ||
        colon == 0 ||
        colon + 1 == authority.size() ||
        !parse_port(authority.substr(colon + 1), &port)) {
      if (error_code != nullptr) {
        *error_code = static_cast<int>(HttpSyncClientError::invalid_url);
      }
      return std::nullopt;
    }
    host = authority.substr(0, colon);
  }

  std::string target = "/";
  if (target_start != std::string_view::npos) {
    const auto suffix = remainder.substr(target_start);
    if (!suffix.empty() && suffix.front() == '?') {
      target = "/" + strip_fragment(suffix);
    } else if (!suffix.empty() && suffix.front() == '#') {
      target = "/";
    } else {
      target = strip_fragment(suffix);
    }
  }

  const bool default_port = (!https && port == 80) || (https && port == 443);
  std::string host_header(host);
  if (!default_port) {
    host_header += ":" + std::to_string(port);
  }

  try {
    return Endpoint{oklib::net::InetAddress(host_for_connect(host), port),
                    std::move(host_header),
                    std::string(host),
                    std::move(target),
                    https};
  } catch (const std::exception&) {
    if (error_code != nullptr) {
      *error_code = static_cast<int>(HttpSyncClientError::unsupported_host);
    }
    return std::nullopt;
  }
}

std::optional<HttpSyncClient::Endpoint> HttpSyncClient::resolve_endpoint(
    const HttpClientRequest& request,
    int* error_code) const {
  if (!request.url().empty()) {
    return parse_url(request.url(), error_code);
  }

  if (!default_endpoint_.has_value()) {
    if (error_code != nullptr) {
      *error_code = static_cast<int>(HttpSyncClientError::invalid_url);
    }
    return std::nullopt;
  }

  Endpoint endpoint = *default_endpoint_;
  endpoint.target = request.target().empty() ? "/" : request.target();
  return endpoint;
}

int HttpSyncClient::send(HttpClientRequest* request, HttpResponseMessage* response) {
  if (request == nullptr || response == nullptr) {
    return static_cast<int>(HttpSyncClientError::null_argument);
  }
  if (oklib::net::EventLoop::current() != nullptr) {
    return static_cast<int>(HttpSyncClientError::called_from_event_loop);
  }

  int error_code = static_cast<int>(HttpSyncClientError::invalid_url);
  auto endpoint = resolve_endpoint(*request, &error_code);
  if (!endpoint.has_value()) {
    return error_code;
  }

#if !OKLIB_ENABLE_TLS
  if (endpoint->https) {
    return static_cast<int>(HttpSyncClientError::tls_not_enabled);
  }
#endif

  auto timeout = request->timeout().count() > 0 ? request->timeout() : options_.timeout;
  if (timeout.count() <= 0) {
    timeout = std::chrono::seconds(10);
  }

  HttpClientRequest owned_request = *request;
  if (!endpoint->target.empty()) {
    owned_request.set_target(endpoint->target);
  }

  HttpClientOptions client_options = options_.client_options;
#if OKLIB_ENABLE_TLS
  if (endpoint->https) {
    client_options.tls.enabled = true;
    if (client_options.tls.server_name.empty()) {
      client_options.tls.server_name = endpoint->host_name;
    }
  }
#endif

  oklib::net::EventLoop loop;
  HttpClient client(&loop,
                    endpoint->address,
                    endpoint->host_header,
                    "oklib-http-sync-client",
                    std::move(client_options));

  int result = static_cast<int>(HttpSyncClientError::timeout);
  bool completed = false;
  client.set_response_callback([&](HttpResponseMessage message) {
    if (completed) {
      return;
    }
    *response = std::move(message);
    completed = true;
    result = static_cast<int>(HttpSyncClientError::ok);
    client.disconnect();
    loop.quit();
  });
  client.set_error_callback([&] {
    if (completed) {
      return;
    }
    completed = true;
    result = static_cast<int>(HttpSyncClientError::request_failed);
    client.stop();
    loop.quit();
  });

  loop.run_after(timeout, [&] {
    if (completed) {
      return;
    }
    completed = true;
    result = static_cast<int>(HttpSyncClientError::timeout);
    client.stop();
    loop.quit();
  });
  client.send(std::move(owned_request));
  loop.loop();
  return result;
}

const char* http_sync_client_error_message(int code) noexcept {
  switch (static_cast<HttpSyncClientError>(code)) {
    case HttpSyncClientError::ok:
      return "ok";
    case HttpSyncClientError::null_argument:
      return "null argument";
    case HttpSyncClientError::invalid_url:
      return "invalid url";
    case HttpSyncClientError::unsupported_scheme:
      return "unsupported scheme";
    case HttpSyncClientError::unsupported_host:
      return "unsupported host";
    case HttpSyncClientError::timeout:
      return "timeout";
    case HttpSyncClientError::request_failed:
      return "request failed";
    case HttpSyncClientError::called_from_event_loop:
      return "called from event loop";
    case HttpSyncClientError::tls_not_enabled:
      return "tls not enabled";
  }
  return "unknown error";
}

}  // namespace oklib::http
