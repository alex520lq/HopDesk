# HopDesk manual feature verification runbook

Use this only on a dedicated Windows + macOS pair. These tests interact with the real
desktop session and may move the cursor, change clipboard contents, or switch displays.
Record each result; do not infer a pass from CI compilation.

## Test environment

- Windows: record version/build, display model, resolution, scaling, keyboard layout.
- macOS: record version/build, display model, resolution, scaling, keyboard layout.
- Network: same LAN/VLAN, RTT, Wi-Fi/Ethernet, firewall state.
- Build both endpoints from the same commit and protocol version.
- Set the same non-empty token on both ends for authenticated runs.
- Save both logs before and after each test.
- macOS: grant Accessibility permission only after first launch; record denied and granted cases separately.
- Display tests require a monitor with DDC/CI enabled and known input-source codes.

## Results format

For every case record: PASS, FAIL, or BLOCKED, commit SHA, OS versions,
steps performed, expected/actual result, log excerpt, and screenshot/video if useful.
Do not mark BLOCKED cases as passed.

## Connection and lifecycle

| ID | Steps | Expected result |
|---|---|---|
| APP-001 | Start Windows server, then macOS client with correct host/ports/token | Control and data channels connect; both logs show matching protocol |
| APP-002 | Configure wrong token on macOS and reconnect | Control handshake is rejected; no sharing or clipboard sync |
| APP-003 | Configure wrong data token only, if test harness can override per channel | Data channel closes; no file/image frame is accepted |
| APP-004 | Start a second Windows instance | Second instance exits with single-instance notice |
| APP-005 | Start a second macOS instance | Second instance exits; original menu-bar item remains |
| APP-006 | Occupy TCP 24817 or 24818, then start server | Clear bind error; no silent port sharing |
| APP-007 | Disconnect LAN for 10 seconds, restore it | Client retries and reconnects; input returns local during outage |
| APP-008 | Sleep and wake either computer | Stale sockets recover; no process restart required |

## Keyboard sharing

| ID | Steps | Expected result |
|---|---|---|
| INPUT-001 | Enter sharing; type letters, digits, punctuation and function keys | Correct key events on Mac; no stuck keys |
| INPUT-002 | Test left/right Ctrl, Alt, Shift, Win, Caps Lock individually | Matches documented Ctrl→Command, Alt→Option, Win→Control mapping |
| INPUT-003 | Hold modifier + letter; press/release in different orders | No stuck modifier; correct shortcut reaches Mac |
| INPUT-004 | Disconnect while a key is held | Mac receives release-all/reset; keyboard remains usable locally |
| INPUT-005 | Test non-US keyboard layout if used | Record known layout limitations and mismatches |

## Mouse sharing and edge handoff

| ID | Steps | Expected result |
|---|---|---|
| INPUT-006 | Move slowly/quickly while sharing | Relative motion remains stable; no cumulative drift |
| INPUT-007 | Test left/middle/right click and horizontal/vertical wheel | Correct button and scroll events |
| INPUT-008 | Move to configured Windows edge and dwell below switch delay | No premature handoff |
| INPUT-009 | Dwell beyond switch delay | Cursor enters Mac; Windows capture activates |
| INPUT-010 | Move away before timer expires | Pending handoff cancels |
| INPUT-011 | Return through configured Mac edge beyond margin | Control returns to Windows |
| INPUT-012 | Repeat rapidly at the seam | Cooldown prevents ping-pong |
| INPUT-013 | Test top/bottom corners and multiple monitors | No accidental handoff or coordinate jump |

## Clipboard text

| ID | Steps | Expected result |
|---|---|---|
| CLIP-001 | Copy plain ASCII on Windows; paste on Mac | Exact text |
| CLIP-002 | Copy Chinese/Japanese/emoji/multiline text in both directions | UTF-8 round-trip without truncation or replacement |
| CLIP-003 | Copy text on destination after receiving remote text | No A→B→A echo loop |
| CLIP-004 | Copy large text near configured protocol limit | Either exact transfer or clean rejection; no hang |
| CLIP-005 | Rapidly change clipboard several times | Latest content arrives; app remains responsive |

## File and folder clipboard transfer

Run every case in both directions (Windows→Mac and Mac→Windows).

| ID | Steps | Expected result |
|---|---|---|
| XFER-001 | Copy a zero-byte file | Empty file appears and can be pasted |
| XFER-002 | Copy 1-byte and small text/binary files | Exact size and SHA-256 match |
| XFER-003 | Copy files of chunk-size−1, chunk-size, chunk-size+1 | Exact bytes; boundary chunk handled |
| XFER-004 | Copy a multi-chunk file and a large file (at least 1 GB if available) | Exact hash; bounded memory; UI remains responsive |
| XFER-005 | Copy multiple files in one operation | All successful files appear in clipboard in original order |
| XFER-006 | Copy nested folders with empty directories | Directory structure and empty folders preserved |
| XFER-007 | Copy Chinese, emoji, spaces and long file names | Names preserved; unsupported names fail cleanly |
| XFER-008 | Cancel during a large transfer | Partial files removed; both sides stop the batch |
| XFER-009 | Disconnect during a chunk, then reconnect | Job retries within configured budget; no corrupt final file |
| XFER-010 | Make destination unwritable / disk full | DISK ACK/failure reported; no partial file offered in clipboard |
| XFER-011 | Send malformed traversal path using protocol test harness | Receiver rejects path; no file outside temp root is created |
| XFER-012 | Corrupt one chunk using protocol test harness | CRC ACK/retry; other files in batch continue |
| XFER-013 | Interrupt after one file ACK in a multi-file batch | Confirm only unacknowledged entries are resent |
| XFER-014 | Slow destination disk until receive queue fills | Memory remains bounded; sender slows; no deadlock |

## Image clipboard

| ID | Steps | Expected result |
|---|---|---|
| CLIP-006 | Copy a small screenshot on Windows and paste on Mac | Image dimensions and pixels visually match |
| CLIP-007 | Copy a Mac image and paste on Windows | DIB appears correctly; no upside-down image |
| CLIP-008 | Test transparency and padded row stride | Alpha/orientation/row alignment are correct or documented |
| CLIP-009 | Test large 4K image | Transfer succeeds within configured cap |
| CLIP-010 | Send invalid dimensions/stride via protocol harness | Receiver rejects without excessive allocation |
| CLIP-011 | Send image with missing final chunk, gap, overlap or extra chunk | Image is not applied to clipboard; batch is rejected |
| CLIP-012 | Corrupt one image chunk CRC | Image is discarded; no partial image reaches clipboard |

## Display control

| ID | Steps | Expected result |
|---|---|---|
| DISP-001 | Trigger Windows→Mac display switch | Mac wake and input-source command execute; keyboard returns local during switch |
| DISP-002 | Trigger Mac→Windows switch | Windows DDC input switches; local control restored |
| DISP-003 | Trigger Mac display sleep | Display sleeps; app/network state remains recoverable |
| DISP-004 | Disconnect display or use unsupported DDC input | Clear failure; no app crash or stuck sharing |
| DISP-005 | Run without m1ddc installed / command returns failure | Error is logged; app remains alive |

## Soak and recovery

| ID | Steps | Expected result |
|---|---|---|
| STAB-001 | Leave both apps connected and idle for 8 hours | No disconnect loop, CPU spike or memory growth |
| STAB-002 | Transfer files continuously for 2 hours | No corruption, deadlock or unbounded RSS |
| STAB-003 | Repeat connect/disconnect 100 times | No leaked handles, threads or stale state |
| STAB-004 | Repeatedly cancel and restart transfers | Queue and clipboard state reset correctly |
| STAB-005 | Revoke Accessibility permission while connected | Injection failure is safe; app recovers after permission restored |

## Current status

The GitHub-hosted CI covers portable core validation and TCP loopback only.
All rows above require actual desktop sessions, hardware, or a dedicated protocol
test harness. Record them as BLOCKED until run on the real two-machine setup.
