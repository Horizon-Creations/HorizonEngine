#!/usr/bin/env python3
"""topic 355 step 4: one tile-column capture of Catania at a camera pose, on a private copy of the project and config.
usage: run_view.py LABEL x,y,z,yawRad,pitchRad WARMUP_FRAMES [extra he_perf_capture args...]   (tile-world coordinates, 0..100000 m)
env: ROOT (default /tmp/he_s4: holds tiles/ = the migrated project, cfg/ = the private config dir, raw/ = output), REPO"""
import json, os, subprocess, sys
REPO = os.environ.get("REPO", "/Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-landscape-one-system")
W = os.environ.get("ROOT", "/tmp/he_s4")
OUT = f"{W}/raw"
label, cam, frames = sys.argv[1], sys.argv[2], sys.argv[3]
extra = sys.argv[4:]
x, y, z, yaw, pitch = (float(t) for t in cam.split(","))
os.makedirs(OUT, exist_ok=True)
json.dump({"ortho": False, "pitch": pitch, "pivot": 100.0, "position": [x, y, z], "yaw": yaw}, open(f"{W}/tiles/Saved/EditorView.json", "w"), indent=2)
user = json.load(open(os.path.expanduser("~/Library/Application Support/HorizonEngine/config.json")))
KEYS = ["AASharpness", "AntiAliasing", "BloomEnabled", "BloomIntensity", "BloomThreshold", "DoFAperture", "DoFEnabled",
        "DoFFocusDistance", "DoFFocusRange", "GIIndirectIntensity", "GILightRadius", "GIReflBlur", "GIReflBounces",
        "GIReflIntensity", "GIReflMaxRoughness", "GIReflQuality", "GIReflectionsEnabled", "GIShadowQuality",
        "GlobalIlluminationEnabled", "GpuParticles", "MaterialQuality", "MaxFps", "MotionBlurEnabled", "OcclusionCulling",
        "RenderPath", "RenderScale", "RuntimeVirtualTextures", "SSAOEnabled", "SSAOIntensity", "SSAOMethod", "SSAORadius",
        "SSREnabled", "SSRIntensity", "SSRMaxRoughness", "SSRQuality", "SpecularAA", "SpecularAAStrength", "TextureQuality"]
sets = []
for c in user["CustomConfig"]:
    if c["Key"] in KEYS:
        sets += ["--set", f'{c["Key"]}={json.dumps(c["Value"])}']
pic = f"{OUT}/{label}.ppm"
cmd = ["python3", f"{REPO}/scripts/he_perf_capture.py", "--project", f"{W}/tiles/HEProject.heproj", "--label", label, "--out", OUT,
       "--cfgdir", f"{W}/cfg", "--warmup", frames, "--frames", os.environ.get("FR", "60")] + ([] if os.environ.get("COUNTERS") else ["--no-counters"]) + [
       "--env", "HE_HIDDEN_WINDOW=1", "--env", f"HE_CAPTURE_FRAME={int(frames)-5}", "--env", f"HE_CAPTURE_PATH={pic}",
       "--env", "HE_COLLAB_OFFLINE=1", "--env", "HE_BACKGROUND_FPS=0", "--env", "HE_RVT_TILE_PROBE=1"] + sets + extra
print(" ".join(cmd[:8]), "...", flush=True)
subprocess.run(cmd)
