#include <oklib/base/blocking_queue.h>
#include <oklib/base/countdown_latch.h>
#include <oklib/base/log_stream.h>
#include <oklib/base/logging.h>
#include <oklib/base/noncopyable.h>
#include <oklib/base/thread_pool.h>
#include <oklib/base/timestamp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool wait_for_file_contains(const std::filesystem::path& path,
                            const std::string& needle,
                            std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (read_file(path).find(needle) != std::string::npos) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

std::vector<std::filesystem::path> log_files_with_prefix(const std::filesystem::path& directory,
                                                         const std::string& prefix) {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto filename = entry.path().filename().string();
    if (filename.starts_with(prefix) && filename.ends_with(".log")) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::string read_files(const std::vector<std::filesystem::path>& files) {
  std::string content;
  for (const auto& file : files) {
    content += read_file(file);
  }
  return content;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary);
  output << content;
}

}  // namespace

int main() {
  const auto now = oklib::Timestamp::now();
  require(now.valid(), "Timestamp::now is valid");
  require(!now.to_string().empty(), "Timestamp string is non-empty");

  oklib::LogStream stream;
  stream << "answer=" << 42 << ' ' << std::string_view("ok");
  require(stream.str() == "answer=42 ok", "log stream formats common values");
  oklib::LogStream large_stream;
  const std::string large_payload(5000, 'x');
  large_stream << large_payload;
  require(large_stream.str() == large_payload, "log stream grows beyond inline buffer");

  std::string captured;
  oklib::Logger::set_output([&](std::string_view message) { captured.assign(message); });
  OKLIB_LOG_INFO << "hello " << 42;
  require(captured.find("INFO") != std::string::npos, "logger emits level");
  require(captured.find("hello 42") != std::string::npos, "logger emits stream contents");
  oklib::Logger::set_output({});

  require(oklib::Logger::file_basename().find("oklib_base_test") != std::string::npos,
          "default log file basename uses current program name");

  const auto log_dir = std::filesystem::temp_directory_path() /
                       ("oklib-logger-test-" + std::to_string(now.microseconds_since_epoch()));
  std::filesystem::create_directories(log_dir);
  oklib::Logger::set_log_directory(log_dir);
  oklib::Logger::set_file_basename("custom-base");
  oklib::Logger::set_level(oklib::Logger::Level::trace);
  OKLIB_LOG_INFO << "info-file-message";
  OKLIB_LOG_WARN << "warn-file-message";
  oklib::Logger::flush();

  const auto info_log = read_file(log_dir / "custom-base.info.log");
  const auto warn_log = read_file(log_dir / "custom-base.warn.log");
  require(info_log.find("info-file-message") != std::string::npos,
          "info log writes to custom info file");
  require(warn_log.find("warn-file-message") != std::string::npos,
          "warn log writes to custom warn file");
  require(info_log.find("warn-file-message") == std::string::npos,
          "warn log is separated from info log");

  const auto periodic_dir = std::filesystem::temp_directory_path() /
                            ("oklib-logger-periodic-test-" +
                             std::to_string(now.microseconds_since_epoch()));
  std::filesystem::create_directories(periodic_dir);
  oklib::Logger::set_log_directory(periodic_dir);
  oklib::Logger::set_file_basename("periodic-base");
  oklib::Logger::set_flush_interval(std::chrono::milliseconds(50));
  require(oklib::Logger::flush_interval() == std::chrono::milliseconds(50),
          "flush interval is configurable");
  std::atomic<bool> keep_logging{true};
  std::jthread noisy_logger([&] {
    while (keep_logging.load(std::memory_order_relaxed)) {
      OKLIB_LOG_INFO << "periodic-noise";
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  OKLIB_LOG_INFO << "periodic-target";
  require(wait_for_file_contains(periodic_dir / "periodic-base.info.log",
                                 "periodic-target",
                                 std::chrono::milliseconds(500)),
          "logger flushes on interval even while logs continue");
  keep_logging = false;
  noisy_logger.join();
  oklib::Logger::flush();

  const auto rolling_dir = std::filesystem::temp_directory_path() /
                           ("oklib-logger-rolling-test-" +
                            std::to_string(now.microseconds_since_epoch()));
  std::filesystem::create_directories(rolling_dir);
  oklib::Logger::set_log_directory(rolling_dir);
  oklib::Logger::set_file_basename("rolling-base");
  oklib::Logger::set_roll_size(512);
  oklib::Logger::set_max_roll_files(3);
  require(oklib::Logger::roll_size() == 512, "roll size is configurable");
  require(oklib::Logger::max_roll_files() == 3, "max roll files is configurable");
  for (int i = 0; i < 8; ++i) {
    OKLIB_LOG_INFO << "rolling-message-" << i << ' ' << std::string(80, 'x');
  }
  oklib::Logger::flush();
  require(std::filesystem::exists(rolling_dir / "rolling-base.info.log"),
          "active rolling log remains at base name");
  require(std::filesystem::exists(rolling_dir / "rolling-base.info.1.log"),
          "first backup uses spdlog-style index");
  require(!std::filesystem::exists(rolling_dir / "rolling-base.info.4.log"),
          "logger does not create backups beyond configured max files");
  auto rolled_files = log_files_with_prefix(rolling_dir, "rolling-base.info");
  const auto rolled_content = read_files(rolled_files);
  require(rolled_content.find("rolling-message-0") != std::string::npos,
          "rolled logs contain first message");
  require(rolled_content.find("rolling-message-7") != std::string::npos,
          "rolled logs contain last message");

  const auto restart_dir = std::filesystem::temp_directory_path() /
                           ("oklib-logger-restart-roll-test-" +
                            std::to_string(now.microseconds_since_epoch()));
  std::filesystem::create_directories(restart_dir);
  const auto restart_active = restart_dir / "restart-base.info.log";
  write_file(restart_active, "existing-over-limit\n" + std::string(700, 'o'));
  oklib::Logger::set_log_directory(restart_dir);
  oklib::Logger::set_file_basename("restart-base");
  oklib::Logger::set_roll_size(512);
  oklib::Logger::set_max_roll_files(3);
  OKLIB_LOG_INFO << "restart-new-message";
  oklib::Logger::flush();
  const auto restart_active_content = read_file(restart_active);
  const auto restart_backup_content = read_file(restart_dir / "restart-base.info.1.log");
  require(restart_active_content.find("restart-new-message") != std::string::npos,
          "restart writes new message to fresh active file");
  require(restart_active_content.find("existing-over-limit") == std::string::npos,
          "restart does not append to oversized active file");
  require(restart_backup_content.find("existing-over-limit") != std::string::npos,
          "restart rotates oversized active file to first backup");

  const auto retention_dir = std::filesystem::temp_directory_path() /
                             ("oklib-logger-retention-test-" +
                              std::to_string(now.microseconds_since_epoch()));
  std::filesystem::create_directories(retention_dir);
  oklib::Logger::set_log_directory(retention_dir);
  oklib::Logger::set_file_basename("retention-base");
  oklib::Logger::set_roll_size(256);
  oklib::Logger::set_max_roll_files(3);
  for (int i = 0; i < 20; ++i) {
    OKLIB_LOG_INFO << "retention-message-" << i << ' ' << std::string(80, 'r');
  }
  oklib::Logger::flush();
  require(std::filesystem::exists(retention_dir / "retention-base.info.3.log"),
          "logger keeps the oldest configured backup index");
  require(!std::filesystem::exists(retention_dir / "retention-base.info.4.log"),
          "logger removes backups beyond max roll files");

  const auto truncate_dir = std::filesystem::temp_directory_path() /
                            ("oklib-logger-truncate-roll-test-" +
                             std::to_string(now.microseconds_since_epoch()));
  std::filesystem::create_directories(truncate_dir);
  oklib::Logger::set_log_directory(truncate_dir);
  oklib::Logger::set_file_basename("truncate-base");
  oklib::Logger::set_roll_size(256);
  oklib::Logger::set_max_roll_files(0);
  for (int i = 0; i < 10; ++i) {
    OKLIB_LOG_INFO << "truncate-message-" << i << ' ' << std::string(80, 't');
  }
  oklib::Logger::flush();
  require(std::filesystem::exists(truncate_dir / "truncate-base.info.log"),
          "truncate mode keeps active file");
  require(!std::filesystem::exists(truncate_dir / "truncate-base.info.1.log"),
          "truncate mode does not keep backup files");

  oklib::CountDownLatch latch(2);
  std::jthread t1([&] { latch.count_down(); });
  std::jthread t2([&] { latch.count_down(); });
  latch.wait();
  require(latch.count() == 0, "latch reaches zero");

  oklib::BlockingQueue<int> queue;
  std::jthread producer([&] { queue.put(7); });
  require(queue.take() == 7, "blocking queue transfers value");

  oklib::ThreadPool pool("base-test");
  std::atomic<int> ran{0};
  oklib::CountDownLatch done(4);
  pool.start(2);
  for (int i = 0; i < 4; ++i) {
    pool.run([&] {
      ran.fetch_add(1, std::memory_order_relaxed);
      done.count_down();
    });
  }
  done.wait();
  pool.stop();
  require(ran.load(std::memory_order_relaxed) == 4, "thread pool runs all tasks");

  return 0;
}
