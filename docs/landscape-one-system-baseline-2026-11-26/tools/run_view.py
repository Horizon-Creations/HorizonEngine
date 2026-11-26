#!/usr/bin/env python3
"""one-landscape-syst-3: one tile-column capture at a view. usage: run_view.py LABEL VIEW(1-5) FRAMES [extra he_perf_capture args...]"""
import json, os, subprocess, sys
REPO = "/Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-landscape-one-system"
sys.path.insert(0, "/tmp/he_s3")
W = "/tmp/he_base_catania"
OUT = "/tmp/he_s3/raw"
label, view, frames = sys.argv[1], int(sys.argv[2]), sys.argv[3]
extra = sys.argv[4:]
VIEWS = {1: "7795.838,113.462,-4011.834,69.634,-0.167", 2: "7795.838,113.462,-4011.834,71.205,-0.167", 3: "0,10,0,0,-0.05",
         4: "2811.128,2113.346,2811.128,-0.7853982,-0.4876162", 5: "20000,300,20000,2.0,-0.1"}
col = "tiles"
x, y, z, yaw, pitch = (float(t) for t in VIEWS[view].split(","))
x += 50000.0; z += 50000.0
if os.environ.get("CAM"):
    x, y, z, yaw, pitch = (float(t) for t in os.environ["CAM"].split(","))   # tile-world coordinates
os.makedirs(OUT, exist_ok=True)
json.dump({"ortho": False, "pitch": pitch, "pivot": 100.0, "position": [x, y, z], "yaw": yaw}, open(f"{W}/{col}/Saved/EditorView.json", "w"), indent=2)
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
cmd = ["python3", f"{REPO}/scripts/he_perf_capture.py", "--project", f"{W}/{col}/HEProject.heproj", "--label", label, "--out", OUT,
       "--cfgdir", f"{W}/cfg", "--warmup", frames, "--frames", os.environ.get("FR","60"), "--no-counters",
       "--env", "HE_HIDDEN_WINDOW=1", "--env", f"HE_CAPTURE_FRAME={int(frames)-50}", "--env", f"HE_CAPTURE_PATH={pic}",
       "--env", "HE_COLLAB_OFFLINE=1", "--env", "HE_BACKGROUND_FPS=0"] + sets + extra
print(" ".join(cmd[:8]), "...", flush=True)
subprocess.run(cmd)
