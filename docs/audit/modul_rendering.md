# Modulbericht Rendering – HorizonRendering + Backends (93 k LOC)

Pfad: `src/HE_Rendering` · Teilberichte: 9 (`_parts/Rendering_*.md`)

## Modulzusammenfassung

> **Abdeckungshinweis:** `src/HE_Rendering/shaders/gi_probe_hw.hlsl` (nur Z. 1–60, 120–252) und `gi_shadow_hw.hlsl` (nur per Grep) wurden nicht vollständig gelesen; sie waren in der Slice-Liste nicht enthalten. Alle übrigen Dateien vollständig. Wiederkehrendes Muster über alle 5 Backends: `resolveShaders` pro Draw, Cull/Sort pro Shadow-Layer, TAA-`unordered_map`, Sky-IBL-Bake, `BuildSSAOKernel` pro Frame (siehe AUDIT.md).

**204 Funde**: Hoch 33, Mittel 108, Niedrig 63 · Typ: Vereinfachung 61, Performance 132, Beides 11 · Kategorien: A 44, B 12, C 43, D 74, E 14, F 17

| Teilbericht | Hoch | Mittel | Niedrig |
|---|---|---|---|
| Rendering_1.md | 3 | 10 | 10 |
| Rendering_2.md | 4 | 17 | 3 |
| Rendering_3.md | 2 | 7 | 12 |
| Rendering_4.md | 8 | 17 | 4 |
| Rendering_6.md | 5 | 20 | 4 |
| Rendering_7.md | 1 | 3 | 5 |
| Rendering_8.md | 5 | 12 | 5 |
| Rendering_9.md | 3 | 9 | 11 |
| Rendering_10.md | 2 | 13 | 9 |

### Hoch-Funde (Index)

- **REN-101** – Graph-Material-Draw kopiert und hasht pro Draw den kompletten GLSL-Fragment-String
- **REN-102** – Pro Draw werden Textur-Cache-Schlüssel als `std::string` gebaut und Pfade kopiert
- **REN-103** – Jeder Graph-Material-Draw bindet ~25 Ressourcen und stellt den Built-in-Zustand komplett wieder her; HeParams wird pro Instanz neu hochgeladen
- **REN-201** – Pro Graph-Material-Draw wird der komplette GLSL-Quelltext kopiert und gehasht
- **REN-202** – Statische Mesh-Puffer (und GI-BVH) liegen dauerhaft im UPLOAD-Heap
- **REN-203** – Shadow-Pass rendert jeden Frame alle Kaskaden und alle lokalen Layer neu, jeweils mit eigenem Cull + Sort
- **REN-204** – Debug-Line-Vertex-/Constant-Buffer existiert nur einmal, nicht pro Frame in Flight (Datenrennen CPU/GPU)
- **REN-301** – `MetalRenderer` ist eine 1400-Zeilen-Gottklasse mit offenem Innenleben im öffentlichen Header
- **REN-302** – Pro Frame frische `MTLBuffer` trotz vorhandenem `FrameUploadRing`; Header-Kommentar rechtfertigt es mit veraltetem Stand
- **REN-401** – Sky-Env-Cube: 98k CPU-Atmosphärenintegrale bei jeder Sonnenbewegung
- **REN-402** – Skinning: pro Objekt und Frame ein neuer 8-KB-`MTLBuffer` samt Identity-Fill
- **REN-403** – UI-Material-Quads: pro Quad und Frame GLSL-Kopie, Hash und Volltextsuche
- **REN-404** – GI-Accel-Build: pro Frame lineare BLAS-Suche, Material-Auflösung und Transform-Inverse für jedes Objekt
- **REN-405** – GI-TLAS wird jedes Frame komplett neu allokiert und gebaut (kein Dirty-Check)
- **REN-406** – `BuildBLAS`: blockierendes `waitUntilCompleted` pro neuem Mesh auf dem Render-Thread
- **REN-407** – Shadow-Pass: Cull **und** Sort pro Layer (bis zu 3 Kaskaden + 16 Lokal-Layer)
- **REN-408** – `EncodeGBuffer` ist eine ~250-Zeilen-Kopie der Forward-Opaque-Schleife in `EncodeScene`
- **REN-601** – `fillMatLight` füllt das komplette Lighting-UBO-Struct bei jedem Material-Draw, obwohl nur einmal pro Frame hochgeladen wird
- **REN-602** – `resolveMaterialShader` kopiert und hasht die komplette GLSL-Quelle pro Draw-Call und Frame, ohne Memo
- **REN-603** – UI-Pass: ein Draw pro Quad/Glyph mit 15 `glUniform*`-Aufrufen, kein Batching, kein Uniform-Cache
- **REN-604** – UI-Material-Quads: pro Quad Auflösung, UBO-Uploads und (bei „Frosted“) komplette Framebuffer-Kopie + Mipmaps
- **REN-605** – Schattenpass: pro Cascade und pro lokaler Ebene vollständiger Cull + Sort, keine Wiederverwendung statischer Ebenen
- **REN-701** – Drei parallele Uniform-Location-Sätze (unlit / skinned / instanced) im OpenGLRenderer
- **REN-801** – Graph-Material-Draw: Descriptor-Set-Allokation, ~25 Writes und Shader-String-Kopie/-Hash pro Draw (und pro Instanz)
- **REN-802** – Jeder Asset-Upload blockiert die Queue (`vkQueueWaitIdle`) auf dem Render-Thread
- **REN-803** – Mesh-/Skeletal-/Ribbon-Buffer liegen in host-visible Speicher; je Buffer eigener `vkAllocateMemory`, kein Sub-Allocator
- **REN-804** – Validation-Layer fest eingeschaltet (`gpuDebug = true`) auch in Release
- **REN-805** – Cull + Sort bis zu 5× pro Frame über denselben Objektsatz
- **REN-901** – CommandBuffer::recordDraw/recordSkinnedDraw kopieren DrawCall samt Instanz- und Bone-Vektoren
- **REN-902** – Section-Expansion kopiert die komplette Instanzliste pro Material-Slot
- **REN-903** – BuildClusterLights legt pro Frame 3456 Vektoren an und sammelt Indizes per `push_back`
- **REN-1001** – Voller GLSL-String wird pro Draw und Frame kopiert und gehasht
- **REN-1002** – DrawCall wird mehrfach tief kopiert (inkl. `instanceTransforms`)

## Zusammenfassungen der Teilberichte

**Rendering_1.md**: Der Code ist gründlich kommentiert, funktional sauber und für D3D11 sinnvoll aufgebaut (Instancing für Depth-/Szenen-Pässe, Dynamic-CBs mit `MAP_WRITE_DISCARD`, GPU-Timer-Ring ohne Stall). Die größten Hebel liegen im Hot Path der Draw-Schleife (`drawDC`, Graph-Material-Zweig): dort werden pro Draw GLSL-Strings kopiert und gehasht, Textur-Schlüssel als `std::string` gebaut und rund 25 Binds + Restores abgesetzt. Danach folgen per-Frame-Doppelarbeit (Cluster-Build/Upload zweimal, GI-Instanzbuffer wird jedes Frame neu angelegt, volle Probe-Atlas-Kopie), eine ca. 1500 Zeilen lange `DrawScene`-Funktion und ein 4700-Zeilen-`Impl`-Struct mit vielfach kopierten Lambdas (`makeRT` 6x, `makeCB` 6x). Insgesamt 23 Funde: 3 Hoch, 10 Mittel, 10 Niedrig. Alle Performance-Aussagen sind Schätzungen ohne Messdaten (siehe "Unsicher / Messung nötig").

---

**Rendering_2.md**: `D3D12Renderer.cpp` ist funktional reif (Frames in Flight, Ring-Buffer pro Slot, Retire-Listen, DRED, DXR-Zweig), aber über die Jahre zu einer einzigen Übersetzungseinheit mit ca. 1 200 Zeilen eingebettetem HLSL und einem ca. 7 300 Zeilen langen Inline-`struct D3D12RendererImpl` gewachsen. Die größten Hebel liegen im Hot Path: Pro Graph-Material-DrawCall werden der komplette GLSL-Quelltext kopiert und gehasht (Zeile 10971-10975), Mesh-/BVH-Puffer liegen dauerhaft im UPLOAD-Heap, alle 3 Kaskaden und bis zu 16 lokale Shadow-Layer werden jeden Frame neu gecullt, sortiert und gezeichnet, und der Debug-Line-Puffer ist nicht pro Frame-in-Flight vorhanden (Datenrennen). Dazu kommen viele einzelne Frame-Kosten (Bloom läuft bei "aus", doppelte Per-Frame-/Cluster-Füllung, volle Atlas-Kopien der GI-Probes, Root-Signature-/Heap-Wechsel pro Material-Draw) und viel Boilerplate (20x Root-Signature-Serialisierung, 8x identische `compile`-Lambdas, 5 parallele Retire-Listen). Funde: 4 Hoch, 17 Mittel, 3 Niedrig (davon 2 reine Vereinfachung). Alle Zeilenangaben sind gegen die Datei verifiziert; Messdaten liegen nicht vor, GPU-seitige Effekte sind als Schätzung gekennzeichnet.

---

**Rendering_3.md**: Slice 3 besteht fast nur aus Deklarationen und Shader-Quelltext. Die Logik sitzt in den zugehörigen `.cpp`/`.mm`, die in anderen Slices liegen. Die Funde betreffen deshalb vor allem Struktur und Wartbarkeit (`MetalRenderer.h`) und GPU-seitige Kosten in `HlslSources.h`. Wo ein Header-Kommentar eine Laufzeitannahme macht, habe ich sie gegen das `.mm` geprüft (Zeilen angegeben). `D3D12Renderer.h` ist mit 120 Zeilen und Pimpl sauber. `HlslSources.h` ist durchdacht und gut dokumentiert. Slice 5 (Metal-Shader-Manager) ist klein und hat kein Laufzeitproblem, aber toten Code und eine Vertrags-Inkonsistenz zu den anderen vier Managern.

Anzahl Funde: Hoch 2, Mittel 7, Niedrig 12 (gesamt 21; davon Slice 5: REN-351 bis REN-354, 0 Hoch / 1 Mittel / 3 Niedrig).

**Rendering_4.md**: `MetalRenderer.mm` ist funktional ausgereift und an vielen Stellen bereits performance-bewusst (Upload-Ring, Instancing-Pfade, Einmal-Extract pro Frame, memoisierte Mesh-/Material-Auflösung im Draw-Loop, parallele Sky-Env-Bake). Die Datei besteht zu rund 6350 Zeilen aus eingebetteten MSL-Strings (Z. 238–6367), danach ~11800 Zeilen Objective-C++-Host-Code. Hauptprobleme: (1) eine Reihe von **Pro-Frame-Heap-/Metal-Objekt-Allokationen** außerhalb des vorhandenen Upload-Rings (Skinning-Bone-Puffer, GI-TLAS samt Hilfspuffer, Partikel-/Ribbon-/Preview-Puffer), (2) **blockierende CPU-GPU-Syncs** (`waitUntilCompleted` im BLAS-Build und in jeder Preview/Thumbnail), (3) **O(n)-/O(n log n)-Arbeit pro Shadow-Layer und pro UI-Quad**, die sich einmal pro Frame erledigen ließe, und (4) **massive Code-Duplikation** (G-Buffer- vs. Forward-Draw-Loop ~250 Zeilen, 6× Extract-Block, 3× SSR-Blur, 7× identische MSL-Hilfsfunktionen, 168 händische `CFBridgingRelease`-Blöcke) und eine 1000-Zeilen-Funktion `EncodeFrame`.
Funde: 8 Hoch, 17 Mittel, 4 Niedrig (29 gesamt). Alle Performance-Schätzungen sind ohne Messdaten und als Schätzung gekennzeichnet; Verifikation jeweils über `HE_PROFILE_SCOPE_N`/Metal-System-Trace und die vorhandenen Headless-Captures (`HE_SKY_TIME`, he_shot).

---

**Rendering_6.md**: `OpenGLRenderer.cpp` ist funktional sehr vollständig und durchgehend gut kommentiert, aber in drei Punkten teuer: (1) der Pro-Draw-Call-Pfad für Graph-Materialien arbeitet ohne Memoisierung (GLSL-Kopie + Hash, Lighting-Struct-Fill, String-Schlüssel-Lookups pro Draw), obwohl für Mesh/Material-Parameter bereits ein `lastMatId`-Memo existiert; (2) UI-Quads und Schattenebenen werden ohne Batching bzw. ohne Caching jedes Frame komplett neu aufgebaut; (3) die Datei enthält sehr viel Copy-Paste (≈38 `glLinkProgram`-Blöcke, ≈34 `glGenFramebuffers`-Blöcke, dreifach geführte Uniform-Location-Listen, GLSL-Zwillinge). Zusätzlich gibt es tote Pfade (deaktiviertes Reflection-Temporal inkl. VRAM) und Lücken in `Shutdown()`.
Funde: **5 Hoch, 20 Mittel, 4 Niedrig** (29 gesamt). Alle Laufzeit-Aussagen sind aus dem Code abgeleitet, nicht gemessen; wo die Größenordnung unklar ist, steht sie unter „Unsicher / Messung nötig“.

---

**Rendering_7.md**: Der Slice ist klein (2640 Zeilen) und besteht fast nur aus Headern bzw. dem schmalen Software-Backend. Der Code ist durchgehend gut kommentiert; die Software-Rasterung ist sauber vom Fenster-Shell getrennt und hat bereits Dirty-Rects. Die größten Hebel: (1) `OpenGLRenderer.h` trägt rund 600 Zeilen handgepflegter `int m_u…`-Uniform-Locations in drei fast identischen Sätzen (unlit/skinned/instanced), obwohl `SceneLightingLocs` und `GISceneLocs` schon den besseren Weg zeigen; (2) der Software-Renderer macht pro Dirty-Rect und Objekt unnötige Arbeit (Texture-Lookup, sin/cos vor dem Reject) und konvertiert jedes Pixel über `SDL_MapRGBA`; (3) `OpenGLShaderManager` ist faktisch toter Code. Funde: 1 Hoch, 3 Mittel, 5 Niedrig. Keine Messdaten vorhanden; Performance-Schätzungen sind als solche markiert.

**Rendering_8.md**: Der Vulkan-Backend ist funktional reich (Deferred, TAA, SSR, SW/HW-GI, Decals, Preview), aber die Datei ist eine einzige 14,7k-Zeilen-Klasse mit sehr viel kopiertem Vulkan-Boilerplate (33× `vkCreateGraphicsPipelines`, 16× `VkPipelineViewportStateCreateInfo`, 21× One-Shot-Command-Buffer mit `vkQueueWaitIdle`, 58× `findMemoryType`, 53× `vkAllocateMemory`, 4× identisches `makeModule`-Lambda). Die schwersten Performance-Probleme liegen im Draw-Pfad der Graph-Materialien (pro Draw/Instanz ein frisch allokiertes Descriptor-Set mit ~25 Writes plus Shader-Quelltext-Kopie und -Hash), in der blockierenden Asset-Upload-Strategie (je Mesh/Textur eigener Submit + `vkQueueWaitIdle`, je Buffer eigene `vkAllocateMemory`, Vertex-/Index-Buffer in host-visible Speicher), in der fest eingeschalteten Validation-Layer und in der mehrfachen Cull+Sort-Arbeit pro Frame. Gefunden: 5 Hoch (REN-801 bis 805), 12 Mittel (REN-806 bis 817), 5 Niedrig (REN-818 bis 822), zusammen 22 Funde. Es gibt keine Pipeline-Cache-Nutzung (`VkPipelineCache`: 0 Treffer) und keinen Sub-Allocator. Vorschläge erhalten das sichtbare Verhalten; wo nicht, ist es vermerkt. Keine Messdaten verfügbar: Effekte sind Schätzungen.

---

**Rendering_9.md**: Der Slice (50 Dateien, ca. 9,5k Zeilen; davon 2568 Zeilen `SkyShaderSource.h` und 1566 Zeilen `VulkanRenderer.h`) ist insgesamt gut kommentiert und bereits stark von Cross-Backend-Duplikation befreit (LightPacking, ClipSpace, GIProbeGrid, SkyFrameParams). Die relevanten Befunde liegen bei der Draw-Call-Aufzeichnung (tiefe Kopien der Instanzlisten), beim pro Frame neu aufgebauten Cluster-Licht-Grid, bei GPU-seitig wiederholt berechneten Sky-Konstanten (Atmosphere, Aurora) und bei kleineren Per-Frame-Allokationen im Vulkan-Header (String-Keys, `unordered_map` fuer TAA). Es gibt 3 Funde mit Schwere Hoch, 9 Mittel und 11 Niedrig (gesamt 23). Alle Zeilenangaben wurden gegen den Stand der Dateien geprueft; GPU-Kosten (Sky-Shader) und Rebake-Haeufigkeit sind ohne Messdaten als "messen" markiert. Backend-Rendererdateien (`*Renderer.cpp/.mm`) gehoeren nicht zu diesem Slice, wurden aber gezielt per Grep zur Verifikation von Aufrufstellen angesehen.

**Rendering_10.md**: Der Frontend-Code ist insgesamt sauber strukturiert (Phasen-Funktionen im Extractor, Frame-Cache mit Reuse-Pfad, vorberechnete Sort-Keys, Depth-Run-Batching, Occlusion-Culler mit klaren Konservativitäts-Garantien). Die größten Kosten liegen nicht in der Algorithmik, sondern in **wiederholtem Kopieren großer Strukturen pro Frame** (DrawCall mit `instanceTransforms`, Bone-Matrizen, GLSL-Quelltexte pro Draw) und in **hand-gepflegter Duplikation** (Shader-Blöcke in `MaterialShaderLibrary.cpp`, sieben GI-Kernel-Kopien). Gefunden: **2 Hoch, 13 Mittel, 9 Niedrig** (insgesamt 24). Die Datei-Liste des Slice enthielt `src/HE_Rendering/shaders/` nur unvollständig: es fehlten `gi_probe.comp`, `gi_probe_hw.comp`, `gi_probe_hw.hlsl`, `gi_shadow.comp`, `gi_shadow_hw.comp`, `gi_shadow_hw.hlsl` (6 von 45 Dateien, 1276 Zeilen); die vier GLSL-Kernel wurden zusätzlich vollständig gelesen, die beiden HLSL-Twins nur teilweise (siehe „Gelesen“), und sie sind in den Funden REN-1013/1014/1023 berücksichtigt. Hinweis: `CommandBuffer`/`Culler` liegen als Header/Impl in anderen Slices bzw. wurden nur gezielt (`CommandBuffer.cpp/.h`, `RenderObject.h`, `RenderSorter.h`) gelesen, soweit sie die Funde belegen. Nichts davon wurde gemessen; Aufwandsschätzungen sind Schätzungen.

---

## Cross-Modul-Hinweise

**Rendering_1.md**

- **`src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-952, 1000-1009` (Slice 10)**: `resolveShaders/resolveFragment` kopieren GLSL und hashen es bei jedem Aufruf. Ursache von REN-101 in allen fünf Backends. Parallelstellen mit demselben Per-Draw-Aufruf: `D3D12Renderer.cpp:10974`, `VulkanRenderer.cpp:8254`, `OpenGLRenderer.cpp:3599`, `MetalRenderer.mm:12815`. Eine gemeinsame Lösung (Generationszähler/gecachter Hash am `MaterialAsset`, `resolveShaderKey`) wäre einmal in `MaterialShaderLibrary` + `ContentManager` zu bauen.
- **`BuildSSAOKernel` pro Frame**: `OpenGLRenderer.cpp:5784`, `VulkanRenderer.cpp:12172`, `D3D12Renderer.cpp:5792`, `MetalRenderer.mm:12550` (siehe REN-114). Besser: `HE::SsaoKernel()` liefert `const std::array<glm::vec4,32>&` (statisch, festes Seed) in `src/HE_Rendering/src/SsaoKernel.cpp` (Slice 10).
- **`BuildClusterLights` doppelt pro Frame**: Prüfen, ob die gleiche Doppel-Fill-Struktur (`fillPerFrame` vor + nach den Pässen) auch in `D3D12Renderer.cpp:10318`, `OpenGLRenderer.cpp:11416`, `VulkanRenderer.cpp:7875` auftritt.
- **Embedded HLSL-Duplikation D3D11/D3D12**: `kFxaaHLSL/kSmaaHLSL/kBloom*HLSL/kAABlitHLSL/kUIHLSL` in `D3D11Renderer.cpp:934-1220` und `kFxaaHLSL12/kSmaaHLSL12/kBloomBrightHLSL12/kBloomBlurHLSL12/kAABlitHLSL12/kUIHLSL12` in `D3D12Renderer.cpp:945-1210`; laut `D3D_Shared/HlslSources.h` nur durch `Texture2D _dummy : register(t1)` (und bei UI Glyph-UV im VS/PS) verschieden. Gemeinsame Quelle mit Präprozessor-Makro ist machbar (REN-108).
- **`UpdateSkyEnvCube` / sky env bake**: `OpenGLRenderer`, `Vulkan`, `Metal` haben dieselbe Bake-/Upload-Struktur (REN-116); gemeinsamer Helfer `HE::BakeSkyEnvHalf(...)` in `SkyEnvBake.h` könnte `px`+`half` zusammenführen.
- **`SkyNoise3D.cpp:10` (Slice 10)**: `BuildSkyNoise3D(256)` läuft synchron beim Renderer-Start für alle Backends (D3D11: `createSkyPipeline`, Z.5468); die Datei enthält laut Kommentar bereits eine Optimierung (Worley-Cache, vorher ~10.7 s). Kein `parallel_for` zu sehen; Start-Messung sinnvoll.
- **`RenderSorter::sort/batchDepthCasters` (nicht in diesem Slice)**: REN-113 ruft sie bis zu 19x pro Frame auf; lohnt Blick in `src/HE_Rendering/src/RenderSorter.cpp` (Slice 10), ob für Depth-only eine Gruppierung ohne Tiefensort reicht.

**Rendering_2.md**

- **MaterialShaderLibrary** (`src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-952`, `:1000-1009`): `resolveFragment`/`resolveShaders` kopieren und hashen den GLSL-Text bei jedem Aufruf. Wird von allen Backends pro DrawCall genutzt (D3D11 `:7009`, GL `:3599`, Metal `:12815`). Ein Hash-Cache im Asset/der Library hilft allen vier Backends (REN-201).
- **RenderSorter** (`src/HE_Rendering/include/HorizonRendering/RenderSorter.h`, `SortKey { meshHi, meshLo, distSq, index }`): kein Material-/PSO-Anteil im Schlüssel; Grundlage für REN-205 und für Zustandswechsel in allen Backends.
- **RenderExtractor/RenderWorld**: Dirty-Flags pro Objekt/Licht (für Transform, `refineWorldBounds`, GI-Instanzen, Shadow-Layer-Caching) existieren nur indirekt über `GIProbeSceneSignature` (`GIProbeGrid.h`). Ein gemeinsames Frame-zu-Frame-Delta (geänderte Entity-IDs) würde REN-203, REN-210 und REN-221 gleichzeitig tragen.
- **D3D11/GL/Metal**: dieselben `glm::inverse(obj.transform)` in `updateGiAccel`-Äquivalenten (`D3D11Renderer.cpp:3184`, `OpenGLRenderer.cpp:5974`, `MetalRenderer.mm:8245`); `BuildClusterLights` je Backend aus `fillPerFrame`-Äquivalenten (`D3D11Renderer.cpp:6332`, `OpenGLRenderer.cpp:11416`, `MetalRenderer.mm:14920`), dort ebenfalls auf Doppelaufruf prüfen; Vulkan nutzt dieselbe feste 10-Pass-Bloom-Kette (`VulkanRenderer.cpp:1000`).
- **D3D_Shared/HlslSources.h**: `kSkyFuncHLSL` wird in `PSMain` dreimal pro Pixel ausgewertet (Ambient diffus, Env-Spec, Nebel); nicht im Fokus dieses Slices, aber ein Kandidat für die Slice, die die Shared-Dateien prüft.

**Rendering_3.md**

- `src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm` (18158 Zeilen): Ergänzend zu REN-302 gibt es `newBufferWithBytes` auch bei `10509`, `10844` (Previews, unkritisch) und `10947`; dort Hot-Path-Relevanz prüfen. Zwölf `waitUntilCompleted`: unkritisch sind die Editor-Pfade (Previews/Thumbnails/Capture `9998`, `10078`, `10221`, `10600`, `10907`, `11135`, `11281`) und der Profiler-Pfad (`16636`, `17372`, laut Kommentar „capture is slow"); kritisch ist nur `8164` (REN-303).
- `.mm:7183-7191` und `m_ribbonBuffers` (Header 1372): Die Freigabe „on the NEXT frame's collect, by which time the command buffer … is long done" gilt nur, wenn höchstens ein Frame in Flight ist. Ob `WaitForFrame` (`.mm:17535`) das erzwingt, ist unsicher und im `.mm`-Slice zu prüfen (Dangling-GPU-Referenz, wenn mehrere Frames in Flight sind).
- `src/HE_Rendering/src/Backends/D3D12/D3D12Renderer.cpp`: 27 `D3DCompile`-Aufrufe (`D3D11Renderer.cpp`: 30) und kein persistenter Shader-/PSO-Cache (kein `ID3D12PipelineLibrary`/CachedPSO gefunden), obwohl Metal mit `MTLBinaryArchive` (Header 690-697) einen hat. Startzeit-Thema (Kategorie E), im jeweiligen Renderer-Slice bewerten.
- `D3D12Renderer.cpp:2844`, `2955`: Der Sky läuft bevorzugt über den cross-kompilierten GL-Sky (`skyFullModel`), die HLSL-Fassung `kSkyPSHLSL12` ist nur Fallback. Damit gibt es für den Sky weiterhin drei bis vier Handkopien (GL `kSkyFS`, `HlslSources.h` `kSkyFuncHLSL`, `kSkyPSHLSL12`, Metal `kSkyMSL`). `CLAUDE.md` nennt `kSkyFS` die „single source"; das gilt für die D3D-Fallback-Pfade nicht.
- `HlslSources.h` ist laut eigenem Kommentar mit GL/Metal handgespiegelt (kGi*, kTaa*, kSSAO*, kUnderwater*). Änderungen an den Algorithmen (REN-305, REN-313, REN-314, REN-315) müssen daher in `OpenGLRenderer.cpp`, den Vulkan-Shadern und `MetalRenderer.mm` (`kGISWMSL`, `kGIReflMSL`, `kTaaMSL`) gleichzeitig erfolgen.
- Gleiche Struktur wie REN-301 (Gottklasse im Header): `include/Backends/OpenGL/OpenGLRenderer.h` (1397 Zeilen) und `include/Backends/Vulkan/VulkanRenderer.h` (1566 Zeilen).

**Rendering_4.md**

- `src/HE_Rendering/src/RenderSorter.cpp:12–46`: `sort()` sortiert immer nach (Mesh-ID, Kamera-Abstand). Für Depth-/Shadow-Passes reicht ein Mesh-Gruppen-Sort; der Mehrfach-Aufruf je Layer (REN-407) trifft auch `VulkanRenderer.cpp:7007–7009` (m_shadowSorted) und vermutlich D3D11/D3D12/OpenGL – Lösung gehört in den gemeinsamen Sorter (`partition(sorted, visibleMask)`).
- `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:1000–1008` (`resolveFragment`): kopiert und hasht den kompletten GLSL-String bei jedem Aufruf. Ein `uint64_t`-Hash (oder eine Revision) im `MaterialAsset` würde diesen Aufruf in allen Backends (GL: `OpenGLRenderer`, Vulkan, D3D) auf O(1) bringen; betrifft REN-403.
- `src/HE_Rendering/include/material/MaterialShaderLibrary.h:40ff` (`Lighting`): >2 KB Strukt, die jede Backend-Draw-Schleife per Bytes/UBO sendet; ein „einmal pro Pass binden"-Konzept (REN-416) gilt analog für GL/D3D/Vulkan-Materialpfade.
- `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp:4226` (`UpdateSkyEnvCube`): gleicher per-Frame-CPU-Bake wie REN-401; gemeinsame Lösung (niedrigere Auflösung/Toleranz in `HorizonRendering/SkyEnvBake.h`) wirkt für beide Backends.
- `OpenGLRenderer.cpp:5972` / `MetalRenderer.mm:8190`: `HE::giInstanceSurface(obj, cm)` wird pro Objekt/Frame aufgerufen; ein Cache im Extractor (`src/HE_Rendering/src/GiInstanceSurface.cpp`) würde beide Backends beschleunigen (REN-404).
- `src/HE_Rendering/include/Backends/Metal/MetalRenderer.h` (898–899, 1321–1331): Cache-Typen (`unordered_map<std::string,void*>` für Graph-/UI-Texturen, `unordered_map<uint32_t,mat4>` für TAA) sind Ursprung von REN-412/417; Header-Änderung nötig.
- `docs/audit/_parts/Rendering_7.md` (OpenGL/Software, anderer Slice): `HE_SKY_TIME`-getenv-Muster (`OpenGLRenderer.cpp:5076, 5659, 10711`) kommt dort ebenfalls pro Frame vor.

**Rendering_6.md**

- `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:1000-1008` – `resolveFragment` kopiert die GLSL-Quelle und hasht sie bei jedem Aufruf; Hash gehört in `MaterialAsset` (`src/HE_Core/include/ContentManager/…`) und wird bei Material-Änderung neu berechnet. Betrifft alle Backends, die `resolveShaders` pro Draw rufen (D3D11/D3D12/Vulkan/Metal prüfen: `grep resolveShaders src/HE_Rendering/src/Backends/*`).
- `src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm`, `src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h`, `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp` – dieselben Zwillinge (`heSpecAARoughness`, DDGI-Sampling, SMAA/FXAA/Bloom/DoF/MotionBlur-Kernel) wie in REN-619; ein gemeinsamer Snippet-Header würde Drift (z. B. der PCG-Hash-Fix, Kommentar 2533) an einer Stelle beheben.
- `RenderSorter::batchDepthCasters`, `m_culler.cull` (`src/HE_Rendering/…/RenderCuller*`, `RenderSorter*`) – die Schattenpass-Mehrfach-Cull-/Sort-Schleife (REN-605) sollte als Backend-neutrale Funktion (ein sortierter Caster-Pool, mehrere Frustum-Filter, Dirty-Cache) in der gemeinsamen Render-Schicht liegen, nicht je Backend.
- `src/HE_Rendering/include/HorizonRendering/LightPacking.h` – `BuildPackedLightArray`, `BuildMaskedLocalLights`, `dominantDirectionalLight` werden pro Frame mehrfach berechnet (GI-Shadow, GI-Refl, Probe-Update, Cloud-Shadow, UI-Pass, Caustics, `fillMatLight` bis zu 3× im Deferred-Pfad); einmal pro Frame in `RenderWorld` cachen.
- Skinned Meshes tauchen im GL-Schatten-, GI- und Velocity-Pass nicht auf (`UpdateGIAccel`-Kommentar 5948-5950, `renderDepthLayer` nutzt nur `m_renderWorld.objects`); Parität zu Metal/Vulkan/D3D prüfen (`grep skinned src/HE_Rendering/src/Backends/*`).
- `src/HE_Rendering/include/Backends/OpenGL/OpenGLRenderer.h` (≈ 1300 Zeilen Member): die in REN-620/621/622 vorgeschlagenen Helfer (`GlTarget`, `SceneProgramLocs`, Programm-RAII) würden die Member-Anzahl deutlich reduzieren.

**Rendering_7.md**

- `src/HE_Rendering/src/Backends/Vulkan/VulkanRenderer.cpp:2860-2880, 8290-8560`: Pro-Draw-Descriptor-Set für Graph-Materialien (siehe REN-709); `k_matMaxDraws` als harte Grenze mit `noteRingFull()` – Verhalten bei Überlauf: Draw verschwindet leise. Das gehört in den Vulkan-Slice.
- `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp:11501/11545/12547` (BindSceneLighting-Aufrufer) und `:6063` (`FetchGISceneLocs`) sind die Ziel-Stellen von REN-701; Metal/D3D11/D3D12 haben parallele Uniform-/Binding-Sätze (`MetalRenderer.mm`, `D3D11Renderer.cpp`, `D3D12Renderer.cpp`) und sollten bei einer Vereinheitlichung geprüft werden.
- `src/HE_Rendering/src/Backends/D3D11/D3D11ShaderManager.cpp`, `D3D12ShaderManager.cpp`: Gegenstücke zu REN-706; dort werden die `m_loaded*`-Listen befüllt.
- `src/HE_Scene` / `RenderExtractor::extractUI`: REN-705 hängt davon ab, ob `extractUI` `uiObjects` leert und mit Kapazität wiederverwendet.
- `HE_Editor/EditorUI.cpp:173`, `EditorApplication.cpp:276`: Kommentare zum früher leckenden `fs`-Alias aus `ShaderManager.h` (REN-706, vor Entfernen prüfen).

**Rendering_8.md**

- `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-1009` (`resolveShaders`/`resolveFragment`): kopiert pro Aufruf die komplette `customShaderFragGlsl` in einen `std::string` und hasht sie (`std::hash<std::string>`). Dasselbe Muster bei jedem Draw in den Parallelstellen `Backends/D3D11/D3D11Renderer.cpp:7009`, `Backends/D3D12/D3D12Renderer.cpp:10974`, `OpenGLRenderer.cpp:3599`, `MetalRenderer.mm:12815` (und deren Warm-up-Schleifen D3D11:5318, D3D12:4406). Besser: `MaterialAsset` hält `uint64_t shaderHash` (beim Laden/Edit berechnet) und die Library liefert `const std::string&` / `string_view` ohne Kopie. Eine Änderung beschleunigt alle fünf Backends (REN-801).
- `Backends/D3D11/D3D11Renderer.cpp:7079` enthält denselben Block `matGlslTextureArrayMask`/`gIds/gPaths` pro Draw: gleiche Cache-Lösung (Material-Draw-Info-Cache) einmal gemeinsam in `src/HE_Rendering/src/material/` anlegen.
- Cull+Sort mehrfach pro Frame (REN-805): ähnlich D3D11 (`D3D11Renderer.cpp:6260`), D3D12 (`:10253`), OpenGL (`:10923`), Metal (`:7963`, `:17906`) – dort ist die Anzahl nicht geprüft. Ein gemeinsamer `RenderCuller`-Cache pro Frame (`src/HE_Rendering/src/`, `RenderCuller`/`RenderSorter`) wäre backendübergreifend wiederverwendbar.
- `RenderExtractor` (`m_extractor.extract(...)` in 3880, 6871, 7807, 11487, 12651): nur dann günstig, wenn die Walk-Key-Logik (Aspect, Settings) exakt zutrifft; Metrik „Anzahl vollständiger Walks pro Frame“ (`fullExtractCount()`, 6885) im Debug-HUD ausgeben, damit Regressionen auffallen.
- `HE::BuildSkyEnvFaceRow` (`HorizonRendering/SkyEnvBake.h`, REN-810): Variante mit Half-Float-Ausgabe anbieten (Vulkan und Metal nutzen RGBA16F).
- `HE::BuildSkyNoise3D(256)` (`HorizonRendering/SkyNoise3D.h`, REN-808): Bake bei jedem Start; als Pak-Asset oder Disk-Cache bereitstellen, gilt für alle Backends, die das Volumen nutzen.
- `IRenderer::SetDebugLines` (REN-818): Interface auf Move/`span` umstellen (alle Backends).
- Editor (`src/HE_Editor`): Viewport-Größenänderung entprellen (REN-813), sonst löst jeder Drag-Frame `createViewportResources` aus.
- `ContentManager::resolveTextureRef` lädt synchron (Kommentar 3616-3617): Mit REN-802 gekoppelt – Textur- und Mesh-Load im Render-Thread sollten in einen Streaming-Job wandern.

**Rendering_9.md**

- `src/HE_Rendering/src/RenderPass.cpp:62-118,137-140,165,209` (nicht in diesem Slice): hier entstehen die in REN-901/902 beschriebenen Kopien; die Aenderung gehoert in den Slice, der `RenderPass.cpp` pruefen wird.
- `src/HE_Rendering/src/RenderExtractor.cpp:349` ruft `giAutoLandscapeSliceMeans` pro Auto-Landscape und Extract (REN-913); `src/HE_Editor/EditorApplication.cpp:7740` ebenfalls.
- `src/HE_Rendering/src/Backends/{OpenGL,Metal,D3D11,D3D12}/*`: gleiche Muster wie REN-906 (Graph-Textur-Schluessel als String), REN-911 (TAA-Transform-Map), REN-908 (Bake-Treiber pro Backend, fuenfmal) und REN-912 (Preview-Frame-Aufbau und Wait) sind dort sehr wahrscheinlich ebenfalls vorhanden; gezielt mit `grep` pruefen (`graphTexKey`, `m_taaCurTransforms`, `BuildSkyEnvFaceRow`, `buildWorldPreviewFrame`).
- `src/HE_Rendering/include/HorizonRendering/ParticleShaderTemplates.h:132-150` vs. `:70-89`: das Metal-Partikel-Fragment (`heParticleGraphFragment`) hat kein Aequivalent zu `if (result.a < 0.003) discard;` der GLSL-Variante (Zeile 85). Das ist ein Verhaltensunterschied (Overdraw bei fast transparenten Partikeln, ggf. kein sichtbarer Unterschied bei Alpha-Blending), nicht Teil dieser Pruefung, aber fuer den Metal-/Partikel-Auditor relevant.
- `include/HorizonRendering/BloomKernel.h:21-24`: nur Metal nutzt den 5-Tap-Fold, die anderen Backends (`OpenGL postfx_bloom_blur.frag`, D3D11, D3D12) fahren noch den 9-Tap-Loop (laut Kommentar) – der Gewinn kann dort uebernommen werden (Shader-Auditoren der Backends).
- `include/HorizonRendering/SkyFrameParams.h:26-32`: OpenGL mappt ~54 Sky-Uniforms von Hand pro Frame (`OpenGLRenderer::DrawScene`), mit dem Hinweis "A NEW FIELD MUST BE ADDED THERE TOO". Das ist Pflege- und Performance-Thema (jeder `glUniform*`-Aufruf pro Frame) fuer den OpenGL-Auditor; ein UBO-Pfad fuer GL 4.1 ist moeglich (UBOs sind in 4.1 Kern).
- `tests/test_culling.cpp` pinnt Hashes fuer `BuildSkyEnvFace` und `BuildSkyNoise3D`; jede Aenderung an SkyEnvBake/SkyNoise3D (REN-904/908) betrifft diese Pins.

**Rendering_10.md**

- `src/HE_Rendering/src/Backends/{D3D11,D3D12,Vulkan}/…Renderer.cpp`: `resolveShaders` im Draw-Loop (siehe REN-1001), `recordDraw`-Konsumenten lesen `dc.instanceTransforms`/`boneMatrices` (REN-1002/1003) – Umstellung betrifft alle fünf Backends; `ShadowPass`-Sinks in D3D11 (`:6597-6640`), D3D12 (`:10607-10614`), GL (`:10992-11001`) verwerfen `cmds` (REN-1004).
- `src/HE_Rendering/src/Backends/Vulkan/VulkanRenderer.cpp`: `resolveWorldMaterialScalars` an drei Stellen pro Frame (`:3890`, `:7838`, `:11495`); CPU-Seite der 128-B-Instanzdaten (REN-1009) dort lesen; `scene.frag`-Himmel analytisch (REN-1008).
- `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp` (`kUnlitFS`), `Metal/MetalRenderer.mm` (`fragmentMain`), `D3D*/…` (`kSceneHLSL`/`GI_PROBE_OCT`-Blöcke) und `D3D_Shared/HlslSources.h`: wahrscheinlich dieselbe Reihenfolge „Schatten vor Dämpfungstest“ wie in heLitP/scene.frag (REN-1007) – nicht gelesen, per Grep `localShadowFactor` prüfen.
- `src/HE_Core/include/ContentManager/Assets.h` (`MaterialAsset`): Hash der Custom-Shader-Quellen vorhalten (REN-1001).
- `src/HE_Scene/src/WaterQuery.cpp` (`WaterIndex::build`): Cache nutzlos bei Neubau pro Aufruf (REN-1021); `EngineApi.cpp:1578` hat dieselbe Nutzung.
- `src/HE_Rendering/src/Backends/*`: `OcclusionCuller::refine` nur in GL (`:10929`) und Metal (`:17907`) verdrahtet; D3D11/D3D12/Vulkan rufen es nicht auf (Feature-Parität bzw. Absicht dokumentieren).
- `include/HorizonRendering/LightPacking.h:27` (`kMaxLightWindow`) und `RenderConstants.h:14` (`kShadowMapResolution`) sollten die einzigen Quellen für 8 bzw. 2048 sein (REN-1017).

**Nachtrag 17.10.2026 (Thema 182 Schritt 4, Entscheidung 110): Muster „D16-Tiefe auf D3D/Vulkan“, jetzt zweimal aufgefallen**

- Die eigenen Tiefenpuffer der Kamera-Vorpässe sind auf D3D11/D3D12/Vulkan **D16** (GL 24 Bit, Metal 32). Ab einigen zehn Metern fallen Flächen auf gleiche
  16-Bit-Werte, und die Zeichenreihenfolge entscheidet, welche im Vorpass landet: ein Bild, das von Batching/Sortierung abhängt. Gefunden im GI-G-Buffer
  (Thema 204) und im SSAO-Positionsvorpass (Thema 182: Foliage-Cluster gegen ordered 0,21 %/0,51 % des Bodens auf D3D11/Vulkan, mit D32 auf D3D11 identisch;
  `docs/foliage-instancing-lod-plan-2026-10-10.md` §17.3). Stand dieses Branches: SSAO-Vorpass `D3D11Renderer.cpp:2430`, `D3D12Renderer.cpp:5592/5943/5961/6229`,
  `VulkanRenderer.cpp:12406/12747/13201` (Reflexions-MRT-Pass); GI-G-Buffer `D3D11Renderer.cpp:3436`, `D3D12Renderer.cpp:7387/7588` (falls Thema 204 sie noch nicht
  umgestellt hat). Neue Vorpässe auf D3D/Vulkan: D32_FLOAT / D32_SFLOAT nehmen, und vor einem Bild-A/B nach `D16_UNORM` greppen.
- **Erledigt (Thema 182 Schritt 6):** der SSAO-Positionsvorpass (mit dem Reflexions-MRT-Vorpass) ist auf allen drei D32, je Backend `kSsaoPosDepthFormat`,
  bewacht von `test_foliage_cluster.cpp`; Cluster gegen ordered jetzt pixelgleich auf D3D11/D3D12/Vulkan (Plan §19). Offen bleibt nur der GI-G-Buffer, den
  Thema 204 (`520f1453`) umstellt. Sind beide auf release/0.7.0, gibt es in den drei Backends kein `D16_UNORM` mehr; dann taugt ein Guard „kein D16“.

## Unsicher / Messung nötig

**Rendering_1.md**

- REN-101/102/103: tatsächlicher CPU-Anteil pro Graph-Material-Draw (Zielwert 5-20 µs ist Schätzung). Messung: `EngineProfiler`-Scope um den Graph-Zweig, 500 Material-Draws.
- REN-106: ob der GI-Probe-Kernel ausschließlich eigene Tiles aus den Prev-Texturen liest (`kGiProbeCSHLSL` in `D3D_Shared/HlslSources.h` nicht gelesen); GPU-Kosten der Atlas-Kopie.
- REN-110: reale `D3DCompile`-Zeit der Szenen-Shader (Start-Messung).
- REN-113: durchschnittliche Zahl aktiver Schatten-Layer und Anteil statischer Lichter in typischen Szenen.
- REN-115/REN-119: Wirkung einzelner Lookups/Binds im Bereich unter 0.5 ms ist ohne Profil nicht belegbar; nur umsetzen, wenn REN-101-103 gemessen sind.
- REN-120: GPU-Zeit der Bloom-Kette (PIX/GPU-Timer nur whole-frame).
- Nicht verifiziert: Verhalten von `RenderSorter::partitionByOpacity/sort` (ob die Opaque-Liste bereits nach Material sortiert ist; relevant für REN-103 Punkt 3).

**Rendering_2.md**

- REN-202: Gewinn von DEFAULT- statt UPLOAD-Heap hängt von GPU-Architektur (dGPU vs UMA) ab; mit `GetFrameGpuStats().gpuFrameMs` (Zeile 12891) auf dGPU messen.
- REN-203: Anteil unveränderter Shadow-Layer in realen Szenen (statische Lichter) und tatsächlicher Anteil des Shadow-Passes an der Frame-Zeit.
- REN-205: Kosten von `SetDescriptorHeaps`/Root-Signature-Wechseln sind treiberabhängig (NVIDIA/AMD/Intel); mit PIX-Capture bestätigen.
- REN-208/REN-219: GPU-Zeit der Bloom-Kette bzw. des Sky-Passes pro Auflösung (1080p/1440p/4K).
- SSAO läuft in voller Auflösung (`createSSAOTargets(width, height)` bei `:10817-10818`, Position-Prepass `RGBA16F` plus extra Geometriepass): halbe Auflösung wäre ein typischer Gewinn, ändert aber das Bild (nicht verhaltenserhaltend), daher nur mit Qualitätsentscheidung.
- Sky-Noise-Bake: `HE::BuildSkyNoise3D(256)` (Zeile 2704-2708, nur Release) erzeugt 256³ x 4 B = 64 MiB auf der CPU, dazu ein gleich großer Upload-Buffer und eine 64-MiB-Textur (ca. 192 MiB transient beim Start). Startzeit messen; Offline-Bake oder Compute-Bake wäre Alternative.
- `D3D12CreateDevice(nullptr, ...)` (Zeile 9862) nimmt den Standard-Adapter; auf Hybrid-Laptops kann das die iGPU sein. Ob `EnumAdapterByGpuPreference` gewünscht ist, ist eine Produktentscheidung.
- Swapchain ohne `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` / `SetMaximumFrameLatency` (Zeile 9912-9921): Frame-Latenz hängt vom Treiber ab; nicht geprüft, ob das Pacing-Problem (Kommentar 83-86) damit besser lösbar ist.
- REN-211: ob irgendein Pass zwischen den zwei `fillPerFrame`-Aufrufen die CB braucht, bitte mit Debug-Layer prüfen, bevor die Erstfüllung entfällt.

**Rendering_3.md**

- REN-302: tatsächliche Zeit von `newBufferWithBytes` je skinned Objekt auf Apple-Silicon und Intel/AMD-Macs (Profiler-CPU-Zähler).
- REN-303: GI-Aktivierungs-Hitch in Szenen mit vielen Meshes (Frame-Zeit der ersten GI-Frames).
- REN-304: Anteil von Frustum-Cull und Sort an der CPU-Frame-Zeit bei großen Szenen.
- REN-305, REN-313, REN-314: GPU-Zeiten der GI-Pässe je Backend (GPU-Pass-Timer), erst danach optimieren.
- REN-309 (b): Ob der HW-Probe-Kernel (`MetalRenderer.mm` um 2680-2700) den Sekundärschattenstrahl wirklich nicht hat (dann HW/SW-Abweichung).
- REN-317: Ob `IRenderer` Kopieren verbietet.
- REN-351: externe Nutzer der `ShaderManager`-Klassen außerhalb von `src/`.
- Nicht aufgenommen, weil vermutlich vom Compiler zusammengefasst: doppeltes Laden von `uCurrent`/`uVelocity` im TAA-Resolve (`HlslSources.h:630-645`, `657-661`); im Shader-Disassembly prüfen, falls das Pass-Budget knapp ist.
- Der Kommentar in `kGiShadowCSHLSL` (`963-966`, `break`/`continue` hebelten `[unroll]` aus) passt nicht zum A-Trous-Shader, der `[unroll]` mit `continue` verwendet (`1100-1105`, kompiliert offenbar). Die tatsächliche Ursache von X3511/X3500 ist vermutlich das dynamisch indizierte Vektor-Lvalue, nicht `continue`. Der Kommentar ist ggf. ungenau (Verständnisfrage, kein Funktionsfehler).

**Rendering_4.md**

- **REN-401**: tatsächliche Dauer von `UpdateSkyEnvCube` auf M-Serie bei Auto-Advance (Kommentar nennt ~60 ms seriell; parallel unbekannt). Profil: `HE_PROFILE_SCOPE_N` um den Bake, Instruments Time Profiler.
- **REN-405/406**: Gewinn von TLAS-Refit vs. Neubau und Hitch-Dauer beim Laden – Metal System Trace (Acceleration-Structure-Build-Spuren) mit 1k/10k Instanzen.
- **REN-407**: Anteil von Cull+Sort an `Metal::EncodeShadowMap` bei 50k Entities, 3 Kaskaden + 4–8 Lokal-Lichtern.
- **REN-413**: GPU-Kosten des Cloud-Shadow-Passes (512²) im Stil „Realistic" vs. Classic; ob Zeitquantisierung sichtbar ruckelt.
- **REN-422**: Bandbreitengewinn von Ping-Pong statt Blit auf Apple-GPUs (TBDR; ggf. kleiner als gedacht); Bloom-Kette → Mip-Pyramide ändert das Bild.
- **REN-424/428**: Alle Shader-Änderungen nur mit Metal GPU Counters/Capture-A/B (kein Messwert im Repo vorhanden).
- **Forward-SSAO-Pre-Pass** (EncodeSSAO 12353ff, nicht als Fund aufgeführt): re-rasterisiert in der Forward-Pfad die gesamte Geometrie (zusätzliche View-Space-Position-Pre-Pass-Geometrie); ob ein vorhandener Depth-Pre-Pass wiederverwendbar ist, hängt vom Gesamtdesign ab – Architekturentscheidung, nicht aus dieser Datei allein ableitbar.
- **Custom-Material-Draws sind nie instanziert** (14636, Kommentar „optics before draw-call savings"): potenziell viele Draws bei Foliage/Partikeln mit Graph-Material; Gewinn nur messbar mit Szenen-Daten.
- **Frame-Retire-Konstante 3** (`RetireTexture`/`RetireGIObject` 11373, 11404) statt Command-Buffer-Completion: bei >3 Frames GPU-Latenz theoretisch Use-after-free möglich; nicht beobachtet, Validation-Layer prüfen.
- **Shadow-Rendering pro Layer separater Encoder** (7977): alle Kaskaden in einem Pass über `render_target_array_index` rendern wäre möglich; Gewinn unklar.

**Rendering_6.md**

- REN-605: tatsächlicher Anteil von Cull+Sort am Schattenpass (CPU) und GPU-Kosten der lokalen Ebenen — `GpuPassScope "Shadow"` und CPU-Zeit in `renderDepthLayer` messen, bevor Caching gebaut wird.
- REN-603/604: wie viele UI-Quads pro Frame im realen Spiel/Editor-Viewport vorkommen (nur Zähler `m_counters.draws` zeigt die UI nicht gesondert); Frosted-Anteil.
- REN-608: ob Orphaning von `m_instanceVBO` auf Ziel-Treibern (NVIDIA/AMD/Intel/Mesa/macOS) real kostet.
- REN-612: Kosten von `glGetUniformLocation` sind treiberabhängig (≈ 0,1–2 µs).
- REN-615: Ob `GpuParticleParams.count` bereits mit `coverage` skaliert wird (Aufrufer in `HE_Scene`/`HE_Rendering` prüfen, `SetGpuParticleParams`-Caller); falls ja, entfällt der Gewinn.
- REN-616: Tatsächliche Dauer des `BuildSkyEnvCube` in Release (Kommentar: „few ms“) und Auswirkung einer 64²/RGBA16F-Reduktion auf das Ambient.
- REN-618: Ob Wind-Materialien mit Instanz-Läufen in realen Szenen vorkommen (dann N Draws + N UBO-Uploads).
- REN-625: Größe der Spitzen beim Szenenladen (Zeit in `ResolveMesh`/`uploadTextureAssetGL`, vermutlich dominiert von Texturen).
- REN-626: Ob `WarmupMaterials` in allen Lade-Pfaden (Editor, Game, Hot-Reload) greift; falls ja, ist der Fund rein theoretisch.
- REN-628: GPU-Zeiten der Kandidaten-Passes aus dem Profiler (`GpuPassScope`) vor jeder Shader-Änderung.
- Ob `GLWireScope`/`Wireframe`-Pfade und Deferred-Pfad dieselben Memo-Strukturen brauchen (Deferred ist per `HE_HAVE_SHADERC` gated; Messung dort separat).

**Rendering_7.md**

- REN-702/703/705/707: tatsächliche CPU-Kosten des Software-Renderers bei realen Anwendungs-UIs (Quad-Zahl, Fenstergröße) messen; ohne Messung bleiben es Schätzungen.
- REN-704: Kosten der TAA-Velocity-Map pro Frame bei großen Szenen (10k+ Objekte) per Profiler (CPU-Zeit in `RenderVelocity`).
- REN-709: Anzahl der Deskriptor-Writes pro Material-Draw in `VulkanRenderer.cpp:8559` (nicht Teil dieses Slice).
- Ob `UIRenderObject` Heap-Member enthält (bestimmt die Kosten der Kopie in REN-705): `Renderer/UIRenderObject.h` prüfen.
- REN-701: Anzahl tatsächlich fehlender Uniforms (Skinned-Wetness/Snow) – ob absichtlich oder Versäumnis, nicht verifiziert.
- Per-Draw `glUniform*`-Aufrufe im Opaque-Loop (mutmaßlich viele; liegt in `OpenGLRenderer.cpp`, nicht Teil dieses Slice).

**Rendering_8.md**

- Bloom: 10 getrennte Blur-Passes (1000-1012, je eigener Renderpass + Barrier, Half-Res): ob ein Dual-Kawase/Downsample-Kette mit weniger Passes sichtbar gleich aussieht (Verhaltensänderung!) – nur mit Bildvergleich entscheiden; GPU-Zeit via `gpuTimerBegin/End` (14646) pro Pass messen (derzeit nur „whole-frame“).
- Wie teuer `resolveWorldMaterialScalars`, `refineObjectBounds` (je Objekt `resolveMesh`-Hash-Lookup, 6889-6891) und `cull+sort` bei 50k Objekten sind – Zeitscopes um 3890, 6889, 7840 setzen, bevor REN-805 priorisiert wird.
- Ob `GpuMesh`-Zugriffe `m_meshCache.find(UUID)` pro Draw (7721) relevant sind (Hash von 128-Bit-UUID); ein direkter Zeiger im `RenderObject` (Mesh-Handle) würde sie sparen – Cross-Modul (Extractor).
- Bestätigung, dass die `m_giNodeBuf`/`m_giTriBuf`-Neuallokation bei Wachstum (10317-10329, 10185-10190) nie ein in-flight Frame liest (Kommentar sagt „VK-B revisits“): möglicher Synchronisationsfehler, nicht verifiziert.
- `m_shaderManager = VulkanShaderManager();` (216) – ob die Zuweisung nach den Pipeline-Erzeugungen etwas überschreibt, das vorher befüllt wurde (Header `VulkanRenderer.h`, Slice 9, prüfen).
- Wirkung von REN-803 (Device-Local) hängt von der GPU-Klasse ab (dGPU vs. UMA/iGPU/lavapipe): Messung auf Zielhardware.
- `k_matMaxDraws = 1024` / `k_maxMeshTextures = 1024` / `k_maxDecals` / `k_sceneMatSlots = 16384` / `kMaxSkinnedVk = 256` sind harte Obergrenzen; reale Szenengrößen der Zielprojekte nicht bekannt.

**Rendering_9.md**

- REN-904: Anteil der Sky-Pass-Zeit an der GPU-Frame-Zeit (1080p/4K, Wolken aus vs. an, Low-Res-Cloud-Pfad); erst dann entscheiden, ob eine Sky-View-LUT lohnt.
- REN-905: Kosten der Aurora-Parameterberechnung pro Fragment (GPU-Timer, Aurora an/aus, Nacht); der Shader-Compiler kann `auroraRnd(fk, c)` bei entrolltem Loop teilweise konstant falten; auf Metal/Vulkan/D3D-HLSL unterschiedlich.
- REN-908: Haeufigkeit des Rebake (Zeitraffer, Day-Night-Zyklus) und Dauer pro Bake (parallel_for ueber 768 Zeilen); ob 64² Faces optisch ausreichen.
- REN-909: Dauer von `FrustumCuller::cull` bei 10k/50k Objekten; Nutzen eines Hot/Cold-Splits.
- REN-910/915: Mikrokosten (atomare Refcounts, Switch pro Instanz) – bei typischen Foliage-Szenen vermutlich < 0,1 ms; nur mit Profiler (`ExtractFoliage`-Scope existiert) entscheiden.
- REN-903: Einfluss der Cluster-Vektor-Allokation auf die CPU-Frame-Zeit in Szenen mit 50-256 Lichtern; `glm::inverse` → View-Spalte nur umsetzen, wenn der Test bitgleich bleibt.
- REN-912: GPU-Wartezeit der beiden `vkDeviceWaitIdle` bei mehreren offenen Preview-Fenstern (Profiler-Scope um den Aufruf).
- `VulkanRenderer.h:443`: Kommentar "key = hash ^ hdr-bit" passt nicht zur Signatur `GetOrBuildMaterialPipeline(…, hdr, transparent, gbuffer)` (Zeile 152-155); ob `transparent`/`gbuffer` im Schluessel stecken, steht in der .cpp (nicht in diesem Slice) – bei Abweichung Kollisionsrisiko bzw. veralteter Kommentar.
- `VulkanRenderer.h:1478-1481`: Kommentar zur Einzel-Slot-Bone-UBO ("only one skinned mesh pose renders correctly per frame") passt eventuell nicht mehr zum aktuellen Code (`m_previewBoneRing` hat pro Draw einen Block); in der .cpp pruefen.

**Rendering_10.md**

- Anteil der Meshes mit `sections.size() == 1` (bestimmt den Gewinn aus REN-1005).
- Tatsächliche Zahl Graph-Material-Draws pro Frame und Kosten von Kopie+Hash (REN-1001) – Profiler-Scope um `resolveShaders`.
- D3D12-Sinkcode jenseits Zeile 10614: liest der Shadow-Zweig wirklich nie `cmds`? (REN-1004).
- Verbleibende Bake-Zeit von `BuildSkyNoise3D(256)` nach der Jitter-Tabelle und Anzahl der Renderer-Instanzen im Editor (REN-1015).
- Ob die GL/Metal/D3D-Fassungen von heLitP/`localShadowFactor` ebenfalls vor dem `atten`-Test auswerten (REN-1007) und ob der Compiler `atten==0` per Dynamic Branching ohnehin ausspart (Treiberabhängig; GPU-Profil).
- Sichtbarkeit des Lese/Schreib-Hazards in `gi_probe.comp` (REN-1023); Verhalten der GL/Metal-Kernel nicht geprüft.
- Wird `std::hash<std::string>` plattformabhängig (libstdc++ Murmur vs. MSVC FNV) als Cache-Schlüssel in persistenten Caches benutzt? (REN-1001/1018 – bisher nur In-Process-Caches gesehen.)
- Sync-Kompilierung beim ersten Draw (`MaterialShaderLibrary::fragment` ruft glslang+SPIRV-Cross inline, D3D11 `:7019` „else cross-compile now“): Backends haben Prewarm-Pfade (`D3D11:5318`, `Vulkan:7669`), Restfälle (Material wird erst zur Laufzeit geladen) können Frame-Hitches erzeugen – messen.
- `MaterialShaderLibrary` ist nicht thread-safe, und `clear()` invalidiert die zurückgegebenen `const Compiled&`-Referenzen (Hot-Reload während Pipelines gebaut werden?) – Nutzung aus mehreren Threads nicht geprüft.


---
# Funde (vollständig, nach Teilbericht)

## Rendering Slice 1 – D3D11-Backend (D3D11Renderer/ShaderManager/MaterialBindings) + D3D12MaterialRootSignature.h

Dateien (alle unter `src/HE_Rendering/`): `include/Backends/D3D11/D3D11MaterialBindings.h`, `src/Backends/D3D11/D3D11Renderer.cpp` (8601 Z.), `include/Backends/D3D11/D3D11Renderer.h`, `src/Backends/D3D11/D3D11ShaderManager.cpp`, `include/Backends/D3D11/D3D11ShaderManager.h`, `include/Backends/D3D12/D3D12MaterialRootSignature.h`.

## Zusammenfassung

Der Code ist gründlich kommentiert, funktional sauber und für D3D11 sinnvoll aufgebaut (Instancing für Depth-/Szenen-Pässe, Dynamic-CBs mit `MAP_WRITE_DISCARD`, GPU-Timer-Ring ohne Stall). Die größten Hebel liegen im Hot Path der Draw-Schleife (`drawDC`, Graph-Material-Zweig): dort werden pro Draw GLSL-Strings kopiert und gehasht, Textur-Schlüssel als `std::string` gebaut und rund 25 Binds + Restores abgesetzt. Danach folgen per-Frame-Doppelarbeit (Cluster-Build/Upload zweimal, GI-Instanzbuffer wird jedes Frame neu angelegt, volle Probe-Atlas-Kopie), eine ca. 1500 Zeilen lange `DrawScene`-Funktion und ein 4700-Zeilen-`Impl`-Struct mit vielfach kopierten Lambdas (`makeRT` 6x, `makeCB` 6x). Insgesamt 23 Funde: 3 Hoch, 10 Mittel, 10 Niedrig. Alle Performance-Aussagen sind Schätzungen ohne Messdaten (siehe "Unsicher / Messung nötig").

---

### REN-101 – Graph-Material-Draw kopiert und hasht pro Draw den kompletten GLSL-Fragment-String
- **ID**: REN-101
- **Kategorie**: D (wiederholtes Hashing/Kopieren im Hot Path), C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:7006-7041` (Aufrufer); `src/material/MaterialShaderLibrary.cpp:941-952, 1000-1009` (Slice 10, Ursache); `D3D11Renderer.cpp:7021-7022`
- **Problem**: Für jeden Draw eines Graph-Materials ruft `drawDC` `resolveShaders()` auf. Das kopiert `mat->customShaderFragGlsl` (typisch mehrere KB, Heap-Allokation) nach `matFrag`, berechnet `std::hash<std::string>` über den ganzen Text, kopiert zusätzlich `customShaderVertGlsl` und hasht es erneut. Erst danach folgt der `m_materialShaders`-Lookup per Hash, der in der Regel ein Cache-Treffer ist, sodass Kopie und Hash umsonst waren. Zusätzlich wird `precompiledFor(getMaterial(...))` (Z.7021) berechnet, bevor man weiß, dass es gebraucht wird (nur bei Cache-Miss nötig), und `getMaterial(dc.materialAssetId)` wird im selben Draw noch dreimal gelöst (Z.7022, 7077, 7114) plus `resolveMaterialOverride` (Hash-Lookup). Alle fünf Backends (GL, Vulkan, D3D12, Metal, D3D11) rufen dieselbe `resolveShaders`.
- **Code-Auszug**:
```cpp
uint64_t matHash = 0; std::string matFrag, matVertBody;
if (p.m_matShaderLib.resolveShaders(*m_contentManager, dc.materialAssetId, matHash, matFrag, matVertBody))
{ ...
    const MaterialShaderVariant* matPre = HE::MaterialShaderLibrary::precompiledFor(
        m_contentManager->getMaterial(dc.materialAssetId), HE::RendererBackend::D3D11);
// MaterialShaderLibrary.cpp
glslOut = mat->customShaderFragGlsl;
hashOut = std::hash<std::string>{}(glslOut);
```
- **Lösung**: (1) Pro Frame (oder bis `InvalidateMaterial`) ein kleiner Cache `materialAssetId -> {hash, MatShaders*, matPre, const MaterialAsset*}` im Renderer; die String-Kopie und der Hash laufen nur beim ersten Auftreten bzw. nach Invalidierung. (2) Alternativ `resolveShaders` zerlegen: `bool resolveShaderKey(cm, id, uint64_t& hash)` mit gecachtem Hash am `MaterialAsset` (Generationszähler), und Strings nur bei Cache-Miss holen (`const std::string&` statt Kopie). (3) `precompiledFor` und `frag/vertBody` nur im Miss-Pfad von `GetOrBuildMaterialShaders` auswerten. Skizze:
```cpp
struct MatDrawKey { uint64_t hash; D3D11RendererImpl::MatShaders* sh; const MaterialAsset* ma; };
auto it = p.matKeyCache.find(dc.materialAssetId);          // geleert in processPendingInvalidations()
if (it == end) it = p.matKeyCache.emplace(id, buildKey(...)).first;   // einmalig resolveShaders + GetOrBuild
```
- **Erwarteter Effekt** (Schätzung): Bei einigen hundert Graph-Material-Draws pro Frame fallen pro Draw eine String-Allokation/-Kopie von mehreren KB und ein Hash über denselben Text weg; geschätzt 5-20 µs CPU pro Draw, bei 500 Draws also 2-10 ms CPU pro Frame. Zu messen.
- **Risiko / Verifikation**: Cache muss bei `InvalidateMaterial`, Material-Hot-Reload und `Shutdown` geleert werden (`processPendingInvalidations`, Z.5271ff.). `tests/test_material_graph.cpp` (WARP-Fälle) deckt den D3D11-Pfad ab; zusätzlich ein Test, dass nach `InvalidateMaterial` mit geändertem GLSL ein neues Shaderpaar entsteht. Dieselbe Änderung in den vier anderen Backends (Parallelstellen unten in Cross-Modul).

---

### REN-102 – Pro Draw werden Textur-Cache-Schlüssel als `std::string` gebaut und Pfade kopiert
- **ID**: REN-102
- **Kategorie**: C, D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:7069-7111`, `4947-4961`, `1453-1456`
- **Problem**: Im Graph-Material-Zweig wird pro Draw ein `std::string gPaths[4]` angelegt und aus `graphTexturePaths` kopiert (Heap bei Pfaden > 15 Zeichen). Danach baut jeder `resolveGraphTexture`-Aufruf den Cache-Schlüssel neu: `graphTexKey()` macht zwei `std::to_string` plus Konkatenation, bei Array-Slots kommt `key += "#arr"` dazu. Das geschieht für bis zu 4 Textur-Slots und zusätzlich für die Weightmap (bei Nicht-Landscape-Draws jedes Mal für `kDefaultLayer0WeightTextureId`). Ergebnis ist pro Draw ein Dutzend kleiner Heap-Operationen für ein Ergebnis, das sich zwischen zwei Frames nicht ändert.
- **Code-Auszug**:
```cpp
static std::string graphTexKey(const HE::UUID& id, const std::string& path)
{ return id != HE::UUID{} ? (std::to_string(id.hi) + ":" + std::to_string(id.lo)) : path; }
...
if (!heWeights)
    heWeights = p.resolveGraphTexture(HE::kDefaultLayer0WeightTextureId, {}, m_contentManager);
```
- **Lösung**: (1) UUID-keyed Lookups direkt über `unordered_map<HE::UUID, ...>` statt String-Key (Pfad-Keys in eine zweite Map `unordered_map<std::string,...>` nur für lose Editor-Assets); `#arr` als zweiter UUID-Map-Eintrag oder Schlüssel `{UUID, bool}`. (2) Das aufgelöste Slot-Array (`heTexP[4]`, `heWeights`, `heTex0`) pro Material (nicht pro Draw) in dem Cache aus REN-101 halten und bei `InvalidateTexture/Material/Mesh` verwerfen. (3) Die Default-Weightmap einmal als Member (`ID3D11ShaderResourceView* defaultWeights`) auflösen.
- **Erwarteter Effekt** (Schätzung): Pro Draw 8-15 Heap-Allokationen und mehrere Hash-Berechnungen über Strings weniger; zusammen mit REN-101 der größte CPU-Hebel des Graph-Material-Pfads.
- **Risiko / Verifikation**: Das Verhalten "Null wird gecacht (kein Retry pro Frame)" und "Pfad-Schlüssel wird nicht hot-reloaded" (Kommentar Z.1443-1447) muss erhalten bleiben. `resolveTextureRef` kann synchron laden und Pointer verschieben (Kommentar Z.4940-4942): gecachte `MaterialAsset*` nicht über einen Resolve hinweg halten. Test: `tests/test_material_graph.cpp` (Texture Sample / Texture Array Sample).

---

### REN-103 – Jeder Graph-Material-Draw bindet ~25 Ressourcen und stellt den Built-in-Zustand komplett wieder her; HeParams wird pro Instanz neu hochgeladen
- **ID**: REN-103
- **Kategorie**: B, D (Render-State-Wechsel, Doppel-Uploads)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:7119-7248` (Bind + Restore), `7163-7199` (`drawMatInstance`), `D3D11MaterialBindings.h:67-133, 204-222`
- **Problem**: Pro Draw: VS/PS/IL, 2 CB-Binds, Tex0, 4x TexP, 2 Sampler-Calls, `BindLandscapeWeights` (2), `BindDDGIAtlases` (4), `BindSkyEnvAndAO` (3), VB/IB. Direkt danach Restore: `RestoreAfterMaterialDraw` (2), `RestoreBuiltinSkyEnvAOSlots` (2), `RestoreBuiltinGISlots` (4), VS/PS/IL, 2 CBs, t2, `PSSetShaderResources(4,4)`, Sampler s2. Das sind über 45 Context-Calls pro Draw, auch wenn der nächste Draw dasselbe Material nutzt und der Restore sofort wieder überschrieben wird. Die Bindings, die nur von Frame-Zustand abhängen (DDGI-Atlanten, Sky-Cube, AO, Cluster-Listen, Sampler s0/s1/s3/s15), ändern sich zwischen Draws überhaupt nicht. Außerdem steht der HeParams-Upload (`float padded[64]`, `Map/Unmap`, memcpy) im pro-Instanz-Lambda, obwohl `params` für alle Instanzen eines DrawCalls identisch ist; bei `instanceTransforms` mit N Einträgen werden U **und** HeParams N-mal gemappt.
- **Code-Auszug**:
```cpp
auto drawMatInstance = [&](const glm::mat4& model) {
    ... Map(p.m_matObjCB ...)
    float padded[64] = { 0.0f };
    if (params) std::memcpy(padded, params->data(), ...);
    ... Map(p.m_matParamCB ...)   // identisch für jede Instanz
```
- **Lösung**: (1) HeParams-Upload vor `if (!dc.instanceTransforms.empty())` einmal pro DrawCall ausführen (Verhalten identisch, Risiko minimal). (2) Frame-konstante Bindings (Atlanten, Sky-Cube, AO, Sampler s0/s1/s3/s15) einmal beim Eintritt in eine Folge von Graph-Material-Draws binden und erst beim Wechsel zurück zu einem Built-in-Draw bzw. am Pass-Ende wiederherstellen (kleine Zustandsmaschine `enum { Builtin, Material }` in `drawDC`). (3) Optional: Sortierung des opaken Passes nach Material-Hash (falls `RenderSorter` das nicht schon tut) und Binds nur bei Wechsel von `sh`, `heTex0`, `heTexP`.
- **Erwarteter Effekt** (Schätzung): D3D11 ist treiber-CPU-lastig; 30-40 eingesparte Calls pro Draw dürften bei 500-1000 Material-Draws 1-3 ms/Frame CPU bringen. HeParams-Hoisting spart bei instanzierten Draws 1 Map/Unmap pro Instanz.
- **Risiko / Verifikation**: Die Reihenfolge-Fallen sind im Code dokumentiert (t16/SSR, s0 Albedo-Sampler, DSV/SRV-Konflikte); das WARP-Test-Setup in `tests/test_material_graph.cpp` (verwendet `D3D11MaterialBindings.h`) sollte um "zwei Material-Draws hintereinander, danach Built-in-Draw" erweitert werden, damit ein vergessener Restore auffällt. Mit PIX/RenderDoc gegenprüfen, dass die Bildausgabe bit-gleich bleibt.

---

### REN-104 – Cluster-Licht-Build und -Upload laufen zweimal pro Frame; der Kommentar behauptet "einmal"
- **ID**: REN-104
- **Kategorie**: D (Doppelarbeit), C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:6317-6368` (`uploadClusters`), `6373-6389` (`fillPerFrame`), `6439` und `6869` (zwei Aufrufe)
- **Problem**: `uploadClusters` ruft `HE::BuildClusterLights` (CPU-Scatter über alle Lichter/Zellen) und lädt danach 3 strukturierte plus (bei `matClustered`) 3 rohe Buffer per `Map(DISCARD)` hoch. `fillPerFrame` wird einmal vor den Pässen (Z.6439) und einmal nach GI/SSAO (Z.6869) aufgerufen und ruft jedes Mal `uploadClusters`. Der Kommentar darüber sagt "Built once per frame"; tatsächlich läuft alles doppelt, obwohl sich nur die GI-Maskenkanal-Lane (`giActive && giLocalMaskSRV`) unterscheiden kann. Zusätzlich wird `BuildDirectionalLightWindow` und `FillMaterialLightWindow` (fillMatLight, Z.6569 und 6871) doppelt ausgeführt.
- **Code-Auszug**:
```cpp
// "Built once per frame — the light set and camera do not change between the two fillPerFrame calls below"
auto uploadClusters = [&](bool giActive) {
    frameClusters = HE::BuildClusterLights(p.m_renderWorld, localShadows, giActive && p.giLocalMaskSRV);
```
- **Lösung**: Den ersten `fillPerFrame/fillMatLight`-Aufruf (Z.6439/6569) nur ausführen, wenn zwischen ihm und Z.6869 etwas den Per-Frame-CB liest (laut Code liest keiner der Prepässe `perFrameCB`/`m_matLightCB`: Schatten, SSAO, GI-G-Buffer, SSR haben eigene CBs; vor dem Verlassen von `if (!kBackbufferTarget)` wird nicht gezeichnet). Falls doch nötig: `BuildClusterLights` nur beim zweiten Mal neu ausführen, wenn sich `giActive` geändert hat (`giActive` ist im ersten Aufruf `false`), sonst Ergebnis wiederverwenden.
- **Erwarteter Effekt** (Schätzung): Halbiert die CPU-Kosten des Cluster-Builds (je nach Lichtanzahl 0.05-0.5 ms) und spart 6 Map/Unmap + CopyBuffer-Bandbreite; bei vielen Lichtern spürbar.
- **Risiko / Verifikation**: Prüfen, dass der Entfall des ersten Fills kein Frame-1-Artefakt (z. B. Sky-Pass, `drawDebugLines`) erzeugt: `drawSky` liest den PerFrame-CB nicht. Test: `tests/test_culling.cpp` (Cross-Copy-Drift), Bildvergleich mit vielen Point-Lights, `HE_FORWARD_CLUSTER=0/1`.

---

### REN-105 – GI-Instanzbuffer wird jedes Frame als IMMUTABLE-Buffer neu erzeugt; `glm::inverse` für jedes Objekt jedes Frame
- **ID**: REN-105
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:3152-3232` (`updateGiAccel`), konkret `3183-3188` und `3199-3230`
- **Problem**: Bei aktivem GI wird pro Frame über **alle** Casting-Objekte iteriert, je Objekt `glm::inverse(obj.transform)` (4x4-Inversion) berechnet und danach `makeSB` aufgerufen. `makeSB` setzt `buf.Reset()`, legt einen neuen `D3D11_USAGE_IMMUTABLE`-Buffer plus SRV an (`CreateBuffer`, `CreateShaderResourceView`) und lässt den alten vom Treiber deferred freigeben. Der Kommentar "counts are small → IMMUTABLE re-create is fine" ist für Szenen mit tausenden Casters nicht belegt; Ressourcen-Erzeugung pro Frame ist Treiber-Allokation und kann Hitches verursachen.
- **Code-Auszug**:
```cpp
inst.invTransform = glm::inverse(obj.transform);
...
makeSB(giInstancesCpu.data(), static_cast<UINT>(giInstancesCpu.size()),
       sizeof(GIInstanceGpu), giInstanceSB, giInstanceSRV);   // CreateBuffer + CreateSRV pro Frame
```
- **Lösung**: (1) Instanzbuffer als `D3D11_USAGE_DYNAMIC` structured buffer mit Kapazität (Wachstum in Zweierpotenzen, wie `uploadRibbon`/`debugVB`) anlegen und per `Map(WRITE_DISCARD)` füllen; SRV nur bei Kapazitätswechsel neu. (2) `invTransform` pro `entityId` cachen und nur bei geänderter `obj.transform` neu berechnen (oder Affin-Inverse über Transponierte/Skalierung statt generischem `inverse`). (3) Optional: Instanzliste nur neu aufbauen, wenn sich Transform-Hash/Objektanzahl geändert hat (Dirty-Flag aus dem Extractor).
- **Erwarteter Effekt** (Schätzung): Bei 1000 Casters entfallen pro Frame ~1000 Inversionen (~50-100 µs) und eine Buffer-/SRV-Neuanlage (Treiberabhängig 20-200 µs, mit Hitch-Potenzial).
- **Risiko / Verifikation**: Die Struktur `GIInstanceGpu` und HLSL `GiInst` bleiben unverändert. `tests/test_gi_bvh.cpp` deckt nur die BVH ab; einen WARP-/Headless-Lauf mit GI an (`HE_GI_LOG_INSTANCES`) zum Vergleich der Probe-Ergebnisse nutzen.

---

### REN-106 – Probe-Update kopiert pro Frame beide kompletten DDGI-Atlanten, obwohl nur `budget` Probes geschrieben werden
- **ID**: REN-106
- **Kategorie**: D (Bandbreite, unnötige GPU-Kopie)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:3727-3734` (`dispatchGiProbeUpdate`)
- **Problem**: Für das Lesen der Vorwerte werden `giIrrTex` (RGBA16F) und `giVisTex` (RG16F) jedes Frame vollständig per `CopyResource` in die Prev-Texturen kopiert. Der Kernel schreibt aber nur `budget` (Standard 256, max 4096) aufeinanderfolgende Probes (`giProbeCursor` rotiert). Bei z. B. 20x10x20 = 4000 Probes à 8x8 Texel ist der Atlas 256x1024 Texel (2 MB + 1 MB); davon werden nur ~6 % wirklich geändert.
- **Code-Auszug**:
```cpp
ctx->CopyResource(giIrrPrevTex.Get(), giIrrTex.Get());
ctx->CopyResource(giVisPrevTex.Get(), giVisTex.Get());
```
- **Lösung**: Nur die Region der aktuellen Probe-Batch kopieren: Probes liegen zeilenweise (`giProbesPerRow = min(count, 32)`), die Batch `[cursor, cursor+budget)` belegt wenige Tile-Zeilen. `CopySubresourceRegion` mit Box der betroffenen Zeilen (bei Wrap-around zwei Boxen). Die Prev-Textur ist an allen anderen Stellen laut Kernel-Kommentar nicht gelesen ("texels outside this batch keep their values since the kernel never writes them"); Prev muss dort aber bei Leseversatz identisch zu Canon sein, daher Regionen, die der Kernel liest (nur die eigenen Tiles), genügen.
- **Erwarteter Effekt** (Schätzung): GPU-Bandbreite -90 % für diesen Schritt (einige MB pro Frame), vernachlässigbar auf großen GPUs, relevant auf iGPUs/Laptops.
- **Risiko / Verifikation**: Hängt davon ab, dass `GiProbeCS` ausschließlich Texel seiner eigenen Probe-Tiles als Vorwert liest (HLSL `kGiProbeCSHLSL` in `D3D_Shared/HlslSources.h` prüfen; ich habe sie nicht gelesen). Unsicher bis dahin; erst messen (GPU-Zeit des Schritts), dann umbauen. Test: gleiche Atlas-Bytes nach N Frames mit alter und neuer Variante (WARP, Compute 5.0).

---

### REN-107 – `DrawScene` ist eine 1466-Zeilen-Funktion mit 350-Zeilen-`drawDC`-Lambda, Flag-Variablen und tiefer Verschachtelung
- **ID**: REN-107
- **Kategorie**: A (zu lange Funktionen, Flag-Parameter, Verschachtelung)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `D3D11Renderer.cpp:6178-7643` (`DrawScene`), darin `6981-7337` (`drawDC`), `6597-7642` (Render-Graph-Callback)
- **Problem**: Eine Funktion erledigt Extraktion, Cull, Sort, Cluster, Shadow-Pass, GI, SSAO, SSR, Deferred, Skinned, Decals, Transparenz. Zustand wird über Lambda-Captures per Referenz und mutable Flags (`gbufferPass`, `allowInstancing`, `builtinPS`, `deferredForwardDCs`) zwischen den Phasen gereicht. Die Verschachtelung im Graph-Material-Zweig von `drawDC` erreicht 8-9 Ebenen (`if matReady` → `if resolveShaders` → `if (sh && ...)` → Lambda → `if (params)`). Rekursives Verständnis und Test einzelner Phasen sind kaum möglich; fast jede Änderung (REN-101/103) berührt dieselbe Funktion.
- **Code-Auszug**:
```cpp
bool gbufferPass = false;
ID3D11PixelShader* builtinPS = p.ps.Get();
std::vector<const DrawCall*> deferredForwardDCs;
auto drawDC = [&](const DrawCall& dc) { ... 350 Zeilen ... };
```
- **Lösung**: In Member-Funktionen des Impl (oder eine `FrameContext`-Struct mit den Frame-Konstanten: `viewProj`, `aoSRV`, `ssrSRV`, `giShadingActive`, `localShadowSrv_`) zerlegen: `renderShadowPass`, `runPrepasses`, `drawOpaque`, `drawDeferred`, `drawSkinned`, `drawTransparent`. `drawDC` in `drawBuiltinDC` und `drawMaterialDC` teilen, das Flag `gbufferPass` zu einem `enum class PassKind { Forward, GBuffer }`-Parameter machen. Rein mechanisches Extract-Function, kein Verhaltenswechsel.
- **Erwarteter Effekt**: Keine Laufzeitänderung; deutlich geringere Fehleranfälligkeit und kürzere Compile-Zeit je Änderung (Schätzung).
- **Risiko / Verifikation**: Nur Refactoring, aber Draw-Reihenfolge und State-Restore sind subtil (Kommentare Z.6824ff.); als Absicherung Bildvergleich Forward/Deferred/SSR/TAA mit `HE_DUMP_GBUFFER` und den vorhandenen WARP-Tests. Am besten gemeinsam mit REN-103 umbauen, damit nicht zweimal angefasst wird.

---

### REN-108 – `D3D11RendererImpl` ist ein 4700-Zeilen-God-Object, und 1135 Zeilen HLSL stecken im selben `.cpp`
- **ID**: REN-108
- **Kategorie**: A, F (Struktur, schwere Übersetzungseinheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `D3D11Renderer.cpp:1317-6037` (Impl), `86-1220` (HLSL-Literale: `kSkyPSHLSL`, `kSceneHLSL`, `kAABlitHLSL`, `kSmaaHLSL`, `kFxaaHLSL`, `kBloom*HLSL`, `kUIHLSL`)
- **Problem**: Der Impl-Struct enthält ca. 450 Member (SSAO, SSR, GI, TAA, Decals, Deferred, Sky, UI, Preview, GPU-Timer) und deren Pipeline-/Target-/Pass-Funktionen. Jedes Teilsystem hat die Form `xxxReady/xxxFailed` + Shader + CB + Targets + `ensureXxx()/destroyXxx()`, aber ohne Kapselung. Dazu kommen ~1135 Zeilen Shader-Quelltext, die bei jeder C++-Änderung mit-kompiliert werden und beim Lesen von Logik stören. Das D3D12-Backend hat dasselbe Muster (`kFxaaHLSL12`, `kSmaaHLSL12`, `kUIHLSL12`, `D3D12Renderer.cpp:945-1200`), `D3D_Shared/HlslSources.h` hat bereits ~750 Zeilen herausgelöst.
- **Code-Auszug**:
```cpp
struct D3D11RendererImpl {
    ... // 4700 Zeilen
    bool ssrReady = false; bool ssrFailed = false; bool ssrEnabled = false; ...
    bool decalReady = false; bool decalFailed = false; ...
```
- **Lösung**: Pro Teilsystem eine eigene Struct/Datei (`D3D11Ssao.cpp`, `D3D11Ssr.cpp`, `D3D11Gi.cpp`, `D3D11Taa.cpp`, `D3D11Decals.cpp`, `D3D11Deferred.cpp`, `D3D11Ui.cpp`) mit `Init(device)`, `Resize(w,h)`, `Record(ctx, FrameInputs)`; die State-Flags (`ready/failed`) in ein kleines `LazyPipeline`-Hilfsobjekt. Embedded HLSL für Post-Passes (FXAA/SMAA/Bloom/AABlit/UI) in `D3D11Hlsl.cpp` bzw. besser nach `D3D_Shared/HlslSources.h` mit einem `#define`/Prefix für den `_dummy`-Unterschied von D3D12 (siehe Kommentar im Header) auflösen.
- **Erwarteter Effekt**: Schnellere inkrementelle Builds (nur das geänderte Teilsystem), bessere Testbarkeit; Laufzeit unverändert.
- **Risiko / Verifikation**: Größerer, rein mechanischer Umbau. Der Test `tests/test_culling.cpp` greift den Text der Szenen-HLSL ab (Kommentar Z.750); beim Verschieben des Shader-Texts muss dieser Pfad angepasst werden. Schrittweise (je Teilsystem ein PR) mit WARP-Tests.

---

### REN-109 – Dieselben Helfer-Lambdas sind im Impl 6-7x kopiert (`makeRT`, `makeCB`, `compile`)
- **ID**: REN-109
- **Kategorie**: A (Redundanz)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `makeRT`: Z.1808, 2207, 2367, 2408, 2827 (+ `colour` Z.5737, `makeTex` Z.3337); `makeCB`: Z.2776, 3273, 4307, 4563, 5206; `compile`: Z.3259, 3790, 3887, 5157, 5396, 5706; manuelle `D3D11_BUFFER_DESC`-Blöcke an Dutzenden Stellen (z. B. Z.2276, 2303, 2309, 3844-3868, 4193-4200, 5800-5805)
- **Problem**: `makeRT` (Texture2D + RTV + SRV mit `BIND_RENDER_TARGET|SHADER_RESOURCE`) existiert in fünf praktisch gleichen Varianten, `makeCB` (Dynamic-CB auf 16 B aufgerundet) fünfmal, `compile` (D3DCompile + Logging) sechsmal mit leicht verschiedenem Fehlertext. Teilweise gibt `makeRT` keinen Fehler zurück (`ok = makeRT & makeRT`, Z.2425 und Z.2844 mit `&` bzw. `&=` statt `&&`: absichtlich ohne Kurzschluss, aber inkonsistent zu Z.1823 mit `&&`). Fehler beim Erzeugen von RTV/SRV werden nur über `rtv && srv` abgefangen.
- **Code-Auszug**:
```cpp
auto makeRT = [&](DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11RenderTargetView>& rtv,
                  ComPtr<ID3D11ShaderResourceView>& srv) -> bool {
    D3D11_TEXTURE2D_DESC td{}; ... // 5x kopiert
```
- **Lösung**: Drei Member-Helfer im Impl: `bool createColorTarget(fmt, w, h, Target& t)` mit `struct Target { Tex, RTV, SRV, UAV }` plus `reset()`; `ComPtr<ID3D11Buffer> createDynamicCB(UINT bytes)`; `ComPtr<ID3DBlob> compileHlsl(src, entry, profile, tag, flags)` (einziger Ort für `D3DCOMPILE_DEBUG|SKIP_OPTIMIZATION`, Fehlertext-Kürzung wie in `makePS` Z.2757-2765). Damit entfallen ~300 Zeilen und die `Reset()`-Orgien (Z.2358-2365, 2809-2815, 3511-3524) werden zu `target.reset()`.
- **Erwarteter Effekt**: -300 Zeilen, einheitliche Fehlerbehandlung.
- **Risiko / Verifikation**: Gering; Formate/Flags je Aufrufer als Parameter durchreichen (UAV-Varianten bei GI, Depth-Typeless bei Viewport). Smoke-Run mit `HE_HIDDEN_WINDOW=1`.

---

### REN-110 – Alle ~30 Shader werden bei jedem Start zur Laufzeit mit `D3DCompile` kompiliert, teilweise dieselbe Quelle mehrfach, ohne Disk-Cache
- **ID**: REN-110
- **Kategorie**: D (wiederholtes Parsen/Kompilieren), E (blockierend beim Start)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:4145-4189` (`createPipeline`: `sceneSource` 5x für `VSMain/PSMain/VSDepth/VSDepthInstanced/VSMainInstanced`), `2253-2271` (SSAO-Pos 3x), `3285-3301` (`kGiGBufHLSL` 3x), `3805-3816`, `5171-5182`; 29 `D3DCompile(`-Aufrufe in der Datei
- **Problem**: FXC parst und optimiert jedes Mal den kompletten Quelltext. Die Szenen-Quelle (Sky-Funktion + `kSceneHLSL`, ca. 700 Zeilen mit entrollten PCF-Schleifen und DDGI-Schleife) wird allein in `createPipeline` fünfmal kompiliert, danach nochmals in `createDeferredPipelines` (Z.5171) für `GBufPS`; `sceneSource` wird dort per `std::string(kSkyFuncHLSL) + kSceneHLSL` ein zweites Mal gebaut. Ein Compile-Cache (z. B. Hash der Quelle + Flags → `ID3DBlob` auf Platte) fehlt; die Material-Shader-Pfade besitzen immerhin einen In-Memory-Cache (`m_materialShaders`) und gebackene Pak-Varianten.
- **Code-Auszug**:
```cpp
const std::string sceneSource = std::string(kSkyFuncHLSL) + kSceneHLSL;
D3DCompile(sceneSource.c_str(), ..., "VSMain", "vs_5_0", ...);
D3DCompile(sceneSource.c_str(), ..., "PSMain", "ps_5_0", ...);
D3DCompile(sceneSource.c_str(), ..., "VSDepth", ...);        // + VSDepthInstanced, + VSMainInstanced
```
- **Lösung**: (1) `sceneSource` einmal als `static const std::string` bauen und an `createDeferredPipelines` weiterreichen. (2) Kleiner Shader-Blob-Cache auf Platte (`%LOCALAPPDATA%/Horizon/shadercache/<hash>.bin`, Schlüssel = Quelle + Entry + Profil + Flags + FXC-Version) hinter `compileHlsl` (REN-109). (3) Alternativ vor dem Ablegen im Pak (`asset_compiler`) vorkompilieren, analog zu `MaterialShaderVariant`.
- **Erwarteter Effekt** (Schätzung): Start auf der Windows-Plattform um einige hundert ms bis Sekunden schneller; Shader-Hitches bei Ressourcen-Neuanlage (z. B. Shadow-Resize) bleiben unberührt.
- **Risiko / Verifikation**: Cache muss bei Änderung der Quelle/Flags invalidieren (Hash reicht). Unsicher: tatsächliche Compile-Zeit je Shader; Start einmal mit Stoppuhr um `createPipeline()` messen (`HE_LOG_INFO` mit `SDL_GetTicks`) vor jeder Arbeit.

---

### REN-111 – UI-Pass: pro Quad ein `Map(DISCARD)` + `Draw`, `RSGetState` pro Pass, kein Batching
- **ID**: REN-111
- **Kategorie**: D, B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:5901-5945` (`renderUIPass`), Header `D3D11Renderer.cpp:2026` (Kommentar zu `uiCB`)
- **Problem**: Jedes `UIRenderObject` (Panel, Glyph, Border…) füllt einen 176-Byte-`UICBData`, mappt `uiCB` mit DISCARD (Treiber benennt den Buffer um) und zeichnet 4 Vertices. Eine Textzeile mit 40 Glyphen sind 40 Map + 40 Draws; ein komplexes In-Game-HUD kommt schnell auf einige Tausend Draws pro Frame. Die Vertexposition wird im VS aus `SV_VertexID` und der CB berechnet; das schließt Batching über einen strukturierten Instanzbuffer (wie `instanceSB` für Meshes) nicht aus.
- **Code-Auszug**:
```cpp
for (const UIRenderObject& obj : m_renderWorld.uiObjects) {
    ... UICBData cb; ... 
    if (SUCCEEDED(ctx->Map(uiCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) { memcpy; Unmap; }
    ctx->Draw(4, 0);
}
```
- **Lösung**: `UICBData` je Objekt in einen strukturierten `StructuredBuffer<UICBData>` (Dynamic, Kapazität wachsend) schreiben, einmal mappen, danach Runs gleicher State (Clip-Rect, Atlas/Textur, Blend) mit `DrawInstanced(4, runLength, 0, firstInstance)` zeichnen; VS liest `uUI[iid]`. Reihenfolge bleibt erhalten (Painter-Algorithmus bleibt dank Run-Bildung nur bei identischem State gebündelt).
- **Erwarteter Effekt** (Schätzung): Bei 1000-3000 UI-Quads Verringerung der CPU-Kosten um 50-80 % im UI-Pass; zu messen mit `FrameGpuStats::drawCalls`.
- **Risiko / Verifikation**: HLSL `kUIHLSL` (Z.1116-1220) und `UICBData` ändern; Parallelstellen: `kUIHLSL12` in `D3D12Renderer.cpp:945`, GL `kUIVS/kUIFS`. Test: `tests/test_ui_widgets.cpp`-Pfad (Render-Vergleich) und Screenshot-Vergleich der Widget-Beispiele.

---

### REN-112 – TAA-Velocity-Pass: pro Objekt `Map/Unmap` und zwei `unordered_map<entityId, mat4>` pro Frame neu gefüllt
- **ID**: REN-112
- **Kategorie**: D, C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:1847-1912` (`renderTaaVelocity`), Member `1782`
- **Problem**: Pro sichtbarem Objekt: ein Hash-Lookup in `taaPrevTransforms`, ein Insert in `taaCurTransforms` (Knoten-Allokation je Eintrag, `clear()` gibt Knoten wieder frei und `swap` tauscht die Maps), danach 3 Matrixmultiplikationen, `Map/Unmap` des `taaVelocityCB` und ein eigener `DrawIndexed`. Instancing wie bei Schatten/SSAO fehlt (der Pass nimmt `m_sortedIndices` und zeichnet Objekt für Objekt, auch gleiche Meshes).
- **Code-Auszug**:
```cpp
const auto it = taaPrevTransforms.find(obj.entityId);
const glm::mat4 prevModel = (it != taaPrevTransforms.end()) ? it->second : obj.transform;
taaCurTransforms[obj.entityId] = obj.transform;
...
ctx->Map(taaVelocityCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m) ... DrawIndexed(...)
```
- **Lösung**: (1) `unordered_map` durch `std::vector<glm::mat4>` indiziert nach dem stabilen Objektindex bzw. durch eine flache Map (z. B. sortiertes Vektorpaar `{entityId, mat4}` mit Merge-Join gegen die letzte Frame-Liste) ersetzen; Kapazität behalten. (2) Per Objekt nur `mvpJitter/mvpNow/mvpPrev` in einen strukturierten Instanzbuffer schreiben und je Mesh-Run per `DrawIndexedInstanced` zeichnen (analog `drawDepthInstanced`). (3) Alternativ Velocity im G-Buffer/Szenenpass als zweites Render-Target mitschreiben.
- **Erwarteter Effekt** (Schätzung): Nur bei TAA relevant; bei 2000 Objekten ca. 0.2-0.5 ms CPU/Frame.
- **Risiko / Verifikation**: Reihenfolge `vpJit * transform` beibehalten (Kommentar Z.1843-1846, LESS_EQUAL-Test hängt davon ab). Hängt mit REN-103/107 zusammen. Test: Bildvergleich mit TAA, bewegte und statische Objekte (kein Ghosting).

---

### REN-113 – Schatten: alle Kaskaden und bis zu 16 lokale Layer werden jedes Frame neu geculled, sortiert und gerendert
- **ID**: REN-113
- **Kategorie**: D (fehlende Dirty-Flags)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:6645-6698` (`renderDepthLayer`-Lambda und beide Schleifen)
- **Problem**: Pro Layer: `m_culler.cull(...)`, `m_sorter.sort(...)` über alle Objekte, `batchDepthCasters`, Viewport/DSV-Wechsel, Clear. Mit 3 Kaskaden und 16 lokalen Layern (Punktlicht = 6 Faces) sind das bis zu 19 vollständige Cull+Sort+Render-Läufe pro Frame, auch wenn Lichter und Caster in diesem Bereich statisch sind. Ein Cache für statische lokale Lichter (Layer nur neu rendern, wenn Licht oder ein Caster im Light-Frustum seine Transform geändert hat) fehlt; der Sort dient laut Kommentar nur dem Gruppieren nach Mesh.
- **Code-Auszug**:
```cpp
p.m_culler.cull(p.m_renderWorld, viewProj, p.shadowVisible);
p.m_sorter.sort(p.m_renderWorld, p.shadowVisible, p.shadowSorted);
RenderSorter::batchDepthCasters(p.m_renderWorld, p.shadowSorted, skipEntity, p.shadowBatches);
```
- **Lösung**: (1) Für lokale Layer einen Signatur-Hash pro Layer (Light-VP, Owner-Entity, Hash der Caster-Transforms im Frustum, Mesh-Invalidierungsgeneration) halten; bei unveränderter Signatur Layer überspringen (Atlas-Inhalt bleibt, `ClearDepthStencilView` und Render entfallen). (2) Für die Sortierung ein direktes Gruppieren nach Mesh-UUID ohne Tiefensortierung (Depth-only braucht keine Tiefenordnung) statt `m_sorter.sort`.
- **Erwarteter Effekt** (Schätzung): Bei Szenen mit vielen statischen Punktlichtern GPU- und CPU-Zeit des Shadow-Passes -50 % bis -90 %; sonst keine Wirkung.
- **Risiko / Verifikation**: Hoch genug, dass Messdaten nötig sind: Caster-Transform-Änderungen, Skinned-Caster (werden in dieser Schleife nicht behandelt), Mesh-Hot-Reload und `shadowSize`-Resize müssen die Signatur ändern. Ohne vorherige Messung ("wie viele lokale Layer sind real aktiv?") nicht umsetzen.

---

### REN-114 – Per-Frame-Heap-Allokationen in Render-Pfaden (Vektoren, Maps, Kernel, Bones, Debug-Lines)
- **ID**: REN-114
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:6720` (`opaqueDCs_/transparentDCs_`), `6913-6915` (`ribbonDCs`, `ribbonMeshes`, `ribbonMeshByDC`), `6922` (`rebased`), `6980` (`deferredForwardDCs`), `7544` (`boneScratch(128)`), `2624` (`BuildSSAOKernel` pro Frame), `5607-5612` (`drawDebugLines` baut `verts` neu), `2478/3532` (`std::function`-Parameter), `8381`
- **Problem**: Alle genannten Container werden pro Frame neu angelegt und wieder freigegeben. `runSSAO` ruft pro Frame `HE::BuildSSAOKernel(kSsaoKernelSize)` (RNG mit festem Seed `0x9E3779B9u`, ergibt jedes Mal das identische Ergebnis, plus `vector`-Allokation und `normalize` je Sample) und befüllt daraus `cb.kernel[]`. Die `std::function`-Parameter `resolveMeshFn` werden pro Draw (virtueller Aufruf, ggf. Heap bei großen Captures) benutzt, obwohl es immer derselbe Lambda `p.resolveMesh` ist. Skinned-Pass füllt 128 Matrizen (8 KB) pro Draw mit Identität und kopiert immer 8 KB in den CB, auch bei 20 Bones.
- **Code-Auszug**:
```cpp
std::vector<glm::vec3> kernel = HE::BuildSSAOKernel(HE::kSsaoKernelSize);   // Z.2624, jedes Frame
std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));              // Z.7544
const std::function<const GpuMesh*(HE::UUID)>& resolveMeshFn                 // Z.2478
```
- **Lösung**: Container als Member des Impl (`clear()` statt neu anlegen, `reserve` behalten); SSAO-Kernel einmal beim Anlegen von `ssaoCB` in ein `static const std::array<glm::vec4,32>` bzw. direkt in den CB-Zwischenspeicher; `resolveMeshFn` als Template-Parameter oder als `GpuMesh* (D3D11RendererImpl::*)(...)`; Bone-Upload auf `n * sizeof(mat4)` begrenzen und nur den Rest bei Bedarf mit Identität füllen (der Shader indiziert nur gültige Bones).
- **Erwarteter Effekt** (Schätzung): Einige µs bis ~0.1 ms pro Frame, vor allem weniger Allokator-Druck; kein Muss.
- **Risiko / Verifikation**: Kleinteilig, bit-identisch. Parallelstellen für `BuildSSAOKernel` pro Frame: D3D12Renderer.cpp:5792, OpenGLRenderer.cpp:5784, VulkanRenderer.cpp:12172, MetalRenderer.mm:12550 (Cross-Modul-Hinweis).

---

### REN-115 – Mesh-Lookups über `unordered_map<UUID>`: pro Objekt dreimal pro Frame
- **ID**: REN-115
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:6245-6249` (`refineWorldBounds`-Schleife über alle Objekte), `6981-6991` (`drawDC`), `6659` (Shadow-Batches), `1879` (Velocity), `3180` (GI)
- **Problem**: `resolveMesh()` macht pro Aufruf einen `meshCache.find()`. Pro Objekt geschieht das in der Bounds-Schleife, im Shadow-Pass (pro Batch), im SSAO-/GI-Prepass (pro DrawCall), im Hauptpass und im Velocity-Pass. Bei 5000 Objekten sind das einige 10 000 Hash-Lookups pro Frame; die UUID-Hash-Funktion ist nicht eingesehen (`HE::UUID` in `Types/UUID.h`, außerhalb des Slices).
- **Code-Auszug**:
```cpp
for (RenderObject& obj : p.m_renderWorld.objects)
    if (const GpuMesh* mesh = p.resolveMesh(obj.meshAssetId, m_contentManager))
        obj.refineWorldBounds(mesh->localBounds);
```
- **Lösung**: Die in der Bounds-Schleife aufgelöste `GpuMesh*` in einem per-Frame-Vektor `std::vector<const GpuMesh*> objMesh` (Index = Objektindex) ablegen und in den späteren Pässen nutzen; `meshCache` ist `unordered_map` (stabile Knoten), die Zeiger bleiben bis `processPendingInvalidations` (Frame-Anfang) gültig.
- **Erwarteter Effekt** (Schätzung): -1 bis -3 Lookups pro Objekt und Frame (~20-50 ns je), also ~0.1-0.5 ms bei 5000 Objekten; nur mit Messung angehen.
- **Risiko / Verifikation**: `resolveMesh` kann beim ersten Aufruf Buffer erzeugen (Upload auf Render-Thread). Pointer-Stabilität prüfen (keine `meshCache`-Rehash-Invalidierung bei `emplace`: für `unordered_map` bleiben Elementzeiger gültig).

---

### REN-116 – `updateSkyEnvCube` allokiert bei jedem Re-Bake 2.3 MB und packt die Halbfloats single-threaded
- **ID**: REN-116
- **Kategorie**: C, B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:4615-4637`
- **Problem**: Bei jeder Sonnenbewegung (Tag-Nacht-Automatik, Totband 2e-3) legt die Funktion `std::vector<float> px(128*128*6*4)` (1.5 MB) und `std::vector<uint16_t> half(...)` (0.8 MB) neu an. Der Bake selbst läuft über `parallel_for`, die anschließende Konvertierung `packHalf1x16` über 393 216 Werte läuft seriell auf dem Render-Thread. Der Bake blockiert den Render-Thread bis zum Ende (`parallel_for` wartet).
- **Code-Auszug**:
```cpp
std::vector<float> px(static_cast<size_t>(N) * N * 6 * 4);
parallel_for(...BuildSkyEnvFaceRow...);
std::vector<uint16_t> half(px.size());
for (size_t i = 0; i < px.size(); ++i) half[i] = glm::packHalf1x16(px[i]);
```
- **Lösung**: `px`/`half` als wiederverwendete Member; Halbfloat-Packen in den `parallel_for`-Körper pro (face,row) verlegen (jede Zeile schreibt direkt `uint16_t`-Output). Dasselbe gilt für die anderen Backends (Parallelstellen unten).
- **Erwarteter Effekt** (Schätzung): ~0.3-1 ms weniger bei jedem Re-Bake, kein Allokator-Hitch.
- **Risiko / Verifikation**: Ergebnis-Bytes identisch; der WARP-Orientierungstest in `tests/test_material_graph.cpp` ruft `UploadSkyEnvCube` über dieselbe Funktion und deckt den Upload ab.

---

### REN-117 – `Shutdown()` listet ~120 `Reset()`-Aufrufe von Hand, obwohl `~D3D11Renderer` das Impl löscht
- **ID**: REN-117
- **Kategorie**: A, F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `D3D11Renderer.cpp:6096-6176`, `6039-6040`
- **Problem**: Alle Member sind `ComPtr`/Container; der Destruktor `delete m_impl` räumt ohnehin alles ab. `Shutdown()` zählt trotzdem einen Teil der Member manuell auf (SSR, TAA, GI, Sky, Decals ...), aber nicht alle (z. B. `vs`, `ps`, `hdrTex`, `bloomTex[]`, `gbTex[]`, `shadowTex`, `localShadowTex`, `giGBuf*`, `worldPreview[]`, `uiFontAtlases` ist drin, `m_debugLines` nicht). Neue Member müssen an zwei Stellen gepflegt werden; vergessene bleiben nach `Shutdown()` am Leben und werden bei erneutem `Initialize()` still überschrieben.
- **Code-Auszug**:
```cpp
void D3D11Renderer::Shutdown() {
    m_impl->meshCache.clear(); ... m_impl->device.Reset();
}
```
- **Lösung**: `Shutdown()` auf `delete m_impl; m_impl = new D3D11RendererImpl{};` (nach `m_impl->context->ClearState(); Flush();`) reduzieren. Die Reihenfolge der Freigabe ist bei D3D11 (Refcounting) unkritisch; `swapchain` vor `device` ist über Member-Reihenfolge gegeben.
- **Erwarteter Effekt**: -80 Zeilen, keine vergessenen Ressourcen.
- **Risiko / Verifikation**: Prüfen, dass kein Aufrufer nach `Shutdown()` auf Impl-Zustand zugreift (`GetCapabilities`, `GetDevice`). Headless-Start/Stop-Test.

---

### REN-118 – `D3D11ShaderManager` wird vom Renderer nicht benutzt (nur Test); doppelte Compile-Logik, schwerer Header, lineare Scans
- **ID**: REN-118
- **Kategorie**: F (toter Code im Produktpfad), A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `D3D11ShaderManager.h:29-31` (Eingeständnis im Kommentar), `D3D11ShaderManager.h:1-14`, `D3D11ShaderManager.cpp:25-47, 56-59, 125-126, 150-156`; Nutzer: nur `tests/test_d3d_shader_manager.cpp`, `HE_Rendering/CMakeLists.txt`
- **Problem**: Der Renderer hält Shader selbst in ComPtr-Membern (Header-Kommentar). Die Klasse ist damit ein Test-/Zukunftsobjekt mit einer weiteren D3DCompile-Pipeline (anders als die sechs Compile-Lambdas im Renderer: ohne Debug-Flags, ohne Fehlertext-Kürzung). Weitere Kleinigkeiten: `load()` macht `fs::exists` und anschließend `ifstream` (doppelter Syscall, TOCTOU); Quelle wird über `stringstream` kopiert; `m_loadedShaders`/`m_loadedPrograms` werden per linearer Suche gepflegt (`release`, `compile`: `for (auto& h : m_loadedShaders)`), obwohl die IDs aufsteigend sind; der Header zieht `windows.h`, `d3d11.h` und `wrl/client.h` in jeden Includer.
- **Code-Auszug**:
```cpp
// Note: D3D11Renderer does not route its shaders through this class — it holds them itself
for (auto& h : m_loadedShaders) if (h.id == handle.id) h.ready = true;
```
- **Lösung**: Entweder den Renderer über diese Klasse leiten (und `compileHlsl` aus REN-109 hier hinein verlegen) oder die Klasse klar als Test-/Tool-Hilfe kennzeichnen. In jedem Fall: `fs::exists` entfernen (der `is_open()`-Check genügt), `m_loadedShaders` als `unordered_map<id, ShaderHandle>`, Header-Includes mit Forward-Deklarationen schlanker halten (`ID3D11Device` ist nur ein ComPtr-Member, dafür braucht der Header `d3d11.h`; ggf. `Impl`-Zeiger).
- **Erwarteter Effekt**: Weniger Code-Duplikation; keine Laufzeitwirkung.
- **Risiko / Verifikation**: `tests/test_d3d_shader_manager.cpp` bleibt grün lassen (ComPtr-Refcount-Test nach `cleanup()`).

---

### REN-119 – Built-in-Draw bindet pro Draw Konstantenpuffer und Textur erneut (`uploadObject`), ohne Änderungsprüfung
- **ID**: REN-119
- **Kategorie**: B (redundante State-Wechsel)
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:6577-6594` (`uploadObject`), `7255-7262, 7329-7336`, `7583-7584`
- **Problem**: `uploadObject` mappt den PerObject-CB und ruft danach jedes Mal `VSSetConstantBuffers(0,...)` und `PSSetConstantBuffers(0,...)` mit demselben Buffer auf; der Buffer ändert sich zwischen den Draws nicht (nur sein Inhalt). Zusätzlich `PSSetShaderResources(0, ...)` und `IASetVertexBuffers` pro Draw auch bei gleichem Mesh/Textur wie beim Vorgänger. Der Hauptpass sortiert zudem nach Tiefe, sodass Folgedraws oft verschiedene Meshes haben; Textur und Mesh-VB sind trotzdem häufig identisch (Instanced-Runs sind schon abgedeckt).
- **Code-Auszug**:
```cpp
ctx->VSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
ctx->PSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());   // pro Draw, derselbe Buffer
```
- **Lösung**: Die CB-Binds aus `uploadObject` entfernen und einmal nach den Material-Draws bzw. am Pass-Anfang setzen (sie werden nach jedem Graph-Material-Draw ohnehin wiederhergestellt, Z.7235-7236); `lastAlbedo`/`lastVB` im `drawDC` merken und nur bei Änderung binden.
- **Erwarteter Effekt** (Schätzung): 2-4 API-Calls pro Built-in-Draw, minimal bis ~0.3 ms bei 3000 Draws.
- **Risiko / Verifikation**: Schattenpass nutzt `uploadObject` ebenfalls (Z.6683), dort ist `VSSetConstantBuffers(0)` durch `VSSetShader(depthVS)` nicht gesichert; beim Entfernen sicherstellen, dass b0 vor der ersten Nutzung gebunden ist (`DrawScene`-Setup Z.6832-6835 tut das für den Hauptpass, nicht für den Schattenpass).

---

### REN-120 – Bloom: 10 Blur-Passes (je 9 Taps) mit 11 CB-Maps pro Frame
- **ID**: REN-120
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:3934-3977` (`runBloom`), `3925-3930` (`updatePostFxCB`), `1090-1105` (`kBloomBlurHLSL`)
- **Problem**: Halbauflösende Ping-Pong-Kette mit 5 H- + 5 V-Passes mit 9 Taps, d. h. 90 Taps pro Pixel bei Halbauflösung, plus pro Pass `Map/Unmap` auf dem 16-Byte-`postFxCB`. Die Parameter wechseln nur zwischen zwei Werten (`horiz` an/aus).
- **Code-Auszug**:
```cpp
for (int p = 0; p < 10; ++p) { ... const float cb[4] = { tw, th, horiz ? 1.0f : 0.0f, 0.0f };
    updatePostFxCB(cb); ... ctx->Draw(3, 0); ... }
```
- **Lösung**: Zwei konstante CBs (H/V) einmal pro Frame (oder statisch beim Resize) anlegen und pro Pass nur umbinden; optional Downsample-Kette (Mip-Pyramide, Dual-Kawase) statt 10 Full-Taps. Letzteres ändert das Bild und ist ausdrücklich keine verhaltensgleiche Änderung.
- **Erwarteter Effekt** (Schätzung): CB-Teil gering; die Algorithmusänderung könnte die Bloom-GPU-Zeit stark senken, braucht aber Bildfreigabe.
- **Risiko / Verifikation**: CB-Teil bit-identisch. Algorithmusänderung nur nach GPU-Messung (`FrameGpuStats` ist whole-frame; PIX nötig).

---

### REN-121 – Kommentar-/Doku-Abweichungen und Log-Rauschen
- **ID**: REN-121
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `D3D11Renderer.cpp:6320-6322` ("Built once per frame", siehe REN-104), `2026` (`uiCB; // 64 bytes` vs. tatsächlich 176, Z.5801), `1269-1270` (`PerObjectCB::pbr` Kommentar nennt nicht `w`), `67` und `D3D11Renderer.h:51` (`[blind] added D3D11 sky+IBL+debuglines parity`), `4693-4698` (gesamte Material-HLSL als `HE_LOG_INFO`), `1396` ("shader hash" Kommentar korrekt), `D3D11Renderer.cpp:4595-4604`
- **Problem**: Einige Kommentare beschreiben veralteten oder falschen Stand (CB-Größe, einmal pro Frame). `[blind]` ist ein Entstehungs-Tag ohne Aussage. `GetOrBuildMaterialShaders` schreibt beim ersten Material den kompletten VS- und PS-Quelltext (mehrere 10 KB) als Info-Log, was Logdateien und die Editor-Konsole aufbläht.
- **Code-Auszug**:
```cpp
ComPtr<ID3D11Buffer> uiCB;       // 64 bytes: rect(16)+color(16)+uvRect(16)+viewport(8)+mode(4)+pad(4)
...
HE_LOG_INFO(RHI, "%s", (std::string("D3D11 A4 material VS HLSL:\n") + vsSrc).c_str());
```
- **Lösung**: Kommentare korrigieren, `[blind]`-Marker entfernen, den HLSL-Dump hinter eine Umgebungsvariable (`HE_DUMP_MATERIAL_HLSL=1`) oder `HE_LOG_DEBUG` legen.
- **Erwarteter Effekt**: Klarere Codebasis.
- **Risiko / Verifikation**: Trivial.

---

### REN-122 – `D3D12MaterialRootSignature.h`: Test-only-Parameter in der Produktfunktion, Registertabelle dreifach von Hand synchron gehalten
- **ID**: REN-122
- **Kategorie**: A (Flag-/Default-Parameter), F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `D3D12MaterialRootSignature.h:93-104, 116-122, 141-155, 185-203, 238-248, 258-293`; Gegenstück `D3D11MaterialBindings.h:45-49, 85-91, 145-147, 195-197`
- **Problem**: `DescribeMaterialRootSignature(out, rangeCount, samplerCount, paramCount)` hat drei Defaultparameter, die ausschließlich für Test-Negativkontrollen existieren (`kLegacyRangeCount`, `kLegacySamplerCount`, `kPreClusterParamCount`), aber in der Produktdeklaration stehen. Die Zuordnung "GLSL-Binding → HLSL-Register" steht an drei Stellen von Hand: `kHlslMaterialPins` in `MaterialShaderLibrary.cpp` (Slice 10), die Slot-Konstanten in `D3D11MaterialBindings.h` und `D3D12MaterialRootSignature.h` (`kSlot*`, `kSamplers[]`); beide Header sagen im Kommentar "Must match kHlslMaterialPins". `DescribeResolveRootSignature` baut zunächst die komplette Material-Signatur und kopiert sie anschließend. Zudem sind Sampler-Registerlisten als Literale (`kSamplers[]`, `kResolvePointSamplers`) statt aus den Pins abgeleitet.
- **Code-Auszug**:
```cpp
inline void DescribeMaterialRootSignature(MaterialRootSignature& out,
    UINT rangeCount = kRangeCount, UINT samplerCount = kSamplerCount, UINT paramCount = kParamCount)
```
- **Lösung**: Test-Varianten in einen kleinen `DescribeLegacyMaterialRootSignature`-Wrapper im Testcode (oder `#ifdef HE_TESTS`); Register-Konstanten aus einer einzigen `constexpr`-Tabelle in `MaterialShaderLibrary.h` ableiten, `static_assert`s zwischen D3D11/D3D12-Slots und den Pins (z. B. `static_assert(kWeightmapSrvSlot == MaterialShaderLibrary::kPinLandscapeWeights.srv)`), damit der "Must match"-Kommentar vom Compiler geprüft wird.
- **Erwarteter Effekt**: Verhindert Drift (das dokumentierte Fehlerbild "Thema 56/57").
- **Risiko / Verifikation**: Die Pins-Tabelle liegt außerhalb dieses Slices (`MaterialShaderLibrary.cpp`, nicht gelesen); Umsetzung dort abstimmen. Bestehende WARP-PSO-Tests in `tests/test_material_graph.cpp` als Absicherung.

---

### REN-123 – Fullscreen-Pass-Setup und "Restore scene state"-Blöcke sind mehr als ein Dutzend Mal von Hand wiederholt
- **ID**: REN-123
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: z. B. `D3D11Renderer.cpp:2606-2612, 2894-2901, 3661-3667, 3939-3946, 5577-5597, 5631-5649, 7741-7752, 7790-7801, 7818-7825, 8417-8427`; Save/Restore per `OMGetRenderTargets` an 9 Stellen
- **Problem**: Acht Stellen setzen Fullscreen-VS (`fsVS`), `noDepthDSS`, `fsRastState`, `IASetInputLayout(nullptr)`, Topologie von Hand; Passes enden mit individuell zusammengestellten "restore scene state"-Listen (`drawSky` Z.5595-5597, `drawDebugLines` Z.5640-5649, `RenderWorldPreview` Z.8454-8466), die jeweils unterschiedlich vollständig sind (z. B. stellt `drawSky` Sampler 0 wieder her, setzt aber PS-Shader nicht zurück). Vergessene Restores sind der in den Kommentaren mehrfach beschriebene Fehlertyp (stilles Unbinding, falsche Sampler).
- **Code-Auszug**:
```cpp
ctx->IASetInputLayout(nullptr);
ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
ctx->VSSetShader(fsVS.Get(), nullptr, 0);
ctx->OMSetDepthStencilState(noDepthDSS.Get(), 0);
ctx->RSSetState(fsRastState.Get());
```
- **Lösung**: Hilfsfunktionen `bindFullscreenState(ctx)` und `restoreSceneState(ctx)` (je eine Stelle, die vollständige Liste der Scene-Defaults) bzw. ein RAII-`ScenePassScope`, der beim Verlassen den definierten Basiszustand setzt. Bei Verwendung passt sich REN-103/107 an.
- **Erwarteter Effekt**: -100 Zeilen, geringeres Risiko vergessener Restores.
- **Risiko / Verifikation**: Mechanisch; Bildvergleich Forward/Deferred/Preview.

---

## Gelesen

Alle sechs Dateien vollständig gelesen: ja (`D3D11Renderer.cpp` Z.1-8601 in Blöcken, `D3D11Renderer.h`, `D3D11ShaderManager.cpp/.h`, `D3D11MaterialBindings.h`, `D3D12MaterialRootSignature.h`); zur Verifikation außerhalb des Slices nur `MaterialShaderLibrary.cpp:925-1010`, `MaterialGraph.cpp:1981-1990`, `SsaoKernel.cpp:17-30`, `SkyNoise3D.cpp:10-40`, `D3D_Shared/HlslSources.h:1-40` und Grep-Treffer in den Backend-Dateien.

## Rendering Slice 2 – Backends/D3D12/D3D12Renderer.cpp (12 904 Zeilen)

## Zusammenfassung

`D3D12Renderer.cpp` ist funktional reif (Frames in Flight, Ring-Buffer pro Slot, Retire-Listen, DRED, DXR-Zweig), aber über die Jahre zu einer einzigen Übersetzungseinheit mit ca. 1 200 Zeilen eingebettetem HLSL und einem ca. 7 300 Zeilen langen Inline-`struct D3D12RendererImpl` gewachsen. Die größten Hebel liegen im Hot Path: Pro Graph-Material-DrawCall werden der komplette GLSL-Quelltext kopiert und gehasht (Zeile 10971-10975), Mesh-/BVH-Puffer liegen dauerhaft im UPLOAD-Heap, alle 3 Kaskaden und bis zu 16 lokale Shadow-Layer werden jeden Frame neu gecullt, sortiert und gezeichnet, und der Debug-Line-Puffer ist nicht pro Frame-in-Flight vorhanden (Datenrennen). Dazu kommen viele einzelne Frame-Kosten (Bloom läuft bei "aus", doppelte Per-Frame-/Cluster-Füllung, volle Atlas-Kopien der GI-Probes, Root-Signature-/Heap-Wechsel pro Material-Draw) und viel Boilerplate (20x Root-Signature-Serialisierung, 8x identische `compile`-Lambdas, 5 parallele Retire-Listen). Funde: 4 Hoch, 17 Mittel, 3 Niedrig (davon 2 reine Vereinfachung). Alle Zeilenangaben sind gegen die Datei verifiziert; Messdaten liegen nicht vor, GPU-seitige Effekte sind als Schätzung gekennzeichnet.

---

## Funde (Hoch zuerst)

### REN-201 – Pro Graph-Material-Draw wird der komplette GLSL-Quelltext kopiert und gehasht
- **ID**: REN-201
- **Kategorie**: C / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10971-10987` (drawDC12), `:11055`, `:11093`; Ursache in `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-952` und `:1000-1009`
- **Problem**: `drawDC12` ruft für jeden DrawCall (jede Instanz-Gruppe, jeden Frame) `resolveShaders(...)` auf. `resolveFragment` kopiert `mat->customShaderFragGlsl` (ein ganzer Shader, typisch mehrere KB bis zehn KB) in einen `std::string` und berechnet `std::hash<std::string>` über den ganzen Text; danach wird `customShaderVertGlsl` genauso kopiert und gehasht. Anschließend holt derselbe Draw `getMaterial()` noch dreimal (precompiledFor, `ma0`, `ma`), je eine Map-Suche. Das Ergebnis (hash, "hat Graph-Shader") ändert sich nur, wenn das Material editiert wird. Die Strings werden nur bei einem PSO-Cache-Miss gebraucht (`GetOrBuildMaterialPSO`).
- **Code-Auszug**:
```cpp
uint64_t matHash = 0; std::string matFrag, matVertBody;
if (p.m_matShaderLib.resolveShaders(*m_contentManager, dc.materialAssetId,
                                    matHash, matFrag, matVertBody))
{ ... matPso = p.GetOrBuildMaterialPSO(matHash, matFrag, matVertBody, matPre, p.usingHDR, matTransp);
// MaterialShaderLibrary.cpp
glslOut = mat->customShaderFragGlsl;
hashOut = std::hash<std::string>{}(glslOut);
```
- **Lösung**: (1) `MaterialShaderLibrary` bekommt eine zweite Methode `resolveShaderKey(cm, id) -> {hash, hasGraph, const MaterialAsset*}`, die den Hash pro `MaterialAsset` cached (Feld im Asset oder `unordered_map<UUID, Entry>`, invalidiert über das vorhandene `InvalidateMaterial`). (2) `drawDC12` holt nur den Key; `frag`/`vertBody` werden erst im Miss-Zweig von `GetOrBuildMaterialPSO` per `const std::string&`-Referenz auf das Asset gelesen (kein Copy). (3) `const MaterialAsset* ma` einmal pro DrawCall holen und an `precompiledFor`/Textur-/Param-Auflösung weiterreichen. Das ist ein Vorgriff auf einen "Draw-Vorbereitungs-Cache pro Material pro Frame".
- **Erwarteter Effekt**: Schätzung: pro Material-Draw ein Wegfall von 2 Heap-Allokationen und von O(Shader-Länge) Hash-Arbeit; bei einigen hundert Material-Draws sind das grob 0,1-1 ms CPU pro Frame (messen). Gilt gleich für D3D11, GL, Metal (Parallelstellen unten).
- **Risiko / Verifikation**: Gering. Beobachtbares Verhalten identisch, solange der Cache bei Shader-Edit/Hot-Reload invalidiert wird (`InvalidateMaterial` existiert, Zeile 12859). Tests: bestehende `tests/test_material_*`-Dateien plus ein neuer Test "Hash ändert sich nach Shader-Edit". Parallelstellen: `D3D11/D3D11Renderer.cpp:7009`, `OpenGL/OpenGLRenderer.cpp:3599`, `Metal/MetalRenderer.mm:12815`.

### REN-202 – Statische Mesh-Puffer (und GI-BVH) liegen dauerhaft im UPLOAD-Heap
- **ID**: REN-202
- **Kategorie**: C / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:8406-8416` (uploadMesh), `:8727-8751` (Skeletal-VB/IB), `:4644-4666` (createUploadBuffer), `:7042-7047` (giNodeSB/giTriSB)
- **Problem**: `uploadMesh` legt Vertex- und Index-Buffer jedes Meshes mit `createUploadBuffer` an (`D3D12_HEAP_TYPE_UPLOAD`, `GENERIC_READ`) und behält sie so für die gesamte Lebensdauer. Auf diskreten GPUs liegt der Speicher im Host-RAM und wird pro Vertex-Fetch über PCIe gelesen; Statische Geometrie gehört in DEFAULT-Heap (VRAM). Das gleiche gilt für die BVH-Knoten/Dreiecke, die der GI-Compute-Kernel bei jedem Ray-Schritt liest. Zusätzlich ist jedes Mesh ein eigener `CreateCommittedResource` (zwei pro Mesh, jeweils mit eigener Heap-Granularität von 64 KB); bei vielen kleinen Meshes ist das Speicherverschwendung und langsam beim Laden.
- **Code-Auszug**:
```cpp
mesh.vbuf = createUploadBuffer(vbytes, nullptr, interleaved.data());
mesh.ibuf = createUploadBuffer(ibytes, nullptr, indices.data());
```
- **Lösung**: (1) `uploadMesh` erzeugt DEFAULT-Buffer in `COPY_DEST`, kopiert über einen wiederverwendeten Staging-Ring (siehe REN-212) auf der Frame-Command-List und wechselt per Barrier auf `VERTEX_AND_CONSTANT_BUFFER | INDEX_BUFFER`. (2) Mesh-Daten in einen großen DEFAULT-Pool sub-allokieren (ein VB-Pool, ein IB-Pool, `D3D12MA`-ähnlicher Linear-/Free-List-Allocator); `GpuMesh` hält nur noch Offsets. (3) GI-Node-/Tri-Puffer ebenfalls DEFAULT (einmal kopieren, nur bei `giBlasDirty`). Auf UMA-GPUs (Intel iGPU, Steam Deck) ändert sich nichts, auf dGPU entfällt der PCIe-Lesezugriff.
- **Erwarteter Effekt**: Schätzung: auf dGPU spürbar bei geometrielastigen Szenen und beim GI-Traversal (Vertex-Fetch aus VRAM statt Host-RAM); Gewinn ist nur mit GPU-Messung (PIX/Timestamps, Zeile 1452 liefert Frame-GPU-Zeit) belegbar, deshalb "messen".
- **Risiko / Verifikation**: Mittel. Resource-State-Verfolgung muss stimmen; `retire`-Pfad (`processPendingInvalidations`, 4341-4362) muss die neuen Handles auch freigeben. Verifikation: bestehende WARP-Tests (`tests/` mit D3D12 WARP, z. B. Material-Root-Signature-Test) und ein Rendering-Vergleichs-Screenshot (`HE_WORLD_PREVIEW_DUMP`, Zeile 12602).

### REN-203 – Shadow-Pass rendert jeden Frame alle Kaskaden und alle lokalen Layer neu, jeweils mit eigenem Cull + Sort
- **ID**: REN-203
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10641-10717` (renderDepthLayer + Schleifen)
- **Problem**: `renderDepthLayer` ruft für jede der bis zu 3 Kaskaden und bis zu 16 lokalen Atlas-Layer (Spot = 1, Punktlicht = 6 Würfelseiten) `m_culler.cull(...)`, `m_sorter.sort(...)` und `batchDepthCasters(...)` über die gesamte Objektliste auf, löscht den Layer und zeichnet alle Caster. Also bis zu 19 volle O(N)-Culls plus O(N log N)-Sorts pro Frame, auch wenn Licht und Caster statisch sind (typisch: Punktlichter an unbewegter Architektur). Es gibt keine Statik-/Dirty-Erkennung.
- **Code-Auszug**:
```cpp
for (int v = 0; v < nLocalLayers; ++v)
    renderDepthLayer(p.localShadowDsv(v), D3D12RendererImpl::kLocalShadowSize,
                     csm.localViewProj[v], localClip[v], csm.localOwnerEntity[v]);
...
p.m_culler.cull(p.m_renderWorld, viewProj, p.shadowVisible);
p.m_sorter.sort(p.m_renderWorld, p.shadowVisible, p.shadowSorted);
```
- **Lösung**: (1) Lokale Layer cachen: pro Layer eine Signatur aus Licht-Matrix + Hash der Caster-Transformationen im Licht-Frustum (der Extractor kennt Dirty-Zustände der Entities bereits für `GIProbeSceneSignature`, Zeile 7601); Layer nur neu rendern, wenn die Signatur wechselt, sonst überspringen (Textur bleibt erhalten, kein Clear). Die lokalen Layer sind ohnehin "fixed 1024², built once" (Kommentar 1553). (2) Für die Kaskaden: Sort nur nach Mesh (für Instancing) statt voller Tiefensortierung, weil Depth-only keine Reihenfolge braucht. (3) Der Cull pro Layer kann bei Punktlichtern aus einer Grob-Kugel-Prüfung vorgefiltert werden.
- **Erwarteter Effekt**: Schätzung: bei Szenen mit einigen statischen Punkt-/Spotlichtern wird der Shadow-Pass-Aufwand (CPU und GPU) um den Anteil der unveränderten Layer kleiner, im Idealfall um 50-90 % der Shadow-Kosten. Messen: `statDraws` plus `lastGpuFrameMs`.
- **Risiko / Verifikation**: Mittel-Hoch (Cache-Invalidierung ist die klassische Fehlerquelle: bewegter Caster im Frustum, Lichtparameter, Shadow-Auflösung, Device-Reset). Absicherung: Test, der ein Caster-Transform ändert und fordert, dass die Signatur wechselt; visueller A/B mit abgeschaltetem Cache (Umgebungsvariable wie `HE_DEPTH_INSTANCING`).

### REN-204 – Debug-Line-Vertex-/Constant-Buffer existiert nur einmal, nicht pro Frame in Flight (Datenrennen CPU/GPU)
- **ID**: REN-204
- **Kategorie**: E / C
- **Schweregrad**: Hoch
- **Typ**: Beides (Korrektheit + Vereinfachung)
- **Ort**: `D3D12Renderer.cpp:3024-3026` (Anlage), `:3088-3122` (drawDebugLines), `:3099-3100` (Kappung), Member `:3768-3771`
- **Problem**: Alle anderen Per-Frame-Puffer (`perFrameCB[f]`, `skyCBuf[f]`, `ssaoCB[f]`, ...) sind pro Frame-Slot angelegt. `debugVBuf`/`debugCBuf` existieren dagegen nur einmal. `drawDebugLines` schreibt sie per Mapped-Pointer, die GPU liest sie zur Ausführungszeit des Frames N-1/N-2 weiter (3 Frames in Flight, `waitForFrame` wartet nur auf Frame N-3). Ergebnis: flackernde/falsche Linien und im Extremfall inkonsistente Matrix. Zusätzlich verwirft die Prüfung bei mehr als 4096 Linien ALLE Linien, statt zu kappen.
- **Code-Auszug**:
```cpp
debugVBuf = createUploadBuffer(static_cast<UINT64>(8192 * 6 * sizeof(float)), ...
...
if (!debugVBuf || needed > 8192ULL * 6 * sizeof(float)) return;
if (debugVBufPtr) { float* v = reinterpret_cast<float*>(debugVBufPtr); ... }
if (debugCBufPtr) std::memcpy(debugCBufPtr, glm::value_ptr(viewProj), 64);
```
- **Lösung**: `debugVBuf[k_frameCount]`/`debugCBuf[k_frameCount]` plus Pointer-Arrays; `drawDebugLines` bekommt den Frame-Index (wie `drawSky(cl, fi, ...)` schon). Besser: die Debug-Linien in den vorhandenen Per-Frame-Linear-Allocator legen (REN-217). Kappung: `lineCount = min(lines.size(), kMaxDebugLines)`. Der World-Preview (12500) ruft `drawDebugLines` nach einem Voll-Flush auf, bleibt also korrekt.
- **Erwarteter Effekt**: Korrektheit (kein Flackern bei laufender GPU); Kosten: 3x ca. 196 KB Upload. Performance neutral.
- **Risiko / Verifikation**: Gering. Test: Debug-Layer (`HE_GPU_DEBUG`) zeigt solche Rennen nicht; stattdessen reproduzierbar mit GBV oder durch zwei Frames mit unterschiedlichen Linien und `vsync=false`. Parallele: D3D11 nutzt `MAP_WRITE_DISCARD` und ist sicher (`D3D11Renderer.cpp:5626`).

### REN-205 – Jeder Graph-Material-Draw wechselt Heap, Root Signature, PSO und kopiert 17 Deskriptoren
- **ID**: REN-205
- **Kategorie**: B / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:11101-11130` (Block aufbauen), `:11183-11200` (Zustand zurücksetzen), `RenderSorter.h` (`struct SortKey { meshHi, meshLo, distSq, index }`)
- **Problem**: Pro Material-DrawCall: `CopyDescriptorsSimple(17, ...)`, bis zu 6 `CreateShaderResourceView` (inkl. `GetDesc()` in `srvForTexture`, 3550), danach `SetDescriptorHeaps` (Material-Heap), `SetGraphicsRootSignature(m_matRootSig)`, `bindClusterRoots`, `SetPipelineState`, `SetGraphicsRootDescriptorTable`, und nach dem Draw alles wieder zurück (`SetGraphicsRootSignature(rootSig)`, `SetDescriptorHeaps(sceneSrvHeap)`, 2x Root-Table, 3x Cluster-SRV, PSO). Der Sort-Schlüssel gruppiert nur nach Mesh, nicht nach Material/PSO, daher wechseln built-in und Graph-Material-Draws beliebig oft. `SetDescriptorHeaps`-Wechsel gelten auf einigen GPUs als teuer (Pipeline-Flush).
- **Code-Auszug**:
```cpp
cl->SetDescriptorHeaps(1, mheaps);
cl->SetGraphicsRootSignature(p.m_matRootSig.Get());
p.bindClusterRoots(cl);
cl->SetPipelineState(matPso);
...
cl->SetGraphicsRootSignature(p.rootSig.Get());   // nach jedem Draw zurück
p.bindClusterRoots(cl);
```
- **Lösung**: (1) Opaque-Pass in zwei Phasen: erst alle built-in DrawCalls, dann alle Graph-Material-DrawCalls hintereinander (Tiefentest macht die Reihenfolge für Opaque unerheblich); State-Restore nur einmal am Ende der Material-Phase. Transparent bleibt in Sortierreihenfolge. (2) Zusätzlich innerhalb der Material-Phase nach PSO gruppieren (der Material-Hash liegt vor). (3) Mittelfristig: built-in Pfad auf die Material-Root-Signature heben, dann gibt es nur eine Signatur und einen Heap. (4) Deskriptor-Block pro DrawCall nur schreiben, wenn sich Textur-Tupel gegenüber dem vorherigen DrawCall geändert hat (Block-Wiederverwendung innerhalb des Frames).
- **Erwarteter Effekt**: Schätzung: bei gemischten Szenen mit n Material-Draws entfallen 2n Heap-/Root-Signature-Wechsel; Gewinn nur messbar mit PIX; CPU-Seite ca. 10-20 API-Aufrufe pro Draw.
- **Risiko / Verifikation**: Mittel (Reihenfolge der Opaque-Draws ändert Early-Z-Verhalten, Ergebnis sonst gleich; bei `deferredForwardDCs` unverändert). Absicherung: Bildvergleich Deferred/Forward, `tests/test_culling.cpp`-ähnlicher Textvergleich entfällt.

### REN-206 – Instanzierte Graph-Material-Batches: pro Instanz Param-Ring-Slot und 256-Byte-Kopie mit identischem Inhalt
- **ID**: REN-206
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:11141-11181` (drawMatInstance), Ringe `k_matMaxDraws = 1024` (3623)
- **Problem**: Für jede Instanz wird `float padded[64]` auf Null gesetzt, `params` hineinkopiert und in einen eigenen Slot des HeParams-Rings geschrieben (256 B), obwohl der Inhalt für alle Instanzen eines DrawCalls identisch ist. Zusätzlich frisst jede Instanz einen Slot des 1024-Slot-Rings; ein Batch mit 1000 Instanzen verbraucht den ganzen Frame-Ring (`noteRingFull`, 11010) und der Rest fällt auf Built-in-PBR zurück ("silent optics change", Kommentar 11008).
- **Code-Auszug**:
```cpp
float padded[64] = { 0.0f };
if (params) std::memcpy(padded, params->data(), std::min(params->size(), size_t(64)) * sizeof(float));
std::memcpy(p.m_matParamPtr[p.frameIndex] + i * k_matSlot, padded, sizeof(padded));
...
cl->SetGraphicsRootConstantBufferView(2, parVA);
```
- **Lösung**: HeParams einmal pro DrawCall (vor der Instanzschleife) in einen Slot schreiben und `parVA` für alle Instanzen wiederverwenden. Die Per-Instanz-U-Daten (`MatU`, 176 B) bleiben. Das halbiert Ring-Verbrauch und Schreibvolumen. Weiter: echte Instanzierung für Graph-Materialien (Vertex-Shader liest `mvp/model` aus dem `t3`-Ring wie `VSMainInstanced`, 612-621) ist eine größere Änderung.
- **Erwarteter Effekt**: Schätzung: -50 % Ring-Schreibvolumen und doppelte Batch-Obergrenze (ca. 1024 -> bis ca. 2048 Instanzen, abhängig von U-Slots).
- **Risiko / Verifikation**: Gering; beobachtbar identisch. Test: DrawCall mit `instanceTransforms.size() == 3` und `paramOverride`, beide Varianten rendern denselben Frame (WARP-Screenshot).

### REN-207 – Shadow-Fallback pro Caster verbraucht 256-Byte-CB-Slots des gemeinsamen Draw-Rings und verdrängt die Geometrie
- **ID**: REN-207
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10681-10692`, `:3425` (`count < 2`), `:90-95` (Kommentar zum Budget), Verbraucher `:11241-11244`, `:11632-11637`
- **Problem**: `drawIdx` ist ein einziger Zähler für Shadow-, Geometrie-, Skinned- und TAA-Velocity-Draws (`k_maxDraws = 16384` Slots a 256 B). Ein Caster-Run der Länge 1 (`count < 2`) oder ein Run bei vollem Instance-Ring nimmt einen vollen 256-Byte-Slot, obwohl `VSDepth` nur `uMVP` (64 B) liest. Bei 19 Shadow-Layern mit vielen einzelnen Meshes ist der Ring schnell leer; dann brechen die Geometrie-Draws still ab (`if (drawIdx >= k_maxDraws) return;`, 10950), der Kommentar in 90-95 bestätigt, dass das Budget wegen dieses Effekts schon zweimal verdoppelt wurde.
- **Code-Auszug**:
```cpp
if (!instPSO || !perInstancePtr[fi] || count < 2 || !RenderSorter::depthInstancingEnabled() ...) return false;
...
PerObjectCB o{}; o.mvp = clip * xf[k]; o.model = xf[k];
std::memcpy(ringPtr + drawIdx * k_cbSlot, &o, sizeof(o));
```
- **Lösung**: `count < 2` auf `count < 1` lockern: auch ein Run der Länge 1 geht über den 128-Byte-Instanz-Ring (65 536 Einträge) statt über einen 256-Byte-CB-Slot. Alternativ für Depth-Only `SetGraphicsRoot32BitConstants(0, 16, ...)` (Root-Signature-Änderung nötig). Zusätzlich: getrennte Zähler pro Pass (Shadow bekommt nie Geometrie-Slots).
- **Erwarteter Effekt**: Entfernt die Kapazitätsgrenze, bei der Geometrie verschwindet; Schreibvolumen pro Caster 256 B -> 128 B.
- **Risiko / Verifikation**: Gering. `RenderSorter::depthInstancingEnabled()` (HE_DEPTH_INSTANCING=0) bleibt als A/B-Schalter. Test: Szene mit 20 000 Einzel-Casters, Geometrie-Draw-Zähler gegen `statDraws` prüfen.

### REN-208 – Bloom-Kette (Bright + 10 Blur-Passes + 22 Barrieren) läuft auch bei deaktiviertem Bloom
- **ID**: REN-208
- **Kategorie**: B / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:2577-2609` (Bright + Blur-Schleife), `:2616` (Tonemap-Konstanten)
- **Problem**: `runPostFX` führt Bright-Pass und 10 Gaussian-Blur-Passes auf Halbauflösung immer aus; `bloomEnabled == false` setzt nur die Stärke im Tonemap auf 0. Der GPU-Aufwand (11 Fullscreen-Draws, 22 Barrieren, 10x 9-Tap-Blur) fällt also auch dann an, wenn der Nutzer Bloom ausschaltet, um Leistung zu gewinnen.
- **Code-Auszug**:
```cpp
cl->SetPipelineState(bloomBlurPSO.Get());
for (int pass = 0; pass < 10; ++pass) { ... cl->DrawInstanced(3,1,0,0); horiz = !horiz; }
...
const float cb[4]={exposure, bloomEnabled?bloomStrength:0.0f, 0,0};
```
- **Lösung**: `if (bloomEnabled && bloomStrength > 0.0f) { Bright + Blur } else { t1 = srvGpuHandle(0) (Null-SRV, liest 0) }`. Die Blurpass-Zahl (10) zusätzlich als Qualitätsparameter statt Konstante; Vulkan nutzt dieselbe harte 10 (`VulkanRenderer.cpp:1000`).
- **Erwarteter Effekt**: Schätzung: bei "Bloom aus" entfallen ca. 11 Fullscreen-Passes in Halbauflösung (messen, typischerweise 0,2-0,6 ms auf Mittelklasse-GPU).
- **Risiko / Verifikation**: Gering. Bild bleibt identisch, solange der Null-SRV-Pfad 0 liefert (`nullSrv0`, 2042-2048, ist RGBA8-Null). Zwei WARP-Screenshots (Bloom an mit Stärke 0 / Bloom aus) müssen pixelgleich sein.

### REN-209 – GI-Probe-Update kopiert jeden Frame die kompletten Irradiance- und Visibility-Atlanten
- **ID**: REN-209
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:8033-8052` (dispatchGiProbeUpdate)
- **Problem**: Das Kernel-Update schreibt pro Frame nur `budget` (Standard 256) Probes, doch vor jedem Dispatch werden `giIrrTex` (RGBA16F) und `giVisTex` (RG16F) vollständig per `CopyResource` in die Prev-Texturen kopiert, mit 6 Barrieren drumherum. Bei einem Raster mit mehreren Tausend Probes sind das viele MB pro Frame, obwohl die Prev-Daten nur für die Probes dieses Batches gebraucht werden ("texels outside this batch keep their values in the canonical atlases since the kernel never writes them", Kommentar 8037).
- **Code-Auszug**:
```cpp
cl->CopyResource(giIrrPrevTex.Get(), giIrrTex.Get());
cl->CopyResource(giVisPrevTex.Get(), giVisTex.Get());
```
- **Lösung**: Nur die Region des aktuellen Batches kopieren. Die Atlas-Kacheln liegen zeilenweise (`probesPerRow = min(count, 32)`, Kachel 8x8): der Batch `[cursor, cursor+budget)` entspricht höchstens zwei Rechtecken (Wrap). Zwei `CopyTextureRegion`-Aufrufe pro Atlas mit `D3D12_BOX`. Alternative: Kernel liest Prev über einen Ping-Pong und alle Texel, die der Batch nicht schreibt, kopiert ein trivialer Compute; die Region-Kopie ist einfacher.
- **Erwarteter Effekt**: Schätzung: Kopievolumen sinkt auf budget/probeCount (z. B. 256 von 4096 = 6 %).
- **Risiko / Verifikation**: Mittel (Rand/Wrap-Fall, Batch-Ende am Atlas-Ende). Absicherung: `tests/test_gi_*`-Bildvergleich über mehrere Frames, Unit-Test für die Rechteckberechnung (reine Funktion in `GIProbeGrid.h`).

### REN-210 – `updateGiAccel`: pro Frame Matrix-Inverse und Hash-Lookups je Caster, BVH-Bau im Render-Thread, Komplett-Neuanlage der BVH-Puffer
- **ID**: REN-210
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:6985-7000` (Schleife), `:6924-6951` (BuildGIBlas), `:7038-7048` (Neuanlage), `:6962-6967`
- **Problem**: Für jedes Shadow-Caster-Objekt (ungeculled) laufen jedes GI-Frame: `giBlasCache.find(UUID)` (Zeile 6964, plus `GIBlasRange`-Kopie), bei DXR ein zweites `find`, und `glm::inverse(obj.transform)` (volle 4x4-Inverse, Zeile 6996). Bei statischen Szenen ändert sich fast nichts, trotzdem wird das ganze Instanz-Array neu gebaut und hochgeladen. Neue Meshes bauen ihre CPU-BVH synchron im Render-Thread (`buildGiBvh`), und jede neue BLAS lässt `giNodeSB`/`giTriSB` komplett neu anlegen und neu hochladen (O(Gesamtgröße) pro neuem Mesh). Beim Szenenstart mit 200 Meshes ist das O(n²) Datenbewegung.
- **Code-Auszug**:
```cpp
inst.invTransform = glm::inverse(obj.transform);
...
giNodeSB = giNodesCpu.empty() ? nullptr : createUploadBuffer(giNodesCpu.size() * sizeof(HE::GiBvhNode), nullptr, giNodesCpu.data());
```
- **Lösung**: (1) `invTransform` aus Affine-Inverse (Rotation/Skalierung transponieren/dividieren) oder pro Objekt cachen, solange das Transform unverändert ist (Extractor-Dirty-Flag). (2) Instanz-Upload nur bei Änderung der Objektliste/Transforms (Signatur existiert für die Probe-Grid-Logik). (3) BLAS-Aufbau auf Job-System auslagern und mit Kapazitätswachstum (Verdopplung) statt Komplett-Neuanlage appenden. (4) Zwei Hash-Lookups auf einen reduzieren (Struct mit SW- und HW-BLAS).
- **Erwarteter Effekt**: Schätzung: pro GI-Frame O(n) Inverse und Lookups entfallen; Ladeverhalten bei vielen Meshes deutlich flüssiger. Parallelstellen: `D3D11Renderer.cpp:3184`, `OpenGLRenderer.cpp:5974`, `MetalRenderer.mm:8245` haben dieselbe `glm::inverse`-Zeile.
- **Risiko / Verifikation**: Mittel. Test: `tests/test_gi_bvh.cpp` existiert für die BVH; ergänzen um Vergleich Affine-Inverse vs `glm::inverse` bei nicht-uniformer Skalierung.

### REN-211 – `fillPerFrame` und `fillMatLight` (inkl. `BuildClusterLights`) laufen pro Frame doppelt
- **ID**: REN-211
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10316-10331` (uploadClusters), `:10393` und `:10852` (fillPerFrame), `:10523` und `:10854` (fillMatLight)
- **Problem**: Beide Lambdas werden einmal mit "GI aus" vorbefüllt und später mit der endgültigen GI/AO/SSR-Entscheidung neu gefüllt (der Kommentar bestätigt: "last write wins"). Jeder Aufruf führt in `fillPerFrame` im Cluster-Modus `HE::BuildClusterLights(...)` aus (Clustering aller Lichter gegen bis zu 3456 Zellen), kopiert danach drei Upload-Puffer (Lights/Grid/Indices bis 256 KB) und `BuildDirectionalLightWindow`; `fillMatLight` füllt 832 Byte Lighting inkl. mehrerer Helfer (`FillMaterialLightWindow`, `FillMaterialWeather`, `GetWaterRipples()`, ...). Die erste Füllung wird von keinem Pass gelesen, der vor der zweiten läuft (Shadow, GI, SSAO nutzen eigene Puffer), solange der Backbuffer-Zweig des Render-Graphen immer läuft.
- **Code-Auszug**:
```cpp
fillPerFrame(false, p.ssaoEnabled && p.ssaoReady);   // 10393
...
fillPerFrame(giShadingActive, aoWanted, ssrResult != nullptr);   // 10852
```
- **Lösung**: Erste Füllung nur machen, wenn der Backbuffer-Zweig nicht erreicht wird (sonst entfernen). Falls doch nötig: Cluster-Build einmal (nur das GI-Masken-Kanalfeld hängt von `giActive` ab, siehe Aufruf `BuildClusterLights(..., giActive && p.giLocalMaskTex)`) und die zweite Füllung patcht nur `params.z`.
- **Erwarteter Effekt**: Schätzung: -1 `BuildClusterLights` + 3 Memcpy + 1 `Lighting`-Fill pro Frame (grob 20-100 µs bei vielen Lichtern, messen).
- **Risiko / Verifikation**: Gering-Mittel: nur sicher, wenn kein Pass die CB vor der zweiten Füllung braucht. Verifikation: Debug-Layer-Lauf und Test mit Szene ohne Backbuffer-Pass (Early-Return Zeile 10257).

### REN-212 – Textur-Upload: je Textur ein eigener committed UPLOAD-Buffer, mehrere Vektor-Allokationen, synchron im ersten Draw
- **ID**: REN-212
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:4159-4250` (uploadTexture2D), `:4132-4146` (allocAlbedoSlot), `:3995-4010` (UI-Bilder), `:3588-3602` (Graph-Texturen)
- **Problem**: Jeder erste Draw einer Textur legt auf dem Render-Thread einen `CreateCommittedResource` im UPLOAD-Heap an (Kosten: Heap-Allokation und Kernel-Aufruf), erzeugt drei `std::vector` (`footprints`, `numRows`, `rowSizes`) und kopiert die gesamte Mip-Kette zeilenweise. Das passiert mitten im Frame (Hitch), ohne Budget pro Frame, und der Staging-Buffer wird erst nach `k_frameCount + 2` Frames freigegeben. Laden einer Szene mit 300 Texturen ergibt 300 Kernel-Allokationen in wenigen Frames.
- **Code-Auszug**:
```cpp
std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subs);
std::vector<UINT> numRows(subs);
std::vector<UINT64> rowSizes(subs);
ComPtr<ID3D12Resource> uploadBuf = createUploadBuffer(uploadSize, &mapped);
```
- **Lösung**: (1) Linearer Upload-Ring pro Frame-Slot (z. B. 32-64 MB, wächst), Texturen werden hineinkopiert, Retire entfällt (gleicher Frame-Fence). (2) Footprint-Arrays als `thread_local`/Member-Scratch. (3) Pro Frame Upload-Budget (z. B. 8 MB), Rest im nächsten Frame ("Pop-in" statt Hitch; Fallback bleibt Flat/Null-SRV wie heute). (4) Optional Copy-Queue plus Fence.
- **Erwarteter Effekt**: Schätzung: weniger Frame-Spitzen beim Streaming; gemeinsam mit REN-202 (Mesh-Staging) ein einziger Mechanismus.
- **Risiko / Verifikation**: Mittel (State-/Lifetime-Fehler). Test: `tests/test_texture_array*` (Payload-Layout, `HE::textureArrayPayloadValid`) bleibt maßgeblich; WARP-Test mit 100 Texturen.

### REN-213 – Graph-Material-PSOs und FXC laufen synchron im Draw-Loop, ohne Disk-Cache
- **ID**: REN-213
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:9021-9219` (GetOrBuildMaterialPSO), Aufruf `:11006`, Warmup `:4396-4428`
- **Problem**: Bei einem Cache-Miss ruft der erste Draw des Frames auf dem Render-Thread `D3DCompile` (VS+PS), SPIRV-Cross (über `m_matShaderLib`) und `CreateGraphicsPipelineState` auf; Kosten pro Material typisch Zehner bis Hunderte ms. `WarmupMaterials` entschärft das nur für vorab bekannte IDs. Es gibt keinen On-Disk-Cache für DXBC und keinen `ID3D12PipelineLibrary`, jeder Start kompiliert alles neu.
- **Code-Auszug**:
```cpp
if (FAILED(D3DCompile(vsSrc.c_str(), vsSrc.size(), "matVS", nullptr, nullptr, "main", "vs_5_0", cflags, 0, &vs, &cerr)))
```
- **Lösung**: (1) DXBC-Cache auf Platte, Schlüssel = `hash + cflags + Compiler-Version`; vor `D3DCompile` prüfen. (2) PSO-Bau auf Job-Threads, bis zur Fertigstellung Draw überspringen oder Built-in-Fallback (wie Pak-Variante). (3) `ID3D12PipelineLibrary` für die PSOs. Teilweise vorhanden: Pak-Precompiled-Pfad (`precompiledFor`).
- **Erwarteter Effekt**: Schätzung: keine Frame-Spitzen beim ersten Sichten eines Materials, deutlich kürzere Folgestarts.
- **Risiko / Verifikation**: Mittel (Cache-Invalidierung bei Shader-Änderung, Treiberwechsel). Test: Cache-Treffer = identische Bytecode-Bytes.

### REN-214 – Beim Start wird dieselbe Scene-HLSL-Quelle bis zu zehnmal von FXC kompiliert
- **ID**: REN-214
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance (Startzeit)
- **Ort**: `D3D12Renderer.cpp:5192-5298` (createPipeline: VSMain, PSMain, VSMainInstanced, VSDepth, VSDepthInstanced), `:8621-8624` (PSMain nochmals), `:9611-9648` (VSMain, GBufPS, VSMainInstanced nochmals), `:3206` (VSMain im World-Preview); insgesamt `grep -c "D3DCompile("` = 26
- **Problem**: `PSMain` wird mit identischen Parametern (Quelle, Entry, Profil, Flags) zweimal kompiliert (5201 und 8623), `VSMain` dreimal, `VSMainInstanced` zweimal. Jeder Aufruf verarbeitet die Quelle (ca. 500 Zeilen plus `kSkyFuncHLSL`) erneut. Dazu acht fast identische `compile`-Lambdas (siehe REN-222).
- **Code-Auszug**:
```cpp
const std::string sceneSource = std::string(kSkyFuncHLSL) + kSceneHLSL;   // 5192, 8621, 9611, 3206
D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &ps, &cerr)
```
- **Lösung**: Ein kleiner `ShaderBlobCache` im Impl (`map<(entry,profile) -> ComPtr<ID3DBlob>>`) für die Scene-Quelle; `createPipeline` füllt ihn, `createSkinnedPipeline`, `createDeferredPipelines`, `ensureWorldPreviewPipeline` lesen daraus. Dazu `sceneSource` einmal als Member.
- **Erwarteter Effekt**: Schätzung: 4-5 FXC-Aufrufe weniger beim Start (zusammen vermutlich 50-300 ms, messen).
- **Risiko / Verifikation**: Gering. Flags (`_DEBUG`) bleiben pro Build gleich.

### REN-215 – `SetMoonTexture` zerstört die alte Textur, bevor die GPU damit fertig ist
- **ID**: REN-215
- **Kategorie**: E (Korrektheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung / Korrektheit
- **Ort**: `D3D12Renderer.cpp:12664-12740` (`p.moonTex12.Reset()` in Zeile 12667, `waitForAllFrames()` erst in Zeile 12707)
- **Problem**: Die Textur wird sofort freigegeben, obwohl Frames in Flight noch den Deskriptor in `skyEnvHeap[0]` samplen können (Sky-Pass jeden Frame). Ein Wechsel der Mondtextur kann damit eine zerstörte Ressource referenzieren (Device-Removed/TDR im ungünstigen Fall). Gleiche Reihenfolge-Frage bei `CreateImGuiTexture` (dort wird nichts freigegeben, daher unkritisch).
- **Code-Auszug**:
```cpp
auto& p = *m_impl;
p.moonTex12.Reset();
...
p.waitForAllFrames();   // erst 40 Zeilen später
```
- **Lösung**: `waitForAllFrames()` vor dem `Reset()` oder die alte Textur in `m_retiredTextures` schieben (`emplace_back(std::move(p.moonTex12), k_frameCount + 2)`), wie bei den anderen Hot-Reload-Pfaden.
- **Erwarteter Effekt**: Entfernt ein mögliches Use-After-Free der GPU-Ressource beim Mondtextur-Wechsel.
- **Risiko / Verifikation**: Gering. Test: zweimal `SetMoonTexture` hintereinander mit laufendem Rendern unter `HE_GPU_DEBUG`.

### REN-216 – UI-Pass: String-Schlüssel pro Quad und Frame, ein Draw pro Quad, 256-Byte-Slot für 176 Byte
- **ID**: REN-216
- **Kategorie**: B / C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:4861-4977` (renderUIPass12), `:3995-4010` (uiImageSlotFor), `:3577-3580` (graphTexKey), `:4941`
- **Problem**: (1) Für jedes texturierte Quad (`type == 0 && textureAssetId != 0`) baut `graphTexKey` zweimal pro Frame einen `std::string` (`to_string(hi) + ":" + to_string(lo)`, drei Allokationen) und sucht ihn in einer `unordered_map<string,...>` (Zeile 4873/3997 im Vor-Pass, Zeile 4941 im Zeichen-Pass). (2) Jedes Glyph-Quad ist ein eigener `DrawInstanced(4,1,0,0)` mit eigenem Root-CBV und 256-Byte-Slot (176 genutzt), bis zu 4096 pro Frame; Text ist der Normalfall in UI-Szenen. (3) `m_uiFontAtlases.find` pro Glyph, zwei Durchläufe über `uiObjects`.
- **Code-Auszug**:
```cpp
static std::string graphTexKey(const HE::UUID& id, const std::string& path)
{ return id != HE::UUID{} ? (std::to_string(id.hi) + ":" + std::to_string(id.lo)) : path; }
...
const auto it = m_uiImageCache.find(graphTexKey(obj.textureAssetId, {}));
```
- **Lösung**: (1) UI-Bild-Cache als `unordered_map<HE::UUID, UIImage12>` (wie `decalTexCache`, 3681) statt String-Schlüssel; Invalidierung entsprechend umstellen (4333-4338). (2) Slot pro Objekt im Vor-Pass in ein Scratch-Array schreiben, Zeichen-Pass liest den Slot (kein zweiter Lookup). (3) Glyph-Quads auf einen Structured-Buffer (`StructuredBuffer<UIQuad>` + `SV_InstanceID`) mit einem Draw pro (Atlas, Clip)-Run umstellen, ähnlich dem Instance-Ring `t3`. (4) UICB-Stride auf 192 B reduzieren oder Quads in den Instance-Ring packen.
- **Erwarteter Effekt**: Schätzung: bei 500 Glyphen von 500 Draws auf wenige Draws; String-Allokationen pro Frame entfallen.
- **Risiko / Verifikation**: Mittel (Clip-Wechsel `applyClip` muss Runs trennen, Reihenfolge bleibt Painter-Order). Test: `tests/test_ui_widgets*`-Rendering vergleichen; WARP-Screenshot der Widgets.

### REN-217 – Großzügig dimensionierte, eager angelegte Upload-Ringe (ca. 65 MiB) und viele Einzelringe
- **ID**: REN-217
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `D3D12Renderer.cpp:95-98` (Konstanten), `:5339-5360`, `:5557-5569`, `:7359`, `:4822-4832`, `:8681-8686`
- **Problem**: Aus den Konstanten `k_maxDraws = 16384`, `k_cbSlot = 256`, `k_maxInstances = 65536`, `k_instStride = 128` folgt: `perObjectRing` 4 MiB, `perInstanceRing` 8 MiB, `ssaoPosPerObjRing` 4 MiB, `giGBufObjRing` 4 MiB, `m_uiCB` 1 MiB, `m_bonesCB` je 2 MiB (256 x 8192 B), alles x 3 Frames = grob 65 MiB Upload-Speicher, beim Start angelegt, auch wenn SSAO/GI/UI nie benutzt werden (`createGiPipelines` läuft bewusst eager, 5393). Jedes Feature hat einen eigenen Ring plus eigene Slot-Zähler und eigene Overflow-Logik (`drawIdx`, `gbufIdx`, `posDrawIdx`, `skinnedIdx`, `m_matDrawCursor`, `instRingCursor`).
- **Code-Auszug**:
```cpp
perObjectRing[f]   = createUploadBuffer(static_cast<UINT64>(k_maxDraws) * k_cbSlot, ...);
ssaoPosPerObjRing[f] = createUploadBuffer(static_cast<UINT64>(k_maxDraws) * k_cbSlot, ...);
giGBufObjRing[f] = createUploadBuffer(static_cast<UINT64>(k_maxDraws) * k_cbSlot, ...);
```
- **Lösung**: Ein `FrameUploadArena` pro Frame-Slot (ein großer UPLOAD-Buffer, Bump-Allocator mit 256-B-Ausrichtung, `alloc(bytes) -> {cpuPtr, gpuVA}`, `reset()` pro Frame, Wachstum durch Doppelpuffer-Strategie). Alle Ringe (Object, Instance, SSAO, GI, UI, Debug, Bones, Decal) werden zu `arena.alloc(...)`-Aufrufen; Overflow wird an einer Stelle behandelt und geloggt. Damit lösen sich auch REN-204 und REN-207 mit.
- **Erwarteter Effekt**: Speicher: bedarfsgesteuert statt Worst-Case (Schätzung: 10-20 MiB typisch statt 65 MiB); Code: ca. 300-400 Zeilen Ring-Boilerplate weniger.
- **Risiko / Verifikation**: Mittel. Alle `ringPtr`/`ringBase`-Verwendungen müssen migriert werden; WARP-Test mit Frame-Wiederholung und Debug-Layer.

### REN-218 – Viele vollständige GPU-Drains (32x `waitForAllFrames`) auf Pfaden, die auch zur Laufzeit laufen
- **ID**: REN-218
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:12424`/`:12638` (RenderWorldPreview, pro Aufruf zwei), `:8327`/`:8342` (buildGiHwBlas, pro neuem Mesh zwei), `:12794`/`:12818` (CreateImGuiTexture, pro Icon zwei), `:12707`/`:12731` (SetMoonTexture), `:1685` und `:1990` (createViewportRT -> createPostFXResources, doppelt), `:5683`/`:5708`/`:5753`/`:5777`, `:2755`/`:2780` (Init)
- **Problem**: `waitForAllFrames()` legt CPU und GPU still (Signal + Wait). (1) Jeder `RenderWorldPreview`-Aufruf (mehrere Scene-Viewports pro Frame möglich) drainiert zweimal die komplette Queue, also verliert der Editor-Viewport die CPU/GPU-Überlappung. (2) Jede neue DXR-BLAS wartet zweimal innerhalb des laufenden Frames (`updateGiAccel` läuft in `DrawScene` bei offener Frame-Liste). (3) `CreateImGuiTexture` pro Content-Browser-Icon und Logo zwei Drains. (4) Jede Viewport-Größenänderung (Ziehen eines Dock-Splitters) ruft `createViewportRT` -> `waitForAllFrames` und danach in `createPostFXResources` erneut.
- **Code-Auszug**:
```cpp
p.waitForAllFrames();           // 12424: vor dem Preview
if (!p.ensureWorldPreviewTarget(...)) return nullptr;
...
p.cmdQueue->ExecuteCommandLists(1, lists);
p.waitForAllFrames();           // 12638: nach dem Preview
```
- **Lösung**: (1) Preview: eigener Ring pro Preview-Slot x Frame (CB/Bones) plus Fence; Queue-Reihenfolge reicht, die Hauptqueue muss nicht geleert werden. Alternativ alle Previews eines Frames in eine Liste und ein Drain. (2) BLAS-Builds der Frame-Liste hinzufügen (`BuildRaytracingAccelerationStructure` ist im Frame-`giCmdList4` erlaubt, dahinter UAV-Barrier) und Scratch per Retire freigeben. (3) `CreateImGuiTexture`/`SetMoonTexture`/Init-Uploads über eine gemeinsame `uploadAndFlush`-Hilfsfunktion (siehe REN-222), die mehrere Uploads zu einem Drain bündelt. (4) In `createViewportRT` den zweiten Drain entfernen (der erste hat die GPU schon geleert und seither nichts eingereiht).
- **Erwarteter Effekt**: Schätzung: Editor-Frametime bei mehreren Scene-Viewports deutlich besser (jeder Drain kostet bis zu eine volle Frame-Latenz); Ladezeit mit vielen Icons/Meshes kürzer.
- **Risiko / Verifikation**: Mittel (Synchronisation). `RenderWorldPreview`-Test via `HE_WORLD_PREVIEW_DUMP` (Bildvergleich vorher/nachher).

### REN-219 – Himmel wird als erster Pass über die komplette Bildfläche mit Volumenwolken berechnet und danach überzeichnet
- **ID**: REN-219
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10225-10231` (Aufruf vor allen Draws), `:3036-3085` (drawSky), `:2933-2944` (`DepthEnable = FALSE`), Fallback-Shader `:279-357`
- **Problem**: Der Sky-Pixel-Shader (16 Raymarch-Schritte, je 3 Licht-Schritte mit 4+3 Noise-Fetches, dazu Sterne, Nebel, Aurora) läuft für jedes Pixel des Viewports, bevor die Geometrie gezeichnet wird; überdeckte Pixel (bei Innenräumen oder Terrain meist der größere Teil) werden danach überschrieben. Der Sky-PSO hat keinen Tiefentest.
- **Code-Auszug**:
```cpp
// Sky is independent of scene geometry — draw it before any early returns ...
p.drawSky(cl, p.frameIndex, glm::inverse(svp), p.m_renderWorld.sunDirection, m_environment, ...);
```
- **Lösung**: Sky nach den Opaque-Draws zeichnen, mit `DepthEnable = TRUE`, `DepthFunc = EQUAL`/`LESS_EQUAL` gegen 1.0 und Vertex-Z = 1.0 im `VSSky` (nur Pixel ohne Geometrie werden geshadet). Der Deferred-Pfad rechnet bereits mit "Sky bleibt wo d >= 1" (Kommentar 11329-11334) und müsste Sky vor dem Resolve anders einordnen. Alternativ Early-Out per Stencil-Maske.
- **Erwarteter Effekt**: Schätzung: GPU-Zeit des Sky-Passes sinkt proportional zur Geometrie-Abdeckung (Innenraum bis auf wenige Prozent). Muss gemessen werden, Gewinn hängt vom Cloud-Coverage-Regler ab (`coverage <= 0` überspringt schon den Raymarch).
- **Risiko / Verifikation**: Mittel-Hoch (Passreihenfolge, SSR-Farb-History, Deferred, Decals und Transparenz greifen darauf zu). Nur nach GPU-Messung angehen; Bildvergleich je Pfad. Backend-übergreifend gleiche Struktur in GL/Vulkan/D3D11/Metal.

### REN-220 – TAA-Velocity: ein Draw pro Objekt und zwei Hash-Operationen pro Objekt und Frame
- **ID**: REN-220
- **Kategorie**: B / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:11597-11659`, Member `:1882` (`unordered_map<uint32_t, glm::mat4>`)
- **Problem**: Der Velocity-Pass zeichnet jedes sichtbare Objekt einzeln (Root-CBV 192 B, kein Instancing, `DrawIndexedInstanced(mesh->indexCount, ...)` ohne `DrawIndexRange`), und nutzt `taaPrevTransforms.find(entityId)` plus `taaCurTransforms[entityId] = transform` (zwei Hash-Zugriffe, Map-Knoten-Allokation bei jedem neuen Objekt) pro Objekt und Frame. `taaCurTransforms.clear()` gibt die Knoten jedes Frame frei und allokiert neu.
- **Code-Auszug**:
```cpp
const auto it = p.taaPrevTransforms.find(obj.entityId);
const glm::mat4 prevModel = (it != p.taaPrevTransforms.end()) ? it->second : obj.transform;
p.taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Statt `unordered_map` ein Vektor `{entityId, transform}` pro Frame, sortiert nach `entityId`, mit zwei-Zeiger-Merge oder (einfacher) `vector<glm::mat4>` indiziert über den Objekt-Index plus Parallel-Array `prevIndex` (aus Extractor). Für Instanzgruppen den Instance-Ring nutzen (zwei Matrizen pro Instanz, `mvpPrev` zusätzlich).
- **Erwarteter Effekt**: Schätzung: Entfernt O(n) Hash-Operationen und Allokationen im TAA-Pfad (Messung bei >2 000 Objekten sinnvoll).
- **Risiko / Verifikation**: Gering-Mittel (Reihenfolge der History muss deterministisch bleiben). Test: zwei Frames mit bewegtem Objekt, Velocity-RT per Readback vergleichen.

### REN-221 – `resolveMesh` und `refineWorldBounds` laufen für jedes Objekt mehrfach pro Frame
- **ID**: REN-221
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D12Renderer.cpp:10237-10241` (Bounds), `:10657` (pro Shadow-Batch und Layer), `:10959` (drawDC12), `:6612` (SSAO), `:7838` (GI), `:11618` (TAA); Definition `:8440-8482`
- **Problem**: Jedes Objekt wird pro Frame mindestens in `DrawScene`-Bounds-Schleife, in 1-19 Shadow-Layern, im SSAO-Prepass, im GI-Prepass, im Geometry-Pass und im TAA-Pass über `meshCache.find(UUID)` (128-Bit-Hash) aufgelöst. `refineWorldBounds` rechnet die Welt-AABB jeden Frame neu, auch für unbewegte Objekte. Außerdem nimmt der nicht-gecookte Pfad (`resolveMesh`, 8458-8478) pro Vertex drei `insert`-Aufrufe mit Initializer-Liste (Loose-Assets im Editor).
- **Code-Auszug**:
```cpp
for (RenderObject& obj : p.m_renderWorld.objects)
    if (const GpuMesh* mesh = p.resolveMesh(obj.meshAssetId, m_contentManager))
        obj.refineWorldBounds(mesh->localBounds);
```
- **Lösung**: Bei der Bounds-Schleife den `GpuMesh*` in einem Frame-Scratch-Array (`vector<const GpuMesh*> m_objMesh`, parallel zu `objects`) ablegen und in allen Folgepässen per Objekt-Index nutzen; Draw-Calls tragen ohnehin `meshAssetId`, daher Index in `DrawCall` ergänzen. `refineWorldBounds` nur bei geänderter Transform (Dirty aus Extractor). Interleaving im Loose-Pfad mit einem Index-Loop auf vorallokiertes `built.resize(n*8)`.
- **Erwarteter Effekt**: Schätzung: pro Objekt 5-25 Hash-Suchen pro Frame weniger (grob 0,2-1 ms bei 5 000 Objekten, messen).
- **Risiko / Verifikation**: Gering. Zeiger bleiben gültig, solange `meshCache` nicht rehasht (`unordered_map` hält Knoten stabil; Entfernen nur in `processPendingInvalidations`, vor dem Zeigerbau).

### REN-222 – Boilerplate: 20x Root-Signature-Serialisierung, 8x `compile`-Lambda, 17x `_DEBUG`-Flags, 5 Retire-Listen, 5 One-Shot-Uploads
- **ID**: REN-222
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `compile`-Lambdas `:2112`, `:2303`, `:2801`, `:3192`, `:5404`, `:6180`, `:7134`, `:9590`; `D3D12SerializeRootSignature` 20 Treffer (z. B. `:2146`, `:2208`, `:2263`, `:3244`, `:4778`, `:5114`, `:6087`, `:7167`, `:8607`, `:9304`); Retire-Listen `:1680`, `:3894`, `:4053`, `:4072`, `:4077` mit 5 gleichen Sweeps `:11842-11870`; One-Shot-Uploads `:2700-2793`, `:5632-5720`, `:5724-5789`, `:12664-12740`, `:12751-12823`
- **Problem**: Derselbe Code wiederholt sich: (a) `D3DCompile`-Lambda + Fehlerlog (8 Kopien, leichte Signaturunterschiede), (b) `#ifdef _DEBUG flags |= D3DCOMPILE_DEBUG ...` (17 Kopien), (c) Serialize + `CreateRootSignature` + Fehlerlog (20 Kopien), (d) `D3D12_SHADER_RESOURCE_VIEW_DESC` mit Format/Dimension/Mapping/MipLevels (>30 Kopien, obwohl `srvIntoHeapSlot` in 7582 existiert), (e) Texture-Anlage `makeRT`/`makeTex` Lambdas (`:1906`, `:2001`, `:5823`, `:5963`, `:6015`, `:7450`), (f) fünf parallele "(Ressource, Countdown)"-Listen mit identischem Sweep und der Zahl `k_frameCount + 2` an ~12 Stellen, (g) fünf One-Shot-"Upload + Reset + Execute + Wait"-Blöcke mit je zwei `waitForAllFrames`. Zusammen ca. 1 500-2 000 Zeilen.
- **Code-Auszug**:
```cpp
ComPtr<ID3DBlob> sig, err;
if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
    FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&out))))
```
- **Lösung**: Kleine Helfer im anonymen Namespace oder `D3D12Util.h`: `bool compileHlsl(src, entry, profile, name, ComPtr<ID3DBlob>&)` (ein `constexpr UINT kFxcFlags`), `bool makeRootSig(device, desc, ComPtr<ID3D12RootSignature>&, const char* what)`, `D3D12_GRAPHICS_PIPELINE_STATE_DESC fullscreenPsoDesc(rs, vs, ps, rtFmt)` mit Blend-/Raster-Voreinstellungen, `srvTex2D(device, res, fmt, cpuHandle)`, `makeRenderTarget(...)`, und ein `DeferredRelease`-Queue (Fence-Wert statt Frame-Countdown, exakter und ohne Sweep pro Liste) sowie `uploadTexture1Shot(...)` für die Init-/Editor-Pfade. Die Funktionen sind rein mechanisch und lassen sich in Schritten ersetzen.
- **Erwarteter Effekt**: Lesbarkeit und Fehleranfälligkeit (ein Stück Code statt 20); keine Laufzeitänderung (außer exakteres Retire).
- **Risiko / Verifikation**: Gering (mechanisch). Verifikation: `D3D12MaterialRootSignature.h`-WARP-Tests aus `tests/` laufen weiter; Build mit/ohne `HE_HAVE_SHADERC`.

### REN-223 – Eine Übersetzungseinheit mit ca. 7 300 Zeilen Inline-Struct und 1 200 Zeilen eingebettetem HLSL
- **ID**: REN-223
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `D3D12Renderer.cpp:122-1213` (HLSL-Strings), `:1412-8760` (`struct D3D12RendererImpl` mit allen Passes), nur wenige Member sind ausgelagert (`createMaterialResources` 8774, `createSkyEnvCube` 8935, `GetOrBuildMaterialPSO` 9021, `EnsureDecalPipeline` 9228, `createDeferredPipelines` 9577)
- **Problem**: Post-FX, Underwater, TAA, Sky, Debug, World-Preview, UI, SSAO, SSR, GI (SW + DXR), Materialsystem, Decals, Deferred und Frame-Loop teilen eine Datei und einen Zustand. Folgen: jede Änderung baut 12,9k Zeilen neu (Template-/Header-schwere Includes `d3d12.h`, `glm`, `RenderGraph`), Merge-Konflikte, und die Reviewbarkeit sinkt (CLAUDE.md warnt bereits: nie ganz lesen). Die HLSL-Quellen (Sky-Fallback, Scene, UI, FXAA/SMAA, Bloom) liegen als Raw-Strings im Code statt in `Backends/D3D_Shared/HlslSources.h` bzw. `.hlsl`-Ressourcen.
- **Lösung**: Impl in Features aufteilen, die bereits als Muster vorhanden sind (siehe Out-of-Line-Funktionen): `D3D12Impl.h` (Struct-Deklaration), `D3D12PostFx.cpp`, `D3D12Ssao.cpp`, `D3D12Ssr.cpp`, `D3D12Gi.cpp` (+DXR), `D3D12Ui.cpp`, `D3D12Materials.cpp`, `D3D12Decals.cpp`, `D3D12Frame.cpp` (DrawScene/Render). HLSL: D3D12-eigene Strings nach `D3D12Hlsl.h`. Reihenfolge: zuerst HLSL auslagern (kein Risiko), dann pro Feature ein Commit.
- **Erwarteter Effekt**: Schätzung: Inkrementelle Build-Zeit der Datei sinkt auf ein Drittel bis Viertel; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Gering (reine Verschiebung); Build für alle Plattformen/Backends; `tests/test_culling.cpp` prüft Text-Drift von `kSceneHLSL` gegen andere Backends (`"drift test"` Kommentar 771-777), also muss diese Datei-/Symbolreferenz beim Auslagern angepasst werden.

### REN-224 – Kleinfunde: doppelte Includes, tote/unnötige Berechnung, redundante Clears, Per-Frame-Vektoren, Fallback-Sky
- **ID**: REN-224
- **Kategorie**: F / C
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: siehe Liste
- **Problem / Lösung** (je Zeile belegt):
  1. `#include <cmath>` doppelt (`:53`, `:63`); Zeile 37 enthält zwei angehängte Kommentare (`// TAA jitter sequence ... // GL depth (-1..1) -> D3D depth (0..1)`, der zweite gehört zu `ClipSpace.h` Zeile 36).
  2. `drawSky` baut `SkyCB cb` immer (`:3058-3068`), obwohl bei `skyFullModel` nur `sp` kopiert wird (`:3071`); Bau in den `else`-Zweig verschieben.
  3. `makeTex`-Lambda in `ensureGiShadowTargets` hat den ungenutzten Parameter `clearColor` (`:7450`, `(void)clearColor` in `:7467`); entfernen.
  4. SSAO-Kernel hartkodiert `32` (`:5800`) statt `HE::kSsaoKernelSize`; `ssaoBias` wird in `SetSSAOSettings` ständig auf 0.025 gesetzt (`:12837`).
  5. SSAO-/Blur-Ziele werden voll geclear't (`:6696`, `:6731`), obwohl das Fullscreen-Dreieck jedes Pixel überschreibt (3 Clear-Kommandos pro Frame sparbar).
  6. Pro Frame neu angelegte Vektoren in `DrawScene`: `opaqueDCs`, `transparentDCs` (`:10735`), `deferredForwardDCs` (`:10948`), `ribbonDCs/ribbonMeshes/ribbonMeshByDC` (`:10877-10879`); als Member mit `clear()` wiederverwenden.
  7. `SetDebugLines` kopiert den Vektor jedes Frame (`:12661`); `RenderWorldPreview` wandelt Gitter-Floats in `DebugLine` und `drawDebugLines` zurück in Floats (`:12492-12500`, `:3103-3108`).
  8. Fallback-Sky `kSkyPSHLSL12` (`:122-422`, ca. 300 Zeilen) ist die dritte Kopie der Sky-Logik (neben `kSkyFS` und der Metal-Spiegelung) und weicht schon ab ("old single-colour nebula"). Wenn Windows-Builds immer `HE_HAVE_SHADERC` haben, kann er entfallen; sonst als vorkompilierter HLSL-Blob aus dem Cross-Compiler mitliefern. Unsicher, ob ein Windows-Build ohne ShaderC unterstützt wird.
  9. `UICB` belegt 256-B-Slots für 176 B (`:4887-4891`, siehe REN-216).
- **Erwarteter Effekt**: Klein; Lesbarkeit und einzelne Allokationen/Befehle weniger.
- **Risiko / Verifikation**: Sehr gering. Punkt 8 braucht eine Entscheidung zur Build-Matrix.

---

## Gelesen

Ja: `src/HE_Rendering/src/Backends/D3D12/D3D12Renderer.cpp` vollständig von Zeile 1 bis 12 904 in Blöcken (kein Teil übersprungen); zur Absicherung der Cross-Modul-Hinweise zusätzlich punktuell `MaterialShaderLibrary.cpp:941-1009`, `MaterialShaderLibrary.h:222-245`, `RenderSorter.h` (SortKey), `D3D_Shared/HlslSources.h:39-65` und die genannten Parallelstellen in D3D11/GL/Metal/Vulkan per Grep.

## Rendering Slice 3 (+ Slice 5) – D3D12-Header/-ShaderManager, D3D_Shared/HlslSources.h, MetalRenderer.h (+ MetalShaderManager)

## Zusammenfassung

Slice 3 besteht fast nur aus Deklarationen und Shader-Quelltext. Die Logik sitzt in den zugehörigen `.cpp`/`.mm`, die in anderen Slices liegen. Die Funde betreffen deshalb vor allem Struktur und Wartbarkeit (`MetalRenderer.h`) und GPU-seitige Kosten in `HlslSources.h`. Wo ein Header-Kommentar eine Laufzeitannahme macht, habe ich sie gegen das `.mm` geprüft (Zeilen angegeben). `D3D12Renderer.h` ist mit 120 Zeilen und Pimpl sauber. `HlslSources.h` ist durchdacht und gut dokumentiert. Slice 5 (Metal-Shader-Manager) ist klein und hat kein Laufzeitproblem, aber toten Code und eine Vertrags-Inkonsistenz zu den anderen vier Managern.

Anzahl Funde: Hoch 2, Mittel 7, Niedrig 12 (gesamt 21; davon Slice 5: REN-351 bis REN-354, 0 Hoch / 1 Mittel / 3 Niedrig).

## Funde (Slice 3)

### REN-301 – `MetalRenderer` ist eine 1400-Zeilen-Gottklasse mit offenem Innenleben im öffentlichen Header
- **ID**: REN-301
- **Kategorie**: A (auch F: schwerer Header)
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/include/Backends/Metal/MetalRenderer.h:77-1415` (Includes 5-19)
- **Problem**: Der gesamte Renderzustand von rund 25 Features steht als Member im Header: etwa 340 `m_*`-Felder, davon 164 rohe `void*` für Objective-C-Objekte, die manuell mit `CFBridgingRetain`/`Release` verwaltet werden. Jedes Feature (SSR, GI-Reflexionen, DDGI, TAA, DoF, MotionBlur, Bloom, Wolken, SSAO, Decals, Previews, Thumbnails, Partikel …) wiederholt dasselbe Muster aus Pipeline-`void*`, Textur-`void*`, `m_xxxW/H`, `bool m_xxxTried`, `EnsureXxx()` und `DestroyXxx()`. Allein acht „run-once"-Flags (`...Tried`/`...Checked`: 253, 274, 364, 400, 540, 694, 1051, 1135) bilden dasselbe Lazy-Build-Muster ab. Der Header beschreibt außerdem das ganze Innenleben für jeden Includer (`RendererFactory.cpp:25`, `EditorUI.cpp:168`, `EditorApplication.cpp:271`). Die Parallelstellen D3D11 (96 Zeilen) und D3D12 (120 Zeilen) nutzen Pimpl. Der Header-Kommentar „This header stays plain C++" erreicht dasselbe Ziel schlechter.
- **Code-Auszug**:
```cpp
void* m_ssrTracePipeline     = nullptr; // id<MTLRenderPipelineState>
void* m_ssrCompositePipeline = nullptr;
void* m_ssrBlurPipeline      = nullptr;
bool  m_ssrPipelinesTried    = false;
void* m_ssrReflTex = nullptr;  int m_ssrReflW = 0, m_ssrReflH = 0;
...
bool  EnsureSSRPipelines();  void EnsureSSRTarget(int,int);  void DestroySSRTarget();
```
- **Lösung**:
  1. Schritt 1, ohne Verhaltensänderung: kleines Handle-Template (`MtlRef`, nur im `.mm`) für retained Objective-C-Objekte, das im Destruktor `CFBridgingRelease` aufruft. Damit entfallen die ~160 Destroy-Zeilen und die Leak-Gefahr bei neuen Features.
  2. Schritt 2: Feature-Substrukturen im `.mm` (`struct SsrState`, `GiReflState`, `TaaState`, `DofState`, …) mit eigenen `Ensure`/`Destroy`. Der Header hält nur noch `std::unique_ptr<MetalRendererImpl> m_impl` wie D3D11/D3D12. Public API (die `override`-Zeilen) bleibt unverändert.
  3. Ein `LazyPipeline`-Helfer (`tried`, `ptr`, `build()`) ersetzt die acht Tried-Flags.
- **Erwarteter Effekt** (Schätzung): Header um etwa 85 % kleiner. Inkrementelle Rebuilds der Editor-TUs (`EditorUI.cpp`, `EditorApplication.cpp`) werden spürbar kürzer, denn heute zieht jede Änderung an einem Feature-Member alle drei Includer nach. Neue Features brauchen deutlich weniger Boilerplate.
- **Risiko / Verifikation**: Rein mechanisch, aber riesige Diff-Fläche (`.mm` hat 18158 Zeilen). In Etappen pro Feature umsetzen. Absicherung mit den vorhandenen Culling- und Render-Tests (`tests/test_culling.cpp` greift laut Kommentar Zeile 662 auf `ExtractFrame` zu), dazu Screenshot-Vergleich (`HE_HIDDEN_WINDOW=1`) pro Feature.

### REN-302 – Pro Frame frische `MTLBuffer` trotz vorhandenem `FrameUploadRing`; Header-Kommentar rechtfertigt es mit veraltetem Stand
- **ID**: REN-302
- **Kategorie**: C / D
- **Schweregrad**: Hoch (Hot Path: skinned Objekte, Partikel, Ribbons; Größenordnung unsicher, messen)
- **Typ**: Performance
- **Ort**: Header `MetalRenderer.h:810-820` (Kommentar), `1146-1153`, `1200-1201`, `1212`, `1372`; Gegenstück `1377-1393` (Ring). Nutzstellen in `MetalRenderer.mm:13947` (Bone-Buffer je skinned Objekt), `17832` (Instanzbuffer je Partikel-Batch), `7183-7191` (Ribbon-VB/IB), `8279`, `8476-8479`, `8556-8559` (GI-Instanzbuffer je Frame).
- **Problem**: Der Kommentar bei `DrawParticleGraphBatches` behauptet, Metal gebe keine implizite Synchronisierung und „a few emitters' worth of small per-frame allocations is a non-issue". Mit dem Upload-Ring (Perf-Audit B6, `UploadTransient`, Zeilen 1377-1393) gibt es aber seit dem Kommentar eine Lösung für genau diesen Fall. Trotzdem legen mindestens diese Pfade pro Frame `newBufferWithBytes` an. Beim Bone-Buffer ist es ein 128×64 B (`kMaxBones * sizeof(glm::mat4)`) großer Buffer pro skinned Objekt und Frame. Das ist in Szenen mit vielen Charakteren ein Hot Path (Allokator-Roundtrip plus Autorelease-Druck im Pool).
- **Code-Auszug**:
```cpp
// .mm:13947
id<MTLBuffer> boneBuf = [device newBufferWithBytes:boneScratch.data()
                                            length:kMaxBones * sizeof(glm::mat4)
                                           options:MTLResourceStorageModeShared];
// .mm:17832
id<MTLBuffer> instBuf = [device newBufferWithBytes:inst.data() ...
```
- **Lösung**: Diese Stellen auf `UploadTransientBytes(data,len,buf,offset)` umstellen. Dann `setVertexBuffer:buf offset:offset`. Den Ribbon-Pfad zusätzlich vom Retain-Vektor `m_ribbonBuffers` lösen. Danach den veralteten Kommentar 810-820 ersetzen. Für `m_giTlas`/`m_giInstanceBuffer` (1152-1153) bleibt die „frisch pro Frame"-Strategie wegen der Acceleration-Structure-Lebensdauer ggf. nötig. Der reine Instanz-/Farb-/Land-Index-Upload kann trotzdem in den Ring.
- **Erwarteter Effekt** (Schätzung, nicht gemessen): CPU-Zeit im Encoding um einen niedrigen einstelligen Mikrosekundenbetrag pro skinned Objekt weniger. Bei einigen hundert animierten Objekten ist das ein messbarer Anteil der Frame-Zeit. Weniger Pool-Druck.
- **Risiko / Verifikation**: Offsets müssen Metal-Anforderungen erfüllen (Konstantenpuffer-Alignment 256 B bei `setVertexBuffer` nicht nötig, bei Argument-Buffern prüfen). Ring-Frame muss geöffnet sein, sonst Fallback (steht schon im Kommentar). Vorher/nachher mit dem Profiler (`GetFrameGpuStats`, CPU-Zähler) und Szene mit vielen Skinned Meshes messen.

### REN-303 – Blockierendes `waitUntilCompleted` pro neuem Mesh beim BLAS-Aufbau (Hitch bei GI-Aktivierung)
- **ID**: REN-303
- **Kategorie**: E / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: Deklaration `MetalRenderer.h:1229` (`void* BuildBLAS(const GpuMesh&)`) und `185-187`; Implementierung `MetalRenderer.mm:8150-8164`
- **Problem**: Jedes bisher ungesehene Mesh bekommt einen eigenen Scratch-Buffer, einen eigenen Command Buffer und danach ein hartes `waitUntilCompleted` auf dem Render-Thread. Beim ersten GI-Frame einer Szene mit N Meshes sind das N serialisierte CPU/GPU-Roundtrips. Der Kommentar nimmt den „kleinen Hitch" bewusst in Kauf, skaliert aber linear mit N und wird im Editor beim Einschalten von GI sichtbar.
- **Code-Auszug**:
```cpp
id<MTLBuffer> scratch = [device newBufferWithLength:... options:MTLResourceStorageModePrivate];
id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
... buildAccelerationStructure:blas ...
[cmdBuf commit];
[cmdBuf waitUntilCompleted];
```
- **Lösung**: `EncodeGIAccelBuild(cmdBuf, aspect)` bekommt schon den Frame-`cmdBuf` (Header 1235). Alle fehlenden BLAS in einem Acceleration-Structure-Encoder dieses Command Buffers bauen, mit einem gemeinsamen Scratch-Buffer (Größe = Maximum, seriell wiederverwendet; der Encoder serialisiert Builds mit Barriere). Der Scratch-Buffer geht per `RetireGIObject` (Header 1225) ans Ende. Das TLAS-Build im selben Command Buffer sieht die BLAS durch Metals Hazard-Tracking. `waitUntilCompleted` entfällt. Alternativ pro Frame auf K Meshes budgetieren (Aufbau über mehrere Frames, TLAS überspringt Instanzen ohne fertige BLAS).
- **Erwarteter Effekt** (Schätzung): Einmalige Spitze von N × (Submit + GPU-Latenz) wird zu einer Spitze von einer GPU-Zeit. Szenenabhängig, bei dutzenden Meshes im Bereich zweistelliger Millisekunden. Unsicher / messen.
- **Risiko / Verifikation**: Ordnungs- und Residency-Fragen (`useResource` für BLAS in Compute-Encodern ist in `m_giUniqueBlas` schon vorgesehen, Header 1240). Mit `HE_GI_FORCE_SW` unberührt. Prüfen mit Metal-API-Validation und einer Szene mit vielen Meshes.

### REN-304 – Bis zu vierfaches Extract/Cull/Sort pro Metal-Frame
- **ID**: REN-304
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `MetalRenderer.h:591-606` (`CullCameraObjects`), `438-441` (`EncodeGBuffer`: „extract/cull/sort of its own"), `1120-1123` (`EncodeSSAO`: „Runs its own extract/cull/sort"), `1279-1282`
- **Problem**: Der Kommentar nennt es selbst: „the Metal frame runs it up to four times (GI G-buffer, SSAO pre-pass, scene, deferred G-buffer), each after its own extraction". Gecacht ist nur der teure Occlusion-Anteil (`m_occlusionCache*`). Frustum-Culling und Sort laufen weiterhin pro Pass. Sie sind deterministisch und liefern denselben Draw-Satz, solange Kamera, Aspect und Weltstand gleich sind (der Extractor hat dafür schon einen Key-Vergleich, Header-Kommentar 655-663). Bei großen Szenen ist das ein pro-Frame-Vielfaches.
- **Code-Auszug**:
```cpp
// Header 591-594
// ... the Metal frame runs it up to four times (GI G-buffer, SSAO pre-pass,
// scene, deferred G-buffer), each after its own extraction.
void  CullCameraObjects();
```
- **Lösung**: Das Ergebnis (`m_visible` + `m_sortedIndices`) analog zum Occlusion-Cache mit Schlüssel (Frame-Stamp, Walk-Zähler des Extractors, View-Proj, Objektanzahl) halten und bei Treffer weiterverwenden. Wichtig: SSAO und GI nutzen ggf. andere Filter (z. B. Skinned oder Caster-Flags). Der Cache gilt nur für den reinen Kamera-Cull, wie `CullCameraObjects` ihn schon kapselt. Ein Trefferzähler im Profiler zeigt den Nutzen.
- **Erwarteter Effekt** (Schätzung): Bei N Objekten bis zu zwei bis drei von vier Frustum-Cull-Läufen und Sorts pro Frame gespart. Absolute Zahl hängt von N ab. Messen mit dem vorhandenen CPU-Zähler.
- **Risiko / Verifikation**: Mittel. Deferred und Forward unterscheiden sich im Aspect/Jitter (`JitteredViewProj`). Ein falscher Cache-Treffer würde Objekte fälschlich verwerfen. Test: `tests/test_culling.cpp` um Fälle mit gleichem/anderem Key erweitern; Bildvergleich Deferred/Forward/TAA an.

### REN-305 – GI: lineare Instanzschleife pro Strahl ohne grobe Vorabprüfung
- **ID**: REN-305
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h:829-859` (`giSceneAnyHit`, `giSceneClosestHit`), Aufrufer `944-951`, `919`, `1236`, `1258`
- **Problem**: Jeder Strahl (Sun-Cone, Local-Lights, Probe-Shadow-Rays, Probe-Hauptstrahl) testet alle Instanzen nacheinander. Pro Instanz werden eine 4×4- und eine 3×3-Matrixmultiplikation ausgeführt (Transformation des Strahls in den Objektraum), erst danach der Root-AABB-Test im BLAS-Traversal. Die Kosten wachsen also mit der Instanzzahl × Strahlzahl, und die Transformation ist dort die Haupt-Eintrittskosten. Der Kommentar nennt es offen „TLAS analogue: linear instance loop". `giInsts[i]` wird zudem je Iteration zweimal indiziert.
- **Code-Auszug**:
```hlsl
for (int i = 0; i < uGiCount.x; ++i)
{
    float3 oL = mul(giInsts[i].invTransform, float4(o, 1.0)).xyz;
    float3 dL = mul((float3x3)giInsts[i].invTransform, d);
    ...
    if (giBlasHit(giInsts[i].offsets.x, giInsts[i].offsets.y, oL, dL, tMin, tMax, true, t))
```
- **Lösung**: (a) kurzfristig: `GiInst inst = giInsts[i];` einmal laden; (b) World-Space-AABB pro Instanz (CPU schreibt sie in ein freies `offsets.zw`/Padding oder in `baseColor.w`-freie Lane, die Struktur ist 96 B bei `float4x4 + float4 + int4`) und vor der Matrix-Transformation einen Slab-Test im Weltraum machen; (c) mittelfristig ein TLAS-BVH über Instanzen (GiBvh.cpp bietet die Bausteine). Wirkt identisch auf die Parallelstellen: GL `gi_*.comp`, Vulkan, Metal `kGISWMSL`.
- **Erwarteter Effekt** (Schätzung): Bei Szenen mit Hunderten Instanzen deutlich weniger GPU-Zeit im GI-Shadow- und Probe-Pass. Bei wenigen Instanzen vernachlässigbar. Messen mit GPU-Pass-Timer des jeweiligen Backends.
- **Risiko / Verifikation**: Strukturlayout ist auf CPU-Seite (D3D11, D3D12, GL, Vulkan, Metal-SW `GISwInstanceCPU`, Header 1180) fix. Bei (b) alle fünf Parallelstellen und `GiBvh`-Tests anpassen. Bit-gleiches Ergebnis ist machbar, denn die Vorprüfung ist konservativ.

### REN-306 – Einstellungs-Structs werden in Einzelfelder zerlegt statt gespeichert
- **ID**: REN-306
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `MetalRenderer.h:264-269` (SSR), `339-347`, `360-361` (GI-Reflexion), `983-987` (Bloom), `1010-1013` (DoF), `1032-1035` (MotionBlur), `1114-1117` (SSAO), `1138-1145` (GI); Gegenbeispiel `647-648` (`ShadowSettings m_shadowSettings`)
- **Problem**: Für `SetBloomSettings`, `SetSSAOSettings`, `SetGISettings`, `SetSSRSettings`, `SetDepthOfFieldSettings`, … kopiert die Implementierung jedes Feld in ein eigenes Member mit teils abweichendem Namen/Typ (`m_ssrQuality` als `int`, `m_ssaoMethod` als `int`). Das sind rund 40 Felder mit doppelter Pflege: Kommt ein Settings-Feld hinzu, muss ein Member, ein Setter und jede Lesestelle angepasst werden. `m_shadowSettings` zeigt, dass es auch direkt als Struct geht.
- **Code-Auszug**:
```cpp
bool  m_ssrEnabled      = false;
float m_ssrIntensity    = 1.0f;
float m_ssrMaxRoughness = 0.6f;
float m_ssrMaxDistance  = 30.0f;
float m_ssrThickness    = 0.5f;
int   m_ssrQuality      = 1;      // 0 = 16 steps, 1 = 32, 2 = 64
```
- **Lösung**: `SSRSettings m_ssr;` (und analog) halten, Setter wird zu `m_ssr = s;` plus ggf. Dirty-Flag. Lesestellen `m_ssr.intensity`. Schritt für Schritt je Feature. Es hilft zugleich REN-301.
- **Erwarteter Effekt** (Schätzung): Etwa 40 Member und 8 Setter-Rümpfe weniger. Keine Laufzeitwirkung.
- **Risiko / Verifikation**: Niedrig, aber jede Lesestelle im 18k-Zeilen-`.mm` muss gefunden werden (Compiler hilft beim Umbenennen). Wo der Setter Seiteneffekte hat (Target-Neuanlage bei Qualitätswechsel), diese erhalten.

### REN-307 – Sieben handgeschriebene Offscreen-Ziele (Color/Depth/Größe) für Previews und Thumbnails
- **ID**: REN-307
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `MetalRenderer.h:701-706` (Material), `713-717` (Skeletal), `733-741` (`WorldPreviewTarget`), `747-750` (Partikel), `758-767` (Thumbnail + UI-Thumbnail); allgemeiner dieselbe Struktur bei HDR/LDR/Bloom/DoF/MB/UC/Cloud (`828-831`, `938-941`, `979-982`, `1004-1009`, `1028-1031`, `1048-1051`, `1062-1064`)
- **Problem**: Das Paar „Color-Textur + Depth-Textur + Größe + EnsureXxx/DestroyXxx" ist rund zehnmal von Hand angelegt, obwohl `WorldPreviewTarget` (733) das Muster schon als Struct zeigt. Jede Kopie hat ihre eigene Resize- und Release-Logik; sie driften bereits (z. B. teils `RetireTexture`, teils direkt freigegeben).
- **Code-Auszug**:
```cpp
void* m_previewColorTex = nullptr;  void* m_previewDepthTex = nullptr;  int m_previewSize = 0;
void* m_skelPreviewColorTex = nullptr; void* m_skelPreviewDepthTex = nullptr; int m_skelPreviewW = 0, m_skelPreviewH = 0;
void* m_particlePreviewColorTex = nullptr; void* m_particlePreviewDepthTex = nullptr; int m_particlePreviewSize = 0;
```
- **Lösung**: Ein `struct OffscreenTarget { void* color; void* depth; int w, h; bool ensure(device, fmt, w, h); void release(retire); }` im `.mm`. Alle Previews und Thumbnails nutzen es.
- **Erwarteter Effekt** (Schätzung): Etwa 150 bis 250 Zeilen `.mm` weniger. Einheitliches Retire-Verhalten.
- **Risiko / Verifikation**: Niedrig. Formate unterscheiden sich (RGBA16F vs. BGRA8, 765), also als Parameter. Smoke-Test über die Preview-/Thumbnail-Tests im Editor (`HE_UI_DUMP_DIR`).

### REN-308 – TAA-Transform-Historie als zwei `unordered_map` mit Per-Frame-Einträgen
- **ID**: REN-308
- **Kategorie**: C / D
- **Schweregrad**: Mittel (nur bei aktivem TAA, objektzahlabhängig)
- **Typ**: Performance
- **Ort**: `MetalRenderer.h:898-899`; Nutzung `MetalRenderer.mm:12973-13029`
- **Problem**: Jedes Frame wird `m_taaCurTransforms.clear()` aufgerufen und danach pro Objekt `m_taaCurTransforms[entityId] = transform` eingefügt. Danach folgt `swap`. `std::unordered_map` allokiert pro Eintrag einen Knoten (`malloc` pro Entity pro Frame) und der Lookup in der Vorgänger-Map ist ein weiterer Hash-Zugriff. Bei tausenden Objekten sind das tausende Allokationen pro Frame im Velocity-Pass.
- **Code-Auszug**:
```cpp
std::unordered_map<uint32_t, glm::mat4> m_taaPrevTransforms;
std::unordered_map<uint32_t, glm::mat4> m_taaCurTransforms;
// .mm
const auto it = m_taaPrevTransforms.find(obj.entityId);
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Zwei `std::vector<std::pair<uint32_t,glm::mat4>>` (oder ein Flat-Hash-Map), die pro Frame mit `clear()` (Kapazität bleibt) gefüllt und nach `entityId` sortiert werden. Alternativ dichter Vektor mit Slot-Index pro Entity, falls die IDs dicht sind. Der Lookup ist dann ein Binary Search oder Index. Das `swap` bleibt.
- **Erwarteter Effekt** (Schätzung): Null Heap-Allokationen im eingeschwungenen Zustand statt einer pro Objekt und Frame; Velocity-Pass-CPU-Zeit bei großen Szenen messbar kleiner. Messen.
- **Risiko / Verifikation**: Niedrig. Verhalten für „erstmals gesehen → keine Bewegung" (12994-13006) muss bleiben. Test: zwei Frames mit bewegtem/neuem Objekt, Velocity-Ausgabe vergleichen.

### REN-309 – Veraltete Kommentare bei GI und Header-Größenangaben
- **ID**: REN-309
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MetalRenderer.h:1130-1134`, `1284-1298`; `MetalRenderer.mm:2686-2690` vs. `3645-3647`; `HlslSources.h:3-5`
- **Problem**: (a) Header 1130-1134: „This checkpoint only builds the structures; the ray-traced shadow pass and DDGI probe pass that consume them land in later checkpoints." Beides ist längst vorhanden (`EncodeGIShadowRays`, `EncodeGIProbeUpdate`, Zeilen 1282, 1318). (b) Header 1292-1297: „no secondary shadow ray at the hit point (hit surfaces are treated as fully sun-lit)", aber der SW-Kernel hat sie (`.mm:3645-3647` „hit surfaces are no longer assumed fully sun-lit"), ebenso die HLSL-Variante (`HlslSources.h:1234-1237`). Der HW-Kernel-Kommentar `.mm:2686-2690` behauptet weiterhin das Gegenteil. Entweder ist der HW-Kernel wirklich zurück (dann ist es eine Verhaltensabweichung zwischen HW und SW-Pfad) oder der Kommentar ist falsch. Unsicher, im Kernel prüfen. (c) `HlslSources.h:3-4` spricht von „~750 lines" bei 1352 Zeilen.
- **Code-Auszug**:
```cpp
// no secondary
// shadow ray at the hit point (hit surfaces are treated as fully sun-lit) ...
```
- **Lösung**: Kommentare auf den Ist-Stand bringen. Für (b) zuerst den HW-Kernel (`.mm` um 2680-2700) gegen den SW-Kernel lesen. Fehlt der Sekundärstrahl dort, ist das ein Funktionsfund für den `.mm`-Slice (Probe-Beleuchtung weicht zwischen HW und SW ab).
- **Erwarteter Effekt**: Weniger Fehlinformation für die nächste Person; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Keines (nur Kommentare), außer (b) deckt eine echte Abweichung auf.

### REN-310 – Zwei identische Retire-Queues
- **ID**: REN-310
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MetalRenderer.h:1223-1227` (`RetiredGIObject`, `RetireGIObject`, `AgeRetiredGIObjects`, `DrainRetiredGIObjects`) und `1352-1356` (`RetiredTexture`, `RetireTexture`, `AgeRetiredTextures`, `DrainRetiredTextures`)
- **Problem**: Beide haben dieselbe Form (`{void*, int framesLeft}`, Vektor, Retire/Age/Drain). Der Kommentar bei 1219-1222 sagt selbst „same lifetime problem and fix as m_retiredTextures/RetireTexture, generalised to any retained Objective-C object". Dann reicht die generalisierte Variante.
- **Code-Auszug**:
```cpp
struct RetiredGIObject { void* object; int framesLeft; };
struct RetiredTexture  { void* texture; int framesLeft; };
```
- **Lösung**: `RetireTexture(t)` durch `RetireObject(t)` ersetzen, eine Queue, ein `Age`, ein `Drain`.
- **Erwarteter Effekt** (Schätzung): Etwa 30 Zeilen weniger, eine Stelle statt zwei beim Anpassen der Verzögerung.
- **Risiko / Verifikation**: Niedrig; die Verzögerungsanzahl (`framesLeft`-Startwert) muss in beiden Fällen gleich sein oder bleibt parametrisiert.

### REN-311 – `ResolveGraphTexture` baut einen `std::string`-Schlüssel pro Aufruf
- **ID**: REN-311
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: Deklaration `MetalRenderer.h:509`, `1330-1331`; Implementierung `MetalRenderer.mm:9511-9517`; Aufrufer u. a. `.mm:7179` (Ribbons, pro Frame), `13567`, `14457` (nur im Cache-Miss-Block, dort unkritisch)
- **Problem**: Der Schlüssel entsteht jedes Mal aus zwei `std::to_string` und einer String-Verkettung (plus `#arr`); danach ein String-Hash. Das ist pro Textur-Slot und Aufruf eine Heap-Allokation (Schlüssel länger als die SSO-Grenze). Wo der Aufruf pro Frame läuft (Ribbon-Kollektion, 7179), summiert es sich.
- **Code-Auszug**:
```cpp
std::string key = texId != HE::UUID{}
    ? (std::to_string(texId.hi) + ":" + std::to_string(texId.lo)) : path;
...
if (array) key += "#arr";
```
- **Lösung**: Schlüsseltyp `struct GraphTexKey { HE::UUID id; uint8_t array; }` (Hash über `hi/lo`) plus getrennte Map für pfadbasierte Texturen. Dann entfällt jede Stringbildung für UUID-Texturen.
- **Erwarteter Effekt** (Schätzung): Pro Aufruf eine Allokation und mehrere hundert Nanosekunden weniger. Praktisch nur relevant bei vielen Ribbon-/Material-Batches pro Frame. Messen.
- **Risiko / Verifikation**: Niedrig. `m_uiTexCache` teilt die Schlüssel-Konvention (.mm 9531), beide gemeinsam umstellen. `.mm:16543-16550` (Invalidate) iteriert über die Maps und muss mitziehen.

### REN-312 – HLSL-Quelltext wiederholt dieselben Deklarationen in jeder Konstante
- **ID**: REN-312
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `HlslSources.h:116-122`, `705-711`, `1301-1307` (identischer `PerObject`-Cbuffer), `135`, `1317` (identisches `VSOut`), `In`/`FsIn`-Struct in `205`, `369`, `407`, `450`, `515`, `624`, `678`, `989`, `1071`
- **Problem**: Der `PerObject`-Cbuffer und das Skinned-`VSOut` stehen dreimal bzw. zweimal wortgleich da. Das Fullscreen-Eingangs-Struct `{float4 pos : SV_POSITION; float2 uv : TEXCOORD0;}` steht neunmal. Layout-Änderungen (z. B. ein weiteres Feld in `PerObject`) müssen an drei Stellen und zusätzlich in `kSceneHLSL` beider Backends (nicht in diesem Slice) nachgezogen werden. Die Kommentare (z. B. 703 „Reuses the PerObject cbuffer layout") zeigen, dass das Kopplung ist, die nur im Kopf der Autoren gesichert wird.
- **Code-Auszug**:
```hlsl
cbuffer PerObject : register(b0)
{ float4x4 uMVP; float4x4 uModel; float4 uColor; float4 uPBR; };
```
- **Lösung**: Kleine Prelude-Konstanten (`kPerObjectCBHLSL`, `kFsInHLSL`), die die Backends wie `kSkyFuncHLSL` schon per `std::string(...) + ...` voranstellen (z. B. `D3D11Renderer.cpp:4145`, `D3D12Renderer.cpp:2844`). Alternativ ein `#include`-Handler im `D3DCompile`-Aufruf.
- **Erwarteter Effekt**: Rund 40 Zeilen weniger, eine Quelle für das Cbuffer-Layout. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Niedrig. Byte-Identität der kompilierten Shader prüfen (Hash des Bytecodes vorher/nachher). Der Registerbezug (`b0`) bleibt.

### REN-313 – GI-Denoiser: teure Pro-Tap-Mathematik und inkonsistente `Sample`/`SampleLevel`
- **ID**: REN-313
- **Kategorie**: D
- **Schweregrad**: Niedrig (unsicher, messen)
- **Typ**: Performance
- **Ort**: `HlslSources.h:1100-1112` (A-Trous, 25 Taps × zwei Iterationen), `1034-1048` (Temporal, 5×5-Fenster), `1005-1008`/`1093-1096` (Footprint-Berechnung doppelt)
- **Problem**: (a) A-Trous: pro Tap werden `exp`, `pow(x, 32)`, ein zweites `exp`, ein `normalize` und zwei Texturzugriffe auf `uGNorm`/`uGPos` ausgeführt. `pow(max(dot,0), 32)` lässt sich durch fünf Quadrierungen ersetzen. Die gespeicherte Normale ist bereits normiert (`GiGBufPS`, Zeile 741), das erneute `normalize` kostet nur dann etwas Genauigkeit, wenn das Zielformat quantisiert. (b) Temporal: 25 Fetches und 9 Summen über je 9 Werte (81 Additionen); mit Spaltensummen (5 Spalten × 3-Fenster) ginge es mit etwa 15 Additionen. (c) Der Footprint (4 Nachbar-Taps plus `min`/`max`) wird in Temporal und A-Trous identisch berechnet (Kommentar „same constants (SYNC)"), eine Zwischenspeicherung im G-Buffer wäre denkbar. (d) `kGiTemporalHLSL` nutzt `Sample` hinter divergenten frühen `return`s (996-1000); der A-Trous-Kommentar 1058-1059 nennt genau das als Grund für `SampleLevel`. Bei Point-Sampler/Single-Mip ist das Ergebnis gleich, aber FXC kann dafür Warnungen oder Gradient-Anomalien erzeugen.
- **Code-Auszug**:
```hlsl
w *= exp(-abs(dot(n, q.xyz - pv.xyz)) / (1.0 * fp));
w *= pow(max(dot(n, normalize(uGNorm.SampleLevel(uPointSamp, uv, 0).xyz)), 0.0), 32.0);
w *= exp(-abs(v - c) / (2.0 * sig + 1e-3));
```
- **Lösung**: `pow` durch Quadrierungskette ersetzen (Ergebnis nicht bit-gleich, aber innerhalb Rundungsfehler; deshalb per GL/Metal-Parallelstelle gleichzeitig ändern, sonst driften die Backends); `Sample` → `SampleLevel(…, 0)` in `kGiTemporalHLSL`; `1.0 * fp` bereinigen (reines Rauschen).
- **Erwarteter Effekt** (Schätzung): Gering (halbaufgelöster Pass); nur bei GI-lastigen Szenen relevant. Ohne GPU-Timer keine Zusage. Unsicher / messen.
- **Risiko / Verifikation**: Visuelle Änderung möglich. Parallelstellen: GL `gi_atrous.frag`, `gi_temporal.frag`, Metal-Pendants. GI-Bildvergleich.

### REN-314 – BLAS-Traversal ohne Kind-Reihenfolge (Closest-Hit der Probes)
- **ID**: REN-314
- **Kategorie**: D
- **Schweregrad**: Niedrig (unsicher, messen)
- **Typ**: Performance
- **Ort**: `HlslSources.h:784-825` (`giBlasHit`), Aufrufer `843-859`, `1220`
- **Problem**: Beim Absteigen werden beide Kinder auf den Stack gelegt (`stack[sp++] = ...left; stack[sp++] = ...left+1`), und das rechte wird zuerst abgearbeitet, unabhängig davon, welches näher am Strahlursprung liegt. Für Any-Hit (Schatten) ist das egal. Für Closest-Hit (Probe-Update, `giSceneClosestHit`) wird dadurch `best` später verkleinert, und mehr Knoten werden besucht. Zusätzlich liegt ein `int stack[64]` pro Thread im Registerdruck-Bereich (GPU-Occupancy).
- **Code-Auszug**:
```hlsl
else if (sp + 2 <= 64)
{
    stack[sp++] = nodeOfs + leftFirst;
    stack[sp++] = nodeOfs + leftFirst + 1;
}
```
- **Lösung**: Beide Kind-AABBs testen und das nähere zuletzt pushen (zuerst abgearbeitet) bzw. nur bei `anyHit == false`. Teurer pro Knoten, aber weniger Knoten. Nur mit Messdaten durchführen.
- **Erwarteter Effekt**: Unbekannt; hängt von BVH-Qualität und Szene ab. Unsicher / messen.
- **Risiko / Verifikation**: Ergebnis des Closest-Hit bleibt gleich (nur Besuchsreihenfolge). Parallelstellen GL/Vulkan/Metal gleichzeitig ändern, damit Rauschmuster/Zeitverhalten nicht driften.

### REN-315 – SSAO leitet die Texelgröße aus dem Noise-Scale ab (versteckte Kopplung)
- **ID**: REN-315
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `HlslSources.h:222`, Konstanten `198-204`
- **Problem**: Die Nachbar-Offsets für die Normalenrekonstruktion nutzen `rcp(uSSAONoiseScale.xy * 4.0f)`. Das funktioniert nur, solange `uSSAONoiseScale = Auflösung / 4` gilt (4×4-Noise-Kachel). Wird die Noise-Kachelgröße oder die Berechnung des Scale geändert, ändert sich still der Rekonstruktionsradius. Zudem benutzen HBAO (236-279) und GTAO (280-334) fast identischen Slice-Setup-Code (`V`, `jitter`, `depthScale`, `omegaUV`, `projN`, `axis`).
- **Code-Auszug**:
```hlsl
float2 texel = rcp(float2(uSSAONoiseScale.xy * 4.0f));
```
- **Lösung**: Texelgröße explizit im Cbuffer übergeben (ein Platz in `uSSAOParams` oder `uSSAONoiseScale.zw` ist zu klären; die CPU-Seite in D3D11/D3D12, GL und Metal-Spiegelung ändern). Die gemeinsamen Teile von HBAO/GTAO in eine Hilfsfunktion `ssaoSliceSetup()`.
- **Erwarteter Effekt**: Lesbarkeit; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Erfordert Cbuffer-Layout-Änderung auf der CPU-Seite in zwei Backends; bildgleich testbar (AO-Screenshots für die drei Methoden).

### REN-316 – `MetalRenderer.h` zieht 14 schwere Header für jeden Includer
- **ID**: REN-316
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MetalRenderer.h:2-24`
- **Problem**: Der Header bindet unter anderem `RenderExtractor.h`, `OcclusionCuller.h`, `RenderGraph.h`, `CommandBuffer.h`, `GiBvh.h`, `GIProbeGrid.h`, `MaterialShaderLibrary.h` und `<mutex>`/`<atomic>` ein, nur weil Member-Typen by value im Header stehen. `RendererFactory.cpp` braucht nur den Klassennamen und den Konstruktor; die Editor-TUs nur die Handle-Zugriffe. `<limits>` wird nur für ein `quiet_NaN` (1086), `<mutex>`/`<atomic>` nur für `MetalGpuTimerShared` (32-40) gebraucht.
- **Code-Auszug**:
```cpp
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/OcclusionCuller.h>
#include <HorizonRendering/GiBvh.h>
#include <material/MaterialShaderLibrary.h>
```
- **Lösung**: Folgt aus REN-301 (Pimpl). Zwischenlösung: Vorwärtsdeklarationen und `std::unique_ptr<…>` für schwere Member sowie `MetalGpuTimerShared` ins `.mm` verschieben.
- **Erwarteter Effekt** (Schätzung): Kürzere Compile-Zeit der drei Includer; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Niedrig; Link-/Compile-Check auf macOS.

### REN-317 – `D3D12Renderer` besitzt `m_impl` per rohem `new/delete` ohne Kopierschutz
- **ID**: REN-317
- **Kategorie**: C / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `D3D12Renderer.h:119` (`D3D12RendererImpl* m_impl = nullptr;`), `D3D12Renderer.cpp:9807` (`~D3D12Renderer() { delete m_impl; }`); Parallelstelle `D3D11Renderer.h:95`
- **Problem**: Ein roher besitzender Zeiger mit implizit erzeugtem Kopierkonstruktor/-zuweisung: Eine versehentliche Kopie führte zu Doppel-Delete. Ob `IRenderer` die Kopie schon verbietet, habe ich nicht geprüft (unsicher).
- **Code-Auszug**:
```cpp
D3D12RendererImpl* m_impl = nullptr;
```
- **Lösung**: `std::unique_ptr<D3D12RendererImpl> m_impl;` (Destruktor ist bereits out-of-line deklariert, `D3D12Renderer.h:21`, also ist ein unvollständiger Typ unproblematisch). Gleiches für D3D11.
- **Erwarteter Effekt**: Sicherheit, 1 Zeile weniger.
- **Risiko / Verifikation**: Praktisch keines; Windows-Build.

## Funde (Slice 5 – Metal-Shader-Manager)

### REN-351 – `MetalShaderManager` (und die vier Schwestern) haben keine Produktionsaufrufer für `load`/`compile`/`createProgram`
- **ID**: REN-351
- **Kategorie**: F / A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/include/Backends/Metal/MetalShaderManager.h:1-35`, `src/Backends/Metal/MetalShaderManager.mm:1-138`; Nutzung `MetalRenderer.mm:6659` (`setDevice`), `6697` (`cleanup`); Schwestern `D3D12ShaderManager.{h,cpp}`, `D3D11ShaderManager`, `OpenGLShaderManager`, `VulkanShaderManager`
- **Problem**: In `MetalRenderer.mm` wird nur `setDevice` und `cleanup` aufgerufen. Alle echten Pipelines baut der Renderer selbst (`newLibraryWithSource` im Renderer, `MaterialShaderLibrary`). Das Grep über `src/` nach den Managern liefert außer den Renderern, den Manager-Dateien und `tests/test_d3d_shader_manager.cpp` nur Kommentare. Die D3D12-Header-Notiz bestätigt es: „D3D12Renderer does not route its shaders through this class" (`D3D12ShaderManager.h:283`). OpenGL/Vulkan weisen den Manager nur zu (`OpenGLRenderer.cpp:3372`, `VulkanRenderer.cpp:216`). Die Klassen sind damit kompilierter, getesteter, aber ungenutzter Code mit fünf Implementierungen und einem Interface (`ShaderManager.h`).
- **Code-Auszug**:
```cpp
// MetalRenderer.mm:6659 / 6697
m_shaderManager.setDevice(m_device);
m_shaderManager.cleanup();
```
- **Lösung**: Entscheiden: (a) entfernen samt Tests und Interface, oder (b) tatsächlich verwenden (z. B. für die bei `MetalRenderer` handgepflegten Spiegel-Shader, damit Fehler einheitlich geloggt werden). Mindestens im Header als „nur Tests / Legacy" kennzeichnen. Vor dem Entfernen öffentliche Verwendung durch externe Module (HE_Python, Plugins) prüfen: im `src/`-Baum habe ich keine gefunden, externe Verbraucher sind unsicher.
- **Erwarteter Effekt** (Schätzung): Etwa 500 bis 600 Zeilen weniger über alle Backends, kleinere Testfläche.
- **Risiko / Verifikation**: Niedrig, wenn Grep über das gesamte Repo (inkl. `docs/`, Tools) bestätigt, dass nichts den Manager nutzt. Build aller Backends.

### REN-352 – Dateilesecode fünffach kopiert und doppelt ineffizient
- **ID**: REN-352
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `MetalShaderManager.mm:371-389`; wortgleich `D3D12ShaderManager.cpp:147-166`, `D3D11ShaderManager.cpp:~35`, `OpenGLShaderManager.cpp:~24`, `VulkanShaderManager.cpp:~25`
- **Problem**: `fs::exists` + `std::ifstream` + `is_open` + `stringstream << rdbuf()` + `buffer.str()` ist in jedem Manager derselbe Block mit nur anderem Präfix im Fehlertext. Dabei ist die `exists`-Vorprüfung redundant zu `is_open` (zweiter Syscall plus Race), und der Weg über `stringstream` kopiert den Inhalt zweimal (Stream-Puffer, dann `str()`).
- **Code-Auszug**:
```cpp
if (!fs::exists(path)) { HE_LOG_ERROR(RHI, "%s", (std::string("MetalShaderManager: shader file not found: ") + path).c_str()); return { 0, false }; }
std::ifstream file(path);
...
std::stringstream buffer; buffer << file.rdbuf();
m_pendingSources[handle.id] = buffer.str();
```
- **Lösung**: Ein geschützter Helfer in `ShaderManager` (`static bool readSourceFile(const char* path, std::string& out, const char* who)`), der `ifstream` binär öffnet, mit `seekg`/`tellg` vorab dimensioniert und einmal liest. Fehlertext: „nicht gefunden" bei `errno==ENOENT` beibehalten.
- **Erwarteter Effekt**: Rund 60 Zeilen weniger, eine Kopie weniger. Laufzeit irrelevant (Startpfad).
- **Risiko / Verifikation**: Niedrig; Tests `test_d3d_shader_manager.cpp` prüfen die Fehlertexte ggf. wörtlich. Dann Texte unverändert lassen. Hinweis: `std::ifstream` im Textmodus konvertiert unter Windows CRLF, im Binärmodus nicht; für HLSL/MSL-Quelltext unkritisch, aber Verhalten erwähnt.

### REN-353 – `releaseProgram` hat auf Metal andere Besitzsemantik als in den anderen Managern
- **ID**: REN-353
- **Kategorie**: A (Vertrag)
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MetalShaderManager.mm:455-479` (`createProgram`/`releaseProgram`) gegen `D3D12ShaderManager.cpp:209-236`, `VulkanShaderManager.cpp:105-110`; Vertrag `ShaderManager.h` („Every resource tied to that program should be released with it.")
- **Problem**: Metal gibt in `releaseProgram` die Stage-Funktionen aller enthaltenen Shader frei (`for (shader : handle.shaders) release(shader)`). D3D12 und Vulkan lassen die Shader unberührt, D3D12 entfernt nur den Programmeintrag. Wird ein Shader in zwei Programmen verwendet oder nach `releaseProgram` weiterbenutzt, funktioniert es in D3D12 und scheitert in Metal („compile called on unknown handle" / „uncompiled shader"). Metal führt keine Programmliste (`createProgram` merkt sich nichts, `cleanup` kann daher keine Programme freigeben, was dort unkritisch ist, weil Programme keine GPU-Ressource sind). Ein doppeltes `releaseProgram` ist auf Metal harmlos, aber die Semantik ist implizit.
- **Code-Auszug**:
```cpp
void MetalShaderManager::releaseProgram(ShaderProgramHandle handle)
{
	for (const auto& shader : handle.shaders)
		release(shader);
}
```
- **Lösung**: Den Vertrag in `ShaderManager.h` explizit machen (gehören Shader dem Programm oder nicht) und alle fünf Manager angleichen. Wenn REN-351 (b) nicht umgesetzt wird, wird das Thema mit dem Entfernen hinfällig.
- **Erwarteter Effekt**: Konsistenz; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Verhaltensänderung für jeden Aufrufer. Tests pro Backend schreiben (Shader in zwei Programmen, `releaseProgram` dann weiterverwenden).

### REN-354 – D3D12-Manager behält Quelltext nach `compile()` und führt dreifache Buchführung
- **ID**: REN-354
- **Kategorie**: C / A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `D3D12ShaderManager.cpp:164-165`, `201-205`; `D3D12ShaderManager.h:306-311` (Kommentar „consumed by compile()"); Gegenbeispiel `MetalShaderManager.mm:449` (`m_pendingSources.erase(srcIt)`)
- **Problem**: Der Header sagt, `m_sources` werde von `load()` gefüllt und von `compile()` „consumed", aber `compile()` löscht den Eintrag nie. Damit bleibt der volle HLSL-Text bis `release`/`cleanup` im Speicher (bei großen Shadern dutzende KB pro Handle). Außerdem werden pro Shader drei Container parallel gepflegt (`m_sources`, `m_blobs`, `m_loadedShaders`), und `compile` sucht in `m_loadedShaders` linear nach dem Handle (`for (auto& h : m_loadedShaders) if (h.id == handle.id)`), `release` per `remove_if`.
- **Code-Auszug**:
```cpp
m_blobs[handle.id] = blob;
handle.ready = true;
for (auto& h : m_loadedShaders)
    if (h.id == handle.id) h.ready = true;
```
- **Lösung**: Entweder den Quelltext nach erfolgreichem Compile freigeben (dann ist Recompile desselben Handles nicht mehr möglich, wie bei Metal; der Header-Kommentar stimmt dann) oder den Kommentar korrigieren und bewusst behalten (Hot-Reload). Die drei Container durch `unordered_map<id, Entry{source, blob, ready}>` ersetzen; `loadedShaderCount()` wird `map.size()`.
- **Erwarteter Effekt** (Schätzung): Etwas Speicher; weniger Zeilen. Praktisch ohne Laufzeitwirkung.
- **Risiko / Verifikation**: `test_d3d_shader_manager.cpp` prüft Zähler und Refcounts (laut Header-Kommentar 280-281). Das Verhalten „compile ein zweites Mal" ist dort evtl. abgedeckt, vorher lesen.

## Gelesen

Ja, vollständig: `D3D12Renderer.h`, `D3D12ShaderManager.cpp`, `D3D12ShaderManager.h`, `HlslSources.h` (1-1352), `MetalRenderer.h` (1-1415) sowie Slice 5 `MetalShaderManager.h` und `MetalShaderManager.mm`. Zur Verifikation von Header-Annahmen wurden nur gezielte Ausschnitte von `MetalRenderer.mm`, `D3D12Renderer.cpp`, `ShaderManager.h` und den Schwester-Managern gelesen (nicht vollständig, sie gehören zu anderen Slices).

## Rendering Slice 4 – src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm (18158 Zeilen)

## Zusammenfassung

`MetalRenderer.mm` ist funktional ausgereift und an vielen Stellen bereits performance-bewusst (Upload-Ring, Instancing-Pfade, Einmal-Extract pro Frame, memoisierte Mesh-/Material-Auflösung im Draw-Loop, parallele Sky-Env-Bake). Die Datei besteht zu rund 6350 Zeilen aus eingebetteten MSL-Strings (Z. 238–6367), danach ~11800 Zeilen Objective-C++-Host-Code. Hauptprobleme: (1) eine Reihe von **Pro-Frame-Heap-/Metal-Objekt-Allokationen** außerhalb des vorhandenen Upload-Rings (Skinning-Bone-Puffer, GI-TLAS samt Hilfspuffer, Partikel-/Ribbon-/Preview-Puffer), (2) **blockierende CPU-GPU-Syncs** (`waitUntilCompleted` im BLAS-Build und in jeder Preview/Thumbnail), (3) **O(n)-/O(n log n)-Arbeit pro Shadow-Layer und pro UI-Quad**, die sich einmal pro Frame erledigen ließe, und (4) **massive Code-Duplikation** (G-Buffer- vs. Forward-Draw-Loop ~250 Zeilen, 6× Extract-Block, 3× SSR-Blur, 7× identische MSL-Hilfsfunktionen, 168 händische `CFBridgingRelease`-Blöcke) und eine 1000-Zeilen-Funktion `EncodeFrame`.
Funde: 8 Hoch, 17 Mittel, 4 Niedrig (29 gesamt). Alle Performance-Schätzungen sind ohne Messdaten und als Schätzung gekennzeichnet; Verifikation jeweils über `HE_PROFILE_SCOPE_N`/Metal-System-Trace und die vorhandenen Headless-Captures (`HE_SKY_TIME`, he_shot).

---

## Funde (nach Schweregrad)

### REN-401 – Sky-Env-Cube: 98k CPU-Atmosphärenintegrale bei jeder Sonnenbewegung
- **ID**: REN-401
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:13811–13840 (`UpdateSkyEnvCube`), Aufrufer 14107 (`EncodeScene`) und 15015 (`EncodeDeferredResolveTile`)
- **Problem**: Der Cube wird neu gebacken, sobald sich `sunDir` um mehr als `1e-4` verändert. Bei aktivem Tag-Nacht-Zyklus (Auto-Advance) ist das **jeder Frame**. Pro Bake werden 6×128×128 = 98 304 Texel mit `HE::SkyColorCPU` ausgewertet (je 12×5-Schritt-Integral, ~216 `exp`), dazu ein 1,5-MB-`std::vector<float>` frisch allokiert und per `replaceRegion` (synchron, Shared-Storage, CPU→GPU) hochgeladen. Der eigene Kommentar nennt ~60 ms seriell; `parallel_for` verteilt das nur auf die Jobs, blockiert aber den Render-Thread bis zum Ende. Der Cube dient nur als diffuse/grob-spekulare IBL (Sample mit `Nup`/`Rrough`), 128² ist dafür deutlich überdimensioniert.
- **Code-Auszug**:
```cpp
if (m_skyEnvValid && glm::distance(sunDir, m_skyEnvSunDir) < 1e-4f) return;
...
std::vector<float> px(static_cast<size_t>(N) * N * 6 * 4);
parallel_for(static_cast<size_t>(6) * N, [&](size_t idx){ ... HE::BuildSkyEnvFaceRow(...) }, "SkyEnvBake", 1);
```
- **Lösung**: (a) Toleranz auf ~2e-3 (≈0,1°) anheben – für Ambient unsichtbar; (b) Auflösung auf 32² senken (16× weniger Arbeit) oder (c) den Bake auf die GPU legen: `EncodeSkyReflCube` (13771) zeigt bereits, wie der Sky-Pass in Cube-Faces rendert; mit `useSkyLut=true` ist das billig und braucht keinen CPU-Upload. (d) Mindestens: `px` als Member-Scratch wiederverwenden und eine Face pro Frame zeitlich verteilen.
- **Erwarteter Effekt**: bei laufendem Tag-Nacht-Zyklus ca. 1–10 ms CPU/Frame weniger (Schätzung, abhängig von Kernzahl); bei 32²/Toleranz-Anhebung >90 % weniger Arbeit.
- **Risiko / Verifikation**: Ambient-Farbübergänge dürfen nicht stufig werden (Toleranz nicht zu groß; Bake-Intervall mit Interpolation zwischen zwei Cubes lösbar). Vergleich per `HE_SKY_TIME`-Headless-Captures vor/nach; GL-Backend hat dieselbe Stelle (`OpenGLRenderer.cpp:4226`) – Ergebnis soll backend-gleich bleiben (`SkyEnvBake.h`).

### REN-402 – Skinning: pro Objekt und Frame ein neuer 8-KB-`MTLBuffer` samt Identity-Fill
- **ID**: REN-402
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:13930–13949 (`EncodeSkinnedObjects`); gleiches Muster 10840–10846 (`RenderWorldPreview`)
- **Problem**: Für jedes `SkinnedRenderObject` wird `boneScratch` (128 × `mat4` = 8 KB) komplett mit Identity überschrieben, die echten Bones darüber kopiert und danach `newBufferWithBytes` (Kernel-Allokation + Memcpy + ARC-Objekt) aufgerufen. Bei 100 Charakteren sind das 100 Metal-Buffer pro Frame (~800 KB Memcpy, ~800 KB Fill), obwohl ein Upload-Ring (`UploadTransientBytes`) für exakt diesen Zweck existiert und an 15+ anderen Stellen genutzt wird.
- **Code-Auszug**:
```cpp
std::fill(boneScratch.begin(), boneScratch.end(), glm::mat4(1.0f));
if (boneCount > 0) std::copy_n(obj.boneMatrices.begin(), boneCount, boneScratch.begin());
id<MTLBuffer> boneBuf = [device newBufferWithBytes:boneScratch.data()
                                            length:kMaxBones * sizeof(glm::mat4) ...];
```
- **Lösung**: `UploadTransient(max(boneCount,1) * sizeof(mat4))`, Bones direkt in den Ring-Slice schreiben (nur `boneCount` Matrizen, kein Identity-Fill nötig, solange Vertex-Bone-IDs < boneCount; sonst eine einzige Identity am Ende), `setVertexBuffer:offset:`. Pro Objekt ein Slice, pro Section dieselbe Bindung.
- **Erwarteter Effekt**: pro Charakter ~1 Metal-Allokation und ~16 KB Speicherverkehr weniger; bei 50–200 Skinned-Objekten spürbar (Schätzung 0,2–1 ms CPU/Frame).
- **Risiko / Verifikation**: Shader `skinnedVertex` indexiert `boneMats[ids.x]` ohne Bounds-Check: Mesh-Assets mit Bone-IDs ≥ `boneCount` würden aus dem Slice lesen (vorher Identity). Entweder `max(boneCount, maxBoneIdOfMesh+1)` allokieren/auffüllen oder Assets beim Laden validieren. Test: Skinned-Szene vor/nach per Headless-Screenshot vergleichen.

### REN-403 – UI-Material-Quads: pro Quad und Frame GLSL-Kopie, Hash und Volltextsuche
- **ID**: REN-403
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:13272–13283 (`GetOrBuildUIMaterialPipeline`), Aufruf 13508–13509 (`EncodeUIPass`); Quelle `MaterialShaderLibrary.cpp:1000–1008` (`resolveFragment`)
- **Problem**: Der Cache-Lookup (`m_uiMaterialPipelines.find(key)`) steht *nach* `ResolveMaterialShader`. Dieses kopiert den kompletten `customShaderFragGlsl` (typisch Zehntausende Bytes) in einen `std::string`, hasht ihn komplett (`std::hash<std::string>`) und anschließend läuft `fragGlsl.find("heBackdrop")` über denselben String – für **jeden UI-Quad mit Material in jedem Frame**. Ein HUD mit 50 Material-Quads erzeugt damit 50 Heap-Kopien + 50 volle Hash-/Suchläufe pro Frame. Im Scene-Loop ist das durch `lastMatId`-Memoisierung gedämpft; im UI-Pfad nicht.
- **Code-Auszug**:
```cpp
uint64_t key = 0; std::string fragGlsl, vertBody;
if (!ResolveMaterialShader(materialId, key, fragGlsl, vertBody)) return nullptr;
if (usesBackdrop) *usesBackdrop = fragGlsl.find("heBackdrop") != std::string::npos;
if (auto it = m_uiMaterialPipelines.find(key); it != ...) return it->second;
```
- **Lösung**: Cache nach `materialId` (UUID) statt nach Source-Hash: `unordered_map<UUID, {void* pso; bool backdrop;}>`; Invalidierung über das vorhandene `InvalidateMaterial`. Die Source nur auf Cache-Miss auflösen. Zusätzlich pro UI-Pass die letzte Material-ID memoisieren (Quads kommen in Baumreihenfolge).
- **Erwarteter Effekt**: UI-CPU-Kosten für Material-Quads gehen von O(Quads × GLSL-Länge) auf O(Quads) (Schätzung: 10–100 µs pro Quad gespart).
- **Risiko / Verifikation**: Material-Hot-Reload muss den Cache invalidieren (`InvalidateMaterial` erweitern; heute löscht es nur `m_materialTexCache`, nicht die Pipeline-Caches – bestehendes Verhalten beibehalten, aber testen: Material im Editor ändern → UI-Quad zeigt neue Version). Test: Widget-Test mit Material-Quad vor/nach.

### REN-404 – GI-Accel-Build: pro Frame lineare BLAS-Suche, Material-Auflösung und Transform-Inverse für jedes Objekt
- **ID**: REN-404
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:8427–8465 (HW), 8236–8254 (SW), `giInstanceShading` 8186–8195
- **Problem**: Bei aktivem GI/GI-Reflexionen läuft pro Frame über **alle** Caster: (a) lineare Suche `for (u < uniqueBlas.size()) if (uniqueBlas[u] == blas)` → O(Objekte × einzigartige Meshes); bei 20k Objekten und 500 Meshes ≈ 10 M Vergleiche/Frame; (b) `giInstanceShading` → `HE::giInstanceSurface(obj, cm)` löst pro Objekt Material/Textur über den ContentManager auf (Hash-Lookups), obwohl sich Material und Farbe nur selten ändern; (c) im SW-Pfad zusätzlich `glm::inverse(obj.transform)` (4×4-Inverse) pro Objekt. Alles läuft auch dann, wenn sich in der Szene nichts bewegt hat.
- **Code-Auszug**:
```cpp
for (size_t u = 0; u < uniqueBlas.size(); ++u)
    if (uniqueBlas[u] == blas) { idx = (int)u; break; }
...
giInstanceShading(obj, m_contentManager, albedo, emissive, metallic, roughness);
```
- **Lösung**: `std::unordered_map<void*, uint32_t>` (BLAS-Zeiger → Index, pro Frame `clear()` mit behaltener Kapazität) statt linearer Suche. Instanz-Shading in einen Cache pro (`entityId`, Material-Revision) auslagern bzw. das Ergebnis in `RenderObject` vom Extractor mitliefern lassen (dort gibt es ohnehin Dirty-Information). SW: `glm::inverse` nur bei geänderter Transform (Hash/Vergleich gegen Vorjahres-Matrix) oder affine Schnell-Inverse.
- **Erwarteter Effekt**: O(n·u) → O(n); bei 10k+ Objekten mehrere ms CPU/Frame (Schätzung).
- **Risiko / Verifikation**: Reihenfolge der `uniqueBlas` muss stabil bleiben (TLAS-`instancedAccelerationStructures`-Array). Test: `tests/test_gi_bvh.cpp` deckt nur die BVH ab – GI-Szene vor/nach per Capture vergleichen, Instanz-IDs (`get_committed_instance_id`) müssen identisch bleiben.

### REN-405 – GI-TLAS wird jedes Frame komplett neu allokiert und gebaut (kein Dirty-Check)
- **ID**: REN-405
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:8335–8576 (`EncodeGIAccelBuild`), 8266–8282 (SW), 8295–8333 (`BuildGILandscapeTable`)
- **Problem**: In jedem GI-aktiven Frame werden TLAS (`newAccelerationStructureWithSize`), Instanz-Puffer, Farb-Puffer, Landscape-Index-Puffer, Mesh-Ptr-Puffer, Mesh-Index-Puffer und ein Private-Scratch-Puffer (`newBufferWithLength`) neu erzeugt, die alten via `RetireGIObject` 3 Frames später freigegeben; die Instanz-Descriptoren werden pro Instanz mit 12 skalaren Schreibzugriffen befüllt. Für statische Szenen ist das reine Verschwendung: der TLAS-Inhalt ist bit-identisch. Zusätzlich verwaltet `m_retiredGIObjects` 6+ Einträge/Frame.
- **Code-Auszug**:
```cpp
id<MTLAccelerationStructure> tlas = [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
id<MTLBuffer> scratch = [device newBufferWithLength:... options:MTLResourceStorageModePrivate];
...
RetireGIObject(m_giTlas); m_giTlas = nullptr; RetireGIObject(m_giInstanceBuffer); ...
```
- **Lösung**: (1) Szenen-Signatur (Anzahl Caster, Summe der Transform-/Material-Revisionen, `GIProbeSceneSignature` existiert bereits in `EnsureGIProbeGrid`) bilden; bei unverändertem Wert TLAS/Puffer des Vorjahres weiterverwenden. (2) Bei Änderung: persistenten Scratch (wachsend) und ein 3-fach-Ring der Instanz-/Farb-Puffer verwenden; `refitAccelerationStructure:` statt Neubau, wenn nur Transforms wechseln (Instanzanzahl/BLAS-Set gleich). (3) Instanz-Transform per Memcpy statt 12 Einzelzuweisungen.
- **Erwarteter Effekt**: bei statischen GI-Szenen ~6 Metal-Allokationen + TLAS-Build pro Frame eingespart (GPU- und CPU-Zeit; Schätzung 0,3–2 ms bei mittleren Szenen, mehr bei großen).
- **Risiko / Verifikation**: Hoch – das In-Flight-GPU-Race ist der Grund für die Frisch-Allokation (Kommentar am Member). Ring über 3 Frames oder Completion-Handler-Fence nötig. Verifikation mit Metal API Validation + `HE_GI_FORCE_SW=1`/HW-Capture. Test: GI-Capture einer bewegten und einer statischen Szene.

### REN-406 – `BuildBLAS`: blockierendes `waitUntilCompleted` pro neuem Mesh auf dem Render-Thread
- **ID**: REN-406
- **Kategorie**: E
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:8122–8169 (`BuildBLAS`), Aufruf 8439 in `EncodeGIAccelBuild`
- **Problem**: Jedes erstmals gesehene Mesh bekommt einen eigenen Command-Buffer, wird committed und der Render-Thread wartet synchron auf die GPU. Beim Laden einer Szene mit N Meshes entstehen N serielle CPU-GPU-Roundtrips innerhalb eines Frames (Hitch von N × (Submit-Latenz + Build-Dauer)). Zusätzlich neuer Scratch-Puffer pro BLAS.
- **Code-Auszug**:
```cpp
[cmdBuf commit];
// One-shot, lazy ... waiting here keeps the BLAS fully built before any TLAS references it
[cmdBuf waitUntilCompleted];
```
- **Lösung**: BLAS-Builds in den Frame-Command-Buffer (`cmdBufPtr` ist in `EncodeGIAccelBuild` vorhanden) auf einen gemeinsamen `accelerationStructureCommandEncoder` legen und einen gemeinsamen, wachsenden Scratch-Puffer verwenden (Builds seriell mit Offset). Metal ordnet Encoder desselben Command-Buffers; der TLAS-Build (gleicher Buffer, späterer Encoder) sieht die fertigen BLAS. Kein Wait nötig.
- **Erwarteter Effekt**: Szenenladen/Mesh-Streaming ohne N Pipeline-Stalls; Hitch skaliert nicht mehr mit Mesh-Anzahl (Schätzung: von mehreren 10–100 ms auf <5 ms CPU).
- **Risiko / Verifikation**: Die BLAS-Objekte werden während des Frames erzeugt und in `mesh.blas` gespeichert, bevor die GPU sie gebaut hat – andere Passes im selben Frame, die `mesh.blas` lesen (nur GI), kommen später im selben Command-Buffer, also korrekt. Mit Metal Validation prüfen; Test: Szene mit vielen neuen Meshes laden, Frame-Zeit im Profiler (`Metal::EncodeGIAccelBuild`).

### REN-407 – Shadow-Pass: Cull **und** Sort pro Layer (bis zu 3 Kaskaden + 16 Lokal-Layer)
- **ID**: REN-407
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:7960–7990 (`encodeDepthLayer` in `EncodeShadowMap`); Sorter `RenderSorter.cpp:12–46`
- **Problem**: Für jeden Layer läuft `m_culler.cull(viewProj)` **und** `m_sorter.sort(...)`. Der Sort ordnet nach (Mesh-ID, **Kamera**-Abstand) – der Schlüssel ist für alle Layer identisch (Kamera ändert sich nicht), nur die Sichtbarkeitsmenge variiert. Bei 3 Kaskaden + z. B. 8 Lokal-Layern sind das 11 × O(n log n) auf allen sichtbaren Objekten (SortKey-Aufbau, `std::sort`, Index-Kopie). Der Kommentar in `RefineObjectBounds` (7898) beziffert eine einzelne O(n)-Schleife mit ~3,5 ms bei 50k Entities; Cull+Sort ist teurer.
- **Code-Auszug**:
```cpp
auto encodeDepthLayer = [&](...) {
    m_culler.cull(m_renderWorld, viewProj, m_visible);
    m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
```
- **Lösung**: Einmal pro Frame alle Objekte nach (Mesh, Distanz) sortieren (nur für `castsShadow`); pro Layer nur noch das Sichtbarkeits-Bitset per Cull berechnen und die vorsortierte Liste linear filtern (O(n) statt O(n log n)). Das Ergebnis ist identisch geordnet. Alternativ Cull-Jobs der Kaskaden parallelisieren (JobSystem). Das Vulkan-Backend hat dieselbe Struktur (`VulkanRenderer.cpp:7007–7009`) – Lösung kann im Shared-Code (`RenderSorter`) liegen.
- **Erwarteter Effekt**: Sort-Anteil der Shadow-Pass-CPU-Zeit nahezu eliminiert; bei vielen Local-Lights/50k Objekten mehrere ms/Frame (Schätzung).
- **Risiko / Verifikation**: Verhalten identisch, solange die Filterung die Sortierreihenfolge erhält. `batchDepthCasters` setzt Mesh-Gruppierung der Eingabe voraus – bleibt erfüllt. Test: Shadow-Capture bitgleich; Unit-Test für `RenderSorter` mit Teilmengen.

### REN-408 – `EncodeGBuffer` ist eine ~250-Zeilen-Kopie der Forward-Opaque-Schleife in `EncodeScene`
- **ID**: REN-408
- **Kategorie**: A
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung
- **Ort**: MetalRenderer.mm:14375–14683 (Forward) vs. 16179–16457 (G-Buffer); dazu die Vor-Blöcke 14229–14240 vs. 16124–16157
- **Problem**: Die Memoisierungs-Variablen (`lastMeshId`, `cMaterialPipeline`, `cGraphTex`…), die Material-/Shader-/Textur-Auflösung, die dense-Vector-Pointer-Reparatur nach `ResolveMesh`, `drawInstance`-Lambda, Transparent-Routing, Param-Padding (`float padded[64]`), WPO-Binds, Weightmap-Bind und der Instancing-`fits`-Block existieren zweifach mit nur kleinen Abweichungen (G-Buffer-Pipeline, `forwardOpaque`-Routing). Änderungen (z. B. das Weightmap-Binding oder der `dense vector`-Fix) müssen doppelt gepflegt werden – der Kommentar „the G-buffer twin of the forward pass's re-take" belegt die Drift-Gefahr bereits.
- **Code-Auszug**:
```cpp
HE::UUID  lastMeshId{}; const GpuMesh* cMesh = nullptr; bool meshValid = false;
HE::UUID  lastMatId{};  bool matValid = false;
void*     cOverrideTex = nullptr; bool cHasOverride = false;
...   // identisch in EncodeScene (14384ff) und EncodeGBuffer (16184ff)
```
- **Lösung**: Einen `struct DrawLoopState` (Memo-Zustand) plus `ResolveDrawMaterial(state, dc)` und `ResolveDrawMesh(state, dc)` extrahieren; die Pass-spezifischen Teile (Ziel-Pipeline: `defaultPipeline` vs. `gbufferPipeline`, Routing nach `forwardOpaque`/`transparent`) als kleine Policy-Funktion/Lambda übergeben. Der `fits`-Instancing-Block nimmt zwei Pipelines als Parameter. Reihenfolge: erst die Auflösungs-Helfer (reiner Refactor), dann die Draw-Emission.
- **Erwarteter Effekt**: ca. 200–250 Zeilen weniger, ein Ort für künftige Material-Features; keine Laufzeitänderung.
- **Risiko / Verifikation**: Verhalten muss bit-identisch bleiben (Headless-Captures Forward+Deferred+Tile, `HE_RENDER_PATH=1`, `HE_MTL_INSTANCING=0/1`). Keine Tests im Repo für die Metal-Draw-Loops → Capture-Vergleich ist Pflicht.

### REN-409 – `EncodeFrame` (≈1000 Zeilen) mit verschränktem Detailed-Capture-Pfad
- **ID**: REN-409
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: MetalRenderer.mm:16461–17455
- **Problem**: Eine Funktion enthält Frame-Setup, Invalidierungen, GPU-Timer-Setup (zwei Modi + Fallback), Shadow/GI/SSAO/G-Buffer/HDR/Post/Tonemap/AA/UI/Swapchain und drei Varianten der Completion-Handler. `flushPass(...)`-Aufrufe (7×) und `curPass`-Zustand verzahnen den Debug-Modus „detailed" mit dem normalen Pfad; die verschachtelten Lambdas `attachDetail`/`flushPass` und mehrere `static`-Zustände (`s_lastSceneW`, `s_lastMode`, `s_loggedDetailed`) machen Kontrollfluss und Lebensdauer schwer prüfbar.
- **Code-Auszug**:
```cpp
auto flushPass = [&](const char* nextPass) { if (!detailed) return; attachDetail(cmdBuf, curPass); [cmdBuf commit]; [cmdBuf waitUntilCompleted]; ...};
flushPass("GIAccel"); ... flushPass("GIShadow"); ... flushPass("Present");
```
- **Lösung**: Aufteilen in `BeginFrame()` (Invalidierungen, Retire-Aging), `SetupGpuTiming()` (liefert ein `FrameTiming`-Objekt mit `flush(next)` und `finish(cmdBuf)`), `EncodeShadowStage()`, `EncodeGIStage()`, `EncodeSceneStage()` (SSAO/G-Buffer/HDR), `EncodePostStage()` (DoF/UW/MB/Bloom/MFX/Tonemap/TAA/AA), `EncodePresent()`. Das `FrameTiming`-Objekt kapselt Detailed-vs-Counter-vs-Whole-Frame in einer Klasse statt drei `else if`-Ästen am Funktionsende.
- **Erwarteter Effekt**: bessere Lesbarkeit/Testbarkeit; keine Laufzeitänderung.
- **Risiko / Verifikation**: Mittel – Reihenfolge der Encoder muss exakt bleiben (Kommentare zu Ordnungsabhängigkeiten: SSAO vor Tile-Pass, Velocity nach G-Buffer). Capture-Vergleich Forward/Deferred/Tile + Profiler-Pass-Namen unverändert.

### REN-410 – `CreateScenePipeline`: 625-Zeilen-Monolith, ~35× identischer Fehlerblock, alle Libraries eager beim Start
- **ID**: REN-410
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: MetalRenderer.mm:7199–7824 (`CreateScenePipeline`), `Initialize` 6609–6692
- **Problem**: (1) Jede der ~16 Libraries/~30 Pipelines wiederholt Schema „`newLibraryWithSource` → `if (!lib) throw runtime_error(std::string(...) + (err ? [[err localizedDescription] UTF8String] : "unknown"))` → Descriptor-Aufbau → PSO → `CFBridgingRetain`". (2) Alle Libraries – inkl. des enormen `kSkyMSL` (Wolken, Nebel, Aurora, ≈2100 Zeilen) sowie Tonemap, FXAA/SMAA/TAA, DoF, Motion Blur, Bloom, SSAO, UI – werden **synchron beim Start** auf dem Main-Thread kompiliert, auch wenn das Feature (z. B. DoF, Motion Blur, TAA, SMAA) nie genutzt wird. Die GI-/SSR-/Underwater-Pfade machen es bereits richtig (lazy `Ensure*`).
- **Code-Auszug**:
```cpp
throw std::runtime_error(std::string("MetalRenderer: bloom bright pipeline creation failed: ")
    + (blError ? [[blError localizedDescription] UTF8String] : "unknown"));
```
- **Lösung**: Hilfen `MakeLibrary(src, label)` und `MakePso(desc, label)` (werfen bzw. loggen zentral) sowie ein kleines `PipelineDesc`-Builder-Lambda für Fullscreen-Passes (`vertex`, `fragment`, `colorFormat`, optional `depthFormat`, Blend-Preset). Rarely-used-Gruppen (DoF, MotionBlur, TAA/SMAA, Underwater, UI-Materialien, Cloud-Shadow, Sky-LUT) lazy beim ersten Gebrauch bauen (`Ensure*Pipelines()` Muster aus Z. 15187). Startzeit und Initialisierungs-Peak-Speicher sinken.
- **Erwarteter Effekt**: ~300–400 Zeilen weniger; Startzeit des Renderers sinkt um die Compile-Zeit ungenutzter Libraries (Schätzung 50–300 ms, **messen**).
- **Risiko / Verifikation**: Lazy-Build verlagert Compile-Hitches in den ersten Frame mit Feature → bei Post-FX-Toggle im Editor kurzer Hitch (Warmup-Option). Tests: Initialisierung + jedes Feature einmal einschalten (Headless).

### REN-411 – UI-Quads: ~12 `setBytes`-Aufrufe pro Quad, String-Schlüssel pro Textur-Quad
- **ID**: REN-411
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:13583–13654 (`EncodeUIPass`), 9533–9543 (`ResolveUITexture`)
- **Problem**: Jeder Solid-/Glyph-Quad setzt 3 Vertex- und 9 Fragment-`setBytes` (jeweils eigene Command-Stream-Einträge) und führt einen eigenen Draw aus; Text mit 500 Glyphen = 6000 Encoder-Aufrufe + 500 Draws pro Frame. Textur-Quads bilden in `ResolveUITexture` pro Aufruf `std::to_string(hi) + ":" + std::to_string(lo)` (zwei Heap-Allokationen) für den Cache-Key.
- **Code-Auszug**:
```cpp
[enc setVertexBytes:&rect length:sizeof(rect) atIndex:0];  ... [enc setFragmentBytes:&innerCol length:sizeof(innerCol) atIndex:8];
...
const std::string key = texId != HE::UUID{} ? (std::to_string(texId.hi) + ":" + std::to_string(texId.lo)) : path;
```
- **Lösung**: Die 9 Fragment-Konstanten in **einen** `struct UIQuadFrag { float4 ... }` packen (ein `setFragmentBytes`, Shader entsprechend anpassen) bzw. alle Quads einer Clip-/Atlas-/Material-Gruppe als Array in einen Ring-Slice schreiben und mit `instanceCount` zeichnen (`instance_id` indiziert die Struct-Array). Cache-Key als `UUID` (Hash über `hi/lo`) – String nur für Pfad-only-Texturen.
- **Erwarteter Effekt**: Encoder-Overhead pro Quad ~10× kleiner; für UI-lastige Games messbarer CPU-Gewinn (Schätzung 0,2–1 ms bei 500 Quads).
- **Risiko / Verifikation**: Shader-Signatur (`kUIMSL`) und Host-Struct-Layout synchron halten (`static_assert`). UI-Widget-Tests (`tests/test_ui_widgets.cpp`, nur gezielt lesen) laufen headless gegen den Software-Renderer – Metal-Pfad per Capture prüfen.

### REN-412 – Velocity-Pass: Hash-Map-Knoten pro Objekt und Frame, Foliage-Cluster pro Instanz gezeichnet
- **ID**: REN-412
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:12945–13030 (`EncodeVelocity`), Member `m_taaPrevTransforms/m_taaCurTransforms` (Header 898–899)
- **Problem**: Bei TAA/MetalFX wird pro Objekt `m_taaPrevTransforms.find` + `m_taaCurTransforms[id] = transform` (Knotenallokation in `unordered_map<uint32_t, mat4>`) ausgeführt und die Map am Ende getauscht; `m_taaCurTransforms.clear()` gibt alle Knoten (je ~80 B) wieder frei → bei 20k Objekten ~20k malloc/free pro Frame. Dazu kommt ein Einzel-Draw (`setVertexBytes` 192 B + Draw) pro Objekt **ohne** die Instancing-Batches der anderen Passes, und Foliage-Cluster zeichnen `forEachInstance` **eine Instanz pro Draw** (mehrere tausend Draws pro Cluster möglich).
- **Code-Auszug**:
```cpp
const auto it = m_taaPrevTransforms.find(obj.entityId);
const glm::mat4 prevModel = (it != end) ? it->second : obj.transform;
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Flache `std::vector<std::pair<uint32_t, mat4>>` (sortiert nach `entityId` oder Open-Addressing-Tabelle mit behaltener Kapazität) statt Knoten-Map. Velocity mit `batchDepthRuns` (wie SSAO/GI-Prepass) instanziieren: `{mvpJitter, mvpNow, mvpPrev}` (192 B/Instanz) in einen Ring-Slice, `velocityVertexInstanced`. Cluster-Instanzen statisch ebenfalls als Instanz-Array zeichnen.
- **Erwarteter Effekt**: Entfernt per-Frame-Heap-Churn und reduziert Draws auf die Anzahl Mesh-Gruppen (Schätzung: bei TAA + 20k Objekten 1–3 ms CPU).
- **Risiko / Verifikation**: Pass muss bit-identische Geschwindigkeiten liefern (Kommentar „entity-keyed history holds ONE pose per entity"). Capture-Vergleich mit TAA/MetalFX; Geister-Test bewegter Objekte.

### REN-413 – Cloud-Shadow-Pass: pro Frame O(n)-Szenen-AABB und vollständiges 512²-Re-Render
- **ID**: REN-413
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:11801–11888 (`EncodeCloudShadow`), Shader-Feld 4720–4822
- **Problem**: (a) Die Bodenhöhe wird über eine Schleife über **alle** `m_renderWorld.objects` (`sceneBox.expand`) ermittelt, nur um `groundY` (min) auf einem sehr grob hysteresierten Wert zu halten (±35 % Wolkenhöhe Totband). (b) Die Karte (512² = 262k Texel × 6 Marsch-Schritte × `cloudFieldDensity`, Realistic-Stil mit ≥12 3D-Textur-Fetches/Auswertung) wird **jeden Frame** neu gerendert, obwohl sie nur von Zeit, Wind, Sonne, Coverage und texel-gerasterter Kameraposition abhängt (Origin ist auf Texelgrenzen gerastert, ändert sich also selten).
- **Code-Auszug**:
```cpp
HE::AABB sceneBox;
for (const RenderObject& o : m_renderWorld.objects) sceneBox.expand(o.worldBounds);
...
[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];  // jeden Frame
```
- **Lösung**: (a) Szenen-Bounds einmal pro Extract im `RenderExtractor` (oder zumindest nur alle N Frames / bei Objektanzahl-Änderung) berechnen und hier lesen – `RefineObjectBounds` hat die Daten bereits angefasst. (b) Karte nur bei Änderung neu rendern: Zeit-Quantisierung (z. B. alle 4–8 Frames bzw. wenn `|Δtime·evo|` einen Schwellwert überschreitet), sofort bei Änderung von Sonne/Wind/Coverage/Origin-Texel.
- **Erwarteter Effekt**: GPU-Zeit des Passes um 75–90 % reduzierbar; CPU-Schleife entfällt (Schätzung 0,05–0,5 ms CPU, GPU-Gewinn **messen**).
- **Risiko / Verifikation**: Schatten dürfen nicht sichtbar ruckeln (Interpolation zwischen zwei Karten oder Zeitquantisierung < Wolkenbewegung/Texel). Vergleich mit `HE_SKY_TIME`-Captures.

### REN-414 – Preview-/Thumbnail-Pfade warten pro Aufruf auf die GPU (auch ohne Readback)
- **ID**: REN-414
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:9997–9998 (`RenderMaterialPreview`), 10077–10078 (`CommitAndReadThumbnail`), 10599–10600 (`RenderSkeletalPreview`), 10906–10907 (`RenderWorldPreview`), 11134–11135 (`RenderParticlePreview`)
- **Problem**: `[cb waitUntilCompleted]` wird in den Live-Previews (Material-Editor, Skeletal-Editor, World-Preview, Particle-Preview) **in jedem Frame** unbedingt aufgerufen, obwohl die Texturen nur an ImGui weitergereicht werden und nur der Debug-Dump (`HE_*_PREVIEW_DUMP`, nur bei gesetzter Env-Variable) einen CPU-Readback braucht. Der Editor-Frame stallt damit pro offenem Preview-Panel auf Vorgänger-Latenz + GPU-Zeit. Content-Browser-Thumbnails (`RenderAssetThumbnail`) warten je Kachel einzeln.
- **Code-Auszug**:
```cpp
[cb commit];
[cb waitUntilCompleted];
if (staging) { ...Dump... }
return m_previewColorTex; // id<MTLTexture> for ImGui::Image
```
- **Lösung**: Wait nur wenn `staging` gesetzt ist (Dump-Pfad) bzw. für Thumbnails, die zwingend Bytes liefern. Live-Previews reihen sich auf derselben Queue vor dem ImGui-Pass ein – die Reihenfolge ist durch die Queue garantiert, kein Wait nötig. Thumbnails: Batch (mehrere Kacheln in einem Command-Buffer, ein Wait) oder asynchron per Completed-Handler mit Callback.
- **Erwarteter Effekt**: Entfernt einen CPU-GPU-Roundtrip pro Preview und Frame (editor-only; Schätzung 0,5–3 ms/Frame je offenem Preview).
- **Risiko / Verifikation**: Die Preview-Ziele sind Member-Texturen, die im nächsten Frame wiederverwendet werden: Da dieselbe Queue sequenziell abarbeitet, kein Hazard. Bei Größenänderung (`CFBridgingRelease` der Textur, 9945ff) muss die Texture-Retire-Logik (`RetireTexture`) statt sofortiger Freigabe genutzt werden, sobald nicht mehr gewartet wird – Test: Panel-Resize während Preview.

### REN-415 – Pro-Frame-`MTLBuffer`/`std::vector`-Allokationen außerhalb des Upload-Rings
- **ID**: REN-415
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 7183–7191 (`CollectRibbonDraws`), 17828–17833 (`DrawParticleGraphBatches`), 10942–10949 (`EncodeParticleBillboards`), 10787–10792 (World-Preview-Grid), 8266–8282 (SW-GI-Instanzen), 14649–14658 und 16426–16436 (`std::vector<glm::mat4> xf` pro instanziertem DrawCall)
- **Problem**: Die Datei hat mit `UploadTransient`/`UploadTransientBytes` (7032–7081) einen Ring für Frame-Daten, nutzt ihn aber für Ribbons (2 Puffer pro Batch), Particle-Graph-Batches (1 Puffer + `std::vector<float>` pro Batch), Preview-Partikel, World-Preview-Grid und SW-GI-Instanzen nicht. Im Opaque-/G-Buffer-Loop wird pro instanziertem `DrawCall` ein neuer `std::vector<glm::mat4> xf` mit `reserve` allokiert, dann per `UploadTransientBytes` ein zweites Mal kopiert.
- **Code-Auszug**:
```cpp
id<MTLBuffer> vbuf = [device newBufferWithBytes:rb.vertices.data() ...];
id<MTLBuffer> ibuf = [device newBufferWithBytes:rb.indices.data() ...];
m_ribbonBuffers.push_back((void*)CFBridgingRetain(vbuf));
...
std::vector<glm::mat4> xf; xf.reserve(instCount * 2);   // pro DrawCall
```
- **Lösung**: Für Ring-fähige Daten `UploadTransient` verwenden und direkt in den Slice schreiben (`float* ptr = UploadTransient(...)`, wie `EncodeDebugLines` 7001). Ribbons/Particle-Batches brauchen dafür `vbufOffset` in `TPDraw` (heute nicht vorhanden – `TPDraw` erweitern). `xf` als wiederverwendeten Member-Scratch oder direkt in den Ring-Slice schreiben, die Zwischenkopie entfällt.
- **Erwarteter Effekt**: Entfernt 1–3 Metal-Allokationen je Batch/Frame und eine Heap-Allokation je instanziertem Draw (Schätzung: kleine, aber kontinuierliche Entlastung; relevant bei Partikel-/Trail-reichen Szenen).
- **Risiko / Verifikation**: `m_ribbonBuffers`-Lebensdauer-Hack („Last frame's staging buffers go now") fällt weg, wenn der Ring den Lifetime-Job übernimmt. Capture-Vergleich Ribbons/Partikel.

### REN-416 – Transparenz-Replay: schwergewichtige `TPDraw`-Kopien, Sortierung großer Structs, `matLight` pro Draw neu gesendet
- **ID**: REN-416
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 6402–6408 (`TPDraw`), 14533–14547, 14709–14712, 14326, 14739–14754
- **Problem**: `TPDraw` hält `UnlitUniforms` (224 B), einen `std::vector<float> params` (Heap-Kopie des Material-Parameterblocks pro transparentem Draw: `t.params = *cMaterialParams`), ein `gtex[4]`-Array usw. (~330 B). `std::sort` verschiebt diese Structs O(n log n) statt Indizes. Im Replay und im `forwardOpaque`-Replay wird bei jedem Draw mit eigener Pipeline `setFragmentBytes(&matLight, sizeof(matLight))` gesendet – `HE::MaterialShaderLibrary::Lighting` ist >2 KB groß (u. a. `localShadowVP[16][16]` = 1 KB, `csmVP`), obwohl sich der Block innerhalb des Passes nicht ändert (Kommentar 14730: „matLight@1 is still bound"); WPO-Draws senden ihn zusätzlich als Vertex-Bytes.
- **Code-Auszug**:
```cpp
if (cMaterialParams) t.params = *cMaterialParams;
...
std::sort(transparent.begin(), transparent.end(), [](const TPDraw& a, const TPDraw& b){ return a.distSq > b.distSq; });
...
[encoder setFragmentBytes:&matLight length:sizeof(matLight) atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];
```
- **Lösung**: `params` als Zeiger/Index in ein Frame-Arena (oder `shared_ptr<const vector>`) statt Kopie; Sortierung über `vector<pair<float,uint32_t>>`-Index. `matLight` nur beim Pipeline-Wechsel setzen, wenn der Block nicht ohnehin seit der letzten Bindung gültig ist (Buffer-Slot bleibt im Encoder bestehen, solange kein anderer Pass ihn überschreibt) oder einmal in den Ring legen und per `setFragmentBuffer:offset:` binden.
- **Erwarteter Effekt**: Weniger Heap-Allokationen und Speicherverkehr bei Transparenz-/Partikel-Szenen (Schätzung: 2 KB × Draws gespart; bei 500 transparenten Custom-Draws ~1 MB/Frame).
- **Risiko / Verifikation**: Zwischen Pipeline-Wechseln darf sich Buffer 1 nicht ändern – `BindMaterialPreambleSlots`/Sky-Pass überschreiben nicht Slot `kMetalLightingBufferIndex`, aber der Sky-Pass setzt Fragment-Buffers 0–2 → vor dem Replay einmal neu binden (heute schon der Fall, 14715).

### REN-417 – `ResolveGraphTexture` baut pro Aufruf einen String-Schlüssel (auch im Draw-Pfad)
- **ID**: REN-417
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:9511–9524; Aufrufer im Hot Path: 14588–14591 und 16370–16373 (pro Draw/Instanz, auch für `kDefaultLayer0WeightTextureId`), 13567 (pro UI-Material-Quad × 4 Slots), 14457/16253, 8312, 15169
- **Problem**: `key = std::to_string(texId.hi) + ":" + std::to_string(texId.lo)` (+ `"#arr"`) erzeugt drei String-Allokationen pro Lookup, danach Hash des Strings. Besonders ärgerlich: jeder Custom-Material-Draw ohne eigene Weightmap fragt pro Instanz zweimal (`dc.weightmapTextureId` leer → Default-Layer0-Textur), also zwei Lookups mit String-Bau pro Instanz-Draw.
- **Code-Auszug**:
```cpp
std::string key = texId != HE::UUID{} ? (std::to_string(texId.hi) + ":" + std::to_string(texId.lo)) : path;
if (array) key += "#arr";
if (auto it = m_graphTexCache.find(key); ...)
```
- **Lösung**: UUID-basierter Schlüssel: `unordered_map<GraphTexKey{UUID id; bool array}, void*>` (Hash über `hi^lo`), Pfad-only-Texturen in eine zweite Map. Default-Weightmap als gecachter Member-Zeiger (`m_defaultWeightTex`, bei `InvalidateTexture` zurücksetzen). Die Weightmap-Bindung pro Draw nur bei Änderung des Zeigers neu setzen.
- **Erwarteter Effekt**: Entfernt 3 Allokationen × Lookups pro Custom-Draw (Schätzung 100–300 ns pro Lookup; bei 5k Draws 1–3 ms/Frame).
- **Risiko / Verifikation**: Invalidierung (`m_pendingTexInvalidations`, 16545–16557) benutzt heute die String-Schlüssel `"hi:lo"` und `"#arr"` – muss mit der neuen Schlüsselform konsistent bleiben. Test: Weightmap-Paint (Terrain) aktualisiert die Textur sichtbar.

### REN-418 – Pipeline-Archiv wird bei jeder neuen PSO komplett neu serialisiert (synchron)
- **ID**: REN-418
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:12781–12795 (`GetOrBuildMaterialPipeline`), `WarmupMaterials` 9588–9626
- **Problem**: Nach jedem erfolgreich gebauten Material-PSO (pro Material mindestens opaque + blend, im Deferred-Modus +G-Buffer) wird `addRenderPipelineFunctionsWithDescriptor:` aufgerufen und danach `serializeToURL:` – die **gesamte** Archivdatei wird synchron auf dem Render-Thread neu geschrieben. Beim Warmup von M Materialien entstehen 2–3·M Serialisierungen eines wachsenden Archivs (O(M²) I/O). Die Datei-I/O liegt im Frame-Pfad (Ladezeit/Hitch).
- **Code-Auszug**:
```cpp
if ([arch addRenderPipelineFunctionsWithDescriptor:desc error:&aerr] && !m_matArchivePath.empty())
    [arch serializeToURL:[NSURL fileURLWithPath:@(m_matArchivePath.c_str())] error:nil];
```
- **Lösung**: Serialisierung entkoppeln: Dirty-Flag setzen und einmal pro Warmup-Ende bzw. mit Debounce (z. B. 2 s Idle) oder im `Shutdown()` auf einem Hintergrund-Job schreiben. `WarmupMaterials` ruft am Ende genau einmal `FlushMaterialArchive()`.
- **Erwarteter Effekt**: Warmup/Szenenöffnen mit vielen Materialien: von O(M²) Datei-I/O auf O(M); Hitch-Reduktion (Schätzung bei 100 Materialien Sekunden bis wenige 100 ms).
- **Risiko / Verifikation**: Absturz-Sicherheit (Kommentar 12598: `serializeToURL` ist auf macOS 26+ instabil und deshalb dort ganz abgeschaltet) – Hintergrund-Thread darf Metal-Archive nicht parallel zu `addRender…` anfassen; deshalb nur am definierten Punkt auf dem Render-Thread flushen. Test: Neustart lädt Archiv, `HE_MTL_ARCHIVE=1`.

### REN-419 – Mesh-Invalidierung gibt die Mesh-Textur nicht frei (Leak) und lädt sie bei jedem Edit neu
- **ID**: REN-419
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: MetalRenderer.mm:16516–16524 (Invalidierung), 9248–9267 (`ResolveMesh`: Textur-Upload), 6722–6728 (Shutdown gibt `mesh.texture` frei)
- **Problem**: `ResolveMesh` retained pro Mesh `vertexBuf`, `indexBuf`, `texture` (und ggf. `blas`). Bei `m_pendingMeshInvalidations` (Sculpting, Terrain-Edit) werden nur `vertexBuf`, `indexBuf`, `blas` mit `CFBridgingRelease` freigegeben; `it->second.texture` fehlt, obwohl `Shutdown()` (6727) sie korrekt freigibt. Jedes Invalidieren eines texturierten Meshes leakt damit eine `MTLTexture` (bis zu MB-Größe), und der nächste `ResolveMesh` lädt dieselbe Basis-Textur erneut hoch (`uploadMetalTexture`) statt sie zu behalten. Zusätzlich: Die Freigabe erfolgt sofort statt über `RetireTexture`/Frame-Alter, obwohl dieselbe Datei an anderen Stellen genau dafür Retire-Listen hat.
- **Code-Auszug**:
```cpp
if (it->second.vertexBuf) CFBridgingRelease(it->second.vertexBuf);
if (it->second.indexBuf)  CFBridgingRelease(it->second.indexBuf);
if (it->second.blas)      CFBridgingRelease(it->second.blas);
m_meshCache.erase(it);       // texture bleibt retained → Leak
```
- **Lösung**: `if (texture) RetireTexture(texture)` ergänzen **oder** besser: Texture beim Invalidieren im Cache behalten (nur Geometrie ersetzen) und beim Neu-Resolve wiederverwenden. Dasselbe gilt für `m_skeletalMeshCache`, falls dort invalidiert wird (derzeit nicht).
- **Erwarteter Effekt**: Beseitigt wachsenden VRAM-/Shared-Memory-Verbrauch bei Sculpt-Sessions; weniger Upload-Last pro Edit.
- **Risiko / Verifikation**: Niedrig. Test: Terrain-Sculpt-Session (100 Edits) + Speicher-Monitor/Instruments (Allocations) vor/nach.

### REN-420 – Gleicher Extract-/Setup-Block sechsmal kopiert; `matLight`-Fill in vier Varianten
- **ID**: REN-420
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: Extract-Block: 7881–7891 (`ExtractFrame`), 8374–8380, 12367–12374, 14009–14019, 16095–16103, 17073–17079; `matLight`-Fills: 13426–13444 (UI), 14785–14903 (`FillMaterialLighting`), 16130–16154 (G-Buffer)
- **Problem**: Die vier Zeilen `setDayNight(...)`/`setContentManager`/`extract(...)` existieren bereits als `ExtractFrame(aspect)`, werden aber in `EncodeGIAccelBuild`, `EncodeSSAO`, `EncodeScene`, `EncodeGBuffer` und im Low-Res-Cloud-Pfad von `EncodeFrame` inline wiederholt (Kommentar: „the established convention in this file"). Zusätzlich existieren drei Teil-Duplikate der Lighting-Fill-Logik (Dominant-Light, Camera, Wind/Weather/Water), die von `FillMaterialLighting` abweichen; `FillMaterialLighting` selbst läuft bis zu 3× pro Frame (EncodeScene 14235, EncodeDeferredResolveTile 15031, EncodeSSRPasses 15494) mit identischen Eingaben inklusive 16 lokaler Shadow-Matrizen, `glm::inverse(view)` und Cascade-Matrizen.
- **Code-Auszug**:
```cpp
m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay, env.sunColor, env.sunIntensity,
                        env.moonColor, env.moonIntensity, env.cloudCoverage);
m_extractor.setContentManager(m_contentManager);
m_extractor.extract(*m_world, m_renderWorld, aspect, &m_editorCamera);
```
- **Lösung**: Alle Stellen auf `ExtractFrame(aspect)` (optional mit `extractUI`-Flag) umstellen. `FillMaterialLighting` einmal pro Frame in einen Member-Block (`m_frameMatLight`) schreiben und pro Pass nur die abweichenden Felder (`specAA[1]`, `ssr[...]`, Cluster) auf einer Kopie patchen. UI-/G-Buffer-Varianten als „Minimal-Profil" von derselben Funktion bedienen.
- **Erwarteter Effekt**: ca. 60–100 Zeilen weniger; gleiche Ergebnisse; weniger CPU für Matrix-Arbeit (Schätzung <0,1 ms/Frame).
- **Risiko / Verifikation**: `ExtractFrame` hat identischen Inhalt (7884–7890). Wegen des `FrameScope` (16469) ist der wiederholte `extract()` ein Key-Compare. Capture-Vergleich aller Pfade.

### REN-421 – SSR-Trace und Blur-Kette dreifach implementiert
- **ID**: REN-421
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: 15366–15422 (Trace-Uniforms/Encoder in `EncodeSSRPasses`) vs. 15584–15637 (`EncodeForwardSSR`); `blurPass`-Lambda 15431–15449, 15643–15662, 15970–15989 (`EncodeGIReflections`)
- **Problem**: Die Befüllung von `SSRTraceUniforms` (15 Felder), die Temporal-Hist-Berechnung (4×4-Delta-Schleife) und das `blurPass`-Lambda stehen mit minimalen Abweichungen (Textur-Quellen) in drei Funktionen. Dazu zwei weitere unabhängige Kopien der Qualitäts-Tier-Logik (`m_ssrQuality <= 0 ? 16 : …`).
- **Code-Auszug**:
```cpp
auto blurPass = [&](void* src, void* dst, float dx, float dy) {
    HE::MaterialShaderLibrary::SSRBlurUniforms bu; bu.dir[0] = dx; ...
    MTLRenderPassDescriptor* bp = [MTLRenderPassDescriptor renderPassDescriptor]; ...
```
- **Lösung**: Member-Funktionen `EncodeFullscreenBlur(cmdBuf, src, dst, dx, dy, texW, texH)` und `EncodeSsrTrace(cmdBuf, TraceInputs{colorTex, attrTex, depthTex, forwardColorMode})`; `SSRTraceUniforms` über eine `MakeSsrTraceUniforms(viewProj, hist, seed, mode)`-Funktion befüllen. Das Muster mit `fullscreenPass` aus `EncodeBloom` (11922) kann dasselbe Lambda für alle verwenden.
- **Erwarteter Effekt**: ~120 Zeilen weniger; ein Ort für Tier-Anpassungen.
- **Risiko / Verifikation**: bit-identische Captures mit SSR Q0/Q1/Q2, deferred + forward, GI-Refl Q0–Q2.

### REN-422 – Post-Chain: Vollbild-Kopien statt Ping-Pong, Bloom als 11 Einzel-Encoder
- **ID**: REN-422
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 13060–13069 (`EncodeTaa`: Blit `m_taaResolved → m_taaHistory`), 17113–17143 (Forward-SSR: Blit `m_hdrColor → m_ssrColorHist`), 11911–11963 (`EncodeBloom`)
- **Problem**: TAA kopiert jedes Frame das komplette LDR-Bild (BGRA8, sceneW×sceneH) in die History; Forward-SSR kopiert jedes Frame das komplette HDR-Bild (RGBA16F, ~8 B/Pixel → 16 MB bei 1080p). Beides wäre als Ping-Pong zweier Texturen kopierfrei. Bloom nutzt eine Kette 1 Bright + 10 Blur-Encoder auf Halbauflösung (5 H + 5 V), jeder als eigener Render-Pass mit Store/Load im DRAM; eine Downsample-Kette (Mips, Dual-Kawase) deckt dieselbe Reichweite mit weniger/ kleineren Passes ab.
- **Code-Auszug**:
```cpp
id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
[blit copyFromTexture:(__bridge id<MTLTexture>)m_taaResolved ... toTexture:(__bridge id<MTLTexture>)m_taaHistory ...];
```
- **Lösung**: TAA: zwei `m_taaResolved[2]`, History = Vorjahres-Resolved; Index flippen statt Blit. Forward-SSR: HDR-Ziel alternierend (`m_hdrColor[2]`) oder SSR liest direkt die Vorjahres-Scene-Color. Bloom: Kette auf 5–6 Passes über Mip-Pyramide umbauen (Qualität prüfen).
- **Erwarteter Effekt**: pro Frame ein Vollbild-Blit (1080p BGRA8 ≈ 8 MB / RGBA16F ≈ 16 MB Lese+Schreib) weniger; Bloom GPU-Zeit je nach Auflösung deutlich geringer (**messen**, Apple-TBDR-Bandbreite ist hier der Engpass).
- **Risiko / Verifikation**: Ping-Pong ändert Textur-Lebensdauern (Resize, MetalFX-Output, `m_taaHistoryValid`). Bloom-Umbau ändert das Bild → nur mit visuellem A/B.

### REN-423 – 168 händische `if (x) { CFBridgingRelease(x); x = nullptr; }`-Blöcke
- **ID**: REN-423
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: 6694–6920 (`Shutdown`, ~150 Zeilen), `Destroy*`-Funktionen (8726, 11492, 11544, 11709, 12000, 12111, 12338, 13227, …), `void*`-Member in `MetalRenderer.h`
- **Problem**: Alle Metal-Objekte liegen als `void*` mit manuellem `CFBridgingRetain/Release` im Header (ARC-Bridging wegen C++-Header). `grep` zählt 168 solche Blöcke; ein vergessenes Feld in `Shutdown` oder `Destroy*` ist ein stiller Leak (Beispiel REN-419). `Shutdown` ist 227 Zeilen reine Freigabe.
- **Code-Auszug**:
```cpp
if (m_fxaaPipeline)  { CFBridgingRelease(m_fxaaPipeline);  m_fxaaPipeline = nullptr; }
if (m_aaBlitPipeline){ CFBridgingRelease(m_aaBlitPipeline);m_aaBlitPipeline = nullptr; }
```
- **Lösung**: Kleines `MtlHandle`-RAII (`struct MtlHandle { void* p; ~MtlHandle(){ if(p) CFBridgingRelease(p);} ... }`, move-only, `get()`) oder zumindest freie Funktion `static void ReleaseHandle(void*& p)`. Mit RAII entfällt `Shutdown` weitgehend (Destruktor), Reihenfolge-Abhängigkeiten (Device zuletzt) per Member-Deklarationsreihenfolge. Header-Änderung betrifft nur die Member-Typen.
- **Erwarteter Effekt**: ~250 Zeilen weniger, Leak-Klasse eliminiert.
- **Risiko / Verifikation**: Mittel – Destruktionsreihenfolge (Device/Queue zuletzt, `m_retiredTextures` vor Targets, Completion-Handler halten `m_uploadRing` per `shared_ptr`). Bestehende Re-Initialize-Semantik (Flags `m_*Tried` zurücksetzen) muss bleiben.

### REN-424 – Software-GI-Kernel: lineare Instanzschleife pro Strahl (kein TLAS)
- **ID**: REN-424
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:3411–3444 (`giSceneAnyHit`, `giSceneClosestHit`), 3771–3793 (`giSceneClosestHitTri`)
- **Problem**: Der SW-Fallback testet **jede** Instanz pro Strahl (inkl. 2× `float4x4 × float4` pro Instanz) und fährt dann erst die BLAS. Kosten pro Strahl O(Instanzen); bei Shadow-Rays pro Pixel (`spp` Strahlen) plus Probe-Rays plus Refl-Rays mit bis zu 4 Bounces multipliziert das massiv. Der Pfad greift auf Geräten ohne HW-RT bzw. mit `HE_GI_FORCE_SW`, ist also Sekundärpfad, aber dann ist er der einzige.
- **Code-Auszug**:
```cpp
for (int i = 0; i < instCount; ++i) {
    const float3 oL = (insts[i].invTransform * float4(o, 1.0)).xyz;
    ...
    if (giBlasHit(...)) return true;
```
- **Lösung**: Top-Level-BVH über die Instanz-Welt-AABBs (CPU-Build in `EncodeGISwAccelBuild`, wie `HE::buildGiBvh` es für Meshes schon kann) und im Kernel dieselbe Traversal-Routine (`giBlasHit`-Muster) auf Instanz-Boxen; erst bei Blatt der Instanz in die BLAS. Alternativ grobes Uniform-Grid der Instanz-Indizes.
- **Erwarteter Effekt**: von O(I) auf ~O(log I) pro Strahl; bei >100 Instanzen mehrfacher GPU-Gewinn (Schätzung).
- **Risiko / Verifikation**: GI-SW-Ergebnisse müssen gleich bleiben (gleicher Test wie HW-Kernel; `tests/test_gi_bvh.cpp` auf TLAS erweitern, CPU-Referenz `giBvhIntersect` existiert). Das GL-4.3-Port (`GiBvh`) profitiert vom selben Top-Level-BVH.

### REN-425 – Identische MSL-Hilfscode-Blöcke in sieben Libraries, nur per Kommentar synchron gehalten
- **ID**: REN-425
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `octEncode/octDecode` (565, 2576, 2591, 2897–2912, 3549, 3564), `giPcg3d/giHash2/giConeSample` (2433–2456, 2922–2943, 3460–3481), `GILand/giAutoLandAlbedo/giLandscapeAlbedo` (2827–2893 vs. 3288–3342), `giSampleFieldIrradiance`/`giReflField` (2602, 2949, 3571, 3797), Fullscreen-Triangle-Vertex (10×, z. B. 1004, 1090, 1300, 1538, 1595, 1723, 1840, 1963, 2244, 4050)
- **Problem**: Jeder `R"MSL(…)"`-String wird als eigene Library kompiliert; die Kopien tragen Kommentare wie „keep every copy on this one" / „byte-identical twin" / „SYNC". Die PCG-Hash-Funktion wurde schon einmal an einer Stelle gefixt (Thema 131), die Gefahr von Drift ist belegt. `kSkyMSL`/`kSkyFuncMSL` (4024–6367) ist zudem eine handgepflegte Spiegelung des Sky-Shaders in `SkyShaderSource.h` (siehe CLAUDE.md §1) mit vier laut Kommentar synchron zu haltenden Kopien (Z. 6167–6169).
- **Code-Auszug**:
```cpp
// Cone-jitter random numbers ... keep every copy on this one.
static uint3 giPcg3d(uint3 v) {...}            // Z. 2433, 2922 (als giPcg3dR), 3460
```
- **Lösung**: Gemeinsame Snippets als `static const char*` (z. B. `kMslOct`, `kMslPcg`, `kMslGiLand`, `kMslFsTriVertex`) definieren und per Konkatenation/`injectSkyMSL`-Muster (6370) in die Library-Strings einsetzen. Die einzelnen Libraries bleiben getrennte Kompilationseinheiten (Compile-Isolation bleibt), der Quelltext ist aber einmalig. Für `kSky*`: Mittel- bis langfristig Quelle aus `SkyShaderSource.h` per Übersetzung (shaderc/SPIRV-Cross) erzeugen, wie es `MaterialShaderLibrary` für Materialien bereits macht – nur Bewertung, größerer Umbau.
- **Erwarteter Effekt**: ~300 Zeilen Shader-Duplikat entfallen; geringere Drift-Gefahr zwischen Metal/GL/Vulkan.
- **Risiko / Verifikation**: Reiner Textumbau, Shader-Ergebnisse unverändert (Kompilation + Capture). Wegen `#include <metal_raytracing>` nur in den RT-Libraries die Snippets ohne Namensraum-Konflikte einfügen (`static`-Funktionen).

---

### REN-426 – Kleinere Mehrfach-Duplikate: `HE_SKY_TIME` (9×), `precompiledShaders`-Suche (7×), Half-Float-Decoder (4×), Preview-Target-Boilerplate
- **ID**: REN-426
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `getenv("HE_SKY_TIME")`: 10771, 11880, 12235, 13792, 14038, 15014, 15484, 16129 (je pro Frame/Aufruf!); `var.backend == …Metal`-Schleife: 7163, 9604, 9649, 13298, 14433, 16228, 17817; `auto h2f`: 10005, 10083, 10607, 11142; Preview-Ziel-(Re)Creation: 9945–9958, 10038–10052, 10168–10182, 10448–10463, 10719–10742, 11064–11078; Preview-Sphere-Rebuild: 9680–9692 vs. 10284–10298; Mesh-Interleave: 9226–9241 vs. 9297–9310
- **Problem**: `std::getenv` (lineare `environ`-Suche, bei manchen libc mit Lock) läuft in 8 Pro-Frame-Pfaden; das Ergebnis ist prozesskonstant. `MaterialShaderLibrary::precompiledFor(ma, RendererBackend::Metal)` existiert bereits (`MaterialShaderLibrary.cpp:954`) und liefert für Metal das identische Ergebnis wie die handgeschriebene Schleife. `h2f` ist ein viermal kopierter Half→Float-Decoder; Clang auf Apple-Hardware stellt `_Float16`/`__fp16` bereit. Die `built.insert(end, {…})`-Initializer-List-Variante im Interleave ist langsamer als direktes Beschreiben eines vorab dimensionierten Arrays.
- **Code-Auszug**:
```cpp
if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov) skyClock = static_cast<float>(std::atof(ov));
```
- **Lösung**: `static float SkyClockSeconds()` (einmal gelesener Override als `static const`) für alle 8 Stellen; `precompiledFor` verwenden; `HalfToFloat`/Dump-Helfer (`WriteDumpPPM`) einmal definieren; `EnsureColorDepthTarget(void*& color, void*& depth, w, h, fmt)` für die Preview-Ziele; `InterleaveMeshVertices(asset, out)` für beide Mesh-Uploads.
- **Erwarteter Effekt**: ~150 Zeilen weniger; vernachlässigbarer, aber kostenloser Laufzeitgewinn (getenv im Frame entfällt).
- **Risiko / Verifikation**: Gering. Hinweis: Wenn jemand `HE_SKY_TIME` zur Laufzeit ändert (Debug), wäre der Wert nach „einmal lesen" eingefroren – dann nur bei Capture-Start neu lesen.

### REN-427 – Tote/veraltete Stellen: ungenutzte Variable/Parameter, falsche Zeilenreferenz im Kommentar, `const_cast`, redundante Kopie
- **ID**: REN-427
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: 6993 (`EncodeDebugLines`: `device` unbenutzt), 13901–13902 (`EncodeSkinnedObjects(…, bool shadows, …)`: `shadows` nie gelesen), 18037 (Kommentar „MetalRenderer.mm:6282" – tatsächliche UI-Pipeline-Erstellung bei 7378–7399), 8438 (`const_cast<GpuMesh&>`), 6928–6931 (`SetDebugLines` kopiert den Vektor jedes Frame), 7022 (`glm::mat4 vp = viewProj;` nur für `setVertexBytes`)
- **Problem**: Kleine Reste, die beim Lesen irritieren: unbenutzte Variable/Parameter, ein Verweis auf eine Zeilennummer, die sich nicht mehr deckt (Kommentare, die Code statt Absicht beschreiben, sind in dieser Datei sonst vorbildlich), und ein `const_cast`, weil `ResolveMesh` einen `const GpuMesh*` liefert, der `blas` später nachträglich setzt.
- **Code-Auszug**:
```cpp
id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;   // Z. 6993, nie benutzt
GpuMesh& mesh = const_cast<GpuMesh&>(*resolved);            // Z. 8438
```
- **Lösung**: Entfernen/umbenennen; `ResolveMeshMutable()` oder `EnsureBlas(const HE::UUID&)` statt `const_cast`; Zeilenverweis durch Funktionsnamen ersetzen („`CreateScenePipeline`"); `SetDebugLines` per `std::move`-Overload/`swap`.
- **Erwarteter Effekt**: Lesbarkeit; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Keines (Compiler-Warnungen `-Wunused-variable`, `-Wunused-parameter` aktivieren, um weitere zu finden).

### REN-428 – Shader-Mikro-Optimierungen (Fragment-Hot-Path): `tan(acos())`, 9 manuelle PCF-Taps, Per-Pixel-Sun-Probe im Lens-Flare
- **ID**: REN-428
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: 498 und 541 (`shadowFactor`, `localShadowFactorAt`), 501–506 / 543–548 (3×3-PCF), 1043–1044 (`lensFlareOverlay`), 2171–2181 (`ssaoBlurFragment`)
- **Problem**: (a) `tan(acos(ndl))` pro Fragment und pro Licht (zwei transzendente Funktionen) ist algebraisch `sqrt(1−ndl²)/ndl`. (b) 3×3-PCF = 9 Einzel-`sample`+Vergleiche pro Licht und Fragment; MSL bietet `depth2d_array::sample_compare` mit Hardware-PCF (bilinear 2×2) und weniger Taps. (c) Der 5-Tap-HDR-Luma-Probe am Sonnenort ist für alle Pixel identisch, wird aber pro Pixel ausgeführt, sobald `lf.w > 0` – lässt sich auf CPU/ein Mini-Pass auslagern. (d) SSAO-Blur macht 16 `sample`s statt 4 bilinear versetzten.
- **Code-Auszug**:
```msl
float bias = clamp(scene.shadowBias.x * tan(acos(ndl)), scene.shadowBias.y, 0.02) * float(c + 1);
for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) { ... shadowMap.sample(...) ... }
```
- **Lösung**: (a) `sqrt(max(1.0 - ndl*ndl, 0.0)) / max(ndl, 1e-4)`; (b) `sample_compare` mit Comparison-Sampler (`compare_func::less_equal`) und 2×2-Kernel; (c) Sun-Luma einmalig pro Frame; (d) vier bilineare Taps mit Halb-Texel-Versatz.
- **Erwarteter Effekt**: ALU-/Tex-Einsparungen im Shading-Pass (Schätzung 3–10 % Fragmentzeit in schattenlastigen Szenen, **messen mit Metal GPU Counters**).
- **Risiko / Verifikation**: (a) Numerisch äquivalent bis auf Rundung am ndl→0-Rand (Clamp deckt). (b) verändert die Schattenweichheit sichtbar → nur mit Bildvergleich (GL/D3D/Vulkan haben eigene, daher Backend-Parität prüfen). Nicht ohne Messung umsetzen.

### REN-429 – `EncodeSkyReflCube`: 6 Render-Encoder + Sky-Pass pro Frame, solange GI-Reflexionen aktiv sind
- **ID**: REN-429
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: MetalRenderer.mm:13771–13809
- **Problem**: Die Reflection-Sky-Cube wird jeden Frame auf allen 6 Faces (128² mit Wolken-Marsch, ggf. Aurora/Nebel bei Nacht) neu gerendert; der Kommentar rechtfertigt es mit driftenden Wolken. Wolken bewegen sich aber im Sekunden-/Minutenmaßstab; jede Face bekommt eigenen Encoder und Render-Pass-Setup.
- **Code-Auszug**:
```cpp
for (int f = 0; f < 6; ++f) { ... [cmdBuf renderCommandEncoderWithDescriptor:pass]; EncodeSky(..., /*cubeBake=*/true); [enc endEncoding]; }
```
- **Lösung**: Eine Face pro Frame (Round-Robin, 6-Frame-Zyklus) oder Neubake nur bei Änderung von Sonne/Zeitquantum; optional alle Faces in einem Pass mit `[[render_target_array_index]]` (Amplification) rendern.
- **Erwarteter Effekt**: 6× weniger Sky-GPU-Last dieses Features (**messen**).
- **Risiko / Verifikation**: Leichte Zeitversetztheit der Reflexionen; für Spiegel/Wasser mit Kamerabewegung ist `camPos` Teil der Eingabe – bei Kamerabewegung ggf. jedes Frame voll rendern.

---

## Gelesen

Ja, MetalRenderer.mm wurde vollständig gelesen (Z. 1–18158: alle eingebetteten MSL-Strings, alle Host-Funktionen), zusätzlich gezielte Stichproben in `MetalRenderer.h`, `MaterialShaderLibrary.h/.cpp`, `RenderSorter.cpp`, `SsaoKernel.h`, `OpenGLRenderer.cpp`/`VulkanRenderer.cpp` für die Cross-Backend-Hinweise; keine Quelldatei wurde geändert.

## Rendering Slice 6 – src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp (Zeilen 1–13589)

## Zusammenfassung

`OpenGLRenderer.cpp` ist funktional sehr vollständig und durchgehend gut kommentiert, aber in drei Punkten teuer: (1) der Pro-Draw-Call-Pfad für Graph-Materialien arbeitet ohne Memoisierung (GLSL-Kopie + Hash, Lighting-Struct-Fill, String-Schlüssel-Lookups pro Draw), obwohl für Mesh/Material-Parameter bereits ein `lastMatId`-Memo existiert; (2) UI-Quads und Schattenebenen werden ohne Batching bzw. ohne Caching jedes Frame komplett neu aufgebaut; (3) die Datei enthält sehr viel Copy-Paste (≈38 `glLinkProgram`-Blöcke, ≈34 `glGenFramebuffers`-Blöcke, dreifach geführte Uniform-Location-Listen, GLSL-Zwillinge). Zusätzlich gibt es tote Pfade (deaktiviertes Reflection-Temporal inkl. VRAM) und Lücken in `Shutdown()`.
Funde: **5 Hoch, 20 Mittel, 4 Niedrig** (29 gesamt). Alle Laufzeit-Aussagen sind aus dem Code abgeleitet, nicht gemessen; wo die Größenordnung unklar ist, steht sie unter „Unsicher / Messung nötig“.

---

## Funde

### REN-601 – `fillMatLight` füllt das komplette Lighting-UBO-Struct bei jedem Material-Draw, obwohl nur einmal pro Frame hochgeladen wird
- **ID**: REN-601
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:12114-12126` (Aufrufstelle), `11598-11690` (Lambda `fillMatLight`)
- **Problem**: Im Forward-Pfad läuft für *jeden* Draw-Call mit Graph-Material (`matProg != 0`) `Lighting lit{}` (Null-Init, > 2 KB: `localShadowVP[16][16]` allein 1 KB) und `fillMatLight(lit)` (dominantDirectionalLight, `FillMaterialLightWindow`, bis zu 16 `zRemap * localViewProj`-Matrixprodukte + memcpy, Cluster-/GI-/Wind-/Weather-/Water-Fill). Das Ergebnis wird nur beim ersten Draw des Frames hochgeladen (`m_matLightUploadedThisFrame`); in allen weiteren Draws wird der Fill komplett verworfen. Der Transparenz-Pfad (12770-12778) macht es richtig, der Opaque-Pfad nicht.
- **Code-Auszug**:
```cpp
glUseProgram(matProg);
HE::MaterialShaderLibrary::Lighting lit{};
fillMatLight(lit); // shared heLitP ABI fill (see the lambda above)
if (!m_matLightUploadedThisFrame) { ... glBufferSubData(..., &lit); m_matLightUploadedThisFrame = true; }
```
- **Lösung**: Fill in den `if` verschieben (wie bei 12770):
```cpp
if (!m_matLightUploadedThisFrame) {
    HE::MaterialShaderLibrary::Lighting lit{};
    fillMatLight(lit);
    glBindBuffer(GL_UNIFORM_BUFFER, m_matLightUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(lit), &lit);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    m_matLightUploadedThisFrame = true;
}
```
- **Erwarteter Effekt**: (Schätzung) pro Graph-Material-Draw ~1–3 µs CPU gespart (Struct-Init + Fill); bei 1–5k Material-Draws ein spürbarer Teil des CPU-Frames.
- **Risiko / Verifikation**: Verhalten identisch (Fill ist innerhalb eines Frames deterministisch; `lit` wird sonst nirgends gelesen). Prüfen, dass kein Draw `m_matLightUploadedThisFrame=false` zwischen Draws setzt (UI/Preview setzen es, laufen aber nicht innerhalb der Schleife). Test: Headless-Frame-Dump vorher/nachher bytegleich.

### REN-602 – `resolveMaterialShader` kopiert und hasht die komplette GLSL-Quelle pro Draw-Call und Frame, ohne Memo
- **ID**: REN-602
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:12066-12079` (Forward), `12006-12034` (transluzent), `11754-11788` (Deferred, zusätzlich `resolveMaterialShaderGB`), `12675-12709` (Ribbons), `4000-4011` (UI pro Quad), `8956-8957`; Ursache `MaterialShaderLibrary.cpp:1000-1008` (`resolveFragment`)
- **Problem**: `resolveMaterialShader()` → `resolveFragment()` macht `glslOut = mat->customShaderFragGlsl;` (Kopie mehrerer KB) und `std::hash<std::string>{}(glslOut)` (O(Länge)) — für *jeden* DrawCall, jeden Frame, nur um danach einen `unordered_map<uint64_t>`-Treffer zu bekommen (`GetOrBuildMaterialProgram`). Zusätzlich werden pro DC `std::string shFrag, shVert` konstruiert, `getMaterial()` 2–3× aufgerufen, die `precompiledShaders`-Liste linear durchsucht und `HE::matGlslTextureArrayMask` + `std::vector<float> mParams`-Kopie (Deferred, 11774) erzeugt. Mesh und Material-Parameter sind dagegen per `lastMatId`/`lastMeshId` memoisiert (11961-11973) — die Custom-Material-Auflösung nicht.
- **Code-Auszug**:
```cpp
uint64_t shKey; std::string shFrag, shVert;
if (resolveMaterialShader(dc.materialAssetId, shKey, shFrag, shVert)) { ...
    matProg = GetOrBuildMaterialProgram(shKey, shFrag, shVert, pre);
```
```cpp
glslOut = mat->customShaderFragGlsl;
hashOut = std::hash<std::string>{}(glslOut);   // MaterialShaderLibrary.cpp:1006-1007
```
- **Lösung**: (1) Im Renderer die Auflösung an das vorhandene `lastMatId`-Memo hängen: pro Material-ID einmal pro Frame `{ matProg, gbProg, precompiled*, arrMask }` ermitteln (kleine `unordered_map<UUID, ResolvedMat>` oder Einzel-Memo wie `cMesh`), pro Draw nur noch lesen. (2) In `MaterialShaderLibrary` Hash einmal beim Laden/Editieren in `MaterialAsset` ablegen (`customShaderHash`) und die Quelle nur bei Cache-Miss kopieren (`resolveShaders` mit `const std::string*`-Out oder Key-only-Variante). (3) Invalidierung über das vorhandene `InvalidateMaterial`.
- **Erwarteter Effekt**: (Schätzung) pro Custom-Material-Draw Wegfall von 1 String-Kopie + 1 O(n)-Hash (≈ 10–50 µs bei 10–40 KB GLSL) → bei einigen hundert Draws ein sichtbarer CPU-Block; im Deferred-Pfad doppelt (Forward + GB-Variante).
- **Risiko / Verifikation**: Cache muss bei Material-Edit (Graph-Neucompile → neue `customShaderFragGlsl`) invalidiert werden (`m_pendingMaterialInvalidations` existiert bereits). Tests: Material-Hot-Reload im Editor, `tests/` zu MaterialShaderLibrary; Bildvergleich Forward/Deferred.

### REN-603 – UI-Pass: ein Draw pro Quad/Glyph mit 15 `glUniform*`-Aufrufen, kein Batching, kein Uniform-Cache
- **ID**: REN-603
- **Kategorie**: B/D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:7873-8027` (insb. `8006-8025`), Shader `3117-3253`
- **Problem**: Jedes `UIRenderObject` (auch jedes einzelne Glyphen-Quad, `type == 2`) setzt unbedingt Rect, Color, UVRect, Rotation, Mode, CornerRadius, BorderWidth, BorderColor, Gradient, GradientColor, GradientAngle, GradientShape, Blur, InnerBlur, InnerColor (15 Uniform-Calls) und zeichnet ein `GL_TRIANGLE_STRIP` mit 4 Vertices. Ein Text mit 100 Zeichen = 100 Draws + 1500 Uniform-Calls pro Frame. Die allermeisten Quads haben Default-Werte für Border/Gradient/Blur/Inner, werden aber trotzdem jedes Mal neu gesetzt.
- **Code-Auszug**:
```cpp
glUniform4f(m_uUIRect,  obj.position.x, obj.position.y, obj.size.x, obj.size.y);
glUniform4f(m_uUIColor, ...); ... glUniform4f(m_uUIInnerColor, ...);
glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
```
- **Lösung**: Stufe 1 (exakt, minimal): Letzte gesetzte Werte für die selten wechselnden Uniforms (`CornerRadius`, `Border*`, `Gradient*`, `Blur`, `InnerBlur`, `InnerColor`, `Rotation`) in einer kleinen Struct cachen und nur bei Änderung setzen. Stufe 2: Läufe aufeinanderfolgender Quads mit gleichem Programm/Atlas/Clip in einen Instanz-Puffer (pro Quad ein `struct{vec4 rect, color, uv, ...}`) schreiben und mit `glDrawArraysInstanced(GL_TRIANGLE_STRIP,0,4,n)` zeichnen (Painter-Reihenfolge bleibt erhalten, da nur aufeinanderfolgende Quads zusammengefasst werden). Stufe 2 verlangt Shader-Änderung `kUIVS/kUIFS` und Spiegelung im Metal-`kUIMSL`.
- **Erwarteter Effekt**: (Schätzung) Stufe 1: 70–90 % der UI-Uniform-Calls entfallen; Stufe 2: Draw-Calls der UI von O(Quads) auf O(Zustandswechsel).
- **Risiko / Verifikation**: Stufe 1 risikolos. Stufe 2 betrifft Rundung/Gradient/Rotation-Pfade im Shader → Pixel-Vergleich mit den UI-Widget-Tests (`tests/test_ui_widgets.cpp`, Software-Raster-Dump) und den GL-Thumbnails (`RenderWidgetThumbnail`).

### REN-604 – UI-Material-Quads: pro Quad Auflösung, UBO-Uploads und (bei „Frosted“) komplette Framebuffer-Kopie + Mipmaps
- **ID**: REN-604
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:7846-7871` (`snapshotBackdrop`), `7873-7975` (Material-Zweig), `3997-4011` (`GetOrBuildUIMaterialProgram`)
- **Problem**: (a) `backdropStale` wird nach *jedem* Quad wieder `true` (7973, 8026); jedes folgende Quad, dessen Fragment `heBackdrop` enthält, löst `glCopyTexSubImage2D` über das ganze Ziel + `glGenerateMipmap` aus → N Frosted-Elemente = N Vollbild-Kopien mit Mip-Kette pro Frame. (b) Pro Material-Quad: `GetOrBuildUIMaterialProgram` → `resolveMaterialShader` (GLSL-Kopie + Hash, siehe REN-602) + `fragGlsl.find("heBackdrop")` über die gesamte Quelle (Zeile 4008, vor dem Memo-Lookup!) + `EnsureMaterialUBOs`; danach `getMaterial`, `padded[64]`-Memcpy, 3 UBO-`glBufferSubData` (U, HeUI, HeParams), pro Textur ein `std::string gp`-Copy + `ResolveGraphTexture` mit String-Schlüssel (REN-607), `matGlslTextureArrayMask`. (c) Die Lighting-UBO wird dagegen korrekt einmal pro Pass hochgeladen (7901).
- **Code-Auszug**:
```cpp
if (usesBackdrop) *usesBackdrop = fragGlsl.find("heBackdrop") != std::string::npos;  // vor m_uiMaterialPrograms.find
...
glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, pw, ph);
glGenerateMipmap(GL_TEXTURE_2D);
```
- **Lösung**: (a) Snapshot-Bedarf nur neu nehmen, wenn seit dem letzten Snapshot tatsächlich etwas *unter* dem Rect gezeichnet wurde (Dirty-Rect-Schnitt) bzw. mehrere Frosted-Quads ohne dazwischenliegende Zeichnung teilen sich den Snapshot; Mips nur bis zur nötigen Blur-Stufe. (b) `ResolvedUIMaterial {program, usesBackdrop, arrMask}` in `m_uiMaterialPrograms` ablegen (statt nur `unsigned int`), Auflösung nur bei Miss; `usesBackdrop` beim Bau berechnen. (c) `paramData` + Textur-IDs pro Material-ID einmal pro Pass vorbereiten.
- **Erwarteter Effekt**: (Schätzung) (a) bei 5 Frosted-Panels 5 × (Vollbild-Copy + Mip-Chain) → 1–2; (b) pro Material-Quad ~10–50 µs CPU.
- **Risiko / Verifikation**: (a) ändert das Aussehen nur, wenn „stale“ bewusst Überlagerungen abbilden soll (Kommentar: Dialog über Panel blurt das Panel) → Dirty-Rect-Logik muss genau diese Reihenfolge erhalten. Visueller Test mit Dialog-über-Panel-Szene.

### REN-605 – Schattenpass: pro Cascade und pro lokaler Ebene vollständiger Cull + Sort, keine Wiederverwendung statischer Ebenen
- **ID**: REN-605
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:11023-11083` (`renderDepthLayer`), `11085-11098`
- **Problem**: `renderDepthLayer` ruft pro Layer `m_culler.cull(...)`, `m_sorter.sort(...)` und `batchDepthCasters(...)` über die gesamte `RenderWorld` auf: 3 Cascades + bis zu 16 lokale Ebenen (Punktlicht = 6 Ebenen) = bis zu 19 Cull+Sort-Durchläufe plus 19 Depth-Renderings pro Frame — auch wenn Licht und alle Caster in der Ebene unverändert sind (typisch: statische Lampen in statischer Geometrie). Dazu kommt das Hochladen der Instanz-Matrizen je Batch/Ebene (siehe REN-608).
- **Code-Auszug**:
```cpp
glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, target, 0, layer);
glClear(GL_DEPTH_BUFFER_BIT);
m_culler.cull(m_renderWorld, vp, m_shadowVisible);
m_sorter.sort(m_renderWorld, m_shadowVisible, m_shadowSorted);
```
- **Lösung**: (1) Einmal pro Frame nach Mesh-ID vorsortierte Caster-Liste (nur `castsShadow`) erzeugen; pro Layer nur noch frustum-filtern (stabiler Filter ⇒ Reihenfolge bleibt sortiert, kein Re-Sort). (2) Für lokale Ebenen einen Hash aus (Layer-ViewProj, Transform-Version/Hash der Caster im Frustum) halten; unveränderte Ebenen nicht neu rendern (Atlas-Layer bleibt erhalten, solange kein anderer Pass ihn überschreibt). (3) Cascades mit unverändertem Light-VP (Licht und Kamera statisch) analog überspringen.
- **Erwarteter Effekt**: (Schätzung) bei Szenen mit vielen statischen Punktlichtern kann der Schattenpass um >50 % schrumpfen; Cull/Sort-CPU-Anteil sinkt auf 1× pro Frame.
- **Risiko / Verifikation**: Cache-Invalidierung muss jede Caster-Änderung (Transform, Mesh-Wechsel, Sichtbarkeit, Skinned-Posen) und Atlas-Resize (`m_shadowSizeDirty`) erfassen; Fehler zeigen sich als „eingefrorene“ Schatten. Messen (siehe unten) vor Umsetzung; Test: bewegter Caster im statischen Licht, Licht bewegt, Licht an/aus.

### REN-606 – Transparenz-Sammlung: `std::vector<float>`-Kopie pro Instanz, Sort über große Structs, kein `reserve`
- **ID**: REN-606
- **Kategorie**: C/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:11569-11589` (`TPDraw`), `11794-11811`, `11854-11869`, `12039-12056`, `12733-12734`, `12715-12722`
- **Problem**: `TPDraw` (≈250 Byte: 2 × mat4 + Farbe + Skalare + Pointer + `std::vector<float> params` + `gtex[4]`) wird für *jede Instanz* eines transparenten Batches (z. B. Partikel/Foliage) erzeugt, und jedes Mal wird `tp.params = mParams` kopiert (Heap-Alloc pro Instanz). Die Vektoren `transparent`/`deferredForward` werden pro Frame neu angelegt, ohne `reserve`. Danach sortiert `std::sort` die Structs *by value* (Moves von 250 B + vector-Move).
- **Code-Auszug**:
```cpp
auto pushTP = [&](const glm::mat4& t) {
    TPDraw tp{ viewProj * t, t, baseColor, ... backToFrontKey(t, camPos) };
    tp.params  = tpParams;        // Heap-Kopie pro Instanz
    transparent.push_back(std::move(tp)); };
...
std::sort(transparent.begin(), transparent.end(), [](const TPDraw& a, const TPDraw& b){ return a.distSq > b.distSq; });
```
- **Lösung**: `params` pro DC einmal in einen frame-lokalen Arena-Vektor (`std::vector<float> paramPool`) schreiben und im `TPDraw` nur `uint32_t paramOffset, paramCount` halten. `transparent`/`deferredForward` als Member (Kapazität bleibt) mit `clear()` pro Frame. Sortierung über `std::vector<uint32_t>` Indizes + Key-Array (`{distSq, index}`), anschließend in Reihenfolge abarbeiten.
- **Erwarteter Effekt**: (Schätzung) bei 10k transparenten Instanzen: 10k Heap-Allokationen/Frame entfallen, Sort verschiebt 16 B statt 250 B pro Swap.
- **Risiko / Verifikation**: Sortierstabilität: `std::sort` ist nicht stabil – bei gleichem `distSq` kann die Reihenfolge variieren; mit Index-Tie-Break wird sie deterministisch (leichte, harmlose Änderung). Bildvergleich mit Partikel-Szene.

### REN-607 – Textur-Caches mit `std::string`-Schlüsseln und `to_string`-Bau im Draw-Pfad
- **ID**: REN-607
- **Kategorie**: D/C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:8824-8844` (`ResolveGraphTexture`), `8860-8870` (`ResolveUITexture`), `8848-8851` (`BindGraphTexture`); Aufrufer u. a. `11783`, `11792`, `11852`, `11909`, `12029`, `12037`, `12172`, `12199`, `5998`, `7960`, `7998`, `8294`; Typen `OpenGLRenderer.h:640-644`
- **Problem**: Bei jedem Aufruf (pro Draw: bis zu 4 Graph-Texturen + 1 Weightmap auf Unit 13; pro UI-Quad mit Textur; pro GI-Landscape/Frame) wird `std::to_string(id.hi) + ":" + std::to_string(id.lo)` (+ `"#arr"`) gebaut – Strings > 15 Zeichen ⇒ Heap-Alloc – und in `unordered_map<std::string,…>` nachgeschlagen. `BindGraphTexture` macht zusätzlich einen `unordered_set<unsigned>::count` pro Bind. Das ist reiner Overhead, da die IDs (`HE::UUID`) bereits als Hash-Key taugen (`m_meshCache` nutzt sie).
- **Code-Auszug**:
```cpp
std::string key = id != HE::UUID{} ? (std::to_string(id.hi) + ":" + std::to_string(id.lo)) : path;
if (array) key += "#arr";
if (auto it = m_graphTexCache.find(key); it != m_graphTexCache.end()) ...
```
- **Lösung**: Zwei Maps: `unordered_map<UUID, GlTex>` (+ zweite für Array-Variante oder Schlüssel `{UUID, bool}`) für ID-Zugriffe, `unordered_map<std::string, GlTex>` nur für Pfad-Fallback (ID leer). `GlTex = {GLuint id; GLenum target;}` – dann entfällt `m_glArrayTex`. Invalidierung (`m_pendingTexInvalidations`, 10826-10837) wird dadurch ebenfalls einfacher (kein String-Bau).
- **Erwarteter Effekt**: (Schätzung) pro Graph-Material-Draw bis zu 5 Heap-Allokationen + 5 String-Hashes weniger; in UI-Szenen pro Image-Quad 1.
- **Risiko / Verifikation**: Invalidierungslogik anpassen (zwei Caches, `#arr`); Test: Landscape-Weightmap-Paint (re-upload), Texture-Array-Material.

### REN-608 – Instanz-Matrizen werden pro Pass und Batch wiederholt in denselben `m_instanceVBO` hochgeladen
- **ID**: REN-608
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:6500-6506` (GI-Prepass), `7317-7323` (SSAO), `11046-11051` (Schatten, je Layer/Batch), `11822-11826` (G-Buffer), `12085-12089` (Szene)
- **Problem**: Dieselben `dc.instanceTransforms` eines GeometryPass-Batches werden pro Frame bis zu 4–5× per `glBufferData(…GL_STREAM_DRAW)` in einen *einzigen* gemeinsamen VBO geschrieben (GI-Prepass, SSAO-/Reflection-Prepass, Szene; Schatten zusätzlich je Cascade und lokalem Layer). Jeder Upload orphaned den Puffer; auf manchen Treibern bedeutet das Allokation + Kopie + ggf. Sync auf vorherige Draws.
- **Code-Auszug**:
```cpp
glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
glBufferData(GL_ARRAY_BUFFER, dc.instanceTransforms.size() * sizeof(glm::mat4), dc.instanceTransforms.data(), GL_STREAM_DRAW);
```
- **Lösung**: Einmal pro Frame alle Batch-Transformationen der `CommandBuffer` in einen großen Puffer packen (ein `glBufferData`/`glBufferSubData`, Offsets pro DC merken) und pro Draw nur die vier `glVertexAttribPointer(4..7, …, offset)` auf den DC-Offset setzen (GL 4.1-kompatibel; `BaseInstance` erst ab 4.2). Schatten-Batches analog mit eigener Offsets-Tabelle.
- **Erwarteter Effekt**: (Schätzung) 3–4 Orphan-Uploads weniger pro Batch und Frame; Treiberabhängig, daher messen.
- **Risiko / Verifikation**: Alle VAOs haben die Attribute auf `m_instanceVBO` gebunden (8637-8649) – `glVertexAttribPointer` pro Draw ändert VAO-State; Test mit GeometryPass-Batches (Foliage-Szene), Bildvergleich in Schatten/GI/SSAO.

### REN-609 – Skinned-Draws laden pro Draw immer 128 Bone-Matrizen (8 KB) und füllen vorher den Scratch neu mit Identität
- **ID**: REN-609
- **Kategorie**: C/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:12576-12594`
- **Problem**: Pro Skinned-DrawCall: `std::fill` von 128 `mat4` (8 KB), `copy_n` der echten Matrizen und `glUniformMatrix4fv(m_uSkinnedBones, 128, …)`. Ein typischer Charakter hat 20–80 Joints; bei 50 Charakteren sind das 400 KB Uniform-Upload und 400 KB Fill pro Frame. Skinned-Draws werden außerdem nie instanziert/gebatcht.
- **Code-Auszug**:
```cpp
std::fill(boneScratch.begin(), boneScratch.end(), glm::mat4(1.0f));
... std::copy_n(dc.boneMatrices.begin(), boneCount, boneScratch.begin());
glUniformMatrix4fv(m_uSkinnedBones, kMaxBones, GL_FALSE, glm::value_ptr(boneScratch[0]));
```
- **Lösung**: Nur `boneCount` Matrizen direkt aus `dc.boneMatrices` hochladen (`glUniformMatrix4fv(loc, boneCount, …, dc.boneMatrices.data())`), Identität einmalig beim Programmbau in die übrigen Slots schreiben (Uniform-Werte persistieren pro Programm) – dann ist weder Fill noch Scratch nötig. Mittelfristig: Bone-Matrizen in UBO/Textur.
- **Erwarteter Effekt**: (Schätzung) Upload-Volumen proportional zu echten Joints statt 8 KB; Wegfall des 8-KB-Fill pro Draw.
- **Risiko / Verifikation**: Slots ≥ `boneCount` enthalten dann Werte eines früheren Draws, falls ein Vertex Bone-IDs ≥ `boneCount` referenziert (der Scratch deckte das bisher mit Identität ab, Kommentar 12578). Vorher sicherstellen (Importer/Test), dass `max(boneID) < boneCount`; sonst Slots bis `maxBoneId+1` mit Identität auffüllen. Test: Skeletal-Animations-/Bone-Overlay-Tests.

### REN-610 – `UpdateGIAccel`: pro Frame `glm::inverse` je Objekt, Surface-Lookup je Objekt und komplette SSBO-Neuladung (Instanzen + Landscapes)
- **ID**: REN-610
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:5959-5983`, `5989-6020`, `6040-6043`
- **Problem**: Bei aktivem GI/GI-Reflections: für jedes Objekt jeden Frame `glm::inverse(obj.transform)` (volle 4×4-Inverse), `HE::giInstanceSurface(obj, m_contentManager)` (Material-Lookup), anschließend `glBufferData` des *gesamten* Instanz-SSBO, auch wenn sich nichts bewegt hat. Der Landscape-SSBO wird jeden Frame neu allokiert und befüllt (`std::vector<GILandGpu> lands` pro Frame, `ResolveGraphTexture` je Landscape → String-Key, siehe REN-607), obwohl sich Landscapes selten ändern.
- **Code-Auszug**:
```cpp
inst.invTransform = glm::inverse(obj.transform);
const HE::GiInstanceSurface surf = HE::giInstanceSurface(obj, m_contentManager);
...
glBufferData(GL_SHADER_STORAGE_BUFFER, m_giInstancesCpu.size() * sizeof(GIInstanceGpu), m_giInstancesCpu.data(), GL_DYNAMIC_DRAW);
```
- **Lösung**: (1) Affine Inverse (Rotation/Skalierung transponieren/invertieren) statt `glm::inverse`, oder Inverse nur neu berechnen, wenn sich `obj.transform` gegenüber dem Vorframe geändert hat (Cache `entityId → {transform, inv}`). (2) Surface-Daten pro `(meshId, materialId)` cachen. (3) Instanz-SSBO nur hochladen, wenn sich Hash der Instanzliste geändert hat; sonst nur geänderte Bereiche per `glBufferSubData`. (4) Landscape-SSBO nur bei Änderung der `landscapes`-Liste/Texturen.
- **Erwarteter Effekt**: (Schätzung) bei 5k Objekten ~0.3–1 ms CPU/Frame und der SSBO-Upload (5k × 112 B = 560 KB) in statischen Szenen eliminiert.
- **Risiko / Verifikation**: Cache-Invalidierung bei Material-/Mesh-Änderung. GI-Verhalten muss bytegleich bleiben → vorhandener `tests/test_gi_bvh.cpp` deckt nur Traversal; zusätzlicher Vergleichstest der Instanzliste (alt vs neu) empfohlen.

### REN-611 – BLAS-Aufbau synchron auf dem Renderthread, komplette Neu-Upload-Kaskade, keine Eviction
- **ID**: REN-611
- **Kategorie**: E/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:5911-5939` (`BuildGIBlas`), `5952-5958`, `6028-6039`, `10811-10817`
- **Problem**: Die erste Verwendung eines Meshes im GI-Modus baut den BVH komplett auf dem Renderthread (`HE::buildGiBvh`) – bei großen Meshes ein Frame-Hitch. Jede neu angehängte BLAS setzt `m_giBlasDirty` und `glBufferData` lädt anschließend *alle* Knoten und Dreiecke aller Meshes erneut hoch (O(Gesamtgröße) pro neu gesehenem Mesh). `m_giBlasCache` wird nie bereinigt, wenn Meshes entladen werden (nur bei `InvalidateMesh` wird *alles* gelöscht und alle BVHs werden neu gebaut, 10811-10817).
- **Code-Auszug**:
```cpp
if (m_giBlasCache.count(id)) { m_giBlasCache.clear(); m_giNodesCpu.clear(); m_giTrisCpu.clear(); m_giBlasDirty = true; }
```
- **Lösung**: BVH-Aufbau in den Job-System-Worker verlagern (vorhandenes `JobSystem`), bis zur Fertigstellung das Objekt aus der GI-Instanzliste ausnehmen (Cube-Fallback existiert bereits). SSBO mit `glBufferSubData` am Ende erweitern (Kapazitätsverdopplung) statt Neu-Upload. Pro Mesh invalidieren (`free list`/Kompaktierung) statt alles zu verwerfen.
- **Erwarteter Effekt**: (Schätzung) Hitch beim Laden GI-aktiver Szenen von Frames-Spitzen (10–100 ms bei Millionen Dreiecken) auf Hintergrundarbeit.
- **Risiko / Verifikation**: Asynchronität ändert die erste Sichtbarkeit (Schatten/Reflexionen erscheinen verzögert). Messen; Tests: `tests/test_gi_bvh.cpp` + Szene mit nachgeladenen Meshes.

### REN-612 – `glGetUniformLocation` mit String-Literal pro Frame in GI-, Partikel- und Graph-Pfaden
- **ID**: REN-612
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:6471-6476`, `6563-6635` (GI-Shadow, ≈14 Lookups), `6724-6786` (`loc`-Lambda ≈ 25 Lookups + 4 × `"uLandWeights[" + to_string(i) + "]"`), `6947-6970` (≈ 12), `13273-13276` (4 pro Partikel-Batch), `13213`; insgesamt 298 `glGetUniformLocation(..., "…")` in der Datei
- **Problem**: Der Programm-Locations-Cache existiert für Szene/Sky/Post (Member `m_u…`), aber nicht für die GI-Compute/Temporal-/Atrous-/Refl-Programme und die ParticleGraph-Programme: dort wird pro Frame (bzw. pro Emitter) per Namen nachgeschlagen (Treiber hasht den String; `std::string`-Konstruktion für die Landscape-Samplernamen).
- **Code-Auszug**:
```cpp
auto loc = [&](const char* n) { return glGetUniformLocation(m_giReflCSProgram, n); };
glUniform4f(loc("uCamPos"), ...); ... 
const std::string nm = "uLandWeights[" + std::to_string(i) + "]";
```
- **Lösung**: Je Programm eine kleine Struct (`GiReflLocs`, `GiShadowLocs`, `GiProbeLocs`, `GiTemporalLocs`, …) in `CreateGIPipelines()` einmal füllen (wie bereits `FetchGISceneLocs`); für Partikel-Programme die Locations im Programm-Cache (`m_particlePrograms`) mitführen (`struct {GLuint prog; GLint viewProj, camRight, camUp, hasTex;}`).
- **Erwarteter Effekt**: (Schätzung) ≈ 60 String-Lookups pro Frame bei aktivem GI entfallen (je 0,2–1 µs, treiberabhängig).
- **Risiko / Verifikation**: Rein mechanisch; Locations ändern sich nach Relink nicht. Kein Verhaltensunterschied.

### REN-613 – `DrawSkyFullscreen` setzt ≈ 60 Uniforms + `getenv` je Aufruf und wird für die Reflection-Cube 6× pro Frame aufgerufen
- **ID**: REN-613
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:10665-10775`, Aufrufer `6692-6701` (`RenderSkyReflCube`), `10649`, `9734`; `getenv` in `10711` und `5076`
- **Problem**: Bei aktivem GI-Reflection-Pfad rendert `RenderSkyReflCube` jedes Frame 6 Cube-Faces (je 128², volle Wolken-/Sky-Shader) und jede Face ruft `DrawSkyFullscreen` mit allen ≈ 60 `glUniform*` neu auf (360 Uniform-Calls). Nur `uInvViewProj` unterscheidet die Faces. Zusätzlich liest `DrawSkyFullscreen` (und `RenderCloudShadowMap`) jedes Frame `std::getenv("HE_SKY_TIME")` + `atof`. Der Bake hat keine Änderungserkennung (Sonne/Wolken/Kamera).
- **Code-Auszug**:
```cpp
float skyClock = static_cast<float>(SDL_GetTicks()) / 1000.0f;
if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov) skyClock = static_cast<float>(std::atof(ov));
```
- **Lösung**: `DrawSkyFullscreen` in `SetSkyUniforms(env, sunDir, camPos, clock)` + `DrawSkyPass(invViewProj)` teilen; die Cube-Faces rufen `SetSkyUniforms` einmal und dann 6× nur `glUniformMatrix4fv`. `HE_SKY_TIME` einmal in einer `static const` auswerten (wie `s_timeOv` bei 5659/11608 bereits). Optional: Cube nur neu rendern, wenn sich Sonnenrichtung/Wolkenzeit/Kameraposition über Schwelle geändert haben.
- **Erwarteter Effekt**: (Schätzung) ≈ 300 Uniform-Calls + 6 `getenv` weniger pro Frame (nur bei GI-Reflections); Cube-Bake optional 1/N der Frames.
- **Risiko / Verifikation**: Nur die Teilung ist verhaltensgleich. Das Überspringen des Bakes ändert Latenz der Wolkenspiegelung → bewusst entscheiden (Visueller Vergleich).

### REN-614 – Totes deaktiviertes Reflection-Temporal (`if (false && …)`) hält Shader, Programm und 4 RGBA16F-Texturen vor
- **ID**: REN-614
- **Kategorie**: A/F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:6803-6837`, `6296-6313`, `2728-2779` (`kGiReflTemporalFS`), `6163-6164`, `6838`
- **Problem**: Der Temporal-Zweig ist mit `if (false && …)` und `const float blend = 0.0f; (void)camMoved;` deaktiviert („one constant to undo“). Trotzdem werden `kGiReflTemporalFS` bei jedem GI-Start kompiliert/gelinkt, `m_giReflHistTex[2]` + `m_giReflHistPosTex[2]` (4 × RGBA16F im Prepass-Format, halbe Auflösung) + 2 MRT-FBOs alloziert und per Clear initialisiert, und `m_giReflPrevViewProj` wird pro Frame geschrieben. Der Code enthält außerdem eine nie erreichte `camMoved`-Berechnung.
- **Code-Auszug**:
```cpp
if (false && m_giReflQuality >= 2 && m_giReflTemporalProgram && m_giReflHistFBO[0]) { ...
    const bool camMoved = (viewProj != m_giReflPrevViewProj);
    const float blend = 0.0f;
    (void)camMoved;
```
- **Lösung**: Entweder entfernen (Programm, Shader-String, History-Texturen, FBOs, `m_giReflHistIdx/Valid/PrevViewProj`, Zweig) und den Rückbau-Hinweis in `docs/gi-reflections-plan.md` verlinken; oder hinter eine `constexpr bool kGiReflTemporal = false;` stellen und Ressourcen nur dann anlegen. 
- **Erwarteter Effekt**: ≈ 4 × (w/2 × h/2 × 8 B) VRAM gespart (bei 1080p ≈ 16 MB), ≈ 60 Zeilen Code und 1 Shader-Kompilierung weniger.
- **Risiko / Verifikation**: Keine Bildänderung (Zweig nie aktiv). Kompilieren + GI-Reflection-Headless-Dump vorher/nachher.

### REN-615 – Wetter-Partikel: Simulation und Draw laufen immer über den vollen Pool, auch wenn nur `coverage` × Pool aktiv ist
- **ID**: REN-615
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:12969-12995` (`alive = step(iSeed, coverage)`), `13147-13181` (`SimulateGpuParticles`), `13183-13206` (`DrawGpuParticles`, `glDrawArraysInstanced(…, count)`); Seed: `WeatherParticleSeed.h` (`seed = (i + 0.5f) / count`)
- **Problem**: Slots mit `seed > coverage` werden nur „geparkt“ (`pos.y = -100000`, `life = -1`), aber trotzdem jedes Frame per Transform Feedback simuliert (`count` Punkte) und als `count` Instanzen × 4 Vertices gezeichnet; der Vertex-Shader verwirft sie erst (`gl_Position = vec4(2,…)`). Bei Pool bis 1 000 000 und leichtem Regen (coverage 0.1) laufen 90 % der Arbeit für nichts. Da `seed` monoton in `i` steigt, sind aktive Slots exakt die ersten `ceil(coverage × count)`.
- **Code-Auszug**:
```glsl
float alive = step(iSeed, coverage);
...
else { life = -1.0; pos = camPos + vec3(0.0, -100000.0, 0.0); vel = vec3(0.0); }
```
- **Lösung**: `activeCount = min(count, ceil(coverage * count))` berechnen; `glDrawArrays(GL_POINTS, 0, activeCount)` in der Simulation und `glDrawArraysInstanced(…, activeCount)` im Draw verwenden. Beim Erhöhen von `coverage` müssen neu aktivierte Slots entweder ihr Parken-Reset behalten (Slot wurde beim letzten Aktivsein in unbekanntem Zustand eingefroren) → beim Wechsel der `activeCount`-Obergrenze einmal `life <= 0` für die Differenz setzen (Respawn) oder Pool nur neu seeden.
- **Erwarteter Effekt**: (Schätzung) bei coverage 0.1–0.3: 70–90 % weniger Vertex-Arbeit im Wetter-Pass.
- **Risiko / Verifikation**: Verhalten bleibt gleich, solange eingefrorene Slots beim Reaktivieren respawnen. Das Verhalten beim Hochfahren der Dichte (Plop-Effekt) muss verglichen werden; Metal-Zwilling `HE::SeedWeatherParticles`-Layout identisch halten. Prüfen: `GpuParticleParams.count` enthält coverage evtl. schon (siehe „Unsicher“).

### REN-616 – Sky-Environment-Cube: 1,5-MB-Vector pro Neubau, 6×128²×RGBA32F, Thread-Pool-Stall auf dem Renderthread
- **ID**: REN-616
- **Kategorie**: C/E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:64-81` (`BuildSkyEnvCube`), `3437`, `4226-4244` (`UpdateSkyEnvCube`), `3546-3558`
- **Problem**: Bei jeder Sonnenbewegung > 2e-3 (unter Tag/Nacht-Automatik ≈ jedes zweite Frame) wird auf dem Renderthread ein neuer `std::vector<float>` mit 6 × 128 × 128 × 4 Floats (1,5 MB) alloziert, per `parallel_for` befüllt (laut Kommentar „a few ms“, der Renderthread wartet auf den Pool) und als RGBA32F-Cube hochgeladen. IBL-Ambient ist niederfrequent (Kommentar 4228-4231), 128² RGBA32F (6 × 256 KB = 1,5 MB Upload) ist dafür deutlich überdimensioniert.
- **Code-Auszug**:
```cpp
const std::vector<float> px = BuildSkyEnvCube(kSkyEnvFace, sunDir);
for (int f = 0; f < 6; ++f) glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, kSkyEnvFace, kSkyEnvFace, GL_RGBA, GL_FLOAT, px.data() + f * faceFloats);
```
- **Lösung**: (1) Puffer als Member wiederverwenden (kein Alloc). (2) Bake asynchron als Job starten und im nächsten Frame hochladen (1 Frame Latenz bei langsamer Sonnenbewegung unsichtbar). (3) RGBA16F statt RGBA32F (halbe Bandbreite) und/oder 64² (4× weniger Texel); Auswirkung auf Ambient-Qualität vergleichen. Metal bäckt pro Face on-demand (Zeile 61-63), GL könnte pro Frame nur 1–2 Faces rotierend aktualisieren.
- **Erwarteter Effekt**: (Schätzung) 1,5 MB Alloc + Wartezeit des Renderthreads (ms) pro zweitem Frame entfallen.
- **Risiko / Verifikation**: Auflösung/Format ändern minimal das Ambient; für exakte Bildgleichheit nur (1) und (2). Vergleich per Headless-Dump bei mehreren Sonnenständen.

### REN-617 – Mehrere unabhängige O(N)-Durchläufe über alle Objekte pro Frame
- **ID**: REN-617
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:10918-10920` (`ResolveMesh` + `refineWorldBounds` für jedes Objekt), `5033` (Szenen-AABB nur für Cloud-Shadows), `6366-6371` (`GIProbeSceneSignature` / Refine), `5959-5981` (GI-Instanzen), `4747-4788` (Velocity: `unordered_map`-Einfügen pro Entity und Frame)
- **Problem**: Pro Frame läuft mindestens ein Durchlauf über `m_renderWorld.objects` für: Bounds-Refinement (Hash-Lookup `m_meshCache` je Objekt), Cloud-Shadow-Szenenbox (wenn Cloud-Shadows an), GI-Signatur (wenn GI an), GI-Instanzen (wenn GI an), TAA-Velocity-History (`m_taaCurTransforms[obj.entityId] = …` ⇒ Node-Allokation pro Entity/Frame, `swap` mit Vorframe). Die Refinement-Schleife kann außerdem das *synchrone Hochladen neuer Meshes* auslösen (REN-625).
- **Code-Auszug**:
```cpp
for (RenderObject& obj : m_renderWorld.objects)
    if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId)) obj.refineWorldBounds(mesh->localBounds);
...
for (const RenderObject& o : m_renderWorld.objects) sceneBox.expand(o.worldBounds);
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: `localBounds` einmal pro Mesh-Asset in `RenderObject`/Extractor beim Erzeugen setzen (Extractor kennt `ContentManager`) und die Refine-Schleife auf neu geladene Meshes beschränken; Szenenbox einmal pro Frame im Extractor berechnen und von Cloud-Shadows/GI-Grid wiederverwenden; Velocity-History als `std::vector<glm::mat4>` indiziert über einen stabilen Slot (Entity-Index) oder ein flat-hash-Map (keine Node-Allokation).
- **Erwarteter Effekt**: (Schätzung) bei 10k Objekten 3–5 Hash-Lookups/Objekt/Frame und 10k Node-Allokationen (TAA) entfallen.
- **Risiko / Verifikation**: `refineWorldBounds` hängt von der Auflösung des GPU-Meshs ab (Kommentar 6358); Extractor kennt die Bounds evtl. erst nach Upload → Refine bei Mesh-Auflösung mit Version/Flag. Test: Culling-Tests, TAA-Velocity bei Objekt-Spawn.

### REN-618 – Graph-Material-Draws binden pro Draw Frame-konstante Texturen neu und laden Objekt-UBO pro Instanz
- **ID**: REN-618
- **Kategorie**: B/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:12127-12201`, `12204-12221`, `11879-11934`, `12801-12809`
- **Problem**: Im Material-Zweig werden pro Draw die Units 14 (Sky-Cube), 15 (AO), 16/17 (DDGI) neu gebunden (`glActiveTexture`/`glBindTexture` ×8), obwohl sie pro Frame konstant sind und „only ever used by these material programs“ (Kommentar 12176-12179). Außerdem Unit 13 (Weightmap) per Draw mit String-Lookup (REN-607), `glBindBufferBase` ×3 und – bei Instanz-Batches – pro Instanz ein eigener `glBufferSubData` des 176-B-Objekt-UBO + ein `glDrawElements` (die Instanzierung wird für Graph-Materialien bewusst ausgelassen, 12081).
- **Code-Auszug**:
```cpp
glActiveTexture(GL_TEXTURE14); glBindTexture(GL_TEXTURE_CUBE_MAP, m_skyEnvCube);
glActiveTexture(GL_TEXTURE15); glBindTexture(GL_TEXTURE_2D, aoActive ? aoTex : m_whiteTex);
...
glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(obj), &obj); glDrawElements(...);   // pro Instanz
```
- **Lösung**: Units 14–17 (und die Lighting-/Param-`glBindBufferBase`) einmal vor der Draw-Schleife binden (und nach der Transparenz-Sammlung nur wenn eine andere Stelle sie überschreibt – laut Kommentar verwenden nur Material-Programme diese Units). Für Instanz-Läufe eines Graph-Materials den UBO als Array/SSBO mit `gl_InstanceID`-Variante der Standard-Vertexshader (`standardVertex`) anbieten – oder wenigstens alle Instanzen eines Batches in ein UBO-Array hochladen und mit `glDrawElementsInstanced` zeichnen (WPO/Wind braucht pro Instanz das `model`).
- **Erwarteter Effekt**: (Schätzung) ≈ 10 GL-Calls pro Material-Draw entfallen; Instanz-Läufe (Foliage mit Wind-Material) von N Draws auf 1.
- **Risiko / Verifikation**: Reihenfolge: Frame-Binds müssen nach `RenderUIPass`/Previews (die Units 5 benutzen) erneuert werden. Instanzierung erfordert Shader-Variante und Metal-Parität; zuerst nur die Bind-Hoisting-Maßnahme.

### REN-619 – GLSL-Zwillinge (DDGI-Sampling, Cone-Hash, Oct-Encode, SpecAA, Temporal-Kernel) mehrfach im selben File
- **ID**: REN-619
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:195-200`/`712-717`/`2268-2273`/`2462-2467` (octEncode ×4), `208-264` vs `2469-2525` (`sampleDDGIIrradiance`), `2018-2039` vs `2535-2556` (`giPcg3d/giHash2/giConeSample`), `150-162` vs `722-732` (`heSpecAARoughness`), `2103-2175` vs `2728-2779` (Temporal)
- **Problem**: Mehrfache wortgleiche GLSL-Funktionen, die nur über „SYNC“-Kommentare zusammengehalten werden (z. B. 2458-2461, 2527-2528, 148-149). Zusätzlich existieren Spiegel in `MetalRenderer.mm`, `D3D_Shared/HlslSources.h` und `MaterialShaderLibrary.cpp` (Preamble `heSpecAARoughness`). Der Code zeigt selbst, dass Drift die reale Fehlerquelle ist (Kommentar 2533-2534: gelöster Hash-Bug musste in jeder Kopie nachgezogen werden).
- **Code-Auszug**:
```cpp
// SYNC: byte-for-byte the scene shader's giOctEncode / sampleDDGIIrradiance (kUnlitFS above)
```
- **Lösung**: Wie bei `kSkyFuncGLSL` / `injectSkyFunc` gemeinsame Snippet-Strings (`kGiOctGLSL`, `kDdgiSampleGLSL`, `kGiConeHashGLSL`, `kSpecAAGLSL`) in einem Header (`HorizonRendering/*ShaderSource.h`) halten und per Marker/String-Konkatenation in `kUnlitFS`, `kGBufFS`, `kGiReflCS`, `kGiShadowCS` einsetzen (GLSL hat kein `#include`, `injectSkyFunc` ist das vorhandene Muster). Für Cross-Backend (Vulkan/D3D über `kSkyFS`) dasselbe Snippet-Header nutzen.
- **Erwarteter Effekt**: ≈ 150 Zeilen GLSL weniger, Wegfall der „SYNC“-Pflege im GL-File; keine Laufzeitänderung.
- **Risiko / Verifikation**: `scripts/validate_embedded_shaders.py` (Kommentar 3288-3290) muss die zusammengesetzten Texte weiterhin bauen; Shader-Compile auf GL 4.10 (Mac) und 4.30 prüfen.

### REN-620 – 38 × `glCreateProgram…glLinkProgram`-Boilerplate in 8+ Varianten, teilweise ohne Fehlerprüfung
- **ID**: REN-620
- **Kategorie**: A/F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:3465-3473`, `3893-3898`, `3964-3971`, `4250-4256`, `4329-4336`, `4426-4433`, `4451-4458`, `4473-4480`, `4495-4502`, `4519-4526`, `4591-4612`, `4846-4871` (Bloom: **kein** Link-Check), `5141-5160`, `5319-5338`, `5592-5599`, `5700-5781`, `6124-6155`, `7010-7028`, `7448-7472`, `8172-8188`, `8337-8355`, `9099-9105`, `9397-9422`, `9873-9879`, `10112-10126` (Debug-Lines: **weder Compile- noch Link-Check**), `13071-13109`
- **Problem**: Jede Pipeline-Funktion definiert ihr eigenes `link`-Lambda oder wiederholt die 8 Zeilen (create/attach/link/delete shaders/status/log). Fehlerbehandlung ist uneinheitlich: einige werfen (`throw std::runtime_error`), andere loggen und geben 0 zurück, Bloom (4846-4871) und Debug-Lines (10112-10126) prüfen gar nicht (Fehlschlag ⇒ stiller schwarzer Pass). Unterschiedliche `#version` (410 vs 330 beim Debug-Line-Shader).
- **Code-Auszug**:
```cpp
GLuint vs = CompileStage(GL_VERTEX_SHADER, kTonemapVS);
GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kBloomBrightFS);
m_bloomBrightProgram = glCreateProgram();
glAttachShader(...); glLinkProgram(m_bloomBrightProgram);   // kein GL_LINK_STATUS
```
- **Lösung**: Ein Datei-weiter Helfer `static GLuint LinkProgram(std::initializer_list<GLuint> stages, const char* what, bool throwOnFail)` (löscht Shader, prüft Status, loggt via `HE_LOG_ERROR(RHI, …)`), plus `LinkFullscreen(const char* fsSrc, const char* what)` für alle Programme mit `kTonemapVS`. Die lokalen Lambdas (4591, 5141, 5319, 6124, 7448, 8172) ersetzen.
- **Erwarteter Effekt**: ≈ 400 Zeilen weniger; einheitliche Fehlerbehandlung; Bloom/Debug-Line-Fehler werden sichtbar.
- **Risiko / Verifikation**: Wirf-/Log-Verhalten pro Aufrufer erhalten (Pflicht-Programme werfen, optionale geben 0). Initialisierung headless starten (`HE_HIDDEN_WINDOW=1`).

### REN-621 – Render-Target-/Textur-Anlage 17× handkopiert (156 × `glTexParameteri`, 34 × `glGenFramebuffers`)
- **ID**: REN-621
- **Kategorie**: A/F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `4560-4576`, `4664-4679`, `4899-4911`, `4935-4944`, `4967-4977`, `5204-5219`, `5369-5384`, `5627-5636`, `7128-7140`, `7158-7172`, `7526-7537`, `7597-7609`, `8044-8076`, `8095-8107`, `9158-9178`, `9329-9348`, `9455-9475`, `9680-9714`, `10042-10061`, `10537-10550`
- **Problem**: Gleiches Muster `glGenFramebuffers → glGenTextures → glTexImage2D → 4 × glTexParameteri → glFramebufferTexture2D → Completeness-Check` mit marginal unterschiedlichen Parametern (Format, Filter, Wrap, Depth-RBO oder Textur). Preview-Ziele (Material, Skeletal, Particle, Thumbnail, World) sind fünfmal identisch (FBO + RGBA8 + D24-Renderbuffer, Delete+Recreate bei Größenwechsel). Auch die Destroy-Funktionen (`DestroyBloomTargets`, `DestroyDepthOfFieldTargets`, `DestroyGIShadowTargets`, …) sind Listen manuell geführter `if (x) glDelete…`.
- **Code-Auszug**:
```cpp
glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, fmt, type, nullptr);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter); ... (4×)
glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
```
- **Lösung**: Kleine RAII-Struct `GlTarget { GLuint fbo, tex, depthRbo; int w, h; bool ensure(w, h, TargetDesc); void destroy(); }` mit `TargetDesc{ internalFmt, fmt, type, filter, wrap, withDepth }`; alle `Ensure*Target`-Funktionen darauf abbilden. `glTexStorage2D` (4.2) wird bereits bei GI genutzt, für 4.1 weiter `glTexImage2D`.
- **Erwarteter Effekt**: ≈ 500–700 Zeilen weniger; Leaks bei vergessenen Deletes (siehe REN-624) strukturell ausgeschlossen.
- **Risiko / Verifikation**: Pro Target identische Parameter beibehalten (Filter NEAREST/LINEAR, Border, sRGB). Headless-Dump jeder Pass-Kette vergleichen; Preview-Panel-UI-Tests.

### REN-622 – Uniform-Location-Listen für Unlit/Skinned/Instanced dreifach geführt (≈ 140 Member, 3 × 45 Zeilen)
- **ID**: REN-622
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:3527-3582`, `4125-4164`, `4183-4222`; `BindSceneLighting`-Aufrufe `11501-11505`, `11545-11551`, `12547-12553`; Per-Frame-Pushes `11425-11498`, `11526-11541`, `12541-12574`
- **Problem**: Die drei Programme teilen `kUnlitFS` (Kommentar 10589-10596), aber für jedes gibt es einen eigenen Satz Member (`m_uXxx`, `m_uSkinnedXxx`, `m_uInstXxx`), eine eigene `glGetUniformLocation`-Liste und einen eigenen Per-Frame-Pushblock mit Brace-Init-Listen aus 18 Locations. Neue Uniforms müssen an 6+ Stellen ergänzt werden (Drift bereits sichtbar: Skinned setzt `uWetness/uSnow/uSpecAA` nicht, Instanced schon).
- **Code-Auszug**:
```cpp
BindSceneLighting({ m_uInstLightCount, m_uInstLightPos, ..., m_uInstClusterParams, m_uInstClusterCamFwd }, shadowFrame, frameClusters);
```
- **Lösung**: `struct SceneProgramLocs { GLint mvp, model, color, ..., giLocs; SceneLightingLocs lighting; };` und `FetchSceneProgramLocs(prog)` (gleicher Locations-Namensliste für alle drei, `-1` bleibt No-Op); `PushSceneFrameUniforms(const SceneProgramLocs&, const FrameState&)` ersetzt die drei Per-Frame-Blöcke.
- **Erwarteter Effekt**: ≈ 150 Zeilen weniger, ein Ort für neue Uniforms.
- **Risiko / Verifikation**: Skinned fehlte `uWetness/uSnow/uSpecAA` bisher — Vereinheitlichung ändert das Aussehen von Skinned-Meshes bei Nässe/Schnee (vermutlich Bugfix, aber sichtbar). Entweder bewusst übernehmen oder Locations für Skinned absichtlich auslassen; Test mit Wetter-Szene.

### REN-623 – Vier Kopien derselben Draw-Call-Schleife (Mesh-Resolve + Instanz/Einzel-Zweig + Section-Range)
- **ID**: REN-623
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:6479-6541` (GI-Prepass), `7295-7377` (SSAO/Refl-Prepass), `11035-11079` (Schatten), `11720-11950` (G-Buffer), `11956-12239` (Forward), `4747-4782` (Velocity)
- **Problem**: Jede Schleife wiederholt: `ResolveMesh` mit „last id“-Memo, Fallback auf `kDefaultCubeMeshId`, `DrawIndexRange`, Instanzen-Zweig mit `glBufferData` + Programmwechsel-Tracking (`boundProgram`), Einzel-Zweig mit `pushDraw` und `static bool loggedOnce`. Änderungen am Instanz-Pfad (z. B. REN-608) müssen an 5 Stellen erfolgen.
- **Code-Auszug**:
```cpp
if (!valid || dc.meshAssetId != lastId) { cMesh = ResolveMesh(dc.meshAssetId); lastId = dc.meshAssetId; valid = true; }
const GpuMesh* mesh = cMesh ? cMesh : ResolveMesh(HE::kDefaultCubeMeshId);
```
- **Lösung**: `template<class OnInstanced, class OnSingle> void ForEachGeometryDraw(const CommandBuffer&, Filter, OnInstanced, OnSingle)` bzw. kleine Klasse `DrawIterator`, die Mesh-Memo, Fallback-Cube, `DrawIndexRange` und die Instanz-Aufteilung kapselt; Pässe liefern nur ihre Programm-/Uniform-Logik.
- **Erwarteter Effekt**: ≈ 150 Zeilen weniger; ein Ort für Instanz-Upload-Optimierung (REN-608) und Material-Memo (REN-602).
- **Risiko / Verifikation**: Templates im Hot Path inlinen; per Bildvergleich je Pass (GI, SSAO, Schatten) absichern.

### REN-624 – `Shutdown()` lässt Programme/Targets aus und setzt „Tried“-Flags nicht zurück (stale Handles bei Re-Initialize)
- **ID**: REN-624
- **Kategorie**: C/F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:10220-10388`; fehlend gegenüber `5584-5643`, `6118-6189`, `9392-9435`, `9437-9592`, `9869-9905`, `10032-10089`
- **Problem**: `Shutdown()` löscht nicht: `m_ucProgram` (Underwater-Caustics-Programm; `m_ucFBO/m_ucColor` werden nur in `EnsureUnderwaterCausticsTarget` beim Resize gelöscht), `m_skelPreviewProgram`, `m_skelPreviewLineProgram`, `m_skelPreviewLineVAO/VBO`, `m_skelPreviewFBO/Color/Depth`, `m_particlePreviewProgram/VAO/InstVBO/FBO/Color/Depth`, `m_giReflCSProgram`, `m_giReflTemporalProgram`, `m_giReflBlurProgram`, `m_giReflMixProgram` (diese vier nur im Fehlerzweig von `CreateGIPipelines`). Der Kommentar bei 10323-10324 („their programs are the skeletal preview's, freed with those“) trifft nicht zu – dort wird nichts freigegeben. Die Flags `m_ucTried` und `m_giPipelinesBuilt` werden nicht zurückgesetzt, obwohl derselbe Block andere `…Tried`-Flags explizit zurücksetzt (10266-10270, „re-Initialize() gets a fresh build attempt“). Nach Shutdown→Initialize liefert `EnsureSkelPreviewPrograms()` die alte (tote) Programm-ID als „vorhanden“, `CreateGIPipelines()` baut nie neu (`m_giPipelinesBuilt == true`, GI bleibt still aus).
- **Code-Auszug**:
```cpp
if (m_skelPreviewProgram && m_skelPreviewLineProgram) return true;   // 9394, auch nach Shutdown wahr
if (m_giPipelinesBuilt) return;                                       // 6120, nie zurückgesetzt
```
- **Lösung**: Fehlende `glDelete*` ergänzen und Handles/Flags nullen (`m_ucTried = false; m_giPipelinesBuilt = false;`). Strukturell: Mit REN-621 (`GlTarget`) + einem `ProgramHandle`-RAII-Typ (Destruktor löscht) wird `Shutdown()` kürzer und vollständig.
- **Erwarteter Effekt**: Keine Leaks beim Context-Wechsel/Re-Init, keine stale IDs.
- **Risiko / Verifikation**: Nur relevant, wenn `Initialize` nach `Shutdown` erneut aufgerufen wird (Kommentar 10266-10270 sagt ja). Test: Renderer-Re-Init (Backend-Wechsel im Editor), danach GI/Skelett-Preview/Underwater aktivieren.

### REN-625 – Mesh-/Textur-Upload synchron beim ersten Draw (Refine-Schleife, Draw-Schleife, UI) ohne Budget oder PBO
- **ID**: REN-625
- **Kategorie**: E/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:8566-8670` (`ResolveMesh`), `8673-8783`, `8446-8514` (`uploadTextureAssetGL`), `8523-8561`, `10918-10920`
- **Problem**: Der erste Zugriff auf ein Mesh/eine Textur lädt im selben Frame alles hoch: Interleave-Build (`vector::insert(initializer_list)` je Vertex, 8596-8609), `glBufferData`, Textur-Upload mit allen Mips bzw. `glGenerateMipmap` für Loose-Assets (8508) – ohne Frame-Budget, ohne asynchrones PBO. Beim Laden einer Szene werden alle Meshes in der Refine-Schleife (10918) im ersten Frame hochgeladen ⇒ Frame-Spike. 4K-RGBA8-Texturen sind 64 MB je Upload.
- **Code-Auszug**:
```cpp
built.insert(built.end(), { asset->vertices[v*3+0], asset->vertices[v*3+1], asset->vertices[v*3+2] });
...
glTexImage2D(GL_TEXTURE_2D, 0, rgbaFmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex->data.data());
glGenerateMipmap(GL_TEXTURE_2D);
```
- **Lösung**: Upload-Queue mit Zeit-/Byte-Budget pro Frame (z. B. 2 ms), bis dahin Platzhalter (Default-Cube / Weiß); Interleaving mit `resize` + direkter Indizierung statt `insert`; für große Texturen PBO + `glTexSubImage2D` pro Mip. Cooked-Assets laden ohnehin direkt (ein Memcpy-Pfad).
- **Erwarteter Effekt**: (Schätzung) Szenen-Load-Spike von 100+ ms auf mehrere Frames verteilt; Interleave-Build ≈ 2–3× schneller.
- **Risiko / Verifikation**: Pop-in sichtbar (Platzhalter). Messen (siehe unten). Editor-Thumbnail-/Preview-Pfade rufen `ResolveMesh` direkt auf und erwarten sofortiges Ergebnis → dort Budget umgehen.

### REN-626 – `GetOrBuildMaterialProgram` / `setupProgram` hinterlassen `glUseProgram(0)` mitten in der Draw-Schleife
- **ID**: REN-626
- **Kategorie**: B/F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:3822-3873` (`setupProgram`), `4070-4090`, `5785-5787`, `12009-12017`, `12226-12235`, `4047-4100` + `7978`
- **Problem**: `setupProgram` bindet die neue Programm-ID (`glUseProgram(prog)`) um die Sampler-Units zu setzen und endet mit `glUseProgram(0)`. Wird ein Programm erstmals *mitten* in der Opaque-Schleife gebaut (z. B. im Transluzent-Zweig, 12017, oder im UI-Pass), ist danach kein Programm gebunden; der folgende Built-in-Einzeldraw (12226-12235) setzt Uniforms und zeichnet ohne erneutes `glUseProgram(m_unlitProgram)` (GL_INVALID_OPERATION/kein Draw für diesen einen Draw). Analog im UI-Pass: nach dem Bauen eines (fehlgeschlagenen) Material-Programms folgt der Solid-Pfad, dessen `basicBound`-Flag noch `true` ist (7976).
- **Code-Auszug**:
```cpp
glUseProgram(prog);
if (GLint l = glGetUniformLocation(prog, "heTex0"); l >= 0) glUniform1i(l, 0);
...
glUseProgram(0);
```
- **Lösung**: `glProgramUniform1i(prog, loc, unit)` (GL 4.1, `ARB_separate_shader_objects`, in Core 4.1 enthalten) statt `glUseProgram`+`glUniform1i` – lässt den aktuellen Programmzustand unberührt. Alternativ aktuelles Programm per `glGetIntegerv(GL_CURRENT_PROGRAM)` sichern/restaurieren.
- **Erwarteter Effekt**: Verhindert einen einmaligen Draw-Aussetzer beim ersten Material-Build; macht Aufrufstellen robust (weniger implizite „Programm muss neu gebunden werden“-Regeln).
- **Risiko / Verifikation**: Praktisch nur sichtbar, wenn `WarmupMaterials` das Programm nicht schon gebaut hat (unsicher, ob die Situation in Releases auftritt). `glProgramUniform*` auf macOS 4.1 verfügbar. Test: Szene mit transluzentem Graph-Material ohne Warmup, erster Frame.

### REN-627 – Startup: `kUnlitFS` wird 3× kompiliert, Built-in-Programme ohne Binary-Cache, Cache-I/O auf dem Renderthread
- **ID**: REN-627
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:3463-3512` (`LinkSceneProgram`, aufgerufen in `3516`, `4111`, `4169`), `3615-3688` (Cache), `3876-3917`
- **Problem**: Scene/Skinned/Instanced kompilieren und linken denselben großen Fragment-Shader (`injectSkyFunc(kUnlitFS)` + ggf. `SceneStageSource`) je einmal (3 volle Compiles), obwohl ein Shader-Objekt an mehrere Programme angehängt werden kann. Der vorhandene On-Disk-Programm-Cache (`glTryLoadCachedProgram`) wird nur für Graph-Materialien benutzt, nicht für Built-in-Programme (Scene, Sky, SSAO, GI, SMAA …). `glSaveCachedProgram` (Datei schreiben) und `glTryLoadCachedProgram` (Datei lesen) laufen synchron beim ersten Draw.
- **Code-Auszug**:
```cpp
GLuint vs = CompileStage(GL_VERTEX_SHADER,   vsSrc);
GLuint fs = CompileStage(GL_FRAGMENT_SHADER, injectSkyFunc(kUnlitFS).c_str());   // je Programm erneut
```
- **Lösung**: Fragment-Shader einmal kompilieren (Member-lokal) und an alle drei Programme attachen; `LinkSceneProgram`-Varianten (clustered/plain) analog. Built-in-Programme über dieselbe Cache-Funktion laufen lassen (Schlüssel: Quellen-Hash).
- **Erwarteter Effekt**: (Schätzung) Startzeit: 2 große FS-Compiles weniger (je 20–200 ms treiberabhängig); beim zweiten Start deutlich weniger Link-Zeit.
- **Risiko / Verifikation**: Shader-Objekt-Sharing ist GL-Standard. Cache-Invalidierung existiert (Device-Signatur + Quell-Hash). Startzeit messen.

### REN-628 – Shader-/Pass-Mikro-Optimierungen (niedrige Priorität, erst messen)
- **ID**: REN-628
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:334`, `415` (`tan(acos(ndl))`), `2158-2172` (Temporal 25 Taps + 9 Mittelwerte), `1476-1495` + `5123-5134` (Bloom: 10 Blur-Passes à 9 Taps), `5474-5503` (Caustics: `radians/cos/sin` je Pixel und Train), `1337-1338`
- **Problem**: (a) `tan(acos(ndl))` pro Pixel/Licht/Cascade ist `sqrt(1−ndl²)/ndl`; (b) `kGiTemporalFS` berechnet 9 überlappende 3×3-Mittelwerte aus 25 Taps einzeln (81 Additionen) statt separabel; (c) Bloom blurt 5 × (H+V) mit 9 Taps auf Halb-Res, ohne die Bilinear-Tap-Technik (5 statt 9 Taps) und ohne zusätzliche Downsample-Stufen; (d) Caustics: `radians(kTrains…)`, `cos`, `sin` pro Pixel und Train konstant über Frame → in Uniforms vorberechenbar.
- **Code-Auszug**:
```glsl
float bias = clamp(uShadowBias.x * tan(acos(ndl)), uShadowBias.y, 0.02) * float(c + 1);
```
- **Lösung**: (a) `sqrt(max(1.0 - ndl*ndl, 0.0)) / max(ndl, 1e-4)`; (b) Box-Mittel über Summed-Area (separabel); (c) Bilinear-Gauss 5-tap + Mip-Chain-Downsample; (d) pro Frame `vec2 dir[6]`, `k[6]` etc. als Uniform-Arrays.
- **Erwarteter Effekt**: Gering (GPU-seitig, nur messen, wenn der jeweilige Pass im GPU-Profil (`GpuPassScope`) auffällt).
- **Risiko / Verifikation**: (a) Grenzfall `ndl→0`: `tan(acos(0))` ist ~1.6e16, `clamp(…, , 0.02)` ergibt identisch 0.02 → bei der Ersetzung durch den `max(ndl, 1e-4)`-Nenner muss nach dem Clamp dasselbe herauskommen; (b)-(d) verändern Bildqualität leicht (Bloom). Messen vor Änderung.

### REN-629 – Kleinkram: State-Queries, Voll-Inverse für Kamera-Forward, Kopien in Debug-Linien/Previews, Shadowing
- **ID**: REN-629
- **Kategorie**: C/D/F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `10942-10943` (`glGetIntegerv(GL_FRAMEBUFFER_BINDING)` jedes Frame), `6684-6686` (3 State-Queries pro Frame in `RenderSkyReflCube`), `10966-10967` + `7630-7631` (`glm::inverse(view)[2]` nur für die Kamera-Forward-Richtung), `10091-10094` + `10144-10156` (`m_debugLines = lines` Kopie + Repack je Frame), `9505`/`9798` (8-KB-`boneScratch` je Skinned-Preview-Objekt), `9623-9646` (`RenderExtractor` + `RenderWorld` je Aufruf von `RenderWorldPreview`), `9184-9194` (`glReadPixels` synchron + doppelte Kopie je Thumbnail), `4054`/`4095` (Variablen `ok`/`log` verdecken äußere), `7693-7698` (4 × `glTexParameteri` je Frame auf den SSR-History-Texturen), `7589-7619` (Vollbild-Blit der HDR-Farbe jedes Frame für SSR-History)
- **Problem**: Je für sich geringfügig; zusammen vermeidbarer Dauer-Overhead und Lesbarkeitsmängel: Treiber-State-Queries (`glGetIntegerv`) können bei Threaded-Treibern synchronisieren; die Forward-Richtung ergibt sich aus der Transponierten der oberen 3×3 des View (`-vec3(view[0][2], view[1][2], view[2][2])`) ohne Inverse; Debug-Linien werden kopiert und neu gepackt, obwohl sich `std::vector<DebugLine>` mit `std::move`/`swap` übernehmen lässt.
- **Code-Auszug**:
```cpp
const glm::vec3 camFwd = -glm::normalize(glm::vec3(glm::inverse(m_renderWorld.camera.view)[2]));
void OpenGLRenderer::SetDebugLines(const std::vector<DebugLine>& lines) { m_debugLines = lines; }
```
- **Lösung**: Framebuffer-Bindung als Member (`m_currentTarget`) mitführen statt abzufragen; Forward aus View-Zeilen berechnen (`-normalize(vec3(view[0][2], view[1][2], view[2][2]))`, für Rotation+Translation identisch); `SetDebugLines(std::vector<DebugLine>&&)`/Swap und Debug-Vertexpuffer als Member mit `reserve`; Sampler-Objekte (`glGenSamplers`) für SSR-History statt `glTexParameteri` pro Frame; Shadowing beseitigen; Preview-Extractor als Member.
- **Erwarteter Effekt**: Klein; jede Maßnahme < 0,1 ms.
- **Risiko / Verifikation**: Forward aus View-Zeilen gleich der Inversen nur bei orthonormaler Rotation; bei nicht-uniformer Skalierung in der Kamera-Matrix (unüblich) Abweichung → `normalize` fängt Skalierung ab. Sonst risikolos.

---

## Gelesen

Ja – `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp` (Zeilen 1–13589) vollständig in Blöcken von 700–1500 Zeilen gelesen; zusätzlich gezielt (Grep/Auszug) `MaterialShaderLibrary.cpp:935-1010`, `MaterialGraph.cpp:1981-1990`, `OpenGLRenderer.h` (Member-Typen der Caches) und `WeatherParticleSeed.h:25-80` zur Verifikation der Funde.

## Rendering Slice 7 – OpenGLRenderer.h, OpenGLShaderManager, Software-Backend, VulkanMaterialLayout.h

## Zusammenfassung
Der Slice ist klein (2640 Zeilen) und besteht fast nur aus Headern bzw. dem schmalen Software-Backend. Der Code ist durchgehend gut kommentiert; die Software-Rasterung ist sauber vom Fenster-Shell getrennt und hat bereits Dirty-Rects. Die größten Hebel: (1) `OpenGLRenderer.h` trägt rund 600 Zeilen handgepflegter `int m_u…`-Uniform-Locations in drei fast identischen Sätzen (unlit/skinned/instanced), obwohl `SceneLightingLocs` und `GISceneLocs` schon den besseren Weg zeigen; (2) der Software-Renderer macht pro Dirty-Rect und Objekt unnötige Arbeit (Texture-Lookup, sin/cos vor dem Reject) und konvertiert jedes Pixel über `SDL_MapRGBA`; (3) `OpenGLShaderManager` ist faktisch toter Code. Funde: 1 Hoch, 3 Mittel, 5 Niedrig. Keine Messdaten vorhanden; Performance-Schätzungen sind als solche markiert.

## Funde

### REN-701 – Drei parallele Uniform-Location-Sätze (unlit / skinned / instanced) im OpenGLRenderer
- **ID**: REN-701
- **Kategorie**: A
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/include/Backends/OpenGL/OpenGLRenderer.h:470-514`, `:516-556`, `:558-601`; Verbraucher in `src/Backends/OpenGL/OpenGLRenderer.cpp:11501`, `:11545`, `:12547`
- **Problem**: Die drei Programme werden laut Header-Kommentar (`:206-210`) aus demselben `kUnlitFS` gelinkt und deklarieren „byte-identische“ Licht-/Schatten-Uniforms. Trotzdem existieren drei benannte Sätze à ca. 40 `int`-Member (`m_uLightCount` / `m_uSkinnedLightCount` / `m_uInstLightCount`, usw.), die an je einer Stelle mit `glGetUniformLocation` gefüllt und an je einer Stelle wieder zu einem `SceneLightingLocs` zusammengesetzt werden (`BindSceneLighting({ m_uLightCount, m_uLightPos, … })`). Zusätzlich gibt es mit `GISceneLocs m_giLocsUnlit/Skinned/Instanced` (`:1302`) bereits das richtige Muster (struct + `Fetch…(program)`). Jede neue Beleuchtungs-Uniform (Wetness, CloudShadow, SpecAA wurden offenbar so nachgezogen: `m_uSpecAA` vs. `m_uInstSpecAA`, aber kein Skinned-Gegenstück) muss in 3 Member-Listen, 3 Fetch-Blöcken und 3 Zusammenbau-Stellen gepflegt werden; Auslassungen sind bereits sichtbar (`m_uSkinnedWetness/Snow` fehlen, `m_uInstShadowEnabled` steht isoliert).
- **Code-Auszug**:
```cpp
int m_uLightCount = -1; ... int m_uCascadeVP = -1; ...      // :479-
int m_uSkinnedLightCount = -1; ... int m_uSkinnedCascadeVP = -1;   // :528-
int m_uInstLightCount = -1; ... int m_uInstCascadeVP = -1;         // :571-
```
- **Lösung**: (1) `struct ScenePassLocs { SceneLightingLocs light; GISceneLocs gi; int ao, viewport, ssaoEnabled, wetness, snow, cloudShadowMap/A/B, fogDensity, fogHeightFalloff, sunDir, ambient, skyEnv, color, hasTex, tex, metallic, roughness, opacity, noShadow; };` (2) `ScenePassLocs FetchScenePassLocs(unsigned prog) const;` – eine Funktion, die die gemeinsamen Namen einmal abfragt (fehlende → -1, wie bisher). (3) Die drei Programme behalten nur ihre Sonder-Uniforms (`mvp/model` bzw. `viewProj` bzw. `bones`) als kleine Zusatzmember. (4) `m_locsUnlit/m_locsSkinned/m_locsInstanced` ersetzen die ~120 Member; `BindSceneLighting(m_locsX.light, …)`. Schrittweise: erst Skinned (kleinster Satz), dann Instanced, dann Unlit; je Schritt bytegleiche Screenshots (`HE_UI_DUMP_DIR`/Headless-Dump) vergleichen.
- **Erwarteter Effekt**: Header −~110 Zeilen und −~120 Member, ca. −300 Zeilen in der .cpp (Schätzung); weniger Drift-Risiko. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Mittel: Reihenfolge/Namen der Uniforms müssen exakt gleich bleiben, `-1`-Semantik (Programm ohne `HE_CLUSTERED`) beibehalten. Absicherung: GL-Render-Vergleich vor/nach (Szene mit Skinned, Instanced, Schatten, Cluster), da `tests/` kein GL-Gerät nutzt.

### REN-702 – Software-Renderer: Texture-Lookup und sin/cos vor dem Reject, O(Rects × Objekte)
- **ID**: REN-702
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/Backends/Software/SoftwareRaster.cpp:254-302`, aufgerufen aus `SoftwareRenderer.cpp:205-210`
- **Problem**: `draw()` wird pro Dirty-Rect (bis 32, `SoftwareRaster.cpp:213`) mit der **gesamten** Objektliste aufgerufen. Pro Objekt passiert vor dem Reject-Test (`px1 <= px0`, `:302`): `texResolve(...)` (`:276`, bei `SoftwareRenderer.cpp:27-49` ein `cm->getTexture(id)`-Lookup), `std::cos/std::sin` (`:294`, auch bei `rot == 0`) und `quadBounds(o)` (bei Rotation nochmals cos/sin). Für Objekte, die das Dirty-Rect gar nicht berühren (der Normalfall bei Teilupdates), ist das reine Verschwendung: 32 Rects × N Quads Lookups/Trigonometrie pro Frame.
- **Code-Auszug**:
```cpp
if (o.type == 0 && texResolve && o.textureAssetId != HE::UUID{})
    tv = texResolve(o.textureAssetId, texUser);          // :276 – vor dem Reject
...
const float cs = std::cos(rot), sn = std::sin(rot);        // :294 – auch bei rot == 0
const glm::vec4 bounds = quadBounds(o);
...
if (px1 <= px0 || py1 <= py0) continue;                    // :302
```
- **Lösung**: (1) `bounds = quadBounds(o)` und die px/py-Schnittmenge **zuerst** berechnen und `continue`n; (2) `texResolve` erst danach aufrufen; (3) `cs/sn` nur `if (rot != 0)` (oder `cs=1,sn=0`). Ergebnis ist identisch, nur die Reihenfolge ändert sich. Optional später: `quadBounds` einmal pro Objekt und Frame vorberechnen statt pro Rect.
- **Erwarteter Effekt**: Bei vielen Quads (Listen, Inventare) und mehreren Dirty-Rects spürbar weniger CPU; Größenordnung linear in (Rects×Quads) – **messen** (unsicher, wie viele Quads reale Apps haben).
- **Risiko / Verifikation**: Niedrig, rein umgeordnet. Bestehende Raster-Tests (`tests/test_ui_widgets.cpp` nutzt `HE::sw::draw`) müssen pixelgleich bleiben.

### REN-703 – Pixelweiser `SDL_MapRGBA`-Aufruf beim Blit in die Fensterfläche
- **ID**: REN-703
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/Backends/Software/SoftwareRenderer.cpp:231-239`
- **Problem**: Jeder Pixel des Dirty-Bereichs geht durch `SDL_MapRGBA(fmt, nullptr, r,g,b,255)`, eine nicht inlinebare Bibliotheksfunktion mit generischem Format-Pfad. Bei einem vollen 4K-Repaint (die Datei selbst nennt 8,3 Mio. Pixel, `SoftwareRaster.h:100`) sind das 8,3 Mio. Aufrufe pro Frame – bei Resize/Drag genau der Fall, für den der Blit gebaut wurde.
- **Code-Auszug**:
```cpp
for (int x = x0; x < x1; ++x, src += 4)
    dst[x] = SDL_MapRGBA(fmt, nullptr, src[0], src[1], src[2], 255);
```
- **Lösung**: Vor der Schleife einmal prüfen: `surf->format` ist `SDL_PIXELFORMAT_XRGB8888`/`ARGB8888` → `dst[x] = 0xFF000000u | (src[0]<<16) | (src[1]<<8) | src[2];` (bei `ABGR8888`/`XBGR8888` Kanäle vertauschen); sonst bestehender generischer Pfad als Fallback. Alternativ `SDL_ConvertPixels` zeilenweise.
- **Erwarteter Effekt**: Schätzung 3-10x schnellerer Blit bei vollem Repaint; **messen**.
- **Risiko / Verifikation**: Farbkanal-Vertauschung („it looked blue“, Kommentar `:214-217`) ist das Risiko. Test: Pixel-Roundtrip über eine Offscreen-`SDL_Surface` je Format (XRGB/ARGB/ABGR) gegen den `SDL_MapRGBA`-Pfad vergleichen.

### REN-704 – TAA-Velocity: zwei `unordered_map<uint32_t, mat4>` mit Knoten-Allokation pro Objekt und Frame
- **ID**: REN-704
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: Deklaration `OpenGLRenderer.h:877-878`; Nutzung `src/Backends/OpenGL/OpenGLRenderer.cpp:4746-4788`
- **Problem**: Pro Frame wird `m_taaCurTransforms.clear()` (gibt alle Knoten frei) aufgerufen und danach pro sichtbarem Objekt `m_taaCurTransforms[obj.entityId] = …` (je ein Heap-Knoten, 64 B + Overhead) sowie ein `find` in der Vorgänger-Map ausgeführt; am Ende `swap`. Bei 10k sichtbaren Objekten sind das 10k Allokationen/Frees plus 20k Hash-Operationen pro Frame, nur solange TAA aktiv ist.
- **Code-Auszug**:
```cpp
m_taaCurTransforms.clear();
...
const auto it = m_taaPrevTransforms.find(obj.entityId);
...
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Flache Struktur: `std::vector<std::pair<uint32_t, glm::mat4>>` für Cur (reserve, kein Free), nach `entityId` sortiert (oder dichte Tabelle `entityId → slot` falls Ids dicht sind) und `Prev` per `lower_bound` suchen; alternativ `robin_hood`/Flat-Map, falls im Projekt vorhanden. Pro Frame wäre dann nur `clear()` ohne Freigabe nötig.
- **Erwarteter Effekt**: Entfällt 1 Alloc/Objekt/Frame; Schätzung −0,2…0,5 ms bei 10k Objekten, nur mit TAA – **messen**.
- **Risiko / Verifikation**: Niedrig; Verhalten (neues Objekt → keine Bewegung) beibehalten. Prüfen, ob `entityId` bei Instanz-Ketten mehrfach vorkommt (letzter Eintrag gewinnt in der Map; Sortier-/Duplikatsemantik gleich halten).

### REN-705 – Software-Renderer kopiert die komplette Quad-Liste jeden Frame (auch im „nichts geändert“-Fall)
- **ID**: REN-705
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/Backends/Software/SoftwareRenderer.cpp:189-197`, `:211`; `SoftwareRenderer.h:62`
- **Problem**: Genau der Pfad, der laut Kommentar „nichts kostet“ (`:191-193`), kopiert `m_renderWorld.uiObjects` nach `t.prevObjects` (`:195`). `UIRenderObject` enthält u. a. UUIDs, vec4s und (vermutlich) mehr; für N Quads ist das ein O(N)-Speichercopy pro Idle-Frame. Im Idle-Fall sind beide Listen nach `dirtyRects` gleich (außer evtl. unsichtbare Quads, die `dirtyRects` überspringt, `:207`).
- **Code-Auszug**:
```cpp
t.lastPixels = 0;
t.prevObjects = m_renderWorld.uiObjects;
return;
```
- **Lösung**: Im Idle-Fall die Kopie weglassen, **wenn** `prevObjects.size() == uiObjects.size()` (dann sind alle Quads per `sameQuad` gleich oder unsichtbar-gleich). Im Dirty-Fall `std::swap(t.prevObjects, m_renderWorld.uiObjects)` statt Kopie, sofern `extractUI` die Liste vor dem Befüllen leert (`RenderExtractor::extractUI` prüfen). 
- **Erwarteter Effekt**: Idle-Frame wird wirklich kostenlos; Gewinn klein (Schätzung <0,1 ms bei 1000 Quads) – **messen**.
- **Risiko / Verifikation**: Niedrig-Mittel: nach dem Swap ist `uiObjects` leer/alt; `GetFrameGpuStats` nutzt nur `lastQuads`, `CaptureViewport` nur `frame`. Test: zwei identische Frames, dann einer mit geändertem Quad, Dirty-Rect-Ergebnis unverändert.

### REN-706 – `OpenGLShaderManager` ist praktisch toter Code und hat Fehler im Lifecycle
- **ID**: REN-706
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/Backends/OpenGL/OpenGLShaderManager.cpp:14-104`, `include/Backends/OpenGL/OpenGLShaderManager.h:1-17`, `OpenGLRenderer.h:273`, `OpenGLRenderer.cpp:3372`
- **Problem**: (a) Der einzige Verwendungsort von `m_shaderManager` ist die Zuweisung `m_shaderManager = OpenGLShaderManager();` (grep über `src/HE_Rendering`: keine Aufrufe von `load/compile/createProgram`); der Renderer baut seine Programme selbst (`LinkSceneProgram`, `GetOrBuild…Program`). (b) `m_loadedShaders`/`m_loadedPrograms` werden nie befüllt (anders als D3D11/D3D12, `D3D11ShaderManager.cpp:45,146`), also ist `cleanup()` ein No-op. (c) `createProgram` gibt bei Link-Fehler die `program.id` nicht frei (`:76-84`, Leak); `load` legt bei Fehlern kein `glDeleteShader` an. (d) `fs::exists` + `ifstream` ist doppelte Dateisystemarbeit. (e) Der Header hat kein `#pragma once` und keinen `<vector>`-Include; er hängt vom Vorinclude von `ShaderManager.h` ab, und `OpenGLRenderer.h` zieht ihn in jeden Includer.
- **Code-Auszug**:
```cpp
m_shaderManager = OpenGLShaderManager();   // OpenGLRenderer.cpp:3372 – einzige Verwendung
...
void OpenGLShaderManager::cleanup()
{   for (const auto& program : m_loadedPrograms) releaseProgram(program); ... }  // Listen immer leer
```
- **Lösung**: Entweder entfernen (Member, Include, Zeile 3372, .cpp/.h, CMake-Einträge `CMakeLists.txt:43`) – oder, falls das `ShaderManager`-Interface backend-übergreifend gebraucht wird, die Listen füllen, `#pragma once` ergänzen, Leak in `createProgram` beheben. Vor dem Entfernen klären, ob `ShaderManager` in anderen Modulen (Editor-Shaderkompilierung) über die Basisklasse verwendet wird (Hinweise in `HE_Editor/EditorUI.cpp:173`).
- **Erwarteter Effekt**: −~120 Zeilen, ein Include weniger im schweren Header; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Niedrig; Build aller Targets (Windows/Linux GL), `grep` nach `ShaderManager` in `HE_Editor`/`HE_Game`.

### REN-707 – Per-Pixel-Konstanten und Byte-Schleifen im Software-Rasterizer
- **ID**: REN-707
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `SoftwareRaster.cpp:357-374`, `:72-76`, `:85-90`, `:419-430`
- **Problem**: (a) Der lineare Gradient berechnet `std::sin(a)` / `std::cos(a)` pro Pixel (`:370-371`), obwohl der Winkel pro Quad konstant ist. (b) Pro Pixel wird die gesamte Verzweigungskette (type, tv.rgba, gradient, maxR/border/blur) neu ausgewertet, und `over` ruft `clamp01` dreifach. Ein deckendes Quad ohne Rundung/Border/Textur (`src = fill`, `:379`) läuft trotzdem über Pixel-Source-Over mit Float-Division durch 255. (c) `Image::clear` schreibt byteweise statt `std::fill`/Wiederholung eines 32-Bit-Wertes; `clearRect` analog.
- **Code-Auszug**:
```cpp
const float a = o.gradientAngleDeg * 0.017453292f;
t = clamp01((lu - 0.5f) * std::sin(a) + (lv - 0.5f) * std::cos(a) + 0.5f);   // pro Pixel
```
- **Lösung**: `sin/cos` vor der Pixelschleife (neben `cs/sn`) berechnen. Optional: Schnellpfad für `fill.a >= 1` ohne Form/Textur: Span-Fill der Zeile mit einem `uint32`. `Image::clear` über ein `uint32_t` pro Pixel (`std::fill_n` auf `uint32_t*`, Alignment der `vector<uint8_t>` beachten bzw. `memcpy`).
- **Erwarteter Effekt**: (a) klein; (c) klein; Schnellpfad: für große Flächen deutlich (Schätzung 5-10x für Vollflächen) – nur nach Messung, bevor die Funktion „drei Kopien in Sync“ (`SoftwareRaster.h:22-24`) verkompliziert wird.
- **Risiko / Verifikation**: (a) trivial, bytegleich; Schnellpfad ändert Rundung (`+0.5f`) potenziell – mit Pixelvergleichs-Tests absichern.

### REN-708 – Schwerer Header `OpenGLRenderer.h` mit impliziten Abhängigkeiten
- **ID**: REN-708
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.h:1-25`, `:642`
- **Problem**: Der Header inkludiert RenderWorld, RenderExtractor, FrustumCuller, OcclusionCuller, RenderSorter, RenderGraph, CommandBuffer, GiBvh, GIProbeGrid, MaterialShaderLibrary und `OpenGLShaderManager.h`, weil alle Member by value gehalten werden. Gleichzeitig fehlen direkte Includes: `std::unordered_set` (`:642`) und `std::vector` kommen nur transitiv, `<glm/…>` wird nirgends direkt inkludiert. Es gibt sinnvolle `struct`-Vorwärtsdeklarationen (`:22-25`), aber der Header selbst bleibt der schwerste Teil jeder GL-Übersetzungseinheit.
- **Code-Auszug**:
```cpp
std::unordered_set<unsigned int> m_glArrayTex;   // :642 – kein #include <unordered_set>
```
- **Lösung**: Fehlende Std-Includes (`<unordered_set>`, `<vector>`, `<glm/glm.hpp>`) ergänzen (sofort, risikofrei). Mittelfristig GI/SSR/DoF/MotionBlur-Zustand (ca. Zeilen 980-1380) in Teilstrukturen (`struct GiState`, `struct SsrState`) in eigenen Headern auslagern und im Renderer per `std::unique_ptr` oder Wert halten, sodass nur `OpenGLRenderer_*.cpp` die Details sehen. Das ist die gleiche Auslagerung wie REN-701.
- **Erwarteter Effekt**: Kürzere Kompilierzeit der GL-Dateien (Schätzung gering, **messen** mit `-ftime-trace`); bessere Lesbarkeit.
- **Risiko / Verifikation**: Niedrig für Includes; Mittel für die Strukturaufteilung (Zugriffe in einer 12k+-Zeilen-.cpp).

### REN-709 – VulkanMaterialLayout: Tabellen-Redundanz und volle Set-Größe pro Draw
- **ID**: REN-709
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `include/Backends/Vulkan/VulkanMaterialLayout.h:59-109`, `:175-197`, `:140-144`; Verbraucher `src/Backends/Vulkan/VulkanRenderer.cpp:2805-2880`, `:8300`
- **Problem**: (a) Die Pool-Dimensionierung (`VulkanRenderer.cpp:2873-2878`) legt pro Draw ein Set mit **25 Deskriptoren** an (17 Sampler, 5 UBO, 3 SSBO), obwohl pro Draw typischerweise nur wenige (b0,b1,b2,b3) material-spezifisch sind; die übrigen 13+ Sampler (GI, AO, SkyEnv, CSM, …) sind frame-konstant. Das ist ein Descriptor-Set-Allocate + Write pro Material-Draw (`k_matMaxDraws`) – ein klassisches Ziel für „Set 0 = frame-konstant, Set 1 = pro Material/Draw“. (b) Die Resolve-Tabelle dupliziert 12 Zeilen der Material-Tabelle (10-13, 15-18, 31-33), manuell parallel gepflegt, nur durch `static_assert`-Zählungen abgesichert. (c) `landscapeWeightsFit` vergleicht zweimal `fragmentSamplerCount(true)`; `std::min`-Vergleich gegen beide Limits wäre kürzer. (d) `kPreClusterBindingCount = kBindingCount - 3` verdrahtet die Zahl 3 getrennt von den drei Cluster-Konstanten.
- **Code-Auszug**:
```cpp
constexpr uint32_t kPreClusterBindingCount = kBindingCount - 3;
...
return fragmentSamplerCount(true) <= maxPerStageSamplers
    && fragmentSamplerCount(true) <= maxPerStageSampledImages;
```
- **Lösung**: (a) Layout in zwei Sets aufteilen: Set 0 mit den frame-konstanten Samplern/UBO (einmal pro Frame geschrieben), Set 1 mit b0..b9 der Materialparameter; Descriptor-Writes pro Draw sinken auf ~4-6. Das ist ein Umbau in `VulkanRenderer.cpp`/Shader-Bindings der `MaterialShaderLibrary` (SPIR-V-Generierung!) und nur mit Messung (Draws × Writes) sinnvoll. (b)-(d) kosmetisch: gemeinsame `constexpr`-Teiltabelle bzw. benannte Konstante `kClusterBindingCount = 3`.
- **Erwarteter Effekt**: (a) CPU-Zeit im Vulkan-Material-Draw-Loop sinkt vermutlich merklich bei vielen Materialdraws; **unsicher / messen** (hängt davon ab, ob `VulkanRenderer.cpp:8559` pro Draw alle 25 oder nur geänderte Bindings schreibt – dort nicht gelesen, nicht Teil des Slice). (b)-(d) keine.
- **Risiko / Verifikation**: (a) Hoch: ändert Shader-Bindings, D3D12/Metal-Parallelen und den SPIR-V-Reflexionstest (`he_tests`, Layout-gegen-SPIR-V). (b)-(d) niedrig; die vorhandenen `static_assert`s decken sie.

## Gelesen
Alle acht Dateien des Slice vollständig gelesen: ja (`OpenGLRenderer.h` 1397 Zeilen in zwei Blöcken, `OpenGLShaderManager.cpp/.h`, `SoftwareRaster.cpp/.h`, `SoftwareRenderer.cpp/.h`, `VulkanMaterialLayout.h`); `OpenGLRenderer.cpp`, `VulkanRenderer.cpp` und `D3D*ShaderManager.cpp` nur per gezieltem Grep/Ausschnitt zur Verifikation der Querverweise.

## Rendering Slice 8 – Backends/Vulkan/VulkanRenderer.cpp (14.698 Zeilen)

Datei: `src/HE_Rendering/src/Backends/Vulkan/VulkanRenderer.cpp` (alle Zeilenangaben beziehen sich darauf, sofern nicht anders genannt).

## Zusammenfassung

Der Vulkan-Backend ist funktional reich (Deferred, TAA, SSR, SW/HW-GI, Decals, Preview), aber die Datei ist eine einzige 14,7k-Zeilen-Klasse mit sehr viel kopiertem Vulkan-Boilerplate (33× `vkCreateGraphicsPipelines`, 16× `VkPipelineViewportStateCreateInfo`, 21× One-Shot-Command-Buffer mit `vkQueueWaitIdle`, 58× `findMemoryType`, 53× `vkAllocateMemory`, 4× identisches `makeModule`-Lambda). Die schwersten Performance-Probleme liegen im Draw-Pfad der Graph-Materialien (pro Draw/Instanz ein frisch allokiertes Descriptor-Set mit ~25 Writes plus Shader-Quelltext-Kopie und -Hash), in der blockierenden Asset-Upload-Strategie (je Mesh/Textur eigener Submit + `vkQueueWaitIdle`, je Buffer eigene `vkAllocateMemory`, Vertex-/Index-Buffer in host-visible Speicher), in der fest eingeschalteten Validation-Layer und in der mehrfachen Cull+Sort-Arbeit pro Frame. Gefunden: 5 Hoch (REN-801 bis 805), 12 Mittel (REN-806 bis 817), 5 Niedrig (REN-818 bis 822), zusammen 22 Funde. Es gibt keine Pipeline-Cache-Nutzung (`VkPipelineCache`: 0 Treffer) und keinen Sub-Allocator. Vorschläge erhalten das sichtbare Verhalten; wo nicht, ist es vermerkt. Keine Messdaten verfügbar: Effekte sind Schätzungen.

---

## Funde (Hoch zuerst)

### REN-801 – Graph-Material-Draw: Descriptor-Set-Allokation, ~25 Writes und Shader-String-Kopie/-Hash pro Draw (und pro Instanz)
- **ID**: REN-801
- **Kategorie**: D (zusätzlich C)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: VulkanRenderer.cpp:8251-8259 (Resolve pro Draw), 8317-8352 (Texturen), 8364-8566 (`drawMatInstance`), 8561-8584 (Re-Bind)
- **Problem**: Für jeden DrawCall mit Graph-Material passiert pro Draw: (a) `resolveShaders(...)` kopiert den kompletten Fragment-GLSL-Text in einen neuen `std::string` und berechnet `std::hash<std::string>` über ihn (`MaterialShaderLibrary.cpp:1000-1008`), bei WPO zusätzlich Hash des Vertex-Bodys (`:947-952`); (b) `precompiledFor`, 3-4× `getMaterial`, `matGlslTextureArrayMask`, vier `std::string gPaths[]`, je Textur-Slot `resolveGraphTexture` (baut `graphTexKey`-String + `"#arr"` und hasht, 3616-3622); (c) in `drawMatInstance` pro **Instanz**: `vkAllocateDescriptorSets` + bis zu 25 `VkWriteDescriptorSet` + `vkUpdateDescriptorSets` + eigener Pipeline-/Set-Bind, obwohl sich pro Draw nur zwei UBO-Offsets (Objekt/Params) ändern; fast alle Image-Bindings (GI, CSM, SSR, Sky, AO, Cluster-SSBOs) sind frame-konstant. Das Ring-Limit `k_matMaxDraws = 1024` (Header :457) wird dadurch zu einer harten Szenengrenze (Überschuss rendert still als Built-in-PBR, 8299/8365).
- **Code-Auszug**:
```cpp
uint64_t matHash = 0; std::string matFrag, matVertBody;
if (m_matShaderLib.resolveShaders(*m_contentManager, dc.materialAssetId, matHash, matFrag, matVertBody)) { ...
auto drawMatInstance = [&](const glm::mat4& model) {
    ...
    vkAllocateDescriptorSets(m_device, &dsai, &set) ...
    vkUpdateDescriptorSets(m_device, nWrites, w, 0, nullptr);
```
- **Lösung**:
  1. Pro Material (Key = `UUID`) einmal `{hash, const string* frag, vertBody-Hash, precompiled*, arrMask, Texturslots, Params}` in einer Cache-Struktur halten (Invalidierung über das vorhandene `InvalidateMaterial`); `resolveShaders` nur auf Miss. Der Hash gehört in `MaterialAsset` (einmal berechnet), nicht per Draw.
  2. Frame-konstante Bindings (0 Lighting, 10-13, 15-18, 24-26, 31-33) in ein **frame-weites Set** legen oder per Descriptor-Set-Layout-Split (Set 0 = Frame, Set 1 = Material, Set 2 = Draw mit Dynamic-UBO-Offsets für `U` und `HeParams`). Pro Draw dann nur `vkCmdBindDescriptorSets` mit Dynamic Offsets; ein Set je (Material, Albedo/Override-Textur, Weightmap) cachen und beim Frame-Wechsel nur bei geänderten Views neu schreiben.
  3. Instanzen: Per-Instanz-Daten in die Instanz-Vertex-Buffer-Schiene legen (wie A3 für Built-in) statt Set pro Instanz.
- **Erwarteter Effekt**: (Schätzung) CPU-Zeit im Material-Draw-Pfad um ein Mehrfaches niedriger (Descriptor-Writes ~25→0-2 pro Draw, Heap-Allokationen ~10→0); Entfall der 1024-Draw-Grenze. Messen mit CPU-Profiler an einer Szene mit ≥500 Graph-Material-Objekten.
- **Risiko / Verifikation**: Hoch (Descriptor-Layout ist durch `HE::vkmat::kBindings` fixiert und gegen SPIR-V getestet, siehe `tests/` Material-Layout-Tests). Layout-Änderung heißt: Shader-Reflexionstest anpassen. Bildvergleich mit `tests/test_*` GL/Vulkan-Parität und Validation-Layer-Lauf (Layouts GENERAL vs. READ_ONLY beachten, 8444-8451).

### REN-802 – Jeder Asset-Upload blockiert die Queue (`vkQueueWaitIdle`) auf dem Render-Thread
- **ID**: REN-802
- **Kategorie**: E (D)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: 7217-7332 (`uploadRGBA8Image`, Wait 7314), 7338-7509 (`uploadTextureImage`, Wait 7491), 7760-7764 (`resolveMesh`, Kommentar bestätigt Stall), 9903 (`SetMoonTexture`), 10052 (`CreateImGuiTexture`), 14494 (`uiFontAtlasSet`), 12042/12148/12880 (SSAO/Prime), 10552 (BLAS)
- **Problem**: 21× `vkQueueWaitIdle`, 22× `vkDeviceWaitIdle`. `resolveMesh`/`resolveAndUploadAlbedo`/`resolveGraphTexture`/`resolveDecalTexture`/`resolveUIImageSet` werden **mitten im Draw-Loop** aufgerufen; jeder erste Zugriff erzeugt Staging-Buffer + Memory, Command Buffer, Submit und Wait-Idle: die GPU wird komplett geleert, der Frame-Overlap geht verloren. Beim Laden einer Szene mit N Meshes/Texturen sind das N harte Pipeline-Stalls innerhalb eines Frames. Zusätzlich ein zusätzlicher `memcpy` der Textur in den Staging-Buffer (7410) und je Mip ein eigener `vkCmdCopyBufferToImage` (7461-7472).
- **Code-Auszug**:
```cpp
vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
vkQueueWaitIdle(m_graphicsQueue);
vkFreeCommandBuffers(m_device, m_cmdPool, 1, &oneCB);
```
- **Lösung**:
  1. Kleine `UploadContext`-Klasse: ein persistenter Staging-Ring (z. B. 32-64 MB host-visible, per Frame-Slot), ein Transfer-Command-Buffer pro Frame-Slot. `uploadX` schreibt in den Ring, zeichnet Copy + Barrier in den Upload-CB auf und gibt View/Image sofort zurück; der Upload-CB wird am Frame-Anfang vor dem Haupt-CB submittet (gleiche Queue ⇒ Reihenfolge reicht, Semaphore nicht nötig). Freigabe von Staging über Frame-Fence (`m_frameSerial`/`GetCompletedFrameSerial` existiert bereits).
  2. Bis ein Asset „fertig“ ist: wie heute Fallback (weiße Textur / Würfel); dafür müssen Caches einen `pending`-Zustand kennen. Alternativ erst den Einmal-Helper (ein Wait pro **Batch** am Frame-Ende statt pro Ressource) einführen.
  3. Die Dubletten zu einer Hilfsfunktion `submitOneShot(fn)` zusammenführen (siehe REN-815).
- **Erwarteter Effekt**: (Schätzung) Beim Streaming vieler Assets entfallen N Voll-Stalls; typische Hitches im ms-Bereich pro Asset (Treiber/GPU-abhängig) verschwinden. Messen: Zeit von `resolveMesh` beim ersten Zugriff.
- **Risiko / Verifikation**: Mittel-hoch (Layout/Barrier-Reihenfolge, Lebenszeit der Staging-Daten). Tests: bestehende Render-Smoke-Läufe mit lavapipe (HE_HIDDEN_WINDOW), Validation-Layer sauber halten.

### REN-803 – Mesh-/Skeletal-/Ribbon-Buffer liegen in host-visible Speicher; je Buffer eigener `vkAllocateMemory`, kein Sub-Allocator
- **ID**: REN-803
- **Kategorie**: C / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: 7107-7136 (`createMeshBuffers`), 13986-14018 (`makeHostBuf` Skeletal), 7153-7182 (Ribbon), Allokationsstatistik: 53× `vkAllocateMemory`, 29× `HOST_VISIBLE_BIT`
- **Problem**: Statische Vertex-/Index-Buffer werden mit `HOST_VISIBLE|HOST_COHERENT` angelegt, einmal befüllt und nie wieder von der CPU geschrieben. Auf diskreten GPUs liest die GPU sie dann über PCIe (bzw. aus BAR-Speicher). Zusätzlich 2 Allokationen pro Mesh (vbuf, ibuf; Skeletal 4, plus Textur-Image+Staging). Der Treiber-Grenzwert `maxMemoryAllocationCount` (typisch 4096) begrenzt damit die Szene hart (≈2000 Meshes ohne Textur); `k_maxMeshTextures = 1024` (Header :550) ist die Descriptor-Variante derselben Grenze.
- **Code-Auszug**:
```cpp
mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
...
std::memcpy(p, data, static_cast<size_t>(size));
```
- **Lösung**: Einen Mesh-Pool einführen: große DEVICE_LOCAL-Blöcke (z. B. 64 MB Vertex+Index in einem Buffer, `vkCmdBindVertexBuffers` mit Offset, `firstIndex`/`vertexOffset` im Draw), Befüllung über den Staging-Ring aus REN-802. Gleiche Idee für Bild-Speicher (ein Block, mehrere `vkBindImageMemory` mit Offset). Vorarbeit: `GpuMesh` bekommt `vbufOffset/ibufOffset`. Bei Wegfall von vmem/imem im Struct fallen auch die manuellen Free-Zweige in `processPendingInvalidations` (7605-7612) und `Shutdown` (351-361) weg.
- **Erwarteter Effekt**: (Schätzung) Vertex-Fetch-Bandbreite aus VRAM statt PCIe (auf dGPU deutlich), weniger Allokationen, weniger Handle-Verwaltung. Auf iGPU/UMA keine Änderung.
- **Risiko / Verifikation**: Mittel. `firstIndex`/Index-Range-Logik (`DrawIndexRange`, 67-73, hängt an Buffer-Länge) muss mit Offsets korrekt bleiben (Tests rund um Sections/`DrawIndexRange` im Tests-Verzeichnis prüfen). Fragmentierung/Invalidierung (Mesh-Sculpt) braucht Freelist.

### REN-804 – Validation-Layer fest eingeschaltet (`gpuDebug = true`) auch in Release
- **ID**: REN-804
- **Kategorie**: F / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: 1283-1288, 1309-1313, 1341
- **Problem**: Die Instanz aktiviert `VK_LAYER_KHRONOS_validation`, sobald installiert, unabhängig von Build-Typ oder Umgebungsvariable. Kommentar nennt es „TEMPORARILY“. Validation-Layer verteuert jeden `vk*`-Aufruf um ein Vielfaches; zusammen mit REN-801 (hunderte Descriptor-Updates pro Frame) wird die CPU-Zeit verfälscht und der Backend unnötig langsam. Außerdem Log-Ausgabe der Meldung „(HE_GPU_DEBUG)“, obwohl die Variable nie gelesen wird.
- **Code-Auszug**:
```cpp
const bool gpuDebug = true;
(void)0;
```
- **Lösung**: `const bool gpuDebug = [] { const char* e = std::getenv("HE_GPU_DEBUG"); return (e && *e && *e != '0')
 #ifndef NDEBUG
 || true
 #endif
 ; }();` Die Brace-Struktur 1289-1314 (ein überflüssiger innerer Block) gleichzeitig begradigen.
- **Erwarteter Effekt**: (Schätzung) Auf Rechnern mit installiertem Vulkan SDK deutlich geringere CPU-Last pro Frame; Startzeit kürzer. Auf Rechnern ohne SDK keine Änderung.
- **Risiko / Verifikation**: Niedrig. Die CI (lavapipe) hängt laut Kommentar 5773-5783 teils an Validation als Fehler-Erkennung: dort `HE_GPU_DEBUG=1` setzen.

### REN-805 – Cull + Sort bis zu 5× pro Frame über denselben Objektsatz
- **ID**: REN-805
- **Kategorie**: D
- **Schweregrad**: Hoch (Annahme; messen)
- **Typ**: Performance
- **Ort**: 3891-3892 (`EncodeDecalDepth`), 7840-7841 (`DrawScene`), 11510-11511 (`runGi`), 12656-12657 (`runSSAO`); zusätzlich `resolveWorldMaterialScalars` in 3890, 7838, 11495; `refineObjectBounds` je Aufruf mit Frühausstieg (6888)
- **Problem**: Der Extraktor ist per `FrameScope` bereits auf einen Walk pro Frame optimiert (413-418), aber `m_culler.cull(m_renderWorld, m_visible)` und `m_sorter.sort(...)` laufen für dieselbe Kamera und dasselbe Aspect in bis zu vier Passes erneut. Ergebnis wird jeweils in dieselben Member (`m_visible`, `m_sortedIndices`) geschrieben. Bei 50k Objekten ist jede Wiederholung O(n) Frustum-Test + O(n log n)-Sortierung.
- **Code-Auszug**:
```cpp
m_culler.cull(m_renderWorld, m_visible);
m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
```
- **Lösung**: Wie bei `refineObjectBounds` einen Schlüssel `(fullExtractCount, objects.data(), count, view*proj-Hash, taaJitter)` führen und `cullAndSortOnce()` schreiben; die vier Aufrufer rufen es auf und überspringen bei gleichem Schlüssel. Achtung: `EncodeDecalDepth` extrahiert mit eigenem Aspect (3880) und löst dadurch einen neuen Walk aus; im Viewport-Pfad ist der Aspect gleich, dort greift der Cache. `resolveWorldMaterialScalars` analog (idempotent pro Walk) hinter denselben Schlüssel.
- **Erwarteter Effekt**: (Schätzung) 2-4 Cull+Sort-Durchläufe weniger pro Frame im Viewport-HDR-Pfad bei aktivem SSAO/GI/Decals. Messen: Zeitscope um die vier Aufrufstellen.
- **Risiko / Verifikation**: Mittel. `tests/test_culling.cpp` pinnt bereits die FrameScope-Regeln; für diesen Cache einen Test ergänzen, der bei geändertem Jitter/Aspect neu berechnet. Beachten: `m_sortedIndices` wird in `encodeTaaVelocity` über `m_taaSceneSorted` indirekt vorausgesetzt (5638).

---

## Mittel

### REN-806 – TAA-Velocity: `unordered_map` pro Frame komplett neu aufgebaut
- **ID**: REN-806
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 5648-5674; Header `VulkanRenderer.h:874` (`std::unordered_map<uint32_t, glm::mat4> m_taaPrevTransforms, m_taaCurTransforms;`)
- **Problem**: Pro TAA-Frame wird `m_taaCurTransforms.clear()` aufgerufen (gibt alle Knoten frei) und dann für jedes sichtbare Objekt `m_taaCurTransforms[entityId] = transform` eingefügt (je ein Heap-Knoten, 72 B), dazu ein `find` in der Vorgänger-Map. Danach `swap`. Bei n sichtbaren Objekten sind das n Allokationen + n Frees pro Frame. Die Velocity-Passage zeichnet außerdem jedes Objekt einzeln (kein Batching), siehe auch REN-812.
- **Code-Auszug**:
```cpp
m_taaCurTransforms.clear();
...
const auto it = m_taaPrevTransforms.find(obj.entityId);
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Flache Struktur: `std::vector<std::pair<uint32_t, glm::mat4>>` sortiert nach `entityId` (oder offene Adressierung) für prev/cur; `m_sortedIndices` ist stabil sortiert nach Mesh, daher zuerst nach `entityId` sortieren oder Hash-Tabelle mit Reserve (`reserve(max(prev.size(), n))`, `clear()` ohne Rehash als flat map). Minimal: `m_taaCurTransforms.reserve(...)` und `swap` reicht nicht (Knoten-Allokation bleibt) – flache Tabelle ist die eigentliche Lösung.
- **Erwarteter Effekt**: (Schätzung) bei 10k+ sichtbaren Objekten mehrere zehn µs bis ms pro Frame (Allocator-Druck); messen.
- **Risiko / Verifikation**: Niedrig. Verhalten identisch („neues Objekt = keine Bewegung“). Testen mit TAA-Dump (`HE_*`-Capture) auf Gleichheit der Velocity-Textur.

### REN-807 – GI: pro Frame `glm::inverse` + Upload für alle Objekte, BLAS-Builds blockierend
- **ID**: REN-807
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 10226-10352 (`updateGiAccel`), 10263-10314, 10442-10572 (`buildGiHwBlas`, `vkQueueWaitIdle` 10552), 10154-10179 (`BuildGIBlas` CPU-BVH on demand)
- **Problem**: Jeder Frame mit aktiver GI iteriert über alle `castsShadow`-Objekte, berechnet `glm::inverse(obj.transform)` (4×4 vollständig), baut `GIInstanceGpu`- und `VkAccelerationStructureInstanceKHR`-Vektoren neu (Heap pro Frame trotz `reserve`) und kopiert den gesamten Array in den host-visible Ring (10334). Es gibt keine Dirty-Erkennung (statische Welt = identische Daten). Erste Sichtung eines Meshes baut BVH auf der CPU im Render-Thread (`BuildGIBlas`) bzw. das HW-BLAS mit eigener Submit+Wait-Idle-Runde. Außerdem: bei Mesh-Invalidierung wird der **gesamte** BLAS-Cache verworfen (7618-7624).
- **Code-Auszug**:
```cpp
inst.invTransform = glm::inverse(obj.transform);
...
if (!uploadGiBuffer(m_giInstanceBuf[m_currentFrame], instances.data(), instances.size() * sizeof(GIInstanceGpu)))
```
- **Lösung**: (1) Inverse nur bei geänderter Transform (Hash/Version aus Extraktor, falls vorhanden; sonst Vergleich mit Vorframe-Matrix pro Objekt) – oder vom Extraktor mitliefern lassen. (2) Persistente `std::vector` als Member (`clear()` statt neu allozieren). (3) Upload überspringen, wenn Instanz-Array unverändert (Hash/Memcmp gegen letzten Frame je Ring-Slot). (4) BLAS-Aufbau über den Upload-Kontext aus REN-802 asynchron bzw. auf Job-Thread (BVH ist CPU-rein).
- **Erwarteter Effekt**: (Schätzung) GI-CPU-Kosten pro Frame bei statischen Szenen ~O(1) statt O(n); Hitch beim ersten Mesh entfällt. Messen.
- **Risiko / Verifikation**: Mittel. Instanzreihenfolge muss mit `InstanceId` (`vi.instanceCustomIndex`, 10303) übereinstimmen – deshalb Reihenfolge nicht ändern. Kommentar 10319-10322 weist selbst darauf hin, dass `m_giNodeBuf/m_giTriBuf` bei Wachstum zerstört werden, während ein Vorframe sie noch lesen kann (Hazard, unsicher; nicht verifiziert) – bei Umbau mit abdecken.

### REN-808 – Keine `VkPipelineCache`; Material-Pipelines und Sky-Shader werden synchron beim ersten Draw / Start gebaut
- **ID**: REN-808
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 3076-3302 (`GetOrBuildMaterialPipeline`), 7659-7694 (`drainMaterialWarmup`), 9345-9368 (Sky-Fragment-Shader-Kompilierung per shaderc bei jedem Start), 9139-9145 (3D-Rauschvolumen 256³ = 64 MB CPU-Bake + Upload bei jedem Start im Release)
- **Problem**: Alle 33 `vkCreateGraphicsPipelines`/Compute-Aufrufe übergeben `VK_NULL_HANDLE` als Cache. Ein neues Graph-Material löst Quelltext→SPIR-V (glslang) + Pipeline-Kompilierung im Draw-Loop aus (Warm-up existiert, deckt aber nur angemeldete IDs ab, 7659). Der Sky-Fragment-Shader (großer GL-Text) wird bei jedem Start erneut zu SPIR-V kompiliert; das Rauschvolumen wird bei jedem Start neu berechnet (`HE::BuildSkyNoise3D(256)`, deterministisch).
- **Code-Auszug**:
```cpp
vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe)
```
- **Lösung**: Ein `VkPipelineCache` Member (Initialdaten aus `<userdata>/vk_pipeline_cache.bin`, beim Shutdown zurückschreiben) an alle Create-Aufrufe geben; Sky-SPIR-V und Rauschvolumen nach Hash der Eingabe auf Platte cachen (oder Rauschen als gepackte Asset-Datei liefern). Pipeline-Build optional in einem Job (`JobSystem`) und der Draw wird bis dahin übersprungen/Built-in gezeichnet.
- **Erwarteter Effekt**: (Schätzung) Start- und Material-Hitch-Zeiten sinken von zig-hundert ms auf wenige ms bei warmem Cache; Messung beim Start (`HE_LOG_INFO` Zeitstempel um 9345-9368 und 9145).
- **Risiko / Verifikation**: Niedrig (Cache ist treiberseitig validiert). Cache-Datei bei Treiber-/UUID-Wechsel verwerfen (Header-Check).

### REN-809 – Decals: pro Decal ein Descriptor-Set, `glm::inverse`, UUID-Cache-Lookup
- **ID**: REN-809
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 3986-4058 (`EncodeDecals`), 3593-3607 (`resolveDecalTexture`)
- **Problem**: Pro Decal: `vkAllocateDescriptorSets` + 3 Writes + `vkUpdateDescriptorSets`, `glm::inverse(dcl.transform)`, Map-Lookup in `m_decalTexCache`. Das UBO-Slot-Schema (`k_decalSlotStride`) ist bereits da und würde mit Dynamic-Offset-UBO ein einziges Set pro Textur erlauben. Deckel `k_maxDecals` fix.
- **Code-Auszug**:
```cpp
if (vkAllocateDescriptorSets(m_device, &dsai, &set) != VK_SUCCESS) break;
...
vkUpdateDescriptorSets(m_device, 3, w, 0, nullptr);
```
- **Lösung**: Binding 23 auf `UNIFORM_BUFFER_DYNAMIC` ändern, je Textur-View ein gecachtes Set (Key = View-Handle), pro Decal nur UBO-Write + Bind mit Offset. `invModel` aus dem Extraktor liefern lassen.
- **Erwarteter Effekt**: (Schätzung) bei vielen Decals (>50) Reduktion der Descriptor-Arbeit auf ~0; klein bei wenigen.
- **Risiko / Verifikation**: Niedrig-mittel. Decal-Shader-Layout (b19/b22/b23, generiertes SPIR-V) muss Dynamic-UBO tolerieren (Layout-Typ ändert sich, SPIR-V nicht). Decal-Parity-Tests prüfen.

### REN-810 – `updateSkyEnvCube`: 1,5-MB-Vektor pro Neuberechnung + serielle Half-Konvertierung
- **ID**: REN-810
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 3019-3070
- **Problem**: Bei jeder Sonnenverschiebung > 2e-3 (Tag/Nacht-Zyklus ⇒ regelmäßig) wird `std::vector<float> px(128*128*6*4)` (~1,5 MB) allokiert, per `parallel_for` gefüllt und anschließend **seriell** `packHalf1x16` über alle 393k Floats in den Staging-Puffer geschrieben (3036-3037). Allokation + serielle Schleife laufen auf dem Render-Thread.
- **Code-Auszug**:
```cpp
std::vector<float> px(static_cast<size_t>(N) * N * 6 * 4);
...
for (size_t i = 0; i < px.size(); ++i)
    dst[i] = glm::packHalf1x16(px[i]);
```
- **Lösung**: Scratch als Member (einmal allokieren) und die Half-Konvertierung in die `parallel_for`-Zeilenfunktion verlagern (jede Zeile schreibt ihren Zeilenbereich in `dst`). Alternativ `HE::BuildSkyEnvFaceRow` direkt auf Half-Ausgabe erweitern (Cross-Modul, REN-Cross-1).
- **Erwarteter Effekt**: (Schätzung) < 1 ms gespart je Neuberechnung; sichtbar nur bei laufendem Tag/Nacht. Messen.
- **Risiko / Verifikation**: Niedrig; Bytegleichheit des Cube mit Test (`SkyEnvBake.h`-Tests) vergleichbar.

### REN-811 – `findMemoryType` fragt die Speichertypen bei jedem Aufruf neu ab
- **ID**: REN-811
- **Kategorie**: D
- **Schweregrad**: Mittel (häufig, aber billig)
- **Typ**: Performance + Vereinfachung
- **Ort**: 1644-1652 (58 Aufrufstellen)
- **Problem**: `vkGetPhysicalDeviceMemoryProperties` wird bei jeder Allokation aufgerufen, auch im Laufzeitpfad (Mesh-/Textur-Upload, GI-Buffer-Wachstum). Funktion wirft eine Exception bei Fehlschlag, was an einigen Stellen mit `try/catch (...)` umschifft wird (10406-10407).
- **Code-Auszug**:
```cpp
VkPhysicalDeviceMemoryProperties mp{};
vkGetPhysicalDeviceMemoryProperties(m_physDevice, &mp);
```
- **Lösung**: Einmal in `pickPhysicalDevice` in ein Member `m_memProps` kopieren; zusätzlich eine zentrale `allocBuffer(size, usage, memFlags, Buffer&)` / `allocImage(...)` Hilfe (REN-815), damit die 58 Stellen verschwinden.
- **Erwarteter Effekt**: gering (µs pro Aufruf), aber Grundlage für REN-815.
- **Risiko / Verifikation**: Sehr niedrig.

### REN-812 – Redundante State-Binds im Built-in-/Material-Draw-Loop
- **ID**: REN-812
- **Kategorie**: B / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: 8575-8584, 8623-8637, 8656-8681, 7855 (36× `vkCmdBindDescriptorSets` in der Datei)
- **Problem**: Nach **jedem** Graph-Material-Draw: Pipeline-Rebind der Szenenpipeline + Neubinden von Set 0 (8576-8584). Im Built-in-Pfad pro Draw: Set 0 (mit Dynamic Offset), Set 2 (Albedo), Vertex-/Index-Buffer-Bind, auch wenn Mesh/Material/Albedo gleich dem Vorgänger sind (die Sortierung `RenderSorter` gruppiert nach Mesh). Kein State-Cache (zuletzt gebundene Pipeline/Set/Mesh).
- **Code-Auszug**:
```cpp
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeMatScenePipe);
vkCmdBindDescriptorSets(cmd, ..., m_scenePipelineLayout, 0, 1, &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);
```
- **Lösung**: Kleiner `struct BindState { VkPipeline pipe; VkDescriptorSet set0, set2; uint32_t dynOff; VkBuffer vb, ib; }` im Frame, mit Hilfsfunktionen `bindPipeline/bindSet/bindMesh`, die nur bei Änderung `vkCmd*` aufrufen (analog `UIPass::bindSet`, 14274-14280). Der Re-Bind nach Material-Draw entfällt, weil die nächste Bind-Funktion den Stand kennt. Hinweis: Set 0 des Material-Layouts ist zu Set 0 des Szenen-Layouts inkompatibel; der State-Cache muss das als Invalidierung (`set0 = null`) werten.
- **Erwarteter Effekt**: (Schätzung) 20-50 % weniger Bind-Aufrufe in Szenen mit vielen Instanzen derselben Mesh/Material; Wirkung vor allem CPU/Treiber.
- **Risiko / Verifikation**: Niedrig-mittel. Layout-Kompatibilität (Set 0) genau beachten; Validation-Layer-Lauf.

### REN-813 – Viewport-Resize/Invalidierung: mehrfach `vkDeviceWaitIdle` + `vkQueueWaitIdle` pro Änderung
- **ID**: REN-813
- **Kategorie**: E
- **Schweregrad**: Mittel (Editor)
- **Typ**: Performance
- **Ort**: 516-518 + 5237 (`destroyPostFXResources` wartet erneut) + 5548 (TAA-Prime) + 12880/13414 (`primeShaderReadLayout` je Bild: 6× pro `createSSRTargets`) + 7583 (`processPendingInvalidations`) + 6123/6169 (`RenderSceneImage`) + 6551 (`RenderWorldPreview` pro Aufruf), 5933 (`CaptureViewport`)
- **Problem**: Eine Viewport-Größenänderung (Ziehen eines Docks erzeugt pro Frame eine neue Größe) löst `vkDeviceWaitIdle` in `Render()`, dann nochmals in `destroyPostFXResources`, dann Neuanlage von HDR/Bloom/LDR/TAA/SSAO/Decal-Depth; die Prime-Helfer submitten jeweils einzeln mit `vkQueueWaitIdle`. `processPendingInvalidations` idlet bei jeder Material-/Mesh-/Textur-Invalidierung (während Terrain-Sculpt jeden Frame, wie der Kommentar 7580-7582 selbst notiert).
- **Code-Auszug**:
```cpp
void VulkanRenderer::destroyPostFXResources()
{
    vkDeviceWaitIdle(m_device);
```
- **Lösung**: (1) Editor-seitig Resize entprellen (nur anwenden, wenn die Größe ≥ N ms stabil ist, oder auf 8-px-Raster runden – Cross-Modul). (2) Einen „retire list“ (wie `m_retiredViewports`, 443-454) für Bilder/Framebuffer/Views erweitern: statt Idle werden Ressourcen N Frames später freigegeben (D3D12-Weg laut Kommentar 7582). (3) `primeShaderReadLayout` zu einem Batch (alle Bilder in einem CB) bündeln. (4) Doppelten Idle entfernen: `destroyPostFXResources` wird nur aus bereits wartenden Pfaden aufgerufen (`createViewportResources` hinter Idle) – der Kommentar 13398-13401 verbietet das Entfernen **upstream**, deshalb besser den Idle dort belassen und in `destroyPostFXResources` weglassen, sofern alle Aufrufer vorher warten (`Shutdown` tut es, 222).
- **Erwarteter Effekt**: (Schätzung) deutlich flüssigeres Resize im Editor; eliminiert wiederholte GPU-Leerläufe.
- **Risiko / Verifikation**: Mittel – wegen der dokumentierten Sicherheitsannahmen (Kommentar 13398-13401). Nur mit Validation-Layer + Resize-Stresstest ändern.

### REN-814 – Pipeline-State-Boilerplate und HDR/LDR-Doppelaufbau dupliziert
- **ID**: REN-814
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: 2413-2545 (`createScenePipeline`) vs. 5016-5094 (HDR-Kopie derselben Pipelines), 3209-3301, 3733-3804, 4280-4337, 4863-4896, 4948-5003, 5321-5441, 6316-6358, 9378-9441, 9633-9719, 10898-10985, 12398-12514, 13228-13287, 13846-13935, 14170-14241
- **Problem**: 16× ein vollständiger Satz `ia/vp/rs/ms/ds/cba/cb/dyn` mit praktisch identischen Werten (Dreiecksliste, Cull None, CCW, Dynamic Viewport/Scissor). `createPostFXPipelines` baut die Scene-Pipelines (HDR, Instanced HDR, Transparent HDR) als zweite handgeschriebene Kopie (5016-5094) mit eigenen Variablennamen (`vi2`, `ia2` …) statt `createScenePipeline` mit einem `VkRenderPass`-Parameter aufzurufen. Änderungen am Zustand (z. B. Culling) müssen so an ~16 Stellen nachgezogen werden.
- **Code-Auszug**:
```cpp
VkPipelineViewportStateCreateInfo vp2{ ... }; vp2.viewportCount=1; vp2.scissorCount=1;
VkPipelineRasterizationStateCreateInfo rs2{ ... }; rs2.polygonMode=VK_POLYGON_MODE_FILL; rs2.cullMode=VK_CULL_MODE_NONE; ...
```
- **Lösung**: Kleiner `GraphicsPipelineBuilder` (Defaults: Dreiecksliste, FILL, CULL_NONE, CCW, 1 Sample, Dynamic Viewport/Scissor; Setter für Vertex-Input, Depth, Blend-Preset (None/Alpha/Premult), Layout, RenderPass, Shader-Stages; `build(cache)`), und `buildScenePipelineSet(VkRenderPass rp, Pipelines& out)` für LDR und HDR. Optional eine `ShaderModuleGuard`-RAII-Klasse (`makeModule` ist 4× kopiert: 3118, 3715, 4461, 13207).
- **Erwarteter Effekt**: -1500 bis -2500 Zeilen; Konsistenz; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Niedrig; Render-Pass-Kompatibilität bleibt, Pipeline-Parameter 1:1 übernehmen. Smoke-Test + Bild-Hash.

---

## Mittel (Vereinfachung / Struktur)

### REN-815 – Buffer-/Image-/One-Shot-Boilerplate in ~50 Kopien
- **ID**: REN-815
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: z. B. 2141-2168, 2220-2234, 2852-2865, 3699-3711, 4429-4442, 6226-6256, 7110-7129, 7153-7182, 10181-10224, 10391-10417, 12158-12170, 13101-13114, 13986-14007; One-Shot-Submit: 2739-2765, 4746-4764, 5525-5548, 7270-7315, 7439-7492, 9194-9246, 9850-9904, 9999-10053, 11266-11293, 11420-11450, 12018-12043, 12124-12149, 12864-12881, 13176-13203, 14441-14495
- **Problem**: Das Muster „`vkCreateBuffer` → `GetBufferMemoryRequirements` → `findMemoryType` → `vkAllocateMemory` → `vkBindBufferMemory` → `vkMapMemory`“ steht ~25× fast wörtlich in der Datei (Lambda-Namen `makeBuf`, `makeSSBO`, `makeHostBuf`, `ensure`, `ensurePreviewRing`, `uploadGiBuffer`, `createGiHwBuffer`, `ssaoMakeImage` …). Dasselbe für Bilder (`makeImg` ×6) und One-Shot-Command-Buffer (21×). Fehlerpfade sind uneinheitlich (`vkCheck` wirft vs. `return false` vs. unbehandelt, z. B. 9156-9168, 9814-9826, 10061-10066: Rückgabewerte nicht geprüft).
- **Lösung**: `VkBufferEx createBuffer(size, usage, MemKind{DeviceLocal, HostVisibleCoherent, HostMapped})`, `createImage2D(desc)` und `oneShot(std::function)` bzw. Template-Variante (im Cold-Path ok) als private Helfer oder eigener `VulkanResources.h`. Fehlerbehandlung vereinheitlichen. Basis für REN-802/803.
- **Erwarteter Effekt**: -1500 Zeilen, ein Ort für Allokator-Umstellung.
- **Risiko / Verifikation**: Niedrig; mechanische Umstellung, stückweise.

### REN-816 – Handle-Lebenszeit manuell: `Shutdown()` und ~40 `destroy*`-Funktionen mit `if (h) { vkDestroy…; h = VK_NULL_HANDLE; }`
- **ID**: REN-816
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: 219-407 (`Shutdown`, 190 Zeilen), 1778-1787, 2611-2657, 2901-2926, 3840-3862, 4523-4546, 5099-5126, 5235-5261, 6396-6410, 6501-6529, 9451-9474, 9721-9737, 10354-10385, 11298-11316, 13306-13349
- **Problem**: Reihenfolgeabhängige manuelle Zerstörung (z. B. „destroyDecalPipelines vor destroyScenePipeline – borrows m_albedoSampler“, 378; „SSR vor destroySSAOTargets“, 379-381). Reihenfolgefehler sind nur per Kommentar abgesichert; Leaks bei neuen Members sind leicht (z. B. `m_whiteArrayView` wird nur in `destroyMaterialResources` freigegeben, das nur bei `m_matReady` sinnvoll durchläuft).
- **Lösung**: Kleine RAII-Wrapper (`Unique<VkImageView>`, `UniqueBuffer {buf, mem}`, `UniqueImage {img, mem, view}`) mit `VkDevice`-Referenz oder Deleter; ein `Frame/DeviceScope`, das beim Verlassen in umgekehrter Deklarationsreihenfolge freigibt. Dadurch `Shutdown` auf wenige Zeilen.
- **Erwarteter Effekt**: Weniger Fehleranfälligkeit; -600 Zeilen; Verhalten gleich.
- **Risiko / Verifikation**: Mittel (Destruktionsreihenfolge der Member in `VulkanRenderer.h` ist dann semantisch relevant). Mit Validation-Layer-Shutdown-Lauf prüfen.

### REN-817 – `DrawViewportFrame` (~480 Zeilen) und `drawDCVk` (~460 Zeilen) als Monolithen mit Flag-Parametern
- **ID**: REN-817
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: 710-1193 (`DrawViewportFrame`), 8230-8690 (`drawDCVk`-Lambda in `DrawScene`, ~1000 Zeilen mit Rest), 1109-1131 vs. 1165-1186 (UI-Canvas-Block 2× identisch), 653-661 (3. Variante), 844-965 (Unterwasser-Fog + Caustics inline)
- **Problem**: Verschachtelte Lambdas (`blitPass`, `depthBarrier`, `drawDCVk` ⊃ `noteRingFull`, `drawMatInstance`, `wr`, `drawOne`), Zustandsflags `hdr`, `gbufferPass`, `deferred`, `allowInstancing`, `activeMatScenePipe` im Closure; schwer testbar. `DrawScene` ist >1000 Zeilen. Der UI-Canvas-Block ist zweimal vollständig kopiert; Viewport-/Scissor-Setzen mit `VkViewport vp{...}` ist ≥30× wörtlich wiederholt.
- **Lösung**: Aufteilen in `encodeUnderwater(cmd)`, `encodeBloom(cmd)`, `encodeUiCanvasOntoViewport(cmd)`, `encodeTonemapAndAA(cmd)`; `drawDCVk` → Methoden `drawGraphMaterialDC`, `drawBuiltinDC` mit einer `DrawPassState`-Struktur statt Closure-Flags. `setViewportScissor(cmd, w, h)` Helfer.
- **Erwarteter Effekt**: Lesbarkeit/Reviewbarkeit; kein Laufzeiteffekt (Methoden-Calls gegenüber Lambdas gleich).
- **Risiko / Verifikation**: Niedrig, wenn mechanisch und mit Render-Hash-Tests abgesichert; zuerst REN-801 festlegen, dann refaktorieren, sonst doppelte Arbeit.

---

## Niedrig

### REN-818 – `SetDebugLines` kopiert den Vektor; danach zweite Kopie beim Packen
- **ID**: REN-818
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: 9789-9792, 9757-9768
- **Problem**: `m_debugLines = lines;` kopiert pro Frame alle Debug-Linien (Allokation bei Wachstum), `drawLineList` schreibt dann nochmals in den gemappten Buffer. Das Interface (`IRenderer::SetDebugLines(const std::vector<DebugLine>&)`) erzwingt die Kopie; D3D11:8517, D3D12:12659, GL:10091 haben dieselbe Form.
- **Lösung**: Direkt in den gemappten Frame-Ring des **nächsten** Frame-Slots packen oder `assign` (kein Realloc dank bleibender Kapazität); mittelfristig Interface auf `std::span`/Move ändern (Cross-Modul).
- **Erwarteter Effekt**: klein; relevant bei vielen tausend Debug-Linien (Physik-Visualisierung).
- **Risiko / Verifikation**: Niedrig.

### REN-819 – Pro-Frame-Heap im Draw-Pfad (`opaqueDCs`, `transparentDCs`, Ribbon-Container, `writes`)
- **ID**: REN-819
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: 8140 (`std::vector<const DrawCall*> opaqueDCs, transparentDCs`), 8153-8155 (`ribbonDCs`, `ribbonMeshes`, `std::unordered_map<const DrawCall*, const GpuMesh*> ribbonMeshByDC`), 8229 (`deferredForwardDCs`), 11589 (`std::vector<VkWriteDescriptorSet> writes` ohne `reserve`), 6569-6570 (Preview), 3026 (siehe REN-810)
- **Problem**: Jeder Frame allokiert diese Vektoren neu (Kapazität geht verloren). Die Ribbon-Map ist eine Pointer-gekeyte `unordered_map` mit Knoten-Allokation pro Trail.
- **Lösung**: Als Member halten und `clear()` (Kapazität bleibt); für Ribbons eine `std::vector<std::pair<const DrawCall*, const GpuMesh*>>` oder ein Indexfeld in `DrawCall` (siehe Cross-Modul-Hinweise). `writes.reserve(64)` bzw. festes Array.
- **Erwarteter Effekt**: wenige Allokationen pro Frame weniger; sehr klein.
- **Risiko / Verifikation**: Niedrig. Achtung: `transparentDCs` hält Zeiger in `ribbonDCs`; dessen `reserve` (8160) muss bestehen bleiben.

### REN-820 – Editor-/Capture-Pfade blockieren und holen Env-Variablen pro Aufruf
- **ID**: REN-820
- **Kategorie**: E
- **Schweregrad**: Niedrig (Editor)
- **Typ**: Performance
- **Ort**: 5905-5993 (`CaptureViewport`: Staging pro Aufruf + `vkDeviceWaitIdle` + `vkQueueWaitIdle`), 6531-6802 (`RenderWorldPreview`: pro Aufruf `vkDeviceWaitIdle`, `vkAllocateCommandBuffers`, `vkQueueWaitIdle`, `std::getenv` ×2, Grid-Vektoren neu: 6674-6682), 6050-6215 (`RenderSceneImage`)
- **Problem**: Pro Preview-Tile (Asset-Browser kann viele Slots aktualisieren) volle GPU-Idle-Synchronisation; `buildPreviewGrid` erzeugt pro Aufruf zwei Vektoren und kopiert sie in `DebugLine`.
- **Lösung**: Preview in den normalen Frame-CB einhängen (Fence pro Slot statt Device-Idle), Grid pro (Extent, Origin) cachen; `getenv` einmal in Initialize lesen. Für `CaptureViewport` Staging-Buffer cachen und Fence statt Queue-Idle nutzen.
- **Erwarteter Effekt**: Editor-Ruhe beim Preview-Update; nur im Editor.
- **Risiko / Verifikation**: Mittel (Synchronisation mit Preview-Rings, 6585-6603). Preview-Dump-Test (`HE_WORLD_PREVIEW_DUMP`) beibehalten.

### REN-821 – Doppelter Swapchain-Aufbau für Sekundärfenster, `renderWindowData` ohne OUT_OF_DATE-Behandlung
- **ID**: REN-821
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: 1809-1935 (`createWindowData`) vs. 1536-1595 + 1654-1717 (Primär); 1956-2012
- **Problem**: Format-/Extent-Wahl, Swapchain-/View-/Renderpass-/Framebuffer-Erzeugung sind für Primär- und Sekundärfenster kopiert (Unterschied: Sekundär wählt nur `B8G8R8A8_UNORM`, keine R8G8B8A8-Alternative, kein Depth; `fmts[0]` bei leerer Liste UB). Der Sekundärpfad fängt `VK_ERROR_OUT_OF_DATE_KHR` nicht ab (`vkCheck` wirft) und `vkQueuePresentKHR` wird ignoriert; laut 2038-2050 und `GetCapabilities` ist das Feature ohnehin deaktiviert (`supportsSecondaryWindows == false`).
- **Lösung**: `createSwapchainFor(surface, SwapchainData&)` als gemeinsame Funktion; Sekundärpfad entweder fertigstellen (Recreate) oder als „nicht unterstützt“ löschen, bis A5 umgesetzt ist (Toter Code ~200 Zeilen).
- **Erwarteter Effekt**: -150 bis -200 Zeilen.
- **Risiko / Verifikation**: Niedrig; Pfad ist laut Kommentar nicht erreichbar.

### REN-822 – Veraltete Kommentare, tote Reste, ungenutzte Parameter und Includes
- **ID**: REN-822
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: 
  - 8969-8971: „NOTE: a single per-frame bone UBO means only the LAST skinned draw's bone pose is visible …“ – widerspricht dem direkt darunter implementierten Dynamic-Offset-Ring (8982-9011).
  - 10150-10152: „GetCapabilities keeps supportsGlobalIllumination = false“ – tatsächlich `true` (1207).
  - 11, 37: `<glm/gtc/packing.hpp>` doppelt inkludiert.
  - 1287: `(void)0;` Rest; 216: eingerückte Zeile mit Tab `m_shaderManager = VulkanShaderManager();` (überschreibt am Ende von `Initialize` den Member, sofern vorher benutzt – unsicher, ob beabsichtigt).
  - 11891-11896 (`ssaoMakeImage`): Parameter `memTypeBits` wird immer mit `0` aufgerufen und nie benutzt; die Suche geht über einen `std::function`-Callback (Overhead + Hürde), obwohl `findMemoryType` eine Memberfunktion ist.
  - 9476 (`drawSky`): `width/height` ungenutzt; `Render()` 1286-1288.
  - 1304-1308: Meldung „(HE_GPU_DEBUG)“ ohne dass die Variable ausgewertet wird (siehe REN-804).
- **Lösung**: Kommentare korrigieren/löschen, doppelte Includes entfernen, `ssaoMakeImage` in Memberfunktion ohne `std::function` und `memTypeBits` umwandeln.
- **Erwarteter Effekt**: Wartbarkeit.
- **Risiko / Verifikation**: Sehr niedrig.

---

## Gelesen

Ja: `VulkanRenderer.cpp` vollständig (Zeilen 1–14698, in Blöcken von 500-1500 Zeilen, nichts übersprungen). Ergänzend gezielt gegengelesen: `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-1009`, `src/HE_Core/src/MaterialGraph/MaterialGraph.cpp:1981-1990`, `include/Backends/Vulkan/VulkanRenderer.h` (nur Zeilen für Konstanten/Member per `grep`), sowie `grep`-Stichproben in D3D11/D3D12/GL/Metal für die Cross-Modul-Hinweise.

## Rendering Slice 9 – Shared Rendering-Header/-Sources (CommandBuffer, Foliage/Frustum/GI/Light-Packing, RenderWorld/-Object/-Sorter, Sky*, VulkanRenderer.h, VulkanShaderManager, MaterialShaderBake)

## Zusammenfassung

Der Slice (50 Dateien, ca. 9,5k Zeilen; davon 2568 Zeilen `SkyShaderSource.h` und 1566 Zeilen `VulkanRenderer.h`) ist insgesamt gut kommentiert und bereits stark von Cross-Backend-Duplikation befreit (LightPacking, ClipSpace, GIProbeGrid, SkyFrameParams). Die relevanten Befunde liegen bei der Draw-Call-Aufzeichnung (tiefe Kopien der Instanzlisten), beim pro Frame neu aufgebauten Cluster-Licht-Grid, bei GPU-seitig wiederholt berechneten Sky-Konstanten (Atmosphere, Aurora) und bei kleineren Per-Frame-Allokationen im Vulkan-Header (String-Keys, `unordered_map` fuer TAA). Es gibt 3 Funde mit Schwere Hoch, 9 Mittel und 11 Niedrig (gesamt 23). Alle Zeilenangaben wurden gegen den Stand der Dateien geprueft; GPU-Kosten (Sky-Shader) und Rebake-Haeufigkeit sind ohne Messdaten als "messen" markiert. Backend-Rendererdateien (`*Renderer.cpp/.mm`) gehoeren nicht zu diesem Slice, wurden aber gezielt per Grep zur Verifikation von Aufrufstellen angesehen.

## Funde

### REN-901 – CommandBuffer::recordDraw/recordSkinnedDraw kopieren DrawCall samt Instanz- und Bone-Vektoren
- **ID**: REN-901
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/CommandBuffer.cpp:15-23`, `include/HorizonRendering/CommandBuffer.h:52,57,67,76-77`; Aufrufer `src/HE_Rendering/src/RenderPass.cpp:89,117,198,218,252`
- **Problem**: `recordDraw(const DrawCall&)` macht `push_back(call)` – eine tiefe Kopie von `instanceTransforms` (bis `kMaxInstancesPerDraw` = 65536 `mat4` = 4 MiB), `paramOverride` und bei `SkinnedDrawCall` von `boneMatrices`. Der Aufrufer (`RenderPass.cpp:137-140`) baut die Liste vorher selbst auf (`reserve` + `appendInstances`), kopiert sie dann in den CommandBuffer und zerstoert das Original gleich danach. Bei Foliage-Batches entstehen so pro Frame zwei volle Durchlaeufe ueber alle Matrizen plus eine Heap-Allokation pro Draw. `reset()` (`CommandBuffer.cpp:5`) ruft `clear()` und gibt damit auch die Vektor-Kapazitaeten aller DrawCalls frei, es wird nichts ueber Frames wiederverwendet.
- **Code-Auszug**:
```cpp
void CommandBuffer::recordDraw(const DrawCall& call) { m_drawCalls.push_back(call); }
void CommandBuffer::recordSkinnedDraw(const SkinnedDrawCall& call) { m_skinnedDrawCalls.push_back(call); }
```
- **Lösung**: (1) Move-Overloads `recordDraw(DrawCall&&)` / `recordSkinnedDraw(SkinnedDrawCall&&)` ergaenzen und in `RenderPass.cpp` mit `std::move(dc)` bzw. `std::move(chunk)` aufrufen (der Lambda `recordRun` muss dazu den letzten Abschnitt bewegen statt kopieren). (2) Optional: Chunk-Pfad `RenderPass.cpp:165` direkt per `emplace_back` in den CommandBuffer bauen und `instanceTransforms` dort `assign`en, statt ueber `chunk` zu gehen. (3) Optional spaeter: `instanceTransforms` aus einer pro Frame wiederverwendeten Arena (ein grosser `std::vector<glm::mat4>` im CommandBuffer + `{first,count}` im DrawCall) statt pro Draw ein Vektor; das aendert die Backend-Schnittstelle und ist ein eigener Schritt.
- **Erwarteter Effekt**: Schaetzung: bei grossen Foliage-Batches (10^4-10^5 Instanzen) fallen ein voller Matrix-Kopiervorgang (64 B x N) und eine Allokation pro Draw weg; bei Szenen ohne Instancing vernachlaessigbar. Messen mit `HE_PROFILE_SCOPE` um `GeometryPass::execute`.
- **Risiko / Verifikation**: Gering, solange die Backends den CommandBuffer nur lesen. `tests/test_skeletalmeshcomponent.cpp:52-77` ruft `recordDraw(dc)` mit einem Lvalue auf; die `const&`-Overloads muessen bleiben. Sonst vorhandene Render-Graph-/Foliage-Tests (`tests/test_foliage_cluster.cpp`) laufen lassen.

### REN-902 – Section-Expansion kopiert die komplette Instanzliste pro Material-Slot
- **ID**: REN-902
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderPass.cpp:98-118` (`DrawCall sd = d;`), `:209` (`SkinnedDrawCall sd = dc;`); Struktur in `include/HorizonRendering/CommandBuffer.h:15-61`
- **Problem**: Ein Multi-Section-Mesh erzeugt pro Slot einen `DrawCall` per Wertkopie des Basis-DrawCalls – inklusive `instanceTransforms` und `paramOverride`. Ein geclustertes Mesh mit 3 Slots und N Instanzen kopiert die Liste dreimal (plus die Kopie aus REN-901). Fuer jeden Draw desselben Mesh-Runs sind die Instanzdaten aber identisch. Bei skinned Meshes wird zusaetzlich `boneMatrices` pro Section kopiert (`RenderPass.cpp:209`).
- **Code-Auszug**:
```cpp
DrawCall sd        = d;
sd.indexOffset     = sec.indexOffset;
...
outCmds.recordDraw(sd);
```
- **Lösung**: Instanz-/Bone-Daten als geteilte, unveraenderliche Referenz fuehren: `std::shared_ptr<const std::vector<glm::mat4>> instances` (oder `{const mat4* ptr; uint32_t count}` in einer CommandBuffer-eigenen Arena). Dann kopiert `sd = d` nur Zeiger. Kleinere Zwischenloesung ohne Schnittstellenaenderung: nur den ersten Slot kopieren/moven und fuer weitere Slots die Liste ebenfalls per `=` zuweisen (kein Gewinn), daher ist der Zeiger-/Arena-Weg der eigentliche Fix. Die Backends lesen `dc.instanceTransforms` an vielen Stellen (`D3D11/D3D12/Vulkan/GL/Metal`), die Umstellung betrifft alle fuenf Backends.
- **Erwarteter Effekt**: Schaetzung: Faktor = Anzahl Slots weniger Matrix-Kopien bei Multi-Section-Foliage/Instancing; fuer Einzel-Section-Meshes keine Aenderung.
- **Risiko / Verifikation**: Mittel (Backend-Schnittstelle). Absichern: `tests/test_foliage_cluster.cpp`, Section-Tests (`grep -l sectionIndex tests/`), visuelle Gleichheit per Headless-Dump auf mindestens GL und Vulkan.

### REN-903 – BuildClusterLights legt pro Frame 3456 Vektoren an und sammelt Indizes per `push_back`
- **ID**: REN-903
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/LightPacking.cpp:131-245` (besonders `:139-146`, `:224-227`, `:231-237`)
- **Problem**: `std::vector<std::vector<uint32_t>> cells(kClusterCount)` (16x9x24 = 3456 Vektoren, ~83 KB, alle zero-initialisiert) wird in jedem Aufruf angelegt, und jede beruehrte Zelle wird per `push_back` ohne `reserve` gefuellt (bis `kMaxClusterIndices` = 65536 Eintraege, also viele einzelne kleine Allokationen/Reallokationen). Danach wird alles nochmals in `out.indices` kopiert. Der Aufruf passiert pro Frame in jedem Backend (`OpenGLRenderer.cpp:11416`, `D3D11Renderer.cpp:6332`, `D3D12Renderer.cpp:10318`, `VulkanRenderer.cpp:7875`, `MetalRenderer.mm:14920`). Zusaetzlich `glm::inverse(rw.camera.view)` (Z. 141) nur um die Kamera-Vorwaertsrichtung zu holen.
- **Code-Auszug**:
```cpp
std::vector<std::vector<uint32_t>> cells(kClusterCount);
...
cells[(z * kClusterGridY + y) * kClusterGridX + x].push_back(li);
...
out.indices.insert(out.indices.end(), cells[cIdx].begin(), cells[cIdx].end());
```
- **Lösung**: Zwei-Pass-Counting-Sort statt Vektor-pro-Zelle: Pass 1 zaehlt pro Zelle (`std::array<uint32_t,kClusterCount> counts`) waehrend die Lichter durchlaufen werden und merkt sich pro Licht `{z0,z1,y0,y1,x0,x1,li}` (max. 256 Eintraege, Stack); Praefixsumme liefert `grid`-Offsets; Pass 2 schreibt `indices[offset[cell]++] = li`. Keine Heap-Allokation ausser `out.*`; Reihenfolge der Indizes je Zelle (aufsteigend nach Lichtindex) bleibt identisch, weil Lichter in derselben Reihenfolge eingetragen werden. Die Forward-Richtung kann aus der View-Matrix direkt gelesen werden: `-glm::vec3(view[0][2], view[1][2], view[2][2])` ist die dritte Zeile der Rotation und ist gleich `-normalize(inverse(view)[2].xyz)` bei orthonormaler View. Das ist bit-identisch nicht garantiert; Test pruefen.
- **Erwarteter Effekt**: Schaetzung: einige Dutzend bis einige hundert Allokationen pro Frame und Backend weniger, ca. 83 KB weniger Speicher-Churn. Gewinn bei 0-2 Lichtern gering, bei vielen Punktlichtern spuerbar. Messen.
- **Risiko / Verifikation**: Gering-mittel: Ausgabe ist vertragsrelevant (Shader indexieren positionell). Es gibt Tests zu BuildClusterLights (`grep -l BuildClusterLights tests/`); neuen Vergleichstest "alt vs. neu" mit zufaelligen Lichtern schreiben, bevor die Implementierung ersetzt wird. Die `inverse`-Aenderung separat und nur, wenn der Test bit-gleich bleibt.

### REN-904 – Sky: Atmosphaerenintegral wird pro Pixel gerechnet, obwohl es nur von der Richtung und der Sonne abhaengt
- **ID**: REN-904
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonRendering/SkyShaderSource.h:2278-2330` (`atmoScatter`), `:2331-2427` (`skyColor`), Aufrufer `:2179`, `:2166`, `:471` (`overcastDeck`); CPU-Spiegel `include/HorizonRendering/SkyEnvBake.h:47-97`
- **Problem**: `atmoScatter` macht pro Pixel 12 View-Samples mit je 2 `exp` plus einem 5-Sample-Sonnenmarsch mit je 3 `exp` plus `exp(-tau)`, also grob 200 `exp` und mehrere Kugelschnitte je Fragment. `skyColor` wird zusaetzlich in `overcastDeck` (pro Cloud-Pixel unter Overcast), im Cloud-Prepass (`:2166`) und in der Scene-IBL-Variante aufgerufen. Das Ergebnis haengt nur von `dir` (2 Winkel) und `sunDir` (1 Winkel: Sonnenhoehe, da der Aufpunkt `ro` fix ist) ab, aendert sich also nur wenn die Sonne wandert.
- **Code-Auszug**:
```glsl
for (int i = 0; i < 12; ++i) { ... float dR = exp(-hgt / HR) * ds; ... for (int j = 0; j < 5; ++j) { ... exp(-hq / HR) ... } ... exp(-tau) ... }
```
- **Lösung**: Transmittance/Sky-View-LUT nach Hillaire: eine kleine Textur (z. B. 128x64, Azimut-unabhaengig, da nur Hoehenwinkel und Sonnen-Azimutdifferenz zaehlen) wird nur neu gerendert wenn sich `sunDir` aendert (derselbe Trigger wie der IBL-Rebake), `skyColor` sampelt dann. Gleicher Pfad fuer GL/Vulkan/D3D (geteilter Shader) und Metal (Spiegel in `MetalRenderer.mm`). Das ist ein Look-aendernder Eingriff (LUT-Interpolation, Horizont-Clamp), deshalb eigener Schritt mit visuellem A/B.
- **Erwarteter Effekt**: Schaetzung: grosse Entlastung der GPU-ALU bei Vollbild-Sky (Faktor mehrere; die Wolken-/Stern-Marsche dominieren aber im Nachtbetrieb). Messen mit GPU-Timer (`kGpuTimerRing`) bei 1080p/4K, Wolken aus.
- **Risiko / Verifikation**: Hoch fuer Bildgleichheit. `tests/test_sky_shader.cpp` und der Hash-Pin in `tests/test_culling.cpp` (IBL-Bake) muessen bewusst neu gepinnt werden. Nicht ohne Messdaten angehen; vorher pruefen, ob die Sky-Pass-Zeit ueberhaupt relevant ist (Low-Res-Cloud-Pfad existiert bereits).

### REN-905 – Aurora: pro-Kurtinen-Parameter werden pro Pixel neu aus ~27 Hashes berechnet
- **ID**: REN-905
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonRendering/SkyShaderSource.h:1682-1731` (22 Kurtinen x `auroraRnd(fk, s)`), `:1592-1595`
- **Problem**: `auroraRnd(k, s)` ist eine reine Funktion von `k` und `s` (`starHash(vec3(k*0.7331+1.13, s*1.9137+7.71, 19.37))`) – unabhaengig vom Pixel. Trotzdem werden pro Fragment und Kurtine ca. 27 Hashes sowie 6 `sin`/`cos` fuer Layout/Heading/Meander-Parameter berechnet, bevor die drei `auroraSlab`-Tests (`:1739-1741`) die meisten Kurtinen mit `continue` verwerfen. Das sind rund 600 Hashes pro Fragment bei aktiver Aurora (`intensity > 0`, nachts), allein fuer Parameter.
- **Code-Auszug**:
```glsl
for (int k = 0; k < 22; ++k) {
    float rr = (fk + auroraRnd(fk, 0.0)) / 22.0; ... float a1 = scl * (0.55 + 0.85 * auroraRnd(fk, 5.0)); ... (insgesamt auroraRnd(fk,0..26))
```
- **Lösung**: Parameter einmalig auf der CPU (oder pro Draw im Vertex-Shader) berechnen und als Array (`vec4 curtain[22*7]`) im Sky-UBO bzw. als Konstanten-Array ablegen. `SkyFrameParams` hat feste Layout-Garantie (336 B, `static_assert`), also ein zweiter UBO/SSBO oder ein Const-Array im Shader, das beim Shader-Build per String-Splice erzeugt wird (Muster wie `causticsWaveTrainsList()`, `UnderwaterCausticsShared.h:19`). Die Werte sind zeitunabhaengig und deterministisch, damit bleibt das Bild gleich (bis auf Float-Rundung der `sin/cos`, die bei der CPU-Berechnung in Double sogar genauer wird).
- **Erwarteter Effekt**: Schaetzung: Aurora-Nachtframes sparen einen grossen Teil der Per-Kurtine-ALU vor dem Early-Out. Betrifft nur Nacht mit Aurora > 0, daher nicht jeder Frame. Messen.
- **Risiko / Verifikation**: Mittel: Float-Genauigkeit CPU vs. GPU-`fract` im Hash (`starHash` nutzt `fract`/`dot` – bitgleiche CPU-Reproduktion ist schwer). Alternative ohne Bit-Anspruch: Parameter in einem Vorpass in eine 1D-Textur rendern. Visuell vergleichen (Aurora-Szene, mehrere `uTime`).

### REN-906 – graphTexKey baut pro Draw und Texturslot einen std::string
- **ID**: REN-906
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/Backends/Vulkan/VulkanRenderer.h:612,624-627`; Aufrufer `src/Backends/Vulkan/VulkanRenderer.cpp:3609-3624` (`resolveGraphTexture`), `:8338,8349,8351` (pro Graph-Material-Draw)
- **Problem**: `graphTexKey` erzeugt aus der UUID `std::to_string(id.hi) + ":" + std::to_string(id.lo)` (bis ~40 Zeichen, also ueber der SSO-Grenze → Heap-Allokation), `resolveGraphTexture` haengt fuer Array-Slots noch `"#arr"` an und sucht dann in einer `unordered_map<std::string,…>` (`m_graphTexCache`). Das passiert pro Draw fuer bis zu 4 Projekt-Texturen plus die Landscape-Weightmap (leerer `std::string{}` wird je Aufruf mitgebaut). Dieselbe Funktion bedient `m_uiImageCache` (`VulkanRenderer.cpp:7592-7595,14555`) pro UI-Quad mit Bild.
- **Code-Auszug**:
```cpp
static std::string graphTexKey(const HE::UUID& id, const std::string& path)
{ return id != HE::UUID{} ? (std::to_string(id.hi) + ":" + std::to_string(id.lo)) : path; }
```
- **Lösung**: Schluessel als Struct `{HE::UUID id; bool array;}` fuer den UUID-Fall (`std::hash<HE::UUID>` existiert bereits fuer `m_materialTexCache`) und eine zweite, seltener genutzte `unordered_map<std::string,…>` nur fuer den Pfad-Fall (lose Editor-Assets). Alternativ die aufgeloesten `VkImageView`s pro Material einmal cachen (Material-ID → `std::array<VkImageView,4>`), dann entfaellt der Lookup pro Draw ganz. Die Invalidierung (`InvalidateTexture`) muss beide Maps bedienen.
- **Erwarteter Effekt**: Schaetzung: pro Graph-Material-Draw 4-5 Heap-Allokationen + String-Hashing weniger; skaliert mit der Draw-Zahl (`k_matMaxDraws` = 1024). Gering bei wenigen Graph-Materialien. Messen mit Draw-lastiger Szene.
- **Risiko / Verifikation**: Gering. Die Parallelstellen in `OpenGLRenderer.cpp` (`ResolveGraphTexture`, gleicher "hi:lo"-Schluessel laut Kommentar `VulkanRenderer.h:607`) und Metal haben vermutlich dasselbe Muster; in einem Zug angleichen.

### REN-907 – GIProbeSceneSignature laeuft jeden Frame ueber alle Objekte, in jedem Backend
- **ID**: REN-907
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonRendering/GIProbeGrid.h:121-132`, `:140-150` (Tracker); Aufrufer `OpenGLRenderer.cpp:6366`, `VulkanRenderer.cpp:11327`, `D3D11Renderer.cpp:3417`, `D3D12Renderer.cpp:7601`, `MetalRenderer.mm:8977`
- **Problem**: Der "O(1)-Skip" des Trackers (`canSkip`, Kommentar `:135`) ist nur O(1) *nach* dem Signaturaufruf; die Signatur selbst ist O(n) mit einem Splitmix-Finalizer (zwei 64-Bit-Multiplikationen) je Objekt, jeden Frame, auch wenn nichts geaendert wurde. Bei 50k Objekten (Zahl aus `RenderExtractor.h:190`) sind das 50k Hashes pro Frame nur fuer einen Gleichheitsvergleich. Der Extractor weiss bereits, ob sich die Struktur geaendert hat (`HorizonWorld::structureEpoch`, `RenderExtractor.h:203,349`).
- **Code-Auszug**:
```cpp
for (const RenderObject& obj : objects) { uint64_t h = (uint64_t(obj.entityId) << 32) ^ obj.meshAssetId.hi ^ (...); h ^= h >> 31; h *= 0x94D0...; h ^= h >> 29; sum += h; }
```
- **Lösung**: Signatur nur neu berechnen, wenn `RenderExtractor::fullExtractCount()` sich aendert (oder besser die Signatur im Extractor waehrend der Objektschleife mitzaehlen und in `RenderWorld` ablegen: ein Feld `uint64_t geometrySignature`). `GIProbeSceneSignature` bleibt als Referenz fuer Tests. Die LOD-/Mesh-Wechsel-Erkennung bleibt erhalten, weil die Signatur weiter von (entityId, meshAssetId) abhaengt.
- **Erwarteter Effekt**: Schaetzung: O(n) → O(1) pro Frame und Backend (nur einer der fuenf Backends laeuft zur Laufzeit). Relevant erst bei sehr grossen Szenen; Wert ~ n x ~2 ns, also ~0,1 ms bei 50k. Niedrige Prioritaet, wenn nicht GI-lastig.
- **Risiko / Verifikation**: Gering; Tests zu `GIProbeGridTracker` (`grep -l GIProbeGridTracker tests/`) decken Signatur-Aenderungen ab.

### REN-908 – Sky-IBL-Rebake: 98k CPU-Atmosphaerenintegrale, frische Allokation, 5-fach kopierte Treiberlogik
- **ID**: REN-908
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `include/HorizonRendering/SkyEnvBake.h:47-97,198-209`; Aufrufer `VulkanRenderer.cpp:3021-3040` (Parallelstellen `OpenGLRenderer.cpp:77`, `D3D11Renderer.cpp:4626`, `D3D12Renderer.cpp:8980`, `MetalRenderer.mm:13833`)
- **Problem**: (1) Jede Sonnenbewegung > 2e-3 loest einen Rebake von 6 x 128 x 128 = 98304 Texeln aus; jeder Texel ruft `SkyColorCPU` → `AtmoScatterCPU` (12x(2 `exp` + 5x3 `exp`) plus `pow` fuer Mie-Phase und drei `pow(s, …)` fuer die Sonne). Bei Zeitraffer/Tag-Nacht-Zyklus mit schneller Sonne wird das jeden Frame fällig. Die Ambient-Cubemap ist niederfrequent (Kommentar `VulkanRenderer.cpp:3015-3019`). (2) `std::vector<float> px` (1,5 MiB) wird pro Rebake neu angelegt (`:3027`), danach serielle Halbfloat-Konvertierung ueber 393k Floats (`:3036-3037`). (3) `BuildSkyEnvFaceRow` normalisiert `dir` doppelt (`SkyEnvBake.h:205` und `:103`). (4) Dieselbe "px-Vektor + parallel_for + Konvertierung"-Logik steht fuenfmal in den Backends.
- **Code-Auszug**:
```cpp
std::vector<float> px(static_cast<size_t>(N) * N * 6 * 4);
parallel_for(static_cast<size_t>(6) * N, [&](size_t idx){ ... HE::BuildSkyEnvFaceRow(N, f, t, sunDir, &px[...]); }, "SkyEnvBake", 1);
for (size_t i = 0; i < px.size(); ++i) dst[i] = glm::packHalf1x16(px[i]);
```
- **Lösung**: (a) Einen geteilten Helfer `HE::BakeSkyEnvCube(N, sunDir, scratch)` in `SkyEnvBake.h` (oder `SkyEnvBake.cpp`) mit wiederverwendetem Scratch-Buffer ergaenzen; die Backends rufen nur noch den Helfer. (b) Doppelte `normalize` in `BuildSkyEnvFaceRow` entfernen (`SkyColorCPU` normalisiert intern). (c) Konvertierung nach Halffloat in die Zeilenfunktion verlagern (parallel). (d) Messen, ob 64² Faces statt 128² visuell reichen (Faktor 4); das aendert Hash-Pin in `tests/test_culling.cpp`.
- **Erwarteter Effekt**: Schaetzung: (a)-(c) sparen die 1,5 MiB-Allokation pro Rebake und die serielle Konvertierung (einige 100 µs bis ms); (d) viertelt die Bake-Zeit. Ohne Messdaten (Rebake-Frequenz bei Zeitraffer, Zeit pro Bake) nicht priorisieren.
- **Risiko / Verifikation**: (a)-(c) ohne Bildaenderung (Hash-Pin in `tests/test_culling.cpp` muss unveraendert bleiben). (d) aendert die Ambient-Beleuchtung auf allen Plattformen.

### REN-909 – Fette AoS-Strukturen: Culling liest 24 Byte pro ~300-Byte-Objekt, DrawCall dupliziert RenderObject
- **ID**: REN-909
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonRendering/RenderObject.h:30-129`, `include/HorizonRendering/CommandBuffer.h:15-61`, `src/HE_Rendering/src/FrustumCuller.cpp:61-65`, `include/HorizonRendering/RenderWorld.h:362`
- **Problem**: `RenderObject` enthaelt u. a. `mat4` (64 B), zwei UUIDs (32 B), `HE::AABB`, `vector<RenderSection>` (24 B), `vector<float> paramOverride` (24 B), Handles, PBR-Skalare, Tint, Flags: grob 300+ Byte. `FrustumCuller::cull` iteriert parallel ueber `world.objects[i].worldBounds` und liest pro Objekt nur die AABB, aber jede Zeile liegt in einer eigenen Cache-Line-Gruppe; bei 50k Objekten sind das 15 MB Stride-Durchlauf fuer 1,2 MB Nutzdaten. `DrawCall` dupliziert danach ca. 17 dieser Felder (`GeometryPass`) inklusive zweier Vektoren, obwohl die meisten Objekte keine Sections/Overrides haben (leere Vektoren kosten nur 48 B, aber verhindern trivial-copyable Strukturen).
- **Code-Auszug**:
```cpp
parallel_for(count, [&](size_t i) {
    const HE::AABB& bounds = world.objects[i].worldBounds;
    if (bounds.isValid()) outVisible[i] = frustum.intersects(bounds) ? 1u : 0u;
}, "FrustumCull");
```
- **Lösung**: Hot/Cold-Split: `RenderWorld` fuehrt eine parallel gepflegte `std::vector<HE::AABB> objectBounds` (vom Extractor gefuellt, von `refineWorldBounds` aktualisiert) fuer den Cull; oder `RenderObject` in `RenderObjectHot` (Bounds, Transform, ids) und `RenderObjectCold` (sections, paramOverride, weightmap, landscape) teilen. Erst nach Messung (`FrustumCuller::cull`-Profilscope, bei >10k Objekten) angehen; die Schnittstelle `RenderObject` wird von allen Backends direkt gelesen.
- **Erwarteter Effekt**: Schaetzung: Cull-Durchlauf bei vielen Objekten 2-4x schneller (Speicherbandbreite). Bei < 5k Objekten irrelevant. Messen.
- **Risiko / Verifikation**: Mittel-hoch (breite Auswirkung). `RenderObject.h:139-141` erwaehnt einen Test (`tests/test_foliage_cluster.cpp`), der Backend-Quellen auf handgeschriebenes "refine" prueft; Aenderungen an `refineWorldBounds` muessen diesen Test erfuellen.

### REN-910 – FoliageExtract: pro Bucket zwei InstanceBlock-Kopien (Atomic-Refcount, 96 B) und pro Layer temporaere Vektoren
- **ID**: REN-910
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/FoliageExtract.cpp:332-335,366-383,385-391,396-423`; `include/HorizonRendering/RenderWorld.h:229-241`
- **Problem**: Pro Bucket (O(Buckets), explizites Designziel der Datei) passiert `InstanceBlock block = layer;` (Zeile 387; kopiert `shared_ptr` mit atomarem Zaehler und eine 64-B-`mat4`) und danach in `emit` `out.instanceBlocks.push_back(block)` (Zeile 380; zweite Kopie, ohne `reserve` des Zielvektors). Dazu pro Layer und Frame neue `std::vector`s `straddlers`/`wholes` (Zeilen 332-335) und ein `std::make_shared` fuer `scratch` bei Straddlern. `out.objects.push_back(obj)` ohne `reserve` mit einem `RenderObject` von ~300 B.
- **Code-Auszug**:
```cpp
InstanceBlock block = layer;
block.first = w.bucket->first;
block.count = w.bucket->count;
emit(block, clusterBounds(*w.bucket), w.nearSq);   // -> out.instanceBlocks.push_back(block);
```
- **Lösung**: `emit(InstanceBlock&& block, …)` und `push_back(std::move(block))`; `out.objects.reserve(out.objects.size() + wholes.size() + straddlers.size())` und dasselbe fuer `out.instanceBlocks`; `wholes`/`straddlers` als `thread_local` oder per Aufrufer uebergebene Scratch-Vektoren (`clear()` statt neu anlegen). Langfristig: `parent` (64 B) nur einmal pro Layer in `RenderWorld` halten und `InstanceBlock` auf `{layerIndex, first, count}` reduzieren; das aendert die `InstanceBlock`-API.
- **Erwarteter Effekt**: Schaetzung: pro Bucket 2 atomare Inkrement/Dekrement-Paare und 128 B Kopie weniger; bei 10^3-10^4 Buckets pro Frame im Mikrosekundenbereich (messen). Niedrige bis mittlere Prioritaet.
- **Risiko / Verifikation**: Gering; `tests/test_foliage_cluster.cpp` pruefen Bucket-Ausgabe (Reihenfolge der Objekte unveraendert lassen).

### REN-911 – TAA: zwei unordered_map<uint32_t, mat4> pro Frame fuer Objekt-Transforms
- **ID**: REN-911
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/Backends/Vulkan/VulkanRenderer.h:874`; Nutzung `src/Backends/Vulkan/VulkanRenderer.cpp:5593-5594,5647,5660-5662,5674`
- **Problem**: `m_taaPrevTransforms`/`m_taaCurTransforms` sind `std::unordered_map<uint32_t, glm::mat4>`; pro sichtbarem Objekt pro Frame passieren ein `find` und ein `operator[]` (Knotenallokation, 80 B), danach `clear()` (Knoten-Freigabe) und `swap`. Das sind O(Objekte) Heap-Operationen pro TAA-Frame. Die D3D11/D3D12/GL/Metal-Backends haben laut Kommentaren (`TemporalAA.h:5-8`) dieselbe Funktion; vermutlich dasselbe Muster.
- **Code-Auszug**:
```cpp
std::unordered_map<uint32_t, glm::mat4> m_taaPrevTransforms, m_taaCurTransforms;
...
const auto it = m_taaPrevTransforms.find(obj.entityId);
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Flacher Speicher: `std::vector<glm::mat4>` indiziert nach `RenderWorld::objects`-Index plus `std::vector<uint32_t>` der entityIds, oder ein sortiertes `vector<pair<uint32_t,mat4>>` (Objekte kommen bereits sortiert aus dem Extractor, falls entityId-Reihenfolge stabil). Alternative mit Minimalaenderung: `reserve(objects.size())` und `reserve` statt `clear` ist bei `unordered_map` wirkungslos; daher Flat-Map.
- **Erwarteter Effekt**: Schaetzung: Pro TAA-Frame O(n) Allokationen weg (n = sichtbare Objekte); bei 5k Objekten ca. 5k Mallocs. Nur wenn TAA aktiv ist.
- **Risiko / Verifikation**: Gering-mittel (Verhalten bei neuen/verschwundenen Entities muss gleich bleiben: fehlender Eintrag → aktuelle Transform). Visuell: TAA-Szene mit bewegten Objekten (Velocity-Dump).

### REN-912 – World-Preview: zwei vkDeviceWaitIdle pro Aufruf, frischer RenderExtractor und RenderWorld pro Frame
- **ID**: REN-912
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/Backends/Vulkan/VulkanRenderer.h:991-1000,99-105`; `src/Backends/Vulkan/VulkanRenderer.cpp:6551`; `include/HorizonRendering/WorldPreviewFrame.h:35-65`
- **Problem**: Der Header dokumentiert "Every call waits for the device before and after"; `VulkanRenderer.cpp:6551` ruft `vkDeviceWaitIdle`. Mehrere Preview-Viewports (Class Editor, Mesh Viewer, Sekundaer-Scene-Views) rufen das pro Frame und Slot, d. h. die CPU wartet pro Preview auf die komplette GPU-Pipeline des Hauptframes. Zusaetzlich baut `buildWorldPreviewFrame` pro Aufruf einen neuen `RenderExtractor` (Konstruktor mit `unordered_set`, `setContentManager` verwirft die "fehlend"-Cache-Eintraege) und weist `out.snapshot = RenderWorld{}` zu (gibt alle Vektor-Kapazitaeten des letzten Frames frei, statt `clear()`). Editor-only, daher Mittel.
- **Code-Auszug**:
```cpp
RenderExtractor extractor;
extractor.setContentManager(cm);
...
out.snapshot = RenderWorld{};
extractor.extract(world, out.snapshot, aspect, &previewCam);
```
- **Lösung**: (1) `out.snapshot.clear()` statt Neuzuweisung (die Methode existiert: `RenderWorld.h:290`); `RenderExtractor` als Member in `WorldPreviewFrame`/Renderer halten. (2) Die `vkDeviceWaitIdle`-Aufrufe durch Fence-/Serial-Tracking ersetzen (`m_frameSerial`/`GetCompletedFrameSerial` existieren, `VulkanRenderer.h:115-123`) und die gemeinsamen Sky/Debug-Puffer pro Preview-Slot separat halten. (2) ist invasiv; erst (1).
- **Erwarteter Effekt**: Schaetzung: (1) kleine Allokationsersparnis pro Preview-Frame; (2) kann den Editor bei mehreren offenen Previews merklich glaetten (Stalls von GPU-Frame-Dauer). Messen (GPU-Wait-Zeit im Profiler).
- **Risiko / Verifikation**: (1) gering; (2) hoch (Synchronisation). Parallelstellen: D3D11/D3D12 (`D3D11Renderer.cpp:8267`, `D3D12Renderer.cpp:12430`), GL/Metal haben eigene Kopien.

### REN-913 – giAutoLandscapeSliceMeans: pow() pro Texel bei jedem Extract
- **ID**: REN-913
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/GiAutoLandscape.cpp:64-103` (Aufrufer `src/RenderExtractor.cpp:349`, `EditorApplication.cpp:7740`); Kommentar `include/HorizonRendering/GiLandscape.h:126-130`
- **Problem**: Pro Auto-Landscape und pro `extract` (jeden Frame, laut Kommentar absichtlich "cheap enough to redo every frame") werden 5 Slices x bis zu 16x16 Texel x 3 Kanaele durch die sRGB-Dekodierung geschickt (`std::pow` pro Wert, ca. 3840 pow). Das Ergebnis aendert sich nur, wenn die Textur neu importiert wird. `decode` arbeitet auf `uint8_t`, also gibt es nur 256 moegliche Eingaben.
- **Code-Auszug**:
```cpp
auto decode = [&](uint8_t b) { const float c = b / 255.0f; if (!t.srgb) return c; return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
```
- **Lösung**: Statisches `std::array<float,256>` (einmal beim ersten Aufruf gefuellt) fuer sRGB-Dekodierung; optional Ergebnis pro `(TextureAsset*, contentEpoch)` cachen. Ergebnis bit-identisch zur jetzigen Berechnung, wenn die LUT mit demselben Ausdruck gefuellt wird.
- **Erwarteter Effekt**: Schaetzung: ~3840 `pow` pro Landscape und Extract weniger (einige 10 µs). Gering; wegen der Einfachheit trotzdem sinnvoll.
- **Risiko / Verifikation**: Praktisch keines; vorhandener Test zu `giAutoLandscapeSliceMeans` (`grep -l giAutoLandscape tests/`) prueft das Ergebnis.

### REN-914 – GPUMemoryAllocator: Doku und Verhalten widersprechen sich, drei Container fuer einen LRU
- **ID**: REN-914
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/HorizonRendering/GPUMemoryAllocator.h:22-24,51-53`, `src/HE_Rendering/src/GPUMemoryAllocator.cpp:9-29`
- **Problem**: Der Header sagt "Returns false if the allocation would exceed the budget even after evicting all LRU candidates", die Implementierung prueft aber nur `sizeBytes > m_budget` (Zeile 11) und addiert zu `m_used`; `m_used` kann das Budget beliebig ueberschreiten, ohne dass evictiert oder abgelehnt wird. Die Verwaltung braucht drei Strukturen (`m_sizes`, `m_lruList`, `m_lruIndex`), obwohl `m_sizes` und `m_lruIndex` denselben Schluessel haben (ein `unordered_map<key, {size, list::iterator}>` genuegt). Der Typ ist laut Kommentar "STAGED API", ohne Produktionsnutzer (verifiziert: `grep` findet nur `RenderResourceManager.cpp`).
- **Code-Auszug**:
```cpp
bool GPUMemoryAllocator::requestAllocation(uint64_t sizeBytes, RenderHandle handle)
{ if (sizeBytes > m_budget) return false; ... m_used += sizeBytes; return true; }
```
- **Lösung**: Entweder Doku an die Implementierung anpassen oder die Budget-Pruefung `while (m_used + size > m_budget && !m_lruList.empty()) evictLRU();` implementieren, bevor der Typ einen Produktionsnutzer bekommt. Container zu einem `unordered_map<uint64_t, Entry{size, it}>` zusammenfassen. Da Staged, geringe Prioritaet; wichtig ist nur, dass der erste Nutzer nicht auf die Doku vertraut.
- **Erwarteter Effekt**: Kein Laufzeiteffekt; verhindert falsche Annahme beim spaeteren Upload-Budgeting.
- **Risiko / Verifikation**: Bestehende Tests (`grep -l GPUMemoryAllocator tests/`) pruefen das jetzige Verhalten; vor einer Aenderung klaeren, ob ein Test auf "kein Evict in requestAllocation" baut.

### REN-915 – InstanceBlock::world()/position() verzweigen pro Instanz auf `kind`
- **ID**: REN-915
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `include/HorizonRendering/RenderWorld.h:264-286` (`position`, `world`), `:344-362` (`appendInstances`, `forEachInstance`)
- **Problem**: `kind` ist fuer den ganzen Block konstant, wird aber in jeder Instanz neu per `switch` ausgewertet; bei `Identity` (haeufigster Fall: Terrain ohne Verschiebung bzw. gestreamtes Scratch, `FoliageExtract.cpp:416-418`) ist `appendInstances` effektiv ein `memcpy` von N x 64 B, laeuft aber als Schleife mit Zuweisung der Rueckgabe-`mat4` pro Element. `out.resize(base + count)` initialisiert zudem erst auf Identitaet (vom `mat4`-Konstruktor), bevor sie ueberschrieben wird.
- **Code-Auszug**:
```cpp
out.resize(base + b->count);
for (uint32_t i = 0; i < b->count; ++i) out[base + i] = b->world(i);
```
- **Lösung**: In `appendInstances` nach `kind` verzweigen: `Identity` → `out.insert(out.end(), src.begin()+first, src.begin()+first+count)` (kein Vorinitialisieren), `Translation` → eine Schleife mit `w[3] += offset`, `General` → `parent * m`. Das Ergebnis bleibt bitgleich (Kommentar `:230-231` verlangt Bit-Identitaet; die Addition `m[3] + parent[3]` muss dieselbe Reihenfolge behalten).
- **Erwarteter Effekt**: Schaetzung: bis ca. 2x schneller beim Entfalten sehr grosser Foliage-Batches; sonst unbedeutend. Messen vor Umsetzung (Compiler kann die Verzweigung evtl. schon herausziehen).
- **Risiko / Verifikation**: Gering; `tests/test_foliage_cluster.cpp` vergleicht bereits bitgenau.

### REN-916 – VulkanShaderManager ist praktisch tot, ohne `#pragma once`, und fuehrt ein nie bereinigtes Tracking
- **ID**: REN-916
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/Backends/Vulkan/VulkanShaderManager.h:1-25`, `src/Backends/Vulkan/VulkanShaderManager.cpp:13-122`; Nutzung nur `VulkanRenderer.cpp:216` (`m_shaderManager = VulkanShaderManager();`) und `VulkanRenderer.h:914`
- **Problem**: Verifiziert per Grep: ausser Konstruktion/Zuweisung wird `m_shaderManager` im Vulkan-Backend nirgends benutzt (Module werden ueber `loadShaderModule`/MaterialShaderLibrary gebaut). Die Datei hat keinen Include-Guard (`#pragma once` fehlt in Zeile 1, Datei endet ohne Newline), `load()` macht `fs::exists` plus `ifstream` (doppelter Dateizugriff), `static unsigned nextId` ist nicht thread-sicher, `release()` entfernt den Handle nicht aus `m_loadedShaders` (nur `cleanup()` leert die Liste, spaetes Doppel-`release` ist harmlos), und `m_loadedPrograms` speichert Kopien der `ShaderProgramHandle`-Vektoren ohne Zweck (`releaseProgram` ist ein No-op). Der Header zieht zusaetzlich `<vulkan/vulkan.h>` und `<unordered_map>` in `VulkanRenderer.h` (und damit in jeden Includer).
- **Code-Auszug**:
```cpp
void VulkanShaderManager::releaseProgram(ShaderProgramHandle handle) { (void)handle; }
```
- **Lösung**: Pruefen, ob irgendein Test/Tool den Manager braucht (`grep` in `tests/` fand nur die D3D-Manager). Wenn nicht: Klasse und Member entfernen (Interface `ShaderManager.h` bleibt fuer die anderen Backends). Wenn behalten: `#pragma once` ergaenzen, Tracking-Vektoren streichen.
- **Erwarteter Effekt**: Kleinere Header-Abhaengigkeit (kein `vulkan.h`-Include mehr ueber diesen Pfad), weniger toter Code.
- **Risiko / Verifikation**: Gering; Build aller Backends (Vulkan nur auf Windows/Linux) und `he_tests`.

### REN-917 – VulkanRenderer.h: fest verdrahtete Frame-Zahlen [2]/[3] und Monolith-Header
- **ID**: REN-917
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/Backends/Vulkan/VulkanRenderer.h` (z. B. `:349-356,381,411,426-428,446-451,580,1350-1356,1371,1482-1485`); `src/Backends/Vulkan/VulkanRenderer.cpp:44` (`static constexpr uint32_t k_maxFramesInFlight = 2;`)
- **Problem**: Der Header kommentiert mehrfach `k_maxFramesInFlight` (`:580,1371,1424,1478,1549`), aber die Konstante ist file-lokal in der .cpp. Im Header stehen deshalb ~40 Literale `[2]`, und die GI-Ringe verwenden `[3]` ("static_assert at use site", `:1371`) – zwei verschiedene Frame-Zahlen im selben Backend, die nur durch Kommentar-Disziplin zusammenpassen. Die Klasse umfasst 1566 Zeilen mit ~600 Members, vom Swapchain bis zur Hardware-RT-Beschleunigung, und `VulkanRenderer.h` inkludiert `RenderExtractor.h`, `GiBvh.h`, `GIProbeGrid.h`, `MaterialShaderLibrary.h`, `VulkanShaderManager.h`, `vulkan.h` – jeder Includer zahlt das.
- **Code-Auszug**:
```cpp
VkSemaphore m_imageReady[2]{};   ... GiBuffer m_giInstanceBuf[3]; // k_maxFramesInFlight (static_assert at use site)
```
- **Lösung**: `inline constexpr uint32_t kMaxFramesInFlight` in `RenderConstants.h` (neben `kGpuTimerRing`) einfuehren, im Header verwenden und das `static_assert`-Muster vereinheitlichen. Mittelfristig Teilsysteme (SSAO, SSR, GI, TAA, Decals) als eigene Strukturen/Klassen mit eigener Header-Datei ausgliedern (Pimpl), damit Aenderungen am GI-Block nicht alle Includer neu uebersetzen. Reine Struktur, kein Laufzeiteffekt.
- **Erwarteter Effekt**: Weniger Fehlerrisiko bei einer Aenderung der Frame-Zahl; schnellere inkrementelle Builds (nicht gemessen).
- **Risiko / Verifikation**: Gering; Vulkan-Build. Die Drei-Slot-GI-Ringe haengen laut Kommentar an `k_maxFramesInFlight`; vor dem Ersetzen im .cpp klaeren, warum GI 3 Slots nutzt (Swapchain-Anzahl?).

### REN-918 – RenderSorter::partitionByOpacity: Flag-Parameter mit totem Default-Zweig
- **ID**: REN-918
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/HorizonRendering/RenderSorter.h:42-60`; Aufrufer `D3D11Renderer.cpp:6721-6722`, `D3D12Renderer.cpp:10736-10737`, `VulkanRenderer.cpp:8141-8142`
- **Problem**: `partitionByOpacity(..., bool sectionAware = false)` unterscheidet zwei Modi, aber alle drei Aufrufer (verifiziert per Grep) uebergeben `true`; der `false`-Modus ("conservative guard", droppt `sectionIndex > 0`) wird in Produktionscode nie ausgefuehrt. Ein bool-Flag-Parameter mit gefaehrlichem Default ist eine Fehlerquelle fuer einen sechsten Backend-Port; der Kommentar rechtfertigt das selbst.
- **Code-Auszug**:
```cpp
static void partitionByOpacity(const std::vector<DrawCall>& drawCalls, std::vector<const DrawCall*>& outOpaque,
                               std::vector<const DrawCall*>& outTransparent, bool sectionAware = false);
```
- **Lösung**: Parameter entfernen, immer section-aware; den `false`-Pfad in `RenderSorter.cpp:44ff` und die zugehoerigen Tests (`grep -l sectionAware tests/`) streichen. Falls der Guard fuer externe Backends gedacht ist: Default auf `true` drehen.
- **Erwarteter Effekt**: Weniger Verzweigung/Doku; kein Laufzeitgewinn.
- **Risiko / Verifikation**: Gering; Tests, die den `false`-Modus pruefen, anpassen.

### REN-919 – SkyShaderSource.h: 146 KB Literal im Header in vier Backend-TUs, drei grosse String-Kopien beim Bau
- **ID**: REN-919
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `include/HorizonRendering/SkyShaderSource.h:36-2248` (`kSkyFS`), `:2541-2548` (`BuildSkyFragmentGLSL450`); Includer: `OpenGLRenderer.cpp`, `VulkanRenderer.cpp`, `D3D11Renderer.cpp`, `D3D12Renderer.cpp`, `tests/test_sky_shader.cpp`, `tests/test_culling.cpp`
- **Problem**: Die Datei ist ein reiner Daten-Header mit 146 071 Byte; jede Includer-Uebersetzungseinheit parst das Literal. `BuildSkyFragmentGLSL450` kopiert `kSkyFS` in `const std::string fs` (Zeile 2543), erzeugt `substr` (weitere Kopie) und konkateniert Prelude + `kSkyFuncGLSL` + Rest (dritte): drei volle Kopien bei jeder Pipeline-Erzeugung, in jedem der drei cross-kompilierenden Backends. Einmalig beim Start, daher unkritisch. Ausserdem ist `skyColor` im Shader (`:2331-2427`) mit `sunGlare` (`:2087-2103`) bewusst doppelt gerechnet ("col -= sunGlare … byte-identical", Kommentar `:1977-1982`).
- **Code-Auszug**:
```cpp
const std::string fs = kSkyFS;
...
return std::string(kSkyVulkanPrelude) + kSkyFuncGLSL + fs.substr(at + marker.size());
```
- **Lösung**: `std::string_view` auf `kSkyFS` benutzen (Suche mit `find` ohne Kopie) und das Ergebnis per `reserve` + `append` bauen. Die grosse Quelle in eine `.cpp` (`SkyShaderSource.cpp`) mit `extern const char*`-Deklaration verschieben, damit nur eine TU das Literal enthaelt – Achtung `ClipSpace.h`-Kommentar: Backend-Static-Libs linken nicht gegen HorizonRendering, eine neue `.cpp` braucht deshalb Build-Plumbing (siehe Argument in `SkyEnvBake.h:22-24`). Sonst Header lassen.
- **Erwarteter Effekt**: Geringe Compile-Zeit- und Startup-Ersparnis. Nicht priorisieren.
- **Risiko / Verifikation**: Gering; `tests/test_sky_shader.cpp` pruefen die Prelude-Zuordnung.

### REN-920 – Schwere Header-Abhaengigkeiten (ParticleGraph.h in RenderWorld.h, <fstream> in WorldPreviewGrid.h)
- **ID**: REN-920
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/HorizonRendering/RenderWorld.h:5-6`, `include/HorizonRendering/ParticleShaderTemplates.h:20`, `include/HorizonRendering/WorldPreviewGrid.h:6,94-113`, `include/HorizonRendering/ShaderManager.h:19-40`
- **Problem**: `RenderWorld.h` (von allen Backends, Extractor, Culler, Sorter, Passes inkludiert) zieht `<ParticleGraph/ParticleGraph.h>` und `<Renderer/UIRenderObject.h>`, obwohl nur `HE::ParticleEmitterConfig` (Wert-Member in `ParticleBatch`) gebraucht wird. `ParticleShaderTemplates.h` inkludiert dasselbe nur fuer `hashParticleShaderConfig`. `WorldPreviewGrid.h` zieht `<fstream>` fuer `writeWorldPreviewDump`, einen Debug-Dump, in einen Header, den jedes Backend inkludiert. `ShaderManager::releaseProgram(ShaderProgramHandle handle)` (Zeile 38) nimmt eine Struktur mit `std::vector` by value, jede Uebergabe kopiert den Vektor.
- **Code-Auszug**:
```cpp
virtual void releaseProgram(ShaderProgramHandle handle) = 0;   // ShaderProgramHandle enthaelt std::vector<ShaderHandle>
```
- **Lösung**: `ParticleEmitterConfig` in einen leichten Header (`ParticleEmitterConfig.h`) auslagern und nur diesen inkludieren; `writeWorldPreviewDump` nach `WorldPreviewGrid.cpp`/ein Backend-gemeinsames `.cpp` oder `inline` in eigener Datei; `releaseProgram(const ShaderProgramHandle&)`. Geringe Compile-Zeit-Ersparnis, keine Laufzeitauswirkung.
- **Erwarteter Effekt**: Schaetzung: weniger Rebuild-Kaskaden beim Aendern von `ParticleGraph.h`.
- **Risiko / Verifikation**: Gering; Header-Aenderungen mit vollem Build aller Targets pruefen (HE_Editor, HE_Game, `he_tests`).

### REN-921 – giBvhIntersect: Stack-Ueberlauf verwirft Kinder stillschweigend, Baum ohne Tiefenbegrenzung
- **ID**: REN-921
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/src/GiBvh.cpp:174-217` (insbesondere `:182,210-214`), Median-Split `:36-81`
- **Problem**: Bei vollem 64er-Stack wird der Kindknoten per `else if (sp + 2 <= …)` einfach nicht gepusht – der Teilbaum ist dann fuer den Strahl unsichtbar, ohne Hinweis. Der Median-Split (`nth_element` ueber `order` mit indirektem Zugriff auf `tris[a].centroid`) garantiert zwar Tiefe ~log2(N), aber Duplikat-Centroids brechen mit `extent <= 1e-12f` ab und erzeugen uebergrosse Blaetter (`:57`), was die Traversal-Laufzeit pro Blatt erhoeht, aber die Tiefe nicht. Die Obergrenze wird also praktisch nie erreicht; trotzdem verdeckt der stille Drop jeden zukuenftigen Fehler im Build. Zusaetzlich liegt `BuildTri` (84 B) mit `v0,v1,v2,centroid,bmin,bmax` neben `order`-Indirektion, was die Build-Zeit durch Cache-Misses verlangsamt; nur beim Mesh-Upload relevant.
- **Code-Auszug**:
```cpp
else if (sp + 2 <= static_cast<int>(sizeof(stack) / sizeof(stack[0])))
{ stack[sp++] = static_cast<uint32_t>(node.leftFirst); stack[sp++] = static_cast<uint32_t>(node.leftFirst + 1); }
```
- **Lösung**: In `buildGiBvh` die maximale Tiefe messen und bei > 60 den Split abbrechen (Blatt) oder asserten; Traversal dann ohne Guard (Shader-Spiegel ebenso). Bei Bedarf Test mit entarteter Geometrie (alle Dreiecke mit gleichem Centroid, sehr viele Dreiecke) ergaenzen. Build-Optimierung (Dreiecke direkt in Leaf-Reihenfolge umsortieren statt `order`-Indirektion) nur nach Messung.
- **Erwarteter Effekt**: Kein Performance-Effekt; verhindert stille Fehlerbilder bei degenerierten Meshes.
- **Risiko / Verifikation**: Gering; Tests zu `giBvhIntersect` (`grep -l GiBvh tests/`) und Shader-Kernel bleiben die Referenz ("mirrored 1:1").

### REN-922 – FoliageExtract: thread_local Bounds-Cache waechst ohne Verdraengung
- **ID**: REN-922
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/FoliageExtract.cpp:92-108`
- **Problem**: `thread_local std::unordered_map<HE::UUID, BoundsEntry> cache` wird nie bereinigt; ein neu geladenes Asset (anderer Zeiger) ueberschreibt nur denselben Schluessel. Bei langen Editor-Sitzungen mit vielen Foliage-Meshes oder Projektwechsel bleiben Eintraege fuer nicht mehr vorhandene UUIDs stehen. Zusaetzlich wird fuer jeden Layer und Frame `cm->getStaticMesh(id)` aufgerufen (Hash-Lookup), obwohl der Cache bereits eine Pruefung auf `asset == m` macht.
- **Code-Auszug**:
```cpp
thread_local std::unordered_map<HE::UUID, BoundsEntry> cache;
...
cache[id] = e;
```
- **Lösung**: Cache an `ContentManager::contentEpoch()` koppeln (leeren bei Epoch-Wechsel, wie bereits `RenderExtractor::setContentManager` es fuer `m_sectionMaterialMissing` tut, `RenderExtractor.h:263-267`) oder die Bounds im `StaticMeshAsset` selbst vorhalten (`boundsMin/Max` existieren schon, `scanMeshBounds` fuellt nur Luecken).
- **Erwarteter Effekt**: Vernachlaessigbar (Speicher im KB-Bereich); Sauberkeit.
- **Risiko / Verifikation**: Gering.

### REN-923 – FillMaterialDirectionalWindow: Zwischenstruktur plus verschachtelte Schleife mit Pro-Element-Verzweigung
- **ID**: REN-923
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/src/LightPacking.cpp:247-278`
- **Problem**: `FillMaterialDirectionalWindow` baut zuerst ein `DirectionalLightWindow` (4 Arrays x 8 `vec4`, 512 B) und schreibt es dann in 8x4-Schleife elementweise in `Lighting` um, mit `on`-Pruefung pro Skalar und `(on && k == 0)` fuer `lightParams`. Das ist schwer lesbar (die 4er-Schleife `k` verdeckt, dass `lightParams[i][1..3]` immer 0 sind).
- **Code-Auszug**:
```cpp
for (int i = 0; i < kMaxLightWindow; ++i)
    for (int k = 0; k < 4; ++k) { const bool on = i < w.count; out.lightPos[i][k] = on ? w.pos[i][k] : 0.0f; ... out.lightParams[i][k] = (on && k == 0) ? w.params[i][0] : 0.0f; }
```
- **Lösung**: Zwei kurze Schleifen: `for i < w.count` kopiert per `std::memcpy(out.lightPos[i], &w.pos[i], 16)` (`vec4` und `float[4]` sind layout-kompatibel), `for i >= w.count` nullt; `lightParams[i][0] = w.params[i][0]; [1..3] = 0`. Verhalten gleich.
- **Erwarteter Effekt**: Lesbarkeit; Laufzeit unbedeutend (einmal pro Frame).
- **Risiko / Verifikation**: Gering; `tests/test_light_packing*.cpp` (falls vorhanden, `grep -l FillMaterialDirectionalWindow tests/`).

## Gelesen

Alle 50 Dateien der Slice-Liste wurden vollstaendig gelesen (auch `SkyShaderSource.h` und `VulkanRenderer.h` komplett in Bloecken); zur Verifikation von Aufrufstellen wurden zusaetzlich einzelne Ausschnitte aus `RenderPass.cpp`, `RenderExtractor.cpp`, `VulkanRenderer.cpp` und `OpenGLRenderer.cpp` per Grep/`sed` angesehen, nicht vollstaendig.

## Rendering Slice 10 – Frontend (RenderExtractor, RenderGraph/-Pass, Sorter, OcclusionCuller, ScenePick, MaterialShaderLibrary, Sky/SSAO-Helfer) + `shaders/`

## Zusammenfassung

Der Frontend-Code ist insgesamt sauber strukturiert (Phasen-Funktionen im Extractor, Frame-Cache mit Reuse-Pfad, vorberechnete Sort-Keys, Depth-Run-Batching, Occlusion-Culler mit klaren Konservativitäts-Garantien). Die größten Kosten liegen nicht in der Algorithmik, sondern in **wiederholtem Kopieren großer Strukturen pro Frame** (DrawCall mit `instanceTransforms`, Bone-Matrizen, GLSL-Quelltexte pro Draw) und in **hand-gepflegter Duplikation** (Shader-Blöcke in `MaterialShaderLibrary.cpp`, sieben GI-Kernel-Kopien). Gefunden: **2 Hoch, 13 Mittel, 9 Niedrig** (insgesamt 24). Die Datei-Liste des Slice enthielt `src/HE_Rendering/shaders/` nur unvollständig: es fehlten `gi_probe.comp`, `gi_probe_hw.comp`, `gi_probe_hw.hlsl`, `gi_shadow.comp`, `gi_shadow_hw.comp`, `gi_shadow_hw.hlsl` (6 von 45 Dateien, 1276 Zeilen); die vier GLSL-Kernel wurden zusätzlich vollständig gelesen, die beiden HLSL-Twins nur teilweise (siehe „Gelesen“), und sie sind in den Funden REN-1013/1014/1023 berücksichtigt. Hinweis: `CommandBuffer`/`Culler` liegen als Header/Impl in anderen Slices bzw. wurden nur gezielt (`CommandBuffer.cpp/.h`, `RenderObject.h`, `RenderSorter.h`) gelesen, soweit sie die Funde belegen. Nichts davon wurde gemessen; Aufwandsschätzungen sind Schätzungen.

---

## Funde

### REN-1001 – Voller GLSL-String wird pro Draw und Frame kopiert und gehasht
- **ID**: REN-1001
- **Kategorie**: C / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-952`, `:981-998`, `:1000-1009`; Aufrufer im Draw-Loop: `Backends/D3D11/D3D11Renderer.cpp:7009`, `Backends/D3D12/D3D12Renderer.cpp:10974`, `Backends/Vulkan/VulkanRenderer.cpp:8254` (je pro `DrawCall`), GL/Metal über `resolveMaterialShader` (`OpenGLRenderer.cpp:3599`, `MetalRenderer.mm:12815`)
- **Problem**: `resolveFragment`/`resolveShaders`/`resolveGBufferShaders` kopieren `customShaderFragGlsl` (und `customShaderVertGlsl`) in Out-Strings und berechnen `std::hash<std::string>` über den **gesamten Quelltext** – bei jedem Draw jedes Frames, nur um danach den Cache-Key `hash` für `fragment()` zu bekommen. Graph-Materialien haben mehrere KB GLSL (Preamble wird erst später injiziert, trotzdem Kopie+Hash des Material-Bodys). Die Deklaration in den Backends (`std::string matFrag, matVertBody;` direkt vor dem Aufruf, D3D11:7008) legt dazu pro Draw zwei Strings auf dem Stack an, die beim Kopieren Heap-Allokationen auslösen.
- **Code-Auszug**:
```cpp
glslOut = mat->customShaderFragGlsl;                 // 1009: Kopie des gesamten Shaders
hashOut = std::hash<std::string>{}(glslOut);         // pro Draw, pro Frame
...
vertBodyOut = mat->customShaderVertGlsl;
hashOut ^= std::hash<std::string>{}(vertBodyOut) * 0x9E3779B97F4A7C15ULL;
```
- **Lösung**: (1) Hash einmal beim Laden/Ändern des Materials berechnen und im `MaterialAsset` ablegen (`customShaderHash`, `customShaderGBufHash`, `customShaderVertHash`; Invalidierung dort, wo die Strings gesetzt werden – Graph-Compile, PSHD-Load). (2) API auf Zeiger/`const std::string&` umstellen: `bool resolveShaders(cm, id, uint64_t& hash, const std::string*& frag, const std::string*& vert)`; Strings werden nur bei Cache-Miss in `fragment()` gebraucht. (3) Backends cachen ihr Ergebnis pro `materialAssetId` für die Dauer eines Frames (kleine flache Map), damit Mehrfachdraws desselben Materials nicht erneut auflösen.
- **Erwarteter Effekt**: Pro Graph-Material-Draw entfallen 1-2 Heap-Kopien und ein Hash über mehrere KB (Schätzung: ~1-5 µs je Draw, bei ein paar tausend Graph-Draws 1-several ms/Frame auf dem Render-Thread). Messen mit Profiler-Scope um den Aufruf.
- **Risiko / Verifikation**: Cache-Key-Wert muss bitgleich bleiben (er ist Schlüssel für `m_fragCache`, Pipelines der Backends und teils Dateicaches) – sonst Pipeline-Rebuilds. Test: `tests/test_material_graph.cpp` (Resolve/Hash-Fälle), zusätzlich Test „Hash vor/nach Refactor identisch für Fixture-Materialien“.

### REN-1002 – DrawCall wird mehrfach tief kopiert (inkl. `instanceTransforms`)
- **ID**: REN-1002
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderPass.cpp:78`, `:102`, `:147-168`; `src/HE_Rendering/src/CommandBuffer.cpp:17-20`; `include/HorizonRendering/CommandBuffer.h:62-65`
- **Problem**: (a) `recordDraw(const DrawCall&)` kopiert jeden Aufruf per `push_back(call)`; `DrawCall` enthält `std::vector<glm::mat4> instanceTransforms` und `std::vector<float> paramOverride`. (b) Bei Multi-Section-Meshes macht `recordRun` pro Section `DrawCall sd = d;` – jede Kopie dupliziert die komplette Instanzliste (Section-Anzahl × N × 64 B), danach nochmal die Kopie in `recordDraw`. (c) Im Split-Pfad (`total > kMaxInstancesPerDraw`, z. B. 140k Pflanzen) entstehen `all` (9 MB bei 140k), `chunk = dc`, `assign(...)` und wieder die Kopie in `recordDraw`: jede Instanz-Matrix wird pro Frame 3× kopiert, bevor das Backend sie ein weiteres Mal in den Instance-Buffer hochlädt. (d) `dc.paramOverride = first.paramOverride` (Z. 78) kopiert den 64-Float-Block je Run.
- **Code-Auszug**:
```cpp
DrawCall sd        = d;                       // RenderPass.cpp:102 – kopiert instanceTransforms
...
outCmds.recordDraw(sd);                       // CommandBuffer.cpp:19: m_drawCalls.push_back(call) – nochmals Kopie
```
- **Lösung**: Instanzdaten aus dem `DrawCall` herauslösen, wie es `DepthBatchList` schon tut: `CommandBuffer` besitzt einen flachen `std::vector<glm::mat4> m_instances`; `DrawCall` trägt nur `{uint32_t instanceFirst, instanceCount}`. `GeometryPass` hängt die Matrizen **einmal** per `appendInstances` an und alle Sections/Chunks eines Runs teilen sich die Range (Chunking = Range-Slicing, kein `assign`). `recordDraw(DrawCall&&)`/`emplace_back` für den Rest. Backends lesen `cmds.instances().data() + dc.instanceFirst` statt `dc.instanceTransforms`. Zwischenschritt ohne ABI-Änderung: `recordDraw(DrawCall&&)` anbieten und im Section-Pfad nur die **letzte** Section verschieben; Kopien nur für die ersten n-1.
- **Erwarteter Effekt**: Entfällt (Sections+1)× das Kopieren aller Instanz-Matrizen und die Mini-Allokation pro Draw. Bei 140k Foliage-Instanzen ca. 2× 9 MB weniger Speicherverkehr/Frame; `CommandBuffer::reset()` behält Kapazität → keine Heap-Last nach Warm-up (Schätzung, messen).
- **Risiko / Verifikation**: Alle fünf Backends lesen `dc.instanceTransforms` (Grep nötig) – mechanische Änderung, aber breit. Absichern: `tests/test_rendergraph.cpp` (GeometryPass-Batching, Sections), `tests/test_foliage_cluster.cpp` (Cluster-Entfaltung/Chunking), Bildvergleich je Backend. Beobachtbares Verhalten (Draw-Reihenfolge, Kontrakt „1 Instanz = leere Liste“) bleibt erhalten.

### REN-1003 – Bone-Matrizen werden pro Frame 3-4× kopiert
- **ID**: REN-1003
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderExtractor.cpp:639-641`, `:647`; `src/HE_Rendering/src/RenderPass.cpp:194`, `:209`; `src/HE_Rendering/src/CommandBuffer.cpp:22-25`
- **Problem**: Je Skinned-Entity: `smc.boneMatrices` → `obj.boneMatrices` (Kopie, bis 128×64 B = 8 KB), → `dc.boneMatrices = obj.boneMatrices` (2. Kopie), → bei Multi-Section pro Section `SkinnedDrawCall sd = dc` (n-te Kopie), → `recordSkinnedDraw(const&)` (weitere Kopie). Außerdem wird pro Entity `m->sections` (Z. 647) kopiert. Skinned Objekte werden nicht gecullt (`RenderPass.cpp:174-175`, `worldBounds = kUnitCube…` Z. 636) – die Kopien fallen also auch für nicht sichtbare Charaktere an.
- **Code-Auszug**:
```cpp
obj.boneMatrices = smc.boneMatrices.empty() ? std::vector<glm::mat4>{ glm::mat4(1.0f) } : smc.boneMatrices; // 639
dc.boneMatrices  = obj.boneMatrices;                                                                         // RenderPass.cpp:194
SkinnedDrawCall sd = dc;                                                                                     // :209
```
- **Lösung**: Bone-Daten wie Instanzen in einen flachen Frame-Puffer (`RenderWorld::boneArena`, `std::vector<glm::mat4>`) schreiben; `SkinnedRenderObject`/`SkinnedDrawCall` halten `{boneFirst, boneCount}`. Extractor kopiert **einmal** (oder zeigt direkt auf `smc.boneMatrices`, solange die Welt während des Frames unverändert ist – Lebenszeit prüfen). Section-Kopien tragen nur die Range.
- **Erwarteter Effekt**: Bei z. B. 200 Skinned-Entities × 8 KB × (3 Kopien eingespart) ≈ 5 MB/Frame weniger Kopierverkehr (Schätzung). Zusätzlich kein Heap-Churn pro Entity.
- **Risiko / Verifikation**: Lebensdauer des Puffers (`RenderWorld` wird von Backends über Frames gehalten? – `RenderExtractor::beginFrame/endFrame` regelt Reuse). Test: `tests/test_skeletalmeshcomponent.cpp`, `tests/test_rendergraph.cpp` (Skinned-Fall).

### REN-1004 – `ShadowPass` zeichnet Commands auf, die D3D11/D3D12/GL verwerfen
- **ID**: REN-1004
- **Kategorie**: D / F
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Rendering/src/RenderPass.cpp:234-254`, `:266-277`; Verbraucher: `Backends/D3D11/D3D11Renderer.cpp:6268`, `:6600-6604`; `Backends/D3D12/D3D12Renderer.cpp:10261`, `:10611-10614`; `Backends/OpenGL/OpenGLRenderer.cpp:10985-11001`
- **Problem**: D3D11, D3D12 und GL fügen `ShadowPass` in den Graph ein. Ihre Sink-Callbacks benutzen im `kShadowMapTarget`-Zweig aber ausdrücklich **nicht** das aufgezeichnete `cmds` („not the graph's whole-scene ShadowPass set“), sondern culling/sortieren pro Kaskade selbst und nutzen `batchDepthCasters`. Der Pass verursacht trotzdem pro Frame O(N) Objektschleife, Frustum-Test und je Caster `DrawCall`-Konstruktion (~260 B inkl. zweier Vektoren) + Kopie in `recordDraw`. Metal und Vulkan haben den Pass nicht im Graph (verifiziert per Grep). `ShadowPass::describe()` wird nur für `io.output.id`/Größe gebraucht.
- **Code-Auszug**:
```cpp
for (size_t idx = 0; idx < world.objects.size(); ++idx) { ... DrawCall dc; ... outCmds.recordDraw(dc); }  // RenderPass.cpp:240-253
// D3D11: "...not the graph's whole-scene ShadowPass set, which is culled against the legacy single-map frustum"
```
- **Lösung**: `ShadowPass::execute` zu einem No-Op machen (Marker wie `PostProcessPass`), der Pass bleibt als Ziel-Deklaration im Graph (`describe()` liefert weiter `kShadowMapTarget`). Vorher in D3D12/GL prüfen, dass der Zweig `cmds` wirklich nie liest. Danach `recordDraw`-Pfad der Klasse nur noch für Tests; Tests, die `ShadowPass` Draws erwarten, auf den Marker umstellen oder die Draw-Aufzeichnung hinter eine Test-Option legen.
- **Erwarteter Effekt**: Entfällt eine komplette O(N)-Schleife mit Frustum-Test und N DrawCall-Kopien pro Frame in drei Backends (Schätzung: ~0,1-0,5 ms bei 10k Objekten; messen).
- **Risiko / Verifikation**: Falls ein Backend (z. B. D3D12-Pfad im Sink) `cmds.drawCalls()` im Shadow-Zweig doch liest → sichtbare Schattenregression. Absichern mit `tests/test_rendergraph.cpp` (Anzahl Draws des ShadowPass) und Backend-Smoke-Test mit Schatten. **Nicht verifiziert** für D3D12-Sinkcode über Z. 10611 hinaus.

### REN-1005 – Extractor: Heap-Kopien pro Mesh-Entity und Frame
- **ID**: REN-1005
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderExtractor.cpp:208-209`, `:254-264`, `:439-443`, `:449`, `:471-472`
- **Problem**: Für **jede** sichtbare Mesh-Entity wird pro Voll-Extract `std::vector<MeshSection> table = m->sections;` kopiert (enthält `materialPath` als `std::string`), obwohl `resolveEntitySlots` für `sections.size() <= 1` (der Normalfall) nur `sections[0]` liest und nichts nachlädt. Dazu `lod0SectionsOf` (weitere `try_get<LODComponent>` + Kopie via `meshSectionsOf`), `slotOf` (Vektor) und in der Schleife `items.push_back(d)` (Kopie statt Move; `EntityData` hält zwei Vektoren) sowie `obj.sections = d.sections; obj.paramOverride = d.paramOverride;` im `parallel_for` (Kopie statt Move, außerdem Heap-Allokation aus mehreren Threads → Allokator-Kontention).
- **Code-Auszug**:
```cpp
std::vector<MeshSection> table = m->sections;                       // 439: Kopie je Entity
d.sections = resolveEntitySlots(std::move(table), lod0SectionsOf(reg, e, d.meshId, *contentManager), ...);
items.push_back(d);                                                 // 449: Kopie
obj.sections = d.sections;  obj.paramOverride = d.paramOverride;    // 471-472 im parallel_for
```
- **Lösung**: (1) In `resolveEntitySlots` Fast-Path vor jeder Kopie: wenn `m->sections.size() <= 1` und kein Slot-Override/LOD0-Fall, nur `sections[0].materialId/materialPath` lesen (kein Copy der Tabelle). Kopie nur im Multi-Section-Fall, wo `loadAsset` die Pointer ungültig machen kann (das ist der dokumentierte Grund für By-Value). (2) `items.push_back(std::move(d))`. (3) `obj.sections = std::move(items[i].sections)` (items sind danach tot; `EntityData&` statt `const`). (4) `lodSlotMap` nur aufrufen, wenn `lod0Sections` nicht leer.
- **Erwarteter Effekt**: Pro Mesh-Entity 1-2 Heap-Allokationen weniger (Schätzung: 10k Entities → einige Hundert µs/Voll-Extract; nur relevant, wenn `sections` bei den meisten Assets nicht leer ist – **messen**: Anteil Meshes mit `sections.size()==1`).
- **Risiko / Verifikation**: Die By-Value-Regel (Dangling nach `loadAsset`) muss für Multi-Section erhalten bleiben. Tests: `tests/test_foliage_cluster.cpp`, `tests/test_editor_icons.cpp`, `tests/test_light_window_order.cpp` + ein Test mit Mesh mit/ohne Sections und Slot-Override (Precedence unverändert).

### REN-1006 – Sort-Key ohne Material → Batch-Runs fragmentieren
- **ID**: REN-1006
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderSorter.cpp:26-36`; Konsument `src/HE_Rendering/src/RenderPass.cpp:32-61`
- **Problem**: `RenderSorter::sort` ordnet nach Mesh-UUID, dann nach Kamera-Distanz. `GeometryPass` bricht einen Run aber bei jedem Wechsel von `materialAssetId`, `paramOverride`, `instanceTint`, `receivesShadow`, `weightmapTextureId`. Teilen sich viele Objekte ein Mesh (Prefab-Varianten, Bäume mit 2-3 Materialien, Props), werden sie innerhalb der Mesh-Gruppe nach Distanz gemischt → Material wechselt von Objekt zu Objekt → ein DrawCall pro Objekt statt eines instanzierten Draws plus mehr Material-/Pipeline-Wechsel (v. a. bei Graph-Materialien, siehe REN-1001).
- **Code-Auszug**:
```cpp
m_keys.push_back(SortKey{ o.meshAssetId.hi, o.meshAssetId.lo, glm::dot(d, d), i });   // 26
... if (a.meshLo != b.meshLo) return a.meshLo < b.meshLo; return a.distSq < b.distSq;  // 33-35
```
- **Lösung**: Key erweitern: `(mesh, materialAssetId, weightmap/tint-Hash, dist)`. Materialwechsel-Grenze mit einem 64-Bit-Hash aus `materialAssetId` + `receivesShadow` + (`paramOverride.empty()`) im Key; Distanz bleibt als letzte Stufe. Zusätzlich wäre ein einmaliger Radix-/Hash-Gruppierungsschritt statt `std::sort` über 128-Bit-UUID-Vergleiche möglich (Niedrig, messen).
- **Erwarteter Effekt**: Weniger DrawCalls/State-Wechsel bei Szenen mit gleichem Mesh in mehreren Materialien (Schätzung, stark szenenabhängig – Zähler `counters`/DrawCall-Anzahl im Test messen). Preis: Front-to-back innerhalb eines Meshes bleibt, über Materialgrenzen hinweg nicht mehr → minimale Early-Z-Verschlechterung.
- **Risiko / Verifikation**: Ändert die Zeichenreihenfolge (kann Z-Fight/Transparenz-Reihenfolge nicht betreffen, da Transparente später `sortBackToFront` laufen). Tests: `tests/test_rendergraph.cpp` (Batching-Erwartung ggf. anpassen), `tests/test_culling.cpp`.

### REN-1007 – Schatten-PCF vor dem Dämpfungstest in den 8-Licht-Schleifen
- **ID**: REN-1007
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:763-828` (heLitP, Schattenaufruf `:818-822`, Vergleich: Cluster-Schleife prüft `if (atten <= 0.0) continue;` vor dem Schatten `:607`, `:1242`); `src/HE_Rendering/shaders/scene.frag:484-531` (Schatten `:523`; Cluster-Schleife `:570`)
- **Problem**: In der Fenster-Schleife wird für jedes Punkt-/Spotlicht `heLocalShadowFactor`/`localShadowFactor` (9 Texture-Fetches + Matrix-Multiplikation) berechnet, **bevor** bekannt ist, ob das Licht das Fragment überhaupt erreicht (`atten == 0`, außerhalb von `range` oder außerhalb des Spot-Kegels). Das Ergebnis geht mit Faktor 0 in die Summe. Die Cluster-Varianten haben den Early-Out bereits.
- **Code-Auszug**:
```glsl
if (type != 0) { sh = min(sh, heLocalShadowFactor(i, worldPos, n)); localIdx++; }   // MaterialShaderLibrary.cpp:818-822
...
result += (diffuseColor * ndl + specColor * spec) * lc * atten * sh;                // 827 – atten kann 0 sein
```
- **Lösung**: Nach der Dämpfungsberechnung `if (atten <= 0.0) { if (type != 0) localIdx++; continue; }` einfügen (Zählerfortschritt für die GI-Maskenkanäle beibehalten!). Gleiches in `scene.frag` (`giLocalIdx++` erhalten). Prüfen, ob GL `kUnlitFS`, Metal und D3D-HLSL dieselbe Reihenfolge haben (nicht Teil dieses Slice; per Grep `localShadowFactor` in den Backends).
- **Erwarteter Effekt**: Bei Szenen mit mehreren schattenwerfenden Local-Lights spart jedes Fragment außerhalb der Reichweite 9 Fetches + 1 Mat-Mul pro Licht (Schätzung: 10-30 % der Fragment-Zeit in lichtlastigen Szenen; GPU-Profil nötig).
- **Risiko / Verifikation**: Bitgleichheit der Ausgabe: bei `atten <= 0` war der Beitrag `… * 0 * sh` = 0 (außer `sh` = NaN; PCF liefert nie NaN). Drift-Guard `tests/test_culling.cpp` („GI kernels: the constants…“) und `tests/test_material_graph.cpp` (Preamble-String-Vergleiche) müssen angepasst werden; SYNC-Kopien in allen Backends gleichziehen (`MaterialShaderLibrary.cpp:171-198` listet sie).

### REN-1008 – `scene.frag` (Vulkan) wertet `skyColor()` pro Fragment 2-3× analytisch aus
- **ID**: REN-1008
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/shaders/scene.frag:255-294` (Definition), `:445`, `:455`, `:590`
- **Problem**: Ambient-Diffus (`skyColor(Nup, …)`), Spekular-IBL (`skyColor(Rrough, …)`) und Fog-Farbe (`skyColor(ray/dist, …)`) rufen pro Fragment die komplette analytische Himmelsfunktion auf (mehrere `smoothstep`, `pow` mit Exponenten 1800/180/22/5/8/2.5, `normalize` …). GL/Metal/Graph-Materialien sampeln dafür einen einmal pro Frame gebackenen Sky-Cubemap (`uSkyEnv` / `heSkyEnv`); der Header des Shaders dokumentiert das als bekannte Drift („This file evaluates skyColor() analytically per fragment“).
- **Code-Auszug**:
```glsl
vec3 ambDiff = skyColor(Nup,    uf.sunDir.xyz) * base * kd;   // 445
vec3 envSpec = skyColor(Rrough, uf.sunDir.xyz);               // 455
vec3  fogCol = skyColor(ray/dist, uf.sunDir.xyz);             // 590
```
- **Lösung**: Vulkan nutzt für Graph-Materialien bereits `heSkyEnv` (gebackener Cubemap-Pfad im Preamble). Dieselbe Textur an `scene.frag` binden (neuer Binding-Slot analog `uSkyEnv` in GL) und `texture(uSkyEnv, dir)` statt `skyColor()` verwenden; damit entfällt zugleich die dokumentierte Drift und der Unterschied zwischen Built-in- und Graph-Material. Als Zwischenschritt: `skyColor` für die beiden IBL-Aufrufe nur in einem Branch, wenn `giParams.y < 0.5` (ambDiff) bzw. nur bei Bedarf auswerten.
- **Erwarteter Effekt**: Deutlich weniger ALU je Fragment (3 × ~30 transzendente Ops → 2-3 Cubemap-Fetches); Schätzung: bei hoher Auflösung einige Prozent bis zweistellig GPU-Zeit im Forward-Pass (Messung nötig, Vulkan-GPU-Timer).
- **Risiko / Verifikation**: **Sichtbare Änderung** der Vulkan-Beleuchtung (Annäherung an GL/Metal – gewollt laut Banner, aber Reviewer-Entscheidung). Deskriptor-Layout (`VulkanRenderer.cpp` Scene-Set), `FrameUBOData` unverändert. Test: Bild-Smoke Vulkan vs. GL; `tests/test_sky_shader.cpp`.

### REN-1009 – Instanced-Vertex-Shader lesen 128 B pro Instanz (MVP + Model), CPU multipliziert pro Pass
- **ID**: REN-1009
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/shaders/scene_instanced.vert:17-24`; `gi_gbuf_instanced.vert:12-19`; `ssao_pos_instanced.vert:15-22`; `scene_shadow_instanced.vert:15-18`
- **Problem**: Jede Instanz trägt `mvp` (4 vec4) und `model`/`modelView` (4 vec4) als Instanz-Attribute. Die CPU berechnet pro Instanz **pro Pass** (Szene, SSAO-Prepass, GI-G-Buffer, jede Shadow-Kaskade/Local-Layer) `viewProj * model` und lädt 128 B hoch. Bei Foliage-Runs (bis 65536 Instanzen, `RenderConstants.h:31`) sind das pro Pass bis zu 8 MB Upload + 65k Mat4×Mat4 auf der CPU. Der Shadow-Shader liest nur 64 B (`scene_shadow_instanced.vert` nutzt nur MVP), die übrigen Passes brauchen `model` (bzw. `modelView`), das aus `model` und einem Pass-Uniform ableitbar wäre.
- **Code-Auszug**:
```glsl
layout(location = 3)  in vec4 iMvp0; ... layout(location = 6) in vec4 iMvp3;
layout(location = 7)  in vec4 iModel0; ... layout(location = 10) in vec4 iModel3;   // scene_instanced.vert
```
- **Lösung**: Instanz-Buffer nur mit `model` (64 B, oder 48 B als 3 vec4) füllen und **einmal pro Frame** hochladen; `viewProj` (und `view` für den SSAO-Prepass) per Push-Constant/UBO (64 B) pro Pass. Shader: `gl_Position = pc.viewProj * (model * vec4(aPos,1))`. Alle Passes teilen denselben Instanz-Buffer (Shadow-Kaskaden sparen den Re-Upload komplett).
- **Erwarteter Effekt**: Halbe bis ein Viertel der Instanz-Upload-Bandbreite und der CPU-Matrixmultiplikationen bei Foliage/Instancing (Schätzung; Pass-Anzahl ≥ 5 pro Frame). Vertex-ALU steigt minimal (1 Mat4×Vec4 mehr).
- **Risiko / Verifikation**: Float-Genauigkeit: `viewProj*(model*p)` vs. vorab multipliziertes `(viewProj*model)*p` – leichte Rundungsunterschiede, kann Z-Fighting/Depth-Differenz zwischen Prepass und Scene-Pass erzeugen (derzeit **bitgleich** garantiert: Kommentar in `ssao_pos_instanced.vert:8`). Falls Depth-Konsistenz zwischen Passes nötig ist, `invariant gl_Position` bzw. alle Passes auf dieselbe Rechnung stellen. CPU-Seite liegt in `VulkanRenderer.cpp` (nicht in diesem Slice; vor Umsetzung lesen). Test: Vulkan-Bildvergleich, `tests/test_culling.cpp`.

### REN-1010 – `resolveWorldMaterialScalars` läuft bis zu 3×/Frame über alle Objekte mit Hash-Lookup je Objekt/Section
- **ID**: REN-1010
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/MaterialScalars.cpp:11-23`, `:30`, `:36`, `:40-45`; Aufrufer Vulkan `VulkanRenderer.cpp:3890` (EncodeDecalDepth), `:7838` (DrawScene), `:11495` (runGi); D3D11 `:6253`; D3D12 `:10245`
- **Problem**: Jeder Aufruf löst pro `RenderObject` und pro `RenderSection` über `cm->getMaterial(id)` (UUID→Handle→Dense-Array-Lookup, `ContentManager.cpp:2546`) die Skalare neu auf – auch wenn tausende Objekte dasselbe Material teilen. Vulkan ruft das in drei verschiedenen Pfaden pro Frame auf, jeweils mit unverändertem Ergebnis, solange sich Materialien nicht ändern.
- **Code-Auszug**:
```cpp
for (RenderObject& obj : world.objects) resolveObject(cm, obj);        // 43
const MaterialAsset* mat = cm->getMaterial(materialId);                // 16, pro Objekt/Section
```
- **Lösung**: (1) Idempotenz-Flag im `RenderWorld` (`scalarsEpoch = cm->contentEpoch()` + Frame-Nummer): zweiter/dritter Aufruf im selben Frame kehrt sofort zurück. (2) Innerhalb des Aufrufs ein kleines lokales Memo (`UUID → Skalare`, z. B. letzter Treffer + flache Hash-Map) oder die Auflösung in den Extractor verlagern (er kennt bereits `contentEpoch`), dann entfällt die Schleife für nicht geänderte Materialien ganz. 
- **Erwarteter Effekt**: Von 3 auf 1 Durchlauf in Vulkan, Lookups pro eindeutigem Material statt pro Objekt (Schätzung: 10k Objekte × 2-3 Lookups × ~30-50 ns ≈ 1 ms eingespart/Frame; messen).
- **Risiko / Verifikation**: Materialänderung zwischen den Aufrufen im selben Frame (Hot Reload) → Epoch in den Key. Test: neuer Test „Skalare nach Materialänderung aktualisiert“; vorhandene Material-Tests (`tests/test_material_graph.cpp`).

### REN-1011 – Niederschlag als Einzel-`RenderObject` pro Tropfen
- **ID**: REN-1011
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderExtractor.cpp:514-617` (v. a. `:558-615`)
- **Problem**: Jeder Regen-/Schneetropfen wird zu einem vollwertigen `RenderObject` (~300 B inkl. zweier Vektoren, `mat4`, AABB) mit eigener CPU-Billboard-Matrix (Cross-Products, 2-3 `normalize`, `sqrt`) und läuft danach durch Frustum-Cull, `RenderSorter` (SortKey 32 B je Objekt), `GeometryPass` (Run-Bildung, danach `instanceTransforms`-Kopie 64 B je Tropfen, siehe REN-1002) und Occlusion. Der Kommentar nennt „tens of thousands of drops per frame“. Außerdem wird `out.objects.resize(base + n)` zuerst alle neuen Objekte default-initialisiert und dann überschrieben; im `parallel_for` läuft pro Partikel ein `std::upper_bound` über die Emitter.
- **Code-Auszug**:
```cpp
out.objects.resize(base + pin.size());                                  // 562
const PrecipEmitter& em = *std::upper_bound(emitters.begin(), emitters.end(), i, ...);  // 568 – pro Partikel
```
- **Lösung**: Niederschlag wie die `ParticleBatch`-Pipeline (GPU-Instancing, `ParticleShaderTemplates`) als eigener Batch führen: pro Tropfen nur `{position, velocity}` (24 B) hochladen, Billboard-Basis im Vertex-Shader (kamerabezogen). Alternativ minimal: statt `upper_bound` je Partikel pro Emitter einen eigenen `parallel_for` über dessen Range (Emitterzahl ≈ 1).
- **Erwarteter Effekt**: Bei 20-50k Tropfen: ~300 B → 24 B pro Partikel und keine CPU-Matrix, kein Sort/Cull/Instanz-Kopie (Schätzung: mehrere ms CPU/Frame bei Starkregen; messen).
- **Risiko / Verifikation**: Optik muss bitnah gleich bleiben (Billboard-Maße 0.6/0.02, 0.16); Backend-Arbeit in allen fünf Backends (neuer Draw-Pfad), daher der teuerste Vorschlag – nur nach Messung. Zwischenlösung (Emitter-Loop statt `upper_bound`) ist risikolos. Test: `tests/test_particles.cpp`.

### REN-1012 – Mehrfach duplizierte Shader-Blöcke in `MaterialShaderLibrary.cpp`
- **ID**: REN-1012
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:418-449` (heLocalShadowFactor), `:539-570` (heFwdClusterShadow), `:1158-1189` (heClusterShadow); Weather/Material-Split `:648-671`, `:1198-1212`, `:1807-1818`; HLSL-Pin-Tabellen `:1368-1387` vs. `:2714-2734`; Fresnel/Fog-Faktor in `kSSRCompositeFS` `:1845-1867` vs. heLitP/heApplyFog
- **Problem**: Dieselbe Logik existiert 3× (lokale Schatten-PCF), 3× (Schnee/Nässe-Material-Aufspaltung), 2× (HLSL-Register-Pins) als Textkopien mit „SYNC“-Kommentaren und Drift-Tests (`test_material_graph.cpp`, `test_culling.cpp`), die die Kopien nur „modulo textureLod“ vergleichen. Jede Änderung (z. B. REN-1007) muss an 3-6 Stellen erfolgen.
- **Code-Auszug**:
```glsl
float heLocalShadowFactor(int i, vec3 worldPos, vec3 n) { ... }                    // 418
float heFwdClusterShadow(vec4 posType, vec4 params, vec3 worldPos, vec3 n) { ... }  // 539 – gleicher Körper
float heClusterShadow(vec4 posType, vec4 params, vec3 worldPos, vec3 n) { ... }     // 1158 – gleicher Körper
```
- **Lösung**: Gemeinsame GLSL-Snippets als `constexpr const char*` (oder per `std::string`-Zusammensetzung wie `buildDeferredResolveSource`) – z. B. `kLocalShadowCore` (explizite Licht-Daten) einmal definieren; `heLocalShadowFactor(i,…)` ruft `heLocalShadowAt(lightPosType, params.y, …)`; `textureLod`-Variante per bereits vorhandenem `explicitLodClusterSamples`-Ersetzer. HLSL-Pin-Liste einmal als `kHlslMaterialPins` bauen und `compileResolveVariant` nur die Zusatz-Pins anhängen.
- **Erwarteter Effekt**: ~250 Zeilen weniger Shader-Text, Drift-Risiko sinkt; keine Laufzeitänderung.
- **Risiko / Verifikation**: Der Quelltext gelangt in precompilierte/gecachte Shader (Hash ändert sich → Pipeline-Cache-Neuaufbau, PSHD-Blobs im Pak sind unabhängig, weil sie kompiliert gespeichert sind – prüfen). Die String-Vergleichs-Guards in `tests/test_material_graph.cpp`/`tests/test_culling.cpp` anpassen; HLSL-FXC-Test (Windows) und Metal-Compile-Test laufen lassen.

### REN-1013 – Sieben handgepflegte GI-Kernel-Kopien, obwohl `he::shaderc` Cross-Compile kann
- **ID**: REN-1013
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/shaders/gi_probe.comp`, `gi_probe_hw.comp`, `gi_probe_hw.hlsl`, `gi_shadow.comp`, `gi_shadow_hw.comp`, `gi_shadow_hw.hlsl` (Banner `gi_probe_hw.comp:13-51`, `gi_shadow_hw.comp:11-39`); Parallelstellen: `Backends/D3D_Shared/HlslSources.h` (`kGiTraversalHLSL`, `kGiProbeCSHLSL`, `kGiShadowCSHLSL`), `Backends/OpenGL/OpenGLRenderer.cpp` (`kGiTraversalGLSL`, `kGiProbeCS`, `kGiShadowCS`), `Backends/Metal/MetalRenderer.mm` (`kGISWMSL`, `kGIProbeMSL`, `kGIShadowMSL`)
- **Problem**: DDGI-Probe-Update und GI-Shadow-Kernel existieren je API-Dialekt und je Traversal-Pfad (SW-BVH / HW-RayQuery) – laut Banner nichts generiert, nur ein Guard-Test für drei Dateien, vier String-Kopien „out of reach“. Gleichzeitig kompiliert die Engine Sky (`SkyShaderSource.h`) und Material-Shader bereits aus **einer** GLSL-Quelle per `he::shaderc` für Metal/HLSL/GLSL.
- **Code-Auszug**:
```
// gi_probe_hw.comp:13: "SYNC: seven hand-kept copies of this kernel ... Nothing generates them from a common source"
```
- **Lösung**: Kernel einmal als `.comp`-GLSL mit `#ifdef HE_RAYQUERY` (SW-BVH-Traversal vs. `rayQueryEXT`) pflegen, über `he::shaderc` (SPIRV-Cross) zu HLSL (SM 5/6), MSL und GLSL430 transpilieren (wie `deferredResolve`/`compileResolveVariant`), Binding-Pins wie bei `kHlslMaterialPins`. RayQuery→DXR/MSL-Intersector bleibt ggf. eine kleine backend-spezifische Funktion (`giSceneAnyHit`/`giSceneClosestHit`), der Rest (Kegel-Sampling, Hysterese, Oct-Mapping, Feld-Irradiance) ist identisch.
- **Erwarteter Effekt**: Vier String-Kopien entfallen, Drift-Risiko in einem Subsystem, das laut Banner „nirgends durch Bildtests“ gedeckt ist. Aufwand hoch (mehrere Tage, Hardware-Verifikation auf allen Backends).
- **Risiko / Verifikation**: HW-RayQuery-Pfade (SPIRV-Cross unterstützt `GL_EXT_ray_query` nur eingeschränkt) – ggf. nur die SW-Kopien vereinheitlichen. Absichern: `tests/test_culling.cpp` („GI kernels“), `tests/test_gi_bvh.cpp`, Vergleichsbild pro Backend.

### REN-1014 – SW-Traversierung: lineare Instanzschleife (TLAS) pro Strahl, Closest-Hit ohne Nahe-zuerst-Ordnung
- **ID**: REN-1014
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/shaders/gi_shadow.comp:97-108`; `gi_probe.comp:61-98`, `:101-130`
- **Problem**: `giSceneAnyHit`/`giSceneClosestHit` iterieren **alle** Instanzen pro Strahl (`for (i < instCount)`), transformieren Ursprung und Richtung (Mat4×Vec4 + Mat3×Vec3 aus einem 112-B-Struct) und testen erst dann die BLAS-Wurzel. Kosten pro Strahl O(Instanzen); mit der Pixel-Auflösung × `spp` + bis zu 8 Sekundärstrahlen je Probe-Texel skaliert das linear in der Szenenkomplexität (Bäume/Props als eigene Instanzen). `giBlasClosest` pusht beide Kinder ohne Distanz-Ordnung (`:91-95`), was bei Closest-Hit mehr Knoten besucht, als nötig.
- **Code-Auszug**:
```glsl
for (int i = 0; i < instCount; ++i) {
    vec3 oL = (giInsts[i].invTransform * vec4(o, 1.0)).xyz;      // gi_shadow.comp:102
    vec3 dL = mat3(giInsts[i].invTransform) * d;
    if (giBlasAnyHit(...)) return true; }
```
- **Lösung**: Echten TLAS-BVH über die Instanz-Weltboxen auf der CPU bauen (`GiBvh` kann das Prinzip bereits, `GiBvh.h:8` nennt „flat instance array stays renderer-side“), als weitere Knotenliste hochladen und im Kernel zuerst die Welt-AABB testen, bevor `invTransform` angewendet wird; Minimalvariante: Welt-AABB pro Instanz (aus `GiInst`, 1 vec4-Paar) vor der Strahl-Transformation testen. Closest-Hit: Kind mit kleinerem `tN` zuerst absteigen.
- **Erwarteter Effekt**: Für Szenen mit einigen hundert Instanzen Reduktion der pro Strahl durchlaufenen Instanzen um 1-2 Größenordnungen (Schätzung; GI-Pass-GPU-Zeit messen, `HE_GPU_TIMERS`/`GpuPassScope`).
- **Risiko / Verifikation**: Ergebnis muss identisch bleiben (Treffer-Menge, `t`-Werte): `tests/test_gi_bvh.cpp` (CPU-Referenz `giBvhIntersect`) erweitern. Gleiche Änderung in den HLSL/GL/MSL-Kopien nötig (siehe REN-1013).

### REN-1015 – Sky-Noise: 64 MiB 3D-Textur, CPU-Bake bei jedem Renderer-Start
- **ID**: REN-1015
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/SkyNoise3D.cpp:57`, `:63-81`; Aufrufer: `OpenGLRenderer.cpp:4407-4411`, `VulkanRenderer.cpp:9141-9145`, `D3D11Renderer.cpp:5464-5468`, `D3D12Renderer.cpp:2704-2708`, `MetalRenderer.mm:7592-7593` (Release: `kNoiseN = 256`)
- **Problem**: `n = 256` ergibt 256³ × 2 Kanäle × 2 B = **64 MiB** CPU-Vektor, danach als 3D-Textur (64 MiB VRAM) pro Renderer-Instanz. Der Bake (16,7 M Voxel × 27 Worley-Zellen, bis 8 `std::thread`) läuft synchron im Renderer-Init (Kommentar nennt früher 10,7 s, jetzt mit Jitter-Tabelle schneller, Restzeit unbekannt). Rohe `std::thread`s statt `JobSystem`. Der `.r`-Kanal ist nur ein `starHash` pro Voxel (kein Gewinn gegenüber Berechnung im Shader außer Filterung).
- **Code-Auszug**:
```cpp
std::vector<uint16_t> d(static_cast<size_t>(n) * n * n * 2);                 // 57 – 64 MiB bei n=256
const int workers = n < 32 ? 1 : std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
```
- **Lösung**: (1) Messen (Bake-Zeit, VRAM). (2) Optionen: Cache der gebackenen Daten auf Disk (deterministisch, Hash der Parameter), Bake im Hintergrund-Job statt im Init, oder Auflösung/Format senken (RG8 → 32 MiB, 128³ → 8 MiB; Worley mit Wrap hält sich gut) – optische Änderung. (3) Bei mehreren Renderern (Editor-Viewports + Preview) denselben Vektor teilen (`static` Cache hinter dem Funktionsaufruf).
- **Erwarteter Effekt**: Start-/Ladezeit und 64 MiB RAM/VRAM weniger (Schätzung je nach Variante).
- **Risiko / Verifikation**: Bytes sind „pinned“ (Kommentar in Z. 25-28: Bit-Determinität über Compiler/Backends): Disk-Cache/Teilen ändert nichts, Auflösungsreduktion schon (Wolken-Look). Test: `tests/test_sky_shader.cpp`, ein Byte-Hash-Test des Bakes.

### REN-1016 – Toter Resource-/Handle-Unterbau und doppelte `RenderGraph::execute`-Überladung
- **ID**: REN-1016
- **Kategorie**: F / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/src/RenderResourceManager.cpp` (komplett), `include/HorizonRendering/GPUMemoryAllocator.h:10-14`, `src/HE_Rendering/src/GPUMemoryAllocator.cpp`; `RenderObject.h:54-55`, `CommandBuffer.h:21-22`; `RenderPass.cpp:66-67`, `:181-182`; `RenderGraph.cpp:24-33`
- **Problem**: `RenderResourceManager` wird außer in `tests/test_rendermanager.cpp` nirgends benutzt (Grep über `src/`); `GPUMemoryAllocator.h` sagt selbst „which the backends do not use either“. `RenderObject::meshHandle/materialHandle` und `DrawCall::mesh/material` (4 × 8 B) werden nie gesetzt (immer `invalid()`), aber pro Objekt/DrawCall mitkopiert (`RenderPass.cpp:66-67`). Die `execute(…, CommandBuffer&)`-Überladung wird nur von Tests benutzt (Backends nutzen die `PassSink`-Form).
- **Code-Auszug**:
```cpp
dc.mesh     = first.meshHandle;     // RenderPass.cpp:66 – immer RenderHandle::invalid()
dc.material = first.materialHandle;
```
- **Lösung**: Entscheiden: entweder Plan „3.7 Handle-Auflösung“ wirklich umsetzen, oder `RenderResourceManager`, `GPUMemoryAllocator`, die vier Handle-Felder und die Merged-Buffer-`execute`-Überladung entfernen (Tests ggf. auf `PassSink` umstellen). Entfernen spart ~32 B je `DrawCall`/`RenderObject` (siehe REN-1002).
- **Erwarteter Effekt**: Weniger Code (~150 Zeilen), kleinere Structs (Cache), eine Abstraktion weniger.
- **Risiko / Verifikation**: Öffentliche Header (DLL-Export) – externe Nutzer? `tests/test_rendermanager.cpp`, `tests/test_rendergraph.cpp` anpassen.

### REN-1017 – Extractor-Kleinkram: Magic Numbers, rohe Licht-Typ-Ints, doppelte Day/Night-Logik
- **ID**: REN-1017
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `RenderExtractor.cpp:64`, `:887-890`, `:916`, `:1062`, `:1068`, `:1089`, `:1106`, `:1064`, `:1515`/`:1569`, `:1531-1541`/`:1553-1560`; `RenderPass.cpp:272-273`; `RenderConstants.h:14`; `LightPacking.h:27`
- **Problem**: (a) `constexpr int kMaxShaderLights = 8;` dupliziert `kMaxLightWindow` (`LightPacking.h:27`); ändert jemand eine Konstante, divergieren Fenster und Schatten-Layer-Zuweisung still. (b) `l.type != 0`, `== 1`, `== 2` statt `HE::LightType` (`orderLightWindow`, `fitDirectionalShadow`, `assignLocalShadowLayers`). (c) `ShadowPass::describe()` hartkodiert `2048` statt `kShadowMapResolution`. (d) `extractTransforms(world, reg)` hat einen ungenutzten `entt::registry&`-Parameter. (e) `applyDayNight`: Luminanzformel doppelt, Sonne-/Mond-Synthese als Kopie, und mehrfaches „push_back kann Zeiger invalidieren“-Zurücksetzen (`moonLight = nullptr; for … find`). (f) `assignLocalShadowLayers` alloziert pro Aufruf ein `std::vector<int> shadowed` (max. 8 Einträge).
- **Code-Auszug**:
```cpp
constexpr int kMaxShaderLights = 8;                   // 1062
io.output.width = 2048;                               // RenderPass.cpp:272 (kShadowMapResolution = 2048 existiert)
moonLight = nullptr;  for (LightData& l : out.lights) if (l.envRole == 2) { moonLight = &l; break; }   // 1539-1541
```
- **Lösung**: `kMaxLightWindow` und `kShadowMapResolution` verwenden; `LightType`-Enum-Vergleiche; `applyDayNight`: erst fehlende Sun-/Moon-Lichter anhängen (Indizes statt Zeiger), danach einmal Zeiger ermitteln und befüllen; `std::array<int,8>` für `shadowed`; ungenutzten Parameter entfernen.
- **Erwarteter Effekt**: Lesbarkeit/Drift-Sicherheit; Laufzeit vernachlässigbar.
- **Risiko / Verifikation**: Reihenfolge der angehängten Lichter (Sonne vor Mond) muss gleich bleiben (`orderLightWindow` stabil). `tests/test_light_window_order.cpp`, `tests/test_culling.cpp`.

### REN-1018 – `MaterialShaderLibrary`: handgepackte Cache-Keys und Whitespace-Nachbau in `assembleVertex`
- **ID**: REN-1018
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/material/MaterialShaderLibrary.h:510-522`; `src/material/MaterialShaderLibrary.cpp:1467`, `:1980`, `:2002`, `:2025`, `:2051`, `:2218`, `:2242`, `:2259`, `:2538-2539`, `:920`; `assembleVertex` `:41-45`, `:65-66`
- **Problem**: Zehn `unordered_map<int|uint64_t, Compiled>` mit von Hand zusammengebauten Schlüsseln (`backend*4 + k`, `+ 64`, `0x100 + backend`, `hash ^ (K * (backend+1))`); der Header-Kommentar (`:519-521`) gesteht eine frühere Schlüsselkollision. Außerdem emuliert `assembleVertex` die Ausrichtungs-Leerzeichen der drei Ur-Varianten (`wpPad`), obwohl Whitespace im GLSL-Quelltext die Kompilierung nicht beeinflusst (Begründung in Z. 41-45 „a single stray character is a runtime shader-compile failure“ gilt nicht für Leerzeichen).
- **Code-Auszug**:
```cpp
const int key = static_cast<int>(backend) * 4 + 3;                  // 1980 (m_ssrCache)
const char* wpPad = wpo ? "   " : (ssbo ? "  " : " ");               // 66
```
- **Lösung**: Eine `enum class ShaderSlot { …}` und `std::array<std::optional<Compiled>, kSlots>` pro Backend (oder `struct Key{Backend b; Slot s;}` mit Hash); Hash-Keys für Fragment/Vertex als `struct{uint64_t hash; Backend; bool clustered;}`. `wpPad` ersatzlos streichen (Hash-Wechsel einmalig akzeptieren).
- **Erwarteter Effekt**: Weniger fehleranfälliger Code; keine Laufzeitänderung.
- **Risiko / Verifikation**: Entfernen des Padding ändert `std::hash` des Vertex-Quelltexts → In-Process-Caches neu (kein Disk-Cache gefunden; prüfen, ob `ShaderCompiler.h` auf Platte cached). Tests: `tests/test_material_graph.cpp`.

### REN-1019 – `OcclusionCuller`: Rasterizer-Inneres, Doppel-Lookups, Vollsortierung (Feature standardmäßig aus)
- **ID**: REN-1019
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/OcclusionCuller.cpp:139`, `:164-165`/`:342-343`, `:178-186`/`:287-292`, `:189-192`, `:200-257`, `:294-309`
- **Problem**: `Settings::enabled = false` per Default, nur GL/Metal rufen `refine` (`OpenGLRenderer.cpp:10929`), daher niedrige Priorität. Dennoch: (a) pro Pixel dreimal `edge(...)` mit `swap`-Verzweigung statt inkrementeller Edge-Funktion/Tile-Reject; (b) jeder Occluder transformiert **alle** Vertices (`m_clipScratch.resize(vcount)`), auch wenn nur ein Index-Teilbereich (Section) gezeichnet wird; (c) `getStaticMesh`/`opaqueRanges`/`boxCorners` werden für ausgewählte Kandidaten doppelt berechnet; (d) `std::sort` aller Kandidaten, danach `resize(maxOccluders=48)` (→ `nth_element`/`partial_sort`); (e) `m_depth.assign(W*H, 0)` pro Frame (256×~144, klein).
- **Code-Auszug**:
```cpp
if (s * edge(x0, y0, x1, y1, cx, cy) < 0.0f) continue;      // 249-251: 3× pro Pixel mit swap-Branch in edge()
std::sort(candidates.begin(), candidates.end(), ...);       // 189, danach nur 48 behalten
```
- **Lösung**: Edge-Koeffizienten (A,B,C) pro Dreieck vorberechnen und über die Zeile inkrementieren (die „kanonische Reihenfolge“ für Nahtfreiheit bleibt, indem die Koeffizienten aus der kanonischen Kante abgeleitet werden); Vertices nur aus dem Indexbereich transformieren (Min/Max-Index je Range) bzw. Transformation lazy per Cache; `partial_sort` mit 48.
- **Erwarteter Effekt**: Gering bei 256×144 und ≤ 48 Occludern (Schätzung < 0,2 ms); nur relevant, falls das Feature standardmäßig aktiviert wird.
- **Risiko / Verifikation**: Nahtfreiheit (kein Loch entlang Diagonalen) ist ein explizites Designziel. Test: `tests/test_culling.cpp` (Occlusion-Fälle) erweitern um Bitgleichheit des Tiefenpuffers vor/nach Umbau.

### REN-1020 – `sortBackToFront` berechnet den Key im Vergleich über Zeiger auf große DrawCalls
- **ID**: REN-1020
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderSorter.cpp:67-76`; Aufrufer D3D11 `:6966`, D3D12 `:10929`, Vulkan `:8205`
- **Problem**: Der Kommentar in `sort()` (Z. 7-11) begründet, warum der Key vorab berechnet wird; `sortBackToFront` macht es genau umgekehrt: pro Vergleich zwei Matrix-Spaltenzugriffe über `const DrawCall*` (~260 B-Struct, Cache-Miss), O(n log n) Mal.
- **Code-Auszug**:
```cpp
return backToFrontKey(a->transform, camPos) > backToFrontKey(b->transform, camPos);   // 73-74 im Comparator
```
- **Lösung**: `std::vector<std::pair<float, const DrawCall*>>` (Key vorab) sortieren, danach zurückschreiben. Header-Kommentar „std::sort ist nicht stabil“ bleibt gültig – das Verhalten bei Gleichstand dürfte sich minimal ändern (nicht stabil, daher nicht beobachtbar spezifiziert).
- **Erwarteter Effekt**: Klein bei wenigen hundert transparenten Draws (Schätzung µs); relevant bei vielen transparenten Einzel-Draws (Partikel-Objekte, Tint-Fades).
- **Risiko / Verifikation**: Reihenfolge bei exakt gleicher Distanz kann sich ändern (Header erklärt, dass absichtlich nicht „repariert“). Test: `tests/test_rendergraph.cpp`/Transparenz-Tests prüfen.

### REN-1021 – `queryUnderwaterCamera` baut pro Voll-Extract einen frischen `WaterIndex`
- **ID**: REN-1021
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderExtractor.cpp:1195-1203`, `:1418`; `src/HE_Scene/src/WaterQuery.cpp:33-86`
- **Problem**: Jeder Voll-Extract (auch pro Viewport) durchläuft alle `TerrainComponent`, holt für jedes Terrain mit Wasser `worldMatrixOf` (Hierarchie-Walk), `glm::inverse`, baut einen `std::vector<Entry>` und sortiert ihn; der im `refresh` vorgesehene Cache der Wet-Rect-Daten (`WaterQuery.cpp:75-86`) greift nie, weil `build()` jedes Mal bei leerem `m_entries` beginnt. Die Kosten fallen nur bei Terrains mit Wasserbodies an, dafür garantiert jeden Frame.
- **Code-Auszug**:
```cpp
const water::WaterIndex index = water::WaterIndex::build(world);     // 1202
if (!index.surfaceAt(cameraPos.x, cameraPos.z, 0.0, surf)) return out;
```
- **Lösung**: Schnell-Exit vor dem Build (kein `TerrainComponent` mit `water.bodies` → sofort leer; per `registry.view` mit `any_of`), oder einen `WaterIndex` als Member des `RenderExtractor` halten und per `refresh` aktualisieren (Cache bleibt erhalten). 
- **Erwarteter Effekt**: Entfällt 1 Vektor-Allokation + Inverse je Wasser-Terrain/Frame (Schätzung µs; nur bei vorhandenen Wasserterrains).
- **Risiko / Verifikation**: `m_entries` hält `const TerrainComponent*` (Zeiger auf Registry-Speicher) – bei persistentem Index nach `refresh` vor jeder Abfrage gültig halten. Test: `tests/test_underwater_camera.cpp`.

### REN-1022 – `ScenePick::raycast`: Matrixinversion vor dem Boxtest, Normalen-Transpose bei jedem Treffer
- **ID**: REN-1022
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/ScenePick.cpp:35-79`, `:47`, `:77`
- **Problem**: Editor-Interaktion (pro Mausereignis, nicht pro Frame). Für **jedes** Objekt wird `glm::inverse(obj.transform)` berechnet und `lookup(...)` (`std::function`) aufgerufen, bevor die AABB geprüft wird; `worldBounds` der Objekte (bereits vorhanden) wird nicht für einen billigen Strahl-Vorab-Test genutzt. Bei jedem *verbessernden* Dreieck wird `glm::transpose(invModel)` + `normalize` ausgeführt statt einmal am Ende.
- **Code-Auszug**:
```cpp
const glm::mat4 invModel = glm::inverse(obj.transform);                               // 47
best.normal = glm::normalize(glm::vec3(glm::transpose(invModel) * glm::vec4(nLocal, 0.0f)));  // 77
```
- **Lösung**: Vor `lookup`/`inverse` Strahl gegen `obj.worldBounds` (falls valid) testen und mit `bestT` abbrechen; Normale erst nach der Schleife aus gemerktem `bestObj`/`nLocal` berechnen.
- **Erwarteter Effekt**: Klick-/Hover-Latenz bei 10k+ Objekten (Schätzung: ms → <1 ms); niedrige Priorität (Editor-only).
- **Risiko / Verifikation**: Gleiches Pick-Ergebnis; `tests/test_scene_pick.cpp`, `tests/test_viewport_pick.cpp`, `tests/test_gizmo_pick.cpp`.

### REN-1023 – `gi_probe.comp`/`gi_probe_hw.comp`: Lese-nach-Schreib-Hazard auf dem EMA-Image (unsicher)
- **ID**: REN-1023
- **Kategorie**: F (Korrektheit/Nebenläufigkeit)
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/shaders/gi_probe.comp:153-160`, `:185`, `:267-273`; `gi_probe_hw.comp:127-133`, `:159`, `:241-247`; Vergleich `gi_probe_hw.hlsl:121-123` (liest `uIrrPrev`-SRV-Kopie)
- **Problem**: Die GLSL-Kernel lesen die Irradiance der **Nachbarprobes** per `imageLoad(uIrr, …)` (`giSampleFieldIrradiance`), während andere Workgroups desselben Dispatches ihre Texel via `imageStore` in dasselbe Image schreiben. Der Kommentar („imageLoad returns the previous frame's values, only this thread's own texel is written“) gilt nur für das eigene Texel; Nachbarprobe-Tiles der aktuellen Batch können bereits überschrieben sein. Es gibt kein `coherent`/Barrier. Die HLSL-Variante vermeidet das mit einer SRV-Kopie (`uIrrPrev`). Folge: nichtdeterministische Mehrfach-Bounce-Beiträge (EMA dämpft stark).
- **Code-Auszug**:
```glsl
sum += imageLoad(uIrr, tile + octTexel).rgb * w;     // gi_probe.comp:185 – Nachbarprobe, evtl. schon neu geschrieben
imageStore(uIrr, outCoord, vec4(mix(radiance, oldIrr.rgb, hIrr), 1.0));   // :269
```
- **Lösung**: Wie HLSL: Ping-Pong-Images (Prev/Cur) oder Kopie des Atlas vor dem Dispatch, `uIrrPrev` als `sampler2D`/readonly-Image. Alternativ Probe-Batches so wählen, dass Nachbarn nicht im selben Dispatch liegen (Round-Robin-Cursor erlaubt das nicht garantiert).
- **Erwarteter Effekt**: Deterministische GI; ggf. leichtes Flackern der Bounce-Komponente verschwindet. **Unsicher**: ob der Effekt sichtbar ist (nicht gemessen); Speicher +1 Atlas.
- **Risiko / Verifikation**: Zusätzlicher Atlas/Barrier pro Frame (Perf-Kosten messen); Gleichstand mit D3D/GL/Metal herstellen (`kGIProbeMSL`, `kGiProbeCS` prüfen, ob dort dasselbe gilt – nicht gelesen).

### REN-1024 – Shader-Mikrooptimierungen (nur nach Messung)
- **ID**: REN-1024
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `shaders/ssao_blur.frag:14-16`; `shaders/gi_atrous.frag:66-78` (`:75`); `shaders/postfx_bloom_blur.frag:8-11`; `shaders/taa_resolve.frag:29-37`; `shaders/postfx_underwatercaustics.frag:75-88`
- **Problem**: (a) `ssao_blur`: 16 Taps für eine 4×4-Box (`x,y ∈ [-2,2)`), mit linearem Sampler und Pixel-Offsets exakt durch 4 bilineare Taps bei halben Texelversätzen (±1) ersetzbar. (b) `gi_atrous`: `pow(max(dot(...),0), 32.0)` pro Tap (25×) – `x^32` per fünf Quadrierungen; zusätzlich holt jede Tap Position und Normale getrennt. (c) `postfx_bloom_blur`: 9-Tap-Gauß → 5 Taps mit Linear-Sampling-Trick. (d) `taa_resolve`: 9× `length(v)` zur Maximum-Suche → `dot(v,v)` vergleichen. (e) `postfx_underwatercaustics`: Winkel-/`cos`/`sin`-Berechnung je Train und Pixel aus `const`-Array (6×) – kann der Compiler bei Unroll falten, sonst Vorberechnung in Push-Constants.
- **Code-Auszug**:
```glsl
for (int x = -2; x < 2; ++x) for (int y = -2; y < 2; ++y) sum += texture(uAOInput, vUV + vec2(x, y) * texel).r;   // ssao_blur.frag:14-16
w *= pow(max(dot(n, normalize(tap(uGNorm, uv).xyz)), 0.0), 32.0);                                              // gi_atrous.frag:75
```
- **Lösung**: wie oben aufgeführt; jeweils nur mit GPU-Timer-Nachweis.
- **Erwarteter Effekt**: Kleine Gewinne pro Pass (Schätzung: ssao_blur ~4× weniger Fetches; übrige < 5 %); in Summe vermutlich unter der Messschwelle – bewusst als „nur bei Bedarf“ geführt.
- **Risiko / Verifikation**: (a) ist exakt nur bei `LINEAR`-Sampler und gleicher Auflösung von Ein-/Ausgabe (Texel-Mittelpunkte) – sonst Abweichung; (b)-(d) geringfügige Rundungsunterschiede. Bildvergleich Vulkan; D3D/GL/Metal-Twins (`kSmaaFS`, `kGiAtrousFS`, `giShadowAtrous`) parallel ändern.

---

## Gelesen

Alle 55 Dateien des Slice 10 vollständig gelesen (inkl. `MaterialShaderLibrary.cpp` 2768 Z. in drei Blöcken und `RenderExtractor.cpp` 1590 Z. in zwei Blöcken), dazu die in der Slice-Liste fehlenden sechs Shader `gi_probe.comp`, `gi_probe_hw.comp`, `gi_probe_hw.hlsl` (nur Kopf + Hauptteil, Zeilen 1-60 und 120-252), `gi_shadow.comp`, `gi_shadow_hw.comp`, `gi_shadow_hw.hlsl` (nur per Grep auf Schlüsselstellen, nicht vollständig); außerdem gezielt `CommandBuffer.h/.cpp`, `RenderObject.h`, `RenderSorter.h`, `RenderGraph.h`, `OcclusionCuller.h` (Defaults), `WaterQuery.cpp`, `EntityActive.*` und Backend-Aufrufstellen (nur Ausschnitte).
