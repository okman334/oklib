#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "oklib/http/http_request.h"

namespace oklib::http {

[[nodiscard]] std::string_view standard_method_token(HttpMethod method) noexcept;
[[nodiscard]] std::optional<HttpMethod> standard_method_from_token(std::string_view token);
[[nodiscard]] bool is_safe_method(HttpMethod method) noexcept;
[[nodiscard]] bool is_idempotent_method(HttpMethod method) noexcept;

[[nodiscard]] std::string_view standard_reason_phrase(int status_code) noexcept;
[[nodiscard]] bool is_standard_status_code(int status_code) noexcept;
[[nodiscard]] bool status_code_allows_body(int status_code) noexcept;

struct ByteRangeSpec {
  std::optional<std::uint64_t> first;
  std::optional<std::uint64_t> last;
  std::optional<std::uint64_t> suffix_length;
};

struct RangeSet {
  std::string unit;
  std::vector<ByteRangeSpec> ranges;
};

struct ContentRange {
  std::string unit;
  bool unsatisfied{false};
  std::optional<std::uint64_t> first;
  std::optional<std::uint64_t> last;
  std::optional<std::uint64_t> complete_length;
};

[[nodiscard]] std::optional<RangeSet> parse_range_header(std::string_view value);
[[nodiscard]] std::optional<ContentRange> parse_content_range_header(std::string_view value);

struct EntityTag {
  bool weak{false};
  std::string tag;
};

[[nodiscard]] std::optional<EntityTag> parse_entity_tag(std::string_view value);
[[nodiscard]] std::vector<EntityTag> parse_entity_tag_list(std::string_view value);
[[nodiscard]] bool strong_entity_tag_equal(const EntityTag& lhs, const EntityTag& rhs) noexcept;
[[nodiscard]] bool weak_entity_tag_equal(const EntityTag& lhs, const EntityTag& rhs) noexcept;
[[nodiscard]] bool if_none_match_matches(std::string_view header, const EntityTag& current);
[[nodiscard]] bool if_match_matches(std::string_view header, const EntityTag& current);

using HttpTime = std::chrono::system_clock::time_point;

[[nodiscard]] std::optional<HttpTime> parse_http_date(std::string_view value);
[[nodiscard]] std::string format_http_date(HttpTime time);
[[nodiscard]] bool if_modified_since_not_modified(std::string_view header, HttpTime last_modified);
[[nodiscard]] bool if_unmodified_since_allows(std::string_view header, HttpTime last_modified);

}  // namespace oklib::http
