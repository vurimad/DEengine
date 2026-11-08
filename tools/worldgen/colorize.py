"""A look for a map's mountains painted per pixel, as terrain tools such as World Creator paint theirs: no tiled texture
for the color, but rules over the land's own shape (shaders/scene/terrain.slang, paint_ground). This bakes only what the
rules can't see from one pixel, over the whole map at the heights' own spacing (layers/masks/<name>_wc_paint.png):

    r: ridges and hollows, 0.5 + meters above (or below) the land around / RIDGE_SCALE_M
    g: drainage, (log10 of the m^2 draining through) - 2) / 4, from the shape as it is (no erosion added)
    b: scree, as the map has it (its details' blue)
    a: how much the painted color applies: the mountains

and writes <name>_wc.terrainlayers and <name>_wc.world beside the map's own, the same but for "colors". Run after the
map's own generator (coast.py):

    python tools/worldgen/colorize.py coast
"""
import json
import os
import site
import sys
import time

sys.path.append(site.getusersitepackages())
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

import erosion  # noqa: E402
import snow  # noqa: E402
import worldgen as wg  # noqa: E402

Image.MAX_IMAGE_PIXELS = None
RIDGE_SCALE_M = 6.0  # kRidgeScaleM in shaders/scene/terrain.slang


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else 'coast'
    started = time.time()
    out = wg.OUT
    folder = f'{out}/layers/masks'
    with open(f'{out}/{name}.terrain', encoding='utf-8') as f:
        terrain = json.load(f)
    spacing = float(terrain['meters_per_pixel'])
    raw = np.asarray(Image.open(f"{out}/{terrain['height']}"), dtype=np.float32)
    heights = terrain['lowest_m'] + raw / 65535.0 * (terrain['highest_m'] - terrain['lowest_m'])
    rows, cols = heights.shape
    details = np.asarray(Image.open(f'{folder}/{name}_details.png'), dtype=np.float32) / 255.0
    assert details.shape[:2] == heights.shape, 'the details lie on the heights\' own grid'
    scree, mountain = details[..., 2], details[..., 3]

    # Ridges and hollows: how far each place stands above the land around it, at about 10 and 34 m.
    ridge = (heights - snow._box(heights, 2)) + 0.5 * (heights - snow._box(heights, 8))
    # Where water would gather, over four times the spacing (erosion.carve_range given no time to cut: its fill,
    # receivers and drainage area).
    shape = (rows // 4, cols // 4)
    coarse = snow._resize(heights, shape).copy()
    where = np.clip(mountain * 1.5, 0.0, 1.0)
    _, area = erosion.carve_range(coarse, snow._resize(where, shape) < 0.05, np.zeros_like(coarse), np.ones_like(coarse),
                                  spacing * 4.0, steps=1, dt=0.0, talus=1e6)
    drains = snow._resize(np.log10(area), (rows, cols))
    print('masks', f'{time.time() - started:.0f}s')

    channels = [0.5 + ridge / RIDGE_SCALE_M, (drains - 2.0) / 4.0, scree, np.clip(mountain * 2.5, 0.0, 1.0)]
    Image.merge('RGBA', [Image.fromarray((np.clip(c, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)) for c in channels]).save(
        f'{folder}/{name}_wc_paint.png', compress_level=2)
    with open(f'{folder}/{name}_wc_paint.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'metallic_roughness': f'{name}_wc_paint.png'}}, f, indent=1)

    # The same map and look, but painted.
    with open(f'{out}/{name}.terrainlayers', encoding='utf-8') as f:
        layers = json.load(f)
    layers['colors'] = f'layers/masks/{name}_wc_paint.material'
    with open(f'{out}/{name}_wc.terrainlayers', 'w', encoding='utf-8') as f:
        json.dump(layers, f, indent=1)
    with open(f'{out}/{name}.world', encoding='utf-8') as f:
        world = json.load(f)
    world['terrain_layers'] = f'{name}_wc.terrainlayers'
    with open(f'{out}/{name}_wc.world', 'w', encoding='utf-8') as f:
        json.dump(world, f, indent=1)
    with open(f'{out}/{name}.views', encoding='utf-8') as f:
        views = f.read()
    with open(f'{out}/{name}_wc.views', 'w', encoding='utf-8') as f:
        f.write(views)
    print('done', f'{time.time() - started:.0f}s')


if __name__ == '__main__':
    main()
