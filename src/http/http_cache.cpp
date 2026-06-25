#include "oklib/http/http_cache.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <utility>

namespace oklib::http {
namespace {

bool is_ows(char c) noexcept {
  return c == ' ' || c == '\t';
}

std::string_view trim(std::string_view value) {
  while (!value.empty() && is_ows(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_ows(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

std::string lower_ascii(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char c : value) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return result;
}

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    const auto l = static_cast<unsigned char>(lhs[i]);
    const auto r = static_cast<unsigned char>(rhs[i]);
    if (std::tolower(l) != std::tolower(r)) {
      return false;
    }
  }
  return true;
}

std::vector<std::string_view> split_commas(std::string_view value) {
  std::vector<std::string_view> result;
  bool quoted = false;
  bool escaped = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (quoted && c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      quoted = !quoted;
      continue;
    }
    if (!quoted && c == ',') {
      result.push_back(trim(value.substr(start, i - start)));
      start = i + 1;
    }
  }
  result.push_back(trim(value.substr(start)));
  return result;
}

std::optional<std::string> unquote(std::string_view value) {
  value = trim(value);
  if (value.empty()) {
    return std::string{};
  }
  if (value.front() != '"') {
    return std::string(value);
  }
  if (value.size() < 2 || value.back() != '"') {
    return std::nullopt;
  }

  value.remove_prefix(1);
  value.remove_suffix(1);
  std::string result;
  result.reserve(value.size());
  bool escaped = false;
  for (const char c : value) {
    if (escaped) {
      result.push_back(c);
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    result.push_back(c);
  }
  if (escaped) {
    return std::nullopt;
  }
  return result;
}

CacheControl cache_control_from_headers(const HttpHeaders& headers) {
  CacheControl result;
  for (const auto& value : headers.values("Cache-Control")) {
    const auto parsed = parse_cache_control(value);
    for (const auto& directive : parsed.directives()) {
      result.add(directive.name, directive.value);
    }
  }
  return result;
}

std::chrono::seconds clamp_nonnegative_seconds(HttpTime::duration duration) {
  if (duration <= HttpTime::duration::zero()) {
    return std::chrono::seconds(0);
  }
  return std::chrono::duration_cast<std::chrono::seconds>(duration);
}

}  // namespace

void CacheControl::add(std::string name, std::optional<std::string> value) {
  directives_.push_back({lower_ascii(name), std::move(value)});
}

bool CacheControl::has(std::string_view name) const {
  return std::any_of(directives_.begin(), directives_.end(), [name](const auto& directive) {
    return equals_ignore_case(directive.name, name);
  });
}

std::optional<std::string> CacheControl::value(std::string_view name) const {
  for (const auto& directive : directives_) {
    if (equals_ignore_case(directive.name, name)) {
      return directive.value;
    }
  }
  return std::nullopt;
}

std::vector<std::string> CacheControl::values(std::string_view name) const {
  std::vector<std::string> result;
  for (const auto& directive : directives_) {
    if (equals_ignore_case(directive.name, name) && directive.value.has_value()) {
      result.push_back(*directive.value);
    }
  }
  return result;
}

std::optional<std::chrono::seconds> CacheControl::delta_seconds(std::string_view name) const {
  const auto raw = value(name);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  return parse_delta_seconds(*raw);
}

CacheControl parse_cache_control(std::string_view value) {
  CacheControl result;
  for (const auto part : split_commas(value)) {
    if (part.empty()) {
      continue;
    }

    const auto equals = part.find('=');
    const auto name = trim(equals == std::string_view::npos ? part : part.substr(0, equals));
    if (name.empty()) {
      continue;
    }

    if (equals == std::string_view::npos) {
      result.add(std::string(name), std::nullopt);
      continue;
    }

    const auto maybe_value = unquote(part.substr(equals + 1));
    if (!maybe_value.has_value()) {
      continue;
    }
    result.add(std::string(name), *maybe_value);
  }
  return result;
}

std::optional<std::chrono::seconds> parse_delta_seconds(std::string_view value) {
  value = trim(value);
  if (value.empty() || value.front() == '-') {
    return std::nullopt;
  }

  std::uint64_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed, 10);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  const auto max_seconds =
      static_cast<std::uint64_t>(std::numeric_limits<std::chrono::seconds::rep>::max());
  if (parsed > max_seconds) {
    return std::nullopt;
  }
  return std::chrono::seconds(static_cast<std::chrono::seconds::rep>(parsed));
}

std::optional<std::chrono::seconds> parse_age_header(std::string_view value) {
  return parse_delta_seconds(value);
}

std::optional<HttpTime> parse_expires_header(std::string_view value) {
  return parse_http_date(value);
}

VaryFields parse_vary_header(std::string_view value) {
  VaryFields result;
  for (const auto field : split_commas(value)) {
    if (field.empty()) {
      continue;
    }
    if (field == "*") {
      result.any = true;
      result.fields.clear();
      return result;
    }
    result.fields.push_back(lower_ascii(field));
  }
  return result;
}

bool vary_matches_request_headers(const VaryFields& vary,
                                  const HttpHeaders& cached_request_headers,
                                  const HttpHeaders& current_request_headers) {
  if (vary.any) {
    return false;
  }
  for (const auto& field : vary.fields) {
    if (cached_request_headers.values(field) != current_request_headers.values(field)) {
      return false;
    }
  }
  return true;
}

std::chrono::seconds cache_current_age(const HttpHeaders& response_headers,
                                       HttpTime response_time,
                                       HttpTime now) {
  const auto age_value = parse_age_header(response_headers.get("Age")).value_or(std::chrono::seconds(0));
  return age_value + clamp_nonnegative_seconds(now - response_time);
}

std::optional<std::chrono::seconds> cache_freshness_lifetime(const HttpHeaders& response_headers,
                                                            HttpTime response_time,
                                                            CacheKind kind) {
  const auto cache_control = cache_control_from_headers(response_headers);
  if (kind == CacheKind::shared) {
    if (const auto s_maxage = cache_control.delta_seconds("s-maxage"); s_maxage.has_value()) {
      return s_maxage;
    }
  }
  if (const auto max_age = cache_control.delta_seconds("max-age"); max_age.has_value()) {
    return max_age;
  }

  const auto expires = parse_expires_header(response_headers.get("Expires"));
  if (!expires.has_value()) {
    return std::nullopt;
  }
  const auto date = parse_http_date(response_headers.get("Date")).value_or(response_time);
  return clamp_nonnegative_seconds(*expires - date);
}

bool cache_requires_validation(const HttpHeaders& response_headers, CacheKind kind) {
  (void)kind;
  const auto cache_control = cache_control_from_headers(response_headers);
  return cache_control.has("no-cache");
}

bool cache_allows_storage(const HttpHeaders& response_headers, CacheKind kind) {
  const auto cache_control = cache_control_from_headers(response_headers);
  if (cache_control.has("no-store")) {
    return false;
  }
  return kind != CacheKind::shared || !cache_control.has("private");
}

CacheFreshness evaluate_cache_freshness(const HttpHeaders& response_headers,
                                        HttpTime response_time,
                                        HttpTime now,
                                        CacheKind kind) {
  CacheFreshness result;
  result.current_age = cache_current_age(response_headers, response_time, now);
  result.freshness_lifetime = cache_freshness_lifetime(response_headers, response_time, kind);
  result.requires_validation = cache_requires_validation(response_headers, kind);
  result.fresh = cache_allows_storage(response_headers, kind) && !result.requires_validation &&
                 result.freshness_lifetime.has_value() &&
                 result.current_age < *result.freshness_lifetime;
  return result;
}

HttpHeaders make_cache_validation_headers(const HttpHeaders& cached_response_headers) {
  HttpHeaders result;
  const auto etag = cached_response_headers.get("ETag");
  if (!etag.empty()) {
    result.add("If-None-Match", etag);
  }
  const auto last_modified = cached_response_headers.get("Last-Modified");
  if (!last_modified.empty()) {
    result.add("If-Modified-Since", last_modified);
  }
  return result;
}

}  // namespace oklib::http
