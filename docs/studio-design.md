# Studio chrome: Quattro first

Studio uses Bettershot as an editing-workflow reference, not as its visual theme.
Its controls follow Omarchy Quattro; screenshot chrome is unchanged.

## Current contract

Verified against the installed Quattro shell on 2026-09-06:

- `Commons/Color.qml` reads `~/.local/state/omarchy/current/theme/colors.toml`.
  The old `~/.config/omarchy/current/theme` location is not the current contract.
- `Commons/Style.qml` defaults to zero corner radius, a 12 px monospace body,
  10/6 px control padding, an 8 px control gap, and 18 px panel padding.
  Normal/hover/selected/pressed fills use foreground at 4/8/18/22 percent.
- The shell can additionally customize surfaces/states/font sizes in
  `shell.toml` and derive rounding from Hyprland. Studio does **not** implement
  the full shell configuration API: it intentionally uses shared square geometry,
  default control metrics, and the pinned monospace helper. It reads no Hyprland
  configuration and performs no shell restart or theme switch.

`StudioChrome` centralizes palette roles, derived colors, geometry, and the Qt
Widgets stylesheet. Custom-painted timeline and preview chrome consume the same
palette. `chromeMonoFont()` retains the established monospace/fontconfig family
path without loading a desktop font/theme plugin. The normal screenshot font
helpers and application defaults are untouched.

## Palette resolution

| Role | Preferred key | Alternative |
|---|---|---|
| Background | `background` | `color0` |
| Foreground | `foreground` | `color7` |
| Accent | `accent` | `color4` |
| Muted | `muted` | `color8`, then foreground |
| Urgent/error | `red` | `color1` |

Explicit roles win regardless of line order. Recognized values are six-digit
hex colors, single/double quoted or bare, with optional trailing comments.
Unknown keys and invalid values are ignored; this is a bounded palette parser,
not a new general TOML dependency. Missing foundational roles use built-in
defaults. A missing, entirely invalid, non-regular, or oversized file falls back
to the whole built-in palette rather than retaining colors from a previous theme.

Defaults match Quattro's foundational palette: background `#101315`, foreground
and accent `#cacccc`, urgent `#a55555`, muted `#707880`. Small muted labels are
blended toward foreground as needed for legibility, and filled accent controls
choose black/white text by contrast. Status errors use the urgent role.

## Reload and performance

`StudioTheme` reads on a Qt Concurrent worker at startup and once per second.
Reads are limited to 64 KiB. At most one read runs at a time, with a coalesced
request if a reload arrives while busy. The logical filename is reopened so
atomic file replacement and whole theme-directory/symlink swaps are recognized.
Polling avoids depending on a specific shell IPC API or a watcher attached to
an obsolete inode. The tiny palette is the only periodically read input.

Only a changed resolved palette triggers restyling. No filesystem reads, process
spawns, parsing, or style-sheet generation happen in frame/paint callbacks.
Invalid startup data uses the built-in theme while the first read completes.
No settings UI, theme hook, shell process, or new package is needed.

## Chrome is not the canvas

- Theme state is not part of `StudioStyle`, edit history, or the video sidecar.
- Theme reload changes controls, timeline, preview surround, and editing markers.
- Saved canvas backgrounds, video padding/roundness, source pixels, and FFmpeg
  arguments are independent of the active theme.
- Reload must not seek, pause, reset selection, or add an undo step.
- Existing and future dialogs/menus must inherit the same resolved stylesheet.
  New controls must not introduce fixed accent colors or arbitrary rounding.

There are no Select/Range mode buttons: timeline gestures choose the operation.
Split/Delete are non-checkable actions on the right. Keyboard focus uses a
dashed foreground border, distinct from a checked control's solid accent/fill.
Shift+drag selects a range directly on the timeline; no separate numeric range
panel or preview transport is shown. The ordinary transport reviews the selection.
Ctrl+drag paints a cached clip card at the pointer, not a second
decoder or a project edit per mouse move; dropping commits one history entry.

## Verification

`tests/studio-theme-smoke.cpp` covers role precedence, ANSI alternatives,
malformed/missing/oversized data, atomic replacement, directory replacement,
unchanged-palette suppression, square stylesheet tokens, and canvas isolation.
Studio interaction tests change the palette during playback and verify that
transport and edits survive. Live Wayland checks remain necessary for GPU
surround rendering, popup appearance, focus, and scaling; offscreen checks alone
do not establish native rendering performance.
