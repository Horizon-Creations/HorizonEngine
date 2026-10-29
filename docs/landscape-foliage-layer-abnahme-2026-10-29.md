# LandscapeFoliageLayer: backend acceptance (Thema 327, step 5), 2026-10-29

Branch `claude/neuer-asset-typ-landscapefoliagelayer-foliage-spawn-pro-land` @ `9e12afa2`
(+ this doc). NN-WS03 (Windows, MSVC 18.0/14.51, Ninja, build tree `C:/hw327`, **Debug**).

Steps 1-4 of this topic touch no shader and no renderer code: step 1 is a design doc, step 2
is an asset type (CPU-side `.hasset`/Hpak read/write), step 3 is a metadata-only Material-Graph
node (no pins, shader text unchanged when wired or not — `859e2eb4`'s own verification), step 4
materializes an asset's entries into ordinary, tagged `FoliageLayer`s that go through the
unchanged `FoliageSystem`/Thema 163/182 scatter-cluster-LOD-pooling/render path. So this step is
not hunting for a visual regression from new rendering code — there is none — it confirms that
nothing in steps 1-4 broke what each backend already does with a `FoliageLayer`, across all five
render backends, and that the Apply-on-click design carries no per-frame cost.

## Verdict per backend

| Backend | Verdict | How |
|---|---|---|
| D3D11 | **verified** (build, visual); tests 3675/3678, **one open** (see below) | full build, `he_tests`, `HE_DUMP_FOLIAGETEST` witness |
| D3D12 | **verified** (build, visual); same open test item | same; pixel-identical to D3D11 (0 px differ) |
| Vulkan | **verified** (build, visual); same open test item | same; 19/921600 px differ (known sky-edge run-to-run noise, see `foliage_graph_ab.py`'s docstring) |
| OpenGL | **verified** (build, visual); same open test item | same; shading-only difference from D3D, traced to source (see Findings) |
| Metal | **verified (witness only), step 6, see "Metal addendum" below** | `HE_DUMP_FOLIAGETEST` on MacBook-Air-von-Connor (Apple M5): identical counters, near-identical to OpenGL's image. Not covered on Metal: `he_tests` and the full test run (Mac tests were not part of step 6). The original Windows step asked for this via `hive_request` #157. CI matrix for the Windows HEAD: run **40852164598** — `https://github.com/Horizon-Creations/HorizonEngine/actions/runs/40852164598` |

## Results

* **Build:** `HorizonEditor` and `he_tests` up to date at HEAD (`9e12afa2`), rc 0, `C:/hw327/deploy/Editor`.
  D3D11/D3D12/Vulkan/OpenGL are all compiled into the one Windows binary and selected at runtime
  (`HE_DUMP_RHI`), so one build exercises all four.
* **Tests:** full `he_tests` (scratch `APPDATA`, Debug): **3675/3678 passed** (838009/838012
  assertions). `test_terrain_tools_ui.cpp` (step 4's own cases) and
  `test_landscape_foliage_layer.cpp`/`test_landscape_foliage_sync.cpp`/
  `test_landscape_foliage_node.cpp` (steps 2/3) all pass inside the full run.
  `scripts/editor_help_audit.py --check`: **1245/1245** entries covered. The 3 failures, re-run
  in isolation via `ctest -R <name> --output-on-failure` (clean, no other build running):
  * `test_inspector_ui.cpp` — **passes alone**, order-dependent (matches `vulkan-d3d-baseline-noise.md`).
  * `test_physics_threading.cpp` — **crashes alone too** (`SEH exception`), the known pre-existing
    Debug-only physics crash from the same memory entry.
  * `test_hc_codegen_export_build.cpp` — **fails alone too, deterministically** (3 repeated runs,
    `REQUIRE( vi.size() == vc.size() )`, `6 == 0`: the compiled HorizonCode class ran but left no
    variables where the interpreter left 6; `out.ok` was true and no error line was captured, so
    the build itself succeeded). **Not closed as noise**: the last CI run before step 4
    (`40844751464`, commit `859e2eb4`) ran this exact test on this same self-hosted device and it
    **passed** (18.06 s, Windows job log). Neither step 4's diff (`FoliageComponent.h`,
    `TerrainTools.cpp`, `SceneSerializer.cpp`) nor steps 1-3's touch anything this test exercises
    (Transform/EngineWorld fixture, `pos`/`scale`/`name`/`spawned`/`found`), and a stale local
    build tree (`C:/hw327` has been rebuilt across 1714/1715/1721/1722) is a plausible alternative
    explanation that CI's fresh checkout rules out either way. **Open**: wait for CI run
    `40852164598` (current HEAD, same self-hosted Windows runner) to resolve this — if it passes
    there, this was local-tree staleness; if it fails there too, it is a real regression and needs
    its own look before this topic's branch is considered backend-accepted.
* **Visual comparison across backends:** `HE_DUMP_FOLIAGETEST=2000` (the existing Thema 163
  cross-backend foliage witness) on D3D11/D3D12/Vulkan/OpenGL, same camera/field/shadow pins. Note
  on scope: this witness builds its `FoliageLayer` directly — it does **not** go through
  `LandscapeFoliageLayerAsset` → `applyFoliageSet()` (step 4's own chain is editor-click-driven and
  not scriptable from a headless dump, and `applyFoliageSet` writes the exact same `FoliageLayer`
  struct, per its own test in `test_terrain_tools_ui.cpp`, so there is nothing backend-specific
  left for a render dump to exercise beyond this). What it shows: the `FoliageLayer`
  render/extract/cull/batch path that the new asset's entries end up flowing through still works
  the same on all four backends after steps 1-4's header/data-model/editor changes — not a
  regression check of the new chain itself, which steps 2-4 already verified at the data level.
  All four report the **identical** `dump counters — draws=5 tris=45352 visible=8/20 occluded=0`.
  Pixel diff against D3D11: D3D12 0/921600 px (byte-identical BMPs), Vulkan 19/921600 px >10 (known
  sky-edge run-to-run noise, see `foliage_graph_ab.py`'s docstring), OpenGL 302500/921600 px with
  mean diff 9.8/255 — same silhouette and draw/tri counts, a shading-only difference traced to
  source, not this branch (see Findings). Evidence:
  `docs/landscape-foliage-layer-abnahme/d3d11-foliagetest.png`,
  `docs/landscape-foliage-layer-abnahme/opengl-foliagetest.png`.
* **Performance budget:** `applyFoliageSet()` (`src/HE_Editor/TerrainTools.cpp:307`, called only
  from the "Apply Foliage" button's `if (EditorWidgets::button(...))` block at line 1614-1618) runs
  exactly once per click, never per frame. The passive pull of a not-yet-resident set
  (`loadAssetAsync`) is gated by a `static std::unordered_set<HE::UUID> s_asked`, so it also fires
  at most once per set, not every frame the Setup tab is open. None of this code runs in
  `HorizonGame` (editor-only, `TerrainTools.cpp`). Once applied, a tagged layer is an ordinary
  `FoliageLayer`, written field-for-field by `applyFoliageSet` (verified by its own test), and so
  costs exactly what a hand-added layer of the same density costs through the unchanged
  `FoliageSystem`. No new per-frame work, no backend-specific cost — this is a property of the
  design (steps 1 and 4), not something a render dump can additionally confirm.

## Findings (not fixed, no regression in this step's scope)

1. **OpenGL vs D3D/Vulkan shading tone** on the plain-cube foliage witness: same cube silhouette,
   same draw/tri counts, but roughly a third of pixels differ by up to 80/255 (mean 9.8) — darker
   side faces are more visible on GL. Confirmed by source, not just inference, that this predates
   the branch: `git diff 9c1b7f5b..HEAD --stat -- src/HE_Rendering src/HE_Scene/src/FoliageSystem.cpp
   src/HE_Rendering/src/FoliageExtract.cpp` is **empty** — no rendering or foliage-extraction code
   changed anywhere in this topic, so the tone gap cannot come from steps 1-4. Not diagnosed
   further (no `C:/hw182*` editor left to shoot the same witness through for a direct before/after;
   the source diff is the evidence instead).
2. Sky is black in both witness images (`HE_DUMP_SKYTEST` was not set for this comparison, only the
   ground/plant field mattered) — expected, not a bug.

## Handoff

* **Verified:** table above. Reproduce with:
  `HE_CONFIG_DIR=<scratch> HE_HIDDEN_WINDOW=1 HE_DUMP_PATH=<out>.bmp HE_DUMP_QUIT=1
  HE_DUMP_RHI=<D3D11|D3D12|Vulkan|OpenGL> HE_DUMP_FOLIAGETEST=2000 HE_DUMP_FOLIAGESIZE=100
  HE_DUMP_CAMX=0 HE_DUMP_CAMY=25 HE_DUMP_CAMZ=60 HE_DUMP_PITCH=-20 HE_DUMP_SHADOW=0.1
  deploy/Editor/HorizonEditor.exe` (use forward slashes in every path — a `\\` immediately
  before `${VAR}` did not expand in this Git-Bash session, forward slashes side-stepped it).
* **Not verified:** Metal at runtime (no macOS device available to this run — requested from the
  queen, `hive_request` #157, a Mac bee can run the one-liner witness command above); live-editor
  drag-drop/Apply-Foliage-click smoke test with a *real* foliage mesh (placeholder meshes only,
  by design — step 4 handoff, design §0); the three engine group materials' "Apply Group Meshes"
  path (unchanged by this topic, out of scope); `test_hc_codegen_export_build.cpp` (see above —
  open, not closed as noise).
* **Traps:** `he_tests` wipes the real `%APPDATA%` config — always a scratch `APPDATA`/`HE_CONFIG_DIR`.
  A `\\` immediately before `${VAR}` did not expand in this Git-Bash session (`echo "C:\\hw327\\${RHI}.bmp"`
  printed the literal `${RHI}` unexpanded) — use forward slashes in dump paths instead, which the
  engine accepts fine on Windows.
* **Next:** (1) when CI run `40852164598` finishes, check specifically whether
  `test_hc_codegen_export_build` passes on its Windows job — that resolves whether the local
  failure above was build-tree staleness or a real regression; if it fails there too, it needs its
  own investigation before this topic's branch is backend-accepted, not a line in this report.
  (2) wait for the Mac bee's Metal witness (request #157) or the macOS CI job, whichever answers
  first. No further code changes are expected from this step otherwise — it is acceptance only.

## Metal addendum (Thema 327, step 6, 2026-10-29)

MacBook-Air-von-Connor (Apple M5, macOS), branch @ `d234ee67`, **Release**, Ninja, build tree
`<worktree>/cmake-build-release` (739 steps, rc 0, HorizonEditor only; no `he_tests` run on the Mac).
Binary: `out/deploy/Editor/HorizonEditor`. No code, shader or test changed. The log shows
`Configuration: backend=Metal` and `MetalRenderer: initialized on Apple M5`, no fallback.

Witness: the Reproduce one-liner above with `HE_DUMP_RHI=Metal` and a scratch `HE_CONFIG_DIR`
(plus `HE_COLLAB_OFFLINE=1 HE_NET_LOOPBACK_ONLY=1`), run from `out/deploy/Editor`. Same pins as the
Windows rows: `HE_DUMP_SKYTEST` not set, `HE_DUMP_SHADOW=0.1`.

| Backend | `dump counters` line (verbatim) |
|---|---|
| D3D11 / D3D12 / Vulkan / OpenGL | `draws=5 tris=45352 visible=8/20 occluded=0` (all four, from the table above) |
| **Metal** | `EditorApplication: dump counters — draws=5 tris=45352 visible=8/20 occluded=0` |

**draws, tris and visible all match.** Run-to-run: two Metal runs are byte-identical BMPs.

* **Shadow caveat checked, not assumed.** The Windows run used `HE_DUMP_SHADOW=0.1`; so did Metal. On
  Metal `HE_DUMP_SHADOW=150,3` (a real shadow range) gives the same counters **and a byte-identical BMP**
  (the default time of day has no sun up, hence the black sky), so in this witness the shadow setting cannot
  move the counters. Metal's `draws=5` is therefore not inflated by cascades.
* **Pin trap:** adding `HE_DUMP_SKYTEST=1` (as `foliage_graph_ab.py` does) changes the counters on
  Metal to `draws=5 tris=58060 visible=16/20`: the sky test adds scene geometry. Do not compare counters
  across runs with and without it. `HE_DUMP_TOD=0.4`, `HE_SKY_TIME`, AA/clouds off do not change them.
* **Image** (`docs/landscape-foliage-layer-abnahme/metal-foliagetest.png`): the cube field is visible, grey
  ground, black sky (56 black rows, same as D3D11). Against the committed D3D11 PNG: 302496/921600 px
  differ by >10, mean diff 9.77/255. Against the committed **OpenGL** PNG: 10 px >10 (max 54), mean 0.01 and mean
  luminance 174.0 vs 174.0. So Metal reproduces OpenGL's shading, not D3D's (D3D11 mean luminance 183.7).
  This agrees with Finding 1: the tone gap splits Windows D3D (brighter) from the GL-family look. Whether
  the D3D side or the GL/Metal side is "right" was not examined; the numbers only show Metal is not an outlier.
* **Not done:** no live-editor session, no real foliage mesh, no `he_tests` on the Mac, no Windows/D3D/Vulkan runs.

## Handoff (step 6)

* **Done:** Metal witness above. All three counters match the four Windows backends.
* **Pending:** nothing for Metal. Windows-only open items stay with the Windows side:
  `test_hc_codegen_export_build` and CI run `40852164598`.
* **Reproduce (Mac):** `cd out/deploy/Editor && HE_CONFIG_DIR=/tmp/x HE_HIDDEN_WINDOW=1 HE_DUMP_PATH=/tmp/m.bmp
  HE_DUMP_QUIT=1 HE_DUMP_RHI=Metal HE_DUMP_FOLIAGETEST=2000 HE_DUMP_FOLIAGESIZE=100 HE_DUMP_CAMX=0 HE_DUMP_CAMY=25
  HE_DUMP_CAMZ=60 HE_DUMP_PITCH=-20 HE_DUMP_SHADOW=0.1 ./HorizonEditor > m.log 2>&1` (redirect stdout to a file,
  the run is ~2 s). The bmp is bottom-up 24-bit; `sips -s format png` converts it.
* **Next:** optional, only if wanted: look into the D3D vs GL/Metal shading-tone gap (Finding 1).
