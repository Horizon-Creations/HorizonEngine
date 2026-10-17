# Material-Graph: Displacement-Pin mit GPU-Tessellation — Umsetzung (Schritt 2)

Thema 207, Schritt 2 von 4. Entwurf: `docs/material-graph-tessellation-displacement-entwurf-2026-10-15.md`.
Dieser Schritt baut Pin, Codegen und alle Shaderstufen; verdrahtet ist nur **OpenGL**
(auf dem Mac prüfbar). D3D11/D3D12/Vulkan bekommen ihre Stufen schon aus der Bibliothek,
binden sie aber noch nicht; dort gilt bis dahin der Fallback (= Pin unverbunden).

## Was steht

- **Graph:** Output-Pin 9 „Displacement" (Float, Meter entlang der Normale), angehängt, kein
  Versions-Bump. `MatTessSettings` an `MatGraphNode` (nur am Output), JSON-Objekt `"tess"`
  (`max/near/far/maxDisp`), nur geschrieben, wenn ungleich Standard. Scope `"ds"` (`dsStage`):
  Texture/Array-Sample → `textureLod(…, 0.0)`, Bombing mit Gradient 0, Normal-Map-Knoten und
  Screen Position → neutrale Werte. Die Deklarationen der Stufe kommen aus derselben
  `emitDeclarations` wie die des Fragments (ohne `hePerturbNormal`/Backdrop).
- **Format** `MatShaderGen::displacementBody` = `// heTess <max> <near> <far> <maxDisp>` +
  Deklarationen + `// heDispBody` + Anweisungen bis `float heDisp`. `matSplitDisplacement`
  zerlegt, `matSanitizeTess` klemmt. Unverbunden: leer, alles andere byte-gleich.
- **Asset:** `MaterialAsset::customShaderDispGlsl`, letztes MTRL-Tail-Feld (hinter dem
  Approx-Block, den der Packer neu faltet und danach verbatim weiterreicht). 15 Kopierstellen
  neben `customShaderVertGlsl = gen.vertexBody`.
- **he::shaderc:** `Stage::TessControl/TessEval`; HLSL-Ziel mit Tess-Stufe → klarer Fehler;
  `compileHlslKernel` (Vertex-Stufe mit umbenanntem Entry → `<name>_inner()`).
- **MaterialShaderLibrary:** `tessControl`/`tessEval` (`MaterialShaderLibrary.cpp:1169`,
  `:1192`), ein Domain-Assembler mit zwei Prologen (`assembleDomain` `:1022`), D3D-Schalen
  (`hlslHullShell` `:1101`, Register wie das Material-Fragment: b0/b1/b3, t2/t4–t7, t14/s0).
  `resolveShaders` ist unverändert (Thema 214 baut daran).
- **OpenGL:** `m_tessSupported` (GL 4.0+, `HE_TESSELLATION=0` schaltet ab, `:3390`);
  `foldDisplacement` faltet den Body nur dann in den Schlüssel (`:3625`); vierstufiges Programm
  in `GetOrBuildMaterialProgram` (`linkWithTess` `:3988`, bei Fehler zweistufig neu);
  `MaterialPrimitive()` an fünf Zeichenstellen (Vorschau, G-Buffer, Forward, Deferred-Replay,
  Transluzent).
- **OcclusionCuller:** Displacement-Materialien sind keine Verdecker (wie WPO).

## Prüfung

| Was | Ergebnis |
|---|---|
| `he_tests --source-file='*test_material_tessellation.cpp'` | 11 Fälle, 230 Assertions grün |
| material_graph, mcp_tools_material, export_profiles, engine_materials, asset_ref_retarget, gltf_material_import, sky_shader | 241 Fälle grün |
| voller `he_tests`-Lauf (Release) | 5346/5347 grün; rot nur `test_hpak.cpp:1186` (`6 == 2`): zählt die 4 Assets im lokalen EngineContentCache mit, geräteabhängig, nicht dieser Zweig |
| CGL-Probe: macOS-GL 4.1 (M5) tesselliert, `ccw` erhält CCW-Dreiecke, `cw` verwirft sie | ja |
| `scripts/tess_gl_harness.c`: erzeugtes 4-Stufen-Programm linkt + zeichnet auf dem Apple-Treiber | ja |
| HLSL-Schalen durch `glslangValidator -D` (HSMain/tesc, DSMain/tese), Negativkontrolle schlägt fehl | ja (1 Warnung: Attribut am HS-Entry) |
| GL-Dump `MATERIALTEST=displace`: unverbunden 2× | 0 px Unterschied |
| … `HE_TESSELLATION=0`, Höhe 0.4 gegen unverbunden (Fallback-Vertrag) | **0 px**, bitgleich |
| … Höhe 0.4 gegen unverbunden | 162 442 px, Silhouette verformt (`docs/images/material-tessellation-gl-height04.png`) |
| … Höhe 0, maxFactor 1 gegen unverbunden | **43 px, max 3/255 — nicht bitgleich** |
| … Höhe 0, maxFactor 16 gegen unverbunden | 906 px, max 15/255 |
| Metal-Dump: Höhe 0.4 gegen unverbunden (Fallback-Backend) | **0 px**, bitgleich |
| GL deferred (`RENDERPATH=1`): Höhe 0.4 gegen unverbunden (G-Buffer-Programm mit 4 Stufen) | 170 592 px, verformt |

Die Entwurfsbehauptung „Höhe 0 = bitgleich" hält auf Apple-GL nicht ganz: schon ohne neue
Vertices unterscheiden sich 43 Kantenpixel. Vermutung (nicht geprüft): der tessellierte Weg
läuft im GL-auf-Metal-Treiber über eine andere Positionsberechnung. Der Fallback-Vertrag
(Backend ohne Tessellation = unverbundener Pin) ist davon nicht betroffen und bitgleich (GL mit
`HE_TESSELLATION=0` und Metal). Entwurf §3.2 ist entsprechend zu lesen: pro Vertex exakt, die
Rasterung darf auf Apple-GL an einigen Kantenpixeln abweichen.

## Handoff

- **Topic / step:** 207, Schritt 2 von 4
- **Branch / commit:** `claude/material-graph-shader-tessellation-displacement-als-pin-am-o` @ 6a36dd61, d5a0cc0a + Doku-Commit (gepusht)
- **Done:** Pin 9 mit Tess-Einstellungen, Codegen Scope „ds", Asset-Tail, Tess-Stufen für GLSL/SPIR-V/HLSL, OpenGL zeichnet Displacement-Materialien tesselliert.
- **Files touched:** `MaterialGraph.cpp:573` (dsStage), `:1575` (emitDeclarations), `:1949` (Emission), `:2078` (Split); `MaterialShaderLibrary.cpp:1022–1215`; `OpenGLRenderer.cpp:3390/3625/3988` + 5 Draws; `ShaderCompiler.cpp`; `ContentManager.cpp` (MTRL-Tail); `tests/test_material_tessellation.cpp`; `scripts/tess_gl_harness.c`; Editor-Zeuge `EditorApplication.cpp` (`MATERIALTEST=displace`).
- **Verified:** siehe Tabelle oben (Release-Build im Worktree, he_tests, CGL, GL-Dumps).
- **Not verified:** D3D11/D3D12/Vulkan zur Laufzeit (nicht verdrahtet); HLSL nur per glslang, kein FXC; was ein ALTER Build mit einem Link auf Pin 9 macht (Entwurf §7, Collab zwischen Versionen); Windows/Linux nur per CI.
- **Decisions taken:** Settings als Kopfzeile im Body (ein Asset-Feld, keine 4 Floats); Stufen binden die Fragment-Bindings (eine Tabelle, D3D11 kann PS-Slots spiegeln); vNormal bleibt unnormalisiert (Fragment normalisiert); Schlüssel wird im Backend gefaltet, nicht in `resolveShaders`; Pak-Varianten bekommen keine Tess-Stufen, GL kompiliert sie zur Laufzeit nach (ohne shaderc = Fallback).
- **Decisions suggested, not taken:** Bounds-Aufblähen fürs Frustum-Culling um `maxDisplacement` (WPO macht es auch nicht); Tess-Stufen im PSHD-Bake.
- **Pending:** Vulkan/D3D11/D3D12-Verdrahtung; Schritt 3 (Editor-UI, Hinweis „wirkt nur auf…", `matGlslUsesTime` für den Body in `MaterialEditorPanel.cpp:2448`); Schritt 4 (Abnahme).
- **Traps:** `OpenGLRenderer::foldDisplacement` hasht den Body pro Draw und Material: wenn der Resolve-Cache aus Thema 214 (REN-101) kommt, gehört die Faltung in diesen Cache, sonst geht sie beim Merge verloren oder kostet doppelt. Vulkan-Default-Domain-Origin ist UPPER_LEFT → `VkPipelineTessellationDomainOriginStateCreateInfo{LOWER_LEFT}` einhängen, sonst dreht `ccw`. D3D11 hat kein `DSSet*` bisher; der DS liest b0/b1/b3, t2/t4–t7/t14 und s0/s2/s4–s7. D3D12: Root-Parameter mit `VISIBILITY_VERTEX` → `ALL`, PSO-Topologie PATCH. Vulkan: Feature `tessellationShader` aktivieren, Set-Layout-Stages erweitern. Header-Edits während eines laufenden Builds: betroffene `.o` danach löschen.
- **Next steps:** 1. Vulkan: `tessControl/tessEval(…, SpirV)` in den Material-Pipelinebau (`VulkanRenderer.cpp` ~3175), Feature + Domain-Origin + `PATCH_LIST`. 2. D3D11/D3D12 mit `kHlslHullEntry`/`kHlslDomainEntry` (`hs_5_0`/`ds_5_0`). 3. Auf NN-WS03 sichtprüfen (`MATERIALTEST=displace`, `RHI=D3D11|D3D12|Vulkan`).
