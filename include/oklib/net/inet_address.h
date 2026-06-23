#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace oklib::net {

class InetAddress {
 public:
  InetAddress();
  explicit InetAddress(uint16_t port, bool loopback_only = false);
  InetAddress(std::string_view ip, uint16_t port);
  explicit InetAddress(const sockaddr_in& address);

  static InetAddress any(uint16_t port);
  static InetAddress loopback(uint16_t port);

  [[nodiscard]] const sockaddr* sockaddr_ptr() const noexcept {
    return reinterpret_cast<const sockaddr*>(&address_);
  }
  [[nodiscard]] sockaddr* sockaddr_ptr() noexcept {
    return reinterpret_cast<sockaddr*>(&address_);
  }
  [[nodiscard]] socklen_t length() const noexcept { return sizeof(address_); }

  [[nodiscard]] uint16_t port() const noexcept;
  [[nodiscard]] std::string to_ip() const;
  [[nodiscard]] std::string to_ip_port() const;
  [[nodiscard]] const sockaddr_in& raw() const noexcept { return address_; }

 private:
  sockaddr_in address_{};
};

}  // namespace oklib::net
