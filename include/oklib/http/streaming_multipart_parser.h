#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "oklib/http/http_headers.h"

namespace oklib::http {

struct StreamingMultipartPart {
  HttpHeaders headers;
  std::string name;
  std::string filename;
  std::string content_type;

  [[nodiscard]] bool is_file() const noexcept { return !filename.empty(); }
};

struct StreamingMultipartParserOptions {
  std::size_t max_header_bytes{64 * 1024};
  std::size_t max_field_bytes{64 * 1024};
};

class StreamingMultipartParser {
 public:
  using PartCallback = std::function<void(const StreamingMultipartPart&)>;
  using DataCallback = std::function<void(std::string_view)>;
  using CompleteCallback = std::function<void()>;

  StreamingMultipartParser(std::string boundary,
                           PartCallback part_callback,
                           DataCallback data_callback,
                           CompleteCallback complete_callback = {},
                           StreamingMultipartParserOptions options = {});

  bool append(std::string_view data);
  bool finish();

  [[nodiscard]] bool ok() const noexcept { return error_message_.empty(); }
  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] const std::string& error_message() const noexcept { return error_message_; }

 private:
  enum class State {
    first_boundary,
    headers,
    body,
    boundary_suffix,
    done,
    error,
  };

  bool process();
  bool process_first_boundary();
  bool process_headers();
  bool process_body();
  bool process_boundary_suffix();
  bool fail(std::string message);
  void complete_once();
  void emit_data(std::string_view data);

  std::string boundary_;
  std::string marker_;
  std::string delimiter_;
  PartCallback part_callback_;
  DataCallback data_callback_;
  CompleteCallback complete_callback_;
  StreamingMultipartParserOptions options_;
  State state_{State::first_boundary};
  StreamingMultipartPart current_part_;
  std::string buffer_;
  std::size_t current_field_bytes_{0};
  bool completed_callback_called_{false};
  std::string error_message_;
};

}  // namespace oklib::http
