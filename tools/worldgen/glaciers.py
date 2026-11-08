"""Where the real glaciers are, on a map built from real heights: the Randolph Glacier Inventory's outlines (RGI 6.0,
CC BY 4.0, local only: build/terrain_raw/rgi60_nz, see its README for the source and credit), drawn onto the map's grid.
Inside them the ground is ice, so a crevasse or an icefall is ice and snow however steep it is, never bare rock.

The outlines are a shapefile in longitude and latitude (WGS84, within a meter of NZGD2000 here); this reads it itself
(polygons only), and projects to NZTM2000 the way the height data is.
"""
import math
import struct

import numpy as np
from PIL import Image, ImageDraw

# NZTM2000 (EPSG:2193): transverse Mercator on GRS80, Krueger's series to n^6.
_A_AXIS = 6378137.0
_F = 1.0 / 298.257222101
_K0 = 0.9996
_LON0 = math.radians(173.0)
_FE, _FN = 1600000.0, 10000000.0
_n = _F / (2.0 - _F)
_E = math.sqrt(_F * (2.0 - _F))
_A = _A_AXIS / (1.0 + _n) * (1.0 + _n ** 2 / 4.0 + _n ** 4 / 64.0 + _n ** 6 / 256.0)
_ALPHA = [
    _n / 2 - 2 * _n ** 2 / 3 + 5 * _n ** 3 / 16 + 41 * _n ** 4 / 180 - 127 * _n ** 5 / 288 + 7891 * _n ** 6 / 37800,
    13 * _n ** 2 / 48 - 3 * _n ** 3 / 5 + 557 * _n ** 4 / 1440 + 281 * _n ** 5 / 630 - 1983433 * _n ** 6 / 1935360,
    61 * _n ** 3 / 240 - 103 * _n ** 4 / 140 + 15061 * _n ** 5 / 26880 + 167603 * _n ** 6 / 181440,
    49561 * _n ** 4 / 161280 - 179 * _n ** 5 / 168 + 6601661 * _n ** 6 / 7257600,
    34729 * _n ** 5 / 80640 - 3418889 * _n ** 6 / 1995840,
    212378941 * _n ** 6 / 319334400,
]


def to_nztm(lat_deg, lon_deg):
    """Easting and northing in meters, for arrays of latitude and longitude in degrees."""
    phi = np.radians(lat_deg)
    lam = np.radians(lon_deg) - _LON0
    s = np.sin(phi)
    t = np.sinh(np.arctanh(s) - _E * np.arctanh(_E * s))
    xi_p = np.arctan2(t, np.cos(lam))
    eta_p = np.arctanh(np.sin(lam) / np.sqrt(1.0 + t * t))
    xi, eta = xi_p.copy(), eta_p.copy()
    for j, a in enumerate(_ALPHA, start=1):
        xi += a * np.sin(2 * j * xi_p) * np.cosh(2 * j * eta_p)
        eta += a * np.cos(2 * j * xi_p) * np.sinh(2 * j * eta_p)
    return _FE + _K0 * _A * eta, _FN + _K0 * _A * xi


def read_polygons(path):
    """Each polygon of a shapefile as (bounding box, rings), a ring an array of (x, y) points."""
    with open(path, 'rb') as f:
        data = f.read()
    shape_type = struct.unpack_from('<i', data, 32)[0]
    if shape_type not in (5, 15, 25):
        raise ValueError(f'{path}: shape type {shape_type}, not polygons')
    out = []
    at = 100
    while at + 8 <= len(data):
        length = struct.unpack_from('>i', data, at + 4)[0] * 2
        record = at + 8
        at = record + length
        if struct.unpack_from('<i', data, record)[0] == 0:
            continue  # a null shape
        box = struct.unpack_from('<4d', data, record + 4)
        parts, points = struct.unpack_from('<2i', data, record + 36)
        starts = list(struct.unpack_from(f'<{parts}i', data, record + 44)) + [points]
        xy = np.frombuffer(data, dtype='<f8', count=points * 2, offset=record + 44 + 4 * parts).reshape(points, 2)
        out.append((box, [xy[starts[k]:starts[k + 1]] for k in range(parts)]))
    return out


def ice_map(shp_path, west_e, north_n, spacing_m, cols, rows):
    """How much of each map sample a glacier covers, 0 or 1 but at edges: sample (i, j) at (west_e + i spacing_m,
    north_n - j spacing_m) in NZTM2000. Rings that run clockwise are a glacier's edge; the others, holes in it."""
    east_e, south_n = west_e + (cols - 1) * spacing_m, north_n - (rows - 1) * spacing_m
    scale = 2  # drawn at half the spacing, then averaged: edges fall between 0 and 1
    image = Image.new('L', (cols * scale, rows * scale), 0)
    draw = ImageDraw.Draw(image)
    drawn = 0
    for box, rings in read_polygons(shp_path):
        e0, n0 = to_nztm(np.array([box[1]]), np.array([box[0]]))
        e1, n1 = to_nztm(np.array([box[3]]), np.array([box[2]]))
        if e1[0] < west_e - 500 or e0[0] > east_e + 500 or n1[0] < south_n - 500 or n0[0] > north_n + 500:
            continue
        for ring in rings:
            e, n = to_nztm(ring[:, 1], ring[:, 0])
            # Shoelace over (easting, northing): clockwise, as an outer ring runs, is negative.
            area = 0.5 * float(np.sum(e[:-1] * n[1:] - e[1:] * n[:-1]))
            x = ((e - west_e) / spacing_m + 0.5) * scale
            y = ((north_n - n) / spacing_m + 0.5) * scale
            draw.polygon(list(zip(x.tolist(), y.tolist())), fill=255 if area < 0 else 0)
        drawn += 1
    small = image.resize((cols, rows), Image.BOX)
    print('glaciers drawn', drawn)
    return np.asarray(small, dtype=np.float32) / 255.0
