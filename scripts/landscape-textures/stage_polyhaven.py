#!/usr/bin/env python3
"""Thema 177, Schritt 1: stage downloaded landscape textures for `landscape_tex_gen --pack`.

    python3 stage_polyhaven.py <source-root> <staging-dir> [--size 2048]

<source-root> holds one folder per layer (Grass, Dirt, Rock, Snow, WetGround; case and
spelling are forgiving, "DIrt" works), each with the files of ONE downloaded texture set
as they come from Poly Haven / ambientCG (docs/auto-landscape-material-textures.md §4):

    Albedo     *_diff_*  *_diffuse_*  *_color_*  *_albedo_*  *_basecolor_*   jpg/png/exr
    Normal     *_nor_gl_*  *_normalgl_*  *_normal_*                           png/exr  (NOT nor_dx / NormalDX)
    Roughness  *_rough_*  *_roughness_*                                       jpg/png/exr
    AO         *_ao_*  *_ambientocclusion_*  (or the R channel of *_arm_*)
    Height     *_disp_*  *_displacement_*  *_height_*                         png/exr

Everything else in a folder (translucent, metal, .blend, ...) is listed and ignored.

<staging-dir> gets the flat 8-bit PNG set `landscape_tex_gen --pack` reads:
`<Layer>_{Albedo,Normal,Roughness,AO,Height}.png`, every map `--size` x `--size`, plus
`stage_manifest.txt` (which source file became which map: the licence/provenance note
of §4.1). A map a set does not have is simply not written; `--pack` fills it with its
neutral value (AO white, height mid-grey, flat normal) and says so.

What it does that `--pack` cannot:
  * reads EXR (stb_image, which the importer uses, cannot). Poly Haven ships its
    normal maps as DWAA-compressed half-float EXR, so this needs the OpenEXR module.
  * shrinks 4K -> 2K by exact 2x2 averaging in float, and renormalises the normals
    afterwards (an average of unit vectors is shorter than 1).
  * stretches the height to the full 0..1 range over the tile (0.5 % / 99.5 % clip),
    which is what the height blend between layers and the puddles expect (§4.2).
It does NOT flip anything: the importer flips on load, so the files stay as downloaded
(normal maps in OpenGL convention, green = +Y).

Needs `pip install numpy Pillow OpenEXR` (use a throw-away venv). Run it with `python3 -I`.
"""
import argparse
import re
import sys
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError as e:  # pragma: no cover
    sys.exit(f"stage_polyhaven: {e}. pip install numpy Pillow OpenEXR (in a venv)")

Image.MAX_IMAGE_PIXELS = None

LAYERS = ["Grass", "Dirt", "Rock", "Snow", "WetGround"]
MAPS = ["Albedo", "Normal", "Roughness", "AO", "Height"]

# token (of the lower-cased file stem) -> map; the first hit in this order wins.
TOKENS = [
    ("Normal",    {"nor_gl", "normalgl", "normal_gl", "nor", "normal"}),
    ("Albedo",    {"diff", "diffuse", "color", "colour", "albedo", "basecolor", "base_color"}),
    ("Roughness", {"rough", "roughness"}),
    ("AO",        {"ao", "ambientocclusion", "occlusion"}),
    ("Height",    {"disp", "displacement", "height"}),
    ("ARM",       {"arm"}),
]
SKIP_TOKENS = {"nor_dx", "normaldx", "normal_dx", "dx"}
EXT_RANK = {".exr": 0, ".png": 0, ".tif": 0, ".tiff": 0, ".jpg": 1, ".jpeg": 1}


def norm_layer(name):
    key = re.sub(r"[^a-z]", "", name.lower())
    return {"grass": "Grass", "dirt": "Dirt", "rock": "Rock", "snow": "Snow",
            "wetground": "WetGround", "wet": "WetGround", "mud": "WetGround"}.get(key)


def classify(path):
    stem = path.stem.lower()
    parts = re.split(r"[^a-z0-9]+", stem)
    pairs = {"_".join(parts[i:i + 2]) for i in range(len(parts) - 1)}
    words = set(parts) | pairs | {stem}
    if words & SKIP_TOKENS:
        return "skip-dx"
    for kind, names in TOKENS:
        if words & names:
            return kind
    return None


def read_exr(path):
    try:
        import OpenEXR
    except ImportError:
        sys.exit("stage_polyhaven: reading .exr needs the OpenEXR module (pip install OpenEXR)")
    ch = OpenEXR.File(str(path)).channels()
    if "RGB" in ch:
        a = np.asarray(ch["RGB"].pixels, dtype=np.float32)
    elif all(k in ch for k in "RGB"):
        a = np.stack([np.asarray(ch[k].pixels, dtype=np.float32) for k in "RGB"], axis=-1)
    else:
        a = np.asarray(next(iter(ch.values())).pixels, dtype=np.float32)
    return a, "float"


def read_any(path):
    """-> (float32 array HxW or HxWxC in 0..1 (EXR may exceed it), 'float'|'8bit'|'16bit')."""
    if path.suffix.lower() == ".exr":
        return read_exr(path)
    im = Image.open(path)
    if im.mode in ("I;16", "I;16L", "I;16B", "I"):
        return np.asarray(im, dtype=np.float32) / 65535.0, "16bit"
    if im.mode in ("L", "P", "LA"):
        return np.asarray(im.convert("L"), dtype=np.float32) / 255.0, "8bit"
    return np.asarray(im.convert("RGB"), dtype=np.float32) / 255.0, "8bit"


def to_size(a, size):
    h, w = a.shape[:2]
    if h != w:
        sys.exit(f"stage_polyhaven: not square: {w}x{h}")
    if h == size:
        return a
    if h > size and h % size == 0:
        k = h // size
        return a.reshape(size, k, size, k, *a.shape[2:]).mean(axis=(1, 3))
    chans = [a] if a.ndim == 2 else [a[..., c] for c in range(a.shape[2])]
    flt = Image.BOX if size < h else Image.BILINEAR
    out = [np.asarray(Image.fromarray(np.ascontiguousarray(c, dtype=np.float32)).resize((size, size), flt)) for c in chans]
    return out[0] if a.ndim == 2 else np.stack(out, axis=-1)


def srgb_encode(x):
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def gray(a):
    return a if a.ndim == 2 else a[..., 0]


def u8(a):
    return np.rint(np.clip(a, 0.0, 1.0) * 255.0).astype(np.uint8)


def make_map(kind, a, how, size, stretch):
    note = []
    a = to_size(a, size)
    if kind == "Albedo":
        rgb = a if a.ndim == 3 else np.repeat(a[..., None], 3, axis=-1)
        if how == "float":
            rgb = srgb_encode(rgb)
            note.append("linear EXR -> sRGB")
        return u8(rgb), note
    if kind == "Normal":
        if a.ndim != 3:
            sys.exit("stage_polyhaven: a normal map needs three channels")
        if a.min() < -0.05:
            sys.exit("stage_polyhaven: normal map holds negative values (a -1..1 vector, not a 0..1 encoding)")
        n = np.clip(a, 0.0, 1.0) * 2.0 - 1.0
        length = np.linalg.norm(n, axis=-1, keepdims=True)
        n = np.where(length > 1e-6, n / np.maximum(length, 1e-6), np.array([0.0, 0.0, 1.0], np.float32))
        note.append("renormalised, mean |n| before %.3f" % float(length.mean()))
        return u8(n * 0.5 + 0.5), note
    g = gray(a)
    if kind == "Height":
        if stretch:
            lo, hi = np.percentile(g, [0.5, 99.5])
            if hi - lo > 1e-6:
                g = (g - lo) / (hi - lo)
                note.append("stretched %.3f..%.3f -> 0..1" % (lo, hi))
            else:
                g = np.full_like(g, 0.5)
                note.append("flat height -> 0.5")
    return u8(g), note


def pick(files):
    return sorted(files, key=lambda p: (EXT_RANK.get(p.suffix.lower(), 9), p.name))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("source", type=Path)
    ap.add_argument("staging", type=Path)
    ap.add_argument("--size", type=int, default=2048)
    ap.add_argument("--no-height-stretch", action="store_true")
    args = ap.parse_args()

    if not args.source.is_dir():
        sys.exit(f"stage_polyhaven: {args.source} is not a folder")
    src_abs, out_abs = args.source.resolve(), args.staging.resolve()
    if out_abs == src_abs or src_abs in out_abs.parents or out_abs in src_abs.parents:
        sys.exit("stage_polyhaven: the staging folder must be separate from the source folder")
    args.staging.mkdir(parents=True, exist_ok=True)

    manifest = ["# landscape textures staged by scripts/landscape-textures/stage_polyhaven.py",
                f"# size {args.size}x{args.size}, source {src_abs}", ""]
    missing, seen = [], set()
    for d in sorted(p for p in args.source.iterdir() if p.is_dir()):
        layer = norm_layer(d.name)
        if layer is None:
            print(f"[skip] folder {d.name}: not one of {', '.join(LAYERS)}")
            continue
        seen.add(layer)
        picked, extra = {}, []
        for f in sorted(p for p in d.iterdir() if p.is_file() and not p.name.startswith(".")):
            kind = classify(f)
            if kind == "skip-dx" or kind is None or f.suffix.lower() not in EXT_RANK:
                extra.append(f.name)
                continue
            picked.setdefault(kind, []).append(f)
        print(f"== {layer} ({d.name})")
        for name in extra:
            print(f"   ignored  {name}")
        arm = pick(picked.pop("ARM", []))
        outputs = {}
        for kind in MAPS:
            cands = pick(picked.get(kind, []))
            if not cands:
                continue
            f = cands[0]
            if len(cands) > 1:
                print(f"   {kind}: using {f.name}, also found {', '.join(c.name for c in cands[1:])}")
            a, how = read_any(f)
            outputs[kind] = (f, a, how, None)
        if arm and ("AO" not in outputs):
            a, how = read_any(arm[0])
            if a.ndim == 3:
                outputs["AO"] = (arm[0], a[..., 0], how, "R of ARM")
                if "Roughness" not in outputs:
                    outputs["Roughness"] = (arm[0], a[..., 1], how, "G of ARM")
        for kind in MAPS:
            if kind not in outputs:
                missing.append((layer, kind))
                print(f"   {kind:<9} (none)")
                manifest.append(f"{layer}_{kind}: none")
                continue
            f, a, how, via = outputs[kind]
            res = f"{a.shape[1]}x{a.shape[0]}"
            data, note = make_map(kind, a, how, args.size, not args.no_height_stretch)
            dest = args.staging / f"{layer}_{kind}.png"
            Image.fromarray(data).save(dest, compress_level=6)   # (H,W,3) -> RGB, (H,W) -> L
            detail = "; ".join(([via] if via else []) + note)
            print(f"   {kind:<9} {f.name} ({res}) -> {dest.name}" + (f"  [{detail}]" if detail else ""))
            manifest.append(f"{layer}_{kind}: {f.name} ({res}, {how})" + (f" [{detail}]" if detail else ""))
    for layer in LAYERS:
        if layer not in seen:
            print(f"== {layer}: no folder found")
            manifest.append(f"{layer}: no folder")
    (args.staging / "stage_manifest.txt").write_text("\n".join(manifest) + "\n")
    if missing:
        print("\nmissing (landscape_tex_gen --pack fills the neutral value): " +
              ", ".join(f"{layer}_{kind}" for layer, kind in missing))


if __name__ == "__main__":
    main()
