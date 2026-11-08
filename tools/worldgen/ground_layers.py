"""Fetches ground texture sets and packs each into a layer under content/terrain/layers: color, normal, and one texture of
occlusion, roughness and height. Poly Haven's are CC0, through its API; Megascans ones are read from build/fab_raw, where
their zips are unpacked by hand. Nothing here is committed: content/terrain is ignored.

    python tools/worldgen/ground_layers.py slug slug ...     fetch and pack these Poly Haven sets
    python tools/worldgen/ground_layers.py --grey name       a copy of a layer with most of its color out, to tint per region
"""
import json
import os
import shutil
import site
import sys
import urllib.request

sys.path.append(site.getusersitepackages())
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

Image.MAX_IMAGE_PIXELS = None
LAYERS = 'D:/DE/content/terrain/layers'
RAW = 'D:/DE/build/terrain_raw'
UA = {'User-Agent': 'DEengine-asset-pilot/1.0'}
RES = '2k'
# Sets whose broad bands are the look, so they keep them.
KEEP_BROAD = {'cliff_side': 0.25}


def get_json(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r:
        return json.load(r)


def fetch(url, path):
    if os.path.exists(path):
        return
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=600) as r:
        data = r.read()
    with open(path + '.part', 'wb') as f:
        f.write(data)
    os.replace(path + '.part', path)


def stretch_height(image):
    """Heights come normalised differently per library: 2% to 98% of what's there becomes 0 to 1, with broad swell out."""
    a = np.asarray(image.convert('I;16') if image.mode.startswith('I') else image.convert('L'), dtype=np.float32)
    small = np.asarray(Image.fromarray(a).resize((64, 64), Image.BOX).resize(image.size, Image.BICUBIC), dtype=np.float32)
    detail = a - small * 0.7
    low, high = np.percentile(detail[::4, ::4], [2.0, 98.0])
    return Image.fromarray((np.clip((detail - low) / max(high - low, 1e-3), 0, 1) * 255.0 + 0.5).astype(np.uint8))


def flatten(image, strength):
    """Takes a tile's blotches out: they're what lines up into rows from afar. Color across the land comes from noise."""
    if strength <= 0.0:
        return image.convert('RGB')
    a = np.asarray(image.convert('RGB'), dtype=np.float32) / 255.0
    linear = a ** 2.2
    size = 12
    small = np.asarray(Image.fromarray((a * 255).astype(np.uint8)).resize((size, size), Image.BOX), dtype=np.float32) / 255.0
    small = small ** 2.2
    wrapped = np.pad(small, ((size, size), (size, size), (0, 0)), mode='wrap')
    channels = [np.asarray(Image.fromarray(wrapped[:, :, c]).resize((image.size[0] * 3, image.size[1] * 3), Image.BICUBIC), dtype=np.float32) for c in range(3)]
    low = np.stack(channels, axis=2)[image.size[1]:image.size[1] * 2, image.size[0]:image.size[0] * 2]
    mean = linear.mean(axis=(0, 1), keepdims=True)
    result = linear * (mean / np.maximum(low, 1e-3)) ** strength
    return Image.fromarray((np.clip(result, 0, 1) ** (1 / 2.2) * 255.0 + 0.5).astype(np.uint8))


def write_layer(name, base, normal, ao, rough, height, flat):
    folder = f'{LAYERS}/{name}'
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    flatten(base, flat).save(f'{folder}/{name}_base.png')
    normal.convert('RGB').save(f'{folder}/{name}_normal.png')
    Image.merge('RGB', (ao, rough, height.resize(ao.size, Image.BILINEAR))).save(f'{folder}/{name}_orh.png')
    with open(f'{folder}/{name}.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'base_color': f'{name}_base.png', 'normal': f'{name}_normal.png', 'metallic_roughness': f'{name}_orh.png'},
                   'params': {'metallic': 0.0, 'roughness': 1.0, 'normal_scale': 1.0}}, f, indent=1)


def poly_haven(slug):
    raw = f'{RAW}/{slug}'
    os.makedirs(raw, exist_ok=True)
    info = get_json(f'https://api.polyhaven.com/info/{slug}')
    base, normal, arm, disp = f'{raw}/diff.jpg', f'{raw}/nor_gl.jpg', f'{raw}/arm.jpg', f'{raw}/disp.png'
    if not os.path.exists(disp):
        files = get_json(f'https://api.polyhaven.com/files/{slug}')
        fetch(files['Diffuse'][RES]['jpg']['url'], base)
        fetch(files['nor_gl'][RES]['jpg']['url'], normal)
        fetch(files['arm'][RES]['jpg']['url'], arm)
        fetch(files['Displacement'][RES]['png']['url'], disp)
    a, r, _ = Image.open(arm).convert('RGB').split()
    write_layer(slug, Image.open(base), Image.open(normal), a, r, stretch_height(Image.open(disp)), KEEP_BROAD.get(slug, 0.9))
    print(slug, 'ok,', max(info.get('dimensions', [2000])) / 1000.0, 'm tile')


def grey_copy(name, keep_color=0.2, contrast=1.35):
    """Tinting a strongly colored texture to another color scales up its faintest channel, noise and all. Take the color out."""
    target = f'{name}_grey'
    source, folder = f'{LAYERS}/{name}', f'{LAYERS}/{target}'
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    a = (np.asarray(Image.open(f'{source}/{name}_base.png').convert('RGB'), dtype=np.float32) / 255.0) ** 2.2
    luminance = a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    out = a * keep_color + luminance[:, :, None] * (1.0 - keep_color)
    # Strata read by contrast, and color was part of it.
    mean = float(luminance.mean())
    out *= ((np.maximum(luminance, 1e-4) / mean) ** (contrast - 1.0))[:, :, None]
    Image.fromarray((np.clip(out, 0, 1) ** (1 / 2.2) * 255.0 + 0.5).astype(np.uint8)).save(f'{folder}/{target}_base.png')
    shutil.copy(f'{source}/{name}_normal.png', f'{folder}/{target}_normal.png')
    shutil.copy(f'{source}/{name}_orh.png', f'{folder}/{target}_orh.png')
    with open(f'{folder}/{target}.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'base_color': f'{target}_base.png', 'normal': f'{target}_normal.png', 'metallic_roughness': f'{target}_orh.png'},
                   'params': {'metallic': 0.0, 'roughness': 1.0, 'normal_scale': 1.0}}, f, indent=1)
    print(target, 'ok, linear mean', out.reshape(-1, 3).mean(axis=0))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--grey':
        grey_copy(sys.argv[2])
    else:
        for name in sys.argv[1:]:
            poly_haven(name)
