#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include "oklib/base/log_stream.h"
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

  Logger(Level level, const char* file, int line);

  ~Logger();

  template <typename T>
  Logger& operator<<(T&& value) {
    stream_ << std::forward<T>(value);
    return *this;
  }

  using StreamManipulator = LogStream::StreamManipulator;
  Logger& operator<<(StreamManipulator manipulator) {
    stream_ << manipulator;
    return *this;
  }

  static void set_level(Level level);

  static Level level();

  static bool enabled(Level level);

  static void set_output(Output output);
  static void set_log_directory(std::filesystem::path directory);
  static void set_file_basename(std::string basename);
  static std::filesystem::path log_directory();
  static std::string file_basename();
  static void set_flush_interval(std::chrono::milliseconds interval);
  static std::chrono::milliseconds flush_interval();
  static void set_roll_size(std::uintmax_t bytes);
  static std::uintmax_t roll_size();
  static void set_max_roll_files(std::size_t files);
  static std::size_t max_roll_files();
  static void flush();

 private:
  static Output output();
  static const char* level_name(Level level) noexcept;
  static const char* basename(const char* path) noexcept;

  Level level_;
  LogStream stream_;
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
