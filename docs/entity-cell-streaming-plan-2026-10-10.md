# Entities pro Zelle streamen: Bauplan (Thema 164, Schritt 1)

Stand 10.10.2026, Zweig `claude/entities-pro-zelle-streamen-nur-die-umgebung-der-kamera-exis`,
Basis `202e14f2` (= `origin/release/0.7.0`). Die Abschnitte 0 bis 11 sind der Bauplan aus Schritt 1
(**nur Doku**, Zeilennummern gelten für `202e14f2`); **Abschnitt 12 hält fest, was Schritt 2a daraus
gebaut hat und was dabei anders war.** Bezug: `docs/world-streaming-baseline-2026-10-06.md` (Thema 153, §9.4, §10.5–10.7,
§11.4–11.5) und der Plan von Thema 162 (`docs/render-extractor-shadow-pass-plan.md`, liegt nur auf
`origin/claude/render-extractor-und-schatten-pass-einmal-pro-frame-statt-me`, Commit `16947c08`,
noch nicht auf `release/0.7.0`; hier nur zitiert, nicht übernommen).

## 0. Kurz

**Die Annahme im Thema stimmt nicht mehr.** Dort steht: „bisher gibt es nur Szenen-Zellen zum
Bearbeiten, aber kein Streaming von Entities zur Laufzeit“. Auf `release/0.7.0` gibt es das
Laufzeit-Streaming schon: `HE::CellStreamer` lädt und entlädt Zellen nach Kameraabstand mit
Vorausschau, Lesen und Parsen auf dem Job-Pool, Aufbau mit 4 ms Budget am Hauptthread, Jolt-Bodies
und Asset-Streaming je Zelle (Thema 153 Schritt 5, `CellStreamer.h/.cpp`,
`GameApplication::updateCellStreaming`). Schritt 6 hat dazu die Editor-Werkzeuge gebaut
(Split/Merge, Zellansicht). Der Bauplan baut deshalb **nichts neu**, sondern benennt, was zwischen
diesem Stand und dem Ziel „Dörfer, Wegpunkte, NPCs“ fehlt.

Was fehlt, in einem Satz: In Zellen darf heute nur **unveränderlich Platziertes** liegen (Meshes,
Punkt-/Spotlichter, statische Collider, Decals), jede Zelle bekommt bei **jedem Laden neue UUIDs**,
nur **die erste Kamera** ist Anker, eine Zelle wird **in einem Stück** aufgebaut, und im **Editor-Play
streamt nichts**.

Entscheidungen dieses Plans (Begründung in den Abschnitten):

| # | Entscheidung | Abschnitt |
|---|---|---|
| D1 | **Ein Dateiformat, eine Regeltabelle.** Laufzeit-Zellen sind dieselben `<Szene>.cells/cell_x_z.hescene` wie die Szenen-Zellen aus Schritt 6. Neu ist nur ein kleiner Kopf (`streaming`) und eine Manifest-Spalte. Split/Merge bleibt Editor-Werkzeug und ist die einzige Stelle, die entscheidet, was in eine Zelle darf | 3, 4 |
| D2 | **Zellen laden mit ihren gespeicherten UUIDs** (`preserveIds`), nicht mit frischen. Eine Zelle ist nie zweimal geladen, das Argument gegen das Wiederherstellen (Kopien derselben Szene) trifft hier nicht zu | 4.3 |
| D3 | **Drei Streaming-Klassen:** *Resident* (Basis), *Zelle-statisch*, *Zelle-zustandsbehaftet*. NPCs mit Skript und Zustand sind die dritte Klasse: beim Entladen wird der Zustand herausgeschrieben und die Entity zerstört, beim Laden wieder angewandt. Keine Simulation außerhalb des Radius | 4.4 |
| D4 | **Verweise über Zellgrenzen gibt es nicht.** Der Splitter hält Verweis-Cluster zusammen (Ref-Hülle). Was ein Verweis nicht darf, bleibt Resident. Ein Verweis auf eine entladene Entity ist `entt::null`, nie ein hängender Zeiger | 5 |
| D5 | **Anker statt „die Kamera“:** `CellStreamer::update` bekommt eine Liste (Kamera, Spieler, Skript-Pins). Server ohne Kamera laden um ihre Spieler | 4.5 |
| D6 | **Aufbau gestückelt:** Der Worker teilt die geparste Zelle in Gruppen kleiner Teilbäume, der Hauptthread baut Gruppen, solange das Budget reicht (statt einer ganzen Zelle) | 4.6 |
| D7 | **Ein gemeinsamer Host (`CellRuntime`) für Spiel und Editor-Play** statt einer dritten Kopie der Zonen-Verdrahtung. Der Editor streamt nicht (er bearbeitet die ganze Szene), das Play schon | 8 |
| D8 | **Zellen laden/entladen nur im Update-Teil des Frames**, nie innerhalb eines `RenderExtractor::FrameScope`. Dazu schlägt dieser Plan einen Struktur-Zähler vor (`HorizonWorld::structureEpoch`), den Thema 162 lesen kann; wer zuerst landet, baut ihn. **Ein** Zähler, nicht zwei | 7 |

Nicht Teil: Terrain-Streaming, Foliage/Instancing (Thema 163), Streaming im Editor, Simulation
entladener NPCs, vertikale Zellen, Asset-Eviction (Abschnitt 9).

## 1. Bestand auf release/0.7.0

| Baustein | Stelle | Was er kann | Herkunft |
|---|---|---|---|
| Manifest | `CellManifest` (`CellStreamer.h:33`, `.cpp:22`) | `cellSize`, `loadRadius`, `unloadRadius`, `lookaheadSec`, `dir`, Liste `[x, z, entities]`; `around()`/`distanceTo()` für die Editor-Ansicht | 153/5 |
| Streamer | `CellStreamer::update` (`CellStreamer.cpp:187`) | sucht nur die Gitterquadrate um Kamera und Vorausschau (`scan`, Zeile 209), Lesen+Parsen als Job (`CellLoad`, High wenn die Kamera drinsteht), Abbruch per `CancelToken`, Bau nächste zuerst, Entladen jenseits `unloadRadius` | 153/5 |
| Hooks | `CellStreamer::Hooks` (`.h:92`) | `loaded(root, created)`, `unloading(root)` | 153/5 |
| Spiel-Verdrahtung | `GameApplication::updateCellStreaming` (`GameApplication.cpp:1491`) | Reader (Pak per `detachedMountedEntryReader`, sonst lose Datei), Hooks für Jolt (`addEntity` je Entity) und `streamSceneAssets` unter eigenem Token; Aufruf im Update nach dem Floating Origin (`:3212`) | 153/5 |
| Aufbau | `SceneSerializer::loadAdditiveFromJson` (`SceneSerializer.cpp:2802`) → `applyAdditiveJson` (`:2101`) | legt eine Zelle additiv unter die Weltwurzel; die Zellwurzel steht auf `-origin` (`CellStreamer.cpp:337`), fest gegenüber Floating Origin | 153/5 |
| Parsen auf dem Worker | `parseSceneText`/`parseSceneCbor` (`SceneJsonParse.h`) | JSON oder CBOR, gleiche Zelldatei lose und im Pak | 153/5 |
| Splitter | `splitSceneIntoCells`, `splitWorldIntoCells`, `mergeCellsIntoWorld` (`CellSplit.h/.cpp`) | Regeln aus `scripts/split_scene_cells.py` in C++; Merge stellt UUIDs, Ordner und Reihenfolge her; je ein Undo-Schritt | 153/6 |
| Editor-Ansicht | `StreamingDebugView.*`, *Show ▸ Streaming Cells*, Profiler-Tab *Streaming* | zeigt, was das Spiel von der Editor-Kamera aus laden würde | 153/6 |
| Floating Origin | `FloatingOrigin.h/.cpp` | schiebt Wurzel-Kinder, also auch die Zellwurzeln, samt Jolt | 153/5 |

Gemessen (153 §10.5, Bench `'Cell streaming bench*'`, 200k-Welt): ganze Welt 2,6–2,8 s;
**Basis 4–8 ms + 14 Zellen (11 527 Entities) in 87–170 ms**, schlechtester Hauptthread-Frame
10–99 ms. Der Editor-Frame bei 101k Entities kostet CPU 145 ms, davon `RenderExtractor::extract`
76,6 ms (§11.3). Ein geladener Ausschnitt von ~11k liegt in der 10k-Zeile (extract 6,4 ms).

## 2. Was fehlt, gemessen am Ziel „Dörfer, Wegpunkte, NPCs“

**L1. Zellen dürfen nur Unveränderliches enthalten.** `movableKey` (`CellSplit.cpp:25`) erlaubt
`transform, mesh, material, light, lod, collider, rigidbody, decal, inactive`; Richtungslicht und
dynamische Rigidbodies sind ausgenommen (`:54–59`). Ein Teilbaum geht nur dann in eine Zelle, wenn
jede seiner Entities nur diese Komponenten trägt. Skripte, Charaktere, Nav-Agents, Audio,
Partikel, Skelett-Meshes, Prefab-Instanzen bleiben in der Basis (153 §10.7: „Dynamische Bodies,
Skripte, Prefab-Instanzen und Kameras bleiben in der Basis“). Ein Dorf aus Prefab-Häusern mit NPCs
bleibt damit fast vollständig Basis, und genau die Basis ist es, die nicht skaliert.

**L2. Jede Zelle bekommt bei jedem Laden neue UUIDs.** `applyAdditiveJson` erzeugt alle Entities frisch
und stellt die gespeicherten UUIDs ausdrücklich nicht her (`SceneSerializer.cpp:2111–2118`, Kommentar:
„The stored UUIDs are deliberately NOT restored here“). Es gibt keinen Remap-Schritt für Verweise in
Komponenten, nur `rebuildHierarchy`. Folgen:
- Jeder Verweis per UUID zeigt auf eine UUID, die nach dem Laden nicht existiert: `JointComponent::target`
  (`PhysicsWorld.cpp:1614`), `RopeComponent::attachStart/End` (`RopeTrailSystem.cpp:79`),
  `CameraRigComponent::target` (`CameraRigController.cpp:65`), IK-`lookAt` (`PoseFinalize.cpp:393`),
  Sequencer-Bindungen (`SequenceEval.cpp:119`), die Bindungen einer `PrefabInstanceComponent`.
- `SaveStateComponent` setzt eine **szenenautorierte** UUID voraus (`SaveStateComponent.h:15`): der
  gespeicherte Zustand einer Entity in einer Zelle trifft nach dem Wiederladen nie wieder.
- Replikation adressiert über die UUID (`SpawnReplicator.cpp:260`).

Ein Mechanismus zum Wiederherstellen existiert schon, nur nicht auf diesem Pfad:
`applyPrefabJson(..., preserveIds, ...)` (`SceneSerializer.cpp:2257, 2285`), von der Zusammenarbeit
benutzt. 153 §11.5 hat das als „Verweise über Zellgrenzen im Spiel: außerhalb dieses Schritts“
liegen lassen. Das ist Schritt 3 hier.

**L3. `findByEntityId` ist linear.** `HorizonWorld.cpp:417` läuft über den ganzen Id-Pool („Linear over
the id pool: fine for load-time resolution and tests, not for a per-frame lookup“, `HorizonWorld.h`).
Die Aufrufer oben laufen teils pro Frame (Kamera-Rig, Seil, IK). Bei 100k+ Entities in der Basis ist
das ein O(N)-Zugriff pro Verweis und Frame. Zellen drücken N, aber sobald UUID-Verweise wirklich
auflösen, braucht es einen Index.

**L4. Nur die erste Kamera ist Anker.** `updateCellStreaming` nimmt die erste Entity mit
`TransformComponent`+`CameraComponent` (`GameApplication.cpp:1573–1578`) und `return` ohne Kamera.
Ein dedizierter Server hat keine Kamera, ein zweiter Spieler (Split-Screen, Mehrspieler) wird nicht
beachtet, und ein Teleport lädt erst nach dem Sprung, ohne dass jemand weiß, wann der Boden da ist.

**L5. Eine Zelle wird in einem Stück gebaut.** Das 4-ms-Budget wird nur **zwischen** Zellen geprüft
(`CellStreamer.cpp:308–311`, „one always“). Eine Zelle mit ~800 Entities überschreitet es: gemessen
bis 99 ms (153 §10.7). Kleinere Zellen helfen, kosten aber mehr Dateien und Manifest.

**L6. Jolt-Verdrahtung ist per Entity.** Der Hook ruft `addEntity` je Entity (`GameApplication.cpp:1538`).
Jeder Aufruf macht `removeEntityImpl` und danach `resolvePendingJoints` (`PhysicsWorld.cpp:1978–2015`).
Das Entladen ruft `removeEntity` (`requeueJoints=false`, `:2059`): ein Gelenk von einem Basis-Körper
zu einem Zellen-Körper geht beim Entladen **dauerhaft** verloren. Nichts hält Basis-Körper davon ab,
durch einen entladenen Boden zu fallen. Es gibt keine Abfrage der Body-Zahl gegen `kMaxBodies = 65 536`
(`PhysicsWorld.cpp:583`, nur eine Warnung ab 90 %, `:1971`).

**L7. Im Editor-Play streamt nichts.** `EditorApplication.cpp` enthält weder `CellStreamer` noch
`cellManifestJson` (Suche über `src/`, nur `GameApplication`, `HorizonWorld`, `SceneSerializer`,
`StreamingDebugView` treffen). Nach einem Split enthält die Editor-Welt nur die Basis; ein Play zeigt
dann vermutlich nur die Basis (nicht gelaufen, siehe Abschnitt 10). Die Zonen-Verdrahtung ist heute
schon doppelt vorhanden (`GameApplication.cpp:2044` und `EditorApplication.cpp:2796`); eine dritte
Kopie für Zellen wäre die falsche Antwort.

**L8. Kein „Struktur hat sich geändert“-Signal.** `markHierarchyDirty()` wird von Ladern und
Editor-Code von Hand gesetzt (`CellStreamer.cpp:338`, `SceneSerializer.cpp:2094, 2142, 2382`, 22
Stellen im Editor) und nur vom Outliner gelesen und gelöscht (`OutlinerPanel.cpp:597, 652`).
`createEntity`/`destroyEntity`/`reparentEntity` in `HorizonWorld.cpp` rufen es nicht auf. Unter
Thema 162 ist das entscheidend (Abschnitt 7).

**L9 (kein Fehler, aber zu wissen).** Assets einer entladenen Zelle bleiben im Speicher: kein
Eviction, kein LRU, `unloadAsset` nur manuell (153 §4.2, §10.7). Der Speicher wächst mit der
erkundeten Welt, nicht mit dem geladenen Ausschnitt. Das Thema löst das nicht (Abschnitt 9), muss es aber
in der Messung ausweisen.

## 3. Abgrenzung zu den Szenen-Zellen aus Thema 153 Schritt 6

Entscheidung D1: **Eine Zelldatei, zwei Verbraucher.**

| | Szenen-Zellen (153/6) | Laufzeit-Zellen (164) |
|---|---|---|
| Zweck | eine große Szene teilen und zurückführen, bleibt bearbeitbar | Entities nach Ankerabstand laden und entladen |
| Wer | Editor: *Split into Streaming Cells*, *Merge Cells into the Scene*, Undo | Spiel und Editor-Play: `CellStreamer` über `CellRuntime` |
| Code | `CellSplit.h/.cpp`, `StreamingDebugView.*`, `scripts/split_scene_cells.py` | `CellStreamer.h/.cpp`, `CellRuntime.*` (neu), Hooks in `GameApplication`/`EditorApplication` |
| Datei | `<Szene>.cells/cell_<x>_<z>.hescene` + Objekt `cells` in der Basisszene | **dieselben Dateien**; Kopf `streaming` je Zelle, Spalte `bodies` im Manifest |
| Wer entscheidet, was in eine Zelle darf | `CellSplit.cpp` (`movableKey`, `movableEntity`) | **niemand**: die Laufzeit vertraut der Datei. Sie liest nur den Kopf |
| Lebensdauer der Entities | Merge baut alles wieder in die Szene, UUIDs bleiben | Entladen zerstört, Laden baut neu **mit denselben UUIDs** |

Was Schritt 6 unverändert bleibt: Split/Merge, `cellFolders`, Undo, die Sperre in Collab-Sitzungen,
die Zellansicht. Neu ist nur, dass der Splitter eine **Regeltabelle mit Klassen** statt einer
Ja/Nein-Liste hat (4.4) und den Kopf schreibt.

Warum kein eigenes Laufzeitformat (Alternative verworfen): Der Lader (`loadAdditiveFromJson`), die Pak-Pfade
(`detachedMountedEntryReader`, CBOR), Merge, Debug-Ansicht und alle Tests hängen an der Zelldatei als
`.hescene`. Ein Zweitformat bräuchte einen Konverter, einen zweiten Parser und eine Regel, wann welches
gilt. Der Export packt Zellen schon als Szenen in die `.hpak`. Gebackene Beschleunigung (Asset-Liste,
Bounds) kann später als optionales Feld im Kopf folgen, ohne dass die Laufzeit sie braucht.

Das Python-Skript `scripts/split_scene_cells.py` wird **nicht erweitert**. Es schreibt weiter
Version-1-Zellen (kein Kopf), die die Laufzeit unverändert liest (4.2). Die C++-Fassung ist die
Referenz; im Skript-Kopf steht ein Hinweis darauf. Das Dublettenproblem aus 153 §11.5 („Zwei Splitter“)
wird so nicht größer.

## 4. Zellformat v2

### 4.1 Manifest (Objekt `cells` der Basisszene)

Rückwärtskompatibel: ein v1-Manifest parst unverändert (`CellManifest::parse`, `.cpp:22`).

```json
"cells": {
  "version": 2,
  "cellSize": 256, "loadRadius": 384, "unloadRadius": 480, "lookaheadSec": 2.0,
  "dir": "Content/Village.cells",
  "list": [[0, 0, 812, 640], [0, 1, 790, 590], [-1, 0, 120, 4]]
}
```

- `version` fehlt = 1.
- Eintrag `[x, z, entities, bodies]`: `bodies` ist neu (Zahl der Entities mit Rigidbody/Collider).
  `parse` liest heute `c.size() > 2`; ein viertes Element anzuhängen bricht kein altes Manifest.
  Der Streamer nutzt `bodies`, um ein Laden zu **verschieben**, wenn die Body-Reserve nicht reicht (6).
- Das Gitter ist **nur x/z** (`CellManifest::cellIndex`, `distanceTo`). Vertikale Welten (Höhlen unter
  Dörfern, Hochhäuser) sind nicht Thema; eine Zelle ist eine Säule.

### 4.2 Kopf der Zelldatei

Ein Objekt `streaming` auf oberster Ebene der Zelldatei, neben `entities` und `cellFolders`:

```json
"streaming": { "version": 2, "cell": [0, 0], "bodies": 640, "classes": { "static": 812, "stateful": 0 } }
```

- Fehlt der Kopf, ist es eine v1-Zelle: alles gilt als Zelle-statisch, UUIDs werden **nicht** wiederhergestellt
  (altes Verhalten, damit ein altes Projekt unverändert läuft).
- `version >= 2` heißt: die Datei ist von der C++-Splittung geschrieben, UUIDs sind stabil und eindeutig
  über die ganze Szene, die Ref-Hülle (5) ist eingehalten.
- Bewusst **nicht** im Kopf: Asset-Liste und Bounds. Eine Asset-Liste im JSON zu errechnen hieße,
  das Wissen aus `SceneSystems::collectAssetRefs` zu duplizieren (Driftgefahr); Bounds brauchen
  Mesh-AABBs, die in den Assets liegen. Beides nur nachrüsten, wenn Schritt 4 zeigt, dass die Asset-Latenz
  stört (7.5). Der Pop-in-Regler bleibt `loadRadius` (≥ Sichtweite + halbe größte Objektausdehnung,
  eine Einstellung, keine Berechnung; die Zellansicht zeigt den Radius).

### 4.3 Lade-Semantik

- `loadAdditiveFromJson` bekommt Optionen (`struct AdditiveOptions { bool preserveIds = false; }`).
  `applyAdditiveJson` (`:2101`) setzt bei `preserveIds` im Pass 1 die UUID wie `applyPrefabJson` (`:2285`).
  `CellStreamer` setzt sie für `streaming.version >= 2`.
- **Kollision:** existiert die UUID schon in der Welt (kopierte Zelldatei, Projekt mit zwei
  Szenen-Kopien), wird für **diese** Entity eine frische vergeben und eine Warnung geloggt;
  `Stats::idCollisions` zählt mit. Für die Prüfung braucht es den Index aus 5.1, sonst wäre das
  O(N) je Entity.
- Die Zellwurzel (`Cell x,z`, direktes Kind der Weltwurzel) bleibt, ihre Position bleibt `-origin`.
  Alle Entities der Zelle sind Nachfahren dieser einen Wurzel. Das ist die Einheit für Entladen und, in
  Abschnitt 7, für einen späteren Teilbaum-Zähler.

### 4.4 Streaming-Klassen und Komponenten

Die Tabelle ist die einzige Quelle (`CellSplit.cpp`, ersetzt `movableKey`). Ein Teilbaum geht nur in
eine Zelle, wenn **jede** Entity darin zu einer Zellen-Klasse gehört; eine unbekannte Komponente macht
ihn Resident (Whitelist-Prinzip bleibt).

| Klasse | Heißt | Komponenten | Beim Entladen | Schritt |
|---|---|---|---|---|
| **Zelle-statisch** | Platziertes ohne Zustand, der ein Entladen überleben muss | heute: `transform, mesh, material, lod, collider, rigidbody (statisch), decal, inactive, light (Punkt/Spot)`. Neu: `audioSource, particleSystem, skeletalMesh` + `animator*`, `propertyAnimator`, **`prefabInstance`** | Entity zerstören, Bodies entfernen | 2 (Tabelle), 3 (Prüfung je Komponente) |
| **Zelle-zustandsbehaftet** | hat Zustand und/oder Skript | `script` (Lua/Python/HorizonCode-Entity-Klasse), `saveState`, `navAgent`, `characterController`+`movement` (NPC); die Verweis-Komponenten (`joint`, `rope`, `cameraRig`, `ik`, `sequencePlayer`) nur innerhalb einer Ref-Hülle | Zustand über `SaveState` herausschreiben (`CellState`), dann zerstören | 3 |
| **Resident** | muss immer da sein | `camera`, `cameraRig`, `audioListener`, Richtungslicht, Terrain/Terrain-Chunk, Himmel/Wetter/Umgebung, `network`, `replicatedVars`, dynamische `rigidbody`, Spieler, alles mit unbekannter Komponente, alles was ein Autor als Resident markiert | bleibt | – |

Zur Zeile *Zelle-statisch, neu*: Das sind dekorative Komponenten ohne Verweis. Nach dem Laden
starten ihre Systeme neu (Animationsphase, Partikel, Audio `playOnStart`); das ist gewollt und wird
in `test_cell_split` je Komponente belegt, nicht angenommen. **Prefab-Instanzen** sind der Hebel für
„Dörfer“ (Häuser sind Prefabs). Ihre `PrefabInstanceComponent::bindings` zeigen per UUID auf Entities
des eigenen Teilbaums; das trägt nur mit `preserveIds` (L2), und deshalb sind Prefab-Instanzen erst
ab D2 zellenfähig.

Wegpunkte: Eine Entity nur mit `transform` (und Name) ist heute schon „movable“. Sucht ein Skript
oder ein NPC einen Wegpunkt per Name oder UUID, während dessen Zelle entladen ist, bekommt es `null`.
Bis es eine datenbasierte Darstellung gibt (Abschnitt 9), gilt: **Wegpunkte, auf die Basis-Logik
zugreift, sind Resident.** Der Autor setzt das mit dem Schalter *Streaming ▸ Resident* (4.7).

### 4.5 Anker

`CellStreamer::update(world, anchors, budgetMs)`, `anchors` ist ein Vektor aus
`{ glm::dvec3 position; glm::vec3 velocity; float radiusScale = 1; }`. Gesucht wird um jeden Anker
(`scan`, `CellStreamer.cpp:209`, läuft heute für Kamera und Vorausschau). Eine Zelle ist gewollt,
wenn irgendein Anker sie wünscht, und wird erst entladen, wenn **alle** Anker jenseits des Radius sind.
Quellen:
- Hauptkamera (wie heute, aus `GameApplication.cpp:1573`),
- jeder Spieler-Charakter (Server und Split-Screen); auf dem dedizierten Server ist das der einzige Anker,
- **Skript-Pins:** `streaming.pin(position, radius)` → Handle, `streaming.unpin(handle)`. Brauchen Teleport,
  Zwischensequenz mit Kamera woanders, Quest-Orte.

`CellStreamer::isSettled(position, radius)`: wahr, wenn alle Zellen im Radius gebaut sind **und** ihre
Bodies im Jolt-Welt stehen. Ein Teleport wartet darauf, bevor er den Spieler freigibt; sonst fällt er durch
den Boden einer Zelle, die erst im Aufbau ist.

### 4.6 Gestückelter Aufbau

Der Worker teilt die geparste Zelle (er hat sie ohnehin in der Hand, `CellStreamer.cpp:264–272`) in
**Gruppen**: je ein oder mehrere Teilbäume unter der Zellwurzel, bis eine Obergrenze Entities (Start 128,
Einstellung) erreicht ist. Jede Gruppe ist ein eigenes kleines Szenen-JSON, dessen Wurzeleintrag an die
(schon gebaute) Zellwurzel gehängt wird. Der Hauptthread baut Gruppen, solange das Budget reicht
(`budgetMs`, mindestens eine). Daraus folgt:
- Das Budget gilt je **Gruppe**, nicht je Zelle: der Frame-Ausreißer ist die größte Gruppe, nicht die größte Zelle.
- Es gibt nie ein halbes Objekt: Teilbäume sind unteilbar, Verweise liegen innerhalb eines Teilbaums
  oder eines Clusters (5), und ein Cluster ist immer **eine** Gruppe.
- Hooks: `loadedSlice(root, created)` je Gruppe (Jolt, Asset-Streaming), `loaded(root, all)` einmal nach der
  letzten (Skripte starten erst jetzt, damit ihr `BeginPlay` die ganze Zelle sieht, wie bei Zonen
  `GameApplication.cpp:2017`).
- Eine Zelle gilt als geladen, sobald die letzte Gruppe steht; `isSettled` fragt darauf.

**Offen, Schritt 2 klärt es als Erstes: wie eine Gruppe an die Zellwurzel kommt.** `rebuildHierarchy`
(`SceneSerializer.cpp:1980`) verbindet nur Einträge, die in der `idMap` **desselben** Aufrufs stehen. Eine
Gruppe kann die Zellwurzel (aus einem früheren Aufruf) deshalb nicht als `parent` benennen, und
`createEntity` hängt jede neue Entity an die Weltwurzel. Zwei Wege:
- **(a) `applyAdditiveJson` je Gruppe, danach die Spitzen umhängen:** die obersten Entities jeder Gruppe
  werden aus den `children` der Weltwurzel genommen und an die Zellwurzel gehängt (dieselben zwei
  Zeilen wie in `rebuildHierarchy`: `children.push_back`, `parent =`). Ob `reparentEntity` dabei die
  lokalen Werte behält, ist nicht geprüft; die Zellwurzel steht auf `-origin`, die lokalen Positionen
  der Spitzen sind absolut, ein Umhängen mit Weltwert-Erhalt wäre falsch.
- **(b) `applyPrefabJson(world, scene, prefabParent = Zellwurzel, preserveIds = true)`** (`:2256`) hat beides schon:
  das Elternteil und die UUIDs. Es verlangt genau **einen** Eintrag ohne Elternteil (`prefabRoot`). Das
  passt, wenn eine Gruppe **ein** oberster Teilbaum ist (ein Haus, ein Cluster mit einer Spitze). Mehrere
  Spitzen bräuchten je einen Aufruf, und ein Cluster aus mehreren Spitzen müsste im selben
  Budgetschritt gebaut werden. Ein künstliches Zwischen-Root wäre falsch (es ändert die Hierarchie gegenüber
  der Datei und damit den Merge). Zu klären: was `applyPrefabJson` über das Anlegen hinaus tut (Prefab-Bindungen,
  `ensureEnvironmentLights` fehlt dort, `:2257–2380`).
Entscheidung im Schritt, mit einem Test, der Hierarchie, Reihenfolge der Kinder und lokale Positionen gegen
das ungestückelte Laden vergleicht.

### 4.7 Autorenschalter

Ein Schalter je oberster Entity: *Streaming: Auto / Resident*. Auto = der Splitter entscheidet nach 4.4;
Resident = bleibt in der Basis. Die Daten brauchen einen Ort: eine kleine Komponente `StreamingComponent`
(`enum Mode`), weil Inspector und Splitter sie lesen. Das ist eine neue ECS-Komponente mit
sechs Registrierstellen (Memory `new-component-registration-places`: `SceneSerializer` Speichern+Laden,
`HE_SCENE_COMPONENT_KEYS`, `isKnownComponentKey`, `InspectorPrefabKeys.cpp`, `kComponentScopes` in
`EditorHelp.cpp`, `HorizonScene.h` + Add-Component-Zeile + Hilfe-Einträge). Ob es ein vorhandenes Tag
oder einen Layer gibt, den man stattdessen nehmen könnte, ist **nicht geprüft**; Schritt 3 prüft das zuerst und
nimmt die Komponente nur, wenn nichts passt.

## 5. Verweise über Zellgrenzen

Regeln (D4):

| # | Regel |
|---|---|
| R1 | **Hierarchie** kreuzt nie eine Grenze. Der Splitter nimmt ganze Teilbäume; die Zellwurzel hängt an der Weltwurzel. Bleibt wie heute |
| R2 | **UUID-Verweise** (Gelenk, Seil, Kamera-Rig, IK, Sequencer, Prefab-Bindungen, Skript-`Ref`-Variablen) gehen nur **innerhalb desselben Clusters**. Der Splitter bildet Cluster per Union-Find über die Verweisfelder (Liste unten) und schiebt einen Cluster nur als Ganzes in **eine** Zelle (die des ersten Mitglieds). Hat ein Cluster Mitglieder in verschiedenen Klassen oder zeigt ein Verweis aus der Basis hinein, **bleibt der ganze Cluster Resident** |
| R3 | Zur **Laufzeit** ist ein Verweis auf eine entladene Entity `entt::null` (`findByEntityId` liefert null), wie bei einer zerstörten. Alle Verbraucher tolerieren das heute schon (Seil, Kamera-Rig-Fallback, Gelenk-Warteliste, Sequencer-Slot) und bekommen in Schritt 3 je einen Test |
| R4 | **Skripte** erfahren Zellen über die API (4.5): `streaming.cellAt`, `streaming.isLoaded`, `streaming.isSettled`, `streaming.pin/unpin`. Eine UUID in einer Skriptvariable bleibt über ein Entladen nicht gültig; ein Skript, das eine Entity außerhalb seiner Zelle braucht, pinnt oder der Autor setzt sie Resident |
| R5 | **Zustand** einer zustandsbehafteten Entity überlebt das Entladen über `CellState` (6.3), geschlüsselt mit der stabilen UUID. Das geht nur mit D2 |

Verweisfelder, wie im Code gefunden (Liste für `CellSplit.cpp`; **keine Reflexion vorhanden**, die Liste
ist von Hand und muss bei einer neuen Komponente mitwachsen, siehe Risiko in 10):
`JointComponent::target`, `RopeComponent::attachStart/attachEnd`, `CameraRigComponent::target`,
`IkComponent` (`lookAt.targetEntityId`), `SequencePlayerComponent` (Bindungen/Slots/Overrides),
`PrefabInstanceComponent::bindings` (`instanceEntity`, `templateEntity` sind **innerhalb** des Teilbaums),
`CharacterControllerComponent`/`NavAgentComponent` Ziele (nur zu prüfen, ob Entity-Ziele).
Eine Komponente, die nicht in der Tabelle 4.4 steht, macht ihren Teilbaum ohnehin Resident.

### 5.1 UUID-Index in `HorizonWorld`

`findByEntityId` (`HorizonWorld.cpp:417`) wird O(1) über `std::unordered_map<HE::UUID, Entity>`.
**Gepflegt über entt-Signale** (`on_construct/on_update/on_destroy` von `EntityIdComponent`), nicht
über die Aufrufer: die UUID wird an fünf Stellen direkt geschrieben, ohne `setEntityId`
(`HorizonWorld.cpp:30, 402, 430`, `SceneSerializer.cpp:164, 2286`, `CollabController.cpp:2283`). Ein Index, der nur
`createEntity` und `setEntityId` kennt, wäre still falsch. Ein Fallstrick bei den Signalen: `on_update`
(`emplace_or_replace` auf eine vorhandene Komponente) feuert **nach** dem Schreiben und liefert nur den
neuen Wert, die alte UUID ist dann weg. Der Index braucht deshalb eine Rückabbildung Entity → UUID, über die
der alte Schlüssel gelöscht wird (oder die Schreiber rufen vorher `patch`/`replace` an einer Stelle auf).

### 5.2 Gründe gegen globale Verweise zu entladenen Entities

Ein Manifest-Index „UUID → Zelle“ (wo ist Entity X?) würde jedem Verweis erlauben, die Zelle zu
laden, in der sein Ziel liegt. Das macht aus dem Streaming ein Abhängigkeitsproblem (Zelle A zieht
Zelle B, die zieht A) und verschiebt die Last vom Autor zur Laufzeit. Der Plan wählt den Weg „der
Splitter sorgt dafür, dass es nie nötig ist“. Was das ändern würde: Gameplay, das per Namen oder UUID quer über die Welt
adressiert (Questziele, Funkverkehr). Dafür gäbe es dann eine **Marker-Tabelle** im Manifest
(Name → Position, Zelle) für ausgewählte Entities, keinen vollen Index (Abschnitt 9).

## 6. Jolt

### 6.1 Aufbau

- Der Hook baut nicht mehr `addEntity` je Entity, sondern **eine Charge je Gruppe**:
  `PhysicsWorld::addEntities(world, span)` baut die Bodies, gibt sie über Jolts
  `AddBodiesPrepare/AddBodiesFinalize` an das Broadphase-System (der Jolt-Header empfiehlt das ausdrücklich für
  mehrere Bodies, `BodyInterface.h:96`) und ruft `resolvePendingJoints` **einmal** (heute je Entity,
  `PhysicsWorld.cpp:2015`).
- `OptimizeBroadPhase()` nach einer Charge: **offene Messung**, keine Entscheidung. Es baut den ganzen
  Baum neu (Kommentar `PhysicsWorld.cpp:2018`); ob einmal pro großer Zelle besser ist als gar nicht, misst
  Schritt 4.
- **Body-Reserve:** `PhysicsWorld::bodyCount()` (neu, heute nur intern `entityToBody.size()`) gegen
  `kMaxBodies`. Der Streamer **verschiebt** ein Laden, wenn `bodyCount + cell.bodies` über 90 % läge, und
  loggt es; er überspringt keine Zelle. Das Limit bleibt hart; dass ein Mensch es sieht, ist Absicht (Profiler-Tab).

### 6.2 Entladen

- Bodies einer Zelle werden **vor** dem Zerstören in einem eigenen Durchgang entfernt (wie bei Zonen,
  `GameApplication.cpp:2044` Kommentar), mit `requeueJoints = true` für Gelenke, deren **Besitzer in der Basis**
  liegt. Beim Wiederladen löst `resolvePendingJoints` sie wieder auf (R2 verhindert das meistens schon; das ist
  das Netz darunter).
- **Dynamische Basis-Körper über entladenem Boden:** Dynamische Rigidbodies und Charaktere bleiben Resident
  (D3). Das Gelände ist Terrain (Basis, immer geladen); die Gefahr liegt bei Zellen-Collidern (Böden von
  Gebäuden, Brücken). Regel: ein dynamischer Körper außerhalb von `unloadRadius` **aller** Anker wird aus der
  Simulation genommen (Jolt `RemoveBody`, Body-Objekt und Zustand bleiben) und beim Betreten des
  `loadRadius` wieder eingefügt, **nachdem** `isSettled` wahr ist. Mechanismus: `PhysicsWorld::setRegionHold`
  (neu). Alternative (`SetMotionType(Static)` und zurück) wird in Schritt 3 gegen diese gemessen; die
  Anforderung steht fest, der Mechanismus nicht.
- Charaktere (`CharacterVirtual`) haben keinen Jolt-Body (`PhysicsWorld.cpp:2073` Kommentar); ihr Halt
  ist deshalb die Bedingung „der Spieler ist Anker, seine Zellen sind `settled`“, nicht `RemoveBody`.

### 6.3 `CellState` (Zustand zustandsbehafteter Zellen)

`CellState` (neu, `HE_Scene`) hält je Zelle `{ tombstones: [uuid], states: { uuid: json } }`.
- **Beim Entladen** (`unloading`, vor `destroyEntity`): für jede Entity mit `SaveStateComponent` die
  markierten Attribute herausschreiben, mit demselben Code wie `entity.saveState` (`EngineApi.cpp`,
  Transform, Sichtbarkeit, `Save Game`-Skriptvariablen). Dazu die Namen der Entities, die ein Skript
  per `entity.destroy` entfernt hat (Tombstones; sonst kommt die Kiste nach dem Wiederladen zurück).
- **Nach dem Laden** (`loaded`, vor dem Start der Skripte): Tombstones zerstören, Zustände anwenden
  (`entity.applySavedState`-Logik).
- Der Speicher ist **In-Memory je Sitzung** in Schritt 3. Ins Savegame zu schreiben ist eine Zeile in dessen
  Abschnitt, **wenn** das Savegame eine Erweiterungsstelle hat (nicht geprüft, 10); sonst Folgearbeit. Das
  ist die Grenze, die der Mensch kennen muss: ohne diese Zeile bleibt eine geöffnete Tür nur bis zum Spielende offen.
- Lua- und Python-Zustand wird nicht erfasst (`SaveStateComponent.h`: die Backends können nicht
  zurücklesen); HorizonCode-`Save Game`-Variablen schon.

### 6.4 Netz

Entities mit `NetworkComponent`/`ReplicatedVars` sind Resident (D3), also gibt es keine replizierten
Entities in Zellen. Server und Client laden Zellen unabhängig nach ihren Ankern; mit `preserveIds` haben
gleiche Zellen gleiche UUIDs auf beiden Seiten. Was **nicht** gebaut wird: Replikation von Zellinhalt.

## 7. Zusammenspiel mit dem Extract einmal pro Frame (Thema 162)

**Zellen machen die Arbeit pro Frame kleiner (N sinkt), Thema 162 macht sie billiger (pro Entity).** Beide
Hebel sind unabhängig und ersetzen sich nicht. Entladene Zellen sind keine Entities im Registry: sie fallen aus
`RenderExtractor::extract`, `propagateTransforms`, `FrustumCuller::cull`, `LODSystem::update` und den
Systemen heraus, ohne dass 162 etwas dafür tut. Die Zellen **sind** die grobe räumliche Vorfilterung, die
153 §5 Punkt 1 als dritten Hebel nennt („vor dem Sammeln räumlich vorfiltern“); 162 muss dafür keine zweite bauen.

### 7.1 Der Vertrag

Zellen laden und entladen **nur im Update des Frames**, nach Floating Origin, vor den Systemen und vor dem
Rendern: `GameApplication.cpp:3203–3212`, `updateFloatingOrigin` dann `updateCellStreaming`. Nie zwischen zwei
`extract()`-Aufrufen eines `FrameScope` (`RenderExtractor.h:131–163`). Das ist dieselbe Regel, die
`FloatingOrigin.h:40–41` für den Ursprungswechsel nennt. Heute ist sie nur ein Vertrag: `FrameKey`
(`RenderExtractor.cpp:1242`) enthält den `HorizonWorld`-Zeiger und Einstellungen, aber **keine
Versionszahl**. Ein Zellwechsel zwischen zwei Extrakten würde also still aus der alten Kopie bedient.

### 7.2 Koordinationspunkt: `HorizonWorld::structureEpoch`

Der Plan von 162 (Weg A, Abschnitt 5: „ein Änderungs-Zähler auf der Registry oder ein globales
Dirty-Bit“) und dieser Plan brauchen **dasselbe**. Vorschlag: Thema 164 baut in Schritt 2 genau einen
`uint64_t HorizonWorld::structureEpoch()`; Thema 162 liest ihn in `FrameKey` (dann wird der Vertrag
aus 7.1 erzwungen: ändert sich die Epoche zwischen zwei `extract()` im Scope, wird nicht
wiederverwendet) und im behaltenen `RenderWorld`.
- Hochgezählt in `HorizonWorld::createEntity`, `destroyRecursive`/`destroyEntity`, `reparentEntity`, `clear`
  und in dem direkten `m_registry.destroy` bei `HorizonWorld.cpp:498`. **Nicht** über `markHierarchyDirty`
  (L8): das ist von Hand gesetzt und wird nur vom Outliner gelöscht.
- Er deckt die **Menge der Entities und ihre Eltern**. Er deckt **nicht** Komponentenwerte und nicht
  das Hinzufügen oder Entfernen einer `MeshComponent` an einer lebenden Entity (ein Skript kann das). Ein
  behaltener `RenderWorld` braucht dafür eigene Änderungserkennung; das ist 162, nicht 164.
- **Eigentümer:** wer zuerst landet. Der andere liest nur. Dieser Plan wird dem Bearbeiter von 162
  (`render-extractor-u-1`) und der Königin genannt, damit nicht zwei Zähler entstehen.

### 7.3 Granularität für einen behaltenen RenderWorld (162 Weg A)

Weil jede Zelle **eine Wurzel** hat (4.3), ist Laden/Entladen ein „Teilbaum hinzugefügt/entfernt“, nicht
Tausende Einzelereignisse. Mit der gestückelten Bauweise (4.6) kommt der Teilbaum über mehrere Frames; jede Gruppe ist selbst ein
vollständiger Teilbaum, es gibt nie halbe Objekte. Die Weltwurzel hat nur so viele Kinder wie geladene Zellen (statt
100k): das hilft `propagateFrom` (`TransformHierarchy.cpp:39`, Abstieg über `children`) und dem Outliner.
Weg B von 162 (alle Pässe lesen `m_renderWorld`, ein Extract pro Frame) braucht von 164 nichts.

### 7.4 Ursprungswechsel

`shiftWorldOrigin` verschiebt die Zellwurzeln mit. Unter 162 Schritt 3 (Vergleich von `parentWorld` mit einem
gespeicherten Wert) heißt das: **ein** voller Durchlauf je Verschiebung über alle geladenen Zellen. Das ist
selten (gemessen 2,2 ms bei 50k, 153 §10.5) und akzeptabel; kein Sonderpfad nötig.

### 7.5 Assets und Extract

Ist ein Mesh noch nicht resident, lässt der Extraktor die Bounds ungültig („never culled“,
`RenderExtractor.h`) und das Backend löst es beim ersten Zeichnen auf. Eine frisch gebaute Zelle zeigt
ihre Dinge deshalb, wenn die Assets eintreffen, nicht beim Aufbau. Der Start der Asset-Loads liegt heute im
`loaded`-Hook nach dem Bau (`GameApplication.cpp:1545`); mit Gruppen (4.6) beginnt er je Gruppe früher und
überlappt mit dem Rest des Aufbaus. Ob das reicht oder die Zelle erst sichtbar werden soll, wenn ihre
Assets da sind, misst Schritt 4 (Pop-in), und nur dann kommt die Asset-Liste in den Kopf (4.2).

### 7.6 Was Thema 164 in Schritt 2–3 nicht anfasst

`RenderExtractor.*`, `FrustumCuller`, alle fünf Backends, `propagateTransforms`. Der Editor extrahiert weiter
seine ganze Szene (Pick-Snapshot, Snap-Drag, zweites Scene-Fenster, 153 §11.5): er profitiert von 162, nicht
von 164. Das Editor-Play profitiert von beidem (D7).

## 8. Bauplan

Der Titel von Schritt 2 in der Hive-Planung stimmt nicht mehr: „Laden/Entladen zur Laufzeit umsetzen“ ist
Thema 153/5. **Vorschlag: Schritt 2 umbenennen** in „Laufzeit-Zellen für Entities ausbauen: stabile
Identität, Anker, gestückelter Aufbau, gemeinsamer Host für Spiel und Play“. Schritt 4 (Verifikation) passt.

**Schritte 2 und 3 sind zu groß für je eine Sitzung. Vorschlag für den Schnitt, nach dem, was „Dörfer“
freischaltet (L1 + L2), zuerst:**

| Teil | Inhalt | Warum an dieser Stelle |
|---|---|---|
| **2a Kern** | UUID-Index mit Rückabbildung (5.1); `preserveIds` auf dem Zellpfad samt Kollisionsprüfung (4.3); `prefabInstance` (und die dekorativen Komponenten) in der Klassentabelle (4.4); Manifest-Spalte `bodies` und Kopf (4.1, 4.2); der Lauf zu L7 vorab. Test: Prefab-Haus lädt, entlädt, lädt wieder mit denselben UUIDs und intakten Bindungen | ohne stabile Identität trägt nichts anderes; kleinster Eingriff mit der größten Wirkung |
| **2b zweite Reihe** | Anker-Liste (4.5), gestückelter Aufbau (4.6, mit der Anhänge-Frage), `structureEpoch` (7.2, klein und unabhängig: kann jeder zuerst bauen, 162 oder 164), Zellansicht | macht das Streaming robust, schaltet aber keine neuen Inhalte frei |
| **2c dritte Reihe** | `CellRuntime`/`ICellHost`, Streaming im Editor-Play | reines Umsortieren plus eine Produktentscheidung (Play); darf hinter 3a/3b rutschen, wenn die Warnung unter der Tabelle reicht |
| **3a Physik und Ref-Hülle** | `addEntities`-Charge, `bodyCount`, `requeueJoints` beim Entladen, `setRegionHold` (6); Ref-Hülle im Splitter (5) | trägt die Sicherheit: nichts fällt durch den Boden, kein Gelenk geht verloren |
| **3b Zustand und Skripte** | `CellState` (6.3), Skriptstart und -abbau pro Zelle, Gruppe `streaming` in der API (Pins, `isSettled`), `StreamingComponent`, Spieler als Anker auf dem Server | braucht 2a und 3a; erst jetzt dürfen NPCs in Zellen |

Der Königin zur Entscheidung: ob 2c vor oder nach 3 kommt. Gegen 2c vor 3 spricht, dass Play ohne Zellen dann bis
dahin eine **sichtbare Warnung** („Play zeigt nur die Basis“) braucht; die kostet eine Zeile und steht in 2a.

### Schritt 2: Identität, Anker, gestückelter Aufbau, Host

| Datei | Änderung |
|---|---|
| `src/HE_Scene/include/HorizonScene/HorizonWorld.h`, `src/HE_Scene/src/HorizonWorld.cpp` | UUID-Index über entt-Signale (5.1), `findByEntityId` O(1); `structureEpoch()` (7.2) |
| `src/HE_Scene/include/HorizonScene/SceneSerializer.h`, `src/HE_Scene/src/SceneSerializer.cpp` | `AdditiveOptions { preserveIds }` für `loadAdditiveFromJson` und `applyAdditiveJson` (`:2101`, `:2802`); Kollisionsbehandlung (4.3) |
| `src/HE_Scene/include/HorizonScene/CellStreamer.h`, `src/HE_Scene/src/CellStreamer.cpp` | Manifest v2 (`version`, `bodies`); Kopf lesen; `update(world, anchors, budgetMs)`; Worker-Gruppierung und gestückelter Aufbau (4.6); Hooks `loadedSlice`/`loaded`/`unloading`; `isSettled`, Pins; Stats `idCollisions`, `slices`, `deferredForBodies` |
| `src/HE_Scene/include/HorizonScene/CellSplit.h`, `src/HE_Scene/src/CellSplit.cpp` | Klassentabelle statt `movableKey` (4.4, Stufe „Zelle-statisch, neu“); Kopf `streaming` schreiben; Manifest-Spalte `bodies`; `mergeCellsIntoWorld` liest den Kopf nicht, aber verliert nichts (Test) |
| `src/HE_Scene/include/HorizonScene/CellRuntime.h`, `src/HE_Scene/src/CellRuntime.cpp` (neu), `src/HE_Scene/CMakeLists.txt` | `CellRuntime` besitzt den `CellStreamer`, den Reader und die Ankerquellen und spricht mit der Anwendung über ein kleines Interface `ICellHost` (Bodies hinzufügen/entfernen, Assets streamen, Skripte starten/stoppen). Löst die Lambdas aus `GameApplication.cpp:1514–1566` ab |
| `src/HE_Game/src/GameApplication.cpp/.h` | `updateCellStreaming` (`:1491`) wird dünn: Anker einsammeln (Kamera + Spieler), `CellRuntime::update`; `ICellHost` implementieren; Zonen-Pfade bleiben unberührt |
| `src/HE_Editor/EditorApplication.cpp` | Play: eigenes `CellRuntime` aus `m_editorWorld->cellManifestJson()` beim Start, **vor** der Wiederherstellung der Welt beim Stop `clear()`; Floating Origin gibt es dort nicht (153 §10.7), die Welt ist absolut. Fallback, wenn das zu groß wird: bei nicht-leerem Manifest eine sichtbare Warnung „Play zeigt nur die Basis“ und der Rest als Folgethema |
| `src/HE_Editor/StreamingDebugView.cpp` | Anker, Gruppen, `idCollisions`, `settled` im Tab; Overlay zeichnet Anker |
| `tests/test_world_scale.cpp` | bestehende Zellfälle (Laden, Entladen, Hysterese, Vorausschau, Origin) laufen weiter; neu: stabile UUIDs über Entladen/Laden, Kollision, Gruppen halten das Budget (Negativkontrolle mit ganzer Zelle: rot), mehrere Anker, Entladen erst wenn alle jenseits |
| `tests/test_cell_split.cpp` | Klassentabelle je Komponente, Kopf, Manifest `bodies`, Merge-Rundweg mit neuen Klassen |
| `tests/test_cell_runtime.cpp` (neu), `tests/CMakeLists.txt` | `CellRuntime` gegen einen Fake-`ICellHost`; PIE-Aufruf und -Abbau |
| `tests/test_scene_serializer.cpp` (oder `test_world_identity.cpp`) | Index nach `emplace` an den fünf direkten Schreibstellen; `preserveIds` auf dem additiven Pfad |

Abnahme Schritt 2: Alle bestehenden Zelltests grün; eine Zelle mit einem Prefab-Haus lädt, entlädt und lädt
wieder mit **denselben** UUIDs und intakten `PrefabInstance`-Bindungen; Bench `'Cell streaming bench*'` zeigt
den schlechtesten Hauptthread-Frame unter dem Budget plus der größten Gruppe (Ziel < 8 ms statt 10–99 ms);
Play eines gesplitteten Projekts zeigt die Zellen (Bild-Zeuge oder `HE_DUMP`-Zeuge, nicht nur ein Zähler).

### Schritt 3: Physik, Verweise, Zustand

| Datei | Änderung |
|---|---|
| `src/HE_Scene/include/HorizonScene/PhysicsWorld.h`, `src/HE_Scene/src/PhysicsWorld.cpp` | `addEntities(world, span)` mit `AddBodiesPrepare/Finalize` und **einem** `resolvePendingJoints` (`:1978–2015`); `bodyCount()`; `removeEntityTree(..., requeueJoints)` (`:2080`); `setRegionHold`; Messung `OptimizeBroadPhase` (6.1) |
| `src/HE_Scene/src/CellSplit.cpp` | Ref-Hülle per Union-Find über die Verweisfelder (5); Klasse „Zelle-zustandsbehaftet“; Autorenschalter beachten |
| `src/HE_Scene/include/HorizonScene/CellState.h`, `src/HE_Scene/src/CellState.cpp` (neu) | Zustand und Tombstones (6.3); eingehängt in die `unloading`/`loaded`-Hooks |
| `src/HE_Game/src/GameApplication.cpp` | `ICellHost`: Skripte am Zellenende starten (`startScriptsFor(created)`, `:2087`), beim Entladen wie in `UnloadZone` (`:2065–2067`: `m_scriptInstances.erase`, `unwatch(scriptToken)`, `ScriptApi::destroy`); Body-Reserve; Teleport-Gate |
| `src/HE_Editor/EditorApplication.cpp` | gleicher Host für das Play; Skript-Teardown dort (die Zonenvariante dort, `:2796–2815`, ruft kein `unwatch` und löscht keine `m_scriptInstances`, bei der Übernahme abgleichen) |
| `src/HE_Scene/include/HorizonScene/EngineApi.h`, `src/HE_Scene/src/EngineApi.cpp`, `src/HE_Editor/HcNodeDocs.cpp` | neue Gruppe `streaming` mit `cellAt`, `isLoaded`, `isSettled`, `pin`, `unpin`: **drei bis vier Stellen je Row** (Display-Name-Map, `HcNodeDocs`-Beschreibung, `isScriptGroup` für die neue Gruppe, bei C++-GameLogic-Schnittstelle zusätzlich Services-Tabelle); danach voller `ctest`, nicht nur `-tc` |
| `src/HE_Scene/include/HorizonScene/Components/StreamingComponent.h` (neu, falls kein vorhandenes Tag passt) und die sechs Registrierstellen aus 4.7 | Autorenschalter *Auto/Resident* |
| `src/HE_Scene/include/HorizonScene/Net/NetGameSession.h` und Umgebung | Spieler als Anker auf dem Server (nur Anker, keine Replikation von Zellinhalt) |
| `tests/test_cell_state.cpp` (neu), `tests/test_cell_split.cpp`, `tests/test_physics_*` | Zustand überlebt Entladen/Laden; Tombstone; Verweis auf entladene Entity ist null und Verbraucher stürzen nicht; Gelenk Basis→Zelle wird nach dem Wiederladen wieder gebaut; dynamischer Körper fällt nicht durch einen entladenen Boden (Negativkontrolle ohne Hold: rot); 3 000+ Bodies in Zellen unter `kMaxBodies` |

Abnahme Schritt 3: wie die Tests; dazu ein Dorf (Prefab-Häuser, 50 NPCs mit Skript und `SaveState`,
Wegpunkte als Resident), dessen Zellen mehrfach entladen und geladen werden, ohne dass ein NPC seine
Position, ein Gelenk oder eine zerstörte Kiste verliert.

### Schritt 4: Verifikation (nur Zielwerte, nichts gemessen)

- Welt mit 100k+ Entities über viele Zellen, Messleiter wie 153 §11.3 (`scripts/perf/world_streaming_ladder.sh`,
  `ladder_table.py`), Bench `'Cell streaming bench*'` (braucht die geteilte Referenzwelt, Rezept im Test).
  **Zielwert:** `RenderExtractor::extract` p50 mit ~11k geladenen von 101k im Bereich der 10k-Zeile (6,4 ms) statt 76,6 ms;
  Speicher (RSS) und Asset-Zahl getrennt ausweisen, weil Assets nicht entladen werden (L9).
- Bild-Zeuge für Pop-in an der Zellgrenze (Metal, `scripts/he_shot.py`); Vulkan/D3D nicht (kein Backend-Code).
- Vollbau, `ctest -j4 --timeout 1500` mit `shaderc ON`, und der Lauf der Streaming-Benches.

## 9. Nicht Teil dieses Themas, und was es auslösen würde

| Punkt | Warum nicht | Wann doch |
|---|---|---|
| Streaming im Editor (Zelle öffnen/schließen in der Editor-Welt) | Auswahl, Undo, Speichern über mehrere Dateien und Collab hängen daran | wenn die Editor-Welt selbst nicht mehr in den Speicher passt |
| Simulation entladener NPCs | Zustand wird eingefroren, nichts läuft außerhalb des Radius | wenn Quests NPCs über die Karte laufen lassen; dann Resident oder „Simulations-LOD“ als Daten |
| Datenbasierte Wegpunkte / Marker-Tabelle im Manifest | Wegpunkte, die Basis-Logik braucht, sind Resident | wenn Resident-Wegpunkte die Basis wieder aufblähen |
| Asset-Eviction (LRU) | `ContentManager` kennt kein Entladen (153 §4.2) | wenn Schritt 4 zeigt, dass RSS mit der Erkundung wächst; eigenes Thema |
| Vertikale Zellen | Gitter ist x/z | wenn Höhlen/Hochhäuser echte Welten werden |
| Terrain- und Foliage-Streaming | Terrain bleibt Basis; Foliage und Instancing sind Thema 163 | – |
| GPU-Upload-Budget | Renderer-Arbeit in fünf Backends (153 §10.6) | – |
| Backend-Parität (Vulkan ohne `FrameScope`) | gehört nicht zu diesem Thema, siehe 162 | – |

## 10. Nicht geprüft, Annahmen, Risiken

**Nicht geprüft (nur im Code gelesen, nichts gebaut oder gelaufen):**
- **L7:** dass Editor-Play einer gesplitteten Szene nur die Basis zeigt. Belegt ist nur, dass es keinen Aufruf des
  Streamers im Editor gibt. Schritt 2 beginnt mit einem Lauf (Split, Play, Entity-Zahl).
- Ob `createEntity`/`destroyEntity`/`reparentEntity` über andere Wege die Hierarchie als schmutzig markieren (sie tun es nicht in
  `HorizonWorld.cpp`; der Rest des Codes wurde nicht auf Aufrufer durchgesehen).
- Ob das Savegame eine Erweiterungsstelle für einen Abschnitt „Zellen“ hat (6.3).
- Ob es ein vorhandenes Tag/Layer für den Autorenschalter gibt (4.7).
- Der Skript-Abbau: `ICellHost` verspricht „Skripte starten/stoppen“. Zum Starten gibt es `startScriptsFor` →
  `EntityHost::bindFor` (`GameApplication.cpp:2087`). Ein passendes Lösen im `EntityHost` wurde **nicht** gefunden/geprüft;
  `UnloadZone` räumt nur `m_scriptInstances`, `unwatch` und `ScriptApi::destroy` ab (`:2065–2067`), das ist Abbau
  der Instanzen, kein Lösen der HorizonCode-Entity-Klasse. Schritt 3b liest `EntityHost.h/.cpp` zuerst.
- Dass jede der neuen Zelle-statisch-Komponenten (Audio, Partikel, Skelett, Animator, Prefab-Instanz) nach dem Laden
  sauber neu startet. Das ist als Test je Komponente geplant, nicht behauptet.
- Die Jolt-Charge: nur die Existenz von `AddBodiesPrepare/Finalize` ist belegt (`BodyInterface.h:96, 121–127`
  im Jolt-Quellbaum), nicht der Gewinn. Schritt 3 misst.
- Windows/Linux, D3D/Vulkan: dieser Plan berührt keinen Backend-Code und wurde auf dem Mac nicht gebaut.

**Risiken:**
1. **Verweisliste von Hand (5).** Eine neue Komponente mit einem Entity-Verweis, die der Liste fehlt, würde
   zerrissene Verweise erzeugen, und kein Test merkt es. Abmilderung: Whitelist-Prinzip (unbekannt = Resident) und
   ein Test, der jede Komponente der Tabelle 4.4 mit einem Verweis-Feld gegen die Liste prüft.
2. **Zustand ist opt-in (`SaveState`).** Eine Entity ohne `SaveStateComponent` in einer „zustandsbehafteten“ Zelle
   verliert ihren Zustand beim Entladen. Der Splitter sollte `script` ohne `saveState` melden (Warnung), nicht
   still verschieben.
3. **Budget je Gruppe.** Eine einzelne Gruppe kann das Budget überschreiten (ein Teilbaum mit vielen Entities,
   ein Cluster). Die Obergrenze 128 ist ein Startwert, den Schritt 4 misst.
4. **Play-Stop und Zellen.** Das Zurücksetzen der Welt beim Stop darf den Streamer nicht mit Handles einer
   ersetzten Welt zurücklassen. Deshalb `clear()` **vor** der Wiederherstellung (Schritt 2).
5. **Zwei Zähler.** Entsteht in 162 ein eigener Änderungs-Zähler, bevor 7.2 abgestimmt ist, gibt es zwei. Der Bauplan
   nennt den Punkt, die Abstimmung liegt bei der Königin.

## 11. Rezept für die nächsten Schritte

```sh
# Release-Build im Worktree (Deps vom Nachbar-Build, siehe Memory headless-dump-log-and-worktree-configure)
cmake --build out/build/release -j8 --target he_tests
out/build/release/tests/he_tests --source-file='*test_world_scale.cpp'
out/build/release/tests/he_tests --source-file='*test_cell_split.cpp'
out/build/release/tests/he_tests --source-file='*test_streaming_view.cpp'
# Bench (laeuft in der CI nicht, doctest::skip), braucht die geteilte Referenzwelt:
out/build/release/tests/he_tests --no-skip --test-case='Cell streaming bench*'
# Nach jeder neuen Registry-Row und jeder neuen Komponente: voller ctest, nicht nur -tc
(cd out/build/release && ctest -j4 --timeout 1500)
```

## 12. Schritt 2a: was gebaut ist und was der Bauplan anders sah (10.10.2026)

Teil 2a (stabile Identität) steht auf dem Zweig. Die Abnahme ist ein Test, kein Zähler:
`test_cell_split.cpp`, „A prefab house in a cell: unloaded and loaded again it is the same entities,
bindings intact“ (Split, Streamen, Entladen, Wiederladen, Merge: dieselben UUIDs, dieselben
`PrefabInstance`-Bindungen, derselbe Datensatz je Entity). Dazu die Negativkontrolle „Cells without the
streaming head load as they always did“ (ohne Kopf: frische Ids, die Bindungen zeigen ins Leere, das ist L2).
Die neuen Tests wurden mit absichtlich kaputt gemachtem Code geprüft: ohne den `on_update`-Hörer, ohne die
Bindungsprüfung des Splitters und ohne `preserveIds` werden genau die zugehörigen Fälle rot (und, beim
`on_update`-Hörer, auch Fälle in `test_scene_serializer` und `test_prefab`).

**Gebaut**

| Teil | Wo | Stand |
|---|---|---|
| UUID-Index (5.1) | `HorizonWorld::findByEntityId`, O(1) | gepflegt über die entt-Signale `on_construct/on_update/on_destroy` von `EntityIdComponent`, mit Rückabbildung Entity → UUID; zwei Halter einer Id bleiben beide auffindbar. Nicht erfasst: ein Schreiben direkt in die Komponente (`registry.get<EntityIdComponent>(e).id = x`), das feuert kein Signal; drei Teststellen in `test_prefab.cpp` standen so und gehen jetzt über `setEntityId`. `HorizonWorld` ist jetzt ausdrücklich nicht kopier- und verschiebbar (die Hörer zeigen auf `this`; vorher war es nur implizit so) |
| `preserveIds` auf dem Zellpfad (4.3) | `SceneSerializer::AdditiveOptions`, `CellStreamer::update` | der Streamer setzt es, wenn der Kopf `streaming.version >= 2` sagt; eine belegte Id wird nicht übernommen (frische Id, eine Sammelwarnung je Ladevorgang, `Stats::idCollisions`). Zonen und alle anderen additiven Laden bleiben bei frischen Ids |
| Klassentabelle (4.4) | `CellSplit.cpp`, `kComponentClasses` | Zelle-statisch: die alten Schlüssel plus `prefab`, `particlesystem`, `skeletalmesh`, `animator`, `animatorblend`, `propertyanimator`; alles Unbekannte bleibt Resident. Zelle-zustandsbehaftet ist als Klasse da, enthält aber noch nichts (3b) |
| Prefab-Bindungen im Splitter | `bindingsStayInside` | ein Teilbaum mit einer Prefab-Bindung auf eine Entity außerhalb des Teilbaums bleibt in der Basis (ein herausgezogenes Kind der Instanz) |
| Manifest v2, Kopf (4.1, 4.2) | `CellManifest::version`, `Cell::bodies`; Kopf `streaming` je Zelle | `bodies` = Entities mit Rigidbody oder Collider, eine obere Schranke: ein Collider allein baut heute keinen Body (`PhysicsWorld::buildBodyFor` braucht ein `RigidBodyComponent`). Der Streamer liest `bodies` noch nicht (3a) |
| Kopf in der Welt | `HorizonWorld::cellHeadJson` | siehe Befund 1 |
| Play-Warnung | `EditorApplication::setPlayMode` | siehe Befund 4 |
| Texte | Profiler-Tab, Tooltip *Split into Streaming Cells*, Kopf von `split_scene_cells.py` | sagen, was jetzt wandert; das Skript schreibt weiter Version 1 |

**Befunde, die der Bauplan anders sah oder nicht kannte**

1. **Der Export schreibt jede Szene neu.** `ExportDialogPanel.cpp` lädt jede `.hescene` des Projekts in eine
   `HorizonWorld` und speichert sie mit `saveToMemory` (`:1282–1289`); `buildSceneJson` schreibt nur
   `entities`, `version`, `levelScript` und `cells`. Ein Kopf, der nur in der Datei steht, wäre im Pak weg, und das
   ausgelieferte Spiel würde jede Zelle wieder mit frischen Ids laden. Deshalb trägt die Welt den Kopf wie das
   Manifest (`applySceneJson` liest ihn, `buildSceneJson` schreibt ihn, `clear()` löscht ihn; eine additive Ladung
   fasst ihn nie an). Test: „The cell head survives the export's load and save“. Dasselbe gilt für jeden künftigen
   Schlüssel auf oberster Ebene einer Zelldatei; `cellFolders` (nur für den Merge) geht im Pak verloren, das ist gewollt.
2. **`audioSource` bleibt Resident, anders als in 4.4.** Der Plan nimmt an, dass `playOnStart` nach dem Laden
   neu läuft. `AudioSystem::playOnStart` läuft aber nur beim Szenenstart und beim Szenenwechsel
   (`GameApplication.cpp:1370`, `:1879`), nie für das, was eine Zelle (oder Zone) bringt, und niemand stoppt die
   Stimme (`AudioSourceComponent::handle`), wenn die Zelle geht. Eine Quelle in einer Zelle bliebe stumm. Das gehört in den
   Host (`ICellHost`, 2c/3b): Start für die `created`-Entities, `AudioEngine::stop(handle)` beim Entladen.
3. **`animstatemachine` bleibt Resident:** `AnimatorHost::begin` bindet die Zustandsautomaten einmal beim
   Szenenstart (`AnimatorHost.cpp:34`). `animationlayers`, `rootmotion`, `ik`, `sequenceplayer` sind nicht
   geprüft (die beiden letzten nennen andere Entities) und bleiben in der Basis. `particlesystem`,
   `skeletalmesh`, `animator`, `animatorblend`, `propertyanimator` halten nur Asset-Ids, ihr Laufzustand
   liegt im eigenen Component, und ihre Systeme laufen je Frame über die Registry; der Rundlauf-Test
   („The decorative components that may stream come back from a cell as they went in“) belegt die Werte, nicht
   das Aussehen auf den Backends (Schritt 4).
4. **L7 (Play zeigt nur die Basis) ist durch den Code belegt, nicht durch einen Lauf.**
   `EditorApplication.cpp` enthält weder `CellStreamer` noch einen Streamer-Aufruf (nur `StreamingDebugView`
   liest das Manifest), und `setPlayMode` sichert und stellt dieselbe Editor-Welt wieder her, die nach einem Split
   nur die Basis enthält (`test_streaming_view`: `meshCount(world) == 0` nach dem Split). Ein GUI-Lauf war hier
   nicht möglich (unter den `HE_DUMP_*`-Schaltern gibt es keinen für Play). Geliefert ist die Warnzeile; sie steht im
   Post-Play-Bericht des Editors.
5. **Ref-Hülle fehlt weiter (3a).** 2a prüft nur die Bindungen einer Prefab-Instanz. Verweise aus der Basis
   in einen Zellen-Teilbaum (Gelenk, Rig, Seil) lösen jetzt auf, solange die Zelle geladen ist, und sind
   `entt::null`, sobald sie entladen ist (R3); der Splitter verhindert das noch nicht.
6. **Das Handbuch stimmt nicht mehr:** `HorizonEngineDocs` (Website-Checkout, nicht in diesem Repo) sagt, welche
   Dinge in Zellen wandern. Aus dem Repo nicht änderbar; die Texte im Editor (Profiler-Tab, Tooltip) sind angepasst.
7. **Fünf Tests sind auf `release/0.7.0` rot, bevor 2a etwas ändert:** `test_material_graph`, `test_culling`,
   `test_terrain_tools_ui`, `test_assimpimport`, `test_editor_help` (gemessen mit einem vollen ctest auf dem
   unveränderten Stand, Release, shaderc ON). Vier davon sind Thema 181; `test_culling` (zwei Fälle zur Himmels-Shader-Kopie:
   „Dome clouds: Metal and GL march and shadow them the same way“, „Nebula: Metal's kSkyMSL copy matches the GL sky shader after
   normalisation“) steht in dessen Liste nicht.

8. **Prefab-Instanzen in Zellen werden vom Editor nicht abgeglichen, solange sie in Zellen liegen.** Der Abgleich
   mit dem Prefab-Asset (`syncPrefabInstances`) läuft beim Öffnen und vor dem Speichern über die Editor-Welt, und
   die enthält nach einem Split nur die Basis. Der Export lädt dagegen jede Szene einzeln und gleicht sie ab
   (`syncPrefabsForExport`, auch jede Zelle; die Bindungen halten, weil der Ladevorgang dort die Ids wiederherstellt).
   Wer ein Prefab ändert, sieht es in den Zellen des Editor-Stands also erst nach *Merge Cells into the Scene* und im Export.
   Der Grund, aus dem das Skript `split_scene_cells.py` Instanzen in der Basis ließ („the editor keeps those in sync“), gilt
   für Version-2-Zellen damit eingeschränkt weiter.

**Gemessen** (Release, M-Serie, nebenbei lief Last, drei Läufe, daher nur die Größenordnung; `he_tests --no-skip --test-case='Id index bench*'`, läuft in der CI nicht):

| Entities | `createEntity` mit Index | dasselbe ohne Hörer | Auflösen einer Id: Index | Auflösen einer Id: Scan (alt) |
|---|---|---|---|---|
| 50 000 | 31–34 ms | 9–13 ms | 0,06 µs (3 ms für 50 000) | 32 µs (6,4 ms für 200 Aufrufe) |
| 200 000 | 141–170 ms | 25–50 ms | 0,10–0,12 µs (20–25 ms für 200 000) | 246 µs (49 ms für 200 Aufrufe) |

Der Index macht das Auflösen einer Id 500- bis 2000-mal billiger (je nach Weltgröße) und das Anlegen einer
Entity um rund 0,5 µs teurer (zwei knotenbasierte Hash-Einfügungen): bei einer
ganzen 200k-Welt etwa 100 ms auf 2,7 s Ladezeit (153 §10.5), bei einer Zelle von 800 Entities etwa 0,4 ms. Der
Speicher je Entity ist nicht gemessen (grob zwei Hash-Knoten, der Größenordnung nach 100 Byte); wer ihn senken
will, ersetzt `m_idIndexed` durch einen Vektor nach Entity-Nummer und die Multimap durch eine flache Tabelle.

**Offen für die folgenden Teile**

- 2b: Anker, gestückelter Aufbau (die Anhänge-Frage aus 4.6 ist unberührt), `structureEpoch`, Zellansicht; der Streamer ignoriert `bodies` und die Klassenzahlen im Kopf noch.
- 2c/3b: Audio-Start und -Stopp, Zustandsautomaten-Bindung, Skripte: gehören in den Zellen-Host.
- 3a: Ref-Hülle im Splitter (heute nur die Prefab-Bindungen), Body-Reserve aus `bodies`.
- Handbuch auf der Website nachziehen (Befund 6).
