# Distributed C++ Build System

A gRPC coordinator schedules compilation jobs from a dependency DAG across worker
processes, with heartbeat-driven failure recovery and a content-addressed cache.
Two-week interview-prep build. Full day-by-day plan: see `docs/14-day-plan.md`.

## Scope — do not re-litigate mid-build

**Built for real:** coordinator/worker gRPC core, DAG scheduler, in-process failure
recovery (heartbeats, leases, retries), content-addressed cache on local disk.

**Deferred, interface-ready — do not implement unless explicitly asked:**
Redis-backed job queue, Kafka event stream, MinIO/S3 storage, Prometheus/Grafana,
HA coordinator. These are placeholders behind interfaces on purpose. Do not reach
for them "since it's easy" — that was a deliberate two-week scope cut, not an
oversight.

## Stack

- Build: CMake + vcpkg
- RPC: gRPC + Protocol Buffers, sync stubs unless told otherwise
- Job queue: in-process `WorkQueue` interface — `std::queue` + `mutex` +
  `condition_variable`. Redis is a future second implementation of this same
  interface, not a reason to change the interface now.
- Cache: local content-addressed disk, behind an interface (swap-in point for
  MinIO later)
- Correctness gate: ThreadSanitizer + AddressSanitizer clean before a phase is
  considered done

## Build & test

```bash
cmake --preset default && cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --preset tsan && cmake --build build-tsan -j && ./build-tsan/run_tests
cmake --preset asan && cmake --build build-asan -j && ./build-asan/run_tests
```

(Fill in real preset/target names once they exist — keep this block accurate,
it's the first thing a new session should be able to trust.)

## Conventions for this project

- Scope every change by interface first (e.g. "implement `WorkQueue` with this
  signature") rather than by component name. Small, reviewable diffs.
- Any code touching state shared across gRPC handler threads (worker table,
  ready queue, job table) needs an explicit note on what protects it.
- Do not hand-write the DAG, scheduler, or lease/retry tests for me — I'm
  writing those myself by hand. You may scaffold the test file structure, not
  the assertions.
- Redirect sanitizer/build output to a log file and summarize the relevant
  lines rather than pasting full raw output back into the conversation.
- Real C++ TUs are siblings — only generated-header dependencies (protoc
  output, etc.) create genuine compile-time DAG edges. `main.cpp` does not
  depend on `parser.o`; only the link does.
- Cache key must be header-aware (via `-MD` output or a manifest) plus
  compiler binary content hash, canonicalized flags, and target triple — never
  `hash(source + compiler_version + flags)`.

## Compact Instructions

On `/compact`, preserve:
- Which day/phase of `docs/14-day-plan.md` is in progress and what's left in it
- Any unresolved TSan/ASan finding and the current hypothesis for it
- Files changed but not yet committed, and why
- Any deliberate deviation from this file's stated scope/stack, and the reason

Drop raw compiler/sanitizer output — keep only the one-line conclusion.

## Current state

<!-- Update this before /clear or /compact. Overwrite, don't append. -->
- Last completed: Day 1 — proto definitions, CMake+vcpkg skeleton, coordinator/worker gRPC
  ping round-trip verified end to end (process logs show both sides). vcpkg vendored as a
  pinned git submodule; CMakePresets.json has default/tsan/asan configure+build presets.
- In progress: —
- Next step: Day 2 — worker's fork/exec compile pipeline for one hardcoded .cpp file
- Known issues: proto package collision bug hit and fixed — both .proto files originally
  shared `package distbuild;`, so both generated C++ class `distbuild::PingRequest`, which
  collided at compile time the moment one file (coordinator/main.cpp) included both generated
  headers. Fixed by giving each service its own package (`distbuild.buildsvc` /
  `distbuild.workersvc`). Not yet committed to git — build artifacts and submodule are in
  place but nothing is committed.