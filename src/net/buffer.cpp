#include "oklib/net/buffer.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/uio.h>
#include <unistd.h>

namespace oklib::net {

Buffer::Buffer(std::size_t initial_size)
    : buffer_(k_cheap_prepend + initial_size),
      reader_index_(k_cheap_prepend),
      writer_index_(k_cheap_prepend) {}

const char* Buffer::find_crlf() const {
  return find_crlf(peek());
}

const char* Buffer::find_crlf(const char* start) const {
  static constexpr char k_crlf[] = "\r\n";
  const char* crlf = std::search(start, begin_write(), k_crlf, k_crlf + 2);
  return crlf == begin_write() ? nullptr : crlf;
}

void Buffer::retrieve(std::size_t len) {
  if (len < readable_bytes()) {
    reader_index_ += len;
  } else {
    retrieve_all();
  }
}

void Buffer::retrieve_until(const char* end) {
  retrieve(static_cast<std::size_t>(end - peek()));
}

void Buffer::retrieve_all() noexcept {
  reader_index_ = k_cheap_prepend;
  writer_index_ = k_cheap_prepend;
}

std::string Buffer::retrieve_as_string(std::size_t len) {
  len = std::min(len, readable_bytes());
  std::string result(peek(), len);
  retrieve(len);
  return result;
}

std::string Buffer::retrieve_all_as_string() {
  return retrieve_as_string(readable_bytes());
}

void Buffer::append(std::string_view data) {
  append(data.data(), data.size());
}

void Buffer::append(const void* data, std::size_t len) {
  ensure_writable_bytes(len);
  const auto* bytes = static_cast<const char*>(data);
  std::copy(bytes, bytes + len, begin_write());
  has_written(len);
}

void Buffer::ensure_writable_bytes(std::size_t len) {
  if (writable_bytes() < len) {
    make_space(len);
  }
}

ssize_t Buffer::read_fd(int fd, int* saved_errno) {
  char extra[65536];
  iovec vec[2];
  const auto writable = writable_bytes();
  vec[0].iov_base = begin_write();
  vec[0].iov_len = writable;
  vec[1].iov_base = extra;
  vec[1].iov_len = sizeof(extra);
  const int iovcnt = writable < sizeof(extra) ? 2 : 1;
  const auto n = ::readv(fd, vec, iovcnt);
  if (n < 0) {
    *saved_errno = errno;
  } else if (static_cast<std::size_t>(n) <= writable) {
    writer_index_ += static_cast<std::size_t>(n);
  } else {
    writer_index_ = buffer_.size();
    append(extra, static_cast<std::size_t>(n) - writable);
  }
  return n;
}

void Buffer::make_space(std::size_t len) {
  if (writable_bytes() + prependable_bytes() < len + k_cheap_prepend) {
    buffer_.resize(writer_index_ + len);
    return;
  }

  const auto readable = readable_bytes();
  std::copy(begin() + reader_index_, begin() + writer_index_, begin() + k_cheap_prepend);
  reader_index_ = k_cheap_prepend;
  writer_index_ = reader_index_ + readable;
}

}  // namespace oklib::net
