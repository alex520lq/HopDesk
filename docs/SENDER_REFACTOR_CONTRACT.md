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


## 11. Sender clock/wait context: first production adoption (2026-11-10)

- Both Windows and macOS now construct a platform-owned hop_xfer_sender_context clock subset during xfer_init.
- Sender rate pacing calls the injected now_ns, wait_ns, and is_stopping callbacks instead of directly implementing the pacing wait loop in rate_wait_for_chunk.
- The macOS wait adapter uses monotonic nanosleep in bounded slices and checks shutdown between interrupted waits. The Windows adapter uses the existing high-resolution waitable-timer path, with its existing fallback, and observes the stop event.
- Added hop_xfer_sender_context_clock_valid to validate this incremental subset without weakening hop_xfer_sender_context_valid, which still requires the complete execution/lifecycle callback set.
- Extended the production-linked context contract test to prove that the clock subset is valid while the incomplete full context is rejected.
- This is the first production use of the Sender context, but only for rate pacing. Socket/session writes, cancellation/peer-abort, status/logging and Task lifecycle still use their existing platform paths and are not claimed as injected.
- Validation must be recorded against the resulting Actions run before this slice is considered CI-verified.

Next slice: route cancellation and peer-abort observations through the context, then move Task lifecycle transitions behind the same context while preserving the existing hop_xfer_task ownership handle in the platform worker. Socket/session and complete-frame write callbacks follow after their lock-order and frame-serialization behavior are covered by production-linked tests.


## 12. Shared Sender execution engine (2026-11-10)

- Added `src/core/xfer_sender_engine.[ch]` as the shared file/image execution policy owner.
- Both platform `run_job` adapters now delegate file ACK-window fill/drain, retry classification, FIFO ACK consumption, and contiguous reconnect checkpoint advancement to `hop_xfer_sender_run_files`.
- The shared file-entry path now owns FILE_BEGIN payload construction, ACK registration/mark/deadline ordering, chunk reads, per-chunk CRC, whole-file CRC folding, source-size probing, FILE_END and read/open failure semantics. Windows/macOS provide native open/read/close callbacks and frame/ACK adapters.
- The shared image path now owns IMAGE_BEGIN/chunk/CRC/FILE_END sequencing, cancellation/peer-abort checks, and completion state. The duplicate platform image loops were removed.
- `hop_xfer_sender_context` now has a complete production dependency set: monotonic clock, full-duration interruptible wait, stop/cancel/peer-abort, session validation, serialized multi-part frame writes, Task bind/state/completion/finish/dispose, and optional status/log callbacks. `send_small` and `send_chunk` use the context's atomic frame-write boundary.
- Sender worker Task lifecycle operations now call the context adapter rather than directly reaching into Task Manager. The Task cancellation callback intentionally does not acquire the queue lock; the queue-cancel flag is checked by the caller to avoid recursive lock acquisition in `queue_take_cancelled`.
- Added dedicated production-linked tests for file-window checkpoint/retry/reconnect policy, image chunk order/CRC/cancel, file IO/CRC/ACK reservation/source mutation/failure, and the context contract. Both platform Core scripts and the Windows HA fixture link the production engine.
- The first CI attempt exposed a forward declaration and timing-stage include omission in the new Core file; those were corrected. The next full Windows/macOS Actions run is the acceptance gate. Do not treat the earlier failed run as validation.

Remaining Sender work:
1. Move platform Sender Protocol adapters into independent `.c` compilation units; eliminate the remaining `xfer_sender_protocol.inc` compatibility fragments.
2. Extract worker queue scheduling, retry deadlines, terminal Task/status/history ordering behind an explicit worker context; keep OS thread/QoS and socket primitives in platform adapters.
3. Remove remaining Sender global captures and `xfer_sender_worker.inc` / `xfer_submit.inc` include-fragment dependencies.
4. Run dual-platform CI, then physical two-device clipboard/file transfer, reconnect, cancel/ABORT, weak-network and long-duration stress tests.

No physical two-device or weak-network acceptance is claimed by the unit/CI tests.


## 13. Shared DONE/DONE_ACK commit policy (2026-11-10)

- Added `hop_xfer_sender_run_commit` to the shared Sender Core. Windows/macOS `send_done_result` now only adapt current connection/session snapshots, DONE frame emission, ACK Coordinator calls, monotonic clocks and interruptible waits.
- Core owns reconnect-budget deadline, session replacement rejection, retry after send failure/ACK timeout/send-dead, DONE_ACK expected COMPLETE/PARTIAL classification, explicit FAILED rejection, and peer ABORT outcome.
- Preserved the existing 100 ms retry cadence, DONE_ACK slot, ACK timeout cap, reconnect budget, and wire payload (`batch`, `ok`, `total`, zero reserved bytes). The adapter sets the send epoch before writing so stale connections cannot receive a DONE frame.
- Added production-linked `xfer_sender_commit_tests.c` covering complete/partial, failed ACK, peer abort, timeout/send-dead retry, unavailable connection recovery, transient send failure, session replacement, shutdown and ACK reservation failures.
- The previous platform-specific completion-loop fixture has been replaced with assertions that both adapters call the shared Core. The dedicated Core suite now owns deterministic commit-policy behavior.

## 14. Shared Sender queue policy extraction (2026-11-10)

- Added src/core/xfer_sender_queue.[ch] as the platform-neutral owner of intrusive FIFO storage and scheduling policy. Core now owns head/tail/count, push-back/push-front, batch lookup, detach-all, cancelled-task selection, head-only FIFO eligibility, retry-time gating, cancellation-batch expiry, reconnect-budget expiry, and link repair.
- Windows and macOS retain native job records and their existing platform queue locks, but no longer own queue head/tail or mutate job links directly. Their adapters expose node accessors and invoke the same Core queue API. Native synchronization, worker thread/QoS, socket readiness snapshots, retry timestamp storage/requeue orchestration, and terminal side effects remain platform-owned.
- Preserved the important platform behavior: individually cancelled jobs can be removed from any queue position without disturbing FIFO order; only the head can be selected; cancellation/deadline expiry is processed offline; Windows requires a live socket before dequeue; macOS retains its 200 ms state-refresh poll and suppresses a ready dequeue on the same pass that woke from the condition wait.
- Added tests/xfer_sender_queue_tests.c for middle/tail/head cancellation, tail repair, FIFO no-overtake, peer readiness, wake/poll gating, cancel-batch expiry, and reconnect-budget expiry while offline. Wired the production Core module and test into both scripts/test-core.sh and scripts/test-core.ps1; updated source-contract regression checks to verify both platform adapters call Core.
- Validation: **GitHub Actions Run #1371 (commit 1769ccfde5bd617177e396b938e82079d9fed67e) passed on both Windows MSVC and macOS Apple Clang.** App builds, Core/regression tests (including shared queue tests), transfer protocol, TCP loopback, native control/data role + reconnect tests, and packaging all passed. Physical two-device and long-duration soak tests remain unverified.
- Remaining worker extraction: move native thread start/stop/join and queue synchronization behind explicit platform worker adapters; move Task/Reservation completion-gate, finish/dispose and status/history publication behind a Worker Context; remove the xfer_sender_worker.inc include fragments only after production-linked tests cover stop/join timeout, shutdown drain, cancel/ABORT, retry requeue and exactly-once Task/Reservation release.

## 15. Shared Sender reconnect retry policy (2026-11-10)

- Added src/core/xfer_sender_retry.[ch] as a pure Core policy for incrementing reconnect attempts, initializing/preserving the reconnect give-up deadline, deciding whether another retry is permitted, and calculating the next retry timestamp.
- Windows/macOS workers now call the same policy and only apply its returned attempt/deadline values to their native job record. Backoff delay values still come from the existing shared hop_xfer_job_retry_delay_ms helper; protocol sequencing, retry count limit, reconnect budget, queue-front reinsert and checkpoint ownership are unchanged.
- Retry deadline addition saturates at UINT64_MAX instead of wrapping. Attempt count is bounded by the existing HOP_XFER_JOB_RETRY_MAX policy.
- Added tests/xfer_sender_retry_tests.c for first/repeated retry, attempt exhaustion, exact give-up boundary, saturating deadlines and invalid input. Wired the production Core module and test into both platform Core scripts and the Windows HA link target; regression assertions verify both workers use the shared planner.
- Validation: **GitHub Actions Run #1371 (commit 1769ccfde5bd617177e396b938e82079d9fed67e) passed on both Windows MSVC and macOS Apple Clang.** App builds, Core/regression tests (including shared queue tests), transfer protocol, TCP loopback, native control/data role + reconnect tests, and packaging all passed. Physical two-device and long-duration soak tests remain unverified.
- Next worker slice: finish explicit Task/Reservation lifecycle callbacks in the Worker Context, then split native thread start/stop/join adapters from the remaining worker orchestration.

## 16. Shared Sender terminal lifecycle decision (2026-11-10)

- Extended the shared Sender job-result enum with COMMIT_UNKNOWN and COMMIT_REJECTED, matching the existing platform protocol outcomes.
- Added src/core/xfer_sender_lifecycle.[ch] to map every terminal sender result to one Task terminal state, status phase and user-visible detail. The mapping covers complete/partial success, local cancellation, receiver cancellation versus peer failure, rejected DONE, uncertain DONE delivery, and exhausted reconnect.
- Windows/macOS workers now use the same terminal decision after the existing completion/cancellation arbitration and before Task finish/status publication. Platform-specific ABORT/DONE sends and completion-gate ordering remain in the worker.
- Added tests/xfer_sender_lifecycle_tests.c for every result branch and invalid input; wired it into both Core test scripts, the Windows HA link target and source-contract regressions.
- Validation: **GitHub Actions Run #1371 (commit 1769ccfde5bd617177e396b938e82079d9fed67e) passed on both Windows MSVC and macOS Apple Clang.** App builds, Core/regression tests (including shared queue tests), transfer protocol, TCP loopback, native control/data role + reconnect tests, and packaging all passed. Physical two-device and long-duration soak tests remain unverified.
- Remaining Worker Context work: move completion-gate ownership, terminal finish/dispose and status publication behind explicit per-operation callbacks; then split native thread start/stop/join adapters from worker orchestration.

## 17. Sender platform runtime context adoption (2026-11-10)

- Windows and macOS now bind the Sender Context user pointer to an explicit platform runtime adapter containing references to the native session/socket state, state/queue lock, frame-write lock, stop signal, rate timer where applicable, and peer-abort batch.
- Session validation, session snapshot, complete-frame serialization, stop observation, peer-ABORT observation and interruptible pacing waits now read platform state through that supplied context rather than capturing s_x/s_txlock directly inside those callbacks.
- The context is initialized after the transfer state is zeroed and before worker startup; it stores stable addresses of the platform-owned fields, and callbacks are only invoked after the native locks/events are initialized.
- Updated the production session-transport fixture to inject a fake platform context. Regression checks assert that these context callbacks do not directly reference s_x and that sender_context_init binds the user pointer.
- This is an incremental dependency-inversion slice, not the end of Sender Context migration: Task Manager, status and logging adapters still route through platform singleton functions, while some higher-level protocol orchestration still reads s_x directly. The next slice must pass the context explicitly through worker/job operations and move Task finish/dispose/status effects behind per-operation callbacks.
- Validation: **GitHub Actions Run #1371 (commit 1769ccfde5bd617177e396b938e82079d9fed67e) passed on both Windows MSVC and macOS Apple Clang.** App builds, Core/regression tests (including shared queue tests), transfer protocol, TCP loopback, native control/data role + reconnect tests, and packaging all passed. Physical two-device and long-duration soak tests remain unverified. Physical two-device and long-duration soak tests remain unverified.

## 18. Explicit FILE_BEGIN buffer capacity (2026-11-10)

- Updated hop_xfer_sender_send_file_entry to accept begin_buffer_size separately from chunk_size. FILE_BEGIN payload validation now checks the actual BEGIN buffer capacity instead of incorrectly comparing the BEGIN payload length with the data chunk size.
- Windows/macOS adapters pass the available capacity of the buffer region beginning at tx + CHUNK_HDR. This preserves the existing shared TX allocation and wire behavior.
- Updated tests/xfer_sender_file_tests.c to verify a 256-byte BEGIN buffer can send a file using 8-byte data chunks, and that an undersized BEGIN capacity is rejected before ACK registration or message send.
- Validation: covered by GitHub Actions Run #1371; both platform app builds and all Core, transfer protocol, TCP loopback and native connection/reconnect tests passed. No protocol bytes or message ordering changed.

## 19. Per-job Sender effects and DONE wait context (2026-11-10)

- Sender jobs already snapshot the initialized platform context at submit time. This slice now routes worker status start/end (including history persistence), file-phase progress, stage/chunk timing, and worker/engine log messages through that job-owned context. Platform adapters remain responsible for mapping the opaque job to the native tracking/history implementation.
- File and image frame writes now use the job's session/epoch snapshot and serialized write callback for FILE_BEGIN/FILE_END, IMAGE_BEGIN/IMAGE_END and chunk frames; the shared protocol bytes and frame ordering are unchanged. ACK coordinator operations and native rate-limiter storage remain platform adapter dependencies.
- Fixed a real DONE/DONE_ACK integration defect: the platform stop callback previously called sender_context_is_stopping(NULL), which is fail-closed and therefore always reported stopping. The commit loop could consequently return COMMIT_UNKNOWN before waiting for DONE_ACK. send_done_result now supplies the owning job as ops.user, and both stop and retry-wait callbacks read that job's context. The existing 100 ms retry cadence, session checks, ACK reservation and reconnect budget are unchanged.
- Extended the session-transport production fixture to execute the real platform stop/wait adapters against fake per-job callbacks, including both running and stopping cases. Extended the context contract test to exercise status start/end, file phase, stage timing and chunk progress callbacks. Source-contract checks prevent worker status publication from bypassing the per-job context.
- Validation: GitHub Actions Run #1395 passed on commit a3b7b2f06d409aa6535ec36bacedfeb27c95ed72 (Windows MSVC and macOS Apple Clang, full build/Core/protocol/loopback/reconnect/package jobs). This validates the per-job status, frame and DONE stop/wait changes in this section. Physical two-device, disconnect-during-DONE and long-duration soak tests remain unverified.
- Next: extract reconnect-kick and ACK coordinator ownership behind explicit platform Sender adapters, then continue separating worker orchestration from platform I/O. Do not remove worker include fragments until stop/join timeout, shutdown drain, cancel/ABORT, retry requeue and exactly-once Task/Reservation release are production-linked and tested.

## 20. Per-job ACK, DONE and ABORT transport context (2026-11-10)

- Added an explicit batch-cancel query to Sender Context. The native adapter reads the batch cancellation flag under the existing queue/state lock; the callback contract explicitly forbids calling it while that lock is already held. Task cancellation remains a separate Task Manager query, so Core queue selection continues to use its lock-safe task-only predicate.
- Sender ACK wait now receives the owning job context, task ID, socket handle and connection epoch. It checks receiver ABORT, batch/task cancellation, shutdown and session/socket/epoch validity through callbacks rather than reading platform globals directly. The ACK coordinator and its native wait primitive remain platform-owned.
- File ACK and DONE_ACK paths both pass their owning job context. The commit connection adapter snapshots socket/epoch into the job before sending DONE, so the subsequent ACK wait validates the exact connection that carried that DONE.
- DONE and cancellation ABORT frames now use the per-job serialized frame writer/session snapshot. The legacy global wrappers remain only for non-job/receiver-side protocol messages.
- Extended the context contract tests for batch cancellation; source-contract regressions assert ACK wait has no direct platform-global access and that DONE/ABORT frame paths use the job context.
- Validation: GitHub Actions Run #1415 passed on commit 8db079ef16052ca758b51c8579f8562cd3f074d0 (Windows MSVC and macOS Apple Clang, full build/Core/protocol/loopback/package jobs). This validates the per-job ACK, DONE and ABORT context changes.
- Remaining platform edges: reconnect kick, ACK coordinator storage/registration and queue lock ownership. Native thread lifecycle is now behind platform adapters; these remaining pieces must be separated without changing ACK exactly-once semantics, retry checkpoints or shutdown behavior.

## 21. Native Sender thread lifecycle adapters (2026-11-10)

- Added src/win/xfer_sender_threads.[ch] and src/mac/xfer_sender_threads.[ch]. xfer.c no longer creates or joins the native listener/connection and sender threads directly; it delegates thread-handle ownership and native start/join operations to the platform adapter.
- Preserved Windows startup order (listener then sender), failure cleanup, 3-second per-thread shutdown wait, and the existing policy of closing handles while skipping shared-state cleanup if either thread has not joined.
- Preserved macOS startup order (sender then connection) and shutdown join order (connection then sender). Unlike the previous unchecked pthread_create calls, partial startup failure is now detected; the already-started sender is stopped, signalled and joined before writer/ACK/history/lock cleanup. A failed join preserves shared state rather than destroying synchronization primitives under a potentially live thread.
- macOS shutdown now destroys the sender mutexes and condition variable only after both threads have joined and receiver/writer cleanup has completed.
- Added native adapter tests for Windows and macOS and wired them into their platform Core test scripts. The Windows HA fixture links the new adapter. Build scripts discover the new platform .c files automatically; source-contract checks prohibit direct thread creation/join calls in xfer.c.
- **Validation: GitHub Actions Run #1591 (ID `43019588698`, tested commit `58b99d969f90fb3e8756a47b5ec6b62113217fcd`) passed on Windows MSVC and macOS Apple Clang.** Both app builds, Portable Core and production regression suites, file-transfer protocol tests, TCP loopback, native control/data role + reconnect integration tests, and platform packaging passed. PR release publication was skipped. Stop/join timeout behavior is still not tested with an intentionally stuck production worker; physical two-device shutdown and long-duration soak remain unverified.
- Next: extract reconnect-kick and ACK coordinator ownership behind explicit platform Sender adapters, then separate worker orchestration from the remaining platform-specific I/O and shutdown code.



## 22. Per-job reconnect-kick callback (2026-11-10)

- Added the mandatory `request_reconnect` callback to `hop_xfer_sender_context`. The shared Sender engine no longer calls the global `xfer_kick()` entry point directly; it requests reconnect through the active job's captured context.
- Windows and macOS platform contexts now bind the peer-ready flag together with their native session lock and socket. The adapter preserves the existing behavior: mark the peer not ready and shutdown the current data socket while holding the same lock.
- The public `xfer_kick()` API remains for existing receiver/network callers, but delegates to the same platform context adapter so there is only one implementation of the reconnect side effect per platform.
- Extended the production-linked Sender Context test to exercise the reconnect callback and reject a context missing it. Source-contract regression checks ensure the shared engine uses the job context and the public wrapper delegates to the adapter.
- **Validation: GitHub Actions Run #1429 (ID `42812591315`, tested commit `1d917dcd3711705287376cb3f9eac337ddee3cac`) passed on Windows MSVC and macOS Apple Clang.** Both app builds, portable Core/regression suites, transfer protocol tests, TCP loopback tests, native connection/reconnect integration tests, and packaging passed. Release publication was skipped for the PR.
- Next: move ACK coordinator ownership and all ACK register/deliver/wait/wake operations behind the same explicit platform Sender Context, then extract worker scheduling/retry/terminal ordering. The ACK adapter must preserve exact `(batch,id)` identity, early-ACK/deadline behavior, wake-up semantics, and destroy-only-after-join lifetime.


## 23. ACK coordinator ownership through Sender Context (2026-11-10)

- Removed the file-static `s_ack` pointer. The opaque ACK coordinator is now owned by the platform Sender context, with explicit create/destroy/wake lifecycle helpers. Destruction remains after sender threads have joined; cancellation, shutdown and receiver ABORT all wake through the context adapter.
- Added mandatory context callbacks for ACK registration, send timestamp, deadline, wait/take, per-entry and per-batch unregister, inbound delivery, and wake. File and DONE commit callbacks now forward through the owning job's captured `sender_context`, preserving the exact `(batch,id)` key.
- Kept platform timing behavior intact: Windows waits until the remaining deadline (bounded by the platform wait maximum); macOS retains its 100 ms polling slice. ACK arrival still uses the same monotonic timestamp source and coordinator semantics.
- Removed the Receiver ABORT handler's direct reference to the old ACK global. It now invokes the platform context wake adapter. Removed the obsolete global peer-abort fallback from the file engine callback; it now queries only the owning job context.
- Extended the production-linked Sender Context test to exercise all ACK operations and to reject contexts missing any ACK callback. Regression checks prohibit `s_ack` in Sender/Receiver source paths and verify the ACK wait/deliver calls cross the context boundary.
- **Validation: GitHub Actions Run #1433 (ID `42814619801`, tested commit `f8034bf162710058ed528e75c9cd441a31fc604b`) passed on Windows MSVC and macOS Apple Clang.** Both app builds, portable Core/regression suites, transfer protocol tests, TCP loopback tests, native role/reconnect integration tests, and packaging passed. Release publication was skipped for the PR.
- Next: extract the thin platform ACK adapter functions from `xfer_sender_protocol.inc` into independently compiled platform adapter units, then finish Sender worker scheduling, queue ownership, retry and terminal lifecycle boundaries. Keep the coordinator opaque to shared Core.


## 24. Independently compiled Sender ACK adapter (2026-11-10)

- Added the shared opaque API in src/core/xfer_sender_ack_adapter.h/.c. The adapter owns the platform ACK coordinator and receives the same injected monotonic clock used by Sender deadlines.
- Split hop_xfer_sender_context.ack_user from the general platform user pointer. ACK register/mark/deadline/wait/unregister/deliver/wake callbacks use the dedicated adapter owner; socket/session, cancellation, status and Task callbacks continue using the general context.
- The platform coordinator remains opaque behind xfer_ack_coordinator.h; shared Sender protocol code no longer includes or calls coordinator implementation details.
- Transfer service init creates the adapter after native synchronization setup. Failure/shutdown destroys it only after sender/connection threads have joined. Receiver delivery and shutdown/ABORT wake continue through explicit context callbacks.
- Added production-linked tests for injected ACK timestamps, ACK-before-deadline-installation, receive-time deadline expiry, duplicate/stale identity, batch cleanup and adapter lifecycle. Updated both Core test scripts and the Windows HA fixture link.
- Validation: GitHub Actions Run #1437 (ID 42816625819, commit ef0df8aefad3a67a2d07b8ea4d560faebe8bf145) passed on Windows MSVC and macOS Apple Clang. App builds, Core/regression suites, transfer protocol tests, TCP loopback, native role/reconnect tests and packaging passed. Release publication was skipped for the PR.

## 25. Shared Sender post-run lifecycle and retry flow (2026-11-10)

- Added src/core/xfer_sender_worker_flow.[ch] to own the common post-run policy for both platforms: cancellation arbitration, retry planning, Task requeue transition, FIFO-front retry admission, ABORT/DONE dispatch, terminal Task/status mapping and final job disposal.
- Windows/macOS worker adapters now provide native effects only: active-job/peer-cancel snapshots under the existing lock, socket ABORT/DONE operations, queue lock/requeue/signal, retry-state write-back, Task/status callbacks and job memory release.
- Preserved ordering: non-DONE paths snapshot peer cancellation and clear the active marker atomically before cancellation checks; DONE paths retain the marker through completion gate and DONE_ACK, then atomically re-read peer cancellation and clear it. Retry state is written before Task returns to QUEUED; queue admission follows that transition.
- Preserved reconnect policy inputs, retry delays, retry budget, next_i checkpoint, FIFO-front requeue and platform-specific ABORT behavior. Normal terminal paths retain Task finish before status end; retry-policy/state/queue admission failures retain their original status-end-before-Task-finish order. Both dispose the job exactly once; successful retry does not dispose it.
- Added production-linked Core tests for scheduled/exhausted retry, cancellation override, completion-gate cancellation, DONE uncertainty, receiver ABORT mapping, requeue failure and invalid contracts. Wired them into Windows/macOS Core tests and the Windows HA link target.
- **Validation: GitHub Actions Run #1591 (ID `43019588698`, tested commit `58b99d969f90fb3e8756a47b5ec6b62113217fcd`) passed on Windows MSVC and macOS Apple Clang.** Both app builds, Portable Core and production regression suites, file-transfer protocol tests, TCP loopback, native control/data role + reconnect integration tests, and platform packaging passed. PR release publication was skipped. Physical two-device reconnect, concurrent cancel/ABORT and long-duration soak remain unverified.
- Next: move queue ownership/wait scheduling behind an explicit Sender Worker adapter. Do not remove platform worker fragments until queue lock ordering, offline expiry, shutdown drain, stuck-thread timeout and concurrent submit/cancel/retry are covered by production-linked tests.

## 26. Sender Shutdown ownership and Reservation release (2026-11-10)

- Paused further Sender module extraction. PR #11 remains open; no merge is part of this shutdown slice.
- Added explicit Job ownership states: SUBMITTER -> QUEUE -> WORKER, with retry returning WORKER -> QUEUE. Once workers have joined, queued/deferred Jobs transfer to SHUTDOWN. Only the recorded owner may release a Job; successful disposal marks it RELEASED. A failed Task dispose retains the Job and its Task handle on the shutdown-owned retry list instead of freeing memory or losing the Reservation handle.
- Submission now holds a shared lifecycle lock from before scanning through queue admission. Shutdown takes the exclusive side, closes admission, and waits for every accepted submitter to finish before touching queue or Task state.
- A worker that sees shutdown does not requeue a disconnected Job. If stop wins between retry planning and queue admission, the queue adapter rejects the requeue and shared worker flow resolves the Job as Interrupted. A worker keeps ownership until its Job is terminal and disposed; Shutdown drains only after Join succeeds.
- Windows Join timeout now retains the native thread handle. A later Shutdown call can retry Join; shared locks, ACK state, queued Jobs and Reservations are not destroyed while a worker may still own a Job. macOS follows the same ownership rule and leaves queued Jobs for the post-Join shutdown drain.
- Shutdown drains queued/deferred Jobs through the single Job disposer, terminalizes each nonterminal Task once (preserving a prior user cancellation), and lets Task Manager return its Reservation. Receiver reset and writer-queue stop follow; writer Join failure also preserves the queue and shared state for retry.
- Before closing Task history or destroying native synchronization, Shutdown reconciles the Task/Reservation ledger and requires zero live Tasks, Job/entry reservations, pending/image bytes, RX queue slots/bytes and image-temp bytes, plus zero Reservation accounting errors. A failed audit retains shared state rather than reporting a clean shutdown.
- Both platforms retain clipboard image-work Task owners when temporary-byte release or Task disposal fails. Clipboard shutdown retries those owners and reports success only after they are disposed; application shutdown does not proceed to xfer teardown while one remains.
- macOS application exit now waits for clipboard image workers, stops and joins the detached control reader/sender workers, and only then calls xfer_shutdown. This prevents late reconnect kicks from touching destroyed transfer locks.
- Production-linked tests cover shutdown before retry, stop winning the requeue race, real Task terminal/dispose and Reservation return, plus retryable Windows Join timeout. Source-contract tests enforce owner transitions and teardown ordering.
- **Validation: GitHub Actions Run #1591 (ID `43019588698`, tested commit `58b99d969f90fb3e8756a47b5ec6b62113217fcd`) passed on Windows MSVC and macOS Apple Clang.** Both app builds, Portable Core and production regression suites, file-transfer protocol tests, TCP loopback, native control/data role + reconnect integration tests, and platform packaging passed. PR release publication was skipped. The exact-head Task/Reservation shutdown and accounting tests passed. An intentionally stuck production worker, physical two-device shutdown and long-duration soak remain unverified. PR #11 remains open; no merge was performed.


## 27. Sender terminal status publication after disposal (2026-11-10)

- Windows and macOS now retain terminal status details on the Job instead of publishing them from the worker-flow callback as soon as Task reaches a terminal state.
- The single Job disposer publishes the pending status only after Task disposal succeeds and the Task handle is cleared. A failed disposal therefore keeps both the Job owner and its not-yet-published terminal status on the Shutdown retry list.
- Queued-expiry and invalid-worker-context paths now use the same deferred status path; no special path can announce completion before the Task/Reservation owner has been released.
- Regression checks require status publication to occur after `sender_task_dispose` and prohibit direct publication from the worker status callback.
- **Validation: GitHub Actions Run #1510 (ID `42848693214`, tested commit `a043fe8dd1c56f8141fcb61042aa5d7246ee2991`) passed on Windows MSVC and macOS Apple Clang.**
- Next: extract the common Sender worker scheduling loop (wait, queue selection, cancellation/deadline expiry and Job ownership claim) behind an explicit Core scheduler contract. Keep native event/condition waits and platform locks as adapters; preserve the 200 ms reconnect/cancel polling and the Windows/macOS lock-order constraints.


## 28. Shared Sender worker scheduler (2026-11-10)

- Added `src/core/xfer_sender_scheduler.[ch]` to own the common wait/select loop: cancellation scan, FIFO-head deadline/retry eligibility, offline expiry, stop checks, and the single transition from queue ownership to worker ownership.
- Windows and macOS now provide only native synchronization adapters. Windows releases the queue critical section around its persistent stop/queue event wait and reacquires it before the next scheduler pass. macOS uses `pthread_cond_timedwait` with the queue mutex held, preserving atomic unlock/wait/relock and avoiding lost queue wakeups.
- macOS preserves the lock order by snapshotting peer readiness under the session lock before acquiring the queue lock. Windows captures peer socket/session/epoch while holding its existing queue/state critical section. Both retain the 200 ms reconnect/cancel poll.
- Added production-linked scheduler tests for immediate ready selection, cancelled middle-node removal while offline, peer-state refresh after wait, offline deadline expiry, FIFO head priority, stop while waiting, lock release on wait failure, and invalid adapters. Wired tests into Windows and macOS Core suites; app build scripts pick up the new Core file automatically.
- Source-contract regressions ensure platform worker loops no longer perform queue take policy themselves and that native wait callbacks preserve lock semantics.
- **Validation: GitHub Actions Run #1522 (ID `42854615651`, tested commit `7c0c2f48e486b470e7d099b0120d455a310295b5`) passed Windows MSVC, macOS Apple Clang, portable Core tests and the Windows HA fixture.**
- Next: extract active-job/peer-ABORT arbitration and the platform worker runtime context into an explicit adapter, then remove remaining duplicated sender worker orchestration from the platform include fragments. Keep all native socket/session and thread handles in platform-owned code.


## 29. Shared active-job and peer-ABORT arbitration (2026-11-10)

- Added `src/core/xfer_sender_active_job.[ch]` to own the shared rules for active batch/task identity, cancel-target selection, peer ABORT recording, peer-cancel classification, and exact active-marker clearing.
- Windows and macOS no longer maintain separate `cur_batch`, `cur_task_id`, `abort_batch` and `abort_reason` fields in their transfer state. Both platform adapters call the same Core policy while holding their native queue/state lock.
- Receiver ABORT handling, new-session reset, UI cancellation targeting, scheduler Job claim and worker completion now use the shared active-job API. The sender platform context points to the shared ABORT batch field for existing engine cancellation checks.
- Worker disposal and early-stop/task-state-failure paths explicitly clear the matching active marker. A stale callback cannot clear a different Job because the Core helper requires both batch and Task ID to match.
- Added portable tests for active-job start, cancellation target, batch-scoped peer ABORT, exact Task-ID matching, non-cancel ABORT, session reset and invalid identities. Wired them into Windows/macOS Core tests and the Windows HA fixture.
- **Validation: GitHub Actions Run #1522 (ID `42854615651`, tested commit `7c0c2f48e486b470e7d099b0120d455a310295b5`) passed Windows MSVC, macOS Apple Clang, portable Core tests and the Windows HA fixture.**
- Next: consolidate the remaining platform worker runtime callbacks and extract the remaining sender protocol compatibility include fragments into independent compile units. Do not move socket handles, native synchronization or UI callbacks into Core.


## 30. Shared Sender context callback adapter (2026-11-10)

- Added `src/core/xfer_sender_context_adapter.[ch]` to centralize Task cancellation checks, batch/task cancellation composition, session binding, Task state/completion/finish/disposal forwarding, status telemetry forwarding and formatted logging.
- Removed the duplicated 120-line context-wrapper implementations from both platform Sender protocol fragments. Protocol and worker code now call the same Core adapter functions; socket, session, lock and status-storage implementations remain platform callbacks.
- Optional telemetry/log callbacks safely no-op when absent. Task lifecycle callbacks fail closed when required callbacks are absent; a Task handle is considered disposed only after the platform callback clears both `initialized` and `id`.
- Added portable tests for callback argument forwarding, cancellation composition, exact session bytes, Task lifecycle, status telemetry and optional observability. Wired the adapter into Windows/macOS Core tests, session-transport fixtures and the Windows HA fixture.
- **Validation: GitHub Actions Run #1591 (ID `43019588698`, tested commit `58b99d969f90fb3e8756a47b5ec6b62113217fcd`) passed on Windows MSVC and macOS Apple Clang.** Both app builds, Portable Core and production regression suites, file-transfer protocol tests, TCP loopback, native control/data role + reconnect integration tests, and platform packaging passed. PR release publication was skipped.
- Next: finish isolating the remaining DONE/DONE_ACK transport adapter and remove obsolete warning-only platform wrappers. Keep all socket/session state and native synchronization behind explicit context callbacks.


## 31. Shared Sender ACK wait and invalid-view cleanup (2026-11-11)

- Added `src/core/xfer_sender_ack_wait.[ch]` as the single ACK wait state machine used by per-file ACK and batch DONE_ACK on both platforms. It owns deadline checks, ACK delivery/receive-time expiry, cancellation, peer ABORT, connection/epoch validation, unregister-on-exit and RTT conversion.
- The adapter takes a polling policy rather than embedding OS waits: Windows retains a remaining-deadline wait bounded below `MAXDWORD`; macOS retains its 100 ms polling slice. Native ACK coordinator locks and event/condition waits remain behind the existing context adapter.
- Removed the duplicated `ack_wait` implementations from Windows and macOS. Both file ACK and DONE_ACK paths now call the shared helper.
- Fixed the Worker invalid-View path: if the partial view exposes a Task ID, the Worker clears only the matching Active Job marker before releasing the worker-owned Job. A regression test injects an invalid view and verifies marker cleanup and exactly one release.
- Removed the session-transport fixture's stale extraction/call of the deleted `job_bind_session` helper. Session rebinding and checkpoint reset are now exercised by the production-linked shared Worker tests; the fixture continues to test the real platform session snapshot and wire path.
- Added standalone ACK-wait tests for deadline expiry, fixed/dynamic wait slices, ACK RTT, late ACK, cancellation, peer ABORT, dead connection and invalid contracts.
- **Validation: GitHub Actions Run #1591 (ID `43019588698`, tested commit `58b99d969f90fb3e8756a47b5ec6b62113217fcd`) passed on Windows MSVC and macOS Apple Clang.** Both app builds, Portable Core and production regression suites, file-transfer protocol tests, TCP loopback, native control/data role + reconnect integration tests, and platform packaging passed. PR release publication was skipped.

## 32. Shared Sender chunk transport and engine adapter (2026-11-11)

- Added `src/core/xfer_sender_chunk.[ch]` to own FILE_CHUNK pacing, token-bucket admission, protocol header/prefix construction, scatter/gather frame parts, serialized frame write, send-time feedback cadence and chunk-stage telemetry. Platform code now supplies only the live rate limiter, socket handle and existing feedback callback.
- Added `src/core/xfer_sender_engine_adapter.[ch]` to build the file/image engine callback tables and centralize context forwarding, ACK result mapping, retry delay, wait/reconnect, status/logging, image begin/end payloads and file-entry adaptation.
- Windows and macOS retain only platform-owned entry accessors, native file open/read/close, socket send, rate feedback and CPU-yield callbacks. Their `run_job` functions now provide a compact adapter contract instead of separately assembling the file and image state machines.
- Added production-linked chunk tests for owned/external data buffers, exact frame decoding, token-bucket waiting, burst rejection, stale session and telemetry; added adapter tests for file and image jobs.
- Wired the new Core tests into macOS and Windows Core test scripts and the Windows HA link target. App build scripts already include all Core C files automatically.
- **Validation: GitHub Actions Run #1591 (ID `43019588698`, tested commit `58b99d969f90fb3e8756a47b5ec6b62113217fcd`) passed on Windows MSVC and macOS Apple Clang.** Both app builds, Portable Core and production regression suites, file-transfer protocol tests, TCP loopback, native control/data role + reconnect integration tests, and platform packaging passed. PR release publication was skipped.


## 33. DONE/DONE_ACK and reconnect/ACK platform adapter isolation (2026-11-11)

Implementation slice in progress:

- Added independently compiled Windows/macOS xfer_sender_commit_adapter.c modules. They own the native current-connection/session snapshot, DONE frame send, ACK registration/deadline/unregister forwarding, shared ACK-wait invocation, and platform result mapping.
- The platform Sender orchestration now delegates completion through sender_commit_adapter_run; shared DONE retry/commit policy remains in src/core/xfer_sender_engine.c::hop_xfer_sender_run_commit.
- DONE_ACK continues to use the single shared hop_xfer_sender_ack_wait state machine. The platform adapters retain only their native poll policy (Windows 0 ms event wait; macOS 100 ms bounded polling).
- Added independently compiled Windows/macOS xfer_sender_connection_adapter.c modules for reconnect-kick. The callback still invalidates peer readiness and shuts down the current socket under the existing native state/session lock; only its ownership/location changed.
- Added independently compiled Windows/macOS xfer_sender_ack_platform_adapter.c modules for ACK adapter creation/destruction, wake-up, and inbound delivery. Native event/condition synchronization remains in the opaque xfer_ack_coordinator modules.
- Removed obsolete ACK forwarding wrappers and unused legacy DONE send wrappers from both xfer_sender.c files. Removed duplicate ACK result macro blocks and duplicate comments.
- Updated Windows HA source linkage and source-contract regression assertions to follow the new platform adapter boundaries.
- App build scripts discover nested platform .c files recursively; the new adapters are therefore included by both native app builds.

Validation status at this documentation update:
- Static repository checks confirm each platform Sender source has exactly one definition of each ACK result macro, no legacy ACK forwarding wrappers, no legacy send_done wrapper, and delegates DONE through sender_commit_adapter_run.
- GitHub Actions Run #1603 (ID 43029913816) was triggered for this implementation and is in progress. No build/test pass is claimed until it completes.
- Real physical two-device disconnect/cancel/reconnect and long-duration transfer testing remain unperformed and are still mandatory final acceptance gates.

No protocol bytes, DONE payload layout, retry budget, ACK timeout, ACK slot identity, session/epoch check, or Worker terminal outcome policy is intentionally changed in this extraction.


CI follow-up:
- Run #1614 exposed a Windows return-code macro accidentally removed with the obsolete DONE wrapper block; the code now returns the shared HOP_XFER_SENDER_JOB_NET_DOWN enum directly.
- Run #1619 confirmed both native application builds compile, including the new platform adapters. Its portable regression phase then exposed stale source-extraction fixtures that still expected the deleted send_done wrapper and looked for moved commit callbacks in xfer_sender.c. The fixture now builds its DONE test frame locally and extracts the commit callbacks from xfer_sender_commit_adapter.c.
- A fresh full Windows/macOS Actions run is required for the updated head; neither failed run is counted as acceptance.


## 34. Worker runtime platform Adapter extraction (2026-11-11)

- Moved the platform Worker runtime integration out of `xfer_sender.c` into independently compiled `xfer_sender_worker_adapter.c` on Windows and macOS.
- The new modules own native scheduler locking/waiting/claim callbacks, Job View projection, connection/status snapshots, active-job cancellation handoff, platform ABORT send adaptation, and the native `send_thread` entrypoint.
- Shared queue selection, deadline/reconnect scheduling, Worker Job lifecycle flow, and terminal policy remain in `src/core/xfer_sender_scheduler.c`, `xfer_sender_worker.c`, and `xfer_sender_worker_flow.c` respectively. No second retry or terminal state machine was introduced.
- `xfer_sender.c` retains protocol/job execution and Job resource ownership; only the small set of functions required by the platform Worker adapter are exposed through `xfer_sender_internal.h`. Platform-specific lock/event/condition/QoS behavior remains native.
- Windows HA source linkage and source-contract regression checks now include the new Worker adapter and assert that `send_thread` no longer lives in `xfer_sender.c`.
- **Final CI validation: GitHub Actions Run #1635 (ID `43037203168`, tested head `07ecf5fbea98477ad67efd2d2a0f58900e6d13fb`) passed on Windows MSVC and macOS Apple Clang**: app build, portable Core/regression, file-transfer protocol, TCP loopback, native control/data role/reconnect (Windows), control/listener loopback (macOS), updater/bundle checks, and both package uploads. Release publication was skipped as expected for a PR.
- Real two-device disconnect/cancel/reconnect and long-duration transfer validation remains outstanding.


## 35. Real-device acceptance checklist (not yet executed)

The following checks require two physical HopDesk devices and must not be inferred from CI loopback/protocol-peer tests:

1. Cross-platform baseline in both directions: Windows sender → macOS receiver, then macOS sender → Windows receiver. Transfer a mixed batch of small files, large files, nested directories, and an image batch; compare source/destination sizes and SHA-256 hashes.
2. Mid-stream disconnect: interrupt the active data channel during a large file. Confirm the Task remains recoverable, reconnect restores the same authenticated peer/session policy, and transfer resumes from the last receiver-confirmed checkpoint without silently skipping or duplicating bytes.
3. Cancel while disconnected and during reconnect: cancel the active batch while the peer is unavailable, then reconnect. Confirm one terminal Cancelled outcome, no stale retry/requeue, no unexpected DONE, and no leftover partial output beyond the documented cleanup policy.
4. Repeated reconnect stress: repeat disconnect/reconnect during active transfer and while jobs are queued; include a delayed/late ACK after connection replacement. Confirm old epoch/session ACKs cannot complete the new connection's work and each Task reaches one terminal outcome.
5. Sustained transfer: run a long mixed-file transfer under normal use and record duration, total bytes, throughput, CPU/memory trend, and final SHA-256 results. Observe queue/ACK-window bounds and confirm no growing reservation/task/resource ledger.
6. After each scenario, inspect sender/receiver status and Task history; confirm no stuck Active/Queued Task, no duplicate history entry, no leaked staging directory, and no unexplained pending Reservation.

Record device OS/build SHA, direction, network conditions, file manifest/hashes, interruption point, reconnect count, duration, and observed resource metrics. Any failed scenario blocks Sender refactor acceptance. CI Run #1635 validates automated production builds and integration fixtures only; it is not evidence that these physical-device checks have passed.
