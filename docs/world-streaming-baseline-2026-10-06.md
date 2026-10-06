# Welt-Streaming: Bestandsaufnahme und Messbasis (Thema 153, Schritt 1)

Stand 06.10.2026, Zweig `claude/welt-streaming-job-planung-groessere-welten-mehr-entities`
(Basis `2b0ea05f` = main, Messhilfen in `e6ee064a` und `3e9dd8e3`). In diesem Schritt
wurde nichts umgebaut. Neu sind nur Messhilfen: zwei Log-Zeilen mit Ladezeiten und drei Skripte
(Abschnitt 7). Die Zahlen sind die Vorher-Werte, gegen die Schritt 5 misst.

**Schritt 3 (Streaming-Durchsatz, Lade-Latenz) ist in Abschnitt 8 nachgemessen:** Szene laden
200k 5,3 s → 2,7 s, Pak-Streaming von 4 000 kleinen Assets 1,0 s → 0,1 s.

**Schritt 4 (mehr Entities, größere Welten) steht in Abschnitt 9:** CPU pro Frame im Editor
10k 64 → 22 ms, 50k 321 → 118 ms; Jolt 65 536 statt 1 024 Bodies.

**Kurz:**
- Die Ladezeit ist nicht das Problem. 50 000 Entities sind in 1,3 s geladen, und die Ladezeit
  wächst linear.
- Das Problem sind die Kosten pro Frame. Im Editor kostet jedes Entity etwa 6,4 µs CPU pro Frame,
  die Kurve ist linear. Bei 30 FPS ist deshalb bei rund 5 000 Entities Schluss.
- Diese Kosten fallen an drei Stellen an:
  - Der Outliner zeichnet jede Zeile.
  - Der Metal-Renderer extrahiert die Welt viermal pro Frame, einmal je Pass.
  - Die Extraktion läuft jedes Mal über die ganze Hierarchie.
- Gestreamt wird heute nur der Asset-Inhalt, und nur im Spiel. Welt und Entities werden nirgends
  gestreamt.
- Ab etwa 32 km vom Ursprung ruckeln Objekte um mehr als 1 Pixel.

## 1. Messbedingungen

| Punkt | Wert |
|---|---|
| Gerät | MacBook Air M5 (4 P- und 6 E-Kerne), 24 GB, macOS 27.0 |
| Energie | **Stromsparmodus AN** (`pmset lowpowermode 1`, ohne sudo nicht umschaltbar). Der Time Profiler zählt **0 P-Kern-Proben**: alle CPU-Zahlen sind E-Kern-Zahlen |
| Bildschirm | **gesperrt** (`CGSSessionScreenIsLocked=Yes`). Unter Sperre sind FPS und NextDrawable nicht aussagekräftig (siehe Memory „Perf-Messung auf dem M5“). Verglichen werden deshalb nur die CPU-Scopes |
| Fremdlast | GPU vor den Läufen bei 57 % „Device Utilization“, ohne laufenden Editor (Fremdprozess). Load average 2,1–2,8. Parallel aktiv war die Hive-Biene `vulkan-csm-kaskade-1` (wartete auf CI) |
| Build | Release, Ninja, `HE_PROFILING=ON`, `HE_ENABLE_SHADERC=ON`, eigener Worktree-Build unter `out/build/release`. Der Deploy enthält nachweislich die Messhilfen (`strings`) |
| Programm | **Editor** (`HorizonEditor`) über `scripts/he_perf_capture.py`, Metal, Fenster 2840×1528 px, `--no-counters`, `--warmup 0` |
| Projekt | Kopie von `~/HorizonEngineProjects/Test` nach `/tmp/ws_proj/Test`. Die Szene wird per `--scene` über die Startszene gelegt |

**Was die Messung nicht abdeckt:**
- Die Spiel-Runtime (`HorizonGame`) mit ihrem asynchronen Asset-Streaming. Dafür bräuchte es ein
  exportiertes Spiel. Das gehört zu Schritt 3, die Messhilfe `SceneLoadTiming` greift dort genauso.
- Jolt-Physik. Der Editor baut Bodies erst im Play-Modus, deshalb laufen die Referenzwelten ohne
  Physik. Das Body-Limit unten stammt aus dem Code, nicht aus einer Messung.

## 2. Referenzwelt

Die Szenen erzeugt `scripts/perf/gen_reference_world.py`. Das Ergebnis ist deterministisch (Seed 1).
- **Inhalt:** N Mesh-Entities (eingebauter Würfel und Kugel, Material ohne Asset) auf einem
  gejitterten Raster über **8 × 8 km** um den Ursprung.
- **Hierarchie:** N/100 Gruppen-Entities, Tiefe 2.
- **Dazu:** 64 Punktlichter. Umgebung, Wetter und Terrain stammen aus
  `docs/perf-audit/scenes/landscape_noclouds.hescene`.
- **Kamera:** `--cam 0,25,90,0,-0.25`.

| N (Meshes) | Entities gesamt | Dateigröße |
|---|---|---|
| 1 000 | 1 078 | 1,0 MB |
| 10 000 | 10 168 | 5,9 MB |
| 50 000 | 50 568 | 27,9 MB |
| 100 000 | 101 068 | 55,3 MB |
| 200 000 | 202 068 | 110,2 MB |

## 3. Messwerte

### 3.1 Ladezeit (Editor-Startpfad, aus `SceneLoadTiming` / `SceneOpenTiming`)

| Entities | parse (inkl. Datei lesen) | build (Entities anlegen) | load gesamt¹ | prefabSync | preload | warmup² | total |
|---|---|---|---|---|---|---|---|
| 1 078 | 24,7 ms | 9,4 ms | 37,1 ms | 0,02 ms | 0,8 ms | 533 ms | 571 ms |
| 10 168 | 176,5 ms | 69,2 ms | 280,2 ms | 0,02 ms | 1,1 ms | 577 ms | 859 ms |
| 50 568 | 831,3 ms | 323,0 ms | 1 313,6 ms | 0,02 ms | 2,7 ms | 576 ms | 1 892 ms |
| 101 068 | 1 577,1 ms | 628,8 ms | 2 495,4 ms | 0,02 ms | 4,6 ms | 581 ms | 3 081 ms |
| 202 068 | 3 264,0 ms | 1 404,7 ms | 5 255,5 ms | 0,02 ms | 11,1 ms | 581 ms | 5 847 ms |

¹ `load` ist der Aufruf `SceneSerializer::load` von außen. Der Rest über parse + build hinaus
(34 ms bei 10k, 159 ms bei 50k, also ~12 %) ist das Freigeben des nlohmann-JSON-Baums am
Funktionsende.
² Warmup = `warmupWorldMaterials`. Die Zeit hängt nicht von der Entity-Zahl ab. Beim **ersten**
Start eines frisch gebauten Binarys waren es **4 796 ms** (Pipeline-Cache kalt, Lauf `base-1000`),
ab dem zweiten Start 530–580 ms.

Daraus ergibt sich pro Entity (E-Kern):
- JSON-Parse ~16,5 µs
- Entity-Aufbau ~6,4 µs
- zusammen ~26 µs inklusive Freigeben

Der Parser schafft gut 30 MB/s. Das Asset-Preload kostet hier fast nichts, weil die Referenzwelt
nur eingebaute Meshes benutzt. Mit echten Assets wäre das anders (siehe 4.2).

### 3.2 Kosten pro Frame (600 Frames ab dem ersten Frame nach dem Laden, p50)

| Entities | CPU/Frame p50 | p99 | RSS max | extract³ | EncodeScene | EncodeSSAO | EncodeShadowMap | Overlay | FrustumCull |
|---|---|---|---|---|---|---|---|---|---|
| 1 078 | 8,6 ms | 73,5 ms⁴ | 347 MB | 2,1 | 0,9 | 0,8 | 0,9 | 0,6 | 0,15 |
| 10 168 | 64,4 ms | 81,0 ms | 432 MB | 19,7 | 6,5 | 6,5 | 6,8 | 5,1 | 0,6 |
| 50 568 | 322,5 ms | 346,3 ms | 1 154 MB | 104,9 | 35,3 | 35,3 | 34,3 | 25,2 | 3,2 |
| 101 068 | 662,2 ms | 1 490 ms⁵ | 2 126 MB | 219,9 | 78,4 | 77,8 | 69,9 | 49,9 | 8,4 |
| 202 068 | 1 312,7 ms | 2 335 ms⁵ | 4 149 MB | 411,0 | 154,2 | 150,5 | 140,7 | 110,8 | 18,7 |

³ Summe pro Frame über alle Aufrufe. `RenderExtractor::extract` läuft **4× pro Frame**
(2 399 Aufrufe in 600 Frames).
⁴ Die p99 bei 1k kommt von NextDrawable unter Bildschirmsperre. Das ist kein Engine-Hänger.
⁵ Bei 100k und 200k nur 120 Frames (`FRAMES=120`). Die p99 sind dort die Frames 0/1 (3.4).

Die Kosten wachsen linear bis 200k: **6,45 µs pro Entity und Frame** von 10k auf 50k, 6,5 µs von 100k auf 200k. Die Scopes
oben erklären etwa 190 ms der 322 ms. Den Rest trägt `OnRender` als Eigenzeit (166 ms bei 50k), und
die hat der Time Profiler aufgeschlüsselt (3.3).

### 3.3 Wohin die Zeit geht (xctrace Time Profiler, 10k Entities, 10 s, Hauptthread)

| Funktion (inklusiv) | Anteil |
|---|---|
| `OutlinerPanel::render` | **50,8 %** |
| ↳ davon ImDrawList-Pfade (`AddPolyline` 20,8 %, `AddEllipse` 11,1 %, `AddConvexPolyFilled` 7,3 %, `AddRectFilled` 7,1 %) | Zeilen-Icons jeder Zeile |
| `MetalRenderer::EncodeFrame` | 39,1 % |
| ↳ `RenderExtractor::extract` (alle 4 Aufrufe) | 29,6 % |
| ↳↳ `HE::propagateTransforms` | 14,9 % (davon `localMatrix` 10,4 %, `sincosf` 8,3 %) |
| ↳ `AABB::transformed`, `entityVisibility`, `isEntityActive`, `vector<RenderObject>::resize`, `RenderWorld::clear` | je 1–3 % |
| `ViewportPanel::render` → `ViewportActions::anyHidden` (Lauf über die ganze Szene) | 1,7 % |

`OutlinerPanel.cpp` benutzt keinen `ImGuiListClipper`. Jede Zeile wird mit Icon gezeichnet, auch
die außerhalb des sichtbaren Bereichs. Dieser Teil betrifft nur den Editor. Die Spiel-Runtime hat
keinen Outliner, Extraktion und Hierarchie-Lauf trägt sie aber genauso.

### 3.4 Hänger nach dem Laden

- **Frames 0 und 1** kosten 0,5–1,1 s CPU. Fast alles davon liegt in `Metal::EncodeScene`: 467 und
  735 ms schon bei 1k Entities, 482 und 724 ms bei 50k. Die Zeit hängt also nicht von der
  Entity-Zahl ab. Wahrscheinlich sind es Pipeline-Erstellung oder Himmel-Bake beim ersten
  Gebrauch. Ohne `--detailed` gibt es keine Unter-Scopes, das ist offen für Schritt 3.
- Ab Frame 2 laufen die Frames gleichmäßig. Die Referenzwelt löst keinen weiteren Nachlade-Hänger
  aus: die Meshes sind eingebaut, und das Terrain wird beim Laden einmal gebaut.
- Gemessen bis zum fertigen Laden plus 2 Frames sind das bei 1k Entities **~1,9 s** (warmer Cache),
  bei 50k **~3,9 s**, bei 100k ~6,1 s und bei 200k ~10,6 s. Frame 0/1 wachsen ab 100k mit, weil
  dann der Frame selbst schon über 0,6 s kostet.

### 3.5 Weltgrenzen: Float-Präzision (`scripts/perf/float_precision_probe.cpp`)

Die Probe rechnet mit der Engine-eigenen glm dieselbe Mathematik wie der Renderer und vergleicht
gegen eine double-Referenz:
- `mvp = proj * view * model` in float auf der CPU, wie `MetalRenderer`/`D3D11Renderer`
  (`viewProj * transform`).
- Kamera und Objekt liegen auf der Diagonalen im Abstand d vom Ursprung, das Objekt 5 m vor der
  Kamera.
- Gemessen wird der schlechteste Wert über 64 Lagen.

| Abstand vom Ursprung | float-Schritt | Fehler 60-Hz-Laufschritt (1,4 m/s) | Bildzittern (1080p, 60° FOV) |
|---|---|---|---|
| 1 km | 0,06 mm | 0,1 % | 0,05 px |
| 4 km | 0,24 mm | 0,4 % | 0,13 px |
| 8 km | 0,49 mm | 0,4 % | 0,25 px |
| 16 km | 0,98 mm | 0,4 % | 0,78 px |
| **32 km** | 1,95 mm | 0,4 % | **1,55 px** |
| 65 km | 3,9 mm | 0,4 % | 2,0 px |
| 100 km | 7,8 mm | 0,4 % | 5,2 px |
| 250 km | 15,6 mm | **33 %** | 9,2 px |
| 1 000 km | 62,5 mm | 100 % (Schritt geht verloren) | 40 px |
| 10 000 km | 500 mm | 100 % | 154 px, Tiefenreihenfolge kippt |

**Folgerung:**
- Bis ~16 km bleibt das Zittern unter 1 px.
- Ab ~32 km ist es sichtbar.
- Ab ~250 km wird auch Bewegung grob gerastert.

Es gibt keine kamerarelative Darstellung und keinen Floating Origin (Suche nach rebase, origin
shift, floatingOrigin: leer). Jolt ist ohne `JPH_DOUBLE_PRECISION` gebaut (`CMakeLists.txt:107-124`).
Die Netzwerk-Quantisierung nimmt `worldExtent = 4096 m` an (`ProjectSettings.h:306`).

### 3.6 Feste Obergrenzen (aus dem Code)

| Grenze | Wert | Stelle |
|---|---|---|
| Lebende Entities pro Registry (EnTT 3.13, 32-bit-Id) | 1 048 575 | `vendor/entt/entity/entity.hpp:43` |
| Jolt-Bodies / Body-Paare / Kontakt-Constraints | **1 024** / 1 024 / 1 024 | `PhysicsWorld.cpp:385-575` (Warnung ab 90 %: `:1940`) |
| Jolt-Job-System | single-threaded (`JobSystemSingleThreaded`) | `PhysicsWorld.cpp` |
| Terrain-Chunks pro Terrain | 16×16 = 256 (Auflösung ≤ 1024) | `TerrainSystem.cpp:44-55, 464-476` |
| Tessellierte Chunks | 2 Builds pro Tick, 16 pro Terrain | `TerrainSystem.cpp` (`kTessBuildsPerTick`, `kTessMaxChunksPerTerrain`) |
| Clustered Lights | 256 | `LightPacking.h:160` |
| Kamera-Far-Plane (Spiel / Editor) | 1 000 m / 5 000 m | `CameraComponent.h:7`, `EditorCamera.h:127` |

**Entity-Obergrenze (Kriterium vorab festgelegt: CPU-Frame p50 ≤ 33,3 ms im Editor):**
- Linear zwischen 1k und 10k interpoliert liegt sie bei **≈ 5 000 Entities**.
- Für 60 FPS (16,7 ms) liegt sie bei ≈ 2 300.
- Bei 50k läuft der Editor mit 3 FPS.
- **Eine harte Wand gibt es bis 200k nicht.** 202 068 Entities laden in 5,3 s und laufen ohne Absturz, aber mit 0,76 FPS (1,31 s CPU pro Frame) und 4,1 GB RSS. Die Obergrenze ist allein die lineare Kurve pro Frame. Nach RSS (~20 KB pro Entity im Editor) wäre bei 24 GB etwa bei 1 Mio. Schluss, ungefähr dort, wo auch die EnTT-Grenze liegt.

## 4. Wo heute gestreamt wird (Bestandsaufnahme)

### 4.1 Job-System (`src/HE_Core/include/JobSystem/JobSystem.h`, `JobSystem.cpp`)

- **Ein** `ThreadPool` mit `hardware_concurrency` Threads, auf dem M5 10 (`globalPool()`). Der Pool
  entsteht erst beim ersten Gebrauch, im Log nach dem Laden der Szene.
- **Eine FIFO-Queue hinter einem Mutex.** Es gibt keine Prioritäten, keine Abhängigkeiten, keinen
  Abbruch und keine Gruppen oder Zähler zum Warten. `submit()` liefert `std::future`, `post()`
  ist fire-and-forget.
- `parallel_for`:
  - Mindestkorn 256.
  - Höchstens 4×(Worker+1) Chunks.
  - Der Aufrufer arbeitet nur die eigenen Chunks ab.
  - Helfer-Tasks landen **in derselben Queue wie Asset-Loads** (`AssetLoad`, `AssetLoadPak`),
    Content-Sync und Thumbnails (`JobSystem.h:150-154`). Steht ein großer Load vorne, rechnet der
    Frame allein. Das deadlockt nicht, verliert aber die Parallelität.
- **Aufrufer:**
  - `ContentManager.cpp:1118` (AssetLoad) und `:1360` (AssetLoadPak)
  - `IntegrityProbe.cpp:224`
  - `EngineContentSync.cpp:208`
  - `ContentBrowserPanel.cpp:211`, `:2627`
  - `FrustumCuller.cpp:61`
  - `OcclusionCuller.cpp:332`
  - `RenderExtractor.cpp:408`, `:518`
  - Sky-Env-Bake in allen fünf Backends
- **HE_Scene benutzt den Pool nicht.** Auch Jolt nicht: es läuft single-threaded.

### 4.2 Assets (ContentManager, `.hpak`)

- **Synchron** (Hauptthread):
  - `loadAsset`, `ensureResident`, alle `acquireXxx`, `resolveMaterialRef`/`resolveTextureRef`.
  - `SceneSystems::preloadAssetRefs` (`SceneSystems.cpp:280`). Der Editor ruft es bei jedem
    Szenen-Öffnen. Es dedupliziert die Referenzen nicht: N Meshes bedeuten N Aufrufe von
    `ensureResident`.
  - `RenderExtractor::resolveSectionMaterial` → `ensureResident` liegt im Frame-Pfad
    (`RenderExtractor.cpp:144`).
- **Asynchron** (nur im Spiel):
  - `GameApplication::streamSceneAssets` (`GameApplication.cpp:1449`) ruft `loadAssetAsync(UUID)`
    für jede Referenz der Szene.
  - Im Pool laufen nur Lesen, Entschlüsseln und Entpacken.
  - Parsen und Registrieren laufen am Hauptthread in `pollAsyncResults`. Das Budget dort zählt
    **Assets, nicht Zeit**: 16 pro Frame im Spiel, 4 im Editor.
  - `expandFrontier` lädt Abhängigkeiten nach.
- **GPU-Upload** passiert synchron beim ersten Zeichnen (z. B. `MetalRenderer::ResolveMesh`,
  `.mm:8808`). Es gibt **kein Upload-Budget**.
- **Kein automatisches Entladen:**
  - Es gibt keine Eviction und kein LRU, `unloadAsset` gibt es nur manuell.
  - Beim Entladen einer Zone (`GameApplication.cpp:1872-1900`) werden Entities und Bodies
    zerstört, Assets aber nicht entladen.
- **`.hpak`:**
  - Kein mmap. Ein persistenter `ifstream` pro Reader, der nicht threadsicher ist.
  - Einträge werden immer ganz gelesen und entpackt (LZ4/zstd, AES-GCM). Block-Framing ist
    reserviert und wird abgelehnt.
  - **Jeder `AssetLoadPak`-Job baut einen neuen `HpakReader`.** Er liest und hasht dabei das ganze
    Inhaltsverzeichnis neu (`ContentManager.cpp:1367`) und loggt „Mounted package…“ auf INFO.
    Bei vielen kleinen Assets ist das Durchsatz für Schritt 3.

### 4.3 Szenen und Entities

- `.hescene` ist JSON. In der `.hpak` liegt die Szene als CBOR desselben JSON.
- Laden heißt immer: Datei ganz lesen, ganz parsen, ganz anwenden (`applySceneJson`,
  `SceneSerializer.cpp:1964`), **in einem blockierenden Aufruf am Hauptthread**. Nichts davon ist
  inkrementell oder zeitlich gestückelt.
- **Zonen:** Das einzige Level-Streaming sind die Zonen der Skript-API
  (`HE::api::scene::loadAdditive/unloadZone/showZone/…`, `EngineApi.h:2319-2370`).
  - Sie werden per Skript ausgelöst, nicht nach Entfernung.
  - `loadAdditive` deserialisiert synchron, legt Physik je Entity an und startet danach
    `streamSceneAssets`. **Das geht erneut über die ganze Welt**, nicht nur über die neue Zone.
- Begriffe wie SubLevel, WorldPartition oder Cell-Streaming gibt es nicht.

### 4.4 Terrain

- **Kein Streaming nach Entfernung.** Ist ein Terrain `dirty`, baut `TerrainSystem::updateTerrains`
  alle Chunks samt aller LOD-Meshes in einem Tick am Hauptthread, ohne Budget
  (`TerrainSystem.cpp:348-626`).
- Alle LOD-Meshes bleiben dauerhaft registriert.
- Nach Entfernung arbeitet nur die Tessellierung (`updateTessellation`, max. 2 Builds pro Tick).
- `FoliageSystem` streut die ganze Landschaft synchron. Ein Limit gibt es nicht, nur eine Warnung
  ab 200 000 Instanzen.

### 4.5 Was pro Frame über alle Entities läuft

- **`propagateTransforms`** (`TransformHierarchy.cpp:14-26, 85-96`): rekursiv über die ganze
  Hierarchie, ohne Dirty-Flag. Für jedes Entity wird `localMatrix` mit `sin`/`cos` aus
  Euler-Graden neu gerechnet. Aufgerufen in **jedem** `extract`.
- **`RenderExtractor::extract`**: läuft im Metal-Renderer je Pass neu.
  - Aufrufe in `EncodeShadowMap` (`MetalRenderer.mm:7551`), `EncodeSSAO` (`:11819`),
    `EncodeScene` (`:13387`), `EncodeGBuffer` (`:15464`), `EncodeGIAccelBuild` (`:7992`) und
    `EncodeFrame` (`:16416`).
  - Gemessen sind 4 Aufrufe pro Frame.
  - Gesammelt wird vor dem Culling über alle Entities. Seriell ist dabei der Teil mit den
    ContentManager-Lookups, parallel nur das Kopieren.
- **`FrustumCuller::cull`**: linear über alle Objekte, ohne BVH, Octree oder Grid. Bei 50k sind
  das 3,2 ms, also (noch) klein.
- **`LODSystem::update`**: `worldPositionOf` pro Entity, mit einer `std::vector`-Allokation je
  Aufruf (`TransformHierarchy.cpp:47`).
- **Nur im Editor:**
  - `OutlinerPanel::render`: alle Zeilen, ohne Clipper.
  - `ViewportActions::anyHidden`: Lauf über die ganze Szene.
  - `Metal::Overlay`: 25 ms bei 50k. Das ist der ImGui-Pass (`m_overlayCallback`,
    `MetalRenderer.mm:16669`), er rendert die Draw-Listen der Outliner-Zeilen. Fällt mit Punkt 2
    in Abschnitt 5 weg.

## 5. Die drei größten Engpässe

Gewichtet nach den gemessenen Kosten bei 10k–50k Entities.

1. **Die Welt wird mehrfach pro Frame komplett extrahiert und die Hierarchie jedes Mal neu
   durchlaufen.**
   - `RenderExtractor::extract` läuft 4× pro Frame. Jeder Lauf ruft `propagateTransforms` über
     alle Entities, ohne Dirty-Flag, mit `sin`/`cos` pro Entity.
   - Bei 50k sind das ~105 ms für extract und ~70 ms Encode-Anteil pro Frame.
   - Zusammen mit den Encode-Pässen ist das der größte Block, der auch im Spiel anfällt.
   - Hebel:
     - einmal pro Frame extrahieren und den RenderWorld in den Pässen wiederverwenden;
     - Transforms nur für geänderte Teilbäume propagieren;
     - vor dem Sammeln räumlich vorfiltern (Zellen oder BVH).
   - Gehört zu Schritt 4 (mehr Entities). Ohne diesen Umbau bleibt jede Welt über ~5k Entities
     unter 30 FPS.
2. **Der Editor-Outliner zeichnet jede Zeile.** Das ist 51 % der Hauptthread-Zeit bei 10k
   Entities, ohne `ImGuiListClipper`. Es betrifft nur den Editor. Weil `he_perf_capture` aber den
   Editor misst, verdeckt es jede andere Verbesserung. Deshalb muss es vor der Vorher/Nachher-Messung
   in Schritt 5 behoben oder herausgerechnet werden (z. B. Outliner zu).
3. **Laden und Streamen sind blockierend und haben kein Zeitbudget.**
   - Szenen und Zonen werden in einem Stück am Hauptthread geparst und gebaut: 26 µs pro Entity,
     1,3 s bei 50k.
   - `pollAsyncResults` begrenzt die Zahl der Assets, nicht die Zeit. Der GPU-Upload hat gar kein
     Budget.
   - Asset-Jobs teilen sich die eine FIFO-Queue mit den Frame-Helfern und kennen weder Priorität
     noch Abbruch. Jeder Pak-Job öffnet das Inhaltsverzeichnis neu.
   - Nach Entfernung gestreamt wird nichts: Terrain, Foliage und Entities sind immer ganz da, und
     Assets werden nie entladen.
   - Das ist der Kern von Schritt 2 (Job-Prioritäten, Abbruch) und Schritt 3 (Durchsatz, Latenz).

**Außerhalb der drei, aber mit fester Grenze:**
- Float-Präzision: sichtbares Zittern ab ~32 km, Bewegungsraster ab ~250 km. Es gibt weder einen
  Floating Origin noch kamerarelatives Rendern.
- Jolt mit 1 024 Bodies und single-threaded.

Für „größere Welten“ (Schritt 4) sind das die harten Wände. Das Body-Limit kommt vor der
Präzisionsgrenze.

## 6. Offen / nicht gemessen

- Die Spiel-Runtime mit einem exportierten `.hpak`, also der echte asynchrone Streaming-Pfad mit
  Upload-Hängern.
- Der 0,5–0,7 s teure `EncodeScene` in Frame 0/1. Unter-Scopes braucht `--detailed`.
- Eine Messung ohne Stromsparmodus und ohne Bildschirmsperre. Alle Zahlen hier sind E-Kern-Zahlen
  unter Sperre. Schritt 5 muss unter **denselben** Bedingungen messen oder beide Seiten neu messen.
- Jolt mit mehr als 1 024 Bodies (nur Play-Modus).

## 7. Messhilfen und Wiederholung

- `scripts/perf/gen_reference_world.py`: Referenzwelt. Parameter: Anzahl, Ausdehnung, Versatz,
  Lichter, Physik-Anteil, Gruppen.
- `scripts/perf/world_streaming_ladder.sh`: `he_perf_capture` je Größe, druckt die Timing-Zeilen.
  Umgebung: `EDITOR=`, `WARMUP`, `FRAMES`, `TIMEOUT`, `EXTENT`, `OFFSET`.
- `scripts/perf/float_precision_probe.cpp`: die Tabelle in 3.5.
- Log-Zeilen:
  - `SceneLoadTiming: entities= parseMs= buildMs=` in `SceneSerializer::loadJSON`, gilt für
    Editor **und** Spiel.
  - `SceneOpenTiming: loadMs= prefabSyncMs= preloadMs= warmupMs= totalMs=` im Startpfad des
    Editors.
- Rohdaten: `docs/perf-audit/raw-streaming/*.summary.json`. Die Logs schließt die Repo-`.gitignore`
  aus, die Profil-Dumps (6–10 MB) die `.gitignore` im Ordner. Die Timing-Zeilen stehen oben in 3.1.

```sh
# Release-Build im Worktree (Deps vom Nachbar-Build, siehe Memory headless-dump-log-and-worktree-configure)
cmake --build out/build/release -j8
cp -R ~/HorizonEngineProjects/Test /tmp/ws_proj/
scripts/perf/world_streaming_ladder.sh /tmp/ws_proj/Test/Test.heproj docs/perf-audit/raw-streaming base 1000 10000 50000
FRAMES=120 TIMEOUT=1500 scripts/perf/world_streaming_ladder.sh /tmp/ws_proj/Test/Test.heproj docs/perf-audit/raw-streaming base 100000 200000
clang++ -std=c++17 -O2 -I src/HE_Rendering/glm scripts/perf/float_precision_probe.cpp -o /tmp/fpp && /tmp/fpp
# Profil: während eines Laufs
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun xctrace record --template 'Time Profiler' --attach <pid> --time-limit 10s --output t.trace
```

## 8. Schritt 3: Streaming-Durchsatz und Lade-Latenz (Nachher-Messung)

Stand 06.10.2026, Commits `b3ed94d1` und `3813a591` auf dem Zweig, Basis `652c0b66` (Ende Schritt 2).

**Kurz:**
- Szene laden im Editor: **200k Entities 5,32 s → 2,73 s, 50k 1,36 s → 0,70 s** (je −49 %).
  Der Parse läuft jetzt parallel auf dem Pool, das Freigeben des JSON-Baums auf einem Worker.
- Pak-Streaming (Spielpfad, Mikro-Bench): **4 000 kleine Assets 1,0 s → 0,1 s** (Faktor 10).
  Jeder Job hatte das ganze Inhaltsverzeichnis neu gelesen und gehasht, das war quadratisch.
- Hauptthread-Anteil beim Streamen großer Assets: **16,5 ms → 2 ms** für 128 × 1 MiB, weil die
  Chunk-Zerlegung (eine volle Kopie) jetzt auf dem Worker passiert.
- Die Kosten pro Frame sind unverändert. Sie gehören zu Engpass 1 (Schritt 4).

### 8.1 Messbedingungen

| Punkt | Wert |
|---|---|
| Gerät, Build | wie Abschnitt 1 (M5, Release, `HE_PROFILING=ON`, `HE_ENABLE_SHADERC=ON`, `out/build/release`) |
| Energie | **Stromsparmodus AN** (`pmset -g`: `lowpowermode 1`), also E-Kern-Zahlen wie in Schritt 1 |
| Bildschirm | gesperrt (`CGSSessionScreenIsLocked=Yes`) |
| GPU-Fremdlast | 15 % „Device Utilization“ vor den Läufen (Schritt 1: 57 %) |
| Systemlast | vorher und Teil 1: load average 4–5 (andere Bienen bauten), nachher: ~2 |
| Programm | Editor über `scripts/perf/world_streaming_ladder.sh` (`he_perf_capture`, `--warmup 0`), 120 Frames bei 1k–50k, 20 Frames bei 200k; vor jeder Serie ein verworfener Lauf (kalter Pipeline-Cache) |

**„Vorher“ ist neu gemessen**, auf HEAD `652c0b66` (nach Schritt 2), nicht aus Abschnitt 3 übernommen:
Schritt 2 hat das Job-System umgebaut, und die Bedingungen (GPU-Last, Systemlast) waren andere.
Die neuen Vorher-Werte liegen dicht an Abschnitt 3.1 (200k: 5 316 ms gegen 5 256 ms).

Der Editor, den die Leiter misst, lief nachweislich auf diesem Build: Die Logs enthalten
`SceneLoadTiming`, das nur auf diesem Zweig existiert (die installierte App in `/Applications`
kennt es nicht, siehe 8.5).

### 8.2 Ladezeit im Editor (Referenzwelt aus Abschnitt 2)

`load` = `SceneSerializer::load` von außen, `parse` schließt das Lesen der Datei ein.

| Entities | load vorher | load Teil 1¹ | **load nachher** | parse vorher → nachher | build vorher → nachher | total vorher → nachher |
|---|---|---|---|---|---|---|
| 1 078 | 35,0 ms | 30,5 ms | **28,7 ms** | 23,8 → 18,4 ms | 9,2 → 9,9 ms | 604 → 595 ms |
| 10 168 | 224,3 ms | 229,3 ms | **150,3 ms** | 150,9 → 74,9 ms | 42,2 → 75,3 ms² | 792 → 685 ms |
| 50 568 | 1 356,6 ms | 1 095,8 ms | **697,6 ms** | 857,1 → 332,0 ms | 340,9 → 365,4 ms | 1 929 → 1 207 ms |
| 202 068 | 5 315,7 ms | 4 387,4 ms | **2 727,0 ms** | 3 301,0 → 1 307,9 ms | 1 439,3 → 1 418,9 ms | 5 903 → 3 364 ms |

¹ Teil 1 (`b3ed94d1`): Datei am Stück lesen, aus dem Puffer parsen, JSON-Baum auf einem Worker
freigeben. Das Freigeben (Abschnitt 3.1, Fußnote 1: ~11 %) fällt damit aus `load` heraus, der
Parse aus dem Puffer statt aus dem `std::istream` bringt nur ~10 %. Der DOM-Aufbau von nlohmann
selbst ist der Engpass, deshalb Teil 2.
² Kein Effekt des Umbaus: Zwei Wiederholungen im Endstand ergaben 77,9 und 76,1 ms, Abschnitt 3.1
hatte 69,2 ms. Die 42,2 ms im Vorher-Lauf sind der Ausreißer. Den Aufbau hat Schritt 3 nicht
angefasst. Der parallele Parse streut mit der Systemlast: 50k in zwei Wiederholungen 225 und 355 ms.

Pro Entity (E-Kern, Last ~2): Parse ~6,5 µs statt ~16,5 µs, Aufbau unverändert ~7 µs.
`warmup` (530–640 ms) und die Kosten pro Frame (CPU p50 1k 8,6 / 10k 64,2 / 50k 320,7 /
200k 1 349 ms) sind wie vorher.

### 8.3 Pak-Streaming, der Spielpfad (Ersatzmessung)

`he_perf_capture` kann nur den Editor starten, und einen Export von der Kommandozeile gibt es nicht.
Den Spielpfad (gemountete `.hpak` → `loadAssetAsync` → `pollAsyncResults`) misst deshalb ein
Bench in `tests/test_hpak.cpp`, Release-Build, vorher und nachher auf derselben Maschine:

```sh
out/build/release/tests/he_tests --no-skip --test-case='Streaming bench*'
```

Er streamt zwei Paks vollständig und pumpt dabei ohne Schlafen (im Stromsparmodus dauert
`sleep_for(2ms)` ~150 ms). Gemessen werden die Wandzeit bis alles registriert ist und die Zeit am
Hauptthread in `pollAsyncResults` (Höchstzahl 16 pro Aufruf, wie das Spiel bisher). Drei Läufe je
Seite, Rohausgabe in `docs/perf-audit/raw-streaming/s3-bench-{vorher,nachher}.txt`.

| Last | Wand vorher | **Wand nachher** | Hauptthread vorher | **Hauptthread nachher** |
|---|---|---|---|---|
| 4 000 Materialien (je ~100 B, zstd) | 980–1 018 ms | **95–101 ms** | 17–19 ms | 11–17 ms |
| 128 Assets à 1 MiB (zstd) | 326–329 ms | 328–360 ms | 16,0–16,7 ms | **1,3–2,3 ms** |

- **Kleine Assets:** Jeder `AssetLoadPak`-Job baute einen neuen `HpakReader`, der das ganze
  Inhaltsverzeichnis las, hashte und „Mounted package…“ loggte. Bei N Einträgen sind das N × N.
  Jetzt teilen sich die Jobs das beim Mount geprüfte Inhaltsverzeichnis (`HpakReader::sharedToc`,
  `openShared`) und lesen nur noch den Header, um `tocHash` abzugleichen.
- **Große Assets:** Die Wandzeit begrenzt das Entpacken (zstd), sie bleibt. Der Hauptthread spart
  die Chunk-Zerlegung (`HAsset::Reader::openData`, eine volle Kopie), die jetzt im Worker läuft.

Was das im Spiel heißt: `GameApplication` registriert jetzt nach **Zeit** statt nach Stückzahl,
4 ms pro Frame für Registrieren und Material-Warmup zusammen, in Paketen zu 16. Vorher waren es
fest 16 Assets pro Frame. Ein Level mit 4 000 kleinen Assets brauchte damit allein 250 Frames
zum Registrieren, wie billig jedes einzelne auch war.

### 8.4 Was umgesetzt ist

| Maßnahme | Stelle |
|---|---|
| Szene: Datei in einem Stück lesen, aus dem Puffer parsen (JSON und CBOR) | `SceneSerializer.cpp` (`readWholeFile`) |
| Szene: JSON-Baum auf einem Low-Job freigeben (`load`, `loadAdditive`, `loadBinary`, `loadFromMemory`, `loadAdditiveFromMemory`) | `SceneSerializer.cpp` (`releaseOnWorker`) |
| Szene: `entities` parallel parsen, Ergebnis gleich `json::parse`, bei jeder unerwarteten Form Rückfall auf den ganzen Parse | `SceneJsonParse.{h,cpp}` (`HE::parseSceneText`) |
| Pak: Inhaltsverzeichnis pro Mount einmal lesen und prüfen, Jobs teilen es | `HpakReader::sharedToc/openShared`, `ContentManager::launchPakLoad` |
| Chunk-Zerlegung im Worker statt am Hauptthread | `AsyncResult::asset`, `splitChunks` |
| Zeitbudget für `pollAsyncResults` (mindestens ein Ergebnis pro Aufruf, nur was beim Aufruf schon da war) | `ContentManager::pollAsyncResults(max, budgetMs)` |
| Spiel: 4 ms pro Frame für Registrieren und Warmup | `GameApplication.cpp` |
| Asset-Referenzen der Szene ohne Duplikate (Reihenfolge der ersten Nennung) | `SceneSystems::collectAssetRefs` |

Tests: `test_scene_serializer` (Äquivalenz mit Strings voller Klammern und Escapes, mit
Positivkontrolle, dass wirklich geteilt wurde; Ausweichformen; große Szene über `load`),
`test_hpak` (`openShared` und ersetzter Pak, Rückfall im ContentManager, Zeitbudget,
Chunk-Zerlegung gleich synchronem Laden, Duplikate). 227 Testfälle in den betroffenen Dateien grün.

### 8.5 Offen und nicht gemacht

- **Vorausladen nach Kamerabewegung:** Bewusst nicht gemacht. Es gibt nichts nach Entfernung zu
  laden: keine Zellen, Zonen werden per Skript geladen (Abschnitt 4.3). Braucht erst eine
  räumliche Einteilung der Welt, das ist Schritt 4.
- **CBOR-Szenen (Spielstart aus der `.hpak`, Undo) parsen weiter sequentiell.** CBOR-Elemente
  lassen sich ohne Dekodieren nicht abgrenzen. Sie haben nur das Freigeben auf dem Worker.
- **GPU-Upload ohne Budget** (Abschnitt 4.2) bleibt: Renderer-Arbeit, nicht Teil dieses Themas.
- **Der Editor** pollt weiter `pollAsyncResults(4)` ohne Zeitbudget, nur das Spiel hat es.
- `releaseOnWorker` gibt den JSON-Baum später frei: Eine RSS-Messung direkt nach `load` sieht
  ihn noch, und bei voller Low-Obergrenze wartet die Freigabe hinter Streaming-Jobs.
- **Pak-Loads** brechen weiterhin nur vor dem Start ab (`readEntry` hat keinen Checkpoint).
- **Hänger in Frame 0 und 1** (0,46 und 0,72 s in `Metal::EncodeScene`, schon bei 1k Entities)
  ist weiter unaufgelöst. `--detailed` schlüsselt nur die GPU auf, Unter-Scopes auf der CPU gibt es
  nicht. Ein Time-Profiler-Mitschnitt mit `xctrace record --launch` startete auf diesem Mac die
  installierte `/Applications/HorizonEditor.app` statt des Deploy-Binarys, auch mit absolutem Pfad,
  und war damit wertlos (zu sehen an den Binary-Pfaden im Export). Beide Versuche haben die
  installierte App dabei sichtbar gestartet (unter der Bildschirmsperre). Für den nächsten Versuch: Editor
  per `he_perf_capture` starten und mit `--attach` sofort anhängen, oder `EncodeScene` mit
  Unter-Scopes versehen.
- Die Spiel-Runtime ist weiter nur über den Bench gemessen, nicht über ein exportiertes Spiel.

Rohdaten: `docs/perf-audit/raw-streaming/s3vorher-*`, `s3teil1-*`, `s3nachher-*`, `s3wdh{a,b}-*`
(`*.summary.json`), die Timing-Zeilen aller Läufe in `s3-timings.txt`.

Wiederholen:

```sh
cmake --build out/build/release -j8 --target HorizonEditor he_tests
FRAMES=30 scripts/perf/world_streaming_ladder.sh /tmp/ws_proj/Test/Test.heproj /tmp/ws3/raw verwerfen 1000
FRAMES=120 scripts/perf/world_streaming_ladder.sh /tmp/ws_proj/Test/Test.heproj /tmp/ws3/raw s3nachher 1000 10000 50000
FRAMES=20 TIMEOUT=1500 scripts/perf/world_streaming_ladder.sh /tmp/ws_proj/Test/Test.heproj /tmp/ws3/raw s3nachher 200000
out/build/release/tests/he_tests --no-skip --test-case='Streaming bench*'
```

## 9. Schritt 4: Größere Welten und mehr Entities (Richtungsmessung)

Stand 06.10.2026, Commit `5aa5f65a` auf dem Zweig, Basis `5d9b8a09` (Ende Schritt 3). Die
endgültige Vorher/Nachher-Messung ist Schritt 5. Hier steht ein Richtungszeuge mit der Messleiter.

**Kurz:**
- CPU pro Frame im Editor: **10k 64,2 → 21,8 ms, 50k 320,7 → 118,0 ms** (je etwa −65 %).
  Pro Entity und Frame **≈ 2,4 µs statt 6,45 µs**.
- Die 30-FPS-Grenze aus 3.6 (≈ 5 000 Entities) liegt jetzt bei **≈ 15 000**, die 60-FPS-Grenze
  bei ≈ 8 000 statt ≈ 2 300 (linear zwischen 10k und 50k).
- RSS bei 50k **1 154 → 500 MB**. Der größte Teil davon waren die ImDrawList-Puffer der
  Outliner-Icons.
- Jolt trägt **65 536 Bodies statt 1 024**. Das war die erste harte Wand für größere Welten.

### 9.1 Messbedingungen

Wie 8.1: M5, Release (`HE_PROFILING=ON`, `HE_ENABLE_SHADERC=ON`), **Stromsparmodus AN**,
**Bildschirm gesperrt**, GPU-Fremdlast 58 % vor den Läufen (Schritt 1: 57 %), Load average 2,4–2,9.
Editor über `scripts/perf/world_streaming_ladder.sh`, 120 Frames, `--warmup 0`, vorher ein
verworfener Lauf. „Vorher“ sind die Werte aus 8.2 (Ende Schritt 3, `s3nachher-*`), nicht neu
gemessen: Schritt 4 hat Laden und Parse nicht angefasst, die Bedingungen sind dieselben.

### 9.2 Kosten pro Frame (p50)

| Entities | CPU vorher | **CPU nachher** | `extract` vorher → nachher¹ | `OnRender` vorher → nachher | `Metal::Overlay` vorher → nachher | RSS max vorher → nachher |
|---|---|---|---|---|---|---|
| 1 078 | 8,6 ms | 11,4 / 10,1 ms² | 2,1 → 1,0 ms | 4,5 → 1,5 ms | 0,6 → 0,1 ms | 343 → 336 MB |
| 10 168 | 64,2 ms | **21,8 / 22,0 ms** | 19,6 → 10,1 ms | 38,7 → 10,4 ms | 5,1 → 0,1 ms | 433 → 313 MB |
| 50 568 | 320,7 ms | **118,0 ms** | 102,8 → 64,3 ms | 191,7 → 56,7 ms | 25,1 → 0,1 ms | 1 154 → 500 MB |

¹ Summe aller `RenderExtractor::extract`-Aufrufe pro Frame. Es sind weiter vier Aufrufe pro Frame
(479 in 120 Frames), aber zwei davon (SSAO und Szene) antworten aus der Kopie des ersten:
`RenderExtractor::reuse` kostet 0,8 ms bei 10k und 4,4 ms bei 50k statt je eines vollen Laufs.
Voll laufen noch der Schatten-Pass und der Pick-Snapshot des `ViewportPanel` (9.4).
² Zwei Läufe. Bei 1k ist der Frame so billig geworden, dass er unter Bildschirmsperre auf
`Metal::NextDrawable` wartet (`WaitForFrame` 4,9 ms, vorher nicht unter den größten Scopes).
Die Engine-Scopes selbst sind kleiner geworden. Das ist die Falle aus der Memory „Perf-Messung
auf dem M5“: gesperrt nur die Pass-Scopes vergleichen.

Die Ladezeit ist unverändert (`SceneLoadTiming` 50k: parse 320 ms, build 336 ms, wie 8.2).

### 9.3 Was umgesetzt ist

| Maßnahme | Stelle | Wirkung |
|---|---|---|
| Lokale Matrix wird pro Entity gecacht, zusammen mit den Position/Rotation/Skala-Werten, aus denen sie gebaut wurde. `sin`/`cos` nur bei Wertänderung. Vergleich nach Wert, nicht über `dirty`, weil Inspector und Gizmo das Flag nicht setzen | `TransformComponent::localCache*`, `propagateTransforms`, `HE::cachedLocalMatrix` | `propagateTransforms` war 14,9 % des Hauptthreads bei 10k (3.3) |
| `worldMatrixOf` legt die Elternkette auf den Stack statt in einen `std::vector` pro Aufruf und liest denselben Cache | `TransformHierarchy.cpp` | `LODSystem::update` fragt das für jedes LOD-Entity jeden Tick (4.5) |
| Extraktion einmal pro Frame: Zwischen `beginFrame` und `endFrame` beantwortet `extract()` weitere Aufrufe mit gleichen Eingaben (Welt, Editor-Kamera nach Wert, Tag/Nacht, Schatten-Einstellungen, ContentManager samt Epoche) aus der Kopie des ersten Laufs. Weicht nur das Seitenverhältnis ab (SSAO in halber, gerundeter Auflösung), werden Projektion, Kaskaden-Fit und lokale Schatten-Layer neu gerechnet, genau wie im vollen Lauf | `RenderExtractor::beginFrame/endFrame/FrameScope`, `MetalRenderer::EncodeFrame` | Engpass 1 aus Abschnitt 5 |
| Jolt: 65 536 Bodies, 65 536 Body-Paare, 10 240 Kontakte (Jolts eigene Empfehlung) statt je 1 024 | `PhysicsWorld.cpp` (`Impl::kMaxBodies` …) | harte Wand aus 3.6 |
| Outliner: Auge und Schloss werden für weggescrollte Zeilen nicht gezeichnet, und der Teilbaum-Lauf für das Auge entfällt dort. Gleiches Layout per `Dummy` | `OutlinerPanel.cpp` (`drawRowIcons`) | Engpass 2 aus Abschnitt 5, ohne Clipper |

Die Wiederverwendung ist nur innerhalb eines `FrameScope` scharf. Ein Backend, das ihn nicht
setzt (OpenGL, Vulkan, D3D11, D3D12, Software), extrahiert unverändert bei jedem Aufruf. Der
Vertrag: zwischen zwei Aufrufen im Scope wird die Welt nicht verändert. Im Metal-Renderer läuft der
Overlay-Callback (dort baut der Editor seine Panels) erst nach dem letzten Extract.

Bild-Zeuge im echten Metal-Passablauf: `he_shot.py` mit `SHADOWINSTTEST=contact
LOCALSHADOW=point SSAO=1 TOD=0.35 PITCH=-18 CAMY=5 AA=0 RENDERSCALE=0.77 BLOOM=0 MOTIONBLUR=0
DOF=0 GI=0`, `HE_SKY_TIME=1.0`, frisches `HE_CONFIG_DIR`, forward (`RENDERPATH=0`) und deferred
(`RENDERPATH=1`). `RENDERSCALE=0.77` sorgt dafür, dass Schatten- und Szenen-Pass mit
verschiedenem Seitenverhältnis extrahieren. Zwei Läufe mit `FrameScope` waren bytegleich
(Rauschboden), ein Lauf mit auskommentiertem `FrameScope` ebenfalls bytegleich zu ihnen
(md5 forward `1c29ed2d…`, deferred `b22e44a6…`). Die Wiederverwendung ändert also kein Pixel.

Tests: `tests/test_world_scale.cpp`. Cache-Korrektheit (Schreiben ohne `dirty`, bewegter
Elternknoten, Umhängen, bitgleich mit `localMatrix`), 100 000 Entities (Stichproben gegen
`worldMatrixOf`, nach Bewegung einer Gruppe; Extraktor sieht alle 100 000 Meshes), Wiederverwendung
(gleiche Eingaben, anderes Seitenverhältnis gegen einen vollen Lauf, kein Wiederverwenden ohne
Scope oder mit anderer Kamera/Tageszeit, neues Entity im nächsten Frame), 3 000 Bodies plus ein
fallender (Negativkontrolle mit dem alten Limit: 1 023 gebaut, Test rot), eine Position 100 km
draußen, Replikation einer 30-km-Position. Dazu 36 betroffene Testdateien grün (Physik,
Replikation, Savegame-Pfade, Outliner, LOD, Culling, Sequencer, Kamera-Rig, Szenen-Serializer).

### 9.4 Offen und bewusst nicht gemacht

- **Floating Origin / kamerarelatives Rendern: nicht angefangen.** Die Referenzwelt (8 × 8 km)
  liegt unter der 16-km-Grenze aus 3.5, und ein Ursprungswechsel berührt Physik, Replikation,
  Savegames und alle fünf Renderer zugleich. Skizze für später: `HorizonWorld` hält einen
  `glm::dvec3`-Ursprung; überschreitet die Kamera einen Radius (z. B. 8 km), werden alle Wurzel-
  Entities und Jolt-Bodies um ganze Zellen verschoben (`PhysicsWorld::setPosition` je Body) und
  der Ursprung entsprechend versetzt. Savegames und
  `GameReplication` schreiben dann `lokal + Ursprung` (die Quantisierung ist schon auf bis zu
  1 000 km einstellbar, `ProjectSettings::kMaxWorldExtent`). Weltraum-Partikel und Trails müssen
  mitgeschoben werden.
- **Entities pro Zelle streamen: nicht angefangen.** Es gibt kein Zellformat der Szene, und
  `FrustumCuller::cull` ist mit 3,2 ms bei 50k nicht der Engpass. Der nächste Hebel pro Entity ist
  die Extraktion selbst (ein voller Lauf ≈ 30 ms bei 50k, seriell wegen der ContentManager-
  Lookups).
- **Der Pick-Snapshot des `ViewportPanel`** extrahiert jeden Frame mit einem eigenen Extractor
  voll (≈ 30 ms bei 50k). Er dient Gizmo, Box-Auswahl, Kontextmenü und Terrain-Sculpt; ihn aus
  dem Frame des Renderers zu speisen oder nur bei Bedarf zu ziehen ist Editor-Arbeit (Schritt 6).
- **Andere Backends:** Vulkan extrahiert ebenfalls mehrfach pro Frame (`VulkanRenderer.cpp`), hat
  aber keinen `FrameScope`. Backend-Parität gehört nicht zu diesem Thema.
- **Jolt läuft weiter single-threaded** (`JobSystemSingleThreaded`). Mehr Bodies heißen jetzt
  mehr Arbeit auf einem Kern.
- **Der Outliner** zeichnet weiter jede Zeile als `TreeNodeEx` (kein `ImGuiListClipper`, die
  Baumstruktur mit `TreePush/Pop` macht das aufwendig). Teuer waren nur die Icons.
- 200k nicht nachgemessen (Richtungsmessung, Schritt 5 misst die ganze Leiter).

Rohdaten: `docs/perf-audit/raw-streaming/s4-*`, `s4wdh-*` (`*.summary.json`),
Timing-Zeilen in `s4-timings.txt`.

```sh
cmake --build out/build/release -j8 --target HorizonEditor he_tests
out/build/release/tests/he_tests --source-file='*test_world_scale.cpp'
FRAMES=30 scripts/perf/world_streaming_ladder.sh /tmp/ws_proj/Test/Test.heproj /tmp/ws4/raw verwerfen 1000
FRAMES=120 scripts/perf/world_streaming_ladder.sh /tmp/ws_proj/Test/Test.heproj /tmp/ws4/raw s4 1000 10000 50000
```
