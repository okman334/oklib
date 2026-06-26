#include <oklib/base/countdown_latch.h>
#include <oklib/net/channel.h>
#include <oklib/net/event_loop.h>

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

void close_fd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

}  // namespace

int main() {
  using namespace std::chrono_literals;

  {
    std::promise<oklib::net::EventLoop*> ready;
    std::atomic<int> ran{0};
    std::thread loop_thread([&] {
      oklib::net::EventLoop loop;
      ready.set_value(&loop);
      loop.loop();
    });
    auto* loop = ready.get_future().get();
    loop->queue_in_loop([&] {
      ran.fetch_add(1, std::memory_order_relaxed);
      loop->quit();
    });
    loop_thread.join();
    require(ran.load(std::memory_order_relaxed) == 1, "queue_in_loop runs on loop thread");
  }

  {
    std::atomic<int> fired{0};
    oklib::net::EventLoop loop;
    loop.run_after(10ms, [&] {
      fired.fetch_add(1, std::memory_order_relaxed);
      loop.quit();
    });
    loop.loop();
    require(fired.load(std::memory_order_relaxed) == 1, "run_after fires once");
  }

  {
    std::atomic<int> fired{0};
    oklib::net::EventLoop loop;
    const oklib::net::TimerId custom_id(1234);
    const auto returned = loop.set_timer(10ms, [&](oklib::net::TimerId timer_id) {
      require(timer_id == custom_id, "set_timer callback receives custom timer id");
      fired.fetch_add(1, std::memory_order_relaxed);
      loop.quit();
    }, 1, custom_id);
    require(returned == custom_id, "set_timer returns custom timer id");
    loop.loop();
    require(fired.load(std::memory_order_relaxed) == 1, "set_timer fires custom timer once");
  }

  {
    std::atomic<int> first{0};
    std::atomic<int> second{0};
    oklib::net::EventLoop loop;
    const oklib::net::TimerId custom_id(5678);
    loop.set_timer(10ms, [&](oklib::net::TimerId) {
      first.fetch_add(1, std::memory_order_relaxed);
    }, 1, custom_id);
    loop.set_timer(30ms, [&](oklib::net::TimerId timer_id) {
      require(timer_id == custom_id, "replacement timer receives custom timer id");
      second.fetch_add(1, std::memory_order_relaxed);
    }, 1, custom_id);
    loop.run_after(80ms, [&] { loop.quit(); });
    loop.loop();
    require(first.load(std::memory_order_relaxed) == 0, "custom timer id replacement cancels old timer");
    require(second.load(std::memory_order_relaxed) == 1, "custom timer id replacement keeps new timer");
  }

  {
    std::atomic<int> fired{0};
    oklib::net::EventLoop loop;
    const oklib::net::TimerId custom_id(42);
    loop.set_timer(10ms, [&](oklib::net::TimerId timer_id) {
      require(timer_id == custom_id, "finite repeat callback receives custom timer id");
      fired.fetch_add(1, std::memory_order_relaxed);
    }, 3, custom_id);
    loop.run_after(80ms, [&] { loop.quit(); });
    loop.loop();
    require(fired.load(std::memory_order_relaxed) == 3, "set_timer finite repeat count is honored");
  }

  {
    int fds[2]{-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair succeeds");

    std::promise<void> ready;
    std::atomic<int> reads{0};
    std::thread loop_thread([&] {
      oklib::net::EventLoop loop;
      oklib::net::Channel channel(&loop, fds[1]);
      channel.set_read_callback([&](oklib::Timestamp) {
        char byte = 0;
        const auto n = ::read(fds[1], &byte, 1);
        if (n == 1 && byte == 'x') {
          reads.fetch_add(1, std::memory_order_relaxed);
        }
        loop.quit();
      });
      channel.enable_reading();
      ready.set_value();
      loop.loop();
      channel.disable_all();
      channel.remove();
    });

    ready.get_future().wait();
    char byte = 'x';
    require(::write(fds[0], &byte, 1) == 1, "socketpair write succeeds");
    loop_thread.join();
    require(reads.load(std::memory_order_relaxed) == 1, "channel handles read event");
    close_fd(fds[0]);
    close_fd(fds[1]);
  }

  return 0;
}
