"""A map from a layout: regions at set heights, walls and layers between them, ways up, and room for water and towns.

The layout below is by hand, so geography stays a decision: where each region is, how high it stands, which way rivers and
roads run. Noise only wanders its edges and fills in the ground. Output goes under projects/dreadworld/content/terrain/, which isn't committed.

    python tools/worldgen/worldgen.py sketch    a top-down picture of the layout, in seconds
    python tools/worldgen/worldgen.py build     the height image, masks, ground rules, world and views
    python tools/worldgen/worldgen.py build real   the same with a real range in the north (REAL_SOURCE), as realm_real

Everything is in meters, X east, Z south. Needs numpy and Pillow.
"""
import json
import math
import os
import site
import sys
import time

sys.path.append(site.getusersitepackages())
import numpy as np  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import erosion  # noqa: E402
import lightbake  # noqa: E402

Image.MAX_IMAGE_PIXELS = None
NAME = 'realm'
OUT = 'D:/DE/projects/dreadworld/content/terrain'
EYES = 'D:/DE/build/eyes/realm'
SAMPLES = 8192
SPACING_M = 2.0
WORLD_M = SAMPLES * SPACING_M
LOWEST_M, HIGHEST_M = -60.0, 3800.0
# A real height map to put along the north in place of the eroded range, for `build real`: local, never committed. Its
# folder holds <name>_heights.png (16-bit) and <name>.json (spacing_m, lowest_m, highest_m, and its licence). The
# source's highest point lands at REAL_PEAK_AT_KM.
REAL_SOURCE = ('D:/DE/build/terrain_raw/aoraki', 'aoraki')
REAL_PEAK_AT_KM = (8.6, 1.9)
REAL = False
SEED = 2026
# The sun the map is lit by: the environment says so, and the light map is baked for it.
SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG = 215.0, 32.0

# --- The layout ------------------------------------------------------------------------------------------------------

# What each kind of land is like. relief_m: how far its ground rolls. shelf_m: how far blocks of it stand from each other.
# fine, mid, coarse: how much of it takes each spacing of layers (22 to 66, 44 to 110, 80 to 170 m apart); the rest of the
# ground stays rolling, and patches says how much of it comes in patches rather than everywhere.
BIOMES = {
    'plains': {'color': (146, 170, 84), 'relief_m': 16, 'shelf_m': 16, 'fine': 0.0, 'mid': 0.0, 'coarse': 0.8, 'patches': 0.75},
    'meadow': {'color': (120, 186, 90), 'relief_m': 22, 'shelf_m': 14, 'fine': 0.0, 'mid': 0.7, 'coarse': 0.0, 'patches': 0.8},
    'forest': {'color': (62, 116, 62), 'relief_m': 46, 'shelf_m': 30, 'fine': 0.0, 'mid': 0.2, 'coarse': 0.7, 'patches': 0.45},
    'autumn': {'color': (196, 128, 52), 'relief_m': 40, 'shelf_m': 54, 'fine': 0.1, 'mid': 0.9, 'coarse': 0.0, 'patches': 0.2},
    'red': {'color': (186, 86, 54), 'relief_m': 10, 'shelf_m': 40, 'fine': 1.0, 'mid': 0.0, 'coarse': 0.0, 'patches': 0.0},
    'yellow': {'color': (222, 190, 112), 'relief_m': 6, 'shelf_m': 14, 'fine': 0.0, 'mid': 0.5, 'coarse': 0.0, 'patches': 0.8},
    'marsh': {'color': (92, 124, 104), 'relief_m': 3, 'shelf_m': 3, 'fine': 0.0, 'mid': 0.0, 'coarse': 0.0, 'patches': 1.0},
    'snow': {'color': (226, 232, 240), 'relief_m': 30, 'shelf_m': 40, 'fine': 0.0, 'mid': 0.0, 'coarse': 0.0, 'patches': 0.0},
}
TIER_M = [24, 88, 196, 336, 516, 700, 930, 1130]

# The land: lobes that make up the main island and the small ones around it (x km, z km, radius km), minus bays cut into
# it. Coasts are frayed at every size from 2 km to 150 m, so they come out as arms, bays, points and islets rather than
# the circles they start as. Elden Ring's structure in our own shapes, for now: a start lowland in the south, its way
# north-west held by a castle; a lake basin in the north-west, far below the cliffs of a high plateau in the middle; the
# range on the north lobe with sea past it; snow at the end in the north-east, across a fjord; a red region east, a raised
# mesa beyond it; a marsh peninsula south-east; a forested arm reaching west.
LAND = [(8.2, 7.6, 4.4), (5.0, 6.0, 2.6), (8.8, 3.0, 2.0), (5.6, 3.2, 1.4), (12.8, 3.6, 1.8), (13.2, 8.4, 2.4), (14.6, 6.8, 1.2),
        (7.6, 11.8, 2.6), (11.8, 13.2, 1.7), (12.9, 14.4, 1.0), (3.6, 9.8, 2.0), (1.8, 10.8, 1.3),
        # Islands.
        (2.0, 2.4, 0.9), (1.2, 3.8, 0.45), (15.2, 1.6, 0.7), (15.6, 12.0, 0.6), (2.4, 14.2, 0.8), (6.2, 15.0, 0.4), (10.6, 0.9, 0.5)]
BAYS = [(10.8, 4.2, 0.8), (10.2, 12.8, 0.8), (4.8, 13.2, 0.9), (16.0, 9.8, 0.9)]

# Regions: land nearest each seed is that region, after a wander, so borders are irregular. x km, z km, kind, how high it
# stands (an index into TIER_M).
REGIONS = [
    # The range, on the north lobe, and snow at the end across the fjord.
    (6.6, 2.8, 'snow', 7), (8.8, 2.4, 'snow', 7), (10.4, 3.0, 'snow', 7), (5.0, 3.6, 'snow', 7),
    (12.8, 3.2, 'snow', 6), (12.4, 4.6, 'snow', 6),
    # The high plateau, golden.
    (8.6, 6.0, 'autumn', 5), (10.0, 6.8, 'autumn', 5), (7.4, 6.6, 'autumn', 5),
    # The lake basin, far below it.
    (4.4, 5.4, 'marsh', 0), (3.0, 6.8, 'marsh', 0), (5.2, 7.2, 'marsh', 0),
    # The castle's rock in the pass.
    (5.9, 8.4, 'plains', 2),
    # The forested arm to the west.
    (2.6, 9.8, 'forest', 1), (4.4, 9.6, 'forest', 1), (1.6, 11.0, 'forest', 1),
    # The start lowland.
    (6.8, 10.8, 'plains', 1), (8.6, 10.2, 'plains', 1), (7.4, 12.6, 'plains', 0), (9.2, 12.0, 'plains', 0),
    # The red region and the raised mesa.
    (12.4, 8.0, 'red', 1), (13.8, 9.0, 'red', 2), (12.2, 10.0, 'red', 1), (14.4, 6.8, 'yellow', 4),
    # The marsh peninsula.
    (11.4, 12.8, 'marsh', 0), (12.6, 14.0, 'marsh', 0),
    # The islands.
    (2.0, 2.4, 'forest', 1), (15.2, 1.6, 'snow', 5), (15.6, 12.0, 'red', 1), (2.4, 14.2, 'plains', 0), (10.6, 0.9, 'snow', 5),
]

# Rivers run from their first point to their last, and only downhill: they cut gorges through whatever stands in the way.
# width_m: the bed at the mouth. Water itself comes later.
RIVERS = [
    # Off the range through the lake basin, out to the west sea.
    {'width_m': 26, 'path': [(5.8, 3.8), (5.0, 5.0), (4.2, 6.4), (3.0, 7.2), (1.8, 7.6)]},
    # Out of the snow, down the gorge between the lowland and the red region, to the bay at the peninsula's neck.
    {'width_m': 30, 'path': [(11.8, 4.6), (11.2, 6.4), (11.2, 8.4), (10.8, 10.4), (10.3, 12.4)]},
]
# Bowls that can hold water later: x km, z km, radius m. The lake basin's lakes, around the island with the lake tower.
LAKES = [(3.6, 5.4, 450), (5.2, 5.4, 450), (3.8, 6.8, 420), (5.0, 6.8, 400)]
# Roads run through passes: where one crosses a wall, the land ramps instead.
ROADS = [
    [(7.0, 12.6), (6.8, 11.2), (6.4, 9.8), (5.9, 8.8)],                          # from the start north to the castle gate
    [(5.9, 8.0), (5.4, 7.6), (5.9, 6.0), (5.8, 4.6)],                            # through the castle, along the basin
    [(6.8, 11.2), (5.2, 10.6), (4.4, 9.4), (4.8, 8.0)],                          # around the castle through the forest
    [(5.2, 10.6), (3.8, 10.2), (2.4, 10.4)],                                     # out along the western arm
    [(6.8, 11.2), (8.6, 10.6), (10.0, 10.2), (11.2, 9.6), (13.2, 8.6)],          # east over the gorge into the red region
    [(10.0, 10.2), (10.8, 11.4), (11.4, 12.6), (12.2, 13.6)],                    # over the neck onto the peninsula
    [(5.8, 4.6), (6.6, 4.4), (7.4, 5.0), (8.0, 5.6)],                            # up the plateau's shoulder to the capital
    [(8.0, 5.6), (9.4, 4.8), (11.4, 4.8), (12.8, 3.4)],                          # from the capital into the snow
    [(13.2, 8.6), (14.0, 7.8), (14.4, 6.8)],                                     # up onto the mesa
]
# Level ground for towns, castles and camps: x km, z km, radius m.
TOWNS = [(7.0, 12.6, 180), (5.9, 8.4, 240), (5.8, 4.6, 160), (8.0, 5.6, 300), (13.2, 8.6, 180), (12.2, 13.6, 180),
         (12.8, 3.4, 160), (14.4, 6.8, 150), (2.4, 10.4, 150), (9.0, 10.4, 140)]
# You start near the south coast, facing north: the castle ahead, the plateau's cliffs and its landmark beyond it.
SPAWN = (7.0, 12.8)
# Needles of rock that stand for landmarks until there's art: name, x km, z km, rise m, radius at the foot m. The great
# landmark on the plateau, seen from everywhere, and the tower in the lake basin.
SPIRES = [('the great landmark', 8.6, 6.2, 500.0, 200.0), ('the lake tower', 4.4, 6.1, 140.0, 55.0)]
# Where a great lift will climb the plateau's south cliff from the lowland: x km, z km.
LIFTS = [(9.0, 7.6)]
# Cave mouths, reserved for an underground later, so nothing has to move when it comes: x km, z km.
CAVES = [(4.0, 11.2), (9.6, 8.6), (12.6, 10.6), (14.6, 9.6), (3.2, 4.4), (12.0, 5.2), (11.8, 14.4)]

# The mountains along the north are their own kind of land: sharp peaks where ridges meet, valleys carved wide and flat
# by ice between them, gullies down every face, snow above a line. Peaks: x km, z km, height m, reach m. The first is the
# highest. They stand on the north lobe and rise from the sea past it.
PEAKS = [(9.0, 2.6, 3150, 2600), (6.6, 2.8, 2700, 2200), (11.0, 2.8, 2600, 2000), (5.2, 3.4, 2300, 1600), (13.0, 2.6, 2500, 1800)]
# The range's crest runs west to east along here, and its land reaches about this far south.
ALPINE_AXIS_KM = 2.6
ALPINE_ROWS_KM = 6.5
SNOW_LINE_M = 1550
# Which way to look at the start: north, toward the great landmark.
SPAWN_LOOK = (8.6, 6.2)

# --- Fields ----------------------------------------------------------------------------------------------------------


class Grid:
    """A square of samples over the whole map."""

    def __init__(self, cells):
        self.n = cells
        self.m = WORLD_M / cells
        axis = (np.arange(cells, dtype=np.float32) + 0.5) * self.m
        self.x = axis[None, :]
        self.z = axis[:, None]


rng = np.random.default_rng(SEED)


def noise(grid, cell_m):
    """Smooth, 0 to 1, changing over about cell_m."""
    cells = int(WORLD_M / cell_m) + 2
    coarse = rng.random((cells + 2, cells + 2), dtype=np.float32)
    box = (0.5, 0.5, 0.5 + WORLD_M / cell_m, 0.5 + WORLD_M / cell_m)
    return np.asarray(Image.fromarray(coarse).resize((grid.n, grid.n), Image.BICUBIC, box=box), dtype=np.float32)


def fbm(grid, cell_m, octaves=3):
    out = np.zeros((grid.n, grid.n), dtype=np.float32)
    amp = 0.5
    total = 0.0
    for o in range(octaves):
        size = cell_m / (2 ** o)
        if size < grid.m * 1.5:
            break
        out += noise(grid, size) * amp
        total += amp
        amp *= 0.5
    return out / total


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def box_blur(a, radius):
    """Three box passes, close to a gaussian. radius in cells."""
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


def resize(a, cells, kind=Image.BICUBIC):
    return np.asarray(Image.fromarray(a.astype(np.float32)).resize((cells, cells), kind), dtype=np.float32)


def blocks(grid, size_m, bend_m=0.0):
    """A value for each block of a jittered grid, and how far each point is from its block's edge, in meters. bend_m bends
    the edges, since straight ones hundreds of meters long look machine-made."""
    count = int(WORLD_M / size_m) + 3
    fx = (np.arange(count, dtype=np.float32)[None, :] + rng.random((count, count), dtype=np.float32) - 1.0) * size_m
    fz = (np.arange(count, dtype=np.float32)[:, None] + rng.random((count, count), dtype=np.float32) - 1.0) * size_m
    value = rng.random((count, count), dtype=np.float32)
    px, pz = grid.x, grid.z
    if bend_m > 0.0:
        px = px + (fbm(grid, size_m * 0.6, 2) - 0.5) * 2.0 * bend_m
        pz = pz + (fbm(grid, size_m * 0.6, 2) - 0.5) * 2.0 * bend_m
    ix = (np.floor(px / size_m).astype(np.int32) + 1)
    iz = (np.floor(pz / size_m).astype(np.int32) + 1)
    best = np.full((grid.n, grid.n), 1e14, dtype=np.float32)
    second = np.full((grid.n, grid.n), 1e14, dtype=np.float32)
    out = np.zeros((grid.n, grid.n), dtype=np.float32)
    for dz in (-1, 0, 1):
        zi = np.clip(iz + dz, 0, count - 1)
        for dx in (-1, 0, 1):
            xi = np.clip(ix + dx, 0, count - 1)
            d = (fx[zi, xi] - px) ** 2 + (fz[zi, xi] - pz) ** 2
            closer = d < best
            second = np.where(closer, best, np.minimum(second, d))
            out = np.where(closer, value[zi, xi], out)
            best = np.where(closer, d, best)
    return out, np.sqrt(second) - np.sqrt(best)


def polyline_samples(path_km, step_m):
    points = []
    for (x0, z0), (x1, z1) in zip(path_km, path_km[1:]):
        length = math.hypot(x1 - x0, z1 - z0) * 1000.0
        count = max(int(length / step_m), 1)
        for i in range(count):
            t = i / count
            points.append(((x0 + (x1 - x0) * t) * 1000.0, (z0 + (z1 - z0) * t) * 1000.0))
    points.append((path_km[-1][0] * 1000.0, path_km[-1][1] * 1000.0))
    return np.array(points, dtype=np.float32)


def wander_path(samples, grid_noise_x, grid_noise_z, grid, amount_m):
    """A hand-drawn line made less straight, by the noise fields."""
    ix = np.clip((samples[:, 0] / grid.m).astype(np.int32), 0, grid.n - 1)
    iz = np.clip((samples[:, 1] / grid.m).astype(np.int32), 0, grid.n - 1)
    fade = np.minimum(np.arange(len(samples)), np.arange(len(samples))[::-1]) / max(len(samples) * 0.04, 1.0)
    fade = np.clip(fade, 0, 1)
    out = samples.copy()
    out[:, 0] += (grid_noise_x[iz, ix] - 0.5) * amount_m * fade
    out[:, 1] += (grid_noise_z[iz, ix] - 0.5) * amount_m * fade
    return out


def nearest_samples(grid, samples):
    """Jump flooding: for each cell, which sample is about nearest. Returns its index and the distance."""
    n = grid.n
    index = np.full((n, n), -1, dtype=np.int32)
    ix = np.clip((samples[:, 0] / grid.m).astype(np.int32), 0, n - 1)
    iz = np.clip((samples[:, 1] / grid.m).astype(np.int32), 0, n - 1)
    index[iz, ix] = np.arange(len(samples), dtype=np.int32)
    px = np.broadcast_to(grid.x, (n, n))
    pz = np.broadcast_to(grid.z, (n, n))
    sx = np.concatenate([samples[:, 0], [1e9]])
    sz = np.concatenate([samples[:, 1], [1e9]])

    def dist(idx):
        return (sx[idx] - px) ** 2 + (sz[idx] - pz) ** 2

    step = n // 2
    while step >= 1:
        for dz, dx in ((-step, 0), (step, 0), (0, -step), (0, step), (-step, -step), (-step, step), (step, -step), (step, step)):
            other = np.roll(np.roll(index, dz, axis=0), dx, axis=1)
            better = dist(other) < dist(index)
            index = np.where(better, other, index)
        step //= 2
    return index, np.sqrt(dist(index))


class Layout:
    """The layout on a 16 m grid: which region is where, how high it stands, and how the land there lies."""

    def __init__(self):
        grid = Grid(1024)
        self.grid = grid
        # Regions: land nearest each seed, after a wander, so borders wander too.
        wander_x = (fbm(grid, 1800.0, 3) - 0.5) * 1500.0 + (fbm(grid, 450.0, 2) - 0.5) * 260.0
        wander_z = (fbm(grid, 1800.0, 3) - 0.5) * 1500.0 + (fbm(grid, 450.0, 2) - 0.5) * 260.0
        px = grid.x + wander_x
        pz = grid.z + wander_z
        best = np.full((grid.n, grid.n), 1e14, dtype=np.float32)
        region = np.zeros((grid.n, grid.n), dtype=np.int32)
        for i, (xk, zk, _, _) in enumerate(REGIONS):
            d = (px - xk * 1000.0) ** 2 + (pz - zk * 1000.0) ** 2
            closer = d < best
            best[closer] = d[closer]
            region[closer] = i
        self.region = region
        names = list(BIOMES)
        self.names = names
        self.biome = np.array([names.index(r[2]) for r in REGIONS], dtype=np.int32)[region]
        tier = np.array([TIER_M[r[3]] for r in REGIONS], dtype=np.float32)
        jitter = 1.0 + rng.uniform(-0.08, 0.08, size=len(REGIONS)).astype(np.float32)
        self.level = (tier * jitter)[region]

        # Land: the lobes and islands, minus the bays, its coast frayed at every size.
        qx = grid.x + (fbm(grid, 2600.0, 2) - 0.5) * 1400.0
        qz = grid.z + (fbm(grid, 2600.0, 2) - 0.5) * 1400.0
        shape = np.full((grid.n, grid.n), -1.0, dtype=np.float32)
        for xk, zk, rk in LAND:
            shape = np.maximum(shape, 1.0 - np.hypot(qx - xk * 1000.0, qz - zk * 1000.0) / (rk * 1000.0))
        for xk, zk, rk in BAYS:
            shape = np.minimum(shape, np.hypot(qx - xk * 1000.0, qz - zk * 1000.0) / (rk * 1000.0) - 1.0)
        fray = ((fbm(grid, 2200.0, 2) - 0.5) * 0.55 + (fbm(grid, 900.0, 2) - 0.5) * 0.38 + (fbm(grid, 350.0, 2) - 0.5) * 0.24 +
                (fbm(grid, 140.0, 2) - 0.5) * 0.12)
        land = (shape + fray) > 0.0
        # Sea is only what reaches the map's edge: water shut inside the land is land (lakes come from LAKES).
        sea = ~land
        reach = np.zeros_like(sea)
        reach[0, :], reach[-1, :], reach[:, 0], reach[:, -1] = sea[0, :], sea[-1, :], sea[:, 0], sea[:, -1]
        while True:
            grown = reach.copy()
            grown[1:] |= reach[:-1]
            grown[:-1] |= reach[1:]
            grown[:, 1:] |= reach[:, :-1]
            grown[:, :-1] |= reach[:, 1:]
            grown &= sea
            if np.array_equal(grown, reach):
                break
            reach = grown
        land = ~reach

        # The coast, as signed distance: positive inland.
        padded = np.pad(land, 1, mode='edge')
        inner = padded[:-2, 1:-1] & padded[2:, 1:-1] & padded[1:-1, :-2] & padded[1:-1, 2:]
        zs, xs = np.nonzero(land & ~inner)
        samples = np.stack([(xs + 0.5) * grid.m, (zs + 0.5) * grid.m], axis=1).astype(np.float32)
        _, away = nearest_samples(grid, samples)
        self.coast = np.where(land, away, -away)
        noise_x, noise_z = fbm(grid, 900.0, 3), fbm(grid, 900.0, 3)

        self.rivers = [wander_path(polyline_samples(r['path'], 8.0), noise_x, noise_z, grid, 650.0) for r in RIVERS]
        small_x, small_z = fbm(grid, 380.0, 2), fbm(grid, 380.0, 2)
        self.roads = [wander_path(wander_path(polyline_samples(r, 20.0), noise_z, noise_x, grid, 420.0), small_x, small_z, grid, 240.0) for r in ROADS]
        road_points = np.concatenate(self.roads)
        _, self.road_away = nearest_samples(grid, road_points)

    def macro(self):
        """How high the land stands before layers: a 16 m field, with every region's own ground."""
        g = self.grid
        n = g.n
        one_hot = {name: (self.biome == i).astype(np.float32) for i, name in enumerate(self.names)}
        soft = {name: box_blur(mask, 8) for name, mask in one_hot.items()}

        # Blocks of a region stand off from each other: what makes a plain into shelves.
        shelf, _ = blocks(g, 460.0, 150.0)
        shelf_small, _ = blocks(g, 170.0, 60.0)
        shelf_m = sum(one_hot[name] * BIOMES[name]['shelf_m'] for name in self.names)
        level = self.level + box_blur(shelf_m * ((shelf - 0.5) * 1.3 + (shelf_small - 0.5) * 0.5), 1)
        # Keep edges firm: a short blur is what the layers then cut walls into.
        field = box_blur(level, 2)

        relief_m = sum(soft[name] * BIOMES[name]['relief_m'] for name in self.names)
        field += (fbm(g, 760.0, 4) - 0.5) * 2.0 * relief_m

        # Mesas stand out of the red country, and a few out of the yellow; canyons cut down through it.
        mesa, _ = blocks(g, 560.0, 190.0)
        field += soft['red'] * box_blur(smoothstep(0.62, 0.66, mesa) * (55.0 + (mesa - 0.62) * 420.0), 1)
        butte, _ = blocks(g, 310.0, 110.0)
        field += soft['red'] * box_blur(smoothstep(0.86, 0.88, butte) * 70.0, 1)
        field += soft['yellow'] * box_blur(smoothstep(0.84, 0.86, butte) * 46.0, 1)
        canyon = 1.0 - np.abs(fbm(g, 1700.0, 3) * 2.0 - 1.0)
        field -= soft['red'] * smoothstep(0.93, 0.985, canyon) * 74.0
        ravine = 1.0 - np.abs(fbm(g, 1200.0, 3) * 2.0 - 1.0)
        field -= (soft['autumn'] * 42.0 + soft['forest'] * 22.0) * smoothstep(0.94, 0.99, ravine)

        # The sea. Low coasts come down gently to a beach; where the land stands high it ends in a cliff.
        coast = self.coast
        steep_shore = smoothstep(0.46, 0.6, fbm(g, 1000.0, 2))
        reach = 260.0 * (1.0 - steep_shore) + 34.0 * steep_shore
        field = np.where(coast < reach, -32.0 + (field + 32.0) * smoothstep(-70.0, reach, coast), field)
        low_shore = (1.0 - steep_shore) * (1.0 - smoothstep(60.0, 420.0, coast)) * (coast > -70.0)
        field = field + (np.minimum(field, 1.4 + np.maximum(coast, 0.0) * 0.02) - field) * low_shore
        return field

    def soft_masks(self, radius):
        return {name: box_blur((self.biome == i).astype(np.float32), radius) for i, name in enumerate(self.names)}


def tiers(wanted, wander, slope, cell_m, gap_choices, wall_low, wall_high, tread_rise):
    """Heights mapped through levels: long benches with steep walls between them. Back on the land's own height after. On
    steep land a wall gets a bigger share of its level, or it would be narrower than the grid can show: a wall in one or two
    cells comes out as a staircase along its length, which reads as ribs running down it."""
    gaps = rng.choice(gap_choices, size=256).astype(np.float32)
    levels = np.concatenate([[-800.0], -800.0 + np.cumsum(gaps)]).astype(np.float32)
    wall_share = rng.uniform(wall_low, wall_high, size=256).astype(np.float32)
    k = np.clip(np.searchsorted(levels, wanted, side='right') - 1, 0, len(levels) - 2)
    low, high = levels[k], levels[k + 1]
    t = np.clip((wanted - low) / (high - low), 0.0, 1.0)
    wall_from = 1.0 - np.maximum(wall_share[k], np.minimum(4.5 * cell_m * slope / (high - low), 0.8))
    tread = t * tread_rise / np.maximum(wall_from, 1e-3)
    wall = tread_rise + (t - wall_from) / np.maximum(1.0 - wall_from, 1e-3) * (1.0 - tread_rise)
    return low + (high - low) * np.where(t < wall_from, tread, wall) - wander


def mountains(layout, g, R):
    """The range along the north, carved by erosion over the first ALPINE_ROWS_KM of the map (erosion.py): rock rising
    most under the peaks, rivers wearing it into ridges and valleys, coarse to fine; then rain, rockfall and snow at the
    4 m grid. Returns its heights, how much of each point is mountain, and the details found on the way."""
    started = time.time()
    g16 = layout.grid
    rows16 = int(ALPINE_ROWS_KM * 1000.0 / g16.m)
    snow = layout.names.index('snow')
    on_land = smoothstep(-60.0, 250.0, layout.coast)
    weight16 = (smoothstep(0.05, 0.6, box_blur((layout.biome == snow).astype(np.float32), 16)) * on_land)[:rows16]
    px = np.broadcast_to(g16.x, (rows16, g16.n))
    pz = np.broadcast_to(g16.z[:rows16], (rows16, g16.n))
    # Rock rises most under the peaks and along the crest, less toward the front.
    axis_m = ALPINE_AXIS_KM * 1000.0 + (fbm(g16, 2600.0, 2)[:rows16] - 0.5) * 900.0
    south_m = np.maximum(pz - axis_m, 0.0)
    crest = 900.0 + 1400.0 * np.exp(-(south_m / 2600.0) ** 2)
    top = crest
    for xk, zk, height, reach in PEAKS:
        t = np.exp(-2.3 * (np.hypot(px - xk * 1000.0, pz - zk * 1000.0) / reach) ** 2)
        top = np.maximum(top, height * t + crest * (1.0 - t))
    uplift16 = weight16 * (0.2 + 0.8 * np.clip((top - 600.0) / 2550.0, 0.0, 1.0))
    basin16 = box_blur(((layout.biome == snow) & (layout.level < TIER_M[7] * 0.9)).astype(np.float32), 12)[:rows16]
    uplift16 *= 1.0 - 0.65 * basin16
    # How easily rock wears, at several sizes: harder bands stand as buttresses and ribs, and steeper.
    hardness16 = 1.0 + (fbm(g16, 1800.0, 3)[:rows16] - 0.5) + (fbm(g16, 500.0, 2)[:rows16] - 0.5) * 0.6 + (fbm(g16, 160.0, 2)[:rows16] - 0.5) * 0.4
    rows = int(ALPINE_ROWS_KM * 1000.0 / g.m)
    base16 = np.asarray(Image.fromarray(R[:rows]).resize((g16.n, rows16), Image.BILINEAR), dtype=np.float32).copy()

    def at(a, shape, kind=Image.BILINEAR):
        return np.asarray(Image.fromarray(a.astype(np.float32)).resize((shape[1], shape[0]), kind), dtype=np.float32)

    weight = at(weight16, (rows, g.n))
    if REAL:
        return real_range(R, rows, weight, g, started)
    h = None
    for cell_m, steps in ((32.0, 400), (16.0, 150), (8.0, 60)):
        shape = (int(rows16 * 16.0 / cell_m), int(g16.n * 16.0 / cell_m))
        base, level_weight = at(base16, shape), at(weight16, shape)
        fixed = level_weight < 0.03
        if h is None:
            # Rough to start with, or the first rivers run dead straight.
            h = base + (rng.random(shape, dtype=np.float32) - 0.5) * 30.0 * level_weight
        else:
            h = at(h, shape, Image.BICUBIC)
            h = np.where(fixed, base, h)
        h, _ = erosion.carve_range(h, fixed, at(uplift16, shape), at(hardness16, shape), cell_m, steps, 10.0, k=2.5e-3)
        print(f'  range at {cell_m:.0f} m: {time.time() - started:.0f}s, up to {float(h.max()):.0f} m')
    H = at(h, (rows, g.n), Image.BICUBIC)
    H = R[:rows] + (H - R[:rows]) * weight

    # Rain cuts gullies, rock falls from what's too steep to stand, snow slides from what's too steep to hold it.
    flow, deposited = erosion.rain(H, g.m, (weight * np.clip((H - 700.0) / 400.0, 0.0, 1.0)).astype(np.float32), 6_000_000, SEED)
    scree = np.zeros_like(H)
    erosion.slide(H, scree, g.m, 62.0, 25, layer_only=False, where=weight)
    snow_depth = (np.clip((H - SNOW_LINE_M) / 700.0, 0.0, 1.0) * 6.0 * weight).astype(np.float32)
    erosion.slide(H, snow_depth, g.m, 47.0, 80)
    H += snow_depth * 0.7
    print(f'  rain, rockfall, snow: {time.time() - started:.0f}s')
    details = {
        'snow': np.clip((snow_depth - 0.2) / 1.0, 0.0, 1.0),
        'flow': np.clip(np.log1p(flow) / max(float(np.log1p(np.percentile(flow[flow > 0], 99.5))), 1e-3), 0.0, 1.0),
        'scree': np.clip((deposited + scree) / 6.0, 0.0, 1.0),
        'mountain': weight,
    }
    return H, weight, details


def real_range(R, rows, weight, g, started):
    """The real range from REAL_SOURCE in place of the eroded one: already worn by real water and ice, so only snow is
    worked out on it."""
    folder, name = REAL_SOURCE
    with open(f'{folder}/{name}.json', encoding='utf-8') as f:
        meta = json.load(f)
    if abs(float(meta['spacing_m']) - g.m) > 1e-3:
        raise SystemExit(f'{name} is at {meta["spacing_m"]} m; this grid is {g.m} m')
    raw = np.asarray(Image.open(f'{folder}/{name}_heights.png'), dtype=np.float32)
    source = float(meta['lowest_m']) + raw / 65535.0 * (float(meta['highest_m']) - float(meta['lowest_m']))
    peak_row, peak_col = np.unravel_index(int(np.argmax(source)), source.shape)
    r0 = int(peak_row - REAL_PEAK_AT_KM[1] * 1000.0 / g.m)
    c0 = int(peak_col - REAL_PEAK_AT_KM[0] * 1000.0 / g.m)
    if r0 < 0 or c0 < 0 or r0 + rows > source.shape[0] or c0 + g.n > source.shape[1]:
        raise SystemExit(f'{name} is too small for the mountains at REAL_PEAK_AT_KM')
    H = R[:rows] + (source[r0:r0 + rows, c0:c0 + g.n] - R[:rows]) * weight
    snow_depth = (np.clip((H - SNOW_LINE_M) / 700.0, 0.0, 1.0) * 6.0 * weight).astype(np.float32)
    erosion.slide(H, snow_depth, g.m, 47.0, 80)
    H += snow_depth * 0.7
    print(f'  real range from {name}, snow: {time.time() - started:.0f}s')
    zero = np.zeros_like(H)
    return H, weight, {'snow': np.clip((snow_depth - 0.2) / 1.0, 0.0, 1.0), 'flow': zero, 'scree': zero, 'mountain': weight}


def filter_square(a, radius, fn):
    out = a
    for axis in (0, 1):
        acc = out.copy()
        for shift in range(1, radius + 1):
            acc = fn(acc, fn(np.roll(out, shift, axis), np.roll(out, -shift, axis)))
        out = acc
    return out


def sample_field(grid, field, points):
    ix = np.clip((points[:, 0] / grid.m).astype(np.int32), 0, grid.n - 1)
    iz = np.clip((points[:, 1] / grid.m).astype(np.int32), 0, grid.n - 1)
    return field[iz, ix]


def build_heights(layout):
    """Everything from the layout to the final heights, at SAMPLES."""
    started = time.time()
    g16 = layout.grid
    field16 = layout.macro()
    # Passes: where a road runs, the land ramps. A long blur makes a grade a horse can climb.
    road16 = 1.0 - smoothstep(40.0, 130.0, layout.road_away)
    field16 = field16 + (box_blur(field16, 16) - field16) * road16
    level_edges = np.abs(box_blur(layout.level, 2) - box_blur(layout.level, 14))
    soft = layout.soft_masks(6)
    spawn_away = np.hypot(g16.x - SPAWN[0] * 1000.0, g16.z - SPAWN[1] * 1000.0)
    calm = smoothstep(420.0, 800.0, spawn_away) * (1.0 - road16)

    weights = {}
    for key in ('fine', 'mid', 'coarse'):
        weights[key] = sum(soft[name] * BIOMES[name][key] for name in layout.names)
    patches16 = sum(soft[name] * BIOMES[name]['patches'] for name in layout.names)
    # Between regions at different heights there's always a wall, whatever the land is like.
    edge = smoothstep(5.0, 24.0, level_edges)
    weights['coarse'] = np.maximum(weights['coarse'], edge - weights['fine'] - weights['mid'])

    g = Grid(4096)
    print('macro', f'{time.time() - started:.0f}s')
    field = resize(field16, g.n)
    # Layers run at a slight dip across the land and wander widely, so a lone hill doesn't become a stepped pyramid; blocks
    # nudge where walls run, which gives them corners.
    block_value, block_edge = blocks(g, 70.0)
    small_value, small_edge = blocks(g, 26.0)
    dip = g.x * 0.014 - g.z * 0.008
    wander = (fbm(g, 700.0, 2) - 0.5) * 90.0 + (fbm(g, 200.0, 2) - 0.5) * 22.0 + (box_blur(block_value, 2) - 0.5) * 18.0 + (box_blur(small_value, 1) - 0.5) * 6.0 + dip
    wanted = field + wander
    patch_noise = smoothstep(0.47, 0.6, fbm(g, 1300.0, 2))
    patches = resize(patches16, g.n, Image.BILINEAR)
    calm4 = resize(calm, g.n, Image.BILINEAR)
    gz, gx = np.gradient(box_blur(wanted, 2), g.m)
    wanted_slope = np.hypot(gx, gz)
    del gz, gx
    R = field.copy()
    for key, choices, low, high, rise in (('fine', [22, 28, 34, 42, 52, 66], 0.05, 0.13, 0.2), ('mid', [44, 54, 66, 80, 110], 0.05, 0.11, 0.28), ('coarse', [80, 100, 124, 146, 170], 0.05, 0.1, 0.34)):
        layered = tiers(wanted, wander, wanted_slope, g.m, np.array(choices, dtype=np.float32), low, high, rise)
        # Ridges and slots too thin to be rock are gone: thin things in, then thin things out.
        layered = filter_square(filter_square(layered, 2, np.minimum), 2, np.maximum)
        layered = filter_square(filter_square(layered, 2, np.maximum), 2, np.minimum)
        # Low land keeps most of its height: a bench cut down to the sea would make a beach far inland.
        layered = np.maximum(layered, np.minimum(field, 7.0 + field * 0.35))
        layered = box_blur(layered, 1) * 0.5 + layered * 0.5
        w = resize(weights[key], g.n, Image.BILINEAR)
        take = np.clip(w * (1.0 - patches + patches * patch_noise), 0, 1) * calm4
        R += (layered - field) * take
        del layered, take, w
        print(' ', key, 'layers', f'{time.time() - started:.0f}s')
    del wanted, wanted_slope, wander, dip, patch_noise, patches
    R = box_blur(R, 1) * 0.35 + R * 0.65

    # Dunes: long ridges across the wind, in patches of the yellow desert, on level ground. A long slope up, a short one down.
    angle = math.radians(32.0)
    along = g.x * math.cos(angle) + g.z * math.sin(angle)
    phase = (along + (fbm(g, 900.0, 2) - 0.5) * 900.0 + (fbm(g, 260.0, 2) - 0.5) * 120.0) / 210.0
    phase = phase - np.floor(phase)
    crest = smoothstep(0.0, 0.78, phase) * (1.0 - smoothstep(0.78, 1.0, phase))
    gz, gx = np.gradient(box_blur(R, 3), g.m)
    level_ground = box_blur(1.0 - smoothstep(0.08, 0.3, np.hypot(gx, gz)), 6)
    dunes = resize(soft['yellow'], g.n, Image.BILINEAR) * smoothstep(0.42, 0.62, fbm(g, 1300.0, 2)) * level_ground * calm4
    R += dunes * crest * (7.0 + 18.0 * fbm(g, 500.0, 2))
    del along, phase, crest, gz, gx, level_ground, dunes

    # Landmarks as needles of rock, their feet ragged, their sides steep, standing over everything near them.
    for _, sx, sz, rise_m, foot_m in SPIRES:
        d = np.hypot(g.x - sx * 1000.0, g.z - sz * 1000.0)
        angle = np.arctan2(g.z - sz * 1000.0, g.x - sx * 1000.0)
        reach = foot_m * (1.0 + 0.18 * np.sin(3.0 * angle + 1.3) + 0.1 * np.sin(7.0 * angle + 0.4))
        ground = float(np.median(R[d < foot_m * 2.5]))
        R = np.where(d < reach, np.maximum(R, ground + rise_m * np.clip(1.0 - d / reach, 0.0, 1.0) ** 0.45), R)
        del d, angle, reach

    mountain, mountain_weight, details = mountains(layout, g, R)
    rows = mountain.shape[0]
    R[:rows] = mountain
    del mountain
    not_mountain = np.ones_like(R)
    not_mountain[:rows] -= mountain_weight
    print('mountains', f'{time.time() - started:.0f}s')

    # Rivers only run downhill, and cut through what's in their way: gorges in hard country, valleys in soft.
    all_points, all_bed, all_half = [], [], []
    ground = box_blur(R, 2)
    for river, spec in zip(layout.rivers, RIVERS):
        heights = sample_field(g, ground, river)
        bed = np.minimum.accumulate(heights) - 2.0
        # It keeps falling to its end, at the sea or at the river it joins.
        fall = np.linspace(0.0, 1.0, len(bed)) ** 2
        end = 0.0
        if 'joins' in spec:
            joined = all_points[spec['joins']]
            nearest = np.argmin((joined[:, 0] - river[-1, 0]) ** 2 + (joined[:, 1] - river[-1, 1]) ** 2)
            end = float(all_bed[spec['joins']][nearest])
        bed = bed * (1.0 - fall) + np.minimum(bed[-1], end) * fall
        all_points.append(river)
        all_bed.append(bed)
        all_half.append(np.linspace(spec['width_m'] * 0.2, spec['width_m'] * 0.5, len(bed)))
    points = np.concatenate(all_points)
    beds = np.concatenate(all_bed).astype(np.float32)
    halves = np.concatenate(all_half).astype(np.float32)
    index, away = nearest_samples(Grid(2048), points)
    bed4 = resize(beds[index], g.n, Image.BILINEAR)
    half4 = resize(halves[index], g.n, Image.BILINEAR)
    away4 = resize(away, g.n)
    del ground, index, away
    hard = resize(soft['red'] + soft['autumn'] * 0.8 + soft['snow'] * 0.7, g.n, Image.BILINEAR)
    bank = 0.16 + np.clip(hard, 0, 1) * 1.3
    carved = bed4 + np.maximum(away4 - half4, 0.0) * bank + np.maximum(away4 - half4 - 90.0, 0.0) * 0.6
    R = np.minimum(R, carved)
    del bed4, half4, away4, hard, bank, carved

    # Lake bowls and level ground for towns.
    wobble = fbm(g, 300.0, 2) * 0.35 + 0.82
    for xk, zk, radius in LAKES:
        d = np.hypot(g.x - xk * 1000.0, g.z - zk * 1000.0) / wobble
        inside = d < radius * 0.6
        level = float(np.median(R[inside])) - 1.0
        bowl = np.where(d < radius, level - 9.0 * (1.0 - (d / radius) ** 2), level + (d - radius) * 0.1)
        R += (np.minimum(R, bowl) - R) * smoothstep(radius * 2.8, radius * 1.8, d)
    for xk, zk, radius in TOWNS:
        d = np.hypot(g.x - xk * 1000.0, g.z - zk * 1000.0)
        inside = d < radius
        level = float(np.median(R[inside]))
        R += (level - R) * smoothstep(radius * 1.7, radius * 0.85, d)
    print('water and towns', f'{time.time() - started:.0f}s')

    # Layers on what's steep now: ledges, blocks standing out, cracks between them.
    gz, gx = np.gradient(box_blur(R, 1), g.m)
    slope = np.hypot(gx, gz)
    steep = smoothstep(0.7, 1.5, slope)
    near_steep = np.maximum(steep, box_blur(steep, 2))
    holds_ledges = near_steep * (1.0 - smoothstep(1.3, 2.2, slope)) * not_mountain
    del gx, gz, steep, slope
    bands = (fbm(g, 170.0, 2) - 0.5) * 12.0
    step_m = 13.0
    y = R + bands
    stepped = np.floor(y / step_m) * step_m + step_m * smoothstep(0.3, 0.7, (y / step_m) - np.floor(y / step_m)) - bands
    R += (stepped - R) * holds_ledges * 0.75
    del y, stepped, bands, holds_ledges
    R += (block_value - 0.5) * 5.0 * near_steep * not_mountain
    R -= (1.0 - smoothstep(0.0, 7.0, block_edge)) * 3.5 * near_steep * not_mountain
    R -= (1.0 - smoothstep(0.0, 3.0, small_edge)) * 1.2 * near_steep * not_mountain
    del not_mountain
    del block_value, block_edge, small_value, small_edge
    R += (fbm(g, 18.0, 2) - 0.5) * 2.2 * (near_steep + 0.1)
    R += (fbm(g, 80.0, 3) - 0.5) * 3.0 * (1.0 - near_steep) * smoothstep(4.0, 16.0, R)
    print('ledges', f'{time.time() - started:.0f}s')

    # To the final grid, with fine grain on rock. Land stays above the sea, and the sea stays.
    sea = resize((layout.coast > 0.0).astype(np.float32), g.n, Image.BILINEAR)
    R = np.where(sea > 0.5, np.maximum(R, 0.6), R)
    fine = resize(R, SAMPLES)
    del R
    grain = np.asarray(Image.fromarray(rng.random((SAMPLES // 2, SAMPLES // 2), dtype=np.float32)).resize((SAMPLES, SAMPLES), Image.BICUBIC), dtype=np.float32)
    steep_fine = resize(near_steep, SAMPLES, Image.BILINEAR)
    fine = fine + (grain - 0.5) * 0.9 * steep_fine
    del grain, steep_fine, near_steep
    fine = np.clip(fine, LOWEST_M, HIGHEST_M)
    print('final grid', f'{time.time() - started:.0f}s', 'heights', float(fine.min()), float(fine.max()))
    return fine, details


def write_terrain(fine):
    os.makedirs(OUT, exist_ok=True)
    pixels = ((fine - LOWEST_M) / (HIGHEST_M - LOWEST_M) * 65535.0 + 0.5).astype(np.uint16)
    Image.fromarray(pixels).save(f'{OUT}/{NAME}_tmp.png', format='PNG', compress_level=2)
    os.replace(f'{OUT}/{NAME}_tmp.png', f'{OUT}/{NAME}.heightpng')
    with open(f'{OUT}/{NAME}.terrain', 'w', encoding='utf-8') as f:
        json.dump({'height': f'{NAME}.heightpng', 'meters_per_pixel': SPACING_M, 'lowest_m': LOWEST_M, 'highest_m': HIGHEST_M}, f, indent=1)


def write_masks(layout):
    soft = layout.soft_masks(14)
    folder = f'{OUT}/layers/masks'
    os.makedirs(folder, exist_ok=True)
    soft['autumn'] = np.maximum(soft['autumn'], soft['snow'])
    channels = [Image.fromarray((np.clip(soft[name], 0, 1) * 255.0 + 0.5).astype(np.uint8)) for name in ('red', 'yellow', 'forest', 'autumn')]
    Image.merge('RGBA', channels).save(f'{folder}/{NAME}_masks.png')
    with open(f'{folder}/{NAME}_masks.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'metallic_roughness': f'{NAME}_masks.png'}}, f, indent=1)


def write_details(fine, details):
    """Snow, flow, scree and how much is mountain, over the whole map at 4 m, for the ground's rules."""
    n = SAMPLES // 2
    rows = details['snow'].shape[0]
    channels = []
    for name in ('snow', 'flow', 'scree', 'mountain'):
        full = np.zeros((n, n), dtype=np.float32)
        full[:rows] = details[name]
        channels.append(full)
    folder = f'{OUT}/layers/masks'
    Image.merge('RGBA', [Image.fromarray((np.clip(c, 0, 1) * 255.0 + 0.5).astype(np.uint8)) for c in channels]).save(f'{folder}/{NAME}_details.png')
    with open(f'{folder}/{NAME}_details.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'metallic_roughness': f'{NAME}_details.png'}}, f, indent=1)


def write_lighting(fine):
    """Shadows from far off and the sky each place sees, baked at 4 m for the environment's sun (lightbake.py)."""
    started = time.time()
    lightbake.write_light_map(f'{OUT}/layers/masks', NAME, resize(fine, SAMPLES // 2, Image.BILINEAR), SPACING_M * 2.0,
                              SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG)
    print('light map', f'{time.time() - started:.0f}s')


def write_ground():
    """The ground's rules. Layers are in projects/dreadworld/content/terrain/layers (tools/worldgen/ground_layers.py)."""

    SCREE_TINT = [0.5, 0.78, 1.3]
    ROCK_TINT = [1.5, 1.72, 1.72]
    SANDSTONE_TINT = [2.77, 2.26, 1.17]
    ALPINE_ROCK_TINT = [0.66, 0.74, 0.86]

    def layer(name, tile, **rules):
        out = {'material': f'layers/{name}/{name}.material', 'tile_m': tile}
        out.update(rules)
        return out

    red = {'mask': 'r', 'mask_range': [0.3, 0.7]}
    yellow = {'mask': 'g', 'mask_range': [0.3, 0.7]}
    forest = {'mask': 'b', 'mask_range': [0.3, 0.7]}
    autumn = {'mask': 'a', 'mask_range': [0.3, 0.7]}
    no_scree = {'detail': 'b', 'detail_range': [0.5, 0.15]}
    stack = [
        layer('gravelly_sand', 3.0, variation=0.3, tint=SCREE_TINT),
        layer('leafy_grass', 2.0, height_m=[1.5, 6, 520, 720], slope_deg=[0, 0, 30, 38], noise=0.9, variation=0.26, tint=[0.46, 0.68, 0.3], **no_scree),
        layer('withered_grass', 2.0, height_m=[420, 620, SNOW_LINE_M - 100, SNOW_LINE_M + 250], slope_deg=[0, 0, 30, 38], noise=1.6, noise_large=0.75, variation=0.2, tint=[0.74, 0.72, 0.55], **no_scree),
        layer('forest_leaves_02', 3.0, slope_deg=[0, 0, 32, 40], noise=1.4, noise_large=0.5, variation=0.16, tint=[0.3, 0.5, 0.62], **forest),
        layer('dry_decay_leaves', 2.0, height_m=[-60, -60, 800, 1000], slope_deg=[0, 0, 32, 40], noise=1.4, noise_large=0.5, variation=0.2, tint=[1.5, 1.5, 1.2], **autumn),
        layer('sand_03', 2.0, slope_deg=[0, 0, 32, 40], noise=1.2, noise_large=0.5, variation=0.1, tint=[3.0, 2.6, 1.8], **yellow),
        # Cracked pans lie in the low basins, breaking up as the land rises.
        layer('dry_ground_01', 4.0, height_m=[-60, -60, 10, 60], slope_deg=[0, 0, 6, 14], noise=2.0, noise_large=0.7, variation=0.1, tint=[1.85, 1.75, 1.45], **yellow),
        layer('desert_outback', 2.6, slope_deg=[0, 0, 34, 42], noise=1.4, noise_large=0.5, variation=0.12, tint=[0.8, 0.6, 0.48], **red),
        layer('damp_beach_sand', 2.0, height_m=[-60, -60, 2.5, 9], slope_deg=[0, 0, 14, 24], noise=0.5, variation=0.08, tint=[2.2, 2.1, 1.9]),
        layer('gravelly_sand', 3.0, slope_deg=[28, 36, 90, 90], noise=0.8, variation=0.14, tint=[1.4, 1.7, 1.7], **yellow),
        layer('desert_outback', 3.4, slope_deg=[24, 34, 46, 56], noise=0.8, variation=0.14, tint=[0.72, 0.52, 0.42], **red),
        # Above the scree, so high ground stays white; bare rock still shows where it's too steep to hold snow.
        # Rock with strata below the mountains, colored by the land it's in; dark, broken rock in them.
        layer('cliff_side_grey', 17.0, slope_deg=[40, 50, 90, 90], projection='sides', noise=0.6, variation=0.14, tint=ROCK_TINT, detail='a', detail_range=[0.5, 0.15]),
        layer('rock_face_03_grey', 13.0, slope_deg=[34, 46, 90, 90], projection='sides', noise=0.6, variation=0.16, tint=ALPINE_ROCK_TINT, detail='a', detail_range=[0.15, 0.5]),
        # Snow where the snow settled, on top of the rock.
        layer('snow_02', 2.0, slope_deg=[0, 0, 62, 75], noise=0.6, variation=0.04, tint=[1.9, 1.9, 1.95], detail='r', detail_range=[0.1, 0.55]),
        layer('cliff_side_grey', 15.0, slope_deg=[36, 46, 90, 90], projection='sides', noise=0.6, variation=0.14, tint=SANDSTONE_TINT, **yellow),
        layer('cliff_side', 17.0, slope_deg=[30, 44, 90, 90], projection='sides', noise=0.6, variation=0.14, tint=[0.92, 0.78, 0.68], **red),
    ]
    with open(f'{OUT}/{NAME}.terrainlayers', 'w', encoding='utf-8') as f:
        json.dump({'masks': f'layers/masks/{NAME}_masks.material', 'details': f'layers/masks/{NAME}_details.material',
                   'lighting': f'layers/masks/{NAME}_light.material', 'blend': 0.28, 'layers': stack,
                   'grass': {'density_per_m2': 300, 'height_m': [0.2, 0.58], 'width_m': 0.024, 'where_height_m': [1.5, 6, SNOW_LINE_M - 200, SNOW_LINE_M], 'where_slope_deg': [0, 0, 28, 38],
                             'not_where': 'rgb', 'patches': 0.25, 'base': [0.04, 0.07, 0.018], 'tip': [0.15, 0.21, 0.05],
                             'other': {'mask': 'a', 'base': [0.1, 0.06, 0.02], 'tip': [0.36, 0.24, 0.08]}}}, f, indent=1)


def write_world_and_views(fine):
    """The world, starting at SPAWN, and places to look from, found from the heights."""
    step = 4
    H = fine[::step, ::step]
    m = SPACING_M * step
    gz, gx = np.gradient(H, m)
    slope = np.hypot(gx, gz)

    def height_at(x, z):
        return float(fine[int(np.clip(z / SPACING_M, 0, SAMPLES - 1)), int(np.clip(x / SPACING_M, 0, SAMPLES - 1))])

    sx, sz = SPAWN[0] * 1000.0, SPAWN[1] * 1000.0
    spawn_y = height_at(sx, sz) + 1.8
    yaw = math.degrees(math.atan2(-(SPAWN_LOOK[0] * 1000.0 - sx), -(SPAWN_LOOK[1] * 1000.0 - sz)))
    environment = f'{NAME}.environment'
    with open(f'{OUT}/{environment}', 'w', encoding='utf-8') as f:
        json.dump({'sun': {'elevation_deg': SUN_ELEVATION_DEG, 'azimuth_deg': SUN_AZIMUTH_DEG, 'color': [1, 0.82, 0.6], 'intensity': 4.6},
                   'sky': {'color': [0.30, 0.40, 0.62], 'intensity': 1},
                   'ground': {'color': [0.14, 0.11, 0.08], 'intensity': 1},
                   'shadow_color': [0.07, 0.07, 0.085],
                   'exposure_ev': 0,
                   'shadows': {'resolution': 2048, 'distance_m': 140, 'split_lambda': 0.75, 'blend_band': 0.12, 'caster_reach_m': 200, 'penumbra_per_m': 0.06, 'max_penumbra_texels': 6},
                   'fog': {'density_per_m': 0.000002, 'height_density_per_m': 0.0002, 'height_falloff_per_m': 0.006, 'base_height_m': 0, 'extinction_tint': [0.75, 1.0, 1.35],
                           'sun_forward': {'color': [1, 0.8, 0.55], 'intensity': 0.18}, 'sun_back': {'color': [0.6, 0.55, 0.55], 'intensity': 0.05}, 'sun_height_attenuation': 1,
                           'ambient': {'color': [0.30, 0.34, 0.42], 'intensity': 1}, 'ambient_height_attenuation': 1,
                           'start_m': 400, 'sun_start_m': 400, 'end_m': 20000, 'volume_distance_m': 0,
                           'low_camera': {'keep': 0.3, 'below_m': 60, 'above_m': 350}},
                   # A steady breeze off the sea with gusts rolling across the grass.
                   'wind': {'toward_deg': 20, 'speed_mps': 4, 'gust_mps': 5, 'gust_size_m': 55, 'turbulence': 0.35}}, f, indent=1)
    with open(f'{OUT}/{NAME}.world', 'w', encoding='utf-8') as f:
        json.dump({'environment': environment, 'terrain': f'{NAME}.terrain', 'terrain_layers': f'{NAME}.terrainlayers',
                   'start': {'position': [round(sx, 1), round(spawn_y, 2), round(sz, 1)], 'yaw_deg': round(yaw, 1), 'pitch_deg': 2}, 'cells': []}, f, indent=1)
    views = [f'start {sx:.1f} {spawn_y:.1f} {sz:.1f} {yaw:.1f} 2']
    for i, turn in enumerate((90, 180, 270)):
        views.append(f'start_{i + 1} {sx:.1f} {spawn_y:.1f} {sz:.1f} {yaw + turn:.1f} 1')

    grid = Grid(H.shape[0])

    def looks_at(name, target_km, spread_m, least, most, want_steep=True, eye=2.2):
        views_rng = np.random.default_rng(sum(name.encode()))
        tx0, tz0 = target_km[0] * 1000.0, target_km[1] * 1000.0
        for _ in range(9000):
            tx = tx0 + views_rng.uniform(-spread_m, spread_m)
            tz = tz0 + views_rng.uniform(-spread_m, spread_m)
            ti, tj = int(np.clip(tz / m, 1, H.shape[0] - 2)), int(np.clip(tx / m, 1, H.shape[1] - 2))
            if want_steep and slope[ti, tj] < 1.0:
                continue
            angle = views_rng.uniform(0, math.tau)
            d = views_rng.uniform(least, most)
            x, z = tx + d * math.cos(angle), tz + d * math.sin(angle)
            if not (60 < x < WORLD_M - 60 and 60 < z < WORLD_M - 60):
                continue
            i, j = int(z / m), int(x / m)
            ground = float(H[i, j])
            if ground < 2.0 or slope[i, j] > 0.3:
                continue
            y = ground + eye
            ty = float(H[ti, tj]) + 8.0
            t = np.linspace(0.05, 0.9, 28)
            between = H[((z + (tz - z) * t) / m).astype(np.int64).clip(0, H.shape[0] - 1), ((x + (tx - x) * t) / m).astype(np.int64).clip(0, H.shape[1] - 1)]
            if np.any(between > y + (ty - y) * t):
                continue
            view_yaw = math.degrees(math.atan2(-(tx - x), -(tz - z)))
            pitch = float(np.clip(math.degrees(math.atan2(ty - y, d)), -6, 26))
            views.append(f'{name} {x:.1f} {y:.1f} {z:.1f} {view_yaw:.1f} {pitch:.1f}')
            return
        print('no view for', name)

    looks_at('plateau_wall', (8.8, 7.4), 900, 120, 500)
    looks_at('plateau_far', (8.6, 6.2), 800, 1500, 3000)
    looks_at('red_near', (13.0, 9.0), 1200, 40, 120)
    looks_at('red_mid', (13.0, 9.0), 1200, 200, 500)
    looks_at('red_far', (13.0, 9.0), 1200, 700, 1500)
    looks_at('yellow_mesa', (14.2, 7.4), 700, 150, 500)
    looks_at('yellow_dunes', (14.4, 6.6), 500, 60, 300, want_steep=False)
    looks_at('forest_wall', (3.4, 9.8), 1200, 80, 400, want_steep=False)
    looks_at('autumn_wall', (8.4, 6.4), 1200, 80, 400, want_steep=False)
    looks_at('snow_face', (9.0, 2.6), 900, 400, 1400)
    looks_at('escarpment', (14.0, 7.6), 800, 300, 900)
    looks_at('coast_west', (1.8, 10.8), 1500, 150, 600)
    for name, xk, zk, txk, tzk in (('first_rise', 6.8, 11.2, 5.9, 8.4), ('castle_gate', 6.2, 9.4, 5.9, 8.4), ('basin', 5.6, 5.0, 4.4, 6.1),
                                   ('basin_cliff', 4.6, 7.6, 8.6, 6.2), ('peak_lake', 11.4, 4.4, 9.0, 2.6), ('lift', 9.0, 8.4, 9.0, 7.2),
                                   ('range_west', 5.0, 5.0, 5.2, 3.4), ('range_east', 12.6, 5.0, 13.0, 2.6), ('spire_near', 9.8, 6.6, 8.6, 6.2)):
        x, z, tx, tz = xk * 1000.0, zk * 1000.0, txk * 1000.0, tzk * 1000.0
        y = height_at(x, z) + 2.0
        d = math.hypot(tx - x, tz - z)
        pitch = math.degrees(math.atan2(height_at(tx, tz) * 0.55 - y, d))
        views.append(f'{name} {x:.1f} {y:.1f} {z:.1f} {math.degrees(math.atan2(-(tx - x), -(tz - z))):.1f} {pitch:.1f}')
    looks_at('coast_cliff', (7.0, 14.0), 2000, 100, 500)
    for name, x, z, turn, up, pitch in (('air_center', 8000, 11600, 0, 500, -16), ('air_east', 9800, 9400, -80, 420, -16), ('air_west', 6600, 9200, 95, 420, -16), ('air_north', 8000, 7800, 0, 620, -12), ('air_range', 8400, 6800, 0, 900, -4), ('air_high', 8200, 16000, 0, 3800, -30)):
        views.append(f'{name} {x} {height_at(x, min(z, WORLD_M - 10)) + up:.1f} {z} {turn} {pitch}')
    with open(f'{OUT}/{NAME}.views', 'w', encoding='utf-8') as f:
        f.write('\n'.join(views) + '\n')
    print('\n'.join(views))


def top_down(fine):
    step = 8
    H = fine[::step, ::step]
    gz, gx = np.gradient(H, SPACING_M * step)
    light = np.array([-0.55, 0.7, -0.45])
    light = light / np.linalg.norm(light)
    n = np.stack([-gx, np.ones_like(gx), -gz], axis=2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    shade = np.clip((n @ light) * 1.05, 0.0, 1.1)
    os.makedirs(EYES, exist_ok=True)
    Image.fromarray((shade * 230).astype(np.uint8)).save(f'{EYES}/top_down.png')
    for a in (30, 40, 50, 60):
        print(f'  over {a} deg: {100 * float((np.hypot(gx, gz) > math.tan(math.radians(a))).mean()):.1f}%')


def sketch(layout):
    """The layout from above: what each region is, how the land stands, and what goes where."""
    os.makedirs(EYES, exist_ok=True)
    g = layout.grid
    field = layout.macro()
    gz, gx = np.gradient(field, g.m)
    light = np.array([-0.55, 0.7, -0.45])
    light = light / np.linalg.norm(light)
    n = np.stack([-gx, np.ones_like(gx), -gz], axis=2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    shade = np.clip((n @ light) * 1.05, 0.25, 1.15)
    colors = np.array([BIOMES[name]['color'] for name in layout.names], dtype=np.float32)
    rgb = colors[layout.biome] * (shade[..., None] * (0.72 + 0.28 * np.clip(field / 900.0, 0, 1))[..., None])
    sea = field < 0.0
    rgb[sea] = (np.array([28.0, 58.0, 96.0])[None, :] * (0.7 + 0.3 * np.clip(1.0 + field[sea] / 32.0, 0, 1))[:, None])
    image = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).resize((1400, 1400), Image.BILINEAR)
    draw = ImageDraw.Draw(image)
    scale = 1400.0 / WORLD_M
    for k in range(1, 17):
        at = k * 1000.0 * scale
        draw.line([(at, 0), (at, 1400)], fill=(255, 255, 255), width=1 if k % 4 else 2)
        draw.line([(0, at), (1400, at)], fill=(255, 255, 255), width=1 if k % 4 else 2)
        draw.text((at + 3, 2), f'{k}', fill=(255, 255, 255))
        draw.text((2, at + 2), f'{k}', fill=(255, 255, 255))
    for river in layout.rivers:
        draw.line([(p[0] * scale, p[1] * scale) for p in river], fill=(70, 150, 255), width=4)
    for road in layout.roads:
        points = [(p[0] * scale, p[1] * scale) for p in road]
        for a in range(0, len(points) - 1, 6):
            draw.line(points[a:a + 4], fill=(40, 30, 20), width=3)
    for xk, zk, radius in LAKES:
        r = radius * scale
        c = (xk * 1000.0 * scale, zk * 1000.0 * scale)
        draw.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=(70, 150, 255), outline=(255, 255, 255))
    for xk, zk, radius in TOWNS:
        c = (xk * 1000.0 * scale, zk * 1000.0 * scale)
        draw.rectangle([c[0] - 6, c[1] - 6, c[0] + 6, c[1] + 6], fill=(255, 255, 255), outline=(0, 0, 0))
    for xk, zk in CAVES:
        c = (xk * 1000.0 * scale, zk * 1000.0 * scale)
        draw.ellipse([c[0] - 5, c[1] - 5, c[0] + 5, c[1] + 5], fill=(0, 0, 0), outline=(255, 255, 255))
    for xk, zk in LIFTS:
        c = (xk * 1000.0 * scale, zk * 1000.0 * scale)
        draw.rectangle([c[0] - 4, c[1] - 12, c[0] + 4, c[1] + 12], fill=(255, 255, 255), outline=(0, 0, 0))
        draw.text((c[0] + 8, c[1] - 6), 'lift', fill=(255, 255, 255))
    for name, xk, zk in [(s[0], s[1], s[2]) for s in SPIRES] + [('the highest peak', PEAKS[0][0], PEAKS[0][1])]:
        c = (xk * 1000.0 * scale, zk * 1000.0 * scale)
        draw.polygon([(c[0], c[1] - 14), (c[0] - 9, c[1] + 8), (c[0] + 9, c[1] + 8)], fill=(255, 220, 60), outline=(0, 0, 0))
        draw.text((c[0] + 12, c[1] - 6), name, fill=(255, 255, 255))
    c = (SPAWN[0] * 1000.0 * scale, SPAWN[1] * 1000.0 * scale)
    draw.ellipse([c[0] - 9, c[1] - 9, c[0] + 9, c[1] + 9], fill=(255, 50, 50), outline=(255, 255, 255), width=2)
    draw.text((c[0] + 12, c[1] - 6), 'start', fill=(255, 255, 255))
    # A legend.
    draw.rectangle([1120, 1068, 1395, 1395], fill=(20, 20, 20))
    y = 1076
    for name, label in (('snow', 'the range, snow at the end'), ('autumn', 'the high plateau'), ('forest', 'forest'), ('plains', 'green lowland, castle'), ('red', 'the red region'), ('yellow', 'the raised mesa'), ('marsh', 'lake basin, marsh peninsula')):
        draw.rectangle([1128, y, 1146, y + 14], fill=BIOMES[name]['color'])
        draw.text((1154, y + 1), label, fill=(255, 255, 255))
        y += 22
    draw.line([(1128, y + 7), (1146, y + 7)], fill=(70, 150, 255), width=4)
    draw.text((1154, y + 1), 'rivers, lake bowls', fill=(255, 255, 255))
    y += 22
    draw.line([(1128, y + 7), (1146, y + 7)], fill=(40, 30, 20), width=3)
    draw.text((1154, y + 1), 'roads through passes', fill=(255, 255, 255))
    y += 22
    draw.rectangle([1130, y + 1, 1142, y + 13], fill=(255, 255, 255), outline=(0, 0, 0))
    draw.text((1154, y + 1), 'room for a town', fill=(255, 255, 255))
    y += 22
    draw.ellipse([1131, y + 2, 1141, y + 12], fill=(0, 0, 0), outline=(255, 255, 255))
    draw.text((1154, y + 1), 'cave mouth, reserved', fill=(255, 255, 255))
    y += 22
    draw.text((1128, y + 1), 'grid: 1 km. 16.4 x 16.4 km', fill=(255, 255, 255))
    image.save(f'{EYES}/sketch.png')
    land = field >= 0.0
    print('sketch saved', 'land share', round(float(land.mean()), 3))
    for i, name in enumerate(layout.names):
        print(f'  {name:7s} {100 * float((layout.biome[land] == i).mean()):5.1f}% of land')


def main():
    started = time.time()
    stage = sys.argv[1] if len(sys.argv) > 1 else 'sketch'
    if 'real' in sys.argv[2:]:
        global NAME, EYES, REAL
        NAME, EYES, REAL = 'realm_real', 'D:/DE/build/eyes/realm_real', True
    layout = Layout()
    if stage == 'sketch':
        sketch(layout)
    elif stage == 'build':
        fine, details = build_heights(layout)
        write_terrain(fine)
        write_masks(layout)
        write_details(fine, details)
        write_lighting(fine)
        write_ground()
        write_world_and_views(fine)
        top_down(fine)
    elif stage == 'ground':
        write_masks(layout)
        write_ground()
    print(f'done in {time.time() - started:.0f}s')


if __name__ == '__main__':
    main()
