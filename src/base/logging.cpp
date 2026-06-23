#include "oklib/base/logging.h"

#include <array>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <vector>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

#include "oklib/base/timestamp.h"

namespace oklib {
namespace {

constexpr std::size_t k_log_buffer_flush_size = 4 * 1024 * 1024;
constexpr auto k_flush_interval = std::chrono::seconds(3);

std::string path_basename(std::string_view path) {
  std::size_t pos = path.find_last_of("/\\");
  if (pos == std::string_view::npos) {
    return std::string(path);
  }
  return std::string(path.substr(pos + 1));
}

std::string current_program_name() {
#if defined(__APPLE__)
  if (const char* name = ::getprogname(); name != nullptr && *name != '\0') {
    return name;
  }
#elif defined(__linux__)
  std::array<char, PATH_MAX> buffer{};
  const auto len = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (len > 0) {
    return path_basename(std::string_view(buffer.data(), static_cast<std::size_t>(len)));
  }
#endif
  return "oklib";
}

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

std::filesystem::path& log_directory_storage() {
  static std::filesystem::path directory = std::filesystem::current_path();
  return directory;
}

std::string& file_basename_storage() {
  static std::string basename = current_program_name();
  return basename;
}

std::string level_suffix(Logger::Level level) {
  switch (level) {
    case Logger::Level::trace:
      return "trace";
    case Logger::Level::debug:
      return "debug";
    case Logger::Level::info:
      return "info";
    case Logger::Level::warn:
      return "warn";
    case Logger::Level::error:
      return "error";
    case Logger::Level::fatal:
      return "fatal";
  }
  return "unknown";
}

std::filesystem::path log_path(const std::filesystem::path& directory,
                               const std::string& basename,
                               Logger::Level level) {
  return directory / (basename + "." + level_suffix(level) + ".log");
}

class AsyncFileLogger : private Noncopyable {
 public:
  struct Event {
    Logger::Level level;
    std::filesystem::path directory;
    std::string basename;
    std::string message;
  };

  ~AsyncFileLogger() {
    stop();
  }

  void append(Event event) {
    {
      std::lock_guard lock(mutex_);
      ensure_started_locked();
      pending_.push_back(std::move(event));
    }
    cv_.notify_one();
  }

  void flush() {
    std::unique_lock lock(mutex_);
    ensure_started_locked();
    const auto generation = ++flush_generation_;
    flush_requested_ = true;
    cv_.notify_one();
    cv_.wait(lock, [this, generation] { return flushed_generation_ >= generation; });
  }

 private:
  void stop() {
    {
      std::lock_guard lock(mutex_);
      if (!started_) {
        return;
      }
      stopping_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  void ensure_started_locked() {
    if (started_) {
      return;
    }
    started_ = true;
    thread_ = std::thread([this] { run(); });
  }

  void run() {
    std::map<std::filesystem::path, std::string> buffers;
    std::map<std::filesystem::path, std::ofstream> streams;

    for (;;) {
      std::vector<Event> events;
      bool force_flush = false;
      std::uint64_t flush_generation = 0;
      {
        std::unique_lock lock(mutex_);
        bool timed_out = false;
        if (!stopping_ && pending_.empty() && !flush_requested_) {
          timed_out = cv_.wait_for(lock, k_flush_interval) == std::cv_status::timeout;
        }
        events.swap(pending_);
        force_flush = flush_requested_ || stopping_ || timed_out;
        flush_generation = flush_generation_;
        flush_requested_ = false;
      }

      for (auto& event : events) {
        const auto path = log_path(event.directory, event.basename, event.level);
        auto& buffer = buffers[path];
        buffer.append(event.message);
        if (buffer.size() >= k_log_buffer_flush_size) {
          write_buffer(path, buffer, streams, true);
        }
      }

      if (force_flush) {
        flush_buffers(buffers, streams);
        std::lock_guard lock(mutex_);
        flushed_generation_ = flush_generation;
        cv_.notify_all();
      }

      std::lock_guard lock(mutex_);
      if (stopping_ && pending_.empty()) {
        break;
      }
    }

    flush_buffers(buffers, streams);
  }

  static void write_buffer(const std::filesystem::path& path,
                           std::string& buffer,
                           std::map<std::filesystem::path, std::ofstream>& streams,
                           bool do_flush) {
    if (buffer.empty()) {
      return;
    }
    std::filesystem::create_directories(path.parent_path());
    auto& stream = streams[path];
    if (!stream.is_open()) {
      stream.open(path, std::ios::app | std::ios::binary);
    }
    stream.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.clear();
    if (do_flush) {
      stream.flush();
    }
  }

  static void flush_buffers(std::map<std::filesystem::path, std::string>& buffers,
                            std::map<std::filesystem::path, std::ofstream>& streams) {
    for (auto& [path, buffer] : buffers) {
      write_buffer(path, buffer, streams, true);
    }
    for (auto& [_, stream] : streams) {
      stream.flush();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Event> pending_;
  std::thread thread_;
  bool started_{false};
  bool stopping_{false};
  bool flush_requested_{false};
  std::uint64_t flush_generation_{0};
  std::uint64_t flushed_generation_{0};
};

AsyncFileLogger& async_file_logger() {
  static AsyncFileLogger logger;
  return logger;
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
    async_file_logger().append(AsyncFileLogger::Event{
        .level = level_,
        .directory = log_directory(),
        .basename = file_basename(),
        .message = message,
    });
  }
  if (level_ == Level::fatal) {
    flush();
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

void Logger::set_log_directory(std::filesystem::path directory) {
  std::lock_guard lock(config_mutex());
  log_directory_storage() = std::move(directory);
}

void Logger::set_file_basename(std::string basename) {
  std::lock_guard lock(config_mutex());
  file_basename_storage() = basename.empty() ? current_program_name() : std::move(basename);
}

std::filesystem::path Logger::log_directory() {
  std::lock_guard lock(config_mutex());
  return log_directory_storage();
}

std::string Logger::file_basename() {
  std::lock_guard lock(config_mutex());
  return file_basename_storage();
}

void Logger::flush() {
  async_file_logger().flush();
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
