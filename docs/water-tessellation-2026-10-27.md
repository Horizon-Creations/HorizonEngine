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

## Update: calmer waves, flat coast, per-body numbers, the underwater effect follows the surface
* Geometry is now the three knob trains as PLAIN sines (`HE::water::sampleWaves`' formula, no bombing/warp/gust),
  times `height` (default 0.6, 0..2) times a shore factor `smoothstep(0,1,depth/shoreWidth)` (default 1.8 m, 0.1..3):
  flat AT the waterline, full from `shoreWidth` of water on. Peak ~0.16 m at the default knobs (was ~0.55), x3.4 in a storm.
* Per body: `water::TessellationParams {height, shoreWidth}` (`Body::tessellation`, `setTessellation`, scene key
  `tessellation` per body, UI "Tessellated Waves" in the landscape water details). WaterSurface pushes them into the surface
  entity's material as the override of the new knob `TessWaves` (exists only in the Tessellation permutation).
* The underwater effect: `queryUnderwaterCamera(world, pos, waveClock)`; with a landscape's tessellation on, surfaceY /
  signedDepth / wet / fog tint / caustics follow `level + water::tessellatedHeight(...)` under the camera (same formula,
  same clock as the shader: SDL ticks, `HE_SKY_TIME` pins it). The post pass still treats the surface as a plane at that
  local height; it does not follow the waves per pixel.
Verified: he_tests water/underwater/engine materials; GL shots (open water gentle, beach flat). Not verified: the underwater
effect by eye (clock sync between extract and draw is within a frame), other backends, per-body UI on screen.
