#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "oklib/base/noncopyable.h"
#include "oklib/net/callbacks.h"
#include "oklib/net/inet_address.h"

namespace oklib::net {

class Acceptor;
class EventLoop;
class EventLoopThreadPool;

class TcpServer : private oklib::Noncopyable {
 public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;

  enum class Option {
    no_reuse_port,
    reuse_port,
  };

  TcpServer(EventLoop* loop, const InetAddress& listen_address, std::string name,
            Option option = Option::no_reuse_port);
  ~TcpServer();

  [[nodiscard]] EventLoop* loop() const noexcept { return loop_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] uint16_t port() const;
  [[nodiscard]] InetAddress listen_address() const;

  void set_thread_num(int num_threads);
  void set_thread_init_callback(ThreadInitCallback callback) { thread_init_callback_ = std::move(callback); }
  void start();

  void set_connection_callback(ConnectionCallback callback) { connection_callback_ = std::move(callback); }
  void set_message_callback(MessageCallback callback) { message_callback_ = std::move(callback); }
  void set_write_complete_callback(WriteCompleteCallback callback) { write_complete_callback_ = std::move(callback); }
  void set_high_water_mark_callback(HighWaterMarkCallback callback, std::size_t mark) {
    high_water_mark_callback_ = std::move(callback);
    high_water_mark_ = mark;
  }

 private:
  void new_connection(int sockfd, const InetAddress& peer_address);
  void remove_connection(const TcpConnectionPtr& connection);
  void remove_connection_in_loop(const TcpConnectionPtr& connection);

  using ConnectionMap = std::map<std::string, TcpConnectionPtr>;

  EventLoop* loop_;
  std::string name_;
  std::unique_ptr<Acceptor> acceptor_;
  std::shared_ptr<EventLoopThreadPool> thread_pool_;
  ConnectionCallback connection_callback_{default_connection_callback};
  MessageCallback message_callback_{default_message_callback};
  WriteCompleteCallback write_complete_callback_;
  HighWaterMarkCallback high_water_mark_callback_;
  std::size_t high_water_mark_{64 * 1024 * 1024};
  ThreadInitCallback thread_init_callback_;
  std::atomic<bool> started_{false};
  int next_connection_id_{1};
  ConnectionMap connections_;
};

}  // namespace oklib::net
