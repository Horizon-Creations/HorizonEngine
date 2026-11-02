# Editor viewport: FPS drop while the camera moves (Thema 221, step 1)

Report: since the merges of 18.10.2026 the editor viewport falls from ~120 to at most ~30 FPS
while the camera moves and recovers at once when it stops. The human's details (via the queen):
build release/0.7.0 = main @ 1c1fd193, project **Catania**, Outliner + Content Browser open,
movement with WASD + mouse; **turning alone (mouse, no WASD) does not drop, only a position change**.

## 1. What is actually in the bisect window

Of the audit stages only **Stage 1 Core (#166)** and **Stage 6 Editor (#167)** are on
main/release/0.7.0. Rendering frontend (214), Metal (216), Scene (218) and Net (219) are NOT merged
(their branches are 10-19 commits ahead), so FrameVisibility, the sky-IBL throttle, the terrain gates
and the settled flags named in the topic are not in the reported build at all. Between the last
deploy before (70925e74, 16.10.) and 1c1fd193 are exactly six merges: #162 GI terrain seams (chunk
seam stitching), #163 cell streaming in Play, #165 underwater caustics (shader), #166 Core, #167
Editor, #164 runtime-size thresholds.

## 2. Harness (commit cf4227a2, all env-gated, off by default)

| Env | Where | Does |
|---|---|---|
| `HE_PERF_CAM_SPIN=<rad>` / `HE_PERF_CAM_FLY=<units>` | `src/HE_Editor/EditorCamera.cpp` (end of `update`) | turn / move the camera per update, reversing every 240 updates |
| `HE_PERF_KEY_HOLD=<key>` | `src/HE_Core/src/Application/Application.cpp` (next to `HE_PERF_INPUT_EVENTS`) | SDL key-down from frame 50, a repeat every frame |
| `HE_PERF_KEY_RMB=1` | same | pointer to the window centre (frame 45), right button down (frame 49) |

`HE_PERF_KEY_RMB` only reaches the fly-look with `--set PointerInput=1`: PointerInput 0 (Auto)
means trackpad mode on any machine with a battery, and the trackpad fly toggle reads the physical
SDL button, which a pushed event does not set. Proof that the WASD path ran: the editor's own
`Edit-nav diagnostic` line (`navigating=1 look=1 W=1`), written every 60 frames while navigating.

## 3. Measurements (M5 MacBook Air, Metal, Release, screen locked, hidden 3200x1800 window, vsync off, `--no-counters`, 400 frames after 200 warm-up)

Private copy of Catania, the human's settings (Occlusion Culling on, forward, FXAA, SSAO on).
`occl` = p50 of `OcclusionCuller::refine` per frame (frames 150+).

| Run | Camera | FPS | CPU p50 | GPU p50 | occl | draws |
|---|---|---|---|---|---|---|
| head-still | (0,2,0) looking up, still | 35.7 | 9.8 | 17.0 | 2.3 | 72 |
| head-fly / head-spin | same, FLY 0.15 / SPIN 0.004 | 35.1 / 34.3 | 9.1 / 9.7 | 15.3 / 16.1 | 4.4 / 3.0 | 72 / 75 |
| head-still3 | (0,30,0) over the terrain, still | 65.5 | 14.5 | 10.9 | 12.0 | 73 |
| head-fly2 | same, FLY 1.0 (±240 m) | 60.3 | 13.2 | 10.8 | 10.8 | 69 |
| head-keyW | same, W held, no RMB (`navigating=0`) | 63.3 | 14.5 | 10.9 | 11.9 | 73 |
| head-rmbW2 | same, RMB + W, real fly-look (`navigating=1 look=1 W=1`) | 75.5 | 11.8 | 8.9 | 9.4 | 44 |
| head-occoff-still / -fly | (0,30,0), Occlusion Culling off | 37.4 / 38.1 | 10.9 / 10.3 | 18.8 / 18.7 | 0 | 73 / 69 |
| **before-still** | (0,30,0), **70925e74 build of 16.10.** | 63.6 | 14.6 | 10.9 | 12.0 | 73 |
| low-still / low-rmbW | (0,5,0) near horizontal, still / RMB + W | 42.0 / 83.3 | 9.0 / 10.4 | 13.5 / 8.5 | 5.9 / 7.8 | 73 / 46 |

Findings:
1. **No reproduction headless.** Moving (scripted or through the real RMB+W path), turning and a
   held key cost the same as standing still, within the run-to-run noise; the flights end up
   faster because they reach cheaper views.
2. **The six merges do not change the still frame**: the 16.10. build and HEAD are identical at
   the terrain pose (CPU 14.6 / 14.5 ms, GPU 10.9 / 10.9 ms).
3. **Biggest CPU item in every terrain view: `OcclusionCuller::refine`**, 6-12 ms of a 9-15 ms CPU
   frame, nested in `Metal::EncodeSSAO`, every frame (its cache is per frame only,
   `MetalRenderer::CullCameraObjects`, `m_frameStamp`). It rasterizes up to 48 occluders of up to
   16384 triangles on the main thread, and Catania's terrain chunks all qualify. It saves GPU: off,
   GPU 10.9 → 18.8 ms. Its cost follows the view, not the motion, and it dates from 15.09. (not in the window).
4. The many `Metal::NextDrawable` stalls of head-fly2 (sum 2.1 s vs 0.2 s) all fall in frames 0-90
   of the capture, a lock-screen/hidden-window artifact, not motion.
5. The human's own log of the installed app (`/Applications/HorizonEditor.app`, built 13.10., vsync on)
   shows 40-60 FPS while flying with W on 15.10. and 60 FPS on 17.10. (FPS from the frame numbers of the
   `Edit-nav diagnostic` lines). No editor built from 1c1fd193 exists on this Mac.

## 4. FPS from an editor log

The editor logs `Edit-nav diagnostic` every 60 frames while the right button or W/S is held, so
frame number and timestamp of consecutive lines give the FPS of a real flight, W=1 vs only turning:

```sh
grep 'Edit-nav diagnostic' HorizonEngine.log | python3 -c "
import sys,re
prev=None
for l in sys.stdin:
    m=re.match(r'\[(\d+):(\d+):([\d.]+)\].*\[f\s+(\d+)\].*W=(\d)',l)
    if not m: continue
    t=int(m[1])*3600+int(m[2])*60+float(m[3]); f=int(m[4])
    if prev and 0<t-prev[0]<5: print(m[0][:13], 'W='+m[5], round((f-prev[1])/(t-prev[0]),1), 'fps')
    prev=(t,f)"
```

## Handoff

- **Topic / step:** 221, step 1 (localise, no fix)
- **Branch / commit:** claude/editor-viewport-fps-brechen-bei-kamerabewegung-von-120-auf-3 @ cf4227a2 + this doc
- **Done:** harness for moving-vs-still captures; 14 captures on Catania; window narrowed to six merges; the still frame proven unchanged by them; steady-state CPU hot path named (`OcclusionCuller::refine`).
- **Files touched:** `src/HE_Editor/EditorCamera.cpp` (SPIN/FLY), `src/HE_Core/src/Application/Application.cpp` (KEY_HOLD/KEY_RMB)
- **Verified:** every number above (raw dumps under `/tmp/he221/raw`, not kept); WASD path reached per `Edit-nav diagnostic`.
- **Not verified:** the drop itself. Not reproduced: no visible window (screen locked), no 120 Hz display, no editor from 1c1fd193 on this Mac, trackpad-mode fly toggle not injectable.
- **Decisions taken:** no bisect: with no slower frame at HEAD it cannot tell six merges apart.
- **Pending:** the human's real data (requested via the queen: Profiler Overview CPU/GPU ms still vs flying, an fn+F9 dump while flying, the 1c1fd193 `HorizonEngine.log`, the same flight in the 16.10. build).
- **Traps:** PointerInput Auto = trackpad on a laptop (see §2); compare scopes, not FPS, under a locked screen; `HE_HIDDEN_WINDOW=1` runs at 3200x1800 here.
- **Next steps:** 1. With the human's log: FPS per `Edit-nav diagnostic` line, W=1 vs W=0 (script in §4). 2. With a dump: the scope tree of the slow frames names the path; then build that merge's parent with cf4227a2 cherry-picked and A/B with `low-rmbW` settings. 3. If the 16.10. build drops too, the window is wrong: widen to 2500a502..70925e74.
