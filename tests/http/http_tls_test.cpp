#include <oklib/http/http_client.h>
#include <oklib/http/http_response.h>
#include <oklib/http/http_server.h>
#include <oklib/net/event_loop.h>
#include <oklib/net/inet_address.h>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

struct TestCertificate {
  std::filesystem::path directory;
  std::filesystem::path cert_file;
  std::filesystem::path key_file;
};

TestCertificate make_test_certificate() {
  char dir_template[] = "/tmp/oklib-tls-XXXXXX";
  char* dir = ::mkdtemp(dir_template);
  require(dir != nullptr, "temporary certificate directory created");

  TestCertificate certificate;
  certificate.directory = dir;
  certificate.cert_file = certificate.directory / "server.crt";
  certificate.key_file = certificate.directory / "server.key";

  const std::string command =
      "openssl req -x509 -newkey rsa:2048 -nodes -keyout " + certificate.key_file.string() +
      " -out " + certificate.cert_file.string() +
      " -subj /CN=localhost -days 1 >/dev/null 2>&1";
  require(std::system(command.c_str()) == 0, "self-signed certificate generated");
  return certificate;
}

void test_https_client_server_round_trip() {
  const auto certificate = make_test_certificate();

  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "https-roundtrip-server");
  oklib::http::TlsServerOptions server_tls;
  server_tls.enabled = true;
  server_tls.cert_file = certificate.cert_file.string();
  server_tls.key_file = certificate.key_file.string();
  server.set_tls_options(std::move(server_tls));
  server.set_http_callback([](const oklib::http::HttpRequest& request,
                              oklib::http::HttpResponse* response) {
    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->add_header("X-TLS", "yes");
    response->set_body("secure " + request.path());
  });
  server.start();

  oklib::http::HttpClientOptions options;
  options.tls.enabled = true;
  options.tls.verify_peer = false;
  options.tls.server_name = "localhost";
  oklib::http::HttpClient client(&loop,
                                 server.listen_address(),
                                 "localhost",
                                 "https-roundtrip-client",
                                 std::move(options));

  std::vector<oklib::http::HttpResponseMessage> responses;
  bool error = false;
  client.set_response_callback([&](oklib::http::HttpResponseMessage response) {
    responses.push_back(std::move(response));
    client.disconnect();
    loop.quit();
  });
  client.set_error_callback([&] {
    error = true;
    loop.quit();
  });

  client.send(oklib::http::HttpClientRequest("GET", "/tls"));
  loop.run_after(std::chrono::seconds(3), [&] { loop.quit(); });
  loop.loop();

  require(!error, "HTTPS round trip does not error");
  require(responses.size() == 1, "HTTPS client receives one response");
  require(responses[0].status_code == 200, "HTTPS response status parsed");
  require(responses[0].headers.get("X-TLS") == "yes", "HTTPS response headers parsed");
  require(responses[0].body == "secure /tls", "HTTPS response body parsed");
}

void test_https_client_rejects_untrusted_self_signed_certificate() {
  const auto certificate = make_test_certificate();

  oklib::net::EventLoop loop;
  oklib::http::HttpServer server(&loop, oklib::net::InetAddress::loopback(0), "https-verify-server");
  oklib::http::TlsServerOptions server_tls;
  server_tls.enabled = true;
  server_tls.cert_file = certificate.cert_file.string();
  server_tls.key_file = certificate.key_file.string();
  server.set_tls_options(std::move(server_tls));
  server.set_http_callback([](const oklib::http::HttpRequest&,
                              oklib::http::HttpResponse* response) {
    response->set_status_code(oklib::http::HttpStatusCode::ok);
    response->set_body("unexpected");
  });
  server.start();

  oklib::http::HttpClientOptions options;
  options.tls.enabled = true;
  options.tls.verify_peer = true;
  options.tls.server_name = "localhost";
  oklib::http::HttpClient client(&loop,
                                 server.listen_address(),
                                 "localhost",
                                 "https-verify-client",
                                 std::move(options));

  bool error = false;
  bool response = false;
  client.set_response_callback([&](oklib::http::HttpResponseMessage) {
    response = true;
    loop.quit();
  });
  client.set_error_callback([&] {
    error = true;
    loop.quit();
  });

  client.send(oklib::http::HttpClientRequest("GET", "/verify"));
  loop.run_after(std::chrono::seconds(3), [&] { loop.quit(); });
  loop.loop();

  require(error, "untrusted self-signed certificate triggers client error");
  require(!response, "untrusted certificate does not deliver response");
}

}  // namespace

int main() {
  test_https_client_server_round_trip();
  test_https_client_rejects_untrusted_self_signed_certificate();
  return 0;
}
