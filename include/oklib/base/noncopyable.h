#pragma once

namespace oklib {

class Noncopyable {
 protected:
  constexpr Noncopyable() noexcept = default;
  ~Noncopyable() = default;

  Noncopyable(const Noncopyable&) = delete;
  Noncopyable& operator=(const Noncopyable&) = delete;
};

}  // namespace oklib
