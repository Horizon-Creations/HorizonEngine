# Welt-Streaming: Bestandsaufnahme und Messbasis (Thema 153, Schritt 1)

Stand 06.10.2026, Zweig `claude/welt-streaming-job-planung-groessere-welten-mehr-entities`
(Basis `2b0ea05f` = main, Messhilfen in `e6ee064a` und `3e9dd8e3`). In diesem Schritt
wurde nichts umgebaut. Neu sind nur Messhilfen: zwei Log-Zeilen mit Ladezeiten und drei Skripte
(Abschnitt 7). Die Zahlen sind die Vorher-Werte, gegen die Schritt 5 misst.

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
