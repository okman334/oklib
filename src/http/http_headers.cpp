#include "oklib/http/http_headers.h"

#include <algorithm>
#include <cctype>

namespace oklib::http {
namespace {

bool is_ows(char c) noexcept {
  return c == ' ' || c == '\t';
}

std::string trim(std::string_view value) {
  while (!value.empty() && is_ows(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_ows(value.back())) {
    value.remove_suffix(1);
  }
  return std::string(value);
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

}  // namespace

void HttpHeaders::add(std::string_view field, std::string_view value) {
  entries_.push_back({trim(field), trim(value)});
}

void HttpHeaders::set(std::string_view field, std::string_view value) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [field](const Entry& entry) {
                                  return equals_ignore_case(entry.field, field);
                                }),
                 entries_.end());
  add(field, value);
}

bool HttpHeaders::contains(std::string_view field) const {
  return std::any_of(entries_.begin(), entries_.end(), [field](const Entry& entry) {
    return equals_ignore_case(entry.field, field);
  });
}

std::string HttpHeaders::get(std::string_view field) const {
  auto it = std::find_if(entries_.begin(), entries_.end(), [field](const Entry& entry) {
    return equals_ignore_case(entry.field, field);
  });
  return it == entries_.end() ? std::string{} : it->value;
}

std::vector<std::string> HttpHeaders::values(std::string_view field) const {
  std::vector<std::string> result;
  for (const auto& entry : entries_) {
    if (equals_ignore_case(entry.field, field)) {
      result.push_back(entry.value);
    }
  }
  return result;
}

}  // namespace oklib::http
