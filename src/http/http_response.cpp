#include "oklib/http/http_response.h"

#include "oklib/http/http_semantics.h"
#include "oklib/net/buffer.h"

namespace oklib::http {

void HttpResponse::append_headers_to_buffer(oklib::net::Buffer* output, bool chunked) const {
  output->append("HTTP/1.1 " + std::to_string(status_code_) + " " + status_message() + "\r\n");
  output->append(close_connection_ ? "Connection: close\r\n" : "Connection: Keep-Alive\r\n");
  if (!status_code_allows_body(status_code_)) {
    output->append("Content-Length: 0\r\n");
  } else if (chunked) {
    output->append("Transfer-Encoding: chunked\r\n");
  } else {
    output->append("Content-Length: " + std::to_string(body_.size()) + "\r\n");
  }
  for (const auto& [field, value] : headers_) {
    output->append(field + ": " + value + "\r\n");
  }
  output->append("\r\n");
}

void HttpResponse::append_to_buffer(oklib::net::Buffer* output, bool include_body) const {
  append_headers_to_buffer(output);
  if (include_body && status_code_allows_body(status_code_)) {
    output->append(body_);
  }
}

std::string HttpResponse::status_message() const {
  if (!status_message_.empty()) {
    return status_message_;
  }
  const auto standard = standard_reason_phrase(status_code_);
  if (!standard.empty()) {
    return std::string(standard);
  }
  return "Unknown";
}

}  // namespace oklib::http
