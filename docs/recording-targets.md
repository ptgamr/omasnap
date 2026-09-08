# Recording targets and gpu-screen-recorder coordinates

The recording plan
([docs/recording-studio-plan.md](recording-studio-plan.md)) says the GSR
region coordinate space must be *proven*, never assumed to equal either
rectangle Omasnap already has. This is that proof, and the contract it
settles.

## What Omasnap already has

Hyprland reports each output's logical position and size, and
`parseMonitor()` turns that into `MonitorInfo::geometry` — a
**compositor-global logical** rectangle — while dividing out the scale and
swapping the axes for a rotated output. The selector then works in
`CaptureData::previewSize`, which is `geometry.size()`: the same logical
units, but **monitor-local**.

So a selection is monitor-local logical, and the only conversion the
recorder needs is a translation by `geometry.topLeft()`.

## What gpu-screen-recorder wants

Measured against **gpu-screen-recorder 6.0.1** on Hyprland 0.56, on
`HDMI-A-1`: 3840×2160 mode, `scale = 1.5`, logical origin `(2560, 0)`,
so logical size 2560×1440.

```
gpu-screen-recorder -w 800x600+3000+300 -f 30 -c mp4 -k h264 -o b.mp4
```

- The encoded video is **1200×900** — that is `800 × 1.5` by `600 × 1.5`.
  GSR takes the region in logical units and encodes at native pixels.
- Comparing a frame against a simultaneous `grim -o HDMI-A-1` shot (3840×2160
  native pixels), the frame matches the crop at native-pixel offset
  **(660, 450)**, mean absolute luma difference 0.75/255. That offset is
  `(3000 − 2560) × 1.5` by `300 × 1.5`.
  - Reading the offset as native pixels instead — crop at (440, 300) — scores
    8.6/255, more than eleven times worse, and an exhaustive coarse search over
    the whole screenshot found nothing better than the logical reading.

**Conclusion:** `-w WxH+X+Y` is compositor-**global logical**, and the output
resolution is that rectangle multiplied by the monitor scale.
`MonitorInfo::geometry.translated()` of a selection is therefore exactly the
argument, with no scale factor applied and no per-monitor origin fixup.

Two further facts from the same run, both load-bearing:

- The separate `-region` flag is **deprecated** in 6.0.1; it warns and tells
  you to pass the geometry inline to `-w`. `recordTargetSourceArguments()`
  emits the inline form.
- `--list-monitors` reports `DP-1|1440x2560` for a rotated output and
  `HDMI-A-1|3840x2160` for a scaled one: post-transform, pre-scale-division.
  That is *not* `MonitorInfo::pixelSize` (the raw mode), which is why
  `makeRecordTarget()` derives `sourcePixels` from `logical × scale` instead.

## The contract

`RecordTarget` (`src/record-target.hpp`) is versioned because it crosses a
process boundary: the selector writes it, the recorder reads it.

| Field | Space | Used for |
|---|---|---|
| `logical` | monitor-local logical | UI, and clamping against the output |
| `globalLogical` | compositor-global logical | the GSR `-w` argument |
| `sourcePixels` | native pixels | the resolution to expect from the encoder |
| `output` | Hyprland/GSR output name | `-w <output>` for a whole display |

A selection arrives as a `QRectF`, so `makeRecordTarget()` rounds *outward*
to whole logical pixels — rounding inward would silently drop a column the
user dragged over — and then intersects with the monitor, because GSR rejects
a region that hangs off the output.

## Still unproven

The plan asks for the matrix; this covers 1× and 1.5×. Not yet measured here:
1.25×, 2×, a rotated output's region (as opposed to its whole-display
capture), and a negative-origin output. Treat those as open until someone
runs the same comparison on them.
