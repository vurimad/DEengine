# image_diff

Compares two PNGs, so an agent can tell whether a picture changed (the engine's side of this: `engine/app/README.md`, The agent's eyes). C++, like the other tools here, on `platform/file.h` and `stb_image`, so it runs wherever the engine builds.

```
image_diff A.png B.png [--mask M.png] [--threshold N] [--max-pixels N] [--out DIFF.png]
```

| Flag | What it does |
| --- | --- |
| `--threshold N` | How much a pixel may differ and still count as the same, 0 to 255 (default 0: exactly) |
| `--max-pixels N` | How many pixels may differ by more than that (default 0) |
| `--mask M.png` | A picture of the same size. Where it's black (red, green and blue all below 128) pixels aren't compared; paint out what may move |
| `--out DIFF.png` | Writes what differs, passing or not: red on a dim grey copy of A, at least half bright so one level shows, black where the mask leaves pixels out. Makes its folder |

Paths are the OS's. Any PNG will do: grey, palette and 16-bit ones are read as 8-bit RGBA. A file without alpha reads as opaque.

## What it says

One line (`[info] image_diff:`, on stderr, as the cooker's are):

```
FAIL: 145280 of 1440000 pixels differ by over 2 (0 allowed); max 112 at 581,284; mean 0.856534
```

A pixel's difference is its largest channel's, alpha included. The count is of pixels past the threshold, and the maximum (with the first pixel to reach it) and the mean are over all the pixels the mask leaves, whatever the threshold.

| Exit code | Means |
| --- | --- |
| 0 | Same, within the threshold |
| 1 | Different |
| 2 | Nothing was compared: bad arguments, a file that can't be read or isn't a PNG, sizes that differ, a mask that leaves nothing |

## Which threshold

Pictures from one GPU and driver with the settle rule and the fixed step are identical, so baselines are per GPU and the default of no difference at all is the right one there (the measurements: `engine/app/README.md`). Pictures from other GPUs differ: compare them only with a threshold measured for that pair.
