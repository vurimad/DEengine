"""The shrine: a small test world to judge lighting and global illumination on, not part of the game's map. A round
stone hall under a low dome, daylight through one arched doorway, a basin in the middle ringed with candles, a kneeling
statue at its far edge and a few props along the wall: most of the room is lit only by light that has bounced.

    python tools/worldgen/shrine.py [--props build/agent/shrine_scene/5_models/final]

The props are models made elsewhere (one .glb each: candle, clay_pot, basin_empty, crate, lantern, statue,
pillar_fallen, sword, banner), copied in as they are. The lantern is a prefab (props/lantern.prefab: its frame and
the light inside it). A candle is a prefab (props/candle.prefab: the model and its
flame, an effect in effects/), placed once for each; dust motes drift in the hall's air (effects/dust_motes.vfx). The hall is drawn here, by rules, as a stand-in until a modelled
one replaces it: its sizes are read from the scene's section drawing; its surfaces wear Poly Haven textures (CC0)
where those are in hall/polyhaven/, and noise drawn here where they aren't. Its floor is paved
slab by slab, each a block of its own, some heaved, sunk, cracked or chipped, and what stands on it rests on them;
or, with --paving, a floor tile made elsewhere is laid instead, and things rest on that. Writes
projects/dreadworld/content/shrine/ and its folder rules in meta/, which aren't committed.
"""
import argparse
import json
import math
import os
import shutil
import site
import struct
import sys

sys.path.append(site.getusersitepackages())
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
PROJECT = os.path.join(ROOT, 'projects', 'dreadworld')
OUT = os.path.join(PROJECT, 'content', 'shrine')
META = os.path.join(PROJECT, 'meta', 'shrine')

CENTER = (64.0, 0.0, 64.0)       # the middle of the hall's floor, in its cell (0, 0)

# The hall, meters. Angles run clockwise from north (-Z) toward east (+X), like the sun's azimuth.
RADIUS = 7.2                     # inside
WALL = 0.9                       # how thick: thick enough that no shadow leaks
WALL_TOP = 4.4                   # floor to the top of the cornice, where the dome springs
DOME_TOP = 8.8                   # floor to the dome's top, inside
BAYS = 10                        # a pilaster between each two; bay 0 is the north one, behind the statue
DOOR_BAY = -2                    # west-north-west
DOOR_WIDTH, DOOR_HEIGHT = 1.4, 3.2   # a round arch: its top half a circle
DOOR_AT = math.radians(DOOR_BAY * 360.0 / BAYS)
DOOR_SPRING = DOOR_HEIGHT - DOOR_WIDTH / 2
DOME_RADIUS = (RADIUS * RADIUS + (DOME_TOP - WALL_TOP) ** 2) / (2 * (DOME_TOP - WALL_TOP))  # a cap of a sphere
DOME_CENTER_Y = DOME_TOP - DOME_RADIUS
SHELL = 0.5                      # the dome's thickness
ROOF_Y = WALL_TOP + 0.5          # the outside wall's top
RING_OUTER, RING_INNER = 1.3, 0.95   # the closed ring at the dome's top

# The floor: slabs laid in courses, each its own block, settled unevenly over the years.
PAVING_RIM = RADIUS + WALL + 0.05    # out under the wall and through the door
JOINT = 0.020                    # the gap between two slabs
JOINT_Y = -0.022                 # the dirt in it
EDGE, EDGE_DROP = 0.024, 0.012   # how far in a slab's worn edge starts, and how far down it ends
QUIET = (1.7, 2.7)               # from the middle: level under the basin and the statue, fully uneven past this
HEAVED, SUNK, CRACKED, CHIPPED = 0.18, 0.08, 0.10, 0.14   # how many of the slabs, of 1

# The sun stands a little north of the door's own direction, so its light crosses the floor past the basin.
SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG = 296.0, 26.0

# What the hall's surfaces wear: a Poly Haven texture (CC0, polyhaven.com) where its folder is there, hall/polyhaven/<name>
# holding <name>_diff_2k.jpg, _nor_gl_2k.jpg and _arm_2k.jpg, or else the noise drawn here. Each texture with how wide
# a piece of the world it shows, meters.
POLYHAVEN = {'rock_01': 1.5, 'rock_boulder_dry': 1.8, 'plastered_stone_wall': 2.5, 'medieval_wall_01': 2.0,
             'medieval_blocks_03': 2.0, 'rocky_trail': 2.0}
SURFACES = {'floor': 'rock_01',               # each slab cuts its own piece (livelier: rock_boulder_dry)
            'plaster': 'plastered_stone_wall',   # the wall and the dome, inside (lighter: medieval_wall_01)
            'stone': 'rock_01',               # plinth, pilasters, cornice, ribs, the doorway
            'outside': 'medieval_blocks_03',  # the wall, outside
            'roof': 'rock_01',                # the dome, outside
            'ground': 'rocky_trail'}          # the land around
# Spread wider than life where a texture would repeat in plain sight: its marks get that much bigger.
STRETCH = {'plaster': 1.5, 'ground': 2.0}
DRAWN = {'floor': 'floor', 'plaster': 'plaster', 'stone': 'stone', 'outside': 'stone', 'roof': 'stone'}
DRAWN_TILE = {'floor': 4.0, 'plaster': 3.0, 'stone': 2.0}
# How far apart each surface's texture repeats, meters: set by main() from what it wears.
TILE = {'floor': 4.0, 'plaster': 3.0, 'stone': 2.0, 'outside': 2.0, 'roof': 2.0, 'ground': 4.0}

UP = np.array([0.0, 1.0, 0.0])


def radial(theta):
    return np.array([math.sin(theta), 0.0, -math.cos(theta)])


def tangent(theta):
    return np.array([math.cos(theta), 0.0, math.sin(theta)])


def cyl(theta, r, y):
    return radial(theta) * r + UP * y


class Part:
    """The triangles of one material."""

    def __init__(self):
        self.positions, self.normals, self.uvs, self.indices = [], [], [], []

    def triangle(self, a, b, c):
        pa, pb, pc = self.positions[a], self.positions[b], self.positions[c]
        face = np.cross(pb - pa, pc - pa)
        if np.dot(face, face) < 1e-16:
            return
        # Counter-clockwise seen from where the normals point.
        if np.dot(face, self.normals[a] + self.normals[b] + self.normals[c]) < 0.0:
            b, c = c, b
        self.indices += [a, b, c]

    def grid(self, f, nu, nv):
        """f(u, v), both 0 to 1, gives a position, a normal and a uv."""
        base = len(self.positions)
        for j in range(nv + 1):
            for i in range(nu + 1):
                position, normal, uv = f(i / nu, j / nv)
                self.positions.append(np.asarray(position, dtype=np.float64))
                self.normals.append(np.asarray(normal, dtype=np.float64) / np.linalg.norm(normal))
                self.uvs.append(uv)
        for j in range(nv):
            for i in range(nu):
                a = base + j * (nu + 1) + i
                self.triangle(a, a + 1, a + nu + 2)
                self.triangle(a, a + nu + 2, a + nu + 1)

    def quad(self, corners, normal, tile):
        """A flat face, its texture laid along its first edge."""
        normal = np.asarray(normal, dtype=np.float64)
        along = corners[1] - corners[0]
        along = along / np.linalg.norm(along)
        across = np.cross(normal, along)

        def f(u, v):
            p = (corners[0] * (1 - u) + corners[1] * u) * (1 - v) + (corners[3] * (1 - u) + corners[2] * u) * v
            return p, normal, (float(np.dot(p, along)) / tile, float(np.dot(p, across)) / tile)
        self.grid(f, 1, 1)

    def sweep(self, profile, tile, segments=120, flip=False, start=0.0, end=2 * math.pi):
        """A profile of (radius, height) points turned about the hall's axis, flat between its points. Its faces look
        to the left of the way it runs (toward the axis, for one that runs upward), or to the right with flip."""
        length = 0.0
        for (ra, ya), (rb, yb) in zip(profile, profile[1:]):
            dr, dy = rb - ra, yb - ya
            run = math.hypot(dr, dy)
            nr, ny = (-dy / run, dr / run) if not flip else (dy / run, -dr / run)

            def f(u, v, ra=ra, ya=ya, dr=dr, dy=dy, nr=nr, ny=ny, run=run, length=length):
                theta = start + (end - start) * u
                return (cyl(theta, ra + dr * v, ya + dy * v), radial(theta) * nr + UP * ny,
                        (theta * RADIUS / tile, (length + run * v) / tile))
            self.grid(f, segments, 1)
            length += run

    def box(self, theta, x0, x1, near, far, y0, y1, tile, faces):
        """A block against the wall at theta: x across it, near and far its distances from the hall's axis."""
        out, across = radial(theta), tangent(theta)

        def at(x, d, y):
            return out * d + across * x + UP * y
        if 'front' in faces:
            self.quad([at(x0, near, y0), at(x1, near, y0), at(x1, near, y1), at(x0, near, y1)], -out, tile)
        if 'sides' in faces:
            self.quad([at(x0, far, y0), at(x0, near, y0), at(x0, near, y1), at(x0, far, y1)], -across, tile)
            self.quad([at(x1, near, y0), at(x1, far, y0), at(x1, far, y1), at(x1, near, y1)], across, tile)
        if 'top' in faces:
            self.quad([at(x0, near, y1), at(x1, near, y1), at(x1, far, y1), at(x0, far, y1)], UP, tile)
        if 'bottom' in faces:
            self.quad([at(x0, near, y0), at(x1, near, y0), at(x1, far, y0), at(x0, far, y0)], -UP, tile)


def arch_y(x):
    """The door's top, x meters to the side of its middle."""
    half = DOOR_WIDTH / 2
    return DOOR_SPRING + math.sqrt(max(0.0, half * half - x * x))


def wall_face(part, r, y0, y1, inward, tile, shifts=None):
    """The wall's face at radius r, all round but the door's opening, bay by bay. With shifts (one (u, v) per bay,
    in meters), each bay takes its own piece of the texture: the pilasters stand on the seams. Without, it runs round
    unbroken, squeezed a little so that a whole number of repeats goes round."""
    sign = -1.0 if inward else 1.0
    half = math.asin(DOOR_WIDTH / 2 / r)
    rows = (y0, DOOR_SPRING, y1)
    around = 2 * math.pi * r
    wide = tile if shifts else around / max(1, round(around / tile))

    def uv(theta, y, shift):
        return ((theta * r + shift[0]) / wide, (shift[1] - y) / tile)

    def strip(a, b, shift):
        def f(u, v):
            theta, y = a + (b - a) * u, rows[round(v * 2)]
            return cyl(theta, r, y), radial(theta) * sign, uv(theta, y, shift)
        part.grid(f, max(1, round((b - a) / math.radians(3.0))), 2)

    for bay in range(BAYS):
        middle = math.radians(bay * 360.0 / BAYS)
        a, b = middle - math.pi / BAYS, middle + math.pi / BAYS
        shift = shifts[bay] if shifts else (0.0, 0.0)
        if bay != DOOR_BAY % BAYS:
            strip(a, b, shift)
            continue
        door = DOOR_AT % (2 * math.pi)
        strip(a, door - half, shift)
        strip(door + half, b, shift)

        def over_door(u, v, shift=shift, door=door):
            x = -DOOR_WIDTH / 2 + DOOR_WIDTH * u
            theta, y = door + math.asin(x / r), arch_y(x) + (y1 - arch_y(x)) * v
            return cyl(theta, r, y), radial(theta) * sign, uv(theta, y, shift)
        part.grid(over_door, 24, 1)


def door_tunnel(part, tile):
    """The doorway through the wall: its two sides and the arch over them."""
    out, across = radial(DOOR_AT), tangent(DOOR_AT)
    half = DOOR_WIDTH / 2

    def depth(x, r):
        return math.sqrt(r * r - x * x)
    for side in (-1.0, 1.0):
        x = side * half
        near, far = depth(x, RADIUS), depth(x, RADIUS + WALL)
        foot = out * near + across * x + UP * JOINT_Y
        part.quad([foot, foot + out * (far - near), out * far + across * x + UP * DOOR_SPRING,
                   out * near + across * x + UP * DOOR_SPRING], across * -side, tile)

    def soffit(u, v):
        phi = math.pi * u
        x, y = half * math.cos(phi), DOOR_SPRING + half * math.sin(phi)
        d = depth(x, RADIUS) + (depth(x, RADIUS + WALL) - depth(x, RADIUS)) * v
        return (out * d + across * x + UP * y, -(across * math.cos(phi) + UP * math.sin(phi)),
                (half * phi / tile, d / tile))
    part.grid(soffit, 24, 1)


def sphere_band(part, rho, a0, a1, inward, tile, segments=120, rings=20):
    """A band of the dome's sphere between two angles from straight up."""
    sign = -1.0 if inward else 1.0

    def f(u, v):
        theta, alpha = 2 * math.pi * u, a0 + (a1 - a0) * v
        direction = radial(theta) * math.sin(alpha) + UP * math.cos(alpha)
        return (UP * DOME_CENTER_Y + direction * rho, direction * sign,
                (u * max(1, round(2 * math.pi * RADIUS / tile)), rho * alpha / tile))
    part.grid(f, segments, rings)


def rib(part, theta, a0, a1, tile, width=0.36, depth=0.12):
    """A rib down the inside of the dome, from the ring to the cornice."""
    out, across = radial(theta), tangent(theta)

    def at(alpha, rho, x):
        return UP * DOME_CENTER_Y + (out * math.sin(alpha) + UP * math.cos(alpha)) * rho + across * x

    def front(u, v):
        alpha = a0 + (a1 - a0) * v
        return (at(alpha, DOME_RADIUS - depth, (u - 0.5) * width), -(out * math.sin(alpha) + UP * math.cos(alpha)),
                (u * width / tile, DOME_RADIUS * alpha / tile))
    part.grid(front, 1, 16)
    for side in (-1.0, 1.0):
        def flank(u, v, side=side):
            alpha = a0 + (a1 - a0) * v
            return (at(alpha, DOME_RADIUS - depth * (1 - u), side * width / 2), across * side,
                    (u * depth / tile, DOME_RADIUS * alpha / tile))
        part.grid(flank, 1, 16)


class Slab:
    """One slab of the floor: its outline seen from above (x, z), and the plane its top lies in."""

    def __init__(self, outline, height, slope, rng):
        self.outline = outline
        self.center = (sum(x for x, _ in outline) / len(outline), sum(z for _, z in outline) / len(outline))
        self.height, self.slope = height, slope
        self.shift = (rng.uniform(0.0, 8.0), rng.uniform(0.0, 8.0))   # its own piece of the stone's texture
        self.turn = int(rng.integers(0, 4))

    def top(self, x, z):
        return self.height + self.slope[0] * (x - self.center[0]) + self.slope[1] * (z - self.center[1])

    def holds(self, x, z):
        sign = 0.0
        for (ax, az), (bx, bz) in zip(self.outline, self.outline[1:] + self.outline[:1]):
            side = (bx - ax) * (z - az) - (bz - az) * (x - ax)
            if side * sign < 0.0:
                return False
            sign = side if side != 0.0 else sign
        return True

    def uv(self, x, z):
        u, v = [(x, z), (z, -x), (-x, -z), (-z, x)][self.turn]
        return ((u + self.shift[0]) / TILE['floor'], (v + self.shift[1]) / TILE['floor'])


def clipped(outline, a, b, c):
    """What of a convex outline lies where a*x + b*z <= c."""
    kept = []
    for (px, pz), (qx, qz) in zip(outline, outline[1:] + outline[:1]):
        dp, dq = a * px + b * pz - c, a * qx + b * qz - c
        if dp <= 0.0:
            kept.append((px, pz))
        if (dp < 0.0 < dq) or (dq < 0.0 < dp):
            t = dp / (dp - dq)
            kept.append((px + (qx - px) * t, pz + (qz - pz) * t))
    return kept


def area(outline):
    return 0.5 * sum(ax * bz - bx * az for (ax, az), (bx, bz) in zip(outline, outline[1:] + outline[:1]))


_paving = None


def paving():
    """The floor's slabs: courses 0.62 to 0.92 m deep running east to west, slabs 0.6 to 1.15 m long, cut off round
    where the hall's wall stands on them. Most lie nearly level; some have heaved up on one side, some have sunk, some
    have cracked in two or lost a corner."""
    global _paving
    if _paving is not None:
        return _paving
    rng = np.random.default_rng(5)
    slabs = []

    def settle(outline):
        outline = [p for i, p in enumerate(outline)
                   if math.hypot(p[0] - outline[i - 1][0], p[1] - outline[i - 1][1]) > 1e-5]
        if len(outline) < 3 or abs(area(outline)) < 0.03:
            return
        cx, cz = sum(x for x, _ in outline) / len(outline), sum(z for _, z in outline) / len(outline)
        quiet = float(smoothstep(QUIET[0], QUIET[1], math.hypot(cx, cz)))
        kind = rng.random()
        if kind < HEAVED:        # one side up to a few centimeters proud of its neighbours
            way, steep = rng.uniform(0.0, 2 * math.pi), math.tan(math.radians(rng.uniform(1.0, 2.2)))
            height, slope = rng.uniform(0.004, 0.016), (math.cos(way) * steep, math.sin(way) * steep)
        elif kind < HEAVED + SUNK:
            height, slope = -rng.uniform(0.008, 0.018), tuple(rng.normal(0.0, 0.006, 2))
        else:
            height, slope = float(np.clip(rng.normal(0.0, 0.004), -0.009, 0.009)), tuple(rng.normal(0.0, 0.005, 2))
        slabs.append(Slab(outline, height * quiet, (slope[0] * quiet, slope[1] * quiet), rng))

    z = -PAVING_RIM - rng.uniform(0.0, 0.5)
    while z < PAVING_RIM:
        depth = rng.uniform(0.62, 0.92)
        x = -PAVING_RIM - rng.uniform(0.0, 1.0)
        while x < PAVING_RIM:
            length = rng.uniform(0.6, 1.15)
            half = JOINT / 2
            outline = [(x + half, z + half), (x + length - half, z + half), (x + length - half, z + depth - half),
                       (x + half, z + depth - half)]
            x += length
            reach = [math.hypot(px, pz) for px, pz in outline]
            if min(reach) > PAVING_RIM and not (outline[0][0] < 0.0 < outline[1][0] or outline[0][1] < 0.0 < outline[2][1]):
                continue
            if max(reach) > PAVING_RIM:
                for k in range(96):
                    angle = 2 * math.pi * k / 96
                    outline = clipped(outline, math.cos(angle), math.sin(angle), PAVING_RIM)
                    if len(outline) < 3:
                        break
            elif rng.random() < CHIPPED:      # a corner gone
                corner = int(rng.integers(0, 4))
                (px, pz), (ax, az), (bx, bz) = outline[corner], outline[corner - 1], outline[(corner + 1) % 4]
                da, db = rng.uniform(0.05, 0.16), rng.uniform(0.05, 0.16)
                la, lb = math.hypot(ax - px, az - pz), math.hypot(bx - px, bz - pz)
                ex, ez = px + (ax - px) * da / la, pz + (az - pz) * da / la
                fx, fz = px + (bx - px) * db / lb, pz + (bz - pz) * db / lb
                nx, nz = fz - ez, ex - fx
                if nx * (px - ex) + nz * (pz - ez) < 0.0:
                    nx, nz = -nx, -nz           # toward the corner that goes
                outline = clipped(outline, nx, nz, nx * ex + nz * ez)
            elif rng.random() < CRACKED:      # broken in two, each half settled its own way
                way = rng.uniform(0.0, math.pi)
                nx, nz = math.cos(way), math.sin(way)
                through = nx * (x - length * rng.uniform(0.35, 0.65)) + nz * (z + depth * rng.uniform(0.35, 0.65))
                halves = [clipped(outline, nx, nz, through - 0.004), clipped(outline, -nx, -nz, -through - 0.004)]
                for piece in halves:
                    if len(piece) >= 3 and abs(area(piece)) > 0.03:
                        settle(piece)
                continue
            if len(outline) >= 3 and abs(area(outline)) > 0.03:
                settle(outline)
        z += depth
    _paving = slabs
    return slabs


class TileFloor:
    """A floor made of one square tile repeated: a model made elsewhere (content/shrine/floor/), its corner at its
    origin, laid on a grid so that the hall's middle is a tile's middle. Read from its .glb, for how high it stands."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            magic, _, _ = struct.unpack('<4sII', f.read(12))
            length, kind = struct.unpack('<I4s', f.read(8))
            assert magic == b'glTF' and kind == b'JSON', path
            document = json.loads(f.read(length).decode('utf-8'))
            length, _ = struct.unpack('<I4s', f.read(8))
            blob = f.read(length)

        def read(index, dtype, width):
            accessor = document['accessors'][index]
            view = document['bufferViews'][accessor['bufferView']]
            assert 'byteStride' not in view, path
            start = view.get('byteOffset', 0) + accessor.get('byteOffset', 0)
            return np.frombuffer(blob, dtype, accessor['count'] * width, start).reshape(accessor['count'], width)
        primitive = document['meshes'][0]['primitives'][0]
        positions = read(primitive['attributes']['POSITION'], '<f4', 3).astype(np.float64)
        indices_of = document['accessors'][primitive['indices']]
        indices = read(primitive['indices'], '<u4' if indices_of['componentType'] == 5125 else '<u2', 1).reshape(-1, 3)
        corners = positions[indices]                       # triangles, their three corners
        face = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
        tops = face[:, 1] > 0.7 * np.linalg.norm(face, axis=1)
        self.tops = corners[tops]
        self.size = float(positions[:, 0].max() - positions[:, 0].min())
        self.triangles = len(indices)

    def tiles(self, reach):
        """The corners of the tiles it takes to cover that far from the hall's middle, each way."""
        count = 2 * math.ceil((reach - self.size / 2) / self.size) + 1
        first = -count * self.size / 2
        return [(first + i * self.size, first + j * self.size) for j in range(count) for i in range(count)]

    def height(self, x, z):
        u, w = (x + self.size / 2) % self.size, (z + self.size / 2) % self.size
        a, b, c = self.tops[:, 0], self.tops[:, 1], self.tops[:, 2]
        # Where the point is in each top triangle, seen from above.
        det = (b[:, 2] - c[:, 2]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 2] - c[:, 2])
        det = np.where(np.abs(det) < 1e-12, 1e-12, det)
        la = ((b[:, 2] - c[:, 2]) * (u - c[:, 0]) + (c[:, 0] - b[:, 0]) * (w - c[:, 2])) / det
        lb = ((c[:, 2] - a[:, 2]) * (u - c[:, 0]) + (a[:, 0] - c[:, 0]) * (w - c[:, 2])) / det
        lc = 1.0 - la - lb
        inside = (la >= -1e-6) & (lb >= -1e-6) & (lc >= -1e-6)
        if not inside.any():
            return 0.0
        return float((la * a[:, 1] + lb * b[:, 1] + lc * c[:, 1])[inside].max())


# The floor laid instead of the slabs drawn here, if main() was asked for one.
_tile_floor = None
_tile_model = None


def floor_height(x, z, reach=0.0):
    """How high the floor stands at a place (meters from the hall's middle), or the highest it stands within reach of
    it: what a thing that wide comes to rest on."""
    places = [(x, z)] + [(x + reach * math.cos(a), z + reach * math.sin(a)) for a in np.arange(8) * math.pi / 4
                         if reach > 0.0]
    if _tile_floor is not None:
        return max(_tile_floor.height(px, pz) for px, pz in places)
    heights = [slab.top(px, pz) for px, pz in places for slab in paving() if slab.holds(px, pz)]
    return max(heights) if heights else 0.0


def build_paving(part):
    """Each slab as a block: its top, a worn edge round it, its sides down into the dirt."""
    for slab in paving():
        outline = slab.outline if area(slab.outline) > 0.0 else slab.outline[::-1]
        count = len(outline)
        inward = []
        for (ax, az), (bx, bz) in zip(outline, outline[1:] + outline[:1]):
            run = math.hypot(bx - ax, bz - az)
            inward.append((-(bz - az) / run, (bx - ax) / run))
        inner = []
        for i, (x, z) in enumerate(outline):
            (ax, az), (bx, bz) = inward[i - 1], inward[i]
            spread = max(1.0 + ax * bx + az * bz, 0.35)     # a sharp corner's edge isn't pulled in further than this
            inner.append((x + EDGE * (ax + bx) / spread, z + EDGE * (az + bz) / spread))
        up = np.array([-slab.slope[0], 1.0, -slab.slope[1]])
        up /= np.linalg.norm(up)

        def high(x, z):
            return np.array([x, slab.top(x, z), z])

        def low(x, z):
            return np.array([x, slab.top(x, z) - EDGE_DROP, z])

        # The top: a fan about its middle.
        base = len(part.positions)
        cx, cz = sum(x for x, _ in inner) / count, sum(z for _, z in inner) / count
        for x, z in [(cx, cz)] + inner:
            part.positions.append(high(x, z))
            part.normals.append(up)
            part.uvs.append(slab.uv(x, z))
        for i in range(count):
            part.triangle(base, base + 1 + i, base + 1 + (i + 1) % count)

        for i in range(count):
            j = (i + 1) % count
            out = np.array([-inward[i][0], 0.0, -inward[i][1]])
            # The worn edge: its normals run from the top's to halfway outward, so it reads as rounded.
            base = len(part.positions)
            rounded = (out + UP) / math.sqrt(2.0)
            for position, normal, (x, z) in ((high(*inner[i]), up, inner[i]), (high(*inner[j]), up, inner[j]),
                                             (low(*outline[j]), rounded, outline[j]),
                                             (low(*outline[i]), rounded, outline[i])):
                part.positions.append(position)
                part.normals.append(normal)
                part.uvs.append(slab.uv(x, z))
            part.triangle(base, base + 1, base + 2)
            part.triangle(base, base + 2, base + 3)
            # The side, down past the dirt.
            base = len(part.positions)
            along = 0.0
            for (x, z), bottom in ((outline[i], False), (outline[j], False), (outline[j], True), (outline[i], True)):
                position = low(x, z)
                if bottom:
                    position[1] = JOINT_Y - 0.02
                along = math.hypot(x - outline[i][0], z - outline[i][1])
                part.positions.append(position)
                part.normals.append(out)
                part.uvs.append(((along + slab.shift[0]) / TILE['floor'],
                                 (slab.shift[1] - position[1]) / TILE['floor']))
            part.triangle(base, base + 1, base + 2)
            part.triangle(base, base + 2, base + 3)


def build_hall():
    """The hall's parts, by material."""
    floor, joints, plaster, stone, outside, roof, ground = Part(), Part(), Part(), Part(), Part(), Part(), Part()
    rng = np.random.default_rng(3)

    # The floor's slabs, out under the wall and through the door, on the dirt that shows between them; the land
    # around, a step lower.
    if _tile_floor is None:
        build_paving(floor)
    joints.sweep([(PAVING_RIM, JOINT_Y), (0.0, JOINT_Y)], 2.0, flip=True)
    ground.quad([np.array([-50.0, -0.04, 50.0]), np.array([50.0, -0.04, 50.0]), np.array([50.0, -0.04, -50.0]),
                 np.array([-50.0, -0.04, -50.0])], UP, TILE['ground'])

    # Inside: the wall, the dome between its ribs, the ring that closes its top.
    wall_face(plaster, RADIUS, JOINT_Y, WALL_TOP, True, TILE['plaster'],
              [(rng.uniform(0.0, 8.0), rng.uniform(0.0, 8.0)) for _ in range(BAYS)])
    spring = math.asin(RADIUS / DOME_RADIUS)
    ring = math.asin(RING_OUTER / DOME_RADIUS)
    sphere_band(plaster, DOME_RADIUS, spring, ring, True, TILE['plaster'])
    ring_y = DOME_CENTER_Y + math.sqrt(DOME_RADIUS ** 2 - RING_OUTER ** 2)
    stone.sweep([(RING_OUTER, ring_y), (RING_OUTER, DOME_TOP - 0.3), (RING_INNER, DOME_TOP - 0.3),
                 (RING_INNER, DOME_TOP - 0.15)], TILE['stone'], segments=60)
    plaster.sweep([(RING_INNER, DOME_TOP - 0.15), (0.0, DOME_TOP - 0.15)], TILE['plaster'], segments=60)

    # The stonework: a plinth along the wall's foot, a cornice under the dome, a pilaster and a rib between bays.
    foot = math.asin((DOOR_WIDTH / 2 + 0.02) / RADIUS)
    stone.sweep([(RADIUS - 0.08, JOINT_Y), (RADIUS - 0.08, 0.45), (RADIUS, 0.45)], TILE['stone'], segments=116,
                start=DOOR_AT + foot, end=DOOR_AT - foot + 2 * math.pi)
    stone.sweep([(RADIUS, WALL_TOP - 0.45), (RADIUS - 0.08, WALL_TOP - 0.4), (RADIUS - 0.08, WALL_TOP - 0.25),
                 (RADIUS - 0.2, WALL_TOP - 0.18), (RADIUS - 0.2, WALL_TOP), (RADIUS, WALL_TOP)], TILE['stone'])
    for bay in range(BAYS):
        theta = math.radians((bay + 0.5) * 360.0 / BAYS)
        stone.box(theta, -0.25, 0.25, RADIUS - 0.15, RADIUS, 0.6, WALL_TOP - 0.68, TILE['stone'], ('front', 'sides'))
        stone.box(theta, -0.31, 0.31, RADIUS - 0.21, RADIUS, JOINT_Y, 0.6, TILE['stone'], ('front', 'sides', 'top'))
        stone.box(theta, -0.31, 0.31, RADIUS - 0.21, RADIUS, WALL_TOP - 0.68, WALL_TOP - 0.4, TILE['stone'],
                  ('front', 'sides', 'top', 'bottom'))
        rib(stone, theta, ring, spring, TILE['stone'])
    door_tunnel(stone, TILE['stone'])

    # Outside, so that the sun's shadows have something to be cast by: the wall, a flat roof to the dome, the dome,
    # a low drum over the ring.
    wall_face(outside, RADIUS + WALL, -0.04, ROOF_Y, False, TILE['outside'])
    outer = DOME_RADIUS + SHELL
    roof_r = math.sqrt(outer ** 2 - (ROOF_Y - DOME_CENTER_Y) ** 2)
    roof.sweep([(RADIUS + WALL, ROOF_Y), (roof_r, ROOF_Y)], TILE['roof'], flip=True)
    drum_r = RING_OUTER + 0.1
    drum_y = DOME_CENTER_Y + math.sqrt(outer ** 2 - drum_r ** 2)
    sphere_band(roof, outer, math.asin(roof_r / outer), math.asin(drum_r / outer), False, TILE['roof'])
    roof.sweep([(drum_r, drum_y), (drum_r, drum_y + 0.35), (0.0, drum_y + 0.35)], TILE['roof'], segments=60,
               flip=True)
    return {'floor': floor, 'joints': joints, 'plaster': plaster, 'stone': stone, 'outside': outside, 'roof': roof,
            'ground': ground}


# --- Textures -------------------------------------------------------------------------------------------------------

def noise(n, seed, slope, low=1.0, high=None):
    """Noise that repeats over the square: mean 0, deviation 1. slope is how fast it thins toward fine detail; low and
    high are the coarsest and finest waves across the square it holds."""
    rng = np.random.default_rng(seed)
    frequency = np.fft.fftfreq(n) * n
    fx, fy = np.meshgrid(frequency, frequency)
    r = np.hypot(fx, fy)
    keep = r >= low if high is None else (r >= low) & (r <= high)
    amplitude = np.zeros_like(r)
    amplitude[keep] = r[keep] ** -slope
    field = np.real(np.fft.ifft2(amplitude * np.exp(2j * math.pi * rng.random((n, n)))))
    return ((field - field.mean()) / field.std()).astype(np.float32)


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def save_material(folder, name, color, height_m, roughness, occlusion, size_m):
    """Base color (linear in, sRGB out), a normal map from the heights (y up, as glTF has it) and occlusion,
    roughness and metal packed as glTF reads them."""
    n = height_m.shape[0]
    texel = size_m / n
    across = (np.roll(height_m, -1, 1) - np.roll(height_m, 1, 1)) / (2 * texel)
    down = (np.roll(height_m, -1, 0) - np.roll(height_m, 1, 0)) / (2 * texel)
    normal = np.dstack([-across, down, np.ones_like(height_m)])
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    linear = np.clip(color, 0.0, 1.0)
    srgb = np.where(linear <= 0.0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - 0.055)
    packed = np.dstack([occlusion, roughness, np.zeros_like(roughness)])

    def write(suffix, values):
        Image.fromarray((np.clip(values, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)).save(
            os.path.join(folder, f'{name}_{suffix}.png'))
    write('color', srgb)
    write('normal', normal * 0.5 + 0.5)
    write('orm', packed)


def paving_stone(folder, n=2048, size_m=4.0, seed=7):
    """The stone the floor's slabs are cut from: worn, pitted, damp in patches. Each slab takes its own piece of it,
    so its shade wanders widely across the square."""
    broad, middle, fine = noise(n, seed + 1, 1.7), noise(n, seed + 2, 1.2, 6.0), noise(n, seed + 3, 0.6, 40.0)
    warmth = noise(n, seed + 6, 2.0, 1.0, 4.0)
    damp = smoothstep(0.4, 1.5, noise(n, seed + 4, 2.0, 1.0, 5.0))
    pits = smoothstep(2.5, 3.3, noise(n, seed + 5, 0.5, 90.0, 500.0))   # few, and small
    height = 0.0012 * middle + 0.0004 * fine - 0.0025 * pits
    shade = (1.0 + 0.17 * broad + 0.11 * middle + 0.06 * fine) * (1.0 - 0.25 * pits)
    stone = np.dstack([0.205 * shade * (1.0 + 0.035 * warmth), 0.196 * shade, 0.183 * shade * (1.0 - 0.035 * warmth)])
    stone *= (1.0 - 0.3 * damp)[..., None]         # damp stone is darker
    roughness = np.clip(0.86 - 0.4 * damp + 0.05 * middle + 0.1 * pits, 0.25, 1.0)
    save_material(folder, 'floor', stone, height, roughness, 1.0 - 0.4 * pits, size_m)


def plaster(folder, n=2048, size_m=3.0, seed=21):
    """Old grey lime plaster: mottled, stained, evenly worn so it repeats without a seam."""
    broad, middle, fine = noise(n, seed, 1.3, 2.0), noise(n, seed + 1, 1.0, 8.0), noise(n, seed + 2, 0.4, 40.0)
    stain = smoothstep(0.8, 2.2, noise(n, seed + 3, 1.8, 2.0, 8.0))
    shade = (1.0 + 0.045 * broad + 0.05 * middle + 0.05 * fine) * (1.0 - 0.12 * stain)
    color = np.dstack([0.330 * shade, 0.316 * shade, 0.292 * shade])
    height = 0.003 * broad + 0.0012 * middle + 0.0003 * fine
    save_material(folder, 'plaster', color, height, np.clip(0.9 - 0.04 * middle, 0.0, 1.0), np.ones_like(broad), size_m)


def cut_stone(folder, n=1024, size_m=2.0, seed=33):
    """Plain dressed stone for the pilasters, the cornice and the outside."""
    broad, middle, fine = noise(n, seed, 1.7), noise(n, seed + 1, 1.0, 5.0), noise(n, seed + 2, 0.3, 30.0)
    shade = 1.0 + 0.09 * broad + 0.08 * middle + 0.07 * fine
    color = np.dstack([0.270 * shade, 0.262 * shade, 0.246 * shade])
    height = 0.002 * broad + 0.001 * middle + 0.0005 * fine
    save_material(folder, 'stone', color, height, np.clip(0.8 - 0.05 * middle, 0.0, 1.0), np.ones_like(broad), size_m)


# --- Files ----------------------------------------------------------------------------------------------------------

def write_gltf(folder, name, parts, materials):
    """A .gltf with its .bin: a mesh with one primitive per material."""
    blob = bytearray()
    views, accessors, primitives = [], [], []

    def add(values, kind, component, target):
        while len(blob) % 4:
            blob.append(0)
        data = values.tobytes()
        views.append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': len(data), 'target': target})
        blob.extend(data)
        accessor = {'bufferView': len(views) - 1, 'componentType': component, 'count': len(values), 'type': kind}
        if kind == 'VEC3' and target == 34962:
            accessor['min'], accessor['max'] = values.min(axis=0).tolist(), values.max(axis=0).tolist()
        accessors.append(accessor)
        return len(accessors) - 1

    triangles = 0
    materials = [(key, look) for key, look in materials if parts[key].indices]   # a part may hold nothing
    for index, (key, _) in enumerate(materials):
        part = parts[key]
        primitives.append({
            'attributes': {'POSITION': add(np.array(part.positions, np.float32), 'VEC3', 5126, 34962),
                           'NORMAL': add(np.array(part.normals, np.float32), 'VEC3', 5126, 34962),
                           'TEXCOORD_0': add(np.array(part.uvs, np.float32), 'VEC2', 5126, 34962)},
            'indices': add(np.array(part.indices, np.uint32), 'SCALAR', 5125, 34963),
            'material': index})
        triangles += len(part.indices) // 3

    images, textures, gltf_materials = [], [], []

    def texture(file):
        if {'uri': file} not in images:
            images.append({'uri': file})
            textures.append({'source': len(images) - 1, 'sampler': 0})
        return {'index': images.index({'uri': file})}
    for key, look in materials:
        material = {'name': key, 'pbrMetallicRoughness': {'metallicFactor': 0.0,
                                                          'roughnessFactor': look.get('roughness', 1.0)}}
        if 'factor' in look:
            material['pbrMetallicRoughness']['baseColorFactor'] = look['factor'] + [1.0]
        if 'maps' in look:
            color, normal, packed = look['maps']   # packed: occlusion, roughness, metal
            material['pbrMetallicRoughness']['baseColorTexture'] = texture(color)
            material['pbrMetallicRoughness']['metallicRoughnessTexture'] = texture(packed)
            material['pbrMetallicRoughness']['metallicFactor'] = 1.0   # times the texture's
            material['normalTexture'] = texture(normal)
            material['occlusionTexture'] = texture(packed)
        gltf_materials.append(material)

    document = {
        'asset': {'version': '2.0', 'generator': 'tools/worldgen/shrine.py'},
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'name': name, 'mesh': 0}],
        'meshes': [{'name': name, 'primitives': primitives}],
        'materials': gltf_materials, 'textures': textures, 'images': images,
        'samplers': [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497}],
        'buffers': [{'uri': f'{name}.bin', 'byteLength': len(blob)}], 'bufferViews': views, 'accessors': accessors}
    with open(os.path.join(folder, f'{name}.bin'), 'wb') as f:
        f.write(blob)
    with open(os.path.join(folder, f'{name}.gltf'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(document, f, indent=1)
    return triangles


def copy_glb(source, target):
    """Copies a .glb, if the one there isn't it already."""
    if (not os.path.exists(target) or os.path.getsize(target) != os.path.getsize(source)
            or os.path.getmtime(target) < os.path.getmtime(source)):
        shutil.copyfile(source, target)


# --- The scene ------------------------------------------------------------------------------------------------------

def yaw(deg):
    half = math.radians(deg) / 2
    return np.array([0.0, math.sin(half), 0.0, math.cos(half)])


def pitch(deg):
    half = math.radians(deg) / 2
    return np.array([math.sin(half), 0.0, 0.0, math.cos(half)])


def turned(a, b):
    """The rotation b, then a (quaternions x, y, z, w)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz])


def at_wall(theta_deg, r, y=0.0):
    p = cyl(math.radians(theta_deg), r, y)
    return [round(float(c), 4) for c in p]


def on_floor(theta_deg, r, reach=0.1, lift=0.0):
    """As at_wall, resting on the slabs there: on the highest within reach, a few millimeters into it."""
    p = cyl(math.radians(theta_deg), r, 0.0)
    return [round(float(p[0]), 4), round(floor_height(p[0], p[2], reach) - 0.004 + lift, 4), round(float(p[2]), 4)]


def facing_center(theta_deg, turn_deg=0.0):
    """A model's front is its +Z: turned to look at the hall's middle from theta, then by turn_deg more."""
    return yaw(-theta_deg + turn_deg)


def entity(name, mesh=None, position=(0.0, 0.0, 0.0), rotation=None, scale=None, folder=None, collision=False,
           parent='shrine', **more):
    e = {'name': name}
    if folder:
        e['folder'] = folder
    if parent:
        e['parent'] = parent
    if mesh:
        e['mesh'] = mesh
    e['position'] = [round(float(c), 4) for c in position]
    if rotation is not None:
        e['rotation'] = [round(float(c), 6) for c in rotation]
    if scale is not None:
        e['scale'] = scale
    if collision:
        e['collision'] = 'mesh'
    e.update(more)
    return e


def scene_entities():
    rng = np.random.default_rng(11)
    entities = [
        entity('shrine', position=CENTER, parent=None),
        entity('hall', 'hall/hall.gltf', collision=True),
        # Dust in the air, so the daylight through the door shows as a shaft.
        entity('dust', position=(0.0, 2.6, 0.0), scale=[7.0, 2.6, 7.0],
               fog_volume={'shape': 'ellipsoid', 'density_per_m': 0.012, 'edge_m': 2.0}),
        # The hall is under a roof: no fog of its own, only that the sky's ambient doesn't reach its air. A sphere
        # about the dome's own centre that ends inside the dome's shell: all of the dome's inner face and its ribs
        # are well inside it whichever way they face (a rib's side asks about the sky half a meter sideways, which a
        # sphere drawn to the dome's inner face left half open: the ribs shone), and the roof's top is outside it.
        entity('roof', position=(0.0, round(DOME_CENTER_Y, 2), 0.0), scale=[round(DOME_RADIUS + 0.35, 2)] * 3,
               fog_volume={'shape': 'ellipsoid', 'density_per_m': 0, 'edge_m': 0.12, 'under_roof': 1}),
        # The basin stands a hair above the floor under it: its own bottom is a plug just under its origin.
        entity('basin', 'props/basin_empty.glb', (0.0, floor_height(0.0, 0.0, 1.3) + 0.01, 0.0), collision=True),
        entity('basin_water', position=(0.0, 0.25, 0.0), scale=[1.31, 1.0, 1.31],
               water={'shape': 'ellipse', 'waves': 0.0}),
        entity('statue', 'props/statue.glb', on_floor(0.0, 1.95, 0.25), collision=True),
        entity('pillar', 'props/pillar_fallen.glb', on_floor(62.0, 5.9, 1.3, -0.006), yaw(90.0 - 62.0 - 10.0),
               folder='props', collision=True),
        entity('banner', 'props/banner.glb', at_wall(36.0, RADIUS - 0.09, 1.4), facing_center(36.0), folder='props'),
        # Leaning on the wall: point down, its top tipped back against the plaster.
        entity('sword', 'props/sword.glb', on_floor(-10.0, RADIUS - 0.29, 0.0), turned(facing_center(-10.0), pitch(-16.0)),
               folder='props'),
        # The lantern prefab: its frame, and the light of the candle inside it.
        entity('lantern', None, on_floor(-23.0, 6.25, 0.07), facing_center(-23.0, 20.0), folder='props',
               prefab='props/lantern.prefab'),
        entity('crate', 'props/crate.glb', on_floor(-31.0, 6.5, 0.25), facing_center(-31.0, 12.0), folder='props',
               collision=True),
        entity('pot_1', 'props/clay_pot.glb', on_floor(-42.5, 6.6), yaw(40.0), folder='props', collision=True),
        entity('pot_2', 'props/clay_pot.glb', on_floor(-47.0, 6.5), yaw(200.0), 0.85, folder='props', collision=True),
        # One on its side, mouth toward the room.
        entity('pot_3', 'props/clay_pot.glb', on_floor(-52.0, 6.1, 0.2, 0.2), turned(yaw(128.0), pitch(90.0)), 0.9,
               folder='props', collision=True),
        entity('pot_4', 'props/clay_pot.glb', on_floor(-104.0, 6.55), yaw(75.0), 1.1, folder='props', collision=True),
        entity('pot_5', 'props/clay_pot.glb', on_floor(-110.5, 6.6), yaw(310.0), 0.8, folder='props', collision=True),
    ]
    # A floor made elsewhere: its tile, as many times as it takes to reach under the wall.
    if _tile_floor is not None:
        for index, (x, z) in enumerate(_tile_floor.tiles(PAVING_RIM)):
            entities.append(entity(f'floor_{index + 1:02d}', _tile_model, (x, 0.0, z), folder='floor', collision=True))
    # Candles in threes: round the basin on the floor, one group on its rim.
    groups = [(-25.0, 2.0, 0.0), (25.0, 2.0, 0.0), (62.0, 2.1, 0.0), (-75.0, 2.05, 0.0), (-115.0, 2.3, 0.0),
              (135.0, 1.4, 0.3), (165.0, 2.15, 0.0)]
    count = 0
    for theta_deg, r, y in groups:
        theta = math.radians(theta_deg)
        turn = rng.uniform(0.0, 2 * math.pi)
        on_rim = y > 0.0
        for k, size in enumerate((1.0, 0.85, 0.7)):
            if on_rim:   # along the rim: it's narrow
                p = cyl(theta + (k - 1) * 0.13, r + rng.uniform(-0.02, 0.02), y)
            else:
                angle = turn + k * 2 * math.pi / 3
                p = cyl(theta, r, y) + np.array([math.cos(angle), 0.0, math.sin(angle)]) * rng.uniform(0.09, 0.13)
            if not on_rim:
                p[1] = floor_height(p[0], p[2], 0.05) - 0.004
            count += 1
            # Each is the candle prefab (its wax and its flame), placed: one line a candle. The tallest of a group
            # casts the group's shadow; the other two light without one (candle_fill.prefab: a light's shadow is
            # most of what it costs, and three candles together cast nearly one shadow).
            entities.append(entity(f'candle_{count:02d}', None, p, yaw(rng.uniform(0.0, 360.0)),
                                   round(size * rng.uniform(0.92, 1.06), 3), folder='candles',
                                   prefab='props/candle.prefab' if k == 0 else 'props/candle_fill.prefab'))
    # Dust in the air that the daylight catches: specks, where the fog volume above is the haze.
    entities.append(entity('motes', position=(0.0, 2.2, 0.0), effect='effects/dust_motes.vfx'))
    return entities


# What's placed by name above and made by hand, not drawn here: written only where it's missing, and a person's or an
# agent's to change from then on (the effects are in effects/, beside it).
CANDLE_PREFAB = """{
  // A candle: its wax, and at its wick a flame and the light it gives. Unlit, it has neither.
  "parts": {
    "wax":   {"mesh": "candle.glb"},
    "flame": {"effect": "../effects/candle_flame.vfx", "light": {"color": [1, 0.52, 0.2], "intensity": 0.6, "radius_m": 3.5}, "position": [0, 0.196, 0]}
  },
  "variants": {
    "unlit": {"flame": null}
  }
}
"""

LANTERN_PREFAB = """{
  // A lantern: its frame and glass, and the light of the candle inside it. Dark, it has no light.
  "parts": {
    "frame": {"mesh": "lantern.glb"},
    "light": {"light": {"color": [1, 0.6, 0.28], "intensity": 0.9, "radius_m": 5}, "position": [0, 0.16, 0]}
  },
  "variants": {
    "dark": {"light": null}
  }
}
"""


ENVIRONMENT = """{
  // The shrine: a clear afternoon outside, the sun low enough to reach across the floor through the door.
  "sun": {"elevation_deg": %(elevation)s, "azimuth_deg": %(azimuth)s, "color": [1, 0.94, 0.85], "intensity": 5},
  "sky": {"color": [0.34, 0.45, 0.66], "intensity": 1},
  "ground": {"color": [0.11, 0.10, 0.085], "intensity": 1},
  "shadow_color": [0.05, 0.06, 0.09],
  "exposure_ev": 0,
  "shadows": {"resolution": 2048, "distance_m": 60, "split_lambda": 0.75, "blend_band": 0.12,
              "caster_reach_m": 60, "penumbra_per_m": 0.06, "max_penumbra_texels": 6},
  "fog": {
    "density_per_m": 0.000005, "height_density_per_m": 0.0004, "height_falloff_per_m": 0.01, "base_height_m": 0,
    "extinction_tint": [0.8, 1.0, 1.3],
    "sun_forward": {"color": [1, 0.9, 0.78], "intensity": 0.3},
    "sun_back": {"color": [0.5, 0.6, 0.8], "intensity": 0.08},
    "sun_height_attenuation": 1,
    "ambient": {"color": [0.34, 0.45, 0.66], "intensity": 1},
    "ambient_height_attenuation": 1,
    "start_m": 0, "sun_start_m": 0, "end_m": 20000,
    "volume_distance_m": 64
  }
}
"""

WORLD = """{
  // The shrine: one cell, a round hall to judge lighting on (tools/worldgen/shrine.py).
  "environment": "shrine.environment",
  "start": {"position": [%(x)s, 1.7, %(z)s], "yaw_deg": 0, "pitch_deg": -6},
  "cells": ["cell_0_0.cell"]
}
"""


def write_text(path, text):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--props', default=os.path.join(ROOT, 'build', 'agent', 'shrine_scene', '5_models', 'final'),
                        help='the folder holding the props, one .glb each')
    parser.add_argument('--no-textures', action='store_true', help="leave the hall's drawn textures as they are")
    for surface, name in SURFACES.items():
        parser.add_argument(f'--{surface}', default=name, metavar='NAME',
                            help=f"the Poly Haven texture the {surface} wears (default {name}); 'drawn' for noise")
    parser.add_argument('--paving', default='slabs', metavar='NAME',
                        help="the floor: 'slabs', drawn here block by block, or a square tile's model in "
                             "content/shrine/floor/ without its .glb ('flagstone_tile'), laid on a grid")
    args = parser.parse_args()

    global _tile_floor, _tile_model
    if args.paving != 'slabs':
        _tile_model = f'floor/{args.paving}.glb'
        _tile_floor = TileFloor(os.path.join(OUT, 'floor', f'{args.paving}.glb'))
        print(f'floor: {args.paving}, {_tile_floor.size:g} m tiles of {_tile_floor.triangles} triangles, '
              f'{len(_tile_floor.tiles(PAVING_RIM))} of them')

    hall = os.path.join(OUT, 'hall')
    props = os.path.join(OUT, 'props')
    for folder in (hall, props, os.path.join(META, 'hall')):
        os.makedirs(folder, exist_ok=True)

    for name in ('candle', 'clay_pot', 'basin_empty', 'crate', 'lantern', 'statue', 'pillar_fallen', 'sword',
                 'banner'):
        copy_glb(os.path.join(args.props, f'{name}.glb'), os.path.join(props, f'{name}.glb'))
    print('props copied')

    looks, drawn = {}, set()
    for surface in SURFACES:
        name = getattr(args, surface)
        if os.path.isfile(os.path.join(hall, 'polyhaven', name, f'{name}_diff_2k.jpg')):
            base = f'polyhaven/{name}/{name}'
            looks[surface] = {'maps': (f'{base}_diff_2k.jpg', f'{base}_nor_gl_2k.jpg', f'{base}_arm_2k.jpg')}
            TILE[surface] = POLYHAVEN.get(name, 2.0) * STRETCH.get(surface, 1.0)
        elif surface in DRAWN:
            base = DRAWN[surface]
            drawn.add(base)
            looks[surface] = {'maps': (f'{base}_color.png', f'{base}_normal.png', f'{base}_orm.png')}
            TILE[surface] = DRAWN_TILE[base]
        else:
            looks[surface] = {'factor': [0.19, 0.165, 0.125], 'roughness': 0.95}
        print(f'{surface}: {name if "polyhaven" in str(looks[surface]) else "drawn"}, repeats every {TILE[surface]:g} m')
    # The drawn textures: made when something wears them, removed with their metas when nothing does.
    for base, draw in (('floor', paving_stone), ('plaster', plaster), ('stone', cut_stone)):
        if base in drawn:
            if not args.no_textures:
                draw(hall)
        else:
            for suffix in ('color', 'normal', 'orm'):
                for stale in (os.path.join(hall, f'{base}_{suffix}.png'),
                              os.path.join(META, 'hall', f'{base}_{suffix}.png.json')):
                    if os.path.exists(stale):
                        os.remove(stale)
    triangles = write_gltf(hall, 'hall', build_hall(), [
        ('floor', looks['floor']),
        ('joints', {'factor': [0.05, 0.045, 0.037], 'roughness': 0.95}),
        ('plaster', looks['plaster']),
        ('stone', looks['stone']),
        ('outside', looks['outside']),
        ('roof', looks['roof']),
        ('ground', looks['ground'])])
    print(f'hall: {triangles} triangles')
    # One level of detail (the camera is always inside it or beside it), and all of it to collide with.
    write_text(os.path.join(META, 'hall', '_folder.json'),
               '{"rules": [{"match": "hall.gltf", "mesh": {"lod_count": 1, "collision_triangles": 200000}}]}\n')

    candle_prefab = os.path.join(props, 'candle.prefab')
    if not os.path.exists(candle_prefab):
        write_text(candle_prefab, CANDLE_PREFAB)
    candle_fill = os.path.join(props, 'candle_fill.prefab')
    if not os.path.exists(candle_fill):
        write_text(candle_fill, CANDLE_PREFAB.replace('"radius_m": 3.5}', '"radius_m": 3.5, "shadow": 0}'))
    lantern_prefab = os.path.join(props, 'lantern.prefab')
    if not os.path.exists(lantern_prefab):
        write_text(lantern_prefab, LANTERN_PREFAB)
    for effect in ('candle_flame', 'dust_motes'):
        if not os.path.exists(os.path.join(OUT, 'effects', f'{effect}.vfx')):
            print(f'effects/{effect}.vfx is missing: the cell names it, and the cooker will say so')

    cell = {'coord': [0, 0], 'entities': scene_entities()}
    lines = ',\n'.join('    ' + json.dumps(e) for e in cell['entities'])
    write_text(os.path.join(OUT, 'cell_0_0.cell'), '{\n  "coord": [0, 0],\n  "entities": [\n' + lines + '\n  ]\n}\n')
    write_text(os.path.join(OUT, 'shrine.environment'),
               ENVIRONMENT % {'elevation': SUN_ELEVATION_DEG, 'azimuth': SUN_AZIMUTH_DEG})
    write_text(os.path.join(OUT, 'shrine.world'), WORLD % {'x': CENTER[0], 'z': CENTER[2] + 6.2})

    def view(name, from_deg, r, y, toward_deg, pitch_deg):
        """Standing r from the middle toward from_deg, looking toward toward_deg (a camera's yaw is minus that)."""
        p = cyl(math.radians(from_deg), r, y)
        return f'{name} {CENTER[0] + p[0]:.3f} {y:.3f} {CENTER[2] + p[2]:.3f} {-toward_deg:g} {pitch_deg:g}\n'
    door_deg = math.degrees(DOOR_AT)
    write_text(os.path.join(OUT, 'shrine.views'),
               '# The shrine: name, x, y, z, yaw and pitch in degrees (tools/worldgen/shrine.py).\n'
               + view('front', 180.0, 6.2, 1.7, 0.0, -6)                      # the concept's: from the south wall
               + view('door', door_deg + 180.0, 5.6, 1.7, door_deg, -4)       # across the room, into the daylight
               + view('bounce', door_deg - 30.0, 5.4, 1.7, door_deg + 170.0, -4)  # the side no sunlight reaches
               + view('statue', 170.0, 1.2, 1.2, 0.0, -8)                     # across the basin
               + view('candles', 160.0, 3.6, 0.5, -10.0, -10)                 # low, by the near candles
               + view('floor', 215.0, 4.0, 0.6, 305.0, -10)                   # low, across the slabs in the sun
               + view('lantern', -23.0, 5.45, 0.28, -23.0, 0)                 # close, through the lantern's glass
               + view('dome', 180.0, 5.0, 1.2, 0.0, 40)                       # up into the dome
               + view('top', 180.0, 0.01, 8.2, 0.0, -88)                    # straight down from under the ring
               + view('outside', door_deg, 20.0, 2.0, door_deg + 180.0, -2))  # the door, from outside
    print(f'wrote {os.path.normpath(OUT)}')


if __name__ == '__main__':
    main()
