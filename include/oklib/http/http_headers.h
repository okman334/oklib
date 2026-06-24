#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace oklib::http {

class HttpHeaders {
 public:
  struct Entry {
    std::string field;
    std::string value;
  };

  void add(std::string_view field, std::string_view value);
  void set(std::string_view field, std::string_view value);
  void clear() noexcept { entries_.clear(); }

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool contains(std::string_view field) const;
  [[nodiscard]] std::string get(std::string_view field) const;
  [[nodiscard]] std::vector<std::string> values(std::string_view field) const;
  [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

 private:
  std::vector<Entry> entries_;
};

}  // namespace oklib::http
