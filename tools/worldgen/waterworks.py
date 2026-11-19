"""The waterworks: a small made-up land to work on water with, not part of the game's map. 2 x 2 km at 2 m: a sea along
the south below a beach, a lake basin in the west (rim 45 m, room for water at 40 m), a pond up in the east (rim 90 m,
water at 88 m), a cliff band across the north where a plateau drops about 35 m, and a river valley from the northern
hills over that cliff down to the lake. Its shape is drawn here, by rules, since it only has to hold water; the game's
own land comes from World Creator.

    python tools/worldgen/waterworks.py [--name waterworks]

Writes projects/dreadworld/content/terrain/<name>.*, which isn't committed.
"""
import argparse
import json
import math
import os
import site
import sys
import time

sys.path.append(site.getusersitepackages())
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

import fine  # noqa: E402
import lightbake  # noqa: E402
import shore  # noqa: E402
import surf_film  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'projects', 'dreadworld', 'content', 'terrain')
SIZE = 1024       # samples a side
SPACING_M = 2.0
SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG = 200.0, 32.0

# Where things are, meters east and south of the north-west corner.
LAKE = (700.0, 950.0, 230.0)     # middle x, z, radius
LAKE_RIM_M, LAKE_FLOOR_M = 45.0, 30.0
POND = (1520.0, 720.0, 70.0)
POND_RIM_M, POND_FLOOR_M = 90.0, 85.0
LAKE_LEVEL_M, POND_LEVEL_M = 40.0, 88.0
CELL_M = 128.0                   # kCellSize: a cell holds what stands in its square
CLIFF_Z_M, CLIFF_DROP_M = 520.0, 35.0
RIVER = [(1260.0, 0.0), (1200.0, 300.0), (1120.0, 520.0), (980.0, 680.0), (820.0, 760.0), (720.0, 900.0)]
RIVER_WIDTH_M, RIVER_DEPTH_M = 22.0, 5.0
BEACH_Z_M = 1640.0               # where the sand meets the sea's level
SEA_FLOOR_M = -30.0
SHELF_SLOPE = 40.0


def value_noise(x, z, scale, seed):
    """Smooth noise, 0 to 1, about scale meters across."""
    fx, fz = x / scale, z / scale
    ix, iz = np.floor(fx).astype(np.int64), np.floor(fz).astype(np.int64)
    tx, tz = fx - ix, fz - iz
    tx, tz = tx * tx * (3 - 2 * tx), tz * tz * (3 - 2 * tz)

    def corner(a, b):
        h = (a * 374761393 + b * 668265263 + seed * 2147483647) & 0xFFFFFFFF
        h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
        return (h & 0xFFFF) / 65535.0
    top = corner(ix, iz) * (1 - tx) + corner(ix + 1, iz) * tx
    bottom = corner(ix, iz + 1) * (1 - tx) + corner(ix + 1, iz + 1) * tx
    return top * (1 - tz) + bottom * tz


def smoothstep(a, b, t):
    t = np.clip((t - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def distance_to_polyline(x, z, points):
    best = np.full(x.shape, 1e9)
    for (ax, az), (bx, bz) in zip(points[:-1], points[1:]):
        dx, dz = bx - ax, bz - az
        t = np.clip(((x - ax) * dx + (z - az) * dz) / (dx * dx + dz * dz), 0.0, 1.0)
        best = np.minimum(best, np.hypot(x - (ax + t * dx), z - (az + t * dz)))
    return best


def heights():
    z, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64) * SPACING_M
    detail = (value_noise(x, z, 180.0, 1) - 0.5) * 14.0 + (value_noise(x, z, 45.0, 2) - 0.5) * 4.0
    # Land rising to the north: low by the beach, hills past the cliff.
    land = 6.0 + smoothstep(BEACH_Z_M, 600.0, z) * 60.0 + detail
    # The plateau north of the cliff, and the cliff itself (a drop over a few meters, its edge wandering).
    edge = CLIFF_Z_M + (value_noise(x, z * 0.0, 120.0, 3) - 0.5) * 60.0
    plateau = smoothstep(edge + 6.0, edge - 2.0, z)
    land += plateau * (CLIFF_DROP_M + 60.0 * smoothstep(CLIFF_Z_M, 0.0, z))
    # The beach and the sea: the land eases down to a shelf that slopes gently out under the sea (1 in SHELF_SLOPE, as a
    # sandy surf beach does, so waves break a long way out and run in over the shallows), then the floor.
    # The shoreline wanders: broad bays and points, and smaller bends in them.
    shore_z = BEACH_Z_M + (value_noise(x, z * 0.0, 320.0, 5) - 0.5) * 120.0 + (value_noise(x, z * 0.0, 70.0, 6) - 0.5) * 30.0
    beach = smoothstep(shore_z - 160.0, shore_z, z)
    shelf = np.maximum(SEA_FLOOR_M, -(z - shore_z) / SHELF_SLOPE)
    land = land * (1.0 - beach) + shelf * beach
    # The river's valley, cut into whatever is there, its bed falling with the land.
    river = distance_to_polyline(x, z, RIVER)
    bank = smoothstep(RIVER_WIDTH_M * 2.5, RIVER_WIDTH_M * 0.5, river)
    land -= bank * RIVER_DEPTH_M + smoothstep(RIVER_WIDTH_M * 7.0, RIVER_WIDTH_M, river) * 14.0
    # The lake: a rim, then a bowl to a flat-ish floor.
    for (cx, cz, r), rim, floor_m in ((LAKE, LAKE_RIM_M, LAKE_FLOOR_M), (POND, POND_RIM_M, POND_FLOOR_M)):
        d = np.hypot(x - cx, z - cz) * (1.0 + (value_noise(x, z, r * 0.6, 4) - 0.5) * 0.25)
        # The land around eases to the rim's height, then the bowl goes down inside it.
        ring = smoothstep(1.8 * r, 1.0 * r, d)
        land = land * (1.0 - ring) + rim * ring
        bowl = floor_m + (rim - floor_m) * smoothstep(0.35 * r, 1.0 * r, d)
        land = np.minimum(land, np.where(d < r, bowl, land))
    return land.astype(np.float32)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--name', default='waterworks')
    args = parser.parse_args()
    started = time.time()
    out = os.path.normpath(OUT)
    name = args.name
    folder = f'{out}/layers/masks'
    os.makedirs(folder, exist_ok=True)

    h = heights()
    lowest, highest = math.floor(float(h.min())) - 1.0, math.ceil(float(h.max())) + 1.0
    pixels = ((h - lowest) / (highest - lowest) * 65535.0 + 0.5).astype(np.uint16)
    Image.fromarray(pixels).save(f'{out}/{name}_tmp.png', format='PNG', compress_level=2)
    os.replace(f'{out}/{name}_tmp.png', f'{out}/{name}.heightpng')
    with open(f'{out}/{name}.terrain', 'w', encoding='utf-8') as f:
        json.dump({'height': f'{name}.heightpng', 'meters_per_pixel': SPACING_M, 'lowest_m': lowest,
                   'highest_m': highest}, f, indent=1)
    h = lowest + pixels.astype(np.float32) / 65535.0 * (highest - lowest)
    print('heights', f'{lowest:.0f} to {highest:.0f} m', f'{time.time() - started:.0f}s')

    # The ground's normal, light baked for the sun, and the sea's shore map.
    fine.write_normal_map(f'{folder}/{name}_normals_tmp.png', h, SPACING_M)
    os.replace(f'{folder}/{name}_normals_tmp.png', f'{folder}/{name}_normals.png')
    with open(f'{folder}/{name}_normals.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'normal': f'{name}_normals.png'}}, f, indent=1)
    lightbake.write_light_map(folder, name, h, SPACING_M, SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG)
    shore_material = shore.write_shore_map(folder, name, h, SPACING_M, 0.0, film=surf_film.write_surf_film(folder, name))
    print('maps', f'{time.time() - started:.0f}s')

    # A plain look: sand by the water, grass and dry ground above, rock on steep faces (from the sides).
    def layer(material, tile, **rules):
        out_layer = {'material': f'layers/{material}/{material}.material', 'tile_m': tile}
        out_layer.update(rules)
        return out_layer
    look = {'lighting': f'layers/masks/{name}_light.material', 'normals': f'layers/masks/{name}_normals.material',
            'blend': 0.28,
            'layers': [layer('gravelly_sand', 3.0, variation=0.25),
                       layer('withered_grass', 2.0, height_m=[2, 6, 400, 450], slope_deg=[0, 0, 26, 34], noise=1.2,
                             variation=0.2, tint=[0.78, 0.8, 0.55]),
                       layer('dry_ground_01', 4.0, height_m=[2, 6, 400, 450], slope_deg=[0, 0, 4, 12], noise=2.0,
                             variation=0.12),
                       layer('rock_face_03_grey', 4.0, slope_deg=[30, 40, 90, 90], projection='sides',
                             variation=0.1)]}
    with open(f'{out}/{name}.terrainlayers', 'w', encoding='utf-8') as f:
        json.dump(look, f, indent=1)
    with open(f'{out}/coast.environment', encoding='utf-8') as f:
        environment = json.load(f)
    environment['sun'] = dict(environment.get('sun', {}), elevation_deg=SUN_ELEVATION_DEG, azimuth_deg=SUN_AZIMUTH_DEG)
    with open(f'{out}/{name}.environment', 'w', encoding='utf-8') as f:
        json.dump(environment, f, indent=1)
    # The lake and the pond, each in the cell whose square its middle is in: still water at its level, as wide as its
    # basin's rim.
    cells = []
    os.makedirs(f'{out}/{name}', exist_ok=True)
    for label, (cx, cz, r), level, waves in (('lake', LAKE, LAKE_LEVEL_M, 0.2), ('pond', POND, POND_LEVEL_M, 0.08)):
        coord = [int(cx // CELL_M), int(cz // CELL_M)]
        cell = {'coord': coord, 'entities': [{'name': label, 'position': [cx, level, cz], 'scale': [r, 1.0, r],
                                              'water': {'shape': 'ellipse', 'waves': waves}}]}
        cell_name = f'cell_{coord[0]}_{coord[1]}.cell'
        with open(f'{out}/{name}/{cell_name}', 'w', encoding='utf-8') as f:
            json.dump(cell, f, indent=1)
        cells.append(f'{name}/{cell_name}')
    # On the beach, looking north over the lake toward the cliff.
    start_x, start_z = 760.0, 1560.0
    start_y = float(h[int(start_z / SPACING_M), int(start_x / SPACING_M)]) + 2.0
    world = {'environment': f'{name}.environment', 'terrain': f'{name}.terrain',
             'terrain_layers': f'{name}.terrainlayers',
             'sea': {'level_m': 0.0, 'shore': f'layers/masks/{shore_material}', 'foam': '../water/sea_foam.material'},
             'start': {'position': [start_x, start_y, start_z], 'yaw_deg': 0.0, 'pitch_deg': 4.0}, 'cells': cells}
    with open(f'{out}/{name}.world', 'w', encoding='utf-8') as f:
        json.dump(world, f, indent=1)
    print('done', f'{time.time() - started:.0f}s')


if __name__ == '__main__':
    main()
