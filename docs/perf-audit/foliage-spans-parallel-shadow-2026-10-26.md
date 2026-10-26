# Foliage einmal auspacken, Schatten-Ansichten parallel vorbereiten (26.10.2026)

Zweig `claude/perf-foliage-persistent-encode` ab `release/0.7.0` @ ddd4a93e. Folgt auf
`frame-parallelism-audit-2026-10-26.md` (Hebel 1 vollständig, Hebel 2).

## 1. Warum

Time Profiler, Foliage-300k-Szene, 10 s, Hauptthread:

| Posten | Anteil |
|---|---|
| Auspacken der Cluster in Listen (`writeInstancePieces`) | 28 % |
| Matrix-Produkte (Auspacken mit General-Eltern und `mvp`) | 25 % |
| Nullen frisch vergrößerter Listen (`__bzero`) | 9 % |
| `UploadMvpModel` | 8 % |
| Encoder öffnen und Draws absetzen | ca. 5 % |

Die Matrizen der Pflanzen ändern sich nicht, trotzdem wurde jeder Block in jedem Pass jedes Frames
neu ausgepackt: drei Kaskaden, SSAO, Szene. Das eigentliche Kodieren ist kein Engpass. Deshalb ist
„paralleles Kodieren“ hier als paralleles **Vorbereiten** umgesetzt: Cull, Sortieren und Läufe
pro Schatten-Ansicht laufen gleichzeitig. Die Metal-/GL-Aufrufe bleiben auf ihrem Thread.

## 2. Was sich ändert

- **`instanceWorldMatrices(block)`** (`RenderWorld.h`, `RenderWorld.cpp`):
  - liefert die Weltmatrizen eines Blocks, einmal berechnet und über Frames behalten;
  - der Schlüssel besteht aus Speicher, Bereich und den Bits der Elternmatrix, und der Cache hält den Speicher fest;
  - Identitäts-Eltern brauchen keine Kopie, der Zeiger zeigt direkt in den Speicher;
  - `instanceWorldCacheNextFrame()` gibt frei, was 8 Frames lang niemand gelesen hat.
- **`InstanceSpan`** (`CommandBuffer.h`): n Matrizen, gelesen dort, wo sie liegen.
  - `RenderSorter::DepthBatchList::useSpans` und `GeometryPass::setInstanceSpans` bilden dieselben Läufe und Draws, nur mit Spans statt Kopien.
  - Lesen geht über `DrawCall::instanceTotal / forEachInstance / firstInstance`.
- **Metal** (`MetalRenderer.mm`):
  - Spans in Kaskaden, SSAO-Vorpass, Forward- und G-Buffer-Szene; der GI-Vorpass behält Kopien;
  - der Schattenpass ist in vier Schritte geteilt (`ShadowView`): (1) alle Ansichten gleichzeitig cullen, sortieren und Läufe bilden; (2) auf dem Hauptthread Meshes auflösen und Ring-Abschnitte reservieren; (3) alle `mvp`-Produkte gemeinsam auf den Workern; (4) kodieren;
  - der Kamera-Cull für SSAO und Szene läuft in Schritt 1 mit, außer bei aktivem Occlusion-Culling.
- **OpenGL**:
  - Spans im Graph und im Schattenpass;
  - `BufferInstanceSpans` schreibt jeden Span per `glBufferSubData` direkt aus dem Cache, ohne CPU-Kopie;
  - Schatten-Ansichten werden parallel vorbereitet.
- **A/B-Schalter:**
  - `HE_INSTANCE_SPANS=0` stellt die Kopien wieder her;
  - `HE_MTL_PARALLEL_SHADOW=0` bereitet die Ansichten nacheinander vor (gilt für Metal und GL).

## 3. Messung (Metal, M5, P-Kerne, verstecktes Fenster, ABAB ×3, Median der Scope-p50 pro Frame)

**Foliage 300k, Kopien → Spans:**

| Pass | vorher | nachher |
|---|---|---|
| Schatten | 2,17 ms | 1,80 ms |
| SSAO | 1,03 ms | 0,74 ms |
| Szene | 1,06 ms | 0,67 ms |
| **Summe** | **4,26 ms** | **3,21 ms (−25 %)** |

Die 50k-Szene ohne Foliage bleibt unverändert.

**Parallele Vorbereitung, aus → an (derselbe Build):**
- 50k-Szene: Schatten plus SSAO 1,32 → 0,98 ms, weil der Kamera-Cull neben den Kaskaden läuft.
- Foliage: gleich. Cull und Sortieren kosten dort 0,09 ms, die Zeit liegt in den `mvp`-Produkten (1,5 ms, schon parallel).

**Bild:** `frame_pixel_ab.sh all` gegen den Stand davor, nach jedem Schritt 42/42 md5-gleich, auf Metal und auf OpenGL.

## 4. Was auf Windows nachgeholt werden muss (D3D11, D3D12, Vulkan)

Der gemeinsame Code ist fertig, nichts davon braucht ein neues Shader-Layout. Alle drei Backends laden
heute Modellmatrizen hoch: Die Spans schreiben sie direkt in den Upload-Ring, statt sie erst in
`dc.instanceTransforms` oder `DepthBatchList::transforms` zu kopieren. Vorlage ist der GL-Port in diesem Zweig.

**Schritt A, je Backend: Spans im Szenen-Graph**
1. `std::make_unique<GeometryPass>()` durch einen Pass mit `setInstanceSpans(true)` ersetzen (Schalter `HE_INSTANCE_SPANS`).
2. Jeden Leser von `dc.instanceTransforms` umstellen:
   - `.size()` / `.empty()` → `instanceTotal()`;
   - Schleifen → `forEachInstance`;
   - `memcpy(dst, instanceTransforms.data(), …)` → pro Span kopieren (`for (s : dc.instanceSpans) memcpy(dst + k, s.src, s.n*64)`), ohne Spans wie bisher.

| Backend | GeometryPass | Leser von `instanceTransforms` |
|---|---|---|
| D3D11 `D3D11Renderer.cpp` | `:7010` | `:2645`, `:3810` (Vorpässe, `dc->instanceTransforms`), `:7794-7795`, `:8030-8048`, `:8132-8171` |
| D3D12 `D3D12Renderer.cpp` | `:11282` | `:6913`, `:8721`, `:12033`, `:12245-12270`, `:12337-12383`, `:12708` |
| Vulkan `VulkanRenderer.cpp` | `:8645` | `:9078`, `:9406-9428`, `:9503-9537` |

**Schritt B, je Backend: Spans und parallele Vorbereitung im Schattenpass**
1. Pro Ansicht ein `ShadowView` (eigenes `visible`, `sorted`, `RenderSorter`, `DepthBatchList` mit `useSpans`) und ein `parallel_for` über alle Ansichten für cull, `sortByMesh` und `batchDepthCasters`.
2. Danach wie bisher zeichnen; `xf = transforms.data() + b.first` durch die Spans des Laufs ersetzen.
3. Wo ein Backend eigene Command-Lists erlaubt (D3D12, Vulkan), lassen sich die Kaskaden auch tatsächlich parallel aufzeichnen. Das ist optional und auf dem Mac nicht nötig gewesen.

| Backend | Cull/Sort (heute pro Kaskade nacheinander) | `xf`-Leser |
|---|---|---|
| D3D11 | `:7407-7409` (`sort` statt `sortByMesh`, Hebel 4b gleich mitnehmen) | `:7418` |
| D3D12 | `:11680-11682` (ebenso) | `:11691` |
| Vulkan | `:7323-7325` (ebenso) | `:7334` |

**Schritt C, nur Vulkan:** Zwei Stellen packen Cluster noch direkt aus. Sie gehen auf `instanceSpan` + Kopie in den Ring:

| Stelle | Ersetzen |
|---|---|
| `:4141` (`EncodeDecalDepth`) | `appendInstances(obj, m_clusterScratch)` |
| `:14227` (`runSSAO`) | `appendInstances(obj, m_clusterScratch)` |
| `:12545/:12555` und `:14143/:14153` | `batchDepthRuns` mit `useSpans` |

**Dazu aus dem Frame-Audit noch offen** (Hive-Thema 238, Notiz vom 26.10.):
- Hebel 7: Cull über `m_refinedBounds`.
- Hebel 4a: Vulkan cullt und sortiert die Kamera viermal pro Frame (`:4083`, `:8638`, `:12376`, `:14072`).
- Hebel 5: die Szenen-Box für den Wolkenschatten.

**Nötige Tests auf NN-WS03:**
1. `he_tests` grün, vor allem `test_foliage_cluster`. Dessen drei neue Fälle prüfen Spans gegen Kopien bitgenau, im gemeinsamen Code und für jedes Backend gleich.
2. Pixel-A/B je Backend: alter gegen neuen Deploy, die Zeugen aus `frame_pixel_ab.sh` einzeln per `he_shot.py` mit `RHI=D3D11/D3D12/Vulkan`. Erwartet: md5-gleich, wie auf Metal und GL.
3. Perf-A/B mit `he_perf_capture.py` auf der Foliage-300k- und der 50k-Szene, `HE_INSTANCE_SPANS=0` und `HE_MTL_PARALLEL_SHADOW=0` gegen Standard. Verglichen werden die Scope-p50 der Pässe.

## 5. Nicht geprüft

- D3D11, D3D12 und Vulkan: nicht angefasst, sie setzen keinen der Schalter und verhalten sich wie bisher.
- ThreadSanitizer: nicht gelaufen. Die parallelen Aufgaben schreiben nur in ihre eigene `ShadowView` bzw. in `m_cameraCull` und lesen `m_refinedBounds` und `m_renderWorld`.
- Spiel (`HorizonGame`) nicht gesondert gemessen. Es nutzt denselben Renderer.

## 6. Windows: GL, D3D11, D3D12, Vulkan (NN-WS03, RTX 4070, 26.10.2026)

Zweig `claude/foliage-spans-windows` = dieser Zweig + PR #187 (`FrameCull`, `fillInstancePairs`). Die
Hebel aus §4 sind umgesetzt, als gemeinsamer Code statt je Backend einmal:

- **`InstanceRun`** (`CommandBuffer.h`): die Matrizen eines Laufs als Spans oder als kopierte Liste, `DrawCall::instanceRun`,
  `DepthBatchList::run`, `RenderWorld::instanceRun(obj)`. `RenderSorter::fillInstancePairs(run, fill)` und
  `copyInstanceModels` schreiben die Ring-Records Span für Span, ab 4096 Instanzen breit. Jeder Leser von
  `instanceTransforms` in D3D11, D3D12 und Vulkan geht darüber (Szene, Material-Zwilling, SSAO-, GI-, Decal- und
  Reflexions-Vorpass); die Quelltext-Wächter in `test_foliage_cluster` verbieten `instanceTransforms` dort.
- **`FrameCull::prepareShadowViews` + `ShadowViewCpu`:** Cull, `sortByMesh` und Läufe jeder Kaskade und jedes lokalen Layers
  nebeneinander (`parallel_for`), danach wird je Ansicht gezeichnet. GL nutzt dasselbe (sein `ShadowView` ist weg).
- **Schalter einmal:** `RenderSorter::instanceSpansEnabled()` (`HE_INSTANCE_SPANS`) und `parallelShadowViewsEnabled()`
  (`HE_MTL_PARALLEL_SHADOW`); Metal und GL lasen sie je einmal selbst und rufen jetzt diese. **Metal ist deshalb auf dem Mac
  neu zu bauen** (nur die zwei Schalter-Aufrufe geändert, auf Windows nicht übersetzbar).
- `instanceWorldCacheNextFrame()` pro Frame auch in GL, D3D11, D3D12, Vulkan (vorher nur Metal: der Cache gab nie etwas frei).
- Teil D (Hebel 7/4a/5) kam mit #187; Vulkans zwei Reste in §4 C (Decal-Tiefe, SSAO) sind jetzt Spans, der SSR-Vorpass liest den Block in place.

**Bild.** `frame_pixel_ab_win.ps1`-Zeugen (28 je Backend, Wolken aus), alter Deploy (Merge-Stand) gegen neuen:
D3D11 28/28 md5-gleich, D3D12 und Vulkan je 25/28. Die drei übrigen (`dn_0/25/75_fwd`, Sterne) weichen um höchstens 3/255
ab; alt gegen alt rauscht dieselbe Aufnahme um bis zu 30/255. OpenGL nicht neu gemessen (nur der Schalter-Aufruf und
der gemeinsame Schattenpfad geändert, `test_foliage_cluster` prüft Spans gegen Kopien bitgenau).

**Tests.** `test_foliage_cluster`, `test_culling` (neu: `InstanceRun` Spans gegen Liste, `copyInstanceModels`),
`test_graph_instancing`, `test_material_graph`, `test_profiler` (neu: `GpuPassTimeline`), `test_auto_landscape_variants`,
`test_mcp_tools_material`: 305 Fälle grün.

**Perf A/B (Endstand, nach der zweiten Runde unten).** 50k-Objekte-Szene, 2000x1125, CPU-Frame p50, Median aus 4 verschachtelten Läufen,
ohne `--detailed` und mit `HE_GPU_PASS_TIMING=0` (`matrix9.py`), Fenster hidden, Rechner ruhig (Streuung 0,1 bis 0,3 ms):

| Backend | alt (Merge-Stand) | Spans-Stand | Endstand |
|---|---|---|---|
| D3D11 | 9,77 ms | 9,55 ms (-2 %) | **5,01 ms (-49 %)** |
| D3D12 | 9,91 ms | 9,75 ms (-2 %) | **5,09 ms (-49 %)** |
| Vulkan | 9,30 ms | 9,51 ms (+2 %) | **5,64 ms (-39 %)** |

Die Spans allein ändern bei einer Szene ohne Foliage nichts; der Gewinn kommt aus dem Extract, den breiten Cull-/Refine-Pässen und dem
abgeschalteten Schatten-Pass der zweiten Runde. Foliage 300k wurde nur mit schwankender GPU gemessen (VALORANT lief): nicht belastbar, zu wiederholen.

**Zeit pro Pass auf D3D11, D3D12, Vulkan (neu, war bisher nur Gesamt-Frame).** `GpuPassTimeline` + `GpuPassScope`
(`HE_Core/Renderer/GpuPassTimeline.h`): eine Marke je Pass-Grenze, verschachtelte Pässe unterbrechen den äußeren, die GPU-Zeilen
sind exklusiv und ergeben den Frame. Modi `d3d11-ts` (nur während einer Aufnahme), `d3d12-ts`, `vk-ts`; Namen wie GL
(`Shadow`, `SSAO`, `GIShadow`, `GIProbes`, `GIRefl`, `Decals`, `Scene`, `Bloom`, `Tonemap`, `TAA`, `AA`, `UI`). Derselbe Scope ist
der CPU-Scope des Passes (inklusiv). Dazu CPU-Scopes `RenderGraph::record`, `RefineBounds`, `MaterialScalars`, `SortObjects`,
`WaitFrame`, `Present`/`SubmitPresent`.

Erste Beobachtung (50k-Szene, 2000x1125, ein Lauf, nicht A/B): **die Szene ist CPU-gebunden**, 10 bis 15 ms CPU gegen 2 bis 3 ms GPU.
CPU p50 je Frame (D3D11 / D3D12 / Vulkan): `RenderExtractor::extract` 5,7 / 5,5 / 5,2 ms (der größte Posten, etwa 40 %),
`RefineBounds` 1,4 / 1,4 ms, `Shadow` 1,0 / 1,1 / 1,3 ms, `RenderGraph::record` 1,1 / 1,05 ms, `Present` 0,3 ms;
auf D3D11 bleiben etwa 4 ms in `Render` ohne Scope. GPU je Pass (D3D11): SSAO 2,3 ms, Shadow 0,6 ms, Scene 0,16 ms;
(D3D12) SSAO 1,6 ms, Shadow 0,65 ms; (Vulkan) Shadow 1,5 ms, Scene 0,3 ms, Bloom 0,28 ms.

## 6b. Zweite Runde: CPU-Hotspots des ganzen Frames (26.10.2026, gemeinsamer Code, wirkt auf alle Backends)

Auslöser: die 50k-Szene war CPU-gebunden (10 bis 15 ms CPU gegen 2 bis 3 ms GPU). Scopes pro Phase (`Extract::*`, `RefineBounds`, `ShadowPrepare`,
`RenderGraph::record`, ...) zeigten, wo die Zeit steht. CPU p50 auf D3D12 (ms), vor der Runde -> danach:

| Posten | vorher | nachher | was |
|---|---|---|---|
| `Extract::meshes` | 4,6 | 1,5 bis 1,9 | Schleife in Blöcken auf den Workern, Objekte direkt am Platz gebaut (kein Zwischenstruct), Mehrabschnitts-Meshes seriell danach, const-Pools, Mesh-Memo |
| `RenderExtractor::extract` gesamt | 5,5 | 2,3 | dazu `Extract::transforms` 0,33 -> 0,12 (Transform-Scan in Blöcken), Szenen-Box des Schatten-Fits als parallele Reduktion |
| `RefineBounds` | 1,4 | 0,45 | jedes Mesh einmal erfragt (seriell), das Produkt je Objekt wide |
| `ShadowPrepare` | 0,8 | 0,33 | `sortByMesh` als Zählsortierung statt `std::sort` (gleiche Reihenfolge, Test pinnt sie) |
| `RenderGraph::record` | 1,05 | unter 0,1 | `ShadowPass` zeichnet für D3D11/D3D12/GL nichts mehr auf (`setRecordDraws(false)`): es legte pro Schattenwerfer einen `DrawCall` an, den diese Backends nie lasen |
| `MaterialScalars` | 0,2 | unter 0,1 | wide, mit Memo |

Probiert und wieder entfernt, weil nicht messbar besser: die Felder von `RenderObject` umordnen (hot zuerst), ein Batch-Schlüssel je Objekt samt
Parallel-Tabelle für die Lauf-Erkennung der `GeometryPass`. Der Engpass von `record` war der Schatten-Pass, nicht die Lauf-Erkennung.

**Skalierung** (Referenzwelt aus Würfeln und Kugeln, gleiche Dichte, CPU-Frame p50 in ms, alt -> Endstand, Median aus 2 Läufen; D3D11 / D3D12 / Vulkan):

| Entities | alt (D3D12) | Endstand D3D11 / D3D12 / Vulkan | FPS (CPU) Endstand D3D12 |
|---|---|---|---|
| 10k | 2,5 | 1,6 / 1,4 / 1,7 | 740 |
| 25k | 5,4 | 2,6 / 2,7 / 3,1 | 376 |
| 50k | 9,5 | 4,9 / 4,9 / 5,4 | 203 |
| 100k | 18,3 | 8,8 / 9,0 / 9,6 | 111 |
| 200k | 35,2 | 17,0 / 17,2 / 18,5 | 58 |
| 400k | 67,8 | 33,1 / 31,9 / 35,4 | 31 |

Linear: etwa 0,085 ms je 1000 Entities (vorher 0,17). Grenzen des Renderer-CPU-Teils: 120 FPS etwa 95k (vorher 47k), 60 FPS etwa 195k (vorher 95k),
30 FPS etwa 400k (vorher 195k). Obergrenzen: zwei Meshes, keine Materialien, Physik, Skripte oder Animation, Editor-Frame (rund 1 ms UI); ab etwa 300k
begrenzt die GPU (14 ms bei 400k). Mit der halben Frame-Zeit als Reserve für das Spiel: etwa 95k bei 60 FPS, etwa 45k bei 120 FPS.

## Handoff

- **Topic / step:** Thema 238 (direkter Auftrag des Menschen), Zweig `claude/perf-foliage-persistent-encode`.
- **Done:** §2.
- **Verified:** Pixel-A/B Metal und GL je 42/42; `test_foliage_cluster` 26/26 (3 neu), `test_culling` grün; Messung §3.
- **Not verified:** §5.
- **Decisions taken:** paralleles Vorbereiten statt paralleles Kodieren (Profil §1); kein neues Instanz-Layout in Shadern, alle Backends behalten `{mvp, model}` bzw. Modellmatrizen.
- **Next steps:** 1. §4 auf NN-WS03. 2. Optional: `mvp` in Metal per Compute aus einer dauerhaften Arena (spart die restlichen ca. 1,5 ms `ShadowInstanceMvp` in der Foliage-Szene), braucht Compute vor dem Pass-Encoder.

### Handoff Windows (26.10.2026)

- **Done / verified:** §6 und §6b: Spans, parallele Schatten-Ansichten, Zeit pro Pass, die CPU-Hotspots des Frames. Komplette `he_tests`-Suite grün (5895 Fälle),
  Pixel-A/B der 28 Zeugen je Backend (25 md5-gleich; `dn_0/25/75` rauschen alt gegen alt um bis zu 46/255), Perf-A/B und Skalierung (§6/§6b).
- **Not verified:** Metal und macOS (Build, 42 Zeugen, Perf; siehe `metal-macos-handover-2026-10-26.md`), Foliage 300k sauber (die GPU schwankte), reale Szenen mit vielen
  Meshes, Materialien, Skinned-Meshes und Gameplay-Systemen (die Referenzwelt hat zwei Meshes), Hebel 1b, Vulkans SSR-Vorpass gegen Kopie.
- **Next steps:** 1. Mac: bauen, `he_tests`, `frame_pixel_ab.sh all`, Perf-A/B, Metal auf `FrameCull` umstellen (Handover-Doc). 2. Eine reale Szene (Catania) messen, bevor
  weiter optimiert wird: `extract` (2,3 ms bei 50k) bleibt der größte Posten; der Plan in `extract-incremental-plan-2026-10-26.md` (Variante B) greift erst, wenn er nach dieser Runde
  noch über 1 ms liegt. 3. `Shadow` (0,5 ms): Command-Listen je Kaskade parallel aufzeichnen (D3D12/Vulkan).
- **Traps:** nie bei laufendem Spiel messen (die GPU-Streuung macht Foliage-Zahlen wertlos); `--detailed` blockiert D3D11 auf die GPU (`GpuTimerReap`, 2 bis 7 ms CPU),
  für CPU-Vergleiche ohne `--detailed` messen; per-Pass-Zeitstempel (`HE_GPU_PASS_TIMING=0` schaltet sie ab) lassen die GPU je Grenze leerlaufen.
