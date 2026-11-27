#!/usr/bin/env python3
"""Flies the editor's camera along a path through a project and records it as a video (src/HE_Editor/EditorFlythrough.h).

  python3 scripts/he_flythrough.py --project ~/HorizonEngineProjects/MyGame/MyGame.heproj --path flight.json --out flight.mp4
      [--cfgdir /tmp/he_fly_cfg] [--set RuntimeVirtualTextures=true] [--env HE_LS_FAR=0] [--keep-frames] [--rhi Metal]

The path file: { "fps": 30, "warmupSeconds": 5, "realtime": true,
                 "keys": [ { "t": 0, "pos": [x, y, z], "yaw": deg, "pitch": deg }, { "t": 10, "pos": [...], "lookAt": [x, y, z] } ] }
(absolute world metres; yaw 0 looks down -Z, +yaw turns right, +pitch looks up). The editor runs hidden with a private config
dir (--cfgdir, the user's Preferences are not touched: copy their config.json there first to fly with their settings; --set
writes editor CustomConfig keys on top), holds the
first key for warmupSeconds, then writes one frame per 1/fps of path time and leaves. The frames become an H.264 MP4 through
ffmpeg if it is installed, else through scripts/he_video_encode.swift (AVFoundation, macOS).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from he_perf_capture import EDITOR, REPO, RHI, WINDOWS, parse_val  # noqa: E402


def write_config(cfg_dir: Path, project: Path, sets, rhi: str):
    """The config the editor starts with: the one already in cfg_dir (a copy of someone's Preferences: their RVT, AA, GI ...
    switches are what the flight should show) with the project and the --set keys on top. A fresh dir starts empty."""
    cfg_dir.mkdir(parents=True, exist_ok=True)
    path = cfg_dir / "config.json"
    cfg = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    custom = {c["Key"]: c["Value"] for c in cfg.get("CustomConfig", [])}
    for kv in sets:
        k, v = kv.split("=", 1)
        custom[k] = parse_val(v)
    cfg["CustomConfig"] = [{"Key": k, "Value": v} for k, v in sorted(custom.items())]
    cfg["KnownProjects"] = sorted(set(cfg.get("KnownProjects", [])) | {str(project)})
    cfg["LastProjectPath"] = str(project)
    cfg["RHI"] = RHI[rhi]
    path.write_text(json.dumps(cfg, indent=4), encoding="utf-8")


def encode(frames: Path, fps: int, out: Path):
    if shutil.which("ffmpeg"):
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(fps), "-i", str(frames / "frame_%05d.ppm"),
               "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", str(out)]
    elif sys.platform == "darwin" and shutil.which("swift"):
        cmd = ["swift", str(REPO / "scripts" / "he_video_encode.swift"), str(frames), str(fps), str(out)]
    else:
        sys.exit(f"no encoder (ffmpeg, or swift on macOS): the frames stay in {frames}")
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", required=True)
    ap.add_argument("--path", required=True, help="camera path JSON")
    ap.add_argument("--out", required=True, help="the video (.mp4)")
    ap.add_argument("--cfgdir", default=os.path.join(tempfile.gettempdir(), "he_flythrough_cfg"))
    ap.add_argument("--rhi", default="Metal", choices=sorted(RHI))
    ap.add_argument("--set", action="append", default=[], help="editor CustomConfig KEY=VALUE")
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE environment")
    ap.add_argument("--keep-frames", action="store_true", help="keep the PPM frames beside the video")
    ap.add_argument("--stats-only", action="store_true",
                    help="fly and log the frame times only, no pictures and no video (a read-back waits for the GPU every frame)")
    ap.add_argument("--timeout", type=float, default=1800)
    ap.add_argument("--editor", default=str(EDITOR))
    a = ap.parse_args()

    path = Path(a.path).resolve()
    spec = json.loads(path.read_text(encoding="utf-8"))
    fps = int(round(float(spec.get("fps", 30))))
    out = Path(a.out).resolve()
    frames = out.parent / (out.stem + "_frames")
    shutil.rmtree(frames, ignore_errors=True)
    frames.mkdir(parents=True)
    write_config(Path(a.cfgdir), Path(a.project).resolve(), a.set, a.rhi)

    env = dict(os.environ)
    env.update({"HE_CONFIG_DIR": a.cfgdir, "HE_HIDDEN_WINDOW": "1", "HE_BACKGROUND_FPS": "0", "HE_COLLAB_OFFLINE": "1",
                "HE_FLYTHROUGH": str(path), "HE_FLYTHROUGH_OUT": str(frames)})
    if WINDOWS:
        env.setdefault("HE_NET_LOOPBACK_ONLY", "1")
    if a.stats_only:
        env["HE_FLYTHROUGH_STATS_ONLY"] = "1"
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v

    editor = Path(a.editor).resolve()
    t0 = time.time()
    proc = subprocess.Popen([str(editor)], cwd=str(editor.parent), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        rc = proc.wait(timeout=a.timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        sys.exit(f"timeout after {a.timeout:.0f} s; frames so far in {frames}")
    shutil.copyfile(editor.parent / "HorizonEngine.log", out.with_suffix(".log"))
    n = len(list(frames.glob("frame_*.ppm")))
    print(f"editor rc={rc}, {n} frames in {time.time() - t0:.0f} s (log: {out.with_suffix('.log')})")
    for line in out.with_suffix(".log").read_text(encoding="utf-8", errors="replace").splitlines():
        if "Flythrough: frame work" in line:
            print(line[line.find("Flythrough:"):])
    if a.stats_only:
        shutil.rmtree(frames, ignore_errors=True)
        return
    if n == 0:
        sys.exit("no frames: see the log (\"Flythrough:\" lines)")
    encode(frames, fps, out)
    if not a.keep_frames:
        shutil.rmtree(frames, ignore_errors=True)
    print(out)


if __name__ == "__main__":
    main()
