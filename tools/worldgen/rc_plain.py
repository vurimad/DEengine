"""The radiance cascades' measuring room: the rc_room's hall (tools/worldgen/rc_room.py) in plain colours, with no
textures, normal maps or occlusion maps and nothing that flickers, so the rasterized light can be set against the
path tracer's (r.compare) and differ only by the light itself (docs/design.md, Global illumination: radiance
cascades). The hall on wide ground, its four pillars, the rug, the rune and the crystal, and a glowing block by
the west wall.

Three worlds over the one cell, each a different light:
  rc_plain_sky    a sky the same from every way and no sun: what a surface gets is how much sky it sees
  rc_plain_sun    a low sun through the open top and a dim sky: lit patches that bounce onto the rest
  rc_plain_night  no sky and no sun: only what glows lights the hall
And rc_wall_sky: the ground and one long wall under the even sky, whose answer is known on paper.
And rc_stress_sun: the heaviest picture the cascades can be given, to time them by, not to compare: the whole screen
covered by blocks of every height half a meter apart, every seventh one glowing, under the low sun.

    python tools/worldgen/rc_plain.py

Writes projects/dreadworld/content/reference/rc_plain/, which isn't committed.
"""
import json
import os

import rc_room

OUT = os.path.join(rc_room.ROOT, 'projects', 'dreadworld', 'content', 'reference', 'rc_plain')

FLOOR = {'color': [0.45, 0.45, 0.45], 'roughness': 1.0}
WALL = {'color': [0.60, 0.55, 0.45], 'roughness': 1.0}
PILLAR = {'color': [0.70, 0.70, 0.70], 'roughness': 1.0}
GLOW = {'color': [0.05, 0.05, 0.05], 'roughness': 1.0, 'emissive': [1.0, 0.55, 0.15], 'strength': 4.0}

ENVIRONMENTS = {
    'sky': ('A sky the same from every way and no sun.', 0.0, [1, 1, 1], 1.0, [1, 1, 1], 1.0, 0.0),
    'sun': ('A low sun through the open top and a dim sky.', 6.0, [0.5, 0.6, 1.0], 0.15, [0.3, 0.3, 0.3], 0.05, -1.0),
    'night': ('No sky and no sun: only what glows.', 0.0, [0, 0, 0], 0.0, [0, 0, 0], 0.0, 1.0),
}


def environment(name):
    note, sun, sky, sky_intensity, ground, ground_intensity, exposure_ev = ENVIRONMENTS[name]
    return (
        '{\n'
        f'  // The radiance cascades\' measuring room (tools/worldgen/rc_plain.py). {note}\n'
        f'  "sun": {{"elevation_deg": 38.0, "azimuth_deg": 215.0, "color": [1.0, 0.95, 0.85], "intensity": {sun}}},\n'
        f'  "sky": {{"color": {json.dumps(sky)}, "intensity": {sky_intensity}}},\n'
        f'  "ground": {{"color": {json.dumps(ground)}, "intensity": {ground_intensity}}},\n'
        '  "shadow_color": [0, 0, 0],\n'
        f'  "exposure_ev": {exposure_ev},\n'
        '  "auto_exposure": {"below_ev": 0.0, "above_ev": 0.0},\n'
        '  "shadows": {"resolution": 4096, "distance_m": 40, "split_lambda": 0.75, "blend_band": 0.12,\n'
        '              "caster_reach_m": 30, "penumbra_per_m": 0.0, "max_penumbra_texels": 1},\n'
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


def main():
    os.makedirs(OUT, exist_ok=True)
    # The hall's own meshes, in plain colours, written here.
    rc_room.OUT = OUT
    rc_room.STONE_FLOOR = FLOOR
    rc_room.STONE_WALL = WALL
    rc_room.PILLAR = PILLAR
    rc_room.room()
    rc_room.props()
    rc_room.write_gltf('glow.gltf', [(GLOW, rc_room.cuboid([-0.4, 0.0, -0.4], [0.4, 0.8, 0.4], skip=('-y',)))], 'glow')
    # The ground the hall stands on, well past its walls: nothing hangs in a void.
    rc_room.write_gltf('ground.gltf', [(FLOOR, rc_room.cuboid([-60.0, -0.25, -60.0], [60.0, -0.01, 60.0], skip=('-y',)))],
                       'ground')

    entity = rc_room.entity
    entities = [{'name': 'rc_room', 'position': list(rc_room.CENTRE)}, entity('room', 'room.gltf', 0, 0)]
    for i, (x, z) in enumerate(((-5.0, -2.8), (5.0, -2.8), (-5.0, 2.8), (5.0, 2.8))):
        entities.append(entity(f'pillar_{i}', 'pillar.gltf', x, z, folder='pillars'))
    entities.append(entity('rug', 'rug.gltf', 0.0, 2.2))
    entities.append(entity('rune', 'rune.gltf', 3.4, 4.0))
    entities.append(entity('crystal', 'crystal.gltf', -8.6, -4.6))
    entities.append(entity('glow', 'glow.gltf', -8.0, 2.0))
    entities.append(entity('ground', 'ground.gltf', 0.0, 0.0))
    with open(os.path.join(OUT, 'cell_0_0.cell'), 'w', encoding='utf-8') as f:
        f.write('{\n  "coord": [0, 0],\n  "entities": [\n')
        f.write(',\n'.join('    ' + json.dumps(e) for e in entities))
        f.write('\n  ]\n}\n')

    # The simplest case there is, to check against what's known on paper: the ground and one long wall across it,
    # under the even sky. The wall's face sees half the sky; the ground at its foot, less the closer it is.
    wall = rc_room.cuboid([-30.0, 0.0, -6.4], [30.0, 4.0, -6.0], skip=('-y',))
    rc_room.write_gltf('wall.gltf', [(WALL, wall)], 'wall')
    lone = [{'name': 'rc_room', 'position': list(rc_room.CENTRE)}, entity('ground', 'ground.gltf', 0.0, 0.0),
            entity('wall', 'wall.gltf', 0.0, 0.0)]
    with open(os.path.join(OUT, 'wall_0_0.cell'), 'w', encoding='utf-8') as f:
        f.write('{\n  "coord": [0, 0],\n  "entities": [\n')
        f.write(',\n'.join('    ' + json.dumps(e) for e in lone))
        f.write('\n  ]\n}\n')
    x, y, z = rc_room.CENTRE
    with open(os.path.join(OUT, 'rc_wall_sky.world'), 'w', encoding='utf-8') as f:
        f.write('{\n  // One wall on the ground under an even sky (tools/worldgen/rc_plain.py).\n'
                '  "environment": "rc_plain_sky.environment",\n'
                f'  "start": {{"position": [{x}, {y + 13.0}, {z + 8.0}], "yaw_deg": 0, "pitch_deg": -58}},\n'
                '  "cells": ["wall_0_0.cell"]\n}\n')
    # What costs the cascades most (docs/design.md, their cost): every pixel a surface, every march's samples rising
    # and falling, light and glow everywhere. Blocks half a meter apart, each as high as a hash of its place says.
    blocks, glowing = [], []
    for ix in range(-48, 49):
        for iz in range(-36, 37):
            height = 0.2 + 1.8 * ((((ix * 73856093) ^ (iz * 19349663)) >> 4) & 0xff) / 255.0
            box = rc_room.cuboid([ix * 0.5 - 0.15, 0.0, iz * 0.5 - 0.15], [ix * 0.5 + 0.15, height, iz * 0.5 + 0.15],
                                 skip=('-y',))
            (glowing if (ix * 3 + iz * 5) % 7 == 0 else blocks).extend(box)
    rc_room.write_gltf('stress.gltf', [(PILLAR, blocks), (GLOW, glowing)], 'stress')
    crowd = [{'name': 'rc_room', 'position': list(rc_room.CENTRE)}, entity('ground', 'ground.gltf', 0.0, 0.0),
             entity('stress', 'stress.gltf', 0.0, 0.0)]
    with open(os.path.join(OUT, 'stress_0_0.cell'), 'w', encoding='utf-8') as f:
        f.write('{\n  "coord": [0, 0],\n  "entities": [\n')
        f.write(',\n'.join('    ' + json.dumps(e) for e in crowd))
        f.write('\n  ]\n}\n')
    with open(os.path.join(OUT, 'rc_stress_sun.world'), 'w', encoding='utf-8') as f:
        f.write('{\n  // The heaviest picture for the radiance cascades, to time them by (tools/worldgen/rc_plain.py).\n'
                '  "environment": "rc_plain_sun.environment",\n'
                f'  "start": {{"position": [{x}, {y + 13.0}, {z + 8.0}], "yaw_deg": 0, "pitch_deg": -58}},\n'
                '  "cells": ["stress_0_0.cell"]\n}\n')
    for name in ENVIRONMENTS:
        with open(os.path.join(OUT, f'rc_plain_{name}.environment'), 'w', encoding='utf-8') as f:
            f.write(environment(name))
        with open(os.path.join(OUT, f'rc_plain_{name}.world'), 'w', encoding='utf-8') as f:
            f.write('{\n  // The radiance cascades\' measuring room (tools/worldgen/rc_plain.py).\n'
                    f'  "environment": "rc_plain_{name}.environment",\n'
                    f'  "start": {{"position": [{x}, {y + 13.0}, {z + 8.0}], "yaw_deg": 0, "pitch_deg": -58}},\n'
                    '  "cells": ["cell_0_0.cell"]\n}\n')
    print('wrote', OUT)


if __name__ == '__main__':
    main()
