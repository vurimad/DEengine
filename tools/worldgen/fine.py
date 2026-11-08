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


def paste_heights(fine, fine_meta, box_west_e, box_north_n, heights, west_e, north_n, blend_m=200.0):
    """Puts heights from elsewhere (a terrain tool's export, say) over the 1 m data where they lie, in place, fading
    into what's there over blend_m from their edges. heights: 1 m samples, row 0 north, sample (c, r) centred on
    (west_e + c + 0.5, north_n - r - 0.5) in the same projection as the box (the 1 m data's origin is in metres from the
    box's north-west corner, box_west_e and box_north_n). Their units needn't be metres: they're fitted to what the 1 m
    data has under them (a scale and an offset, least squares), so an export in a tool's own units comes back in metres.
    Returns the scale and offset found, and how far the pasted heights stray from what was there (rms, metres)."""
    p0, q0 = fine_meta['origin_in_aoraki_box_m']
    col = int(round(west_e - box_west_e - p0))
    row = int(round(box_north_n - north_n - q0))
    rows, cols = heights.shape
    r0, r1 = max(row, 0), min(row + rows, fine.shape[0])
    c0, c1 = max(col, 0), min(col + cols, fine.shape[1])
    assert r1 > r0 and c1 > c0, 'the heights lie outside the 1 m data'
    under = fine[r0:r1, c0:c1]
    patch = heights[r0 - row:r1 - row, c0 - col:c1 - col].astype(np.float64)
    known = np.isfinite(under)
    a = np.vstack([patch[known], np.ones(int(known.sum()))]).T
    (scale, offset), *_ = np.linalg.lstsq(a, under[known].astype(np.float64), rcond=None)
    metres = (patch * scale + offset).astype(np.float32)
    stray = float(np.sqrt(np.mean((metres[known] - under[known]) ** 2)))
    # Fading in from the heights' own edges.
    rr = np.arange(r0 - row, r1 - row, dtype=np.float32)[:, None]
    cc = np.arange(c0 - col, c1 - col, dtype=np.float32)[None, :]
    inside = np.minimum(np.minimum(rr, rows - 1 - rr), np.minimum(cc, cols - 1 - cc))
    t = np.clip(inside / blend_m, 0.0, 1.0)
    weight = t * t * (3.0 - 2.0 * t)
    fine[r0:r1, c0:c1] = np.where(known, under + (metres - under) * weight, metres)
    return float(scale), float(offset), stray


def at_vertices(values, col0, row0, cols, rows, blend_samples):
    """A 1 m map at a map's 2 m vertices: vertex (i, j) lies at (col0 + 2 i, row0 + 2 j) in its samples, between two
    each way (a whole number and a half), and takes the mean of the four around it. NaN where it doesn't reach; and a
    weight fading from 0 at its edges to 1 blend_samples in, to lay it over something else."""
    out = np.full((rows, cols), np.nan, np.float32)
    weight = np.zeros((rows, cols), np.float32)
    c = col0 + 2.0 * np.arange(cols)
    r = row0 + 2.0 * np.arange(rows)
    lo_c = np.floor(c).astype(np.int64)
    lo_r = np.floor(r).astype(np.int64)
    ii = np.nonzero((lo_c >= 0) & (lo_c + 1 < values.shape[1]))[0]
    jj = np.nonzero((lo_r >= 0) & (lo_r + 1 < values.shape[0]))[0]
    if ii.size == 0 or jj.size == 0:
        return out, weight
    lc, lr = lo_c[ii], lo_r[jj]
    v = values.astype(np.float32)
    mean = 0.25 * (v[np.ix_(lr, lc)] + v[np.ix_(lr, lc + 1)] + v[np.ix_(lr + 1, lc)] + v[np.ix_(lr + 1, lc + 1)])
    out[np.ix_(jj, ii)] = mean
    inside_c = np.minimum(c[ii], values.shape[1] - 1 - c[ii])
    inside_r = np.minimum(r[jj], values.shape[0] - 1 - r[jj])
    t = np.clip(np.minimum(inside_r[:, None], inside_c[None, :]) / blend_samples, 0.0, 1.0)
    weight[np.ix_(jj, ii)] = (t * t * (3.0 - 2.0 * t)).astype(np.float32)
    return out, weight
