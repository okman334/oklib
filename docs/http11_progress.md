# HTTP/1.1 Progress

## Current Phase

- Phase 1: HTTP core model and incremental parser.

## Completed Phases

- Phase 0: progress tracking and engineering setup.
  - Added `docs/http11_progress.md`.
  - Commit: `8c11053`.
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

## Incomplete Phases

- Phase 2: buffered `HttpServer` upgrade.
- Phase 3: server streaming and backpressure.
- Phase 4: HTTP client.
- Phase 5: RFC 9110 semantic helpers.
- Phase 6: RFC 9111 cache-related helpers.

## Current Blockers

- None.

## Latest Test Result

- 2026-06-24: Debug build passed.
- 2026-06-24: `ctest --test-dir build --output-on-failure` passed, 8/8 tests.

## Latest Commit / Push

- Phase 0 commit pushed: `8c11053`.
- Pending Phase 1 commit and push.

## Next Step

- Start Phase 2 by writing failing server integration tests for body parsing, chunked requests, Host validation, and HTTP/1.1 pipeline handling.
