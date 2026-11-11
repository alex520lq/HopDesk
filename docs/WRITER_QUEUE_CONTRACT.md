# Receive Writer Queue Contract

Status: design contract for architecture refactor phase 2. This document does not claim that the worker has already been extracted.

## Responsibilities

The Writer Queue owns only:
- A bounded FIFO of OPEN / WRITE / CLOSE commands.
- The RX slot + byte accounting and the payload buffer pool.
- Its worker thread, wakeups, drain condition, and stop/join lifecycle.
- The single writer-IO instance, accessed only by the worker.

The Receiver owns:
- Protocol validation, file/batch state, ACK decisions, and terminal Task state.
- The staging path and its lifetime. It must not release a path until the matching OPEN/WRITE/CLOSE commands have drained.
- The decision to enqueue CLOSE before CRC verification or cleanup.

The Task/Budget domain owns:
- RX byte reservation against the receiving Task.
- Exactly-once release of the same reservation after a WRITE finishes or enqueue fails.
- Task identity and terminal state. The writer must not finish/cancel a Task.

The platform adapter owns:
- Native mutex/condition/thread primitives and native file IO.
- Platform path representation and error formatting.
- Thread priority/QoS configuration.

## Command and ownership rules

| Command | Queue slot | Queue bytes | Payload ownership | Completion |
|---|---:|---:|---|---|
| OPEN | 1 | 0 | Queue copies the path into command-owned storage | Worker opens/replaces its file handle |
| WRITE | 1 | len | Queue owns one pool slot and one Task RX reservation until completion | Worker performs offset write, then releases Task reservation and pool slot |
| CLOSE | 1 | 0 | No payload | Worker closes its file handle |

Rules:
1. Every accepted command consumes exactly one slot. WRITE additionally consumes exactly len bytes.
2. A WRITE must reserve queue accounting and Task RX budget before allocating/copying payload bytes.
3. If any later enqueue step fails, rollback queue reservation, Task reservation, and pool slot exactly once.
4. Ownership transfers to the queue only after the command is committed to the ring.
5. After dequeue, the worker owns the command until completion; dequeue does not release slot/byte accounting.
6. Completion releases the Task reservation (WRITE only), pool slot (WRITE only), and queue accounting exactly once, then wakes blocked producers/drainers.
7. drain completes only when queued, busy, and reserved counts are all zero.
8. CLOSE is ordered behind prior writes. Receiver must drain after CLOSE before verifying, deleting, or reusing the file.
9. No caller may mutate or free a WRITE buffer after commit.
10. Queue shutdown rejects new producers, wakes all blocked producers and the worker, drains accepted commands, closes the file, joins the worker, and only then destroys synchronization primitives and pool storage.

## Lifecycle contract

Required states:
- UNINITIALIZED: no thread, no live synchronization objects.
- RUNNING: producers may enqueue; worker consumes.
- STOPPING: no new enqueue accepted; blocked producers wake and fail; worker drains accepted commands.
- STOPPED: worker joined; queue empty; no busy/reserved slot; file closed; pool may be disposed.
- FAILED_JOIN: worker could not be joined within the platform policy. The queue remains allocated and permanently unavailable for restart. The thread handle/identity and synchronization objects must remain valid until a later successful join or process teardown. Never zero/reinitialize shared state while a worker may still access it.

Transitions:
- UNINITIALIZED -> RUNNING only after every mutex/condition/pool/IO component is initialized and thread creation succeeds.
- Partial initialization failure unwinds only successfully initialized components in reverse order.
- RUNNING -> STOPPING under the queue lock, with a broadcast to both worker and producers.
- STOPPING -> STOPPED only after join confirms worker exit.
- STOPPING -> FAILED_JOIN on timeout; retain the complete queue object and worker identity. Do not clear the thread field to make a later start appear possible.

## Required observable invariants

At every queue-lock boundary:
- queued_slots + busy_slots + reserved_slots <= capacity_slots.
- queued_bytes <= capacity_bytes.
- A busy WRITE remains charged to both slot and byte budgets.
- drained == (queued_slots == 0 && busy_slots == 0 && reserved_slots == 0).
- Pool slots in use correspond one-to-one with accepted or currently-copying WRITE commands.
- Task RX budget held by the writer corresponds to the same payload bytes and is released once.
- No writer callback accesses Receiver-owned mutable state except through an explicitly documented callback/context whose lifetime outlives the worker.

## Cross-platform parity

Windows and macOS may retain different synchronization and path APIs, but must share the same command semantics, capacity rules, ownership transfers, error result semantics, and lifecycle state transitions. A shared contract test suite must exercise both implementations.

## Extraction sequence

1. Introduce the command/lifecycle contract and test it independently of the Receiver.
2. Create an opaque platform Writer Queue object with start, enqueue_open, enqueue_write, enqueue_close, drain, snapshot, error, and stop operations.
3. Inject callbacks for Task budget reserve/release, watchdog begin/end, monotonic timing, write-duration reporting, and logging. Callback context must remain valid until successful worker join.
4. Move the Windows worker and queue into a separately compiled production module; keep the Receiver call sites behind a compatibility wrapper during the transition.
5. Run Windows native CI and targeted concurrency/failure tests.
6. Move the macOS worker under the same contract; run macOS native CI and identical semantic tests.
7. Remove the old writer implementation from both receiver include files only after both adapters pass.
8. Extract Receiver Protocol / Batch Lifecycle separately. Do not mix this with the Writer Queue extraction.

## Required tests before declaring the slice complete

- Capacity full / wake-on-completion and blocked producer wake-on-stop.
- Slot and byte limits with both queued and currently-writing payloads.
- Reservation rollback after Task-budget rejection, pool allocation failure, and stop racing with payload copy.
- OPEN/WRITE/CLOSE FIFO ordering and repeated OPEN/CLOSE behavior.
- Disk full, permission denied, partial/failed write, and error persistence until explicitly reset.
- Drain while a producer reservation is in progress.
- Stop with queued work; stop with blocked producers; stop racing with a producer copying data.
- Worker join success and injected join timeout; prove no restart or object destruction while the worker may still be live.
- Exactly-once release for Task budget, pool slot, and queue accounting on every failure path.
- Sanitizer/concurrency stress tests where supported.


## 2026-11-08 worker join parity hardening

The platform worker adapters now share the same bounded join behavior:
- Windows waits at most 3 seconds with `WaitForSingleObject`.
- macOS waits at most 3 seconds for an explicit worker-exit signal using a condition variable, then calls `pthread_join` only after exit is confirmed.
- A timeout leaves the worker in RUNNING state with its thread identity, callback context, task storage, and synchronization objects intact. Callers may retry join; they must not restart or destroy the owner while the worker may still execute.
- Successful join is the only path that releases worker task storage and the macOS exit synchronization primitives.
- `tests/writer_worker_tests.c` now exercises a deliberate >3-second callback, verifies the first join times out without clearing live state, retries join successfully, and verifies restart afterward on both platforms.

This closes a platform lifecycle mismatch in the worker adapter; it does not yet extract the complete Writer Queue from the Receiver include.


## 2026-11-08 Windows production extraction status

The Windows implementation now exists as `src/win/xfer_writer_queue.[ch]` with an opaque queue handle. The Receiver passes Task-budget reserve/release, watchdog, monotonic clock, write-time reporting, and logging through callbacks. The queue owns ring commands, slot/byte accounting, payload pool, worker, IO handle, and stop/join lifecycle.

The Receiver compatibility layer exposes only its existing local operations and no longer reaches into queue internals. `tests/writer_queue_tests.c` links the production queue/worker/IO/Core modules and checks disk bytes plus budget rollback/release.

This is one platform slice only. macOS still uses the previous queue implementation in `xfer_receive.inc`; API and semantic parity plus macOS native tests are required before removing both legacy implementations or declaring phase 2 complete.


## 2026-11-08 Cross-platform production extraction status

Both platform adapters now implement the opaque Writer Queue API:
- Windows: `src/win/xfer_writer_queue.[ch]`
- macOS: `src/mac/xfer_writer_queue.[ch]`

Both Queues own their ring commands, slot/byte accounting, reusable payload pool, worker, writer IO, and stop/join lifecycle. Receiver code supplies Task RX budget reserve/release, watchdog, monotonic clock, write-time reporting, and error logging through callbacks. The platform-specific path representation and synchronization primitives remain local to each adapter.

Production-linked tests exist for both platforms and verify FIFO OPEN/WRITE/CLOSE, actual disk bytes, budget admission rollback/release, drain, and stop. Run #609 (commit `33ec53b9d49c4d2dcb3c0bbe52119c9f3a34e574`) passed the full Windows and macOS CI pipelines, including these tests.

This closes the Writer Queue extraction slice on both platforms, not the whole receive-side refactor. Receiver Protocol / Batch Lifecycle remains in the legacy include and must be extracted separately. Additional stop-vs-producer race, allocation-failure, slow-disk, persistent-error, and sanitizer stress coverage remains required before phase 2 acceptance.


## 2026-11-08 Receiver DONE receipt extraction

Session-bound DONE idempotency is now a shared Core module in `src/core/xfer_rx_receipt.[ch]`. Windows and macOS use the same lookup, exact-payload comparison, clear-on-session-change, and record operations. The module does not own file IO, clipboard effects, or Task terminal state; those remain Receiver/Application responsibilities.

A standalone production-linked test covers session and batch isolation, exact replay, conflicting payload rejection, and invalidation. The ABORT and session-transport regression fixtures link the real Core module rather than recreating its logic. Latest validation run #621 is pending; do not treat the extraction as CI-accepted until both native jobs complete.


## 2026-11-08 Receiver batch transition rules

The cross-platform Receiver now calls shared pure policy functions in `src/core/xfer_rx_batch_rules.[ch]`:
- BEGIN precedence: invalid -> already committed receipt ignored -> tombstoned batch rejected with its stored reason -> same active batch resumed -> new batch started.
- ABORT/termination cleanup is allowed only when the event batch matches the currently active batch.
- DONE outcomes are classified as stale, cancellation-won, complete, partial, or image-failed. The Task manager remains the atomic authority for claiming completion against cancellation; the Core policy receives that result and does not duplicate task ownership.
- Platform adapters still execute all side effects: task state transitions, filesystem cleanup, clipboard publication, status/history updates, and wire ACKs.
- Production-linked unit and regression fixtures cover both platforms. Run #638 (ID `42481821345`, commit `d2800e9bd88d853cc20201d23146f224635a4c0a`) passed the complete Windows and macOS CI pipelines.

This is a shared transition-policy extraction, not completion of the full Receiver protocol/batch lifecycle refactor. Remaining acceptance includes protocol message dispatch extraction, batch/file state ownership, and race/failure injection around cancellation, finish, late frames, and reconnect.


## 2026-11-08 Receiver entry reservation table

- Added `src/core/xfer_rx_entry_reservations.[ch]`; the shared table owns per-batch 1-based entry ID -> declared-size identity and the reservation marker.
- Windows/macOS Receiver state no longer embeds duplicate `reserved_entries[]` and `reserved_entry_sizes[]` arrays.
- New IDs call an injected Task-budget reservation callback before committing the table entry. Duplicate ID+size is idempotent; conflicting size, invalid ID, and budget rejection are distinct results.
- Task Manager remains the budget owner. The table is only the per-batch identity/once-only guard and resets with batch state.
- Production tests and both platforms' regression fixtures link the Core module. Run #644 (ID `42482474497`, commit `f641997d5d70fb46037bcf8e83934ba43c96228b`) passed complete Windows and macOS CI.


## 2026-11-08 Receiver current-file state

- Added `src/core/xfer_rx_file_state.[ch]` to own current entry ID, failure ACK, declared size, contiguous received offset, and file CRC fold.
- Windows/macOS Receiver no longer duplicate `cur_id`, `cur_fail`, `cur_size`, `cur_received`, or `fcrc`; each embeds the shared Core state.
- Chunk validation rejects gaps/overlaps/out-of-range lengths using overflow-safe subtraction. State advances only after successful writer-queue admission, preserving the existing no-advance-on-queue-rejection behavior.
- The module's standalone tests link the production CRC-fold implementation from `xfer_validate.c`. Run #652 (ID `42483467036`, commit `a6f39aa54ed5d6029658eaa77873fdbbcda91865`) passed complete Windows/macOS CI.


## 2026-11-08 FILE_END ACK policy

The shared `hop_xfer_rx_file_state_ack` now owns the FILE_END ACK precedence used by both Receivers:
1. Return a previously recorded per-entry failure (BADPATH/CRC/DISK) unchanged.
2. Reject Writer Queue IO failure as DISK.
3. Reject peer-reported FILE_END status as DISK.
4. Reject incomplete received byte count as CRC.
5. Reject file CRC mismatch as CRC.
6. Otherwise return ACK_OK.

The platform adapters still drain/close the file, log the specific diagnostic, remove failed files, register verified top-level clipboard paths, update status, and send FILE_ACK. Run #658 (ID `42483891419`, commit `c7f072d8527461d73a7d4c0907b7d39dce330565`) passed full Windows/macOS CI.
