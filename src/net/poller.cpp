#include "oklib/net/poller.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

#include "oklib/base/logging.h"
#include "oklib/net/channel.h"

#if defined(__linux__)
#include <sys/epoll.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#else
#error "oklib::net::Poller currently supports Linux epoll and macOS kqueue"
#endif

namespace oklib::net {
namespace {

#if defined(__linux__)
uint32_t to_native_events(int events) {
  uint32_t native = 0;
  if ((events & Channel::k_read_event) != 0) {
    native |= EPOLLIN | EPOLLPRI | EPOLLRDHUP;
  }
  if ((events & Channel::k_write_event) != 0) {
    native |= EPOLLOUT;
  }
  return native;
}

int from_native_events(uint32_t events) {
  int result = 0;
  if ((events & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) != 0) {
    result |= Channel::k_read_event;
  }
  if ((events & EPOLLOUT) != 0) {
    result |= Channel::k_write_event;
  }
  if ((events & EPOLLHUP) != 0) {
    result |= Channel::k_close_event;
  }
  if ((events & EPOLLERR) != 0) {
    result |= Channel::k_error_event;
  }
  return result;
}
#endif

}  // namespace

#if defined(__linux__)
struct Poller::EpollEventStorage {
  std::vector<epoll_event> events{64};
};
#elif defined(__APPLE__)
struct Poller::KqueueEventStorage {
  std::vector<struct kevent> events{64};
};
#endif

Poller::Poller(EventLoop* owner_loop) : owner_loop_(owner_loop) {
#if defined(__linux__)
  poll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (poll_fd_ < 0) {
    throw std::system_error(errno, std::generic_category(), "epoll_create1");
  }
  event_storage_ = std::make_unique<EpollEventStorage>();
#elif defined(__APPLE__)
  poll_fd_ = ::kqueue();
  if (poll_fd_ < 0) {
    throw std::system_error(errno, std::generic_category(), "kqueue");
  }
  event_storage_ = std::make_unique<KqueueEventStorage>();
#endif
}

Poller::~Poller() {
  if (poll_fd_ >= 0) {
    ::close(poll_fd_);
  }
}

oklib::Timestamp Poller::poll(int timeout_ms, ChannelList* active_channels) {
#if defined(__linux__)
  const int num_events =
      ::epoll_wait(poll_fd_, event_storage_->events.data(),
                   static_cast<int>(event_storage_->events.size()), timeout_ms);
  const auto now = oklib::Timestamp::now();
  if (num_events < 0) {
    if (errno != EINTR) {
      OKLIB_LOG_ERROR << "epoll_wait failed: " << std::strerror(errno);
    }
    return now;
  }
  for (int i = 0; i < num_events; ++i) {
    auto* channel = static_cast<Channel*>(event_storage_->events[static_cast<std::size_t>(i)].data.ptr);
    channel->set_revents(from_native_events(event_storage_->events[static_cast<std::size_t>(i)].events));
    active_channels->push_back(channel);
  }
  if (static_cast<std::size_t>(num_events) == event_storage_->events.size()) {
    event_storage_->events.resize(event_storage_->events.size() * 2);
  }
  return now;
#elif defined(__APPLE__)
  timespec timeout{};
  timespec* timeout_ptr = nullptr;
  if (timeout_ms >= 0) {
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_nsec = (timeout_ms % 1000) * 1000 * 1000;
    timeout_ptr = &timeout;
  }

  const int num_events =
      ::kevent(poll_fd_, nullptr, 0, event_storage_->events.data(),
               static_cast<int>(event_storage_->events.size()), timeout_ptr);
  const auto now = oklib::Timestamp::now();
  if (num_events < 0) {
    if (errno != EINTR) {
      OKLIB_LOG_ERROR << "kevent wait failed: " << std::strerror(errno);
    }
    return now;
  }

  std::unordered_map<Channel*, int> aggregated;
  for (int i = 0; i < num_events; ++i) {
    const auto& event = event_storage_->events[static_cast<std::size_t>(i)];
    auto* channel = static_cast<Channel*>(event.udata);
    int revents = 0;
    if ((event.flags & EV_ERROR) != 0) {
      revents |= Channel::k_error_event;
    }
    if ((event.flags & EV_EOF) != 0) {
      revents |= Channel::k_close_event;
    }
    if (event.filter == EVFILT_READ) {
      revents |= Channel::k_read_event;
    } else if (event.filter == EVFILT_WRITE) {
      revents |= Channel::k_write_event;
    }
    aggregated[channel] |= revents;
  }
  for (const auto& [channel, revents] : aggregated) {
    channel->set_revents(revents);
    active_channels->push_back(channel);
  }
  if (static_cast<std::size_t>(num_events) == event_storage_->events.size()) {
    event_storage_->events.resize(event_storage_->events.size() * 2);
  }
  return now;
#endif
}

void Poller::update_channel(Channel* channel) {
  const int fd = channel->fd();
  const int target_events = channel->events();

#if defined(__linux__)
  if (target_events == Channel::k_none_event) {
    remove_channel(channel);
    return;
  }

  epoll_event event{};
  event.events = to_native_events(target_events);
  event.data.ptr = channel;
  const bool exists = channels_.contains(fd);
  const int operation = exists ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  if (::epoll_ctl(poll_fd_, operation, fd, &event) != 0) {
    throw std::system_error(errno, std::generic_category(), "epoll_ctl update");
  }
  channels_[fd] = channel;
  interests_[fd] = target_events;
#elif defined(__APPLE__)
  const int previous_events = interests_.contains(fd) ? interests_[fd] : Channel::k_none_event;
  std::vector<struct kevent> changes;
  changes.reserve(2);

  auto update_filter = [&](int event_bit, int16_t filter) {
    const bool previously_enabled = (previous_events & event_bit) != 0;
    const bool target_enabled = (target_events & event_bit) != 0;
    if (previously_enabled == target_enabled) {
      return;
    }
    struct kevent change {};
    EV_SET(&change, static_cast<uintptr_t>(fd), filter,
           target_enabled ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, channel);
    changes.push_back(change);
  };

  update_filter(Channel::k_read_event, EVFILT_READ);
  update_filter(Channel::k_write_event, EVFILT_WRITE);

  if (!changes.empty() &&
      ::kevent(poll_fd_, changes.data(), static_cast<int>(changes.size()), nullptr, 0, nullptr) != 0) {
    if (!(target_events == Channel::k_none_event && errno == ENOENT)) {
      throw std::system_error(errno, std::generic_category(), "kevent update");
    }
  }

  if (target_events == Channel::k_none_event) {
    channels_.erase(fd);
    interests_.erase(fd);
  } else {
    channels_[fd] = channel;
    interests_[fd] = target_events;
  }
#endif
}

void Poller::remove_channel(Channel* channel) {
  const int fd = channel->fd();
  if (!channels_.contains(fd)) {
    return;
  }

#if defined(__linux__)
  epoll_event event{};
  if (::epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, &event) != 0 && errno != ENOENT && errno != EBADF) {
    throw std::system_error(errno, std::generic_category(), "epoll_ctl delete");
  }
#elif defined(__APPLE__)
  const int current_events = interests_.contains(fd) ? interests_[fd] : Channel::k_none_event;
  std::vector<struct kevent> changes;
  if ((current_events & Channel::k_read_event) != 0) {
    struct kevent change {};
    EV_SET(&change, static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    changes.push_back(change);
  }
  if ((current_events & Channel::k_write_event) != 0) {
    struct kevent change {};
    EV_SET(&change, static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    changes.push_back(change);
  }
  if (!changes.empty() &&
      ::kevent(poll_fd_, changes.data(), static_cast<int>(changes.size()), nullptr, 0, nullptr) != 0 &&
      errno != ENOENT) {
    throw std::system_error(errno, std::generic_category(), "kevent delete");
  }
#endif

  channels_.erase(fd);
  interests_.erase(fd);
}

bool Poller::has_channel(Channel* channel) const {
  const auto it = channels_.find(channel->fd());
  return it != channels_.end() && it->second == channel;
}

}  // namespace oklib::net
