#include "oklib/http/upload_file_writer.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace oklib::http {
namespace detail {

struct UploadBlock {
  std::unique_ptr<char[]> data;
  std::size_t size{0};
  std::size_t capacity{0};
};

struct UploadFileWriterPoolState {
  explicit UploadFileWriterPoolState(UploadFileWriterPoolOptions opts)
      : options(std::move(opts)) {}

  UploadFileWriterPoolOptions options;
  std::atomic_size_t active_uploads{0};
  std::atomic_size_t total_queued_bytes{0};
  std::atomic_uint64_t completed_uploads{0};
  std::atomic_uint64_t failed_uploads{0};
  std::atomic_uint64_t canceled_uploads{0};

  std::mutex path_mutex;
  std::set<std::filesystem::path> reserved_final_paths;
  std::uint64_t next_part_id{0};

  std::mutex block_mutex;
  std::vector<UploadBlock> cached_blocks;
  std::size_t cached_block_bytes{0};
};

struct UploadFileWriterSessionState {
  UploadFileWriterOptions options;
  std::shared_ptr<UploadFileWriterPoolState> pool;
  std::filesystem::path final_path;
  std::filesystem::path part_path;
  UploadFileWriterPool::CompletionCallback completion;
  UploadFileWriterPool::WatermarkCallback high_watermark;
  UploadFileWriterPool::WatermarkCallback low_watermark;

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::deque<UploadBlock> ready_blocks;
  std::vector<UploadBlock> free_blocks;
  UploadBlock pending_block;
  bool has_pending_block{false};
  bool finished{false};
  bool canceled{false};
  bool failed{false};
  bool worker_done{false};
  bool above_high_watermark{false};
  std::size_t queued{0};
  std::size_t written{0};
  std::size_t blocks_allocated{0};
  std::string error_message;
};

}  // namespace detail
namespace {

struct ReservedUploadPaths {
  std::filesystem::path final_path;
  std::filesystem::path part_path;
  std::string file_name;
};

std::string fallback_upload_file_name(std::string file_name) {
  if (file_name.empty()) {
    return "upload.bin";
  }
  return file_name;
}

std::string file_name_with_index(const std::string& file_name, std::uint64_t index) {
  if (index == 0) {
    return file_name;
  }

  const std::filesystem::path path(file_name);
  const auto extension = path.extension().string();
  auto stem = path.stem().string();
  if (stem.empty()) {
    stem = "upload";
  }
  return stem + "-" + std::to_string(index) + extension;
}

ReservedUploadPaths reserve_upload_paths(
    const std::shared_ptr<detail::UploadFileWriterPoolState>& pool,
    const UploadFileWriterOptions& options) {
  const auto requested_file_name = fallback_upload_file_name(options.file_name);

  std::lock_guard lock(pool->path_mutex);
  for (std::uint64_t index = 0;; ++index) {
    auto file_name = file_name_with_index(requested_file_name, index);
    auto final_path = options.upload_dir / file_name;

    std::error_code ec;
    const bool exists = std::filesystem::exists(final_path, ec);
    const bool reserved = pool->reserved_final_paths.find(final_path) != pool->reserved_final_paths.end();
    if ((exists && !options.overwrite_existing) || reserved) {
      continue;
    }

    pool->reserved_final_paths.insert(final_path);
    auto part_name = "." + final_path.filename().string() + "." +
                     std::to_string(++pool->next_part_id) + ".part";
    return {std::move(final_path), options.upload_dir / std::move(part_name), std::move(file_name)};
  }
}

void release_upload_path(const std::shared_ptr<detail::UploadFileWriterSessionState>& state) {
  std::lock_guard lock(state->pool->path_mutex);
  state->pool->reserved_final_paths.erase(state->final_path);
}

bool try_increment_active(const std::shared_ptr<detail::UploadFileWriterPoolState>& pool) {
  auto current = pool->active_uploads.load(std::memory_order_acquire);
  for (;;) {
    if (current >= pool->options.max_active_uploads) {
      return false;
    }
    if (pool->active_uploads.compare_exchange_weak(current,
                                                   current + 1,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
      return true;
    }
  }
}

bool try_reserve_queued(const std::shared_ptr<detail::UploadFileWriterPoolState>& pool,
                        std::size_t bytes) {
  auto current = pool->total_queued_bytes.load(std::memory_order_acquire);
  for (;;) {
    if (bytes > pool->options.max_total_queued_bytes ||
        current > pool->options.max_total_queued_bytes - bytes) {
      return false;
    }
    if (pool->total_queued_bytes.compare_exchange_weak(current,
                                                       current + bytes,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
      return true;
    }
  }
}

void release_queued(const std::shared_ptr<detail::UploadFileWriterPoolState>& pool,
                    std::size_t bytes) {
  if (bytes == 0) {
    return;
  }
  pool->total_queued_bytes.fetch_sub(bytes, std::memory_order_acq_rel);
}

detail::UploadBlock acquire_cached_pool_block(
    const std::shared_ptr<detail::UploadFileWriterPoolState>& pool,
    std::size_t capacity) {
  std::lock_guard lock(pool->block_mutex);
  for (auto it = pool->cached_blocks.begin(); it != pool->cached_blocks.end(); ++it) {
    if (it->capacity != capacity) {
      continue;
    }
    auto block = std::move(*it);
    pool->cached_blocks.erase(it);
    pool->cached_block_bytes -= block.capacity;
    block.size = 0;
    return block;
  }
  return {};
}

void cache_pool_block(const std::shared_ptr<detail::UploadFileWriterPoolState>& pool,
                      detail::UploadBlock block) {
  if (!block.data || block.capacity == 0 ||
      block.capacity > pool->options.max_cached_block_bytes) {
    return;
  }

  block.size = 0;
  std::lock_guard lock(pool->block_mutex);
  if (pool->cached_block_bytes > pool->options.max_cached_block_bytes - block.capacity) {
    return;
  }
  pool->cached_block_bytes += block.capacity;
  pool->cached_blocks.push_back(std::move(block));
}

detail::UploadBlock acquire_block_locked(
    const std::shared_ptr<detail::UploadFileWriterSessionState>& state) {
  if (!state->free_blocks.empty()) {
    auto block = std::move(state->free_blocks.back());
    state->free_blocks.pop_back();
    block.size = 0;
    return block;
  }

  const auto capacity = std::max<std::size_t>(state->options.block_size, 1);
  auto cached = acquire_cached_pool_block(state->pool, capacity);
  if (cached.data) {
    return cached;
  }

  detail::UploadBlock block;
  block.capacity = capacity;
  block.data = std::make_unique<char[]>(block.capacity);
  ++state->blocks_allocated;
  return block;
}

void fail_locked(const std::shared_ptr<detail::UploadFileWriterSessionState>& state,
                 std::string message) {
  if (state->finished || state->canceled || state->failed) {
    return;
  }
  state->failed = true;
  state->finished = true;
  state->error_message = std::move(message);
}

void discard_ready_blocks_locked(
    const std::shared_ptr<detail::UploadFileWriterSessionState>& state) {
  const auto queued = state->queued;
  state->queued = 0;
  if (state->has_pending_block) {
    state->pending_block.size = 0;
    state->free_blocks.push_back(std::move(state->pending_block));
    state->pending_block = detail::UploadBlock();
    state->has_pending_block = false;
  }
  while (!state->ready_blocks.empty()) {
    auto block = std::move(state->ready_blocks.front());
    state->ready_blocks.pop_front();
    block.size = 0;
    state->free_blocks.push_back(std::move(block));
  }
  release_queued(state->pool, queued);
}

void flush_pending_block_locked(
    const std::shared_ptr<detail::UploadFileWriterSessionState>& state) {
  if (!state->has_pending_block || state->pending_block.size == 0) {
    return;
  }
  state->ready_blocks.push_back(std::move(state->pending_block));
  state->pending_block = detail::UploadBlock();
  state->has_pending_block = false;
}

void release_session_blocks_to_pool(
    const std::shared_ptr<detail::UploadFileWriterSessionState>& state) {
  std::vector<detail::UploadBlock> blocks;
  {
    std::lock_guard lock(state->mutex);
    if (state->has_pending_block) {
      blocks.push_back(std::move(state->pending_block));
      state->pending_block = detail::UploadBlock();
      state->has_pending_block = false;
    }
    while (!state->ready_blocks.empty()) {
      blocks.push_back(std::move(state->ready_blocks.front()));
      state->ready_blocks.pop_front();
    }
    for (auto& block : state->free_blocks) {
      blocks.push_back(std::move(block));
    }
    state->free_blocks.clear();
  }

  for (auto& block : blocks) {
    cache_pool_block(state->pool, std::move(block));
  }
}

void finish_worker_state(const std::shared_ptr<detail::UploadFileWriterSessionState>& state,
                         UploadFileWriterResult result) {
  release_session_blocks_to_pool(state);
  release_upload_path(state);
  state->pool->active_uploads.fetch_sub(1, std::memory_order_acq_rel);
  if (result.canceled) {
    state->pool->canceled_uploads.fetch_add(1, std::memory_order_acq_rel);
  } else if (result.ok) {
    state->pool->completed_uploads.fetch_add(1, std::memory_order_acq_rel);
  } else {
    state->pool->failed_uploads.fetch_add(1, std::memory_order_acq_rel);
  }

  UploadFileWriterPool::CompletionCallback completion;
  {
    std::lock_guard lock(state->mutex);
    state->worker_done = true;
    completion = std::move(state->completion);
    state->high_watermark = {};
    state->low_watermark = {};
  }
  state->cv.notify_all();

  if (completion) {
    completion(std::move(result));
  }
}

void run_upload_writer_session(std::shared_ptr<detail::UploadFileWriterSessionState> state) {
  UploadFileWriterResult result;
  result.path = state->final_path;
  result.file_name = state->options.file_name;
  result.content_type = state->options.content_type;

  std::error_code ec;
  std::filesystem::create_directories(state->options.upload_dir, ec);
  bool ok = !ec;
  std::string error = ec ? ec.message() : std::string{};

  std::ofstream file;
  if (ok) {
    file.open(state->part_path, std::ios::binary | std::ios::trunc);
    ok = file.is_open();
    if (!ok) {
      error = "failed to open upload part file";
    }
  }

  for (;;) {
    detail::UploadBlock block;
    UploadFileWriterPool::WatermarkCallback low_callback;
    bool should_stop = false;
    {
      std::unique_lock lock(state->mutex);
      state->cv.wait(lock, [&] {
        return state->finished || state->canceled || state->failed ||
               !state->ready_blocks.empty();
      });

      if (state->canceled || state->failed || !ok) {
        discard_ready_blocks_locked(state);
        should_stop = true;
      } else if (!state->ready_blocks.empty()) {
        block = std::move(state->ready_blocks.front());
        state->ready_blocks.pop_front();
        state->queued -= block.size;
        release_queued(state->pool, block.size);
        if (state->above_high_watermark && state->queued <= state->options.low_watermark) {
          state->above_high_watermark = false;
          low_callback = state->low_watermark;
        }
      } else if (state->finished) {
        should_stop = true;
      }
    }

    if (low_callback) {
      low_callback();
    }
    if (should_stop) {
      break;
    }
    if (block.size == 0) {
      continue;
    }

    if (ok) {
      file.write(block.data.get(), static_cast<std::streamsize>(block.size));
      ok = static_cast<bool>(file);
      if (!ok) {
        error = "failed to write upload file";
      }
    }

    {
      std::lock_guard lock(state->mutex);
      state->written += block.size;
      block.size = 0;
      state->free_blocks.push_back(std::move(block));
      if (!ok) {
        fail_locked(state, error);
      }
    }
    state->cv.notify_all();
  }

  if (file.is_open()) {
    file.close();
    ok = ok && static_cast<bool>(file);
    if (!ok && error.empty()) {
      error = "failed to close upload file";
    }
  }

  bool canceled = false;
  bool failed = false;
  std::size_t written = 0;
  {
    std::lock_guard lock(state->mutex);
    canceled = state->canceled;
    failed = state->failed || !ok;
    written = state->written;
    if (failed && error.empty()) {
      error = state->error_message.empty() ? "upload failed" : state->error_message;
    }
  }

  result.canceled = canceled;
  result.bytes = written;

  if (canceled || failed) {
    std::filesystem::remove(state->part_path, ec);
    result.ok = false;
    result.error_message = canceled ? "upload canceled" : error;
    finish_worker_state(state, std::move(result));
    return;
  }

  std::filesystem::rename(state->part_path, state->final_path, ec);
  if (ec) {
    std::filesystem::remove(state->part_path, ec);
    result.ok = false;
    result.error_message = ec.message();
  } else {
    result.ok = true;
  }
  finish_worker_state(state, std::move(result));
}

}  // namespace

UploadFileWriterSession::UploadFileWriterSession(
    std::shared_ptr<detail::UploadFileWriterSessionState> state)
    : state_(std::move(state)) {}

bool UploadFileWriterSession::append(std::string_view data) {
  if (!state_ || data.empty()) {
    return true;
  }

  UploadFileWriterPool::WatermarkCallback high_callback;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->finished || state_->canceled || state_->failed || state_->worker_done) {
      return false;
    }
    if (data.size() > state_->options.max_upload_size ||
        state_->written + state_->queued > state_->options.max_upload_size - data.size()) {
      fail_locked(state_, "upload size limit exceeded");
      state_->cv.notify_all();
      return false;
    }
    if (!try_reserve_queued(state_->pool, data.size())) {
      fail_locked(state_, "global upload queue limit exceeded");
      state_->cv.notify_all();
      return false;
    }

    auto remaining = data;
    while (!remaining.empty()) {
      if (!state_->has_pending_block) {
        state_->pending_block = acquire_block_locked(state_);
        state_->has_pending_block = true;
      }
      const auto writable = state_->pending_block.capacity - state_->pending_block.size;
      const auto copy_size = std::min(remaining.size(), writable);
      std::memcpy(state_->pending_block.data.get() + state_->pending_block.size,
                  remaining.data(),
                  copy_size);
      state_->pending_block.size += copy_size;
      state_->queued += copy_size;
      remaining.remove_prefix(copy_size);
      if (state_->pending_block.size == state_->pending_block.capacity) {
        flush_pending_block_locked(state_);
      }
    }

    if (!state_->above_high_watermark && state_->queued >= state_->options.high_watermark) {
      state_->above_high_watermark = true;
      high_callback = state_->high_watermark;
    }
  }

  state_->cv.notify_one();
  if (high_callback) {
    high_callback();
  }
  return true;
}

void UploadFileWriterSession::finish() {
  if (!state_) {
    return;
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->canceled || state_->failed) {
      return;
    }
    flush_pending_block_locked(state_);
    state_->finished = true;
  }
  state_->cv.notify_one();
}

void UploadFileWriterSession::cancel() {
  if (!state_) {
    return;
  }
  {
    std::lock_guard lock(state_->mutex);
    if (state_->worker_done) {
      return;
    }
    state_->canceled = true;
    state_->finished = true;
  }
  state_->cv.notify_one();
}

std::size_t UploadFileWriterSession::queued_bytes() const {
  if (!state_) {
    return 0;
  }
  std::lock_guard lock(state_->mutex);
  return state_->queued;
}

std::size_t UploadFileWriterSession::bytes_written() const {
  if (!state_) {
    return 0;
  }
  std::lock_guard lock(state_->mutex);
  return state_->written;
}

std::size_t UploadFileWriterSession::debug_blocks_allocated_for_test() const {
  if (!state_) {
    return 0;
  }
  std::lock_guard lock(state_->mutex);
  return state_->blocks_allocated;
}

void UploadFileWriterSession::wait_for_test() {
  if (!state_) {
    return;
  }
  std::unique_lock lock(state_->mutex);
  state_->cv.wait(lock, [&] { return state_->worker_done; });
}

UploadFileWriterPool::UploadFileWriterPool(UploadFileWriterPoolOptions options)
    : state_(std::make_shared<detail::UploadFileWriterPoolState>(std::move(options))) {}

UploadFileWriterPool::~UploadFileWriterPool() {
  stop();
}

void UploadFileWriterPool::start(int thread_count) {
  workers_.start(thread_count);
}

void UploadFileWriterPool::stop() {
  workers_.stop();
}

std::shared_ptr<UploadFileWriterSession> UploadFileWriterPool::create_session(
    UploadFileWriterOptions options,
    CompletionCallback completion,
    WatermarkCallback high_watermark,
    WatermarkCallback low_watermark) {
  if (!try_increment_active(state_)) {
    return nullptr;
  }

  auto reserved_paths = reserve_upload_paths(state_, options);
  options.file_name = std::move(reserved_paths.file_name);

  auto session_state = std::make_shared<detail::UploadFileWriterSessionState>();
  session_state->options = std::move(options);
  session_state->pool = state_;
  session_state->final_path = std::move(reserved_paths.final_path);
  session_state->part_path = std::move(reserved_paths.part_path);
  session_state->completion = std::move(completion);
  session_state->high_watermark = std::move(high_watermark);
  session_state->low_watermark = std::move(low_watermark);

  auto session = std::shared_ptr<UploadFileWriterSession>(
      new UploadFileWriterSession(session_state));
  workers_.run([session_state] { run_upload_writer_session(session_state); });
  return session;
}

UploadFileWriterStats UploadFileWriterPool::stats() const {
  UploadFileWriterStats snapshot;
  snapshot.active_uploads = state_->active_uploads.load(std::memory_order_acquire);
  snapshot.total_queued_bytes = state_->total_queued_bytes.load(std::memory_order_acquire);
  snapshot.completed_uploads = state_->completed_uploads.load(std::memory_order_acquire);
  snapshot.failed_uploads = state_->failed_uploads.load(std::memory_order_acquire);
  snapshot.canceled_uploads = state_->canceled_uploads.load(std::memory_order_acquire);
  return snapshot;
}

}  // namespace oklib::http
