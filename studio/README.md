# Standalone Studio build

Build the video editor directly from this checkout, independently of the
screenshot/recorder build. The root `CMakeLists.txt`, `Makefile`, and Omarchy
installer are not used or changed by this entry point.

Studio needs Qt **6.8+** (Widgets, Concurrent, Multimedia, MultimediaWidgets,
OpenGLWidgets), a C++23 compiler, CMake 3.24+, and `ffmpeg`/`ffprobe` for media
inspection, thumbnails, and export. Tests additionally require Qt Test.
It does **not** require Hyprland, LayerShellQt, capture protocol XML,
wayland-scanner, gpu-screen-recorder, OCR, or clipboard utilities.
Qt's Wayland platform plugin is still needed for a Wayland desktop.

Without an Omarchy palette, Studio uses its built-in Quattro colors. This is
a standalone **editor**, not a port of screen capture to GNOME or other desktops.

## Ubuntu 24.04 x86_64

These commands are for an Intel/AMD 64-bit machine (`uname -m` prints `x86_64`).
Ubuntu's system Qt 6.4.2 is too old. Install Qt alongside it, not over it.

### 1. Install build tools and runtime dependencies

```bash
sudo apt update
sudo apt install --no-install-recommends \
  build-essential cmake ninja-build python3-venv ca-certificates ffmpeg \
  libgl1-mesa-dev libegl1-mesa-dev libxkbcommon-dev libfontconfig1 libdbus-1-3 \
  libxcb-cursor0 libxkbcommon-x11-0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
  libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libsm6 libice6 \
  libpulse0 libasound2t64 libva2 libva-drm2 libva-x11-2 \
  libwayland-client0 libwayland-cursor0 libwayland-egl1
```

### 2. Install a separate Qt SDK once

[aqtinstall](https://github.com/miurahr/aqtinstall) is an optional setup tool
that downloads Qt's SDK archives; it is not linked into Studio or used when
Studio runs. The virtual environment keeps it out of system Python. Qt 6.8.3
is the reference SDK for these instructions; an existing compatible Qt 6.8+
SDK can be used instead, including one installed with Qt's official installer.

```bash
python3 -m venv "$HOME/.local/share/omasnap-studio/aqt"
"$HOME/.local/share/omasnap-studio/aqt/bin/pip" install aqtinstall
"$HOME/.local/share/omasnap-studio/aqt/bin/python" -m aqt install-qt \
  linux desktop 6.8.3 linux_gcc_64 \
  -O "$HOME/.local/share/omasnap-studio/Qt" -m qtmultimedia
```

The default desktop SDK includes Qt's Wayland plugin. The explicit extra module
is Multimedia. Expect a sizeable SDK download; review Qt's licenses before
redistributing Qt binaries. See the [aqt setup reference](https://github.com/miurahr/aqtinstall/blob/master/docs/getting_started.rst).

### 3. Build from the repository root

```bash
STUDIO_QT="$HOME/.local/share/omasnap-studio/Qt/6.8.3/gcc_64"
"$STUDIO_QT/bin/qmake" -query QT_VERSION

"$STUDIO_QT/bin/qt-cmake" -S studio -B build-studio -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-studio --parallel 4
```

`qt-cmake` selects this SDK explicitly. Your normal `qmake6` and system Qt remain
unchanged. The `build-studio/` directory is separate from the existing `build/`.
If using another Qt SDK, substitute its prefix for `STUDIO_QT`.

### 4. Run

From a Wayland desktop session:

```bash
QT_QPA_PLATFORM=wayland ./build-studio/omasnap-studio /path/to/recording.mp4
# Or reopen a saved project:
QT_QPA_PLATFORM=wayland ./build-studio/omasnap-studio /path/to/project.omasnap.json
```

The recording can come from any recorder. `Space` plays/pauses; `B`, drag,
Delete removes a passage; `Ctrl+Z` undoes; `Ctrl+O` imports scenes; `T` edits a
transition. Run `./build-studio/omasnap-studio --help` for the CLI.

Keep the selected Qt SDK installed: this is a local build, not an AppImage or
other relocatable bundle. Its library search paths refer to that SDK. Avoid
mixing Qt plugins from Ubuntu's Qt 6.4 with the new SDK; if your shell exports
custom `QT_PLUGIN_PATH` or `QT_QPA_PLATFORM_PLUGIN_PATH`, unset those for Studio.

## Tests and optional installation

Enable the same Studio smoke tests used by the main build, without compiling
the screenshot suite:

```bash
"$STUDIO_QT/bin/qt-cmake" -S studio -B build-studio -DBUILD_TESTING=ON
cmake --build build-studio --parallel 4
ctest --test-dir build-studio --output-on-failure
```

The tests run offscreen and include real FFmpeg export checks. They do not
certify GPU/audio behavior on every desktop. A live Wayland run exercises the
GPU paths too:

```bash
QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 ./build-studio/omasnap-studio-smoke
```

Optional user-local installation (no sudo):

```bash
cmake --install build-studio --prefix "$HOME/.local"
```

Only `omasnap-studio` and its desktop entry are installed; no `omasnap` capture
binary, recorder, capture desktop entry, or capture font assets are installed.
The selected Qt SDK must still remain at its original location.

## Maintaining the separate entry point

The version is read from the root CMake file without evaluating that build.
The explicit Studio source/test lists mirror the existing targets: when adding
Studio source files, update both lists. This small duplication keeps the
existing build completely independent. Do not add capture dependencies here.

Validated in an Ubuntu 24.04 x86_64 container with GCC 13.3 and Qt 6.8.3:
standalone configure/build, the complete offscreen Studio suite (including
FFmpeg composition exports), and launching the staged installed binary all
pass. The installation contains only the Studio binary and desktop entry.
The separate `.github/workflows/studio-ubuntu.yml` repeats this path without
changing the existing Arch workflow.

Ubuntu desktop GPU/audio behavior and sustained 4K preview cadence require
machine-specific testing; the existing performance limitation in `PLAN.md`
still applies.
