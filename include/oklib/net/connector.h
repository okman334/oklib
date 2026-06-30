#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "oklib/base/noncopyable.h"
#include "oklib/net/inet_address.h"

namespace oklib::net {

class Channel;
class EventLoop;

class Connector : private oklib::Noncopyable, public std::enable_shared_from_this<Connector> {
 public:
  using NewConnectionCallback = std::function<void(int sockfd)>;

  Connector(EventLoop* loop, InetAddress server_address);
  Connector(EventLoop* loop, std::vector<InetAddress> server_addresses);
  ~Connector();

  void set_new_connection_callback(NewConnectionCallback callback) {
    new_connection_callback_ = std::move(callback);
  }

  void start();
  void restart();
  void stop();
  void set_retry(bool retry) noexcept { retry_ = retry; }

 private:
  enum class State { disconnected, connecting, connected };

  void start_in_loop();
  void stop_in_loop();
  void connect();
  void connecting(int sockfd);
  void handle_write();
  void handle_error();
  void retry(int sockfd);
  [[nodiscard]] const InetAddress& current_address() const;
  int remove_and_reset_channel();
  void reset_channel();
  void set_state(State state) noexcept { state_ = state; }

  EventLoop* loop_;
  std::vector<InetAddress> server_addresses_;
  std::size_t next_address_index_{0};
  bool connect_{false};
  bool retry_{false};
  State state_{State::disconnected};
  std::unique_ptr<Channel> channel_;
  NewConnectionCallback new_connection_callback_;
  int retry_delay_ms_{500};
};

}  // namespace oklib::net
