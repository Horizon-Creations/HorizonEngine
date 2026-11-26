# Reflections against the tile landscape (Thema 326, Schritt 16)

Step 10's handoff (`docs/landscape-world-scale-gi-2026-11-02.md` §1/§3) found that GI reflections
have no path to the tile landscape at all on any backend, and concluded a BVH/BLAS of the drawn
patches would be needed to fix it — "a new, big function". That conclusion holds for the BVH
*itself* (the patches genuinely aren't instances there), but not for reflections: the height-field
pyramid the GI *shadow* kernels already march (`GiHeightfield.h`, `HE_GI_HEIGHTFIELD`) can answer
more than "blocked or not" with a small, additive extension. This step builds that extension and
proves it on OpenGL.

## 1. Current state

**BVH/BLAS reflections** (all five backends, `GiLandscape.h`'s design comment, lines 12–49): a
reflection ray hits a BVH triangle, the kernel recovers `(hitPos, hitNormal)` from the triangle and
the ray, then — only for landscape instances (`giInsts[hitInst].offsets.z` indexes `GiLandscape[]`)
— reconstructs the painted UV from the hit position (`worldToLocal` is affine, UV is linear in it)
and blends the weightmap's layers, or for an auto-material rebuilds the slope/height masks
(`giAutoLandscapeAlbedo`, mirrored by `giAutoLandAlbedo` in `OpenGLRenderer.cpp:2552` and the Metal/
D3D/software copies). **This never fires for the tile landscape**: `LandscapeRuntime` draws tile
patches through `SetLandscapeProvider`, not `RenderWorld::objects`, so no BVH instance — and no
`GiLandscape` entry — exists for it on any backend (confirmed again here: grep `m_giInstancesCpu`
in `OpenGLRenderer.cpp` only ever receives `RenderWorld::objects`/`RenderObject`, never a landscape
provider callback).

**GI shadow height-field march** (GL/Vulkan/D3D11, `GiHeightfield.h` + `kGiHeightfieldGLSL` in
`OpenGLRenderer.cpp:2021-2107`): marches a conservative-max mip pyramid of the height field and
returns a **bool** — `hfAnyHit(o, d, tMin, tMax)`. Since step 4 (`docs/landscape-world-scale-gi-
2026-11-02.md`), `RenderExtractor::extractLandscapeGiHeightfield` (`RenderExtractor.cpp:639-660`)
builds this field from `HE::LandscapeGi`'s windowed clipmap (n×n resident tiles around the camera,
`LandscapeGi.h`) instead of a single `TerrainComponent`, tagged with the sentinel
`GiHeightfield::key == kLandscapeGiHeightfieldKey` so the extractor never emits two heightfields at
once. This is **already the tile landscape's only GPU-visible representation for ray casting** —
reflections just never read it.

**The corrected finding**: `hfAnyHit`'s march (`OpenGLRenderer.cpp:2077-2102`) advances `t`
monotonically from `tMin` and returns `true` at the loop iteration where it first detects the ray
crossing the bilinear-interpolated surface (`y < hfBilinear(tp) - 0.02`). That first crossing *is*
the closest hit along the ray — the function already walks past it, computes it, and then throws
the position away by returning a bare bool. A height field cannot give a hit on an *overhang* (two
crossings at the same `(x,z)`), but a landscape height field has none by construction, so that is
not a limitation here. Step 10's "needs BVH/BLAS" conclusion was about making the tile *patches*
traceable in general; for reflections specifically the existing march already does the hard part.

## 2. Proposed path: closest-hit march + central-difference normal

Split the existing loop into a shared `hfMarch(oL, dL, tMin, tMax, out t, out tp)` that both the
old `hfAnyHit` and a new `hfClosestHit` call — `hfAnyHit` becomes `return hfMarch(...)` (bit-
identical behaviour, no duplicate loop per the "no duplicates" rule). `hfClosestHit` additionally
takes the hit texel coordinate `tp` the march already has and:

* **Position**: not returned at all. Because the clipmap's `worldToLocal` is a pure translation
  (`RenderExtractor.cpp:651-657`, "a pure translation to the window's centre"), `t` is the same
  ray-parameter in world and local space (direction and arc length are translation-invariant), so
  the caller computes the **world** hit position as `origin + Rs * outT` directly — no inverse
  transform needed. A rotated/scaled height field (not a case that exists today) would need this
  revisited.
* **Normal**: central differences of `hfBilinear` one texel either side of the hit in x and z,
  scaled from texel-space to local-space derivatives by `texPerLocal` (already computed in the
  march): `dHdx = (h(tp+1,0) - h(tp-1,0)) * 0.5 * texPerLocal.x`, same for z, then
  `normalize(vec3(-dHdx, 1, -dHdz))`. Two extra `hfBilinear` samples per axis (4 total), each a
  2×2 texel fetch already resident in the mip pyramid's level 0 — cheap, no new texture. Since
  `worldToLocal` is translation-only, this local-space normal **is** the world-space normal
  directly (no inverse-transpose needed, unlike a general transform).

Pseudocode of the new entry point (`kGiHeightfieldGLSL`, shared by the shadow and reflection
programs):

```
bool hfClosestHit(o, d, tMin, tMax, out outT, out outNormal):
    if not enabled: return false
    oL, dL = worldToLocal * (o, d)
    if not hfMarch(oL, dL, tMin, tMax, outT, tp): return false
    dHdx = (hfBilinear(tp+e_x) - hfBilinear(tp-e_x)) * 0.5 * texPerLocal.x
    dHdz = (hfBilinear(tp+e_z) - hfBilinear(tp-e_z)) * 0.5 * texPerLocal.y
    outNormal = normalize(vec3(-dHdx, 1, -dHdz))
    return true
```

**In the reflection kernel** (`kGiReflCS`, `OpenGLRenderer.cpp:2668` `main()`): trace the BVH as
today (`giSceneClosestHitTri`), then run `hfClosestHit` capped to the BVH distance (or the max ray
distance on a BVH miss) so the two candidates compete and the nearer one wins — this is what makes
the landscape *additive*: a mesh standing in front of the terrain still occludes it correctly. On a
landscape hit, skip the BVH's per-triangle `giHitNormal`/instance-albedo/paint-table lookup entirely
(there is no `hitInst`) and feed `(hitPos, hitN, albedo)` into the *same* shading block the BVH path
already uses (sun + local lights + DDGI probe sample + ambient floor) — no new shading code, only a
new source of the three inputs it already consumes.

## 3. Integration: what LandscapeStore / GiLandscape need to export

* **Nothing new from `LandscapeStore`** for this step. The height clipmap the kernel marches
  already comes from `HE::LandscapeGi` (itself reading `LandscapeStore::tile()`), and the kernel
  needs no other query — position and normal both come out of the march. The assignment's
  suggested `LandscapeStore::landscapeHeightAtWorldXZ` query has **no caller** once the kernel
  reads the GPU-resident clipmap directly, so it is not added (an unused function the project rule
  explicitly forbids) — noted here as the place to add it *if* a future CPU-side consumer
  (an editor gizmo, a script query) needs a synchronous height+normal lookup outside the renderer.
* **`GiHeightfield`** (`RenderWorld.h:18-25`) needed one addition, already made as part of this
  step: a shared, named sentinel `kLandscapeGiHeightfieldKey` (was a private `constexpr` inside
  `RenderExtractor.cpp`) so a second reader — the reflection kernel's CPU-side binding code — can
  tell "this is the tile landscape's clipmap" from "this is a single `TerrainComponent`'s shadow-
  only field" without a second copy of the magic number.
* **`GiLandscape`**: unchanged. Full paint/auto-material support for the tile landscape (§5) would
  add an entry here, built from a *weight* clipmap (not built yet — see §5) the same way
  `LandscapeGi` builds the height one, or from `LandscapeWorldComponent::textureGroup`'s
  `AutoLandscapeGroup` array. Out of scope for this step (see §5): the prototype uses a flat
  placeholder colour instead, exactly like a plain, unpainted, non-auto `TerrainComponent` does
  today (`giAutoLandAlbedo`'s "`HE_GIREFL_AUTOLAND=0` = the old flat colour" fallback).

## 4. Per-backend

| Backend | Shares the ray-march kernel? | Why |
|---|---|---|
| OpenGL | Yes — built this step | Compute shader, `kGiHeightfieldGLSL` already shared source between the shadow and (now) reflection programs. |
| Vulkan | Yes, not built this step | `VulkanRenderer.cpp` already has its own `ensureGiHeightfield`/height-field shadow march (same algorithm, GLSL→SPIR-V via the same compiler path per `docs/vulkan-spv-deploy-was-post-build.md`'s lesson that shaders need their own deploy check); the reflection kernel there needs the identical `hfMarch`/`hfClosestHit` pair ported, and the same BVH-vs-heightfield nearest-wins gate. |
| D3D11 | Yes, not built this step | `D3D11RendererImpl::ensureGiHeightfield` (`D3D11Renderer.cpp:3404`) is the same mip-pyramid march in HLSL; `D3D_Shared/HlslSources.h`'s `kGiReflCS` would need the same addition D3D11 and D3D12 both compile from, but D3D12 (next row) cannot use it as-is. |
| D3D12 | No | Per the step-10 finding (`landscape-world-scale-gi-2026-11-02.md` §1), D3D12's GI (shadows *and* reflections) runs exclusively over DXR BLAS instances — it has no height-field march at all (`BuildGiHeightPyramid`/`ensureGiHeightfield` grep: zero hits in `D3D12Renderer.cpp`). Giving it this path means building the march in HLSL for its compute fallback *or* adding the tile patches as DXR BLAS geometry — a materially bigger task than porting an existing march, and explicitly out of scope here. |
| Metal | No | Same shape as D3D12: Metal's GI (`MetalRenderer.mm`) has no height-field shadow march either (`BuildGiHeightPyramid` grep: zero hits) — it is BVH/MPS-ray-intersector-only. The hand-kept MSL mirror convention (`kGIReflMSL`) means this would also need its own port, not a shared source file. |

So: **GL, Vulkan, D3D11 can share one ray-march algorithm** (the one extended here) because they
already share it for shadows. **Metal and D3D12 need their closest-hit path built from nothing**
(no existing march to extend) — equivalent in spirit to "BVH/BLAS of the patches", which was step
10's original (correct, for *those two backends*) conclusion.

## 5. What this does NOT handle

* **Only OpenGL is wired this step** (Limits: "no second backend yet"). Vulkan and D3D11 get the
  identical algorithm later — same `hfMarch`/`hfClosestHit` shape, different shader language.
* **Metal and D3D12 reflections against the tile landscape stay unimplemented.** Per §4 they need
  their own design (most likely: add the resident tile patches as BLAS/DXR geometry after all,
  since there is no march to extend) — tracked as its own follow-up, not scoped here.
* **No painted weights or auto-material in the shipped prototype.** The march gives a geometric hit
  (position + normal); turning that into the *painted* colour needs either (a) a weight clipmap
  parallel to `LandscapeGi`'s height one (same window, same job, same hysteresis — a direct
  extension, not a redesign) feeding a `GiLandscape` weightmap entry, or (b) resolving
  `LandscapeWorldComponent::textureGroup` to its `AutoLandscapeGroup` array and calling
  `giAutoLandscapeSliceMeans` the way `extractMeshes` does for a `TerrainComponent`'s auto material
  (`RenderExtractor.cpp:384-404`) — except keyed off `textureGroup`, not a `MaterialAsset`, since
  `LandscapeWorldComponent` (`Components/LandscapeWorldComponent.h`) carries no material reference
  at all. Both are real but separate pieces of work; the prototype shades the hit with a flat
  placeholder albedo (documented in the code as a `shortcut:`) through the existing sun/local-light/
  DDGI/ambient block, which is enough to prove the geometry and wiring are correct.
* **No distance limit or LOD tuning beyond what the march already has.** The clipmap only covers
  the `n×n`-tile window around the camera (`LandscapeGi.h`, ~2 km at the default tile size); a
  reflection ray that outruns the window before `uReflParams.x` (max ray distance) simply misses,
  same as today's shadow rays do at the window edge.
* **No self-shadowing of the landscape's own reflection hit.** The reflection kernel's sun/local-
  light occlusion rays (`giSceneAnyHit`, `OpenGLRenderer.cpp:2783/2807`) only test the BVH, not the
  height field — a reflected hillside will not self-shadow correctly from its own terrain. The
  *shadow* kernel's `GI_OCCLUDED` macro already combines both (`OpenGLRenderer.cpp:2106`); giving
  the reflection kernel's occlusion rays the same combination is a small, separate follow-up (it
  reuses `hfAnyHit`, already shared source) once the baseline hit path above is verified.
* **No performance measurement.** One extra march per reflection ray (plus 4 extra bilinear taps
  for the normal) was not profiled against the shadow-only baseline; "not measured" per the
  project's measurement rule.
* **Hit position accuracy inherits the shadow march's own coarseness.** `hfMarch`'s finest step is
  half a *texel* along the ray's horizontal projection (`0.5 / texLen`), so for a mostly-vertical
  ray (the common reflection case: a shallow mirror looking down at the ground) that step covers
  several world units of `t` — the doctest added this step needed a tolerance of ~3 units on `t`
  to agree with a dense, texel-fine reference march, confirmed by varying the ray's horizontal
  speed in `tests/test_gi_bvh.cpp` and watching the gap shrink accordingly. `hfAnyHit` has this
  exact same coarseness for shadows today (acceptable there — occlusion only needs a bool); for
  reflections it means the hit position (and therefore the normal and the sun-occlusion rays off
  it) can land up to roughly that step's distance past the true surface. Not a correctness bug,
  not fixed here — a quality-tier pass that interpolates the last step (binary search between the
  last "above" and first "below" sample, the way the step 10 handoff left for later tiers anyway)
  is the natural place to tighten it.

## 6. What was built this step

* `src/HE_Rendering/include/HorizonRendering/RenderWorld.h` — `kLandscapeGiHeightfieldKey` lifted
  out of `RenderExtractor.cpp` into a shared, named constant next to `struct GiHeightfield`.
* `src/HE_Rendering/src/RenderExtractor.cpp` — uses the shared constant instead of its own copy.
* ~~`GiHeightfield.h` — `GiHeightPyramidClosestHitRef` (CPU copy of `hfClosestHit`) + 3 doctests~~ —
  removed in step 24: test-only, already diverged from the GLSL (clamped negative cells), so it
  tested itself, not the shader. The shader is covered by the reflection check in
  `landscape_gpu_check` (step 23).
* `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp`:
  - `kGiHeightfieldGLSL`: `hfAnyHit`'s loop factored into `hfMarch`; new `hfClosestHit` built on it.
  - The reflection compute program now links `kGiHeightfieldGLSL` too (was shadow-only).
  - `RenderGIReflection` binds `uHf`/`uHfWorldToLocal`/`uHfParams`/`uHfParams2` (texture unit 10,
    free in that program), gated on `m_giHfKey == kLandscapeGiHeightfieldKey` so a scene with a
    single `TerrainComponent` (already a BVH instance, already shaded by the paint-table path)
    never double-hits.
  - `kGiReflCS`'s `main()`: traces the height field capped to the BVH's hit distance (or the max
    ray distance on a BVH miss), takes whichever candidate is closer, and on a landscape hit feeds
    the existing shading block a flat placeholder albedo instead of the BVH's paint lookup.

## 7. Verified / not verified

* **Verified**: `he_tests` (existing suite, unchanged) stays green (the CPU-reference doctests
  originally added here were removed in step 24, see section 6). `HorizonEditor` and `he_tests` build with the new GLSL
  source compiled into both programs's link step (a GLSL syntax error surfaces at
  `linkCompute`'s link-status check, at program start-up, not at `cmake --build` time — see
  `docs/verify-runtime-compiled-shaders.md`'s standing lesson that these strings are never checked
  by the build).
* **Verified on real hardware (step 23, NN-WS03, RTX 4070)**: `scripts/validate_embedded_shaders.py
  --glsl-only` compiles `header + kGiTraversalGLSL + kGiHeightfieldGLSL + kGiReflCS` with
  glslangValidator exactly as `OpenGLRenderer.cpp:6721` assembles it (it already had the right job —
  `GLSL_PRELUDE_OF["kGiReflCS"]` — from this step's own change; nobody had RUN it before now). Closed
  the gap this section used to describe as not-worth-the-budget: `landscape_gpu_check`
  (`EngineCheck.cpp`, `runReflectionCheck`, GL-only) builds a tile landscape via `LandscapeRuntime`
  + `HorizonWorld::setLandscapeRuntime` (every other check in that file calls
  `SetLandscapeProvider` but never that — the heightfield extractor has nothing to feed without it),
  a mirror cube (metallic 1, roughness 0.05) standing on it, and a directional light (its absence
  was the harness's own bug: `kUnlitFS`'s `uLightCount == 0` branch — OpenGLRenderer.cpp:537-543 —
  returns a flat unlit colour *before* the code that samples `uGIRefl`, so a scene with no light
  "passes" silently with the reflection path never exercised; bisected by forcing `kGiReflCS`'s
  `main()` to write solid magenta unconditionally and watching it not appear on screen). With the
  light in place the real log line fires (`OpenGLRenderer: GI pipelines built`) and the heightfield
  uploads (`GI height field 1025x1025, 9 mips`). The mirror is *not* green: a heightfield hit still
  returns the flat placeholder albedo `vec3(0.3, 0.32, 0.26)` this section's §5/§6 already flagged
  (no painted weights yet).
* **Step 26 correction (gate run 2575, NN-WS03, RTX 4070)**: the check this section used to describe
  — toggle `GIReflectionSettings::enabled` and require the mirror's pixel to move — was not actually
  evidence of a landscape hit. `env.skyEnabled` defaults true, so `RenderSkyReflCube` bakes a real
  sky cube every frame regardless of the setting, and `kGiReflCS`'s miss branch samples that cube
  with full confidence on *any* miss (`OpenGLRenderer.cpp`'s `if (hitInst < 0 && !hfHit)` block). A
  permanently broken `hfClosestHit` (every ray missing the heightfield) still took that branch and
  still moved the pixel when reflections toggled off, so the old control could not tell "the march
  hit the landscape" from "every ray missed and fell back to the baked sky" — it was never a valid
  geometry-only argument, contrary to what this section previously claimed. The control is now:
  reflections stay ON in both renders; the second render instead calls
  `world.setLandscapeRuntime(nullptr)` (`SetLandscapeProvider` stays, so the ramp still renders —
  only `extractLandscapeGiHeightfield` now has nothing to feed, `RenderExtractor.cpp:804`), which
  drives `uHfParams.w` to 0 and makes `hfClosestHit` return false unconditionally
  (`OpenGLRenderer.cpp:2126`) — the same state a permanently broken march would be in. Measured on
  hardware: with the heightfield march intact, the landscape-on render is rgb(192,196,186) and the
  no-heightfield (sky-only) render is rgb(151,148,141), worst channel delta 48/255 — **PASS**. Forcing
  the march off with the existing `HE_GI_HEIGHTFIELD=0` test hook (`OpenGLRenderer.cpp:6480`, written
  for `scripts/he_gi_hf_check.py`'s own negative control) reproduces exactly the bug this section used
  to miss: both renders come back rgb(151,148,141), delta 0/255, and the check correctly **FAILS**
  (`the mirror shows the landscape's heightfield, not just the baked sky`). This is the proof the old
  control lacked — a broken march can no longer pass by hitting the sky instead.
* **Still not verified**: the landscape's own paint showing through the reflection (the §5/§6 gap
  itself — unchanged by this step, tracked there) end-to-end on D3D11/D3D12/Vulkan/Metal (this
  step was GL-only, matching the gate it closes); and `he_tests` still has no headless GL context
  for GI, so this remains a `landscape_gpu_check` run, not an automated test — rerun it
  (`landscape_gpu_check --only gl`) after any change near `kGiReflCS`/`EnsureGiHeightfield`/
  `extractLandscapeGiHeightfield`.
