# Frame- und Parallelisierungs-Audit (Metal, M5), 26.10.2026

Frage: Was am Frame ist langsam, warum, und was lässt sich besser parallelisieren? Grundlage sind Messungen
im echten Editor-Loop (`scripts/he_perf_capture.py`) und ein Time-Profiler-Mitschnitt je Szene
(`xcrun xctrace`, `scripts/perf/xctrace_timeprofile.py`). Gebaut wurde nichts. Was schon geklärt ist,
übernimmt dieses Dokument und misst es nicht neu: `docs/render-extractor-shadow-pass-plan.md` (ein Walk pro Frame,
Verfeinern einmal je Walk, `propagate` nur für geänderte Teilbäume), `baseline-2026-09-27.md` (GPU-Pässe der kleinen
Szene) und `auto-landscape-material-2-audit-2026-10-21.md` (Foliage-Extract, BCn).

Kurzfassung: **Fast die ganze CPU-Arbeit eines Frames läuft seriell auf dem Hauptthread.** Die zehn Worker
des JobSystems sind im 50k-Fall zu 95 % untätig und im Foliage-Fall zu 99 %. Die größten Posten sind:
- Foliage-Instanzen werden in jedem Pass neu ausgepackt und kopiert, fünfmal pro Frame.
- Der serielle Sammelteil des Extracts.
- Kamera-Cull und Sortierung laufen doppelt.

Die GPU begrenzt nur die kleine Catania-Startszene, und dort vor allem der Scene-Pass mit den Wolken.

## 1. Messbedingungen

| Punkt | Wert |
|---|---|
| Gerät | MacBook Air `Mac17,4`, M5 (4 P + 6 E), 24 GB, Netzteil, Akku 90 % |
| **Stromsparmodus** | **AN** (`pmset lowpowermode 1`, ohne sudo nicht umschaltbar): der Time Profiler zeigt **0 P-Kern-Proben**, alle CPU-Zahlen sind E-Kern-Zahlen. Mit P-Kernen ist alles schneller, die Verhältnisse bleiben |
| **Bildschirm** | **gesperrt** (`CGSSessionScreenIsLocked=Yes`), `caffeinate -u` je Lauf. Folge: GPU-Zeiten ganzer Frames sind aufgebläht (siehe baseline §1), FPS nur innerhalb dieser Sitzung vergleichbar |
| Last | `uptime` 1,7 bis 3 vor den Catania- und 50k-Läufen. Während der ersten Foliage-Läufe (f1, f2) lief fremde Last (Load bis 48, Testlauf einer anderen Sitzung); f3 und r3 sind Wiederholungen bei abklingender Last, Zahlen in §2 sind aus f3/r3. Der Hauptthread war im f2-Mitschnitt zu 97 % auf der CPU, die Anteile im Profil gelten also |
| Fremde GPU-Last | `Device Utilization` 33 % vor dem ersten Lauf, kein fremder HorizonEditor lief |
| Build | Release, Ninja, `HE_PROFILING=ON`, `cmake-build-release` @ `4dade2a2`; Deploy `out/deploy/Editor` per `cmp` byte-gleich mit dem Build (Editor + Rendering/Core/Scene-dylibs) |
| Programm | Editor, Edit-Modus, Metal, Fenster 2840×1528, vsync aus (`he_perf_capture` erzwingt es), Kamera `0,25,90,0,-0.25` |
| Projekt | Kopie von `~/HorizonEngineProjects/Catania` ohne `Export/` in den Scratch (`Test` gibt es nicht mehr); das Projekt des Menschen ist unberührt |
| Szenen | **Catania-Startszene** (Terrain, Sky mit Wolken 0,31, 99 Draws, 469k Dreiecke); **50k** = `gen_reference_world.py --count 50000 --lights 64` auf der Catania-Szene; **Foliage 300k** = `--count 1000 --foliage 300000` |
| Läufe | 240 bis 300 Frames Warmup, 400 bis 1200 Frames Aufnahme; `--no-counters` für FPS, `--detailed` für exklusive GPU-Zeit je Pass; Time Profiler 10 s mitten in der Aufnahme |

Rohdaten (klein): `docs/perf-audit/raw-frame-parallelism-2026-10-26/` (`*.summary.json`, `scopes.txt`,
`xctrace-*.txt`, `conditions.txt`). Die Profil-Dumps und `.trace`-Dateien sind nicht im Repo.

## 2. Messwerte

### 2.1 Frame (p50, ms)

| Szene | FPS | CPU-Frame | GPU-Frame | Begrenzt durch |
|---|---|---|---|---|
| Catania-Start (c1) | 59,4 | 16,7, davon 14,2 Warten auf `NextDrawable` | 32,9 (ganzer Frame, gesperrt, aufgebläht) | GPU (CPU-Arbeit ~2,5 ms) |
| Catania detailed (c2) | (Messmodus) | | **14,8** exklusiv, Summe der Pässe | |
| 50k (r1 / r2 / r3) | 35,8 / 32,8 / 34,4 | 27,7 / 29,6 / 29,2 | 19,1 / 19,1 / 19,0 | **CPU** |
| Foliage 300k (f3; f1 unter Fremdlast) | 23,6 (19,8) | 42,8 (49,2) | 25,4 (22,3) | **CPU** |

### 2.2 CPU-Scopes je Frame (p50 ms)

| Scope | Catania | 50k (r3) | Foliage 300k (f3) |
|---|---|---|---|
| `Render` (Hauptthread) | 1,73 | 25,3 | 41,5 |
| `Metal::ExtractFrame` (Walk + Verfeinern) | 0,41 | 11,8 (Walk 10,1, Verfeinern 1,7) | 0,85 |
| `Metal::EncodeShadowMap` | 0,24 | 3,0 | **15,1** |
| `Metal::EncodeSSAO` | 0,20 | 4,0 | **11,5** |
| `Metal::EncodeScene` | 0,35 | 3,9 | **12,3** |
| `Metal::EncodeCloudShadow` | 0,05 | **1,9** | 0,16 |
| `FrustumCuller::cull` (5×, steckt in den Pässen) | 0,05 | 2,9 | 0,57 |
| `OnRender` (Editor-UI) | 0,76 | 3,6 (davon `OutlinerPanel` 1,1) | 1,2 |
| `SceneSystemsTick` | 0,13 | 0,13 | 0,16 |

### 2.3 Wer arbeitet (Time Profiler, 10 s, Proben à 1 ms auf der CPU)

| Szene | Hauptthread | 10 JobSystem-Worker zusammen | je Worker | Worker tun |
|---|---|---|---|---|
| 50k (r2, ~330 Frames) | 10 451 (≈ 100 % eines Kerns) | 5 208 (≈ 0,5 Kern) | 455 bis 504 (≈ 5 %) | 68 % Frustum-Cull, 19 % `extractMeshes` (paralleler Teil), 7 % Metal-Submit |
| Foliage 300k (f2, ~210 Frames) | 9 727 (97 %) | 392 (≈ 0,04 Kern) | | fast nichts |

Hauptthread 50k, inklusive Zeit je Frame (Proben / 330):
- **`RenderExtractor::extract` 10,5 ms**, davon:
  - seriell: Selbstzeit 2,6, `resolveEntitySlots` 1,5, `vector<MeshSection>`-Kopien 1,2, `operator new` 1,2;
  - `RenderObject`-`resize` 0,9, `RenderWorld::clear` 0,7, `propagateTransforms` 0,7.
- **`CullCameraObjects` 4,8 ms** (zwei Aufrufe: SSAO und Scene).
- **`RenderSorter::sort` 4,4 ms** (fünf Aufrufe).
- `ViewportActions::anyHidden` 2,1 ms.
- `EncodeCloudShadow` Selbstzeit 2,0 ms.
- `RefineObjectBounds` 1,9 ms.
- `OutlinerPanel` 1,3 ms.
- `Frustum::intersects` auf allen Kernen zusammen 4 218 Proben ≈ **12,8 ms CPU je Frame** für 5 × 50k Tests.

Hauptthread Foliage, je Frame (Proben / 210):
- `_platform_memmove` 11,8 ms;
- `RenderWorld::appendInstances` (Selbstzeit) 8,7 ms;
- `glm::mat4 * mat4` 7,2 ms;
- Selbstzeit des Schatten-Lambdas 5,4 ms;
- `DrawCall`-Kopien 1,4 ms.

Zusammen sind das rund **30 ms von 46 ms**. Alle diese Posten liegen unter `RenderSorter::batchDepthRuns` (Schatten, SSAO) und `GeometryPass::execute` (Szene).

### 2.4 GPU je Pass, Catania (detailed, exklusiv, ms)

| Pass | p50 | min | ohne Wolken (c3) p50 / min |
|---|---|---|---|
| Scene (Opak, Himmel, Wolken, Transparent) | 11,66 | 9,28 | 10,89 / **4,94** |
| Shadow | 0,97 | 0,51 | 1,09 / 0,45 |
| SSAO | 0,83 | 0,60 | 1,13 / 0,59 |
| Bloom | 0,48 | 0,31 | 0,64 / 0,31 |
| Present | 0,47 | 0,34 | 0,74 / 0,34 |
| Tonemap | 0,21 | 0,16 | 0,27 / 0,16 |

Unter gesperrtem Bildschirm schwankt p50 mit dem GPU-Takt (c3 p50 nur 0,8 ms unter c2, min 4,3 ms). Die Minima sind
hier die belastbarere Zahl. **Die Wolken kosten im Scene-Pass grob 4 ms von 9 bis 12 ms.** `lowResClouds` ist in Catania aus.

## 3. Hebel, nach erwartetem Gewinn

Jeder Hebel nennt Art (CPU-seriell, Sync/Stall, GPU, Parallelisierung), Beleg, Gewinn und Aufwand. Gewinne sind
aus den Proben geschätzt (E-Kerne), nicht durch einen Umbau gemessen.

**1. Foliage-Instanzen je Pass neu auspacken (CPU-seriell, Kopien).**

*Beleg:*
- `RenderWorld::appendInstances` (`RenderWorld.h:344`) rechnet für jede Instanz `parent * matrix` aus und hängt sie an einen `std::vector<glm::mat4>` an.
- Aufgerufen wird es pro Pass aus `RenderSorter::batchDepthRuns` (`RenderSorter.cpp:100`, Schatten je Kaskade und SSAO-Tiefe) und `RenderPass.cpp:139/150` (Szene).
- `DrawCall chunk = dc` (`RenderPass.cpp:154`) kopiert die Matrixliste noch einmal.
- Dazu kommt der Upload in den Ring.
- Bei 300k Instanzen und 5 Pässen sind das ~96 MB `memmove` je Frame, gemessen ~30 ms von 42 bis 46.

Die Matrizen selbst sind statisch (`InstanceBlock::matrices`, `shared_ptr`).

*Hebel:*
- Die Welt-Matrizen einmal pro Frame oder besser beim Streuen in einen persistenten GPU-Puffer legen.
- Die Pässe zeichnen nur noch Bereiche daraus (offset/count je Cluster).
- Danach Culling je Instanz auf der GPU statt Auspacken auf der CPU.
- Zwischenschritt ohne Pufferumbau: das Auspacken je Frame einmal und per `parallel_for`.

*Gewinn:* ~25 ms bei 300k (CPU-Frame grob 43 auf 18).

*Aufwand und Risiko:* mittel bis hoch. `RenderPass.cpp` und `batchDepthRuns` teilen alle Backends; der Metal-Zeichenweg braucht einen Instanzpuffer-Offset.

**2. Hauptthread trägt alles, Worker stehen (Parallelisierung).**

*Beleg:* §2.3. Nach `ExtractFrame` ist der `RenderWorld` laut Vertrag nur noch lesbar (render-extractor-shadow-pass-plan §6). Trotzdem kodieren Shadow (3 Kaskaden), CloudShadow, SSAO und Scene nacheinander auf dem Hauptthread in **einen** Command-Buffer (`MetalRenderer.mm:16899-17290`). Bei 50k sind das 3,0 + 1,9 + 4,0 + 3,9 = 12,8 ms seriell, bei Foliage 39 ms.

*Hebel:*
- Die Pässe auf Worker verteilen: je Pass ein eigener `MTLCommandBuffer` mit vorher festgelegter `enqueue`-Reihenfolge, oder ein `MTLParallelRenderCommandEncoder` innerhalb eines Passes, etwa je Kaskade.
- **Voraussetzung:** die Pässe teilen sich heute veränderliche Members (`m_visible`, `m_sortedIndices`, `m_sorter`, `m_culler`, Instanz-Ringe). Die müssen je Pass eigene Scratch-Objekte werden.

*Gewinn:* bei 50k von ~13 auf ~4 ms (der längste Pass), also ~8 ms. Bei Foliage nach Hebel 1 deutlich weniger.

*Aufwand und Risiko:* hoch, Datenrennen. Erst die Hebel 3 bis 5, dann ist weniger zu verteilen.

Langfristig, nicht gemessen: Extract des Frames N+1 parallel zum Kodieren von N (Render-Thread). Das verlangt einen doppelt gepufferten `RenderWorld` und widerspricht dem heutigen Frame-Scope-Vertrag (Komponentenwerte dürfen sich im Scope nicht ändern).

**3. Serieller Sammelteil des Extracts (CPU-seriell, Allokationen).**

*Beleg:* `extractMeshes` (`RenderExtractor.cpp:377-450`) läuft pro Entity und pro Frame seriell:
- `getStaticMesh` (Hash-Lookup);
- **kopiert** `m->sections` (`:439`);
- ruft `resolveEntitySlots(std::vector<MeshSection> …)` mit Wert-Argumenten und `lod0SectionsOf(...)`, das einen weiteren Vektor liefert;
- legt den Param-Block mit `std::vector<float>(64)` an;
- kopiert mit `items.push_back(d)` (`:449`) statt zu verschieben.

Der parallele Teil kopiert `paramOverride` und `sections` ein weiteres Mal (`:471-472`). Gemessen: Walk 10,1 ms bei 50k, davon ~7 ms seriell auf dem Hauptthread. Die Worker sehen nur 19 % ihrer 0,5 Kerne in `extractMeshes`.

*Hebel:*
- Sofort: `std::move` statt Kopie (`:449`, `:471-472`) und die Abschnitte nur bei Mehrabschnitts-Meshes oder Slot-Overrides auflösen.
- Mittelfristig: die Auflösung je eindeutigem Schlüssel (Mesh, Material, Overrides) zwischenspeichern; 50k Entities teilen sich wenige Meshes.
- Dann den Sammelteil selbst per `parallel_for`: die ContentManager-Lookups seriell vorab je eindeutiger Mesh-Id.

*Gewinn:* 4 bis 6 ms bei 50k. Der große Schritt ist der im Plan-Dokument offene persistente Zustand über Frames (Walk nur für Geändertes).

*Aufwand und Risiko:* niedrig für die Moves, mittel für den Cache. `isTerrainGenerated`/`structureEpoch` müssen den Cache ungültig machen.

**4. Kamera-Cull und Sortierung doppelt, volle Sortierung je Kaskade (CPU-seriell).**

*Beleg:*
- `CullCameraObjects()` (`MetalRenderer.mm:18095`) rufen SSAO (`:12444`) und Scene (`:14155`) mit derselben Kamera im selben Frame auf, mit vollem `m_culler.cull` und `m_sorter.sort` (`:18126`). Nur der Occlusion-Teil hat schon einen Frame-Cache (`m_occlusionCacheStamp`). Gemessen 4,8 ms für zwei Aufrufe.
- Der Schattenpass sortiert je Kaskade alle sichtbaren Objekte nach Mesh **und Entfernung** (`:8006-8007`). Für reine Tiefe zählt die Bündelung nach Mesh, die Entfernung bringt dort wenig.
- `RenderSorter::sort` gesamt 4,4 ms bei 5 Aufrufen.

*Hebel:*
- Ergebnis von Cull und Sort je Frame und View-Projektion merken: SSAO und Scene teilen es.
- Schatten: nur nach Mesh gruppieren (Radix oder ein stabiler Bucket nach Mesh-Id).
- Die drei Kaskaden-Culls in einem Lauf über die Objekte (Plan-Dokument §8).

*Gewinn:* ~2,4 ms (zweiter Cull+Sort) plus ~1,5 ms (Schattensortierung) bei 50k.

*Aufwand und Risiko:* niedrig. Die Stempel-Logik existiert schon (`m_frameStamp`).

**5. Wolkenschatten läuft über alle Objekte (CPU-seriell).**

*Beleg:* `EncodeCloudShadow` baut jedes Frame eine Szenen-AABB aus allen Objekten (`MetalRenderer.mm:11907`), nur um eine grobe Bodenhöhe zu finden. Das kostet 1,6 bis 1,9 ms bei 50k (Selbstzeit 2,0 ms im Profil).

*Hebel:*
- Die Szenen-Box gibt es schon aus dem Walk (`RenderExtractor::m_frameShadowBox`, `RenderExtractor.h:402`).
- Oder die Box nur bei `fullExtractCount()`-Wechsel neu bilden.

*Gewinn:* ~1,8 ms bei 50k.

*Aufwand und Risiko:* sehr niedrig, die Hysterese bleibt gleich.

**6. Editor-UI über alle Entities je Frame (CPU-seriell, nur Editor).**

*Beleg:* Die macOS-Menüleiste setzt jedes Frame den Titel „Lock/Unlock“ über `ViewportPanel::entityActionState(ctx)` (`EditorUI.cpp:3016`). Das berechnet nebenbei `anyHidden` (`ViewportPanel.cpp:767`), und `anyHidden` läuft ohne Abbruch über alle Entities in der Szene (`ViewportActions.cpp:133`, die Lambda prüft `!any`, die Schleife läuft weiter). Gemessen 2,1 ms bei 50k. `OutlinerPanel::render` 1,1 ms.

*Hebel:*
- Für die Menüleiste nur `primaryLocked` rechnen.
- `anyHidden` bei Treffer abbrechen lassen oder nur beim Öffnen des Menüs fragen.

*Gewinn:* ~2 ms bei 50k, im Spiel 0.

*Aufwand und Risiko:* sehr niedrig.

**7. Frustum-Test liest 280-Byte-Objekte (Speicherlayout, Parallelisierung schon da).**

*Beleg:*
- `FrustumCuller::cull` ist parallel (`FrustumCuller.cpp:61`).
- `Frustum::intersects` liest aber `worldBounds` aus `RenderObject` mit ~280 Byte Abstand, also eine neue Cache-Zeile je Test.
- 5 × 50k Tests kosten 12,8 ms CPU über alle Kerne (≈ 51 ns je Test), Wanduhr 2,9 ms.

*Hebel:*
- Ein kompaktes Bounds-Array (SoA, nach dem Walk gefüllt).
- Die Kaskaden-Culls in einem Lauf (siehe 4).

*Gewinn:* ~2 ms Wanduhr bei 50k, und die Worker werden für Hebel 2 frei.

*Aufwand und Risiko:* mittel. Jeder Leser von `worldBounds` bleibt; das Array ist eine Zusatzkopie mit Serial-Stempel wie beim Verfeinern.

**8. Wolken im Scene-Pass (GPU, nur wenn GPU-begrenzt).**

*Beleg:* §2.4. In der Catania-Startszene ist die GPU die Grenze (14,8 ms exklusiv, die CPU wartet 14 ms auf `NextDrawable`). Die Wolken sind grob 4 ms des Scene-Passes.

*Hebel:*
- `lowResClouds` für Catania prüfen (Bildvergleich, Zeugen-Fallen siehe `cloud-shot-witness-traps`).
- Die offenen Punkte im Himmels-Plan (`sky-weather-performance-plan.md`, 2a/2b).

*Gewinn:* bis ~3 ms GPU in der Startszene.

*Aufwand und Risiko:* gering für den Schalter, die Bildqualität entscheidet der Mensch.

Reihenfolge-Vorschlag: zuerst die billigen 5, 6, 4 und die Moves aus 3 (zusammen ~8 ms bei 50k, wenig Risiko), dann 1 (der
größte Einzelposten, sobald Foliage im Spiel ist), dann 7 und der Cache aus 3, zuletzt 2 auf dem dann kleineren Rest.

## 4. Nicht geprüft

- **Andere Backends:** D3D11/D3D12/Vulkan/GL nicht gemessen. Die Hebel 1, 3 und 7 liegen in gemeinsamem Code (`RenderPass.cpp`, `RenderSorter.cpp`, `RenderExtractor.cpp`, `FrustumCuller.cpp`) und gelten dort genauso. Die Hebel 2, 4 und 5 sind die Metal-Stellen; Vulkan hat dieselben Muster (`VulkanRenderer.cpp:4084/7324/8639`), D3D sortiert Schatten ebenfalls je Pass (`D3D11Renderer.cpp:7408`).
- **P-Kerne:** Stromsparmodus an, alles lief auf E-Kernen. **Entsperrter Bildschirm:** nicht verfügbar, darum keine verlässlichen GPU-Frame-Zeiten außer `--detailed`.
- **Spiel (`HorizonGame`) und Play-Modus:** nicht gemessen. Ohne Editor entfallen Hebel 6 und `OnRender`; Physik, Skripte und Animation kamen in keinem Lauf vor.
- **Gewinne:** aus Profilanteilen geschätzt, kein Hebel wurde umgesetzt oder A/B gemessen.
- **50k-Szene:** liegt auf 2000 m verteilt, 16 252 von 50 324 sichtbar, 120 Draws (Instanzbündel). Eine Welt mit vielen verschiedenen Meshes und Materialien würde Hebel 3 (Abschnitte, Param-Blöcke) stärker zeigen.

## 5. Umsetzung (Zweig `claude/frame-perf-levers`, 26.10.2026)

Umgesetzt wurden die Hebel 1 (Instanzen), 3 (Kopien im Sammelteil), 4 (Kamera-Cull geteilt, Schatten nur nach Mesh),
5 (Wolkenschatten-Box), 6 (Menüleiste) und 7 (kompaktes Bounds-Array). Die Hebel 2, 8 und der Cache aus 3 sind offen.

| Hebel | Was | Wo | Backends |
|---|---|---|---|
| 1 | Instanzen erst anordnen, dann in Stücken ≤ 2048 auf den Workern füllen; gewöhnliche Objekte direkt kopiert; keine Zwischenliste beim Schneiden; Draws werden verschoben statt kopiert | `RenderWorld.h` (`addInstancePiece`, `writeInstancePieces`), `RenderSorter::batchDepthRuns`, `GeometryPass`, `CommandBuffer::recordDraw(DrawCall&&)` | alle |
| 1 | Matrizen pro Instanz direkt in den Upload-Ring, groß parallel | `MetalRenderer::UploadMvpModel`, Schatten- und SSAO-Vorpass | Metal |
| 3 | Ein-Abschnitt-Mesh ohne fremdes LOD0 ohne Tabellenkopien; Moves in den RenderObject | `RenderExtractor.cpp` `extractMeshes` | alle |
| 4 | Cull + Sortierung einmal je Frame, Walk und Kamera, SSAO und Scene teilen es | `MetalRenderer::CullCameraObjects`, `m_cameraCull` | Metal |
| 4 | Schatten-Kaskaden nur nach Mesh sortiert | `RenderSorter::sortByMesh` | Metal nutzt es |
| 5 | Szenen-Box aus der Verfeinern-Schleife | `MetalRenderer::FrameSceneBox` | Metal |
| 6 | Menüleiste fragt nur `primaryLocked`, `anyHidden` bricht beim ersten Treffer ab | `ViewportPanel`, `ViewportActions` | Editor (`anyHidden` überall, Menüleiste macOS) |
| 7 | Frustum-Test über kompaktes `std::vector<HE::AABB>` je Walk | `FrustumCuller::cull(bounds, …)`, `MetalRenderer::CullObjects` | Metal nutzt es |

A/B-Schalter: `HE_PARALLEL_UNFOLD=0` hält das Auspacken und die Matrizen pro Instanz auf dem Aufrufer.

**Bild:** alle 42 Zeugen von `frame_pixel_ab.sh all` md5-gleich zu `4dade2a2`, nach jedem Schritt
(Listen in `raw-frame-levers-2026-10-26/pixel_ab_md5_*.txt`).

**Tests:** gezielte Dateien grün, neu sind „GeometryPass keeps the instance order across its cuts…“
(`test_foliage_cluster.cpp`) und „FrustumCuller over a compact bounds array…“ (`test_culling.cpp`).
Volle Suite: 5795 von 5798 grün. Die drei roten haben mit dem Zweig nichts zu tun:
- `test_hpak.cpp:1188` zählt die 4 Assets in `~/Library/Application Support/HorizonEngine/EngineContentCache` mit (der Test isoliert `userDataDir` nicht).
- zwei `test_profiler`-Fälle, die allein grün sind.

**Messung.** Alt ist der zur Seite gelegte Deploy `4dade2a2` (selfcontain), neu der Zweig. Abwechselnd gemessen, gleiche Szenen und Kamera.
Zwei Bedingungen kamen in der Sitzung vor, die Zahlen sind nur innerhalb einer Bedingung vergleichbar:

*E-Kerne* (Stromsparmodus an, Bildschirm gesperrt, Runden 1 bis 3 und 5), CPU-Frame p50:
- 50k: 28,5 / 27,3 / 27,7 → 24,6 / 23,8 / 24,4 ms.
- Foliage 300k: 36,7 / 36,0 / 37,0 → 29,2 / 28,0 / 32,6 ms.
- Foliage-Pässe (Runde 5): Shadow 16,1 → 9,9, SSAO 12,2 → 3,3, Scene 12,5 → 3,1 ms. Der Hauptthread arbeitet 18 statt 36 ms je Frame und wartet danach 13 ms auf das Drawable: GPU-begrenzt.
- `ExtractFrame` wurde dort trotz entfallener Kopien länger. Im Time Profiler sind die Ziele von Hebel 3 weg (`resolveEntitySlots`, `MeshSection`-Kopien, `operator new`: ~30 % des Walks). Unveränderte Funktionen (`propagateTransforms`, `RenderWorld::clear`) brauchten aber ~30 % länger: das gemeinsame Strombudget im Stromsparmodus. Auf P-Kernen ist der Effekt weg (unten).

*P-Kerne* (Stromsparmodus aus, entsperrt, Akku, verstecktes Fenster), p50 je Frame in ms, je zwei Läufe:

| Scope | 50k alt | 50k neu | Foliage alt | Foliage neu |
|---|---|---|---|---|
| `Metal::ExtractFrame` | 3,15 / 3,19 | 3,09 / 3,18 | 0,18 / 0,17 | 0,24 / 0,25 |
| `Metal::EncodeShadowMap` | 0,70 / 0,81 | 0,68 | 3,63 / 3,58 | 2,63 |
| `Metal::EncodeSSAO` | 0,81 / 0,96 | 0,99 | 2,32 / 2,30 | 1,18 |
| `Metal::EncodeScene` | 0,87 / 1,01 | 0,30 | 2,72 / 2,82 | 1,09 |
| `FrustumCuller::cull` (je Frame) | 0,47 / 0,61 | 0,31 / 0,32 | | |
| `OnRender` | 1,02 / 1,03 | 0,56 / 0,58 | | |

Bei 50k wandert der Cull in den SSAO-Pass (er rechnet ihn, Scene liest ihn), die drei Pässe zusammen 2,10 → 1,96 ms.
Die verdeckten Fenster laufen dort GPU-begrenzt (Render enthält das Warten), deshalb ändert sich der CPU-Frame kaum.
**Nicht gemessen:** P-Kerne bei sichtbarem, entsperrtem Fenster im Spiel; andere Backends (siehe offene Punkte in den Hive-Themen 238 und 239).

## Handoff

- **Topic / step:** Umsetzung der Hebel (direkter Auftrag des Menschen), Zweig `claude/frame-perf-levers` ab `490ed019`, gepusht, kein PR.
- **Done:** Hebel 1, 3, 4, 5, 6, 7 (Metal; Hebel 1 und 3 im gemeinsamen Code), §5.
- **Verified:** Build `cmake-build-release` rc 0; 42 Pixel-Zeugen md5-gleich nach jedem Schritt; gezielte Tests grün, volle Suite bis auf drei fremde Fälle (§5); Messung §5.
- **Not verified:** Vulkan, D3D11, D3D12, OpenGL (gemeinsamer Code nur von Metal ausgeübt; kein Windows-Build auf diesem Zweig); Spiel (`HorizonGame`).
- **Decisions taken:** Gewinne nur als Scope-p50 je Bedingung berichtet; `HE_PARALLEL_UNFOLD` als A/B-Schalter.
- **Next steps:** 1. Gleiche Muster in den anderen Backends (Hive, Themen 238/239: Vulkan `VulkanRenderer.cpp:4083/7323/8638/12376`, D3D11 `:7001/7407`, D3D12 `:11273/11680`, OpenGL `:5171/11230/11335`). 2. Hebel 2 (Pässe parallel kodieren) als eigenes Thema. 3. Cache aus Hebel 3. 4. Hebel 8 (`lowResClouds` in Catania) entscheidet der Mensch.

## Handoff (Audit, vor der Umsetzung)

- **Topic / step:** Frame- und Parallelisierungs-Audit (direkter Auftrag des Menschen, kein Hive-Thema), ein Schritt
- **Branch / commit:** release/0.7.0, nur dieses Dokument und `raw-frame-parallelism-2026-10-26/`
- **Done:** Gemessene Antwort auf "was ist langsam, was lässt sich parallelisieren": drei Szenen (Catania-Start, 50k, Foliage 300k), CPU-Scopes, exklusive GPU-Pässe, Thread-Auslastung per Time Profiler, acht Hebel mit Datei:Zeile.
- **Files touched:** `docs/perf-audit/frame-parallelism-audit-2026-10-26.md`; Rohzusammenfassungen in `docs/perf-audit/raw-frame-parallelism-2026-10-26/`
- **Verified:** Build `cmake-build-release` @ `4dade2a2` rc 0, Deploy per `cmp` gleich; 8 Läufe `he_perf_capture.py` (rc 0, Zahlen in `scopes.txt`); 2 Time-Profiler-Mitschnitte (Thread-Summen in `xctrace-*.txt`); 50k zweimal wiederholt (CPU p50 27,7 / 29,6 / 29,2 ms).
- **Not verified:** siehe §4 (andere Backends, P-Kerne, entsperrter Bildschirm, Spiel, keine Umsetzung).
- **Decisions taken:** Catania-Kopie statt des verschwundenen `Test`-Projekts; GPU-Vergleich nur über `--detailed`-Minima, weil der Bildschirm gesperrt war.
- **Decisions suggested, not taken:** Reihenfolge in §3 (5, 6, 4, Moves aus 3, dann 1, 7, Cache aus 3, zuletzt 2).
- **Traps:** `pgrep -n HorizonEditor` traf beim ersten Versuch einen kurzlebigen Prozess, `pgrep -f deploy/Editor/HorizonEditor` nach 18 s Wartezeit funktioniert. `he_perf_capture --scene` überschreibt die Startszene der Projektkopie: nach jedem Lauf zurückkopieren. Fremde Testläufe (Load 48) verfälschen CPU-Zahlen: `uptime` je Lauf notieren.
- **Next steps:** 1. Hebel 5 (`EncodeCloudShadow` nutzt `m_frameShadowBox`) und 6 (`entityActionState` in der Menüleiste ohne `anyHidden`), je mit 50k-Vorher/Nachher. 2. Hebel 4: Frame-Cache für `CullCameraObjects`, Schattensortierung nur nach Mesh. 3. Moves in `extractMeshes` (`RenderExtractor.cpp:449/471/472`). 4. Hebel 1 als eigenes Thema (Instanzpuffer persistent, alle Backends). 5. Messung mit Stromsparmodus aus und entsperrtem Bildschirm, wenn der Mensch das einrichtet.
