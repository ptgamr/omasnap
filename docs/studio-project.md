# Non-destructive Studio projects

`StudioProject` is the document: assets retain original file paths and probed
metadata; stable clip instances reference half-open source ranges and a speed.
The ordered composition has a separate clock. `studioComposition`,
`studioFrameAt`, and `studioTimelineTime` define that mapping for playback and
export; repeated uses of an asset remain distinct instances.

The document also owns global project-time zoom cues, output dimensions/FPS,
canvas styling, and an optional review/export range. The first source chooses
the output shape (even dimensions) and rational FPS. Each source fits a black
project canvas before global zoom and styling. Preview follows source frame
timestamps; export retimes then normalizes to the fixed project FPS.

`StudioHistory` stores value snapshots with selection and playhead context.
One gesture pushes one state. UI caches of zoom/style are synchronized into the
project when an edit is committed; transport selection alone adds no history.
Undo/redo does not modify source pixels, media files, or duration metadata.

Projects use `<recording>.omasnap.json`; a project can also be opened directly.
There is deliberately no migration from the old zoom sidecar. JSON is bounded
to 4 MiB and validated before publication. Unknown schema, invalid IDs/ranges,
unsupported transforms, and malformed numbers produce an error rather than
silently discarding edits. Saves use QSaveFile on a worker; close waits for the
latest pending save. Missing source paths are reported separately from parsing
errors and can be explicitly relinked to a source long enough for its ranges.

The model allows empty projects and limits documents to 1000 assets/clips and
seven days of composition time. Export currently caps clip occurrences at 64
because each range needs an FFmpeg input; it fails explicitly above that limit.
Preview uses at most two decoders, preloading the incoming clip, never one
resident decoder per scene. Probing, thumbnail generation, persistence, and
export processes run off the UI thread.

Audio policy is explicit: use the primary audio stream in both preview and
export; export resamples it to stereo and fills missing audio with silence.
Additional source audio tracks are retained in the originals but are not mixed
or exported yet. Independent mic/system/music mixing remains milestone 12.

Tests cover pure mapping and persistence, missing/empty projects, history,
mixed-source export order/aspect/FPS, speed and VFR timestamps, audio length,
and live controller seeks and cuts. Later UI milestones consume this model;
they must not add a separate timing or persistence representation.

## Range cuts and splits

`studioSplitClip`, `studioDeleteRange`, and `studioDeleteClip` edit references,
never source assets. Range deletion ripples project-time zoom cues and the
review/export endpoints, drops removed cues, and retains surviving clip IDs.
An interior deletion creates a fresh ID for the right remainder. The window's
ID allocator is monotonic across undo branches. History retains the entire
pre-edit document, selection/range, and playhead; an empty result is undoable.

The model stores source endpoints in integer milliseconds. For retimed clips,
split/cut endpoints snap to representable source positions that conserve total
composition duration. A split too close to an edge or at an unrepresentable
retimed boundary is refused instead of silently introducing timing drift.
Cut results report effective endpoints and removed duration, which are also
used to move the playhead. Ordinary 1x clips preserve requested millisecond cuts.

Range selection is explicit (`B`); `V` selects clips/scrubs. Selection tools
and their hover/range state are not video edits. Video deletion requires an
explicit range or clip; a selected zoom consumes Delete without deleting video.
Text fields retain their normal editing shortcuts. Composition changes clear
stale decoded frames so removed material cannot remain in the editing preview
while the next valid source frame is being prepared.

## Scene arrangement

Import batches are probed on a worker and published atomically: one invalid
file leaves the entire project unchanged. A local file drop on the timeline
inserts at the marked boundary; drops elsewhere and Add scenes append. The
initial project's canvas/FPS remain fixed, including after deleting all clips.
Imports, ordering, duplicates, and source-edge trims share project history.
Undoing an import removes references only; it never deletes a media file.

In Select mode, plain dragging on scene bodies or the ruler scrubs continuously.
Ctrl+drag reorders scene bodies; selected scene edges still trim.
Drag seeks keep one latest pending target and wait for the current frame's
decode/preparation to finish before dispatching another. Same-clip scrubbing
retains the last prepared picture while waiting. A stalled seek can be
superseded after one second so mouse seeking cannot remain locked out. Only
the affected decoder's preparation is invalidated; unrelated preloads survive.
Playback and scrubbing both wait for the newly prepared frame, not merely a
retained older picture. Cached clip reuse checks actual frame timestamps.

Composition edits preserve the visible frame only when its clip, source asset,
source range, speed, and mapped source position remain unchanged, its decoder
is healthy, and neither position is in a transition. Other edits still clear
stale material. Unused
decoders are paused and their priming state is retired. Prepared hard cuts
reuse the incoming frame without an extra seek/copy. Playback caches duration
and the current blend lookup; thumbnail decode/filter threads are bounded.
Deferred source-ready callbacks are scoped to their load generation. Reusing a
slot for a different source retires callbacks when LoadingMedia begins: Qt can
report LoadedMedia for the old source while stopping it, and a seek dispatched
before the replacement finishes loading would otherwise decode from zero.
Regression coverage checks the first decoded timestamp after a paused backward
seek from a duplicate whose next scene preloads a different source.
Reusing a decoder after a backend error reloads its source, even if Qt's media status
still says BufferedMedia. Regression checks cover stale cached timestamps,
delayed incoming preparations, stalled-scrub recovery, unchanged-frame retention,
inactive decoder parking, and same-source error recovery.
The Clip inspector offers source in/out
milliseconds, Duplicate, Earlier, and Later as precise alternatives. I/O/R
continue to control the project-wide review/export range, not the selected scene.
Structural scene edits reset that range to the full composition so newly added
material is not silently excluded from export.

Zooms follow retained scene content through source time. Duplicates receive new
cue IDs; moved fragments keep an original ID on the first surviving original
piece and allocate fresh IDs for separated pieces. Adjacent unchanged pieces
merge. Fragments shorter than the existing 200 ms cue minimum are discarded;
an edit exceeding the 40-cue cap is rejected with an explanation. A continuous
edge drag evaluates against its starting snapshot and is one undo step.

Scene files keep their recorded timing, including VFR; export normalizes to the
project FPS. Audio remains primary-stream-only, with silence for silent scenes.

## Scene transitions

Project schema 2 stores each transition against the ordered outgoing/incoming
clip IDs. There is no schema-1 migration. Hard cuts have no transition record.
Fades and directional wipes/slides overlap the existing kept tail/head; they
never extend source handles or expose excluded footage. Incoming scene start
is outgoing end minus the overlap, so total duration subtracts every overlap.

Durations snap to output-frame intervals. The maximum is half the shorter
neighbor's duration after reserving 250 ms; the frame-rounded limit is shown in
the inspector. This prevents triple overlaps and leaves preparation time between
adjacent blends. A scene too short for a frame of overlap keeps a hard cut.
On slower decoding, transport can buffer while the incoming frame is prepared;
it does not advance the composition clock through an unavailable picture.

Both pictures fit/rotate into the canonical canvas, then blend before the
project-wide zoom and canvas styling. Crossfade weights are `1-u` and `u`.
Fade through black uses `max(1-2u,0)` and `max(2u-1,0)`, giving an actual black
midpoint. Every mode linearly crossfades primary audio with `1-u` and `u`;
silent scenes contribute zero. Export uses a custom RGB expression for the
symmetric black fade, not FFmpeg's differently phased built-in `fadeblack`.
Final encoded timestamps remain on the chosen CFR grid (sub-frame project
endpoints round to an output frame, as for hard-cut export).

Wipe and Slide each support Left, Right, Up, and Down, named for movement of the
incoming picture. At progress `u`, Wipe Left reveals the incoming scene in
`x >= 1-u`; Right reveals `x < u` (Up/Down use the analogous y coordinate).
Slides translate the outgoing and incoming canonical canvases together: Left
uses offsets `-u` and `1-u`, Right uses `u` and `u-1`. Clipping is half-open,
so exactly one source owns a seam. Fit/rotation precedes these transforms;
global zoom and canvas styling follow them. Wipe export uses pixel-center
predicates. Native FFmpeg slides use integer pixel shifts, while GPU sampling
can interpolate subpixels; comparisons allow a one-pixel sampling tolerance.

Reorder/insert/duplicate removes transitions whose exact pair is no longer
adjacent; it never transfers an effect to another pair. Scene trim clamps an
existing overlap to the new limit, or removes it if no overlap frame fits.
Deleting a scene removes its adjacent transitions. All are one undoable edit.
Adding/changing/removing overlaps moves zooms with retained scene content;
continuous pieces of the same cue merge, while collisions between distinct
cues are refused with guidance to move those cues away from the boundary.

Range cuts and splits inside an active blend are deliberately refused: remove
the transition first, then cut. The ordered scene model cannot represent an
interior hole in both contributing layers without introducing additional scene
ordering semantics. Cuts outside blends preserve/retarget surviving pairs and
refuse edits leaving insufficient transition room. Nothing silently discards
the selected transition or loses zoom data to make a cut succeed.

Click the timeline's `+`/`F`/`B`/`W`/`S` boundary badge, or select a scene and
press `T`, to edit its transition to the next scene. The Clip inspector shows the actual
frame-snapped overlap and maximum. Selecting Hard cut removes the overlap.
Space and project undo/redo work from these controls.

Real 4K60 recording tests preserve real-time project clock progression but still
coalesce preview frames; neither
the bounded decoder/preparation design nor these tests guarantee sustained
4K60 presentation. Further cadence/proxy work remains tracked in PLAN item 16.
