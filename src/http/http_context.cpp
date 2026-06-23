#include "oklib/http/http_context.h"

#include <algorithm>
#include <string_view>

#include "oklib/net/buffer.h"

namespace oklib::http {

bool HttpContext::parse_request(oklib::net::Buffer* buffer, oklib::Timestamp receive_time) {
  bool ok = true;
  bool has_more = true;
  while (has_more) {
    if (state_ == State::expect_request_line) {
      const char* crlf = buffer->find_crlf();
      if (crlf == nullptr) {
        has_more = false;
      } else {
        ok = process_request_line(buffer->peek(), crlf);
        if (!ok) {
          return false;
        }
        request_.set_receive_time(receive_time);
        buffer->retrieve_until(crlf + 2);
        state_ = State::expect_headers;
      }
    } else if (state_ == State::expect_headers) {
      const char* crlf = buffer->find_crlf();
      if (crlf == nullptr) {
        has_more = false;
      } else {
        const char* colon = std::find(buffer->peek(), crlf, ':');
        if (colon != crlf) {
          request_.add_header(std::string_view(buffer->peek(), static_cast<std::size_t>(colon - buffer->peek())),
                              std::string_view(colon + 1, static_cast<std::size_t>(crlf - colon - 1)));
        } else {
          state_ = State::got_all;
          has_more = false;
        }
        buffer->retrieve_until(crlf + 2);
      }
    } else {
      has_more = false;
    }
  }
  return ok;
}

void HttpContext::reset() {
  state_ = State::expect_request_line;
  request_ = HttpRequest();
}

bool HttpContext::process_request_line(const char* begin, const char* end) {
  const char* start = begin;
  const char* space = std::find(start, end, ' ');
  if (space == end || !request_.set_method(std::string_view(start, static_cast<std::size_t>(space - start)))) {
    return false;
  }

  start = space + 1;
  space = std::find(start, end, ' ');
  if (space == end) {
    return false;
  }

  const char* question = std::find(start, space, '?');
  if (question != space) {
    request_.set_path(std::string_view(start, static_cast<std::size_t>(question - start)));
    request_.set_query(std::string_view(question + 1, static_cast<std::size_t>(space - question - 1)));
  } else {
    request_.set_path(std::string_view(start, static_cast<std::size_t>(space - start)));
  }

  start = space + 1;
  std::string_view version(start, static_cast<std::size_t>(end - start));
  if (version == "HTTP/1.1") {
    request_.set_version(HttpVersion::http11);
  } else if (version == "HTTP/1.0") {
    request_.set_version(HttpVersion::http10);
  } else {
    return false;
  }
  return true;
}

}  // namespace oklib::http
