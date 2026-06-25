#include "oklib/http/http_client_cache.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace oklib::http {
namespace {

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

bool cacheable_method(const HttpClientRequest& request) {
  return equals_ignore_case(request.method(), "GET") || equals_ignore_case(request.method(), "HEAD");
}

CacheControl request_cache_control(const HttpClientRequest& request) {
  CacheControl result;
  for (const auto& value : request.headers().values("Cache-Control")) {
    const auto parsed = parse_cache_control(value);
    for (const auto& directive : parsed.directives()) {
      result.add(directive.name, directive.value);
    }
  }
  return result;
}

bool request_forces_revalidation(const HttpClientRequest& request) {
  const auto cache_control = request_cache_control(request);
  return cache_control.has("no-cache") || cache_control.delta_seconds("max-age") == std::chrono::seconds(0);
}

bool has_authorization(const HttpClientRequest& request) {
  return request.headers().contains("Authorization");
}

bool response_has_validator(const HttpResponseMessage& response) {
  return response.headers.contains("ETag") || response.headers.contains("Last-Modified");
}

void set_headers(HttpHeaders* destination, const HttpHeaders& source) {
  for (const auto& entry : source.entries()) {
    destination->set(entry.field, entry.value);
  }
}

}  // namespace

HttpClientCache::HttpClientCache(HttpClientCacheOptions options)
    : options_(options) {}

HttpClientCache::LookupResult HttpClientCache::lookup(std::string_view scheme,
                                                      std::string_view host,
                                                      const HttpClientRequest& request,
                                                      HttpTime now) {
  LookupResult result;
  if (!request_allows_cache_read(request)) {
    return result;
  }

  const auto key = primary_key(scheme, host, request);
  const bool force_revalidation = request_forces_revalidation(request);
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->primary_key != key || !vary_matches_request_headers(it->vary, it->request_headers, request.headers())) {
      continue;
    }

    entries_.splice(entries_.begin(), entries_, it);
    auto& entry = entries_.front();
    result.entry_id = entry.id;

    auto freshness = evaluate_cache_freshness(entry.response.headers, entry.stored_at, now);
    if (!force_revalidation && freshness.fresh) {
      result.state = LookupState::fresh;
      result.response = entry.response;
      result.response.source = HttpClientResponseSource::cache;
      return result;
    }

    if (response_has_validator(entry.response)) {
      result.state = LookupState::stale;
      result.validation_headers = make_cache_validation_headers(entry.response.headers);
      return result;
    }

    return result;
  }

  return result;
}

void HttpClientCache::store(std::string_view scheme,
                            std::string_view host,
                            const HttpClientRequest& request,
                            const HttpResponseMessage& response,
                            HttpTime now) {
  if (!request_allows_cache_write(request) || !response_allows_cache_write(response)) {
    return;
  }

  Entry entry;
  entry.id = next_id_++;
  entry.primary_key = primary_key(scheme, host, request);
  entry.request_headers = request.headers();
  entry.vary = parse_vary_header(response.headers.get("Vary"));
  if (entry.vary.any) {
    return;
  }
  entry.response = response;
  entry.response.source = HttpClientResponseSource::cache;
  entry.stored_at = now;
  entry.body_bytes = response.body.size();
  insert_or_replace(std::move(entry));
}

std::optional<HttpResponseMessage> HttpClientCache::update_from_304(std::size_t entry_id,
                                                                    const HttpResponseMessage& response,
                                                                    HttpTime now) {
  auto it = std::find_if(entries_.begin(), entries_.end(), [entry_id](const Entry& entry) {
    return entry.id == entry_id;
  });
  if (it == entries_.end()) {
    return std::nullopt;
  }

  entries_.splice(entries_.begin(), entries_, it);
  auto& entry = entries_.front();
  set_headers(&entry.response.headers, response.headers);
  entry.stored_at = now;

  HttpResponseMessage result = entry.response;
  result.source = HttpClientResponseSource::revalidated;
  return result;
}

void HttpClientCache::clear() noexcept {
  entries_.clear();
  total_body_bytes_ = 0;
}

std::string HttpClientCache::primary_key(std::string_view scheme,
                                         std::string_view host,
                                         const HttpClientRequest& request) const {
  std::string key;
  key.reserve(scheme.size() + host.size() + request.method().size() + request.target().size() + 8);
  key.append(scheme);
  key.append("://");
  key.append(host);
  key.push_back(' ');
  key.append(request.method());
  key.push_back(' ');
  key.append(request.target());
  return key;
}

bool HttpClientCache::request_allows_cache_read(const HttpClientRequest& request) const {
  if (!cacheable_method(request) || !request.body().empty() || has_authorization(request)) {
    return false;
  }
  const auto cache_control = request_cache_control(request);
  return !cache_control.has("no-store");
}

bool HttpClientCache::request_allows_cache_write(const HttpClientRequest& request) const {
  if (!cacheable_method(request) || !request.body().empty() || has_authorization(request)) {
    return false;
  }
  const auto cache_control = request_cache_control(request);
  return !cache_control.has("no-store");
}

bool HttpClientCache::response_allows_cache_write(const HttpResponseMessage& response) const {
  if (response.status_code != 200 || response.body.size() > options_.max_body_bytes ||
      response.body.size() > options_.max_total_body_bytes) {
    return false;
  }
  if (!cache_allows_storage(response.headers)) {
    return false;
  }
  const auto lifetime = cache_freshness_lifetime(response.headers, std::chrono::system_clock::now());
  return lifetime.has_value() || response_has_validator(response);
}

void HttpClientCache::insert_or_replace(Entry entry) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->primary_key == entry.primary_key &&
        vary_matches_request_headers(it->vary, it->request_headers, entry.request_headers)) {
      total_body_bytes_ -= it->body_bytes;
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }

  total_body_bytes_ += entry.body_bytes;
  entries_.push_front(std::move(entry));
  enforce_limits();
}

void HttpClientCache::enforce_limits() {
  while (!entries_.empty() &&
         (entries_.size() > options_.max_entries || total_body_bytes_ > options_.max_total_body_bytes)) {
    total_body_bytes_ -= entries_.back().body_bytes;
    entries_.pop_back();
  }
}

}  // namespace oklib::http
