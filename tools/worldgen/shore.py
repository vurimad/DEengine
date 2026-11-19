"""The shore map: what the water needs to know of the land under it and around it (shaders/scene/water.slang reads it),
over the whole terrain at a few meters a texel, as a material's metallic-roughness texture the world's sea names. It's
read texel by texel and filtered by the shader, so it isn't compressed (a project's meta/.../_folder.json: "*_shore.*"):

    r, g: the signed distance to the waterline at the sea's level, in centimeters, high byte then low, offset by 32768:
          out to sea it's positive, into the land negative (to +-327 m), so filtered it crosses zero smoothly along the
          real waterline, where the water can end and the swash climbs the beach
    b: the water's depth, over DEPTH_M (0 on land)
    a: the ground's height about the sea's level, from GROUND_LOW_M to GROUND_HIGH_M: finely, where the water meets it,
       so the waterline and the swash follow the ground's real slope, not the steps of its cooked heights

The way to land is the distance's own slope.

Waves grow and steepen as the water shallows, run in toward the shore and break white against it, and leave foam
drifting off it, all from these.
"""
import json
import os
import sys

import numba
import numpy as np
from PIL import Image

DISTANCE_STEP_M = 0.01
SIGNED_OFFSET = 32768  # the stored distance's zero: the waterline
GROUND_LOW_M, GROUND_HIGH_M = -1.0, 3.0
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


def _signed_distance(heights, cell_m, level_m):
    """The distance to the waterline at each of the heights' cells, in meters: out to sea positive, into the land
    negative."""
    land = heights >= level_m
    # Cell to cell: out at sea, to the nearest land cell's middle; in the land, to the nearest water cell's. The
    # waterline lies between the two, half a cell from each.
    to_land = _distance(land, np.float32(cell_m))
    to_water = _distance(~land, np.float32(cell_m))
    signed = np.where(land, -(to_water - 0.5 * cell_m), to_land - 0.5 * cell_m)
    # Near the waterline, finer than a cell: how far the ground is below the sea's level, over its slope, is the
    # distance to where it crosses it. Cell to cell, the waterline would follow the cells' edges, in steps.
    # The one eases into the other over a few cells (all the fine one within a cell of the waterline, none of it
    # past three), and the fine one is never taken farther from the cells' than they can be wrong by: switching
    # from one to the other at a distance left a jump of up to half a cell along a line that far from the
    # waterline, either side of it, and everything the water draws by this distance stepped across that line.
    gz, gx = np.gradient(heights, cell_m)
    slope = np.maximum(np.hypot(gx, gz), 1e-3)
    fine = np.clip((level_m - heights) / slope, signed - 0.75 * cell_m, signed + 0.75 * cell_m)
    far = np.clip((np.abs(signed) - cell_m) / (2.0 * cell_m), 0.0, 1.0)
    far = far * far * (3.0 - 2.0 * far)
    return (fine * (1.0 - far) + signed * far).astype(np.float32)


def write_shore_map(folder, name, heights, spacing_m, level_m, step=2, film=None):
    """Writes <name>_shore.png and its material into folder, from the heights (rows north to south) at spacing_m,
    every step-th sample. film: the surf's film beside it (surf_film.write_surf_film: {slot: file name}), which the
    material carries too. Returns the material's file name."""
    rows, cols = heights.shape
    full = np.ascontiguousarray(heights[: rows - rows % step, : cols - cols % step], dtype=np.float32)
    small = full.reshape(full.shape[0] // step, step, full.shape[1] // step, step).mean(axis=(1, 3)).astype(np.float32)
    # The distance from the heights as fine as they come, then averaged down to the map's texels: the cells it's
    # measured between are half the size, and so is what it can be wrong by.
    signed = _signed_distance(full, spacing_m, level_m)
    signed = signed.reshape(small.shape[0], step, small.shape[1], step).mean(axis=(1, 3)).astype(np.float32)
    depth = np.clip(level_m - small, 0.0, None)
    steps = np.clip(np.round(signed / DISTANCE_STEP_M) + SIGNED_OFFSET, 0, 65535).astype(np.uint32)
    channels = [(steps >> 8).astype(np.uint8), (steps & 255).astype(np.uint8),
                (np.clip(depth / DEPTH_M, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8),
                (np.clip((small - level_m - GROUND_LOW_M) / (GROUND_HIGH_M - GROUND_LOW_M), 0.0, 1.0) * 255.0 + 0.5)
                .astype(np.uint8)]
    image = Image.merge('RGBA', [Image.fromarray(c) for c in channels])
    image.save(f'{folder}/{name}_shore.png')
    with open(f'{folder}/{name}_shore.material', 'w', encoding='utf-8') as f:
        textures = {'metallic_roughness': f'{name}_shore.png'}
        if film is not None:
            textures.update(film)
        json.dump({'textures': textures}, f, indent=1)
    return f'{name}_shore.material'


def main():
    """python tools/worldgen/shore.py <world>.terrain <folder> <name>: the shore map again, from the terrain's heights
    as they are, into folder; the material keeps whatever else it carried (the surf's film)."""
    terrain_path, folder, name = sys.argv[1:4]
    with open(terrain_path, encoding='utf-8') as f:
        terrain = json.load(f)
    pixels = np.asarray(Image.open(os.path.join(os.path.dirname(terrain_path), terrain['height'])))
    heights = terrain['lowest_m'] + pixels.astype(np.float32) / 65535.0 * (terrain['highest_m'] - terrain['lowest_m'])
    film = None
    material = f'{folder}/{name}_shore.material'
    if os.path.exists(material):
        with open(material, encoding='utf-8') as f:
            film = {slot: file for slot, file in json.load(f)['textures'].items() if slot != 'metallic_roughness'}
    print(write_shore_map(folder, name, heights, terrain['meters_per_pixel'], 0.0, film=film or None))


if __name__ == '__main__':
    main()
