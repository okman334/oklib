#include "oklib/http/http_request.h"

#include <cctype>

namespace oklib::http {
namespace {

bool is_token_char(char c) noexcept {
  const auto ch = static_cast<unsigned char>(c);
  if (std::isalnum(ch)) {
    return true;
  }
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

}  // namespace

std::string HttpRequest::method_string() const {
  if (!method_token_.empty()) {
    return method_token_;
  }
  switch (method_) {
    case HttpMethod::get:
      return "GET";
    case HttpMethod::head:
      return "HEAD";
    case HttpMethod::post:
      return "POST";
    case HttpMethod::put:
      return "PUT";
    case HttpMethod::delete_:
      return "DELETE";
    case HttpMethod::options:
      return "OPTIONS";
    case HttpMethod::trace:
      return "TRACE";
    case HttpMethod::connect:
      return "CONNECT";
    case HttpMethod::patch:
      return "PATCH";
    case HttpMethod::invalid:
      break;
  }
  return "INVALID";
}

bool HttpRequest::set_method(std::string_view method) {
  if (method.empty()) {
    method_ = HttpMethod::invalid;
    method_token_.clear();
    return false;
  }
  for (const char c : method) {
    if (!is_token_char(c)) {
      method_ = HttpMethod::invalid;
      method_token_.clear();
      return false;
    }
  }

  method_token_.assign(method);
  if (method == "GET") {
    method_ = HttpMethod::get;
  } else if (method == "HEAD") {
    method_ = HttpMethod::head;
  } else if (method == "POST") {
    method_ = HttpMethod::post;
  } else if (method == "PUT") {
    method_ = HttpMethod::put;
  } else if (method == "DELETE") {
    method_ = HttpMethod::delete_;
  } else if (method == "OPTIONS") {
    method_ = HttpMethod::options;
  } else if (method == "TRACE") {
    method_ = HttpMethod::trace;
  } else if (method == "CONNECT") {
    method_ = HttpMethod::connect;
  } else if (method == "PATCH") {
    method_ = HttpMethod::patch;
  } else {
    method_ = HttpMethod::invalid;
  }
  return true;
}

void HttpRequest::set_target(std::string_view target) {
  target_.assign(target);
}

void HttpRequest::add_header(std::string_view field, std::string_view value) {
  headers_.add(field, value);
}

std::string HttpRequest::header(std::string_view field) const {
  return headers_.get(field);
}

}  // namespace oklib::http
