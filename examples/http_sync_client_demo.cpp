#include <chrono>
#include <iostream>
#include <string>

#include "oklib/http/http_sync_client.h"

namespace {

const char* source_name(oklib::http::HttpClientResponseSource source) {
  switch (source) {
    case oklib::http::HttpClientResponseSource::network:
      return "network";
    case oklib::http::HttpClientResponseSource::cache:
      return "cache";
    case oklib::http::HttpClientResponseSource::revalidated:
      return "revalidated";
  }
  return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
  const std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:8080/echo";
  const std::string body = argc > 2 ? argv[2] : "This is a sync request.";

  oklib::http::HttpSyncClient client;
  oklib::http::HttpClientRequest request("POST", "/");
  request.set_url(url);
  request.set_header("Content-Type", "text/plain; charset=utf-8");
  request.set_body(body);
  request.set_timeout(std::chrono::seconds(10));

  oklib::http::HttpResponseMessage response;
  const int ret = client.send(&request, &response);
  if (ret != static_cast<int>(oklib::http::HttpSyncClientError::ok)) {
    std::cerr << "request failed: " << oklib::http::http_sync_client_error_message(ret) << '\n';
    return 1;
  }

  std::cout << response.status_code << " " << response.reason_phrase
            << " source=" << source_name(response.source)
            << " bytes=" << response.body.size() << '\n';
  if (!response.body.empty()) {
    std::cout << response.body << '\n';
  }
  return 0;
}
