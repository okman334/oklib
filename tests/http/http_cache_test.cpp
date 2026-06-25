#include <oklib/http/http_cache.h>
#include <oklib/http/http_headers.h>
#include <oklib/http/http_semantics.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

void test_cache_control_parser() {
  const auto cache_control =
      oklib::http::parse_cache_control("max-age=60, stale-while-revalidate=30, no-cache, PRIVATE=\"Set-Cookie\"");

  require(cache_control.has("MAX-AGE"), "cache-control lookup is case-insensitive");
  require(cache_control.value("private") == "Set-Cookie", "quoted directive value is unquoted");
  require(cache_control.delta_seconds("max-age") == std::chrono::seconds(60),
          "max-age delta-seconds parsed");
  require(cache_control.delta_seconds("stale-while-revalidate") == std::chrono::seconds(30),
          "extension delta-seconds parsed");
  require(cache_control.has("no-cache"), "valueless directive parsed");
  require(!cache_control.value("missing").has_value(), "missing directive has no value");

  require(oklib::http::parse_age_header("120") == std::chrono::seconds(120),
          "Age delta-seconds parsed");
  require(!oklib::http::parse_age_header("-1").has_value(), "negative Age rejected");
  require(!oklib::http::parse_age_header("12x").has_value(), "invalid Age rejected");
}

void test_expires_and_freshness() {
  using namespace std::chrono_literals;

  const auto stored_at = *oklib::http::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT");
  const auto expires_at = *oklib::http::parse_http_date("Sun, 06 Nov 1994 08:50:37 GMT");
  require(oklib::http::parse_expires_header("Sun, 06 Nov 1994 08:50:37 GMT") == expires_at,
          "Expires parses as HTTP date");
  require(!oklib::http::parse_expires_header("invalid").has_value(),
          "invalid Expires rejected");

  oklib::http::HttpHeaders response;
  response.add("Date", "Sun, 06 Nov 1994 08:49:37 GMT");
  response.add("Expires", "Sun, 06 Nov 1994 08:50:37 GMT");

  require(oklib::http::cache_freshness_lifetime(response, stored_at) == 60s,
          "Expires-Date freshness lifetime computed");

  response.set("Cache-Control", "max-age=30, s-maxage=10");
  require(oklib::http::cache_freshness_lifetime(response, stored_at) == 30s,
          "private cache prefers max-age");
  require(oklib::http::cache_freshness_lifetime(response,
                                               stored_at,
                                               oklib::http::CacheKind::shared) == 10s,
          "shared cache prefers s-maxage");

  response.set("Age", "5");
  const auto freshness =
      oklib::http::evaluate_cache_freshness(response, stored_at, stored_at + 20s);
  require(freshness.current_age == 25s, "Age plus resident time produces current age");
  require(freshness.fresh, "response is fresh inside max-age");

  const auto stale =
      oklib::http::evaluate_cache_freshness(response, stored_at, stored_at + 40s);
  require(!stale.fresh, "response is stale past max-age");

  response.set("Cache-Control", "no-cache, max-age=3600");
  require(oklib::http::cache_requires_validation(response), "no-cache requires validation");
  require(!oklib::http::evaluate_cache_freshness(response, stored_at, stored_at + 1s).fresh,
          "no-cache is not served fresh without validation");

  response.set("Cache-Control", "private");
  require(oklib::http::cache_allows_storage(response), "private cache may store private response");
  require(!oklib::http::cache_allows_storage(response, oklib::http::CacheKind::shared),
          "shared cache must not store private response");

  response.set("Cache-Control", "no-store");
  require(!oklib::http::cache_allows_storage(response), "no-store prevents storage");
}

void test_vary_and_validation_headers() {
  const auto vary = oklib::http::parse_vary_header("Accept-Encoding, User-Agent");
  require(!vary.any, "regular Vary is not wildcard");
  require(vary.fields.size() == 2, "Vary field count parsed");
  require(vary.fields[0] == "accept-encoding", "Vary fields are normalized");

  oklib::http::HttpHeaders cached_request;
  cached_request.add("Accept-Encoding", "gzip");
  cached_request.add("User-Agent", "oklib-test");

  oklib::http::HttpHeaders current_request;
  current_request.add("accept-encoding", "gzip");
  current_request.add("user-agent", "oklib-test");
  require(oklib::http::vary_matches_request_headers(vary, cached_request, current_request),
          "matching Vary request headers accepted");

  current_request.set("Accept-Encoding", "br");
  require(!oklib::http::vary_matches_request_headers(vary, cached_request, current_request),
          "different Vary request header rejected");

  const auto wildcard = oklib::http::parse_vary_header("*");
  require(wildcard.any, "Vary wildcard parsed");
  require(!oklib::http::vary_matches_request_headers(wildcard, cached_request, cached_request),
          "Vary wildcard never matches a stored response");

  oklib::http::HttpHeaders cached_response;
  cached_response.add("ETag", "\"abc\"");
  cached_response.add("Last-Modified", "Sun, 06 Nov 1994 08:49:37 GMT");

  const auto validators = oklib::http::make_cache_validation_headers(cached_response);
  require(validators.get("If-None-Match") == "\"abc\"", "ETag validator copied");
  require(validators.get("If-Modified-Since") == "Sun, 06 Nov 1994 08:49:37 GMT",
          "Last-Modified validator copied");
}

}  // namespace

int main() {
  test_cache_control_parser();
  test_expires_and_freshness();
  test_vary_and_validation_headers();
  return 0;
}
