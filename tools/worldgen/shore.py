"""The shore map: what the water needs to know of the land under it and around it (shaders/scene/water.slang reads it),
over the whole terrain at a few meters a texel, as a material's metallic-roughness texture the world's sea names:

    r: distance to the nearest land at the sea's level, over REACH_M (1 at REACH_M or more; 0 on land)
    g: the water's depth, over DEPTH_M (0 on land)
    b, a: the way to that land, x and z, at 0.5 + 0.5 x: where waves near the shore run

Waves grow and steepen as the water shallows, run in toward the shore and break white against it, and leave foam
drifting off it, all from these.
"""
import json

import numba
import numpy as np
from PIL import Image

REACH_M = 255.0
DEPTH_M = 64.0


@numba.njit(cache=True)
def _distance(land, cell_m):
    """Distance to the nearest land cell, in meters: two chamfer passes over 8 neighbours (within 8% of exact)."""
    rows, cols = land.shape
    far = 1e9
    d = np.empty((rows, cols), np.float32)
    for r in range(rows):
        for c in range(cols):
            d[r, c] = 0.0 if land[r, c] else far
    straight = cell_m
    diagonal = cell_m * 1.41421356
    for r in range(rows):
        for c in range(cols):
            v = d[r, c]
            if r > 0:
                v = min(v, d[r - 1, c] + straight)
                if c > 0:
                    v = min(v, d[r - 1, c - 1] + diagonal)
                if c < cols - 1:
                    v = min(v, d[r - 1, c + 1] + diagonal)
            if c > 0:
                v = min(v, d[r, c - 1] + straight)
            d[r, c] = v
    for r in range(rows - 1, -1, -1):
        for c in range(cols - 1, -1, -1):
            v = d[r, c]
            if r < rows - 1:
                v = min(v, d[r + 1, c] + straight)
                if c < cols - 1:
                    v = min(v, d[r + 1, c + 1] + diagonal)
                if c > 0:
                    v = min(v, d[r + 1, c - 1] + diagonal)
            if c < cols - 1:
                v = min(v, d[r, c + 1] + straight)
            d[r, c] = v
    return d


def _box(a, radius):
    size = 2 * radius + 1
    out = a.astype(np.float64)
    for axis in (0, 1):
        pad = [(radius + 1, radius) if i == axis else (0, 0) for i in range(2)]
        c = np.cumsum(np.pad(out, pad, mode='edge'), axis=axis)
        out = ((c[size:, :] - c[:-size, :]) if axis == 0 else (c[:, size:] - c[:, :-size])) / size
    return out.astype(np.float32)


def write_shore_map(folder, name, heights, spacing_m, level_m, step=2):
    """Writes <name>_shore.png and its material into folder, from the heights (rows north to south) at spacing_m,
    every step-th sample. Returns the material's file name."""
    rows, cols = heights.shape
    small = heights[: rows - rows % step, : cols - cols % step]
    small = small.reshape(small.shape[0] // step, step, small.shape[1] // step, step).mean(axis=(1, 3)).astype(np.float32)
    cell_m = spacing_m * step
    land = small >= level_m
    distance = _distance(land, np.float32(cell_m))
    depth = np.clip(level_m - small, 0.0, None)
    # The way to the shore: down the distance's slope, smoothed so it turns gently around headlands.
    smooth = _box(np.minimum(distance, REACH_M * 2.0), 3)
    gz, gx = np.gradient(smooth, cell_m)
    length = np.maximum(np.hypot(gx, gz), 1e-6)
    to_shore_x, to_shore_z = -gx / length, -gz / length
    channels = [distance / REACH_M, depth / DEPTH_M, 0.5 + 0.5 * to_shore_x, 0.5 + 0.5 * to_shore_z]
    image = Image.merge('RGBA', [Image.fromarray((np.clip(c, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)) for c in channels])
    image.save(f'{folder}/{name}_shore.png')
    with open(f'{folder}/{name}_shore.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'metallic_roughness': f'{name}_shore.png'}}, f, indent=1)
    return f'{name}_shore.material'
