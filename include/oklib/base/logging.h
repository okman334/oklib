#pragma once

#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "oklib/base/noncopyable.h"
#include "oklib/base/timestamp.h"

namespace oklib {

class Logger : private Noncopyable {
 public:
  enum class Level {
    trace,
    debug,
    info,
    warn,
    error,
    fatal,
  };

  using Output = std::function<void(std::string_view)>;

  Logger(Level level, const char* file, int line) : level_(level), file_(file), line_(line) {
    stream_ << Timestamp::now().to_formatted_string() << ' ' << level_name(level_) << ' '
            << std::this_thread::get_id() << ' ' << basename(file_) << ':' << line_ << " - ";
  }

  ~Logger() {
    stream_ << '\n';
    const auto message = stream_.str();
    if (auto sink = output()) {
      sink(message);
    } else {
      std::cerr << message;
    }
    if (level_ == Level::fatal) {
      std::abort();
    }
  }

  template <typename T>
  Logger& operator<<(T&& value) {
    stream_ << std::forward<T>(value);
    return *this;
  }

  using StreamManipulator = std::ostream& (*)(std::ostream&);
  Logger& operator<<(StreamManipulator manipulator) {
    manipulator(stream_);
    return *this;
  }

  static void set_level(Level level) {
    std::lock_guard lock(config_mutex());
    level_storage() = level;
  }

  static Level level() {
    std::lock_guard lock(config_mutex());
    return level_storage();
  }

  static bool enabled(Level level) {
    return static_cast<int>(level) >= static_cast<int>(Logger::level());
  }

  static void set_output(Output output) {
    std::lock_guard lock(config_mutex());
    output_storage() = std::move(output);
  }

 private:
  static Output output() {
    std::lock_guard lock(config_mutex());
    return output_storage();
  }

  static const char* level_name(Level level) noexcept {
    switch (level) {
      case Level::trace:
        return "TRACE";
      case Level::debug:
        return "DEBUG";
      case Level::info:
        return "INFO";
      case Level::warn:
        return "WARN";
      case Level::error:
        return "ERROR";
      case Level::fatal:
        return "FATAL";
    }
    return "UNKNOWN";
  }

  static const char* basename(const char* path) noexcept {
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
      if (*p == '/' || *p == '\\') {
        base = p + 1;
      }
    }
    return base;
  }

  static std::mutex& config_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  static Output& output_storage() {
    static Output output;
    return output;
  }

  static Level& level_storage() {
    static Level level = Level::info;
    return level;
  }

  Level level_;
  const char* file_;
  int line_;
  std::ostringstream stream_;
};

}  // namespace oklib

#define OKLIB_LOG_TRACE \
  if (!::oklib::Logger::enabled(::oklib::Logger::Level::trace)) { \
  } else \
    ::oklib::Logger(::oklib::Logger::Level::trace, __FILE__, __LINE__)

#define OKLIB_LOG_DEBUG \
  if (!::oklib::Logger::enabled(::oklib::Logger::Level::debug)) { \
  } else \
    ::oklib::Logger(::oklib::Logger::Level::debug, __FILE__, __LINE__)

#define OKLIB_LOG_INFO \
  if (!::oklib::Logger::enabled(::oklib::Logger::Level::info)) { \
  } else \
    ::oklib::Logger(::oklib::Logger::Level::info, __FILE__, __LINE__)

#define OKLIB_LOG_WARN ::oklib::Logger(::oklib::Logger::Level::warn, __FILE__, __LINE__)
#define OKLIB_LOG_ERROR ::oklib::Logger(::oklib::Logger::Level::error, __FILE__, __LINE__)
#define OKLIB_LOG_FATAL ::oklib::Logger(::oklib::Logger::Level::fatal, __FILE__, __LINE__)
