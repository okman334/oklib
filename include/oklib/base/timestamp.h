#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace oklib {

class Timestamp {
 public:
  using clock = std::chrono::system_clock;
  static constexpr int64_t k_microseconds_per_second = 1000 * 1000;

  constexpr Timestamp() noexcept = default;
  explicit constexpr Timestamp(int64_t microseconds_since_epoch) noexcept
      : microseconds_since_epoch_(microseconds_since_epoch) {}

  static Timestamp now() {
    const auto now = clock::now().time_since_epoch();
    return Timestamp(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
  }

  static constexpr Timestamp invalid() noexcept { return Timestamp(); }

  static constexpr Timestamp from_unix_time(std::time_t seconds, int microseconds = 0) noexcept {
    return Timestamp(static_cast<int64_t>(seconds) * k_microseconds_per_second + microseconds);
  }

  [[nodiscard]] constexpr bool valid() const noexcept { return microseconds_since_epoch_ > 0; }
  [[nodiscard]] constexpr int64_t microseconds_since_epoch() const noexcept {
    return microseconds_since_epoch_;
  }
  [[nodiscard]] constexpr std::time_t seconds_since_epoch() const noexcept {
    return static_cast<std::time_t>(microseconds_since_epoch_ / k_microseconds_per_second);
  }

  [[nodiscard]] std::string to_string() const {
    return std::to_string(microseconds_since_epoch_);
  }

  [[nodiscard]] std::string to_formatted_string(bool show_microseconds = true) const {
    const std::time_t seconds = seconds_since_epoch();
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (show_microseconds) {
      out << '.' << std::setw(6) << std::setfill('0')
          << microseconds_since_epoch_ % k_microseconds_per_second;
    }
    return out.str();
  }

 private:
  int64_t microseconds_since_epoch_{0};
};

inline constexpr bool operator<(Timestamp lhs, Timestamp rhs) noexcept {
  return lhs.microseconds_since_epoch() < rhs.microseconds_since_epoch();
}

inline constexpr bool operator==(Timestamp lhs, Timestamp rhs) noexcept {
  return lhs.microseconds_since_epoch() == rhs.microseconds_since_epoch();
}

inline constexpr double time_difference(Timestamp high, Timestamp low) noexcept {
  const auto diff = high.microseconds_since_epoch() - low.microseconds_since_epoch();
  return static_cast<double>(diff) / Timestamp::k_microseconds_per_second;
}

inline constexpr Timestamp add_time(Timestamp timestamp, double seconds) noexcept {
  const auto delta =
      static_cast<int64_t>(seconds * static_cast<double>(Timestamp::k_microseconds_per_second));
  return Timestamp(timestamp.microseconds_since_epoch() + delta);
}

}  // namespace oklib
