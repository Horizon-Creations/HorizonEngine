# Water: the swell as real geometry (optional tessellation)

2026-10-27, `release/0.7.0`. The engine already had GPU tessellation behind the Output node's Displacement
pin (Thema 207, `docs/material-graph-tessellation-*`). The water now uses it, behind a switch.

* **Material** (`MatGen/main.cpp`, `waterMaterial`): the six wave trains again as height
  (`height * length / 2pi * gust/fade weight`, summed) feed a **Static Switch "Tessellation"** (default OFF) on the
  Displacement pin; tess settings: factor 8 up to 40 m, none from 160 m, `maxDisplacement` 2.5 m (room for a storm sea).
  `Wave` got a `length` pin. The chop layers stay normal-only (finer than the mesh resolves).
* **Codegen** (`MaterialGraph.cpp`, `connectedThroughSwitches`): the Displacement pin follows Static Switches to
  their TAKEN input; a switch that takes an unconnected input is "no link": no tessellation text, every other
  output byte-identical (test: `with.glsl == off.glsl`).
* **Setting**: `water::Field::tessellation` (landscape-wide, like the shore clip; scene key `waterTess` only when
  on; part of `sameContent`/`pristine`), checkbox "Tessellation" in the Spline Tool's lake section (+ help).
* **Runtime** (`WaterSurface.cpp`, `waterMaterialFor`): when on, the surfaces use a Material Instance of the water
  with the switch on, registered in memory under `kEngineWaterTessellatedMaterialId` (never a file). If it cannot
  be made (no graph / no shader compiler: a packed game) it falls back to the plain water once, with a warning.
* `Water.hasset` regenerated with `mat_gen` (+ copied to `out/deploy/Editor`). `HE_DUMP_WATERTESS=1`,
  `scripts/water_baseline_shots.py --views swell,swell-hi` for the shots.

## Handoff
Verified: he_tests (water, engine materials incl. the switch/permutation, serialization, help audit) green; OpenGL on
macOS, lake low over the water, tess off (flat horizon) vs on (undulating silhouette, wave-shaped surface):
`scratchpad/tess/sw{0,1}-opengl-swell-hi.png`. NOT verified: Vulkan/D3D11/D3D12 (same pipeline as the displacement
demo, not run with the water), cost, cracks/seams between water bodies, tessellated water with the wind sea at
storm (maxDisplacement clamp), terrain/shore interplay of a displaced sheet, the Spline Tool checkbox on screen,
the packed-game fallback. **METAL draws the surface flat**: the engine has no Metal tessellation (see the
displacement docs); the switch does nothing there. Next: Metal tessellation (its own task); GPU timing on
Windows; maybe a per-body switch.
