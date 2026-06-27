#include <oklib/http/multipart_parser.h>

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

void test_extracts_boundary_from_content_type() {
  auto boundary = oklib::http::multipart_boundary(
      "multipart/form-data; charset=utf-8; boundary=\"----oklib-boundary\"");
  require(boundary.has_value(), "quoted boundary is found");
  require(*boundary == "----oklib-boundary", "quoted boundary is unescaped");

  boundary = oklib::http::multipart_boundary("text/plain; boundary=ignored");
  require(!boundary.has_value(), "non multipart content type is rejected");
}

void test_parses_field_and_file_part() {
  const std::string mp4_body = std::string("\0\0\0\x18", 4) + "ftypmp42demo";
  const std::string body =
      "------oklib\r\n"
      "Content-Disposition: form-data; name=\"user_id\"\r\n"
      "\r\n"
      "42\r\n"
      "------oklib\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"demo.mp4\"\r\n"
      "Content-Type: video/mp4\r\n"
      "\r\n" +
      mp4_body +
      "\r\n"
      "------oklib--\r\n";

  const auto result = oklib::http::parse_multipart_form_data(
      "multipart/form-data; boundary=----oklib", body);

  require(result.ok(), "multipart body parses");
  require(result.parts.size() == 2, "two parts parsed");

  const auto* user_id = result.find("user_id");
  require(user_id != nullptr, "field part is found by name");
  require(!user_id->is_file(), "field part is not a file");
  require(user_id->body == "42", "field body parsed");

  const auto* file = result.find("file");
  require(file != nullptr, "file part is found by name");
  require(file->is_file(), "file part reports file");
  require(file->filename == "demo.mp4", "filename parsed");
  require(file->content_type == "video/mp4", "part content type parsed");
  require(file->body == mp4_body, "binary file body is preserved");
}

void test_decodes_percent_encoded_multipart_filename() {
  const std::string encoded_name =
      "%E6%A2%A8%E5%AD%90%E8%8A%A5%E8%8F%9C%20-%20"
      "%E6%83%85%E7%BD%91%28%E7%83%9F%E5%97%93%E7%89%88%29.mp4";
  const std::string body =
      "------oklib\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"" + encoded_name + "\"\r\n"
      "Content-Type: video/mp4\r\n"
      "\r\n"
      "video\r\n"
      "------oklib--\r\n";

  const auto result = oklib::http::parse_multipart_form_data(
      "multipart/form-data; boundary=----oklib", body);

  require(result.ok(), "multipart body with percent encoded filename parses");
  const auto* file = result.find("file");
  require(file != nullptr, "file part is found");
  require(file->filename == "梨子芥菜 - 情网(烟嗓版).mp4",
          "multipart filename percent encoding is decoded");
}

void test_prefers_rfc5987_multipart_filename_star() {
  const std::string body =
      "------oklib\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"fallback.mp4\"; "
      "filename*=UTF-8''%E6%A2%A8%E5%AD%90.mp4\r\n"
      "Content-Type: video/mp4\r\n"
      "\r\n"
      "video\r\n"
      "------oklib--\r\n";

  const auto result = oklib::http::parse_multipart_form_data(
      "multipart/form-data; boundary=----oklib", body);

  require(result.ok(), "multipart body with filename star parses");
  const auto* file = result.find("file");
  require(file != nullptr, "filename star file part is found");
  require(file->filename == "梨子.mp4", "filename star utf8 value is decoded and preferred");
}

void test_rejects_missing_boundary() {
  const auto result = oklib::http::parse_multipart_form_data(
      "multipart/form-data", "--missing\r\n");
  require(!result.ok(), "missing boundary fails");
  require(result.error == oklib::http::MultipartParseError::missing_boundary,
          "missing boundary error reported");
}

}  // namespace

int main() {
  test_extracts_boundary_from_content_type();
  test_parses_field_and_file_part();
  test_decodes_percent_encoded_multipart_filename();
  test_prefers_rfc5987_multipart_filename_star();
  test_rejects_missing_boundary();
  return 0;
}
