#pragma once

#include <chrono>
#include <cstddef>
#include <list>
#include <optional>
#include <string>
#include <string_view>

#include "oklib/http/http_cache.h"
#include "oklib/http/http_client.h"
#include "oklib/http/http_headers.h"
#include "oklib/http/http_parser.h"

namespace oklib::http {

struct HttpClientCacheOptions {
  std::size_t max_entries{1024};
  std::size_t max_body_bytes{1024 * 1024};
  std::size_t max_total_body_bytes{64 * 1024 * 1024};
};

class HttpClientCache {
 public:
  enum class LookupState {
    miss,
    fresh,
    stale,
  };

  struct LookupResult {
    LookupState state{LookupState::miss};
    std::optional<std::size_t> entry_id;
    HttpResponseMessage response;
    HttpHeaders validation_headers;
  };

  explicit HttpClientCache(HttpClientCacheOptions options = {});

  [[nodiscard]] LookupResult lookup(std::string_view scheme,
                                    std::string_view host,
                                    const HttpClientRequest& request,
                                    HttpTime now);
  void store(std::string_view scheme,
             std::string_view host,
             const HttpClientRequest& request,
             const HttpResponseMessage& response,
             HttpTime now);
  [[nodiscard]] std::optional<HttpResponseMessage> update_from_304(std::size_t entry_id,
                                                                   const HttpResponseMessage& response,
                                                                   HttpTime now);

  void clear() noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

 private:
  struct Entry {
    std::size_t id{0};
    std::string primary_key;
    HttpHeaders request_headers;
    VaryFields vary;
    HttpResponseMessage response;
    HttpTime stored_at;
    std::size_t body_bytes{0};
  };

  using EntryList = std::list<Entry>;

  [[nodiscard]] std::string primary_key(std::string_view scheme,
                                        std::string_view host,
                                        const HttpClientRequest& request) const;
  [[nodiscard]] bool request_allows_cache_read(const HttpClientRequest& request) const;
  [[nodiscard]] bool request_allows_cache_write(const HttpClientRequest& request) const;
  [[nodiscard]] bool response_allows_cache_write(const HttpResponseMessage& response) const;
  void insert_or_replace(Entry entry);
  void enforce_limits();

  HttpClientCacheOptions options_;
  EntryList entries_;
  std::size_t next_id_{1};
  std::size_t total_body_bytes_{0};
};

}  // namespace oklib::http
