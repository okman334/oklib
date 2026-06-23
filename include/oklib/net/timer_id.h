#pragma once

#include <cstdint>

namespace oklib::net {

class TimerId {
 public:
  constexpr TimerId() noexcept = default;
  explicit constexpr TimerId(uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(TimerId lhs, TimerId rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }

 private:
  uint64_t value_{0};
};

}  // namespace oklib::net
