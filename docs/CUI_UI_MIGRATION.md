# Unified CUI main UI

## Scope

The macOS and Windows main windows use the same CUI 1.5.0 component tree and the
same page/state implementation in `src/ui/hopdesk_ui.c`.

The platform files are adapters only:

- `src/mac/main_ui.m`: Cocoa lifecycle, status/permission/config/audio adapters.
- `src/win/main_ui.c`: Win32 lifecycle, tray/config/audio/display adapters.
- Native menu-bar and tray shells remain platform-native. Network, file transfer,
  clipboard, keyboard/mouse injection, display control and audio engines are not
  moved into CUI.

## Window lifecycle

Opening the main window creates a CUI app/context and its renderer. Closing the
window destroys the CUI app, renderer, font atlas and UI-owned snapshots. The
native tray/status item and background services remain alive. This avoids keeping
a hidden native control tree and its UI allocations resident after the window is
closed. The macOS host restores the activation policy it had before opening CUI:
the menu-bar process returns to Accessory mode, while the separate UI process
remains Regular.

The UI model is revision-driven:

- at most 32 live task summaries and 24 history rows are copied into the UI model;
- file details are fetched 24 rows at a time through the existing tracking page API;
- task and history snapshots are refreshed only when their revisions change;
- active transfers/tasks poll at 500 ms; an idle window polls at 2 s for connection changes;
- platform events and user input request immediate redraws.

## CUI dependency and rendering

CUI is pinned as a Git submodule at the v1.5.0 release commit
`f9597a30d3bba4f7ef3e0f918e31c28bdcd4d2a3`.

- macOS builds include the Cocoa app backend and Metal renderer, with OpenGL and
  software fallbacks.
- Windows builds include the Win32 app backend and Direct3D 12 renderer, with
  OpenGL and software fallbacks.
- CUI's bundled UI fonts are used with one system CJK fallback font where
  available. The CJK font is memory-mapped read-only rather than copied into the
  process heap, and the mapping is released when the CUI window/context closes.

After cloning or updating this branch, initialize the dependency:

```sh
git submodule update --init --recursive
```

Then build with the existing platform scripts:

```sh
sh scripts/build-mac.sh
pwsh scripts/build-win.ps1 -Compiler msvc
```

## Validation

The dual-platform CI build and existing portable/network/transfer tests remain
the acceptance gate. Real two-device checks are still required for tray opening,
window close/reopen, display actions, permissions, audio settings, transfer
pagination/cancel/retry and long-running background services.


### Experimental macOS memory isolation

Normal first launch starts the tray/IPC agent; clicking the tray launches a separate UI process. Closing the window destroys the CUI app and returns from the UI process after any pending settings save. The agent continues network, input, audio, transfer and tray progress handling. No CUI window is permitted in the agent. Automatic application replacement is not yet supported in split mode; stop the agent safely and install the test build manually. The OS AutoFill helper, actual physical footprint and repeated open/close behavior require real-device validation; no measured memory reduction is claimed here.

### Metal memory experiment

The macOS build defaults to two CAMetalLayer drawables, with an explicit three-drawable control build. Retina resolution and UI layout are unchanged. The upstream CUI submodule remains pinned and clean. The expected pixel-capacity difference is one drawable; actual physical footprint and interactive latency must be measured before promotion. Windows still uses one process: the UI directly reads local engine state and dispatches actions locally. A Windows split requires bounded, authenticated local IPC, agent-owned config/transfer/network operations and a separately exiting UI; it is not implemented by this change.
