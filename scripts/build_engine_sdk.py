#!/usr/bin/env python3
"""build_engine_sdk — put a second lib set into the editor's engine SDK.

cmake/HorizonSDK.cmake: one EngineSDK/ holds a lib set per build profile
(lib/<profile>), and a tree installs only the set of its own HE_BUILD_PROFILE.
The editor tree is dev, and building the editor installs lib/dev beside it
(topic 222 step 2). Shipping exports compile against lib/release (step 3), which
only a HE_BUILD_PROFILE=release tree can build: the HE_FEATURE_* sites inside
the engine libraries are #if'd. This script is that second tree.

    scripts/build_engine_sdk.py --editor-tree build [--profile release]
                                [--build-dir DIR] [--jobs N]
                                [--define NAME=VALUE ...] [--configure-only]
    scripts/build_engine_sdk.py --verify <EngineSDK> [--require dev --require release]

The release tree is the editor tree with another profile and nothing else, read
from the editor tree's CMakeCache.txt:

  * the SDK folder: its HE_ENGINE_SDK_DIR. The release tree gets a DEPLOY_DIR
    of its own (inside its build dir): the editor's deploy holds the dev Game
    runtime, and nothing of a release tree may land there but its lib set.
  * the options that change what the engine libraries are or link
    (MIRRORED below), so lib/release on macOS does not link Homebrew's
    libcrypto where lib/dev links static mbedTLS.
  * the FetchContent SOURCES of every dependency (-DFETCHCONTENT_SOURCE_DIR_*:
    the checkouts in its _deps, or the ones it borrowed itself).
    Not only to save the clones: include/ is shared by both sets and its
    header stamp covers glm, nlohmann and SDL3, copied from these checkouts.
    A release tree that fetched another revision would rewrite include/ and
    turn the dev set into "not found".

HE_BUILD_TESTS=OFF and only --target HorizonSDK: a release tree compiles the
runtime, not the editor or he_tests (both use dev-only code by design).

After the install every lib set in the SDK must carry the stamp include/ has
now (HorizonEngineHeaders-<profile>.txt == HorizonEngineHeaders.txt), or the
package refuses it. --verify checks exactly that, for CI after packaging.

WHO CALLS THIS: .github/workflows/ci.yml on a push (and on a dispatch with
release_sdk), after the tests and before the SDK goes into the editor package.
A local editor build does not: a second tree costs a second engine build, and
whoever wants Shipping exports from their own out/deploy runs this once:

    scripts/build_engine_sdk.py --editor-tree out/build/release --jobs 8

An editor build afterwards reinstalls lib/dev only (HorizonSDK.cmake replaces
lib/<profile>, not lib/), so lib/release stays until the headers change, and
then the package refuses it until this runs again.

Exit code 0 when the set was installed and every set in the SDK matches.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from build_runtimes import find_generator  # noqa: E402

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

# Cache entries of the editor tree the second tree takes over verbatim. What is
# not here is the release tree's own default (HE_BUILD_TESTS, DEPLOY_DIR) or has
# no say in the engine libraries (HE_BUNDLE_CMAKE, HE_ENABLE_ASSIMP: editor and
# importer only).
MIRRORED = [
    "CMAKE_BUILD_TYPE",
    "CMAKE_OSX_ARCHITECTURES",
    "CMAKE_OSX_DEPLOYMENT_TARGET",
    "HE_PORTABLE_BUILD",
    "HE_PREFER_MBEDTLS",
    "HE_PROFILING",
    "HE_ENABLE_SHADERC",
    "HE_ENABLE_ASTC",
    "HE_ENABLE_STB_DXT",
    "HE_ENABLE_TRACY",
    "HE_RUNTIME_FLAVOR",
]

PKG = os.path.join("lib", "cmake", "HorizonEngine")


def read_cache(tree):
    path = os.path.join(tree, "CMakeCache.txt")
    if not os.path.isfile(path):
        sys.exit(f"build_engine_sdk: {path} not found — configure and build the editor tree first")
    cache = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line[0] in "#/" or "=" not in line or ":" not in line.split("=", 1)[0]:
                continue
            key_type, value = line.split("=", 1)
            key, typ = key_type.split(":", 1)
            cache[key] = (typ, value)
    return cache


def shared_sources(tree, cache):
    """-DFETCHCONTENT_SOURCE_DIR_<NAME> for every checkout the editor tree compiled.

    A tree borrows its sources itself sometimes (a worktree from the main
    checkout's build): then they are in its cache, not in its _deps.
    """
    dirs = {}
    deps = os.path.join(tree, "_deps")
    if os.path.isdir(deps):
        for entry in sorted(os.listdir(deps)):
            src = os.path.join(deps, entry)
            if entry.endswith("-src") and os.path.isdir(src):
                dirs[entry[:-4].upper()] = src
    for key, (_, value) in cache.items():
        if key.startswith("FETCHCONTENT_SOURCE_DIR_") and value and os.path.isdir(value):
            dirs[key[len("FETCHCONTENT_SOURCE_DIR_"):]] = value
    return [f"-DFETCHCONTENT_SOURCE_DIR_{name}={src}" for name, src in sorted(dirs.items())]


def read_line(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.readline().strip()
    except OSError:
        return ""


def verify(sdk, required):
    """Every lib set in the SDK matches include/; every required one is there."""
    pkg = os.path.join(sdk, PKG)
    now = read_line(os.path.join(pkg, "HorizonEngineHeaders.txt"))
    if not now:
        print(f"build_engine_sdk: {pkg}/HorizonEngineHeaders.txt missing or empty", file=sys.stderr)
        return False
    present = sorted(f[len("HorizonEngineTargets-"):-len(".cmake")]
                     for f in os.listdir(pkg)
                     if f.startswith("HorizonEngineTargets-") and f.endswith(".cmake"))
    ok = True
    print(f"engine SDK {sdk}\n  include/ stamp {now}")
    for profile in present:
        stamp = read_line(os.path.join(pkg, f"HorizonEngineHeaders-{profile}.txt"))
        libdir = os.path.join(sdk, "lib", profile)
        libs = sorted(os.listdir(libdir)) if os.path.isdir(libdir) else []
        state = "ok" if stamp == now and libs else "MISMATCH" if libs else "NO LIBRARIES"
        print(f"  {profile:8} {state:13} stamp {stamp or '-'}  lib/{profile}: {len(libs)} files")
        if state != "ok":
            ok = False
    for profile in required:
        if profile not in present:
            print(f"build_engine_sdk: no '{profile}' lib set in {sdk}", file=sys.stderr)
            ok = False
    return ok


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--editor-tree", help="the configured and built editor tree whose SDK gets the set")
    p.add_argument("--profile", default="release", choices=["dev", "release"])
    p.add_argument("--build-dir", help="default: <repo>/out/build/sdk-<profile>")
    p.add_argument("--jobs", type=int, default=0)
    p.add_argument("--define", action="append", default=[], help="extra -D for the second tree")
    p.add_argument("--configure-only", action="store_true")
    p.add_argument("--verify", metavar="SDK", help="only check an installed EngineSDK folder")
    p.add_argument("--require", action="append", default=[], choices=["dev", "release"],
                   help="with --verify: a lib set that must be present")
    args = p.parse_args(argv[1:])

    if args.verify:
        return 0 if verify(args.verify, args.require) else 1
    if not args.editor_tree:
        p.error("--editor-tree or --verify is required")

    editor_tree = os.path.abspath(args.editor_tree)
    cache = read_cache(editor_tree)
    editor_profile = cache.get("HE_BUILD_PROFILE", ("", "dev"))[1]
    if editor_profile == args.profile:
        sys.exit(f"build_engine_sdk: {editor_tree} already is a '{args.profile}' tree; "
                 "building it installs that set")
    sdk = cache.get("HE_ENGINE_SDK_DIR", ("", ""))[1]
    if not sdk:
        sys.exit(f"build_engine_sdk: {editor_tree} has no HE_ENGINE_SDK_DIR (an engine before topic 211?)")
    if not os.path.isdir(os.path.join(sdk, PKG)):
        sys.exit(f"build_engine_sdk: {sdk} holds no SDK yet — build the editor tree first, "
                 "it installs its own lib set there")

    build_dir = os.path.abspath(args.build_dir or os.path.join(REPO, "out", "build", f"sdk-{args.profile}"))
    print(f"=== engine SDK '{args.profile}' lib set → {sdk}\n    build dir {build_dir}\n"
          f"    editor tree {editor_tree} ('{editor_profile}')", flush=True)

    configure = ["cmake", "-S", REPO, "-B", build_dir,
                 f"-DHE_BUILD_PROFILE={args.profile}",
                 "-DHE_BUILD_TESTS=OFF",
                 f"-DDEPLOY_DIR={os.path.join(build_dir, 'deploy')}",
                 f"-DHE_ENGINE_SDK_DIR={sdk}"]
    for key in MIRRORED:
        if key in cache:
            typ, value = cache[key]
            configure.append(f"-D{key}:{typ if typ != 'UNINITIALIZED' else 'STRING'}={value}")
    configure += [f"-D{d}" for d in args.define]
    configure += find_generator() + shared_sources(editor_tree, cache)
    if subprocess.call(configure) != 0:
        print("build_engine_sdk: configure failed", file=sys.stderr)
        return 1
    if args.configure_only:
        return 0

    cmd = ["cmake", "--build", build_dir, "--target", "HorizonSDK"]
    if cache.get("CMAKE_BUILD_TYPE", ("", ""))[1]:
        cmd += ["--config", cache["CMAKE_BUILD_TYPE"][1]]
    if args.jobs:
        cmd += ["--parallel", str(args.jobs)]
    if subprocess.call(cmd) != 0:
        print("build_engine_sdk: build failed", file=sys.stderr)
        return 1

    return 0 if verify(sdk, [editor_profile, args.profile]) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
