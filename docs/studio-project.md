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
