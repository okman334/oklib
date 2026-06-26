#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include "oklib/net/timer_thread.h"

int main() {
  using namespace std::chrono_literals;

  oklib::net::TimerThread timer("demo-timer");
  std::atomic<int> auto_ticks{0};
  std::atomic<int> custom_ticks{0};

  auto auto_timeout = timer.set_timeout(300ms, [](oklib::net::TimerId id) {
    std::cout << "[TimerThread] auto timeout fired, id=" << id.value() << '\n';
  });
  std::cout << "scheduled TimerThread auto timeout id=" << auto_timeout.value() << '\n';

  auto custom_timeout = timer.set_timeout(1001, 500ms, [](oklib::net::TimerId id) {
    std::cout << "[TimerThread] custom timeout fired, id=" << id.value() << '\n';
  });
  std::cout << "scheduled TimerThread custom timeout id=" << custom_timeout.value() << '\n';

  auto auto_interval = timer.set_interval(250ms, [&](oklib::net::TimerId id) {
    const int count = auto_ticks.fetch_add(1) + 1;
    std::cout << "[TimerThread] auto interval tick " << count << ", id=" << id.value() << '\n';
    if (count == 3) {
      timer.clear_interval(id);
    }
  });
  std::cout << "scheduled TimerThread auto interval id=" << auto_interval.value() << '\n';

  auto custom_interval = timer.set_interval(2001, 300ms, [&](oklib::net::TimerId id) {
    const int count = custom_ticks.fetch_add(1) + 1;
    std::cout << "[TimerThread] custom interval tick " << count << ", id=" << id.value() << '\n';
    if (count == 5) {
      timer.clear_interval(id);
    }
  });
  std::cout << "scheduled TimerThread custom interval id=" << custom_interval.value() << '\n';

  auto global_auto = oklib::net::GlobalTimer::set_timeout(700ms, [](oklib::net::TimerId id) {
    std::cout << "[GlobalTimer] auto timeout fired, id=" << id.value() << '\n';
  });
  std::cout << "scheduled GlobalTimer auto timeout id=" << global_auto.value() << '\n';

  auto global_custom = oklib::net::GlobalTimer::set_timeout(3001, 900ms, [](oklib::net::TimerId id) {
    std::cout << "[GlobalTimer] custom timeout fired, id=" << id.value() << '\n';
  });
  std::cout << "scheduled GlobalTimer custom timeout id=" << global_custom.value() << '\n';

  std::this_thread::sleep_for(2s);
  oklib::net::GlobalTimer::shutdown();
  return 0;
}
