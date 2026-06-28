#include <oklib/http/streaming_multipart_parser.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "require failed: " << message << '\n';
    std::exit(1);
  }
}

void test_streaming_parser_emits_file_chunks_across_input_boundaries() {
  std::vector<std::string> chunks;
  std::string filename;
  std::string content_type;
  bool completed = false;

  oklib::http::StreamingMultipartParser parser(
      "----oklib",
      [&](const oklib::http::StreamingMultipartPart& part) {
        filename = part.filename;
        content_type = part.content_type;
      },
      [&](std::string_view data) {
        chunks.emplace_back(data);
      },
      [&] { completed = true; });

  const std::string body =
      "------oklib\r\n"
      "Content-Disposition: form-data; name=\"title\"\r\n"
      "\r\n"
      "demo\r\n"
      "------oklib\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"demo.txt\"\r\n"
      "Content-Type: text/plain\r\n"
      "\r\n"
      "hello streaming multipart"
      "\r\n------oklib--\r\n";

  for (std::size_t i = 0; i < body.size(); i += 7) {
    require(parser.append(std::string_view(body).substr(i, 7)), "chunk append accepted");
  }
  require(parser.finish(), "parser finishes");
  require(completed, "parser completion callback fired");
  require(filename == "demo.txt", "filename parsed");
  require(content_type == "text/plain", "content type parsed");

  std::string joined;
  for (const auto& chunk : chunks) {
    joined += chunk;
  }
  require(joined == "hello streaming multipart", "file body chunks emitted");
}

void test_streaming_parser_decodes_filename_star() {
  std::string filename;
  std::string body_bytes;

  oklib::http::StreamingMultipartParser parser(
      "----oklib",
      [&](const oklib::http::StreamingMultipartPart& part) {
        filename = part.filename;
      },
      [&](std::string_view data) {
        body_bytes.append(data);
      });

  const std::string body =
      "------oklib\r\n"
      "Content-Disposition: form-data; name=\"file\"; "
      "filename*=UTF-8''%E6%A2%A8%E5%AD%90.mp4\r\n"
      "Content-Type: video/mp4\r\n"
      "\r\n"
      "mp4-bytes"
      "\r\n------oklib--\r\n";

  require(parser.append(body.substr(0, 13)), "first split append accepted");
  require(parser.append(body.substr(13)), "second split append accepted");
  require(parser.finish(), "parser finishes");
  require(filename == "梨子.mp4", "filename* decoded");
  require(body_bytes == "mp4-bytes", "body emitted");
}

}  // namespace

int main() {
  test_streaming_parser_emits_file_chunks_across_input_boundaries();
  test_streaming_parser_decodes_filename_star();
  return 0;
}
