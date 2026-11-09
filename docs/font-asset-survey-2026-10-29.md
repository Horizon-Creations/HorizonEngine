# Font asset survey (topic 226, step 1) - 2026-10-29

Survey only, no code. "Verified" = read in the source at the cited line; nothing here was built or run.

## What a Font asset is today
Import: `.ttf/.otf` -> `FontImporter::import` (`HE_Tools/.../FontImporter.cpp:8-52`, `ImporterCommon.cpp:617,752`, `asset_compiler/main.cpp:64,160`), `size` = bake px (default 48). Asset: `FontAsset{fontData,size}` `Assets.h:576`; load `ContentManager.cpp:534-539`, save `:2256-2261` (chunks FNTI/FNTD), getter `ContentManager.h:179`/`.cpp:2662`. Bake: `UIFontCache::keyFor(stableId, ttf, px)` `UIFont.cpp:366` (`UIFont.h:156`); key = hash(id, px, `uiFontScripts()`), 0 = failed/empty; `find(0)` = shared default (`:389`).

## Use sites
| Site | Status |
|---|---|
| Widget element `font` path -> key: `WidgetManager.cpp:244-279` (`refreshElementAssets`), called at create `:608`, graph `Font` set `:219`/`:195`, grafted rows `:897`. Field `UIElement.h:698-701`, prop `UIElement.cpp:1040-1042`, JSON `UIWidgetTree.cpp:2278`/`:2430`, copy `UIElement.h:1003`, emit `UIElement.cpp:1254-1266` (`emitTextL`) | **used** |
| Widget Designer Font slot `UIEditorPanel.cpp:2062-2066` (all elements with "FontSize") | **used** (authoring) |
| Asset thumbnail `AssetThumbnailCache.cpp:292-300` (bakes at `kBakePx`=64, widgets bake at `fa->size`: different atlas, same face) | **used** |
| Importer, asset load/save, Content Browser type filter `ContentBrowserPanel.cpp:2666,2749,2780`, `AssetStubWriter.cpp:176` (import-only, no Create entry, intended) | **used** |
| Hpak: whole Content dir packed, Font included (`test_hpak.cpp:567,652`) | **used** |
| Widget Designer canvas preview: `UIEditorPanel.cpp` draws text with `dl->AddText(nullptr,...)` (e.g. `:4016,4072,4098,4293`) and never sets `fontAtlasKey` (only `WidgetManager` does, grep `fontAtlasKey *=`); `:4006` tab layout reads it, always 0 | **default-only**: a widget with a Font previews in the editor font |
| `WidgetManager.cpp:6696` (menu bar), `:6763,:6775` (menu rows/chords), `:6852` (ComboBox open list, uses `cb->fontSize` but not `cb->fontAtlasKey`), `:6876,:6898` (tooltip), measure `:1313` (`menuTextWidth`) - all `sharedUIFont()`, key 0 | **default-only**; menu bar/tooltip are manager chrome (decide), ComboBox popup is a real gap |
| Entity UI text: `UITextComponent.h` (text, fontSize, color), `UISystem.cpp:243-247` -> `emitUITextGlyphs(text,...)` overload `UIFont.cpp:1254` = `sharedUIFont(), key 0`; serializer `SceneSerializer.cpp:1143-1148` write, `:2370-2377` read (no font); `ScriptApi.h:65-67`/`ScriptApi.cpp:164-172` text only; Inspector `InspectorPanel.cpp:3892-3900` | **missing** (the main gap) |
| World-space text: grep (`TextMesh/WorldText/3dText/DebugText/drawString`, `Components/` listing) finds no 3D text component, only the 5 `UI*Component`s | **none exists** (grep-based, not exhaustively proven) |
| Editor's own ImGui font `EditorApplication.cpp:1207-1234` (loads `Fonts/Roboto_Condensed-Bold.ttf` as a file), splash `SplashScreen.cpp:222`, `ProjectSettingsPanel.cpp:603` (info line) | out of scope, stays a file |
| Rich text `<b>` inside a Font-asset element stays in that family: `UIFont.cpp:775-783` | **used** (already handled) |
| `TutorialSteps.cpp:24` ("font" name), `:434` text "used by UI widget text and the in-game UI" | no change needed; sentence is true only for widgets until entity UI is wired (optional wording) |

## Export / dependency scan (verified, answers the open question)
* `HpakWriter::addDirectories` (`HpakWriter.cpp:1030-1140`) packs **every** `.hasset` in the Content dir (only `excludePatterns` skip). No reachability pruning, so a Font ships whether or not anything references it; `packedPaths()` goes into the path index (`ProjectExporter.cpp:684-700`).
* `rewriteRefsForPack` (`HpakWriter.cpp:349-368`) touches only mesh/material/scene/particles. Widgets (`UIWT`) pass through verbatim, so `"font"` stays a path string and resolves at runtime via `loadAsset(path)` + the path index, exactly like widget `texture`. `EngineDependencies.cpp:79` Font = payload-only, nothing to follow.
* Rename/delete scans (`AssetRefScan.cpp:31`, `AssetRefRetarget.cpp:34`) treat `UIWT` as a JSON chunk of path strings, so a renamed Font should retarget widget `font`; **not tested** for fonts.
* A `UITextComponent` font stored as a **path** needs no export change; stored as a **UUID** in `.hescene` is rewritten/closed by the scene path (`collectAssetRefs`, `SceneSerializer` UUID form). That choice is step 3.

## Backend table: do they accept non-zero keys? (read, not run; no D3D/Vulkan HW on this Mac)
| Backend | Lookup | Key != 0 | Unknown/failed key |
|---|---|---|---|
| OpenGL | `OpenGLRenderer.cpp:8222-8226`, bind `:8446-8450` | lazy R8 upload | key-0 atlas (wrong UVs, no crash) |
| Metal | `MetalRenderer.mm:13762-13766`, bind `:14002-14008` | lazy R8 upload | key-0 atlas |
| Vulkan | `VulkanRenderer.cpp:16555-16571`, bind `:16513` | lazy set + one-shot upload | key-0 set |
| D3D11 | `D3D11Renderer.cpp:2118-2122`, bind `:6769-6773` | lazy SRV | 1x1 white dummy (solid boxes) |
| D3D12 | `D3D12Renderer.cpp:4228-4233`, `:5197`, `:5253` | slot in heap, **max 16 atlases** (`:4190`) incl. default/bold/icon | `defaultSlot` (wrong UVs) |
| Software | `SoftwareRaster.cpp:266-269` | `UIFontCache::find` | shared font |
All six read `UIRenderObject::fontAtlasKey` (`UIRenderObject.h:75`). Tests: rasterisation only with key 0 (`test_software_raster.cpp:243`); emit-side tests use engine keys (bold/icon `test_ui_font_weight.cpp:110-112`, `test_ui_widgets.cpp:2546`, key 7 `test_ui_text_layout_digest.cpp:145`); nothing sets `UIElement::font` or bakes an imported `FontAsset` through `keyFor`.

## Gaps and the wiring each needs (in order)
1. **Entity UI text has no font** (`UITextComponent`, `UISystem.cpp:243`). Step 2: add a font path + transient key to the component, resolve like `WidgetManager.cpp:268-279` (`loadAsset(path)` -> `getFont` -> `keyFor(hi^lo, fontData, size>0?size:48)`), emit with the `emitUITextGlyphs(font, key, ...)` overload `UIFont.h:361-367` (as `UIElement.cpp:1254-1266`). Inspector: `InspectorPanel.cpp:3892-3900` (reuse `assetSlot`-style control from `UIEditorPanel`). Script setter: `ScriptApi.h:65-67`; a new registry row needs the "three (four) places" (memory `engine-api-row-three-places`).
2. **Where the key is resolved is the design question for step 2**: `UISystem::extract(world, vp, out)` (called `RenderExtractor.cpp:1776`) has no `ContentManager`; `HorizonWorld` holds none (`world.widgets()` is a `WidgetManager`, `HorizonWorld.h:224`, whose `m_content` `WidgetManager.h:1316` is private, set only inside widget create/load calls `.cpp:465,661,778`, no public accessor); `RenderExtractor` has `m_contentManager` (`.h:340`, set via `setContentManager`). Options: resolve in the extractor, pass a resolver into `extract`, or add a public accessor (the touch point if chosen). Key must be re-resolved on scene load and play start (it is transient, like `fontAtlasKey`).
3. **Designer preview** ignores the Font asset (see table). Optional follow-up, not required for the game; ImGui can't draw our atlas without a custom draw path.
4. **ComboBox popup** `WidgetManager.cpp:6852` and its measure ignore `cb->fontAtlasKey`; small fix, same file as step 2 only if wanted.
5. Step 3: serialization (`SceneSerializer.cpp:1143` / `:2370`, old files must still load: absent key -> empty font), no export change needed (above).

## Handoff (step 1, survey; superseded by the final Handoff below)
**Verified (by reading):** every file:line above (re-grepped `UIFontCache::`, `getFont(`, `UITextComponent`, `emitUITextGlyphs`, `fontAtlasKey`). The brief's `ContentManager.cpp:2256` is the Font *save*, the getter is `:2662`.
**Not verified:** nothing compiled or run; D3D11/D3D12/Vulkan/Metal/GL/Software accept non-zero keys by code reading only; font rename-retarget; "no world-space text" is grep-based.
**Decisions taken:** none beyond the survey. Suggested: step 2 copies the widget path (path string + transient key), no new route.
**Traps for step 2:** (a) `keyFor` caches by (id, px, scripts) and never re-reads bytes (`UIFont.cpp:366-387`): a re-imported font with the same UUID keeps the old bake until restart. (b) The thumbnail bakes at 64 px, widgets at `FontAsset::size` (default 48, clamped 8..120): different atlases, so budget D3D12's 16 slots. (c) `UIElement` already re-measures on key/pointer change (`UIElement.cpp:1425-1433`); `UITextComponent` has no measure cache, so nothing to add there. (d) A runtime-registered Font needs `path` set or `loadAsset(path)` returns `UUID{}` (memory `asset-pointer-lifetime`, UI-texture lesson). (e) `getFont` pointers die at the next `loadAsset`: copy `size`/bytes into `keyFor` immediately, as `WidgetManager` does.
**Do not touch in step 2:** `UIFont.cpp` bake/key logic, any backend `*Renderer.*`, `EditorApplication.cpp:1207-1234` (editor font), `HpakWriter.cpp`/`ProjectExporter.cpp` (no export change needed), `UIWidgetTree.cpp` serialization of `font`.
**Next steps:** 1) step 2 as in gaps 1-2, test via a `FontAsset` built like `test_hpak.cpp:567` + `emitUITextGlyphs` output `fontAtlasKey != 0`; 2) step 3 serializer round trip incl. old form; 3) step 4 full `he_tests`.

## Handoff

- **Topic / step:** 226 (Font asset for the game), step 4 of 4 (verification); steps 1-3 are done
- **Branch / commit:** `claude/font-asset-typ-f-rs-spiel-import-und-nutzung-in-widgets-und-` @ a0ed92aa5 + this doc commit (pushed: see the step result)
- **Done:** `UITextComponent` (entity UI text) now carries a Font asset path. `UISystem::extract` resolves it to a baked atlas key every extract and draws with that atlas; empty path = shared default font as before. Inspector has a Font picker, scenes save/load the path. Nothing in this step changed code.
- **Files touched (steps 2-3):**
  - `src/HE_Scene/include/HorizonScene/Components/UITextComponent.h:12-15` — `font` (serialized) and `fontAtlasKey` (transient)
  - `src/HE_Scene/src/UISystem.cpp:146,244-274` — `contentManager` param, `loadAsset` -> `getFont` -> `UIFontCache::keyFor(hi^lo, bytes, size>0?size:48)`, falls back to the key-0 emit overload when no face is found
  - `src/HE_Scene/include/HorizonScene/UISystem.h` — `extract(..., ContentManager* = nullptr)`
  - `src/HE_Rendering/src/RenderExtractor.cpp:1776` — passes `m_contentManager`
  - `src/HE_Scene/src/SceneSerializer.cpp:1153` (write, only when set), `:2381` (read, absent = empty); no version bump
  - `src/HE_Editor/InspectorPanel.cpp` (hunks near 397 and 3955) and `EditorHelp.cpp:1474` ("UI Text/Font")
  - tests: `tests/test_ui.cpp:223`, `tests/test_scene_serializer.cpp:2928`
- **Verified (Mac, Release, `cmake-build-release`):**
  - `cmake --build ... --target he_tests` rc=0 but nothing recompiled (objects were already current). So warnings were checked separately: `-fsyntax-only -Wall -Wextra` with the targets' own flags.make flags on `UISystem.cpp`, `SceneSerializer.cpp`, `RenderExtractor.cpp`, `InspectorPanel.cpp`, `test_ui.cpp`, `test_scene_serializer.cpp`: all rc 0, no warning in a line this topic added. Old warnings remain: `test_scene_serializer.cpp:1494,1500` (unused structured bindings), `InspectorPanel.cpp:2687,2689` (misleading indentation), `:3303` (null char in literal). The `HorizonEditor` target itself was not built or linked here.
  - Full `he_tests` run from the build dir with `HOME=/tmp/<empty>`: **5940 test cases, 5940 passed, 0 failed, 19 skipped** (1887901 assertions). The skips are the env-gated live tests (`HE_TEST_TEAM_SERVER`, `HE_TEST_RELAY`, ...).
  - Targeted: `test_ui` 21/21, `test_scene_serializer` 47/47, `test_ui_widgets` 456/456, `test_ui_font_weight` 3/3, `test_ui_font_scripts` 1/1; `test_hpak` 59/59 with the clean `HOME`.
  - `python3 scripts/editor_help_audit.py --check`: exit 0, 1243/1243 controls covered, 2059 help entries.
- **First full run (real `$HOME`) had 1 failure, NOT caused by this topic:** `test_hpak.cpp:1188` (`scanContentDirectory() == 2`, got 10). Cause: `scanContentDirectory` (`ContentManager.cpp:3433`) also scans `GlobalState::engineContentCacheDir()` = `~/Library/Application Support/HorizonEngine/EngineContentCache`, which holds this Mac's SFTP-synced EngineContent (Audio, Meshes, Textures, manifest.json). With an empty `HOME` the case passes (9/9 asserts) and so does the whole suite. The topic touches nothing in `src/HE_Core`. I did not build `main` for a baseline: the main checkout is on release/0.8.0 with an October binary, and the cause above is shown directly.
- **Not verified:** D3D11/D3D12/Vulkan/Metal/GL rendering of a non-zero entity-text key (backends read it by code only, table above); no screenshot of the Inspector picker or of entity text in a non-default font; Windows run of the full suite (the old five Windows failures, e.g. `test_physics_threading` SEH, were not re-run here and did not appear on this Mac).
- **Decisions taken:** treated the `scanContentDirectory` failure as environmental, proved it by the `HOME` rerun and did not change test or code; renamed the step-1 handoff heading so this is the one `## Handoff`.
- **Decisions suggested, not taken:** make the `test_hpak` case hermetic (point the cache dir at the temp dir, or assert `>= 2` on the two ids it authored) so it does not depend on a developer's synced EngineContent; any Mac that has ever synced content fails it.
- **Pending:**
  1. Designer preview ignores the Font asset (gap 3). 2. ComboBox popup and its measure ignore `cb->fontAtlasKey` (gap 4, `WidgetManager.cpp:6852`). 3. No script setter for the text font (`ScriptApi.h:65-67`; a registry row needs the "three (four) places"). 4. D3D12 has 16 atlas slots, each distinct (font, px) entity text costs one.
- **Traps:** run `he_tests` from the build dir (CWD-relative paths); a real `$HOME` with synced EngineContent turns `test_hpak` scanContentDirectory red; `keyFor` never re-reads bytes, so a re-imported font with the same UUID keeps the old bake until restart; background task output is truncated, redirect to a file.
- **Next steps:** 1. Merge by the queen. 2. Run the full suite once on NN-WS03 (Windows) for the old five. 3. Look at entity text with a non-default font on D3D12/Vulkan. 4. Optionally the hermetic `test_hpak` fix above.
