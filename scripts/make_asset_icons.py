#!/usr/bin/env python3
"""make_asset_icons — generate the Content Browser's asset-type glyphs.

The editor tints these at draw time, so every icon is pure white on a
transparent background (only the alpha channel carries the shape). They are
authored here rather than by hand so the whole set stays visually consistent:
one canvas size, one stroke weight, one safe margin. Re-run after editing a
draw function; the output is deterministic.

    scripts/make_asset_icons.py [OUTDIR]        # default: EditorDeps/Images
    scripts/make_asset_icons.py --sheet OUT.png # contact sheet for review

Style rules (match the pre-existing Folder/Material/Model3D/Script art):
  * 512x512 canvas, shape inside a ~64px margin
  * ~34px stroke for outlines, solid fills for accents
  * bold and geometric — these are read at 60px in the grid, so no hairlines
"""
import os
import sys
import math

from PIL import Image, ImageDraw, ImageFont

S = 512            # canvas
M = 64             # safe margin
W = 34             # default stroke width
WHITE = (255, 255, 255, 255)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_PATH = os.path.join(REPO, "EditorDeps", "Fonts", "Roboto_Condensed-Bold.ttf")


def canvas():
    im = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    return im, ImageDraw.Draw(im)


def rrect(d, box, r, width=W, fill=None):
    d.rounded_rectangle(box, radius=r, outline=None if fill else WHITE,
                        width=width, fill=fill)


def text_centered(d, txt, size, cy=S // 2, cx=S // 2):
    font = ImageFont.truetype(FONT_PATH, size)
    l, t, r, b = d.textbbox((0, 0), txt, font=font)
    d.text((cx - (r + l) / 2, cy - (b + t) / 2), txt, font=font, fill=WHITE)


def arrow(d, x0, y0, x1, y1, width=W, head=46):
    """Line with a solid triangular head at (x1, y1)."""
    ang = math.atan2(y1 - y0, x1 - x0)
    bx, by = x1 - head * math.cos(ang), y1 - head * math.sin(ang)
    d.line([(x0, y0), (bx, by)], fill=WHITE, width=width)
    perp = ang + math.pi / 2
    hx, hy = head * 0.55 * math.cos(perp), head * 0.55 * math.sin(perp)
    d.polygon([(x1, y1), (bx + hx, by + hy), (bx - hx, by - hy)], fill=WHITE)


# ── One function per icon ────────────────────────────────────────────────────
# Each returns a finished RGBA image. Kept deliberately literal (explicit
# coordinates, no clever abstractions) — these are drawings, and the next person
# to nudge one wants to see the numbers, not decode a layout engine.

def icon_material_function():
    """A graph function: pins on both sides of a rounded body holding 'f'."""
    im, d = canvas()
    body = (M + 46, M + 60, S - M - 46, S - M - 60)
    rrect(d, body, 46)
    text_centered(d, "f", 250, cy=S // 2 - 8)
    for y in (S // 2 - 74, S // 2 + 74):                 # input pins (left)
        d.line([(M - 6, y), (body[0], y)], fill=WHITE, width=W - 6)
        d.ellipse([M - 30, y - 24, M + 18, y + 24], fill=WHITE)
    d.line([(body[2], S // 2), (S - M + 6, S // 2)], fill=WHITE, width=W - 6)
    d.ellipse([S - M - 18, S // 2 - 24, S - M + 30, S // 2 + 24], fill=WHITE)
    return im


def icon_shader():
    """A shader ball: lit sphere with a crescent terminator and a highlight.

    Built by masking rather than pieslice — a pie wedge reads as a chart, and the
    whole point of this glyph is that it looks SHADED.
    """
    im, d = canvas()
    box = (M + 10, M + 10, S - M - 10, S - M - 10)
    lit = Image.new("L", (S, S), 0)
    ld = ImageDraw.Draw(lit)
    ld.ellipse(box, fill=255)                                     # full disc …
    ld.ellipse((box[0] + 118, box[1] + 118, box[2] + 210, box[3] + 210), fill=0)  # … minus the shadow
    im.paste(Image.new("RGBA", (S, S), WHITE), (0, 0), lit)
    d.ellipse(box, outline=WHITE, width=W)
    # Filling the LIT side (not the shadow) is what keeps this reading as a
    # sphere: on a white-on-transparent glyph ink means light, and the inverse
    # looked like a crescent moon. No specular — at 60px in the grid it merged
    # with the terminator and turned the whole glyph into a comma.
    return im


def icon_prefab():
    """Stacked cards: a template and the instances made from it.

    Two shapes were tried and rejected at grid size — a frame holding a circle
    and a triangle IS the existing Texture glyph (sun over mountain), and a
    banded package collapsed into a 2x2 pane grid. An isometric cube is out too:
    that is Model3D. Offset cards are the only motif in the set that is unique
    at 60px, and 'a thing you instance' is exactly what a prefab is.
    """
    im, d = canvas()
    step, side = 62, 268
    x, y = M - 24, M + 122
    for i in range(3):                       # back to front
        box = (x + i * step, y - i * step, x + i * step + side, y - i * step + side)
        if i < 2:                            # knock the card out of the one behind it
            d.rounded_rectangle((box[0] - 16, box[1] - 16, box[2] + 16, box[3] + 16),
                                radius=44, fill=(255, 255, 255, 0))
        d.rounded_rectangle(box, radius=34, outline=WHITE, width=W - 4)
    return im


def icon_animation_clip():
    """A timeline with diamond keyframes."""
    im, d = canvas()
    d.line([(M, S // 2), (S - M, S // 2)], fill=WHITE, width=W - 6)
    for cx, r in ((M + 46, 56), (S // 2, 78), (S - M - 46, 56)):
        d.polygon([(cx, S // 2 - r), (cx + r, S // 2), (cx, S // 2 + r), (cx - r, S // 2)],
                  fill=WHITE)
    return im


def icon_property_anim_clip():
    """An animation CURVE — property animation, as opposed to a skeletal clip."""
    im, d = canvas()
    pts = []
    for i in range(65):                                   # ease-in-out S curve
        t = i / 64
        x = M + t * (S - 2 * M)
        y = S - M - (t * t * (3 - 2 * t)) * (S - 2 * M)
        pts.append((x, y))
    d.line(pts, fill=WHITE, width=W, joint="curve")
    for (cx, cy) in (pts[0], pts[-1]):                    # end keys
        d.ellipse([cx - 34, cy - 34, cx + 34, cy + 34], fill=WHITE)
    return im


def icon_widget():
    """A UI window: chrome dots in the title bar, then a button and a slider.

    The window chrome carries the weight here — without it this silhouette is a
    rounded rectangle with something inside, i.e. the key cap below.
    """
    im, d = canvas()
    frame = (M - 10, M + 20, S - M + 10, S - M - 20)
    rrect(d, frame, 30)
    d.rectangle((frame[0] + W, frame[1] + W, frame[2] - W, frame[1] + 100), fill=WHITE)
    for i in range(3):                                        # title-bar dots (knocked out)
        cx = frame[0] + 74 + i * 56
        d.ellipse([cx - 17, frame[1] + 51, cx + 17, frame[1] + 85], fill=(255, 255, 255, 0))
    d.rounded_rectangle((frame[0] + 56, frame[1] + 158, frame[0] + 224, frame[1] + 234),
                        radius=18, fill=WHITE)
    d.line([(frame[0] + 56, frame[3] - 88), (frame[2] - 56, frame[3] - 88)],
           fill=WHITE, width=20)
    d.ellipse([frame[2] - 172, frame[3] - 126, frame[2] - 96, frame[3] - 50], fill=WHITE)
    return im


def icon_horizoncode_class():
    """Two wired graph nodes — the visual-scripting glyph."""
    im, d = canvas()
    a = (M - 10, M + 26, M + 176, M + 186)
    b = (S - M - 176, S - M - 186, S - M + 10, S - M - 26)
    rrect(d, a, 26, width=W - 6)
    rrect(d, b, 26, width=W - 6)
    d.line([(a[2], (a[1] + a[3]) // 2), (S // 2, (a[1] + a[3]) // 2),
            (S // 2, (b[1] + b[3]) // 2), (b[0], (b[1] + b[3]) // 2)],
           fill=WHITE, width=W - 10, joint="curve")
    d.ellipse([a[2] - 22, (a[1] + a[3]) // 2 - 22, a[2] + 22, (a[1] + a[3]) // 2 + 22], fill=WHITE)
    d.ellipse([b[0] - 22, (b[1] + b[3]) // 2 - 22, b[0] + 22, (b[1] + b[3]) // 2 + 22], fill=WHITE)
    return im


def keycap(d, box, r=34, width=W - 6, face_inset=26):
    """A key cap: outline plus a raised face that stops short of the bottom edge,
    which is what makes it read as a key rather than a plain rounded box."""
    x0, y0, x1, y1 = box
    d.rounded_rectangle(box, radius=r, outline=WHITE, width=width)
    d.rounded_rectangle((x0 + face_inset + width, y0 + face_inset + width,
                         x1 - face_inset - width, y1 - face_inset * 2 - width),
                        radius=max(6, r - 18), fill=WHITE)


def icon_input_action():
    """One key cap — a single named input."""
    im, d = canvas()
    keycap(d, (M + 6, M + 34, S - M - 6, S - M - 10), r=52, width=W, face_inset=34)
    return im


def icon_input_mapping_context():
    """Several keys bound to one action: the arrow into the action dot is the
    whole point of a mapping context."""
    im, d = canvas()
    top, size, gap = M + 74, 118, 16
    for i in range(2):                                    # a little 2x2 key block
        for j in range(2):
            x = M - 20 + i * (size + gap)
            y = top + j * (size + gap)
            keycap(d, (x, y, x + size, y + size), r=22, width=20, face_inset=14)
    ax = M - 20 + 2 * size + gap
    arrow(d, ax + 34, S // 2, S - M - 96, S // 2, width=W - 10, head=40)
    d.ellipse([S - M - 84, S // 2 - 52, S - M + 20, S // 2 + 52], outline=WHITE, width=W - 8)
    return im


def icon_particle_system():
    """A burst: dense core, sparser and smaller outward."""
    im, d = canvas()
    cx, cy = S // 2 - 40, S // 2 + 40
    d.ellipse([cx - 56, cy - 56, cx + 56, cy + 56], fill=WHITE)
    specks = [(120, -35, 30), (196, -18, 24), (150, -62, 26), (232, -50, 18),
              (110, -78, 22), (196, -82, 16), (258, -30, 14), (150, -8, 20),
              (280, -66, 12), (86, -12, 18)]
    for dist, ang, r in specks:
        a = math.radians(ang)
        px, py = cx + dist * math.cos(a), cy + dist * math.sin(a)
        d.ellipse([px - r, py - r, px + r, py + r], fill=WHITE)
    return im


def icon_animator_state_machine():
    """Two states and a transition. The arrow carries the meaning, so it is as
    heavy as the states themselves — a hairline connector disappeared entirely
    at grid size and left two unexplained circles."""
    im, d = canvas()
    a = (M - 20, M - 10, M + 168, M + 146)
    b = (S - M - 168, S - M - 146, S - M + 20, S - M + 10)
    rrect(d, a, 78, width=W)
    rrect(d, b, 78, width=W)
    arrow(d, a[2] - 4, a[3] - 10, b[0] + 4, b[1] + 10, width=W, head=62)
    return im


def icon_font():
    """'Aa' set in the editor's own UI face."""
    im, d = canvas()
    text_centered(d, "Aa", 340)
    d.line([(M + 10, S - M - 6), (S - M - 10, S - M - 6)], fill=WHITE, width=W - 12)
    return im


ICONS = {
    "MaterialFunction":    icon_material_function,
    "Shader":              icon_shader,
    "Prefab":              icon_prefab,
    "AnimationClip":       icon_animation_clip,
    "PropertyAnimClip":    icon_property_anim_clip,
    "Widget":              icon_widget,
    "HorizonCodeClass":    icon_horizoncode_class,
    "InputAction":         icon_input_action,
    "InputMappingContext": icon_input_mapping_context,
    "ParticleSystem":      icon_particle_system,
    "AnimatorStateMachine": icon_animator_state_machine,
    "Font":                icon_font,
}


def contact_sheet(path, extra_dir=None):
    """All generated icons (plus the pre-existing set, when extra_dir is given)
    over the editor's panel grey — they are white glyphs, so a white background
    would show nothing at all."""
    items = [(n, f()) for n, f in ICONS.items()]
    if extra_dir:
        for f in sorted(os.listdir(extra_dir)):
            if f.endswith(".png") and f[:-4] not in ("moon", "HC_Logo"):
                items.append((f[:-4] + " (alt)", Image.open(os.path.join(extra_dir, f)).convert("RGBA")))
    cell, cols = 128, 6
    rows = (len(items) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell, rows * (cell + 22)), (38, 38, 42))
    d = ImageDraw.Draw(sheet)
    label = ImageFont.truetype(FONT_PATH, 15)
    for i, (name, im) in enumerate(items):
        g = im.resize((cell - 26, cell - 26), Image.LANCZOS)
        x, y = (i % cols) * cell + 13, (i // cols) * (cell + 22) + 6
        sheet.paste(g, (x, y), g)
        d.text((x - 8, y + cell - 18), name[:17], font=label, fill=(226, 226, 230))
    sheet.save(path)
    return path


def main():
    args = [a for a in sys.argv[1:]]
    if args and args[0] == "--sheet":
        out = args[1] if len(args) > 1 else "icons_sheet.png"
        print(contact_sheet(out, os.path.join(REPO, "EditorDeps", "Images")))
        return
    outdir = args[0] if args else os.path.join(REPO, "EditorDeps", "Images")
    os.makedirs(outdir, exist_ok=True)
    for name, fn in ICONS.items():
        p = os.path.join(outdir, name + ".png")
        fn().save(p)
        print("wrote", p)


if __name__ == "__main__":
    main()
