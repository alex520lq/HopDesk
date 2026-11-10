# Sender / Transfer Service Refactor Contract

> Status: audit and extraction contract; no Sender behavior has been changed by this document.
> Scope: Windows/macOS sender path only. Receiver is treated as a consumer of inbound ACK/ABORT events and is not reworked here.
> Compatibility: preserve the existing wire protocol, ACK window capacity, retry timing, chunk format, Task/Reservation semantics, and public `xfer.h` API.

## 1. Current production layout

The sender is not an independently compiled module yet:

- `src/win/xfer.c` includes `xfer_sender_protocol.inc` and `xfer_sender_worker.inc`, then `xfer_receive.inc`.
- `src/mac/xfer.c` has the same include-based arrangement.
- Windows `xfer_sender_protocol.inc` is approximately 672 lines; macOS is approximately 652 lines.
- The protocol include contains rate pacing, frame send, ACK-window synchronization wrappers, per-file send, DONE commit, job execution, and outbound ACK/ABORT helpers.
- `xfer_sender_worker.inc` contains queue unlink/free and the sender thread; it directly reads and mutates the platform `s_x` state and job structure.
- File enumeration (`elist_push`, `walk_into`, guarded filesystem traversal) remains in each platform `xfer.c`.
- Public status/history/retry/cancel APIs and persistent history are also in `xfer.c`.

The shared Core already contains independently compiled `xfer_ack_window.[ch]`, `xfer_rate.[ch]`, `xfer_task.[ch]`, `xfer_budget.[ch]`, and `xfer_tracking.[ch]`. These are policy/state primitives, not a complete Sender Service.

## 2. Dependency findings that constrain extraction

### ACK path

The ACK window data structure is shared Core, but the synchronization and wake-up mechanism is still hidden in a file-static `s_ack`:

- Windows: `CRITICAL_SECTION + auto-reset event`.
- macOS: `pthread_mutex_t + pthread_cond_t`.
- Sender protocol code owns register/mark-sent/set-deadline/take/unregister/wait.
- Receiver code calls the same translation unit's `ack_deliver`.
- shutdown and cancellation paths directly signal `s_ack`.

Therefore ACK storage can only be moved safely after the inbound delivery, sender wait, cancel wake-up, shutdown wake-up, and lifecycle initialization/teardown paths are all migrated together. Moving only the static declaration or wrapper functions would leave a hidden cross-module dependency.

### Sender execution

The sender worker currently depends on:

- platform socket/session state and epoch checks;
- task cancellation and budget/task APIs;
- queue locks/conditions/events;
- status and history functions;
- the rate limiter and platform clock/sleep;
- filesystem traversal and file metadata snapshots;
- frame encoding and socket write serialization.

A new Sender module must receive these dependencies explicitly; it must not include platform application headers or reach into `s_x`, `s_ack`, `s_txlock`, or global status/history state.

### Platform differences that must remain adapters

- Windows uses Winsock, SRW/critical-section synchronization, waitable timers/events, UTF-16 paths and Win32 file metadata.
- macOS uses POSIX sockets, pthread synchronization, monotonic clock/nanosleep, UTF-8 paths and POSIX metadata.
- Both must preserve the same protocol decisions, ACK result mapping, retry/checkpoint rules and resource admission semantics.

## 3. Target boundaries

| Module | Owns | Must not own |
|---|---|---|
| Transfer Service facade | stable submit/cancel/query API and service lifecycle | platform file traversal or socket calls |
| Scanner adapter | safe enumeration, metadata snapshot, canonical relative paths | network send, ACK state, Task terminal policy |
| Sender Core/Application | job execution order, file/chunk/DONE protocol sequencing, retry/checkpoint decisions | HWND/HANDLE/NS* types, raw platform socket calls |
| ACK coordinator | ACK slot registration/delivery/take and wake-up lifecycle behind an opaque API | socket send, Task terminal ownership |
| Rate adapter | clock, wait/sleep and stop interruption | token-bucket/adaptive-rate policy |
| Socket adapter | session/epoch validation, serialized frame writes | ACK checkpoint policy |
| Status/history adapter | immutable snapshots and persistence | sender control flow |

The existing public `xfer.h` remains the compatibility boundary during migration. Do not introduce a second parallel task lifecycle or a speculative generic framework.

## 4. Extraction sequence

1. **ACK coordinator boundary (first code slice).** Move the platform-specific ACK lock/event/condition and ACK-window operations behind an opaque platform API. Migrate all producers and consumers in both platforms in one slice: sender register/mark/deadline/take/unregister, receiver delivery, cancel wake-up, shutdown wake-up, init/failure cleanup/destroy. Keep the wait policy in Sender until its dependencies are separately injected. Add production-linked tests for early ACK, ACK before deadline assignment, late ACK, duplicate ACK, stale batch, cancel wake-up and shutdown wake-up.
2. **Scanner boundary.** Define a platform-neutral immutable entry snapshot consumed by Sender. Extract safe traversal into platform scanner adapters. Test unreadable directories, symlinks/reparse points, path-length limits, disappearing files, partial enumeration and allocation/admission failure. Never enqueue an incomplete snapshot.
3. **Sender dependency context.** Define an explicit context for socket write, monotonic clock, wait, cancellation, peer-abort, status, logging and Task callbacks. First wire the existing implementation through it without changing behavior.
4. **Sender compilation unit.** Move protocol/job execution out of `.inc` into real compilation units; keep Win32/POSIX calls in adapters. Delete the old include files only after both builds link the new units.
5. **Transfer Service facade and snapshots.** Route public submit/cancel/retry/query operations through one documented service boundary; expose immutable status/history snapshots.
6. **Cross-platform parity.** Run identical Core vectors and paired Windows/macOS integration fixtures for ACK races, checkpoint recovery, cancellation, stale session, reconnect and task/reservation release.

## 5. Non-negotiable invariants

- Only a successful remote FILE_ACK advances the durable per-job checkpoint.
- A local read/send failure never advances the checkpoint.
- An unacknowledged file is retried after reconnect; an acknowledged file is not resent.
- ACK identity is the exact `(batch, id)`; stale/duplicate ACKs cannot satisfy another file's wait.
- ACK may arrive before the sender installs its post-FILE_END deadline; the receive timestamp must still be compared against that deadline.
- Cancel, peer ABORT, socket loss and shutdown wake a blocked ACK waiter without destroying synchronization objects while threads may still use them.
- Task and Reservation ownership remains single-owner and exactly-once.
- Slot and byte budgets remain enforced; no unbounded queue or implicit image copy is introduced.
- Protocol bytes, message ordering, timeout values and user-visible behavior do not change in this extraction.

## 6. Validation gate

For each slice record the exact tested commit and Actions run. Required checks:

- production-linked unit tests for the extracted module (not source-text extraction alone);
- Windows MSVC app build and relevant native tests;
- macOS Apple Clang app build and relevant native tests;
- transfer protocol and TCP loopback tests;
- race/fault tests for early/late/duplicate ACK, cancel, shutdown and reconnect;
- explicit note that physical two-device and long-duration tests remain unverified unless actually performed.

A slice is not complete merely because the new files exist or the CI source-contract assertions pass. The old include/global dependency must be removed from both production paths, and the new module must be linked by both native builds.


## 7. ACK Coordinator extraction status (2026-11-10)

Implemented on both platforms:

- Added opaque `hop_xfer_ack_coordinator` adapters in `src/win` and `src/mac`. The platform modules own the synchronization primitive and the shared Core ACK Window instance.
- Sender registration, sent timestamp, deadline, delivery, take, unregister and batch cleanup now go through the coordinator API.
- Receiver ACK delivery and ABORT/cancel/shutdown wake-up paths now use the coordinator API; no production call site reaches into `s_ack.lock`, `s_ack.event/cond`, or `s_ack.window`.
- `wait_take` preserves the platform-specific no-lost-wakeup contract: Windows uses an auto-reset event; macOS checks and waits under the same mutex/condition-variable lock.
- ACK timestamp/deadline semantics and the existing Core slot state machine are unchanged. Sender cancellation/peer-abort/socket-loss checks remain in Sender orchestration.
- Added `tests/xfer_ack_coordinator_tests.c` and wired it into both platform core test scripts. Cases cover ACK-before-deadline-installation, exact receive-time deadline evaluation, duplicate/stale ACK rejection, batch cleanup and bounded slots.

Validation status:

- Source-level migration is complete on Windows/macOS; this is not yet a claim of successful compilation or runtime validation.
- **GitHub Actions Run #1195 (ID `42765170981`, tested head `83b86e8727ffe36f6ed456a67bcacf78ce216ca0`) passed on both platforms.** Windows MSVC: app build, portable Core/regression (including ACK coordinator), transfer protocol, TCP loopback, native control/data role + reconnect, updater syntax and packaging all passed. macOS Apple Clang: app build, portable Core/regression (including ACK coordinator), transfer protocol, TCP loopback, control/listener loopback and packaging all passed. Release publication was skipped as expected for a PR.
- CI caught two test integration issues before the passing run: the Windows HA fixture needed to link the new platform adapter; the cross-thread test needed to tolerate an unrelated/stale auto-reset event notification, matching the production Sender retry loop. Both were corrected and covered by Run #1195.
- Physical two-device tests remain outside CI and are not claimed.


## 8. Scanner boundary extraction status (2026-11-10)

The scanner already existed as platform-specific \`.c\` files before this slice. This slice completes the ownership and dependency boundary rather than claiming the files were newly created.

- \`elist\` is now only an owned scan snapshot; it no longer embeds or disposes a Task. \`elist_free\` releases snapshot entries only.
- The submit layer owns Task init/state/finish/dispose and passes an explicit \`hop_xfer_scan_services\` context into the scanner.
- Entry reservation, cancellation, snapshot-array reallocation and Watchdog begin/end are injected callbacks. Scanner implementation no longer calls Task, Watchdog or logging globals directly, and its public header no longer includes the Task header.
- Scanner reports typed failure causes (invalid input, cancellation, unavailable source, link/reparse rejection, unsupported entry, path/entry limit, budget rejection, enumeration failure and OOM). Submit rejects the entire snapshot on any failure and logs the scan error; no partial tree is queued.
- macOS \`readdir\` now clears and captures \`errno\` around each read, distinguishing normal EOF from partial enumeration failure. Windows preserves the \`FindNextFileW\` error across watchdog cleanup and rejects non-\`ERROR_NO_MORE_FILES\` termination.
- Added production-linked tests for complete directory snapshots, cancellation, missing paths, symlink rejection (macOS), budget rejection and injected snapshot-allocation failure on both platforms. Source-contract tests verify explicit service injection, no implicit Task/Watchdog/log calls in Scanner and all-or-nothing submit behavior.
- **Validation: GitHub Actions Run #1236 (ID \`42771934666\`, tested head \`8dafd1dbf4adc970b4dbadc3580d5e11962fb5bf\`) passed Windows MSVC and macOS Apple Clang.** Both app builds, portable Core/regressions (including scanner tests), transfer protocol, TCP loopback, platform control/reconnect tests and package steps passed. Release publication was skipped for the PR.
- Remaining scanner hardening: deterministic injection of mid-stream directory enumeration failure and Windows reparse-point runtime fixture are not yet directly exercised; implementation checks and source-contract assertions are present. Real-world filesystem race/TOCTOU behavior remains a separate security-hardening item.

Next slice: define and wire the explicit Sender dependency context (socket write/session validation, clock/wait, cancellation/peer-abort, status/logging and Task callbacks) without changing protocol sequencing. Only after that should the sender protocol include be moved into independent compilation units.


## 9. Sender dependency context contract (scaffolding)

Added `src/core/xfer_sender_context.h` as the platform-neutral callback contract for the next extraction slice.

The context covers:

- monotonic time, interruptible waits and shutdown observation;
- socket/session epoch validation and serialized complete-frame writes;
- batch/task cancellation and peer ABORT;
- status timing and logging callbacks;
- Task state, completion gate and terminal outcome callbacks.

The API uses opaque `intptr_t` socket handles and integer Task IDs; it does not expose Winsock, Win32, POSIX or Objective-C types to Core. The context is borrowed for one operation and must remain alive until all sender/ACK waits return. Optional status/log callbacks may be absent; execution and lifecycle callbacks are mandatory.

**This is scaffolding only, not a completed dependency inversion.** The existing Windows/macOS sender implementation still reads platform globals and invokes platform services directly. The next code step is to construct the context in each platform adapter, route cancellation/session/time/status/Task access through it, and add production-linked context tests before moving protocol/job execution out of the `.inc` files. Do not remove the old globals or includes until both platforms use the context and CI passes.


## 10. Sender context contract test coverage

Added `tests/xfer_sender_context_tests.c` and wired it into both `scripts/test-core.sh` and `scripts/test-core.ps1`.

The test verifies:

- mandatory execution/lifecycle callbacks are rejected when absent;
- status/logging callbacks are optional;
- monotonic time and interruptible wait callbacks are callable through the opaque user context;
- session validation and complete-frame write callbacks receive the socket handle and epoch;
- cancellation and peer-abort callbacks receive the exact batch/task identity;
- Task state/completion/finish callbacks are reachable through the same contract.

This is a contract/ABI test, not a production sender integration test. It does not prove that the production sender uses the context. That remains the next migration gate.
