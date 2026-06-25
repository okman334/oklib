#include <oklib/http/http_request.h>
#include <oklib/http/http_response.h>
#include <oklib/http/http_semantics.h>

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

void test_standard_methods_and_statuses() {
  require(oklib::http::standard_method_token(oklib::http::HttpMethod::get) == "GET",
          "GET method token exposed");
  require(oklib::http::standard_method_token(oklib::http::HttpMethod::delete_) == "DELETE",
          "DELETE method token exposed");
  require(oklib::http::standard_method_from_token("PATCH") == oklib::http::HttpMethod::patch,
          "PATCH method parsed");
  require(!oklib::http::standard_method_from_token("PROPFIND").has_value(),
          "unknown method has no standard enum");
  require(oklib::http::is_safe_method(oklib::http::HttpMethod::get), "GET is safe");
  require(oklib::http::is_idempotent_method(oklib::http::HttpMethod::put), "PUT is idempotent");
  require(!oklib::http::is_safe_method(oklib::http::HttpMethod::post), "POST is not safe");

  require(oklib::http::standard_reason_phrase(200) == "OK", "200 reason phrase exposed");
  require(oklib::http::standard_reason_phrase(206) == "Partial Content", "206 reason phrase exposed");
  require(oklib::http::standard_reason_phrase(418).empty(), "unknown status reason empty");
  require(oklib::http::is_standard_status_code(404), "404 is standard");
  require(!oklib::http::is_standard_status_code(599), "599 is not in standard table");
  require(!oklib::http::status_code_allows_body(204), "204 has no body");
  require(!oklib::http::status_code_allows_body(304), "304 has no body");
  require(oklib::http::status_code_allows_body(206), "206 may have body");

  oklib::http::HttpResponse response;
  response.set_status_code(207);
  response.set_status_message("Multi-Status");
}

void test_range_helpers() {
  const auto range = oklib::http::parse_range_header("bytes=0-99, 200-, -50");
  require(range.has_value(), "range header parses");
  require(range->unit == "bytes", "range unit parsed");
  require(range->ranges.size() == 3, "range count parsed");
  require(range->ranges[0].first == 0 && range->ranges[0].last == 99,
          "closed byte range parsed");
  require(range->ranges[1].first == 200 && !range->ranges[1].last.has_value(),
          "open-ended byte range parsed");
  require(range->ranges[2].suffix_length == 50, "suffix byte range parsed");
  require(!oklib::http::parse_range_header("bytes=100-99").has_value(),
          "invalid descending range rejected");

  const auto content_range = oklib::http::parse_content_range_header("bytes 0-99/200");
  require(content_range.has_value(), "content-range parses");
  require(content_range->unit == "bytes", "content-range unit parsed");
  require(!content_range->unsatisfied, "satisfied content-range parsed");
  require(content_range->first == 0 && content_range->last == 99,
          "content-range bounds parsed");
  require(content_range->complete_length == 200, "content-range complete length parsed");

  const auto unsatisfied = oklib::http::parse_content_range_header("bytes */200");
  require(unsatisfied.has_value(), "unsatisfied content-range parses");
  require(unsatisfied->unsatisfied, "unsatisfied content-range flag parsed");
  require(unsatisfied->complete_length == 200, "unsatisfied content-range length parsed");

  require(!oklib::http::parse_content_range_header("bytes 0-99/abc").has_value(),
          "invalid content-range length rejected");
}

void test_entity_tag_and_date_helpers() {
  const auto strong = oklib::http::parse_entity_tag("\"abc\"");
  const auto weak = oklib::http::parse_entity_tag("W/\"abc\"");
  require(strong.has_value() && !strong->weak && strong->tag == "abc", "strong etag parses");
  require(weak.has_value() && weak->weak && weak->tag == "abc", "weak etag parses");
  require(oklib::http::strong_entity_tag_equal(*strong, *strong), "strong etag equals itself");
  require(!oklib::http::strong_entity_tag_equal(*strong, *weak), "strong compare rejects weak etag");
  require(oklib::http::weak_entity_tag_equal(*strong, *weak), "weak compare matches same opaque tag");
  require(oklib::http::if_none_match_matches("W/\"abc\", \"def\"", *strong),
          "If-None-Match uses weak comparison");
  require(oklib::http::if_match_matches("\"abc\"", *strong),
          "If-Match uses strong comparison");
  require(!oklib::http::if_match_matches("W/\"abc\"", *strong),
          "If-Match rejects weak tag");

  const auto date = oklib::http::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT");
  require(date.has_value(), "IMF-fixdate parses");
  require(oklib::http::format_http_date(*date) == "Sun, 06 Nov 1994 08:49:37 GMT",
          "HTTP date formats as IMF-fixdate");
  require(oklib::http::if_modified_since_not_modified("Sun, 06 Nov 1994 08:49:37 GMT", *date),
          "If-Modified-Since detects not modified");
  require(oklib::http::if_unmodified_since_allows("Sun, 06 Nov 1994 08:49:37 GMT", *date),
          "If-Unmodified-Since allows equal last modified");
}

}  // namespace

int main() {
  test_standard_methods_and_statuses();
  test_range_helpers();
  test_entity_tag_and_date_helpers();
  return 0;
}
