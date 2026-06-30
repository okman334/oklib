#include "oklib/net/resolver.h"

#include <netdb.h>
#include <netinet/in.h>

#include <memory>
#include <string>

namespace oklib::net {

std::vector<InetAddress> resolve_tcp_addresses(std::string_view host, std::uint16_t port) {
  std::vector<InetAddress> addresses;
  if (host.empty() || port == 0) {
    return addresses;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* result = nullptr;
  const std::string host_string(host);
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host_string.c_str(), service.c_str(), &hints, &result) != 0) {
    return addresses;
  }
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(result, ::freeaddrinfo);

  for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
    if ((ai->ai_family == AF_INET || ai->ai_family == AF_INET6) && ai->ai_addr != nullptr) {
      addresses.emplace_back(ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
    }
  }
  return addresses;
}

}  // namespace oklib::net
