# Studio parity implementation plan

Status: Quattro foundation approved; implementing 01–04 sequentially with separate validated commits. Later milestones remain planned.
Created: 2026-09-06.  
Omasnap baseline: `21ab6ec` (`feat/record`).

## At a glance: where the features come from

Use this **Markdown status matrix plus the numbered checklists below** as the
canonical tracker. It is readable in the repository, reviewable in git diffs,
and does not depend on a diagram plugin or a separate project-management service.
The diagram is the quick overview; the matrix records scope and delivery status.

```text
REFERENCE / REQUIREMENT                    OMASNAP STUDIO

Omarchy Quattro ----------------------->  [DONE] 00 Theme palette + sharp chrome
                                          FIRST: foundation for all later UI

Bettershot code + demos ---------------->  [PART] Workspace, styling, manual zoom
                                          [DONE] Non-destructive clip edits
                                          [TODO] Masks, auto zoom,
                                                 speed/audio, camera, captions,
                                                 keystroke overlays

Bettershot website claims ------------->  [TODO] Crossfade / fade through black,
                                                 grading / perspective tilt
                                          Not verified in inspected source

Omascreen website + screenshots -------->  [TODO] Moments / Follow / Raw / Paced,
                                                 click ripples, titles, grain,
                                                 vignettes, wipes / design tray

Your workflow requirements ------------>  [DONE] Drag-select -> Delete -> Undo
                                          [DONE] Import and reorder scene files

Omasnap native implementation --------->  [DONE] GPU playback, basic keyboard
                                                 transport, single-source trim,
                                                 edit undo/save, MP4 export

NEXT: 04 Transitions
```

"Comes from" identifies the behavioral/design reference, **not copied code or
exclusive ownership of a feature**. Several features have multiple references.
Bettershot is not the visual theme; Quattro is. Omascreen evidence currently
comes from its site/screenshots, not a verified source repository.

### Status and reference legend

- **DONE**: the precise scope in the row is implemented and validated on the
  current branch. This does not mean installed, released, or full product parity.
- **PART**: a usable subset is implemented; missing scope and uncommitted work
  are stated explicitly.
- **TODO**: planned but not implemented. **NEXT** marks the first TODO to tackle.
- **DOING** / **BLOCKED**: use only when work actually starts or a named blocker
  prevents it. Milestones 01–04 are authorized in order; later work is not started.
- **B**: Bettershot source/demos inspected; **B-web**: website-only claim.
- **O-web**: Omascreen website/screenshots; **Q**: Quattro design requirement,
  verified against installed shell tokens/geometry (see `docs/studio-design.md`).
- **U**: your requested workflow; **N**: native Omasnap implementation/engineering.

### Landed baseline

| ID | Feature scope that has landed | Reference | Status | Delivery evidence |
|---|---|---|---|---|
| B0 | GPU preview, bounded frame work, coalesced scrubbing | N | DONE | `21ab6ec`; live 4K60/GPU checks |
| B1 | Space play/pause, frame stepping, seek, shortcut help | U + N | DONE | `21ab6ec`; focused-control keyboard checks |
| B2 | Single-source end trim, manual zoom cues, MP4 export | B + N | DONE | Present in `21ab6ec`; smoke and preview/export tests |
| B3 | Undo/redo for existing edits, grouped gestures, asynchronous save-on-close | U + N | DONE | `21ab6ec`; undo/save interaction checks |
| B4 | Thumbnail timeline, inspector tabs, four canvas colors, padding/corners | B + N | DONE | `21ab6ec`; visual and styled-export checks |

These rows describe existing capabilities only. For example, B2 does **not** mean
interior range deletion exists, and B3 does **not** mean multi-scene project undo
exists. The larger parity targets remain in the matrix below.

### Feature-source and delivery matrix

| Milestone | Target feature | Reference | Status | Landed subset / remaining work |
|---|---|---|---|---|
| **00** | **Quattro design foundation** | **Q + U** | **DONE** | `57e3700`; inherited palette/reload/fallback, square mono controls; approved to continue |
| 01 | Project/composition model | B + U + N | DONE | Assets/clip instances, shared time map, project history/persistence, bounded playback and composition export; full and live checks pass |
| 02 | Drag-range delete, split, ripple close, undo | U + B; O-web editing reference | DONE | Explicit range tool, handles, clip/zoom selection, split, delete, exact history; decoded audio/video and UI checks pass |
| 03 | Import, combine, duplicate, reorder scenes | U | DONE | Atomic multi-file import/drop, markers, reorder/duplicate/source trims, source-anchored zooms, undo and reopen; full/live checks pass |
| 04 | Crossfade, fade through black, later wipes/slides | B-web + O-web + U | TODO | No scene transitions in current Studio |
| 05 | Full timeline and keyboard workflow | B + U + N | PART | Transport, range tools, scene shortcuts, thumbnails landed; snapping, timeline zoom, waveforms still needed |
| 06 | Background/layout inspector parity | B | PART | Colors/padding/corners landed; gradients, wallpaper, aspect, crop, shadows, presets pending |
| 07 | Timed blur/pixelate/hide masks | B; U + N privacy requirements | TODO | Video masks and mask lane pending; screenshot redaction is not Studio implementation |
| 08 | Title cards, text overlays, design tray | O-web + U | TODO | Editable timed text and reusable designs pending |
| 09 | Grain, vignette, grading, perspective effects | O-web; B-web grading/tilt | TODO | Timed effect stack and matching export pending |
| 10 | Pointer metadata, cursor styling, click ripples | B + O-web | TODO | A cursor baked into a recording is not editable pointer metadata |
| 11 | Auto zoom, follow/smart/fixed, frequency modes | B; O-web four-mode workflow | PART | Manual eased cues landed; metadata-driven auto zoom, spring planner and named modes pending |
| 12 | Per-scene speed and exported audio editing | B + U | PART | Preview volume/mute landed; scene retiming, independent track mix/import/fades pending |
| 13 | Separate camera source and timed bubble | B | TODO | Capture/import, synchronization, and layout controls pending |
| 14 | Captions and transcript-based cuts | B | TODO | Local transcription decision, caption editing/rendering, and transcript cuts pending |
| 15 | Opt-in keystroke overlays | B | TODO | Consent-safe capture, editable events, and rendering pending |
| 16 | Multi-scene export/recovery/performance hardening | N + U; B workflow reference | PART | Basic MP4/save landed; mixed-source export, recovery, cancel/progress and full-project tests pending |

### How to keep this tracker accurate

1. Before starting a milestone, agree its scope and change its matrix status to
   DOING. Keep the source/evidence distinction; a marketing claim is not a test.
2. Check off its detailed tasks as they pass. Use PART if only some user-visible
   behavior lands; describe the missing behavior in the matrix.
3. Mark DONE only when its acceptance criteria, undo/persistence, relevant
   keyboard behavior, and preview/export checks pass. For chrome-only work,
   verify that saved/exported video styling is unaffected.
4. Add the implementing commit and validation evidence to that milestone, and
   update the diagram's summary if the capability moves from pending to landed.
5. Keep the numbered IDs stable. Add new scope explicitly rather than silently
   broadening a DONE row. Do not use completion percentages that count a button
   the same as a working compositor.

## Goal and working agreement

Turn Studio into a polished screen-recording editor: remove unwanted passages,
combine scenes, add transitions and useful effects, and export exactly what the
preview shows. Use Bettershot for the editing/inspector reference and Omascreen
for its automatic-zoom modes and drag-in design/effect workflow. The visual
language is **Omarchy Quattro**, not Bettershot's macOS chrome: inherit the active
Omarchy theme's color palette and follow Quattro's sharp geometry and control
conventions. Establish that foundation before adding editing features.

- Tackle one numbered item at a time, starting with 00. Every subsequent UI
  milestone depends on the approved Quattro foundation.
- Agree on the next item before implementing it. This file is not permission
  to implement the whole backlog, install dependencies, or change desktop config.
- Keep each item independently testable and reviewable. Update its checkbox
  only after its acceptance checks pass; record any remaining limitations.
- The plan was committed as `4acb775`; implementation is authorized one milestone
  at a time. No installation or release is implied.
- Feature parity means working editing, preview, persistence, undo, and export,
  not merely adding controls that resemble the reference UI.

## What already exists

- Region/window/fullscreen recording, pause/stop, and opening a recording in Studio.
- GPU video preview with bounded worker frame preparation and coalesced scrubbing.
- One source video, one continuous trim interval, manual zoom cues, and MP4 export.
- Thumbnail timeline, transport, and Canvas/Zoom/Clip inspector tabs.
- Four canvas colors, padding, and rounded corners.
- Session undo/redo for existing edits and asynchronous sidecar saving.
- Space transport, frame stepping, five-second seeking, trim/zoom commands,
  save/export commands, and shortcut help.

**Not supported yet:** scene transitions. Importing several scenes, reordering,
duplicating, and trimming them now use the shared composition model alongside
range cuts, splits, and selected-clip/zoom deletion.

## Reference findings and evidence boundaries

Bettershot source inspected at
[`27ccd643627c362d8611732bf7b673bcc2e9f569`](https://github.com/KartikLabhshetwar/better-shot/tree/27ccd643627c362d8611732bf7b673bcc2e9f569)
(release 0.4.4). Both linked demos were downloaded and sampled visually.

- [Clip model](https://github.com/KartikLabhshetwar/better-shot/blob/27ccd643627c362d8611732bf7b673bcc2e9f569/Sources/BetterShot/RecordingClipTimeline.swift):
  non-destructive ranges, split/delete/trim, source-to-editor time mapping, and
  per-segment speed. Its segments reference ranges of one recording; this is not
  evidence of arbitrary multi-file import/reordering.
- [Studio model](https://github.com/KartikLabhshetwar/better-shot/blob/27ccd643627c362d8611732bf7b673bcc2e9f569/Sources/BetterShot/RecordingStudioModel.swift):
  undoable clip edits, transcript-based cuts, and rebuilding motion after edits.
- [Composition builder](https://github.com/KartikLabhshetwar/better-shot/blob/27ccd643627c362d8611732bf7b673bcc2e9f569/Sources/BetterShot/RecordingCompositionBuilder.swift):
  joins surviving ranges and retimes audio/video together for playback/export.
- [Inspector and canvas](https://github.com/KartikLabhshetwar/better-shot/blob/27ccd643627c362d8611732bf7b673bcc2e9f569/Sources/BetterShot/RecordingStudioWindow.swift):
  backgrounds, aspect/fit/fill, layout, zoom, cursor, keystrokes, transcription,
  camera, and audio. [Demo 2](https://www.bettershot.site/feature-2.mp4) visibly
  demonstrates backgrounds, framing, corners/shadow, cursor size, and zoom controls.
- [Demo 3](https://www.bettershot.site/feature-3.mp4) demonstrates timed rectangular
  blur/pixelation masks with strength controls and a dedicated lane. It is not a
  demonstration of scene transitions.
- Bettershot's [website](https://www.bettershot.site/) advertises crossfade,
  fade-through-black, 3D card tilt, grading, and 0.25–4x speed. I did not find the
  corresponding scene-transition/tilt/grading implementation in the inspected
  recording code; its clip speed limits are 1–8x. Treat these as advertised
  targets, not source-verified behavior or code ready to transplant.
- [Omascreen](https://omascreen.com/), including the supplied screenshots,
  describes Moments/Follow/Raw/Paced zoom modes, frequency controls, click ripples,
  titles, transitions, grain, vignettes, wipes, and timeline editing. These are
  website/screenshot references, not source-verified implementations. No source
  repository was linked in the homepage inspected; do not assume a similarly
  named GitHub project is this product. Its separate Edit product advertises
  additional tracks and overlays; that does not establish the recorder's scope.

Use these references for behavior, not a literal copy of macOS APIs or branding.
Review licenses and preserve attribution before porting code or bundling assets;
do not copy Apple's wallpaper collection into Omasnap.

## Implementation checklist

### 00 — Omarchy Quattro design foundation (first)

- [x] Inspect the installed Quattro shell's current theme provider and native
  controls. Record the actual palette contract, typography, border/corner
  treatment, spacing, density, and interaction states; do not guess token names
  or copy outdated screenshots as an implementation specification.
- [x] Define a small shared Studio design-token layer: background/surface,
  foreground/muted text, accent, border, selection, focus, disabled, warning,
  and error colors, plus geometry and typography metrics.
- [x] Derive chrome colors from the active Omarchy theme, not fixed dark gray
  and purple values. Handle theme changes while Studio is open without losing
  edits, resetting playback, or reopening the window. Load/refresh safely off
  the UI thread, with a deterministic fallback for unavailable or invalid data.
- [x] Use Quattro's sharp/square corner treatment for panels, buttons, menus,
  fields, tabs, timeline blocks, and dialogs. Centralize any measured exceptions
  rather than introducing arbitrary rounded cards or pill-shaped controls.
- [x] Standardize fonts, icon strokes, padding, separators, hover/pressed states,
  focus indicators, and selected/disabled states across reusable controls.
  Preserve pinned chrome font helpers unless a measured Quattro mismatch warrants
  an explicitly documented Studio-only change; do not use platform font fallbacks.
- [x] Restyle the existing header, inspector, timeline, transport, tooltips,
  shortcut help, and status/error surfaces first. This becomes the component
  vocabulary for every later feature, not an optional finishing pass.
- [x] Keep application chrome separate from video design: a theme switch must
  not recolor a saved canvas background, change an exported title, or square off
  the user's chosen video corners. Project visuals remain explicit saved edits.
- [x] Document the Studio-specific theme contract and reconcile the existing
  fixed-color guidance when implementing this milestone. Continue using Qt's
  generic platform theme and explicit resolved colors: read Omarchy's palette
  directly rather than loading GTK theme plugins or deriving chrome from
  `QStyle`/`QWidget::palette()`. Do not change screenshot chrome or desktop config.
- [x] Obtain your visual approval before starting milestone 01.

Delivery: `feat(studio): establish Quattro theme and control foundation`.
`src/studio-theme.cpp` owns the
shared contract; `tests/studio-theme-smoke.cpp` covers palette/fallback/reload
and canvas isolation. Live Wayland checks verified playback across palette
changes and existing keyboard/edit/export behavior. Dark/light screenshots
were reviewed without changing the desktop theme. See `docs/studio-design.md`
for the deliberately limited palette contract (not the full shell override API).
Validation on 2026-09-06: `make check` passed (both smoke suites and clang-tidy;
warnings remain, clazy unavailable, no QML pass applicable). Separate Wayland
GPU/interaction/export checks passed. Binary dependencies still isolate
Multimedia/Network from `omasnap` and layer-shell from Studio.

Acceptance: compare the actual Studio window with installed Quattro controls;
review dark and light theme variants where available, accent changes, narrow
tiled layouts, and fractional scaling. Check readable text, visible keyboard
focus, consistent sharp corners, and no flashes of the old fixed palette.
Exercise live theme reload with unsaved edits and active playback; project state
and exported pixels must remain unchanged. Keep theme reads out of painting and
frame callbacks. Approve this visual baseline before moving to 01.

### 01 — Non-destructive project and composition model

- [x] Introduce a Studio project with source assets, ordered clip instances,
  source in/out points, speed, stable IDs, and project canvas settings.
- [x] Keep source time distinct from edited timeline time. Centralize mapping
  for frames, audio, zoom, cursor events, masks, and captions.
- [x] Represent edits through one undoable project command/history mechanism;
  one drag or range deletion is one undo step. Never modify source media.
- [x] Persist the project atomically, support an empty project, and report
  missing media with an explicit relink action.
- [x] Establish a shared composition description for GPU preview and export.
  Prove timestamp/audio handling before promising seamless multi-clip playback.

Acceptance: pure tests cover source/time mapping, repeated use of a source,
split boundaries, undo/redo, empty projects, and save/reopen. No media work blocks
the GUI. This is the prerequisite for 02–04, not a backend rewrite chosen in advance.

Delivery: `feat(studio): add non-destructive composition projects`.
Validated with `make check` and live Wayland decoded-pixel checks: ordered
mixed-aspect scenes, reverse/boundary seeks, repeated sources, speed/VFR export,
primary audio/silence, project reopen, empty/missing media, and history.
See `docs/studio-project.md` for the 64-clip export cap and primary-track
audio policy. Old zoom sidecars remain untouched and are not migrated.

### 02 — Drag-select a passage and Delete it

- [x] Add a visible range-selection tool: drag from start to end on the video
  lane, then adjust either endpoint with handles.
- [x] Delete/Backspace removes the selected passage and closes the gap
  (ripple delete), including a passage inside a clip or spanning several clips.
- [x] Support split at the playhead, select a clip, and delete the selected clip.
- [x] Undo restores the exact clip ranges, attached edits, selection, and
  playhead; redo reapplies the operation. Deleting the final clip is undoable.
- [x] Provide context-menu commands and visible buttons alongside shortcuts.

Interaction proposal: `V` selects/scrubs; `B` enables range selection; `S` splits
at the playhead; `Escape` clears the active selection/tool. A plain drag in range
mode selects time rather than moving the playhead. Delete acts on an explicit
range first, otherwise the selected object; no implicit video deletion when a
zoom or mask is selected. Text editing retains normal Delete/Backspace behavior.

Acceptance: cut a middle section, multiple sections, an edge section, and a
cross-clip range; reverse-direction dragging works. Playback/export contain no
removed frames or audio, and one undo restores each cut. Zero-length selections
are harmless. Existing Space and zoom-deletion shortcuts remain reliable.

Delivery: `feat(studio): add undoable timeline range cuts and splits`.
Validated by full `make check` and Wayland tests. UI checks cover reverse
selection, endpoint resizing, exact undo/redo selection/playhead restoration,
split/delete, final-clip deletion, and text-field Delete isolation. Decoded
export frames and audio contain no removed passage; playback clears stale
deleted frames. Non-unit-speed cuts use explicit duration-conserving snapping.

### 03 — Import and combine scenes

- [x] Drag video files from the file manager into Studio; also provide an Add
  scenes action and `Ctrl+O` import command.
- [x] Show an insertion marker: drop on a boundary to insert there, or after
  the last clip to append. Multiple files arrive as distinct scenes.
- [x] Reorder clips by dragging, duplicate them, trim individual edges, and
  allow the same source to appear multiple times with independent edits.
- [x] Show each scene's filename/thumbnail, duration, and selection state.
- [x] Probe asynchronously; explain invalid/unsupported media without losing
  the existing project. Establish a clear output aspect/FPS policy for mixed media.
- [x] Keep imports, reorder, duplicate, and trim undoable and persistent.

Acceptance: combine three recordings of different dimensions/frame rates,
reorder them, undo, reopen, and export. Frame order, duration, and audio agree
with the project. Missing audio is silence, not an export failure. Handle VFR
explicitly through timestamp-aware playback or an announced normalization path;
never silently assume that all imported clips are CFR.

Delivery: `feat(studio): import and arrange independent scenes`.
Validated with `make check`, live Wayland GPU/keyboard tests, and 1280×820 /
980×680 inspector screenshots using real 4K recordings. Tests cover batch and
boundary drops, invalid-batch atomicity, drag reorder/edge trim, exact undo,
duplicate independence, reopen, mixed-source export pixels/audio and canvas/FPS.
Zooms follow retained scene content; cue-cap overflow is refused. Scene trims
and arrangement reset the project-wide export range. Scroll surfaces inherit
the explicit Quattro palette, with a rendered-background regression check.

### 04 — Scene transitions

- [ ] Keep hard cuts as the default; add Crossfade and Fade through black first.
- [ ] Attach a transition to a clip boundary with a visible editable duration.
- [ ] Blend both scenes on the GPU and crossfade their audio appropriately.
  Preload/decode the incoming scene instead of opening it at the boundary.
- [ ] Define overlap duration and source-handle rules; clamp or explain short
  clips, and make timeline duration match export exactly.
- [ ] Undo adding/removing/changing a transition. Reordering clips must not
  silently attach a transition to the wrong pair.
- [ ] Add directional Wipe/Slide variants only after the first two pass parity tests.

Acceptance: scrub both ways through every transition, pause mid-transition,
frame-step, and export sample frames/audio. No blank frames, audio bursts, or
boundary stalls. Hard cuts need only one active scene; blends use a bounded pair,
not one resident decoder per project clip.

### 05 — Timeline usability and keyboard polish

- [ ] Add horizontal timeline zoom, fit-to-project, scrolling, boundary snapping,
  and precise endpoint/time readouts. Range selection must work while scrolled.
- [ ] Make clip, zoom, mask, title, and effect selections visually distinct.
- [ ] Add audio waveforms with asynchronous generation and bounded caches.
- [ ] Provide action-aware undo/redo labels and discoverable context menus.
- [ ] Test all commands from buttons, sliders, inspectors, numeric fields,
  and text fields; do not intercept text editing or repeatedly toggle on key repeat.

Keep Space, arrows, Shift+arrows, Ctrl+Z, Ctrl+Shift+Z/Ctrl+Y, Ctrl+S, Ctrl+E,
and help working throughout. When multiple clips exist, explicitly define and
document whether I/O edits the selected clip or a project export range; do not
silently give one shortcut two meanings. Item 02's range tool must not steal `R`,
which currently resets trim.

Acceptance: complete a cut/import/reorder/export workflow using the keyboard;
test large projects and narrow tiled windows without overlapping controls.

### 06 — Background and layout inspector parity

- [ ] Add background choices: none, solid swatches/custom color, gradients,
  and user-imported wallpaper, with visual previews.
- [ ] Add Original, 16:9, 9:16, 1:1, and 4:3 canvas presets and Fit/Fill modes.
- [ ] Add crop/reposition, shadow controls, and clearer padding/corner controls.
- [ ] Provide named style presets, reset per section, and a clear distinction
  between project-wide styling and per-scene overrides.
- [ ] Use compact, collapsible Background, Layout, Motion, Cursor, Audio,
  and Overlay sections as features land, built from milestone 00's Quattro
  controls; do not add nonfunctional placeholder UI. These controls edit the
  video canvas, not the application's inherited theme.

Acceptance: portrait and landscape previews match exported geometry, gradients,
crop, shadow, and corners. Imported backgrounds are tracked project assets;
missing images produce a useful recovery path. Preset application is one undo step.

### 07 — Timed privacy masks

- [ ] Draw, move, and resize a rectangular mask in the preview.
- [ ] Add Blur and Pixelate with strength controls, plus a separate opaque Hide
  mode for sensitive information. Blur/pixelation are not a secrecy guarantee.
- [ ] Put masks on a dedicated timeline lane with draggable start/end handles.
- [ ] Keep masks attached to source/clip coordinates through crop, zoom,
  splitting, deletion, speed changes, and transitions.
- [ ] Bake masks into exported pixels; never include an unredacted video stream
  alongside the visible result. Preserve undo only in the editable project.

Acceptance: masks cover the intended content at cue boundaries and through
camera movement. Hide is applied to both contributing scenes before transition
blending. Export tests inspect decoded pixels and output stream contents.

### 08 — Titles, text, and reusable design elements

- [ ] Add a small design tray for title cards, lower thirds, and caption-like
  callouts; drag items onto the timeline.
- [ ] Edit text directly; control placement, size, color, alignment, and backdrop.
- [ ] Distinguish a standalone title scene, which adds duration, from a text
  overlay, which does not. Give both visible duration handles.
- [ ] Add restrained fade/slide entrance and exit animations with shared
  preview/export timing, and save reusable style presets.

Acceptance: text remains editable after reopen, renders consistently at multiple
output sizes, and can be moved, trimmed, deleted, and undone without altering video.

### 09 — Effect blocks and finishing controls

- [ ] Add timed Grain and Vignette effects with intensity controls.
- [ ] Add a small set of reusable wipe/reveal designs using the transition
  framework; distinguish scene changes from overlays on a continuing scene.
- [ ] Add basic exposure/brightness, contrast, saturation, and temperature controls.
- [ ] Evaluate optional 3D card tilt/perspective after composition performance
  is proven; it must transform masks/cursor with the card, not separately.
- [ ] Make effects stackable, reorderable where meaningful, disableable, and
  undoable. Show the affected duration and target scene/layer.

Acceptance: effect order is explicit, animated noise is deterministic for the
same project time, and sampled preview/export frames agree. Measure 4K playback
with a representative stack, not just an empty project.

### 10 — Recorded pointer metadata and click effects

- [ ] Establish a Hyprland-compatible, explicitly enabled pointer/click capture
  path. Do not silently alter input permissions or desktop configuration.
- [ ] Store normalized positions, click times, cursor visibility, and recording
  pause/resume mapping separately from the video.
- [ ] Support adjustable cursor size, smoothing, and click ripples/highlights.
- [ ] Establish whether the recorder can omit the baked-in cursor before
  promising cursor replacement; avoid drawing a second cursor over the original.
- [ ] Imported videos without pointer metadata retain manual zoom; do not
  pretend cursor events can be recovered perfectly from video pixels.

Acceptance: region/window/display coordinates, fractional scaling, pause/resume,
and cut/reorder timing all stay aligned. Capture stops with recording and errors
do not leave an invisible input listener running.

### 11 — Automatic zoom and motion modes

- [ ] Generate editable zoom cues from recorded pointer/click metadata.
- [ ] Add Moments (short excursions), Follow (sustained tracking), Raw
  (event-driven moves), and Paced (moves separated by a wide-view cooldown).
- [ ] Add None/Less/Balanced/More frequency choices where applicable; changing
  frequency or mode regenerates a previewable, undoable set of cues.
- [ ] Support pointer-follow, smart-region, and fixed-target camera behavior;
  tune a damped-spring planner rather than equating it with the current easing curve.
- [ ] Preserve manual overrides or explicitly offer Replace automatic cues;
  never silently erase hand-edited zooms during regeneration.

Acceptance: camera motion remains bounded and continuous across cuts, rapid
clicks, and nearby targets, and stable across preview/export frame rates.
Defaults produce readable demos without requiring a motion-settings panel.

### 12 — Per-scene speed and audio editing

- [ ] Add scene speed controls, including a deliberately chosen slow/fast range;
  document the supported range rather than copying conflicting reference claims.
- [ ] Retime screen video, camera, audio, cursor, captions, and zoom consistently.
- [ ] Expose independent recorded mic/system track volume, mute, fades, and meters.
- [ ] Allow background music or replacement narration import; offer a voiceover
  workflow later without conflating preview mute with exported audio settings.
- [ ] Decide pitch-preservation and optional narration ducking behavior explicitly.

Acceptance: long mixed-speed projects remain synchronized; joins and fades do
not click or clip audibly. Preview and export use the same audio routing policy.

### 13 — Camera overlay

- [ ] Record or import a separate camera source, not a face bubble burned into
  the sole screen master. Permission and recording state must be visible.
- [ ] Add bubble position, size, roundness, shadow, and timed show/hide controls.
- [ ] Keep camera synchronization through pause/resume, cuts, speed, and reorder.

Acceptance: project reopen and export preserve timing and bubble geometry;
disabling the camera does not affect screen playback or audio unexpectedly.

### 14 — Captions and transcript-based editing

- [ ] Evaluate an optional local transcription backend; its dependency/download
  cost requires a separate decision. Do not upload recordings by default.
- [ ] Generate editable captions with timing, placement, wrapping, text style,
  and optional word highlighting.
- [ ] Select transcript words to preview or cut a passage using the same
  undoable range-removal operation as item 02.
- [ ] Offer reviewed silence/filler-word removal as a proposed edit, with undo.

Acceptance: captions follow every timeline edit and remain readable during zoom;
transcript cuts remove precisely the corresponding video/audio passage.

### 15 — Opt-in keystroke overlays

- [ ] Capture shortcuts/keystrokes only with explicit consent and visible state;
  keep disabled by default. Evaluate password/privacy behavior before implementation.
- [ ] Store editable timed events and provide placement/style controls.
- [ ] Allow disabling/deleting events and excluding them from exported media.

Acceptance: no global key capture outside an authorized recording; edits, cuts,
and pauses preserve alignment. This feature must not require unsafe default privileges.

### 16 — Export, project recovery, and release hardening

- [ ] Add output size, aspect, frame-rate, quality, and audio choices with
  conservative defaults; retain MP4 as the first-class output.
- [ ] Add progress, cancel, actionable failure reporting, and recoverable output
  handling without overwriting the original or a previous successful export.
- [ ] Test mixed-source projects, missing files, interrupted saves/exports,
  large timelines, and reopen/relink recovery.
- [ ] Add bounded thumbnail/waveform caches and optional background proxies
  only where measured decoding limits justify them.
- [ ] Finish a feature-by-feature UI/keyboard/export parity audit and document
  unsupported formats/effects honestly.

Acceptance: a project exercising cuts, three scenes, transitions, zoom, styling,
audio, and overlays exports predictably and reopens without losing edits.

## Gates that apply to every item

- All new chrome uses milestone 00's Quattro tokens and reusable components:
  inherited theme colors, sharp geometry, consistent density, and keyboard focus.
  Bettershot/Omascreen supply workflow references, not a competing visual theme.
- Theme changes affect editor chrome only; saved project styling and export
  output must not depend on the theme active when the project is reopened.
- Every user edit is undoable; every drag is one history entry, not hundreds.
- One source of truth determines preview, audio, export, and saved state.
- New decoders, caches, or effects must remain bounded; no full-resolution
  conversion, disk I/O, probing, or encoding on the GUI thread.
- Run `make check` after behavioral changes, plus live Wayland visual/keyboard
  tests for affected interactions. Add decoded-frame and audio timing tests for
  media behavior. A successful headless test is not a GPU performance test.
- Re-run the same 4K60 playback fixture; target source-rate playback without
  recurring UI stalls. Measure sustained playback and edit/transition boundaries.
- Keep `omasnap` free of Qt Multimedia/Qt Network and Studio free of layer-shell.
- New dependencies need a measured justification and explicit approval. The older
  `docs/recording-studio-plan.md` proposes a different process/MLT architecture;
  it is historical planning, not authorization to adopt that architecture now.
- No compatibility shims solely to preserve experimental project formats.
- Screenshot/annotation behavior, startup performance, and desktop configuration
  stay outside this work.

## Separate decisions, not implied by parity

- Cloud sharing/R2 integration and credential storage.
- A general six-track/b-roll editor or stock-media library.
- Additional export formats beyond the MP4 workflow.
- Bundling large asset packs, speech models, or platform-specific wallpaper art.

These can be discussed later; do not expand the local Studio port into them
without a specific request.

## Next item

**00 first:** establish and approve the Quattro theme/geometry foundation on the
existing Studio. Then **01 → 02:** build the smallest correct project/timeline
foundation needed to drag-select an unwanted passage, Delete it, and undo it.
Do not start implementation until requested.
