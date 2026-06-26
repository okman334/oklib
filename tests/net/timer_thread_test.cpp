#include <oklib/net/timer_thread.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  using namespace std::chrono_literals;

  {
    oklib::net::TimerThread timer("timer-thread-test");
    std::atomic<int> fired{0};
    auto id = timer.set_timeout(20ms, [&](oklib::net::TimerId callback_id) {
      require(callback_id.valid(), "timeout callback receives timer id");
      fired.fetch_add(1, std::memory_order_relaxed);
    });
    require(id.valid(), "set_timeout returns valid timer id");
    std::this_thread::sleep_for(100ms);
    require(fired.load(std::memory_order_relaxed) == 1, "timeout fires exactly once");
  }

  {
    oklib::net::TimerThread timer("interval-test");
    std::atomic<int> fired{0};
    oklib::net::TimerId interval;
    interval = timer.set_interval(15ms, [&](oklib::net::TimerId callback_id) {
      require(callback_id.value() == interval.value(), "interval callback receives its timer id");
      const int count = fired.fetch_add(1, std::memory_order_relaxed) + 1;
      if (count == 3) {
        timer.clear_interval(callback_id);
      }
    });
    std::this_thread::sleep_for(120ms);
    require(fired.load(std::memory_order_relaxed) == 3, "clear_interval stops recurring timer");
  }

  {
    oklib::net::TimerThread timer("cross-thread-timer-test");
    std::atomic<int> fired{0};
    std::thread scheduler([&] {
      auto id = timer.set_timeout(20ms, [&](oklib::net::TimerId) {
        fired.fetch_add(1, std::memory_order_relaxed);
      });
      require(id.valid(), "cross-thread set_timeout returns valid id");
    });
    scheduler.join();
    std::this_thread::sleep_for(100ms);
    require(fired.load(std::memory_order_relaxed) == 1, "cross-thread timeout fires");
  }

  {
    std::atomic<int> fired{0};
    auto id = oklib::net::GlobalTimer::set_timeout(20ms, [&](oklib::net::TimerId callback_id) {
      require(callback_id.valid(), "global timeout callback receives timer id");
      fired.fetch_add(1, std::memory_order_relaxed);
    });
    require(id.valid(), "GlobalTimer::set_timeout returns valid id");
    std::this_thread::sleep_for(100ms);
    oklib::net::GlobalTimer::shutdown();
    require(fired.load(std::memory_order_relaxed) == 1, "global timeout fires");
  }

  return 0;
}
