#include "oklib/net/inet_address.h"

#include <arpa/inet.h>

#include <cstring>
#include <stdexcept>

namespace oklib::net {

InetAddress::InetAddress() : InetAddress(0, false) {}

InetAddress::InetAddress(uint16_t port, bool loopback_only) {
  std::memset(&address_, 0, sizeof(address_));
  address_.sin_family = AF_INET;
  address_.sin_addr.s_addr = loopback_only ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
  address_.sin_port = htons(port);
}

InetAddress::InetAddress(std::string_view ip, uint16_t port) {
  std::memset(&address_, 0, sizeof(address_));
  address_.sin_family = AF_INET;
  address_.sin_port = htons(port);
  std::string ip_string(ip);
  if (::inet_pton(AF_INET, ip_string.c_str(), &address_.sin_addr) != 1) {
    throw std::invalid_argument("invalid IPv4 address: " + ip_string);
  }
}

InetAddress::InetAddress(const sockaddr_in& address) : address_(address) {}

InetAddress InetAddress::any(uint16_t port) {
  return InetAddress(port, false);
}

InetAddress InetAddress::loopback(uint16_t port) {
  return InetAddress(port, true);
}

uint16_t InetAddress::port() const noexcept {
  return ntohs(address_.sin_port);
}

std::string InetAddress::to_ip() const {
  char buf[INET_ADDRSTRLEN] = "";
  ::inet_ntop(AF_INET, &address_.sin_addr, buf, sizeof(buf));
  return buf;
}

std::string InetAddress::to_ip_port() const {
  return to_ip() + ":" + std::to_string(port());
}

}  // namespace oklib::net
