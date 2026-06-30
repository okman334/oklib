#include <oklib/net/inet_address.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

void test_ipv4_address_compatibility() {
  oklib::net::InetAddress address("127.0.0.1", 8080);
  require(address.family() == AF_INET, "IPv4 address reports AF_INET");
  require(address.is_ipv4(), "IPv4 address reports is_ipv4");
  require(!address.is_ipv6(), "IPv4 address reports not is_ipv6");
  require(address.port() == 8080, "IPv4 address preserves port");
  require(address.to_ip() == "127.0.0.1", "IPv4 address formats IP");
  require(address.to_ip_port() == "127.0.0.1:8080", "IPv4 address formats IP:port");
  require(oklib::net::InetAddress::loopback(80).to_ip() == "127.0.0.1",
          "loopback remains IPv4");
  require(oklib::net::InetAddress::any(80).to_ip() == "0.0.0.0",
          "any remains IPv4");
  (void)address.raw();
}

void test_ipv6_address_support() {
  oklib::net::InetAddress address("::1", 8080);
  require(address.family() == AF_INET6, "IPv6 address reports AF_INET6");
  require(address.is_ipv6(), "IPv6 address reports is_ipv6");
  require(!address.is_ipv4(), "IPv6 address reports not is_ipv4");
  require(address.port() == 8080, "IPv6 address preserves port");
  require(address.to_ip() == "::1", "IPv6 address formats IP");
  require(address.to_ip_port() == "[::1]:8080", "IPv6 address formats bracketed IP:port");
  require(oklib::net::InetAddress::loopback_ipv6(80).to_ip() == "::1",
          "IPv6 loopback formats correctly");
  require(oklib::net::InetAddress::any_ipv6(80).to_ip() == "::",
          "IPv6 any formats correctly");
  (void)address.raw6();
}

void test_invalid_address_is_rejected() {
  bool rejected = false;
  try {
    (void)oklib::net::InetAddress("not-an-ip-address", 80);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "invalid IP literal is rejected");
}

}  // namespace

int main() {
  test_ipv4_address_compatibility();
  test_ipv6_address_support();
  test_invalid_address_is_rejected();
  return 0;
}
