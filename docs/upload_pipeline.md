# Production Upload Pipeline

`/upload-file-worker` demonstrates the production-oriented upload path. Network reads stay on
`EventLoop` threads, and blocking file writes run in `UploadFileWriterPool`.

## Data Flow

- HTTP streaming callbacks receive `std::string_view` chunks owned by the connection buffer.
- `UploadFileWriterSession::append()` copies each chunk once into reusable fixed-size blocks.
- Writer threads drain queued blocks to `filename.part`.
- Successful uploads rename `.part` to the final file; canceled or failed uploads remove `.part`.

The one owned copy is intentional: connection buffers are reused after the HTTP parser retrieves
bytes, so handing a view directly to another thread would be unsafe.

## Backpressure

- Each session has `high_watermark` and `low_watermark`.
- Crossing the high watermark pauses socket reads through `HttpRequestBodyStream::pause_reading()`.
- Draining below the low watermark resumes reads.
- The pool also enforces `max_active_uploads` and `max_total_queued_bytes`.

Increasing writer threads alone is not enough for production uploads. Without bounded queues and
read backpressure, fast clients can still fill memory faster than disk can flush data.

## Defaults

- `block_size`: 256 KiB
- `high_watermark`: 8 MiB per upload
- `low_watermark`: 4 MiB per upload
- `max_upload_size`: 1 GiB
- `max_active_uploads`: 128
- `max_total_queued_bytes`: 512 MiB
- `timeout`: 5 minutes, exposed in options for route-level timeout policy

## Multipart

`StreamingMultipartParser` parses file parts incrementally. The worker upload route writes the
`file` part, or the first file part, without buffering the whole multipart request body.

## Benchmark

Build `oklib_http_upload_benchmark` and run:

```bash
./build/examples/oklib_http_upload_benchmark --requests 20 --clients 4 --body-size 1048576
./build/examples/oklib_http_upload_benchmark --requests 20 --clients 4 --body-size 1048576 --multipart
```

The benchmark is a smoke benchmark for this library path. Use tools such as `wrk` or `ab` for
external comparisons.
