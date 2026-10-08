#!/usr/bin/env python3
"""Thema 177: look into a texture .hasset (the landscape layer textures and arrays) from Python.

    python3 hasset_tex.py FILE.hasset                       header line: size, mips, layers, sRGB, UUID
    python3 hasset_tex.py FILE.hasset --png OUT.png [--slice N] [--mip M]
                                                            write one slice / mip as a PNG (RGBA8 only)
    python3 hasset_tex.py FILE.hasset --compare SRC.png [--slice N] [--channel R|G|B|A|RGB]
                                                            mip 0 of the slice against a staged PNG:
                                                            max / mean difference per channel

Rows in the file are stored BOTTOM-UP (the importer flips on load, row 0 = v 0), so the
PNG views are flipped back and --compare flips SRC.png the same way. A slice of an array
texture is slice-major with its own mip chain (the D3D subresource order, §8.1 of
docs/auto-landscape-material-textures.md). Needs Pillow; run with `python3 -I`.
"""
import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image, ImageChops, ImageStat
except ImportError as e:  # pragma: no cover
    sys.exit(f"hasset_tex: {e}. pip install Pillow")

Image.MAX_IMAGE_PIXELS = None
CHUNK_META, CHUNK_TXMI, CHUNK_PIXL = b"META", b"TXMI", b"PIXL"


def read_hasset(path):
    raw = Path(path).read_bytes()
    if raw[:4] != b"HAST":
        sys.exit(f"hasset_tex: {path} is not a .hasset")
    version, atype, nchunks = struct.unpack_from("<HHI", raw, 4)
    off, chunks = 32, {}
    for _ in range(nchunks):
        cid, size = struct.unpack_from("<4sQ", raw, off)
        off += 12
        chunks[cid] = raw[off:off + size]
        off += size
    meta, txmi = chunks[CHUNK_META], chunks[CHUNK_TXMI]
    _, hi, lo = struct.unpack_from("<HQQ", meta, 0)
    if len(txmi) >= 24:   # legacy size_t layout (HAsset.h kTextureHeaderLegacyMinSize)
        w, h, ch = struct.unpack_from("<QQQ", txmi, 0)
        o = 24
    else:
        w, h, ch = struct.unpack_from("<III", txmi, 0)
        o = 12
    mips = struct.unpack_from("<I", txmi, o)[0] if o + 4 <= len(txmi) else 1
    fmt = txmi[o + 4] if o + 5 <= len(txmi) else 0
    srgb = bool(txmi[o + 5]) if o + 6 <= len(txmi) else False
    layers = struct.unpack_from("<I", txmi, o + 6)[0] if o + 10 <= len(txmi) else 1
    return dict(width=w, height=h, channels=ch, mips=max(mips, 1), format=fmt, srgb=srgb,
                layers=max(layers, 1), uuid=(hi, lo), pixels=chunks[CHUNK_PIXL])


def slice_bytes(t, layer, mip):
    """Offset/size of one (layer, mip) of an RGBA8 texture."""
    sizes = []
    w, h = t["width"], t["height"]
    for _ in range(t["mips"]):
        sizes.append((w, h, w * h * 4))
        w, h = max(w // 2, 1), max(h // 2, 1)
    per_layer = sum(s[2] for s in sizes)
    off = layer * per_layer + sum(s[2] for s in sizes[:mip])
    return off, sizes[mip]


def view(t, layer, mip):
    if t["format"] != 0 or t["channels"] != 4:
        sys.exit(f"hasset_tex: format {t['format']} / {t['channels']} channels is not RGBA8")
    off, (w, h, n) = slice_bytes(t, layer, mip)
    if off + n > len(t["pixels"]):
        sys.exit("hasset_tex: slice/mip is outside the pixel data")
    im = Image.frombytes("RGBA", (w, h), t["pixels"][off:off + n])
    return im.transpose(Image.FLIP_TOP_BOTTOM)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--png")
    ap.add_argument("--compare")
    ap.add_argument("--slice", type=int, default=0)
    ap.add_argument("--mip", type=int, default=0)
    ap.add_argument("--channel", default="RGB")
    a = ap.parse_args()

    t = read_hasset(a.file)
    expect = sum(max(t["width"] >> m, 1) * max(t["height"] >> m, 1) * 4 for m in range(t["mips"])) * t["layers"]
    print(f"{Path(a.file).name}: {t['width']}x{t['height']} ch{t['channels']} mips {t['mips']} "
          f"layers {t['layers']} {'sRGB' if t['srgb'] else 'linear'} format {t['format']} "
          f"uuid {t['uuid'][0]:#x}/{t['uuid'][1]:#x} pixels {len(t['pixels'])} B"
          + ("" if len(t["pixels"]) == expect else f"  (RGBA8 size would be {expect}!)"))
    if a.png:
        view(t, a.slice, a.mip).save(a.png)
        print(f"wrote {a.png}")
    if a.compare:
        ref = Image.open(a.compare).convert("RGBA")
        got = view(t, a.slice, 0)
        if ref.size != got.size:
            sys.exit(f"hasset_tex: sizes differ, file {got.size} vs {a.compare} {ref.size}")
        for name, idx in (("R", 0), ("G", 1), ("B", 2), ("A", 3)):
            if name not in a.channel.upper() and a.channel.upper() != "RGB":
                continue
            if a.channel.upper() == "RGB" and name == "A":
                continue
            d = ImageChops.difference(got.getchannel(idx), ref.getchannel(idx))
            print(f"  {name}: max |diff| {d.getextrema()[1]}, mean {ImageStat.Stat(d).mean[0]:.4f}")


if __name__ == "__main__":
    main()
