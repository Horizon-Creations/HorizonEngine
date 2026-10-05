#!/usr/bin/env python3
"""he_vk_imagetests — Vulkan image tests on a software ICD (lavapipe) for CI.

Drives the deployed HorizonEditor's headless frame dump (HE_DUMP_* env vars,
see EditorApplication::dumpFrameHeadless) with HE_DUMP_RHI=Vulkan and judges
the pictures as A/B pairs taken in the SAME run — never against checked-in
reference images: lavapipe's output moves with the Mesa version and with the
code LLVM emits for the runner's CPU (docs/ci-software-vulkan-icd-analyse-
2026-10-03.md §4).

Usage:
    scripts/he_vk_imagetests.py --editor out/deploy/Editor/HorizonEditor \\
        --out vk-shots [--cases nebula,clustered,gi] [--timeout 900]

Per shot: a private HOME / HE_CONFIG_DIR (no config.json, no last project, no
remembered dialog state leaks between shots), the editor started with
HE_DUMP_PATH + HE_DUMP_QUIT=1, its stdout/stderr saved next to the BMP.

The verdict comes from the BMP, not from the exit code: the editor is known to
crash on the dump-quit teardown AFTER the file is written (he_shot.py). Exit
codes and run times are logged, and everything lands in metrics.json.

No PIL, no sips: the dump writer emits a 32-bit BGRA top-down BMP, read here
with struct (24-bit and bottom-up are handled too).

Exit code 0 = every selected case passed, 1 = a case failed, 2 = usage.
"""
import argparse, json, os, pathlib, re, shutil, struct, subprocess, sys, time

# ── Cases ────────────────────────────────────────────────────────────────────
# Each case: shared HE_DUMP_* keys (`base`), then named variants that add keys
# (`dump`) or raw environment (`env`). `pairs` are the A/B comparisons;
# `min_mean` is the least mean |Δ| (8-bit steps, all channels) that counts as
# "the feature changed the picture"; None = report only. `require`/`forbid`
# are log witnesses checked on the named variant's log; `allow` lists the
# validation messages a variant may print (see ALLOWED_VALIDATION).
#
# Thresholds sit at roughly half of what lavapipe measured (Mesa 25.2.8,
# runs 37112790557 / 37113763253 on 03.10.2026): nebula 13.1–13.4, clustered
# 2.97, GI 0.46. They separate "the feature drew" from "the feature is
# silently off on Vulkan" (A/B byte-identical), not one shade from another.
# Noise floor, same shot twice: clustered bit-identical across runs; GI
# max |Δ| 1 on 6 pixels (so GI's 0.46 is signal); nebula up to 1.4 between
# any two shots, also within one run (the star field moves), well under 5.0.
CASES = {
    # Nebula on the night sky (docs/nebula-backend-parity-analysis-2026-10-01.md).
    # Dome clouds at zero coverage: the volumetric march is the costliest thing
    # a CPU can be asked to draw and says nothing about the nebula.
    "nebula": {
        "base": {"SKYTEST": "1", "TOD": "0", "CLOUDMODE": "0", "COVERAGE": "0",
                 "NEBQUALITY": "2", "PITCH": "60"},
        "variants": {
            "off": {"dump": {"NEBULA": "0"}},
            "on":  {"dump": {"NEBULA": "0.6"}},
        },
        "pairs": [("off", "on", 5.0)],
        "forbid": {"on": ["falling back to the reduced sky.frag"]},
    },
    # Clustered forward lighting (docs/clustered-lighting-forward-plan-2026-10-01.md
    # §3.4): 16 point lights over a node-graph floor, from above at midnight.
    # HE_FORWARD_CLUSTER=0 keeps the 8-light window — fewer pools on the floor.
    "clustered": {
        "base": {"SKYTEST": "1", "MANYLIGHTS": "16", "TOD": "0", "CAMY": "207",
                 "CAMZ": "2", "PITCH": "-38", "CLOUDMODE": "0", "COVERAGE": "0"},
        "variants": {
            "window":    {"env": {"HE_FORWARD_CLUSTER": "0"}},
            "clustered": {},
        },
        "pairs": [("window", "clustered", 1.5)],
        "require": {"clustered": ["HE_DUMP_MANYLIGHTS witness scene added"]},
    },
    # DDGI diffuse on the GI-reflections witness scene (graph-material cubes, one
    # emissive, on a mirror floor), with the reflections pinned OFF so the A/B is
    # the probe irradiance alone. "sw" forces the software BVH; on lavapipe "on"
    # takes the VK_KHR_ray_query path.
    #
    # NOT the painted terrain (HE_DUMP_LANDSCAPELAYERS) of the GI doc §7.2: its
    # layer-blend material reads binding 14 (heLandscapeWeights), which the Vulkan
    # material layout lacks. NVIDIA answers that with a validation message,
    # lavapipe with SIGSEGV in the draw — first lavapipe run, 03.10.2026.
    "gi": {
        "base": {"GIREFLTEST": "1", "GIREFL": "0", "SKYTEST": "1", "CAMY": "3", "CAMZ": "0",
                 "PITCH": "-12", "TOD": "0.4", "CLOUDMODE": "0", "COVERAGE": "0",
                 "FRAMES": "40"},
        "variants": {
            "off": {"dump": {"GI": "0"}},
            "on":  {"dump": {"GI": "1"}},
            "sw":  {"dump": {"GI": "1"}, "env": {"HE_GI_FORCE_SW": "1"}},
        },
        # hw vs sw is reported, not judged (min_mean None): the two paths are
        # expected to be NEAR each other (first run: max |Δ| of a few steps on a
        # handful of pixels), and "near" has no agreed bound yet.
        "pairs": [("off", "on", 0.3), ("on", "sw", None)],
        "require": {"on": ["GI probe grid", "GI hardware ray tracing available"],
                    "sw": ["GI probe grid", "software GI path forced"]},
        "allow": {"off": ["mat_ubo"], "on": ["mat_ubo"], "sw": ["mat_ubo"]},
    },
    # GI reflections on the same scene: the green and the glowing red cube should
    # appear in the mirror floor (docs/gi-reflections-plan.md). REPORT ONLY:
    # Vulkan has no GI reflections yet (giRefl gate stays 0, GI doc §7.1) and the
    # first lavapipe run measured off == on to the byte. The number is here so the
    # day it moves is visible; make it a judged pair once Vulkan draws them.
    "gi_refl": {
        "base": {"GIREFLTEST": "1", "GI": "1", "SKYTEST": "1", "CAMY": "3", "CAMZ": "0",
                 "PITCH": "-12", "TOD": "0.4", "CLOUDMODE": "0", "COVERAGE": "0",
                 "FRAMES": "40"},
        "variants": {
            "off": {"dump": {"GIREFL": "0"}},
            "on":  {"dump": {"GIREFL": "1"}},
        },
        "pairs": [("off", "on", None)],
        "require": {"on": ["HE_DUMP_GIREFLTEST witness scene added"]},
        "allow": {"off": ["mat_ubo"], "on": ["mat_ubo"]},
    },
}

# Validation messages a case may print while its picture is made, by key:
# (regex, why it is tolerated). A case opts in per variant with "allow"; any
# other message between "frame dump armed" and "frame dumped" fails the case.
# Nebula and clustered allow nothing — the first lavapipe run (Mesa 25.2.8,
# Khronos layers 1.3.275) printed no message for them at all.
#
# Every entry is an open renderer bug found by this job (03.10.2026), not a
# layer quirk. Fix the bug, then delete the entry — the case turns strict.
ALLOWED_VALIDATION = {
    # Built-in (non-graph) material draws write the shared per-draw material UBO
    # with vkCmdUpdateBuffer + a buffer barrier INSIDE the scene render pass
    # (VulkanRenderer.cpp, drawDCVk and its instanced twin: "Update material
    # UBO"). Two messages per built-in draw per frame.
    "mat_ubo": [r"VUID-vkCmdUpdateBuffer-renderpass",
                r"VUID-vkCmdPipelineBarrier-None-07889"],
}

LOG_MARK_ARMED  = "frame dump armed"
LOG_MARK_DUMPED = "frame dumped ("
RE_DEVICE       = re.compile(r"VulkanRenderer: device 0 of \d+: (.*)")
RE_VALIDATION   = re.compile(r"Vulkan validation: (.*)")


# ── BMP ──────────────────────────────────────────────────────────────────────
def read_bmp(path):
    """→ (width, height, bytes BGR per pixel, rows top-down)."""
    data = pathlib.Path(path).read_bytes()
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp not in (24, 32):
        raise ValueError(f"unsupported BMP depth {bpp}")
    px = bpp // 8
    stride = (w * px + 3) & ~3
    top_down = h < 0
    h = abs(h)
    out = bytearray(w * h * 3)
    for y in range(h):
        src_y = y if top_down else h - 1 - y
        row = data[off + src_y * stride: off + src_y * stride + w * px]
        if px == 3:
            out[y * w * 3:(y + 1) * w * 3] = row
        else:
            o = y * w * 3
            out[o + 0:o + w * 3:3] = row[0::4]
            out[o + 1:o + w * 3:3] = row[1::4]
            out[o + 2:o + w * 3:3] = row[2::4]
    return w, h, bytes(out)


def image_stats(pix):
    n = len(pix)
    mean = sum(pix) / n if n else 0.0
    return {"mean": round(mean, 3), "min": min(pix) if n else 0, "max": max(pix) if n else 0}


def diff_stats(a, b):
    """Mean |Δ| over all channels, the largest |Δ|, how many pixels differ at
    all, and the share of pixels with any channel off by more than 2."""
    total = over = changed = peak = 0
    for i in range(0, len(a), 3):
        d0 = abs(a[i] - b[i]); d1 = abs(a[i + 1] - b[i + 1]); d2 = abs(a[i + 2] - b[i + 2])
        m = max(d0, d1, d2)
        if m:
            total += d0 + d1 + d2
            changed += 1
            peak = max(peak, m)
            if m > 2:
                over += 1
    px = len(a) // 3
    return {"mean_abs": round(total / len(a), 4), "max_abs": peak, "px_changed": changed,
            "pct_px_over2": round(100.0 * over / px, 3)}


# ── One shot ─────────────────────────────────────────────────────────────────
def run_shot(editor, outdir, name, dump_kv, env_kv, timeout):
    bmp = outdir / f"{name}.bmp"
    log = outdir / f"{name}.log"
    home = outdir / "_homes" / name
    shutil.rmtree(home, ignore_errors=True)
    (home / "config").mkdir(parents=True)
    if bmp.exists():
        bmp.unlink()

    env = dict(os.environ)
    env["HOME"] = str(home)
    env["XDG_CONFIG_HOME"] = str(home / ".config")
    env["XDG_DATA_HOME"] = str(home / ".local" / "share")
    env["HE_CONFIG_DIR"] = str(home / "config")
    env.setdefault("HE_COLLAB_OFFLINE", "1")
    env.setdefault("HE_HIDDEN_WINDOW", "1")
    env.setdefault("HE_SKY_TIME", "10")
    env["HE_DUMP_RHI"] = "Vulkan"
    env["HE_DUMP_PATH"] = str(bmp)
    env["HE_DUMP_QUIT"] = "1"
    for k, v in dump_kv.items():
        env[f"HE_DUMP_{k}"] = v
    env.update(env_kv)

    t0 = time.monotonic()
    rc, timed_out = None, False
    with open(log, "wb") as lf:
        try:
            rc = subprocess.run([str(editor)], env=env, stdout=lf, stderr=subprocess.STDOUT,
                                cwd=str(pathlib.Path(editor).parent), timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            timed_out = True
    secs = round(time.monotonic() - t0, 1)

    text = log.read_text(errors="replace")
    res = {"name": name, "seconds": secs, "exit_code": rc, "timed_out": timed_out,
           "dump": dump_kv, "env": env_kv, "bmp": bmp.name, "log": log.name}
    m = RE_DEVICE.search(text)
    res["device"] = m.group(1).strip() if m else None
    # Validation messages while the picture is made — not the shutdown leak
    # list, which the dump-quit teardown prints after the BMP is written.
    a = text.find(LOG_MARK_ARMED)
    d = text.find(LOG_MARK_DUMPED)
    window = text[a if a >= 0 else 0: d if d >= 0 else len(text)]
    res["validation_in_dump"] = [v.strip() for v in RE_VALIDATION.findall(window)]
    res["validation_total"] = len(RE_VALIDATION.findall(text))
    res["dumped_line"] = LOG_MARK_DUMPED in text
    try:
        w, h, pix = read_bmp(bmp)
        res.update({"width": w, "height": h, "image": image_stats(pix)})
        res["_pix"] = pix
    except (OSError, ValueError, struct.error) as e:
        res["bmp_error"] = str(e)
    res["_log"] = text
    return res


def tail(text, n=60):
    return "\n".join(text.splitlines()[-n:])


# ── Main ─────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--editor", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cases", default=",".join(CASES))
    ap.add_argument("--timeout", type=int, default=900, help="seconds per shot")
    ap.add_argument("--require-device", default="llvmpipe",
                    help="substring the logged Vulkan device name must contain ('' = any)")
    args = ap.parse_args()

    editor = pathlib.Path(args.editor).resolve()
    if not editor.is_file():
        print(f"::error::editor not found: {editor}")
        return 2
    outdir = pathlib.Path(args.out).resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    selected = [c.strip() for c in args.cases.split(",") if c.strip()]
    unknown = [c for c in selected if c not in CASES]
    if unknown:
        print(f"::error::unknown case(s): {', '.join(unknown)} (known: {', '.join(CASES)})")
        return 2

    print(f"he_vk_imagetests: editor {editor}")
    print(f"  SDL_VIDEO_DRIVER={os.environ.get('SDL_VIDEO_DRIVER', '(unset)')} "
          f"VK_DRIVER_FILES={os.environ.get('VK_DRIVER_FILES', '(unset)')} "
          f"DISPLAY={os.environ.get('DISPLAY', '(unset)')}")

    report = {"cases": {}, "failed": []}
    for case in selected:
        spec = CASES[case]
        print(f"\n── {case} " + "─" * (60 - len(case)), flush=True)
        shots, problems = {}, []
        for vname, v in spec["variants"].items():
            dump_kv = dict(spec["base"]); dump_kv.update(v.get("dump", {}))
            shot = run_shot(editor, outdir, f"{case}_{vname}", dump_kv, v.get("env", {}), args.timeout)
            shots[vname] = shot
            img = shot.get("image")
            print(f"  {vname:10s} {shot['seconds']:7.1f} s  exit={shot['exit_code']}"
                  f"{' TIMEOUT' if shot['timed_out'] else ''}  "
                  f"{'%dx%d' % (shot['width'], shot['height']) if img else 'NO IMAGE'}"
                  f"  mean={img['mean'] if img else '-'}  device={shot['device']}"
                  f"  validation(dump)={len(shot['validation_in_dump'])}"
                  f"/total={shot['validation_total']}", flush=True)
            for line in sorted(set(shot["validation_in_dump"]))[:10]:
                print(f"      validation: {line[:300]}")
            if not img:
                problems.append(f"{vname}: no image ({shot.get('bmp_error', 'missing')})")
                print(f"  ── last log lines of {shot['log']} ──")
                print(tail(shot["_log"]))
                continue
            if args.require_device and (not shot["device"] or args.require_device not in shot["device"]):
                problems.append(f"{vname}: device '{shot['device']}' is not '{args.require_device}'")
            if img["min"] == img["max"]:
                problems.append(f"{vname}: image is one flat value ({img['min']})")
            allowed = [rx for key in spec.get("allow", {}).get(vname, [])
                       for rx in ALLOWED_VALIDATION[key]]
            unexpected = [v for v in shot["validation_in_dump"]
                          if not any(re.search(rx, v) for rx in allowed)]
            if unexpected:
                problems.append(f"{vname}: {len(unexpected)} validation message(s) while drawing")
            if shot["exit_code"] != 0:
                # Reported, not judged: the BMP is written before the teardown.
                print(f"::warning::{case}/{vname}: editor exit code {shot['exit_code']}"
                      f"{' (timeout)' if shot['timed_out'] else ''} after writing the image")
            for needle in spec.get("require", {}).get(vname, []):
                if needle not in shot["_log"]:
                    problems.append(f"{vname}: log lacks '{needle}'")
            for needle in spec.get("forbid", {}).get(vname, []):
                if needle in shot["_log"]:
                    problems.append(f"{vname}: log has '{needle}'")

        pairs = []
        for a, b, min_mean in spec["pairs"]:
            sa, sb = shots.get(a), shots.get(b)
            if not (sa and sb and "_pix" in sa and "_pix" in sb):
                continue
            if (sa["width"], sa["height"]) != (sb["width"], sb["height"]):
                problems.append(f"{a}/{b}: size differs")
                continue
            ds = diff_stats(sa["_pix"], sb["_pix"])
            verdict = "report" if min_mean is None else ("ok" if ds["mean_abs"] >= min_mean else "FAIL")
            if verdict == "FAIL":
                problems.append(f"{a} vs {b}: mean |Δ| {ds['mean_abs']} < {min_mean}")
            pairs.append({"a": a, "b": b, "min_mean": min_mean, "verdict": verdict, **ds})
            print(f"  {a} vs {b}: mean |Δ| {ds['mean_abs']}, max {ds['max_abs']}, "
                  f"{ds['px_changed']} px changed, px>2 {ds['pct_px_over2']} %"
                  f"  → {verdict}{'' if min_mean is None else f' (min {min_mean})'}")

        for p in problems:
            print(f"::error::{case}: {p}")
        if problems:
            report["failed"].append(case)
        report["cases"][case] = {
            "ok": not problems, "problems": problems, "pairs": pairs,
            "shots": {k: {kk: vv for kk, vv in s.items() if not kk.startswith("_")}
                      for k, s in shots.items()},
        }

    shutil.rmtree(outdir / "_homes", ignore_errors=True)
    (outdir / "metrics.json").write_text(json.dumps(report, indent=2))
    print(f"\nhe_vk_imagetests: {len(selected) - len(report['failed'])}/{len(selected)} case(s) passed"
          f"{' — failed: ' + ', '.join(report['failed']) if report['failed'] else ''}")
    return 1 if report["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
