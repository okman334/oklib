#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include "oklib/net/timer_thread.h"

int main() {
  using namespace std::chrono_literals;

  oklib::net::TimerThread timer("demo-timer");
  std::atomic<int> ticks{0};

  timer.set_timeout(1001, 500ms, [](oklib::net::TimerId id) {
    std::cout << "timeout fired, id=" << id.value() << '\n';
  });

  oklib::net::TimerId interval;
  interval = timer.set_interval(2001, 300ms, [&](oklib::net::TimerId id) {
    const int count = ticks.fetch_add(1) + 1;
    std::cout << "interval tick " << count << ", id=" << id.value() << '\n';
    if (count == 5) {
      timer.clear_interval(interval);
    }
  });

  auto global = oklib::net::GlobalTimer::set_timeout(1s, [](oklib::net::TimerId id) {
    std::cout << "global timeout fired, id=" << id.value() << '\n';
  });
  std::cout << "scheduled global timer id=" << global.value() << '\n';

  std::this_thread::sleep_for(2s);
  oklib::net::GlobalTimer::shutdown();
  return 0;
}
