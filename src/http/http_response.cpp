#include "oklib/http/http_response.h"

#include "oklib/net/buffer.h"

namespace oklib::http {

void HttpResponse::append_headers_to_buffer(oklib::net::Buffer* output, bool chunked) const {
  const auto code = static_cast<int>(status_code_);
  output->append("HTTP/1.1 " + std::to_string(code) + " " + status_message() + "\r\n");
  output->append(close_connection_ ? "Connection: close\r\n" : "Connection: Keep-Alive\r\n");
  if (chunked) {
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
  if (include_body) {
    output->append(body_);
  }
}

std::string HttpResponse::status_message() const {
  if (!status_message_.empty()) {
    return status_message_;
  }
  switch (status_code_) {
    case HttpStatusCode::ok:
      return "OK";
    case HttpStatusCode::bad_request:
      return "Bad Request";
    case HttpStatusCode::not_found:
      return "Not Found";
    case HttpStatusCode::not_implemented:
      return "Not Implemented";
    case HttpStatusCode::unknown:
      break;
  }
  return "Unknown";
}

}  // namespace oklib::http
