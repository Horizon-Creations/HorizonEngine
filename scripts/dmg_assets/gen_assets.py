#!/usr/bin/env python3
"""Generate the HorizonEditor macOS DMG assets from the HC logo.

Subcommands:
  icon        Build a macOS AppIcon.icns (full HC lockup on a premium light squircle).
  background  Build the DMG window background (1x PNG + @2x PNG) for a given theme.

Everything is derived deterministically from EditorDeps/Images/HC_Logo.png so the
DMG is fully reproducible from the repo (no hand-edited binary assets).

Requires: Pillow.  iconutil (from Xcode CLT) is shelled out to for .icns assembly.
"""
import argparse
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont

# ── Shared layout constants (must match scripts/dmg_assets/dmg_settings.py) ──────
WIN_W, WIN_H = 640, 400
ICON_SIZE = 128
APP_XY = (168, 256)     # centre of the .app icon (1x, content coords)
APPS_XY = (472, 256)    # centre of the /Applications symlink

# ── Brand palette (sampled from HC_Logo.png) ────────────────────────────────────
IVORY = (240, 230, 206)
GOLD = (234, 194, 78)
AMBER = (206, 124, 36)


def font(size, bold=False):
    """Load a clean system sans-serif, preferring SF / Helvetica."""
    candidates = (
        ["/System/Library/Fonts/SFNSRounded.ttf",
         "/System/Library/Fonts/SFNS.ttf",
         "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
         "/System/Library/Fonts/HelveticaNeue.ttc",
         "/System/Library/Fonts/Helvetica.ttc"] if bold else
        ["/System/Library/Fonts/SFNS.ttf",
         "/System/Library/Fonts/HelveticaNeue.ttc",
         "/System/Library/Fonts/Helvetica.ttc",
         "/System/Library/Fonts/Supplemental/Arial.ttf"]
    )
    for p in candidates:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                continue
    return ImageFont.load_default()


def autocrop_alpha(img, pad_ratio=0.0):
    """Crop an RGBA image to its non-transparent bounding box."""
    img = img.convert("RGBA")
    bbox = img.getchannel("A").getbbox()
    if not bbox:
        return img
    if pad_ratio:
        w, h = img.size
        px = int((bbox[2] - bbox[0]) * pad_ratio)
        py = int((bbox[3] - bbox[1]) * pad_ratio)
        bbox = (max(0, bbox[0] - px), max(0, bbox[1] - py),
                min(w, bbox[2] + px), min(h, bbox[3] + py))
    return img.crop(bbox)


def vgradient(w, h, stops):
    """Vertical gradient. stops = [(pos0..1, (r,g,b)), ...] sorted by pos."""
    base = Image.new("RGB", (w, h))
    px = base.load()
    for y in range(h):
        t = y / max(1, h - 1)
        # find bracketing stops
        lo = stops[0]
        hi = stops[-1]
        for i in range(len(stops) - 1):
            if stops[i][0] <= t <= stops[i + 1][0]:
                lo, hi = stops[i], stops[i + 1]
                break
        span = max(1e-6, hi[0] - lo[0])
        f = (t - lo[0]) / span
        c = tuple(int(lo[1][k] + (hi[1][k] - lo[1][k]) * f) for k in range(3))
        for x in range(w):
            px[x, y] = c
    return base


def radial_glow(w, h, cx, cy, radius, color, max_alpha):
    """Soft radial glow as an RGBA layer (alpha falls off ~smoothstep)."""
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = glow.load()
    r2 = radius * radius
    for y in range(h):
        dy = y - cy
        for x in range(w):
            dx = x - cx
            d2 = dx * dx + dy * dy
            if d2 >= r2:
                continue
            t = 1.0 - math.sqrt(d2) / radius
            t = t * t * (3 - 2 * t)          # smoothstep
            px[x, y] = (color[0], color[1], color[2], int(max_alpha * t))
    return glow


def squircle_mask(size, radius):
    """Big-Sur-ish rounded-square mask (anti-aliased via 4x supersample)."""
    s = size * 4
    m = Image.new("L", (s, s), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=radius * 4, fill=255)
    return m.resize((size, size), Image.LANCZOS)


# ── Background themes ────────────────────────────────────────────────────────────
THEMES = {
    # name: (sky stops, glow color, glow alpha, star count, horizon color)
    "twilight": ([(0.0, (12, 16, 38)), (0.5, (26, 26, 60)),
                  (0.72, (60, 42, 70)), (1.0, (14, 14, 30))],
                 (235, 170, 70), 150, 70, (240, 180, 90)),
    "midnight": ([(0.0, (8, 11, 30)), (0.55, (16, 22, 52)),
                  (0.78, (30, 40, 78)), (1.0, (8, 10, 24))],
                 (150, 180, 240), 110, 130, (150, 185, 240)),
    "sunrise":  ([(0.0, (38, 30, 58)), (0.42, (96, 70, 86)),
                  (0.66, (214, 150, 92)), (1.0, (244, 224, 188))],
                 (255, 214, 150), 200, 30, (255, 226, 170)),
}


def build_photo_base(path, W, H):
    """Cover-crop a screenshot to W:H and lay a readability scrim over it so the
    light logo (top) and white caption (bottom) stay legible on any scene."""
    src = Image.open(path).convert("RGB")
    sr, tr = src.width / src.height, W / H
    if sr > tr:                       # source too wide -> crop width
        nw = int(src.height * tr); x = (src.width - nw) // 2
        src = src.crop((x, 0, x + nw, src.height))
    else:                             # source too tall -> crop height
        nh = int(src.width / tr); y = (src.height - nh) // 2
        src = src.crop((0, y, src.width, y + nh))
    img = src.resize((W, H), Image.LANCZOS).convert("RGBA")

    # vertical alpha scrim (built as 1xH then stretched — fast): base dim + a
    # stronger darkening at the very top (logo) and the bottom band (caption).
    col = Image.new("RGBA", (1, H), (0, 0, 0, 0))
    cp = col.load()
    for y in range(H):
        t = y / max(1, H - 1)
        top = max(0.0, 1.0 - y / (H * 0.42)) * 0.42
        bot = max(0.0, (t - 0.76) / 0.24) * 0.55
        a = int(min(0.82, 0.20 + top + bot) * 255)
        cp[0, y] = (6, 8, 18, a)
    img = Image.alpha_composite(img, col.resize((W, H)))
    return img


def render_background(logo_path, theme, scale, photo=None):
    s = scale
    W, H = WIN_W * s, WIN_H * s

    if photo:
        img = build_photo_base(photo, W, H)
    else:
        sky_stops, glow_rgb, glow_a, n_stars, horizon_rgb = THEMES[theme]
        img = vgradient(W, H, sky_stops).convert("RGBA")

        # stars in the upper sky (deterministic)
        rnd = random.Random(7)
        stars = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        sd = ImageDraw.Draw(stars)
        for _ in range(n_stars):
            x = rnd.randint(0, W - 1)
            y = rnd.randint(0, int(H * 0.48))
            r = rnd.choice([0.5, 0.7, 1.0, 1.0, 1.4]) * s
            a = rnd.randint(40, 170)
            sd.ellipse([x - r, y - r, x + r, y + r], fill=(255, 250, 240, a))
        img = Image.alpha_composite(img, stars)

        # sun-glow bloom behind the logo (top third)
        glow = radial_glow(W, H, W * 0.5, H * 0.22, W * 0.46, glow_rgb, glow_a)
        img = Image.alpha_composite(img, glow)

    # HC logo, top-centre (kept clear of the icon row below)
    logo = autocrop_alpha(Image.open(logo_path))
    lw = int(224 * s)
    lh = int(lw * logo.height / logo.width)
    logo = logo.resize((lw, lh), Image.LANCZOS)
    img.alpha_composite(logo, ((W - lw) // 2, int(20 * s)))

    # subtle framing pads under each icon slot
    pad = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    pd = ImageDraw.Draw(pad)
    for cx, cy in (APP_XY, APPS_XY):
        rx, ry = int(80 * s), int(80 * s)
        pd.rounded_rectangle(
            [cx * s - rx, cy * s - ry, cx * s + rx, cy * s + ry],
            radius=int(24 * s), fill=(255, 255, 255, 12),
            outline=(255, 255, 255, 22), width=max(1, s))
    pad = pad.filter(ImageFilter.GaussianBlur(0.6 * s))
    img = Image.alpha_composite(img, pad)

    # drag arrow between the two slots
    arrow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ad = ImageDraw.Draw(arrow)
    ay = APP_XY[1] * s
    x0, x1 = int(272 * s), int(368 * s)
    shaft_h = int(7 * s)
    ad.rounded_rectangle([x0, ay - shaft_h // 2, x1 - int(14 * s), ay + shaft_h // 2],
                         radius=shaft_h, fill=(255, 244, 224, 230))
    head = int(20 * s)
    ad.polygon([(x1 - int(20 * s), ay - head), (x1, ay),
                (x1 - int(20 * s), ay + head)], fill=(255, 244, 224, 230))
    glow_arrow = arrow.filter(ImageFilter.GaussianBlur(3 * s))
    img = Image.alpha_composite(img, glow_arrow)
    img = Image.alpha_composite(img, arrow)

    # caption
    cap = "Drag Horizon Editor to the Applications folder"
    f = font(int(13 * s))
    d = ImageDraw.Draw(img)
    tb = d.textbbox((0, 0), cap, font=f)
    tw = tb[2] - tb[0]
    cy = int(366 * s)
    d.text(((W - tw) // 2 + 1, cy + 1), cap, font=f, fill=(0, 0, 0, 120))
    d.text(((W - tw) // 2, cy), cap, font=f, fill=(238, 232, 216, 235))

    return img.convert("RGB")


def cmd_background(args):
    out1 = args.out
    base, ext = os.path.splitext(out1)
    out2 = f"{base}@2x{ext}"
    photo = getattr(args, "photo", None)
    render_background(args.logo, args.theme, 1, photo).save(out1)
    render_background(args.logo, args.theme, 2, photo).save(out2)
    print(f"  background: {out1}" + (f"  (photo: {photo})" if photo else ""))
    print(f"  background: {out2}")


# ── Icon ─────────────────────────────────────────────────────────────────────────
def render_icon_master(logo_path, size=1024):
    """Full HC lockup on a premium light squircle (macOS Big Sur proportions)."""
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))

    margin = int(size * 0.10)          # ~10% transparent margin all round
    body = size - 2 * margin
    radius = int(body * 0.2237)        # Big Sur corner radius ratio

    # light vertical gradient body: white -> warm ivory
    grad = vgradient(body, body, [(0.0, (255, 255, 255)),
                                  (0.6, (250, 246, 238)),
                                  (1.0, (242, 232, 212))]).convert("RGBA")
    mask = squircle_mask(body, radius)
    grad.putalpha(mask)

    # soft inner top highlight + thin border for depth
    hl = Image.new("RGBA", (body, body), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    hd.ellipse([int(-body * 0.2), int(-body * 0.55), int(body * 1.2), int(body * 0.35)],
               fill=(255, 255, 255, 90))
    hl.putalpha(Image.composite(hl.getchannel("A"),
                                Image.new("L", (body, body), 0), mask))
    grad = Image.alpha_composite(grad, hl)

    border = Image.new("RGBA", (body, body), (0, 0, 0, 0))
    bd = ImageDraw.Draw(border)
    bd.rounded_rectangle([1, 1, body - 2, body - 2], radius=radius,
                         outline=(206, 124, 36, 60), width=max(2, body // 220))
    grad = Image.alpha_composite(grad, border)

    canvas.alpha_composite(grad, (margin, margin))

    # HC lockup centred within the body
    logo = autocrop_alpha(Image.open(logo_path))
    target_w = int(body * 0.74)
    target_h = int(target_w * logo.height / logo.width)
    if target_h > body * 0.74:
        target_h = int(body * 0.74)
        target_w = int(target_h * logo.width / logo.height)
    logo = logo.resize((target_w, target_h), Image.LANCZOS)
    canvas.alpha_composite(logo, ((size - target_w) // 2, (size - target_h) // 2))

    return canvas


def cmd_icon(args):
    master = render_icon_master(args.logo, 1024)
    workdir = args.workdir or tempfile.mkdtemp(prefix="hcicon_")
    iconset = os.path.join(workdir, "AppIcon.iconset")
    os.makedirs(iconset, exist_ok=True)
    sizes = [(16, "16x16"), (32, "16x16@2x"), (32, "32x32"), (64, "32x32@2x"),
             (128, "128x128"), (256, "128x128@2x"), (256, "256x256"),
             (512, "256x256@2x"), (512, "512x512"), (1024, "512x512@2x")]
    for px, name in sizes:
        master.resize((px, px), Image.LANCZOS).save(
            os.path.join(iconset, f"icon_{name}.png"))
    if shutil.which("iconutil"):
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", args.out],
                       check=True)
        print(f"  icon: {args.out}")
    else:
        # fallback: Pillow can write .icns directly (fewer sizes, still valid)
        master.resize((512, 512), Image.LANCZOS).save(args.out, format="ICNS")
        print(f"  icon (Pillow fallback): {args.out}")
    if not args.workdir:
        shutil.rmtree(workdir, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    pi = sub.add_parser("icon")
    pi.add_argument("--logo", required=True)
    pi.add_argument("--out", required=True)
    pi.add_argument("--workdir", default=None)
    pi.set_defaults(func=cmd_icon)

    pb = sub.add_parser("background")
    pb.add_argument("--logo", required=True)
    pb.add_argument("--theme", default="twilight", choices=list(THEMES))
    pb.add_argument("--photo", default=None,
                    help="use a screenshot as the backdrop instead of a procedural sky")
    pb.add_argument("--out", required=True)
    pb.set_defaults(func=cmd_background)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
