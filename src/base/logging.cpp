#include "oklib/base/logging.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
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
constexpr std::uintmax_t k_default_roll_size = 256 * 1024 * 1024;
constexpr std::uintmax_t k_min_roll_size = 1;
constexpr std::size_t k_default_max_roll_files = 5;
constexpr auto k_default_flush_interval = std::chrono::seconds(3);
constexpr auto k_min_flush_interval = std::chrono::milliseconds(1);

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

const std::string& current_thread_id_string() {
  thread_local const std::string id = [] {
    std::ostringstream stream;
    stream << std::this_thread::get_id();
    return stream.str();
  }();
  return id;
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

std::chrono::milliseconds& flush_interval_storage() {
  static std::chrono::milliseconds interval =
      std::chrono::duration_cast<std::chrono::milliseconds>(k_default_flush_interval);
  return interval;
}

std::uintmax_t& roll_size_storage() {
  static std::uintmax_t size = k_default_roll_size;
  return size;
}

std::size_t& max_roll_files_storage() {
  static std::size_t files = k_default_max_roll_files;
  return files;
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

std::uintmax_t file_size_or_zero(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : size;
}

std::filesystem::path indexed_log_path(const std::filesystem::path& base_path,
                                       std::size_t index) {
  if (index == 0) {
    return base_path;
  }
  const auto stem = base_path.stem().string();
  const auto extension = base_path.extension().string();
  return base_path.parent_path() /
         (stem + "." + std::to_string(index) + extension);
}

class AsyncFileLogger : private Noncopyable {
 public:
  struct Event {
    Logger::Level level;
    std::filesystem::path directory;
    std::string basename;
    std::string message;
  };

  struct Sink {
    std::filesystem::path base_path;
    std::ofstream stream;
    std::uintmax_t written{0};
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

  void notify_flush_interval_changed() {
    {
      std::lock_guard lock(mutex_);
      ++interval_generation_;
    }
    cv_.notify_one();
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
    std::map<std::filesystem::path, Sink> sinks;
    using Clock = std::chrono::steady_clock;
    auto next_flush = Clock::now() + Logger::flush_interval();
    std::uint64_t observed_interval_generation = 0;

    for (;;) {
      std::vector<Event> events;
      bool force_flush = false;
      std::uint64_t flush_generation = 0;
      {
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, next_flush, [this, observed_interval_generation] {
          return stopping_ || flush_requested_ || !pending_.empty() ||
                 interval_generation_ != observed_interval_generation;
        });
        if (interval_generation_ != observed_interval_generation) {
          observed_interval_generation = interval_generation_;
          next_flush = Clock::now() + Logger::flush_interval();
        }
        const bool interval_due = Clock::now() >= next_flush;
        events.swap(pending_);
        force_flush = flush_requested_ || stopping_ || interval_due;
        flush_generation = flush_generation_;
        flush_requested_ = false;
        if (interval_due) {
          next_flush = Clock::now() + Logger::flush_interval();
        }
      }

      for (auto& event : events) {
        const auto path = log_path(event.directory, event.basename, event.level);
        auto& buffer = buffers[path];
        buffer.append(event.message);
        if (buffer.size() >= k_log_buffer_flush_size) {
          write_buffer(path, buffer, sinks, true);
        }
      }

      if (force_flush) {
        flush_buffers(buffers, sinks);
        std::lock_guard lock(mutex_);
        flushed_generation_ = flush_generation;
        cv_.notify_all();
      }

      std::lock_guard lock(mutex_);
      if (stopping_ && pending_.empty()) {
        break;
      }
    }

    flush_buffers(buffers, sinks);
  }

  static void write_buffer(const std::filesystem::path& path,
                           std::string& buffer,
                           std::map<std::filesystem::path, Sink>& sinks,
                           bool do_flush) {
    if (buffer.empty()) {
      return;
    }

    auto& sink = sinks[path];
    ensure_sink(path, sink);
    const auto roll_size = Logger::roll_size();
    std::string_view remaining(buffer.data(), buffer.size());
    while (!remaining.empty()) {
      const auto newline = remaining.find('\n');
      const auto record_size = newline == std::string_view::npos ? remaining.size() : newline + 1;
      const auto record = remaining.substr(0, record_size);
      write_record(sink, record, roll_size);
      remaining.remove_prefix(record_size);
    }

    buffer.clear();
    if (do_flush && sink.stream.is_open()) {
      sink.stream.flush();
    }
  }

  static void flush_buffers(std::map<std::filesystem::path, std::string>& buffers,
                            std::map<std::filesystem::path, Sink>& sinks) {
    for (auto& [path, buffer] : buffers) {
      write_buffer(path, buffer, sinks, true);
    }
    for (auto& [_, sink] : sinks) {
      if (sink.stream.is_open()) {
        sink.stream.flush();
      }
    }
  }

  static void ensure_sink(const std::filesystem::path& path, Sink& sink) {
    if (!sink.base_path.empty()) {
      return;
    }
    sink.base_path = path;
    sink.written = file_size_or_zero(path);
  }

  static void write_record(Sink& sink, std::string_view data, std::uintmax_t roll_size) {
    const auto data_size = static_cast<std::uintmax_t>(data.size());
    if (sink.written > 0 && (sink.written >= roll_size || data_size > roll_size - sink.written)) {
      roll_sink(sink);
    }
    write_to_sink(sink, data);
  }

  static void write_to_sink(Sink& sink, std::string_view data) {
    std::filesystem::create_directories(sink.base_path.parent_path());
    if (!sink.stream.is_open()) {
      sink.stream.open(sink.base_path, std::ios::app | std::ios::binary);
      sink.written = file_size_or_zero(sink.base_path);
    }
    sink.stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    sink.written += data.size();
  }

  static void roll_sink(Sink& sink) {
    if (sink.stream.is_open()) {
      sink.stream.flush();
      sink.stream.close();
    }

    const auto max_files = Logger::max_roll_files();
    if (max_files == 0) {
      std::filesystem::create_directories(sink.base_path.parent_path());
      std::ofstream truncate(sink.base_path, std::ios::binary | std::ios::trunc);
      sink.written = 0;
      return;
    }

    for (std::size_t index = max_files; index > 0; --index) {
      const auto source = indexed_log_path(sink.base_path, index - 1);
      if (!std::filesystem::exists(source)) {
        continue;
      }
      const auto target = indexed_log_path(sink.base_path, index);
      std::error_code error;
      std::filesystem::remove(target, error);
      error.clear();
      std::filesystem::rename(source, target, error);
      if (error && index == 1) {
        std::ofstream truncate(sink.base_path, std::ios::binary | std::ios::trunc);
      }
    }
    sink.written = 0;
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
  std::uint64_t interval_generation_{0};
};

AsyncFileLogger& async_file_logger() {
  static AsyncFileLogger logger;
  return logger;
}

}  // namespace

Logger::Logger(Level level, const char* file, int line) : level_(level) {
  stream_ << Timestamp::now().to_formatted_string() << ' ' << level_name(level_) << ' '
          << current_thread_id_string() << ' ' << basename(file) << ':' << line << " - ";
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

void Logger::set_flush_interval(std::chrono::milliseconds interval) {
  if (interval < k_min_flush_interval) {
    interval = k_min_flush_interval;
  }
  {
    std::lock_guard lock(config_mutex());
    flush_interval_storage() = interval;
  }
  async_file_logger().notify_flush_interval_changed();
}

std::chrono::milliseconds Logger::flush_interval() {
  std::lock_guard lock(config_mutex());
  return flush_interval_storage();
}

void Logger::set_roll_size(std::uintmax_t bytes) {
  if (bytes < k_min_roll_size) {
    bytes = k_min_roll_size;
  }
  std::lock_guard lock(config_mutex());
  roll_size_storage() = bytes;
}

std::uintmax_t Logger::roll_size() {
  std::lock_guard lock(config_mutex());
  return roll_size_storage();
}

void Logger::set_max_roll_files(std::size_t files) {
  std::lock_guard lock(config_mutex());
  max_roll_files_storage() = files;
}

std::size_t Logger::max_roll_files() {
  std::lock_guard lock(config_mutex());
  return max_roll_files_storage();
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
