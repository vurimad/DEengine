"""Fine detail from 1 m height data, for maps at 2 m: the heights a map's vertices take from it, and a normal map at
1 m over the whole map (the .terrainlayers "normals"), which keeps the detail the 2 m mesh can't hold and which the
renderer shades by even where the mesh far off is coarse.

The 1 m data is local only (build/terrain_raw/*, never committed): see its JSON for the source and licence.
"""
import json

import numpy as np
from PIL import Image

# Each way, from four 1 m samples to a 2 m vertex between the middle two: a box of 2 under a tent, so the vertices
# don't alias the finer detail the normal map carries instead.
FILTER = np.array([1.0, 3.0, 3.0, 1.0], np.float32) / 8.0


def load(folder, name):
    """The 1 m heights in meters, NaN where there's no data, and their metadata."""
    with open(f'{folder}/{name}.json', encoding='utf-8') as f:
        meta = json.load(f)
    raw = np.load(f'{folder}/{name}.npy')
    lowest, step = float(meta['lowest_m']), (float(meta['highest_m']) - float(meta['lowest_m'])) / 65534.0
    out = np.empty(raw.shape, np.float32)
    for r in range(0, raw.shape[0], 1024):
        q = raw[r:r + 1024]
        out[r:r + 1024] = np.where(q > 0, lowest + (q.astype(np.float32) - 1.0) * step, np.nan)
    return out, meta


def vertex_heights(fine, col0, row0, cols, rows):
    """Heights at a map's 2 m vertices from 1 m samples. Vertex (i, j) lies at (col0 + 2 i, row0 + 2 j) in fine's
    samples, between two of them each way (col0 and row0 are a whole number and a half). NaN where any sample is."""
    c, r = int(col0 - 1.5), int(row0 - 1.5)
    assert c >= 0 and r >= 0 and c + 2 * cols + 2 <= fine.shape[1] and r + 2 * rows + 2 <= fine.shape[0], 'outside the 1 m data'
    across = sum(FILTER[a] * fine[r + a:r + a + 2 * rows:2] for a in range(4))
    return sum(FILTER[b] * across[:, c + b:c + b + 2 * cols:2] for b in range(4)).astype(np.float32)


def up2(values):
    """Values at the vertices, at the texel centres between them (twice as dense): (rows, cols) to
    (2 rows - 2, 2 cols - 2), as the renderer's map covers the vertices' extent."""
    a, b = values[:, :-1], values[:, 1:]
    across = np.empty((values.shape[0], 2 * (values.shape[1] - 1)), np.float32)
    across[:, 0::2] = 0.75 * a + 0.25 * b
    across[:, 1::2] = 0.25 * a + 0.75 * b
    a, b = across[:-1], across[1:]
    out = np.empty((2 * (values.shape[0] - 1), across.shape[1]), np.float32)
    out[0::2] = 0.75 * a + 0.25 * b
    out[1::2] = 0.25 * a + 0.75 * b
    return out


def write_normal_map(path, heights, spacing_m, fine=None, fine_vertices=None, col0=0.0, row0=0.0, share=None, strip=256):
    """The normal over the map at half its spacing, between the vertices: red its x (east), green its z (south), blue
    its y (up), as the cooker's normal maps are. heights: the map's own vertices. With fine, the 1 m slope that its 2 m
    vertices (fine_vertices, from vertex_heights) lose is added back, times share, a vertex map (how much of the fine
    data the map's heights took at each place: 0 where none)."""
    rows, cols = heights.shape
    gz, gx = np.gradient(heights.astype(np.float32), spacing_m)
    if fine is not None:
        fz, fx = np.gradient(fine_vertices, spacing_m)
    texel_m = spacing_m / 2.0
    out = np.empty((2 * (rows - 1), 2 * (cols - 1), 3), np.uint8)
    for v0 in range(0, rows - 1, strip):
        v1 = min(v0 + strip, rows - 1)
        t0, t1 = 2 * v0, 2 * v1
        sx, sz = up2(gx[v0:v1 + 1]), up2(gz[v0:v1 + 1])
        if fine is not None:
            # Texel (k, l) lies on fine's sample (col0 + 0.5 + k, row0 + 0.5 + l).
            r, c = int(row0 + 0.5) + t0, int(col0 + 0.5)
            block = fine[r - 1:r + (t1 - t0) + 1, c - 1:c + out.shape[1] + 1]
            dx = (block[1:-1, 2:] - block[1:-1, :-2]) / (2.0 * texel_m) - up2(fx[v0:v1 + 1])
            dz = (block[2:, 1:-1] - block[:-2, 1:-1]) / (2.0 * texel_m) - up2(fz[v0:v1 + 1])
            amount = up2(share[v0:v1 + 1])
            sx += np.where(np.isfinite(dx), dx, 0.0) * amount
            sz += np.where(np.isfinite(dz), dz, 0.0) * amount
        inverse = 1.0 / np.sqrt(sx * sx + sz * sz + 1.0)
        for channel, value in enumerate((-sx * inverse, -sz * inverse, inverse)):
            out[t0:t1, :, channel] = np.clip(value * 127.5 + 128.0, 0.0, 255.0).astype(np.uint8)
    Image.fromarray(out).save(path, format='PNG', compress_level=1)
    return out.shape[1], out.shape[0]
