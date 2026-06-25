#include "oklib/http/http_semantics.h"

#include <array>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace oklib::http {
namespace {

struct MethodInfo {
  HttpMethod method;
  std::string_view token;
  bool safe;
  bool idempotent;
};

constexpr auto k_methods = std::to_array<MethodInfo>({
    {HttpMethod::get, "GET", true, true},
    {HttpMethod::head, "HEAD", true, true},
    {HttpMethod::post, "POST", false, false},
    {HttpMethod::put, "PUT", false, true},
    {HttpMethod::delete_, "DELETE", false, true},
    {HttpMethod::options, "OPTIONS", true, true},
    {HttpMethod::trace, "TRACE", true, true},
    {HttpMethod::connect, "CONNECT", false, false},
    {HttpMethod::patch, "PATCH", false, false},
});

struct StatusInfo {
  int code;
  std::string_view reason;
};

constexpr auto k_statuses = std::to_array<StatusInfo>({
    {100, "Continue"},
    {101, "Switching Protocols"},
    {102, "Processing"},
    {103, "Early Hints"},
    {200, "OK"},
    {201, "Created"},
    {202, "Accepted"},
    {203, "Non-Authoritative Information"},
    {204, "No Content"},
    {205, "Reset Content"},
    {206, "Partial Content"},
    {207, "Multi-Status"},
    {208, "Already Reported"},
    {226, "IM Used"},
    {300, "Multiple Choices"},
    {301, "Moved Permanently"},
    {302, "Found"},
    {303, "See Other"},
    {304, "Not Modified"},
    {305, "Use Proxy"},
    {307, "Temporary Redirect"},
    {308, "Permanent Redirect"},
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {402, "Payment Required"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {406, "Not Acceptable"},
    {407, "Proxy Authentication Required"},
    {408, "Request Timeout"},
    {409, "Conflict"},
    {410, "Gone"},
    {411, "Length Required"},
    {412, "Precondition Failed"},
    {413, "Content Too Large"},
    {414, "URI Too Long"},
    {415, "Unsupported Media Type"},
    {416, "Range Not Satisfiable"},
    {417, "Expectation Failed"},
    {421, "Misdirected Request"},
    {422, "Unprocessable Content"},
    {423, "Locked"},
    {424, "Failed Dependency"},
    {425, "Too Early"},
    {426, "Upgrade Required"},
    {428, "Precondition Required"},
    {429, "Too Many Requests"},
    {431, "Request Header Fields Too Large"},
    {451, "Unavailable For Legal Reasons"},
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {502, "Bad Gateway"},
    {503, "Service Unavailable"},
    {504, "Gateway Timeout"},
    {505, "HTTP Version Not Supported"},
    {506, "Variant Also Negotiates"},
    {507, "Insufficient Storage"},
    {508, "Loop Detected"},
    {510, "Not Extended"},
    {511, "Network Authentication Required"},
});

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

std::vector<std::string_view> split_commas(std::string_view value) {
  std::vector<std::string_view> result;
  while (!value.empty()) {
    const auto comma = value.find(',');
    if (comma == std::string_view::npos) {
      result.push_back(trim(value));
      break;
    }
    result.push_back(trim(value.substr(0, comma)));
    value.remove_prefix(comma + 1);
  }
  return result;
}

bool parse_u64(std::string_view value, std::uint64_t* output) {
  value = trim(value);
  if (value.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed, 10);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  *output = parsed;
  return true;
}

bool parse_length_or_star(std::string_view value, std::optional<std::uint64_t>* output) {
  value = trim(value);
  if (value == "*") {
    output->reset();
    return true;
  }
  std::uint64_t parsed = 0;
  if (!parse_u64(value, &parsed)) {
    return false;
  }
  *output = parsed;
  return true;
}

int month_index(std::string_view month) {
  constexpr std::array<std::string_view, 12> months{
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (std::size_t i = 0; i < months.size(); ++i) {
    if (months[i] == month) {
      return static_cast<int>(i) + 1;
    }
  }
  return 0;
}

int64_t days_from_civil(int year, unsigned month, unsigned day) noexcept {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const auto yoe = static_cast<unsigned>(year - era * 400);
  const auto doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

std::optional<HttpTime> make_time_utc(int year, unsigned month, unsigned day,
                                      unsigned hour, unsigned minute, unsigned second) {
  if (month < 1 || month > 12 || day < 1 || day > 31 ||
      hour > 23 || minute > 59 || second > 60) {
    return std::nullopt;
  }
  const auto days = days_from_civil(year, month, day);
  const auto seconds = days * 86400 + static_cast<int64_t>(hour) * 3600 +
                       static_cast<int64_t>(minute) * 60 + second;
  return HttpTime(std::chrono::seconds(seconds));
}

}  // namespace

std::string_view standard_method_token(HttpMethod method) noexcept {
  for (const auto& info : k_methods) {
    if (info.method == method) {
      return info.token;
    }
  }
  return {};
}

std::optional<HttpMethod> standard_method_from_token(std::string_view token) {
  for (const auto& info : k_methods) {
    if (info.token == token) {
      return info.method;
    }
  }
  return std::nullopt;
}

bool is_safe_method(HttpMethod method) noexcept {
  for (const auto& info : k_methods) {
    if (info.method == method) {
      return info.safe;
    }
  }
  return false;
}

bool is_idempotent_method(HttpMethod method) noexcept {
  for (const auto& info : k_methods) {
    if (info.method == method) {
      return info.idempotent;
    }
  }
  return false;
}

std::string_view standard_reason_phrase(int status_code) noexcept {
  for (const auto& info : k_statuses) {
    if (info.code == status_code) {
      return info.reason;
    }
  }
  return {};
}

bool is_standard_status_code(int status_code) noexcept {
  return !standard_reason_phrase(status_code).empty();
}

bool status_code_allows_body(int status_code) noexcept {
  return status_code >= 200 && status_code != 204 && status_code != 205 && status_code != 304;
}

std::optional<RangeSet> parse_range_header(std::string_view value) {
  const auto equals = value.find('=');
  if (equals == std::string_view::npos || equals == 0) {
    return std::nullopt;
  }

  RangeSet result;
  result.unit = std::string(trim(value.substr(0, equals)));
  if (result.unit.empty()) {
    return std::nullopt;
  }

  for (const auto part : split_commas(value.substr(equals + 1))) {
    const auto dash = part.find('-');
    if (dash == std::string_view::npos) {
      return std::nullopt;
    }

    const auto first_part = trim(part.substr(0, dash));
    const auto last_part = trim(part.substr(dash + 1));
    ByteRangeSpec spec;
    if (first_part.empty()) {
      std::uint64_t suffix = 0;
      if (!parse_u64(last_part, &suffix) || suffix == 0) {
        return std::nullopt;
      }
      spec.suffix_length = suffix;
    } else {
      std::uint64_t first = 0;
      if (!parse_u64(first_part, &first)) {
        return std::nullopt;
      }
      spec.first = first;
      if (!last_part.empty()) {
        std::uint64_t last = 0;
        if (!parse_u64(last_part, &last) || last < first) {
          return std::nullopt;
        }
        spec.last = last;
      }
    }
    result.ranges.push_back(std::move(spec));
  }

  if (result.ranges.empty()) {
    return std::nullopt;
  }
  return result;
}

std::optional<ContentRange> parse_content_range_header(std::string_view value) {
  const auto space = value.find(' ');
  if (space == std::string_view::npos || space == 0) {
    return std::nullopt;
  }

  ContentRange result;
  result.unit = std::string(value.substr(0, space));
  const auto rest = trim(value.substr(space + 1));
  const auto slash = rest.find('/');
  if (slash == std::string_view::npos) {
    return std::nullopt;
  }

  const auto range_part = trim(rest.substr(0, slash));
  const auto length_part = trim(rest.substr(slash + 1));
  if (!parse_length_or_star(length_part, &result.complete_length)) {
    return std::nullopt;
  }

  if (range_part == "*") {
    result.unsatisfied = true;
    return result;
  }

  const auto dash = range_part.find('-');
  if (dash == std::string_view::npos) {
    return std::nullopt;
  }
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  if (!parse_u64(range_part.substr(0, dash), &first) ||
      !parse_u64(range_part.substr(dash + 1), &last) ||
      last < first) {
    return std::nullopt;
  }
  if (result.complete_length.has_value() && last >= *result.complete_length) {
    return std::nullopt;
  }
  result.first = first;
  result.last = last;
  return result;
}

std::optional<EntityTag> parse_entity_tag(std::string_view value) {
  value = trim(value);
  EntityTag tag;
  if (value.starts_with("W/")) {
    tag.weak = true;
    value.remove_prefix(2);
  }
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return std::nullopt;
  }
  value.remove_prefix(1);
  value.remove_suffix(1);
  tag.tag.assign(value);
  return tag;
}

std::vector<EntityTag> parse_entity_tag_list(std::string_view value) {
  std::vector<EntityTag> tags;
  for (const auto part : split_commas(value)) {
    if (part == "*") {
      tags.push_back(EntityTag{false, "*"});
      continue;
    }
    auto tag = parse_entity_tag(part);
    if (tag.has_value()) {
      tags.push_back(std::move(*tag));
    }
  }
  return tags;
}

bool strong_entity_tag_equal(const EntityTag& lhs, const EntityTag& rhs) noexcept {
  return !lhs.weak && !rhs.weak && lhs.tag == rhs.tag;
}

bool weak_entity_tag_equal(const EntityTag& lhs, const EntityTag& rhs) noexcept {
  return lhs.tag == rhs.tag;
}

bool if_none_match_matches(std::string_view header, const EntityTag& current) {
  if (trim(header) == "*") {
    return true;
  }
  for (const auto& tag : parse_entity_tag_list(header)) {
    if (weak_entity_tag_equal(tag, current)) {
      return true;
    }
  }
  return false;
}

bool if_match_matches(std::string_view header, const EntityTag& current) {
  if (trim(header) == "*") {
    return true;
  }
  for (const auto& tag : parse_entity_tag_list(header)) {
    if (strong_entity_tag_equal(tag, current)) {
      return true;
    }
  }
  return false;
}

std::optional<HttpTime> parse_http_date(std::string_view value) {
  value = trim(value);
  // IMF-fixdate: Sun, 06 Nov 1994 08:49:37 GMT
  if (value.size() != 29 || value.substr(3, 2) != ", " || value.substr(26, 3) != "GMT") {
    return std::nullopt;
  }

  std::uint64_t day = 0;
  std::uint64_t year = 0;
  std::uint64_t hour = 0;
  std::uint64_t minute = 0;
  std::uint64_t second = 0;
  if (!parse_u64(value.substr(5, 2), &day) ||
      !parse_u64(value.substr(12, 4), &year) ||
      !parse_u64(value.substr(17, 2), &hour) ||
      !parse_u64(value.substr(20, 2), &minute) ||
      !parse_u64(value.substr(23, 2), &second)) {
    return std::nullopt;
  }
  const int month = month_index(value.substr(8, 3));
  if (month == 0) {
    return std::nullopt;
  }
  return make_time_utc(static_cast<int>(year), static_cast<unsigned>(month),
                       static_cast<unsigned>(day), static_cast<unsigned>(hour),
                       static_cast<unsigned>(minute), static_cast<unsigned>(second));
}

std::string format_http_date(HttpTime time) {
  const std::time_t value = std::chrono::system_clock::to_time_t(time);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &value);
#else
  gmtime_r(&value, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
  return out.str();
}

bool if_modified_since_not_modified(std::string_view header, HttpTime last_modified) {
  const auto since = parse_http_date(header);
  if (!since.has_value()) {
    return false;
  }
  const auto last_seconds = std::chrono::time_point_cast<std::chrono::seconds>(last_modified);
  const auto since_seconds = std::chrono::time_point_cast<std::chrono::seconds>(*since);
  return last_seconds <= since_seconds;
}

bool if_unmodified_since_allows(std::string_view header, HttpTime last_modified) {
  const auto since = parse_http_date(header);
  if (!since.has_value()) {
    return true;
  }
  const auto last_seconds = std::chrono::time_point_cast<std::chrono::seconds>(last_modified);
  const auto since_seconds = std::chrono::time_point_cast<std::chrono::seconds>(*since);
  return last_seconds <= since_seconds;
}

}  // namespace oklib::http
