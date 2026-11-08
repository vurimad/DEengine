"""A simple map to build other things on: a long strip of golden field ending in a high cliff over the sea, with snow
mountains behind it. 16.4 x 8.2 km at 2 m. The mountains are the real ones around Aoraki (build/terrain_raw/aoraki, CC BY
4.0, local only), at 80% of their relief so a terrain node holds them; their vertices and the 1 m normal map come from
the 1 m data where it's fetched (build/terrain_raw/aoraki_1m). What this writes stays local (content/terrain/coast.*).

    python tools/worldgen/coast.py
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

import fine as fine_detail  # noqa: E402
import glaciers  # noqa: E402
import lightbake  # noqa: E402
import shore  # noqa: E402
import snow  # noqa: E402
import worldgen as wg  # noqa: E402
from realtest import load, window  # noqa: E402

Image.MAX_IMAGE_PIXELS = None
NAME = 'coast'
SPACING_M = 2.0
COLS, ROWS = 8192, 4096          # 16.4 km east to west, 8.2 km north to south
COAST_Z_M = 5600.0               # where the cliff stands, on average
CLIFF_M = (170.0, 235.0)         # its height, lowest and highest along the coast
FIELD_RISE_M = 160.0             # the field rises this much from the cliff edge to the mountains' foot
MOUNTAINS_FROM_Z_M = 2600.0      # the mountains rise behind this line
ALPINE_RELIEF = 0.8
ENDS_M = 2600.0                  # behind the start, the mountains come down to the sea this far in from the east end
WEST_END_M = 4200.0              # in front of it, the land sinks to a low point over this much of the west end
SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG = 255.0, 11.0
# The user's World Creator export of the summit and its south faces (a 4,096 m square at 1 m), and the cut it was made
# from, which says where it lies. Used if both are there.
WC_EXPORT = 'C:/Users/Ark/Documents/World Creator/Export/Project_Height Map_4096x4096_0_0.tif'
WC_CUT = 'C:/Users/Ark/Documents/World Creator/RealData/Aoraki_South_4096m/Aoraki_South_4096m.json'
# Its splat map, the same square: two layers' weights summing to 255, snow (on the steep faces) and debris (rock and
# moraine on the gentle ground), taken for the map's snow and scree there.
WC_SPLAT = 'C:/Users/Ark/Documents/World Creator/Export/Project_Splat Map_0_0_0.tif'
# Snow (snow.py, after study 12): a winter's, from the real mountain's snow line (the mean winter line near Aoraki is
# about 1,263 m), loaded by the prevailing north-westerlies and thinned on the faces a midwinter sun at 43.6 degrees
# south sees most, low in the north. The real mountains' north is the map's -Z.
SNOW_LINE_REAL_M = 1300.0
SNOW_WIND_FROM_DEG = 300.0
SNOW_SUNS = ((50.0, 10.0), (25.0, 19.0), (0.0, 23.0), (335.0, 19.0), (310.0, 10.0))
ICE_FROM_REAL_M = 2300.0
USE_GLACIERS = False             # the RGI outlines' ice in the snow: off, the user's call (2026-11-08)         # steep rock keeps a coat of ice from here up, all of it 700 m higher
SNOW_FROM_M = 700.0            # no snow below this on the map, rising in over 200 m: the field stops below 520 m
GRASS_TO_M = (600.0, 700.0)      # and grass fades out over this, before the snow starts
SNOW_SHOWS_M = 0.4               # snow shows from this depth up
rng = np.random.default_rng(11)


def noise1(length, cell, octaves=3):
    """Smooth noise along one axis, 0 to 1."""
    out = np.zeros(length, dtype=np.float32)
    amp, total = 0.5, 0.0
    for o in range(octaves):
        size = cell / (2 ** o)
        points = int(length / size) + 3
        coarse = rng.random(points).astype(np.float32)
        x = np.arange(length, dtype=np.float32) / size
        i = x.astype(np.int32)
        f = x - i
        f = f * f * (3 - 2 * f)
        out += (coarse[i] * (1 - f) + coarse[i + 1] * f) * amp
        total += amp
        amp *= 0.5
    return out / total


def main():
    started = time.time()
    g = wg.Grid(1)  # only for its helpers' sake
    x = (np.arange(COLS, dtype=np.float32) + 0.5) * SPACING_M
    z = (np.arange(ROWS, dtype=np.float32) + 0.5) * SPACING_M
    X, Z = np.meshgrid(x, z)

    # The coast: where the cliff stands, wandering, with headlands and coves.
    coast = (COAST_Z_M + (noise1(COLS, 3000.0 / SPACING_M, 3) - 0.5) * 1400.0 + (noise1(COLS, 500.0 / SPACING_M, 2) - 0.5) * 320.0
             + (noise1(COLS, 90.0 / SPACING_M, 2) - 0.5) * 50.0).astype(np.float32)
    # Headlands reaching out to sea, and coves biting in between them.
    for center_m, reach_m, width_m in ((1800.0, 700.0, 420.0), (5200.0, 900.0, 520.0), (8800.0, 520.0, 300.0), (11600.0, 820.0, 460.0),
                                       (14600.0, 650.0, 380.0), (3400.0, -420.0, 260.0), (7000.0, -360.0, 220.0), (10200.0, -480.0, 300.0),
                                       (13200.0, -300.0, 200.0)):
        coast += (reach_m * np.exp(-((x - center_m) / width_m) ** 4)).astype(np.float32)
    cliff_height = CLIFF_M[0] + (CLIFF_M[1] - CLIFF_M[0]) * noise1(COLS, 2500.0 / SPACING_M, 2)
    # Toward the west end the cliff sinks and the coast turns north, so the land ends in a low point with open sea past
    # it: the view west along the cliff from the start stays open.
    west_share = np.clip((WEST_END_M - x) / WEST_END_M, 0.0, 1.0)
    coast = coast - (coast - 700.0) * west_share ** 1.6
    cliff_height = cliff_height * (1.0 - 0.88 * west_share ** 1.2)
    to_edge = coast[None, :] - Z  # positive inland, in meters

    # The field: from the cliff's height at the edge, rising toward the mountains, rolling, with shallow stream valleys
    # running down to the cliff, where they notch it.
    def field_noise(cell_m, octaves):
        small = rng.random((int(ROWS * SPACING_M / cell_m) + 3, int(COLS * SPACING_M / cell_m) + 3)).astype(np.float32)
        big = Image.fromarray(small).resize((COLS, ROWS), Image.BICUBIC, box=(0.5, 0.5, COLS * SPACING_M / cell_m + 0.5, ROWS * SPACING_M / cell_m + 0.5))
        return np.asarray(big, dtype=np.float32)
    # The field rises from the coast's broad line, not its every turn: by those, each column of the field would sit a
    # little higher or lower than the next, all the way to the mountains, in grooves running inland.
    reach = int(1200.0 / SPACING_M)
    broad_coast = np.convolve(np.pad(coast, reach, mode='reflect'), np.ones(2 * reach + 1, np.float32) / (2 * reach + 1), mode='valid')
    to_broad_edge = broad_coast[None, :].astype(np.float32) - Z
    rise = np.clip(to_broad_edge / (COAST_Z_M - MOUNTAINS_FROM_Z_M), 0.0, 1.4)
    field = cliff_height[None, :] + FIELD_RISE_M * (1.0 - 0.8 * west_share[None, :]) * rise ** 1.3
    field += (field_noise(900.0, 1) - 0.5) * 34.0 + (field_noise(260.0, 1) - 0.5) * 10.0 + (field_noise(70.0, 1) - 0.5) * 2.5
    streams = 1.0 - np.abs(np.sin(x / 1300.0 + (noise1(COLS, 2000.0 / SPACING_M, 2) - 0.5) * 4.0))
    valley = np.clip((streams[None, :] - 0.93) / 0.07, 0.0, 1.0) ** 2 * np.clip(to_edge / 200.0, 0.0, 1.0) * np.clip(1.0 - to_broad_edge / 2600.0, 0.0, 1.0)
    field -= valley * 38.0
    print('field', f'{time.time() - started:.0f}s')

    # The cliff: a sheer drop at the coast, ledged where the rock's layers are harder, a scree apron at its foot, a narrow
    # strip of rocks and sand, then the sea floor.
    sea_floor = -32.0 + 30.0 * np.exp(-np.maximum(-to_edge, 0.0) / 120.0)
    face_m = 14.0 + 10.0 * field_noise(400.0, 1)  # how far the face leans back over its height
    t = np.clip((to_edge + face_m * 0.2) / face_m, 0.0, 1.0)
    heights = sea_floor + (field - sea_floor) * t
    ledge = 11.0
    layered = np.floor(heights / ledge) * ledge + ledge * np.clip((heights / ledge - np.floor(heights / ledge) - 0.25) / 0.5, 0.0, 1.0)
    on_face = (t > 0.02) & (t < 0.98)
    heights = np.where(on_face, heights + (layered - heights) * 0.6, heights)
    apron = np.clip(1.0 - np.abs(to_edge + 25.0) / 30.0, 0.0, 1.0)
    heights = np.maximum(heights, sea_floor + apron * (8.0 + 10.0 * field_noise(60.0, 1)))
    # Sea stacks: a few pillars of the cliff's rock standing off the coast.
    for _ in range(9):
        sx = rng.uniform(800.0, COLS * SPACING_M - 800.0)
        col = int(sx / SPACING_M)
        sz = coast[col] + rng.uniform(80.0, 380.0)
        r = rng.uniform(18.0, 45.0)
        d = np.hypot(X - sx, Z - sz)
        top = cliff_height[col] * rng.uniform(0.35, 0.8)
        heights = np.where(d < r, np.maximum(heights, top * np.clip(1.0 - (d / r) ** 6, 0.0, 1.0)), heights)
    print('cliff', f'{time.time() - started:.0f}s')

    # The mountains: the real ones, behind the field.
    aoraki, aoraki_m = load('D:/DE/build/terrain_raw/aoraki', 'aoraki')
    peak_row, peak_col = np.unravel_index(int(np.argmax(aoraki)), aoraki.shape)
    # A window as big as the map, its north edge where the range should be; the mountains wrap round the field like a
    # horseshoe, behind it and down both ends to the sea.
    width_m, depth_m = COLS * SPACING_M, ROWS * SPACING_M
    center_x, center_z = peak_col * aoraki_m + 1500.0, peak_row * aoraki_m - 1400.0 + depth_m / 2.0
    behind = window(aoraki, aoraki_m, center_x, center_z, COLS, ROWS)
    lowest_behind = float(np.percentile(behind, 2))
    # Where the 1 m data covers it, the vertices take their heights from that instead: detail down to 2 m, where the 4 m
    # data blown up holds none. Vertex i lies where the window's sample i does: 4 m sample k's centre is 2 m past 4 k,
    # and 1 m sample c's is half a meter past the 1 m data's own origin plus c.
    fine, fine_meta = fine_detail.load('D:/DE/build/terrain_raw/aoraki_1m', 'aoraki_1m')
    wc_extent = None
    # The big mountain as World Creator left it, where the user's export covers it (local, never committed): its own
    # units fitted back to metres against the real heights under it.
    if os.path.exists(WC_EXPORT) and os.path.exists(WC_CUT):
        import tifffile
        with open(WC_CUT, encoding='utf-8') as f:
            extent = json.load(f)['extent_nztm2000']
        with open('D:/DE/build/terrain_raw/aoraki/aoraki.json', encoding='utf-8') as f:
            box_west, box_north = json.load(f)['box_corners_nztm2000_en']['north_west']
        scale, offset, stray = fine_detail.paste_heights(fine, fine_meta, box_west, box_north, tifffile.imread(WC_EXPORT),
                                                         extent['E_min'], extent['N_max'])
        print('world creator', f'{scale:.3f} m a unit, {offset:.1f} m offset, {stray:.1f} m from the real heights (rms)')
        wc_extent = (extent, box_west, box_north)
    fine_col0 = center_x - COLS / 2.0 * SPACING_M + aoraki_m / 2.0 - fine_meta['origin_in_aoraki_box_m'][0] - 0.5
    fine_row0 = center_z - ROWS / 2.0 * SPACING_M + aoraki_m / 2.0 - fine_meta['origin_in_aoraki_box_m'][1] - 0.5
    fine_vertices = fine_detail.vertex_heights(fine, fine_col0, fine_row0, COLS, ROWS)
    behind = np.where(np.isfinite(fine_vertices), fine_vertices, behind)
    real_m = behind.copy()  # the real mountain's height at each place, which snowfall goes by
    behind = (behind - lowest_behind) * ALPINE_RELIEF

    def smooth(edge0, edge1, value):
        s = np.clip((value - edge0) / (edge1 - edge0), 0.0, 1.0)
        return s * s * (3.0 - 2.0 * s)

    # Behind the field, dwindling to hills toward the west; down to the sea at the east end.
    north = smooth(MOUNTAINS_FROM_Z_M + 500.0, MOUNTAINS_FROM_Z_M - 500.0, Z) * (0.25 + 0.75 * smooth(500.0, WEST_END_M + 1500.0, X))
    east = smooth(width_m - ENDS_M - 700.0, width_m - ENDS_M + 700.0, X)
    fade = np.maximum(north, east)
    # Past their ridges the land falls toward the map's own edge, where only a view from outside would see it end.
    to_edge_m = np.minimum(width_m - X, Z)
    fade = fade * (0.45 + 0.55 * smooth(0.0, 1300.0, to_edge_m))
    heights = heights + behind * fade
    mountain = fade
    print('mountains', round(float(heights.max())), f'{time.time() - started:.0f}s')

    # Grain on steep ground, the sea cliffs', but none on the mountains, even at their feet: they have their own, and in
    # thin snow every grain would decide snow or rock, a speckle.
    gz, gx = np.gradient(heights, SPACING_M)
    steep = np.clip((np.hypot(gx, gz) - 0.6) / 0.8, 0.0, 1.0) * (1.0 - np.clip(mountain * 20.0, 0.0, 1.0))
    heights = heights + (field_noise(6.0, 1) - 0.5) * 1.4 * steep
    heights = np.clip(heights, -60.0, 3800.0)
    print('heights', round(float(heights.min())), 'to', round(float(heights.max())), f'{time.time() - started:.0f}s')

    # Snow, in meters (snow.py). The scan already has the snow's surface in its heights, so it adds none.
    # Only where the mountains rise above the field, whose own climate is a golden summer: the real mountain's snow line
    # lies below its feet here. Where they meet the field the land runs straight, so the line wanders, or it would too.
    wander = (field_noise(400.0, 1) - 0.5) * 200.0 + (field_noise(90.0, 1) - 0.5) * 60.0
    above_field = np.clip((heights - SNOW_FROM_M - wander) / 200.0, 0.0, 1.0)
    # The real glaciers, where the mountains are the real ones: ice, which no slope bares (glaciers.py). Off: their
    # outlines are polygons of straight segments, and the snow drew their zig-zags.
    glacier = np.zeros_like(heights)
    if USE_GLACIERS:
        with open('D:/DE/build/terrain_raw/aoraki/aoraki.json', encoding='utf-8') as f:
            box_west, box_north = json.load(f)['box_corners_nztm2000_en']['north_west']
        glacier = glaciers.ice_map('D:/DE/build/terrain_raw/rgi60_nz/unpacked/18_rgi60_NewZealand.shp',
                                   box_west + center_x - COLS / 2.0 * SPACING_M + aoraki_m / 2.0,
                                   box_north - (center_z - ROWS / 2.0 * SPACING_M + aoraki_m / 2.0), SPACING_M, COLS, ROWS)
        glacier = glacier * np.clip(mountain * 2.5, 0.0, 1.0)
    snow_depth = snow.bake(heights, real_m, mountain * above_field * above_field * (3.0 - 2.0 * above_field), SPACING_M,
                           SNOW_LINE_REAL_M, SNOW_WIND_FROM_DEG, SNOW_SUNS, ICE_FROM_REAL_M, glacier)
    # How much of each 2 m square it covers, from its depth alone: the scan's 1 m ribs and hollows deciding snow or rock
    # drew its ripples as lines of rock (snow.coverage_detailed is there to bring them back).
    snow_cover = snow.coverage(snow_depth, SNOW_SHOWS_M)
    high = (real_m > 2000.0) & (mountain > 0.9)
    print('snow: rock above 2,000 m', f'{float(np.mean(snow_depth[high] < SNOW_SHOWS_M)):.0%} by depth,',
          f'{1.0 - float(np.mean(snow_cover[high])):.0%} by cover', f'{time.time() - started:.0f}s')

    out = wg.OUT
    folder = f'{out}/layers/masks'
    os.makedirs(folder, exist_ok=True)
    lowest, highest = -60.0, 3800.0
    pixels = ((heights - lowest) / (highest - lowest) * 65535.0 + 0.5).astype(np.uint16)
    Image.fromarray(pixels).save(f'{out}/{NAME}_tmp.png', format='PNG', compress_level=2)
    os.replace(f'{out}/{NAME}_tmp.png', f'{out}/{NAME}.heightpng')
    with open(f'{out}/{NAME}.terrain', 'w', encoding='utf-8') as f:
        json.dump({'height': f'{NAME}.heightpng', 'meters_per_pixel': SPACING_M, 'lowest_m': lowest, 'highest_m': highest}, f, indent=1)
    # Masks: golden everywhere (alpha); the rest unused.
    small = (2048, 1024)
    zero = Image.new('L', small, 0)
    Image.merge('RGBA', (zero, zero, zero, Image.new('L', small, 255))).save(f'{folder}/{NAME}_masks.png')
    with open(f'{folder}/{NAME}_masks.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'metallic_roughness': f'{NAME}_masks.png'}}, f, indent=1)
    # Details, at the heights' own 2 m: how much of each texel snow covers, no flow, scree (the cliff's foot, and the
    # mountains' bare ground above where grass gives out), the mountains as mountain.
    in_mountains = np.clip(mountain * 2.5, 0.0, 1.0)
    scree = np.maximum(apron, np.clip((real_m - (SNOW_LINE_REAL_M - 250.0)) / 200.0, 0.0, 1.0) * in_mountains)
    # Where the user's World Creator square lies, its own snow and debris, as its splat map has them, faded in from its
    # edges and only where the mountains are.
    if wc_extent is not None and os.path.exists(WC_SPLAT):
        import tifffile
        extent, box_west, box_north = wc_extent
        p0, q0 = fine_meta['origin_in_aoraki_box_m']
        wc_col0 = box_west + p0 + fine_col0 - extent['E_min']
        wc_row0 = extent['N_max'] - box_north + q0 + fine_row0
        splat = tifffile.imread(WC_SPLAT).astype(np.float32) / 255.0
        wc_snow, weight = fine_detail.at_vertices(splat[..., 0], wc_col0, wc_row0, COLS, ROWS, 200.0)
        wc_debris, _ = fine_detail.at_vertices(splat[..., 1], wc_col0, wc_row0, COLS, ROWS, 200.0)
        del splat
        weight = weight * in_mountains
        have = np.isfinite(wc_snow)
        snow_cover = np.where(have, snow_cover + (np.nan_to_num(wc_snow) - snow_cover) * weight, snow_cover)
        scree = np.where(have, scree + (np.nan_to_num(wc_debris) - scree) * weight, scree)
        print('world creator splat', f'snow {float(np.mean(wc_snow[weight > 0.99])):.0%} of its square in the mountains')
    channels = [Image.fromarray((np.clip(c, 0, 1) * 255.0 + 0.5).astype(np.uint8))
                for c in (snow_cover, np.zeros_like(heights), scree, in_mountains)]
    Image.merge('RGBA', channels).save(f'{folder}/{NAME}_details.png')
    with open(f'{folder}/{NAME}_details.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'metallic_roughness': f'{NAME}_details.png'}}, f, indent=1)
    four = np.asarray(Image.fromarray(heights).resize((COLS // 2, ROWS // 2), Image.BILINEAR), dtype=np.float32)
    lightbake.write_light_map(folder, NAME, four, SPACING_M * 2.0, SUN_AZIMUTH_DEG, SUN_ELEVATION_DEG)
    shore.write_shore_map(folder, NAME, heights, SPACING_M, 0.0)
    # The ground's normal at 1 m: the mesh's, with the mountains' 1 m detail back on it.
    size = fine_detail.write_normal_map(f'{folder}/{NAME}_normals_tmp.png', heights, SPACING_M, fine, fine_vertices, fine_col0,
                                        fine_row0, (mountain * ALPINE_RELIEF).astype(np.float32))
    os.replace(f'{folder}/{NAME}_normals_tmp.png', f'{folder}/{NAME}_normals.png')
    with open(f'{folder}/{NAME}_normals.material', 'w', encoding='utf-8') as f:
        json.dump({'textures': {'normal': f'{NAME}_normals.png'}}, f, indent=1)
    print('maps', size, f'{time.time() - started:.0f}s')

    write_ground(folder)
    with open(f'{out}/realm.environment', encoding='utf-8') as f:
        environment = json.load(f)
    environment['sun']['azimuth_deg'] = SUN_AZIMUTH_DEG
    environment['sun']['elevation_deg'] = SUN_ELEVATION_DEG
    environment['wind'] = {'toward_deg': 20, 'speed_mps': 5, 'gust_mps': 5, 'gust_size_m': 60, 'turbulence': 0.35}
    # A moderate swell rolling in from the south onto the cliffs, under the onshore wind's own chop.
    environment['sea'] = {'swell_height_m': 1.5, 'swell_period_s': 11, 'swell_from_deg': 190, 'fetch_km': 200, 'choppiness': 0.9}
    with open(f'{out}/{NAME}.environment', 'w', encoding='utf-8') as f:
        json.dump(environment, f, indent=1)

    def ground(px, pz):
        return float(heights[int(np.clip(pz / SPACING_M, 0, ROWS - 1)), int(np.clip(px / SPACING_M, 0, COLS - 1))])

    def view(name, px, pz, yaw, pitch, up=1.8):
        return f'{name} {px:.1f} {ground(px, pz) + up:.1f} {pz:.1f} {yaw:.1f} {pitch:.1f}'

    mid = COLS * SPACING_M / 2.0
    edge_mid = float(coast[int(mid / SPACING_M)])
    views = [
        view('start', 8124.0, 5157.1, 96.2, 12.4),                              # where the user chose: along the cliff, west
        view('sea', mid, edge_mid - 8.0, 165.0, -24.0),                         # at the cliff's edge, out to sea and down it
        view('cliff_along_east', mid - 300.0, edge_mid - 60.0, -70.0, -6.0),    # along the coast, its cliffs and headlands
        view('cliff_along_west', mid + 300.0, edge_mid - 60.0, 70.0, -6.0),
        view('mountains', mid, edge_mid - 300.0, 0.0, 6.0),                     # back over the field to the snow
        view('peak_user', 8124.0, 5157.1, 22.8, 23.4),                          # the user's look at the highest peak
        view('beach', mid + 1200.0, float(coast[int((mid + 1200.0) / SPACING_M)]) + 60.0, 0.0, 16.0, up=1.8),  # up the cliff from its foot
    ]
    views.append(f'air_coast {mid:.1f} 600.0 {ROWS * SPACING_M - 300:.1f} 0 -12')
    views.append(view('west_end', 4200.0, float(coast[int(4200.0 / SPACING_M)]) - 400.0, 100.0, 4.0))
    views.append(view('east_end', COLS * SPACING_M - 4200.0, float(coast[int((COLS * SPACING_M - 4200.0) / SPACING_M)]) - 400.0, -100.0, 4.0))
    views.append(f'air_high {mid:.1f} 2400.0 {ROWS * SPACING_M + 2500:.1f} 0 -24')
    # The user's views of the mountain, which the texturing passes are judged on.
    views += [view('m_front', 8124.0, 5157.1, 7.7, 27.4),   # from the cliff, up at the peak
              'm_air 7369.5 1857.4 7834.4 3.2 -7.7',        # from the air over the sea, the whole massif
              'm_ridge 10325.3 2704.3 1785.7 88.0 -21.3',   # high on the range, west along its snow
              'm_far 6692.0 1500.0 16000.0 0.0 1.5',        # 14 km off over the sea, as far as the photo
              'm_photo 6692.0 1500.0 16000.0 0.0 4.2']      # the same with a long lens (--fov 14), framed as the photo
    # The sea, from 10 m above it half a kilometer out: into the low sun, and away from it toward the cliffs.
    views += [f'sea_sun {mid:.1f} 10.0 {edge_mid + 500.0:.1f} 105.0 -3.0',
              f'sea_cliffs {mid:.1f} 10.0 {edge_mid + 500.0:.1f} -20.0 4.0']
    # From the cliff's edge on the headland west of the middle, down along the cliffs to their foot and the surf.
    views.append(view('cliff_down', 5600.0, float(coast[int(5600.0 / SPACING_M)]) - 12.0, -80.0, -28.0))
    views.append('shore_close 7984.4 61.1 5215.0 116.5 -53.6')  # the user's look down at the surf at the cliff's foot
    with open(f'{out}/{NAME}.views', 'w', encoding='utf-8') as f:
        f.write('\n'.join(views) + '\n')
    start = views[0].split()
    with open(f'{out}/{NAME}.world', 'w', encoding='utf-8') as f:
        json.dump({'environment': f'{NAME}.environment', 'terrain': f'{NAME}.terrain', 'terrain_layers': f'{NAME}.terrainlayers',
                   'sea': {'level_m': 0.0, 'shore': f'layers/masks/{NAME}_shore.material', 'foam': '../water/sea_foam.material'},
                   'start': {'position': [float(start[1]), float(start[2]), float(start[3])], 'yaw_deg': float(start[4]),
                             'pitch_deg': float(start[5])}, 'cells': []}, f, indent=1)
    print('\n'.join(views))
    print('done', f'{time.time() - started:.0f}s')


def write_ground(folder):
    """Golden field, pale layered cliffs, rocks and sand at their foot, the mountains' own dark rock and snow."""
    def layer(name, tile, **rules):
        out = {'material': f'layers/{name}/{name}.material', 'tile_m': tile}
        out.update(rules)
        return out

    stack = [
        layer('gravelly_sand', 3.0, variation=0.3, tint=[0.5, 0.78, 1.3]),
        layer('withered_grass', 2.0, height_m=[2, 8, 1500, 1800], slope_deg=[0, 0, 30, 38], noise=1.2, noise_large=0.6, variation=0.22, tint=[0.78, 0.72, 0.52]),
        layer('dry_ground_01', 4.0, height_m=[2, 8, 1500, 1800], slope_deg=[0, 0, 4, 12], noise=2.2, noise_large=0.8, variation=0.12, tint=[1.15, 1.05, 0.8]),
        layer('damp_beach_sand', 2.0, height_m=[-60, -60, 2.5, 9], slope_deg=[0, 0, 14, 24], noise=0.5, variation=0.08, tint=[2.2, 2.1, 1.9]),
        layer('rocks_ground_05', 2.5, slope_deg=[0, 0, 45, 55], noise=0.8, variation=0.14, tint=[0.85, 0.82, 0.78], detail='b', detail_range=[0.2, 0.6]),
        layer('cliff_side_grey', 15.0, slope_deg=[40, 50, 90, 90], projection='sides', noise=0.6, variation=0.14, tint=[1.7, 1.95, 2.0], detail='a', detail_range=[0.5, 0.15]),
        # The mountains' own rock: an albedo near 0.12, dark grey with a little blue (the texture's own is about 0.17).
        layer('rock_face_03_grey', 13.0, slope_deg=[28, 38, 90, 90], projection='sides', noise=0.6, variation=0.16, tint=[0.66, 0.72, 0.82], detail='a', detail_range=[0.15, 0.5]),
        # Snow where it covers the ground: up close, a sharp edge (the renderer keeps it a pixel wide); far off, the share
        # of the ground it covers, so thin ribs of rock still show (detail_coverage), which noise would only bend. An
        # albedo near 0.85 (the texture's own is about 0.37). The bake already decided what's too steep for it.
        layer('snow_02', 2.0, slope_deg=[0, 0, 80, 88], variation=0.04, tint=[2.25, 2.27, 2.32], detail='r',
              detail_range=[0.45, 0.55], detail_coverage=True),
    ]
    with open(f'{wg.OUT}/{NAME}.terrainlayers', 'w', encoding='utf-8') as f:
        json.dump({'masks': f'layers/masks/{NAME}_masks.material', 'details': f'layers/masks/{NAME}_details.material',
                   'lighting': f'layers/masks/{NAME}_light.material', 'normals': f'layers/masks/{NAME}_normals.material',
                   'blend': 0.28, 'layers': stack,
                   'grass': {'density_per_m2': 300, 'height_m': [0.22, 0.62], 'width_m': 0.024, 'where_height_m': [2, 8, *GRASS_TO_M],
                             'where_slope_deg': [0, 0, 28, 38], 'patches': 0.2, 'base': [0.1, 0.06, 0.02], 'tip': [0.36, 0.24, 0.08],
                             'other': {'mask': 'a', 'base': [0.1, 0.06, 0.02], 'tip': [0.38, 0.26, 0.09]}}}, f, indent=1)


if __name__ == '__main__':
    main()
