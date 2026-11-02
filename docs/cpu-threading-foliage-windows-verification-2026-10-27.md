# CPU threading: D3D11/D3D12/Vulkan verification of the foliage pool (Thema 238, Schritt 3)

27.10.2026, NN-WS03 (Windows, AMD Ryzen 9 9950X, 16C/32T). Verifies Schritt 2's foliage pool/replay
parallelization (commit `207322f0`, bit-identical on Metal/OpenGL) on the three remaining backends.
**No engine code changed** — only two measurement scripts fixed for Windows (see §5). Build reused:
`C:/hb/cta5` (Release, Ninja/MSVC VS18 toolset, from Schritt 5), `HorizonEditor` added to it this
session. HEAD `cf70572a`; the only commits since Schritt 5's `d09e65ca` are its own three docs-only
ones, so `ninja` found nothing to rebuild for `he_tests`.

## 1. Which branch, and why

`origin/release/0.7.0` (`553a540c`) is 14 commits ahead of this topic branch. A prior post on this
topic (`mac-frame-levers`, after PR #194) said Schritt 3 should test release/0.7.0, not the bee
branch, once that merge landed — true for the state right after PR #194, but the 14 commits that
have landed on release since are **unrelated to foliage**: checked `git log --name-only` for all 14,
they touch water tessellation (`553a540c`), Steam Audio (4 commits), and water wind/caustics
(includes one touch of `RenderExtractor.cpp`/`RenderWorld.h` for water, not foliage). None touch
`FoliageExtract.{cpp,h}`, `FoliageSystem.cpp`, or `tests/test_foliage_parallel.cpp`. Verifying on the
topic branch (which already carries release's state as of PR #194, including `207322f0`) is
therefore equivalent for this step's purpose; a later step should re-check this assumption before
the final merge if more rendering-adjacent commits land on release in between.

## 2. One Windows build compiles all three backends

`src/HE_Rendering/CMakeLists.txt:218-262` compiles `RendererD3D11` and `RendererD3D12`
unconditionally under `if(WIN32)`; `RendererVulkan` (`:85-121`) compiles whenever the Vulkan SDK is
found. `C:/hb/cta5`'s `CMakeCache.txt` has `HE_VULKAN_ENABLED=TRUE` and `HE_D3D12_GI_SHADERS=TRUE` —
one build, all three backends linked into `HorizonEditor`/`HorizonGame`, selected at runtime
(`HE_DUMP_RHI` for the editor's headless dump path, `config.json`'s `CustomConfig[GameBackend]` for
the game, see lesson 563). So "build on all three" is one compile, not three.

## 3. Tests

`he_tests.exe --source-file="*test_foliage_parallel.cpp"` (scratch `APPDATA`, PowerShell-set per
lesson 567): **4/4 cases, 168026/168026 assertions, SUCCESS.** The test itself never touches a
renderer backend — `FoliageExtract`/`FoliageSystem`/`RenderExtractor` are pure data structures, so
this result already covers the logic for all three backends; building successfully against all
three backend libraries (§2) is the backend-specific half of "build/tests".

Full suite (same build): **5905/5907 cases, 1890617/1890624 assertions.** The 2 failing cases are
both in `test_inspector_ui.cpp` (clipboard), matching the known pre-existing Windows flake (hive
lesson 559/`win-*` baseline). No failure in anything foliage- or rendering-adjacent — no regression.

## 4. Pixel verification

No pre-`207322f0` deploy exists on this machine for this commit (Schritt 2's pixel A/B was Metal/
OpenGL only, run on a Mac), so `scripts/perf/foliage_pixel_ab.py`'s BEFORE/AFTER flow could not be
reused as-is. Instead: a same-binary A/B of the override switches themselves, using the
`HE_DUMP_FOLIAGETEST` witness (100k instances, same camera/pins as that script's case A) across
`HE_DUMP_RHI=D3D11/D3D12/Vulkan`, comparing the shipped default (pool + memo on) against
`HE_FOLIAGE_PARALLEL=0` and `HE_FOLIAGE_MEMO=0` — this directly answers "does turning the pool or
the replay off change a single pixel", which is what this step needs. Throwaway script (not
committed, not engine code): `C:\hb\foliage_parallel_pixel_ab.py`.

```
D3D11    default vs serial   identical
D3D11    default vs nomemo   identical
D3D12    default vs serial   identical
D3D12    default vs nomemo   identical
Vulkan   default vs serial   identical
Vulkan   default vs nomemo   identical
```

All 6 combinations bit-identical BMPs.

## 5. Performance (D3D12, scene `s512_x4`, 524285 instances across 5 layers)

Methodology follows Schritt 5's (WaitFrame nested inside Render inside frame `cpuMs`, subtract it
for WORK — hive lesson 564). Game dir and build from Schritt 5, reused (`C:/hb/game_s512x4`).

| Scenario | ExtractFoliage / whole extract p50 | Note |
|---|---|---|
| Static camera, steady state, pool (default), `HE_PROFILE_WARMUP=120 CAPTURE=250` | 0.019 ms (WORK ≈ 1.36 − 0.63 = 0.73 ms, 2.6 %) | memoized every frame — nothing straddles, `FoliageStraddlers`/`FoliageEmitClusters` absent from the dump, as expected |
| Static camera, steady state, serial fallback (`HE_FOLIAGE_PARALLEL=0`) | 0.018 ms (WORK ≈ 1.33 − 0.58 = 0.75 ms) | same cost as pool — a static camera can't show the pool's win, only confirms the switch doesn't regress the memoized path |
| Moving camera, `he_tests` bench (`--no-skip -tc="*Foliage extract bench*"`), backend-independent | one thread **0.489 ms**, pool **0.247 ms** (median of 600) | **≈2.0× wall-time win** — matches Schritt 5's same-session number (0.488/0.244 ms) and Schritt 2's Mac range (0.343-0.915 ms serial) |
| One-time scatter, frame 0, `HE_PROFILE_WARMUP=1 CAPTURE=5` | `Foliage` wrapper scope **≈21.5 ms** wall (p90/max over 5 frames) | in line with Schritt 5's 23.68 ms (same scene) and faster than Schritt 2's Mac post-pool 30.7-31.9 ms (faster CPU) — no regression |

The static-camera capture cannot show the pool's throughput win by itself (memoization skips the
straddler walk every frame once settled); the moving-camera bench is what demonstrates it, same as
on Mac. Both are reported so the table does not imply "no win" from the static-camera row alone.

## 6. Scripts fixed (within this step's scope, no engine code)

- `scripts/perf/foliage_pixel_ab.py`: `--rhi` only accepted `Metal`/`OpenGL`; the engine has always
  accepted `D3D11`/`D3D12`/`Vulkan` for `HE_DUMP_RHI` (`EditorBackendChoice.h:9`). Widened the
  `choices` list so the next Windows bee (e.g. Schritt 6) can use this script directly.
- `scripts/perf/make_land_game_dir.py`: `cp -cR` is macOS-only (BSD clone flag); three separate runs
  on this topic (Schritt 5, this step) had to patch around it locally without committing a fix.
  Replaced with `shutil.copytree`, smoke-tested against the existing `s512_x4` scene/runtime dir.

## Handoff

- **Topic / step:** Thema 238, Schritt 3 (global step id 1199).
- **Branch / commit:** `claude/cpu-threading-audit-jobsystem-auslastung-daten-vorbereitung-`, this
  doc + the two script fixes (§6), pushed.
- **Done:** D3D11/D3D12/Vulkan verification of Schritt 2's foliage pool/replay (`207322f0`): builds
  (one Windows build covers all three), `he_tests` foliage suite and full suite both green (only
  known pre-existing clipboard flake), pixel-identical across all three backends and both switches,
  performance re-confirmed (~2× extract win on a moving camera, no regression elsewhere).
- **Files touched:** `scripts/perf/foliage_pixel_ab.py:96` (`--rhi` choices),
  `scripts/perf/make_land_game_dir.py:5,17-18` (`cp -cR` → `shutil.copytree`). No engine code.
- **Verified:** `he_tests.exe --source-file="*test_foliage_parallel.cpp"` 4/4; full suite 5905/5907
  (2 known pre-existing failures); `C:\hb\foliage_parallel_pixel_ab.py` 6/6 identical across
  D3D11/D3D12/Vulkan; `HE_PROFILE_CAPTURE` dumps on D3D12 (`game_s512x4`), analyzed with
  `dump_scope_p50.py`; `he_tests --no-skip -tc="*Foliage extract bench*"` fresh this session.
- **Not verified:** the exact BEFORE/AFTER BMP flow of `foliage_pixel_ab.py` against a real
  pre-`207322f0` build (none exists on this machine for this commit; §4 explains the substitute and
  why it answers the same question). D3D11/Vulkan performance numbers (only pixel-checked, not
  profiled — the brief centers performance on D3D12).
- **Decisions taken:** tested the topic branch, not a separate release/0.7.0 worktree (§1) — the
  14-commit divergence is foliage-unrelated, checked by path, not assumed. Fixed the two scripts
  named in the brief's own "within scope if encountered" list rather than leaving local-only patches
  for the next bee to rediscover.
- **Decisions suggested, not taken:** none.
- **Pending:** nothing for this step. Schritt 6 (next real engine-code step on this topic) should
  re-run `foliage_pixel_ab.py` (now Windows-capable) if a true BEFORE deploy becomes available.
- **Traps:** a static-camera `HE_PROFILE_CAPTURE` cannot show the pool's win (§5) — use the
  `he_tests` moving-camera bench for that, or read the whole section before quoting one number.
  `C:/hb/cta5` and `C:/hb/game_s512x4` are reused build/game dirs shared across this topic's steps —
  claimed via `hive_claim(branch="*")` while in use here, released on `hive_leave`.
- **Next steps:** Schritt 6 (not this step) implements the priority list from Schritt 4/5 and repeats
  this five-backend verification cycle.
