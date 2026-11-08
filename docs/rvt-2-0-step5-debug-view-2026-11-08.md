# RVT 2.0 step 5: the debug map as a viewport view mode (Thema 335)

View ▸ View Mode (mac menu) and the viewport's View Mode popup have a row "Virtual Texture" (`HE::ViewMode::RvtDebug` = 7,
after the G-buffer views; existing values unchanged, `kViewModeCount` 8, `HE_DUMP_VIEWMODE=rvt` or `7`). The scene is drawn
unlit and a landscape read from pages shows its page map. No pass: the lookup branches on a flag.

## How it works
* Flag: `heLight.viewMode.y` (the vec4 already in the material light block; `.x` is Unlit). Metal: `FillMaterialLighting`
  AND the G-buffer pass's own minimal block (`MetalRenderer.mm`, after `FillMaterialWater`; **this was the trap**: the deferred
  G-buffer pass does not call FillMaterialLighting, the flag stayed 0 there and the picture was only "unlit"). GL: `fillMatLight`.
  `UnlitViewActive()` includes RvtDebug so page colours reach the screen unlit; `RvtDebugViewActive()` is the new question.
* Shader: `heRvtSample` in `RvtLookup.h`, one branch after the nearest cell is known (colours from the table entry and the
  page-local coordinate BEFORE the filter clamp, so borders are lines of the page): hue = mip of the shown page (green finest),
  dark diagonal stripes = `eff > mip` (a coarser page stands in), pale checker = stale page, white line = border (1.5 px),
  black = no page at any mip (`0xFFFFFFFF`). The text is shared GLSL, so MSL/HLSL/SPIR-V get it by cross-compile.
* Stale: not in the table before. `rvt::kPageTableStale` = bit 30 (mip byte's bit 6), set only by
  `RvtManager::pageTable(volume, mip, markStale=true)`. Every backend's table upload keeps the mip byte (`eff << 24`), so
  it passes through; the lookup masks `(e >> 24) & 0x3F`. `RvtLandscape::setDebugView(on)` (fed from the renderer's view mode in
  `EditorApplication` before the tick) asks for the marks; `RvtManager::dirtyVersion()` (new) is folded into the version the
  driver compares, so an invalidation uploads the table again, and only while the view is on. Off: tables byte-identical to before.
* Page-level dirty flag only; per-layer stale flags (queen's step 6 item) untouched.
* `rvtDebugInfo` stays as the snapshot source (no Project Settings window exists any more; nothing re-added).
* The five `M_AutoLandscape*.hasset` are re-baked (`landscape_tex_gen <dir> --material [--group G]`, `--mix`) because the
  lookup text is stored in them (test "baked GLSL ... is what the codegen makes today"). Only the lookup text differs.

## Verified (Metal, this Mac, release)
* he_tests green: `test_editorcamera` (new mode cases), `test_rvt_core` (new: stale bit only on request, follows dirty flag,
  `dirtyVersion`), `test_rvt_shader`, `test_rvt_material_split`, `test_rvt_auto_landscape`, `test_material_graph`, `test_hpak`,
  `test_landscape_groups`. `editor_help_audit.py --check`: 1258/1258.
* Witness (new part C2 of `HE_DUMP_RVTDRIVER`): `HE_DUMP_AUTOLAND=1 HE_DUMP_RVTDRIVER=1 HE_RVT=1 HE_DUMP_RHI=Metal HE_HIDDEN_WINDOW=1
  HE_DUMP_PATH=x.bmp HE_DUMP_QUIT=1 HorizonEditor` writes `x.bmp.rvt_drv_debug_clean.bmp` / `..._stale.bmp`: clean = all mip-0 green
  with white page borders; after a sculpt stroke with a 1-page budget 5 pages stale (4 mip-0 cells in the grid) and the stroke's pages
  show the checker; picture difference clean vs stale 13.9/255 mean (before the G-buffer fix: 0.02).

## Not verified
* D3D11/D3D12/Vulkan: not filled there (they never set `viewMode` at all, Unlit is also absent), so the row selects a mode that
  draws unlit-less/normal. OpenGL: `fillMatLight` sets y, but no GL run here (no display). Forward path on Metal not looked at
  (the witness ran the default deferred path).
* Hot/cold heat map option of the step text: not built (colour by mip only).
* The right part of the witness frame (outside the 128 m volume) stays grey: no lookup there.

## Handoff
Next: step 6 (export/runtime) as in the step 4 handoff (per-layer stale flags, big-scene profiling). If a backend gets the Unlit /
view mode fill later, add `viewMode[1]` at ITS G-buffer pass block too. Trap: a flag read by the material shader must be set in
the G-buffer pass's own light block, not just in the shared fill.
