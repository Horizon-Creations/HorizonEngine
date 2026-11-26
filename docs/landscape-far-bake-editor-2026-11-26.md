# Landscape: camera per project, far landscape baked in the editor, RVT radii (2026-11-26)

The owner's request (26.11.):

1. A landscape made in a new project showed neither the create grid nor the landscape.
2. Raise or drop the resolution cap, so a 100 × 100 km landscape gets detail.
3. RVT as sharp as possible near the camera, without overflowing VRAM:
   - baked HLODs far away;
   - RVT near;
   - coarser variants in between.
4. HLODs for the landscape and the meshes on it bakeable in the editor, automatically or by a panel, chosen in the settings.

## Handoff

**Done (release/0.8.0)**

- **Bug 1 has three causes.**
  - `378f72fe4`, `e0cef661f`: the editor camera was one global view (`config.json`). After Catania (100 km) it stood 8.7 km from the new project's origin. Now:
    - Each project has its own view, `Saved/EditorView.json` (`EditorApplication::save/loadProjectCameraView`).
    - A project without one is framed on its largest landscape, or gets the first-run view.
    - Create Landscape frames the new landscape (`EditorCamera::frameArea`).
    - The create form frames its grid once when the camera is more than four grid sizes away.
  - The brush hit the infinite base plane. It now hits only the landscape plus one brush radius (`TerrainTools::sculptInViewport`).
  - `6a04b0979`: File ▸ New Project over an open project loaded into the old world. The dialog now ends the session first, and `onProjectLoaded` clears the world.
- **RVT radii.** `RvtLandscape::nativeNearRadius(texel)`, `nearRadiusM` and `band0` follow the volume's clamped texel (`desc().finestMetersPerTexel`), not the setting.
  - With the 1.6 cm preset, a 100 km landscape got 0.5 m pages from about 15 m out (the blurry ring).
  - Now the native material reaches about 470 m (1080p, 60°).
- **Far landscape in the editor.** New `src/HE_Editor/LandscapeFarBake.{h,cpp}`.
  - Reuses the export's `bakeWorldLandscapes` on the live world.
  - Tiles are written to `<project>/Saved/FarLandscape` (index.json with a key per landscape), loaded with `loadHlodAssets`, and the chunks are marked `dirty`.
  - The key covers the heights, three weight pages, material, size, resolution and transform. A changed landscape gets its tiles unloaded, and its far chunks draw the live material again.
  - The setting is `EditorConfig::LandscapeFarBake` (0 Off, 1 Manual (default), 2 Automatic), a Preferences row.
  - The panel is Landscape ▸ Setup ▸ Far Landscape: a state line and a "Bake Far Landscape" button.
  - Automatic bakes after 4 s without a change, without a mouse button held and outside play. A state that baked nothing is not retried.

**Verified**

- The help audit is green.
- Full `ctest` (see the commit).
- Headless runs: a private `HE_CONFIG_DIR`, a copy of the user's config and of nikas test, `HE_EXIT_AFTER_FRAMES`.
  - Camera: without its own view → framed on the landscape. HE_DUMP_LIVE showed it framed whole. With its own view, that view wins.
  - Automatic: after about 4 s, 16 chunks were baked in 0.6 s, 64 files written, and the chunks rebuilt as "16 with a baked far material".
  - Second start: 64 saved tiles loaded, no new bake.
- The witness needs the real `EngineContentCache` (HOME). A private HOME downloads 290 MB first.

**Not verified**

- The Create Landscape click itself (`frameArea` is the same function that is verified).
- The stale path during real sculpting.
- The panel with a screenshot.
- RVT on a real 100 km landscape: the radius fix is not measured, there is no witness.
- Windows / D3D / Vulkan.

**Not delivered, and why**

- **HLODs for meshes on the landscape.** No merged proxy per cell or region exists. It is a new system, not an extension.
- **Resolution cap.** At 4097² the old single landscape costs:
  - 4096 chunks × 4 LOD meshes;
  - about 67 MB of heights inline in the scene and in every autosave;
  - the Jolt/TerrainSync single field.

  It would still be 24 m per sample at 100 km. Real detail at 100 km is the tile world of PR 214 (`claude/landscape-weltmassstab-100km`, plan `docs/landscape-one-system-plan-2026-11-21.md` on that branch), which replaces this system.
- **Sharp RVT on 100 km.** It needs camera-centred fine volumes on top of the coarse one: the 2048-page table, the 11-bit key and the 32 volumes block one fine volume.
- **Automatic bake on a worker.** Baking renders, so it blocks the main thread for the bake (0.6 s for 16 chunks). Only encoding could move to the JobSystem.

**Next steps**

1. The owner decides on PR 214 (merge into 0.8.0? tile size for small worlds?).
2. Camera-centred fine RVT volumes, about 64–128 pages a side per level, within the existing pool (check against `VolumeLayout` first).
3. Proxy HLODs for meshes per streaming cell.
4. Time-slice the automatic bake per chunk (`HlodBaker::bake(cx, cz, …)`), plus its own content key instead of the whole landscape.
