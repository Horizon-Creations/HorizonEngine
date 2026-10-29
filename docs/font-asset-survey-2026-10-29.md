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

## Handoff
**Verified (by reading):** every file:line above (re-grepped `UIFontCache::`, `getFont(`, `UITextComponent`, `emitUITextGlyphs`, `fontAtlasKey`). The brief's `ContentManager.cpp:2256` is the Font *save*, the getter is `:2662`.
**Not verified:** nothing compiled or run; D3D11/D3D12/Vulkan/Metal/GL/Software accept non-zero keys by code reading only; font rename-retarget; "no world-space text" is grep-based.
**Decisions taken:** none beyond the survey. Suggested: step 2 copies the widget path (path string + transient key), no new route.
**Traps for step 2:** (a) `keyFor` caches by (id, px, scripts) and never re-reads bytes (`UIFont.cpp:366-387`): a re-imported font with the same UUID keeps the old bake until restart. (b) The thumbnail bakes at 64 px, widgets at `FontAsset::size` (default 48, clamped 8..120): different atlases, so budget D3D12's 16 slots. (c) `UIElement` already re-measures on key/pointer change (`UIElement.cpp:1425-1433`); `UITextComponent` has no measure cache, so nothing to add there. (d) A runtime-registered Font needs `path` set or `loadAsset(path)` returns `UUID{}` (memory `asset-pointer-lifetime`, UI-texture lesson). (e) `getFont` pointers die at the next `loadAsset`: copy `size`/bytes into `keyFor` immediately, as `WidgetManager` does.
**Do not touch in step 2:** `UIFont.cpp` bake/key logic, any backend `*Renderer.*`, `EditorApplication.cpp:1207-1234` (editor font), `HpakWriter.cpp`/`ProjectExporter.cpp` (no export change needed), `UIWidgetTree.cpp` serialization of `font`.
**Next steps:** 1) step 2 as in gaps 1-2, test via a `FontAsset` built like `test_hpak.cpp:567` + `emitUITextGlyphs` output `fontAtlasKey != 0`; 2) step 3 serializer round trip incl. old form; 3) step 4 full `he_tests`.
