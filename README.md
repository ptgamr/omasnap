# Omasnap

A native Wayland screenshot and annotation overlay designed for Omarchy and Hyprland.
It captures the focused monitor before mapping an exclusive layer-shell surface, so the
editor never appears in its own screenshot. The editor retains annotations as movable,
resizable vector layers and preserves the monitor's native pixels on scaled displays.

[![Looping Omasnap demonstration](assets/omasnap.gif)](assets/omasnap.mp4)

## Features

- Freeform region, window, and full-monitor capture modes.
- A pointer-side readout that turns any drag into a ruler: the pointer position
  while the crosshair is idle, then the frame size in native export pixels while a
  region, a hovered window, or a crop handle is being sized.
- Window capture is a crop of the focused-monitor frame. Overlapping windows stay
  visible; there is no second clean-window recapture.
- Select/move/resize layers, mouse-wheel scaling, and eight external recropping handles.
- Draw, type, resize, or carry a layer past the screenshot edge to grow the canvas.
  Framed growth is the default; `G` cycles to tight Overflow growth (only the
  sides needed by annotations, with no frame), then Image (the original canvas
  size, clipping every outside annotation). `Shift+G` cycles backward without
  changing layer geometry. New framed strips start in window gray with the
  original screenshot's card shadow. `B` cycles through the colorful backdrops,
  shadowed and flat window gray, and Off so a background can always be removed.
  Overflow with no backdrop leaves its added pixels transparent. `Shift+B`
  toggles the current shadow directly, and undo/delete can contract grown strips.
- Arrows, straight lines, smoothed freehand strokes, and translucent highlighter
  strokes that automatically match and stay straight across screenshot text (with
  freehand fallback), plus hollow or filled rectangles (optionally rounded) and
  ellipses, numbered markers, editable text in Neucha, JetBrains Mono, or Inter
  Display (plain, outlined, or on a readability pill), and secure redaction with
  opaque or randomized non-spatial mosaic output.
- Per-layer preset or custom colors (including highlighter ink), undo/redo history,
  one-click whole-image or drag-region OCR (the recognized text is shown beside
  the image and copied to the clipboard),
  mesh-gradient backdrops, and rendered drop shadows on standard backdrop cards.
- Cut tool: drag across a band of the image to remove it and collapse the gap, with a
  live preview and dashed seam marker while dragging; annotations shift to follow.
- Pin a finished capture as a bottom-right always-on-top layer surface, launched
  from the same `omasnap` executable and visible on every workspace.
- Crash-resistant working documents under `/run/user/<UID>/omasnap/` (falling back to
  a private `/tmp/omasnap-<UID>/`): the original source image plus a sidecar JSON
  operation log. Undo still works after a crash or `--file` reopen. Saving and
  copying write a normal flattened PNG to the clipboard or `~/Pictures/Screenshots`.
- Verified PNG clipboard output through `wl-copy`/`wl-paste`, plus timestamped files
  under `~/Pictures/Screenshots` by default.
- Open an image already on the clipboard directly in the annotation editor.
- A recents shelf: the select overlay stacks small cards of the last five captures
  along the right edge; hover to fan them out, click one to reopen it in the editor
  with its layers still editable instead of taking a new screenshot.
- Correct native-pixel export on fractional or integer-scaled monitors.

## Platform scope

The supported target is **Wayland + Hyprland**, with Omarchy as the primary integration.
The renderer, layer surface, clipboard, and monitor capture use Wayland protocols;
monitor/window discovery currently calls `hyprctl`. The focused output is captured
in-process through `ext-image-copy-capture` before the layer maps. Selection displays
that captured frame, while the annotation editor uses
a translucent layer scrim over the live desktop and draws only the selected capture.
Another Wayland compositor could support the application after supplying equivalent
monitor and window discovery; generic Wayland support is not claimed by 1.0.

Runtime commands used by the application:

- `hyprctl`
- `wl-copy` and `wl-paste`
- `tesseract`
- `omarchy-notification-send` when available; saved captures include a thumbnail and
  reopen in Omasnap when clicked. Notification failure does not invalidate output.
- `gpu-screen-recorder` and `ffmpeg`, for `--record` only. Neither is needed to take a
  screenshot, and neither is linked into the `omasnap` binary.

## Install on Omarchy

Clone the repository and run the Omarchy installer:

```bash
git clone https://github.com/tobi/omasnap.git
cd omasnap
./install-omarchy
```

The installer uses Omarchy's package helper for missing dependencies, builds in
`~/.cache/omasnap`, and installs under `~/.local`. It does not modify
Hyprland configuration.

### Hyprland binding

Paste this into a Lua config loaded after `require("default.hypr.omarchy")`:

```lua
hl.unbind("PRINT")
hl.unbind("F12")
hl.unbind("ALT + SHIFT + 4")

o.bind("PRINT", "Screenshot", "omasnap")
o.bind("F12", "Screenshot", "omasnap")
o.bind("ALT + SHIFT + 4", "Screenshot", "omasnap")

hl.layer_rule({
  match = { namespace = "^omasnap$" },
  no_anim = true,
  animation = "none",
  no_screen_share = true,
})

-- The recording indicator is a separate namespace. Note that
-- no_screen_share only hides it from compositor-mediated capture
-- (screenshots, screen sharing) -- NOT from gpu-screen-recorder's default
-- KMS capture, which reads the scanout directly. See "Known gaps".
hl.layer_rule({
  match = { namespace = "^omasnap-record$" },
  no_anim = true,
  animation = "none",
  no_screen_share = true,
})
```

Each of these keys toggles: the first press opens the overlay, the next press dismisses it.

Bind recording the same way if you want it on a key:

```lua
o.bind("SUPER + SHIFT + R", "Record region", "omasnap region --record")
o.bind("SUPER + SHIFT + S", "Stop recording", "omasnap --record --stop")
```

Apply and verify:

```bash
hyprctl reload
hyprctl configerrors
hyprctl binds -j | jq -c \
  '[.[] | select(.description == "Screenshot") | {modmask,key,description}]'
```

`omarchy plugin add` is intentionally not used. Omarchy plugins are Quickshell QML
extensions; they do not install native executables or system packages.

Set `OMASNAP_PREFIX` before running `install-omarchy` to use a prefix other than
`~/.local`.

### Studio only on Ubuntu

For the video editor without the screenshot/recorder dependencies, use the
independent [Studio build and Ubuntu 24.04 instructions](studio/README.md).
It configures with `cmake -S studio -B build-studio`, using Qt 6.8+ installed
alongside Ubuntu's system Qt. The existing root build is unchanged.

### Manual Arch Linux build

Install the complete build/runtime dependency set:

```bash
sudo pacman -S --needed \
  base-devel cmake ninja pkgconf qt6-base layer-shell-qt \
  wayland wayland-protocols hyprland wl-clipboard \
  tesseract tesseract-data-eng \
  qt6-multimedia gpu-screen-recorder ffmpeg
```

The last line is the recording half: `qt6-multimedia` builds `omasnap-studio`, and
`gpu-screen-recorder` and `ffmpeg` are run as subprocesses when you record. Leave it
out and screenshots build and work exactly as before.

Build and install:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel
cmake --install build
```

The install step places:

- `~/.local/bin/omasnap`
- `~/.local/bin/omasnap-studio`
- `~/.local/share/applications/omasnap.desktop`
- `~/.local/share/applications/omasnap-studio.desktop`
- `~/.local/share/licenses/omasnap/Neucha-OFL.txt`
- `~/.local/share/licenses/omasnap/JetBrainsMono-OFL.txt`
- `~/.local/share/licenses/omasnap/Inter-OFL.txt`

Ensure `~/.local/bin` is on `PATH`, then verify the installed CLI:

```bash
omasnap --version
omasnap --help
```

## CLI capture modes

Running without arguments opens freeform region selection:

```bash
omasnap
```

Explicit starting modes:

```bash
omasnap --capture-region
omasnap --capture-window
omasnap --capture-fullscreen
```

Scroll capture stitches a region that is taller (or wider) than the screen:

```bash
omasnap --scroll
```

Drag a region, then pick a direction: **Scroll ↓ / →** scrolls the page
yourself while omasnap captures each step, and **Auto ↓ / →** scrolls it for
you, one acknowledged notch at a time, stopping when the page stops moving.
The frames are aligned and stitched into one image and opened in the editor,
where `Ctrl`+wheel zooms and the wheel scrolls it.

Compatibility positional names are also accepted:

```bash
omasnap region
omasnap windows
omasnap fullscreen
omasnap smart       # maps to region selection
```

These options choose what is initially selected; the editor still controls whether the
result is copied, saved, or both.

Quick output skips the annotation editor. Add `--copy` to copy only, `--save` to save
only, or both flags to copy and save. Region and window captures output after selection;
fullscreen captures output immediately. Quick output cannot be combined with `--file`,
`--clipboard`, or `--pin`.

## Recording

Add `--record` to any capture mode to record that target as video instead of
screenshotting it:

```bash
omasnap region --record       # drag the area to record
omasnap windows --record      # pick a window; records the screen rectangle it occupies
omasnap fullscreen --record   # the focused display, no selector at all
```

Sound is off unless you ask for it. `--audio` adds desktop sound, `--mic` adds the
microphone, and `--fps` sets the frame rate (default 60):

```bash
omasnap region --record --audio --mic --fps 30
```

While recording, a small pill sits under the top bar on the recorded display showing
the elapsed time, with pause and stop buttons. That indicator is the point: nothing
records without something on screen saying so. Stop it from the pill, or from a key:

```bash
omasnap --record --stop
```

Logging out finishes the recording rather than abandoning it, and a recording
interrupted harder than that is promoted to a playable file the next time you record.

Recordings land in `~/Videos/Recordings` as
`recording-<date>_<time>-<what>.mp4`, owner-readable only. The notification that
follows opens the recording in **OmaSnap Studio**.

## Studio

```bash
omasnap-studio ~/Videos/Recordings/recording-2026-09-04_13-30-58-hdmi-a-1.mp4
omasnap-studio ~/Downloads/from-my-ipad.mov     # any file, not just ours
```

**Zoom.** Click the picture where you want the camera to go. Inside an existing
cue that re-aims it; anywhere else it starts a new one at the playhead. Cues are
the blocks on the lane under the trim bar — drag a body to move it, an edge to
change how long it runs — and the slider sets how far in the selected one goes.
The right-hand inspector adjusts magnification and ease-in/out duration.
Viewport size and position ease together, with zero velocity and acceleration
at the ends. Touching cues move directly between targets; leave a gap to return
to the full frame. Panning stays within the source.

**Workspace.** The preview sits above a thumbnail timeline and transport bar,
with Canvas, Zoom, and Clip inspector tabs. The inspector collapses in narrow
tiled windows and can be toggled with `Ctrl+\`. The header has undo/redo, export,
and a keyboard shortcut reference.

Studio chrome follows **Omarchy Quattro**: square controls, monospace labels,
and the active Omarchy palette. Palette changes are picked up automatically
within about a second without interrupting playback or edits. Without a usable
Omarchy palette, Studio uses built-in Quattro-style colors. Theme changes affect
only the editor UI—not the saved canvas colors, video corners, or export.
See [Studio design](docs/studio-design.md) for the palette contract.

**Canvas.** Choose a background color, padding, and rounded corners in the
Canvas tab. These are exported along with the zoom and trim. Reset canvas
returns to the original edge-to-edge framing.

**Projects.** Studio stores source assets, ordered clip instances, source ranges,
speed, project-time zoom cues, and canvas settings in one non-destructive document.
Open a video or its `.omasnap.json` project directly. Missing sources expose
**Relink media**; empty projects remain valid. The first source defines the output
canvas (rounded down to even dimensions) and FPS; other sources fit that canvas.
Use **Add scenes** in the Clip inspector or `Ctrl+O` to import more recordings.
Drop local video files onto a marked timeline boundary to insert, or elsewhere
to append. An invalid import batch leaves the project unchanged.

**Arrange scenes.** In Select mode (`V`), drag scene bodies to reorder them;
the ruler still scrubs. Drag a selected scene's edges to trim it, or use the
Clip inspector's source in/out fields. `Ctrl+D` duplicates the selected scene;
Earlier/Later offer precise reorder buttons. Each operation is undoable.
Zooms follow scene content, and duplicates have independent edits. Structural
scene edits reset the project export range to include the whole composition.
`I`/`O`/`R` still set/reset that project-wide range, not individual scene trims.

**Transitions.** Click a timeline boundary badge (`+`, `F`, `B`, `W`, or `S`), or
select a scene and press `T`. Choose Hard cut, Crossfade, Fade through black,
or a Wipe/Slide in any of four directions in the Clip inspector, then edit the
overlap duration. Left means the incoming scene enters from the right; Up means
it enters from below. Wipes reveal a stationary scene; slides move both scenes.
Both preview and export combine the kept source frames and linearly fade their
primary audio. Short clips clamp
the frame-snapped overlap; no discarded footage is used as hidden handles.
Undo restores transition pairs and timing. Scene arrangement reports any
transitions it removes or shortens. Remove a transition before range-cutting
or splitting inside its overlap. Projects now use schema 2; older project files
are not migrated, and original recordings remain untouched.

**Cut passages.** Choose **Range** (`B`), drag over the video lane in either
direction, adjust the range edges, and press Delete/Backspace. The gap closes
in playback and export, including its audio. `V` returns to clip selection
and scrubbing. `S` splits at the playhead; click a scene to select it, then
Delete removes that scene. Undo restores the ranges, selection, and playhead,
even after deleting the final scene. Buttons and a timeline context menu expose
the same actions. Delete prioritizes a selected range, then the selected zoom
or clip; it never removes video merely because the playhead is over it.

**Playback.** A bounded pair of decoders reads the shared composition time map;
video planes are prepared on a worker and rendered with OpenGL;
only the newest pending frame is retained. Scrub requests are coalesced.
Source probing, thumbnail generation, and saving edits also run off the UI thread.

What you see is what you get: the preview and the export are driven by the same
model, and a golden test renders the same frames both ways across seven
scenarios — one cue, adjacent cues, overlapping cues, a cue shorter than its own
ramps, a 30000/1001 source, a trimmed export, and a portrait file with a display
matrix. A recording zoomed past roughly 3× will look soft, because the export
scales up from the cropped region.

Portrait phone recordings work: the container's rotation is read and applied to
the preview the same way ffmpeg applies it before the export's zoom, so a cue
lands on the same thing in both.

Limits worth knowing, all of them measured rather than guessed:

- **40 zooms per project.** The cap bounds ffmpeg expression complexity. The
  golden test verifies a complete track at the cap with ffmpeg itself.
- **Project FPS is fixed.** Rational rates like 30000/1001 are preserved.
  Source timestamps (including VFR) are mapped into project time; export
  normalizes the assembled composition to the project FPS before camera effects.
- **Primary audio track only.** Composition preview/export use the first audio
  stream, converted to stereo on export; silent sources contribute silence.
  Separate mic/system-track mixing is not implemented yet. Additional tracks
  remain untouched in the source file but are not included in composition export.
- **64 clip occurrences per export.** Larger documents can be saved, but export
  reports this limit rather than opening an unbounded number of FFmpeg inputs.
- **Right-angle rotations only.** Anything else is refused with a reason rather
  than framed differently in the two places, because ffmpeg takes a general
  rotation path that a transpose cannot match.
- **Zooming past roughly 3× looks soft**, because the export scales up from the
  cropped region.

**Keyboard.** Transport shortcuts work with buttons, sliders, and inspector
controls focused. Holding Space does not repeatedly toggle playback. Text
fields retain their editing shortcuts; Space still transports from numeric fields.

| Shortcut | Action |
|---|---|
| `Space` | Play / pause |
| `Ctrl+O` | Add scene files (also available in the Clip inspector) |
| `Ctrl+D` | Duplicate the selected scene |
| `T` | Edit the selected scene's transition to its next neighbor |
| `Left` / `Right` | Previous / next frame |
| `Shift+Left` / `Shift+Right` | Seek backward / forward five seconds |
| `Home` / `End` | Go to trim start / end |
| `I` / `O` / `R` | Set trim start / end / reset trim |
| `Z` | Add zoom at playhead |
| `V` / `B` | Select/scrub tool / range-selection tool |
| `S` | Split scene at playhead |
| `Delete` / `Backspace` | Delete selected range, clip, or zoom |
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z` / `Ctrl+Y` | Redo |
| `M` | Mute / unmute preview |
| `Ctrl+S` / `Ctrl+E` | Save edits / export MP4 |
| `Ctrl+\` | Toggle inspector |
| `Escape` / `Ctrl+W` | Clear selection / close Studio |
| `?` / `F1` | Show keyboard shortcuts |

Projects are saved next to the recording as `<recording>.omasnap.json`.
The old `.omasnap-zoom.json` format is not migrated or modified.
The original video stays untouched. Edits are
undoable within a session, and a slider or timeline drag is one undo step.
Closing waits asynchronously for any pending save.

Recording and screenshots are independent: they use separate locks, so taking a
screenshot during a recording does not stop it, and starting a recording while an
annotation overlay is open reports that it is busy (exit 3) rather than throwing that
overlay away.

Requires `gpu-screen-recorder`. `ffmpeg` is optional: with it, the Matroska master is
remuxed to MP4 by stream copy; without it, the `.mkv` is kept as-is. Only one
recording runs at a time, and Omasnap refuses to start one while another screen
recorder is already running — it never signals a recorder it did not start.

### Known gaps

- **The indicator appears in the recording, and the layer rule does not stop it.**
  Measured on this machine at 4K60: with `no_screen_share` applied to the
  `omasnap-record` namespace, a `grim` screenshot has the pill blacked out — the
  compositor honours the rule — while the recorded video shows it in full. That is
  not a bug in the rule. `gpu-screen-recorder`'s default path captures the KMS
  scanout directly, so the compositor never sees the request and cannot exclude
  anything from it. Any layer rule is powerless there, and so is a canary test:
  the fix has to be a capture path that goes through the compositor (GSR's
  `-w portal`) or an indicator placed outside the recorded rectangle. Until then,
  assume the pill is in your recording and trim it in the Studio.
- **`~/Videos/Recordings` is not configurable**, and the frame rate is the only
  recording setting.
- **Coordinates are proven on 1× and 1.5× outputs only.** See
  [docs/recording-targets.md](docs/recording-targets.md) for what was measured and
  what is still open (1.25×, 2×, a rotated output's *region*, negative origins).
- **Studio parity remains incomplete**: multi-source import and arrangement,
  range cuts/splits, fades, directional wipes/slides, playback, trim,
  manual zoom, canvas styling, undo/redo, and MP4 export exist. Automatic
  pointer zoom, camera
  overlays, captions, and masks are not implemented; see
  [docs/recording-studio-plan.md](docs/recording-studio-plan.md) for where those sit.
- **4K preview cadence still needs work.** Real-time timeline progression does
  not guarantee every decoded frame is presented; 4K60 recordings can still
  skip preview frames, including across transitions. See [PLAN.md](PLAN.md).
- **`omasnap-studio` is optional at build time.** Configure with
  `-DOMASNAP_STUDIO=OFF` for a screenshot-only build that needs no Qt Multimedia.
- **A few narrow races remain, all with the same shape: a same-user process
  impersonating one of ours.** Stopping proves the lock holder is an
  `omasnap --record-run` owned by this user and signals it through a pidfd, but
  that is not proof it is *the* recorder that took the lock; recovery proves a
  master is a single-linked 0600 regular file we own, but not that this program
  wrote it; and a second recorder started in the moment after the conflict scan
  still gets through. Closing these properly needs the plan's per-session
  journal.
- **The parent-death guarantee is unchecked.** If `prctl(PR_SET_PDEATHSIG)` is
  denied — a restrictive seccomp profile, say — nothing notices, and an encoder
  could outlive a hard-killed recorder.

### One instance, toggled by the same hotkey

Only one capture overlay runs at a time, guarded by a lock file in the runtime snapshot
directory. Starting omasnap while an overlay is open sends the running instance `SIGTERM`,
which it handles with a clean Qt shutdown; the new process then exits without capturing.
Pressing `PRINT` therefore opens the overlay and pressing it again dismisses it.

Every capture invocation dismisses this way, quick output included: `--copy`/`--save`
while an overlay is open closes the overlay and outputs nothing, rather than screenshotting
the overlay that is still on screen.

Editing an existing image is never cancelled this way: `--file`, `--clipboard`, or an
image path stops the running instance, waits up to two seconds for the lock, and opens the
editor on that image. That is how a pin's Edit button and a notification click always land
in the editor.

A lock left behind by a crashed instance is removed and reclaimed. A lock file that cannot
be read or written at all is reported on stderr instead of being mistaken for a running
instance.

Exit codes:

| Code | Meaning |
|---|---|
| `0` | Success, including dismissing a running overlay |
| `1` | Capture, image, or single-instance lock failure |
| `2` | Usage error |

### Edit an existing or clipboard image

Point omasnap at any readable image and it opens straight into the annotation editor
with the whole image selected, skipping the screen-capture step:

```bash
omasnap ~/Pictures/Screenshots/screenshot-2026-08-11_10-00-00.png
# or
omasnap --file /path/to/capture.png
```

To open the image currently on the Wayland clipboard:

```bash
omasnap --clipboard
```

The clipboard must offer readable image data. Text-only clipboard contents return an
error instead of opening an empty editor.

File URLs are accepted too. A saved capture notification's "Click to edit" action launches
`omasnap` on the finished screenshot, so it can be reopened and re-annotated.

### Recent captures

Every capture finished from the editor (copied, saved, or both) keeps its working
document, source plus operation log, on a shelf of the five most recent under
`~/.local/state/omasnap/recent/` (`OMASNAP_RECENT_DIR` overrides). The select
overlay shows them as a small stack of cards on the right; hovering fans them out
and clicking one reopens that capture in the editor, undo history intact, in place
of a new screenshot. Finishing a reopened capture replaces its shelf entry.

### Configuration (optional)

Omasnap has no settings UI and runs fine with no config at all. If you want to
change where screenshots land or what they are called, create
`~/.config/omasnap/omasnap.conf` (INI format); every key is optional:

```ini
[output]
# Where saved screenshots go. Default: ~/Pictures/Screenshots
directory = ~/Pictures/Captures
# Filename pattern, without extension (.png is appended).
# Default: screenshot-{date}_{time}-{app}
filename = screenshot-{date}_{time}-{app}

[colors]
# Up to eight preset colors for the palette, and the initial custom color.
palette = #ff375f, #ff9f0a, #ffd60a, #30d158, #0a84ff, #bf5af2, #000000, #ffffff
custom = #ff375f

[background]
# An image used as a "Custom" backdrop. B (or the toolbar button) cycles
# through the four gradients, this image when readable, shadowed/flat window
# gray, and Off.
image = ~/Pictures/backdrops/desk.jpg
# Style a fresh capture starts with: none, off, slate, aurora, sunset, lagoon,
# violet, or custom. `none` still allows Framed canvas growth to add its
# automatic window-gray mat; `off` stays transparent. `custom` only takes
# effect once `image` above loads successfully.
default = custom
```

Filename tokens:

| Token | Expands to |
|---|---|
| `{date}` | `2026-08-23` (yyyy-MM-dd) |
| `{time}` | `14-05-09` (HH-mm-ss) |
| `{app}` | Slug of the app under the selection, e.g. `firefox`, `alacritty`, `nautilus` (from the Hyprland window class). Empty for fullscreen captures, file edits, and when nothing is known — the separator before or after it is dropped too, so the default pattern gives `screenshot-2026-08-23_14-05-09.png`. |

The default keeps the date first so the folder always sorts chronologically:
`screenshot-2026-08-23_14-05-09-firefox.png`. Anything else in the pattern is
literal text (`screenshot-` is just a string). A name that already exists
gets `-2`, `-3`, … appended.

Environment overrides (`OMASNAP_SCREENSHOT_DIR` takes precedence over the config):

```bash
OMASNAP_SCREENSHOT_DIR="$HOME/Pictures/Captures" omasnap
OMASNAP_OCR_LANGS="eng+deu" omasnap
# Thai plus English:
OMASNAP_OCR_LANGS="tha+eng" omasnap
```

Install the corresponding Tesseract language data before adding a language to
`OMASNAP_OCR_LANGS`. When unset, omasnap falls back to Omarchy's
`OMARCHY_OCR_LANGS` (which commonly includes the user's script, e.g.
`tha+eng`), then to `eng`.

## Controls

### Capture selection

Tabs across the top of the overlay switch the capture kind: **Region**,
**Window**, **Scrolling Region**, **Fullscreen**. All four are modes of the
same overlay. Scrolling Region selects exactly like Region; once the region is
drawn, the page inside it goes live and the scroll controls appear in place.
Region and Scrolling Region frame the same rectangle, so switching between the
two keeps it: the frame drawn for a scrolling capture is captured as a region,
and a region just captured frames the scroll panel. Window and Fullscreen pick
an area of their own, so switching to either starts over.
The tabs stay up in the editor too: a tab there drops the edit and goes back to
capturing in that mode, and a small **Scroll capture** button under the image
turns the drawn region into a scrolling capture. The keys below do the same
without reaching for the pointer.

| Input | Action |
|---|---|
| Drag | Select a region, with its native pixel size shown at the pointer |
| `Space` | Step through the capture-kind tabs (Region, Window, Scrolling Region) |
| `S` | Toggle scrolling-region mode |
| `R` | Restore the last region drawn this session (same monitor) |
| `SUPER + Arrow` | Move among windows in window mode |
| `Enter` | Capture the highlighted window |
| `Ctrl+A` | Select the full focused monitor (the Fullscreen tab) |
| Hover the right-edge stack | Fan out the five most recent captures; click one to reopen it |
| `Esc` | Dismiss (while selecting; in the editor, `Esc` returns to Select and a second `Esc` closes) |

### Annotation editor

| Input | Action |
|---|---|
| `V` | Select/move/resize layers; carrying one past the source grows the canvas; drag empty canvas for a marquee; multi-select outlines each layer without treating the canvas as one layer; wheel scales the selected layer |
| `A` | Arrow |
| `S` | Spotlight/loupe; press again to cycle ellipse, rectangle, rounded |
| `L` | Straight line |
| `F` | Freehand stroke |
| `H` | Highlighter; Snap mode uses a mouse-following I-beam at the nearby text height, then locks the drag straight to that row. Press `H` again (or click the active toolbar button) for Normal freehand mode, where wheel or `Alt`+wheel changes thickness; Snap keeps detected-row height automatic and wheel sets only its off-text fallback |
| `I` | Eyedropper in the color popover · sample the image as the custom color |
| `C` | Numbered marker |
| `R` | Rectangle; hover the shape button for rectangle, ellipse, and fill controls; `Alt`+wheel rounds corners |
| `E` | Ellipse; shares the shape submenu and filled/hollow toggle |
| `D` | Redact; press again to toggle randomized pixelation or solid redaction |
| `X` | Cut out a band; drag to preview the crossed-out strip, then release to remove and collapse it |
| `T` | Text on a cream readability pill, with Neucha as the default. Click for a one-line label, or drag a box to give it room for several lines: Enter moves to the next line while there is room and commits on the last one; `Shift+Enter` always adds a line; `Esc` commits too but keeps the label selected, so `Backspace` removes it; clicking away keeps the text; press T again to toggle the pill |
| `Shift+T` | Cycle the next or selected text through Neucha, JetBrains Mono, and Inter Display |
| `O` | Recognize and copy all text in the current image |
| `B` | Cycle shadowed colors, window gray (shadowed and flat), and Off |
| `Shift+B` | Toggle the screenshot card's drop shadow; on by default |
| `G` / `Shift+G` | Cycle canvas boundaries forward/backward: Framed, Overflow, Image. Framed auto-grows with the normal frame; Overflow grows only the sides needed by annotations with no frame; Image clips at the original screenshot edge |
| `1`–`8` | Set annotation color; `7` is black and `8` is white |
| Wheel | Scale selected layer, magnify the spotlight under the cursor, or change active tool size (`Alt`+wheel: rectangle corner radius or spotlight border); while just viewing a zoomed capture, scroll it like a document |
| `Shift`+wheel | Scroll a zoomed capture sideways (a wide stitch); never changes the zoom |
| `Ctrl`+wheel · middle-drag | Zoom about the cursor · pan by dragging |
| `+` / `-` / `0` (also with `Ctrl`) | Zoom in / out / fit |
| Hold `Shift` while dragging | Make rectangles, ellipses, and spotlights 1:1; snap lines and arrows to 45°; while dragging a selected layer's handle, keep a rectangle, redaction or spotlight's aspect ratio (lines and arrows: 45°) |
| Hold `Alt` while dragging | Center rectangles, ellipses, and spotlights on the press point; add `Shift` for a centered square/circle |
| `←` `↑` `→` `↓` | Nudge the selected layer 1 px; hold `Shift` for 10 px (a held key is one undo step). With nothing selected, pan a zoomed capture |
| Double-click text · `Enter` on a selected text | Reopen text editing |
| `Delete` | Delete selected layer |
| `Alt+D` | Duplicate selected layer (offset down-left, or away from a nearby edge); the copy becomes the selection |
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z`, `Ctrl+Y` | Redo |
| `Ctrl+C` | Copy PNG only |
| `Ctrl+S` | Save PNG only |
| `Enter` | Copy and save (with a text layer selected: edit it) |
| `P` | Pin the capture on screen and close the editor |
| `Esc` | Return to Select; press again to close |
| Right-click | Return to Select; cancel active drawing |

### Pinned captures

`P` renders the current capture, writes it to a `pin-<pid>-<n>-<random>.png` under
the runtime snapshot directory, and launches the same `omasnap` executable in
detached pin mode. Active pins stack from the bottom-right and can be dragged
by the image background. The layer stays visible on every workspace without
compositor window rules. It preserves the image
aspect ratio, with a maximum width of one third of the screen and a maximum height of one
half.

Pinning neither touches the clipboard nor writes to the screenshot directory; it is a
fourth output alongside copy, save, and copy-and-save. `P` closes the editor and releases
the single-instance lock immediately. Pins from separate captures accumulate as independent
processes.

Hover the pin to reveal its controls:

| Input on a pin | Action |
|---|---|
| Edit button | Reopen the full-resolution PNG in Omasnap and replace the pin |
| Link button | Copy the source file path |
| Copy button, `Ctrl+C` | Copy the full-resolution PNG |
| Double-wide top-left drag handle | Drag the PNG into a file-capable drop target |
| Wheel | Resize within the screen caps, preserving aspect ratio |
| Close button, `Esc`, middle-click | Close |

Image and path copying use `wl-copy` rather than `QClipboard`, so clipboard data remains
available after the pin is closed. No font-based symbol set or compositor-specific window
rule is required; the controls use the same vector icon renderer as the annotation toolbar.

Canvas boundary changes affect only preview and export clipping. The complete vector
geometry stays in the operation log, so switching back to Grow restores every off-canvas
part of a layer.

Creation tools return to Select after one placement without selecting the new layer. In
Select mode, arrows and lines show only their two endpoint handles; other layers show a
selection boundary. The eight blue/white handles outside the image recrop its corners or
edges. After the canvas grows, those crop handles remain on the original source frame.

## Development and verification

```bash
make check
```

The smoke executable exercises region/window/fullscreen startup modes, capture selection,
working-document persistence (source plus op-log JSON), annotation tools, undo/redo
replay, vector movement and scaling, text editing, OCR, native-DPI output,
endpoint-only line selection, annotation-driven canvas growth and clipping policies,
external crop handles,
and the native-pixel
measurement readout on a scaled monitor. A second offscreen binary,
`omasnap-studio-smoke`, covers the Studio's export command and trim timeline without
linking a media stack into the screenshot suite.

For live launch profiling, the binary has an opt-in millisecond trace from `main()`
through the first completed overlay paint:

```bash
OMASNAP_PROFILE_STARTUP=1 ./build/omasnap 2>startup.log
```

The trace also breaks native capture into Wayland registry, buffer allocation, frame wait,
and pixel handoff stages. It is completely silent by default.

`.github/workflows/build-linux.yml` runs the same `make check` build, interaction smoke,
and available static-analysis checks in an Arch Linux container, stages the CMake installation, and uploads a versioned Linux
artifact. A `v*` tag also attaches that artifact to the corresponding GitHub release.

## Acknowledgements

The capture and annotation workflow is inspired by three excellent screenshot tools:

- [Shottr](https://shottr.cc/) — fast region/window capture, OCR, and polished backdrops.
- [Satty](https://github.com/Satty-org/Satty) — a focused, Wayland-native annotation workflow.
- [Flameshot](https://github.com/flameshot-org/flameshot) — selection-first capture and an
  approachable annotation toolbar.

Thanks to their authors and contributors for establishing the interaction patterns that made
this project possible. Omasnap is an independent implementation and is not
affiliated with those projects.

## Project history

This standalone repository was extracted with `git filter-repo` from the original Omarchy
system-customization repository. The former `omasnap/` directory was promoted to
the repository root while retaining its relevant commit history.

The bundled Neucha, JetBrains Mono, and Inter Display fonts are distributed
under the SIL Open Font License. Their provenance and hashes are recorded in
`assets/FONTS.md`; their licenses are installed with the application.
