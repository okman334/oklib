#include "oklib/base/log_stream.h"

#include <charconv>
#include <cstdio>
#include <cstring>

namespace oklib {

LogStream& LogStream::operator<<(bool value) {
  append(value ? "1" : "0");
  return *this;
}

LogStream& LogStream::operator<<(char value) {
  append_char(value);
  return *this;
}

LogStream& LogStream::operator<<(signed char value) {
  append_char(static_cast<char>(value));
  return *this;
}

LogStream& LogStream::operator<<(unsigned char value) {
  append_char(static_cast<char>(value));
  return *this;
}

LogStream& LogStream::operator<<(short value) {
  append_integer(value);
  return *this;
}

LogStream& LogStream::operator<<(unsigned short value) {
  append_unsigned_integer(value);
  return *this;
}

LogStream& LogStream::operator<<(int value) {
  append_integer(value);
  return *this;
}

LogStream& LogStream::operator<<(unsigned int value) {
  append_unsigned_integer(value);
  return *this;
}

LogStream& LogStream::operator<<(long value) {
  append_integer(value);
  return *this;
}

LogStream& LogStream::operator<<(unsigned long value) {
  append_unsigned_integer(value);
  return *this;
}

LogStream& LogStream::operator<<(long long value) {
  append_integer(value);
  return *this;
}

LogStream& LogStream::operator<<(unsigned long long value) {
  append_unsigned_integer(value);
  return *this;
}

LogStream& LogStream::operator<<(float value) {
  append_floating(static_cast<double>(value));
  return *this;
}

LogStream& LogStream::operator<<(double value) {
  append_floating(value);
  return *this;
}

LogStream& LogStream::operator<<(long double value) {
  append_long_double(value);
  return *this;
}

LogStream& LogStream::operator<<(const void* value) {
  append_pointer(reinterpret_cast<std::uintptr_t>(value));
  return *this;
}

LogStream& LogStream::operator<<(const char* value) {
  append(value == nullptr ? "(null)" : std::string_view(value));
  return *this;
}

LogStream& LogStream::operator<<(std::nullptr_t) {
  append("(null)");
  return *this;
}

LogStream& LogStream::operator<<(std::string_view value) {
  append(value);
  return *this;
}

LogStream& LogStream::operator<<(const std::string& value) {
  append(value);
  return *this;
}

LogStream& LogStream::operator<<(StreamManipulator manipulator) {
  std::ostringstream stream;
  manipulator(stream);
  append(stream.str());
  return *this;
}

LogStream& LogStream::operator<<(IosManipulator manipulator) {
  if (manipulator == static_cast<IosManipulator>(std::hex)) {
    integer_base_ = IntegerBase::hexadecimal;
  } else if (manipulator == static_cast<IosManipulator>(std::oct)) {
    integer_base_ = IntegerBase::octal;
  } else if (manipulator == static_cast<IosManipulator>(std::dec)) {
    integer_base_ = IntegerBase::decimal;
  } else {
    std::ostringstream stream;
    manipulator(stream);
    append(stream.str());
  }
  return *this;
}

void LogStream::append(std::string_view value) {
  if (value.empty()) {
    return;
  }

  if (overflow_.empty() && inline_size_ + value.size() <= k_inline_buffer_size) {
    std::memcpy(inline_buffer_ + inline_size_, value.data(), value.size());
    inline_size_ += value.size();
    return;
  }

  ensure_overflow_buffer();
  overflow_.append(value);
}

std::string LogStream::str() const {
  if (!overflow_.empty()) {
    return overflow_;
  }
  return std::string(inline_buffer_, inline_size_);
}

std::size_t LogStream::size() const noexcept {
  return overflow_.empty() ? inline_size_ : overflow_.size();
}

template <typename T>
void LogStream::append_integer(T value) {
  if (integer_base_ == IntegerBase::decimal) {
    char buffer[32];
    auto [ptr, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error == std::errc{}) {
      append(std::string_view(buffer, static_cast<std::size_t>(ptr - buffer)));
    }
    return;
  }

  using Unsigned = std::make_unsigned_t<T>;
  append_unsigned_integer(static_cast<Unsigned>(value));
}

template <typename T>
void LogStream::append_unsigned_integer(T value) {
  char buffer[32];
  const int base = integer_base_ == IntegerBase::hexadecimal ? 16 : integer_base_ == IntegerBase::octal ? 8 : 10;
  auto [ptr, error] = std::to_chars(buffer, buffer + sizeof(buffer), value, base);
  if (error == std::errc{}) {
    append(std::string_view(buffer, static_cast<std::size_t>(ptr - buffer)));
  }
}

void LogStream::append_char(char value) {
  append(std::string_view(&value, 1));
}

void LogStream::append_pointer(std::uintptr_t value) {
  append("0x");
  char buffer[2 * sizeof(std::uintptr_t)];
  auto [ptr, error] = std::to_chars(buffer, buffer + sizeof(buffer), value, 16);
  if (error == std::errc{}) {
    append(std::string_view(buffer, static_cast<std::size_t>(ptr - buffer)));
  }
}

void LogStream::append_floating(double value) {
  char buffer[64];
  const int length = std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  if (length > 0) {
    append(std::string_view(buffer, static_cast<std::size_t>(length)));
  }
}

void LogStream::append_long_double(long double value) {
  char buffer[80];
  const int length = std::snprintf(buffer, sizeof(buffer), "%.6Lg", value);
  if (length > 0) {
    append(std::string_view(buffer, static_cast<std::size_t>(length)));
  }
}

void LogStream::ensure_overflow_buffer() {
  if (!overflow_.empty()) {
    return;
  }
  overflow_.assign(inline_buffer_, inline_size_);
  inline_size_ = 0;
}

}  // namespace oklib
