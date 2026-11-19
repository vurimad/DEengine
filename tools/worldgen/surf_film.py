"""The surf's film: a strip of beach running out to sea, simulated offline and baked into a texture the water plays
back along every shore (shaders/scene/surf_film.slang reads it; docs/design.md, Water). Across the texture, the place
across the shore; down it, the time, round a loop of a few waves.

The strip is the shallow water equations in one dimension, cells a few centimeters across: waves come in at its seaward
end as the swell does where it's about to break, a set of them, each its own height; they steepen, break into bores,
cross the surf zone, run up the beach as a thin sheet and drain back, each meeting the last one's backwash. In one
dimension and offline that's cheap at a fineness a game's frame couldn't pay for, and what comes out has what a drawn
wave lacks: the pace (bores slow and bunch as it shallows), the run-up's reach and timing, and the sets' slow surge.

The film is for a wave kFilmWaveM high on a beach of slope kFilmSlope; the water scales it to the sea's own waves
(lengths by their height over kFilmWaveM, times by that's square root: the equations don't change under it).

    r: the surface's height about the sea's level, from HEIGHT_LOW_M to HEIGHT_HIGH_M
    g: the water's depth where it's thin, as the square root of its share of THIN_M (fine near none): 0 where it's dry
    b: foam, 0 to 1: made where bores break, carried by the flow, dying away
    a: the surface's slope across the shore (rising seaward positive), from -SLOPE_MOST to SLOPE_MOST

It's read filtered, so it isn't split over bytes, and it isn't compressed (the "*_shore.*" rule of a project's meta).

Beside it, the same film rounded off across the shore (<name>_filmsoft_shore.png, the material's emissive texture): a
bore's step spread over a few meters into a hump, and in its foam channel how much foam there is around (the foam,
spread as far). The water draws a front as the step it is only where it's breaking, with foam on and around it; a
front with none (a stretch of a wave that isn't breaking, a surge up the beach, the backwash meeting the sea) it
draws from this, rounded: a step in clear water is a hard line, which water doesn't have.

And a third (<name>_filmflow_shore.png, the material's occlusion texture): in r, how fast the water is moving across
the shore, seaward positive, from -FLOW_MOST_MPS to FLOW_MOST_MPS. The water slides its foam's pattern along with it,
so foam rides in behind a bore and is drawn back out with the backwash; the pattern fixed to the ground, only how
much of it shows moving, is a texture laid over the water.

    python tools/worldgen/surf_film.py <folder> <name>     writes them there, into <name>_shore.material
    python tools/worldgen/surf_film.py [folder]            previews, for looking at
"""
import json
import math
import os
import sys
import time

import numba
import numpy as np
from PIL import Image

# What surf_film.slang must match.
FILM_SIDE = 1024          # texels each way
FILM_SLOPE = 1.0 / 22.0   # the seabed's: steep enough that the waves break within a few tens of meters of the sand and
                          # their bores reach it still knee-high. As flat as 1 in 40, they break 90 m out, the last
                          # 60 m is ankle-high white water, and from the beach there's nothing to see
FACE_SLOPE = 1.0 / 16.0   # and above the waterline, the beach's face: steeper, as beaches are, so each wave's run-up
                          # turns back in a few seconds. As flat as the seabed, a run-up hasn't drained before the next
                          # wave is on it, and the water's edge goes up and down once a set, not once a wave
FACE_BLEND_M = 3.0        # either side of the waterline, the one slope eases into the other
FILM_WAVE_M = 1.0         # the waves' height where they come in
FILM_LOOP_S = 51.2        # the loop: LOOP_WAVES waves
FILM_LAND_M = 35.0        # how far up the beach it reaches
FILM_SEA_M = 220.0        # and how far out to sea
FILM_SHORE_U = 0.35       # where the waterline is, across the texture
FILM_STRETCH = 5.5        # how much finer it is at the waterline than at its ends (film_place)
HEIGHT_LOW_M, HEIGHT_HIGH_M = -0.5, 2.0
THIN_M = 0.5
SLOPE_MOST = 2.0
FLOW_MOST_MPS = 4.0

GRAVITY = 9.81
LOOP_WAVES = 6
WAVE_SHARES = (0.4, 0.8, 1.0, 0.92, 0.66, 0.3)          # a set: each wave's share of the height, and a lull after it
WAVE_LATE = (0.0, 0.06, -0.05, 0.04, -0.07, 0.05)       # and how late each is, in periods: never a metronome
SIM_SEA_M = 70.0          # the simulated strip's seaward end; past it the swell is drawn as it comes in
CELL_M = 0.04
FRICTION = 0.012
DRY_M = 0.0005
SEEP_MPS = 0.02           # how fast the beach's face takes a thin sheet's water in: what's left at the top of a run-up
                          # sinks into the sand rather than hanging there till it has all drained back
SEEP_BELOW_M = 0.06       # sheets thinner than this
FOAM_LIFE_S = 3.0
THIN_FOAM_LIFE = 0.6      # of that, on water with no depth: a thin sheet's foam goes sooner, into the sand, so the
                          # run-up drains back clear and the water's edge isn't white all the time
THIN_FOAM_M = 0.5         # from this deep, the whole of it
FOAM_OUT_M = 12.0         # past the strip's seaward end the foam it has there dies away over this far: none at sea
SPIN_UP_LOOPS = 3
SOFT_M = 2.5              # how far the rounded film spreads a bore's step, either way
FOAM_EDGE_M = 0.3         # how far foam is spread across the shore: a front's white starts within a step or two
FLOW_EDGE_M = 0.8         # and the flow: across a bore's front it turns from the backwash to the bore's own within a
                          # step, and a pattern slid by that is torn along the front

# The breaker's shape. The shallow water equations take a swell straight from a round hump to a step with a long
# back (a bore), losing height all the way: they have no wave that rears up. A real one, coming to where it breaks,
# grows, its crest sharpens and runs ahead of its base, and the water in front is drawn down into its face: a tall
# peak over a hollow, steep front, which then falls into the bore. So the simulated surface is shaped, most where
# the waves break and not at all in the shallows, where the bores are the simulation's (after Guerrilla's finding
# that a breaking wave's section out of a simulation needed shaping by hand; ours is by rule):
SHAPE_DEPTH_M = (0.0, 0.7, 1.1, 1.3, 1.45, 1.6, 2.0, 2.75, 5.5)   # where the still water is this deep:
SHAPE_WEIGHT = (0.0, 0.0, 0.15, 0.5, 1.0, 1.0, 0.5, 0.25, 0.1)    # how much of the shaping
SHAPE_GROW = 0.35         # how much higher, of its height
SHAPE_LEAN_M = 4.0        # how far its top runs ahead of its base, per meter of its height
SHAPE_SHARP = 0.4         # how much of what stands out from the water around it is added again: a sharper crest,
SHAPE_SHARP_M = 5.0       # and a hollow in front of it; around it: within about this far
SHAPE_TROUGH = 0.3        # of the still water's depth, the lowest a hollow goes
SHAPE_STEP_M = 0.1


def film_place(u):
    """Where a place across the texture (0 to 1) is, in meters seaward of the waterline: fine at the waterline, coarser
    toward either end."""
    a = FILM_SEA_M / math.sinh(FILM_STRETCH * (1.0 - FILM_SHORE_U))
    return a * np.sinh(FILM_STRETCH * (u - FILM_SHORE_U))


def film_bed(place):
    """The beach's height about the sea's level at places seaward of the waterline (meters; up the beach negative):
    FILM_SLOPE out to sea, FACE_SLOPE up its face, the one eased into the other round the waterline, where it's 0."""
    fine = np.linspace(-FILM_LAND_M - 1.0, FILM_SEA_M + 1.0, 60001)
    t = np.clip((fine + FACE_BLEND_M) / (2.0 * FACE_BLEND_M), 0.0, 1.0)
    slope = FACE_SLOPE + (FILM_SLOPE - FACE_SLOPE) * t
    height = -np.concatenate([[0.0], np.cumsum(0.5 * (slope[1:] + slope[:-1]) * np.diff(fine))])
    height -= np.interp(0.0, fine, height)
    return np.interp(place, fine, height)


def incoming(t):
    """The surface's height where the waves come in, at times t (arrays): a train of single crests, each as long as a
    lone wave of its height is on that depth, about a mean of none."""
    period = FILM_LOOP_S / LOOP_WAVES
    depth = SIM_SEA_M * FILM_SLOPE
    out = np.zeros_like(t)
    mean = 0.0
    for i in range(LOOP_WAVES):
        height = FILM_WAVE_M * WAVE_SHARES[i]
        length_s = math.sqrt(4.0 * depth ** 3 / (3.0 * height)) / math.sqrt(GRAVITY * (depth + height))
        at = (i + 0.5 + WAVE_LATE[i]) * period
        for wrap in (-1, 0, 1):
            out += height / np.cosh((t - at - wrap * FILM_LOOP_S) / length_s) ** 2
        mean += height * 2.0 * length_s / FILM_LOOP_S
    return out - mean


@numba.njit(cache=True)
def _hll(hl, ul, hr, ur):
    """The flow of water and of momentum between two states (Harten, Lax and van Leer)."""
    cl = math.sqrt(GRAVITY * hl)
    cr = math.sqrt(GRAVITY * hr)
    sl = min(ul - cl, ur - cr)
    sr = max(ul + cl, ur + cr)
    fl_h = hl * ul
    fl_q = hl * ul * ul + 0.5 * GRAVITY * hl * hl
    fr_h = hr * ur
    fr_q = hr * ur * ur + 0.5 * GRAVITY * hr * hr
    if sl >= 0.0:
        return fl_h, fl_q
    if sr <= 0.0:
        return fr_h, fr_q
    if sr - sl < 1e-9:
        return 0.0, 0.5 * (fl_q + fr_q)
    return ((sr * fl_h - sl * fr_h + sl * sr * (hr - hl)) / (sr - sl),
            (sr * fl_q - sl * fr_q + sl * sr * (hr * ur - hl * ul)) / (sr - sl))


@numba.njit(cache=True)
def _run(bed, wave, wave_dt, dt, steps, start_step, every, surface_out, depth_out, foam_out, flow_out):
    """Steps the strip (cell 0 up the beach, the last where the waves come in). wave: the incoming surface over one
    loop, at wave_dt apart. From start_step on, every every-th step's surface, depth, foam and flow (the water's speed,
    toward the last cell positive) go into the outs' rows."""
    n = bed.shape[0]
    h = np.maximum(-bed, 0.0)
    q = np.zeros(n)
    foam = np.zeros(n)
    u = np.zeros(n)
    flow_h = np.zeros(n + 1)
    flow_ql = np.zeros(n + 1)  # the momentum across face i, as the cell on its left (i - 1) feels it
    flow_qr = np.zeros(n + 1)  # and as the one on its right (i) does
    new_foam = np.zeros(n)
    deep = -bed[n - 1]
    c0 = math.sqrt(GRAVITY * deep)
    row = 0
    for step in range(steps):
        t = step * dt
        # The seaward end: the wave coming in (moving toward cell 0) on the characteristic that comes with it, what
        # the strip sends back out on the other.
        at = (t / wave_dt) % wave.shape[0]
        i0 = int(at)
        frac = at - i0
        eta = wave[i0] * (1.0 - frac) + wave[(i0 + 1) % wave.shape[0]] * frac
        c_in = math.sqrt(GRAVITY * max(deep + eta, 0.01))
        u_in = -2.0 * (c_in - c0)
        r_minus = u_in - 2.0 * c_in
        u_last = q[n - 1] / h[n - 1] if h[n - 1] > DRY_M else 0.0
        r_plus = u_last + 2.0 * math.sqrt(GRAVITY * h[n - 1])
        ghost_c = 0.25 * (r_plus - r_minus)
        ghost_h = ghost_c * ghost_c / GRAVITY
        ghost_u = 0.5 * (r_plus + r_minus)

        for i in range(n):
            u[i] = q[i] / h[i] if h[i] > DRY_M else 0.0
        # Faces: i between cells i - 1 and i. The water each side reaches the face with is what stands above the
        # higher bed (Audusse's hydrostatic reconstruction): still water stays still, and none flows uphill dry.
        flow_h[0] = 0.0
        flow_ql[0] = 0.0
        flow_qr[0] = 0.5 * GRAVITY * h[0] * h[0]
        for i in range(1, n + 1):
            if i < n:
                top = max(bed[i - 1], bed[i])
                hl = max(h[i - 1] + bed[i - 1] - top, 0.0)
                hr = max(h[i] + bed[i] - top, 0.0)
                fh, fq = _hll(hl, u[i - 1], hr, u[i])
                flow_h[i] = fh
                flow_ql[i] = fq + 0.5 * GRAVITY * (h[i - 1] * h[i - 1] - hl * hl)
                flow_qr[i] = fq + 0.5 * GRAVITY * (h[i] * h[i] - hr * hr)
            else:
                fh, fq = _hll(h[n - 1], u[n - 1], ghost_h, ghost_u)
                flow_h[i] = fh
                flow_ql[i] = fq
                flow_qr[i] = fq
        scale = dt / CELL_M
        for i in range(n):
            h_new = h[i] - scale * (flow_h[i + 1] - flow_h[i])
            q_new = q[i] - scale * (flow_ql[i + 1] - flow_qr[i])
            if h_new <= DRY_M:
                h[i] = max(h_new, 0.0)
                q[i] = 0.0
            else:
                # The bed's drag, taken at the step's end so a thin sheet can't be turned round by it.
                speed = abs(q_new / h_new)
                q_new /= 1.0 + dt * FRICTION * speed / h_new
                if bed[i] > 0.0 and h_new < SEEP_BELOW_M:
                    left = max(h_new - SEEP_MPS * dt, 0.0)
                    q_new *= left / h_new
                    h_new = left
                h[i] = h_new
                q[i] = q_new
        # Foam: carried from where each cell's water was, dying away, made where the water piles into itself (a
        # bore's front squeezes it many times harder than any unbroken wave does).
        for i in range(n):
            v = q[i] / h[i] if h[i] > DRY_M else 0.0
            back = i - v * dt / CELL_M
            back = min(max(back, 0.0), n - 1.0)
            j = int(back)
            w = back - j
            carried = foam[j] * (1.0 - w) + foam[min(j + 1, n - 1)] * w
            made = 0.0
            if 0 < i < n - 1 and h[i] > 0.02:
                left = q[i - 1] / h[i - 1] if h[i - 1] > DRY_M else 0.0
                right = q[i + 1] / h[i + 1] if h[i + 1] > DRY_M else 0.0
                squeeze = -(right - left) / (2.0 * CELL_M)
                made = min(max((squeeze - 1.2) / 3.0, 0.0), 1.0) * 25.0 * dt
            thin = 1.0 - min(h[i] / THIN_FOAM_M, 1.0)
            life = FOAM_LIFE_S * (1.0 + (THIN_FOAM_LIFE - 1.0) * thin)
            new_foam[i] = min(carried * math.exp(-dt / life) + made, 1.0)
        for i in range(n):
            foam[i] = new_foam[i]
        if step >= start_step and (step - start_step) % every == 0 and row < surface_out.shape[0]:
            for i in range(n):
                wet = h[i] > DRY_M
                surface_out[row, i] = h[i] + bed[i] if wet else bed[i]
                depth_out[row, i] = h[i] if wet else 0.0
                foam_out[row, i] = foam[i]
                flow_out[row, i] = q[i] / h[i] if wet else 0.0
            row += 1
    return row


def _blur(a, sigma_cells):
    """a's rows, each smoothed along itself."""
    reach = int(3 * sigma_cells) + 1
    kernel = np.exp(-0.5 * (np.arange(-reach, reach + 1) / sigma_cells) ** 2)
    kernel /= kernel.sum()
    padded = np.pad(a, ((0, 0), (reach, reach)), mode='edge')
    out = np.zeros_like(a)
    for k, weight in enumerate(kernel):
        out += weight * padded[:, k:k + a.shape[1]]
    return out


def simulate():
    """The film's fields over the simulated strip: surface, depth and foam, rows of time (FILM_SIDE of them, and as
    many again before the loop for the join), cells from FILM_LAND_M up the beach out to SIM_SEA_M."""
    n = int(round((FILM_LAND_M + SIM_SEA_M) / CELL_M))
    place = -FILM_LAND_M + (np.arange(n) + 0.5) * CELL_M  # seaward of the waterline
    bed = film_bed(place)
    fastest = math.sqrt(GRAVITY * (SIM_SEA_M * FILM_SLOPE + 1.5 * FILM_WAVE_M)) + 3.0
    row_dt = FILM_LOOP_S / FILM_SIDE
    every = int(math.ceil(row_dt / (0.4 * CELL_M / fastest)))
    dt = row_dt / every
    wave_dt = dt
    wave = incoming((np.arange(FILM_SIDE * every) + 0.0) * wave_dt)
    lead = FILM_SIDE // 16  # rows kept from before the loop, to join its end to its start
    loop_steps = FILM_SIDE * every
    start = SPIN_UP_LOOPS * loop_steps - lead * every
    rows = FILM_SIDE + lead
    surface = np.zeros((rows, n))
    depth = np.zeros((rows, n))
    foam = np.zeros((rows, n))
    flow = np.zeros((rows, n))
    written = _run(bed, wave, wave_dt, dt, start + rows * every, start, every, surface, depth, foam, flow)
    assert written == rows, written

    def join(a):
        # The loop's last rows eased into the ones that led up to its first: after the spin-up they're nearly the same.
        out = a[lead:].copy()
        ease = (np.arange(lead) + 0.5) / lead
        out[-lead:] = (1.0 - ease[:, None]) * out[-lead:] + ease[:, None] * a[:lead]
        return out

    global _flow
    _flow = join(flow)
    return place, bed, join(surface), join(depth), join(foam), row_dt


def _shaped(at, surface, depth):
    """The film's surface and depth with the breaker's shape (SHAPE_*): rows of time, places at."""
    fine = np.arange(at[0], at[-1], SHAPE_STEP_M)
    still = np.maximum(-film_bed(fine), 0.0)
    weight = np.interp(still, SHAPE_DEPTH_M, SHAPE_WEIGHT)
    plain = np.stack([np.interp(fine, at, row) for row in surface])
    # Higher; its top ahead of its base (each point takes the height from farther out to sea, the higher it is: the
    # crest comes shoreward, the face steepens, the back stretches); sharper, with a hollow in front.
    shaped = plain * (1.0 + SHAPE_GROW * weight)
    shaped = np.stack([np.interp(fine + SHAPE_LEAN_M * weight * np.maximum(row, 0.0), fine, row) for row in shaped])
    shaped = shaped + SHAPE_SHARP * weight * (shaped - _blur(shaped, SHAPE_SHARP_M / SHAPE_STEP_M))
    shaped = np.maximum(shaped, -SHAPE_TROUGH * still)
    # Only what the shaping changed goes back onto the film's own places: the rest stays as fine as it was.
    shaped_there = np.interp(np.maximum(-film_bed(at), 0.0), SHAPE_DEPTH_M, SHAPE_WEIGHT) > 0.0
    change = np.stack([np.interp(at, fine, row) for row in shaped - plain]) * shaped_there
    return surface + change, np.where(depth > 0.0, np.maximum(depth + change, 0.002), depth)


_strip = None
_flow = None  # the last simulate()'s flow: rows of time, the strip's cells


def _simulated():
    global _strip
    if _strip is None:
        _strip = simulate()
    return _strip


def bake(soft_m=0.0):
    """The film: FILM_SIDE rows of time, FILM_SIDE places across the shore, four fields (see the top). soft_m: rounded
    off across the shore by that much, its foam too (the film's second texture)."""
    place, bed, surface, depth, foam, row_dt = _simulated()
    # A bore's front is a jump a few cells wide: rounded to what the water's mesh can carry.
    surface = _blur(surface, max(0.2, soft_m) / CELL_M)
    if soft_m > 0.0:
        depth = _blur(depth, soft_m / CELL_M)
    foam = _blur(foam, max(FOAM_EDGE_M, soft_m) / CELL_M)
    at = film_place((np.arange(FILM_SIDE) + 0.5) / FILM_SIDE)
    in_sim = at <= place[-1]
    rows = np.arange(FILM_SIDE)
    film_surface = np.zeros((FILM_SIDE, FILM_SIDE))
    film_depth = np.zeros((FILM_SIDE, FILM_SIDE))
    film_foam = np.zeros((FILM_SIDE, FILM_SIDE))
    for r in rows:
        film_surface[r] = np.interp(at, place, surface[r])
        film_depth[r] = np.interp(at, place, depth[r])
        film_foam[r] = np.interp(at, place, foam[r])
    # Past the strip's end, the swell as it comes in: the same train, earlier by the time it takes to get there (at
    # the speed its depth gives, no deeper than 8 m), lower the farther out, down to nothing at the film's edge.
    out_there = at[~in_sim]
    if out_there.size:
        fine = np.linspace(place[-1], FILM_SEA_M, 2000)
        speed = np.sqrt(GRAVITY * np.minimum(fine * FILM_SLOPE, 8.0))
        travel = np.concatenate([[0.0], np.cumsum(np.diff(fine) / (0.5 * (speed[1:] + speed[:-1])))])
        ahead_s = np.interp(out_there, fine, travel)
        share = 1.0 - np.clip((out_there - place[-1]) / (FILM_SEA_M - place[-1]), 0.0, 1.0) ** 2
        t = rows[:, None] * row_dt
        # What the strip's end shows a moment before joins it to the drawn swell.
        edge = surface[:, -1]
        for k in range(out_there.size):
            shifted = np.interp((t[:, 0] + ahead_s[k]) % FILM_LOOP_S, t[:, 0], edge, period=FILM_LOOP_S)
            film_surface[:, np.nonzero(~in_sim)[0][k]] = shifted * share[k]
        film_depth[:, ~in_sim] = THIN_M
        film_foam[:, ~in_sim] *= np.clip(1.0 - (out_there - place[-1]) / FOAM_OUT_M, 0.0, 1.0)[None, :]
    film_surface, film_depth = _shaped(at, film_surface, film_depth)
    # The surface's slope across the shore, where there's water.
    slope = np.gradient(film_surface, at, axis=1)
    slope = np.where(film_depth > 0.002, slope, -FILM_SLOPE)
    return at, film_surface, film_depth, film_foam, slope


def bake_flow():
    """The film's flow: FILM_SIDE rows of time, FILM_SIDE places across the shore; meters a second, seaward positive.
    None past the strip's seaward end, where the swell is drawn."""
    place = _simulated()[0]
    flow = _blur(_flow, FLOW_EDGE_M / CELL_M)
    at = film_place((np.arange(FILM_SIDE) + 0.5) / FILM_SIDE)
    out = np.stack([np.interp(at, place, row) for row in flow])
    out[:, at > place[-1]] = 0.0
    return out


def encode(film_surface, film_depth, film_foam, slope):
    def byte(x):
        return (np.clip(x, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    return [byte((film_surface - HEIGHT_LOW_M) / (HEIGHT_HIGH_M - HEIGHT_LOW_M)),
            byte(np.sqrt(np.clip(film_depth / THIN_M, 0.0, 1.0))),
            byte(film_foam),
            byte(slope / SLOPE_MOST * 0.5 + 0.5)]


def write_surf_film(folder, name):
    """Writes <name>_film_shore.png, <name>_filmsoft_shore.png and <name>_filmflow_shore.png into folder. Returns the
    shore material's textures they are: {slot: file name}."""
    textures = {}
    for slot, label, soft_m in (('base_color', 'film', 0.0), ('emissive', 'filmsoft', SOFT_M)):
        _, film_surface, film_depth, film_foam, slope = bake(soft_m)
        channels = encode(film_surface, film_depth, film_foam, slope)
        image = Image.merge('RGBA', [Image.fromarray(c) for c in channels])
        image.save(f'{folder}/{name}_{label}_tmp.png')
        os.replace(f'{folder}/{name}_{label}_tmp.png', f'{folder}/{name}_{label}_shore.png')
        textures[slot] = f'{name}_{label}_shore.png'
    flow = (np.clip(bake_flow() / FLOW_MOST_MPS * 0.5 + 0.5, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    none, all_of_it = np.zeros_like(flow), np.full_like(flow, 255)
    Image.merge('RGBA', [Image.fromarray(c) for c in (flow, none, none, all_of_it)]).save(f'{folder}/{name}_filmflow_tmp.png')
    os.replace(f'{folder}/{name}_filmflow_tmp.png', f'{folder}/{name}_filmflow_shore.png')
    textures['occlusion'] = f'{name}_filmflow_shore.png'
    return textures


def preview(folder):
    """For looking at: the film as a picture (time down, the beach to the left), and the strip seen from the side at
    a few moments."""
    at, film_surface, film_depth, film_foam, _ = bake()
    os.makedirs(folder, exist_ok=True)
    shade = np.clip((film_surface - HEIGHT_LOW_M) / (HEIGHT_HIGH_M - HEIGHT_LOW_M), 0.0, 1.0)
    picture = np.stack([shade * 0.6 + film_foam * 0.4, shade * 0.75 + film_foam * 0.25,
                        np.where(film_depth > 0.002, 0.55 + 0.45 * shade, shade * 0.4)], axis=2)
    Image.fromarray((np.clip(picture, 0.0, 1.0) * 255.0).astype(np.uint8)).save(f'{folder}/film.png')
    # Side views: 24 moments over the first two waves, the last 60 m to the beach.
    width, height, panels = 900, 110, 24
    sheet = np.full((height * panels, width, 3), 255, np.uint8)
    xs = np.linspace(-25.0, 60.0, width)
    ground = film_bed(xs)
    for p in range(panels):
        row = int(p * (2.0 / LOOP_WAVES) * FILM_SIDE / panels)
        surface = np.interp(xs, at, film_surface[row])
        depth = np.interp(xs, at, film_depth[row])
        foam = np.interp(xs, at, film_foam[row])
        for x in range(width):
            def y_of(z):
                return int(np.clip(height - 12 - (z + 1.6) * (height - 24) / 3.0, 0, height - 1))
            g = y_of(ground[x])
            sheet[p * height + g:(p + 1) * height, x] = (194, 160, 110)
            if depth[x] > 0.002:
                s = y_of(surface[x])
                color = (60 + int(180 * foam[x]), 130 + int(110 * foam[x]), 200 + int(50 * foam[x]))
                sheet[p * height + min(s, g):p * height + g + 1, x] = color
    Image.fromarray(sheet).save(f'{folder}/film_side.png')
    wet = film_depth > 0.002
    reach = np.array([at[np.argmax(row)] if row.any() else 0.0 for row in wet])
    print('run-up: the waterline from', f'{-reach.min():.1f} m up the beach to {reach.max():.1f} m down it;',
          f'highest surface {film_surface.max():.2f} m, lowest {film_surface.min():.2f} m')


def main():
    started = time.time()
    if len(sys.argv) < 3:
        preview(sys.argv[1] if len(sys.argv) > 1 else 'build/agent/film')
        print('preview', f'{time.time() - started:.0f}s')
        return
    folder, name = sys.argv[1], sys.argv[2]
    films = write_surf_film(folder, name)
    material = f'{folder}/{name}_shore.material'
    with open(material, encoding='utf-8') as f:
        described = json.load(f)
    described['textures'].update(films)
    with open(material, 'w', encoding='utf-8') as f:
        json.dump(described, f, indent=1)
    print('film', ', '.join(films.values()), f'{time.time() - started:.0f}s')


if __name__ == '__main__':
    main()
