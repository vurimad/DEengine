"""A test world from real height data, to compare against the generated map: Utah's canyon country on the west half,
the valley under Aoraki on the east, 8 x 4 km at 2 m, with the light map baked, snow worked out on the alpine half and
our ground rules. The data stays local (build/terrain_raw/), and so does what this writes (projects/dreadworld/content/terrain/realtest.*).

    python tools/worldgen/realtest.py
"""
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

import erosion  # noqa: E402
import lightbake  # noqa: E402
import worldgen as wg  # noqa: E402

Image.MAX_IMAGE_PIXELS = None
NAME = 'realtest'
SPACING_M = 2.0
HALF = 2048                # samples per half, 4.1 km
ROWS = 2048
SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG = 215.0, 26.0
# The alpine half's heights are kept to this share of their real relief: a terrain node holds a rise of about 2 km.
ALPINE_RELIEF = 0.8


def load(folder, name):
    with open(f'{folder}/{name}.json', encoding='utf-8') as f:
        meta = json.load(f)
    raw = np.asarray(Image.open(f'{folder}/{name}_heights.png'), dtype=np.float32)
    return float(meta['lowest_m']) + raw / 65535.0 * (float(meta['highest_m']) - float(meta['lowest_m'])), float(meta['spacing_m'])


def window(heights, spacing_m, center_x_m, center_z_m, size_x, size_z):
    """A window of heights at SPACING_M, size_x by size_z samples, centered at a point of the source (meters from its
    north-west corner)."""
    xs = (center_x_m + (np.arange(size_x, dtype=np.float32) - size_x / 2.0) * SPACING_M) / spacing_m
    zs = (center_z_m + (np.arange(size_z, dtype=np.float32) - size_z / 2.0) * SPACING_M) / spacing_m
    x = np.clip(xs[None, :], 0, heights.shape[1] - 1.001)
    z = np.clip(zs[:, None], 0, heights.shape[0] - 1.001)
    x0, z0 = x.astype(np.int32), z.astype(np.int32)
    fx, fz = x - x0, z - z0
    a = heights[z0, x0] * (1 - fx) + heights[z0, x0 + 1] * fx
    b = heights[z0 + 1, x0] * (1 - fx) + heights[z0 + 1, x0 + 1] * fx
    return (a * (1 - fz) + b * fz).astype(np.float32)


def main():
    started = time.time()
    utah, utah_m = load('D:/DE/build/terrain_raw/utah', 'utah')
    aoraki, aoraki_m = load('D:/DE/build/terrain_raw/aoraki', 'aoraki')
    # Utah: the middle of its box.
    west = window(utah, utah_m, utah.shape[1] * utah_m / 2.0, utah.shape[0] * utah_m / 2.0, HALF, ROWS)
    # Aoraki: its summit near the north edge of the half, the valley below it to the south.
    peak_row, peak_col = np.unravel_index(int(np.argmax(aoraki)), aoraki.shape)
    east = window(aoraki, aoraki_m, peak_col * aoraki_m + 300.0, peak_row * aoraki_m + 1600.0, HALF, ROWS)
    # Heights: the canyons from 300 m up; the alpine half from 600 m, its relief kept to ALPINE_RELIEF.
    west = west - float(np.percentile(west, 1)) + 300.0
    east = (east - float(np.percentile(east, 1))) * ALPINE_RELIEF + 600.0
    # Join them over 400 m in the middle, through a valley a little lower than either side.
    t = np.clip((np.arange(2 * HALF, dtype=np.float32) - (HALF - 100)) / 200.0, 0.0, 1.0)
    t = t * t * (3.0 - 2.0 * t)
    heights = np.concatenate([west, east], axis=1)
    wide_west = np.concatenate([west, np.repeat(west[:, -1:], HALF, axis=1)], axis=1)
    wide_east = np.concatenate([np.repeat(east[:, :1], HALF, axis=1), east], axis=1)
    heights = wide_west * (1 - t)[None, :] + wide_east * t[None, :]
    valley = np.exp(-(((np.arange(2 * HALF) - HALF) / 120.0) ** 2))[None, :]
    heights -= valley * 40.0
    # Grain on steep ground the 4 m alpine data doesn't have at 2 m.
    rng = np.random.default_rng(3)
    gz, gx = np.gradient(heights, SPACING_M)
    steep = np.clip((np.hypot(gx, gz) - 0.6) / 0.8, 0.0, 1.0) * t[None, :]
    grain = np.asarray(Image.fromarray(rng.random((ROWS // 2, HALF), dtype=np.float32)).resize((2 * HALF, ROWS), Image.BICUBIC), dtype=np.float32)
    heights = heights + (grain - 0.5) * 1.2 * steep
    print('heights', round(float(heights.min())), 'to', round(float(heights.max())), f'{time.time() - started:.0f}s')

    # Snow on the alpine half, sliding off what's too steep.
    snow_line = 600.0 + 1300.0 * ALPINE_RELIEF
    snow = (np.clip((heights - snow_line) / 600.0, 0.0, 1.0) * 6.0 * t[None, :]).astype(np.float32)
    erosion.slide(heights, snow, SPACING_M, 47.0, 80)
    heights = heights + snow * 0.7
    print('snow', f'{time.time() - started:.0f}s')

    out = wg.OUT
    folder = f'{out}/layers/masks'
    os.makedirs(folder, exist_ok=True)
    lowest, highest = -60.0, 3800.0
    pixels = ((np.clip(heights, lowest, highest) - lowest) / (highest - lowest) * 65535.0 + 0.5).astype(np.uint16)
    Image.fromarray(pixels).save(f'{out}/{NAME}_tmp.png', format='PNG', compress_level=2)
    os.replace(f'{out}/{NAME}_tmp.png', f'{out}/{NAME}.heightpng')
    with open(f'{out}/{NAME}.terrain', 'w', encoding='utf-8') as f:
        json.dump({'height': f'{NAME}.heightpng', 'meters_per_pixel': SPACING_M, 'lowest_m': lowest, 'highest_m': highest}, f, indent=1)
    # Masks: red country west, golden grass in the alpine valley east.
    small = (1024, 512)
    red = Image.fromarray(((1.0 - t) * 255.0 + 0.5).astype(np.uint8)[None, :].repeat(ROWS, axis=0)).resize(small, Image.BILINEAR)
    golden = Image.fromarray((t * 255.0 + 0.5).astype(np.uint8)[None, :].repeat(ROWS, axis=0)).resize(small, Image.BILINEAR)
    zero = Image.new('L', small, 0)
    Image.merge('RGBA', (red, zero, zero, golden)).save(f'{folder}/{NAME}_masks.png')
    with open(f'{folder}/{NAME}_masks.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'metallic_roughness': f'{NAME}_masks.png'}}, f, indent=1)
    # Details: snow, no flow, no scree, the alpine half as mountain (its own rock).
    detail_size = (2 * HALF // 2, ROWS // 2)
    snow_cover = np.clip((snow - 0.2) / 1.0, 0.0, 1.0)
    channels = [Image.fromarray((np.clip(c, 0, 1) * 255.0 + 0.5).astype(np.uint8)).resize(detail_size, Image.BILINEAR)
                for c in (snow_cover, np.zeros_like(snow), np.zeros_like(snow), np.repeat(t[None, :], ROWS, axis=0))]
    Image.merge('RGBA', channels).save(f'{folder}/{NAME}_details.png')
    with open(f'{folder}/{NAME}_details.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'metallic_roughness': f'{NAME}_details.png'}}, f, indent=1)
    # Light: shadows from far off and the sky each place sees, at 4 m.
    four = np.asarray(Image.fromarray(heights).resize((HALF, ROWS // 2), Image.BILINEAR), dtype=np.float32)
    lightbake.write_light_map(folder, NAME, four, SPACING_M * 2.0, SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG)
    print('maps', f'{time.time() - started:.0f}s')

    # Ground rules, environment, world and views, from the generator's own, under this name.
    wg.NAME = NAME
    wg.SUN_AZIMUTH_DEG, wg.SUN_ELEVATION_DEG = SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG
    wg.write_ground()
    with open(f'{out}/realm.environment', encoding='utf-8') as f:
        environment = json.load(f)
    environment['sun']['azimuth_deg'] = SUN_AZIMUTH_DEG
    environment['sun']['elevation_deg'] = SUN_ELEVATION_DEG
    with open(f'{out}/{NAME}.environment', 'w', encoding='utf-8') as f:
        json.dump(environment, f, indent=1)

    def ground(x, z):
        return float(heights[int(np.clip(z / SPACING_M, 0, ROWS - 1)), int(np.clip(x / SPACING_M, 0, 2 * HALF - 1))])

    def look(name, x, z, tx, tz, up=2.0, aim_up=0.0):
        y = ground(x, z) + up
        ty = ground(tx, tz) + aim_up
        yaw = math.degrees(math.atan2(-(tx - x), -(tz - z)))
        pitch = math.degrees(math.atan2(ty - y, math.hypot(tx - x, tz - z)))
        return f'{name} {x:.1f} {y:.1f} {z:.1f} {yaw:.1f} {pitch:.1f}'

    w = 2 * HALF * SPACING_M
    d = ROWS * SPACING_M
    # Where the canyons are deepest and highest, for views of them.
    west_half = heights[:, :HALF - 200]
    low = np.unravel_index(int(np.argmin(west_half)), west_half.shape)
    high = np.unravel_index(int(np.argmax(west_half)), west_half.shape)
    low_x, low_z = low[1] * SPACING_M, low[0] * SPACING_M
    high_x, high_z = high[1] * SPACING_M, high[0] * SPACING_M
    peak = np.unravel_index(int(np.argmax(heights[:, HALF + 200:])), heights[:, HALF + 200:].shape)
    peak_x, peak_z = (peak[1] + HALF + 200) * SPACING_M, peak[0] * SPACING_M
    views = [
        look('start', w * 0.5, d * 0.85, peak_x, peak_z, aim_up=-300.0),
        look('canyon_floor', low_x, low_z, high_x, high_z, aim_up=-60.0),
        look('canyon_rim', high_x, high_z, low_x, low_z, up=2.0, aim_up=0.0),
        look('canyon_far', w * 0.42, d * 0.2, w * 0.15, d * 0.6, up=60.0, aim_up=-40.0),
        look('alpine_valley', w * 0.62, d * 0.9, peak_x, peak_z, aim_up=-600.0),
        look('alpine_face', peak_x - 900.0, peak_z + 2600.0, peak_x, peak_z, aim_up=-400.0),
        look('middle_west', w * 0.5, d * 0.5, w * 0.1, d * 0.5, up=3.0),
        look('middle_east', w * 0.5, d * 0.5, w * 0.9, d * 0.3, up=3.0, aim_up=-200.0),
    ]
    views.append(f'air_both {w * 0.5:.1f} {ground(w * 0.5, d) + 1400:.1f} {d + 1500:.1f} 0 -22')
    views.append(f'air_canyon {w * 0.25:.1f} {ground(w * 0.25, d * 0.9) + 500:.1f} {d * 0.95:.1f} 0 -18')
    with open(f'{out}/{NAME}.views', 'w', encoding='utf-8') as f:
        f.write('\n'.join(views) + '\n')
    start = views[0].split()
    with open(f'{out}/{NAME}.world', 'w', encoding='utf-8') as f:
        json.dump({'environment': f'{NAME}.environment', 'terrain': f'{NAME}.terrain', 'terrain_layers': f'{NAME}.terrainlayers',
                   'start': {'position': [float(start[1]), float(start[2]), float(start[3])], 'yaw_deg': float(start[4]),
                             'pitch_deg': float(start[5])}, 'cells': []}, f, indent=1)
    print('\n'.join(views))
    print('done', f'{time.time() - started:.0f}s')


if __name__ == '__main__':
    main()
