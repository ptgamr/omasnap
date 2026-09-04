# OmaSnap recording and Studio extension plan

**Status:** proposed companion architecture  
**Research date:** 4 September 2026  
**Plan revision:** 2026-09-04.2  
**Target:** OmaSnap 1.20.1 on Omarchy 4 / Hyprland 0.56  
**Pinned baseline:** [`tobi/omasnap@d339588f3aba554f314d6233da2dddc98f83de55`](https://github.com/tobi/omasnap/tree/d339588f3aba554f314d6233da2dddc98f83de55)  
**Primary constraint:** add recording and a video Studio without rebuilding or slowing down OmaSnap's screenshot path

## Decision

Keep OmaSnap as the screenshot engine and user-facing family name. Add video as an **optional companion package with isolated processes**, not as media code inside the current screenshot process:

- `omasnap` remains the existing fast screenshot/editor/pin executable.
- Preferred integration: `omasnap record` is a tiny pre-Qt dispatcher in a downstream integration branch; it immediately replaces itself with `omasnap-record-ui` before creating `QApplication`, loading screenshot code, or taking the screenshot instance lock. If maintaining that fork is undesirable, ship the same flow as external `omasnap-record`/`omasnap-studio` commands while carrying only the pinned selector patch.
- `omasnap --select-target --json` is the only new screenshot-side capability seam. It reuses OmaSnap's current frozen Region/Window/Fullscreen selector and emits versioned geometry instead of rendering or saving a screenshot.
- `omasnap-record-ui` owns the picker, countdown, and recording controls as a small layer-shell client.
- `omasnap-record-worker` owns one exact `gpu-screen-recorder` child and its per-session IPC socket. It writes a recoverable project; it never uses global `pgrep`/`pkill` ownership.
- `omasnap-studio` is a separate ordinary Wayland `xdg_toplevel` Qt Widgets application backed by MLT. It never inherits OmaSnap's process-wide layer-shell setting.

This is an extension of the OmaSnap workflow, not a screenshot rewrite. Existing capture, annotations, redaction, OCR, scrolling capture, recents, pinning, clipboard behavior, and output remain the acceptance baseline and receive regression tests, not replacements.

### Delivery outlook

- Recording MVP—display/area/portal source, system audio/mic, countdown, pause/stop/discard, minimal crash recovery, direct MP4 export: **5–7 solo developer weeks** including integration contingency.
- Editable Studio beta—multi-clip trim/split/speed, crop/background, manual zoom cues, camera composition, masks/text, proxy playback, shared preview/export graph: **13–22 weeks cumulative** if the MLT spike passes.
- Advanced Better Shot-like recording parity—captions/transcription, automatic zoom, transitions, grading, richer overlays, hardened packaging: **24–42 weeks cumulative**, conditional on input-event and media-framework gates.

Do not publish a firmer advanced-parity date before the Phase 0 MLT, source-sync, and pointer/input spikes.

## Why this direction is materially better

OmaSnap already implements most of the requested screenshot experience:

- Region, window, fullscreen, and stitched scrolling capture.
- Native-pixel output on fractionally scaled displays.
- Movable/resizable vector layers, crop, cut, arrows, lines, freehand, highlighter, shapes, numbered markers, text, spotlight, OCR, eyedropper, backgrounds, shadows, and secure redaction.
- Undoable operation logs, crash-resistant working snapshots, five recent editable captures, clipboard/save output, and pinned all-workspace cards.
- In-process Wayland `ext-image-copy-capture` before its overlay maps, so screenshots do not contain the editor.

Rebuilding those features would duplicate the audited tree's roughly 17,800 lines across C++ source/header files at the pinned commit and discard proven Hyprland-specific behavior. That approximate count is context, not an estimation input. The correct reuse boundary is the existing binary plus one small target-selection contract.

## Adjacent implementation: what to reuse from Omascreen

[`k4ditano/omascreen@efd097d`](https://github.com/k4ditano/omascreen/tree/efd097dee572205675c6ed44b82b94171e123216) is a useful second reference. It is an MIT-licensed Omarchy/Quickshell recorder and editor whose documented feature set includes GSR/wf-recorder capture, separate audio tracks, camera, auto-zoom/cursor trails, keyframes, voiceover, ducking, transcription, and MP4/WebM/GIF export.

Treat it as a **code/reference donor and behavioral test oracle**, not as the process architecture for this plan:

| Reuse candidate | How to use it | What not to inherit |
|---|---|---|
| GSR option/device discovery and argument fixtures | Port small pure parsers/builders after source and license review; compare commands against the installed GSR | Do not launch through a shell or copy process-global ownership |
| Multiple-audio behavior | Turn its scenarios into `ffprobe` integration fixtures for system/mic track policy | Do not infer track separation from UI state |
| Pointer sampling, cursor trail, and auto-zoom algorithms | Use as spike inputs only after the input/privacy gate passes | Do not enable raw input capture in MVP |
| FFmpeg filter-graph ideas | Use for cross-checking MLT export output and isolated fallback experiments | Do not create a second preview graph that only imitates final render |
| Omarchy semantic theme mapping | Reuse its token interpretations where they match the current theme contract | Do not embed Studio in the long-lived Quickshell process |

Omascreen's own architecture notes that preview can be an approximation while final render decides the result. That trade-off is unsuitable for OmaSnap Studio: this plan requires one compiled media graph for both preview and export. Reuse must be selective, attributed in per-file provenance comments plus the bundled third-party notice, and covered by the new companion's tests; do not wholesale-copy the plugin.

## Pinned OmaSnap findings

The following statements are based on the pinned commit, not a moving `main` branch.

| Area | Confirmed current design | Consequence for video |
|---|---|---|
| Language/UI | C++23, Qt 6 Widgets, hand-painted `QPainter` chrome | Keep video UI in C++/Qt Widgets; do not introduce QML solely for Studio |
| Shell surface | `main.cpp` sets `QT_WAYLAND_SHELL_INTEGRATION=layer-shell` process-wide | Studio must be a separate executable to get normal `xdg_toplevel` windows |
| Capture | `surface-capture.cpp` uses `ext-image-copy-capture` with `wl_shm` buffers | Reuse for target preview only; reject it as a video recorder |
| Target discovery | `MonitorInfo`, `WindowTarget`, `probeFocusedMonitor()`, `hyprctl` JSON | Reuse these types to produce a recording-target JSON contract |
| Editor model | `CaptureData` + `OperationLog`; `renderCapture()` is deterministic | Preserve untouched; extract paint primitives later only if video needs the exact styles |
| Threading | `QtConcurrent` + `QFutureWatcher`; UI thread does paint/input only | Every probe, media operation, thumbnail, waveform, proxy, and export follows this rule |
| Pin/drag | Separate layer surface, real `QDrag`, runtime snapshot locks | Reuse interaction patterns and tests, not the screenshot image implementation |
| Single instance | `omasnap.instance` lock; another capture terminates the current overlay | Dispatch recording before this lock; recorder gets a distinct session lock/socket |
| Dependencies | Lean Qt6 + LayerShellQt + Wayland; no multimedia framework | Put GSR/MLT/Qt Multimedia in optional `omasnap-video`, never the base screenshot package |
| Theme | Pinned fonts and explicit hard-coded chrome; GTK platform theme is deliberately bypassed | Add a lightweight Omarchy token reader for video; do not load GTK or alter screenshot startup |
| License | MIT | Companion code can remain MIT; dynamically linked MLT is LGPL-2.1-only locally; GSR remains a GPL subprocess |

The repository's [agent guide](https://github.com/tobi/omasnap/blob/d339588f3aba554f314d6233da2dddc98f83de55/AGENTS.md) explicitly says the upstream project is a specialized screenshot tool, a single small binary, and dependency-lean. A large recorder/Studio change is therefore unlikely to belong upstream. The plan assumes either:

1. a downstream OmaSnap integration branch containing only the dispatch and target-selection seams; and
2. an independently packaged `omasnap-video` companion that can track upstream OmaSnap releases.

A narrow `--select-target` contribution may be proposed upstream only if maintainers want it. Video delivery must not depend on that acceptance.

## Scope

### In scope

- Record a named display, selected area, window-shaped area, or portal-selected source.
- Hardware-accelerated capture through `gpu-screen-recorder` with an owned PID and IPC socket.
- System audio and microphone, then a separate editable camera track after synchronization passes.
- Countdown, elapsed time, pause/resume, restart, stop, discard, clear error states.
- Recoverable raw masters, project journal, thumbnails/waveforms, proxy media, and MP4 export.
- Studio with clip import/reorder/trim/split/speed, crop, background, manual zoom cues, camera placement, masks, text, captions, audio mixing, transitions, and color adjustments in phases.
- Omarchy-adaptive colors and metrics for all new video surfaces.
- Existing OmaSnap screenshot behavior and latency as protected regression criteria.

### Explicitly not in the first recording release

- Reimplementing or replacing any screenshot annotation capability.
- Loading recorder, codecs, MLT, FFmpeg libraries, or Studio UI into `omasnap`.
- A generic multi-compositor/Linux abstraction; OmaSnap remains Hyprland/Omarchy-first.
- Silent Hyprland config edits.
- Raw global keylogging or evdev group changes.
- Automatic click zoom before a safe event source exists.
- Cloud upload or R2 sharing; it is a separate product/security phase after local Studio is stable.
- A general-purpose Kdenlive replacement.

## Process and package architecture

```text
Hyprland binding / desktop action
              │
              ▼
     omasnap record …
  pre-QApplication exec handoff
              │
              ▼
┌────────────────────────── omasnap-video ──────────────────────────┐
│                                                                  │
│  omasnap-record-ui         omasnap-record-worker                  │
│  layer-shell picker/bar ── private QLocalSocket ── session state │
│          │                             │                           │
│          │ target request              ├─ exact GSR PID + IPC      │
│          ▼                             ├─ optional camera child    │
│  omasnap --select-target               └─ project/journal          │
│  frozen existing selector                      │                   │
│                                                ▼                   │
│                                      omasnap-studio                │
│                                      xdg_toplevel + MLT            │
└──────────────────────────────────────────────────────────────────┘

Unchanged screenshot path:
omasnap → existing capture/editor/recents/pin/output
```

### Why three video processes

| Process | Failure boundary | What it may load |
|---|---|---|
| `omasnap-record-ui` | A picker/bar bug cannot corrupt the master | Qt Widgets, LayerShellQt, tiny shared chrome/theme code |
| `omasnap-record-worker` | Encoder ownership and recovery survive Studio/UI faults | QCoreApplication, GSR subprocess, IPC, project writer; no QWidget |
| `omasnap-studio` | Codec/effect failures cannot take down screenshot capture or the recording supervisor | Qt Widgets, MLT, FFmpeg-backed MLT services, optional Qt Multimedia |

If the control UI disconnects unexpectedly, the worker stops and finalizes the current segment within two seconds. Recording never continues without a visible indicator. On restart, the UI reads the journal and offers recovery.

## Minimal changes to pinned OmaSnap

### 1. Pre-Qt command dispatch in `src/main.cpp`

Detect `record` and `studio` before setting `QT_WAYLAND_SHELL_INTEGRATION`, constructing `QApplication`, loading fonts, or acquiring `omasnap.instance`:

```text
omasnap record [--display|--area|--window|--portal]
omasnap record --stop
omasnap studio [PROJECT|MEDIA]
```

The dispatcher resolves an allowlisted sibling beside the installed `omasnap` executable and uses an argument-vector `exec`, never `sh -c`. If `omasnap-video` is absent, print one actionable install message. Screenshot startup links no new library and executes no new branch beyond a cheap first-argument comparison.

If the integration is maintained completely outside upstream, expose the identical functions as `omasnap-record` and `omasnap-studio`; the optional dispatcher is the only fork delta.

### 2. Target-only mode in `src/main.cpp`, `src/editor.*`, and new `src/capture-target.*`

Add:

```text
omasnap --select-target --json [region|window|fullscreen]
```

This mode reuses the existing captured monitor image, `MonitorInfo`, `WindowTarget`, selection overlay, fractional-scale transforms, and keyboard behavior. Accepting a target:

- emits exactly one JSON object on stdout; diagnostics remain on stderr;
- never copies/saves PNG data;
- never creates recents or a working operation log;
- never enters annotation/export phase;
- exits non-zero with distinct cancellation, busy, and preempted-by-screenshot results;
- uses a non-preempting acquire against the existing screenshot instance lock: if an annotation/selector overlay already exists, recording selection reports busy and does not terminate it;
- once target selection owns that lock, a subsequently invoked normal screenshot keeps OmaSnap's existing handover behavior and may preempt the selector; the recorder returns to its picker and has not started GSR, so no capture is lost.

Version 1 schema:

```json
{
  "schema": 1,
  "kind": "region",
  "output": "DP-1",
  "workspace": 2,
  "scale": 1.5,
  "transform": 0,
  "logical": { "x": 120, "y": 80, "width": 1280, "height": 720 },
  "globalLogical": { "x": -1800, "y": 80, "width": 1280, "height": 720 },
  "sourcePixels": { "x": 180, "y": 120, "width": 1920, "height": 1080 },
  "window": null
}
```

For `window`, include stable ID, class, title, and the current OmaSnap crop geometry. That mode records the visible screen rectangle, including occlusion, matching OmaSnap screenshot semantics. A separate `portal` choice provides compositor-isolated window selection where supported.

The GSR region coordinate space must be proven on 1×, 1.25×, 1.5×, 2×, transformed, and negative-origin outputs; never assume it equals either JSON rectangle.

### 3. Small shared UI target

Split only reusable, low-risk pieces into `omasnap-ui-core`:

- `overlay-chrome.*` fonts/layout primitives;
- `icons.*` vector symbols;
- a new `omarchy-theme.*` semantic token provider;
- common bounded-process and private-runtime helpers if extraction is behavior-preserving.

Do not link `omasnap-studio` to the current monolithic `omasnap-core`: that would drag screenshot capture, editor, Wayland protocols, and layer-shell assumptions into Studio. Any extraction must keep screenshot golden/smoke output byte-identical and pass the A/B startup protocol in the performance section or be reverted.

### 4. Files deliberately left alone

- `surface-capture.cpp`: remains optimized for screenshot/scroll capture; it ignores DMA-BUF, presentation timestamps, and damage callbacks and copies `wl_shm` frames, so it is not a video backend.
- `capture.cpp` / `renderCapture()`: no video loop and no change to redaction semantics.
- `pin.cpp`, recent snapshots, output configuration, OCR, scrolling capture: no video responsibilities.
- `instance-lock.*`: screenshot handover semantics remain unchanged; video uses separate locks.

## Recording experience

### Picker

A compact bottom-center `omasnap-record` layer surface uses the same typography, icon weight, status pills, spacing, and restrained motion as OmaSnap:

```text
[Display] [Area] [Window area] [Portal…] │ [System] [Mic] [Camera] [Timer] [Record]
```

- `Display`: focused display by default; a menu lists named outputs.
- `Area`: launches the target-only Region selector.
- `Window area`: launches the existing Window selector and records its visible rectangle.
- `Portal…`: invokes GSR's XDG portal source for a compositor-selected display/window; portal consent UI is expected.
- Audio is off until the user explicitly enables system sound and/or mic. After that first choice, the picker may remember the last successful devices while keeping a clear pre-record indicator.
- Camera is hidden behind the Phase 3 capability gate; it is never burned into the only master.
- Timer offers Off, 3, 5, and 10 seconds.

The picker should be usable without a settings window. Durable exceptions live in a small `video.conf`; the screenshot `omasnap.conf` remains unchanged.

### Recording state

```text
● 00:03:17  [Pause] [Restart] [Stop] [Discard]
```

- The control bar stays visible and excluded from recording.
- Pause uses GSR's idempotent `set-paused` IPC command, not signal toggling.
- Stop waits for the GSR `stop` reply and validated saved path before opening Studio.
- Restart finalizes and preserves the current segment as a prior take, then starts a new owned session. “Discard take and restart” is a separate destructive action with confirmation.
- Discard stops first, confirms the worker has exited, then removes only the exact current project.
- Any lost encoder/socket/device state turns the bar urgent and offers Reveal logs, Recover project, or Dismiss; it never shows a fake running timer.

### Capture exclusion and user config

OmaSnap's README already documents an Omarchy Lua layer rule with `no_screen_share = true` for namespace `omasnap`. Video uses a separate stable namespace:

```lua
hl.layer_rule({
  match = { namespace = "^omasnap-record$" },
  no_anim = true,
  animation = "none",
  no_screen_share = true,
})
```

The Omarchy Lua field emits Hyprland's `noscreenshare` layer rule. Installation shows this exact user-owned snippet, previews the target path, and asks the user to apply it; it never edits `/usr/share/omarchy` or silently replaces bindings.

Every enabled display/area/portal backend must pass a canary-frame pixel test showing the controls are absent, not blacked out. A failed probe disables recording with a setup action rather than failing open.

## Recorder implementation

### Capability probe

At startup of the video picker—not screenshot OmaSnap—run bounded probes on a worker:

- `gpu-screen-recorder --version` and `--list-capture-options`;
- `--list-monitors`, `--list-audio-devices`, and `--list-v4l2-devices` only when their UI is opened;
- available codec/encoder/container combinations;
- free disk space and output writability;
- presence/version of MLT for “Open Studio.”

This workstation currently has `gpu-screen-recorder 6.0.1`, FFmpeg 9.0.1, Qt Multimedia 6.11.2, MLT 7.40.0, and Frei0r 3.4.0. GStreamer Editing Services is not installed. MLT is therefore the preferred Studio spike, not an unproven custom NLE.

### Owned GSR session

Construct one argument array, with no shell interpolation. The initial shape is:

```text
gpu-screen-recorder
  -w <output|region|portal>
  [-region <WxH+X+Y>]
  -c mkv
  -k h264
  -f 60
  -fm <cfr|vfr>
  -cursor yes
  -exclude-metadata yes
  -write-first-frame-ts yes
  -ipc <private-project-socket>
  [-a <system-source>]
  [-a <mic-source>]
  -o <project>/source/screen.part.mkv
```

This is a candidate command shape, not a frozen invocation. Phase 0 verifies every flag against installed GSR 6.0.1, compares CFR's editing predictability with VFR's capture efficiency, and records the chosen timing policy in the project. It also verifies whether multiple `-a` inputs become separate tracks. If not, use a single mixed MVP track or a synchronized PipeWire/GStreamer audio helper; do not claim editable track separation without `ffprobe` evidence. Evaluate `-k auto` and `-fallback-cpu-encoding yes` as capability-gated fallbacks rather than assuming a particular GPU encoder.

If `-write-first-frame-ts` is unavailable, derive the first decoded PTS from each container and map it to the journal's monotonic barrier; editable camera remains disabled unless the synchronization gate still passes. If `-exclude-metadata` is unavailable, keep the active `.part.mkv` private and strip global/stream/chapter metadata during validated finalization before exposing `screen.mkv` or any export.

Use Matroska masters for interruption tolerance and remux/export MP4 after a normal stop. The worker stores GSR PID, process start time, executable identity, IPC path, capability snapshot, and output path in its journal. Commands are newline-delimited bounded JSON over the unique GSR IPC socket; every request ID receives and validates its reply.

The worker sets a Linux parent-death signal for the GSR child so an abrupt worker exit requests encoder termination instead of leaving an invisible orphan. The surviving record UI immediately starts a recovery worker. It reads only the exact journal, verifies same UID plus PID, `/proc` start time, executable, socket and project output identity, then reconnects and either re-adopts long enough to stop/finalize or confirms the child has exited. It never adopts from a name-only process scan. If the whole UI/worker tree dies, parent-death handling stops GSR and the next launch validates the stale `.part.mkv`.

Finalization stops GSR, waits for exit, validates streams/duration with `ffprobe`, performs any required metadata-clean remux, then atomically promotes `screen.part.mkv` to immutable `screen.mkv`. On reopen, a stale `.part.mkv` is never called finalized: recovery validates and offers Recover or Discard, preserving the original until a replacement passes validation.

### Worker/UI contract

`omasnap-record-worker` creates a mode-`0700` session directory and a same-user `QLocalServer` socket under `$XDG_RUNTIME_DIR/omasnap-video/`. Messages are bounded enums, never executable strings:

```text
Prepare(config) → Prepared(capabilities, projectId)
Start           → Recording(monotonicStart)
SetPaused(bool) → Paused(bool, projectTime)
Status          → state, elapsed, bytes, dropped/lag counters
Stop            → Finalized(projectPath)
Discard         → Discarded
```

The worker validates peer UID, message size, path containment, state transitions, and one-controller ownership. A UI disconnect starts the two-second finalize-and-stop safety path. A worker disconnect triggers the exact-journal recovery path above. Only the exact child PID/process group and unique IPC socket are controlled.

Before starting, the worker performs read-only process/status inspection to detect an active stock Omarchy recorder and refuses concurrent recording with an actionable message. This global inspection is conflict detection only: it never signals, pauses, adopts, or stops a discovered process. The installed Omarchy path currently relies on broad GSR discovery for parts of its lifecycle, so parallel operation could let another controller affect this session. Remove this exclusion only after both implementations have scoped ownership and an automated coexistence test proves that either recorder cannot pause or stop the other.

### Audio and camera synchronization

- Prefer screen + system audio + microphone in the same GSR mux when it preserves separately addressable tracks and timestamps.
- Camera remains a separate master. Use a separate GSR V4L2/Qt Multimedia capture only after a shared `CLOCK_MONOTONIC` barrier and `-write-first-frame-ts`/equivalent timestamp alignment are proven.
- Persist actual PTS and first-buffer offsets; never align tracks by process-spawn time.
- Accept bounded audio resampling; never silently stretch/drop video to hide drift.
- Gate on start offset below one video frame and end-to-end A/V drift below 50 ms after 30 minutes, including pause/resume.
- If the gate fails, ship without editable camera rather than burning camera into the screen master.

### Cursor, clicks, and keys

Recording MVP uses GSR's embedded cursor and manual Studio zoom cues. That is intentionally less editable but safe.

The current portal lacks cursor-metadata mode and GSR's KMS path does not expose a separate editable pointer stream. A later pointer sampler must pass explicit compositor-overhead and timeout gates. Click and keystroke overlays remain off until a permission-scoped source exists. If keystrokes ever ship, persist only allowlisted semantic shortcut tokens; discard printable text, paste, compose/input-method data, raw scan codes, and every event while a lock/auth surface is active.

## Recording project contract

```text
~/.local/share/omasnap/video/projects/<uuid>.omasnap-video/
├── manifest.json             schema, source, clocks, capabilities, state
├── source/
│   ├── screen.mkv            immutable finalized master
│   ├── screen.part.mkv       active/recoverable master only
│   ├── camera.mkv            optional immutable camera master
│   └── external-audio.*      optional imported soundtrack
├── edit/
│   ├── studio.json           explicit save
│   └── autosave.json         atomic draft
├── events/
│   ├── pointer.jsonl         absent in MVP
│   └── shortcuts.jsonl       semantic tokens only, absent by default
├── cache/
│   ├── proxy-720p.mkv
│   ├── thumbnails/
│   └── waveforms/
└── recovery/
    └── journal.jsonl
```

Rules:

- Directory mode `0700`; persistent files `0600`, independent of umask.
- Source masters are immutable after finalization.
- `manifest.json` and Studio schema are versioned; unknown newer projects open read-only.
- Autosaves use `QSaveFile`/same-directory atomic replacement.
- Only reconstructible caches are auto-pruned.
- Default soft managed-storage warning: 20 GiB; cache cap: 2 GiB; recordings are never silently deleted.
- Before recording, require free space above the greater of 5 GiB or ten minutes at the measured bitrate.
- Project deletion is logical filesystem deletion, not guaranteed secure erasure from SSDs/snapshots.
- Logs contain no pixels, OCR/transcript text, keys, device names, credentials, signed URLs, or full sensitive paths.

## Studio architecture

### Window/toolkit

`omasnap-studio` is an ordinary `xdg_toplevel`, not a layer surface. Use Qt 6 Widgets and C++23 to stay in OmaSnap's implementation language and reuse its worker/watcher discipline. Do not inherit `QT_WAYLAND_SHELL_INTEGRATION=layer-shell`.

```text
┌──────────────────── command bar ────────────────────┐
│ Undo Redo  Project                    Save Export    │
├───────────────────────────────┬─────────────────────┤
│                               │ Inspector           │
│      composited preview       │ clip / scene / audio│
│                               │ captions / export   │
├───────────────────────────────┴─────────────────────┤
│ transport · timecode · zoom                         │
│ clips       [──────][──][────────────]              │
│ camera      [──────────────]                        │
│ zoom/mask   [ cue ]       [ cue ]                   │
│ captions    [ line ][ line ][ line ]                │
│ audio       ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~        │
└─────────────────────────────────────────────────────┘
```

### MLT boundary

Use MLT 7 through its C++ wrapper if the spike passes. MLT provides producers, playlists, multitrack tractors, filters, transitions, consumers, time effects, preview scaling, and headless rendering. It is already installed on the target machine and packaged as LGPL-2.1-only.

OmaSnap's versioned `studio.json` remains the source of truth. A deterministic `TimelineCompiler` builds the same MLT graph for:

1. preview at proxy resolution; and
2. final export from immutable masters.

Do not persist opaque MLT XML as the only project format. Record selected service/filter names and validated parameters so missing plugins produce an actionable degraded/read-only state.

Phase 0 must prove:

- MLT preview embedded in the actual Qt Widgets surface with synchronized audible output, responsive seek, device change/error handling, and one declared audio-clock/device owner;
- frame-accurate seek at 30/60 fps;
- trim, split, reorder, 0.25×–4× speed, and audio pitch policy;
- a camera overlay and one animated zoom transform;
- one blur/pixel mask and text/caption overlay;
- transition timing;
- preview-to-headless-export agreement on a golden ten-second project;
- acceptable GPU/CPU use and Arch dependency footprint.

Phase 0 chooses and documents whether an MLT consumer owns preview audio or decoded audio is bridged to a Qt audio sink; there must be exactly one playback clock and audio-device owner. Re-baseline Phase 4 after this embedded-preview result. If MLT cannot provide a responsive embedded Qt preview with synchronized audio as well as the media graph above, evaluate GStreamer Editing Services. If both fail, stop: a custom NLE is a separate project, not a larger Phase 4.

### Timeline/document model

Store source geometry normalized to each source frame and time as integer ticks on a declared rational timebase. Never store viewport pixels or floating-point seconds as authoritative edit points.

Typed commands cover:

- add/remove/reorder/split/trim clip;
- slip source in/out;
- clip speed and transition;
- crop/background/camera transform;
- zoom, mask, text, caption, and audio cues;
- import/replace media;
- undo/redo transaction grouping.

Every command is serializable and reversible. Undo stores deltas, not decoded frames. Preview and export consume the same compiled scene/timeline graph.

### Preview and proxies

- Create a 720p intra-friendly proxy and waveform asynchronously after capture/import.
- Keep masters untouched and relinkable.
- Decode only the visible playhead vicinity; bound frame and thumbnail caches.
- Coalesce scrub requests; a stale frame result never replaces a newer playhead request.
- Use a custom painted timeline with damaged-region updates, following OmaSnap's 6K pointer-repaint discipline.
- Export always reads masters unless the user explicitly chooses a draft/proxy export.

### Feature increments

1. **Studio foundation:** import/recover recording, playback, seek, one clip, trim, export.
2. **Editing beta:** multi-clip, split/reorder, speed, crop/background, camera placement, manual zoom cues, audio levels.
3. **Evidence/presentation:** timed masks, secure pixelate/redact, text, keystroke tokens, cursor style where a sidecar exists.
4. **Language:** local transcription, editable captions, word highlighting, silence/filler suggestions; destructive transcript cuts remain explicit edit commands.
5. **Polish:** transitions, grading, 3D-style transforms, soundtrack replacement, long-project performance.

“Blur” is cosmetic. A separate secure mask must replace pixels in flattened export. Editable projects retain raw masters and must show that warning; project-package sharing is disabled.

## Omarchy-adaptive visual design

OmaSnap currently bypasses `QT_QPA_PLATFORMTHEME=gtk3` for measured startup reasons and paints explicit colors. Keep that optimization. New video executables use a small `OmarchyTheme` provider, not GTK and not Quickshell imports.

Read:

```text
~/.local/state/omarchy/current/theme/colors.toml
~/.local/state/omarchy/current/theme/shell.toml
~/.local/state/omarchy/current/theme.name
~/.config/omarchy/shell.toml            # user overrides
```

Map semantic roles only:

| Video role | Omarchy source |
|---|---|
| canvas/window | `background`, `dark_background` |
| panel/popover | `popups.background` and border roles |
| primary/muted text | `foreground`, `muted` |
| active tool/playhead | `accent` / selection |
| record/destructive | urgent/red with accessible fallback |
| controls | normal/hover/focus/pressed control roles and alpha |
| metrics | spacing/font roles plus fontconfig `monospace` |

Theme switching replaces the active directory. Watch the parent and marker, debounce, parse one coherent snapshot, atomically swap UI roles, and re-arm watches. Honor light themes, reduced motion, large text, and opaque fallbacks.

The screenshot editor's chrome does not need to change for recording delivery. A later optional “adaptive screenshot chrome” migration may reuse the provider only if startup regresses by less than 5 ms and existing screenshot visual/golden tests are intentionally updated.

## Security and privacy

- All child programs are absolute/allowlisted and receive argument arrays, never shell fragments.
- Recorder and Studio IPC accepts only the same UID, one controller, bounded messages, valid state transitions, and project-contained paths.
- Media metadata is excluded at capture where possible and stripped again from flattened exports.
- Raw masters, audio, camera, transcripts, and any semantic shortcut events are private project data.
- No recording continues without a visible, capture-excluded indicator.
- No printable keystrokes, raw input stream, or broad device-group access.
- Bound media duration, dimensions, frame rate, cache size, JSON depth/count, track/cue counts, and decoded frame memory.
- Decode/proxy/export work never blocks the GUI thread.
- Consider Bubblewrap for MLT/FFmpeg helpers opening untrusted imported media, with no network and narrow mounts.
- Network code is absent from recorder and Studio MVP.
- Export warns before overwrite and uses same-directory temporary output plus validation and atomic rename.
- A secure-redaction export test decodes the result and verifies masked pixels and attachments cannot recover the source region.

## Packaging

### Base package remains lean

`omasnap` keeps its current required set:

```text
qt6-base
layer-shell-qt
wayland / wayland-protocols
wl-clipboard
tesseract (feature-scoped)
```

### Optional `omasnap-video` package

```text
omasnap >= pinned compatible version
gpu-screen-recorder
mlt
ffmpeg
qt6-base
layer-shell-qt
qt6-multimedia       # Studio preview audio/device routing; camera follows in Phase 3
frei0r-plugins       # only when selected effects require it
```

Install sibling executables, desktop actions, the stable `omasnap-record` layer namespace, docs, and user-service metadata if later needed. Do not enable a background daemon by default. Add `install-video` or `install-omarchy --with-video`; the existing default installer remains screenshot-only.

Maintain MIT licensing for new first-party code unless the project owner chooses otherwise. Record any Omascreen-derived code with per-file provenance/copyright headers and a bundled third-party notice. Dynamically link system MLT and comply with LGPL notice/relinking obligations; inventory the selected MLT/Frei0r/FFmpeg runtime modules because optional services and a GPL-enabled system FFmpeg can change distribution obligations. Invoke system GSR as an unmodified subprocess; do not vendor or statically combine it without a fresh license review.

Transcription is not a base dependency. Phase 5 must select a local engine and model license, require an explicit model download/import, pin its version and checksum, show storage size before acquisition, and allow complete removal. The recorder and Studio themselves still make no network requests.

## What the first increment actually shipped

This section is a record of where the implementation stands against the plan
above, written after the fact. It is deliberately specific about the places
the code does something other than what the plan asks for.

Built:

- `omasnap <mode> --record` reusing the frozen selector, `--audio`, `--mic`,
  `--fps`, and `omasnap --record --stop`.
- The target contract (`src/record-target.*`), with the GSR coordinate space
  measured rather than assumed — see [recording-targets.md](recording-targets.md).
- One owned encoder child with a private control socket, pause/stop,
  parent-death handling, and a stop that also answers SIGTERM.
- A capture-excluded indicator, notification, Matroska master remuxed to a
  validated MP4, and next-launch recovery of an abandoned master.
- `omasnap-studio`: playback, a trim range, and an ffmpeg export.

Deliberate deviations:

- **Two processes, not three.** The record UI and the encoder supervisor are
  one process. The plan separates them so a picker bug cannot corrupt the
  master; at this size the "picker" is a pill with two buttons, and the
  master is written by the encoder rather than by us. Killing the recorder
  is covered by the parent-death signal and next-launch recovery, both
  tested. Revisit when the UI grows a settings surface.
- **`omasnap` remains one binary**, with the recorder inside it, because
  nothing it needs is a new library. Only the Studio is split out, and only
  because Qt Multimedia would otherwise be linked into the screenshot path.
- **No project directory yet.** A recording is one file in
  `~/Videos/Recordings`, not a versioned `.omasnap-video` package with a
  journal, proxies, and an edit document. The Studio edits in place and
  exports beside the original.
- **The notification click action is a shell command string**, because that
  is the interface `omarchy-notification-send --exec` offers. It is
  shell-quoted the same way the screenshot path already quotes it.

Known gaps, all of them things the plan asks for:

- **Capture exclusion does not work on the default capture path, and the
  plan's remedy cannot make it work.** Measured: with `no_screen_share` on
  the `omasnap-record` namespace, `grim` blacks the indicator out and the
  recorded video still shows it. `gpu-screen-recorder`'s default path reads
  the KMS scanout, so the compositor is not in the loop and no layer rule
  reaches it. The plan's canary-frame test would therefore fail on every
  supported display path rather than catching a misconfiguration, and its
  "failed probe disables recording" rule would disable recording outright.
  The real options are a compositor-mediated source (`-w portal`, which
  costs a consent dialog and a stored session token) or keeping the
  indicator off the recorded rectangle — which works for region and window
  targets and cannot work for a whole display on a single monitor. This
  needs deciding before the exclusion promise can be kept.
- No camera, no separate audio track policy proven by `ffprobe`, no pointer
  or keystroke sidecar, no captions.
- Coordinates are proven on 1× and 1.5× only.
- Storage quotas, cache pruning, and the free-space preflight are absent.

## Delivery phases

Estimates are solo developer effort and include implementation/test work inside each phase. The headline ranges add roughly 15% cross-phase integration and release contingency.

The 13–22 week Studio-beta range is deliberately rounded one week beyond the straight phase sum because embedded Qt preview/audio is the largest remaining schedule variable; Phase 0 must replace that allowance with measured evidence. Ranges assume the preferred downstream integration branch. Choosing external commands still requires tracking the small selector patch, and that maintenance cost must be re-estimated after the first two upstream upgrades.

### Phase 0 — contracts and kill-the-plan spikes (5–8 days)

- Freeze OmaSnap `d339588`, Better Shot `3d9403c`, GSR/MLT target versions, and parity matrix.
- Prototype `--select-target --json` without snapshot/recent/output side effects.
- Prove GSR region coordinates across fractional scale, rotation, and negative origins.
- Prove exact PID + unique IPC pause/stop and interrupted Matroska recovery.
- Verify system/mic track layout with `ffprobe` and 30-minute A/V drift.
- Compare CFR and VFR capture, seek, pause/resume, proxy generation, and export; freeze one default plus explicit project timing metadata.
- Prove layer `no_screen_share`/Hyprland `noscreenshare` exclusion with pixels.
- Prototype MLT seek, speed, overlay, transition, a Qt-embedded preview with synchronized audio/device handling, proxy playback, and headless export.
- Decide downstream fork versus optional external dispatcher.
- Record screenshot startup timing and golden baseline.

**Exit gate:** target JSON is stable; screenshot startup passes the A/B budget; no screenshot output changes; GSR ownership never touches unrelated recorders; area pixels match the selector within one pixel; controls are absent from capture; audio behavior is proven; killing the worker with `SIGKILL` causes the exact orphan to stop or be journal-verified/re-adopted and stopped while leaving a recoverable master; and one Qt-embedded MLT golden project plays synchronized audio and agrees with headless export within tolerance. Any failed gate removes the dependent promise and the Phase 4 estimate is re-baselined from the embedded-preview result.

### Phase 1 — minimal OmaSnap integration (5–8 days)

- Pre-Qt `record`/`studio` dispatcher or external command-only integration.
- Target-only mode and schema tests.
- Extract tiny `omasnap-ui-core` only where screenshot output stays unchanged.
- Video-specific private runtime helpers, locks, protocol types, and fake executables.
- Optional installer/desktop action wiring.

**Exit gate:** the complete existing `make check` suite passes; screenshot CLI/lock/capture/annotation/pin/recents behavior is unchanged; the A/B startup protocol passes again after any shared-UI extraction; missing video package fails helpfully; target selector serializes and distinguishes cancel, busy, and screenshot-preempted outcomes without terminating an existing screenshot overlay.

### Phase 2 — recorder MVP (10–15 days)

- Display, area, window-area, and portal target flows.
- Capability/device probe, picker, countdown, control bar.
- Worker, exact GSR child/IPC, elapsed clock, pause/resume, stop, discard.
- Screen plus validated system audio/mic policy.
- Matroska master, minimal crash journal/recovery, thumbnail, basic playback, direct MP4 remux/export.
- Capture-exclusion onboarding and probe.

**Exit gate:** repeated 30-minute 1080p60 and 4K60 sessions meet drop/lag and A/V gates; every terminal path has honest UI and owned cleanup; no invisible recording continues; output plays in Chromium/VLC/mpv; screenshot startup remains unchanged.

### Phase 3 — full project durability, camera, and recovery (15–25 days)

- Versioned project/autosave/recovery model and storage UI.
- Separate camera master and clock alignment if the Phase 0 gate passes.
- Separate audio controls/tracks if proven; soundtrack import.
- Device-loss handling, restart semantics, free-space warnings, cache pruning.
- Proxy, waveform, thumbnail workers.

**Exit gate:** GUI crash, worker termination, device unplug, disk pressure, pause/resume, and restart all yield a documented recoverable or explicit failed state; camera/screen/audio remain within 50 ms over 30 minutes.

### Phase 4 — Studio beta on MLT (20–35 days)

- Normal top-level Studio shell and adaptive theme.
- Deterministic `studio.json` → MLT compiler.
- Playback/seek/scrub, proxy switching, multi-clip timeline.
- Trim, split, reorder, speed, crop/background, camera transform.
- Manual zoom cues, masks/text, basic audio levels, undo/redo.
- Shared preview/export graph and golden media tests.

**Exit gate:** a 30-minute multi-clip project with mixed speeds seeks responsively; ten-minute golden output agrees frame/time/audio with preview; reopen/autosave recovery is deterministic. If MLT cannot pass, stop and re-plan rather than starting a custom NLE.

### Phase 5 — advanced Studio parity (40–75 days)

- Transition library and color adjustments.
- Opt-in local caption/transcription worker and licensed/checksummed model flow, editing, highlighting, silence/filler suggestions.
- Timed redact/pixelate, text, spotlight, watermark, and camera layouts.
- Pointer restyle/automatic zoom only if safe sidecar/event gates passed.
- Frame-accurate source-time mapping for variable speed and transcript cuts.
- Export presets for H.264/HEVC, MP4/MOV, 480p/720p/1080p/original.

**Exit gate:** every accepted parity row maps to a fixture and preview/export test; privacy-gated features remain absent rather than partially unsafe; long-project memory and interaction targets hold.

### Phase 6 — packaging and release hardening (10–15 days)

- Accessibility, full keyboard route, reduced motion, light/dark themes.
- Arch/AUR split packaging, dependency/license notices, clean install/upgrade/uninstall.
- Schema compatibility, migration/rollback, crash reports without sensitive data.
- Real GPU matrix and installation documentation.

**Exit gate:** a clean optional video install leaves base OmaSnap lean; upgrade/rollback preserves projects; full acceptance matrix passes on the target Omarchy release.

## Performance budgets

Measure screenshot startup on the target machine with identical release flags, theme, compositor state, output, and CPU power profile. Instrument process entry to the first committed overlay frame; alternate pinned-baseline and candidate runs after five warm-ups and collect at least 30 samples per build. The candidate passes when its median delta is ≤5 ms and its p95 is no more than 10 ms slower. Store raw samples and dependency/link maps with the result.

Phase 0 names each supported GPU/encoder/codec/source combination in the capability snapshot. “Sustained encoder lag” means lag reported continuously for more than five seconds; a path that cannot meet the budget is marked unsupported rather than silently weakening the gate.

| Path | Acceptance budget |
|---|---:|
| Existing screenshot launch | A/B p50 delta ≤5 ms and p95 delta ≤10 ms versus pinned baseline |
| Existing screenshot binary/deps | no media libraries linked; negligible dispatcher size only |
| Record picker cold start | < 250 ms to visible shell, excluding portal chooser |
| Record action → first frame | < 1 s after countdown/permission closes |
| Recording control feedback | < 100 ms for pause/stop state acknowledgement |
| 1080p60/4K60 recording | no encoder lag lasting >5 s; dropped frames measured and < 0.1% on each declared supported path |
| A/V and camera drift | < 50 ms after 30 minutes; start offset < one frame |
| Studio scrub | p95 visible-frame response < 100 ms on 720p proxy |
| Studio playback | UI presents at display refresh up to 60 Hz; decoded frames follow declared 30/60 fps project cadence without cumulative A/V drift |
| Studio memory | <1.5 GiB RSS on the 30-minute reference project and <10% growth over a ten-minute loop after warm-up |
| Export throughput | simple 1080p60 ≥1× real time; golden effects project ≥0.5× on the Phase 0 reference path |
| Project autosave | < 50 ms off GUI thread; atomic |
| Cache/storage | 2 GiB cache cap; 20 GiB warning; free-space preflight |

These are gates to measure on the target hardware, not claims about unimplemented code.

## Verification strategy

### Protected OmaSnap regression suite

Run upstream `make check` after every shared/main/editor change. Add explicit baselines for:

- startup timing with and without `omasnap-video` installed;
- Region/Window/Fullscreen/Scroll selection and native-pixel export;
- operation-log replay, crop/cut, redaction, OCR, recents, pin lifecycle, clipboard;
- 1×, 1.25×, 1.5×, 2×, negative-origin, and transformed monitors;
- target-only cancel/busy/preempted output proving no PNG, recents, or sidecar is written and an in-progress screenshot is never terminated by target-mode launch.

### Recorder unit/fake-backend tests

- CLI argument construction contains no shell fragments.
- GSR capability parser across known versions/malformed output.
- Worker state machine rejects invalid transitions and oversized messages.
- Same-UID/one-controller IPC and path containment.
- GSR fake replies: prepare/start/pause/stop, timeout, crash, partial file, wrong path.
- UI disconnect safety stop, worker `SIGKILL`, parent-death handling, exact-journal orphan re-adoption/stop, stale-PID refusal, and exact PID ownership.
- Target schema migrations and coordinate transforms.
- Storage/free-space/quota and cleanup behavior.

### Media integration tests

- `ffprobe` assertions for video/audio streams, duration, timestamps, metadata, and codecs.
- 30-minute synthetic clock/audio pattern for drift and pause semantics.
- Forced UI/worker/encoder termination at start, mid-record, paused, and stop-finalization.
- Camera unplug/replug, mic change, portal denial, monitor removal, disk full.
- Pixel canary proving control exclusion on display/area/portal paths.
- Intel/AMD/NVIDIA/hybrid-GPU paths where hardware is available.

### Studio golden tests

Use generated media with known frame numbers, colors, tones, and timestamps:

- seek/trim/split boundaries;
- speed and audio pitch policy;
- transitions and overlay timing;
- crop/background/camera transform;
- masks/redaction/text/captions;
- proxy/master geometry agreement;
- preview/export frame hashes or documented small pixel tolerance;
- long variable-speed projects with no cumulative time drift.

### Real Omarchy visual acceptance

- Matte Black plus a light, rounded, square, and gradient-border theme.
- Recorder control bar on each monitor and scale.
- Portal and direct display/area workflows.
- Keyboard-only picker and Studio operation; visible focus.
- Control exclusion, notifications, cancel/recovery, and errors.
- Studio at minimum size, 1080p, ultrawide, and large font scale.
- Test through the running shell; never start a second Quickshell process.

## Risk register

| Risk | Consequence | Gate/mitigation |
|---|---|---|
| Upstream scope rejects video | Long-lived fork friction | Companion package; restrict upstream delta to optional selector/dispatcher seams |
| Screenshot binary gains media cost | Core value regresses | Pre-Qt exec, split packages, no linked media deps, +5 ms hard gate |
| Layer-shell global env leaks into Studio | Studio cannot behave as a normal window | Separate executable/process with xdg-shell environment |
| Screenshot instance lock preempts target selection | Confusing or destructive setup | Target mode never terminates an existing overlay; distinct busy/preempted results; GSR starts only after selection |
| Worker dies while GSR survives | Invisible orphan recording | Parent-death signal plus exact-journal identity verification, re-adoption/stop, and `SIGKILL` integration test |
| `OutputCapture` is reused for video | CPU copies, no audio/timestamps, poor 4K performance | Explicitly forbid; use GSR/PipeWire media path |
| GSR CLI changes | Recording fails after upgrade | Version/capability probe, argument fixtures, fail closed with diagnostic |
| Stock Omarchy recorder uses broad process discovery | One recorder controls or misreports the other | Refuse concurrent use until both have scoped ownership and coexistence tests |
| Fractional/rotated region mismatch | Wrong area recorded | Target JSON plus live pixel-coordinate matrix before release |
| Controls appear in recording | Broken/private output | Stable namespace, user-owned rule, canary pixel gate, fail closed |
| Portal re-prompts or denies | Confusing window flow | Treat chooser as expected; persist token only where supported and consented |
| Multi-source clocks drift | Camera/audio unusable | Common monotonic mapping, timestamped buffers, 30-minute <50 ms gate |
| MLT preview/export mismatch | Studio cannot be trusted | One compiler/graph, proxy/master fixtures, headless golden export |
| MLT is too heavy or insufficient | Schedule blowout | Phase 0 and Phase 4 stop gates; no custom NLE under this plan |
| Raw input leaks secrets | Password exposure | No MVP logger; semantic allowlist only after permission/privacy gate |
| Raw media fills disk | Failed session/system pressure | Free-space preflight, usage UI, cache cap, never silently delete masters |
| Editable redaction retains master | User shares original data | Persistent warning; flattened export only; no project-package sharing |
| Codec/parser compromise | App compromise | Bounds, worker isolation, optional Bubblewrap, no network, narrow mounts |

## Proposed repository layout

If maintained in a downstream OmaSnap integration repository:

```text
omasnap/
├── CMakeLists.txt
├── src/                         existing screenshot code
│   ├── main.cpp                 tiny pre-Qt video dispatch + target option
│   ├── capture-target.cpp/.hpp  versioned selector result only
│   └── ...                      existing behavior protected
├── video/
│   ├── common/                  protocol, project schema, theme, clocks
│   ├── record-ui/               layer-shell picker/countdown/bar
│   ├── record-worker/           headless GSR owner/recovery
│   ├── studio/                  xdg_toplevel, timeline, inspector
│   ├── media/                   MLT compiler, proxy, export, ffprobe
│   └── tests/
├── docs/
│   ├── recording-architecture.md
│   ├── recording-privacy.md
│   └── project-schema.md
└── packaging/
    ├── omasnap/
    └── omasnap-video/
```

If upstream remains untouched, keep `video/` in a companion repository and replace direct selector code with a pinned, minimal patch or an external `omasnap-select-target` helper built from the same MIT source. Do not copy the entire screenshot implementation into the companion.

## Definition of done

### Recording MVP

- Existing OmaSnap screenshots are unchanged in features, output, startup, and dependencies.
- Display, area, window-area, and portal recording have explicit semantics.
- Target selection never terminates an existing screenshot overlay and reports screenshot preemption before GSR starts.
- Only the owned recorder PID/socket is controlled.
- Worker death cannot leave an invisible GSR orphan; exact-journal recovery and parent-death handling pass.
- Pause/stop/discard and every failure state are honest and recoverable where claimed.
- Control surfaces are absent from pixel-tested output.
- System audio/mic behavior is proven, not inferred; A/V drift is below 50 ms over 30 minutes.
- Matroska masters survive the tested interruption cases and export/remux to playable MP4.
- Private storage, free-space checks, and no-sensitive-logging rules pass.

### Studio beta

- MLT—not custom decoder/timeline code—owns the media graph.
- Immutable masters and versioned Studio JSON reopen deterministically.
- Multi-clip trim/split/reorder/speed, crop/background, camera, manual zoom, mask/text, audio, and export work.
- Preview and export share the compiled graph and agree on golden media.
- Proxy playback does not alter master geometry/timing.
- Theme changes apply live without importing GTK/Quickshell or affecting screenshot startup.

### Advanced parity

- Every accepted Better Shot recording feature maps to a phase, fixture, privacy decision, and export test.
- Captions/transcription remain local and removable.
- Pointer/click/keystroke features exist only if their explicit safety gates pass.
- Long variable-speed projects remain time-correct and responsive.
- Optional video install, upgrade, rollback, and uninstall preserve base OmaSnap and user projects.

## Research sources

- [OmaSnap source at `d339588`](https://github.com/tobi/omasnap/tree/d339588f3aba554f314d6233da2dddc98f83de55)
- [OmaSnap README at `d339588`](https://github.com/tobi/omasnap/blob/d339588f3aba554f314d6233da2dddc98f83de55/README.md)
- [OmaSnap contributor/agent guide at `d339588`](https://github.com/tobi/omasnap/blob/d339588f3aba554f314d6233da2dddc98f83de55/AGENTS.md)
- [OmaSnap editing model at `d339588`](https://github.com/tobi/omasnap/blob/d339588f3aba554f314d6233da2dddc98f83de55/docs/editing-model.md)
- [OmaSnap threading contract at `d339588`](https://github.com/tobi/omasnap/blob/d339588f3aba554f314d6233da2dddc98f83de55/docs/threading.md)
- [OmaSnap platform scope at `d339588`](https://github.com/tobi/omasnap/blob/d339588f3aba554f314d6233da2dddc98f83de55/docs/platform-scope.md)
- [OmaSnap dependency policy at `d339588`](https://github.com/tobi/omasnap/blob/d339588f3aba554f314d6233da2dddc98f83de55/docs/dependencies.md)
- [OmaSnap MIT license at `d339588`](https://github.com/tobi/omasnap/blob/d339588f3aba554f314d6233da2dddc98f83de55/LICENSE)
- [Omascreen source at `efd097d`](https://github.com/k4ditano/omascreen/tree/efd097dee572205675c6ed44b82b94171e123216)
- [Omascreen README at `efd097d`](https://github.com/k4ditano/omascreen/blob/efd097dee572205675c6ed44b82b94171e123216/README.md)
- [Omascreen MIT license at `efd097d`](https://github.com/k4ditano/omascreen/blob/efd097dee572205675c6ed44b82b94171e123216/LICENSE)
- [Better Shot product reference](https://www.bettershot.site/)
- [Better Shot source reference at `3d9403c`](https://github.com/KartikLabhshetwar/better-shot/tree/3d9403c84a09e60034a9c8373b4cbc844b793ed2)
- [GPU Screen Recorder manual and IPC](https://man.archlinux.org/man/gpu-screen-recorder.1.en)
- [MLT documentation](https://www.mltframework.org/docs/)
- [MLT framework design](https://www.mltframework.org/docs/framework/)
- [MLT C++ wrapper](https://www.mltframework.org/docs/mlt%2B%2B/)
- [Qt Multimedia on Linux](https://doc.qt.io/qt-6/qtmultimedia-linux.html)
- [XDG ScreenCast portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)
- [Omarchy theming contract](https://github.com/basecamp/omarchy/blob/quattro/docs/theming.md)
- [Hyprland layer rules](https://wiki.hypr.land/Configuring/Layer-Rules/)
