#!/usr/bin/env python3
"""runtime_size — what a shipped game/app runtime actually weighs.

docs/he-apps-plan.md A3 put a size table in the plan and got it wrong the first
time: the numbers came from a worktree deploy tree that was not a Release build.
That is the whole reason this exists as a script with a threshold rather than as
a paragraph — the MEASURING POINT is part of the claim, so it gets printed with
every run and named in every failure.

What it measures: the runtime directory the exporter copies verbatim into a
packaged build (out/deploy/Game by default). Files are grouped by NAME PATTERN,
never by a fixed list, and anything unrecognised lands in a visible "unaccounted"
bucket that still counts toward the total — a new dylib has to move a number, not
escape the measurement.

Two totals, because Python is the one component that is already optional per
project (settings.bundlePython): the full tree, and the tree a non-Python project
actually ships.

There are THREE such directories per platform (docs/he-apps-plan.md A3b), and
they weigh very different amounts on purpose, so each carries its own pair of
limits: the game runtime, the app runtime with Advanced Shader Effects on (one
forward renderer, no shader cross-compiler) and the one with it off (the
software rasterizer alone). --flavor says which is being weighed; without it the
directory name decides, because that name is what the exporter goes by too.

Usage:
    scripts/runtime_size.py [RUNTIME_DIR] [--check] [--build-type=TYPE]
                            [--flavor=game|app-advanced|app-basic]

    --check exits non-zero when a threshold is exceeded. Without it the script
    only reports, which is what one wants while cutting.

    --build-type is what the tree was built as. The thresholds below were
    measured on a RELEASE tree and mean nothing anywhere else — a Debug build of
    the same code is several times the size, and failing on that would be the
    check crying wolf at the one thing it is not measuring. Anything that is not
    a release configuration therefore REPORTS and skips.

Exit codes: 0 fine (or reporting), 1 over threshold, 2 nothing to measure or
nothing this may judge.
"""
import os
import sys

# ── Thresholds, in megabytes ─────────────────────────────────────────────────
# Set from what is MEASURED today plus headroom, deliberately not from the plan's
# aspirational 15 MB: that number is the target AFTER the structural work (A3b —
# the WidgetManager move out of HorizonScene, a shaderc-free runtime, one linked
# renderer). A threshold that fails on the day it is written is a threshold
# everybody learns to ignore.
#
# They exist to catch a REGRESSION — somebody linking a new heavyweight into the
# game runtime — and to be lowered, one step per piece of A3b that lands.
#
# Per flavour, because they are different artefacts and a single number would
# either let the app runtimes grow unwatched or fail the game runtime on day one.
# A flavour whose limits are None is REPORTED and skipped: that is the honest
# state for one nobody has built and measured yet, and it is what the first
# version of the A3 table got wrong by writing down a number it had not weighed.
LIMITS = {
    # (total incl. Python, total without Python)
    "game":         (90.0, 32.0),
    # Both measured 04.09.2026, Release, macOS/arm64, on the tree
    # scripts/build_runtimes.py produces, plus roughly 10% headroom:
    #   app-advanced  76.9 MB total, 22.2 MB without python  (rendering 1.1 MB)
    #   app-basic     76.2 MB total, 21.5 MB without python  (rendering 0.4 MB)
    # The two differ by 0.7 MB and that IS the whole difference: one Metal
    # backend against one software rasterizer. Everything else they carry is the
    # same, which is why their limits are nearly the same number and why a jump
    # in either is worth looking at.
    "app-advanced": (85.0, 26.0),
    "app-basic":    (84.0, 25.0),
}
DEFAULT_FLAVOR = "game"

# Deploy directory name → flavour, so a caller that only knows the path (ctest,
# CI) does not have to repeat itself. Same spelling as HE_RUNTIME_DIR_NAME in the
# root CMakeLists.txt and RuntimeFlavor in Hpak/ProjectExporter.h.
DIR_TO_FLAVOR = {
    "Game":        "game",
    "AppAdvanced": "app-advanced",
    "AppBasic":    "app-basic",
}

# Name patterns → bucket. First match wins, so order matters.
BUCKETS = [
    ("python",    lambda n, rel: ("python" in n.lower() and "horizon" not in n.lower())
                                 or n.endswith(".zip") or n.endswith("._pth")
                                 or rel.startswith("lib-dynload")
                                 or "HorizonPython" in n),
    ("crypto",    lambda n, rel: "crypto" in n.lower() or "ssl" in n.lower()
                                 or "mbedtls" in n.lower()),
    ("net",       lambda n, rel: "HorizonNet" in n),
    ("scene",     lambda n, rel: "HorizonScene" in n),
    ("rendering", lambda n, rel: "HorizonRendering" in n),
    ("core",      lambda n, rel: "HorizonCore" in n),
    ("sdl",       lambda n, rel: n.startswith("libSDL") or n.startswith("SDL")),
    ("compress",  lambda n, rel: "zstd" in n or "lz4" in n),
    ("exe",       lambda n, rel: n in ("HorizonGame", "HorizonGame.exe")),
]


def classify(name, rel):
    for bucket, matches in BUCKETS:
        if matches(name, rel):
            return bucket
    return "unaccounted"


def measure(root):
    sizes = {}
    files = {}
    for dirpath, _dirs, filenames in os.walk(root):
        for fn in filenames:
            path = os.path.join(dirpath, fn)
            if os.path.islink(path):
                continue
            rel = os.path.relpath(path, root)
            size = os.path.getsize(path)
            bucket = classify(fn, rel)
            sizes[bucket] = sizes.get(bucket, 0) + size
            files.setdefault(bucket, []).append((rel, size))
    return sizes, files


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("-")]
    check = "--check" in argv[1:]
    build_type = ""
    flavor = ""
    for a in argv[1:]:
        if a.startswith("--build-type="):
            build_type = a.split("=", 1)[1]
        elif a.startswith("--flavor="):
            flavor = a.split("=", 1)[1]
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = args[0] if args else os.path.join(repo, "out", "deploy", "Game")

    if not flavor:
        flavor = DIR_TO_FLAVOR.get(os.path.basename(os.path.normpath(root)),
                                   DEFAULT_FLAVOR)
    if flavor not in LIMITS:
        print(f"runtime_size: unknown flavour '{flavor}' "
              f"(expected one of {', '.join(LIMITS)})")
        return 2

    if not os.path.isdir(root):
        # Not a failure: a configure without the deploy step, a fresh checkout,
        # or an app flavour nobody built in this tree has nothing to measure and
        # must not go red for it. That last case is the common one — the app
        # runtimes come from scripts/build_runtimes.py, not from a normal build.
        print(f"runtime_size: nothing to measure at {root} (skipped)")
        return 2

    sizes, files = measure(root)
    total = sum(sizes.values())
    without_py = total - sizes.get("python", 0)

    print(f"Runtime measured at: {root}"
          + (f"  ({build_type} build)" if build_type else "")
          + f"  [flavour: {flavor}]")
    print()
    print(f"{'component':<14}{'MB':>10}")
    print("-" * 24)
    for bucket in sorted(sizes, key=lambda b: -sizes[b]):
        print(f"{bucket:<14}{sizes[bucket] / 1e6:>10.1f}")
    print("-" * 24)
    print(f"{'TOTAL':<14}{total / 1e6:>10.1f}")
    print(f"{'without python':<14}{without_py / 1e6:>10.1f}")

    if "unaccounted" in sizes:
        print()
        print("Unaccounted for — add a pattern above, or explain why it ships:")
        for rel, size in sorted(files["unaccounted"], key=lambda p: -p[1]):
            print(f"  {size / 1e6:8.2f} MB  {rel}")

    if not check:
        return 0

    # The thresholds are a RELEASE claim. Judging a Debug tree by them would be
    # a red test that says nothing about what ships — the numbers above are still
    # printed, because knowing what a debug build weighs is occasionally useful.
    # An EMPTY build type is the same case, not a licence to judge: a single-config
    # build directory with no CMAKE_BUILD_TYPE set hands this an empty string, and
    # such a tree is a debug tree in every way that matters here. Judging it fails
    # by three times the threshold and says nothing about what ships.
    if build_type.lower() not in ("release", "relwithdebinfo", "minsizerel"):
        print()
        print(f"runtime_size: {build_type or 'unspecified'} build — the thresholds are "
              "measured on a Release tree, so this run only reports (skipped)")
        return 2

    limits = LIMITS[flavor]
    if limits is None:
        # An unmeasured flavour. Inventing a limit here would be the exact
        # mistake this file exists to prevent, so it reports and stops.
        print()
        print(f"runtime_size: flavour '{flavor}' has no measured threshold yet — "
              "reported only (skipped)")
        return 2
    limit_total, limit_no_py = limits

    failed = []
    if total / 1e6 > limit_total:
        failed.append(f"total {total / 1e6:.1f} MB > {limit_total:.1f} MB")
    if without_py / 1e6 > limit_no_py:
        failed.append(f"without python {without_py / 1e6:.1f} MB "
                      f"> {limit_no_py:.1f} MB")
    if failed:
        print()
        print(f"FAIL: the shipped '{flavor}' runtime grew past its threshold")
        for f in failed:
            print(f"  {f}")
        print(f"  measured at {root}")
        print("  Either something heavy was linked into this runtime that does not"
              " belong there,")
        print("  or the cut is deliberate and scripts/runtime_size.py needs the new"
              " number.")
        return 1
    print()
    print(f"OK: within the '{flavor}' thresholds "
          f"({limit_total:.0f} MB total, {limit_no_py:.0f} MB without python)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
