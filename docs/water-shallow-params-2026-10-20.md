# Water parameter slots: Surface + ShallowWater (Thema 206 step 6)

All 16 parameter slots of the water material were full (`kMatMaxParams`). The four plain floats
FresnelPower, Reflection, Roughness, Specular each used only `.x` of a whole slot. They are now one Vec4
`Surface` (x = FresnelPower 1..10, y = Reflection 0..1, z = Roughness 0..1, w = Specular 0..1), which frees
three slots; a new Vec4 `ShallowWater` takes one. Net: 14 of 16 slots used, 2 free.

`ShallowWater` makes the constants of step 4 knobs (defaults unchanged, so the picture is unchanged):

| comp | name | default | meaning |
|---|---|---|---|
| x | clear depth (m) | 0.8 | from this depth of water on the Opacity floor is whole (was `kShallowClearDepth`) |
| y | tint start | 0.5 | shore closeness `e` (0 = 1 m of water or more, 1 = none) where the depth tint has fully taken over the real depth; 0.5 = half a metre (was `kShallowTintE`). It is in `e`, not metres, because the graph's smoothstep reads `e`; a metre value would need a node more, i.e. a different graph |
| z | opacity floor | 0.25 | share of Opacity left at no depth (was `kShallowOpacity`) |
| w | reserved | 0 | no effect, held back for a later shallow-water term |

Only the two shallow smoothstep edges and the Lerp floor now read the Vec4's components; the maths is untouched.
Like all shore terms they act only on a landscape's water mesh (a plane has no `e`).

## Handoff

- **Done:** `MatGen/main.cpp` (Surface at row 6, ShallowWater at row 7, `kWaterParams`), `Water.hasset` regenerated
  (mat_gen twice, cmp equal) and copied to `EditorDeps/EngineContent/Materials` and `out/deploy/Editor/EngineContent/Materials`;
  `tests/test_engine_materials.cpp` `kWaterKnobs`; `EditorHelp.cpp` `Material/Surface`, `Material/ShallowWater`
  (old `FresnelPower`/`Reflection`/`Specular` entries removed, `Roughness` stays: it is the generic PBR key);
  `EditorDeps/Docs/he-docs.json` Engine Water table; `EditorApplication.cpp` comment of `HE_DUMP_WATERPARAMS`.
- **Verified (Metal + GL, `scripts/water_baseline_shots.py`, views open, open-hi, far, shallow, shallow-hi, shore, before vs after):**
  11 of 12 md5 identical. GL `shore` differs, but that view is not reproducible even with the same asset
  (`--repeat 2` gave two different md5s), so it is noise, not this change. Knob check: with `ClearDepth` 0.2 built in as the
  default (temporary mat_gen edit, reverted, deploy restored) `shallow` and `shallow-hi` differ on both backends and the
  shallows are visibly clearer (the ground shows through further out). `he_tests` engine_materials 11/11, water* 228/228,
  `editor_help_audit.py --check` ok. mat_gen: 14 params.
- **Not verified:** D3D11/D3D12/Vulkan (step 5); the editor was not rebuilt, so the new tooltips/Details rows were not seen in
  the GUI (only by test and audit); underside views (`underwater_look_shots.py`) not re-run: the underside branch does not read
  the changed constants, but it reads `Surface` components (same values). Editing `ShallowWater` live in the Inspector not clicked.
- **Migration of saved overrides: none written.** `MaterialComponent::paramOverrides` is saved by NAME in scenes
  (`SceneSerializer.cpp:346/1292`) and merged by name in `RenderExtractor.cpp:385-396` and the render path. A scene that
  overrides `FresnelPower`/`Reflection`/`Roughness`/`Specular` on the water loads fine, the entry just matches no slot and is
  ignored (the entity falls back to the default for that value). Material instances / `horizon.setMaterialParam` calls with the old
  names behave the same. Folding old names into `Surface` needs the material default for the other three components, i.e. a
  shim at the extractor's merge sites; decide if a released scene can contain them (Water came with 0.7.0, not yet published).
- **Decisions taken:** `y` of ShallowWater stays in shore closeness `e` (see table). ShallowWater at row 7 (rows 16+ of column 0
  hold the graph inputs), group `Surface`.
- **Traps:** `HE_DUMP_WATERPARAMS` only works for `WATERTEST=floor`, not the lake views, and old names only warn. Use
  `Surface=x,y,z,w`. For a knob picture on the shore views, change the default in mat_gen temporarily and copy only to the deploy.
- **Next steps:** step 5 acceptance on D3D11/D3D12/Vulkan with the new `Water.hasset` (the slot numbering changed: heParams.v index of
  Surface/ShallowWater differs from the old floats, the generated HLSL/GLSL takes it from the graph, nothing hand-kept).
