#!/usr/bin/env python3
"""build_runtimes — build the app runtime flavours the exporter ships.

docs/he-apps-plan.md A3b: the exporter copies a runtime directory verbatim into
a packaged build, and there are THREE of them per platform.

    game          all backends of the platform, shader cross-compiler ON
    app-advanced  the platform's forward renderer alone, cross-compiler OFF
    app-basic     RendererSoftware alone,                cross-compiler OFF

By default this script builds the two APP flavours, each in its own build
directory, because the two differences are whole-tree ones: HE_ENABLE_SHADERC
decides whether glslang is fetched at all, and the backend set is baked into one
shared HorizonRendering. A second output of the game build cannot differ in
either.

The game flavour is the normal build of this repository, so on a dev box it is
already there — building the editor builds and deploys it — and asking for it
here would only build it a second time. `--flavor game` exists anyway, for the
one place where that is not true: a CI job that weighs all three runtimes of a
platform wants them built the same way, from the same recipe, in one run. Ask
for it explicitly; it is never a default.

Each flavour lands in <deploy>/AppAdvanced resp. <deploy>/AppBasic, next to the
Game directory the editor already deploys — which is where findRuntimeBundle()
looks for them.

Usage:
    scripts/build_runtimes.py [--flavor game] [--flavor app-advanced]
                              [--flavor app-basic]
                              [--build-type Release] [--jobs N]
                              [--deploy-dir DIR] [--build-root DIR]
                              [--define NAME=VALUE ...]
                              [--configure-only] [--size]

    --size runs scripts/runtime_size.py over each finished tree afterwards.
    --define passes an extra -D straight to cmake (e.g. HE_PREFER_MBEDTLS=ON).

Exit code 0 when every requested flavour built.
"""
import argparse
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Flavour → the deploy directory name the exporter knows it by. Kept in step with
# HE_RUNTIME_DIR_NAME in the root CMakeLists.txt, RuntimeFlavor in
# HE_Core/include/Hpak/ProjectExporter.h and DIR_TO_FLAVOR in runtime_size.py —
# one spelling in four places. Nothing cross-checks them automatically, so a
# rename is a rename in four files; get it wrong and the exporter looks in a
# directory nobody writes, which is why build() below insists on finding the
# deploy directory rather than trusting a successful compile.
FLAVOR_DIRS = {
    "game":         "Game",
    "app-advanced": "AppAdvanced",
    "app-basic":    "AppBasic",
}

# FetchContent packages whose SOURCES are worth reusing from an existing build
# tree: a flavour build otherwise re-downloads SDL, Jolt, Recast and friends for
# nothing. Only the sources are shared — every flavour still compiles its own
# objects, which is the point of a separate tree.
SHARED_SOURCES = ["sdl3", "glm", "joltphysics", "recastnavigation", "lua",
                  "nlohmann_json", "astcenc"]


def find_generator():
    """Ninja when there is one, Makefiles otherwise.

    CLion ships a ninja that is not on PATH (and on this project's macOS boxes
    that is usually the only one), so look there too before giving up.
    """
    ninja = shutil.which("ninja")
    if not ninja:
        for candidate in (
            "/Applications/CLion.app/Contents/bin/ninja/mac/aarch64/ninja",
            "/Applications/CLion.app/Contents/bin/ninja/mac/x64/ninja",
            os.path.expanduser("~/Applications/CLion.app/Contents/bin/ninja/mac/aarch64/ninja"),
        ):
            if os.path.isfile(candidate):
                ninja = candidate
                break
    if ninja:
        return ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"]
    return []


def existing_sources(build_root=None):
    """-DFETCHCONTENT_SOURCE_DIR_<PKG> for every dependency already checked out.

    A dev box has a build tree to borrow from; a CI runner starts with none, and
    there the first flavour of the run becomes the one the next two borrow from.
    That is why build_root is searched as well: without it, a three-flavour run
    clones SDL, Jolt and Recast three times over for nothing.
    """
    trees = [os.path.join(REPO, "cmake-build-release"),
             os.path.join(REPO, "cmake-build-debug")]
    if build_root and os.path.isdir(build_root):
        trees += [os.path.join(build_root, d)
                  for d in sorted(os.listdir(build_root))]
    args = []
    for tree in trees:
        deps = os.path.join(tree, "_deps")
        if not os.path.isdir(deps):
            continue
        for pkg in SHARED_SOURCES:
            src = os.path.join(deps, f"{pkg}-src")
            if os.path.isdir(src):
                flag = f"-DFETCHCONTENT_SOURCE_DIR_{pkg.upper()}={src}"
                if flag not in args:
                    args.append(flag)
        break
    return args


def build(flavor, args):
    build_dir = os.path.join(args.build_root, flavor)
    deploy = os.path.join(args.deploy_dir, FLAVOR_DIRS[flavor])
    print(f"\n=== {flavor} → {deploy}\n    build dir {build_dir}", flush=True)

    configure = ["cmake", "-S", REPO, "-B", build_dir,
                 f"-DCMAKE_BUILD_TYPE={args.build_type}",
                 f"-DHE_RUNTIME_FLAVOR={flavor}",
                 f"-DDEPLOY_DIR={args.deploy_dir}",
                 # A runtime build has no business building the test binaries;
                 # they link the editor-side tools this flavour deliberately drops.
                 "-DHE_BUILD_TESTS=OFF"]
    configure += [f"-D{d}" for d in args.define]
    configure += find_generator() + existing_sources(args.build_root)
    if subprocess.call(configure) != 0:
        print(f"build_runtimes: configure failed for {flavor}", file=sys.stderr)
        return False
    if args.configure_only:
        return True

    cmd = ["cmake", "--build", build_dir, "--target", "HorizonGame",
           "--config", args.build_type]
    if args.jobs:
        cmd += ["--parallel", str(args.jobs)]
    if subprocess.call(cmd) != 0:
        print(f"build_runtimes: build failed for {flavor}", file=sys.stderr)
        return False

    if not os.path.isdir(deploy):
        # The POST_BUILD deploy is what puts the runtime where the exporter looks.
        # Its absence means the build produced a binary nobody can ship, which is
        # a failure even though every compile succeeded.
        print(f"build_runtimes: {flavor} built but nothing was deployed to {deploy}",
              file=sys.stderr)
        return False
    return True


def main(argv):
    p = argparse.ArgumentParser()
    p.add_argument("--flavor", action="append", default=[],
                   choices=["game", "app-advanced", "app-basic"],
                   help="repeatable; default is both app flavours. 'game' is "
                        "the ordinary build of this repository and has to be "
                        "asked for by name")
    p.add_argument("--build-type", default="Release")
    p.add_argument("--jobs", type=int, default=0)
    p.add_argument("--deploy-dir", default=os.path.join(REPO, "out", "deploy"))
    p.add_argument("--build-root", default=os.path.join(REPO, "out", "runtime-builds"))
    p.add_argument("--define", action="append", default=[])
    p.add_argument("--configure-only", action="store_true")
    p.add_argument("--size", action="store_true",
                   help="report scripts/runtime_size.py for each finished flavour")
    args = p.parse_args(argv[1:])
    flavors = args.flavor or ["app-advanced", "app-basic"]

    ok = True
    for flavor in flavors:
        if not build(flavor, args):
            ok = False
            break

    if ok and args.size:
        for flavor in flavors:
            deploy = os.path.join(args.deploy_dir, FLAVOR_DIRS[flavor])
            print(flush=True)
            subprocess.call([sys.executable, os.path.join(REPO, "scripts", "runtime_size.py"),
                             deploy, f"--flavor={flavor}",
                             f"--build-type={args.build_type}"])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
