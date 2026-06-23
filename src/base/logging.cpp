#include "oklib/base/logging.h"

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#include "oklib/base/timestamp.h"

namespace oklib {
namespace {

std::mutex& config_mutex() {
  static std::mutex mutex;
  return mutex;
}

Logger::Output& output_storage() {
  static Logger::Output output;
  return output;
}

Logger::Level& level_storage() {
  static Logger::Level level = Logger::Level::info;
  return level;
}

}  // namespace

Logger::Logger(Level level, const char* file, int line) : level_(level) {
  stream_ << Timestamp::now().to_formatted_string() << ' ' << level_name(level_) << ' '
          << std::this_thread::get_id() << ' ' << basename(file) << ':' << line << " - ";
}

Logger::~Logger() {
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

void Logger::set_level(Level level) {
  std::lock_guard lock(config_mutex());
  level_storage() = level;
}

Logger::Level Logger::level() {
  std::lock_guard lock(config_mutex());
  return level_storage();
}

bool Logger::enabled(Level level) {
  return static_cast<int>(level) >= static_cast<int>(Logger::level());
}

void Logger::set_output(Output output) {
  std::lock_guard lock(config_mutex());
  output_storage() = std::move(output);
}

Logger::Output Logger::output() {
  std::lock_guard lock(config_mutex());
  return output_storage();
}

const char* Logger::level_name(Level level) noexcept {
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

const char* Logger::basename(const char* path) noexcept {
  const char* base = path;
  for (const char* p = path; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  return base;
}

}  // namespace oklib
