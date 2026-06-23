#include <oklib/base/blocking_queue.h>
#include <oklib/base/countdown_latch.h>
#include <oklib/base/logging.h>
#include <oklib/base/noncopyable.h>
#include <oklib/base/thread_pool.h>
#include <oklib/base/timestamp.h>

#include <atomic>
#include <chrono>
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
