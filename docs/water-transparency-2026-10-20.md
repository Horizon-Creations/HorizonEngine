# Water transparency in the shallows (Thema 206, step 4)

Shallow water now lets the ground through and fades from the clear shallows into the deep tint without a
contour. Open water, the underside and everything outside the last metre before a landscape's shore are
byte-identical to before. Nothing in the mesh, the encoding or the parameters changed.

## Finding
At the defaults (`Turbidity` 0.45/m, y = 4 m assumed depth) looking straight down gives a transmittance of
0.18 and a body alpha of 0.91: the water was nearly opaque everywhere, and in the shore range the alpha
ramp of step 2 only faded that opaque sheet out over the last 0.35 m. The cloudy dark patches between deep
colour and shore (visible in `transp-before-*-shallow.png`) were the foam lace wobble of the ramp acting on
an opaque body. The material never knew the real depth except through `e` (step 2), so the tint kept
assuming 4 m even at 10 cm.

## Change (`src/HE_Tools/src/MatGen/main.cpp`, constants ~line 455, graph ~lines 840 and 960)
- `shoreE`/`shoreD` moved up to before the water body (same nodes, the Soft shore block still uses them).
- **Tint depth**: `tintD = lerp(Turbidity.y, shoreD, smoothstep(0, 0.5, e))` replaces `Turbidity.y` in the
  path length. `e = 0` (one metre of water and deeper, planes) gives `Turbidity.y` itself; the real depth has
  taken over from half a metre. Shallow water therefore tints toward `ShallowColor` and its transmission
  rises, which also brightens the caustics there (they ride on `trans`), as wanted.
- **Opacity floor**: `opacEff = Opacity * lerp(0.25, 1, smoothstep(0, 0.8, shoreD))`: whole at 0.8 m and
  deeper, a quarter of itself at no depth. Feeds `bodyA` only; Fresnel lift, foam, the step-2 ramp and the
  shore foam fringe are untouched, so the edge keeps its wobble and lace.
- Neutral by construction at `e = 0`: smoothstep 0 gives `mix(y, d, 0) = y`; `shoreD = 1` gives smoothstep 1,
  `lerp(.25, 1, 1) = 1`. No Divide/Power/Sqrt added, so nothing can go NaN in the `If` branch the underside
  also evaluates; the underside branch itself is not changed.
- No new parameter (all 16 slots full): the constants are `kShallowTintE`, `kShallowOpacity`,
  `kShallowClearDepth`. Tooltips of `Turbidity` and `Opacity` (mat_gen and `EditorHelp.cpp`) say where the
  real depth takes over.
- `scripts/water_baseline_shots.py`: new views `shallow` (CAMX 8, CAMY 306, CAMZ 20, pitch -30) and
  `shallow-hi` over a wide gentle bank (slope 0.02, 3 cm ... 1.5 cm jitter, 0.5 m of water).
- `Water.hasset` regenerated (mat_gen twice, `cmp` equal) and copied to `EditorDeps/...` and
  `out/deploy/Editor/EngineContent/Materials`.

## Handoff

- **Topic / step:** 206 Realistischer Wasser-Shader, step 4 of 5
- **Branch / commit:** claude/realistischerer-wasser-shader-wellen-normals-transparenz-ufe, see `git log -1` (pushed: yes)
- **Done:** shallows (< ~0.8 m) are see-through with a smooth blend to the shore on Metal and GL.
- **Files touched:** `src/HE_Tools/src/MatGen/main.cpp` (constants after `kChopBase`, `tintD`/`path`, `opacEff`/`bodyA`),
  `src/HE_Editor/EditorHelp.cpp` (Opacity, Turbidity text), `scripts/water_baseline_shots.py`, `Water.hasset`,
  images `docs/water-realistic-baseline/transp-{before,after}-{metal,opengl}-{shallow,shallow-hi,beach}.png`.
- **Verified:**
  - Open water md5-identical to before, both backends: `open`, `open-hi`, `far` (Metal dfcb0e3f / 0d80aa69 / 4dba81c8,
    GL e712b9d5 / 87bfc074 / c1773ee0), also after the tooltip-only regeneration.
  - Underside identical (Metal, `underwater_look_shots.py --only up,zenith,deep_up,shore,dry_shallow`, bmp md5
    before/after, fog and no-fog): up, zenith, deep_up, shore all SAME. `dry_shallow` differs only at the far
    waterline (the bowl's shore band, e > 0), as intended.
  - Looked at `transp-before-*` vs `transp-after-*` (shallow, shallow-hi, beach): the cloudy dark patches are gone, the
    deep blue falls off to clear in the last half metre, the ground's facets show through. Metal and GL look the same.
  - `beach` (3 cm ground jitter) shows thin straight lines in the shallows: they are the ground's facets seen through
    the water; `beach-smooth` after (`transp-after-metal-beach-smooth.png`) has none.
  - `he_tests --source-file='*test_engine_materials.cpp'` 11/11, `*test_water*` 228/228, `editor_help_audit.py --check` ok.
- **Not verified:** D3D11/D3D12/Vulkan (step 5); real sand/rock ground (the witness ground is flat grey, so "more
  ground" is judged on its facet shading and colour, not on a texture); motion; GPU time (about 8 extra scalar nodes per
  pixel); a 1 km landscape with terrain LOD; a rotated/scaled landscape entity (the `e` channel rides in the world
  normal, see step 2); the editor was not rebuilt (the tooltip text in `EditorHelp.cpp` is checked by he_tests and the
  audit, not seen in the editor).
- **Decisions taken:** (1) constants in the graph, not a knob: all 16 slots are full and the values only matter in the last
  metre. (2) The depth range stays 1 m (`kShoreFadeDepth`): clear enough water is a shallow-water look, and deeper than 1 m
  keeps the assumed `Turbidity.y`; the blend starts at e = 0 with zero slope so there is no visible step at 1 m. The mesh
  encoding was not touched, it is not needed. (3) The tint depth takes over from e = 0.5 (half a metre), not at once.
- **Decisions suggested, not taken:** a real scene-depth read (SceneColor/SceneDepth) would replace the guess in deep water
  too; out of scope here. Raising `kShoreFadeDepth` to ~2 m would let the clear look reach further out (needs the mesh
  gain re-derived and a loader test for old saves).
- **Pending:** step 5 (D3D11/D3D12/Vulkan acceptance, compare against refs 30/31).
- **Traps:** the `WATERPARAMS` witness only applies to `WATERTEST=floor`, not to the lake views. A saved override of
  `Turbidity` with two values gives z = w = 0 (hard edge), unchanged from step 2. `md5` is not on PATH (`/sbin/md5`).
  GL `beach` is not md5-stable run to run (a few pixels), compare it by eye.
- **Next steps:** 1. Step 5: `scripts/water_baseline_shots.py OUT --prefix after` (views open, open-hi, far, shallow,
  shallow-hi, beach) on NN-WS03 and compare with `transp-after-*` / the open md5s above. 2. If the shallows look too
  hazy with a textured ground, lower `kShallowOpacity` or raise `kShallowTintE` in `MatGen/main.cpp`, regenerate twice.
