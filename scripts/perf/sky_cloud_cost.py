#!/usr/bin/env python3
"""GPU cost of the sky's clouds on Metal: Scene-pass p50 with and without clouds, per quality.

Builds a tiny project (the sky-only perf scene docs/perf-audit/scenes/skyonly.hescene with the
cloud settings of each variant), runs scripts/he_perf_capture.py --detailed on it for every
variant and prints the median Scene-pass and whole-frame GPU time. The clouds' cost is the
difference to the "clear" variant.

    scripts/perf/sky_cloud_cost.py [--out DIR] [--variants clear,q0,q1,q2,lowres[,cov05,cov95,noshad]] [--style 1] [--mode 1]

Run it on an idle machine (ioreg Device Utilization, display awake: see the perf notes) and from the
tree whose out/deploy/Editor you want to measure. Same camera for every variant: on the ground, looking
22 degrees up at a 3D layer 300 m up. Medium/High/Low are cloudQuality 1/2/0; "lowres" is Medium with
the quarter-resolution cloud pass.
"""
import argparse, copy, json, statistics, subprocess, sys, tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
VARIANTS = {
    "clear":  dict(cloudCoverage=0.0),
    "q0":     dict(cloudCoverage=0.55, cloudQuality=0),
    "q1":     dict(cloudCoverage=0.55, cloudQuality=1),
    "q2":     dict(cloudCoverage=0.55, cloudQuality=2),
    "lowres": dict(cloudCoverage=0.55, cloudQuality=1, lowResClouds=True),
    # where the time goes: a nearly empty slab (the column test only), a dense one (bodies + sun march)
    "cov05":  dict(cloudCoverage=0.05, cloudQuality=1),
    "cov95":  dict(cloudCoverage=0.95, cloudQuality=1),
    "noshad": dict(cloudCoverage=0.55, cloudQuality=1, cloudInterShadows=False),
    # the cloud types of the Realistic layer at Medium: flat cumulus, towering cumulus, stratocumulus, cumulonimbus
    "type1":  dict(cloudCoverage=0.55, cloudQuality=1, cloudType=1),
    "type2":  dict(cloudCoverage=0.55, cloudQuality=1, cloudType=2),
    "type3":  dict(cloudCoverage=0.55, cloudQuality=1, cloudType=3),
    "type4":  dict(cloudCoverage=0.55, cloudQuality=1, cloudType=4),
}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(tempfile.gettempdir()) / "sky_cloud_cost"))
    ap.add_argument("--variants", default=",".join(VARIANTS))
    ap.add_argument("--style", type=int, default=1, help="cloudStyle: 0 Classic, 1 Realistic")
    ap.add_argument("--mode", type=int, default=1, help="cloudMode: 0 dome, 1 3D")
    ap.add_argument("--repeat", type=int, default=1, help="captures per variant; prints min and median of the Scene p50s "
                    "(the GPU is shared with the window server and other apps: one run can be 60 percent off)")
    a = ap.parse_args()
    out = Path(a.out); (out / "Content").mkdir(parents=True, exist_ok=True)
    scene = json.load(open(REPO / "docs/perf-audit/scenes/skyonly.hescene"))
    proj = {"name": "SkyPerf", "id": "00000000000000000000000000000001", "version": "1.0", "preset": 1,
            "startupScene": "Content/StartupScene.hescene", "scriptLanguage": "HorizonCode", "appProject": False,
            "advancedShaderEffects": False, "allowFiles": False, "allowProcesses": False, "allowNetwork": False,
            "fontScripts": 0, "fontWeightBold": True, "appIconName": "sports_esports",
            "appIconColor": "#1e70c8", "appVersion": "1.0"}
    json.dump(proj, open(out / "SkyPerf.heproj", "w"), indent=4)
    for name in a.variants.split(","):
        s = copy.deepcopy(scene)
        for e in s["entities"]:
            env = e.get("components", {}).get("environment")
            if env is not None:
                env.update(cloudMode=a.mode, cloudStyle=a.style, cloudHeight=300.0, cloudQuality=1,
                           timeOfDay=0.38, cloudShadows=False, lowResClouds=False)
                env.update(VARIANTS[name])
        json.dump(s, open(out / "Content/StartupScene.hescene", "w"))
        runs = []
        for k in range(a.repeat):
            r = subprocess.run([sys.executable, str(REPO / "scripts/he_perf_capture.py"), "--project", str(out / "SkyPerf.heproj"),
                                "--label", name, "--out", str(out), "--detailed", "--cam", "0,3,0,0,0.38"],
                               capture_output=True, text=True)
            prof = out / f"{name}.profile.json"
            if r.returncode != 0 or not prof.exists():
                print(f"{name:7} FAILED", r.stderr[-200:]); break
            frames = json.load(open(prof))["frames"]
            scene_ms = [p["ms"] for f in frames for p in f.get("gpu", []) if p["n"] == "Scene"]
            runs.append(statistics.median(scene_ms))
        if runs:
            print(f"{name:7} Scene p50 min {min(runs):6.2f}  median {statistics.median(runs):6.2f}  runs {[round(x, 1) for x in runs]}")

if __name__ == "__main__":
    main()
