#include "oklib/http/http_parser.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "oklib/http/http_semantics.h"
#include "oklib/net/buffer.h"

namespace oklib::http {
namespace {

bool is_ows(char c) noexcept {
  return c == ' ' || c == '\t';
}

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

std::string_view trim_ows(std::string_view value) {
  while (!value.empty() && is_ows(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_ows(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

std::string lowercase(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char c : value) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return result;
}

std::vector<std::string_view> split_commas(std::string_view value) {
  std::vector<std::string_view> result;
  while (!value.empty()) {
    const auto comma = value.find(',');
    if (comma == std::string_view::npos) {
      result.push_back(trim_ows(value));
      break;
    }
    result.push_back(trim_ows(value.substr(0, comma)));
    value.remove_prefix(comma + 1);
  }
  return result;
}

bool parse_decimal_u64(std::string_view value, std::uint64_t* output) {
  value = trim_ows(value);
  if (value.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed, 10);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  *output = parsed;
  return true;
}

bool parse_hex_u64(std::string_view value, std::uint64_t* output) {
  value = trim_ows(value);
  if (value.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed, 16);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  *output = parsed;
  return true;
}

bool is_token(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), is_token_char);
}

bool is_quoted_string(std::string_view value) {
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return false;
  }
  value.remove_prefix(1);
  value.remove_suffix(1);

  bool escaped = false;
  for (const char c : value) {
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      return false;
    }
  }
  return !escaped;
}

bool valid_chunk_extension_value(std::string_view value) {
  value = trim_ows(value);
  return is_token(value) || is_quoted_string(value);
}

bool valid_chunk_extensions(std::string_view value) {
  while (!value.empty()) {
    if (value.front() != ';') {
      return false;
    }
    value.remove_prefix(1);

    const auto next = value.find(';');
    const auto extension = trim_ows(next == std::string_view::npos ? value : value.substr(0, next));
    if (extension.empty()) {
      return false;
    }

    const auto equals = extension.find('=');
    const auto name = trim_ows(equals == std::string_view::npos ? extension : extension.substr(0, equals));
    if (!is_token(name)) {
      return false;
    }
    if (equals != std::string_view::npos &&
        !valid_chunk_extension_value(extension.substr(equals + 1))) {
      return false;
    }

    if (next == std::string_view::npos) {
      return true;
    }
    value.remove_prefix(next);
  }
  return true;
}

bool parse_version(std::string_view value, HttpVersion* version) {
  if (value == "HTTP/1.1") {
    *version = HttpVersion::http11;
    return true;
  }
  if (value == "HTTP/1.0") {
    *version = HttpVersion::http10;
    return true;
  }
  return false;
}

void set_request_target(HttpRequest* request, std::string_view target) {
  request->set_target(target);
  if (target == "*") {
    request->set_target_form(HttpRequestTargetForm::asterisk);
    request->set_path("*");
    request->set_query({});
    return;
  }

  const auto scheme = target.find("://");
  if (scheme != std::string_view::npos) {
    request->set_target_form(HttpRequestTargetForm::absolute);
    const auto path_start = target.find('/', scheme + 3);
    std::string_view path = path_start == std::string_view::npos ? std::string_view("/") : target.substr(path_start);
    const auto query = path.find('?');
    if (query == std::string_view::npos) {
      request->set_path(path);
      request->set_query({});
    } else {
      request->set_path(path.substr(0, query));
      request->set_query(path.substr(query + 1));
    }
    return;
  }

  if (!target.empty() && target.front() == '/') {
    request->set_target_form(HttpRequestTargetForm::origin);
    const auto query = target.find('?');
    if (query == std::string_view::npos) {
      request->set_path(target);
      request->set_query({});
    } else {
      request->set_path(target.substr(0, query));
      request->set_query(target.substr(query + 1));
    }
    return;
  }

  request->set_target_form(HttpRequestTargetForm::authority);
  request->set_path(target);
  request->set_query({});
}

std::vector<std::uint64_t> content_lengths(const HttpHeaders& headers, bool* ok) {
  std::vector<std::uint64_t> lengths;
  *ok = true;
  for (const auto& value : headers.values("Content-Length")) {
    for (const auto part : split_commas(value)) {
      std::uint64_t parsed = 0;
      if (!parse_decimal_u64(part, &parsed)) {
        *ok = false;
        return {};
      }
      lengths.push_back(parsed);
    }
  }
  return lengths;
}

bool is_chunked_transfer(const HttpHeaders& headers, bool* present, bool* ok) {
  *present = false;
  *ok = true;
  std::vector<std::string> codings;
  for (const auto& value : headers.values("Transfer-Encoding")) {
    for (const auto part : split_commas(value)) {
      auto coding = lowercase(part);
      const auto semicolon = coding.find(';');
      if (semicolon != std::string::npos) {
        coding.erase(semicolon);
        while (!coding.empty() && is_ows(coding.back())) {
          coding.pop_back();
        }
      }
      if (!coding.empty()) {
        codings.push_back(std::move(coding));
      }
    }
  }
  if (codings.empty()) {
    return false;
  }

  *present = true;
  if (codings.back() != "chunked") {
    *ok = false;
    return false;
  }
  return true;
}

bool forbidden_trailer_field(std::string_view field) {
  const auto lower = lowercase(field);
  return lower == "content-length" || lower == "transfer-encoding" || lower == "host";
}

}  // namespace

HttpParser::HttpParser(HttpParserMode mode, HttpParserOptions options)
    : mode_(mode),
      options_(options) {}

HttpParseStatus HttpParser::parse_request(oklib::net::Buffer* buffer, oklib::Timestamp receive_time) {
  mode_ = HttpParserMode::request;
  return parse(buffer, receive_time);
}

HttpParseStatus HttpParser::parse_request_head(oklib::net::Buffer* buffer, oklib::Timestamp receive_time) {
  mode_ = HttpParserMode::request;
  for (;;) {
    switch (state_) {
      case State::start_line: {
        const char* crlf = buffer->find_crlf();
        if (crlf == nullptr) {
          if (buffer->readable_bytes() > options_.max_start_line) {
            set_error(HttpParseError::bad_start_line);
            return HttpParseStatus::error;
          }
          return HttpParseStatus::incomplete;
        }
        const auto line_size = static_cast<std::size_t>(crlf - buffer->peek());
        if (line_size > options_.max_start_line) {
          set_error(HttpParseError::bad_start_line);
          return HttpParseStatus::error;
        }
        if (!parse_request_line(std::string_view(buffer->peek(), line_size), receive_time)) {
          return HttpParseStatus::error;
        }
        buffer->retrieve_until(crlf + 2);
        state_ = State::headers;
        break;
      }
      case State::headers: {
        const char* crlf = buffer->find_crlf();
        if (crlf == nullptr) {
          if (header_bytes_ + buffer->readable_bytes() > options_.max_headers) {
            set_error(HttpParseError::header_too_large);
            return HttpParseStatus::error;
          }
          return HttpParseStatus::incomplete;
        }

        const auto line_size = static_cast<std::size_t>(crlf - buffer->peek());
        header_bytes_ += line_size + 2;
        if (header_bytes_ > options_.max_headers) {
          set_error(HttpParseError::header_too_large);
          return HttpParseStatus::error;
        }

        if (line_size == 0) {
          buffer->retrieve_until(crlf + 2);
          if (!finish_headers()) {
            return HttpParseStatus::error;
          }
          return HttpParseStatus::complete;
        }

        if (!parse_header_line(std::string_view(buffer->peek(), line_size), &request_.mutable_headers())) {
          return HttpParseStatus::error;
        }
        buffer->retrieve_until(crlf + 2);
        break;
      }
      case State::fixed_body:
      case State::chunk_size:
      case State::chunk_data:
      case State::trailers:
      case State::complete:
        return HttpParseStatus::complete;
      case State::error:
        return HttpParseStatus::error;
    }
  }
}

HttpParseStatus HttpParser::parse_response(oklib::net::Buffer* buffer) {
  mode_ = HttpParserMode::response;
  return parse(buffer, oklib::Timestamp::invalid());
}

HttpParseStatus HttpParser::parse_response_head(oklib::net::Buffer* buffer) {
  mode_ = HttpParserMode::response;
  for (;;) {
    switch (state_) {
      case State::start_line: {
        const char* crlf = buffer->find_crlf();
        if (crlf == nullptr) {
          if (buffer->readable_bytes() > options_.max_start_line) {
            set_error(HttpParseError::bad_start_line);
            return HttpParseStatus::error;
          }
          return HttpParseStatus::incomplete;
        }
        const auto line_size = static_cast<std::size_t>(crlf - buffer->peek());
        if (line_size > options_.max_start_line) {
          set_error(HttpParseError::bad_start_line);
          return HttpParseStatus::error;
        }
        if (!parse_response_line(std::string_view(buffer->peek(), line_size))) {
          return HttpParseStatus::error;
        }
        buffer->retrieve_until(crlf + 2);
        state_ = State::headers;
        break;
      }
      case State::headers: {
        const char* crlf = buffer->find_crlf();
        if (crlf == nullptr) {
          if (header_bytes_ + buffer->readable_bytes() > options_.max_headers) {
            set_error(HttpParseError::header_too_large);
            return HttpParseStatus::error;
          }
          return HttpParseStatus::incomplete;
        }

        const auto line_size = static_cast<std::size_t>(crlf - buffer->peek());
        header_bytes_ += line_size + 2;
        if (header_bytes_ > options_.max_headers) {
          set_error(HttpParseError::header_too_large);
          return HttpParseStatus::error;
        }

        if (line_size == 0) {
          buffer->retrieve_until(crlf + 2);
          if (!finish_headers()) {
            return HttpParseStatus::error;
          }
          return HttpParseStatus::complete;
        }

        if (!parse_header_line(std::string_view(buffer->peek(), line_size), &response_.headers)) {
          return HttpParseStatus::error;
        }
        buffer->retrieve_until(crlf + 2);
        break;
      }
      case State::fixed_body:
      case State::chunk_size:
      case State::chunk_data:
      case State::trailers:
      case State::complete:
        return HttpParseStatus::complete;
      case State::error:
        return HttpParseStatus::error;
    }
  }
}

void HttpParser::reset() {
  state_ = State::start_line;
  error_ = HttpParseError::none;
  request_ = HttpRequest();
  response_ = HttpResponseMessage();
  header_bytes_ = 0;
  fixed_body_bytes_ = 0;
  current_chunk_size_ = 0;
  decoded_body_bytes_ = 0;
  chunked_ = false;
}

bool HttpParser::complete() const noexcept {
  return state_ == State::complete;
}

HttpParseStatus HttpParser::parse(oklib::net::Buffer* buffer, oklib::Timestamp receive_time) {
  for (;;) {
    switch (state_) {
      case State::start_line: {
        const char* crlf = buffer->find_crlf();
        if (crlf == nullptr) {
          if (buffer->readable_bytes() > options_.max_start_line) {
            set_error(HttpParseError::bad_start_line);
            return HttpParseStatus::error;
          }
          return HttpParseStatus::incomplete;
        }
        const auto line_size = static_cast<std::size_t>(crlf - buffer->peek());
        if (line_size > options_.max_start_line) {
          set_error(HttpParseError::bad_start_line);
          return HttpParseStatus::error;
        }
        const std::string_view line(buffer->peek(), line_size);
        const bool ok = mode_ == HttpParserMode::request ? parse_request_line(line, receive_time)
                                                         : parse_response_line(line);
        if (!ok) {
          return HttpParseStatus::error;
        }
        buffer->retrieve_until(crlf + 2);
        state_ = State::headers;
        break;
      }
      case State::headers: {
        const char* crlf = buffer->find_crlf();
        if (crlf == nullptr) {
          if (header_bytes_ + buffer->readable_bytes() > options_.max_headers) {
            set_error(HttpParseError::header_too_large);
            return HttpParseStatus::error;
          }
          return HttpParseStatus::incomplete;
        }

        const auto line_size = static_cast<std::size_t>(crlf - buffer->peek());
        header_bytes_ += line_size + 2;
        if (header_bytes_ > options_.max_headers) {
          set_error(HttpParseError::header_too_large);
          return HttpParseStatus::error;
        }

        if (line_size == 0) {
          buffer->retrieve_until(crlf + 2);
          if (!finish_headers()) {
            return HttpParseStatus::error;
          }
          if (state_ == State::complete) {
            return HttpParseStatus::complete;
          }
          break;
        }

        HttpHeaders* headers =
            mode_ == HttpParserMode::request ? &request_.mutable_headers() : &response_.headers;
        if (!parse_header_line(std::string_view(buffer->peek(), line_size), headers)) {
          return HttpParseStatus::error;
        }
        buffer->retrieve_until(crlf + 2);
        break;
      }
      case State::fixed_body:
        if (!parse_fixed_body(buffer)) {
          return error_ == HttpParseError::none ? HttpParseStatus::incomplete : HttpParseStatus::error;
        }
        return HttpParseStatus::complete;
      case State::chunk_size:
        if (!parse_chunk_size(buffer)) {
          return error_ == HttpParseError::none ? HttpParseStatus::incomplete : HttpParseStatus::error;
        }
        if (state_ == State::trailers) {
          break;
        }
        break;
      case State::chunk_data:
        if (!parse_chunk_data(buffer)) {
          return error_ == HttpParseError::none ? HttpParseStatus::incomplete : HttpParseStatus::error;
        }
        break;
      case State::trailers:
        if (!parse_trailers(buffer)) {
          return error_ == HttpParseError::none ? HttpParseStatus::incomplete : HttpParseStatus::error;
        }
        return HttpParseStatus::complete;
      case State::complete:
        return HttpParseStatus::complete;
      case State::error:
        return HttpParseStatus::error;
    }
  }
}

bool HttpParser::parse_request_line(std::string_view line, oklib::Timestamp receive_time) {
  const auto first_space = line.find(' ');
  if (first_space == std::string_view::npos) {
    return set_error(HttpParseError::bad_start_line);
  }
  const auto second_space = line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos || line.find(' ', second_space + 1) != std::string_view::npos) {
    return set_error(HttpParseError::bad_start_line);
  }

  if (!request_.set_method(line.substr(0, first_space))) {
    return set_error(HttpParseError::bad_start_line);
  }
  set_request_target(&request_, line.substr(first_space + 1, second_space - first_space - 1));

  HttpVersion version = HttpVersion::unknown;
  if (!parse_version(line.substr(second_space + 1), &version)) {
    return set_error(HttpParseError::bad_start_line);
  }
  request_.set_version(version);
  request_.set_receive_time(receive_time);
  return true;
}

bool HttpParser::parse_response_line(std::string_view line) {
  const auto first_space = line.find(' ');
  if (first_space == std::string_view::npos) {
    return set_error(HttpParseError::bad_start_line);
  }

  HttpVersion version = HttpVersion::unknown;
  if (!parse_version(line.substr(0, first_space), &version)) {
    return set_error(HttpParseError::bad_start_line);
  }

  std::string_view rest = line.substr(first_space + 1);
  const auto second_space = rest.find(' ');
  const auto status_token = second_space == std::string_view::npos ? rest : rest.substr(0, second_space);
  if (status_token.size() != 3) {
    return set_error(HttpParseError::bad_start_line);
  }

  int status = 0;
  const auto* begin = status_token.data();
  const auto* end = status_token.data() + status_token.size();
  const auto result = std::from_chars(begin, end, status, 10);
  if (result.ec != std::errc{} || result.ptr != end) {
    return set_error(HttpParseError::bad_start_line);
  }

  response_.version = version;
  response_.status_code = status;
  response_.reason_phrase =
      second_space == std::string_view::npos ? std::string{} : std::string(rest.substr(second_space + 1));
  return true;
}

bool HttpParser::parse_header_line(std::string_view line, HttpHeaders* headers) {
  if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
    return set_error(HttpParseError::bad_header);
  }

  const auto colon = line.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return set_error(HttpParseError::bad_header);
  }
  const auto field = line.substr(0, colon);
  for (const char c : field) {
    if (!is_token_char(c)) {
      return set_error(HttpParseError::bad_header);
    }
  }
  headers->add(field, line.substr(colon + 1));
  return true;
}

bool HttpParser::finish_headers() {
  const HttpHeaders& headers =
      mode_ == HttpParserMode::request ? request_.headers() : response_.headers;

  bool content_lengths_ok = true;
  const auto lengths = content_lengths(headers, &content_lengths_ok);
  if (!content_lengths_ok) {
    return set_error(HttpParseError::bad_message_framing);
  }

  std::optional<std::uint64_t> content_length;
  for (const auto length : lengths) {
    if (!content_length.has_value()) {
      content_length = length;
    } else if (*content_length != length) {
      return set_error(HttpParseError::bad_message_framing);
    }
  }

  bool transfer_encoding_present = false;
  bool transfer_encoding_ok = true;
  const bool is_chunked = is_chunked_transfer(headers, &transfer_encoding_present, &transfer_encoding_ok);
  if (!transfer_encoding_ok) {
    return set_error(HttpParseError::bad_message_framing);
  }
  if (transfer_encoding_present && content_length.has_value()) {
    return set_error(HttpParseError::bad_message_framing);
  }

  if (mode_ == HttpParserMode::response && !status_code_allows_body(response_.status_code)) {
    if (content_length.has_value()) {
      response_.content_length = *content_length;
    }
    return finish_message();
  }

  if (mode_ == HttpParserMode::request && request_.version() == HttpVersion::http11 &&
      !request_.headers().contains("Host")) {
    return set_error(HttpParseError::bad_header);
  }

  if (is_chunked) {
    chunked_ = true;
    if (mode_ == HttpParserMode::response) {
      response_.chunked = true;
    }
    state_ = State::chunk_size;
    return true;
  }

  if (!content_length.has_value() || *content_length == 0) {
    if (content_length.has_value()) {
      if (mode_ == HttpParserMode::request) {
        request_.set_content_length(*content_length);
      } else {
        response_.content_length = *content_length;
      }
    }
    return finish_message();
  }

  if (*content_length > options_.max_body) {
    return set_error(HttpParseError::body_too_large);
  }

  fixed_body_bytes_ = *content_length;
  if (mode_ == HttpParserMode::request) {
    request_.set_content_length(*content_length);
  } else {
    response_.content_length = *content_length;
  }
  state_ = State::fixed_body;
  return true;
}

bool HttpParser::parse_fixed_body(oklib::net::Buffer* buffer) {
  if (buffer->readable_bytes() < fixed_body_bytes_) {
    return false;
  }

  const auto body = buffer->retrieve_as_string(static_cast<std::size_t>(fixed_body_bytes_));
  if (mode_ == HttpParserMode::request) {
    request_.set_body(body);
  } else {
    response_.body = body;
  }
  return finish_message();
}

bool HttpParser::parse_chunk_size(oklib::net::Buffer* buffer) {
  const char* crlf = buffer->find_crlf();
  if (crlf == nullptr) {
    return false;
  }

  std::string_view line(buffer->peek(), static_cast<std::size_t>(crlf - buffer->peek()));
  const auto semicolon = line.find(';');
  const auto size_part = semicolon == std::string_view::npos ? line : line.substr(0, semicolon);
  if (semicolon != std::string_view::npos && !valid_chunk_extensions(line.substr(semicolon))) {
    return set_error(HttpParseError::bad_message_framing);
  }
  std::uint64_t size = 0;
  if (!parse_hex_u64(size_part, &size)) {
    return set_error(HttpParseError::bad_message_framing);
  }

  buffer->retrieve_until(crlf + 2);
  current_chunk_size_ = size;
  if (current_chunk_size_ == 0) {
    state_ = State::trailers;
  } else {
    if (decoded_body_bytes_ > options_.max_body ||
        current_chunk_size_ > options_.max_body - decoded_body_bytes_) {
      return set_error(HttpParseError::body_too_large);
    }
    state_ = State::chunk_data;
  }
  return true;
}

bool HttpParser::parse_chunk_data(oklib::net::Buffer* buffer) {
  const std::uint64_t required = current_chunk_size_ + 2;
  if (required > std::numeric_limits<std::size_t>::max()) {
    return set_error(HttpParseError::body_too_large);
  }
  if (buffer->readable_bytes() < required) {
    return false;
  }

  const auto chunk_size = static_cast<std::size_t>(current_chunk_size_);
  if (buffer->peek()[chunk_size] != '\r' || buffer->peek()[chunk_size + 1] != '\n') {
    return set_error(HttpParseError::bad_message_framing);
  }

  if (mode_ == HttpParserMode::request) {
    request_.append_body(std::string_view(buffer->peek(), chunk_size));
  } else {
    response_.body.append(buffer->peek(), chunk_size);
  }
  decoded_body_bytes_ += current_chunk_size_;
  buffer->retrieve(chunk_size + 2);
  state_ = State::chunk_size;
  return true;
}

bool HttpParser::parse_trailers(oklib::net::Buffer* buffer) {
  for (;;) {
    const char* crlf = buffer->find_crlf();
    if (crlf == nullptr) {
      return false;
    }

    const auto line_size = static_cast<std::size_t>(crlf - buffer->peek());
    if (line_size == 0) {
      buffer->retrieve_until(crlf + 2);
      return finish_message();
    }

    HttpHeaders* trailers =
        mode_ == HttpParserMode::request ? &request_.mutable_trailers() : &response_.trailers;
    const std::string_view line(buffer->peek(), line_size);
    const auto colon = line.find(':');
    if (colon != std::string_view::npos && forbidden_trailer_field(line.substr(0, colon))) {
      return set_error(HttpParseError::bad_header);
    }
    if (!parse_header_line(line, trailers)) {
      return false;
    }
    buffer->retrieve_until(crlf + 2);
  }
}

bool HttpParser::finish_message() {
  state_ = State::complete;
  return true;
}

bool HttpParser::set_error(HttpParseError error) noexcept {
  error_ = error;
  state_ = State::error;
  return false;
}

}  // namespace oklib::http
