#!/usr/bin/env python3
"""build_profile_strings — is the dev code really gone from a release build?

The build profile (cmake/HorizonBuildProfile.cmake, Platform/BuildProfile.h)
removes developer features with the preprocessor, so the proof is in the
binary: a release HorizonGame and its engine libraries must not carry the
environment names, log lines and symbols of those features, and a dev build
must still carry every one of them (otherwise the list below has gone stale and
the release check proves nothing).

    scripts/build_profile_strings.py --profile release <dir-or-file>...
    scripts/build_profile_strings.py --profile dev     <dir-or-file>...

A directory stands for the HorizonGame executable plus the HorizonCore,
HorizonScene, HorizonNet and HorizonEngine libraries found under it (a build tree or a
deploy folder). Exit 1 if a release binary still has something, or a dev
binary set lacks something.

Strings are matched on the raw bytes (environment names must end in a NUL,
so "HE_LOG" does not match inside "HE_LOG_NO_CONSOLE"); symbols through
`nm -C`. The renderer backends' own HE_* switches are not listed: the
renderer is outside the profile's scope so far.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

# feature switch -> what it removes. "env": environment variable names,
# matched as whole C strings. "text": log/UI text, matched as a substring.
# "symbols": demangled symbol names, matched as a substring of `nm -C`.
FEATURES: dict[str, dict[str, list[str]]] = {
    "HE_FEATURE_DEV_HOTKEYS": {
        "env": [],
        "text": ["development hotkeys ON", "stats card ON", "mem://DevStatsTile.hasset"],
        "symbols": ["HE::DevStatsTile::toggle"],
    },
    "HE_FEATURE_PROFILER_CAPTURE": {
        "env": ["HE_PROFILE_CAPTURE", "HE_PROFILE_WARMUP", "HE_PROFILE_NOTE",
                "HE_CAPTURE_FRAME", "HE_CAPTURE_PATH", "HE_PERF_INPUT_EVENTS",
                "HE_PERF_INPUT_LATENCY_HZ"],
        "text": ["F9 benchmark capture", "Profiler: stop requested (F9)"],
        "symbols": ["HE::Application::toggleProfilerCapture", "InputLatencyProbe"],
    },
    "HE_FEATURE_TEST_HOOKS": {
        "env": ["HE_HIDDEN_WINDOW", "HE_EXIT_AFTER_FRAMES", "HE_DUMP_PATH",
                "HE_BACKGROUND_FPS", "HE_NO_SPLASH", "HE_SPLASH_DUMP",
                "HE_CONFIG_FALLBACK_DIR"],
        "text": ["HE_EXIT_AFTER_FRAMES=%llu reached", "Hidden mode on ("],
        "symbols": [],
    },
    "HE_FEATURE_ENV_OVERRIDES": {
        "env": ["HE_CONFIG_DIR", "HE_LOG", "HE_LOG_NO_CONSOLE", "HE_ENGINE_CONTENT_EDITABLE",
                "HE_PHYSICS_THREADS", "HE_PHYSICS_WATER", "HE_PHYSICS_BUDGET_MS",
                "HE_PROPAGATE_FULL", "HE_PROPAGATE_VERIFY"],
        "text": [],
        "symbols": [],
    },
}

GAME_NAMES = ("HorizonGame", "HorizonGame.exe")
# HorizonEngine: the four as one library, what an exported game loads (Thema 211 step 10).
LIB_STEMS = ("HorizonCore", "HorizonScene", "HorizonNet", "HorizonEngine")


def binaries_in(path: Path) -> list[Path]:
    if path.is_file():
        return [path]
    found: list[Path] = []
    for p in path.rglob("*"):
        if not p.is_file() or p.is_symlink():
            continue
        name = p.name
        if name in GAME_NAMES:
            found.append(p)
        elif any(name.startswith(pre + s) for s in LIB_STEMS for pre in ("lib", "")) and \
                p.suffix in (".dylib", ".so", ".dll"):
            found.append(p)
    return sorted(found)


def symbols_of(path: Path) -> str:
    nm = shutil.which("nm")
    if not nm:
        return ""
    r = subprocess.run([nm, "-C", str(path)], capture_output=True, text=True, errors="replace")
    return r.stdout


def scan(files: list[Path]) -> dict[str, set[str]]:
    """item -> names of the files that contain it."""
    hits: dict[str, set[str]] = {}
    for f in files:
        data = f.read_bytes()
        syms = symbols_of(f)
        for feat in FEATURES.values():
            for env in feat["env"]:
                if env.encode() + b"\0" in data:
                    hits.setdefault(env, set()).add(f.name)
            for text in feat["text"]:
                if text.encode() in data:
                    hits.setdefault(text, set()).add(f.name)
            for sym in feat["symbols"]:
                if sym in syms:
                    hits.setdefault(sym, set()).add(f.name)
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--profile", choices=("dev", "release"), required=True)
    ap.add_argument("paths", nargs="+", type=Path)
    args = ap.parse_args()

    files = [b for p in args.paths for b in binaries_in(p)]
    if not files:
        print("no HorizonGame / HorizonCore / HorizonScene / HorizonNet / HorizonEngine binary found", file=sys.stderr)
        return 2
    print("binaries:", ", ".join(str(f) for f in files))
    if not shutil.which("nm"):
        print("note: no nm on PATH, symbols not checked")

    hits = scan(files)
    bad = 0
    for name, feat in FEATURES.items():
        items = feat["env"] + feat["text"] + feat["symbols"]
        if args.profile == "release":
            left = [i for i in items if i in hits]
            print(f"{name}: {len(items) - len(left)}/{len(items)} gone")
            for i in left:
                print(f"    STILL IN {', '.join(sorted(hits[i]))}: {i}")
        else:
            missing = [i for i in items if i not in hits]
            print(f"{name}: {len(items) - len(missing)}/{len(items)} present")
            for i in missing:
                print(f"    MISSING: {i}")
            left = missing
        bad += len(left)
    print("OK" if bad == 0 else f"FAILED: {bad} item(s)")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
