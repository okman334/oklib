#include <oklib/base/timestamp.h>
#include <oklib/http/http_headers.h>
#include <oklib/http/http_parser.h>
#include <oklib/net/buffer.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

oklib::Timestamp receive_time() {
  return oklib::Timestamp::from_unix_time(1);
}

void append(oklib::net::Buffer* buffer, std::string_view data) {
  buffer->append(data);
}

void test_headers_are_case_insensitive_and_keep_repeated_values() {
  oklib::http::HttpHeaders headers;
  headers.add("HOST", "localhost");
  headers.add("Set-Cookie", "a=1");
  headers.add("set-cookie", "b=2");

  require(headers.contains("host"), "contains is case insensitive");
  require(headers.get("Host") == "localhost", "get is case insensitive");
  require(headers.values("SET-cookie").size() == 2, "repeated header values are preserved");
  require(headers.values("set-cookie")[0] == "a=1", "first repeated value preserved");
  require(headers.values("set-cookie")[1] == "b=2", "second repeated value preserved");
  require(headers.entries()[0].field == "HOST", "original field spelling is preserved");
}

void test_parses_content_length_request_and_keeps_pipeline_leftover() {
  oklib::net::Buffer buffer;
  append(&buffer,
         "POST /submit?x=1 HTTP/1.1\r\n"
         "Host: localhost\r\n"
         "Content-Length: 5\r\n"
         "X-Test: one\r\n"
         "\r\n"
         "hello"
         "GET /next HTTP/1.1\r\n"
         "Host: localhost\r\n"
         "\r\n");

  oklib::http::HttpParser parser;
  require(parser.parse_request(&buffer, receive_time()) == oklib::http::HttpParseStatus::complete,
          "content-length request completes");
  const auto& request = parser.request();
  require(request.method_string() == "POST", "method parsed");
  require(request.path() == "/submit", "path parsed");
  require(request.query() == "x=1", "query parsed");
  require(request.header("host") == "localhost", "header lookup is case insensitive");
  require(request.body() == "hello", "body parsed");
  require(request.content_length() == 5, "content length exposed");
  require(buffer.readable_bytes() > 0, "pipeline bytes remain in buffer");

  parser.reset();
  require(parser.parse_request(&buffer, receive_time()) == oklib::http::HttpParseStatus::complete,
          "pipeline request completes");
  require(parser.request().method_string() == "GET", "pipeline method parsed");
  require(parser.request().path() == "/next", "pipeline path parsed");
}

void test_parses_chunked_request_with_trailers() {
  oklib::net::Buffer buffer;
  append(&buffer,
         "POST /chunked HTTP/1.1\r\n"
         "Host: localhost\r\n"
         "Transfer-Encoding: chunked\r\n"
         "Trailer: X-Trailer\r\n"
         "\r\n"
         "5\r\n"
         "hello\r\n"
         "6;ext=1\r\n"
         " world\r\n"
         "0\r\n"
         "X-Trailer: done\r\n"
         "\r\n");

  oklib::http::HttpParser parser;
  require(parser.parse_request(&buffer, receive_time()) == oklib::http::HttpParseStatus::complete,
          "chunked request completes");
  require(parser.request().body() == "hello world", "chunked body decoded");
  require(parser.request().trailers().get("x-trailer") == "done", "trailers parsed");
}

void test_rejects_ambiguous_message_framing() {
  {
    oklib::net::Buffer buffer;
    append(&buffer,
           "POST / HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Transfer-Encoding: chunked\r\n"
           "Content-Length: 5\r\n"
           "\r\n"
           "0\r\n\r\n");
    oklib::http::HttpParser parser;
    require(parser.parse_request(&buffer, receive_time()) == oklib::http::HttpParseStatus::error,
            "transfer-encoding plus content-length is rejected");
    require(parser.error() == oklib::http::HttpParseError::bad_message_framing,
            "te plus content-length reports framing error");
  }

  {
    oklib::net::Buffer buffer;
    append(&buffer,
           "POST / HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Content-Length: 5\r\n"
           "Content-Length: 6\r\n"
           "\r\n"
           "hello!");
    oklib::http::HttpParser parser;
    require(parser.parse_request(&buffer, receive_time()) == oklib::http::HttpParseStatus::error,
            "conflicting content-length values are rejected");
    require(parser.error() == oklib::http::HttpParseError::bad_message_framing,
            "conflicting content-length reports framing error");
  }
}

void test_parses_response_head_and_body_for_client() {
  oklib::net::Buffer buffer;
  append(&buffer,
         "HTTP/1.1 200 OK\r\n"
         "Content-Length: 5\r\n"
         "Connection: keep-alive\r\n"
         "\r\n"
         "hello");

  oklib::http::HttpParser parser(oklib::http::HttpParserMode::response);
  require(parser.parse_response(&buffer) == oklib::http::HttpParseStatus::complete,
          "response completes");
  require(parser.response().version == oklib::http::HttpVersion::http11, "response version parsed");
  require(parser.response().status_code == 200, "response status parsed");
  require(parser.response().reason_phrase == "OK", "response reason parsed");
  require(parser.response().headers.get("connection") == "keep-alive", "response header parsed");
  require(parser.response().body == "hello", "response body parsed");
}

}  // namespace

int main() {
  test_headers_are_case_insensitive_and_keep_repeated_values();
  test_parses_content_length_request_and_keeps_pipeline_leftover();
  test_parses_chunked_request_with_trailers();
  test_rejects_ambiguous_message_framing();
  test_parses_response_head_and_body_for_client();
  return 0;
}
