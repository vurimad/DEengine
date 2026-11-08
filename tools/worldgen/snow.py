"""Snow depth over a map, baked from its land: how many meters lie at each place, which the terrain's snow layer
thresholds into a crisp edge (its "detail" rule on the details' red channel). The recipe is study 12's, section 2
(D:/ERStudy/study/12-real-mountains.md), after what snow does on real mountains and what terrain tools agree on:

1. Snowfall by altitude, ramping in over 300 m around a season's snow line, heavier higher up.
2. Wind: lee slopes and gullies gather it, exposed crests lose it, by how sheltered each place is from the prevailing
   wind (Winstral's Sx: the steepest rise toward where the wind comes from, within a few hundred meters).
3. Sun: the faces that see the most of the season's sun, the land's own shadows counted, keep the least.
4. Gravity, at the map's own spacing: it slides off whatever is steeper than it can lie on and runs down the gullies, so
   it lies in streaks, as it does on a real face.
5. What a slope can hold (SnowSlide's figures, CHM's docs), and up high, where it's cold enough, a coat of ice and rime
   that steep rock keeps whatever slides. Then the fine relief: ribs a few meters across stand out of it, hollows fill.
   That's what draws a face as thousands of thin dark lines from kilometers off.
"""
import math

import numpy as np
from PIL import Image

import erosion
import lightbake

# The most snow a slope can hold, in meters, by its angle: SnowSlide's 3.08 m at 30 degrees, 1.11 at 45, 0.45 at 60, 0.15
# at 75 (https://chm.readthedocs.io/en/develop/api/classsnow__slide.html), and none on a wall.
HOLD_DEG = [0.0, 30.0, 45.0, 60.0, 75.0, 90.0]
HOLD_M = [8.0, 3.08, 1.11, 0.45, 0.15, 0.0]
# The ice and rime steep rock keeps where it's cold, in meters, by slope: none on gentle ground, where snow lies anyway.
COAT_DEG = [0.0, 40.0, 50.0, 65.0, 78.0, 88.0, 90.0]
COAT_M = [0.0, 0.0, 0.9, 0.8, 0.6, 0.2, 0.0]


def _resize(field, shape):
    """Bilinear, to (rows, cols)."""
    return np.asarray(Image.fromarray(np.ascontiguousarray(field, dtype=np.float32)).resize((shape[1], shape[0]), Image.BILINEAR),
                      dtype=np.float32)


def _box(field, radius):
    """The mean over a square 2 radius + 1 samples across, the edge repeated."""
    size = 2 * radius + 1
    out = field.astype(np.float64)
    for axis in (0, 1):
        pad = [(radius + 1, radius) if i == axis else (0, 0) for i in range(2)]
        c = np.cumsum(np.pad(out, pad, mode='edge'), axis=axis)
        out = (c[size:, :] - c[:-size, :]) if axis == 0 else (c[:, size:] - c[:, :-size])
        out /= size
    return out.astype(np.float32)


def _shifted(field, dx, dz):
    """The field at (x + dx, z + dz), cells, its edge repeated past the side."""
    rows, cols = field.shape
    pad = max(abs(dx), abs(dz))
    if pad == 0:
        return field
    padded = np.pad(field, pad, mode='edge')
    return padded[pad + dz:pad + dz + rows, pad + dx:pad + dx + cols]


def shelter_deg(heights, spacing_m, wind_from_deg, reach_m=300.0, spread_deg=30.0, directions=5):
    """Winstral's Sx in degrees: at each place, the steepest rise toward where the wind comes from within reach_m,
    averaged over a fan of directions spread_deg wide. Above 0 sheltered, below 0 exposed. Azimuths clockwise from
    north (-Z) toward east (+X), as the environment's."""
    total = np.zeros_like(heights, dtype=np.float32)
    for k in range(directions):
        angle = math.radians(wind_from_deg + spread_deg * (k / (directions - 1) - 0.5))
        ux, uz = math.sin(angle), -math.cos(angle)
        steepest = np.full(heights.shape, -10.0, np.float32)
        for d in np.arange(spacing_m, reach_m + 1e-3, spacing_m):
            dx, dz = int(round(ux * d / spacing_m)), int(round(uz * d / spacing_m))
            np.maximum(steepest, (_shifted(heights, dx, dz) - heights) / np.float32(d), out=steepest)
        total += np.degrees(np.arctan(steepest)).astype(np.float32)
    return total / directions


def sunshine(heights, spacing_m, suns):
    """How much of the season's sun each place gets, against open flat ground (1): the sum over the suns, each an
    (azimuth, elevation) in degrees, of how square the ground faces it, where the land doesn't hide it."""
    gz, gx = np.gradient(heights.astype(np.float32), spacing_m)
    length = np.sqrt(gx * gx + gz * gz + 1.0)
    total = np.zeros_like(heights, dtype=np.float32)
    flat = 0.0
    for azimuth, elevation in suns:
        a, e = math.radians(azimuth), math.radians(elevation)
        lx, ly, lz = math.sin(a) * math.cos(e), math.sin(e), -math.cos(a) * math.cos(e)
        facing = np.maximum((-gx * lx + ly - gz * lz) / length, 0.0)
        hidden = lightbake.shadow_depth(heights, spacing_m, azimuth, elevation)
        total += facing * np.exp(-hidden / 4.0)  # a few meters of shadow is a soft edge, not a line
        flat += ly
    return total / flat


def coverage(depth, shows_m):
    """How much of each sample's square snow covers, 0 to 1, from where it's at least shows_m deep at four points inside
    it, a quarter of a sample out from its middle each way (bilinear between samples). Whole squares are 0 or 1, and
    only edges fall between, so far off, where the renderer averages it, the share is the rock's own: a rib thinner
    than a pixel darkens it a little rather than vanishing, and thin snow doesn't turn into a grey haze."""
    p = np.pad(depth, 1, mode='edge')
    rows, cols = depth.shape
    middle = p[1:-1, 1:-1]
    total = np.zeros(depth.shape, np.float32)
    for dz in (-1, 1):
        for dx in (-1, 1):
            beside_x = p[1:-1, 1 + dx:cols + 1 + dx]
            beside_z = p[1 + dz:rows + 1 + dz, 1:-1]
            corner = p[1 + dz:rows + 1 + dz, 1 + dx:cols + 1 + dx]
            at = 0.5625 * middle + 0.1875 * (beside_x + beside_z) + 0.0625 * corner
            total += at >= shows_m
    return total / 4.0


def _value_noise(x, z, cell_m, seed):
    """Smooth value noise, 0 to 1, at world points (meters), a value every cell_m: hashed, so any piece of the map gives
    the same as the whole would."""
    gx, gz = x / cell_m, z / cell_m
    ix, iz = np.floor(gx).astype(np.int64), np.floor(gz).astype(np.int64)
    fx, fz = (gx - ix).astype(np.float32), (gz - iz).astype(np.float32)
    fx, fz = fx * fx * (3.0 - 2.0 * fx), fz * fz * (3.0 - 2.0 * fz)

    def corner(cx, cz):
        h = (cx * 374761393 + cz * 668265263 + seed * 2147483647) & 0xFFFFFFFF
        h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
        return ((h ^ (h >> 16)) & 0xFFFF).astype(np.float32) / 65535.0

    top = corner(ix, iz) * (1.0 - fx) + corner(ix + 1, iz) * fx
    bottom = corner(ix, iz + 1) * (1.0 - fx) + corner(ix + 1, iz + 1) * fx
    return top * (1.0 - fz) + bottom * fz


# Rock the scan is too coarse for, on steep ground (study 12's fine structure): strata, ledges a few meters apart that
# hold snow, the steps between them bare, the beds tilted and wandering; and joints, an irregular grain finer still.
LEDGE_EVERY_M = 7.0
LEDGE_DIP = 0.3          # how far the beds rise per meter across the land, along LEDGE_STRIKE_DEG
LEDGE_STRIKE_DEG = 35.0
LEDGE_SNOW_M = 0.0       # the snow a ledge gains, and a step loses (off: drew contour lines, not rock)
JOINT_SNOW_M = 0.0
STEEP_FROM_DEG, STEEP_TO_DEG = 38.0, 55.0


def coverage_detailed(depth, shows_m, heights, spacing_m, slope_deg, fine, fine_col0, fine_row0, fine_share, seed,
                      strip=256):
    """As coverage(), but its four points are the 1 m samples of `fine` inside each square (the map's vertex (i, j)
    lies at (fine_col0 + 2 i, fine_row0 + 2 j) in them, between two each way, as fine.py has it), and each one's depth
    moves before it's tested: by the scan's own relief there at 3 and 5 m, times fine_share (a vertex map: how much of
    the scan the map's heights took), and on steep ground by ledges and joints the scan can't hold."""
    rows, cols = depth.shape
    assert abs(spacing_m - 2.0) < 1e-6, 'the four points are the 1 m samples: a map at 2 m'
    c0, r0 = int(fine_col0 + 0.5), int(fine_row0 + 0.5)
    pad = np.pad(depth, 1, mode='edge')
    pad_h = np.pad(heights.astype(np.float32), 1, mode='edge')
    steep = np.clip((slope_deg - STEEP_FROM_DEG) / (STEEP_TO_DEG - STEEP_FROM_DEG), 0.0, 1.0).astype(np.float32)
    strike = math.radians(LEDGE_STRIKE_DEG)
    out = np.empty((rows, cols), np.float32)
    for v0 in range(0, rows, strip):
        v1 = min(v0 + strip, rows)
        n = v1 - v0
        # The 1 m samples these rows' squares hold, and two more each way for the relief.
        fr0, fr1 = r0 + 2 * v0 - 1 - 2, r0 + 2 * (v1 - 1) + 1 + 2
        fc0, fc1 = c0 - 1 - 2, c0 + 2 * (cols - 1) + 1 + 2
        block = np.full((fr1 - fr0, fc1 - fc0), np.nan, np.float32)
        sr0, sr1, sc0, sc1 = max(fr0, 0), min(fr1, fine.shape[0]), max(fc0, 0), min(fc1, fine.shape[1])
        block[sr0 - fr0:sr1 - fr0, sc0 - fc0:sc1 - fc0] = fine[sr0:sr1, sc0:sc1]
        have = np.isfinite(block)
        filled = np.where(have, block, 0.0).astype(np.float32)
        weight = have.astype(np.float32)
        relief = np.zeros_like(filled)
        for radius, amount in ((1, 0.7), (2, 0.4)):
            around = _box(filled, radius) / np.maximum(_box(weight, radius), 1e-3)
            relief += amount * (around - filled)
        relief = np.clip(np.where(have, relief, 0.0), -1.5, 1.5)[2:-2, 2:-2]  # the squares' own samples, 2 to each
        total = np.zeros((n, cols), np.float32)
        middle = pad[1 + v0:1 + v1, 1:-1]
        middle_h = pad_h[1 + v0:1 + v1, 1:-1]
        share = fine_share[v0:v1]
        steep_here = steep[v0:v1]
        jj, ii = np.mgrid[v0:v1, 0:cols].astype(np.float32)
        for dz in (-1, 1):
            for dx in (-1, 1):
                at = (0.5625 * middle + 0.1875 * (pad[1 + v0:1 + v1, 1 + dx:cols + 1 + dx] + pad[1 + v0 + dz:1 + v1 + dz, 1:-1])
                      + 0.0625 * pad[1 + v0 + dz:1 + v1 + dz, 1 + dx:cols + 1 + dx])
                h = (0.5625 * middle_h + 0.1875 * (pad_h[1 + v0:1 + v1, 1 + dx:cols + 1 + dx] + pad_h[1 + v0 + dz:1 + v1 + dz, 1:-1])
                     + 0.0625 * pad_h[1 + v0 + dz:1 + v1 + dz, 1 + dx:cols + 1 + dx])
                # This point's 1 m sample: row r0 + 2 j + (dz - 1) / 2, column c0 + 2 i + (dx - 1) / 2.
                rr, cc = (dz + 1) // 2, (dx + 1) // 2
                at = at + relief[rr:rr + 2 * n:2, cc:cc + 2 * cols:2] * share
                x = ii * spacing_m + 0.5 * dx
                z = jj * spacing_m + 0.5 * dz
                # Beds: tilted along the strike, wandering by tens of meters over a few hundred.
                bed = (h + LEDGE_DIP * (x * math.cos(strike) + z * math.sin(strike))
                       + 24.0 * _value_noise(x, z, 180.0, seed) + 6.0 * _value_noise(x, z, 35.0, seed + 1))
                ledge = 0.5 + 0.5 * np.sin(bed * (2.0 * math.pi / LEDGE_EVERY_M))
                joints = _value_noise(x, z, 3.0, seed + 2) * 0.6 + _value_noise(x, z, 9.0, seed + 3) * 0.4
                at = at + steep_here * share * (LEDGE_SNOW_M * (2.0 * np.clip((ledge - 0.35) / 0.4, 0.0, 1.0) - 1.0)
                                               + JOINT_SNOW_M * (2.0 * joints - 1.0))
                total += at >= shows_m
        out[v0:v1] = total / 4.0
    return out


def bake(heights, real_m, share, spacing_m, line_m, wind_from_deg, suns, ice_from_m, glacier=None, repose_deg=60.0,
         depth_m=3.0, iterations=240):
    """Snow depth in meters at each of the map's samples.

    heights: the ground at spacing_m; real_m: the height snowfall goes by at each place (the real mountain's, where the
    map is built from one); share: where it falls, 0 to 1; line_m: the snow line, in real_m's terms; wind_from_deg: where
    the prevailing wind comes from; suns: (azimuth, elevation) pairs over a day of the season; ice_from_m: in real_m's
    terms, where steep rock starts to keep a coat of ice, all of it 700 m higher; repose_deg: what snow slides off, at
    spacing_m (steeper than at a coarser one); depth_m: how much falls just past the line. glacier: optional, how much of
    each sample a glacier covers (glaciers.py): there it's ice, which no slope bares, and from the snow line up, white."""
    rows, cols = heights.shape
    heights = heights.astype(np.float32)

    # 1. Snowfall by altitude.
    t = np.clip((real_m - (line_m - 150.0)) / 300.0, 0.0, 1.0)
    fall = depth_m * t * t * (3.0 - 2.0 * t) * np.clip(1.0 + (real_m - line_m) / 1500.0, 1.0, 2.0) * share
    # 2. Wind, at four times the spacing: crests and the hollows behind them are tens of meters.
    quarter = (rows // 4, cols // 4)
    sheltered = _resize(shelter_deg(_resize(heights, quarter), spacing_m * 4.0, wind_from_deg), (rows, cols))
    fall *= np.clip(1.0 + sheltered / 12.0, 0.25, 1.8)
    # 3. Sun, at twice the spacing.
    half = (rows // 2, cols // 2)
    fall *= _resize(np.clip(1.35 - 0.55 * sunshine(_resize(heights, half), spacing_m * 2.0, suns), 0.15, 1.25), (rows, cols))
    # 4. Gravity, at the map's own spacing, so it runs down the gullies in streaks.
    snow = np.ascontiguousarray(fall, dtype=np.float32)
    # On the land as it is over about 14 m (SHAPE_SAMPLES): judged at the map's own 2 m, every step and rib a few meters
    # across bared rock, in spots and streaks rather than faces.
    shape = _box(heights, SHAPE_SAMPLES)
    erosion.slide(shape.copy(), snow, spacing_m, repose_deg, iterations)

    # 5. What each slope holds, the ice up high, and the fine relief: on rock faces, measured at the map's own spacing,
    #    so ribs stand out; on ground that's gentle over tens of meters, a glacier or a snowfield, over that much, so a
    #    crevasse, an ice cliff or a drift's edge is a wall of ice and snow, shaded as one, not a line of bare rock.
    rocky = rockiness(heights, spacing_m)
    if glacier is not None:
        rocky = rocky * (1.0 - glacier)
    gz, gx = np.gradient(shape, spacing_m)
    slope_deg = np.degrees(np.arctan(np.hypot(gx, gz))).astype(np.float32)
    cold = np.clip((real_m - ice_from_m) / 700.0, 0.0, 1.0) * (share > 0.0)
    face = np.minimum(snow, np.interp(slope_deg, HOLD_DEG, HOLD_M).astype(np.float32))
    face = np.maximum(face, cold * np.interp(slope_deg, COAT_DEG, COAT_M).astype(np.float32))
    # No ribs standing out of thin snow, nor hollows filling, by the land's relief: on a rugged face it moved the depth by
    # meters, more than the snow itself, and decided snow or rock in patches.
    gz, gx = np.gradient(_box(heights, BROAD_SAMPLES), spacing_m)
    broad_deg = np.degrees(np.arctan(np.hypot(gx, gz))).astype(np.float32)
    field = np.minimum(_box(snow, BROAD_SAMPLES), np.interp(broad_deg, HOLD_DEG, HOLD_M).astype(np.float32))
    field = np.maximum(field, cold * np.interp(broad_deg, COAT_DEG, COAT_M).astype(np.float32))
    depth = field + rocky * (face - field)
    if glacier is not None:
        # A glacier's surface, past the snow line, is snow and ice however it's broken up: never shallower than this.
        t = np.clip((real_m - (line_m - 150.0)) / 300.0, 0.0, 1.0)
        depth = np.maximum(depth, glacier * t * t * (3.0 - 2.0 * t) * GLACIER_COVER_M)
    return np.maximum(depth, 0.0).astype(np.float32)


GLACIER_COVER_M = 1.0


# What snow lies on: the land over SHAPE_SAMPLES * 2 + 1 samples (14 m at 2 m).
SHAPE_SAMPLES = 3
# Rock shows its ribs only where the ground is steep over BROAD_SAMPLES * 2 + 1 samples (22 m at 2 m): past
# ROCKY_DEG[0] it starts to, past ROCKY_DEG[1] it does fully.
BROAD_SAMPLES = 5
ROCKY_DEG = (32.0, 45.0)


def rockiness(heights, spacing_m):
    """0 to 1: how far the ground is a rock face, not a glacier or a snowfield, by its slope over tens of meters."""
    gz, gx = np.gradient(_box(heights.astype(np.float32), BROAD_SAMPLES), spacing_m)
    broad_deg = np.degrees(np.arctan(np.hypot(gx, gz)))
    t = np.clip((broad_deg - ROCKY_DEG[0]) / (ROCKY_DEG[1] - ROCKY_DEG[0]), 0.0, 1.0)
    return (t * t * (3.0 - 2.0 * t)).astype(np.float32)
