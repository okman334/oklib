#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace oklib::net {

class Buffer {
 public:
  static constexpr std::size_t k_cheap_prepend = 8;
  static constexpr std::size_t k_initial_size = 1024;

  explicit Buffer(std::size_t initial_size = k_initial_size);

  [[nodiscard]] std::size_t readable_bytes() const noexcept { return writer_index_ - reader_index_; }
  [[nodiscard]] std::size_t writable_bytes() const noexcept { return buffer_.size() - writer_index_; }
  [[nodiscard]] std::size_t prependable_bytes() const noexcept { return reader_index_; }

  [[nodiscard]] const char* peek() const noexcept { return begin() + reader_index_; }
  [[nodiscard]] char* begin_write() noexcept { return begin() + writer_index_; }
  [[nodiscard]] const char* begin_write() const noexcept { return begin() + writer_index_; }

  [[nodiscard]] const char* find_crlf() const;
  [[nodiscard]] const char* find_crlf(const char* start) const;

  void retrieve(std::size_t len);
  void retrieve_until(const char* end);
  void retrieve_all() noexcept;
  [[nodiscard]] std::string retrieve_as_string(std::size_t len);
  [[nodiscard]] std::string retrieve_all_as_string();

  void append(std::string_view data);
  void append(const void* data, std::size_t len);
  void ensure_writable_bytes(std::size_t len);
  void has_written(std::size_t len) noexcept { writer_index_ += len; }

  ssize_t read_fd(int fd, int* saved_errno);

 private:
  [[nodiscard]] char* begin() noexcept { return buffer_.data(); }
  [[nodiscard]] const char* begin() const noexcept { return buffer_.data(); }
  void make_space(std::size_t len);

  std::vector<char> buffer_;
  std::size_t reader_index_;
  std::size_t writer_index_;
};

}  // namespace oklib::net
