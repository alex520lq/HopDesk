# HopDesk feature test matrix

This matrix separates portable automated checks from cases requiring physical
Windows/macOS machines, Accessibility permission, real displays, or two devices.
A green compile is not evidence that those manual/device cases passed.

## A. Portable core and data-frame validation (automated on Windows + macOS)

| ID | Feature | Cases |
|---|---|---|
| CORE-001 | Endian/frame encoding | BE16/32/64, type/length/payload, empty payload |
| CORE-002 | Stream decoder | Every split point, byte-at-a-time, coalesced frames |
| CORE-003 | Decoder recovery | Reset mid-frame; oversized frame followed by valid frame |
| CORE-004 | CRC32 | IEEE vector, empty, incremental, corrupted payload |
| CORE-005 | Config | Defaults, parsing, whitespace, legacy aliases, hex values |
| KEY-001 | Key map | Letters, digits, F1-F20, numpad, navigation, modifiers, OEM |
| XFER-001 | Data HELLO | Correct/empty/wrong token, wrong version, truncation, trailing bytes |
| XFER-002 | Relative paths | POSIX absolute, drive/UNC, traversal, dot segments, separators |
| XFER-003 | Image bounds | Zero/negative, stride, side cap, 256 MiB cap, boundary |
| XFER-004 | Chunk bounds | Exact end, overflow, beyond file/image cap, zero, malformed |
| XFER-005 | Retry policy | File/job backoff, retry limits and reconnect budget |
| XFER-006 | FILE_BEGIN parser | Valid fields, zero/oversized name, short payload, truncated path, trailing bytes |
| XFER-007 | IMAGE_BEGIN parser | Exact 20-byte payload, truncated and trailing bytes |
| XFER-008 | Clipboard origin | Apply remote origin; suppress local-origin echo on both endpoints |
| XFER-009 | File CRC fold | Chunk offset + CRC fold, ordering/position sensitivity, sender/receiver shared helper |
| XFER-010 | File wire round-trip | Empty, 1-byte, chunk boundaries, multi-chunk, CRC corruption, offset tamper, reorder, traversal path, empty directory |
| XFER-011 | Image wire round-trip | Exact image bytes, truncated image, reordered chunks, corrupt CRC, beyond-declared-size chunk |
| CLIP-012 | Lazy file clipboard codecs | Offer/clear/request/failed encode-decode, invalid names, request ID namespace, malformed lengths |

## B. Network/data-channel integration (next automation layer)

| ID | Feature | Cases |
|---|---|---|
| NET-001 | TCP framing | Partial reads/writes, split header/body, coalesced frames |
| NET-002 | Disconnect | Close before header, mid-header/body, while sending |
| NET-003 | Reconnect | Server restart, client retry, stale socket cleanup |
| NET-004 | Control handshake | Correct/wrong token, malformed HELLO, invalid screen dimensions |
| XFER-006 | Data handshake | Mutual HELLO, wrong token/version, frame before HELLO |
| XFER-007 | Bidirectional transfer | Both endpoints send concurrently |
| XFER-008 | File transfer | Empty, 1 byte, chunk-1/chunk/chunk+1, multi-chunk, large |
| XFER-009 | Directories | Nested, empty, Unicode names, long names |
| XFER-010 | Integrity/ACK | Chunk/file CRC mismatch, ACK OK/CRC/DISK/BADPATH |
| XFER-011 | Recovery | File retry, link retry, retry exhaustion, queued batch behavior |
| XFER-012 | Cancel | Before start, mid-chunk, waiting ACK, after disconnect |
| XFER-013 | Backpressure | Slow disk, full 8-slot queue, bounded memory |
| XFER-014 | Filesystem safety | Traversal, absolute paths, symlink/reparse escape, collisions |
| XFER-015 | Image transfer | 1x1, common sizes, padded stride, cap, malformed dimensions |
| CLIP-016 | Lazy file paste transaction | Offer then Paste Request, duplicate request idempotency, stale generation rejection, batch id binding |

## C. Platform/hardware tests (manual or dedicated self-hosted runners)

| ID | Feature | Cases |
|---|---|---|
| INPUT-001 | Keyboard | Down/up, modifiers, repeats, caps, release-all on disconnect |
| INPUT-002 | Mouse | Relative move, buttons, wheel, multi-monitor coordinates |
| INPUT-003 | Edge handoff | Delay, cooldown, both edges, return, corners |
| CLIP-001 | Text clipboard | Unicode, multiline, empty/large, bidirectional loop suppression |
| CLIP-002 | File clipboard | Single/multiple, folders, empty folders, actual Ctrl+C/Ctrl+V |
| CLIP-003 | Image clipboard | PNG/TIFF/DIB, alpha, orientation, large/unsupported formats |
| CLIP-004 | Local file paste | Copy file/folder then paste locally: native OS paste, no HopDesk transfer/history |
| CLIP-005 | Cross-device lazy paste | Copy only publishes Offer; remote Ctrl+V triggers exactly one request and one transfer |
| CLIP-006 | Clipboard race | User copies new content during transfer; completed old task must not overwrite it |
| CLIP-007 | Offer lifetime | Local paste does not consume Offer; stale/cleared Offer cannot be requested |
| CLIP-008 | Bidirectional file paste | Windows→Mac and Mac→Windows; single/multiple items and nested/empty folders |
| DISP-001 | Windows DDC | Read/switch input, unsupported monitor, unplug/replug |
| DISP-002 | macOS display | Wake/sleep, m1ddc success/failure/missing command |
| APP-001 | Lifecycle | Single instance, startup, tray/menu, shutdown while connected |
| APP-002 | Permissions | Accessibility denied/granted/revoked |
| APP-003 | Stability | 8-hour idle, 2-hour transfer, CPU/RSS/thread leak |
| APP-004 | Network | Wi-Fi roam, sleep/wake, firewall, occupied port, LAN loss/recovery |

## Exit criteria

- Section A passes on both hosted operating systems.
- Section B becomes automated before transport implementation changes.
- Section C is recorded PASS / FAIL / BLOCKED with OS, hardware and evidence.
- Never mark hardware cases passed based only on successful compilation.

## Dual-role branch acceptance

- Portable core: both role values persist/reload, legacy defaults, invalid role preserves live config, reverse VK mapping.
- Extracted production macOS server dispatch: token/protocol rejection, authenticated ACK/session geometry, input-role gate, clipboard origin (platform effects doubled; scheduling not tested).
- Native Windows HA runner: existing server tests plus client control/data HELLO, peer/session binding and reconnect using a loopback protocol double.
- Native Receiver transfer on both platforms: same-session TCP reconnect after one file ACK and one partial file, stale batch chunk rejection, exact staged bytes, duplicate DONE response with one clipboard handoff, Task cancellation, staging deletion and zero live receive reservations. The TCP peer and clipboard handoff are test fixtures. Run `scripts/test-ha.ps1` on Windows and `python3 scripts/test-mac-receive.py` on macOS.
- Native Writer Queue on both platforms: blocked worker, slot and byte capacity, stop timeout and retry, cancellation, allocation rollback, and native permission/partial-write disk-full errors injected into the production file adapter. Run the platform `test-core` script.
- Physical desktops (pending): Windows server/macOS client AND macOS server/Windows client, four edges, modifiers/drag, Unicode clipboard, images/files in both directions, network failure and history; six UI pages at 100–200% DPI.
