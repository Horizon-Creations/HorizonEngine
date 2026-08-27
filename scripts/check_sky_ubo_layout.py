#!/usr/bin/env python3
"""check_sky_ubo_layout — prueft, ob die Sky-Konstantenpuffer der Backends
offset-gleich zu HE::SkyFrameParams sind.

Warum es das braucht: SkyFrameParams ist die kanonische Sky-Konstantenstruktur
(mat4 + 17 float4 = 336 Bytes, gespiegelt vom MSL-Layout). Vulkan, D3D11 und
D3D12 hatten davon lange eigene, kleinere und anders sortierte Fassungen -- 160,
144 und 160 Bytes -- und mussten Feld fuer Feld umkopieren. Jeder neue
Sky-Parameter war damit eine Stelle, die man an drei Orten nachziehen muss:
C++-Struktur, Shader-Block, Kopiercode. Wird einer vergessen, kommt im Shader
still der falsche Wert an oder gar keiner. Kein Test faellt dabei um; das Bild
sieht nur anders aus. Genau so waren HE_DUMP_STARDENS und der ganze Sternblock
auf allen drei Backends wirkungslos.

Geprueft wird aus dem KOMPILIERTEN Ergebnis, nicht aus dem Quelltext:
  Vulkan  spirv-dis auf sky.frag.spv  -> OpMemberDecorate ... Offset N
  D3D     fxc /Fc auf kSkyParamsHLSL  -> "// float4 name; // Offset: N"
Das ist der Punkt: std140 und die HLSL-Packung haben genug Fallen (ein vec3
belegt 16 Bytes, aber ein float darf dahinter einruecken), dass Nachrechnen von
Hand nicht reicht.

Aufruf:
    python scripts/check_sky_ubo_layout.py [pfad/zu/sky.frag.spv]
Exit 0 = beide Layouts stimmen, 1 = Abweichung (mit Bericht).
"""
import os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Die kanonische Reihenfolge, abgelesen aus BuildSkyFrameParams in
# src/HE_Rendering/src/SkyFrameParams.cpp. Ein Eintrag je float4, in genau der
# Reihenfolge, in der die Struktur sie deklariert.
CANON = [
    ("invViewProj",      0),
    ("sunDir",          64),   # xyz sunDir, w hasMoonTexture
    ("sunColor",        80),   # xyz sunColor, w moonPhase
    ("params",          96),   # timeOfDay, cloudCoverage, time, auroraIntensity
    ("nebulaColor",    112),   # xyz, w nebulaIntensity
    ("auroraColor",    128),   # xyz, w milkyWayIntensity
    ("wind",           144),   # xyz CloudWindVector, w flash
    ("cameraPos",      160),   # xyz, w cloudMode
    ("cloud",          176),   # cloudHeight, density, fluffiness, contrailAmount
    ("cloudTint",      192),   # xyz, w cirrusAmount
    ("cirrus",         208),   # cirrusSeed, auroraHeight, auroraFragmentation, nebulaSeed
    ("nebulaColor2",   224),   # xyz, w nebulaQuality
    ("nebulaColor3",   240),   # xyz, w godRays
    ("auroraColorTop", 256),   # xyz, w shootingStars
    ("starColor",      272),   # xyz, w starBrightness
    ("star",           288),   # starSize, starSizeVariation, starDensity, starGlow
    ("star2",          304),   # starTwinkle, cloudQuality, lowResClouds, rainAmount
    ("neb2",           320),   # nebulaCoverage, cloudStyle, cloudInterShadows, cloudEvolution
]
CANON_BYTES = 336


def compare(got, what):
    """got = [(name, offset), ...] in Deklarationsreihenfolge."""
    bad = []
    for i, (name, off) in enumerate(CANON):
        if i >= len(got):
            bad.append(f"  FEHLT   [{i:2d}] {name:15s} erwartet Offset {off}")
            continue
        gname, goff = got[i]
        if goff != off:
            bad.append(f"  OFFSET  [{i:2d}] {gname:18s} ist {goff}, kanonisch {off} ({name})")
    for i in range(len(CANON), len(got)):
        bad.append(f"  EXTRA   [{i:2d}] {got[i][0]} bei Offset {got[i][1]} — nicht in SkyFrameParams")
    if bad:
        print(f"{what} ist NICHT offset-gleich zu HE::SkyFrameParams:")
        print("\n".join(bad))
        print("  Folge: das memcpy der 336-Byte-Struktur liefert falsche Werte.")
        print("  Siehe docs/backend-parity-plan.md, P3b.")
        return 1
    print(f"  Layout stimmt — {CANON_BYTES} Bytes, alle {len(CANON)} Member offset-gleich.")
    return 0


def find_tool(name, exe, roots):
    if p := shutil.which(name):
        return p
    for r in roots:
        base = Path(os.path.expandvars(r)) if r else None
        if base and base.is_dir():
            hits = sorted(base.rglob(exe), reverse=True)   # neuestes SDK zuerst
            if hits:
                return str(hits[0])
    return None


# ── Vulkan-Seite ─────────────────────────────────────────────────────────────
def check_vulkan(spv_arg):
    spv = Path(spv_arg) if spv_arg else None
    if spv is None:
        for c in (REPO / "out/build/x64-release/Shaders/sky.frag.spv",
                  REPO / "out/build/x64-debug/Shaders/sky.frag.spv"):
            if c.exists():
                spv = c
                break
    if spv is None or not spv.exists():
        print("sky.frag.spv nicht gefunden — Vulkan-Pruefung UEBERSPRUNGEN")
        return 0
    dis = find_tool("spirv-dis", "spirv-dis.exe", [os.environ.get("VULKAN_SDK"), r"C:\VulkanSDK"])
    if not dis:
        print("spirv-dis nicht gefunden (Vulkan SDK) — Vulkan-Pruefung UEBERSPRUNGEN")
        return 0
    out = subprocess.run([dis, str(spv)], capture_output=True, text=True).stdout
    names, offs = {}, {}
    for m in re.finditer(r'OpMemberName %SkyEnv (\d+) "([^"]+)"', out):
        names[int(m.group(1))] = m.group(2)
    for m in re.finditer(r'OpMemberDecorate %SkyEnv (\d+) Offset (\d+)', out):
        offs[int(m.group(1))] = int(m.group(2))
    got = [(names.get(i, f"<{i}>"), offs[i]) for i in sorted(offs)]
    print(f"Vulkan (SPIR-V): {spv.name} — {len(got)} Blockmember")
    return compare(got, "Vulkans Sky-UBO")


# ── D3D-Seite ────────────────────────────────────────────────────────────────
def check_hlsl():
    hdr = REPO / "src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h"
    if not hdr.exists():
        print("HlslSources.h nicht gefunden — D3D-Pruefung UEBERSPRUNGEN")
        return 0
    m = re.search(r'kSkyParamsHLSL = R"HLSL\((.*?)\)HLSL";',
                  hdr.read_text(encoding="utf-8", errors="replace"), re.S)
    if not m:
        print("kSkyParamsHLSL nicht in HlslSources.h gefunden")
        return 1
    fxc = find_tool("fxc", "fxc.exe", [r"C:\Program Files (x86)\Windows Kits\10\bin"])
    if not fxc:
        print("fxc nicht gefunden (Windows SDK) — D3D-Pruefung UEBERSPRUNGEN")
        return 0
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        # Dummy-Entry, damit fxc etwas zu uebersetzen hat. Ungenutzte Member
        # markiert fxc mit [unused], listet ihre Offsets aber trotzdem.
        f = td / "skyparams.hlsl"
        f.write_text(m.group(1) + "\nfloat4 PSMain():SV_Target{return skyNeb2+"
                                  "skyStar2+skyStarColor+uInvViewProj[0];}\n",
                     encoding="utf-8")
        asm = td / "skyparams.asm"
        r = subprocess.run([fxc, "/T", "ps_5_0", "/E", "PSMain", "/Fc", str(asm),
                            "/Fo", str(td / "o.cso"), str(f)],
                           capture_output=True, text=True)
        if r.returncode != 0 or not asm.exists():
            print("kSkyParamsHLSL uebersetzt NICHT:")
            print("  " + (r.stdout or r.stderr)[:800])
            return 1
        text = asm.read_text(encoding="utf-8", errors="replace")
    got = [(mm.group(2), int(mm.group(3))) for mm in
           re.finditer(r"^//\s+(\w[\w<>]*)\s+(\w+);\s+// Offset:\s+(\d+)", text, re.M)]
    print(f"D3D11/D3D12 (kSkyParamsHLSL, fxc ps_5_0) — {len(got)} cbuffer-Member")
    return compare(got, "D3Ds Sky-cbuffer")


def main():
    rc = check_vulkan(sys.argv[1] if len(sys.argv) > 1 else None)
    print()
    rc |= check_hlsl()
    return rc


if __name__ == "__main__":
    sys.exit(main())
