#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "oklib/base/noncopyable.h"
#include "oklib/base/timestamp.h"

namespace oklib::net {

class Channel;
class EventLoop;

class Poller : private oklib::Noncopyable {
 public:
  using ChannelList = std::vector<Channel*>;

  explicit Poller(EventLoop* owner_loop);
  ~Poller();

  oklib::Timestamp poll(int timeout_ms, ChannelList* active_channels);
  void update_channel(Channel* channel);
  void remove_channel(Channel* channel);
  [[nodiscard]] bool has_channel(Channel* channel) const;

 private:
  EventLoop* owner_loop_;
  int poll_fd_{-1};
  std::unordered_map<int, Channel*> channels_;
  std::unordered_map<int, int> interests_;

#if defined(__linux__)
  struct EpollEventStorage;
  std::unique_ptr<EpollEventStorage> event_storage_;
#elif defined(__APPLE__)
  struct KqueueEventStorage;
  std::unique_ptr<KqueueEventStorage> event_storage_;
#endif
};

}  // namespace oklib::net
