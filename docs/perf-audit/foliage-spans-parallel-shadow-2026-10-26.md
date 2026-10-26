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

## Handoff

- **Topic / step:** Thema 238 (direkter Auftrag des Menschen), Zweig `claude/perf-foliage-persistent-encode`.
- **Done:** §2.
- **Verified:** Pixel-A/B Metal und GL je 42/42; `test_foliage_cluster` 26/26 (3 neu), `test_culling` grün; Messung §3.
- **Not verified:** §5.
- **Decisions taken:** paralleles Vorbereiten statt paralleles Kodieren (Profil §1); kein neues Instanz-Layout in Shadern, alle Backends behalten `{mvp, model}` bzw. Modellmatrizen.
- **Next steps:** 1. §4 auf NN-WS03. 2. Optional: `mvp` in Metal per Compute aus einer dauerhaften Arena (spart die restlichen ca. 1,5 ms `ShadowInstanceMvp` in der Foliage-Szene), braucht Compute vor dem Pass-Encoder.
