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

And there are three such directories PER PLATFORM, which is the second half of
the same thought: a Windows runtime carries the MSVC redistributable and a .exe,
a Linux one carries .so files and whatever bundle_native_deps.sh staged next to
them, a macOS one carries dylibs. Those trees weigh different amounts for
reasons that have nothing to do with a regression, so the thresholds are keyed
by platform as well as by flavour and a platform nobody has measured is reported
and skipped rather than judged by another platform's number.

Usage:
    scripts/runtime_size.py [RUNTIME_DIR] [--check] [--build-type=TYPE]
                            [--flavor=game|app-advanced|app-basic]
                            [--platform=darwin|win32|linux]

    --check exits non-zero when a threshold is exceeded. Without it the script
    only reports, which is what one wants while cutting.

    --build-type is what the tree was built as. The thresholds below were
    measured on a RELEASE tree and mean nothing anywhere else — a Debug build of
    the same code is several times the size, and failing on that would be the
    check crying wolf at the one thing it is not measuring. Anything that is not
    a release configuration therefore REPORTS and skips.

    --platform says which platform's thresholds apply. Without it the platform
    this script is RUNNING on decides, which is right for every build that
    measures its own output; the flag exists for weighing a tree that was
    downloaded from another platform's CI run.

Exit codes: 0 fine (or reporting), 1 over threshold, 2 nothing to measure or
nothing this may judge.
"""
import os
import sys

# A Windows console hands Python a cp1252 stdout, and every em dash in the report
# below then raises UnicodeEncodeError instead of printing. The report is the
# whole point of this script, so the console gives way, not the text.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):  # not a text stream, or already redirected
        pass

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
#
# Keyed by PLATFORM first, because the three trees a platform produces are only
# comparable to each other. Windows ships the MSVC redistributable and a .exe
# where macOS ships nothing of the sort; Linux stages its third-party .so next
# to the binaries where macOS rewrites install names. Judging one platform's
# tree by another's number would fail on the difference between platforms, which
# is the one thing this check must never report as a regression.
#
# A platform or flavour whose limits are None is REPORTED and skipped: that is
# the honest state for one nobody has built and measured yet, and it is what the
# first version of the A3 table got wrong by writing down a number it had not
# weighed.
LIMITS = {
    # ── macOS/arm64 ─────────────────────────────────────────────────────────
    "darwin": {
        # (total incl. Python, total without Python)
        "game":         (90.0, 32.0),
        # Both measured 04.09.2026, Release, macOS/arm64, on the tree
        # scripts/build_runtimes.py produces, plus roughly 10% headroom:
        #   app-advanced  76.9 MB total, 22.2 MB without python  (rendering 1.1 MB)
        #   app-basic     76.2 MB total, 21.5 MB without python  (rendering 0.4 MB)
        # The two differ by 0.7 MB and that IS the whole difference: one Metal
        # backend against one software rasterizer. Everything else they carry is
        # the same, which is why their limits are nearly the same number and why
        # a jump in either is worth looking at.
        #
        # Weighed a second time 05.09.2026, on macos-latest with the recipe
        # ci.yml ships with (run 33966761978), and it came out much lighter:
        #   game          54.8 MB total, 23.7 MB without python
        #   app-advanced  48.8 MB total, 17.7 MB without python
        #   app-basic     48.0 MB total, 17.0 MB without python
        # Two measuring points, 28 MB apart, and neither is wrong: the runner
        # has a smaller CPython and static mbedTLS where the local tree has
        # Homebrew's OpenSSL. The limits are deliberately left at the LOCAL
        # numbers so that both trees pass — see the note under the tables.
        "app-advanced": (85.0, 26.0),
        "app-basic":    (84.0, 25.0),
    },
    # ── Windows/x64 ─────────────────────────────────────────────────────────
    # Measured 05.09.2026 on windows-latest, Release, all three flavours built
    # by scripts/build_runtimes.py in .github/workflows/runtime-flavors.yml
    # (run 33966761978) — the first time these trees had ever been built on
    # Windows at all:
    #   game          35.9 MB total, 26.2 MB without python  (rendering 4.8 MB)
    #   app-advanced  32.0 MB total, 22.3 MB without python  (rendering 0.9 MB)
    #   app-basic     31.4 MB total, 21.7 MB without python  (rendering 0.3 MB)
    # Plus the 1.6 MB MSVC redistributable in each, which macOS and Linux do not
    # carry. Windows is the smallest of the three platforms here, and almost all
    # of the difference is its python: 9.8 MB against Linux's 22.7 MB.
    "win32": {
        "game":         (45.0, 34.0),
        "app-advanced": (41.0, 30.0),
        "app-basic":    (40.0, 29.0),
    },
    # ── Linux/x64 ───────────────────────────────────────────────────────────
    # Measured 05.09.2026 on ubuntu-latest, same run, same recipe:
    #   game          52.7 MB total, 29.9 MB without python  (rendering 8.3 MB)
    #   app-advanced  45.8 MB total, 23.1 MB without python  (rendering 1.4 MB)
    #   app-basic     45.0 MB total, 22.2 MB without python  (rendering 0.6 MB)
    # The rendering figures are the point of the whole exercise: the app-advanced
    # tree carries one OpenGL backend where the game tree carries all of them,
    # and app-basic carries the software rasterizer alone.
    "linux": {
        "game":         (64.0, 38.0),
        "app-advanced": (56.0, 31.0),
        "app-basic":    (55.0, 30.0),
    },
}
# ── On the headroom in the two tables above ─────────────────────────────────
# Roughly 10 percent, PLUS about 5 MB, and the 5 MB has a name.
#
# Every number above was measured on a CI runner with the recipe ci.yml ships
# with: HE_PORTABLE_BUILD=ON and HE_PREFER_MBEDTLS=ON. A developer's own tree
# usually has neither, and the mbedTLS one alone moves the total: statically
# linked mbedcrypto costs HorizonCore 0.3 MB where a system libcrypto is a
# 4.8 MB load-time edge that ships beside the binaries (docs/he-apps-plan.md
# A3a). The same macOS flavour weighed 76.9 MB on a local tree and 48.8 MB on
# the runner for exactly that kind of reason.
#
# So the headroom is not slack, it is the width of the recipe. A threshold tight
# enough to fail on a developer's own Release build would be a threshold that
# fails on the difference between two honest trees, which is the one thing this
# check must never report as a regression. It is still far too tight to hide the
# failures it exists for: glslang coming back into an app runtime, or Jolt and
# Recast being dragged in behind HorizonScene, are tens of megabytes each.
FLAVORS = ("game", "app-advanced", "app-basic")
DEFAULT_FLAVOR = "game"


def current_platform():
    """sys.platform, folded onto the three keys of LIMITS.

    Anything else (a BSD, a platform nobody has built this on) is returned
    unchanged and therefore misses the table, which lands it in the
    reported-and-skipped path rather than borrowing a threshold that was never
    measured there.
    """
    if sys.platform.startswith("win"):
        return "win32"
    if sys.platform.startswith("linux"):
        return "linux"
    return sys.platform

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
    # Windows only (he_bundle_crt in the root CMakeLists.txt): the MSVC
    # redistributable, bundled so a clean PC needs no redist install. Its own
    # bucket rather than "unaccounted", because it ships on purpose and only
    # there — a platform where this bucket is missing is not a platform that
    # lost something.
    ("crt",       lambda n, rel: n.lower().startswith(("vcruntime", "msvcp",
                                                       "concrt", "vccorlib"))),
    # Runtime-loaded shader binaries (Vulkan .spv, D3D12 DXR .cso) deployed as a
    # directory next to the exe. Matched by their PLACE, not their extension:
    # the set of files in there is decided by which backends the flavour built,
    # which is exactly what these numbers are meant to show moving.
    ("shaders",   lambda n, rel: rel.replace("\\", "/").startswith("Shaders/")),
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
    platform = ""
    for a in argv[1:]:
        if a.startswith("--build-type="):
            build_type = a.split("=", 1)[1]
        elif a.startswith("--flavor="):
            flavor = a.split("=", 1)[1]
        elif a.startswith("--platform="):
            platform = a.split("=", 1)[1]
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = args[0] if args else os.path.join(repo, "out", "deploy", "Game")

    if not flavor:
        flavor = DIR_TO_FLAVOR.get(os.path.basename(os.path.normpath(root)),
                                   DEFAULT_FLAVOR)
    if flavor not in FLAVORS:
        print(f"runtime_size: unknown flavour '{flavor}' "
              f"(expected one of {', '.join(FLAVORS)})")
        return 2
    if not platform:
        platform = current_platform()

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
          + f"  [flavour: {flavor}, platform: {platform}]")
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

    if platform not in LIMITS:
        # A platform nobody has weighed this on. Borrowing another one's number
        # would fail on the difference between platforms, not on a regression.
        print()
        print(f"runtime_size: platform '{platform}' has no measured thresholds — "
              "reported only (skipped)")
        return 2
    limits = LIMITS[platform][flavor]
    if limits is None:
        # An unmeasured flavour. Inventing a limit here would be the exact
        # mistake this file exists to prevent, so it reports and stops.
        print()
        print(f"runtime_size: flavour '{flavor}' has no measured threshold on "
              f"{platform} yet — reported only (skipped)")
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
        print(f"FAIL: the shipped '{flavor}' runtime grew past its "
              f"{platform} threshold")
        for f in failed:
            print(f"  {f}")
        print(f"  measured at {root}")
        print("  Either something heavy was linked into this runtime that does not"
              " belong there,")
        print("  or the cut is deliberate and scripts/runtime_size.py needs the new"
              " number.")
        return 1
    print()
    print(f"OK: within the '{flavor}' thresholds on {platform} "
          f"({limit_total:.0f} MB total, {limit_no_py:.0f} MB without python)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
