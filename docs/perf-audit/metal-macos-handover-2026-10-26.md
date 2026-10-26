# Übergabe an Metal / macOS: was auf dem Mac noch zu tun und zu prüfen ist (26.10.2026)

Von Windows (NN-WS03, RTX 4070) aus gearbeitet. Metal und die Mac-Läufe konnten dort nicht gebaut, gemessen oder bildgeprüft werden.
Zweig `claude/foliage-spans-windows` (PR 199) enthält PR 198 (Foliage-Spans, Metal + GL) und PR 187 (`FrameCull`, Frame-Hebel);
die Reihenfolge der Merges ändert an der Arbeit nichts, PR 199 schrumpft entsprechend.

## 1. Gemeinsamer Code, den Metal ab jetzt mitbenutzt (hier ist auf dem Mac zu prüfen)

| Was | Datei | Warum es Metal betrifft | Prüfen auf dem Mac |
|---|---|---|---|
| `extractMeshes` läuft in Blöcken auf den Workern (Filter, Zählen, direkt in `out.objects` bauen, Mehrabschnitts-Meshes seriell danach) | `HE_Rendering/src/RenderExtractor.cpp` | der Extractor ist für alle Backends derselbe | `frame_pixel_ab.sh all` (42 Zeugen) md5-gleich; `test_culling` (neuer 6000-Entity-Test) |
| `RenderSorter::sortByMesh` ist eine Zählsortierung | `RenderSorter.cpp` | Metals Schatten-Ansichten sortieren darüber | gleiche Reihenfolge wie vorher (Test pinnt sie gegen den Vergleichssort); Zeugen |
| `RenderSorter::instanceSpansEnabled()` / `parallelShadowViewsEnabled()` | `RenderSorter.h/.cpp`, `MetalRenderer.mm` (3 Aufrufstellen, die lokale Funktion `instanceSpansEnabled` ist weg) | Metal las die Schalter selbst | **Metal muss neu gebaut werden**; die CI (macOS) hat den ersten Stand übersetzt, den letzten bitte prüfen |
| `FrameCull::refine` fragt jedes Mesh einmal und rechnet die Bounds wide | `FrameCull.h` | Metal nutzt `FrameCull` noch nicht (siehe 2) | erst nach der Umstellung relevant |
| `RenderGraph::record` ist ein CPU-Scope | `RenderGraph.cpp` | neuer Scope auch in Metal-Captures | nur Auswertung |
| Transform-Scan wide, `resolveWorldMaterialScalars` wide, Szenen-Box des Schatten-Fits wide | `TransformHierarchy.cpp`, `MaterialScalars.cpp`, `RenderExtractor.cpp` | alle Backends | `he_tests` komplett; Zeugen |
| `instanceWorldCacheNextFrame()` ruft jetzt auch GL, D3D, Vulkan | Backends | Metal rief es schon; kein Eingriff | nichts |

## 2. Was Metal noch selbst hält und durch den gemeinsamen Code ersetzen kann (Doppelungen)

Stellen in `MetalRenderer.mm` (Zeilennummern vom 26.10., vor der Umstellung greppen):

1. **`FrameCull` statt eigener Kopien.** `m_refinedWalk/m_refinedObjects/m_refinedCount`, `m_refinedBounds`, `m_refinedSceneBox`
   (`MetalRenderer.h:703-712`), `RefineObjectBounds()` (`.mm:8037`, sechs Aufrufer), `CullObjects` (`:8073`), `FrameSceneBox` (`:8082`,
   Nutzung `:12101`), `CameraCullCache` und `CullCameraObjects[Fresh]` (`.h:596-610`, `.mm:18266-18289`). Ersatz: ein `FrameCull m_frameCull`;
   `RefineObjectBounds()` wird `m_frameCull.refine(m_renderWorld, walk, localBoundsOf)` (der Lambda muss nur von `meshAssetId` abhängen);
   `CullObjects` = `m_frameCull.cull(...)`; `FrameSceneBox` = `m_frameCull.sceneBox(...)`. Occlusion (`variant` von `cullCamera`) und ihr eigener
   Cache bleiben in Metal (Hook oder `cull` + Sortieren dort), siehe PR 187 Beschreibung.
2. **`ShadowViewCpu` + `FrameCull::prepareShadowViews`** für Schritt (1) von `EncodeShadowMap` (alle Ansichten gleichzeitig cullen, sortieren, in
   Läufe schneiden, `.mm:8137-8170`). Metals `ShadowView` (`.h:660`) hat Felder darüber hinaus (Ring-Abschnitte, `mvp`); `ShadowViewCpu` als Basis
   oder Member nehmen, Schritte 2 bis 4 bleiben. Metals Kamera-Cull neben den Kaskaden (`warmCamera`) ist nicht Teil davon.
3. **`InstanceRun` / `RenderSorter::fillInstancePairs`** gegen Metals `UploadMvpModel` (`.mm:7191-7206`, Aufrufer `:14943, :14976, :16764, :16789`) und die
   Span-Schleifen im Szenen- und Schatten-Pfad prüfen: das gemeinsame `fillInstancePairs(dst, run, fill)` schreibt `{mvp, model}`-Paare span-weise und ab
   4096 Instanzen breit. Passt nur, wenn der Upload-Ring keine nicht-kombinierten Schreibzugriffe verträgt (write-combined) oder die Matrizen ohnehin
   vorher gebildet werden; sonst weglassen (Hebel 1b des Audits, nicht entschieden).
4. Der Quelltext-Wächter in `tests/test_foliage_cluster.cpp` zählt `RefineObjectBounds();`-Aufrufe in `MetalRenderer.mm`; nach der Umstellung auf den
   `FrameCull`-Aufruf anpassen (Tabelle mit den Refine-Stellen).

## 3. Prüfen und messen auf dem Mac

1. Bauen (Editor, `he_tests`), `he_tests` komplett (Windows: 5894 Fälle grün).
2. `frame_pixel_ab.sh all`: 42 Zeugen md5-gleich zu den Ständen `4dade2a2` / `97f84ab0` (dort sind die Referenz-md5 abgelegt). Auf Windows
   rauschen nur drei Nacht-Zeugen (`dn_0/25/75`, Sterne), alt gegen alt um bis zu 46/255; auf Metal ist das bisher nicht vorgekommen.
3. Perf-A/B auf den P-Kernen (`he_perf_capture.py`, 50k-Szene, Foliage 300k, Catania), je alter gegen neuer Stand, `HE_HIDDEN_WINDOW=1
   HE_BACKGROUND_FPS=0`, Capture ohne `--detailed` für die CPU-Zeit. Schalter: `HE_INSTANCE_SPANS=0` und `HE_MTL_PARALLEL_SHADOW=0`.
   Verglichen: p50 von `Render`, `RenderExtractor::extract` (mit `Extract::meshes`, `Extract::transforms`), `Metal::RefineBounds`, `Metal::ShadowPrepare`.
4. Erwartung nach den Windows-Messungen (50k Objekte, CPU-Frame, Median aus 5 verschachtelten Läufen, Richtwerte, ein Rechner mit
   Hintergrundlast): **D3D11 12,6 auf 7,2 ms, D3D12 12,9 auf 7,6 ms, Vulkan 12,0 auf 6,4 ms**. Davon der Extract (`extract` 5,5 auf etwa 2,3 ms,
   `Extract::meshes` 4,6 auf 1,5 ms) ist der gemeinsame Teil, der auf dem Mac genauso greifen muss; die Backend-Teile (Spans, Schatten-Ansichten
   `Shadow` 1,1 auf 0,5 ms) hat Metal schon (PR 198).
5. Auf dem Mac Zeit pro Pass gibt es schon (`GpuPassAccumulator`, detaillierter Modus); `GpuPassTimeline` ist für die Backends ohne eigenes Verfahren
   (D3D11/D3D12/Vulkan) und für Metal nicht nötig.

## 4. Weitere Einsparungen im Frame, die Windows gefunden, aber nicht gemacht hat (gelten für alle Backends)

Gemessen mit den neuen Scopes (50k Objekte, D3D12, CPU p50 nach den Änderungen):
`RenderExtractor::extract` 2,3 ms (davon `Extract::meshes` 1,5), `RenderGraph::record` 1,0, `OnRender` (Editor-UI) 1,0 davon `OutlinerPanel::render` 0,5,
`Shadow` 0,5, `RefineBounds` 0,45, `SubmitPresent` 0,3.

- **`GeometryPass::execute` (`RenderGraph::record`, 1,0 ms):** pro Objekt liest die Lauf-Erkennung `meshAssetId`, `materialAssetId`, `paramOverride`,
  `instanceTint`, `receivesShadow`, `weightmapTextureId` und `sections` (vier bis fünf Cache-Linien je 280-Byte-`RenderObject`). Ein vorberechneter
  Schlüssel je Objekt (aus dem Extractor, im parallelen Teil gebaut) und der Vergleich nur über den Schlüssel würde das auf eine Linie bringen.
  Die reine Umordnung der Felder (hot zuerst) hat nichts gebracht, weil sie `record` nicht begünstigte; mit den Vergleichsfeldern vorn wäre sie neu zu messen.
- **`RefineBounds` (0,45 ms):** entfiele für Objekte, deren Extractor-Bounds schon die echten sind; verlangt eine Garantie, dass Extractor- und GPU-Mesh-Bounds
  übereinstimmen (nicht gegeben bei Primitiven und Billboards).
- **Extract überspringen, wenn nichts geändert ist:** Plan in `extract-incremental-plan-2026-10-26.md` (Variante B zuerst).
- **Editor:** `OutlinerPanel::render` 0,5 ms bei 50k Entities.
- **D3D11-Capture:** `GpuTimerReap` (2 bis 4 ms) ist die erzwungene GPU-Synchronisation des `--detailed`-Captures, keine echte Arbeit.

## Handoff

- **Topic / step:** Thema 238 (Frame-Hebel), Metal/macOS-Teil.
- **Branch / commit:** `claude/foliage-spans-windows` (PR 199) @ der letzte Push; PR 198 und PR 187 sind die Basis.
- **Done (Windows):** Spans, parallele Schatten-Ansichten, Zeit pro Pass (D3D11/D3D12/Vulkan), paralleler Extract, Zählsortierung, wide `RefineBounds`,
  Transform-Scan, Material-Skalare und Szenen-Box wide; alles mit Tests (5894 Fälle) und Pixel-A/B (25 von 28, drei rauschende Zeugen).
- **Not verified:** alles auf Metal: Build, `he_tests`, 42 Zeugen, Perf; `MetalRenderer.mm` wurde nur an drei Stellen angefasst (die Schalter), die CI übersetzt es.
- **Decisions taken:** gemeinsamer Code statt Backend-Kopien; `HE_GPU_PASS_TIMING=0` schaltet die Pass-Marken ab (sie lassen die GPU je Grenze leerlaufen).
- **Decisions suggested, not taken:** Abschnitt 2 (Metal auf `FrameCull`, `prepareShadowViews`, `InstanceRun`), Abschnitt 4.
- **Next steps:** 1. Mac-Build und `he_tests` und 42 Zeugen gegen den Stand von PR 199. 2. Perf-A/B (Abschnitt 3). 3. Umstellung nach Abschnitt 2, danach der Wächter-Test. 4. Abschnitt 4.
- **Traps:** CRLF/LF in den Quellen gemischt (Git normalisiert); Windows-Tests kompilieren Metal nicht; Perf nie bei laufendem Spiel messen (Streuung 2 bis 3-fach);
  Mac-Läufe mit `HE_HIDDEN_WINDOW=1`, sonst stiehlt der Editor den Fokus.
