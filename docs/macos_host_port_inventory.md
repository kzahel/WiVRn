# macOS Host Port Inventory

## Scope

This document captures the first concrete inventory for a WiVRn-derived macOS
host port.

It is intentionally narrower than a full upstream cross-platform plan. The goal
is to identify the minimum host subset needed to reproduce the first ugly frame
milestone on macOS using WiVRn architecture and the existing WiVRn Android
client.

## Target Outcome

First target outcome:

- existing WiVRn Quest client connects to a macOS host
- host uses the active macOS Monado fork
- manual connection is acceptable
- no dashboard is required
- no audio is required initially
- no tracked input return is required initially
- first success is encoded video plus correct 6DoF head motion

This means the first macOS host should be closer to a minimal CLI service than a
full Linux desktop product.

## Current WiVRn Host Split

From the current tree, the host side separates into two broad layers.

### 1. Core streaming/runtime layer

These are the pieces we want to preserve as much as possible:

- Monado-backed WiVRn server runtime
- server-side driver/session model
- WiVRn transport and headset negotiation
- server encoder abstraction
- existing WiVRn Android client compatibility

Relevant files:

- `server/driver/wivrn_session.cpp`
- `server/driver/wivrn_connection.cpp`
- `server/driver/wivrn_comp_target.cpp`
- `server/encoder/*`
- `server/wivrn_ipc.h`
- `server/main.cpp`

### 2. Linux desktop/service shell

These are the main Linux-specific surfaces that should be treated as replaceable
for the first macOS pass:

- Avahi discovery
- DBus control surface
- `libnotify`
- `systemd`
- PipeWire / PulseAudio audio backends
- `uinput`
- some Steam desktop integration assumptions

Relevant files:

- `server/avahi_publisher.cpp`
- generated DBus code from `dbus/*.xml`
- `server/main.cpp`
- `server/sleep_inhibitor.cpp`
- `server/audio/audio_pipewire.cpp`
- `server/audio/audio_pulse.cpp`
- `server/driver/wivrn_uinput.cpp`

## Build-System Findings

### Good news

WiVRn already has a useful separation point:

- `WIVRN_BUILD_SERVER`
- `WIVRN_BUILD_SERVER_LIBRARY`

The runtime/library path is important because the first macOS port should favor
that over the full Linux desktop shell.

Relevant CMake:

- `CMakeLists.txt`
- `server/CMakeLists.txt`

### Current blockers for a direct macOS configure

At the top level, when `WIVRN_BUILD_SERVER` is enabled, the build currently
requires Linux-facing dependencies unconditionally:

- `gdbus-codegen`
- Avahi
- `glib/gio`
- `libnotify`

Even before runtime behavior is considered, this means the current server build
is not structured as a minimal portable host target yet.

### First CMake refactor target

The first macOS-oriented refactor should introduce explicit optional gates for:

- Avahi
- DBus control surface
- notifications
- audio backends
- `uinput`

Recommended first new options:

- `WIVRN_USE_AVAHI`
- `WIVRN_USE_DBUS_CONTROL`
- `WIVRN_USE_LIBNOTIFY`
- `WIVRN_USE_UINPUT`

Suggested initial defaults:

- Linux: keep current behavior by default
- macOS: all of the above default `OFF`

## Monado Patch Inventory

WiVRn depends on a real Monado patch stack. The current macOS Monado fork is:

- repo: `/Users/kgraehl/code/monado`
- branch: `kg/macos-ipc-port-spike`
- revision inspected during this pass:
  `646e5dabb8b5075152224242bac03e7aa9bf3183`

WiVRn pins Monado at:

- `ef152c973b94a313e1ba9552162818b5151ac671`

### Patch-by-patch first assessment

#### `0001-c-multi-early-wake-of-compositor.patch`

Status against current macOS Monado fork: missing.

Evidence:

- no `wake_cond`
- no early compositor wake logic in `comp_multi_system.c`

Assessment:

- likely important for latency and compositor timing
- should be ported early

#### `0002-Use-extern-socket-fd.patch`

Status against current macOS Monado fork: missing.

Evidence:

- Linux and Apple IPC mainloops exist in Monado
- no external `listen_socket` injection hook was found

Assessment:

- important
- probably needs a macOS-specific adaptation rather than a Linux-only copy
- should be ported early because WiVRn host orchestration expects to hand
  Monado a socket path/FD boundary

#### `0003-change-environment-blend-mode-selection-logic.patch`

Status against current macOS Monado fork: missing.

Evidence:

- current `find_active_blend_mode` still uses focused/first-visible logic

Assessment:

- likely relevant once passthrough and mixed-layer behavior matter
- not critical for the very first encoded-video bring-up

#### `0004-st-oxr-forward-0-refresh-rate.patch`

Status against current macOS Monado fork: missing.

Evidence:

- `xrRequestDisplayRefreshRateFB(0)` is still early-returned in
  `oxr_api_session.c`

Assessment:

- low to medium priority
- likely needed for WiVRn refresh negotiation parity

#### `0005-Replace-distortion-with-foveation.patch`

Status against current macOS Monado fork: missing.

Evidence:

- no `view_cbcr` path in current fork
- no WiVRn foveation target changes found

Assessment:

- largest patch
- likely one of the hardest port items
- may be deferrable only if the first macOS host can temporarily tolerate a
  simpler output path than upstream WiVRn expects
- otherwise this is an early major port task

#### `0006-d-steamvr_lh-prevent-crash-on-vive-pro2-WiVRn.patch`

Status against current macOS Monado fork: not checked in detail.

Assessment:

- optional for first macOS bring-up
- tied to SteamVR lighthouse path

#### `0007-st-oxr-push-XrEventDataInteractionProfileChanged-whe.patch`

Status against current macOS Monado fork: missing.

Evidence:

- `XR_NULL_PATH` early break is still present in `oxr_input.c`

Assessment:

- likely useful for input robustness
- not first-video critical

#### `0008-Don-t-get-pose-data-in-compositor.patch`

Status against current macOS Monado fork: already present or substantially
present.

Evidence:

- `comp_renderer.c` already contains `get_projection_layer(...)`
- compositor path already prefers submitted projection-layer poses when present

Assessment:

- this is good news
- one important WiVRn semantic is already carried by the current macOS fork

#### `0009-don-t-verify-GL-stuff.patch`

Status against current macOS Monado fork: missing.

Evidence:

- GLX verification checks are still present in `oxr_verify.c`

Assessment:

- probably irrelevant for the first macOS WiVRn host because the target path is
  not GLX-based
- low priority

#### `0010-configure-u_git_tag-in-WiVRn.patch`

Status against current macOS Monado fork: not applicable as a runtime port item.

Assessment:

- build integration detail only
- low priority for the macOS host effort itself

## Practical Monado Port Order

Recommended first order:

1. `0002` external socket injection or equivalent macOS hook
2. `0001` early compositor wake
3. `0004` refresh-rate forwarding
4. verify current `0008` behavior is sufficient
5. decide whether `0005` is mandatory for first encoded video
6. pull `0003` and `0007` when passthrough/input behavior matters

## First macOS Host Shape

Recommended first host target:

- branch-local experimental target only
- no dashboard
- no Avahi
- no DBus control surface
- no notifications
- no audio
- no `uinput`
- manual connection only
- existing WiVRn Quest client
- x264 or simplest available encoder path first if hardware encode is not ready

## Concrete Code Tasks

### Phase 1: make the server build structurally portable

1. Optionalize hard Linux build requirements in `CMakeLists.txt`
   - Avahi
   - `gdbus-codegen`
   - `glib/gio` control path
   - `libnotify`
2. Optionalize corresponding sources in `server/CMakeLists.txt`
3. Gate `wivrn_uinput.cpp` behind a feature or platform check
4. Allow a "headless server" style build target without dashboard/service shell

### Phase 2: align with macOS Monado fork

1. Port or adapt WiVRn socket-injection expectations to the Apple IPC mainloop
2. Port early compositor wake behavior
3. Port refresh-rate behavior
4. Decide whether first video can ship before full WiVRn foveation patching

### Phase 3: first live macOS host

1. manual connect path only
2. no audio
3. no input return
4. Quest client handshake
5. first encoded stereo video
6. verify correct 6DoF

## Recommended Immediate Next Step

The very next implementation step should be a build-system refactor, not a
codec or client change.

Specifically:

- make a minimal WiVRn host build target that does not require Avahi, DBus,
  `libnotify`, audio backends, or `uinput`
- keep `WIVRN_BUILD_SERVER_LIBRARY` in mind as the preferred runtime-oriented
  seam for macOS

Until that exists, the rest of the macOS port remains blocked behind Linux host
assumptions at configure time.
