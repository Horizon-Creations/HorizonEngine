#!/usr/bin/env python3
"""Driver for the Catania baseline: one he_perf_capture.py run per (column, view, counters).
usage: run_matrix.py COLUMN(old|tiles) VIEWS(1,2,..) MODE(fps|gpu) [TAG]"""
import json, os, subprocess, sys, time
REPO = "/Users/connorjansen/VSCode/HorizonEngine/.claude/worktrees/claude-landscape-one-system"
W = "/tmp/he_base_catania"
OUT = "/tmp/he_base_catania/raw"
VIEWS = {  # x,y,z,yaw,pitch
    1: "7795.838,113.462,-4011.834,69.634,-0.167",   # the owner's own editor camera in Catania
    2: "7795.838,113.462,-4011.834,71.205,-0.167",   # same spot, turned +pi/2
    3: "0,10,0,0,-0.05",                              # near the world origin, low
    4: "2811.128,2113.346,2811.128,-0.7853982,-0.4876162",  # the editor default whole-terrain view of the old Catania scene
    5: "20000,300,20000,2.0,-0.1",                    # 28 km out, horizon
}
KEYS = ["AASharpness", "AntiAliasing", "BloomEnabled", "BloomIntensity", "BloomThreshold", "DoFAperture", "DoFEnabled",
        "DoFFocusDistance", "DoFFocusRange", "GIIndirectIntensity", "GILightRadius", "GIReflBlur", "GIReflBounces",
        "GIReflIntensity", "GIReflMaxRoughness", "GIReflQuality", "GIReflectionsEnabled", "GIShadowQuality",
        "GlobalIlluminationEnabled", "GpuParticles", "MaterialQuality", "MaxFps", "MotionBlurEnabled", "OcclusionCulling",
        "RenderPath", "RenderScale", "RuntimeVirtualTextures", "SSAOEnabled", "SSAOIntensity", "SSAOMethod", "SSAORadius",
        "SSREnabled", "SSRIntensity", "SSRMaxRoughness", "SSRQuality", "SpecularAA", "SpecularAAStrength", "TextureQuality"]
col, views, mode = sys.argv[1], [int(v) for v in sys.argv[2].split(",")], sys.argv[3]
tag = sys.argv[4] if len(sys.argv) > 4 else ""
user = json.load(open(os.path.expanduser("~/Library/Application Support/HorizonEngine/config.json")))
sets = []
for c in user["CustomConfig"]:
    if c["Key"] in KEYS:
        sets += ["--set", f'{c["Key"]}={json.dumps(c["Value"])}']
def cam_for(col, v):
    # The migration puts the terrain's south-west corner at pack (0,0): the old terrain is centred on its entity, so the same
    # ground is at +half the world (50 km) in x and z in the tile world.
    x, y, z, yaw, pitch = VIEWS[v].split(",")
    if col == "tiles":
        x, z = str(float(x) + 50000.0), str(float(z) + 50000.0)
    return ",".join([x, y, z, yaw, pitch])
os.makedirs(OUT, exist_ok=True)
for v in views:
    label = f"{col}-v{v}-{mode}{tag}"
    pic = f"{OUT}/{label}.ppm"
    cmd = ["python3", f"{REPO}/scripts/he_perf_capture.py", "--project", f"{W}/{col}/HEProject.heproj", "--label", label,
           "--out", OUT, "--cfgdir", f"{W}/cfg", "--warmup", "300", "--frames", "600",
           "--env", "HE_HIDDEN_WINDOW=1", "--env", f"HE_CAPTURE_FRAME=250", "--env", f"HE_CAPTURE_PATH={pic}",
           "--env", "HE_COLLAB_OFFLINE=1", "--env", "HE_BACKGROUND_FPS=0"] + sets
    if mode == "det":
        cmd.append("--detailed")
    if mode == "fps":
        cmd.append("--no-counters")
    x, y, z, yaw, pitch = (float(t) for t in cam_for(col, v).split(","))
    # The editor takes its camera from <project>/Saved/EditorView.json (loadProjectCameraView), NOT from config.json, and rewrites it on exit.
    json.dump({"ortho": False, "pitch": pitch, "pivot": 100.0, "position": [x, y, z], "yaw": yaw}, open(f"{W}/{col}/Saved/EditorView.json", "w"), indent=2)
    print(time.strftime("%H:%M:%S"), label, os.popen("pmset -g batt | head -2 | tr '\\n' ' '").read().strip(), "| uptime", os.popen("uptime").read().strip().split("load")[-1], flush=True)
    subprocess.run(cmd)
