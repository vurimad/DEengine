"""Light baked over a height map for one sun (shaders/scene/terrain_light.slang reads it): how deep in shadow each place
lies, and how much sky it sees. Offline, so the game pays one texture read for shadows at any distance.

Shadow: for each place, the height the sun's line has to clear there, from everything between it and the sun. Swept
along the sun's direction on a grid turned to face it, in one pass: each sample keeps the higher of its own ground and
the line from the sample before it, dropping by the sun's slope. What's left above the ground is the depth.

Sky: how far each place sits below the land around it, at three sizes (8, 32 and 128 m or so): the feet of cliffs, the
bottoms of valleys and gullies see less of the sky.
"""
import math
import site
import sys

sys.path.append(site.getusersitepackages())
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402


def _bilinear(field, x, z):
    """Samples a 2D field at float cell coordinates (x along columns), clamped at the edges."""
    rows, cols = field.shape
    x = np.clip(x, 0.0, cols - 1.001)
    z = np.clip(z, 0.0, rows - 1.001)
    x0 = x.astype(np.int32)
    z0 = z.astype(np.int32)
    fx = (x - x0).astype(np.float32)
    fz = (z - z0).astype(np.float32)
    a = field[z0, x0] * (1 - fx) + field[z0, x0 + 1] * fx
    b = field[z0 + 1, x0] * (1 - fx) + field[z0 + 1, x0 + 1] * fx
    return a * (1 - fz) + b * fz


def _box_blur(a, radius):
    if radius < 1:
        return a
    out = a.astype(np.float32)
    size = 2 * radius + 1
    for _ in range(3):
        for axis in (0, 1):
            pad = [(radius + 1, radius) if i == axis else (0, 0) for i in range(2)]
            c = np.cumsum(np.pad(out, pad, mode='edge').astype(np.float64), axis=axis)
            out = ((c[size:, :] - c[:-size, :]) if axis == 0 else (c[:, size:] - c[:, :-size])).astype(np.float32) / size
    return out


def shadow_depth(heights, spacing_m, azimuth_deg, elevation_deg):
    """How far above the ground the sun is hidden, in meters, for a sun at this azimuth (degrees clockwise from north, -Z,
    toward east, +X, as the environment's) and elevation."""
    rows, cols = heights.shape
    # Toward the sun on the ground, in (column, row) terms: x east, z south.
    to_sun = np.array([math.sin(math.radians(azimuth_deg)), -math.cos(math.radians(azimuth_deg))])
    across = np.array([-to_sun[1], to_sun[0]])
    drop = math.tan(math.radians(max(elevation_deg, 0.5))) * spacing_m
    # A grid turned so that each row runs away from the sun: u along it (from the sun's side), v across.
    corners = np.array([[0, 0], [cols, 0], [0, rows], [cols, rows]], dtype=np.float64)
    center = np.array([cols / 2.0, rows / 2.0])
    us = (corners - center) @ (-to_sun)
    vs = (corners - center) @ across
    u = np.arange(math.floor(us.min()), math.ceil(us.max()) + 1, dtype=np.float32)
    v = np.arange(math.floor(vs.min()), math.ceil(vs.max()) + 1, dtype=np.float32)
    # Sample: a point at (u, v) is center + u * away + v * across.
    away = -to_sun
    turned_x = center[0] + u[None, :] * away[0] + v[:, None] * across[0]
    turned_z = center[1] + u[None, :] * away[1] + v[:, None] * across[1]
    inside = (turned_x >= 0) & (turned_x <= cols - 1) & (turned_z >= 0) & (turned_z <= rows - 1)
    turned = np.where(inside, _bilinear(heights, turned_x, turned_z), -1e6).astype(np.float32)
    # The sweep, away from the sun.
    line = np.full(turned.shape[0], -1e6, dtype=np.float32)
    plane = np.empty_like(turned)
    for j in range(turned.shape[1]):
        line = np.maximum(turned[:, j], line - drop)
        plane[:, j] = line
    del turned, turned_x, turned_z, inside
    # Back onto the height map's own grid.
    gx = np.arange(cols, dtype=np.float32)[None, :] + 0.0
    gz = np.arange(rows, dtype=np.float32)[:, None] + 0.0
    pu = (gx - center[0]) * away[0] + (gz - center[1]) * away[1] - u[0]
    pv = (gx - center[0]) * across[0] + (gz - center[1]) * across[1] - v[0]
    plane_here = _bilinear(plane, pu, pv)
    return np.maximum(plane_here - heights, 0.0).astype(np.float32)


def sky_seen(heights, spacing_m):
    """The share of the sky a place sees, 0 to 1, from how far it sits below the land around it."""
    occluded = np.zeros_like(heights, dtype=np.float32)
    for size_m, weight, scale_m in ((8.0, 0.35, 2.0), (32.0, 0.35, 8.0), (128.0, 0.3, 30.0)):
        radius = max(1, int(round(size_m / spacing_m / 2.0)))
        below = _box_blur(heights, radius) - heights
        occluded += weight * np.clip(below / scale_m, 0.0, 1.0)
    return np.clip(1.0 - occluded, 0.15, 1.0).astype(np.float32)


def write_light_map(folder, name, heights, spacing_m, azimuth_deg, elevation_deg, size=None):
    """Writes <name>_light.png and its material into folder: r = 1 - sqrt(depth / 1024 m), g = sky; white is all lit."""
    depth = shadow_depth(heights, spacing_m, azimuth_deg, elevation_deg)
    # A soft edge, wider than a sample: shadows from far off are soft.
    depth = _box_blur(depth, 1)
    sky = sky_seen(heights, spacing_m)
    r = 1.0 - np.sqrt(np.clip(depth / 1024.0, 0.0, 1.0))
    channels = [Image.fromarray((np.clip(c, 0, 1) * 255.0 + 0.5).astype(np.uint8)) for c in (r, sky)]
    if size is not None:
        channels = [c.resize(size, Image.BILINEAR) for c in channels]
    white = Image.new('L', channels[0].size, 255)
    Image.merge('RGBA', channels + [white, white]).save(f'{folder}/{name}_light.png')
    import json
    with open(f'{folder}/{name}_light.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'metallic_roughness': f'{name}_light.png'}}, f, indent=1)
    return depth, sky
