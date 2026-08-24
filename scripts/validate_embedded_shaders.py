#!/usr/bin/env python3
"""validate_embedded_shaders — compile-check the shaders that the C++ build never sees.

Most D3D/GL shaders in this engine are string literals compiled at RUNTIME
(D3DCompile / glCompileShader). A green `cmake --build` therefore proves nothing
about them, and both backends respond to a compile failure by logging once and
disabling the feature for the whole session — so a broken shader looks like
"the toggle does nothing", not like an error.

That is not hypothetical: `kGiShadowCSHLSL` shipped with an `[unroll]` loop that
FXC rejects (X3511 + X3500), which silently killed the ENTIRE GI stack on both
D3D backends — including D3D12's DXR path, which sits behind the software
pipeline's success check. See docs/backend-parity-plan.md §1.1.

This script extracts those strings, reassembles them exactly as the call sites do,
and runs the real compilers over them.

    python scripts/validate_embedded_shaders.py [--hlsl-only|--glsl-only] [-v]

Exit code 0 = every shader compiled. Non-zero = at least one failed, or a shader
string exists that no job covers (see COVERAGE below).

COVERAGE: every extracted string must be reachable by at least one compile job.
When someone adds a new shader string, this script FAILS with "not covered by any
job" until the table below is extended. That is deliberate — a validator that
silently ignores new shaders is worse than none, because it reads as a pass.

Tool discovery: fxc.exe from the Windows SDK, glslangValidator from the Vulkan SDK.
Override with FXC / GLSLANG_VALIDATOR env vars. A missing tool SKIPS that half with
a clear message rather than failing (so non-Windows / no-Vulkan-SDK checkouts work).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RENDER = REPO / "src" / "HE_Rendering" / "src" / "Backends"

HLSL_SOURCES = {
    "HlslSources.h": RENDER / "D3D_Shared" / "HlslSources.h",
    "D3D11Renderer.cpp": RENDER / "D3D11" / "D3D11Renderer.cpp",
    "D3D12Renderer.cpp": RENDER / "D3D12" / "D3D12Renderer.cpp",
}
GLSL_SOURCE = RENDER / "OpenGL" / "OpenGLRenderer.cpp"
# Der analytische Himmelskern ist kein Stringliteral mehr, sondern eine echte
# Shader-Datei — dieselbe, die Vulkans sky.frag per #include zieht. OpenGL
# bekommt sie ueber einen generierten Header (cmake/embed_glsl.cmake), also
# findet extract() sie im C++-Text nicht mehr. Ohne diesen Pfad wuerde
# //#SKYFUNC# durch Leerstring ersetzt und JEDER GL-Shader mit skyColor()
# faellt mit "no matching overloaded function" um — was beim Umbau auch
# genau so passiert ist.
GLSL_SKY_CORE = REPO / "src" / "HE_Rendering" / "shaders" / "sky_core.glsl"

# Strings that are NOT standalone shaders — they are spliced into others and have
# no entry point of their own.
PRELUDES = {"kGiTraversalHLSL", "kSkyFuncHLSL", "kGiTraversalGLSL", "kSkyFuncGLSL"}

# What each call site prepends before compiling. Mirrors the C++ verbatim:
#   D3D11Renderer.cpp:2309  std::string(kSkyFuncHLSL) + kSceneHLSL
#   D3D11Renderer.cpp:2802  std::string(kSkyFuncHLSL) + kSkyPSHLSL
#   D3D11Renderer.cpp:1677  std::string(kGiTraversalHLSL) + kGiShadowCSHLSL
#   D3D11Renderer.cpp:1679  std::string(kGiTraversalHLSL) + kGiProbeCSHLSL
#   OpenGLRenderer.cpp:5607 "#version 430 core\n" + kGiTraversalGLSL + kGiShadowCS
HLSL_PRELUDE_OF = {
    "kGiShadowCSHLSL": "kGiTraversalHLSL",
    "kGiProbeCSHLSL": "kGiTraversalHLSL",
    "kSkyPSHLSL": "kSkyFuncHLSL",
    "kSkyPSHLSL12": "kSkyFuncHLSL",  # D3D12Renderer.cpp:1564 — D3D12's own sky PS copy
    "kSceneHLSL": "kSkyFuncHLSL",
}
GLSL_PRELUDE_OF = {
    "kGiShadowCS": "kGiTraversalGLSL",
    "kGiProbeCS": "kGiTraversalGLSL",
    "kGiReflCS": "kGiTraversalGLSL",
}

# Entry point -> HLSL profile. Taken from the `compile(..., "<entry>", "<profile>", ...)`
# call sites in D3D11Renderer.cpp / D3D12Renderer.cpp.
HLSL_ENTRY_PROFILE = {
    "GiGBufVS": "vs_5_0", "GiGBufPS": "ps_5_0",
    "GiProbeCS": "cs_5_0", "GiShadowCS": "cs_5_0",
    "PSLine": "ps_5_0", "VSLine": "vs_5_0",
    "PSMain": "ps_5_0", "VSMain": "vs_5_0",
    "VSMainInstanced": "vs_5_0", "VSMainSkinned": "vs_5_0", "VSDepth": "vs_5_0",
    "PSPos": "ps_5_0", "VSPos": "vs_5_0",
    "PSSky": "ps_5_0", "VSSky": "vs_5_0",
    "SSAOMain": "ps_5_0", "SSAOBlurMain": "ps_5_0",
    "UIPSMain": "ps_5_0", "UIVSMain": "vs_5_0",
    "main": "ps_5_0",  # the common case; exceptions below
}

# `main` is not always a pixel shader. Keyed by (string name, entry) so the
# exception is explicit rather than a name-suffix guess.
#   D3D12Renderer.cpp:1295 / :3040 / :4079  compile(kFSTriangleVS, "main", "vs_5_0", ...)
HLSL_PROFILE_OVERRIDE = {
    ("kFSTriangleVS", "main"): "vs_5_0",
}

GLSL_STAGE_BY_SUFFIX = [("VS", "vert"), ("FS", "frag"), ("CS", "comp")]


def extract(path: Path, tag: str) -> dict[str, str]:
    """All `kName = R"TAG( ... )TAG";` literals in one file."""
    src = path.read_text(encoding="utf-8", errors="replace")
    pat = r'(k[A-Za-z0-9]*)\s*=\s*R"' + tag + r'\((.*?)\)' + tag + r'";'
    return {m.group(1): m.group(2) for m in re.finditer(pat, src, re.S)}


def find_tool(env_var: str, names: list[str], hints: list[str]) -> str | None:
    if (p := os.environ.get(env_var)) and Path(p).exists():
        return p
    for n in names:
        if (w := shutil.which(n)):
            return w
    for hint in hints:
        base = Path(os.path.expandvars(hint))
        if not base.is_dir():
            continue
        for n in names:
            hits = sorted(base.rglob(n), reverse=True)  # newest SDK first
            if hits:
                return str(hits[0])
    return None


def run(cmd: list[str]) -> tuple[bool, str]:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except Exception as e:  # tool vanished / not executable
        return False, f"could not run {cmd[0]}: {e}"
    out = (r.stdout or "") + (r.stderr or "")
    return r.returncode == 0, out


def check_hlsl(tmp: Path, verbose: bool) -> tuple[int, int, list[str]]:
    fxc = find_tool("FXC", ["fxc.exe", "fxc"],
                    [r"%ProgramFiles(x86)%\Windows Kits\10\bin", r"%ProgramFiles%\Windows Kits\10\bin"])
    if not fxc:
        print("  fxc not found (Windows SDK) — HLSL check SKIPPED")
        return 0, 0, []

    strings: dict[str, str] = {}
    origin: dict[str, str] = {}
    for tag, path in HLSL_SOURCES.items():
        if not path.exists():
            print(f"  !! missing source: {path}")
            return 0, 1, [f"missing {path}"]
        for name, body in extract(path, "HLSL").items():
            # D3D11 and D3D12 keep private copies of scene/sky/etc. under the same
            # name — key by file so both are checked, not just the last one seen.
            strings[f"{tag}:{name}"] = body
            origin[f"{tag}:{name}"] = name

    jobs, covered, failures = [], set(), []
    for key, body in strings.items():
        name = origin[key]
        if name in PRELUDES:
            covered.add(key)
            continue
        pre_name = HLSL_PRELUDE_OF.get(name)
        pre = ""
        if pre_name:
            match = [v for k, v in strings.items() if origin[k] == pre_name]
            if not match:
                failures.append(f"{key}: prelude {pre_name} not found")
                continue
            pre = match[0]
        f = tmp / (key.replace(":", "__").replace(".", "_") + ".hlsl")
        f.write_text(pre + body, encoding="utf-8")
        for entry, profile in HLSL_ENTRY_PROFILE.items():
            if re.search(r"[\w>\]]\s+" + entry + r"\s*\(", body):
                jobs.append((key, f, entry, HLSL_PROFILE_OVERRIDE.get((name, entry), profile)))
                covered.add(key)

    for key in strings:
        if key not in covered:
            failures.append(f"{key}: not covered by any job — extend HLSL_ENTRY_PROFILE")

    ok_n = 0
    for key, f, entry, profile in jobs:
        ok, out = run([fxc, "/nologo", "/T", profile, "/E", entry, "/Fo", os.devnull, str(f)])
        if ok:
            ok_n += 1
            if verbose:
                print(f"  ok   {key:44s} {entry:16s} {profile}")
        else:
            errs = [l.strip() for l in out.splitlines() if "error X" in l]
            failures.append(f"{key} / {entry} ({profile}):\n      " + "\n      ".join(errs[:3]))
            print(f"  FAIL {key:44s} {entry:16s} {profile}")
    return ok_n, len(failures), failures


def check_glsl(tmp: Path, verbose: bool) -> tuple[int, int, list[str]]:
    gv = find_tool("GLSLANG_VALIDATOR", ["glslangValidator.exe", "glslangValidator"],
                   [r"%VULKAN_SDK%\Bin", r"%VULKAN_SDK%\bin", r"C:\VulkanSDK"])
    if not gv:
        print("  glslangValidator not found (Vulkan SDK) — GLSL check SKIPPED")
        return 0, 0, []
    if not GLSL_SOURCE.exists():
        return 0, 1, [f"missing {GLSL_SOURCE}"]

    strings = extract(GLSL_SOURCE, "GLSL")
    # Erst die eigene Datei, dann das alte Literal — letzteres nur noch als
    # Rueckfall, falls jemand den Kern wieder in den C++-Text zieht.
    if GLSL_SKY_CORE.exists():
        skyfunc = "\n" + GLSL_SKY_CORE.read_text(encoding="utf-8", errors="replace")
    else:
        skyfunc = strings.get("kSkyFuncGLSL", "")
    if not skyfunc.strip():
        return 0, 1, [f"sky core leer — weder {GLSL_SKY_CORE.name} noch kSkyFuncGLSL gefunden"]
    ok_n, failures = 0, []

    for name, body in strings.items():
        if name in PRELUDES:
            continue
        stage = next((s for suf, s in GLSL_STAGE_BY_SUFFIX if name.endswith(suf)), None)
        if stage is None:
            failures.append(f"{name}: not covered — no VS/FS/CS suffix, extend GLSL_STAGE_BY_SUFFIX")
            continue
        src = strings.get(GLSL_PRELUDE_OF[name], "") + body if name in GLSL_PRELUDE_OF else body
        src = src.replace("//#SKYFUNC#", skyfunc)          # injectSkyFunc (OpenGLRenderer.cpp:4095)
        if "#version" not in src:
            src = "#version 430 core\n" + src               # the compute call sites' header
        f = tmp / f"gl__{name}.{stage}"
        f.write_text(src, encoding="utf-8")
        ok, out = run([gv, "-S", stage, str(f)])
        if ok:
            ok_n += 1
            if verbose:
                print(f"  ok   {name:44s} {stage}")
        else:
            errs = [l.strip() for l in out.splitlines() if "ERROR:" in l]
            failures.append(f"{name} ({stage}):\n      " + "\n      ".join(errs[:3]))
            print(f"  FAIL {name:44s} {stage}")
    return ok_n, len(failures), failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hlsl-only", action="store_true")
    ap.add_argument("--glsl-only", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    total_ok, all_failures = 0, []
    with tempfile.TemporaryDirectory(prefix="he_shadercheck_") as td:
        tmp = Path(td)
        if not args.glsl_only:
            print("HLSL (D3D11/D3D12, runtime-compiled via D3DCompile):")
            ok, _, f = check_hlsl(tmp, args.verbose)
            total_ok += ok
            all_failures += f
            print(f"  {ok} compiled, {len(f)} failed")
        if not args.hlsl_only:
            print("GLSL (OpenGL, runtime-compiled via glCompileShader):")
            ok, _, f = check_glsl(tmp, args.verbose)
            total_ok += ok
            all_failures += f
            print(f"  {ok} compiled, {len(f)} failed")

    if all_failures:
        print(f"\n{len(all_failures)} PROBLEM(S) — these shaders would silently disable "
              f"their feature at runtime:\n")
        for f in all_failures:
            print(f"  * {f}")
        return 1
    print(f"\nAll {total_ok} embedded shaders compile.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
