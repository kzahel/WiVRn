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

This first branch pass now implements those build flags in CMake and wires `WIVRN_USE_UINPUT` through the session code. The desktop `wivrn-server` entrypoint is still GLib/DBus/Avahi/libnotify-based, so those flags are currently a structural refactor step rather than a complete headless macOS host.

The next branch pass adds `WIVRN_USE_APPLICATIONS` so a minimal runtime-library build can also skip WiVRn's desktop application discovery/icon path. That removes the `librsvg`, PNG, and `libarchive` dependency chain from the first macOS configure attempt. The desktop `wivrn-server` shell still assumes application listing/launch support and currently requires `WIVRN_USE_APPLICATIONS=ON`.

The branch also flips `WIVRN_BUILD_WIVRNCTL` to default `OFF` on macOS. `wivrnctl` is not part of the first host-port milestone, and leaving it enabled by default just creates another early configure failure on Apple before the actual runtime/library work is reached.

As of the April 3, 2026 configure probe on the reference macOS machine, these refactors are enough to move the first failure past Linux host-shell assumptions and down to Boost provisioning:

- with `WIVRN_USE_SYSTEM_BOOST=ON`, CMake fails because the current machine does not have Boost `locale`, `url`, and `iostreams`
- with `WIVRN_USE_SYSTEM_BOOST=OFF`, CMake reaches `FetchContent_MakeAvailable(boost)` and then fails in the current restricted environment when it cannot resolve `github.com`

That is useful progress: the next blocker is now a real dependency strategy question rather than another hidden Linux runtime coupling.

The branch now also supports `WIVRN_MONADO_SOURCE_DIR` for macOS work. That lets the host-port branch point directly at the active local macOS Monado fork instead of cloning upstream Monado and applying the full WiVRn Monado patch stack during configure. This is the correct near-term path for the Mac port because the goal is to converge WiVRn's Monado requirements onto the existing macOS fork, not to mutate that fork through FetchContent patching.

The next configure blocker after that override was the SteamVR Lighthouse Monado driver being enabled by default. The branch now defaults both `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE` and `WIVRN_FEATURE_SOLARXR` to `OFF` on macOS so the first Apple host milestone does not request Linux-only optional tracking drivers from Monado.

The next blocker after that was a packaging/runtime-target assumption in `server/CMakeLists.txt`: WiVRn expected the embedded Monado tree to expose a `monado` target for `libmonado`, but the active macOS Monado fork only provides the OpenXR runtime target in this configuration. The branch now treats `libmonado` as optional when generating the runtime manifest and install target list. That is the correct direction for macOS because the first host milestone only needs the runtime target to exist.

With those fixes in place, the minimal macOS host probe now reaches a real build:

- configure succeeds with `WIVRN_MONADO_SOURCE_DIR=/Users/kgraehl/code/monado`
- `ninja -C build-macos-host openxr_wivrn` succeeds

That is the first concrete "WiVRn-derived server builds on macOS" checkpoint, even though it is still only the runtime-library slice and not the full desktop shell.

The next branch pass adds a first macOS-only host executable,
`wivrn-server-headless`, and a set of Darwin portability fixes:

- explicit headless target split from the Linux desktop shell
- `GitVersion.cmake` no longer depends on implicit build-time working
  directories
- Darwin fallbacks in `common/wivrn_sockets.cpp` for:
  - IPv6 multicast membership constants
  - `sendmmsg` / `recvmmsg`
  - stricter libc++ narrowing rules
- libc++ portability fixes for missing `std::ranges::enumerate_view`
- macOS-safe hostname and process includes
- Monado helper API updated from the removed `u_builders.h` include to
  `target_builder_helpers.h`

As of the April 3, 2026 compile probe on the reference machine:

- `cmake -S . -B build-macos-host ...` succeeds for the headless target
- `ninja -C build-macos-host wivrn-server-headless` succeeds
- `wivrn-server-headless --help` succeeds
- `wivrn-server-headless --no-encrypt` starts and waits for a headset
  connection on TCP port `9757`

The key compatibility changes that enabled that checkpoint were:

- minimal Monado compatibility shims for:
  - `comp_target_image.view_cbcr`
  - `render_resources.distortion.buffer`
  - `render_compute_distortion_foveation_data`
  - `RENDER_FOVEATION_BUFFER_DIMENSIONS`
- Monado helper/session drift reconciliation in WiVRn:
  - `u_builders.h` updated to `target_builder_helpers.h`
  - `xdevs` / `xdev_count` reconciled with newer
    `static_xdevs` / `static_xdev_count`

The next checkpoint on April 3, 2026 moves past "headless host waits for a
connection" and into the first real Quest handshake:

- the local Android client from the same WiVRn branch now builds on macOS and
  installs as `org.meumeu.wivrn.local`
- the initial headset handshake now completes successfully over
  `adb reverse tcp:9757 tcp:9757`
- after that handshake, the host enters Monado/compositor startup and
  enumerates the headset as `Meta Quest 3`

Two concrete portability fixes were needed to make the local Android client
build on macOS:

- `cmake/android/FindOpenSSL.cmake` now discovers the active NDK prebuilt
  `bin` directory instead of hardcoding `linux-x86_64`
- that helper now preserves a sane macOS `PATH` and uses a bounded `make -j`
  level during the Android OpenSSL sub-build

That is enough to get from "protocol mismatch with the store-installed client"
to "branch-matched client and host complete the first WiVRn handshake on
macOS."

The current blockers are now encoder/video-path blockers, not transport or
client-negotiation blockers.

Observed sequence with the default macOS headless build:

- handshake completed
- Monado compositor started
- host identified the headset and GPU correctly
- encoder selection failed with:
  `Failed to find a suitable video encoder`

That failure matches the current Apple defaults on this branch:

- `WIVRN_USE_NVENC=OFF`
- `WIVRN_USE_VAAPI=OFF`
- `WIVRN_USE_VULKAN_ENCODE=OFF`
- `WIVRN_USE_X264=OFF`

The next probe enabled `WIVRN_USE_X264=ON`, which gets past encoder selection,
but initially still failed in the compositor/encoder image path on MoltenVK:

- `VK_ERROR_FEATURE_NOT_PRESENT: vkCreateImage() : Chroma-subsampled formats may only have one array layer`
- `Headless host failed: vmaCreateImage(...): ErrorFeatureNotPresent`

The relevant current assumption is in `server/driver/wivrn_comp_target.cpp`:

- WiVRn allocates a single `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` image
- it uses `arrayLayers = 3` for left, right, and alpha

That layout works for the Linux/Vulkan targets WiVRn was originally built
around but not on the current Metal/MoltenVK stack, where chroma-subsampled
images must have only one array layer.

That old blocker is now cleared on the branch with a temporary Apple software
encode compatibility path:

- Apple x264 now renders into a layered `VK_FORMAT_R8G8B8A8_SRGB` compositor
  target instead of the old subsampled array image
- `video_encoder_x264` reads back per-eye RGBA and converts `RGBA -> NV12` on
  the CPU before encode
- this temporary Apple path disables the third alpha stream for now

That is not the final architecture, but it is enough to move the port from
"handshake works but video bring-up dies in `vkCreateImage`" to a real runtime
checkpoint.

As of the April 3, 2026 validation run on the reference machine:

- branch-matched Quest client handshake on macOS: achieved
- Monado/compositor startup after handshake: achieved
- `openxr_wivrn` now builds on macOS alongside the headless host
- a real macOS OpenXR app now runs through the WiVRn-derived runtime:
  `tests_macos_openxr_vulkan_probe` connected through `libopenxr_wivrn.dylib`
  and submitted 60 projection frames successfully
- the Quest client now reaches decoder startup and logs:
  - `Mediacodec format changed`
  - `decoded image size: 1088x1088`
  - `Stream scene ready`

So the current branch status is now:

- protocol negotiation: working
- macOS host/compositor startup: working
- first encoded video path: partially working on the temporary Apple x264 path
- remaining blocker: stream continuity and visible in-headset confirmation, not
  `vkCreateImage` bring-up

Two important caveats remain:

- Apple alpha passthrough is still disabled on this temporary path and must be
  restored before the real MVP is complete
- Quest logs still showed missing-shard warnings during the first long run, so
  frame delivery stability is not yet proven

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

Concrete evidence from the current headless build:

- `server/driver/wivrn_comp_target.cpp` still expects `comp_target_image.view_cbcr`
- `server/driver/wivrn_foveation.cpp` still expects:
  - `render_resources.distortion.buffer`
  - `render_compute_distortion_foveation_data`
  - `RENDER_FOVEATION_BUFFER_DIMENSIONS`

Those symbols appear directly in WiVRn's
`patches/monado/0005-Replace-distortion-with-foveation.patch` and do not exist
in stock current Monado. The active macOS fork now carries a minimal
compile-compatibility shim for them, but not the full upstream WiVRn
implementation yet.

#### Monado helper/session drift beyond WiVRn patch 0005

Status against current macOS Monado fork: present.

Evidence from current headless build:

- `server/driver/wivrn_session.cpp` no longer matches the current Monado target
  session layout and fails on device-array members such as `xdevs` and
  `xdev_count`
- the old `util/u_builders.h` include is gone from current Monado and had to
  be updated to `src/xrt/targets/helpers/target_builder_helpers.h`

Assessment:

- this is separate from WiVRn's explicit patch stack
- it is normal upstream drift between WiVRn's pinned Monado revision and the
  active macOS fork
- it should be handled alongside the patch-port matrix, not treated as an
  unrelated macOS problem

The branch now handles the easy session drift pieces:

- `u_builders.h` updated to `target_builder_helpers.h`
- `xdevs` / `xdev_count` reconciled with Monado's current
  `static_xdevs` / `static_xdev_count`

That gets the headless host binary built and running. The next meaningful work
is now no longer compile-only reconciliation or first handshake bring-up. It
is:

- instrumenting host-side frame and shard delivery on the Apple path
- doing short in-headset confirmation that the WiVRn macOS stream is actually
  visible and motion-correct
- then redesigning the Apple pre-encode path so alpha can return as a separate
  third stream instead of staying disabled behind the temporary x264 shim

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
