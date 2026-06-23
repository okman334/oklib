#include <oklib/base/blocking_queue.h>
#include <oklib/base/countdown_latch.h>
#include <oklib/base/logging.h>
#include <oklib/base/noncopyable.h>
#include <oklib/base/thread_pool.h>
#include <oklib/base/timestamp.h>

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

}  // namespace

int main() {
  const auto now = oklib::Timestamp::now();
  require(now.valid(), "Timestamp::now is valid");
  require(!now.to_string().empty(), "Timestamp string is non-empty");

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
