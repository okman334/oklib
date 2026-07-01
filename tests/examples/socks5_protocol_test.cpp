#include "socks5_protocol.h"

#include <oklib/net/inet_address.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> values) {
  return std::vector<std::uint8_t>(values);
}

}  // namespace

int main() {
  using namespace oklib::examples::socks5;

  AuthConfig no_auth;
  auto method = choose_auth_method(bytes({0x00, 0x02}), no_auth);
  require(method == AuthMethod::no_auth,
          "no-auth server selects no-auth when offered");

  AuthConfig password_auth;
  password_auth.username = "alice";
  password_auth.password = "secret";
  method = choose_auth_method(bytes({0x00, 0x02}), password_auth);
  require(method == AuthMethod::username_password,
          "password server selects username/password when offered");
  method = choose_auth_method(bytes({0x00}), password_auth);
  require(method == AuthMethod::no_acceptable_methods,
          "password server rejects clients that do not offer username/password");

  auto auth_ok = parse_user_password_auth(
      bytes({0x01, 0x05, 'a', 'l', 'i', 'c', 'e', 0x06, 's', 'e', 'c', 'r',
             'e', 't'}),
      password_auth);
  require(auth_ok.ok, "valid username/password auth succeeds");
  require(auth_ok.consumed == 14, "valid auth consumes all auth bytes");

  auto auth_bad = parse_user_password_auth(
      bytes({0x01, 0x05, 'a', 'l', 'i', 'c', 'e', 0x05, 'w', 'r', 'o', 'n',
             'g'}),
      password_auth);
  require(!auth_bad.ok, "wrong password fails");
  require(auth_bad.consumed == 13,
          "failed auth consumes the complete auth packet");

  auto ipv4 = parse_connect_request(
      bytes({0x05, 0x01, 0x00, 0x01, 127, 0, 0, 1, 0x1F, 0x90}));
  require(ipv4.status == ParseStatus::complete, "IPv4 connect request parses");
  require(ipv4.target.type == AddressType::ipv4,
          "IPv4 request records address type");
  require(ipv4.target.host == "127.0.0.1", "IPv4 host is formatted");
  require(ipv4.target.port == 8080, "IPv4 port is parsed");
  require(ipv4.consumed == 10, "IPv4 request consumes exact request length");

  auto nonzero_rsv = parse_connect_request(
      bytes({0x05, 0x01, 0xff, 0x01, 127, 0, 0, 1, 0, 80}));
  require(nonzero_rsv.status == ParseStatus::error,
          "CONNECT request with nonzero reserved byte is rejected");
  require(nonzero_rsv.reply == ReplyCode::general_failure,
          "nonzero reserved byte maps to general failure");

  auto domain = parse_connect_request(
      bytes({0x05, 0x01, 0x00, 0x03, 11, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
             '.', 'c', 'o', 'm', 0x00, 0x50}));
  require(domain.status == ParseStatus::complete,
          "domain connect request parses");
  require(domain.target.type == AddressType::domain,
          "domain request records address type");
  require(domain.target.host == "example.com", "domain host is parsed");
  require(domain.target.port == 80, "domain port is parsed");
  require(domain.consumed == 18,
          "domain request consumes exact request length");

  auto domain_prefix = parse_connect_request(bytes({0x05, 0x01, 0x00, 0x03}));
  require(domain_prefix.status == ParseStatus::need_more,
          "domain request without length waits for more bytes");

  auto short_request =
      parse_connect_request(bytes({0x05, 0x01, 0x00, 0x03, 11, 'e'}));
  require(short_request.status == ParseStatus::need_more,
          "short domain request waits for more bytes");

  auto ipv6 = parse_connect_request(
      bytes({0x05, 0x01, 0x00, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
             0, 1, 0, 80}));
  require(ipv6.status == ParseStatus::complete, "IPv6 connect request parses");
  require(ipv6.target.type == AddressType::ipv6,
          "IPv6 request records address type");
  require(ipv6.target.host == "::1", "IPv6 host is formatted");
  require(ipv6.target.port == 80, "IPv6 port is parsed");
  require(ipv6.consumed == 22, "IPv6 request consumes exact request length");

  auto short_ipv6 = parse_connect_request(
      bytes({0x05, 0x01, 0x00, 0x04, 0, 0, 0, 0, 0, 0}));
  require(short_ipv6.status == ParseStatus::need_more,
          "short IPv6 request waits for more bytes");

  auto success = build_reply(ReplyCode::success);
  require(success == bytes({0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0}),
          "success reply uses IPv4 wildcard bind address");

  auto bound_ipv4 =
      build_ipv4_reply(ReplyCode::success,
                       std::array<std::uint8_t, 4>{127, 0, 0, 1},
                       8080);
  require(bound_ipv4 ==
              bytes({0x05, 0x00, 0x00, 0x01, 127, 0, 0, 1, 0x1F, 0x90}),
          "success reply can include IPv4 bind address and port");

  auto bound_ipv6 =
      build_ipv6_reply(ReplyCode::success,
                       std::array<std::uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0,
                                                    0, 0, 0, 0, 0, 0, 0, 1},
                       443);
  require(bound_ipv6 ==
              bytes({0x05, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                     0, 0, 0, 0, 0, 1, 0x01, 0xBB}),
          "success reply can include IPv6 bind address and port");

  auto inet_bound_ipv4 =
      build_bound_reply(ReplyCode::success,
                        oklib::net::InetAddress("127.0.0.1", 8080));
  require(inet_bound_ipv4 ==
              bytes({0x05, 0x00, 0x00, 0x01, 127, 0, 0, 1, 0x1F, 0x90}),
          "success reply can be built from IPv4 InetAddress");

  auto inet_bound_ipv6 =
      build_bound_reply(ReplyCode::success,
                        oklib::net::InetAddress("::1", 443));
  require(inet_bound_ipv6 ==
              bytes({0x05, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                     0, 0, 0, 0, 0, 1, 0x01, 0xBB}),
          "success reply can be built from IPv6 InetAddress");

  return EXIT_SUCCESS;
}
