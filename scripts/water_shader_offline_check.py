#!/usr/bin/env python3
"""water_shader_offline_check — the engine water material through every backend's offline compiler.

The water (EditorDeps/EngineContent/Materials/Water.hasset, Thema 152) is a graph
material: its shaders exist only after MaterialShaderLibrary cross-compiles the
graph's GLSL at runtime. he_tests (tests/test_engine_materials.cpp) already holds
them against glslang, FXC, D3D12-on-WARP and SPIRV-Cross reflection in-process.
This script is the other half: it lets he_tests DUMP every variant the renderers
build and runs the SDK tools a developer would reach for over the files:

    GL 4.1 / ES 3.0 / GL 4.3   glslangValidator (OpenGL mode, -l: both stages linked)
    Vulkan                     spirv-val --target-env vulkan1.2 (what he::shaderc targets)
    D3D11 / D3D12              fxc /T vs_5_0 | ps_5_0 /E main (what both renderers call)
    Metal                      xcrun -sdk macosx metal -c (the MSL newLibraryWithSource gets)
    MoltenVK                   spirv-cross --msl over the Vulkan SPIR-V, then metal -c

    python scripts/water_shader_offline_check.py [--he-tests PATH] [--dump-dir DIR]
                                                 [--require-msl] [--keep] [-v]

--he-tests: the he_tests executable (default: the first of out/build/*/tests/he_tests[.exe]
            and build/tests/he_tests[.exe] that exists). It is run with
            -tc="Engine water material: dump*" and HE_DUMP_WATER_SHADERS set.
--dump-dir: use files a previous run dumped instead of running he_tests.

Every tool gets a negative control first — a broken input it must REJECT — so a
tool that swallows errors cannot read as a pass. A missing tool is SKIPPED with
a line saying so (Windows has no metal compiler, a Mac usually no fxc);
--require-msl turns a missing metal compiler into a failure. Exit code 0 means
every stage that ran passed. Tool discovery is validate_embedded_shaders.py's
(env vars FXC / GLSLANG_VALIDATOR / METAL override; SPIRV_VAL / SPIRV_CROSS here).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from validate_embedded_shaders import REPO, find_metal, find_tool, run  # noqa: E402

VK_SDK_HINTS = [r"%VULKAN_SDK%\Bin", r"%VULKAN_SDK%\bin", r"C:\VulkanSDK", "$VULKAN_SDK/bin"]

# What the dump case writes (tests/test_engine_materials.cpp) - kept in step by hand.
GL_PAIRS = [
    ("GL 4.1 forward",   "gl410.vert", "gl410.frag"),
    ("GL 4.1 G-buffer",  "gl410.vert", "gl410_gbuf.frag"),
    ("ES 3.0 forward",   "es300.vert", "es300.frag"),
    ("GL 4.3 clustered", "gl430.vert", "gl430_clustered.frag"),
]
SPV = ["vk_vert.spv", "vk_frag.spv", "vk_frag_clustered.spv"]
HLSL = [("vs_5_0", "hlsl_vert.hlsl"), ("ps_5_0", "hlsl_frag.hlsl"), ("ps_5_0", "hlsl_frag_clustered.hlsl")]
MSL = ["metal_vert.metal", "metal_frag.metal", "metal_frag_clustered.metal", "metal_gbuf.metal"]


class Report:
    def __init__(self, verbose: bool):
        self.verbose = verbose
        self.passed: list[str] = []
        self.failed: list[str] = []
        self.skipped: list[str] = []

    def result(self, what: str, ok: bool, out: str) -> None:
        if ok:
            self.passed.append(what)
            print(f"  ok   {what}")
            if self.verbose and out.strip():
                print("       " + "\n       ".join(out.strip().splitlines()[-3:]))
        else:
            self.failed.append(what)
            lines = [l for l in out.splitlines() if "error" in l.lower()] or out.splitlines()
            print(f"  FAIL {what}\n       " + "\n       ".join(lines[:6]))

    def negative(self, tool: str, ok: bool) -> bool:
        """A broken input must fail; False -> the tool is not judging anything."""
        if ok:
            self.failed.append(f"{tool} negative control")
            print(f"  FAIL {tool}: the negative control was ACCEPTED - this tool checks nothing")
            return False
        print(f"  {tool}: negative control rejected, as it must be")
        return True

    def skip(self, what: str, why: str) -> None:
        self.skipped.append(what)
        print(f"  SKIP {what}: {why}")


def default_he_tests() -> Path | None:
    cands = sorted((REPO / "out" / "build").glob("*/tests/he_tests*")) + \
            [REPO / "build" / "tests" / "he_tests.exe", REPO / "build" / "tests" / "he_tests"]
    for c in cands:
        if c.is_file() and c.suffix in ("", ".exe"):
            return c
    return None


def dump(he_tests: Path, out: Path) -> bool:
    env = dict(os.environ, HE_DUMP_WATER_SHADERS=str(out))
    # he_tests resets the editor config under %APPDATA% - keep it off the real one.
    scratch = out / "appdata"
    scratch.mkdir(parents=True, exist_ok=True)
    env["APPDATA"] = str(scratch)
    env.setdefault("HE_NET_LOOPBACK_ONLY", "1")
    r = subprocess.run([str(he_tests), "-tc=Engine water material: dump*"], env=env,
                       capture_output=True, text=True, timeout=600)
    print("\n".join((r.stdout + r.stderr).strip().splitlines()[-4:]))
    missing = [f for _, v, f in GL_PAIRS for f in (v, f)] + SPV + [f for _, f in HLSL] + MSL
    missing = [f for f in dict.fromkeys(missing) if not (out / f).is_file()]
    if r.returncode != 0 or missing:
        print(f"!! dump failed (exit {r.returncode}); missing: {', '.join(missing) or '-'}")
        return False
    return True


def check_gl(d: Path, rep: Report) -> None:
    gv = find_tool("GLSLANG_VALIDATOR", ["glslangValidator.exe", "glslangValidator"], VK_SDK_HINTS)
    if not gv:
        rep.skip("GL/ES (glslangValidator)", "not found (Vulkan SDK)")
        return
    bad = d / "neg.frag"
    bad.write_text("#version 410\nlayout(std140) uniform U { vec2 m; } u;\nout vec4 o;\n"
                   "void main() { o = vec4(u.m, 0.0, nonsense); }\n", encoding="utf-8")
    ok, _ = run([gv, str(bad)])
    if not rep.negative("glslangValidator", ok):
        return
    for what, vs, fs in GL_PAIRS:
        ok, out = run([gv, "-l", str(d / vs), str(d / fs)])
        rep.result(f"{what}: {vs} + {fs} (glslangValidator -l)", ok, out)


def check_spirv(d: Path, rep: Report) -> None:
    sv = find_tool("SPIRV_VAL", ["spirv-val.exe", "spirv-val"], VK_SDK_HINTS)
    if not sv:
        rep.skip("Vulkan (spirv-val)", "not found (Vulkan SDK)")
        return
    # Negative control: a valid module under a target it does not satisfy.
    # he::shaderc emits SPIR-V 1.5, which Vulkan 1.0 does not accept.
    ok, _ = run([sv, "--target-env", "vulkan1.0", str(d / "vk_frag.spv")])
    if not rep.negative("spirv-val", ok):
        return
    for f in SPV:
        ok, out = run([sv, "--target-env", "vulkan1.2", str(d / f)])
        rep.result(f"Vulkan: {f} (spirv-val vulkan1.2)", ok, out)


def check_hlsl(d: Path, rep: Report) -> None:
    fxc = find_tool("FXC", ["fxc.exe", "fxc"],
                    [r"%ProgramFiles(x86)%\Windows Kits\10\bin", r"%ProgramFiles%\Windows Kits\10\bin"])
    if not fxc:
        rep.skip("D3D11/D3D12 (fxc)", "not found (Windows SDK)")
        return
    obj = d / "out.cso"
    bad = d / "neg.hlsl"
    bad.write_text("float4 main() : SV_Target { return nonsense; }\n", encoding="utf-8")
    ok, _ = run([fxc, "/nologo", "/T", "ps_5_0", "/E", "main", "/Fo", str(obj), str(bad)])
    if not rep.negative("fxc", ok):
        return
    for profile, f in HLSL:
        ok, out = run([fxc, "/nologo", "/T", profile, "/E", "main", "/Fo", str(obj), str(d / f)])
        rep.result(f"D3D11/D3D12: {f} (fxc {profile})", ok, out)


def check_msl(d: Path, rep: Report, require: bool) -> None:
    # MoltenVK, first half: the Vulkan SPIR-V translated the way MoltenVK's own
    # SPIRV-Cross does it (no engine pins). Needs no Xcode, so it runs anywhere
    # the Vulkan SDK is; the second half (metal -c over the result) below.
    sc = find_tool("SPIRV_CROSS", ["spirv-cross.exe", "spirv-cross"], VK_SDK_HINTS)
    mvk: list[Path] = []
    if not sc:
        rep.skip("MoltenVK translation (spirv-cross --msl)", "spirv-cross not found (Vulkan SDK)")
    else:
        bad = d / "neg_garbage.spv"
        bad.write_bytes(b"\x03\x02\x23\x07" + b"\0" * 16)  # SPIR-V magic, nonsense header
        ok, _ = run([sc, "--msl", str(bad), "--output", str(d / "neg_garbage.metal")])
        if rep.negative("spirv-cross --msl", ok):
            for f in SPV:
                msl = d / (f + ".mvk.metal")
                ok, out = run([sc, "--msl", "--msl-version", "20100", str(d / f), "--output", str(msl)])
                rep.result(f"MoltenVK: {f} -> MSL 2.1 (spirv-cross --msl)", ok, out)
                if ok:
                    mvk.append(msl)

    metal, info = find_metal()
    if not metal:
        if require:
            rep.failed.append("Metal (xcrun metal)")
            print(f"  FAIL Metal: required, but no metal compiler ({info})")
        else:
            rep.skip("Metal + MoltenVK compile (xcrun metal)", info)
        return
    print(f"  {' '.join(metal)}: {info}")
    bad = d / "neg.metal"
    bad.write_text("#include <metal_stdlib>\nkernel void k() { this_is_not_msl; }\n", encoding="utf-8")
    ok, _ = run(metal + ["-c", str(bad), "-o", str(d / "neg.air")])
    if not rep.negative("metal", ok):
        return
    for f in MSL:
        ok, out = run(metal + ["-c", str(d / f), "-o", str(d / (f + ".air"))])
        rep.result(f"Metal: {f} (metal -c)", ok, out)
    for msl in mvk:
        ok, out = run(metal + ["-c", str(msl), "-o", str(msl.with_suffix(".air"))])
        rep.result(f"MoltenVK: {msl.name} (metal -c)", ok, out)


def main() -> int:
    # Tool output can carry non-ASCII; a cp1252 console must not end the run.
    sys.stdout.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--he-tests", type=Path)
    ap.add_argument("--dump-dir", type=Path)
    ap.add_argument("--require-msl", action="store_true")
    ap.add_argument("--keep", action="store_true", help="keep the temporary dump directory")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    tmp = None
    if a.dump_dir:
        d = a.dump_dir
    else:
        he = a.he_tests or default_he_tests()
        if not he or not he.is_file():
            print("!! he_tests not found - pass --he-tests or --dump-dir")
            return 2
        tmp = tempfile.mkdtemp(prefix="he_water_")
        d = Path(tmp)
        print(f"dumping with {he} -> {d}")
        if not dump(he, d):
            return 2

    rep = Report(a.verbose)
    print("GL / ES:");        check_gl(d, rep)
    print("Vulkan:");         check_spirv(d, rep)
    print("D3D11 / D3D12:");  check_hlsl(d, rep)
    print("Metal / MoltenVK:"); check_msl(d, rep, a.require_msl)

    print(f"\n{len(rep.passed)} passed, {len(rep.failed)} failed, {len(rep.skipped)} skipped"
          + (f" ({'; '.join(rep.skipped)})" if rep.skipped else ""))
    if tmp and (a.keep or rep.failed):
        print(f"dump kept in {d}")
    elif tmp:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
