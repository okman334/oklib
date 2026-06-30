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
  explicit InetAddress(const sockaddr_in6& address);
  InetAddress(const sockaddr* address, socklen_t length);

  static InetAddress any(uint16_t port);
  static InetAddress loopback(uint16_t port);
  static InetAddress any_ipv6(uint16_t port);
  static InetAddress loopback_ipv6(uint16_t port);

  [[nodiscard]] const sockaddr* sockaddr_ptr() const noexcept {
    return reinterpret_cast<const sockaddr*>(&storage_);
  }
  [[nodiscard]] sockaddr* sockaddr_ptr() noexcept {
    return reinterpret_cast<sockaddr*>(&storage_);
  }
  [[nodiscard]] socklen_t length() const noexcept { return length_; }

  [[nodiscard]] sa_family_t family() const noexcept;
  [[nodiscard]] bool is_ipv4() const noexcept { return family() == AF_INET; }
  [[nodiscard]] bool is_ipv6() const noexcept { return family() == AF_INET6; }
  [[nodiscard]] uint16_t port() const noexcept;
  [[nodiscard]] std::string to_ip() const;
  [[nodiscard]] std::string to_ip_port() const;
  [[nodiscard]] const sockaddr_in& raw() const noexcept {
    return *reinterpret_cast<const sockaddr_in*>(&storage_);
  }
  [[nodiscard]] const sockaddr_in6& raw6() const noexcept {
    return *reinterpret_cast<const sockaddr_in6*>(&storage_);
  }

 private:
  sockaddr_storage storage_{};
  socklen_t length_{sizeof(sockaddr_in)};
};

}  // namespace oklib::net
