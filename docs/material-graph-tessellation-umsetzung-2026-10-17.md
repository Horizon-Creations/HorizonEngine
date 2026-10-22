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

## Schritt 9: Instanz-Zwilling ohne Tessellation (GL)

Review-Fund: `GetOrBuildMaterialProgram` suchte den Displacement-Body unter dem **ungesalzten** Schlüssel, der instanzierte Zwilling (`progKey = key ^ 0x1D57…`) bekam also TCS/TES, landete in `m_tessPrograms`, wurde aber von `DrawGraphBatchInstanced` mit `GL_TRIANGLES` gezeichnet (`GL_INVALID_OPERATION`, Batch unsichtbar).

- **Fix** (`OpenGLRenderer.cpp:3809`, direkt nach dem Cache-Lookup): `instanced && m_tessSupported && m_tessDispByKey.count(key)` → Info-Log, `m_materialPrograms[progKey] = 0`, `return 0`. Der Batch läuft über die Pro-Instanz-Schleife, die `MaterialPrimitive` (GL_PATCHES) kennt. Der Zwilling wird nie mit Tess-Stufen gelinkt; ohne Tessellation (`HE_TESSELLATION=0`, GL < 4.0) bleibt er wie bisher.
- **Nicht gewählt:** `MaterialPrimitive(instProg)` in `DrawGraphBatchInstanced` durchreichen. Das wäre Tessellated Instancing als Feature (eigener Schritt).
- **Zeuge** (neu, `EditorApplication.cpp`): `HE_DUMP_FOLIAGEMAT=displace` (+ `HE_DUMP_DISPCONST`, Standard 0,1 m) legt eine Konstanthöhe auf den Displacement-Pin des Foliage-Materials. Aufruf: `HE_HIDDEN_WINDOW=1 scripts/he_shot.py out.png RHI=OpenGL FOLIAGETEST=40 FOLIAGESIZE=60 FOLIAGEMESH=sphere FOLIAGESCALE=4,6 FOLIAGEMAT=displace DISPCONST=0.8 CAMX=0 CAMY=30 CAMZ=-5 PITCH=-70 DAYNIGHT=0 TOD=0.5`. (`FOLIAGEMESH=sphere` ergab hier Würfel, die Aussage gilt trotzdem.)
- **Gemessen** (Mac M5, GL 4.1, Release): vor dem Fix `glGetError=0x0502`, 10 Draws, 0 Magenta-Pixel (Batch unsichtbar). Nach dem Fix `glGetError=0x0000`, 49 Draws (Pro-Instanz), 305 815 Magenta-Pixel; gegen `HE_GRAPH_INSTANCING=0` IoU 0,9985 (Lauf-zu-Lauf-Rauschen 0,9961), gegen `HE_TESSELLATION=0` (instanziert, ohne Verformung) IoU 0,655, also wirklich tesselliert. Das Gelände flackert zwischen Läufen (Z-Fighting der weißen Streifen, Himmel), deshalb Magenta-Maske statt Pixelvergleich.
- **Andere Backends** (nur gelesen, keine Hardware): D3D11 (`D3D11Renderer.cpp:7498-7615`) bindet HS/DS/Patchlist am Plain-Shader und zeichnet den Zwilling mit `DrawIndexedInstanced` darunter; D3D12 (`:11541`, PSO-Topologie PATCH am Zwilling-PSO, Topologie per `m_tessPSOs.count(matPso)` des Plain-PSO) und Vulkan (Topologie steckt in der Pipeline) bauen den Zwilling ebenfalls mit Tess-Stufen, aber die Draws sind topologisch konsistent: **kein GL_INVALID_OPERATION-Pendant**. Offen und unverifiziert: ob die Zwillings-Geometrie dort stimmt (TES nimmt `u.model` = Identität und `u.mvp` = viewProj, der Welt-Offset ist dann korrekt; das ist Lesen, kein Bild).
- **Tests:** `test_material_graph`, `test_material_tessellation`, `test_engine_materials`, `test_graph_instancing` grün (ctest, Release, lokal). GL-Zeuge Forward vor/nach dem Fix; G-Buffer mit `RENDERPATH=1 GBUFFER=1` (Attributansicht, also wirklich deferred): nach dem Fix `glGetError=0x0000`, 49 Pro-Instanz-Draws, Batch sichtbar. Ein Vorher-Lauf für den G-Buffer fehlt (gleiche Funktion, gleicher Frühausstieg). Hinweis: ohne `GBUFFER=1` bleibt `RENDERPATH=1` im Dump offenbar im Forward-Bild.

## Schritt 10: Merge von release/0.7.0 (PR #182)

Merge-Commit auf dem Zweig, `origin/release/0.7.0` @ 12be6adc eingeflossen (kein Rebase, kein Force-Push).

## Handoff

- **Topic / step:** 207, Schritt 10 (Merge-Konflikte mit release/0.7.0)
- **Konflikte (2 echte):**
  - `src/HE_Core/src/MaterialGraph/MaterialGraph.cpp` (`bombGrid`, ~Zeile 690): Release (Thema 180) hat `heBombGridV` + `variants`-Parameter eingefuehrt, dieser Zweig die Domain-Stage-Gradienten (`dsStage(sc)` → `dx/dy = vec2(0.0)`, kein `dFdx` ohne Pixel). Beides uebernommen: Release-Variantenform, danach der `dsStage`-Zweig fuer `<g>dx/<g>dy`.
  - `tests/CMakeLists.txt`: beide Seiten haben eine Testdatei in dieselbe Stelle eingefuegt (`test_material_tessellation.cpp`, `test_auto_landscape_variants.cpp`); beide bleiben.
- **Automatisch gemergt, gegengelesen:** Texture-/Array-Sample- und Bomb-Emit (Release-`Groups`-Pin + Branch-`dsStage`-Faelle ergeben zusammen sinnvollen Code: im Domain-Stage-Pfad `textureLod`/mip-0-Gradienten, Varianten-Auswahl ueber Hash bleibt); `OpenGLRenderer.cpp` (Schritt-9-Zwilling-Skip neben Release-BCn/Layer-Mesh), `ci.yml`/`runtime_size.py` (Release hat die Windows-Fixes 4171d4b3/7929b814 nicht, bleiben erhalten).
- **Gewichen:** nichts. Release hat keinen neuen `MatNodeType`, die Enum-Reihenfolge und die Registry-Zaehlschleife sind unberuehrt; Release's `Groups`-Pins sind am Ende der Pin-Listen angehaengt, Pin 9 (Displacement) am Output-Knoten kollidiert nicht.
- **Verified (Mac M5, Release-Build im Worktree):** `he_tests` + `HorizonEditor` gebaut (BUILD_RC=0); gesamte ctest-Suite 332/332 gruen mit leerem `HOME` (die zwei `runtime_size_app_*` sind Skipped); `editor_help_audit.py --check` 1216/1216. Gezielt: `test_material_graph`, `test_engine_materials`, `test_material_tessellation`, `test_material_editor_ui`, `test_mcp_tools_material`, `test_culling`, `test_auto_landscape_variants`, `test_texture_bcn`.
- **Not verified:** Domain-Stage + Release-`Groups`-Variante im Zusammenspiel gibt es nur als Codelesen, kein Test mit Displacement-Hoehe aus einem Array-Bomb-Sample mit Groups-Pin. D3D11/D3D12/Vulkan/Windows/Linux nicht gelaufen (nur CI). Kein Bild-/GL-Zeuge nach dem Merge.
- **Traps:** Ohne `HOME=<leerer Ordner>` schlaegt `test_hpak` lokal auf dem Mac fehl (EngineContentCache). Der Build ist nach dem Merge ~425 Ziele gross (~25 min auf dem Air).
- **Next steps:** CI-Lauf des Pushs abwarten (Hive meldet); PR #182 mergt die Queen, nicht dieser Schritt.
