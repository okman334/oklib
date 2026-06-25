#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "oklib/http/http_headers.h"
#include "oklib/http/http_semantics.h"

namespace oklib::http {

enum class CacheKind {
  private_,
  shared,
};

struct CacheControlDirective {
  std::string name;
  std::optional<std::string> value;
};

class CacheControl {
 public:
  void add(std::string name, std::optional<std::string> value);

  [[nodiscard]] const std::vector<CacheControlDirective>& directives() const noexcept {
    return directives_;
  }
  [[nodiscard]] bool has(std::string_view name) const;
  [[nodiscard]] std::optional<std::string> value(std::string_view name) const;
  [[nodiscard]] std::vector<std::string> values(std::string_view name) const;
  [[nodiscard]] std::optional<std::chrono::seconds> delta_seconds(std::string_view name) const;

 private:
  std::vector<CacheControlDirective> directives_;
};

[[nodiscard]] CacheControl parse_cache_control(std::string_view value);
[[nodiscard]] std::optional<std::chrono::seconds> parse_delta_seconds(std::string_view value);
[[nodiscard]] std::optional<std::chrono::seconds> parse_age_header(std::string_view value);
[[nodiscard]] std::optional<HttpTime> parse_expires_header(std::string_view value);

struct VaryFields {
  bool any{false};
  std::vector<std::string> fields;
};

[[nodiscard]] VaryFields parse_vary_header(std::string_view value);
[[nodiscard]] bool vary_matches_request_headers(const VaryFields& vary,
                                                const HttpHeaders& cached_request_headers,
                                                const HttpHeaders& current_request_headers);

struct CacheFreshness {
  std::chrono::seconds current_age{0};
  std::optional<std::chrono::seconds> freshness_lifetime;
  bool requires_validation{false};
  bool fresh{false};
};

[[nodiscard]] std::chrono::seconds cache_current_age(const HttpHeaders& response_headers,
                                                     HttpTime response_time,
                                                     HttpTime now);
[[nodiscard]] std::optional<std::chrono::seconds> cache_freshness_lifetime(
    const HttpHeaders& response_headers,
    HttpTime response_time,
    CacheKind kind = CacheKind::private_);
[[nodiscard]] bool cache_requires_validation(const HttpHeaders& response_headers,
                                             CacheKind kind = CacheKind::private_);
[[nodiscard]] bool cache_allows_storage(const HttpHeaders& response_headers,
                                        CacheKind kind = CacheKind::private_);
[[nodiscard]] CacheFreshness evaluate_cache_freshness(const HttpHeaders& response_headers,
                                                      HttpTime response_time,
                                                      HttpTime now,
                                                      CacheKind kind = CacheKind::private_);
[[nodiscard]] HttpHeaders make_cache_validation_headers(const HttpHeaders& cached_response_headers);

}  // namespace oklib::http
