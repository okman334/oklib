# HTTP/1.1 Progress

## Current Phase

- Phase 3: server streaming and backpressure.
  - Completed in this phase: async buffered response callback, `HttpResponseWriter`, and chunked response streaming.
  - Remaining in this phase: streaming request body callbacks and explicit high-watermark/backpressure HTTP tests.

## Completed Phases

- Phase 0: progress tracking and engineering setup.
  - Added `docs/http11_progress.md`.
  - Commit: `b9e09eb`.
  - Push: `origin/codex/http11-compliance`.
- Baseline verification on branch `codex/http11-compliance`.
  - `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`: passed.
  - `cmake --build build --parallel`: passed.
  - `ctest --test-dir build --output-on-failure`: passed, 7/7 tests.
- Phase 1: HTTP core model and incremental parser.
  - Added `HttpHeaders` with case-insensitive lookup and repeated field preservation.
  - Extended `HttpRequest` with method token, request-target form, body, content length, and trailers.
  - Added `HttpParser` for incremental request/response parsing, fixed-length body, chunked body, trailers, pipeline leftover, and strict ambiguous framing rejection.
  - Added `oklib.http.parser` tests.
  - Commit: `5e44d7b`.
  - Push: `origin/codex/http11-compliance`.
- Phase 2: buffered `HttpServer` upgrade.
  - Routed `HttpContext` through `HttpParser`.
  - `HttpServer` now supports buffered Content-Length request bodies, chunked request bodies, trailers, Host validation, ambiguous framing rejection, and multiple pipelined requests in one read buffer.
  - Added integration coverage in `oklib.http`.
  - Commit: `eb42d8c`.
  - Push: `origin/codex/http11-compliance`.

## Incomplete Phases

- Phase 3: server streaming and backpressure.
- Phase 4: HTTP client.
- Phase 5: RFC 9110 semantic helpers.
- Phase 6: RFC 9111 cache-related helpers.

## Current Blockers

- None.

## Latest Test Result

- 2026-06-24: Debug build passed.
- 2026-06-24: `ctest --test-dir build --output-on-failure` passed, 8/8 tests.
- 2026-06-25: `cmake --build build --parallel` passed.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 8/8 tests.
- 2026-06-25: `cmake --build build --parallel` passed after chunked response streaming.
- 2026-06-25: `ctest --test-dir build --output-on-failure` passed, 8/8 tests after chunked response streaming.

## Latest Commit / Push

- Phase 0 commit pushed: `b9e09eb`.
- Phase 1 commit pushed: `5e44d7b`.
- Phase 2 commit pushed: `eb42d8c`.
- Phase 3 async response commit: `69c37d6`.
- Phase 3 chunked response streaming commit: `710a4cb`.

## Next Step

- Continue Phase 3 with failing tests for streaming request body callbacks and explicit high-watermark/backpressure behavior.
