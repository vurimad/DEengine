"""The Cornell box: a test world with a known answer, to check light that bounces against a path tracer rather than by
eye. A box 2 m inside, open toward the camera: a white floor, ceiling and back wall, a red wall on the left and a green
one on the right, a short and a tall white block turned 17 degrees, and one white light under the ceiling. No sun, no
sky: every bit of light but the first comes off the walls. The colours are the box's usual reflectances, as linear
values (Goral et al. 1984, as the Cornell program's own data gives them, rounded).

    python tools/worldgen/cornell.py

Writes projects/dreadworld/content/reference/cornell/, which isn't committed. The same files make the Blender reference
(build/agent/reference, run with the world's folder).
"""
import base64
import json
import math
import os
import struct

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
OUT = os.path.join(ROOT, 'projects', 'dreadworld', 'content', 'reference', 'cornell')

# Linear reflectance.
WHITE = [0.73, 0.73, 0.73]
RED = [0.63, 0.065, 0.05]
GREEN = [0.14, 0.45, 0.091]

# The light: what it gives a surface facing it from 1 m (the engine's units; the Blender reference turns it into
# watts), just under the ceiling's middle.
LIGHT_INTENSITY = 1.5
LIGHT_HEIGHT_M = 1.75


def quad(corners, normal):
    """Two triangles over four corners, counter-clockwise seen from the side the normal points to."""
    return {'positions': corners, 'normal': normal, 'indices': [0, 1, 2, 0, 2, 3]}


def box_faces(cx, cz, width, depth, height, turn_deg):
    """A block standing on the floor, turned about its middle: its four sides and its top, facing out."""
    turn = math.radians(turn_deg)
    c, s = math.cos(turn), math.sin(turn)

    def at(x, y, z):
        return [cx + x * c + z * s, y, cz - x * s + z * c]

    def turned(n):
        return [n[0] * c + n[2] * s, n[1], -n[0] * s + n[2] * c]

    hw, hd = width / 2, depth / 2
    return [
        quad([at(-hw, height, hd), at(hw, height, hd), at(hw, height, -hd), at(-hw, height, -hd)], [0, 1, 0]),
        quad([at(-hw, 0, hd), at(hw, 0, hd), at(hw, height, hd), at(-hw, height, hd)], turned([0, 0, 1])),
        quad([at(hw, 0, -hd), at(-hw, 0, -hd), at(-hw, height, -hd), at(hw, height, -hd)], turned([0, 0, -1])),
        quad([at(hw, 0, hd), at(hw, 0, -hd), at(hw, height, -hd), at(hw, height, hd)], turned([1, 0, 0])),
        quad([at(-hw, 0, -hd), at(-hw, 0, hd), at(-hw, height, hd), at(-hw, height, -hd)], turned([-1, 0, 0])),
    ]


def write_gltf(path, parts):
    """parts: (material colour, faces). One mesh, a primitive a part, its buffer in the file."""
    blob = bytearray()
    views, accessors, primitives, materials = [], [], [], []

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

    for index, (colour, faces) in enumerate(parts):
        positions, normals, uvs, indices = [], [], [], []
        for face in faces:
            first = len(positions)
            positions += face['positions']
            normals += [face['normal']] * 4
            uvs += [[0, 0], [1, 0], [1, 1], [0, 1]]
            indices += [first + i for i in face['indices']]
        lo = [min(p[k] for p in positions) for k in range(3)]
        hi = [max(p[k] for p in positions) for k in range(3)]
        p = add(b''.join(struct.pack('<3f', *v) for v in positions), 34962, len(positions), 'VEC3', (lo, hi))
        n = add(b''.join(struct.pack('<3f', *v) for v in normals), 34962, len(normals), 'VEC3')
        t = add(b''.join(struct.pack('<2f', *v) for v in uvs), 34962, len(uvs), 'VEC2')
        i = add(b''.join(struct.pack('<I', v) for v in indices), 34963, len(indices), 'SCALAR')
        primitives.append({'attributes': {'POSITION': p, 'NORMAL': n, 'TEXCOORD_0': t}, 'indices': i,
                           'material': index})
        materials.append({'name': f'surface_{index}',
                          'pbrMetallicRoughness': {'baseColorFactor': colour + [1.0], 'metallicFactor': 0.0,
                                                   'roughnessFactor': 1.0}})
    gltf = {
        'asset': {'version': '2.0', 'generator': 'tools/worldgen/cornell.py'},
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0, 'name': 'box'}],
        'meshes': [{'name': 'box', 'primitives': primitives}],
        'materials': materials,
        'buffers': [{'byteLength': len(blob),
                     'uri': 'data:application/octet-stream;base64,' + base64.b64encode(bytes(blob)).decode()}],
        'bufferViews': views, 'accessors': accessors,
    }
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(gltf, f)


def main():
    os.makedirs(OUT, exist_ok=True)
    # The room: x and z from -1 to 1, y from 0 to 2; every face toward the inside; no front.
    floor = quad([[-1, 0, 1], [1, 0, 1], [1, 0, -1], [-1, 0, -1]], [0, 1, 0])
    ceiling = quad([[-1, 2, -1], [1, 2, -1], [1, 2, 1], [-1, 2, 1]], [0, -1, 0])
    back = quad([[-1, 0, -1], [1, 0, -1], [1, 2, -1], [-1, 2, -1]], [0, 0, 1])
    left = quad([[-1, 0, 1], [-1, 0, -1], [-1, 2, -1], [-1, 2, 1]], [1, 0, 0])
    right = quad([[1, 0, -1], [1, 0, 1], [1, 2, 1], [1, 2, -1]], [-1, 0, 0])
    short_block = box_faces(0.33, 0.37, 0.6, 0.6, 0.6, -17.0)
    tall_block = box_faces(-0.35, -0.3, 0.6, 0.6, 1.2, 17.0)
    write_gltf(os.path.join(OUT, 'box.gltf'),
               [(WHITE, [floor, ceiling, back] + short_block + tall_block), (RED, [left]), (GREEN, [right])])

    with open(os.path.join(OUT, 'cell_0_0.cell'), 'w', encoding='utf-8') as f:
        f.write('{\n  "coord": [0, 0],\n  "entities": [\n')
        f.write('    {"name": "cornell", "position": [64.0, 0.0, 64.0]},\n')
        f.write('    {"name": "box", "parent": "cornell", "mesh": "box.gltf", "collision": "mesh"},\n')
        f.write(f'    {{"name": "light", "parent": "cornell", "position": [0.0, {LIGHT_HEIGHT_M}, 0.0], '
                f'"light": {{"color": [1, 1, 1], "intensity": {LIGHT_INTENSITY}, "radius_m": 30}}}}\n')
        f.write('  ]\n}\n')
    with open(os.path.join(OUT, 'cornell.environment'), 'w', encoding='utf-8') as f:
        f.write('{\n  // The Cornell box: no sun and no sky, only its own light.\n'
                '  "sun": {"elevation_deg": 45.0, "azimuth_deg": 0.0, "color": [1, 1, 1], "intensity": 0},\n'
                '  "sky": {"color": [0, 0, 0], "intensity": 0},\n'
                '  "ground": {"color": [0, 0, 0], "intensity": 0},\n'
                '  "shadow_color": [0, 0, 0],\n'
                '  "exposure_ev": 0,\n'
                '  "shadows": {"resolution": 2048, "distance_m": 20, "split_lambda": 0.75, "blend_band": 0.12,\n'
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
    with open(os.path.join(OUT, 'cornell.world'), 'w', encoding='utf-8') as f:
        f.write('{\n  // The Cornell box (tools/worldgen/cornell.py): a test with a known answer for light that bounces.\n'
                '  "environment": "cornell.environment",\n'
                '  "start": {"position": [64.0, 1.0, 67.6], "yaw_deg": 0, "pitch_deg": 0},\n'
                '  "cells": ["cell_0_0.cell"]\n}\n')
    print('wrote', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
