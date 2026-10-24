# Distributed C++ Build System — 14-Day Plan

Build plan, per-day reading, and checklists.

## Overview

Distributed C++ build system: a gRPC coordinator schedules compilation jobs from a dependency DAG across worker processes, with heartbeat-driven failure recovery and a content-addressed compilation cache.

**Built for real:** coordinator/worker gRPC core, DAG scheduler, in-process failure recovery (heartbeats, leases, retries), content-addressed cache on local disk.

**Design-only, interface-ready:** Redis-backed job queue, Kafka event stream, MinIO/S3 storage, Prometheus/Grafana, HA coordinator. State why in the README — correctness and concurrency depth beat five infra pieces integrated shallowly under a two-week clock.

| Concern | Choice |
| --- | --- |
| Build system | CMake + vcpkg |
| RPC | gRPC + Protocol Buffers, sync stubs first |
| Job queue | In-process `WorkQueue` interface, `std::queue` + `mutex` + `condition_variable` |
| Cache storage | Local content-addressed disk, behind an interface (swap-in point for MinIO later) |
| Correctness | ThreadSanitizer + AddressSanitizer as a merge gate every phase |
| Coding method | Agentic AI coding tool for implementation, with review + explain-back + rebuild-from-memory checkpoints |

**Rule for every AI-assisted session:** one component per session, scoped by interface; review every diff touching state shared across gRPC handler threads; write the rationale into `DECISIONS.md` in your own words after each component; forbid scope-add (no Redis/Kafka "since it's easy"); write the scheduler/DAG tests yourself by hand.

### Example run

A build of 40 TUs, 3 workers, warm cache: 6 TUs hit the cache instantly (no compile), the remaining 34 fan out roughly 11–12 per worker, and the DAG holds the single linker job back until all 40 objects exist. Wall-clock time tracks the slowest worker's queue, not the sum of all compiles — that's the entire point of distributing the work.

## Day 1 — Proto definitions, build skeleton, first gRPC ping

**Build:** `.proto` definitions for `BuildService` (CLI → coordinator) and `WorkerService` (coordinator → worker); CMake + vcpkg project structure; coordinator and worker binaries that exchange a trivial gRPC ping.

**Why:** everything else sits on this RPC boundary. Getting the build system and dependency wiring (vcpkg pulling gRPC/Protobuf) working today, separately from any real logic, means Day 2's fork/exec work isn't fighting CMake errors at the same time as fork/exec bugs.

**Read/learn first:**
- gRPC C++ quickstart — service definition, generated stub/skeleton, sync server
- Protocol Buffers language guide (message/service syntax) if new to `.proto` files
- CMake's `FetchContent` vs. vcpkg toolchain-file basics — just enough to link a vcpkg-installed package

**Checklist:**
- [x] `BuildService` and `WorkerService` `.proto` files written and compiling via protoc/CMake
- [x] Coordinator binary builds and starts a gRPC server (`BuildService`, for the future CLI)
- [x] Worker binary builds and starts a gRPC server (`WorkerService`); coordinator is the client that calls it — see note below
- [x] Ping RPC round-trips successfully (coordinator -> worker, confirmed via process logs)
- [ ] Can explain the service boundary (what crosses the RPC, what stays local) without notes

Note: built worker-as-server/coordinator-as-client, not the literal "worker connects as a client"
wording above — matches this doc's own architecture description (coordinator calls
`WorkerService` on a chosen worker) and keeps workers purely reactive, which the lease/retry
design in Days 5-6 depends on.

### Why gRPC specifically

A build coordinator and its workers are separate processes that may run on separate machines — they need a wire protocol, not a function call. gRPC gives strongly-typed messages (via `.proto`), streaming (useful later for large artifact transfer), and generated client/server code, instead of hand-rolling a socket protocol.

### Example

`BuildService.SubmitBuild(BuildRequest{files: ["parser.cpp"]})` from the CLI returns a `BuildId`; the coordinator then calls `WorkerService.Compile(CompileRequest{build_id, file: "parser.cpp", flags: [...]})` on a chosen worker. Two separate services because the CLI never talks to workers directly — only ever to the coordinator.

## Day 2 — Worker's real compile pipeline (fork/exec)

**Build:** worker receives a `CompileRequest` over gRPC for one hardcoded `.cpp` file, forks, execs `clang++ -c`, waits on the child, and returns the resulting `.o` path. CLI sends one hardcoded request through the coordinator to the worker end to end.

**Why:** this is the actual systems-programming core of a "worker" — everything after this (multiple workers, scheduling, failure recovery) is really about routing more of these compile calls around correctly. Getting this pipeline solid, and being able to explain every syscall in it, is non-negotiable interview material regardless of how the rest of the system evolves.

**Read/learn first:**
- `fork(2)`, `execve(2)`, `waitpid(2)` man pages — what each call does, what's shared vs. copied across `fork`
- How a shell captures a child's exit status and stdout/stderr, since the worker needs to do the same for compiler errors
- A skim of how `posix_spawn` differs from `fork`+`exec`, so you can explain why you picked one over the other

**Checklist:**
- [ ] Worker forks, execs `clang++ -c`, and correctly waits for and reports exit status
- [ ] Compiler stdout/stderr captured and returned to the coordinator on failure
- [ ] One `.cpp` file compiles end-to-end: CLI → coordinator → worker → `.o` on disk
- [ ] Can explain fork/exec/waitpid out loud, unprompted, including what happens on a compile error

### Why fork/exec instead of a library call

The compiler is a separate process you don't control the internals of — you need process isolation (a crashing `clang++` shouldn't crash your worker) and you need its exit code and stdout/stderr, which only process-level APIs give cleanly.

### Example

Worker receives `{file: "parser.cpp", flags: ["-O2", "-std=c++20"]}`, forks, the child execs `clang++ -O2 -std=c++20 -c parser.cpp -o parser.o`, the parent waits and reads the exit status: 0 → return the `.o` path; non-zero → return the captured stderr as the compile error.

## Days 3–4 — Real DAG, multiple workers, in-process queue

**Build:** a toy project with a genuine compile-time dependency — a `.proto` file that `protoc` generates a header from, which other translation units `#include`. Coordinator builds the DAG from this, topologically schedules the codegen step before its dependents, and fans independent TUs out across 2–3 worker processes via an in-process `WorkQueue` (`std::queue` + `mutex` + `condition_variable`).

**Why:** plain C++ TUs are siblings — `main.cpp` doesn't depend on `parser.o`, only the *link* does. Real TU-to-TU compile ordering only exists through generated headers (or precompiled headers/modules), so the DAG has to be built around that case to mean anything. This is also where the scheduler's shared state (ready queue, worker table) is touched from multiple gRPC handler threads at once — the concurrency correctness work starts here, not later.

**Read/learn first:**
- Topological sort / Kahn's algorithm — you'll implement this from memory again on Day 13, so understand it now, not just copy it
- `std::condition_variable` producer/consumer queue pattern — the classic bounded/unbounded queue with a mutex + condvar, the shape of your `WorkQueue`
- How `protoc` generates `.pb.h`/`.pb.cc` and how a build system normally expresses "run codegen before compiling this" (skim how CMake or GN handles generated-source dependencies)

**Checklist:**
- [ ] DAG correctly models the codegen → dependent-TU edge, not a compile→link edge
- [ ] Independent TUs are scheduled in parallel across multiple worker processes
- [ ] `WorkQueue` interface defined; in-process implementation passes basic concurrent-access tests
- [ ] Can walk through the topological scheduling logic out loud without the code open

### Why a DAG instead of a flat job list

Most jobs are independent, but a few genuinely aren't — you cannot compile a file that includes a header that hasn't been generated yet. A flat list can't express "run A before B"; a DAG can, and it's what lets the scheduler safely maximize parallelism, since anything with no unmet dependency is immediately safe to run.

### Example

`schema.proto` → `protoc` generates `schema.pb.h`/`schema.pb.cc` → `server.cpp` (which `#include`s `schema.pb.h`) can only be scheduled once that codegen job reports done. Meanwhile `utils.cpp`, which includes nothing generated, is ready immediately and runs on another worker in parallel with the codegen step.

## Days 5–6 — Heartbeats, failure recovery, worker-kill demo

**Build:** workers heartbeat to the coordinator; coordinator tracks lease expiry per assigned job and requeues on timeout; artifact publication is idempotent so a late-completing original and a requeued retry can't both land and corrupt the build. Script the `docker kill worker-2` (or `kill -9`) mid-build demo and run it until it's reliable.

**Why:** this is the failure-recovery story an interviewer will push on hardest — "what if two heartbeat timeouts race with a job completion?" The idempotency requirement exists specifically because a worker can go silent for reasons other than actually failing (GC pause, network blip), so "requeue on timeout" alone isn't safe without it.

**Read/learn first:**
- Lease/lock-with-expiry pattern (how a distributed lock service handles a client that stops renewing) — the concept, not a specific implementation
- Idempotency in distributed job execution — why "at-least-once delivery + idempotent handler" is the standard fix, versus trying to guarantee exactly-once delivery
- `std::chrono::steady_clock` vs. `system_clock` for timeout/lease logic — steady_clock is what you want, and you should know why

**Checklist:**
- [ ] Workers send periodic heartbeats; coordinator detects a missing one within a bounded time
- [ ] Job lease expires and the job is requeued to a different worker
- [ ] Duplicate completion (original finishes late after requeue) does not corrupt the artifact store
- [ ] `docker kill worker-2` mid-build demo succeeds reliably across 10+ runs
- [ ] Can explain the specific race the idempotency check guards against

### Why heartbeats instead of just waiting for a result

Without heartbeats, a hung or crashed worker looks identical to one slowly compiling a big file — the coordinator has no way to tell "still working" from "dead," so the whole build could stall forever waiting on nothing.

### Example

Worker 2 is assigned `big_template_file.cpp` and heartbeats every 2s. At t=10s its container is killed. The coordinator notices no heartbeat since t=8s, lets the lease expire at t=12s, and reassigns the job to Worker 1. If Worker 2 actually finishes and reports success at t=13s (it wasn't dead, just slow to report), the idempotent publish step recognizes the artifact for that job already exists and discards the late duplicate instead of double-writing it.

## Day 7 — Correctness day + memory-rebuild exercise 1

**Build:** run ThreadSanitizer and AddressSanitizer builds across everything so far; fix every finding. Hand-write unit tests for the DAG (cycle detection, topological order) and the lease/retry state machine — written by you, not the AI tool, since writing the test for a race condition is what builds the understanding a passing test alone doesn't.

**Why:** you didn't personally reason through every lock ordering and memory-order decision if an AI tool wrote parts of this — TSan/ASan are how you find out whether it's actually correct rather than just looking correct. This is also the natural checkpoint to prove you can still reconstruct the core logic.

**Read/learn first:**
- What ThreadSanitizer actually detects (data races) vs. what it doesn't (logic bugs, deadlocks it didn't hit this run) — so you interpret a clean run correctly, not overconfidently
- Basics of `std::memory_order` if TSan flags anything atomic-related — you don't need deep expertise, but you need to explain a fix if asked

**Exercise 1:** close the coding tool. From a blank file, write `jobCompleted` and `workerLost` handlers from memory. Diff against the real implementation. Note anything you got wrong or forgot, and why.

**Checklist:**
- [ ] TSan build run and clean (or all findings understood and fixed)
- [ ] ASan build run and clean
- [ ] Hand-written unit tests for DAG cycle detection and topological order
- [ ] Hand-written unit tests for the lease/retry state machine
- [ ] Exercise 1 completed and diffed against the real code

### Why sanitizers instead of just "it ran fine in testing"

A data race often doesn't crash — it corrupts state silently, or only shows up once in a thousand runs under one specific thread interleaving. Passing manual testing proves nothing about races; TSan instruments memory accesses to catch them even when they don't happen to cause a visible failure that particular run.

### Example

Two gRPC handler threads both touch `workerTable[id].lastSeen = now()` and `workerTable.erase(id)` without a lock protecting the map. This can pass every manual test — the timing has to line up exactly wrong — and still be a real bug. TSan flags the unsynchronized access the first run that actually exercises both paths, even if nothing visibly breaks that run.

## Days 8–9 — Content-addressed compilation cache

**Build:** local, content-addressed cache. Cache key covers header-aware hashing (via compiler `-MD` dependency output, or a manifest of included-header hashes), the compiler binary's content hash (not a version string), canonicalized compiler flags, and the target triple. Measure and record first-build time, second-build time, and cache hit rate on a real run.

**Why:** a cache keyed on `hash(source + compiler_version + flags)` gives a silent stale hit the moment a header changes — the build succeeds with wrong output and no error. This is one of the two "caught my own mistake" stories worth telling in an interview, so build it the corrected way from the start and be ready to explain why the naive version is wrong.

**Read/learn first:**
- ccache's direct-mode manifest approach (source+flags → hash of every included header) — this is the mechanism to replicate, not full re-preprocessing on every lookup
- Why hashing a compiler by "clang-18" is unsafe (distro patches, locally built toolchains) versus hashing the binary's actual content
- `-ffile-prefix-map` and why absolute paths in debug info break reproducibility across checkouts — a likely follow-up question

**Checklist:**
- [ ] Cache key includes header-transitive hashing, not just the source file
- [ ] Cache key includes compiler binary content hash, canonicalized flags, target triple
- [ ] Editing a header correctly invalidates the cache for TUs that include it (test this explicitly)
- [ ] Real numbers recorded: first-build time, second-build time, cache hit rate
- [ ] Can explain why the naive `hash(source + version + flags)` key is wrong, unprompted

### Why content-addressing instead of a filename-based cache

A filename tells you nothing about whether the content behind it is still valid — `parser.o` from yesterday and `parser.o` from today are both just "parser.o" unless the cache key is derived from what actually determines the output. Content-addressing makes the key inseparable from the inputs that produced it.

### Example

Build 1: `key = hash(parser.cpp + parser.h contents + clang++ binary hash + flags + target)` → miss → compiles → stores `parser.o` under that key. Build 2, no changes: same key → hit → `.o` copied from cache, no compile. Build 3, `parser.h` edited: the header's hash changes, so the key changes, so it's correctly a miss again — with no separate "did a header change" check needed, because the key itself already encodes it.

## Day 10 — Resource-aware scheduling, or buffer

**Build (if on schedule):** workers advertise architecture/compiler/capacity at registration; scheduler filters to eligible workers per job before assigning.

**Build (if behind):** no new feature — spend the day solidifying Days 1–9 instead. Don't start anything new after this point; the remaining days are for narrative and rehearsal, not features.

**Why:** this maps directly to real cross-target build work (Android APK + Windows EXE from one checkout) and is cheap to add on top of an existing worker table — but it's not worth risking Days 11–14 for.

**Read/learn first:**
- Skim how resource-aware schedulers elsewhere express eligibility (e.g. Kubernetes node selectors/taints, at a concept level only — no need for the implementation)

**Checklist:**
- [ ] Decision made honestly: build this, or take the buffer day
- [ ] (If built) workers advertise capabilities; a job requiring a specific architecture only lands on an eligible worker
- [ ] (If buffer) Days 1–9 checklist items are all genuinely checked, not just attempted

### Why this needs to exist at all

A real build fleet is never homogeneous — building both Android and Windows targets from one checkout means an ARM cross-compile job sent to an x86-only worker would simply fail, or silently produce the wrong architecture's object if the worker isn't careful. The scheduler needs to know which workers *can* run a job before deciding which one *should*.

### Example

Worker A registers with `{arch: "x86_64", compilers: ["clang-18"]}`; Worker B registers with `{arch: "aarch64", compilers: ["clang-18-android-ndk"]}`. A job requiring `aarch64` is filtered to eligible workers first (just B), then scheduled — never round-robined to A and left to fail.

## Days 11–12 — README, decisions log, benchmarks, git history

**Build:** README written as the interview narrative — what's built and running, what's deferred and why, the cache benchmark numbers, the DAG-modeling and cache-key corrections. `DECISIONS.md` finalized from what accumulated during the AI-assisted sessions. Git history cleaned up: one commit per phase, tagged where each demo works.

**Why:** an interviewer reads the README before anything else, and a clean, honest one — clear about scope and trade-offs — is itself a signal of engineering judgment. A tidy, phase-tagged git history is what makes "I built this incrementally, not as one AI-generated dump" credible rather than asserted.

**Read/learn first:**
- Nothing new — this is consolidation. If short on time, prioritize the README over polishing git history.

**Checklist:**
- [ ] README states what's built vs. deferred, and why, in plain terms
- [ ] Cache benchmark numbers are in the README
- [ ] `DECISIONS.md` covers every non-obvious design call (DAG model, cache key, deferred infra, queue interface)
- [ ] Git history has one commit per phase, tagged where each demo works
- [ ] Someone who didn't build this could read the README cold and understand the system

### Why this is engineering work, not paperwork

A system nobody can explain from its own documentation is indistinguishable, to an interviewer or a future maintainer, from a system that was never really understood by the person who built it. Writing the README and decisions log is where you find out whether you can justify each choice in your own words, before someone else asks you to.

### Example

`DECISIONS.md` entry: "Cache key hashes headers via `-MD` output, not just the source file, because a naive key gives silent stale hits on header edits — see Days 8–9." That sentence, written now, is close to what you'll say in the interview.

## Day 13 — Memory-rebuild exercise 2

**Build:** nothing new — from a blank file, rebuild the DAG topological-sort scheduling logic from memory, timed. Compare against the real implementation.

**Why:** this is the single highest-value hour of the two weeks for interview defensibility. If asked to write the scheduler's core logic on a whiteboard, this exercise determines whether you can, cleanly, under pressure — better to find the gap now than in the interview.

**Read/learn first:**
- Whatever gap Exercise 1 (Day 7) surfaced — re-read that specific topic before attempting this, don't just redo the same exercise blind

**Checklist:**
- [ ] Exercise 2 completed and timed
- [ ] Diffed against the real implementation
- [ ] Specific gaps identified and, if time allows, restudied same day

### Why rebuilding from memory, not just re-reading

Recognizing correct code and being able to produce it are different skills — reading your own topological sort and nodding "yes, that's right" proves recognition, not recall. An interviewer asking you to write it live is testing recall, so that's the skill this exercise trains.

### Example

Given `{A: [], B: [A], C: [A], D: [B, C]}`, a correct rebuild produces an order where A comes before B and C, and both B and C come before D (e.g. `A, B, C, D` or `A, C, B, D` — either is valid, since B and C are independent of each other). If your from-memory version can't explain why B and C's relative order doesn't matter but D's does, that's the gap to restudy.

## Day 14 — Rehearsal

**Build:** nothing — this day is entirely rehearsal, spoken out loud, not re-read silently.

**Rehearse:**
- The 30-second elevator pitch, including the "what I built vs. designed and deferred, and why" sentence — said before anyone asks, not after
- Full architecture walkthrough in order: CLI → coordinator → DAG → scheduler → queue → worker fork/exec → artifact → cache check → link
- The two "caught my own mistake" stories: DAG modeling (siblings vs. generated-header edges), cache key (naive vs. header-aware)
- Hard-question answers: ccache+bigger-machine, speedup curve and where it flattens, identical-`.o`-across-workers, why C++, what's next given a month, honest weakest point
- The honest answer to "did you use AI to build this" — yes, plus what you personally decided, reviewed, tested, and can rebuild

**Why:** talking points read on a page and talking points that come out smoothly under a live follow-up are different skills. This day converts one into the other.

**Checklist:**
- [ ] Elevator pitch rehearsed out loud, 3+ times
- [ ] Full architecture walkthrough rehearsed without notes
- [ ] Both "caught my own mistake" stories rehearsed
- [ ] Every hard question has a rehearsed, specific answer
- [ ] Resume bullet matches what was actually built

### Why rehearsal is its own day, not something you do in your head

The gap between "I could explain that" and actually explaining it clearly, out loud, under a follow-up question, is exactly where interviews are won or lost — and it's invisible until you try it out loud and hear yourself stumble.

### Example

Say the elevator pitch to a wall, timed. If it runs over 45 seconds or you lose the thread partway through, that's information you want on Day 14, not in the actual interview.

## Final checklist — done by Day 14

- [ ] `docker-compose up` demo runs the DAG build across 2+ workers
- [ ] Worker-kill mid-build recovery demo, reliable across repeated runs
- [ ] Cache benchmark numbers recorded: first-build time, second-build time, hit rate
- [ ] README states what's built vs. deferred, and why
- [ ] `DECISIONS.md` covers every non-obvious design call
- [ ] Git history: one commit per phase, tagged where each demo works
- [ ] TSan/ASan clean on the core (scheduler, DAG, worker registry)
- [ ] Hand-written tests for scheduler/DAG/lease logic
- [ ] Both memory-rebuild exercises completed
- [ ] Resume bullet and talking points rehearsed out loud