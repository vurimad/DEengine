#!/usr/bin/env python3
"""Generates the terrain capacity test map: the least the engine must carry (docs/design.md, Terrain & vegetation).

16 x 16 km at 2 m, 8192 x 8192 samples: fractal noise with a few steep ranges, written as a 16-bit grayscale PNG with
its .terrain beside it, to the folder real maps use, which isn't committed. Cook it with the cooker. The same seed gives
the same file. Needs numpy.

    python tools/terrain_testmap.py [--out content/terrain] [--name capacity] [--size 8192] [--seed 1]
"""

import argparse
import json
import pathlib
import struct
import zlib

import numpy as np

METERS_PER_PIXEL = 2.0


def noise_layer(rng, size, cells):
    """One octave: random values on a grid of cells across the map, smoothly interpolated, in [-1, 1]."""
    grid = rng.uniform(-1.0, 1.0, (cells + 2, cells + 2)).astype(np.float32)
    at = np.arange(size, dtype=np.float32) * cells / size
    index = at.astype(np.int32)
    t = at - index
    t = t * t * (3.0 - 2.0 * t)
    near_rows = grid[index]
    far_rows = grid[index + 1]
    rows = near_rows[:, index] * (1.0 - t) + near_rows[:, index + 1] * t
    rows_next = far_rows[:, index] * (1.0 - t) + far_rows[:, index + 1] * t
    return rows * (1.0 - t)[:, None] + rows_next * t[:, None]


def fractal(rng, size, first_cells, octaves, gain=0.5):
    """Octaves of noise, each twice as fine as the last and gain times as strong, in about [-1, 1]."""
    total = np.zeros((size, size), dtype=np.float32)
    amplitude = 1.0
    norm = 0.0
    cells = first_cells
    for _ in range(octaves):
        total += noise_layer(rng, size, min(cells, size)) * amplitude
        norm += amplitude
        amplitude *= gain
        cells *= 2
    return total / norm


def range_mask(size, start, end, half_width_m):
    """1 along the line from start to end (in shares of the map), falling smoothly to 0 at half_width_m from it."""
    at = np.arange(size, dtype=np.float32) / size
    x = at[None, :]
    z = at[:, None]
    ax, az = start
    bx, bz = end
    dx, dz = bx - ax, bz - az
    t = np.clip(((x - ax) * dx + (z - az) * dz) / (dx * dx + dz * dz), 0.0, 1.0)
    distance = np.hypot(x - (ax + t * dx), z - (az + t * dz)) * size * METERS_PER_PIXEL
    falloff = np.clip(1.0 - distance / half_width_m, 0.0, 1.0)
    return falloff * falloff * (3.0 - 2.0 * falloff)


def generate(size, seed):
    """Heights in meters: rolling land, then ridged ranges that rise 984 m across a 2 m node at their steepest."""
    rng = np.random.default_rng(seed)
    height = 220.0 + 200.0 * fractal(rng, size, 4, 9)
    ranges = (
        (((0.10, 0.25), (0.55, 0.15)), 1500.0, 1400.0),
        (((0.70, 0.30), (0.90, 0.85)), 1800.0, 1600.0),
        (((0.15, 0.80), (0.60, 0.65)), 1300.0, 1200.0),
    )
    ridges = 1.0 - np.abs(fractal(rng, size, 24, 7, 0.45)) * 2.0
    ridges = np.clip(ridges, 0.0, 1.0)
    for line, rise_m, half_width_m in ranges:
        height += rise_m * range_mask(size, line[0], line[1], half_width_m) * (0.55 + 0.45 * ridges)
    return height


def write_png(path, samples):
    """A 16-bit grayscale PNG, rows from the north."""

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    depth, width = samples.shape
    big_endian = samples.astype(">u2").tobytes()
    row_bytes = width * 2
    compress = zlib.compressobj(6)
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, depth, 16, 0, 0, 0, 0)))
        parts = []
        for row in range(depth):
            parts.append(compress.compress(b"\x00" + big_endian[row * row_bytes : (row + 1) * row_bytes]))
        parts.append(compress.flush())
        out.write(chunk(b"IDAT", b"".join(parts)))
        out.write(chunk(b"IEND", b""))


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=pathlib.Path, default=root / "content" / "terrain")
    parser.add_argument("--name", default="capacity")
    parser.add_argument("--size", type=int, default=8192, help="samples on a side")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    height = generate(args.size, args.seed)
    lowest = float(height.min())
    highest = float(height.max())
    samples = np.round((height - lowest) / (highest - lowest) * 65535.0).astype(np.uint16)

    args.out.mkdir(parents=True, exist_ok=True)
    image = args.out / f"{args.name}.heightpng"
    write_png(image, samples)
    source = {
        "height": image.name,
        "meters_per_pixel": METERS_PER_PIXEL,
        "lowest_m": lowest,
        "highest_m": highest,
        "origin": [0, 0],
    }
    (args.out / f"{args.name}.terrain").write_text(json.dumps(source) + "\n", newline="\n")
    size_km = (args.size - 1) * METERS_PER_PIXEL / 1000.0
    print(f"{image}: {args.size} x {args.size} samples, {size_km:.2f} km across, {lowest:.1f} to {highest:.1f} m")


if __name__ == "__main__":
    main()
