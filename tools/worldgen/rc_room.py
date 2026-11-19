"""The radiance cascades room: a test world for screen-space radiance cascades (docs/design.md, Global illumination,
Other kinds of game), the GI meant for top-down action games. A rectangular stone hall, 20 x 12 m inside and open to a
night sky, seen from above as such a game sees it. Its floor and walls wear Poly Haven textures (CC0, polyhaven.com:
cobblestone_floor_06 and castle_wall_slates, in rc_room/polyhaven/). Four pillars, a red rug (to bleed its colour), the
shrine's props (shrine/props: a statue, banners, crates, pots, a fallen pillar, a basin, swords and candles
with their lights), a glowing blue rune on the floor and a glowing crystal with no light at all (only GI can make them
light what's round them), and a doorway onto the dark outside.

    python tools/worldgen/rc_room.py

Writes projects/dreadworld/content/reference/rc_room/, which isn't committed. Open it in the editor with
`editor.open_world reference/rc_room/rc_room`.
"""
import base64
import json
import math
import os
import struct

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
OUT = os.path.join(ROOT, 'projects', 'dreadworld', 'content', 'reference', 'rc_room')

# Where the room's middle stands in the world.
CENTRE = (64.0, 0.0, 64.0)

# The hall, inside: x from -HALF_X to HALF_X, z from -HALF_Z to HALF_Z.
HALF_X = 10.0
HALF_Z = 6.0
WALL_HEIGHT_M = 4.0
WALL_THICK_M = 0.4
DOOR_HALF_M = 1.25  # the doorway in the east wall (+x), round z = 0

# Materials: linear base colour, roughness, metalness, and emissive (linear, times its strength). A textured one names
# its Poly Haven maps (<base>_diff_2k.jpg, _nor_gl_2k.jpg, _arm_2k.jpg) and how wide one repeat of them is, in meters:
# the faces' uvs are laid out in meters from their positions.
STONE_FLOOR = {'color': [1, 1, 1], 'roughness': 1.0, 'texture': 'polyhaven/cobblestone_floor_06/cobblestone_floor_06',
               'tile_m': 2.0}
STONE_WALL = {'color': [1, 1, 1], 'roughness': 1.0, 'texture': 'polyhaven/castle_wall_slates/castle_wall_slates',
              'tile_m': 2.5}
PILLAR = STONE_WALL
RUG = {'color': [0.60, 0.05, 0.04], 'roughness': 1.0}
IRON = {'color': [0.20, 0.20, 0.21], 'roughness': 0.5, 'metalness': 1.0}
RUNE = {'color': [0.05, 0.05, 0.05], 'roughness': 1.0, 'emissive': [0.15, 0.45, 1.0], 'strength': 1.5}
CRYSTAL = {'color': [0.30, 0.10, 0.40], 'roughness': 0.3, 'emissive': [0.70, 0.20, 1.0], 'strength': 2.0}


def quad(corners, normal):
    """Two triangles over four corners, counter-clockwise seen from the side the normal points to."""
    return {'positions': corners, 'normal': normal, 'indices': [0, 1, 2, 0, 2, 3]}


def cuboid(lo, hi, skip=()):
    """An axis-aligned box from corner lo to corner hi, its faces outward. skip: faces left out ('-y', '+x'...)."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    faces = {
        '+y': quad([[x0, y1, z1], [x1, y1, z1], [x1, y1, z0], [x0, y1, z0]], [0, 1, 0]),
        '-y': quad([[x0, y0, z0], [x1, y0, z0], [x1, y0, z1], [x0, y0, z1]], [0, -1, 0]),
        '+z': quad([[x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]], [0, 0, 1]),
        '-z': quad([[x1, y0, z0], [x0, y0, z0], [x0, y1, z0], [x1, y1, z0]], [0, 0, -1]),
        '+x': quad([[x1, y0, z1], [x1, y0, z0], [x1, y1, z0], [x1, y1, z1]], [1, 0, 0]),
        '-x': quad([[x0, y0, z0], [x0, y0, z1], [x0, y1, z1], [x0, y1, z0]], [-1, 0, 0]),
    }
    return [face for key, face in faces.items() if key not in skip]


def prism(radius, y0, y1, sides, top=True, bottom=False):
    """An upright prism round the y axis, flat-sided, its faces outward."""
    faces = []
    ring = [(radius * math.cos(2 * math.pi * (i + 0.5) / sides), radius * math.sin(2 * math.pi * (i + 0.5) / sides))
            for i in range(sides)]
    for i in range(sides):
        ax, az = ring[i]
        bx, bz = ring[(i + 1) % sides]
        mid = 2 * math.pi * (i + 1) / sides
        normal = [math.cos(mid), 0.0, math.sin(mid)]
        faces.append(quad([[bx, y0, bz], [ax, y0, az], [ax, y1, az], [bx, y1, bz]], normal))
    for cap, y, normal in ((top, y1, [0, 1, 0]), (bottom, y0, [0, -1, 0])):
        if not cap:
            continue
        for i in range(sides):
            ax, az = ring[i]
            bx, bz = ring[(i + 1) % sides]
            tri = [[0, y, 0], [bx, y, bz], [ax, y, az]] if normal[1] > 0 else [[0, y, 0], [ax, y, az], [bx, y, bz]]
            faces.append({'positions': tri, 'normal': normal, 'indices': [0, 1, 2]})
    return faces


def write_gltf(path, parts, name):
    """parts: (material, faces). One mesh, a primitive a part, its buffer in the file."""
    blob = bytearray()
    views, accessors, primitives, materials = [], [], [], []
    images, textures = [], []
    extensions_used = set()

    def texture(uri):
        images.append({'uri': uri})
        textures.append({'source': len(images) - 1, 'sampler': 0})
        return {'index': len(textures) - 1}

    def add(data, target, count, kind, minmax=None):
        while len(blob) % 4:
            blob.append(0)
        offset = len(blob)
        blob.extend(data)
        views.append({'buffer': 0, 'byteOffset': offset, 'byteLength': len(data), 'target': target})
        accessor = {'bufferView': len(views) - 1, 'componentType': 5126 if kind != 'SCALAR' else 5125,
                    'count': count, 'type': kind}
        if minmax:
            accessor['min'], accessor['max'] = minmax
        accessors.append(accessor)
        return len(accessors) - 1

    for index, (material, faces) in enumerate(parts):
        positions, normals, uvs, indices = [], [], [], []
        for face in faces:
            first = len(positions)
            corners = face['positions']
            positions += corners
            normals += [face['normal']] * len(corners)
            if 'tile_m' in material:  # in meters along the face, a repeat of the texture each tile_m
                n = face['normal']
                axis = max(range(3), key=lambda k: abs(n[k]))
                for c in corners:
                    if axis == 1:
                        uvs.append([c[0] / material['tile_m'], c[2] / material['tile_m']])
                    else:
                        across = c[2] * (1 if n[0] > 0 else -1) if axis == 0 else c[0] * (1 if n[2] < 0 else -1)
                        uvs.append([-across / material['tile_m'], -c[1] / material['tile_m']])
            else:
                uvs += [[0, 0], [1, 0], [1, 1], [0, 1]][:len(corners)]
            indices += [first + i for i in face['indices']]
        lo = [min(p[k] for p in positions) for k in range(3)]
        hi = [max(p[k] for p in positions) for k in range(3)]
        p = add(b''.join(struct.pack('<3f', *v) for v in positions), 34962, len(positions), 'VEC3', (lo, hi))
        n = add(b''.join(struct.pack('<3f', *v) for v in normals), 34962, len(normals), 'VEC3')
        t = add(b''.join(struct.pack('<2f', *v) for v in uvs), 34962, len(uvs), 'VEC2')
        i = add(b''.join(struct.pack('<I', v) for v in indices), 34963, len(indices), 'SCALAR')
        primitives.append({'attributes': {'POSITION': p, 'NORMAL': n, 'TEXCOORD_0': t}, 'indices': i,
                           'material': index})
        out = {'name': f'{name}_{index}',
               'pbrMetallicRoughness': {'baseColorFactor': material['color'] + [1.0],
                                        'metallicFactor': material.get('metalness', 0.0),
                                        'roughnessFactor': material['roughness']}}
        if 'texture' in material:
            base = material['texture']
            out['pbrMetallicRoughness']['baseColorTexture'] = texture(f'{base}_diff_2k.jpg')
            packed = texture(f'{base}_arm_2k.jpg')
            out['pbrMetallicRoughness']['metallicRoughnessTexture'] = packed
            out['pbrMetallicRoughness']['metallicFactor'] = 1.0  # times the texture's, which is 0
            out['occlusionTexture'] = dict(packed)
            out['normalTexture'] = texture(f'{base}_nor_gl_2k.jpg')
        if 'emissive' in material:
            out['emissiveFactor'] = material['emissive']
            out['extensions'] = {'KHR_materials_emissive_strength': {'emissiveStrength': material['strength']}}
            extensions_used.add('KHR_materials_emissive_strength')
        materials.append(out)
    gltf = {
        'asset': {'version': '2.0', 'generator': 'tools/worldgen/rc_room.py'},
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0, 'name': name}],
        'meshes': [{'name': name, 'primitives': primitives}],
        'materials': materials,
        'buffers': [{'byteLength': len(blob),
                     'uri': 'data:application/octet-stream;base64,' + base64.b64encode(bytes(blob)).decode()}],
        'bufferViews': views, 'accessors': accessors,
    }
    if images:
        gltf['images'], gltf['textures'] = images, textures
        gltf['samplers'] = [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497}]
    if extensions_used:
        gltf['extensionsUsed'] = sorted(extensions_used)
    with open(os.path.join(OUT, path), 'w', encoding='utf-8') as f:
        json.dump(gltf, f)


def room():
    """The floor (a little past the walls) and four walls, each a solid slab; the east wall split by the doorway."""
    t = WALL_THICK_M
    h = WALL_HEIGHT_M
    floor = cuboid([-HALF_X - t, -0.2, -HALF_Z - t], [HALF_X + t, 0.0, HALF_Z + t], skip=('-y',))
    walls = []
    walls += cuboid([-HALF_X - t, 0, -HALF_Z - t], [HALF_X + t, h, -HALF_Z], skip=('-y',))  # north
    walls += cuboid([-HALF_X - t, 0, HALF_Z], [HALF_X + t, h, HALF_Z + t], skip=('-y',))  # south
    walls += cuboid([-HALF_X - t, 0, -HALF_Z], [-HALF_X, h, HALF_Z], skip=('-y', '-z', '+z'))  # west
    walls += cuboid([HALF_X, 0, -HALF_Z], [HALF_X + t, h, -DOOR_HALF_M], skip=('-y', '-z'))  # east, north of the door
    walls += cuboid([HALF_X, 0, DOOR_HALF_M], [HALF_X + t, h, HALF_Z], skip=('-y', '+z'))  # east, south of it
    # The ground outside the door, so the doorway opens onto something.
    outside = cuboid([HALF_X + t, -0.2, -4.0], [HALF_X + t + 8.0, 0.0, 4.0], skip=('-y',))
    write_gltf('room.gltf', [(STONE_FLOOR, floor + outside), (STONE_WALL, walls)], 'room')


def props():
    write_gltf('pillar.gltf', [(PILLAR, prism(0.32, 0.0, 3.6, 8) +
                                cuboid([-0.45, 0.0, -0.45], [0.45, 0.3, 0.45], skip=('-y',)))], 'pillar')
    write_gltf('rug.gltf', [(RUG, cuboid([-2.0, 0.0, -1.25], [2.0, 0.02, 1.25], skip=('-y',)))], 'rug')
    # The rune: a ring of 16 glowing tiles flush on the floor, 1.2 m across.
    rune = []
    for i in range(16):
        a = 2 * math.pi * i / 16
        cx, cz = 1.2 * math.cos(a), 1.2 * math.sin(a)
        rune += cuboid([cx - 0.12, 0.0, cz - 0.12], [cx + 0.12, 0.015, cz + 0.12], skip=('-y',))
    write_gltf('rune.gltf', [(RUNE, rune)], 'rune')
    write_gltf('crystal.gltf', [(IRON, prism(0.35, 0.0, 0.25, 6)), (CRYSTAL, prism(0.16, 0.25, 1.35, 5))], 'crystal')


SHRINE = '../../shrine/props/'  # the shrine's props, relative to this world's folder


def turn(yaw):
    """A turn about y, as a quaternion (x, y, z, w)."""
    half = math.radians(yaw) / 2
    return [0.0, round(math.sin(half), 6), 0.0, round(math.cos(half), 6)]


def entity(name, mesh, x, z, y=0.0, yaw=0.0, scale=1.0, folder=None):
    record = {'name': name, 'parent': 'rc_room', 'mesh': mesh, 'position': [x, y, z], 'collision': 'mesh'}
    if folder:
        record['folder'] = folder
    if yaw:
        record['rotation'] = turn(yaw)
    if scale != 1.0:
        record['scale'] = scale
    return record


def placed(name, prefab, x, z, y=0.0, yaw=0.0, folder=None):
    record = {'name': name, 'parent': 'rc_room', 'prefab': SHRINE + prefab, 'position': [x, y, z]}
    if folder:
        record['folder'] = folder
    if yaw:
        record['rotation'] = turn(yaw)
    return record


def cell():
    entities = [{'name': 'rc_room', 'position': list(CENTRE)}, entity('room', 'room.gltf', 0, 0)]
    for i, (x, z) in enumerate(((-5.0, -2.8), (5.0, -2.8), (-5.0, 2.8), (5.0, 2.8))):
        entities.append(entity(f'pillar_{i}', 'pillar.gltf', x, z, folder='pillars'))
    entities.append(entity('rug', 'rug.gltf', 0.0, 2.2))
    # The statue against the north wall between two banners, candles at its feet.
    entities.append(entity('statue', SHRINE + 'statue.glb', 0.0, -4.9, scale=2.0))
    for i, x in enumerate((-2.2, 2.2)):
        entities.append(entity(f'banner_{i}', SHRINE + 'banner.glb', x, -5.92, y=0.9, scale=1.2, folder='banners'))
    for i, (x, z) in enumerate(((-0.6, -3.7), (0.0, -3.5), (0.6, -3.7))):
        entities.append(placed(f'candle_{i}', 'candle.prefab', x, z, folder='candles'))
    # The basin in the middle of the hall.
    entities.append(entity('basin', SHRINE + 'basin_empty.glb', 0.0, -0.9, scale=0.8))
    # A candle at each pillar's foot, on the side toward the middle.
    for i, (x, z) in enumerate(((-4.4, -2.3), (4.4, -2.3), (-4.4, 2.3), (4.4, 2.3))):
        entities.append(placed(f'pillar_candle_{i}', 'candle.prefab', x, z, folder='pillar_candles'))
    # Crates stacked in the north-east corner.
    crates = ((8.9, -5.3, 0.0, 10.0), (7.8, -5.4, 0.0, -8.0), (8.8, -4.3, 0.0, 85.0), (8.8, -5.2, 0.62, 25.0))
    for i, (x, z, y, yaw) in enumerate(crates):
        entities.append(entity(f'crate_{i}', SHRINE + 'crate.glb', x, z, y=y, yaw=yaw, scale=2.0, folder='crates'))
    # Pots in the south-west corner.
    pots = ((-9.2, 5.2, 0.0, 1.3), (-8.5, 5.4, 40.0, 1.1), (-9.3, 4.4, 75.0, 1.2), (-7.8, 5.3, 120.0, 0.9))
    for i, (x, z, yaw, scale) in enumerate(pots):
        entities.append(entity(f'pot_{i}', SHRINE + 'clay_pot.glb', x, z, yaw=yaw, scale=scale, folder='pots'))
    # A fallen pillar across the west end, and swords by the south wall.
    entities.append(entity('pillar_fallen', SHRINE + 'pillar_fallen.glb', -6.8, 0.4, yaw=70.0, scale=1.3))
    for i, (x, z, yaw) in enumerate(((6.6, 5.6, 0.0), (7.2, 5.6, 15.0), (7.8, 5.6, -10.0))):
        entities.append(entity(f'sword_{i}', SHRINE + 'sword.glb', x, z, yaw=yaw, folder='swords'))
    # What glows and has no light: only GI makes them light what's round them.
    entities.append(entity('rune', 'rune.gltf', 3.4, 4.0))
    entities.append(entity('crystal', 'crystal.gltf', -8.6, -4.6))
    with open(os.path.join(OUT, 'cell_0_0.cell'), 'w', encoding='utf-8') as f:
        f.write('{\n  "coord": [0, 0],\n  "entities": [\n')
        f.write(',\n'.join('    ' + json.dumps(e) for e in entities))
        f.write('\n  ]\n}\n')


def world():
    with open(os.path.join(OUT, 'rc_room.environment'), 'w', encoding='utf-8') as f:
        f.write('{\n  // The radiance cascades room: a dark night, so what lights the hall is its own fires and glows.\n'
                '  "sun": {"elevation_deg": 50.0, "azimuth_deg": 30.0, "color": [0.6, 0.7, 1.0], "intensity": 0},\n'
                '  "sky": {"color": [0.35, 0.45, 0.8], "intensity": 0.004},\n'
                '  "ground": {"color": [0.3, 0.3, 0.3], "intensity": 0.001},\n'
                '  "shadow_color": [0, 0, 0],\n'
                '  // A dark place stays dark: the exposure starts where the candle-lit floor reads right and follows the\n'
                '  // scene only a little either way, so the hall looks the same from near and from far.\n'
                '  "exposure_ev": 3.0,\n'
                '  "auto_exposure": {"below_ev": 1.5, "above_ev": 1.0},\n'
                '  "shadows": {"resolution": 2048, "distance_m": 40, "split_lambda": 0.75, "blend_band": 0.12,\n'
                '              "caster_reach_m": 20, "penumbra_per_m": 0.06, "max_penumbra_texels": 6},\n'
                '  "fog": {\n'
                '    "density_per_m": 0, "height_density_per_m": 0, "height_falloff_per_m": 0.01,\n'
                '    "base_height_m": 0, "extinction_tint": [1, 1, 1],\n'
                '    "sun_forward": {"color": [1, 1, 1], "intensity": 0},\n'
                '    "sun_back": {"color": [1, 1, 1], "intensity": 0},\n'
                '    "sun_height_attenuation": 1,\n'
                '    "ambient": {"color": [0, 0, 0], "intensity": 0},\n'
                '    "ambient_height_attenuation": 1,\n'
                '    "start_m": 0, "sun_start_m": 0, "end_m": 1000,\n'
                '    "volume_distance_m": 0\n'
                '  }\n}\n')
    x, y, z = CENTRE
    with open(os.path.join(OUT, 'rc_room.world'), 'w', encoding='utf-8') as f:
        f.write('{\n  // The radiance cascades room (tools/worldgen/rc_room.py), seen from above as a top-down game sees it.\n'
                '  "environment": "rc_room.environment",\n'
                f'  "start": {{"position": [{x}, {y + 13.0}, {z + 11.0}], "yaw_deg": 0, "pitch_deg": -50}},\n'
                '  "cells": ["cell_0_0.cell"]\n}\n')


def main():
    os.makedirs(OUT, exist_ok=True)
    room()
    props()
    cell()
    world()
    print('wrote', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
