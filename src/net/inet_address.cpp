#include "oklib/net/inet_address.h"

#include <arpa/inet.h>

#include <cstring>
#include <stdexcept>

namespace oklib::net {
namespace {

sockaddr_in* as_ipv4(sockaddr_storage* storage) noexcept {
  return reinterpret_cast<sockaddr_in*>(storage);
}

const sockaddr_in* as_ipv4(const sockaddr_storage* storage) noexcept {
  return reinterpret_cast<const sockaddr_in*>(storage);
}

sockaddr_in6* as_ipv6(sockaddr_storage* storage) noexcept {
  return reinterpret_cast<sockaddr_in6*>(storage);
}

const sockaddr_in6* as_ipv6(const sockaddr_storage* storage) noexcept {
  return reinterpret_cast<const sockaddr_in6*>(storage);
}

}  // namespace

InetAddress::InetAddress() : InetAddress(0, false) {}

InetAddress::InetAddress(uint16_t port, bool loopback_only) {
  std::memset(&storage_, 0, sizeof(storage_));
  auto* address = as_ipv4(&storage_);
  address->sin_family = AF_INET;
  address->sin_addr.s_addr = loopback_only ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
  address->sin_port = htons(port);
  length_ = sizeof(sockaddr_in);
}

InetAddress::InetAddress(std::string_view ip, uint16_t port) {
  std::memset(&storage_, 0, sizeof(storage_));
  std::string ip_string(ip);
  auto* ipv4 = as_ipv4(&storage_);
  ipv4->sin_family = AF_INET;
  ipv4->sin_port = htons(port);
  if (::inet_pton(AF_INET, ip_string.c_str(), &ipv4->sin_addr) == 1) {
    length_ = sizeof(sockaddr_in);
    return;
  }

  std::memset(&storage_, 0, sizeof(storage_));
  auto* ipv6 = as_ipv6(&storage_);
  ipv6->sin6_family = AF_INET6;
  ipv6->sin6_port = htons(port);
  if (::inet_pton(AF_INET6, ip_string.c_str(), &ipv6->sin6_addr) == 1) {
    length_ = sizeof(sockaddr_in6);
    return;
  }

  throw std::invalid_argument("invalid IP address: " + ip_string);
}

InetAddress::InetAddress(const sockaddr_in& address) {
  std::memset(&storage_, 0, sizeof(storage_));
  *as_ipv4(&storage_) = address;
  length_ = sizeof(sockaddr_in);
}

InetAddress::InetAddress(const sockaddr_in6& address) {
  std::memset(&storage_, 0, sizeof(storage_));
  *as_ipv6(&storage_) = address;
  length_ = sizeof(sockaddr_in6);
}

InetAddress::InetAddress(const sockaddr* address, socklen_t length) {
  if (address == nullptr) {
    throw std::invalid_argument("null socket address");
  }
  if (address->sa_family == AF_INET) {
    if (length < static_cast<socklen_t>(sizeof(sockaddr_in))) {
      throw std::invalid_argument("short IPv4 socket address");
    }
    std::memset(&storage_, 0, sizeof(storage_));
    *as_ipv4(&storage_) = *reinterpret_cast<const sockaddr_in*>(address);
    length_ = sizeof(sockaddr_in);
    return;
  }
  if (address->sa_family == AF_INET6) {
    if (length < static_cast<socklen_t>(sizeof(sockaddr_in6))) {
      throw std::invalid_argument("short IPv6 socket address");
    }
    std::memset(&storage_, 0, sizeof(storage_));
    *as_ipv6(&storage_) = *reinterpret_cast<const sockaddr_in6*>(address);
    length_ = sizeof(sockaddr_in6);
    return;
  }
  throw std::invalid_argument("unsupported socket address family");
}

InetAddress InetAddress::any(uint16_t port) {
  return InetAddress(port, false);
}

InetAddress InetAddress::loopback(uint16_t port) {
  return InetAddress(port, true);
}

InetAddress InetAddress::any_ipv6(uint16_t port) {
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_any;
  address.sin6_port = htons(port);
  return InetAddress(address);
}

InetAddress InetAddress::loopback_ipv6(uint16_t port) {
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_loopback;
  address.sin6_port = htons(port);
  return InetAddress(address);
}

sa_family_t InetAddress::family() const noexcept {
  return storage_.ss_family;
}

uint16_t InetAddress::port() const noexcept {
  if (is_ipv6()) {
    return ntohs(as_ipv6(&storage_)->sin6_port);
  }
  return ntohs(as_ipv4(&storage_)->sin_port);
}

std::string InetAddress::to_ip() const {
  if (is_ipv6()) {
    char buf[INET6_ADDRSTRLEN] = "";
    ::inet_ntop(AF_INET6, &as_ipv6(&storage_)->sin6_addr, buf, sizeof(buf));
    return buf;
  }

  char buf[INET_ADDRSTRLEN] = "";
  ::inet_ntop(AF_INET, &as_ipv4(&storage_)->sin_addr, buf, sizeof(buf));
  return buf;
}

std::string InetAddress::to_ip_port() const {
  if (is_ipv6()) {
    return "[" + to_ip() + "]:" + std::to_string(port());
  }
  return to_ip() + ":" + std::to_string(port());
}

}  // namespace oklib::net
