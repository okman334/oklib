#include <oklib/http/content_type.h>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

void test_parse_content_type_header_value() {
  using oklib::http::ContentType;

  require(oklib::http::content_type_from_string("") == ContentType::none,
          "empty content type is none");
  require(oklib::http::content_type_from_string("Text/HTML; charset=utf-8") ==
              ContentType::text_html,
          "content type parsing ignores parameters and case");
  require(oklib::http::content_type_from_string("multipart/form-data; boundary=abc") ==
              ContentType::multipart_form_data,
          "multipart form data is recognized with boundary parameter");
  require(oklib::http::content_type_from_string("application/x-custom") ==
              ContentType::undefined,
          "unknown content type is undefined");
}

void test_content_type_strings_and_suffixes() {
  using oklib::http::ContentType;

  require(oklib::http::content_type_to_string(ContentType::application_json) ==
              "application/json",
          "content type string is returned");
  require(oklib::http::content_type_suffix(ContentType::video_mp4) == "mp4",
          "content type suffix is returned");
  require(oklib::http::content_type_from_suffix(".MP4") == ContentType::video_mp4,
          "suffix parser ignores leading dot and case");
  require(oklib::http::content_type_from_path("/assets/app.js?v=1") ==
              ContentType::application_javascript,
          "path parser ignores query string");
}

void test_known_content_type_table_is_exposed() {
  const auto types = oklib::http::known_content_types();
  require(!types.empty(), "known content type table is available");

  bool has_webp = false;
  for (const auto& entry : types) {
    if (entry.type == oklib::http::ContentType::image_webp &&
        entry.mime == "image/webp" &&
        entry.suffix == "webp") {
      has_webp = true;
      break;
    }
  }
  require(has_webp, "known content type table contains webp");
}

}  // namespace

int main() {
  test_parse_content_type_header_value();
  test_content_type_strings_and_suffixes();
  test_known_content_type_table_is_exposed();
  return 0;
}
