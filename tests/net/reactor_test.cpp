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
    std::jthread loop_thread([&] {
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
    int fds[2]{-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair succeeds");

    std::promise<void> ready;
    std::atomic<int> reads{0};
    std::jthread loop_thread([&] {
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
