#!/usr/bin/env python3
"""make_skin.py - draw OCKM Wave's battered brushed-metal look (tinted) and build its skin.

Copied from OCKM Gate/make_skin.py; OCKM Wave uses --tint champagne (the default here).

SPDX-License-Identifier: MIT

Run from the MPC-Plugins repo root after a native build (it needs build/params.json).
SDK is the MPC Plugin SDK folder ($MPC_SDK, default /Users/simon/Projects/mpc-plugin-builder-sdk):

    python3 $SDK/tools/build.py "OCKM Wave" --target native
    python3 "OCKM Wave/make_skin.py"
    python3 $SDK/tools/build.py "OCKM Wave"

What it does:
  1. Draws the artwork into assets/ (build.py copies assets/ into Plugin Skins/):
       panel_wave.png, panel_filter.png, panel_mod.png  worn, scratched brushed-aluminium panels (1280 x 628)
                                          with printed section frames, headings and knob scales
       knob.png                           127-frame film strip: black knurled knob, spun-metal cap
       toggle_on.png, toggle_off.png      worn plastic switches (on = lit amber)
  2. Runs tools/gen_ui.py on ui.json into skins/ and sets each page's
     background image to its panel. Because skins/ exists, build.py copies it instead of
     regenerating: re-run this script after any change to ui.json.

Only the standard library and numpy are used. Everything is seeded, so output is repeatable.
"""
import json
import os
import struct
import subprocess
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
# The MPC Plugin SDK lives in its own folder; set MPC_SDK to override the default.
SDK = os.path.abspath(os.path.expanduser(os.environ.get("MPC_SDK", "/Users/simon/Projects/mpc-plugin-builder-sdk")))
if not os.path.isfile(os.path.join(SDK, "tools", "gen_ui.py")):
    sys.exit("MPC Plugin SDK not found at %s: set MPC_SDK to the SDK folder" % SDK)
ASSETS = os.path.join(HERE, "assets")
SKINS = os.path.join(HERE, "skins")

W, H = 1280, 628                                  # tab area
SLOT_X = [23, 343, 663, 983]                      # same grid as tools/gen_ui.py
SLOT_Y = [45, 196, 346, 497]
SLOT_W, SLOT_H = 278, 107
KNOB = 77                                         # gen_ui draws knobImage at 77 x 77, at (15, 15) in the slot

INK = np.array([0.13, 0.12, 0.11])


# ------------------------------------------------------------------------------------------
# PNG output
# ------------------------------------------------------------------------------------------
def write_png(path, rgba):
    """rgba: float array (h, w, 4) in 0..1."""
    img = (np.clip(rgba, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    h, w, _ = img.shape
    raw = b"".join(b"\x00" + img[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


# ------------------------------------------------------------------------------------------
# Texture helpers
# ------------------------------------------------------------------------------------------
def smooth_noise(rng, h, w, cell):
    """Bilinear value noise, roughly -1..1, features about `cell` pixels across."""
    gh, gw = h // cell + 2, w // cell + 2
    g = rng.uniform(-1.0, 1.0, (gh, gw))
    ys = np.arange(h) / cell
    xs = np.arange(w) / cell
    y0 = ys.astype(int)
    x0 = xs.astype(int)
    fy = (ys - y0)[:, None]
    fx = (xs - x0)[None, :]
    fy = fy * fy * (3 - 2 * fy)
    fx = fx * fx * (3 - 2 * fx)
    a = g[y0][:, x0]
    b = g[y0][:, x0 + 1]
    c = g[y0 + 1][:, x0]
    d = g[y0 + 1][:, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def box_blur_x(a, n):
    c = np.cumsum(np.pad(a, ((0, 0), (n, n)), mode="wrap"), axis=1)
    return (c[:, 2 * n:] - c[:, :-2 * n])[:, : a.shape[1]] / (2 * n)


def segment_coverage(px, py, x0, y0, x1, y1, width):
    """Anti-aliased coverage (0..1) of a line segment of the given width at points (px, py)."""
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy + 1e-9
    t = np.clip(((px - x0) * dx + (py - y0) * dy) / L2, 0.0, 1.0)
    d = np.hypot(px - (x0 + t * dx), py - (y0 + t * dy))
    return np.clip(width * 0.5 + 0.5 - d, 0.0, 1.0)


def stamp(canvas, x0, y0, x1, y1, pad, fn):
    """Apply fn(px, py) -> (coverage) inside a clipped bounding box; returns (slice, coverage)."""
    h, w = canvas.shape[:2]
    xa, xb = max(0, int(min(x0, x1) - pad)), min(w, int(max(x0, x1) + pad) + 1)
    ya, yb = max(0, int(min(y0, y1) - pad)), min(h, int(max(y0, y1) + pad) + 1)
    if xa >= xb or ya >= yb:
        return None, None
    py, px = np.mgrid[ya:yb, xa:xb].astype(np.float64) + 0.5
    return (slice(ya, yb), slice(xa, xb)), fn(px, py)


# 5 x 7 block letters for the printed legends.
FONT = {
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "C": ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "G": ["01111", "10000", "10000", "10011", "10001", "10001", "01111"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "I": ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "Q": ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "W": ["10001", "10001", "10001", "10101", "10101", "10101", "01010"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "-": ["00000", "00000", "00000", "01110", "00000", "00000", "00000"],
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    " ": ["00000"] * 7,
}


def text_mask(h, w, text, x, y, scale):
    m = np.zeros((h, w))
    cx = x
    for ch in text:
        glyph = FONT[ch]
        for gy, row in enumerate(glyph):
            for gx, bit in enumerate(row):
                if bit == "1":
                    m[y + gy * scale: y + (gy + 1) * scale, cx + gx * scale: cx + (gx + 1) * scale] = 1.0
        cx += 6 * scale
    return m


def text_width(text, scale):
    return len(text) * 6 * scale - scale


# ------------------------------------------------------------------------------------------
# Panel
# ------------------------------------------------------------------------------------------
def draw_panel(seed, headings, knob_slots, fader_slots, button_slots, plates=(), badge=None):
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)

    # Brushed aluminium: long horizontal grain plus faint per-row streaks.
    grain = box_blur_x(rng.normal(0.0, 1.0, (H, W)), 90)
    grain /= grain.std() + 1e-9
    rows = rng.normal(0.0, 1.0, (H, 1))
    lum = 0.70 + 0.030 * grain + 0.014 * rows
    lum += 0.025 * smooth_noise(rng, H, W, 300)                    # uneven sheen
    lum += 0.035 * (1.0 - yy / H) - 0.02                          # light from above
    rgb = (lum + TINT["lum"])[..., None] * np.array(TINT["rgb"])[None, None, :]

    # Patina and grime: brown-grey blotches, heavier toward the edges and corners.
    blot = smooth_noise(rng, H, W, 70) * 0.6 + smooth_noise(rng, H, W, 23) * 0.4
    edge = np.minimum(np.minimum(xx, W - 1 - xx), np.minimum(yy, H - 1 - yy))
    edge_grime = np.exp(-edge / 35.0) * 0.8
    grime = np.clip(0.45 * np.clip(blot - 0.10, 0, None) + edge_grime * (0.7 + 0.5 * blot), 0, 1)
    stains = np.clip(smooth_noise(rng, H, W, 60) - 0.72, 0, None) * 1.6      # old coffee/oil stains
    grime = np.clip(grime + stains, 0, 1)
    dirt = np.array([0.36, 0.32, 0.26])
    rgb = rgb * (1 - 0.45 * grime[..., None]) + dirt * (0.45 * grime[..., None]) * 0.6

    # Fingerprint-ish smudges around the most-used controls.
    for (sx, sy) in knob_slots[:6]:
        cx, cy = sx + 15 + KNOB / 2, sy + 15 + KNOB / 2
        d = np.hypot(xx - cx - rng.uniform(-10, 10), yy - cy - rng.uniform(-6, 12))
        rgb *= 1 - 0.07 * np.exp(-(d / 60.0) ** 2)[..., None]

    # Printed ink: section frames, headings, knob scales. Worn away in patches and specks.
    ink = np.zeros((H, W))
    for col, title in enumerate(headings):
        x0, x1 = SLOT_X[col] - 9, SLOT_X[col] + SLOT_W + 9
        y0, y1 = 40, 610
        for (a, b, c, d) in ((x0, y0, x1, y0), (x0, y1, x1, y1), (x0, y0, x0, y1), (x1, y0, x1, y1)):
            sl, cov = stamp(ink, a, b, c, d, 3, lambda px, py, a=a, b=b, c=c, d=d: segment_coverage(px, py, a, b, c, d, 2.4))
            if sl is not None:
                ink[sl] = np.maximum(ink[sl], cov)
        # Heading sits in a notch at the top of the frame.
        if not title:
            continue
        tw = text_width(title, 4)
        tx, ty = SLOT_X[col] + 8, 20
        ink[ty - 2: ty + 31, tx - 8: tx + tw + 8] = 0.0
    for (sx, sy) in knob_slots:
        cx, cy = sx + 15 + KNOB / 2, sy + 15 + KNOB / 2
        for k in range(11):
            a = np.radians(-135 + 27 * k)
            r0, r1 = 41.5, (48.0 if k in (0, 5, 10) else 45.5)
            ax, ay = cx + r0 * np.sin(a), cy - r0 * np.cos(a)
            bx, by = cx + r1 * np.sin(a), cy - r1 * np.cos(a)
            sl, cov = stamp(ink, ax, ay, bx, by, 3, lambda px, py, ax=ax, ay=ay, bx=bx, by=by: segment_coverage(px, py, ax, ay, bx, by, 2.0))
            if sl is not None:
                ink[sl] = np.maximum(ink[sl], cov)
    wear = (smooth_noise(rng, H, W, 9) > -0.55) * (rng.random((H, W)) > 0.07)
    wear = wear * (0.75 + 0.25 * smooth_noise(rng, H, W, 30))
    ink *= np.clip(wear, 0, 1)
    # Letters: lighter wear (specks and fading, no missing strokes) so they stay readable.
    letters = np.zeros((H, W))
    for col, title in enumerate(headings):
        if title:
            letters = np.maximum(letters, text_mask(H, W, title, SLOT_X[col] + 8, 20, 4))
    # Name badge in one empty slot per page.
    mark, sub = "OCKM WAVE", "WAVETABLE SYNTH"
    mx = my = mw = -1000
    if badge is not None:
        bx, by = SLOT_X[badge[0]], SLOT_Y[badge[1]]
        mw = text_width(mark, 4)
        mx, my = bx + (SLOT_W - mw) // 2, by + 22
        letters = np.maximum(letters, text_mask(H, W, mark, mx, my, 4))
        sw = text_width(sub, 2)
        letters = np.maximum(letters, text_mask(H, W, sub, bx + (SLOT_W - sw) // 2, my + 44, 2))
    letters *= (rng.random((H, W)) > 0.04) * (0.88 + 0.12 * np.clip(smooth_noise(rng, H, W, 14) + 0.5, 0, 1))
    ink = np.maximum(ink, letters)
    rgb = rgb * (1 - 0.85 * ink[..., None]) + INK * (0.85 * ink[..., None])

    # Faded red stripe under the wordmark (the one splash of colour).
    stripe = ((xx >= mx) & (xx <= mx + mw) & (yy >= my + 33) & (yy <= my + 37)).astype(float)
    stripe *= (rng.random((H, W)) > 0.12) * (smooth_noise(rng, H, W, 12) > -0.4)
    rgb = rgb * (1 - 0.7 * stripe[..., None]) + np.array([0.62, 0.22, 0.15]) * (0.7 * stripe[..., None])

    # Recessed wells under faders and buttons.
    for (sx, sy, x, y, w, h) in fader_slots + button_slots:
        x0, y0 = sx + x - 4, sy + y - 4
        x1, y1 = x0 + w + 8, y0 + h + 8
        inside = (xx >= x0) & (xx < x1) & (yy >= y0) & (yy < y1)
        rgb[inside] *= 0.80
        top = inside & (yy < y0 + 3)
        rgb[top] *= 0.75
        bottom = inside & (yy >= y1 - 2)
        rgb[bottom] = rgb[bottom] * 0.6 + 0.4 * 0.9

    # Scratches: many short fine ones, a few long gouges. Light core, dark lip.
    def scratch(x0, y0, x1, y1, width, strength):
        # Bright scratches catch the light; about one in four is a dark, dirt-filled groove.
        sl, cov = stamp(rgb, x0, y0, x1, y1, width + 2,
                        lambda px, py: segment_coverage(px, py, x0, y0, x1, y1, width))
        if sl is None:
            return
        col = 0.93 if rng.random() > 0.25 else 0.38
        a = (cov * strength)[..., None]
        rgb[sl] = rgb[sl] * (1 - a) + col * a

    for _ in range(300):
        x0, y0 = rng.uniform(0, W), rng.uniform(0, H)
        ang = rng.normal(0.25, 0.9)
        L = rng.exponential(22)
        scratch(x0, y0, x0 + L * np.cos(ang), y0 + L * np.sin(ang), rng.uniform(0.4, 1.0), rng.uniform(0.2, 0.55))
    for _ in range(4):
        x0, y0 = rng.uniform(0, W), rng.uniform(0, H)
        ang = rng.uniform(-0.6, 0.6) + (np.pi if rng.random() < 0.5 else 0)
        L = rng.uniform(100, 260)
        scratch(x0, y0, x0 + L * np.cos(ang), y0 + L * np.sin(ang), rng.uniform(0.8, 1.3), rng.uniform(0.3, 0.45))

    # Dents and dings.
    for _ in range(26):
        cx, cy, r = rng.uniform(0, W), rng.uniform(0, H), rng.uniform(2.0, 6.5)
        d = np.hypot(xx - cx, yy - cy)
        m = d < r * 2.2
        if not m.any():
            continue
        shade = np.exp(-(d[m] / r) ** 2)
        hl = np.exp(-(np.hypot(xx[m] - cx + r * 0.5, yy[m] - cy + r * 0.5) / (r * 0.5)) ** 2)
        rgb[m] = rgb[m] * (1 - 0.35 * shade)[:, None] + (0.35 * hl)[:, None] * 0.6

    # Label plates: a cleaner, lighter strip behind each control's name and value text so the
    # MPC's lettering stays readable over the wear. Feathered edges and a little grime of its own.
    for (x, y, w, h) in plates:
        dx = np.maximum(np.maximum(x - xx, xx - (x + w)), 0)
        dy = np.maximum(np.maximum(y - yy, yy - (y + h)), 0)
        m = np.clip(1.0 - np.hypot(dx, dy) / 4.0, 0, 1)
        m *= 0.92 - 0.10 * np.clip(smooth_noise(rng, H, W, 25), 0, 1)
        sel = m > 0
        tone = 0.83 + 0.014 * grain[sel]
        plate = tone[:, None] * np.array(TINT["plate"])[None, :]
        rgb[sel] = rgb[sel] * (1 - m[sel, None]) + plate * m[sel, None]

    # Edge chips: the paint-free metal edge is darker and bitten.
    border = edge < 3
    rgb[border] *= 0.55

    # Corner screws.
    for (cx, cy) in ((13, 13), (W - 14, 13), (13, H - 14), (W - 14, H - 14)):
        draw_screw(rgb, xx, yy, cx, cy, 8.5, rng.uniform(0, np.pi), rng)

    alpha = np.ones((H, W, 1))
    return np.concatenate([np.clip(rgb, 0, 1), alpha], axis=2)


def draw_screw(rgb, xx, yy, cx, cy, r, rot, rng):
    d = np.hypot(xx - cx, yy - cy)
    rust = (d < r + 4) & (d >= r)
    rgb[rust] = rgb[rust] * 0.75 + np.array([0.40, 0.25, 0.14]) * 0.25
    head = d < r
    nx, ny = (xx[head] - cx) / r, (yy[head] - cy) / r
    lum = 0.62 - 0.22 * (nx + ny) * 0.5 - 0.15 * (nx * nx + ny * ny)
    rgb[head] = lum[:, None] * np.array([1.0, 0.99, 0.95])[None, :]
    rim = (d < r + 1) & (d >= r - 1)
    rgb[rim] *= 0.45
    for a in (rot, rot + np.pi / 2):
        ax, ay = cx + (r - 2) * np.cos(a), cy + (r - 2) * np.sin(a)
        bx, by = cx - (r - 2) * np.cos(a), cy - (r - 2) * np.sin(a)
        cov = segment_coverage(xx[head], yy[head], ax, ay, bx, by, 2.2)
        rgb[head] *= (1 - 0.75 * cov)[:, None]


# ------------------------------------------------------------------------------------------
# Knob film strip
# ------------------------------------------------------------------------------------------
def draw_knob_strip(frames=127, size=KNOB, ss=3):
    rng = np.random.default_rng(303)
    n = size * ss
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float64) + 0.5
    cx = cy = n / 2.0
    dx, dy = (xx - cx) / ss, (yy - cy) / ss            # in output pixels
    r = np.hypot(dx, dy)
    th = np.arctan2(dx, -dy)                           # 0 = up, clockwise
    R_SKIRT, R_CAP = 33.0, 24.0
    # Static (lighting) terms.
    light = -(dx + dy) / (np.sqrt(2) * np.maximum(r, 1e-6))
    ring_noise = np.interp(r, np.linspace(0, 40, 400), rng.normal(0, 1, 400))
    shadow_d = np.hypot(dx - 1.8, dy - 2.6)
    shadow = np.clip((R_SKIRT + 3.5 - shadow_d) / 4.0, 0, 1) * 0.55
    # Rotating wear textures, sampled in the knob's own angle.
    tex_n = 720
    scuff_tex = rng.random((tex_n, 40)) < 0.035
    pointer_wear = np.clip(0.9 + 0.25 * rng.normal(0, 1, 60), 0.45, 1.0)

    strip = np.zeros((frames * size, size, 4))
    for f in range(frames):
        a = np.radians(-135.0 + 270.0 * f / (frames - 1))
        local = (th - a) % (2 * np.pi)
        ti = (local / (2 * np.pi) * tex_n).astype(int) % tex_n
        ri = np.clip(r, 0, 39).astype(int)

        rgb = np.zeros((n, n, 3))
        # Skirt: black, knurled near the rim, dull highlight top-left.
        knurl = 0.5 + 0.5 * np.cos(local * 48)
        skirt = 0.085 + 0.05 * light + 0.05 * knurl * np.clip((r - (R_SKIRT - 5)) / 5, 0, 1)
        skirt = skirt + 0.10 * scuff_tex[ti, ri] * (r > R_CAP + 1)
        rgb[:] = skirt[..., None]
        # Bevel between skirt and cap.
        bevel = (r > R_CAP) & (r < R_CAP + 2.0)
        rgb[bevel] = (0.18 + 0.12 * light[bevel])[:, None]
        # Spun aluminium cap: concentric grain + bow-tie highlight that stays put as it turns.
        capm = r <= R_CAP
        bow = np.cos(2 * (th[capm] + np.pi / 4)) ** 2
        cap = 0.56 + 0.20 * bow + 0.025 * ring_noise[capm] + 0.04 * light[capm]
        cap = cap - 0.10 * scuff_tex[ti[capm], (ri[capm] * 7) % 40]
        rgb[capm] = cap[:, None] * np.array([1.0, 1.0, 0.97])[None, :]
        # Pointer: a worn cream line from near the centre to the rim.
        pw = pointer_wear[np.clip((r / 40 * 59).astype(int), 0, 59)]
        across = np.abs(r * np.sin(local))
        along = r * np.cos(local)
        pm = (across < 1.9) & (along > 4) & (r < R_SKIRT - 1.5)
        pcol = np.where((r[pm] <= R_CAP)[:, None], np.array([0.08, 0.07, 0.07])[None, :], np.array([0.94, 0.91, 0.82])[None, :])
        rgb[pm] = rgb[pm] * (1 - pw[pm, None]) + pcol * pw[pm, None]

        cover = (r <= R_SKIRT).astype(np.float64)
        rim_dark = (r > R_SKIRT - 1.0) & (r <= R_SKIRT)
        rgb[rim_dark] *= 0.5
        out = np.zeros((n, n, 4))
        out[..., :3] = rgb * cover[..., None]
        out[..., 3] = np.maximum(cover, shadow * (1 - cover))
        # Premultiplied downsample, then un-premultiply.
        o = out.reshape(size, ss, size, ss, 4).mean(axis=(1, 3))
        al = o[..., 3:4]
        o[..., :3] = np.where(al > 1e-6, o[..., :3] / np.maximum(al, 1e-6), 0)
        strip[f * size:(f + 1) * size] = o
    return strip


# ------------------------------------------------------------------------------------------
# Buttons (2-way switch segments, 122 x 40)
# ------------------------------------------------------------------------------------------
def draw_button(on, seed, w=122, h=40):
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64) + 0.5
    rad = 5.0
    qx = np.clip(xx, rad, w - rad)
    qy = np.clip(yy, rad, h - rad)
    d = np.hypot(xx - qx, yy - qy)
    cover = np.clip(rad + 0.5 - d, 0, 1)
    base = np.array([0.93, 0.77, 0.45]) if on else np.array([0.64, 0.64, 0.61])
    grad = 1.04 - 0.12 * (yy / h)
    rgb = base[None, None, :] * grad[..., None]
    rgb *= (1 - 0.12 * np.clip(smooth_noise(rng, h, w, 10), 0, 1))[..., None]          # grime
    rgb[(rng.random((h, w)) < 0.02)] *= 0.8
    edge = cover * (d > rad - 1.5)
    rgb *= (1 - 0.4 * edge)[..., None]
    if on:
        glow = np.exp(-((yy - h / 2) / (h * 0.8)) ** 2)
        rgb = np.clip(rgb * (0.95 + 0.1 * glow[..., None]), 0, 1)
    return np.concatenate([rgb, cover[..., None]], axis=2)


# ------------------------------------------------------------------------------------------
# Skin assembly
# ------------------------------------------------------------------------------------------
def page_layout(page):
    knobs, faders, buttons, plates = [], [], [], []
    for i, c in enumerate(page["controls"]):
        if isinstance(c, str):
            continue
        row, col = divmod(i, 4)
        sx, sy = SLOT_X[col], SLOT_Y[row]
        widget = c.get("widget", "slider")
        if widget == "knob":
            knobs.append((sx, sy))
            plates.append((sx + 103, sy + 12, 166, 84))
        elif widget == "fader":
            faders.append((sx, sy, 15, 54, 248, 36))
            plates.append((sx + 13, sy + 8, 252, 34))
        elif widget == "switch":
            # Bool parameters: name/value on the left, a toggle on the right.
            plates.append((sx + 13, sy + 10, 172, 86))
            # No recessed well: where the MPC places the toggle inside its box is unconfirmed.
        elif widget == "slider":
            plates.append((sx + 13, sy + 10, 252, 86))
    return knobs, faders, buttons, plates


class Pairs(list):
    """A JSON object kept as an ordered list of (key, value) pairs, duplicates included."""
    def get(self, key):
        for k, v in self:
            if k == key:
                return v
        raise KeyError(key)

    def set(self, key, value):
        for i, (k, _v) in enumerate(self):
            if k == key:
                self[i] = (k, value)
                return
        raise KeyError(key)


def dump_pairs(o, indent=0):
    pad, pad2 = "  " * indent, "  " * (indent + 1)
    if isinstance(o, Pairs):
        if not o:
            return "{}"
        items = ["%s%s: %s" % (pad2, json.dumps(k), dump_pairs(v, indent + 1)) for k, v in o]
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"
    if isinstance(o, list):
        if not o:
            return "[]"
        return "[\n" + ",\n".join(pad2 + dump_pairs(v, indent + 1) for v in o) + "\n" + pad + "]"
    return json.dumps(o)


HEADINGS = {"WAVE": ["WAVETABLE", "UNISON", "PLAY", "OUT"],
            "FILTER": ["FILTER", "AMP ENV", "MOD ENV", "ENV TO"],
            "MOD": ["LFO", "LFO TO", "WHEEL", ""]}
BADGES = {"WAVE": (3, 2), "FILTER": (3, 3), "MOD": (2, 2)}          # (column, row) of the empty slot that carries the name badge

TINTS = {
    # rgb: colour of the metal; lum: brightness shift; plate: label strips; background: overlay colour
    "silver":    {"rgb": [1.00, 1.01, 0.985], "lum": 0.00,  "plate": [1.00, 1.005, 0.99], "background": "ffa7a8a4"},
    "gunmetal":  {"rgb": [0.90, 0.96, 1.06],  "lum": -0.07, "plate": [0.97, 1.00, 1.04],  "background": "ff8f979f"},
    "champagne": {"rgb": [1.06, 1.00, 0.86],  "lum": 0.00,  "plate": [1.03, 1.00, 0.93],  "background": "ffb3ab96"},
    "bronze":    {"rgb": [1.06, 0.90, 0.72],  "lum": -0.04, "plate": [1.04, 0.98, 0.90],  "background": "ffa48c70"},
}
TINT = TINTS["champagne"]


def main():
    global TINT
    tint = "champagne"
    if "--tint" in sys.argv:
        tint = sys.argv[sys.argv.index("--tint") + 1]
    TINT = TINTS[tint]
    ui_path = os.path.join(HERE, "ui.json")
    params = os.path.join(HERE, "build", "params.json")
    if not os.path.isfile(params):
        sys.exit("build/params.json is missing: run  python3 $SDK/tools/build.py \"OCKM Wave\" --target native  first")
    with open(ui_path) as f:
        ui = json.load(f)
    os.makedirs(ASSETS, exist_ok=True)

    print("drawing knob film strip ...")
    write_png(os.path.join(ASSETS, "knob.png"), draw_knob_strip())
    write_png(os.path.join(ASSETS, "toggle_on.png"), draw_button(True, 7, 64, 32))
    write_png(os.path.join(ASSETS, "toggle_off.png"), draw_button(False, 8, 64, 32))
    # Overlays (the big edit screens) use the ui.json background colour: match the tint.
    with open(ui_path) as f:
        ui_text = f.read()
    ui_json = json.loads(ui_text)
    if ui_json.get("style", {}).get("background") != TINT["background"]:
        ui_json["style"]["background"] = TINT["background"]
        with open(ui_path, "w") as f:
            json.dump(ui_json, f, indent=2)
            f.write("\n")
        ui = ui_json

    backgrounds = []
    for pi, page in enumerate(ui["pages"]):
        name = page["name"]
        file = "panel_%s.png" % name.lower()
        print("drawing %s ..." % file)
        knobs, faders, buttons, plates = page_layout(page)
        write_png(os.path.join(ASSETS, file), draw_panel(1000 + pi, HEADINGS.get(name, ["", "", "", ""]), knobs, faders, buttons, plates,
                                                        BADGES.get(name)))
        backgrounds.append(file)

    # Generate the skin from ui.json, then point each tab's background at its panel.
    if os.path.isdir(SKINS):
        for fn in os.listdir(SKINS):
            os.remove(os.path.join(SKINS, fn))
    subprocess.check_call([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params, "--ui", ui_path,
                           "--out", SKINS, "--assets-dir", ASSETS, "--kit-cache", os.path.join(HERE, "build")])
    # TUI.json legitimately repeats some keys (overlay "version"), so it is edited as ordered
    # key/value pairs and written back without ever collapsing duplicates.
    tui_path = os.path.join(SKINS, "TUI.json")
    with open(tui_path) as f:
        tui = json.load(f, object_pairs_hook=Pairs)
    tabs = tui.get("pageData").get("tabs")
    for tab, bg in zip(tabs, backgrounds):
        bd = tab.get("componentDefinition").get("backgroundData")
        for side in ("focussed", "unfocussed"):
            bd.get(side).set("image", bg)
    # Heavier, larger lettering: names SemiBold, values and captions Bold, about 13% bigger.
    weight = {"Name": "SemiBold", "Value": "Bold", "Text": "Bold"}

    def restyle(o):
        if isinstance(o, Pairs):
            keys = [k for k, _ in o]
            if "textStyle" in keys and "type" in keys:
                font = o.get("textStyle").get("font")
                font.set("style", weight.get(o.get("type"), "SemiBold"))
                font.set("height", float(round(font.get("height") * 1.13)))
            for _, v in o:
                restyle(v)
        elif isinstance(o, list):
            for v in o:
                restyle(v)

    restyle(tui)
    with open(tui_path, "w") as f:
        f.write(dump_pairs(tui))
        f.write("\n")
    print("skin written to %s; now run  python3 $SDK/tools/build.py \"OCKM Wave\"" % SKINS)


if __name__ == "__main__":
    main()
