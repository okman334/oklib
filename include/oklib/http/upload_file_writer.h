#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "oklib/base/thread_pool.h"

namespace oklib::http {

struct UploadFileWriterOptions {
  std::filesystem::path upload_dir{"uploads"};
  std::string file_name{"upload.bin"};
  std::string content_type{"application/octet-stream"};
  std::size_t block_size{256 * 1024};
  std::size_t high_watermark{8 * 1024 * 1024};
  std::size_t low_watermark{4 * 1024 * 1024};
  std::uint64_t max_upload_size{1024ull * 1024ull * 1024ull};
  std::chrono::milliseconds timeout{std::chrono::minutes(5)};
};

struct UploadFileWriterPoolOptions {
  std::size_t max_active_uploads{128};
  std::size_t max_total_queued_bytes{512 * 1024 * 1024};
};

struct UploadFileWriterResult {
  bool ok{false};
  bool canceled{false};
  std::filesystem::path path;
  std::string file_name;
  std::string content_type;
  std::size_t bytes{0};
  std::string error_message;
};

struct UploadFileWriterStats {
  std::size_t active_uploads{0};
  std::size_t total_queued_bytes{0};
  std::uint64_t completed_uploads{0};
  std::uint64_t failed_uploads{0};
  std::uint64_t canceled_uploads{0};
};

namespace detail {
struct UploadFileWriterPoolState;
struct UploadFileWriterSessionState;
}  // namespace detail

class UploadFileWriterSession {
 public:
  bool append(std::string_view data);
  void finish();
  void cancel();

  [[nodiscard]] std::size_t queued_bytes() const;
  [[nodiscard]] std::size_t bytes_written() const;
  [[nodiscard]] std::size_t debug_blocks_allocated_for_test() const;

  void wait_for_test();

 private:
  friend class UploadFileWriterPool;

  explicit UploadFileWriterSession(std::shared_ptr<detail::UploadFileWriterSessionState> state);

  std::shared_ptr<detail::UploadFileWriterSessionState> state_;
};

class UploadFileWriterPool {
 public:
  using CompletionCallback = std::function<void(UploadFileWriterResult)>;
  using WatermarkCallback = std::function<void()>;

  explicit UploadFileWriterPool(UploadFileWriterPoolOptions options = {});
  ~UploadFileWriterPool();

  void start(int thread_count);
  void stop();

  std::shared_ptr<UploadFileWriterSession> create_session(
      UploadFileWriterOptions options,
      CompletionCallback completion = {},
      WatermarkCallback high_watermark = {},
      WatermarkCallback low_watermark = {});

  [[nodiscard]] UploadFileWriterStats stats() const;

 private:
  std::shared_ptr<detail::UploadFileWriterPoolState> state_;
  oklib::ThreadPool workers_{"upload-file-writer"};
};

}  // namespace oklib::http
