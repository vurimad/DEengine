"""Erosion for the generator: rivers carving a range out of rock that rises under it, rain cutting gullies, rockfall
building scree under cliffs, and snow sliding off whatever is too steep to hold it. Offline, so the game pays nothing.

Methods, from public research, in our own code:
- Large shapes: the stream power law with uplift, solved implicitly along the flow (Braun and Willett 2013), as Cordonnier
  et al. 2016 use it to make terrain. Water is routed by steepest descent over heights with their pits filled (priority
  flood with a small rise, Barnes et al. 2014), so it always reaches an outlet.
- Gullies: droplets that pick up sediment by their speed, the slope and their water, and drop it when over capacity (the
  common particle method, as in Beyer 2015).
- Scree and snow: material that moves downhill while a slope is past its angle of repose.

Everything works on float32 grids in meters, rows along Z (south), columns along X (east). Needs numpy and numba.
"""
import math
import site
import sys

sys.path.append(site.getusersitepackages())
import numba  # noqa: E402
import numpy as np  # noqa: E402

# The PC is shared: no more than 12 threads.
numba.set_num_threads(min(12, numba.config.NUMBA_NUM_THREADS))

_DR = np.array([-1, -1, -1, 0, 0, 1, 1, 1], dtype=np.int64)
_DC = np.array([-1, 0, 1, -1, 1, -1, 0, 1], dtype=np.int64)
_DD = np.array([math.sqrt(2.0), 1.0, math.sqrt(2.0), 1.0, 1.0, math.sqrt(2.0), 1.0, math.sqrt(2.0)])


# --- Rivers carving a range ---------------------------------------------------------------------------------------------


@numba.njit(cache=True)
def _fill(h, fixed, rows, cols, rise):
    """Priority flood: from the outlets inward, lowest first, raising each node to just above the one that reached it."""
    n = rows * cols
    out = h.copy()
    closed = np.zeros(n, dtype=np.bool_)
    heap_v = np.empty(n, dtype=np.float64)
    heap_i = np.empty(n, dtype=np.int64)
    size = 0
    for i in range(n):
        if fixed[i]:
            closed[i] = True
            # Push.
            k = size
            size += 1
            while k > 0:
                p = (k - 1) >> 1
                if heap_v[p] <= out[i]:
                    break
                heap_v[k] = heap_v[p]
                heap_i[k] = heap_i[p]
                k = p
            heap_v[k] = out[i]
            heap_i[k] = i
    while size > 0:
        v = heap_v[0]
        i = heap_i[0]
        # Pop.
        size -= 1
        lv = heap_v[size]
        li = heap_i[size]
        k = 0
        while True:
            c = 2 * k + 1
            if c >= size:
                break
            if c + 1 < size and heap_v[c + 1] < heap_v[c]:
                c += 1
            if heap_v[c] >= lv:
                break
            heap_v[k] = heap_v[c]
            heap_i[k] = heap_i[c]
            k = c
        heap_v[k] = lv
        heap_i[k] = li
        r = i // cols
        c0 = i - r * cols
        for d in range(8):
            rr = r + _DR[d]
            cc = c0 + _DC[d]
            if rr < 0 or rr >= rows or cc < 0 or cc >= cols:
                continue
            j = rr * cols + cc
            if closed[j]:
                continue
            closed[j] = True
            if out[j] <= v + rise:
                out[j] = v + rise
            k = size
            size += 1
            while k > 0:
                p = (k - 1) >> 1
                if heap_v[p] <= out[j]:
                    break
                heap_v[k] = heap_v[p]
                heap_i[k] = heap_i[p]
                k = p
            heap_v[k] = out[j]
            heap_i[k] = j
    return out


@numba.njit(parallel=True, cache=True)
def _receivers(hf, fixed, rows, cols, cell_m, seed):
    """Where each node's water goes: a downhill neighbour, picked at random in proportion to how steep the way to it is.
    Always the steepest locks rivers to the grid's eight directions; picked afresh every step, they wander as real ones
    do, and only the average over steps matters."""
    np.random.seed(seed)
    n = rows * cols
    recv = np.arange(n)
    dist = np.full(n, cell_m)
    for r in numba.prange(rows):
        for c in range(cols):
            i = r * cols + c
            if fixed[i]:
                continue
            total = 0.0
            for d in range(8):
                rr = r + _DR[d]
                cc = c + _DC[d]
                if rr < 0 or rr >= rows or cc < 0 or cc >= cols:
                    continue
                s = (hf[i] - hf[rr * cols + cc]) / (cell_m * _DD[d])
                if s > 0.0:
                    total += s
            pick = np.random.random() * total
            for d in range(8):
                rr = r + _DR[d]
                cc = c + _DC[d]
                if rr < 0 or rr >= rows or cc < 0 or cc >= cols:
                    continue
                j = rr * cols + cc
                length = cell_m * _DD[d]
                s = (hf[i] - hf[j]) / length
                if s > 0.0:
                    recv[i] = j
                    dist[i] = length
                    pick -= s
                    if pick <= 0.0:
                        break
    return recv, dist


@numba.njit(cache=True)
def _area(order, recv, cell_area):
    n = order.size
    area = np.full(n, cell_area)
    for k in range(n - 1, -1, -1):
        i = order[k]
        j = recv[i]
        if j != i:
            area[j] += area[i]
    return area


@numba.njit(cache=True)
def _stream_power(h, order, recv, dist, area, uplift, k_dt, m, dt, talus):
    for k in range(order.size):
        i = order[k]
        j = recv[i]
        if j == i:
            continue
        f = k_dt[i] * area[i] ** m / dist[i]
        hi = (h[i] + uplift[i] * dt + f * h[j]) / (1.0 + f)
        # Nothing stands steeper than rock can, above where its water goes.
        limit = h[j] + talus[i] * dist[i]
        h[i] = hi if hi < limit else limit


def carve_range(h, fixed, uplift, hardness, cell_m, steps, dt, k=8.8e-3, m=0.45, talus=1.0):
    """Rock rising by `uplift` (m per unit of time) where `fixed` is False, worn down by the water that crosses it; fixed
    nodes are where water leaves and stay as they are. `hardness` scales how easily rock wears, around 1. Returns the
    heights and how much land drains through each node, in m^2."""
    rows, cols = h.shape
    hf = h.astype(np.float64).ravel().copy()
    fx = fixed.ravel().copy()
    fx[:cols] = True
    fx[-cols:] = True
    fx[::cols] = True
    fx[cols - 1::cols] = True
    up = (uplift.astype(np.float64).ravel() * ~fx)
    hard = np.maximum(hardness.astype(np.float64).ravel(), 0.05)
    k_dt = k * dt / hard
    # Harder rock also stands steeper.
    steepest = talus * np.clip(hard, 0.7, 1.5)
    area = np.full(rows * cols, float(cell_m * cell_m))
    for step in range(steps):
        filled = _fill(hf, fx, rows, cols, 1e-3)
        recv, dist = _receivers(filled, fx, rows, cols, float(cell_m), step + 1)
        order = np.argsort(filled, kind='stable')
        area = _area(order, recv, float(cell_m * cell_m))
        # Pits fill with what washes in.
        hf = filled
        _stream_power(hf, order, recv, dist, area, up, k_dt, m, dt, steepest)
    return hf.reshape(rows, cols).astype(np.float32), area.reshape(rows, cols).astype(np.float32)


# --- Rain cutting gullies -----------------------------------------------------------------------------------------------


def _brush(radius):
    r = int(math.ceil(radius))
    offsets, weights = [], []
    for dz in range(-r, r + 1):
        for dx in range(-r, r + 1):
            d = math.hypot(dx, dz)
            if d < radius:
                offsets.append((dz, dx))
                weights.append(radius - d)
    w = np.array(weights, dtype=np.float32)
    return np.array(offsets, dtype=np.int64), w / w.sum()


@numba.njit(parallel=True, cache=True)
def _droplets(h, starts, brush_off, brush_w, inertia, capacity, min_slope, erode, deposit, evaporate, gravity, max_steps,
              most_per_step, flow, deposited):
    rows, cols = h.shape
    n = starts.shape[0]
    chunks = 96
    for chunk in numba.prange(chunks):
        for d in range(chunk, n, chunks):
            x = starts[d, 0]
            z = starts[d, 1]
            dx = 0.0
            dz = 0.0
            speed = 1.0
            water = 1.0
            sediment = 0.0
            for _ in range(max_steps):
                cx = int(x)
                cz = int(z)
                if cx < 2 or cz < 2 or cx >= cols - 3 or cz >= rows - 3:
                    break
                fx = x - cx
                fz = z - cz
                h00 = h[cz, cx]
                h10 = h[cz, cx + 1]
                h01 = h[cz + 1, cx]
                h11 = h[cz + 1, cx + 1]
                gx = (h10 - h00) * (1.0 - fz) + (h11 - h01) * fz
                gz = (h01 - h00) * (1.0 - fx) + (h11 - h10) * fx
                height = h00 * (1 - fx) * (1 - fz) + h10 * fx * (1 - fz) + h01 * (1 - fx) * fz + h11 * fx * fz
                dx = dx * inertia - gx * (1.0 - inertia)
                dz = dz * inertia - gz * (1.0 - inertia)
                length = math.sqrt(dx * dx + dz * dz)
                if length < 1e-9:
                    break
                dx /= length
                dz /= length
                nx = x + dx
                nz = z + dz
                ncx = int(nx)
                ncz = int(nz)
                if ncx < 2 or ncz < 2 or ncx >= cols - 3 or ncz >= rows - 3:
                    break
                nfx = nx - ncx
                nfz = nz - ncz
                new_height = (h[ncz, ncx] * (1 - nfx) * (1 - nfz) + h[ncz, ncx + 1] * nfx * (1 - nfz) +
                              h[ncz + 1, ncx] * (1 - nfx) * nfz + h[ncz + 1, ncx + 1] * nfx * nfz)
                dh = new_height - height
                cap = max(-dh, min_slope) * speed * water * capacity
                if sediment > cap or dh > 0.0:
                    # Drops sediment: into the pit it's climbing out of, or what it can't carry.
                    amount = min(dh, sediment) if dh > 0.0 else (sediment - cap) * deposit
                    sediment -= amount
                    h[cz, cx] += amount * (1 - fx) * (1 - fz)
                    h[cz, cx + 1] += amount * fx * (1 - fz)
                    h[cz + 1, cx] += amount * (1 - fx) * fz
                    h[cz + 1, cx + 1] += amount * fx * fz
                    deposited[cz, cx] += amount
                else:
                    amount = min((cap - sediment) * erode, -dh, most_per_step)
                    for b in range(brush_w.size):
                        rr = cz + brush_off[b, 0]
                        cc = cx + brush_off[b, 1]
                        if rr >= 0 and rr < rows and cc >= 0 and cc < cols:
                            h[rr, cc] -= amount * brush_w[b]
                    sediment += amount
                speed = math.sqrt(max(speed * speed - dh * gravity, 0.0))
                water *= 1.0 - evaporate
                flow[cz, cx] += water
                x = nx
                z = nz


def rain(h, cell_m, where, count, seed, radius=2.5, inertia=0.12, capacity=5.0, min_slope=0.02, erode=0.25,
         deposit=0.2, evaporate=0.012, gravity=6.0, max_steps=96, most_per_step_m=0.06):
    """Droplets falling where `where` says (0 to 1), each running downhill until it dries or leaves. Changes `h` in place.
    Returns where water ran (droplet visits) and where sediment was dropped (m)."""
    rows, cols = h.shape
    rng = np.random.default_rng(seed)
    starts = []
    have = 0
    while have < count:
        x = rng.uniform(3, cols - 4, size=count)
        z = rng.uniform(3, rows - 4, size=count)
        keep = rng.random(count) < where[z.astype(np.int64), x.astype(np.int64)]
        starts.append(np.stack([x[keep], z[keep]], axis=1))
        have += int(keep.sum())
    starts = np.concatenate(starts)[:count].astype(np.float64)
    offsets, weights = _brush(radius)
    flow = np.zeros_like(h)
    deposited = np.zeros_like(h)
    # The droplets work in cells: heights in cells too, so a slope is a slope.
    cells = (h / cell_m).astype(np.float32)
    _droplets(cells, starts, offsets, weights, inertia, capacity, min_slope, erode, deposit, evaporate, gravity, max_steps,
              most_per_step_m / cell_m, flow, deposited)
    h[:] = cells * cell_m
    return flow, deposited * cell_m


# --- Rockfall and snow --------------------------------------------------------------------------------------------------


@numba.njit(parallel=True, cache=True)
def _slide_out(surface, movable, cell_m, repose, rate, out_amount, out_total):
    rows, cols = surface.shape
    for r in numba.prange(1, rows - 1):
        for c in range(1, cols - 1):
            hi = surface[r, c]
            total = 0.0
            most = 0.0
            for d in range(8):
                diff = hi - surface[r + _DR[d], c + _DC[d]] - repose * cell_m * _DD[d]
                if diff > 0.0:
                    total += diff
                    if diff > most:
                        most = diff
            out_total[r, c] = total
            out_amount[r, c] = min(most * rate, movable[r, c]) if total > 0.0 else 0.0


@numba.njit(parallel=True, cache=True)
def _slide_in(surface, cell_m, repose, out_amount, out_total, change):
    rows, cols = surface.shape
    for r in numba.prange(1, rows - 1):
        for c in range(1, cols - 1):
            gained = 0.0
            hi = surface[r, c]
            for d in range(8):
                rr = r + _DR[d]
                cc = c + _DC[d]
                if out_amount[rr, cc] <= 0.0:
                    continue
                diff = surface[rr, cc] - hi - repose * cell_m * _DD[d]
                if diff > 0.0:
                    gained += out_amount[rr, cc] * diff / out_total[rr, cc]
            change[r, c] = gained - out_amount[r, c]


def slide(ground, layer, cell_m, repose_deg, iterations, rate=0.5, layer_only=True, where=None):
    """Moves material downhill wherever the surface (ground + layer) is past its angle of repose. With layer_only, only the
    layer moves (snow off rock); otherwise the ground itself does (rockfall), only where `where` is above 0, and `layer`
    collects what lands. Changes them in place."""
    repose = math.tan(math.radians(repose_deg))
    out_amount = np.zeros_like(ground)
    out_total = np.zeros_like(ground)
    change = np.zeros_like(ground)
    for _ in range(iterations):
        surface = ground + layer if layer_only else ground
        movable = layer if layer_only else (np.full_like(ground, 1e9) if where is None else where * np.float32(1e9))
        _slide_out(surface, movable, cell_m, repose, rate, out_amount, out_total)
        _slide_in(surface, cell_m, repose, out_amount, out_total, change)
        if layer_only:
            layer += change
            np.maximum(layer, 0.0, out=layer)
        else:
            ground += change
            layer += np.maximum(change, 0.0)
