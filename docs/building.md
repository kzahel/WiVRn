# Building

# Server (PC)

## Compile

From your checkout directory, with automatic detection of encoders
```bash
cmake -B build-server . -GNinja -DWIVRN_BUILD_CLIENT=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-server
```

It is possible to disable specific encoders, by adding options
```
-DWIVRN_USE_NVENC=OFF
-DWIVRN_USE_VAAPI=OFF
-DWIVRN_USE_VULKAN_ENCODE=OFF
-DWIVRN_USE_X264=OFF
```

Force specific audio backends
```
-DWIVRN_USE_PIPEWIRE=ON
-DWIVRN_USE_PULSEAUDIO=ON
```

Systemd service and pretty hostname support
```
-DWIVRN_USE_SYSTEMD=ON
```

Lighthouse driver support for use with lighthouse-tracked devices
```
-DWIVRN_FEATURE_STEAMVR_LIGHTHOUSE=ON
```

Additionally, if your environment requires absolute paths inside the OpenXR runtime manifest, you can add `-DWIVRN_OPENXR_MANIFEST_TYPE=absolute` to the build configuration.

The current macOS host-port branch is starting to separate the Linux desktop shell from the underlying runtime pieces. New host-shell feature gates now exist for:

- `-DWIVRN_USE_AVAHI=OFF`
- `-DWIVRN_USE_DBUS_CONTROL=OFF`
- `-DWIVRN_USE_LIBNOTIFY=OFF`
- `-DWIVRN_USE_UINPUT=OFF`
- `-DWIVRN_USE_APPLICATIONS=OFF`
- `-DWIVRN_BUILD_WIVRNCTL=OFF`

At the moment, disabling those is mainly useful for early portability work and runtime-library-only builds. `WIVRN_USE_APPLICATIONS=OFF` also disables headset-driven desktop application browsing, icon loading, and launch requests, which removes the `librsvg`, `libarchive`, and PNG dependency path from a minimal host build.

The full `wivrn-server` desktop shell still depends on a Linux/GLib entry path and currently requires `WIVRN_USE_APPLICATIONS=ON`. On this branch, `WIVRN_BUILD_WIVRNCTL` now also defaults to `OFF` on macOS because the CLI tool depends on the same Linux control stack assumptions.

A minimal macOS runtime-library configure probe currently looks like:

```sh
cmake -S . -B build-macos-host -GNinja \
  -DWIVRN_BUILD_SERVER=ON \
  -DWIVRN_BUILD_SERVER_LIBRARY=ON \
  -DWIVRN_MONADO_SOURCE_DIR=/Users/kgraehl/code/monado \
  -DWIVRN_FEATURE_SOLARXR=OFF \
  -DWIVRN_FEATURE_STEAMVR_LIGHTHOUSE=OFF \
  -DWIVRN_USE_AVAHI=OFF \
  -DWIVRN_USE_DBUS_CONTROL=OFF \
  -DWIVRN_USE_LIBNOTIFY=OFF \
  -DWIVRN_USE_UINPUT=OFF \
  -DWIVRN_USE_APPLICATIONS=OFF \
  -DWIVRN_USE_PIPEWIRE=OFF \
  -DWIVRN_USE_PULSEAUDIO=OFF
```

On this branch, that probe now gets past the Linux desktop-shell and application-browser dependencies. The next blocker on the current macOS machine is Boost provisioning:

- `WIVRN_USE_SYSTEM_BOOST=ON`: requires a system Boost installation with `locale`, `url`, and `iostreams`
- `WIVRN_USE_SYSTEM_BOOST=OFF`: uses `FetchContent` to download Boost 1.89.0 from GitHub

For macOS host-port work, `WIVRN_MONADO_SOURCE_DIR` is now the preferred path. It allows the branch to use the active local macOS Monado fork directly instead of cloning upstream Monado and applying the Linux-oriented WiVRn patch stack during configure. When this override is used, the caller is responsible for pointing it at a compatible Monado tree.

On this branch, `WIVRN_FEATURE_SOLARXR` and `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE` now also default to `OFF` on macOS because the first Apple host milestone should not request Linux-only Monado drivers by default.

On the reference macOS machine, that configure probe now succeeds against `/Users/kgraehl/code/monado`, and the first runtime target build also succeeds with:

```sh
ninja -C build-macos-host openxr_wivrn
```

That does not mean the full WiVRn host is working on macOS yet. It means the branch has now reached a real buildable OpenXR runtime target on top of the local macOS Monado fork, which is the first meaningful host-port checkpoint.

The next branch checkpoint is a new minimal host target:

```sh
ninja -C build-macos-host wivrn-server-headless
```

On the current macOS port branch, that target now builds successfully on Apple
against the active local Monado fork.

The build-only blockers from the previous pass have been cleared with:

- small Darwin portability fixes in WiVRn host-core code
- Monado compatibility shims for WiVRn's current foveation/compositor
  expectations
- WiVRn session reconciliation with newer Monado device-array naming

On the reference macOS machine, the following now works:

```sh
ninja -C build-macos-host wivrn-server-headless
build-macos-host/server/wivrn-server-headless --help
build-macos-host/server/wivrn-server-headless --no-encrypt
```

and the headless host starts with:

- `WiVRn <rev> headless host starting`
- `Encryption disabled for headless host`
- `Waiting for initial headset connection on TCP port 9757`

That is still not an end-to-end stream proof. It is the first concrete macOS
host-binary checkpoint above the runtime target.

The next branch checkpoint on the reference macOS machine is now beyond
"waiting for a headset connection":

- the local Android client from the same WiVRn branch now builds on macOS with
  `./gradlew installDebug`
- that installs the matching Quest package `org.meumeu.wivrn.local`
- manual USB-tunneled connection now works with:

```sh
adb reverse tcp:9757 tcp:9757
adb shell am start -a android.intent.action.VIEW \
  -d 'wivrn+tcp://localhost:9757' \
  org.meumeu.wivrn.local
```

Two branch-local Android fixes were required to make that client build work on
macOS:

- `cmake/android/FindOpenSSL.cmake` now discovers the active NDK prebuilt
  toolchain directory instead of hardcoding `linux-x86_64`
- that helper now preserves a sane macOS `PATH` and uses a bounded `make -j`
  level during the Android OpenSSL sub-build
- that helper also configures a minimal Android OpenSSL build (`shared`,
  `no-apps`, `no-tests`) so local Quest-client rebuilds do not spend extra time
  compiling unused OpenSSL utilities

With those fixes in place, the initial WiVRn headset handshake now completes
successfully against `wivrn-server-headless` on macOS.

The current Apple bring-up path then needs `x264` enabled explicitly:

```sh
cmake -S . -B build-macos-host -GNinja \
  -DWIVRN_BUILD_SERVER=ON \
  -DWIVRN_BUILD_SERVER_LIBRARY=ON \
  -DWIVRN_MONADO_SOURCE_DIR=/Users/kgraehl/code/monado \
  -DWIVRN_FEATURE_SOLARXR=OFF \
  -DWIVRN_FEATURE_STEAMVR_LIGHTHOUSE=OFF \
  -DWIVRN_USE_AVAHI=OFF \
  -DWIVRN_USE_DBUS_CONTROL=OFF \
  -DWIVRN_USE_LIBNOTIFY=OFF \
  -DWIVRN_USE_UINPUT=OFF \
  -DWIVRN_USE_APPLICATIONS=OFF \
  -DWIVRN_USE_PIPEWIRE=OFF \
  -DWIVRN_USE_PULSEAUDIO=OFF \
  -DWIVRN_USE_X264=ON

ninja -C build-macos-host wivrn-server-headless openxr_wivrn
```

On the current branch, the old MoltenVK blocker from the layered YCbCr target
is now cleared by a temporary Apple software-encode compatibility path:

- Apple x264 uses a layered `VK_FORMAT_R8G8B8A8_SRGB` compositor target
- `video_encoder_x264` converts `RGBA -> NV12` on the CPU before encode
- the Apple path currently disables WiVRn's third alpha stream

That is enough to move the port to a real runtime checkpoint. On the reference
macOS machine, this now works:

```sh
build-macos-host/server/wivrn-server-headless --no-encrypt
```

with the branch-matched Quest client tunneled over USB:

```sh
adb reverse tcp:9757 tcp:9757
adb shell am start -a android.intent.action.VIEW \
  -d 'wivrn+tcp://localhost:9757' \
  org.meumeu.wivrn.local
```

and the WiVRn-derived OpenXR runtime can now drive a real macOS app:

```sh
MONADO_OPENXR_RUNTIME_PATH=build-macos-host/_deps/monado-build/src/xrt/targets/openxr/libopenxr_wivrn.dylib \
  /tmp/monado-macos-kg/tests/tests_macos_openxr_vulkan_probe
```

In the latest validation run, that probe submitted 60 projection frames
successfully and the Quest client reached decoder startup with:

- `Mediacodec format changed`
- `decoded image size: 1088x1088`
- `Stream scene ready`

The most recent Apple x264 diagnostics on April 3, 2026 narrowed the remaining
uncertainty:

- the host now logs completed x264 frames for streams `0` and `1`
- those runs reach `next_mb == num_mb`, so the Apple left/right encode path is
  producing complete frames rather than stalling mid-frame
- the current client-side startup warning is now stream-indexed:
  - `stream 1 frame 6 was not sent because no shard was received`

So the old theory "nothing is being encoded" is no longer the best match for
the evidence. The remaining Apple x264 issue is narrower:

- Quest decoder bring-up succeeds and reaches `Stream scene ready`
- left/right color encode is alive on macOS
- direct probe readback shows the app's own swapchain image contains nonzero
  pixels before `xrEndFrame`
- the Apple x264 path still sees all-zero RGBA after submission
- that same black result reproduces for both projection-layer and quad-layer
  submission

That old theory is no longer the current best explanation. The next Apple
runtime checkpoint changed the picture:

- the macOS Monado fork now skips the MoltenVK queue-idle path that was
  deadlocking WiVRn startup during swapchain creation
- the old Apple swapchain-create timeout in `do_post_create_vulkan_setup()`
  is also bypassed for the current IOSurface path
- default stereo array swapchains now fail later with a clear Metal assertion:
  `IOSurface texture: must be of type MTLTextureType2D, textureType (MTLTextureType2DArray) disallowed`
- with `MACOS_OPENXR_VULKAN_PROBE_PER_VIEW_SWAPCHAINS=1`, that array-texture
  assertion is avoided and the runtime gets deeper into the probe path
- the current live stall on that per-view path is now in the probe's own
  `clear_swapchain_image()` fence wait on MoltenVK

So the current blocker is now Apple swapchain semantics, not the old
post-submit black-frame theory:

- real array swapchains still need an Apple-compatible IOSurface path
- the current probe-side per-view fallback still needs a MoltenVK-safe clear
  path before visible in-headset validation can resume

One more Apple-only runtime quirk then showed up during repeated host restarts:

- the compositor IPC socket path could disappear from the filesystem even while
  `wivrn-server-headless` still had it open
- in that state, `libopenxr_wivrn.dylib` failed with
  `XR_ERROR_RUNTIME_UNAVAILABLE`

The current local workaround in the macOS Monado fork is to preserve the Apple
IPC socket pathname during this port spike instead of unlinking it at teardown.
With that in place, the runtime path works again and this now succeeds on the
reference machine:

```sh
MONADO_OPENXR_RUNTIME_PATH=build-macos-host/_deps/monado-build/src/xrt/targets/openxr/libopenxr_wivrn.dylib \
  MACOS_OPENXR_VULKAN_PROBE_PER_VIEW_SWAPCHAINS=1 \
  MACOS_OPENXR_VULKAN_PROBE_FRAMES=120 \
  /tmp/monado-macos-kg/tests/tests_macos_openxr_vulkan_probe
```

which again submits 120 projection frames through the live WiVRn runtime and
leaves the Quest client at decoder startup plus `Stream scene ready`.

The next blockers are no longer handshake or `vkCreateImage` bring-up. They
are:

- stream continuity and visible in-headset confirmation on the Apple x264 path
- restoring the third alpha stream on an Apple-friendly pre-encode layout

# Dashboard

The WiVRn dashboard requires Qt6, and the WiVRn server.

## Compile

From your checkout directory, compile both the server and the dashboard:
```bash
cmake -B build-dashboard . -GNinja -DWIVRN_BUILD_CLIENT=OFF -DWIVRN_BUILD_SERVER=ON -DWIVRN_BUILD_DASHBOARD=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-dashboard
```

See [Server](#server-pc) for the server compile options.

# Client (headset)

#### Build dependencies
As Arch package names: git pkgconf glslang cmake jdk17-openjdk librsvg cli11 ktx_software-git ([AUR](https://aur.archlinux.org/packages/ktx_software-git))

OpenSSL build dependencies are also needed, as described [here](https://github.com/openssl/openssl/blob/master/INSTALL.md#prerequisites), in particular perl 5.

#### Android environment
Download [sdkmanager](https://developer.android.com/tools/sdkmanager) commandline tool and extract it to any directory.
Create your `ANDROID_HOME` directory, for instance `~/Android`.

Review and accept the licenses with
```bash
sdkmanager --sdk_root="${HOME}/Android" --licenses
```

Install the correct cmake version with
```bash
sdkmanager --install "cmake;3.31.5"
```

#### Apk signing
Your device may refuse to install an unsigned apk, so you must create signing keys before building the client
```
# Create key, then enter the password and other information that the tool asks for
keytool -genkey -v -keystore ks.keystore -alias default_key -keyalg RSA -keysize 2048 -validity 10000

# Substitute your password that you entered in the command above instead of YOUR_PASSWORD
echo signingKeyPassword="YOUR_PASSWORD" > gradle.properties
```
Once you have generated the keys, the apk will be automatically signed at build time

#### Client build
From the main directory.
```bash
export ANDROID_HOME=~/Android
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk/

./gradlew assembleRelease
```

Outputs will be in `build/outputs/apk/release/WiVRn-release.apk`

#### Install apk with adb
Before using adb you must enable usb debugging on your device:
 * Pico - https://developer.picoxr.com/document/unity-openxr/set-up-the-development-environment/ (see first step)
 * Quest - https://developer.oculus.com/documentation/unity/unity-env-device-setup/#headset-setup (see "Set Up Meta Headset" and "Test an App on Headset" until step 4)

Also add your device in udev rules: https://wiki.archlinux.org/title/Android_Debug_Bridge#Adding_udev_rules

Then connect the device via usb to your computer and execute the following commands
```
# Start adb server
adb start-server

# Check if the device is connected
adb devices

# Install apk
adb install build/outputs/apk/release/WiVRn-release.apk

# When you're done, you can stop the adb server
adb kill-server
```
