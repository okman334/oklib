#include "oklib/base/timestamp.h"

#include <iomanip>
#include <sstream>

namespace oklib {

Timestamp Timestamp::now() {
  const auto now = clock::now().time_since_epoch();
  return Timestamp(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string Timestamp::to_string() const {
  return std::to_string(microseconds_since_epoch_);
}

std::string Timestamp::to_formatted_string(bool show_microseconds) const {
  const std::time_t seconds = seconds_since_epoch();
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &seconds);
#else
  localtime_r(&seconds, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  if (show_microseconds) {
    out << '.' << std::setw(6) << std::setfill('0')
        << microseconds_since_epoch_ % k_microseconds_per_second;
  }
  return out.str();
}

}  // namespace oklib
