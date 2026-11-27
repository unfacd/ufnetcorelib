# Session I/O dispatch proof

## Scope

1. The harness compiles `src/ufnet_core/session_io_work.c` directly. It exercises the production queue, continuation builder, server-result classifier and dispatcher.
2. The fixture uses Linux epoll, nonblocking socket pairs, pthread mutexes and read/write locks, atomic reference counters and allocation failure injection.
3. Session acquisition and protocol callbacks are fixture adapters. The acquisition adapter uses the actual session status constants and macros from `session_status.h`; it does not replace suspended/service flags with a boolean. The complete server worker, session recycler, authentication migration, TLS and application protocols are not executed by this harness.
4. The classifier receives real `UFSRVResult` types and codes. The fixture injects `RESCODE_USER_RATELIMIT_EXCEEDED`, `RESCODE_IO_SESSIONSTATE` and `RESCODE_IO_POLL` while keeping an actual socket open. The Redis rate-limit backend and the worker adapter's connection-state checks remain outside this runtime harness.
5. These are executable regression and stress tests. Coverage does not establish correctness for every possible thread interleaving.

## Review findings and checks

| Finding | Implementation | Executed check |
|---|---|---|
| Healthy connections remain queued forever | Parenthesize the mask arguments in the real session status macros; keep those definitions in a lightweight shared header | A connected session must pass both combined-mask guards; combined set/clear operations preserve unrelated bits. The old mask fails the regression assertion, and restoring either broken macro is detected. |
| Allocation failure or full queue loses an edge | One intrusive pending slot per session; submission neither allocates nor has a fixed queue capacity | Queue 4,096 sessions while `malloc` returns NULL; coalesce read/write masks and retries; verify every pending item and reference |
| Buffered-only work must not read the socket | An empty mask with no continuation invokes no socket callback; the worker retains its initial `_s_DrainQueuedPayload` call | Callback counters remain unchanged for buffered-only dispatch; full worker drain remains an integration check |
| Terminal read schedules an unwanted write | Terminal readiness suppresses write dispatch; positive reads schedule a read-only continuation until EOF or no progress | Drain 8,192 bytes in 127-byte chunks after half-close and full close, including HUP with ERR; verify all bytes, zero writes and no additional epoll edge |
| Suspended sessions discard readiness | A suspended session with a valid connection remains pending; invalid connections retire their retained work | Suspend, attempt acquisition, resume, and acquire the original work with its reference intact |
| Blocking session lock stalls unrelated sessions | Try-lock acquisition rotates deferred sessions through a shared worker queue with a 1 ms retry delay | Hold one session lock while four workers process 63 other sessions; unlock and verify its original event completes |
| Recoverable read failure discards the writable edge | `SessionIoReadOutcome` separates successful reads, failures on a valid connection, and terminal results; deferred dispatch retains pending directions | Reject two reads using actual server result codes, retain one queued reference, then read and write successfully without another epoll edge |
| Read deferral spins or fresh notifications bypass the delay | Pending work carries a monotonic `not_before_ns`; merges preserve the later deadline; the worker waits for the earliest queued deadline | Assert no dequeue before either deadline, unrelated-session progress, and retention when a fresh OUT notification is queued during the active read |
| Explicit OUT continuation is ignored | `SessionIoContinuation` accepts `EPOLLOUT` independently of the legacy write-request flag | An explicit OUT retry sets write-only continuation; terminal deferred work retains read-only draining |


Additional checks cover writable-only readiness, combined readiness, actual socket backpressure and EPOLLOUT recovery, stale queued session identity, queued descriptor replacement, shutdown reference cancellation, and simultaneous read/write continuations. Read continuations use `io_retry_events`; the existing `SESNSTATUS_RECYCLEREQUEST` write continuation remains compatible with HTTP sendfile.

## Accepted-connection regression

1. The 2026-11-11 15:35 Docker run accepted TCP connections but did not complete WebSocket handshakes. Both CLI processes exited on their 90-second timeouts before issuing any fence POST.
2. A single identity-only CLI invocation reproduced the handshake timeout within five seconds. A debugger showed three queued work items, both I/O workers in timed waits, and an accepted session with status `0x8` (`CONNECTED`) and fd 46.
3. The guard passed `SUSPENDED | INSERVICE` to `SESNSTATUS_IS_SET(x,y)`, whose expansion was `((x) & y)`. This evaluates as `(status & SUSPENDED) | INSERVICE` and always defers. The corresponding read-validity guard with `SUSPENDED | RECYCLED` was always false.
4. The correction parenthesizes both macro operands; `SESNSTATUS_UNSET` also parenthesizes the mask before complementing it. Flag values and `Session` layout are unchanged by this correction.
5. The status regression test failed before the correction and passed afterwards. Queue and recoverable-read tests now compile the real status definitions. Earlier fixture booleans did not exercise this production guard.
6. The running Docker binaries still require rebuilding and reloading with the corrected library. The handshake and bulk-fence feedback loops have not yet been verified green on a rebuilt server.

## Recoverable read semantics

1. A closed, suspended or invalid connection, a changed descriptor, or an explicit connection-closed/suspension result does not retain obsolete readiness.
2. An unsuccessful read result on a still-valid connection sets `deferred`. Its unserviced read and write directions are copied into `io_retry_events`; the read error remains the dispatch result. Terminal readiness suppresses the write direction.
3. A rate-limit rejection sets `io_retry_after_ns` to the current monotonic time plus the configured request-window interval and 1 ms. Other recoverable failures use a 100 ms retry delay.
4. Submission transfers the deadline into `SessionIoWork.not_before_ns`, merges readiness into any existing pending slot, and retains exactly one reference for that slot. New readiness cannot shorten its deadline. A new session identity or descriptor replaces obsolete pending work and its delay.
5. `SessionIoTake` exposes the earliest pending deadline when no work can be acquired. The worker converts its monotonic wait duration to the condition variable's realtime deadline. Busy-lock retries retain their separate 1 ms delay.
6. Eventual success services the retained directions without requiring a fresh edge. The tests also defer a terminal read, then drain to EOF with no write.

## Executed results — 2026-11-11

| Check | Result |
|---|---|
| `cmake --build build/dev -j 4` | Passed |
| New internal symbol audit of `build/dev/libufnetcorelib.a` | All six scheduler/classifier/dispatch entry points defined; no unresolved internal-style symbols |
| C11 syntax check with `-Wall -Wextra -Werror` | Passed |
| CMake `session_io_work_tests` target and CTest registration | All twelve GoogleTests passed |
| 100 standalone repetitions | 819,200 concurrent socket deliveries; all assertions passed |
| AddressSanitizer + UndefinedBehaviorSanitizer | Ten repetitions passed |
| ThreadSanitizer | Ten repetitions passed |
| CMake `UFSRV_BUILD_WITH_COVERAGE=ON`, Clang + sanitizers | Test suite passed; 100% of 100 instrumented lines executed; 59.03% of 476 instrumented branch outcomes taken |
| GCC 12 gcov without sanitizer instrumentation, production `session_io_work.c` only | 91/91 executable lines; 80/80 branch outcomes taken; 14/14 calls executed |
| Mutation controls | All twelve intentional regressions were rejected by assertions or compiler diagnostics |

Mutation controls introduce queue capacity loss, a lost event mask, a discarded busy session, a spurious read on writable-only readiness, a terminal write, a missing terminal continuation, a lost write continuation when a read retry is also requested, a recoverable read discarded as terminal, unsuccessful result types conflated with closure, bypassed retry deadlines, an unparenthesized status mask, and an unparenthesized status-clear mask. Each mutant is compiled from a temporary copy; production sources remain unchanged.

## Reproduction

Run from the repository root on Linux with socket shutdown permitted:

```sh
cmake -S . -B build/dev-test -DUFSRV_BUILD_WITH_COVERAGE=ON
cmake --build build/dev-test --target session_io_work_tests -j 4
ctest --test-dir build/dev-test -R '^session_io_work_tests$' --output-on-failure
python3 tests/concurrency_correctness_proofs/epoll_io_dispatch_proof.py --repeat 100 --mutation-check
python3 tests/concurrency_correctness_proofs/epoll_io_dispatch_proof.py --sanitize address --repeat 10
python3 tests/concurrency_correctness_proofs/epoll_io_dispatch_proof.py --sanitize thread --repeat 10
```

`build/dev-test` must first be configured with the repository test option enabled. The standalone runner requires a C11 compiler with GNU linker wrapping, pthread support and the selected sanitizer runtime. Each standalone run has a 30-second timeout; the concurrent fixture also enforces a five-second progress deadline per round.

## Integration requirements

1. Rebuild every consumer against the updated `Session` layout before running servers; the embedded pending slot changes its size.
2. Live rebuilt-server validation remains outstanding. The socket fixture does not establish application request delivery, protocol migration, Redis mutations, or production shutdown behavior.
3. Coverage figures apply exclusively to `session_io_work.c`, not the complete worker or protocol I/O implementation.
