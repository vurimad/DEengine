"""The shore map: what the water needs to know of the land under it and around it (shaders/scene/water.slang reads it),
over the whole terrain at a few meters a texel, as a material's metallic-roughness texture the world's sea names. It's
read texel by texel and filtered by the shader, so it isn't compressed (meta/.../_folder.json: "*_shore.*"):

    r, g: distance to the nearest land at the sea's level, in centimeters, high byte then low (0 on land, up to 655 m)
    b: the water's depth, over DEPTH_M (0 on land)
    a: 1

The way to land is the distance's own slope.

Waves grow and steepen as the water shallows, run in toward the shore and break white against it, and leave foam
drifting off it, all from these.
"""
import json

import numba
import numpy as np
from PIL import Image

DISTANCE_STEP_M = 0.01
DEPTH_M = 64.0


@numba.njit(cache=True)
def _squared_1d(f, out, v, z):
    """Felzenszwalb and Huttenlocher's distance transform of one line of squared distances: the lower envelope of the
    parabolas rooted at each sample."""
    n = f.shape[0]
    k = 0
    v[0] = 0
    z[0] = -1e30
    z[1] = 1e30
    for q in range(1, n):
        s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2.0 * q - 2.0 * v[k])
        while s <= z[k]:
            k -= 1
            s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2.0 * q - 2.0 * v[k])
        k += 1
        v[k] = q
        z[k] = s
        z[k + 1] = 1e30
    k = 0
    for q in range(n):
        while z[k + 1] < q:
            k += 1
        out[q] = (q - v[k]) * (q - v[k]) + f[v[k]]


@numba.njit(cache=True)
def _distance(land, cell_m):
    """Exact distance to the nearest land cell, in meters: squared distances along rows, then along columns."""
    rows, cols = land.shape
    far = 1e18
    d = np.empty((rows, cols), np.float64)
    longest = max(rows, cols)
    line = np.empty(longest, np.float64)
    out = np.empty(longest, np.float64)
    v = np.empty(longest, np.int64)
    z = np.empty(longest + 1, np.float64)
    for r in range(rows):
        for c in range(cols):
            line[c] = 0.0 if land[r, c] else far
        _squared_1d(line[:cols], out[:cols], v, z)
        for c in range(cols):
            d[r, c] = out[c]
    for c in range(cols):
        for r in range(rows):
            line[r] = d[r, c]
        _squared_1d(line[:rows], out[:rows], v, z)
        for r in range(rows):
            d[r, c] = out[r]
    result = np.empty((rows, cols), np.float32)
    for r in range(rows):
        for c in range(cols):
            result[r, c] = np.sqrt(d[r, c]) * cell_m
    return result


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
    steps = np.clip(np.round(distance / DISTANCE_STEP_M), 0, 65535).astype(np.uint32)
    channels = [(steps >> 8).astype(np.uint8), (steps & 255).astype(np.uint8),
                (np.clip(depth / DEPTH_M, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), np.full(depth.shape, 255, np.uint8)]
    image = Image.merge('RGBA', [Image.fromarray(c) for c in channels])
    image.save(f'{folder}/{name}_shore.png')
    with open(f'{folder}/{name}_shore.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'metallic_roughness': f'{name}_shore.png'}}, f, indent=1)
    return f'{name}_shore.material'
