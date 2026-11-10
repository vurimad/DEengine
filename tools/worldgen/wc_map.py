"""A map made from a World Creator export alone, as World Creator shows it: its heights in its own meters, its splat
map's layers as the ground's weights, and nothing of ours on top (no snow bake, no fades, no other land). To see how
close the engine gets to World Creator's own view before bringing its terrain into a bigger map.

    python tools/worldgen/wc_map.py HEIGHT.tif SPLAT.tif [--name wc] [--scale 106.45] [--spacing 1]

HEIGHT.tif: World Creator's float height map, in its export units; --scale says how many meters a unit is (World
Creator's ruler over the export's own range: 106.45 for the user's first export). SPLAT.tif: its splat map, a channel
per layer, summing to 255: layer 0 (snow) and layer 1 (debris) here. What this writes stays local (projects/dreadworld/content/terrain/).
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
import tifffile  # noqa: E402
from PIL import Image  # noqa: E402

import fine  # noqa: E402
import lightbake  # noqa: E402

Image.MAX_IMAGE_PIXELS = None
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'projects', 'dreadworld', 'content', 'terrain')
# The light, as World Creator's view has it: a low warm sun from about the south, behind and left of a camera looking
# north-west, blue in the shade.
SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG = 190.0, 20.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('height')
    parser.add_argument('splat')
    parser.add_argument('--name', default='wc')
    parser.add_argument('--scale', type=float, default=106.45)
    parser.add_argument('--spacing', type=float, default=1.0)
    args = parser.parse_args()
    started = time.time()
    out = os.path.normpath(OUT)
    name = args.name
    folder = f'{out}/layers/masks'
    os.makedirs(folder, exist_ok=True)

    heights = tifffile.imread(args.height).astype(np.float32) * np.float32(args.scale)
    rows, cols = heights.shape
    lowest, highest = math.floor(float(heights.min())) - 1.0, math.ceil(float(heights.max())) + 1.0
    print('heights', rows, 'x', cols, f'{lowest:.0f} to {highest:.0f} m', f'{time.time() - started:.0f}s')
    pixels = ((heights - lowest) / (highest - lowest) * 65535.0 + 0.5).astype(np.uint16)
    Image.fromarray(pixels).save(f'{out}/{name}_tmp.png', format='PNG', compress_level=2)
    os.replace(f'{out}/{name}_tmp.png', f'{out}/{name}.heightpng')
    with open(f'{out}/{name}.terrain', 'w', encoding='utf-8') as f:
        json.dump({'height': f'{name}.heightpng', 'meters_per_pixel': args.spacing, 'lowest_m': lowest,
                   'highest_m': highest}, f, indent=1)
    # What the mesh holds, as the cooker will read it back: the normals come from these, so they agree.
    heights = lowest + pixels.astype(np.float32) / 65535.0 * (highest - lowest)

    # The splat map's layers as masks: r its layer 0 (snow), g its layer 1 (debris).
    splat = tifffile.imread(args.splat)
    assert splat.shape[:2] == heights.shape, 'the splat map lies on the heights\' grid'
    zero = np.zeros(heights.shape, np.uint8)
    channels = [splat[..., 0], splat[..., 1] if splat.shape[2] > 1 else zero, zero, zero]
    Image.merge('RGBA', [Image.fromarray(c.astype(np.uint8)) for c in channels]).save(f'{folder}/{name}_masks.png')
    with open(f'{folder}/{name}_masks.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'metallic_roughness': f'{name}_masks.png'}}, f, indent=1)

    # The ground's normal at half the spacing, and the sun's long shadows and the sky seen, from the heights alone.
    fine.write_normal_map(f'{folder}/{name}_normals_tmp.png', heights, args.spacing)
    os.replace(f'{folder}/{name}_normals_tmp.png', f'{folder}/{name}_normals.png')
    with open(f'{folder}/{name}_normals.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'normal': f'{name}_normals.png'}}, f, indent=1)
    half = np.asarray(Image.fromarray(heights).resize((cols // 2, rows // 2), Image.BILINEAR), dtype=np.float32)
    lightbake.write_light_map(folder, name, half, args.spacing * 2.0, SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG)
    print('maps', f'{time.time() - started:.0f}s')

    # The look: debris everywhere, snow over it as much as its layer says, blended by the textures' heights.
    def layer(material, tile, **rules):
        out_layer = {'material': f'layers/{material}/{material}.material', 'tile_m': tile}
        out_layer.update(rules)
        return out_layer
    with open(f'{out}/{name}.terrainlayers', 'w', encoding='utf-8') as f:
        json.dump({'masks': f'layers/masks/{name}_masks.material', 'lighting': f'layers/masks/{name}_light.material',
                   'normals': f'layers/masks/{name}_normals.material', 'blend': 0.25,
                   'layers': [layer('rocks_ground_05', 2.5, tint=[0.62, 0.44, 0.3], variation=0.12),
                              layer('snow_02', 2.0, mask='r', mask_range=[0.0, 1.0], tint=[2.25, 2.27, 2.32],
                                    variation=0.04)]}, f, indent=1)

    with open(f'{out}/coast.environment', encoding='utf-8') as f:
        environment = json.load(f)
    environment.pop('sea', None)
    environment['sun'] = {'elevation_deg': SUN_ELEVATION_DEG, 'azimuth_deg': SUN_AZIMUTH_DEG, 'color': [1.0, 0.78, 0.55],
                          'intensity': 4.6}
    environment['sky'] = {'color': [0.24, 0.37, 0.7], 'intensity': 1}
    environment['shadow_color'] = [0.04, 0.06, 0.12]
    environment['fog']['base_height_m'] = lowest
    with open(f'{out}/{name}.environment', 'w', encoding='utf-8') as f:
        json.dump(environment, f, indent=1)

    # Views: World Creator's, about: from the south-east looking north-west at the summit, from the air.
    peak_row, peak_col = np.unravel_index(int(np.argmax(heights)), heights.shape)
    px, pz, py = peak_col * args.spacing, peak_row * args.spacing, float(heights[peak_row, peak_col])
    views = []

    def view(view_name, bearing_deg, distance_m, height_m):
        b = math.radians(bearing_deg)
        x, z = px + distance_m * math.sin(b), pz - distance_m * math.cos(b)
        x = min(max(x, 0.0), (cols - 1) * args.spacing)
        z = min(max(z, 0.0), (rows - 1) * args.spacing)
        yaw = -((bearing_deg + 180.0) % 360.0)
        pitch = math.degrees(math.atan2(py - height_m, math.hypot(px - x, pz - z)))
        views.append(f'{view_name} {x:.1f} {height_m:.1f} {z:.1f} {yaw:.1f} {pitch:.1f}')
    view('wc_view', 133.0, 1250.0, py - 150.0)  # World Creator's framing, about: the summit filling the view
    view('wc_main', 133.0, 1900.0, py - 250.0)
    view('wc_air', 133.0, 2000.0, py + 400.0)
    view('wc_close', 133.0, 900.0, py - 300.0)
    view('wc_side', 200.0, 1900.0, py - 200.0)
    with open(f'{out}/{name}.views', 'w', encoding='utf-8') as f:
        f.write('\n'.join(views) + '\n')
    start = views[0].split()
    with open(f'{out}/{name}.world', 'w', encoding='utf-8') as f:
        json.dump({'environment': f'{name}.environment', 'terrain': f'{name}.terrain', 'terrain_layers': f'{name}.terrainlayers',
                   'start': {'position': [float(start[1]), float(start[2]), float(start[3])], 'yaw_deg': float(start[4]),
                             'pitch_deg': float(start[5])}, 'cells': []}, f, indent=1)
    print('\n'.join(views))
    print('done', f'{time.time() - started:.0f}s')


if __name__ == '__main__':
    main()
