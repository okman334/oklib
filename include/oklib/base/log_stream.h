#pragma once

#include <cstddef>
#include <cstdint>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#include "oklib/base/noncopyable.h"

namespace oklib {

class LogStream : private Noncopyable {
 public:
  using StreamManipulator = std::ostream& (*)(std::ostream&);
  using IosManipulator = std::ios_base& (*)(std::ios_base&);

  LogStream() = default;

  LogStream& operator<<(bool value);
  LogStream& operator<<(char value);
  LogStream& operator<<(signed char value);
  LogStream& operator<<(unsigned char value);
  LogStream& operator<<(short value);
  LogStream& operator<<(unsigned short value);
  LogStream& operator<<(int value);
  LogStream& operator<<(unsigned int value);
  LogStream& operator<<(long value);
  LogStream& operator<<(unsigned long value);
  LogStream& operator<<(long long value);
  LogStream& operator<<(unsigned long long value);
  LogStream& operator<<(float value);
  LogStream& operator<<(double value);
  LogStream& operator<<(long double value);
  LogStream& operator<<(const void* value);
  LogStream& operator<<(const char* value);
  LogStream& operator<<(std::nullptr_t);
  LogStream& operator<<(std::string_view value);
  LogStream& operator<<(const std::string& value);
  LogStream& operator<<(StreamManipulator manipulator);
  LogStream& operator<<(IosManipulator manipulator);

  template <std::size_t N>
  LogStream& operator<<(const char (&value)[N]) {
    append(std::string_view(value, N > 0 ? N - 1 : 0));
    return *this;
  }

  template <typename T>
    requires(!std::is_arithmetic_v<std::remove_cvref_t<T>> &&
             !std::is_convertible_v<T, std::string_view> &&
             !std::is_convertible_v<T, const void*> &&
             !std::is_same_v<std::remove_cvref_t<T>, StreamManipulator> &&
             !std::is_same_v<std::remove_cvref_t<T>, IosManipulator>)
  LogStream& operator<<(const T& value) {
    std::ostringstream stream;
    stream << value;
    append(stream.str());
    return *this;
  }

  void append(std::string_view value);
  [[nodiscard]] std::string str() const;
  [[nodiscard]] std::size_t size() const noexcept;

 private:
  enum class IntegerBase {
    decimal,
    hexadecimal,
    octal,
  };

  template <typename T>
  void append_integer(T value);

  template <typename T>
  void append_unsigned_integer(T value);

  void append_char(char value);
  void append_pointer(std::uintptr_t value);
  void append_floating(double value);
  void append_long_double(long double value);
  void ensure_overflow_buffer();

  static constexpr std::size_t k_inline_buffer_size = 4096;

  char inline_buffer_[k_inline_buffer_size];
  std::size_t inline_size_{0};
  std::string overflow_;
  IntegerBase integer_base_{IntegerBase::decimal};
};

}  // namespace oklib
