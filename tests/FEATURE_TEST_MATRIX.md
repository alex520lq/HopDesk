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

## C. Platform/hardware tests (manual or dedicated self-hosted runners)

| ID | Feature | Cases |
|---|---|---|
| INPUT-001 | Keyboard | Down/up, modifiers, repeats, caps, release-all on disconnect |
| INPUT-002 | Mouse | Relative move, buttons, wheel, multi-monitor coordinates |
| INPUT-003 | Edge handoff | Delay, cooldown, both edges, return, corners |
| CLIP-001 | Text clipboard | Unicode, multiline, empty/large, bidirectional loop suppression |
| CLIP-002 | File clipboard | Single/multiple, folders, empty folders, actual Ctrl+C/Ctrl+V |
| CLIP-003 | Image clipboard | PNG/TIFF/DIB, alpha, orientation, large/unsupported formats |
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
