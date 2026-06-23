#include "oklib/http/http_request.h"

#include <algorithm>

namespace oklib::http {
namespace {

std::string trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

}  // namespace

std::string HttpRequest::method_string() const {
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
    case HttpMethod::invalid:
      break;
  }
  return "INVALID";
}

bool HttpRequest::set_method(std::string_view method) {
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
  } else {
    method_ = HttpMethod::invalid;
  }
  return method_ != HttpMethod::invalid;
}

void HttpRequest::add_header(std::string_view field, std::string_view value) {
  headers_[trim(field)] = trim(value);
}

std::string HttpRequest::header(std::string_view field) const {
  auto it = headers_.find(std::string(field));
  if (it == headers_.end()) {
    return {};
  }
  return it->second;
}

}  // namespace oklib::http
