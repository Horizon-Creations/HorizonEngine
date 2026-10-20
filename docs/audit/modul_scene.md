# Modulbericht Scene – HorizonScene (72 k LOC)

Pfad: `src/HE_Scene` · Teilberichte: 8 (`_parts/Scene_*.md`)

## Modulzusammenfassung

**153 Funde**: Hoch 20, Mittel 71, Niedrig 62 · Typ: Vereinfachung 21, Performance 114, Beides 18 · Kategorien: A 12, B 4, C 60, D 59, E 7, F 11

| Teilbericht | Hoch | Mittel | Niedrig |
|---|---|---|---|
| Scene_1.md | 4 | 10 | 7 |
| Scene_2.md | 1 | 9 | 8 |
| Scene_3.md | 2 | 9 | 7 |
| Scene_4.md | 2 | 9 | 9 |
| Scene_5.md | 1 | 7 | 8 |
| Scene_6.md | 4 | 11 | 6 |
| Scene_7.md | 4 | 8 | 12 |
| Scene_8.md | 2 | 8 | 5 |

### Hoch-Funde (Index)

- **SCN-101** – Pro Entity und Frame mehrere Heap-Vektoren in allen drei Pose-Treibern
- **SCN-102** – Kein Gate für unsichtbare / ferne / unveränderte Animatoren
- **SCN-103** – Lineare Keyframe-Suche pro Kanal und Frame
- **SCN-112** – Beendete Stimmen werden nie von der Engine abgeräumt (Script-One-Shots bleiben liegen)
- **SCN-201** – Skript-Dispatch: zwei Vektor-Allokationen und ein 280-Byte-`Value` pro API-Aufruf
- **SCN-301** – Client-Interpolation schreibt jeden Frame jede Entity und setzt `dirty`, auch im Stillstand
- **SCN-302** – Assetabgeleitete Daten werden pro Entity kopiert (State-Machine-Graph, Blend-Spaces, Masken, Partikel-Config)
- **SCN-401** – PropertyReplicator::update baut pro Frame und Entity Namen-/Wert-Listen neu auf
- **SCN-402** – LODSystem::update läuft jeden Frame über alle LOD-Entities und komponiert jedes Mal die Eltern-Kette
- **SCN-501** – PropertyReplicator::update allokiert pro Frame und Entity
- **SCN-601** – step(): Write-back-Schleife besucht jeden Body, auch statische und schlafende
- **SCN-602** – Contact-Listener: globaler Mutex pro Kandidaten-Paar und pro Contact
- **SCN-603** – Collider-Shapes werden pro Entity neu gebaut und nie geteilt
- **SCN-604** – RopeTrailSystem: pro Frame Kopie, Hash und Aufräum-Sweep für jedes Seil, ohne Dirty-Flag
- **SCN-701** – buildSceneJson/buildSubtreeJson kopieren den gesamten JSON-Baum mehrfach (Undo-Snapshot)
- **SCN-702** – syncPrefabInstances parst pro Instanz das CBOR neu und löst Bindings quadratisch auf (läuft bei jedem Speichern)
- **SCN-703** – Lua-Dispatch: schwerer `HE::api::Ctx` und zwei `vector<Value>` pro Aufruf
- **SCN-704** – collectAssetRefs(world, onlyEntities) durchsucht für jeden Slice die ganze Welt
- **SCN-801** – updateTessellation läuft jeden Tick über alle Chunks, auch wenn Tessellation aus ist
- **SCN-802** – Chunk-Material-Sync: O(Terrains × Chunks) jeden Tick, ungated

## Zusammenfassungen der Teilberichte

**Scene_1.md**: Der Code ist insgesamt sauber, gut kommentiert und an den Stellen, die jemand schon gemessen hat, bereits optimiert (`worldMatrixOf` mit Stack-Chain, inkrementelles `propagateTransforms`, EQ/Envelope-Handover ohne Lock auf dem Mixer-Thread). Die größten offenen Hebel liegen im Animations-Pfad: Jede spielende Animation wird jedes Frame vollständig gesampelt und gepost (kein Sichtbarkeits-/Distanz-/Dirty-Gate), mit mehreren `std::vector`-Allokationen pro Entity und Frame, linearer Keyframe-Suche und einer 3-Matrix-Zerlegung pro Gelenk. Zweiter Block: `AudioEngine::startSound` kopiert die komplette Clip-Datei pro Stimme, und fertige Stimmen werden von der Engine nie selbst abgeräumt. Dritter Block: `CameraRigController::update` und `CellStreamer::update` laufen jedes Frame mit mehreren O(N)-Scans bzw. Allokationen ohne Early-out. AntiCheat und CellSplit sind überwiegend kalter Code (nur Kleinkram).
Funde: Hoch 4 (SCN-101, 102, 103, 112), Mittel 10 (SCN-104 bis 111, 113, 114), Niedrig 7 (SCN-115 bis 121), insgesamt 21. Die IDs folgen der Reihenfolge Animation, Audio, Kamera, Cell; Schweregrad steht je Fund. Alles ohne Messdaten; Schätzungen sind als solche gekennzeichnet.

---

**Scene_2.md**: Der Slice ist funktional sauber und sehr gut kommentiert; die Probleme sind weniger Logikfehler als Kosten an den Rändern. Der größte Hebel ist die Skript-Grenze in `EngineApi.cpp`: jeder Registry-Aufruf läuft über `std::function`, einen `std::vector<Value>` für Argumente und einen für das Ergebnis, mit einem 280-Byte-`Value` (drei `std::string`, zwei `std::vector`). Der Self-Default-Wrapper kopiert diese Argumentliste bei jedem unverdrahteten Entity-Pin ein zweites Mal. Das ist pro Tick und pro Aufruf aus HorizonCode, Lua und Python relevant. Daneben gibt es mehrere O(n)-Pfade pro Aufruf oder Frame ohne Dirty-Flag (`EntityHost::tick`, `findByName`, Outliner-Zeilen, `FoliageSystem`), synchrone I/O auf dem Update-Thread (Prefs, Watch-Polling) und eine vollständige Foliage-Neuverteilung pro Mal-Frame im Editor. Funde: 1 Hoch, 9 Mittel, 8 Niedrig. Alle Zeilenangaben sind gegen den aktuellen Stand verifiziert; Größenordnungen sind Schätzungen und unter "Unsicher / Messung nötig" aufgeführt.

---

**Scene_3.md**: Der Slice besteht zu etwa 55 % aus `HcCodegen.cpp` (Build-/Export-Zeit-Generator plus Toolchain-Probe, kein Frame-Pfad), zu 10 % aus `GameReplication.cpp` (Netzwerk-Tick) und sonst aus Headern und reinen Daten-Komponenten. Die Komponenten sind überwiegend schlank und nutzen bereits Dirty-Flags oder Revisionszähler (`MeshComponent::dirty`, `MaterialComponent::dirty`, `FoliageComponent::revision`, `AnimatorStateMachineComponent::configDirty`). Die relevanten Funde liegen deshalb in drei Bereichen:
- Per-Frame-Pfade ohne Änderungserkennung: Client-Interpolation, Server-Snapshots, `AudioSystem::updateSpatial`.
- Pro-Entity-Heap für Daten, die aus einem geteilten Asset abgeleitet sind (State-Machine-Graph, Blend-Spaces, Masken, Partikel-Config, 2,5 KB Zufallsgenerator).
- Die vom Codegen erzeugte Laufzeit (String-Dispatch, `std::vector<hc::Value>` pro Engine-Call) und eine Datenrasse in der cmake-Auflösung.

Funde: 2 Hoch, 9 Mittel, 7 Niedrig (18 gesamt). Alle Zeilennummern wurden gegen den aktuellen Stand der Dateien geprüft. Es wurde nichts gemessen. Alle Effekte sind Schätzungen.

---

**Scene_4.md**: Slice 4 besteht fast nur aus Headern (70 Dateien, ca. 10 900 Zeilen, davon `EngineApi.h` 3 094 und `PhysicsWorld.h` 933). Die Header sind inhaltlich sehr gut dokumentiert; die Komponenten selbst sind überwiegend schlanke POD-Structs mit Dirty-Flags (`TransformComponent`, `TerrainComponent`, `SkeletalMeshComponent`). Die eigentlichen Kosten stecken in den dazugehörigen Systemen, deren Signaturen hier deklariert sind. Ich habe die jeweilige `.cpp` nur punktuell als Beleg gelesen (die `.cpp`-Dateien liegen in anderen Slices; dort bitte gegenprüfen). Auffällig sind (a) zwei per-Frame-Pfade ohne Dirty-Tracking mit Heap pro Entity (`PropertyReplicator::update`, `LODSystem::update`), (b) mehrere kleinere per-Frame-Allokationen in Script-/Input-/Rope-Pfaden und (c) ein unbegrenzt wachsender Log-Merker im RPC-Router. Funde: 2 Hoch, 9 Mittel, 9 Niedrig. Alle Performance-Schätzungen sind Schätzungen ohne Messung.

---

**Scene_5.md**: Der Slice besteht zur Hälfte aus gut dokumentierten, reinen Deklarations-Headern (Water*, Terrain*, Spline*, UI*) und zur Hälfte aus Laufzeitcode (HorizonWorld, LOD/Movement/Navigation/Particle, Net-Replikation). Der Code ist insgesamt sauber und sehr gut kommentiert; die Dirty-Flag-Disziplin im Terrain/Water-Bereich ist vorbildlich (Region-Dirty, Revision, Hash). Die Funde konzentrieren sich auf die Per-Frame-Systeme: `PropertyReplicator::update` allokiert pro Frame und pro replizierter Entity mehrere Vektoren samt String-Kopien, `LODSystem`, `MovementSystem` und `NavigationSystem` iterieren jeden Frame alle Komponenten ohne Änderungserkennung, `ParticleSystem` führt pro Partikel einen Raycast aus, und `HorizonWorld::clear()` ist bei flachen Hierarchien quadratisch. Insgesamt 16 Funde: 1 Hoch, 7 Mittel, 8 Niedrig. Alle Laufzeitangaben sind Schätzungen; es lagen keine Messdaten vor.

---

**Scene_6.md**: Der Slice (PhysicsWorld, PlayerHost/-Settings, PoseFinalize/PoseSource, PropertyAnimationSystem, RootMotion*, RopeTrailSystem, SceneJsonParse) ist insgesamt sorgfältig und gut kommentiert. Gefunden wurden vor allem Skalierungsprobleme bei vielen Entities: `PhysicsWorld::step` besucht pro Schritt jeden Body (auch statische und schlafende), der Contact-Listener nimmt für jedes Kandidaten-Paar einen globalen Mutex, und Collider-Shapes werden pro Entity neu gebaut, nie geteilt. `RopeTrailSystem` und die Pose-/Root-Motion-Pfade arbeiten ohne Dirty-Flags und mit Heap-Allokationen pro Entity und Frame. Insgesamt 21 Funde: 4 Hoch, 11 Mittel, 6 Niedrig. `PlayerSettings.cpp` ist unauffällig (kein Hot Path, kein Fund). Es gibt keine Messdaten; Schätzungen sind als solche gekennzeichnet.

---

**Scene_7.md**: Der Slice (13 Dateien, ca. 10,6k Zeilen) ist insgesamt sauber und sehr gut kommentiert. Die Probleme konzentrieren sich auf drei Stellen. (1) Der Szenen-Serializer macht beim Speichern/Undo mehrfach tiefe JSON-Kopien, darunter Multi-MB-Base64-Strings des Terrains. Er parst beim Prefab-Sync dasselbe CBOR-Blob pro Instanz neu. Er hat mehrere quadratische Suchen über lineare `Binding`-Vektoren. (2) Die Lua-Brücke baut pro Aufruf einen schweren `HE::api::Ctx` (ca. 25 `std::function`-Member, drei davon kopiert) und allokiert `vector<Value>`. (3) Per-Frame-Systeme (`SequenceSystem::begin/apply`) iterieren ohne Dirty-Flag über alle `SkeletalMeshComponent` und allokieren pro Spieler/Frame. Die Terrain-Werkzeuge kopieren pro Pinselstrich ganze Felder (Smooth, Weightmap). Gefunden: 4 Hoch, 9 Mittel, 10 Niedrig. Alle Aussagen sind aus dem Code belegt. Messdaten gibt es nicht, Schätzungen sind als solche markiert.

---

**Scene_8.md**: Der Slice ist insgesamt sorgfältig gebaut: Transform-Propagation hat Dirty-Scan, Stats, Verify-Modus und Tests; Wasser und Terrain haben eigene Dirty-Rechtecke und Hash-Gates, und Kommentare begründen Entscheidungen. Die Schwachstellen liegen in den Per-Frame-Pfaden, die trotz sauberer Dirty-Logik an anderer Stelle jeden Tick über alle Chunks, Elemente oder Splines laufen und dabei Heap-Container anlegen. Das sind die Terrain-Tessellation, die Chunk-Material-Synchronisation, der UI-Extract und die Wasser-Spline-Synchronisation. Dazu kommen Wiederholungen im Wassercode (FNV-Hash, Punkt-Segment-Distanz, `hasArea` jeweils 3–5-fach) und node-basierte Maps in der Contour-Extraktion. Insgesamt 15 Funde: 2 Hoch, 8 Mittel, 5 Niedrig. Es gibt keine Messdaten; alle Aufwandsschätzungen sind Schätzungen.

---

## Cross-Modul-Hinweise

**Scene_1.md**

- `src/HE_Core/include/ContentManager/Assets.h:645-651`: `AnimationChannel` hält zwei `std::vector<float>` pro Kanal (3 Kanäle × Gelenkzahl × Clip → viele kleine Heap-Blöcke, Pointer-Chasing in `sampleClip`). Ein SoA-/Arena-Layout je Clip (ein `times`-/`values`-Block mit Offsets) würde Cache-Verhalten und Ladezeit verbessern; zusammen mit SCN-103/104 (sortierte Zeiten, Kanalindex pro Gelenk) im Asset-Loader lösbar.
- `src/HE_Scene/src/PoseSource.cpp:64,154` und `PoseFinalize.cpp:79,213-253`: rufen die hier gefundenen Blend-Space-/Namensauflösungsfunktionen. `resolveBoneMask` / `findJointByName` sind dort bereits gecacht (`lc.resolvedMasks`, `ic.resolvedChain`) – gut; `blendSpaceWeights`/`blendPosesN` sind es nicht (SCN-110). `PoseFinalize` selbst (Allokationen, `composeModelMatrices`-Scratch) gehört zu einem anderen Slice und wird vermutlich dieselben Vektor-Muster wie SCN-101 zeigen.
- `src/HE_Scene/src/EngineApi.cpp:2675,2682`: `audio.play`/`audio.playAt` liefern Handles, aber nichts reapt Einmal-Sounds (SCN-112).
- `src/HE_Scene/src/TransformHierarchy.cpp:309-314`: `localMatrix(TransformComponent)` benutzt dieselbe `translate*mat4_cast*scale`-Zerlegung wie SCN-109 (Pro-Entity-Kosten bei jeder schmutzigen Transform).
- `src/HE_Game/src/GameApplication.cpp:2171`: `cellHolds` wird dort als Lambda an `PhysicsWorld::setRegionHold` übergeben; wird pro Körper pro Physikschritt gerufen (nicht verifiziert) – dann sind die 3 Hash-Lookups in `CellStreamer::holdsAt` (`CellStreamer.cpp:277-286`) und die `dvec3`-Konversion in `CellPhysics.cpp:28-31` ein Hot-Path-Kandidat; in der Physik-Slice prüfen.

**Scene_2.md**

- `src/HE_Core/include/HorizonCode/HorizonCode.h:93-124`: Die Struktur `Value` ist die Ursache von SCN-201 und gilt für das gesamte Skript-Subsystem (Interpreter, Codegen, Variablen, Events). Eine Verkleinerung gehört dem Core-Slice, die Messung dort (`Runtime::fireTick`, `callFunction`) sollte `Value`-Kopien zählen.
- `HorizonCode::Runtime::fireTick/alive` (aufgerufen aus `EntityHost::tick`): Ob für Klassen ohne Tick-Ereignis ein früher Rücksprung existiert, ist im Core-Slice zu prüfen; falls nicht, wäre ein `hasTickHandler(inst)`-Flag die größere Ersparnis zu SCN-203.
- `src/HE_Editor/TerrainTools.cpp:737-756` (SCN-206), `:1596-1599` und `InspectorPanel.cpp:3517,3526` (SCN-205, SCN-217): Editor-Aufrufer, die `cachedInstances`/`coverage` pro Frame lesen.
- `src/HE_Editor/OutlinerPanel.cpp:1232,1562` (SCN-210): Zeilenweise Aufrufe von `isEntityActive`/`subtreeVisibility`.
- `src/HE_Game/src/GameApplication.cpp:3138-3140` und `src/HE_Editor/EditorApplication.cpp:3138`: Rufen `pushSdlSnapshot` jeden Frame (SCN-213).
- `src/HE_Scene/src/PhysicsWorld.cpp:703`: Der eigene `WaterIndex` kann für SCN-211 mitgenutzt werden.
- `HE::api::find(const std::string&)` (`EngineApi.cpp:8773`) nimmt `const std::string&`; Aufrufer mit `const char*` (z. B. `UIEditorPanel.cpp:6753`) allozieren bei Namen über 15 Zeichen. Eine `std::string_view`-Überladung wäre kostenfrei (der Index ist ohnehin `string_view`-gekeyt).

**Scene_3.md**

- `src/HE_Scene/src/PoseFinalize.cpp:434-440` (`poseBeginFrame`): zwei vollständige View-Läufe pro Frame nur zum Zurücksetzen von Bools (siehe SCN-313).
- `src/HE_Scene/src/AnimationStateMachineSystem.cpp`, `AnimationSystem.cpp`, `AnimationBlendSystem.cpp`: per-Entity `params` per String-Hash und per-Entity Blend-Space-Cache (SCN-302). Dort prüfen, ob pro Frame `std::string`-Vergleiche (`currentStateName`, `transitionTarget`) laufen.
- `src/HE_Scene/src/FoliageSystem.cpp` und Editor-Foliage-Paint: Nutzer von `cachedInstances` (SCN-306).
- `src/HE_Scene/src/SceneSystems.cpp`, `HE_Game/src/GameApplication.cpp:3294`, `HE_Editor/EditorApplication.cpp:3950`: Aufruf von `AudioSystem::updateSpatial` — Reihenfolge zur Transform-Propagation prüfen (SCN-305).
- `ContentManager/Assets.h` (`AudioAsset::audioData`): Umstellung auf geteilten, unveränderlichen Puffer (SCN-304).
- `EnvironmentComponent.h:11-13`: Das Environment wird laut Kommentar "pushed to the renderer each frame via IRenderer::SetEnvironmentSettings". Die Struktur ist rund 400 Byte groß. Ein Revisionszähler am Environment würde Vergleich/Upload bei unveränderten Werten sparen (Rendering/Editor prüfen).
- `AntiCheatService.h:296-297` und `AntiCheatTelemetry.h:130`: `Report` enthält `vector<Observation>` mit je zwei Strings; Reports werden in `deque<Report>` und `m_telemetry` kopiert (nur bei Ereignissen, daher unkritisch, aber bei Event-Sturm teuer).
- `GameReplication.h:348` / `NetworkComponent.h`: Bei Umbau nach SCN-301/303 Protokoll- und Client-Tests in `tests/` mitziehen.

**Scene_4.md**

- `src/HE_Scene/src/PhysicsWorld.cpp:3231-3470` (`PhysicsWorld::step`): Der Write-Back iteriert pro Schritt über **alle** Einträge von `entityToBody` (knotenbasierte `unordered_map`, plus Kopie in `stepBodies`), prüft `registry.valid`, `TryGetBody`, Motion-Type und schreibt für jeden nicht-statischen Körper (auch schlafende) die Transform zurück (`writeBackWorldPose` inkl. Welt→Lokal-Umrechnung). Skip für schlafende Körper (`!body->IsActive()`) würde die in `EngineApi.h:331-335` dokumentierte Eigenschaft ändern, dass der Step die Transform jedes Frames überschreibt – nur mit Test und Absicht ändern. Gehört in den Physik-Slice.
- `src/HE_Scene/src/TransformHierarchy.cpp:321-367` (`worldMatrixOf`): Wird von `LODSystem`, `RopeTrailSystem::resolveControlPoints`, `PhysicsWorld` (Joints), `EngineApi` (jede `transform.*`-Zeile) pro Aufruf frisch komponiert. Ein generationsbasierter Cache (invalidiert durch `propagateTransforms`) würde SCN-402 und SCN-404 gleichzeitig entlasten – Entscheidung für den Transform-Slice.
- `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp` (`Runtime::fireEvent`/`dispatchToListeners`): Jeder Aufruf hasht/interniert den Event-String (`eventId(name)`); relevant für SCN-408.
- `src/HE_Core/include/UIWidget/WidgetManager.h` (1 441 Zeilen) und `HorizonCode/HorizonCode.h` (1 551 Zeilen) werden über `HorizonWorld.h` in fast jede Scene-Datei gezogen (SCN-411); eine Aufteilung in „Typen“ und „Manager“ im Core wäre die Wurzellösung.
- Editor: `src/HE_Editor/InspectorPanel.cpp:4041` ruft `componentKeyOfText` im Menü-Pfad auf (SCN-417).
- `src/HE_Game/src/GameApplication.cpp` / `src/HE_Editor/EditorApplication.cpp`: beide rufen `pushSdlSnapshot`, `PlayerHost::tick`, `NetGameSession::update` pro Frame; die Frame-Reihenfolge ist im Header teils nur als Prosa dokumentiert (`SceneSystems.h:43-54`) – ein gemeinsames `HE::Frame`-Modul wäre Entscheidung für den App-Slice.

**Scene_5.md**

- `src/HE_Scene/src/WaterLake.cpp` (`syncSplines`, WaterLake.h:278-284): laut Header wird pro Frame für jeden See ein Fingerabdruck aus Spline-Punkten, Closed-Flag und BEIDEN Weltmatrizen berechnet (`worldMatrixOf` Spline + Terrain). Ohne Dirty-Hinweis (z. B. `SplineComponent`-Revision + Transform-`structureEpoch`) ist das ein ECS-Durchlauf ohne Änderungsmarker. Implementierung liegt in einem anderen Slice; dort prüfen.
- `src/HE_Scene/src/WaterQuery.cpp` (`WaterIndex::refresh`, WaterQuery.h:60-90): wird laut Header einmal pro Physikschritt aufgerufen und berechnet Landscape-`localToWorld`/`worldToLocal` (Inverse) neu; `WaterIndex::build(world)` baut für Einzelabfragen (Skript) einen kompletten Index inklusive Wet-Rect-Scan (O(res²)). Prüfen, ob Skript-Wasserabfragen pro Frame/Entity darauf laufen (EngineApi).
- `src/HE_Scene/src/UISystem.cpp`: `nestingDepth` (UISystem.h:30) und `computeScreenRect` (:37) laufen pro Element die UI-Elternkette hoch; `extract` ruft sie vermutlich pro Element und Frame auf – O(Elemente × Tiefe) ohne Layout-Cache. Dort mit Dirty-Layout prüfen.
- `src/HE_Core/.../HorizonCode` (`Runtime::replicatedVariablesOf`, `getVariable`): von `PropertyReplicator::update` pro Entity und Frame aufgerufen (SCN-501). Eine `replicatedRevision(inst)` und ein Zugriff per Slot-Index statt Namen würden SCN-501 vollständig lösen.
- `src/HE_Scene/include/HorizonScene/Components/ParticleSystemComponent.h:41-42`: `std::mt19937` (~2,5 KB) und `LegacyConfig` inline (SCN-504).
- `src/HE_Scene/src/TransformHierarchy.cpp:321-362`: `worldMatrixOf` ist korrekt allokationsfrei für Tiefe ≤ 32; jeder Aufrufer (LOD, Particle, Navigation, Water) zahlt die Kettenwanderung pro Aufruf erneut. Ein per-Frame-Cache (Epoche + Entity) würde alle vier entlasten.

**Scene_6.md**

- `src/HE_Scene/src/SceneSystems.cpp:213,217,230`: Reihenfolge `rootMotionBeginFrame`/`poseBeginFrame` und `PropertyAnimationSystem::update` (siehe SCN-614, SCN-613).
- `src/HE_Scene/include/HorizonScene/Components/{RopeComponent,TrailComponent,RootMotionComponent,AnimationLayerComponent,IkComponent}.h`: ein Revisions-/Frame-Stempel (SCN-604, SCN-614) gehört in die Komponenten; Serialisierung beachten.
- `src/HE_Scene/src/PlayerHost.cpp:390,401,416`: `callOnInputAxis/-Axis2D/-Pressed` werden für **jede** Lua/Python-Instanz je Aktion und Frame aufgerufen (O(Aktionen × Skript-Instanzen)). Prüfen, ob `ScriptContext` ohne Handler schnell zurückkehrt (Handler-Cache pro Instanz); sonst Aufrufe nur an Instanzen mit `onInputAxis`. Unsicher, Code in `ScriptContext.cpp`.
- `src/HE_Scene/src/PropertyAnimationSystem.cpp:92`: Material-Kanäle schreiben in das **geteilte** `MaterialAsset` (`getMaterialMutable`) und setzen nur `mc->dirty` der animierten Entity. Mehrere Entities mit demselben Material überschreiben sich gegenseitig, und das Asset im Speicher wird verändert (Persistenz?). Das Verhalten steht im Kommentar („written directly to the shared MaterialAsset"), ist aber ein Designrisiko; Prüfung bei MaterialSystem.
- `HE::water::WaterIndex::refresh(world)` (aufgerufen in `PhysicsWorld.cpp:3795`) läuft jeden Schritt; ob der Aufruf bei unveränderten Landscapes billig ist (Revision-Check), steht in `WaterQuery.cpp`, nicht geprüft.
- `src/HE_Core/include/Application/…` (`HE::inputEventAxis` & Co. in `InputAssets.h`): Event-Name-Erzeugung liefert `std::string`; ein `string_view`/interned ID wäre die Grundlage für SCN-606.
- `HorizonCode::Runtime::fireEvent(inst, std::string, …)`: String-Schlüssel-Lookup pro Event; interned Event-IDs würden den Aufruf verbilligen.

**Scene_7.md**

- `src/HE_Scene/include/HorizonScene/Components/PrefabInstanceComponent.h:104-115`: `instanceOf/templateOf` sind lineare Vektorscans über `bindings`. Ein Index (oder zumindest `unordered_map`) hier würde SCN-702 und SCN-708 an der Wurzel lösen.
- `src/HE_Editor/OutlinerPanel.cpp:670-674`: ruft pro Instanz-Zeile `prefabRemovedHereCount` + `prefabAddedHereCount` und pro Zeile unter einer Instanz `prefabInstancesBinding` auf (siehe SCN-708).
- `src/HE_Editor/EditorApplication.cpp:13604`: `syncPrefabInstances("save")` läuft bei **jedem** Speichern über alle Instanzen (SCN-702). Ein Dirty-Flag „Prefab-Asset seit letztem Sync geändert" würde den Sync im Normalfall ganz überspringen.
- `src/HE_Editor/EditorUndo.cpp:10,42,84,118`: Vollszenen-Snapshot (`saveToMemory`) pro Undo-Schritt (SCN-701). Langfristig wären Delta-Snapshots (nur geänderte Entities) die größere Lösung.
- `src/HE_Game/src/GameApplication.cpp:1504-1505`: verwendet die Teilmengen-Variante von `collectAssetRefs` (SCN-704).
- `src/HE_Scene/src/TerrainSystem.cpp:396-420`: Weightmap-Neuaufbau und `computeAverageLayerWeights` laufen bei jedem `weightsDirty` über die ganze Textur (SCN-713).
- Das Ctx-Design in `EngineApi.h:60-150` (25+ `std::function`-Member, „New members still go at the END: … positional aggregate init") begünstigt SCN-703 strukturell. Ein `Services`-Pointer wäre die saubere Lösung, auch für das Python-Backend (`HE_Python`).
- `RopeTrailSystem` (nicht in diesem Slice) ist der vermutliche Hauptaufrufer von `buildRibbon` pro Frame (SCN-716).

**Scene_8.md**

- `src/HE_Scene/src/TerrainMeshGenerator.cpp:72-94` (`computeTerrainHeightField`): kopiert bei Sculpt-Daten das ganze `sculptHeights`; siehe SCN-803.
- `src/HE_Scene/src/SceneSystems.cpp:113,131,140,150`: Reihenfolge der Systeme; `updateTessellation` (SCN-801) läuft jeden Tick; dort steht auch die dritte „erstes WeatherComponent“-Suche (SCN-814).
- `src/HE_Rendering/src/RenderExtractor.cpp:1202`: `WaterIndex::build` pro Frame für den Underwater-Check (SCN-813); `:1586` UISystem::extract pro Frame (SCN-804).
- `src/HE_Scene/include/HorizonScene/UIInputSystem.h:25`: `std::unordered_set<uint32_t> hovered` (SCN-805).
- `src/HE_Scene/src/PhysicsWorld.cpp:3795`: `waterIndex.refresh(world)` in jedem Wasser-Step (SCN-813).
- `src/HE_Editor/EditorApplication.cpp` und `TerrainTools.cpp`: dutzende direkte `updateTerrains`-Aufrufe (u. a. 3897, 6480, 6812, 7214 …); jede Verbesserung des Leerlaufpfads (SCN-802, SCN-807) wirkt dort mehrfach pro Frame.
- `src/HE_Scene/src/CameraRigController.cpp:410,581,645`, `NavigationSystem.cpp:115`: weitere `propagateTransforms`-Aufrufe pro Tick; jeder davon macht den vollen Scan (siehe Unsicher).

## Unsicher / Messung nötig

**Scene_1.md**

- SCN-102: Ob `poseFinalize`/Renderer Unsichtbares bereits ausspart (außerhalb dieses Slice), und wie viel Prozent der Frame-Zeit die Animationsphase in typischen Szenen überhaupt kostet.
- SCN-101/108/110: Tatsächlicher Anteil der malloc/free-Zeit gegenüber Sampling/FK; mit Profiler (`HE_PROFILE_SCOPE`) in einer Szene mit 100–500 Charakteren messen.
- SCN-103: Typische Keyframe-Anzahl je Kanal der Projekt-Clips (Mocap vs. handgemacht); Binärsuche lohnt erst ab ~16–32 Keys.
- SCN-112: Ob irgendwo außerhalb dieses Slice (AudioSource-System, Editor) beendete Stimmen abgeräumt werden; im gelesenen Code und per grep auf `isPlaying`/`stop` nur Sequence/Weather gefunden.
- SCN-113/114: Absolute Kosten von `propagateTransforms` im Leerlauf (`Transforms::propagate`-Profil) und von `CellStreamer::update` bei großem Manifest.
- `CellManifest::around`, `slicesEstimate` (`CellStreamer.cpp:156-157`): `1 + ceil(entities / k)` ergibt für `entities > k` immer eine Slice mehr als `ceil(entities / k)` – ob das beabsichtigt ist (z. B. Wurzel-Slice), ist ohne Blick in `sliceForAdditiveLoad` nicht klar; nur Schätzung für die UI.
- `AntiCheatTelemetry::~AntiCheatTelemetry` / `detach()` blockieren bis zum Timeout, falls ein `std::async`-Upload läuft (Future-Destruktor joint); da `kTimeoutMs` begrenzt, nur beim Beenden relevant – nicht gemessen.

**Scene_2.md**

- Größenordnung von SCN-201/202: Mikro-Benchmark (1.000 bis 10.000 Aufrufe `transform.getPosition` aus HorizonCode-Tick, Lua, Python) vor und nach der `Value`-Verkleinerung.
- SCN-203: Anzahl gebundener Entity-Instanzen in realen Projekten (Zielgröße); Profil von `EntityHost::tick` bei 1.000 Projektilen.
- SCN-204: Verbreitung von `entity.findByName` in Tick-Graphen; Szenengrößen (Anzahl `NameComponent`).
- SCN-206: Dauer einer Neuverteilung bei 50k/200k/500k Instanzen mit und ohne `seed != 0` (fbm) im Editor.
- SCN-208: Ob gängige Projekte `fs.watch` überhaupt nutzen; Kosten eines Scans von 4.096 Einträgen auf Windows (NTFS, `status` je Eintrag) und Netzlaufwerk.
- SCN-210: Verhalten bei Szenen mit sehr großen zugeklappten Gruppen ohne Renderables.
- SCN-215: Ob Skripte (Lua/Python) je außerhalb des Main-Threads laufen; der Kommentar bei `datetime::localParts` behauptet es, die Statics sprechen dagegen.
- `entity::distance` (`EngineApi.cpp:341-345`) benutzt `ScriptApi::getPosition`, laut `transform`-Gruppe (`:634,7075-7092`) die LOKALE Position; für parentete Entities wäre die Distanz damit nicht die Welt-Distanz. Das ist keine Audit-Kategorie, aber verdächtig; `ScriptApi::getPosition` (außerhalb dieses Slices) prüfen.
- `EntityHost::bind` (`EntityHost.cpp:117`): Schlägt `addCompiled/addLevels` fehl (`inst == 0`), kehrt die Funktion ohne Log mit 0 zurück, `spawn` zerstört die Entity dann stillschweigend; nur gelesen, nicht getestet.

**Scene_3.md**

- SCN-301: Anteil statischer Entities in `m_interp` im realistischen Spiel (Verhältnis ruhend zu bewegt) und tatsächliche Kosten von `propagateTransforms` bei gesetztem `dirty`.
- SCN-302: Speicher und Parse-Zeit pro State-Machine-Instanz; Größe von `AnimatorStateMachineGraph` und `BlendSpace`.
- SCN-303: Reale Snapshot-Zeit bei 8 Clients × 2000 Entities; Kosten von `BitWriter`-Konstruktion.
- SCN-304: Kosten von `startSound` (Kopie plus Mixer-Setup) bei langen Clips; ob die Kopie wirklich auf dem Spiel-Thread läuft.
- SCN-305: Kosten und Sperren von `setSoundPosition` im miniaudio-Wrapper (`AudioEngine.cpp`, nicht in diesem Slice). Ob gekindete Audioquellen/Listener vorkommen (lokale vs. Weltposition).
- SCN-308/309: Wie oft generierter Code über den Namenspfad (`fireEvent(name)`, `callApi`) statt über direkte Hooks läuft; nur mit Profil eines kompilierten Tick-Graphen bewertbar.
- `AnimationPose.h:207-214` (`blendPosesN(const std::vector<std::vector<JointTRS>>&, …)`): ob die Aufrufer die Pose-Puffer pro Frame wiederverwenden oder verschachtelte Vektoren neu anlegen (Implementierung in `AnimationPose.cpp`, außerhalb dieses Slices).
- Pure-Ausdruck-Neuberechnung im Codegen (`HcCodegen.cpp:1698` "re-emitted at every read"): Code-Größe wächst bei Fan-out potenziell exponentiell mit der Tiefe. Das ist durch die Parität zum Interpreter gebunden und nicht ohne Parity-Tests änderbar. Messung: Größe generierter Dateien bei typischen Graphen.

**Scene_4.md**

- SCN-406: Wie viele HE::api-Aufrufe pro Frame laufen tatsächlich über `invoke` (Interpreter vs. kompilierte Klassen, die laut `HcCodegen` Direktpfade haben)? Profiler-Scope um `ApiFn::invoke` einbauen.
- SCN-407: Ob die `std::function`-Kopien in `apiCtx` heap-allokieren, hängt von der Capture-Größe der Host-Lambdas und der STL ab (libstdc++ SSO 16 Byte, MSVC 56 Byte); Mikrobenchmark nötig.
- SCN-402: Anteil der LOD-Phase am Frame bei typischen Szenen (Terrain 256 Chunks + Props) aus dem Profiler (`HE_PROFILE_SCOPE_N`) ablesen; `docs/world-streaming-baseline-2026-10-06.md` §4.5 nennt den Posten, aber ohne aktuelle Zahl.
- SCN-415: `std::unordered_map` Default-Konstruktor auf MSVC (Sentinel-Allokation) – nur auf Windows prüfen; ggf. `ScriptComponent` beim Laden von 100 k Entities profilieren.
- `TransformComponent` (`Components/TransformComponent.h:19-54`) ist ca. 200 Byte (zwei `mat4`, vier `vec3`, Flags) in einem dichten Pool, obwohl viele Leser nur `position/rotation/scale` brauchen. Aufteilen von `worldMatrix`/`localCache` in einen eigenen Pool (SoA) könnte Cache-Misse in Physik-Write-Back und Gameplay senken; nur mit Messung (großer Umbau, viele Aufrufer).
- `WeatherComponent` (`Components/WeatherComponent.h:143-145`) trägt einen `std::mt19937` (~2,5 KB) und einen Partikelvektor inline; da es nur ein Weather-Entity gibt, ohne Wirkung, aber der Pool-Platz pro Instanz ist unnötig groß, falls je mehrere Instanzen erlaubt werden.
- X-Macro-Listen für Environment-Felder (`EngineApi.h:1452-1513`): vier Listen, die an mindestens vier Stellen expandiert werden (Deklaration, Implementierung, Registry, Anzeigenamen). Das ist bewusst „single source of truth“; ein constexpr-Array von Deskriptoren wäre lesbarer, aber ohne Typsicherheit der generierten Getter/Setter – kein Handlungsbedarf ohne Anlass.
- SCN-403: Ob `GameReplication::sendSnapshots` bei realistischen Spielerzahlen (≤ 8) und Entities (≤ 2 000) tatsächlich messbar ist, hängt vom Spiel ab; Stats `entitiesSent/entitiesCulled` liefern die Größenordnung.

**Scene_5.md**

- SCN-501: tatsächliche Kosten von `replicatedVariablesOf`/`getVariable` (Kopie oder Referenz, interpretiertes vs. generiertes Backend) – Profil `NetGameSession::update` mit 200-1.000 replizierten Entities.
- SCN-502: Anteil von `LODSystem::update` an einem Frame mit ~1.000 Terrain-Chunks/Props; ob `propagateTransforms` im selben Tick vorgezogen werden kann.
- SCN-503: Kosten eines `PhysicsWorld::raycast`-Aufrufs (Locking, Broadphase) und typische Partikelzahl mit `collisionEnabled`.
- SCN-506: Szenenladezeit-Anteil von Index-Pflege und `UUID::generate()` (RNG-Quelle, evtl. Mutex/`random_device`).
- SCN-514: ob je große Float-Arrays repliziert werden.
- `std::shared_ptr` für `nmc.navMesh`/`navQuery` mit Lambda-Deleter (NavigationSystem.cpp:380, 389): Eigentum wird offenbar geteilt (Komponentenkopien in Undo/Szenenladen); ob `unique_ptr` genügt, ist in diesem Slice nicht entscheidbar.
- `WaterField::Delta` (WaterField.h:545-554) kopiert komplette `Body`-Listen (inkl. `polygon`, `GroundPatch::delta`) vorher/nachher je Strich: Speicher pro Undo-Schritt; nur relevant, falls Delta tatsächlich verwendet wird (Header sagt: Undo nutzt Welt-Snapshot).

**Scene_6.md**

- SCN-601: Anteil der Write-back-Schleife an der Frame-Zeit bei 20 000 Bodies (Quelle: Kommentar Z. 3300-3303, „ein Fünftel"); Verfügbarkeit/Signatur von `GetActiveBodiesUnsafe` in der vendorten Jolt-Version.
- SCN-602: tatsächliche Lock-Contention bei 4-8 Jolt-Workern und dichten Haufen (Referenz: `docs/physics-threading-baseline.md`).
- SCN-603: Häufigkeit identischer Mesh/Hull-Collider in realen Szenen; RAM-Ersparnis pro Shape.
- SCN-604: Anzahl Seile in realen Szenen; Kosten des Hashs vs. Rebuild.
- SCN-605: Allokationsanteil an `AnimationStateMachine`/`AnimationBlend`-Scope bei Crowds.
- SCN-609: Zeitanteil der Einzel-Inserts an `initialize()`; Determinismus bei geänderter Tree-Struktur.
- SCN-611: Defaultwert von `PhysicsBudgetConfig::frameMs` (ob die Sortierung standardmäßig läuft) steht in `PhysicsBudget*.h`, nicht in diesem Slice.
- SCN-615: Wie viele IK-Charaktere realistisch gleichzeitig sichtbar sind; Verhalten bei Distanz-Cutoff ist eine Designentscheidung.
- SCN-620: Gesamtzahl/Tiefe der HorizonCode-Klassen in großen Projekten.
- SCN-621: Anteil des seriellen Scanners an der Szenenladezeit (Messung mit `piecesUsed`/Profil-Scope).
- `PlayerSettings.cpp`: Die drei fast gleichen `effectiveX/applyX`-Paare (Deadzone, VSync, Fullscreen) ließen sich über eine kleine Hilfsvorlage zusammenfassen. Das wurde als Banalität nicht aufgenommen.

**Scene_7.md**

- SCN-703: tatsächliche Kosten von `apiCtx` pro Aufruf (SBO der `std::function`-Kopien, Größe von `Ctx`/`Value` auf MSVC/libc++/libstdc++); Mikro-Benchmark nötig.
- SCN-701: Anteil der JSON-Kopien an der Snapshot-Zeit gegenüber CBOR-Encode und `serializeComponents` selbst.
- SCN-702: wie viele Instanzen/Records in realen Projekten vorkommen; ob `syncPrefabInstances` beim Speichern in der Praxis dominiert.
- SCN-710: ob die Schleife über `SkeletalMeshComponent` bei realistischen Charakterzahlen messbar ist.
- SCN-713: Verhalten pro Backend bei `replaceTexture` + `InvalidateTexture` (ob Teil-Upload existiert).
- SCN-716/SCN-723: Aufrufhäufigkeit pro Frame (RopeTrailSystem außerhalb des Slices).
- Nicht untersucht (außerhalb des Slices): `HE::rootMotionBeginFrame/poseBeginFrame` und die drei Skeletal-Treiber (jeweils eigene Views pro Frame ohne Dirty-Flag).

**Scene_8.md**

- `propagateTransforms` (TransformHierarchy.cpp:168-185): der Scan läuft pro Aufruf über ALLE `TransformComponent`s (memcmp von 36 Byte, laut Kommentar 50–200k Komponenten gemessen). Ein Schreib-Hook (Setter/Dirty-Bit) würde das ersparen, ist aber invasiv; Autoren haben das bewusst als Trade-off dokumentiert. Zu messen: Anzahl der `propagateTransforms`-Aufrufe pro Frame (CameraRig 3 Stellen, Navigation, Tick) und der Anteil im Profil `Transforms::scan`. Die Komponente trägt zusätzlich `localCache` (64 B) und den Schlüssel (36 B) neben `worldMatrix`; eine Trennung in eigene Speicher-Pools könnte den Scan cache-freundlicher machen (SoA), ist aber ohne Messung nicht zu empfehlen.
- `propagateFrom` (TransformHierarchy.cpp:68-82) ist rekursiv; Tiefe bis zu 65 536 Ketten sind in `recomputeFlagged` als Grenze behandelt, hier aber nicht (Stack-Overflow bei absurd tiefer Hierarchie). Niedrige Priorität.
- `WeatherSystem` Partikel-Pfad (WeatherSystem.cpp:303-330): O(maxParticles) pro Frame mit `sampleGround` je Partikel; die Obergrenze (`maxRainParticles`) und die GPU-Alternative (`gpuPrecip`) entscheiden; ohne Messung kein Fund.
- `AudioEngine::isPlaying` (AudioEngine.cpp:1086): ob ein Lock/atomarer Zugriff gegen den Audio-Thread nötig ist, nicht geprüft (SCN-814).
- Kosten von `emitUITextGlyphs` (Renderer/UIFont, nicht im Slice) für SCN-804.
- Tatsächliche Iterationszahl von `keepUnderSheet` (SCN-810) und tatsächliche Kosten von `allBodyCells`/`surfaceHash` bei res 1024–4096 (SCN-807).


---
# Funde (vollständig, nach Teilbericht)

## Scene Slice 1 – Animation*, AnimatorHost, AntiCheat/*, AudioEngine, CameraRigController, CameraShake, Cell*, TransformComponent

## Zusammenfassung

Der Code ist insgesamt sauber, gut kommentiert und an den Stellen, die jemand schon gemessen hat, bereits optimiert (`worldMatrixOf` mit Stack-Chain, inkrementelles `propagateTransforms`, EQ/Envelope-Handover ohne Lock auf dem Mixer-Thread). Die größten offenen Hebel liegen im Animations-Pfad: Jede spielende Animation wird jedes Frame vollständig gesampelt und gepost (kein Sichtbarkeits-/Distanz-/Dirty-Gate), mit mehreren `std::vector`-Allokationen pro Entity und Frame, linearer Keyframe-Suche und einer 3-Matrix-Zerlegung pro Gelenk. Zweiter Block: `AudioEngine::startSound` kopiert die komplette Clip-Datei pro Stimme, und fertige Stimmen werden von der Engine nie selbst abgeräumt. Dritter Block: `CameraRigController::update` und `CellStreamer::update` laufen jedes Frame mit mehreren O(N)-Scans bzw. Allokationen ohne Early-out. AntiCheat und CellSplit sind überwiegend kalter Code (nur Kleinkram).
Funde: Hoch 4 (SCN-101, 102, 103, 112), Mittel 10 (SCN-104 bis 111, 113, 114), Niedrig 7 (SCN-115 bis 121), insgesamt 21. Die IDs folgen der Reihenfolge Animation, Audio, Kamera, Cell; Schweregrad steht je Fund. Alles ohne Messdaten; Schätzungen sind als solche gekennzeichnet.

---

### SCN-101 – Pro Entity und Frame mehrere Heap-Vektoren in allen drei Pose-Treibern
- **ID**: SCN-101
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationSystem.cpp:71`; `src/HE_Scene/src/AnimationBlendSystem.cpp:114,118,153`; `src/HE_Scene/src/AnimationStateMachineSystem.cpp:428,435,454`; `src/HE_Scene/src/AnimationPose.cpp:369` (blendPosesN)
- **Problem**: In der Schleife über alle Animator-Entities wird pro Entity und Frame mindestens ein `std::vector<JointTRS>` in Skelettgröße neu angelegt (Clip: 1, Blend: 3, StateMachine: 2–3 plus `final_trs`). Jedes davon ist ein malloc/free-Paar mit ~`joints*40 Byte` (JointTRS = vec3+quat+vec3). Bei 100 animierten Charakteren mit 60 Gelenken sind das 100–300 Allokationen je Frame allein hier, dazu kommen die Vektoren in `poseFinalize` (nicht Teil dieses Slice). Die Kapazität könnte zwischen Entities und Frames wiederverwendet werden, da alle Vektoren nur innerhalb der Iteration leben.
- **Code-Auszug**:
```cpp
// AnimationSystem.cpp:71
std::vector<JointTRS> localTRS(mesh->skeleton.size());
// AnimationBlendSystem.cpp:114-118,153
std::vector<JointTRS> trsA(jointCount);
std::vector<JointTRS> trsB(jointCount);
std::vector<JointTRS> blended;
// AnimationStateMachineSystem.cpp:428,435,454
std::vector<JointTRS> trsOut(jointCount);
std::vector<JointTRS> final_trs;
std::vector<JointTRS> trsIn(jointCount);
```
- **Lösung**: Ein kleiner Scratch-Struct (`struct PoseScratch { std::vector<JointTRS> a, b, out; }`) als Member des jeweiligen Systems (Systeme sind bereits Klassen mit `update(...)`) bzw. `thread_local`/Parameter, vor jeder Entity per `assign(jointCount, JointTRS{})` zurücksetzen (behält Kapazität). In `blendTRS` darf `out` auf `a` aliasen (siehe SCN-109), damit `blended` entfällt (`blendTRS(trsA, trsB, alpha, trsA)`; `overrideJoint` liest `a` vollständig bevor `out` geschrieben wird, nur in den Randfällen `out = a`/`out = b` ist Self-Assign bzw. Kopie unkritisch). `final_trs = std::move(trsOut)` bleibt möglich, wenn `final_trs` ebenfalls Scratch ist (swap statt move).
- **Erwarteter Effekt**: Schätzung: 3–5 malloc/free je Entity/Frame entfallen; ein Teil der Frame-Zeit der Animationsphase (messen, vermutlich einstellige Prozent der Animationsphase bei vielen Charakteren).
- **Risiko / Verifikation**: Gering. Ergebnis bit-identisch (reine Pufferwiederverwendung). Voraussetzung: Systeme laufen nicht parallel auf mehreren Threads (sonst `thread_local`). Tests: bestehende Animation-Tests (`tests/` Animation/Blend/StateMachine), plus ein Test, der zwei Entities mit unterschiedlicher Skelettgröße hintereinander poset (Reset des Scratch).

---

### SCN-102 – Kein Gate für unsichtbare / ferne / unveränderte Animatoren
- **ID**: SCN-102
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationSystem.cpp:23-28`; `src/HE_Scene/src/AnimationBlendSystem.cpp:36-39`; `src/HE_Scene/src/AnimationStateMachineSystem.cpp:294-301` (State Machine hat nicht einmal ein `playing`-Flag)
- **Problem**: Die einzigen Filter in der View-Schleife sind `playing` und `sequencePosed`. Jeder spielende Animator wird jedes Frame mit der vollen Kette Sample → Root Motion → Layer → FK → IBM bearbeitet, unabhängig davon, ob die Entity im Frustum ist, wie weit sie von der Kamera entfernt ist (kein Update-Rate-Gate / Animation-LOD) oder ob sich die Pose gegenüber dem Vorframe geändert hat (z. B. pausierte StateMachine mit `playbackSpeed == 0`, Clip mit Dauer-Pose). Das ist der klassische „ECS-View ohne Dirty-Flag“-Fall im größten per-Frame-System dieses Slice. Ob `poseFinalize`/der Renderer irgendwo Sichtbarkeit nutzt, ist in diesem Slice nicht erkennbar.
- **Code-Auszug**:
```cpp
// AnimationSystem.cpp:23-28
for (auto [e, animator, smc] : view.each())
{
    if (!animator.playing) continue;
    if (smc.sequencePosed) continue;
// AnimationStateMachineSystem.cpp:294-296
for (auto [e, sm, smc] : view.each())
{
    resolveConfigIfNeeded(sm, cm);
```
- **Lösung**: (1) Sichtbarkeits-/Distanzinformation, die der Renderer/LODSystem ohnehin hat, als Komponentenfeld (`smc.animLodLevel` o. ä.) bereitstellen; die Treiber überspringen das Posen für weit entfernte Entities oder aktualisieren nur jedes n-te Frame (Zeit akkumulieren, damit Root Motion/Notifies weiterhin die volle Spanne bekommen – diese Teile NICHT überspringen). (2) Bei `speed == 0` und unverändertem Zustand (kein Transition, keine Parameteränderung) Pose nicht neu berechnen und die letzten `boneMatrices` stehen lassen. (3) StateMachine: `playing`/`paused`-Flag analog zu den anderen.
- **Erwarteter Effekt**: Schätzung: potenziell größter Einzelgewinn der Animationsphase in großen Szenen (50 %+ bei überwiegend unsichtbaren Charakteren); ohne Messung nicht belegbar.
- **Risiko / Verifikation**: Mittel bis hoch – verändert beobachtbares Verhalten (Pose steht still, Notifies/Root Motion dürfen nicht ausfallen, Shadow-Caster außerhalb des Kamerafrustums). Absichern: Tests, dass Notifies und Root Motion bei übersprungenem Posen identisch bleiben; Messen mit Profiler-Szene (z. B. 500 Animatoren, 90 % außerhalb des Frustums).

---

### SCN-103 – Lineare Keyframe-Suche pro Kanal und Frame
- **ID**: SCN-103
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationEval.cpp:12-21` (findBracket), Aufruf `:38`
- **Problem**: `findBracket` läuft für jeden Kanal (typisch 3 pro Gelenk) in jedem Frame von Index 0 linear bis zum passenden Keyframe: O(Keys) statt O(log Keys). Bei Clips mit hunderten Keys je Kanal (Mocap, 30–60 Hz) ist das der dominante Teil von `sampleClip`; die Laufzeit hängt außerdem von der Abspielposition ab (gegen Ende des Clips am teuersten).
- **Code-Auszug**:
```cpp
static size_t findBracket(const std::vector<float>& times, float t)
{
    size_t k = 0;
    for (size_t i = 0; i + 1 < times.size(); ++i)
    {
        if (times[i + 1] >= t) { k = i; return k; }
        k = i + 1;
    }
    return k;
}
```
- **Lösung**: Semantisch identische Binärsuche (Ergebnis = Index des ersten `j >= 1` mit `times[j] >= t`, minus 1; sonst `n-1`; n==1 → 0):
```cpp
static size_t findBracket(const std::vector<float>& times, float t)
{
    if (times.size() < 2) return 0;
    const auto it = std::lower_bound(times.begin() + 1, times.end(), t);
    return static_cast<size_t>(it - times.begin()) - 1;
}
```
  Optional zusätzlich ein Cursor-Hint pro Kanal (vorheriger Index, da Playhead monoton läuft) – braucht aber veränderlichen Zustand in den Assets, daher nicht empfohlen.
- **Erwarteter Effekt**: Schätzung: O(log n) statt O(n); bei 100–300 Keys je Kanal Faktor 5–20 für diesen Teil von `sampleClip`.
- **Risiko / Verifikation**: Gering, solange `times` aufsteigend sortiert ist (die lineare Variante toleriert unsortierte Daten, die Binärsuche nicht; Importer/`.hasset`-Loader sollten das garantieren, ggf. beim Laden prüfen). Test: Property-Test, der alte und neue Funktion über zufällige sortierte Zeitreihen inkl. Duplikaten, n = 0/1/2 und t vor/hinter dem Bereich vergleicht; vorhandene Sampling-Tests laufen lassen.

---

### SCN-104 – `sampleJoint` scannt alle Kanäle des Clips, mehrfach pro Frame
- **ID**: SCN-104
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationEval.cpp:100-107`; Aufrufer `src/HE_Scene/src/RootMotion.cpp:44,45,89,157`
- **Problem**: Um EIN Gelenk (die Wurzel) zu samplen, wird die komplette Kanalliste (3 × Gelenkzahl) linear durchlaufen. Für Root Motion passiert das pro Frame und Entity mehrfach (Anfang, Ende der Spanne, Frame 0; beim Blend zusätzlich pro Clip), bei Blend/StateMachine mit zwei Clips noch häufiger. Der Wurzel-Kanal ist aber ein fester, pro Clip konstanter Satz von maximal drei Kanälen.
- **Code-Auszug**:
```cpp
JointTRS sampleJoint(const AnimationClipAsset& clip, uint32_t jointIndex, float t)
{
    JointTRS trs;
    for (const auto& channel : clip.channels)
        if (channel.jointIndex == jointIndex)
            applyChannel(channel, t, trs);
    return trs;
}
```
- **Lösung**: Beim Laden/Importieren der Clips die Kanäle nach `jointIndex` sortieren (stabil, damit die Reihenfolge je Gelenk erhalten bleibt) und `sampleJoint` per `std::lower_bound`/`equal_range` arbeiten lassen – oder alternativ einen kleinen `std::vector<uint32_t> firstChannelOfJoint` im `AnimationClipAsset` ablegen. Die Änderung sollte im Asset-Loader (HE_Core) passieren; `sampleClip` braucht keine Ordnung.
- **Erwarteter Effekt**: Schätzung: pro Aufruf von O(3·J) auf O(log) – bei 60 Gelenken ~180 Vergleiche gespart, mal 3–6 Aufrufe je Root-Motion-Entity und Frame.
- **Risiko / Verifikation**: Gering–Mittel (Asset-Layout/Serialisierung berühren; reine Umsortierung im Speicher ist ohne Formatänderung möglich, aber Reihenfolge bei mehrfachen Kanälen desselben Pfads beachten: späterer überschreibt früheren). Test: `sampleJoint` vs. `sampleClip[joint]` auf Test-Clips.

---

### SCN-105 – Blend-System bewertet immer beide Clips, auch bei `blendAlpha` 0 oder 1
- **ID**: SCN-105
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationBlendSystem.cpp:113-120,130-151`; `src/HE_Scene/src/AnimationPose.cpp:13-20` (overrideJoint liefert an den Enden exakt `a` bzw. `b`)
- **Problem**: `trsA` und `trsB` werden jedes Frame vollständig gesampelt (inklusive Allokation, siehe SCN-101), obwohl `overrideJoint` bei `alpha <= 0` bzw. `>= 1` den jeweils anderen Clip ignoriert (`out = a` / `out = b`). Bei den üblichen Fällen „Blend steht auf einem Ende“ wird die Hälfte der Samplingarbeit verworfen.
- **Code-Auszug**:
```cpp
std::vector<JointTRS> trsA(jointCount);
if (clipA && clipA->duration > 0.0f)
    sampleClip(*clipA, std::fmod(t, clipA->duration), trsA);
std::vector<JointTRS> trsB(jointCount);
if (clipB && clipB->duration > 0.0f)
    sampleClip(*clipB, std::fmod(t, clipB->duration), trsB);
```
- **Lösung**: Nur die Seite samplen, die zum Ergebnis beiträgt (`alpha <= 0` → nur A; `alpha >= 1` → nur B). Root Motion und Notifies bleiben davon getrennt, weil `rootMotionSampleClip` die `trs*`-Pose für `lockRootJoint` benutzt: Dort die ungesampelte Seite lazy oder nur für die Wurzel (`sampleJoint`) bereitstellen, oder die Optimierung nur anwenden, wenn die Entity keine `RootMotionComponent` hat.
- **Erwarteter Effekt**: Schätzung: bis zu −50 % Sampling im Blend-System an den Endpunkten.
- **Risiko / Verifikation**: Mittel – `blendRootMotion(dA, dB, alpha)` und die Notify-Bookkeeping-Zustände (`notifiesPrimedA/B`) müssen identisch bleiben. Tests: Blend-Test mit alpha 0 / 1 / 0.5 inkl. Root Motion und Notifies, Ergebnis bit-vergleichen.

---

### SCN-106 – State Machine: String-Vergleiche und String-Map-Lookups pro Entity und Frame
- **ID**: SCN-106
- **Kategorie**: B
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/AnimationStateMachineSystem.cpp:264-269` (findState), `:271-282` (evalTransition), `:313`, `:376-379`, `:442`
- **Problem**: Der aktuelle Zustand ist ein `std::string` (`currentStateName`). Jedes Frame wird per linearem Namensvergleich über alle States gesucht (`findState`), danach über *alle* Transitions des Graphen iteriert und `t.fromState != sm.currentStateName` als String verglichen; für jede passende Transition folgt ein Lookup `sm.params.find(t.paramName)` in einer String-Map. Bei großen Graphen (viele Zustände/Transitions) und vielen NPCs ist das reine String-Arbeit ohne Informationsgewinn, da sich der Zustand nur bei einer Transition ändert.
- **Code-Auszug**:
```cpp
for (const auto& s : sm.resolvedGraph.states)
    if (s.name == name) return &s;
...
for (const auto& t : sm.resolvedGraph.transitions)
{
    if (t.fromState != sm.currentStateName) continue;
    if (!evalTransition(sm, t)) continue;
```
- **Lösung**: Beim Auflösen des Graphen (`resolveConfigIfNeeded`) Indizes bauen: `stateIndexByName`, pro State eine Liste von Transition-Indizes mit bereits aufgelöstem Param-Slot (`std::vector<float>` params + Name→Slot nur beim Auflösen und für Script-Zugriff). `currentStateName` bleibt als serialisierte Wahrheit, dazu ein gecachter `currentStateIdx` (Invalidierung bei `configDirty`, Transition-Ende und Name-Setzen durch Skripte). Vereinfachung: die Zustandsmaschine hört auf, Namen zu vergleichen.
- **Erwarteter Effekt**: Schätzung: O(States + Transitions) String-Compares/Frame/Entity → O(Transitions des aktuellen States) Float-Vergleiche. Klein pro Entity, aber linear in der Zahl der NPCs.
- **Risiko / Verifikation**: Mittel (Script-API `animator.setParam`, `sm.currentStateName` wird extern geschrieben, Serialisierung). Tests: bestehende StateMachine-Tests; neuer Test „Skript setzt `currentStateName` → nächster Frame verwendet neuen State“.

---

### SCN-107 – `AnimatorHost::bind()` läuft pro Entity und Frame, danach String-Zugriffe für Parameter
- **ID**: SCN-107
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationStateMachineSystem.cpp:307-311`; `src/HE_Scene/src/AnimatorHost.cpp:15-24,41-55,108-128`
- **Problem**: Das System ruft bei laufendem Host für jede Entity jedes Frame `sync->bind(e)` auf, dessen Idempotenz-Prüfung `assetOf` (Registry-Lookups + `cm.getAnimatorStateMachine(id)` Content-Lookup), `m_assetOf.find` und `m_byEntity.find` ausführt, bevor es zurückkehrt – das sind 3–4 Hash-Lookups pro Entity und Frame für Entities, die längst gebunden sind. In `fireUpdate` kommen zwei weitere Lookups hinzu, und pro Float-Variable des Graphen ein `getVariable(inst, name)` (Namensauflösung per String) plus `sm->params[name] = …` (String-Hash/Map-Einfügen).
- **Code-Auszug**:
```cpp
// AnimationStateMachineSystem.cpp:307-311
if (sync && sync->running())
{
    sync->bind(e);
    sync->fireUpdate(e, dt);
}
// AnimatorHost.cpp:126-127
for (const std::string& name : pit->second)
    sm->params[name] = m_runtime->getVariable(it->second, name).f;
```
- **Lösung**: `bind` nur bei Änderung aufrufen: der Host lauscht auf `on_construct<AnimatorStateMachineComponent>` bzw. das System ruft `bind` nur, wenn `sm.configDirty` oder `sm.stateMachineAssetId != resolvedFromAssetId` (beides existiert bereits in `resolveConfigIfNeeded`). Parameter: Namen→Variablen-Slot einmal in `bind` auflösen (`std::vector<std::pair<VarSlot, float*>>`).
- **Erwarteter Effekt**: Schätzung: ~4 Hash-Lookups je Entity/Frame entfallen; Parameterweitergabe von String-Hash auf Direktzugriff.
- **Risiko / Verifikation**: Mittel – das Verhalten „Entity bekommt mitten in der Session eine State Machine / tauscht das Asset und wird erfasst“ (Kommentar Z. 303-306) muss erhalten bleiben; Test: Asset-Wechsel zur Laufzeit, Entity nachträglich mit Komponente ausstatten.

---

### SCN-108 – IK/LookAt: pro `rotateJointModel` eine Heap-Allokation und ein Sweep über das Restskelett
- **ID**: SCN-108
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationPose.cpp:86-112` (refreshModelSubtree, Allokation `:98`); Aufrufer `src/HE_Scene/src/AnimationIk.cpp:60-70,160,161,168,178,219,295,314`
- **Problem**: `refreshModelSubtree` legt bei jedem Aufruf `std::vector<char> dirty(jointCount, 0)` an und sweept von `root` bis zum Ende des Skeletts. Ein Zwei-Knochen-IK-Solve ruft `rotateJointModel` dreimal (+ evtl. ein viertes Refresh bei `weight < 1`), ein LookAt mit Kette ein Mal pro Glied, ein Fußausrichten einmal. Pro Bein und Frame also 3–4 Allokationen + 3–4 Skelettsweeps, obwohl nur wenige Gelenke unter `root` liegen.
- **Code-Auszug**:
```cpp
std::vector<char> dirty(jointCount, 0);
dirty[static_cast<size_t>(root)] = 1;
for (size_t i = static_cast<size_t>(root); i < jointCount; ++i)
{
    const int32_t parent = mesh.skeleton[i].parent;
    if (!dirty[i]) { if (parent >= 0 && dirty[static_cast<size_t>(parent)]) dirty[i] = 1; else continue; }
```
- **Lösung**: (a) Kleinstpuffer ohne Heap: bei `jointCount <= 256` ein `std::array<uint8_t,256>`/`std::bitset` auf dem Stack, sonst Vektor; (b) Alternative ohne Zusatzspeicher: da Eltern vor Kindern liegen, genügt es, für `i > root` zu prüfen, ob `parent >= root && model_updated(parent)` – dazu reicht ein Bitset mit `jointCount - root` Bits; (c) langfristig: Subtree-Ende pro Gelenk im `SkeletalMeshAsset` vorberechnen (bei Depth-First-Ordnung ist der Subtree zusammenhängend; bei beliebiger Reihenfolge nicht – daher erst prüfen).
- **Erwarteter Effekt**: Schätzung: 3–4 mallocs je IK-Kette und Frame entfallen; Sweep-Länge bei kleinen Subtrees sinkt (nur wenn (c)).
- **Risiko / Verifikation**: Gering (a/b). Ergebnis muss bit-identisch bleiben (Kommentar Z. 44-46: „refresh must not drift“). Bestehende IK-Tests verwenden; Test mit Skelett > 256 Gelenken für den Fallback.

---

### SCN-109 – `localMatrix`: drei 4×4-Matrizen und zwei volle Matrixprodukte pro Gelenk
- **ID**: SCN-109
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationPose.cpp:47-53`, genutzt in `composeModelMatrices` `:56-69` und `refreshModelSubtree` `:108-110`
- **Problem**: Jedes Gelenk in jedem Frame baut `T`, `R`, `S` als volle `mat4` und multipliziert sie mit `T * R * S` (2 × 64 Multiplikationen), obwohl die Struktur bekannt ist (Spalten von R skaliert, Translation in Spalte 3). Das ist die innere Schleife der gesamten Skelettauswertung für jede animierte Entity.
- **Code-Auszug**:
```cpp
inline glm::mat4 localMatrix(const JointTRS& trs)
{
    const glm::mat4 T = glm::translate(glm::mat4(1.0f), trs.translation);
    const glm::mat4 R = glm::mat4_cast(trs.rotation);
    const glm::mat4 S = glm::scale(glm::mat4(1.0f), trs.scale);
    return T * R * S;
}
```
- **Lösung**: `glm::mat4 m = glm::mat4_cast(rotation); m[0]*=s.x; m[1]*=s.y; m[2]*=s.z; m[3]=vec4(t,1);` – mathematisch identisch, ca. ein Zehntel der Operationen. Für die `TransformComponent` gibt es in `TransformHierarchy.cpp:309-314` dieselbe Dreier-Zerlegung (dort Editor-/Szenen-Hierarchie, nicht Teil dieses Slice).
- **Erwarteter Effekt**: Schätzung: FK-Phase pro Entity 2–4× schneller; absolute Wirkung nur messbar bei vielen Gelenken × vielen Entities.
- **Risiko / Verifikation**: Mittel – Die Ergebnisse sind nur bis auf Rundungsfehler (nicht bit-identisch) gleich, und der Kommentar in `AnimationEval.cpp:122-126` verlangt Bit-Gleichheit zwischen IK- und Nicht-IK-Pfad (beide nutzen aber dieselbe `localMatrix`, bleibt also erfüllt). Goldene Tests mit exakten Zahlenwerten (Skinning-Snapshots) müssen neu bewertet werden; Toleranz-Tests laufen lassen.

---

### SCN-110 – Blend-Space-Gewichte: Sortieren + Heap bei jedem Frame; `blendPosesN` kopiert Posen
- **ID**: SCN-110
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationPose.cpp:230-246` (capAndNormalise), `:249-274` (weights1D, Allokation `:252`), `:339-346` (blendSpaceEvalOrder), `:348-374` (blendPosesN, Allokationen `:352,369`); Aufrufer `PoseSource.cpp:64,154`
- **Problem**: Pro Blend-Space-State und Frame: `weights1D` allokiert `byX` (n Indizes) und sortiert sie stabil – obwohl die Sample-Reihenfolge nach `x` konstant ist; `capAndNormalise` allokiert `order` und sortiert erneut; `blendSpaceEvalOrder` sortiert ein drittes Mal; `blendPosesN` kopiert die erste Pose (`out = poses[order[0]]`) und legt pro weiterem Sample einen neuen `tmp`-Vektor an (`blendTRS(out, poses[i], …, tmp); out = std::move(tmp)`).
- **Code-Auszug**:
```cpp
std::vector<size_t> byX(n);
for (size_t i = 0; i < n; ++i) byX[i] = i;
std::stable_sort(byX.begin(), byX.end(), [&](size_t a, size_t b) { return space.samples[a].x < space.samples[b].x; });
...
std::vector<JointTRS> tmp;
blendTRS(out, poses[i], wi / denom, tmp);
out  = std::move(tmp);
```
- **Lösung**: (1) Sortierung nach `x` einmal beim Parsen des BlendSpace (`blendSpaceFromJson`) in einem Member `byX` ablegen. (2) `blendPosesN`: in-place `blendTRS(out, poses[i], t, out)` (Aliasing ist elementweise sicher; `out.resize(count)` ändert die Größe nicht, wenn gleich lang), so entfällt `tmp`. (3) `order`/`byX` auf kleine feste Größe (`kBlendSpaceMaxActiveSamples`) auf dem Stack begrenzen.
- **Erwarteter Effekt**: Schätzung: 3–6 Allokationen und 3 Sorts je Blend-Space-Entity/Frame entfallen; Blend-Space-Sampling selbst (n Clips) dominiert weiterhin.
- **Risiko / Verifikation**: Gering. Ergebnis identisch (stabile Reihenfolge beibehalten, Tie-Break nach Index). Tests: bestehende Blend-Space-Tests, plus Gleichstand-`x`-Fall.

---

### SCN-111 – `AudioEngine::startSound` kopiert die komplette Clip-Datei pro Stimme
- **ID**: SCN-111
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AudioEngine.cpp:722-723` (Kopie), `:821` (Envelope-Kopie), `:823`/`:967-978`/`:992-1003` (EQ/Envelope-Kopien); Begründung im Header `src/HE_Scene/include/HorizonScene/AudioEngine.h:108-113`
- **Problem**: Jede `play()`-Variante legt eine eigene `std::vector<uint8_t>`-Kopie der Audiodaten an (`snd->bytes = bytes;`). Für PCM16 ist das die ganze Clip-Länge (Header: „multi-megabyte PCM copy per press“), für Vorbis die komprimierte Datei. Bei gestapelten Einmal-Sounds (Schritte, Schüsse, Regen aus `WeatherAudio`) wird bei jedem Start malloc + memcpy mehrerer KB–MB auf dem Game-Thread ausgeführt und pro aktiver Stimme dauerhaft Speicher gehalten. Dasselbe gilt für die Envelope-Kopie (`make_shared<const AudioEnvelope>(*envelope)`).
- **Code-Auszug**:
```cpp
auto snd = std::make_unique<ActiveSound>();
snd->bytes = bytes;
```
- **Lösung**: Audiodaten im `ContentManager` als unveränderlichen, referenzgezählten Blob (`std::shared_ptr<const std::vector<uint8_t>>`) halten; `ActiveSound` hält nur die `shared_ptr` (Lebensdauer geschützt gegen Umlagerung, das ist der im Header genannte Grund für die Kopie). `ma_audio_buffer`/`ma_decoder` lesen direkt daraus (`ma_audio_buffer_config.pData` ist bereits ein Zeiger ohne Kopie, `ma_decoder_init_memory` ebenfalls). Envelope/EQ analog über `shared_ptr` aus dem Asset teilen statt zu kopieren.
- **Erwarteter Effekt**: Schätzung: Spawn-Kosten pro Sound von O(Clipgröße) auf O(1); Speicher pro Stimme von Clipgröße auf ~0 für PCM-Voices. Real nur bei kurzen, oft gestarteten Clips relevant (messen).
- **Risiko / Verifikation**: Mittel – ändert die Asset-API von `ContentManager` (anderes Modul) und alle `play()`-Overloads. Absichern: Test, der ein Asset während einer laufenden Stimme neu lädt/entlädt (kein Dangling), ASAN-Lauf.

---

### SCN-112 – Beendete Stimmen werden nie von der Engine abgeräumt (Script-One-Shots bleiben liegen)
- **ID**: SCN-112
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/AudioEngine.cpp:1011-1024` (einziges Entfernen: `stop`/`stopAll`), `:905-908` (nur Warnung); `src/HE_Scene/include/HorizonScene/AudioEngine.h:63-66`; Aufrufer `src/HE_Scene/src/EngineApi.cpp:2675,2682,2685`
- **Problem**: `m_impl->sounds` wird ausschließlich über `stop()`, `stopAll()` und `removeBus()` verkleinert. Der Header sagt es offen: „Finished voices are still counted until something stops or reaps them“. Reaping gibt es nur bei `SequenceSystem.cpp:593-595` und `WeatherAudio.cpp:241,319`. Die Script-API `audio.play(path)` / `audio.playAt(...)` (EngineApi.cpp:2675/2682) liefert ein Handle, aber ein Skript-Einmalsound (Schuss, Treffer, UI-Klick) wird typischerweise nie gestoppt. Jede solche beendete Stimme hält weiter `ma_sound` + `ma_audio_buffer`/Decoder + die Datenkopie (SCN-111) und verbleibt im `unordered_map` (und in `busVoiceCount`). Das erreicht irgendwann `kVoiceWarnThreshold` (128) – die Warnung beschreibt genau diesen Zustand, behebt ihn aber nicht. Ob andere Teile des Spiels (z. B. ein AudioSource-System) reapen, ist in diesem Slice nicht sichtbar; im gelesenen Code gibt es keinen periodischen Aufruf.
- **Code-Auszug**:
```cpp
void AudioEngine::stop(uint64_t handle) { ... m_impl->sounds.erase(it); }
...
if (m_impl->sounds.size() >= kVoiceWarnThreshold)
    HE_LOG_THROTTLE(Audio, Warning, 10.0, "%zu simultaneous voices are alive — sounds are being started "
                    "faster than they are stopped", m_impl->sounds.size());
```
- **Lösung**: Eine `AudioEngine::reapFinished()` (pro Frame oder alle ~0,25 s vom Game-Loop gerufen), die über `sounds` iteriert und Stimmen entfernt, die `!paused && ma_sound_at_end()` sind und nicht loopen; zusätzlich einen `fireAndForget`-Parameter bzw. `playOneShot`, der die Stimme beim Ende automatisch freigibt. Handle bleibt gültig für Stimmen, die noch laufen.
- **Erwarteter Effekt**: Verhindert unbegrenztes Wachstum von Speicher und Map in langen Sessions; Schätzung: bei vielen Einmal-Sounds der wichtigste Audio-Befund.
- **Risiko / Verifikation**: Mittel – nach dem Reapen liefert `isPlaying(handle)` weiter `false` (bleibt gleich), aber `getSoundBus`/`getSoundCursorFrames` auf alte Handles ändern ihr Ergebnis. Test: Sound kurzer Clip starten, Mixer ziehen (`readMixedFrames`), `reapFinished()`, Map-Größe und Handle-Abfragen prüfen; Pausierte Stimme darf NICHT entfernt werden (Kommentar `AudioEngine.h:228-234`).

---

### SCN-113 – `CameraRigController::update` propagiert die Transform-Hierarchie zweimal pro Frame
- **ID**: SCN-113
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/CameraRigController.cpp:410` und `:581` (plus `:645` in `blendTo`)
- **Problem**: Der Kommentar nennt es selbst: „the extractor propagating again later costs a scan of the transforms“. `propagateTransforms` ist inkrementell, scannt aber den gesamten `TransformComponent`-Storage (`TransformHierarchy.cpp:415-440`, `scanned`-Pfad), um schmutzige Matrizen zu finden – bei N Transforms also zwei O(N)-Scans je Frame allein für die Kamera, zusätzlich zu dem des Extraktors. Nur die Kamera-Entity (und ggf. das Ziel) hat sich geändert.
- **Code-Auszug**:
```cpp
// :410
HE::propagateTransforms(world);
...
// :581 (nach dem Schreiben der Kamera-Transform)
HE::propagateTransforms(world);
```
- **Lösung**: Für den ersten Aufruf: nur nötig, wenn das Ziel (oder seine Vorfahren) im selben Frame geändert wurde – Dirty-Hinweis über `tt.dirty`/Strukturepoche prüfen; sonst entfällt er. Zweiter Aufruf: statt der globalen Propagation nur die Kamera-Entity und ihre Kinder aktualisieren (`worldMatrixOf` / ein exponiertes `propagateSubtree(world, e)`; `propagateFrom` existiert bereits intern, `TransformHierarchy.cpp:68`). Alternative: dem Aufrufer die Verantwortung geben (die Frame-Reihenfolge ist dokumentiert) und nur *einmal* propagieren.
- **Erwarteter Effekt**: Schätzung: −1 bis −2 Scans über alle Transforms pro Frame (Größenordnung 10–100 µs bei 10k–100k Transforms; messen).
- **Risiko / Verifikation**: Mittel – Konsumenten (`SceneSystems` LOD/Niederschlag) setzen frische Weltposition der Kamera voraus (Kommentar `:578-580`). Test: Kamera-Rig-Test, der `ct.worldMatrix` nach `update()` gegen `worldMatrixOf` vergleicht; Profiling mit `Transforms::propagate`-Scope.

---

### SCN-114 – `CellStreamer::update`: pro Frame Scans, Allokationen und vollständige Iteration über geladene Zellen
- **ID**: SCN-114
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/CellStreamer.cpp:316-323` (Anchor-Vektor), `:333`, `:341-354`, `:359-382`, `:437-470`, `:477`, `:576-582`, `:584-593`
- **Problem**: Selbst wenn sich kein Anchor über eine Zellgrenze bewegt hat und nichts in Arbeit ist, läuft jeden Frame: Allokation `std::vector<Anchor>{cam}` (`:322`), `reachesOf` (Vektor), Schritt 0 prüft `registry().valid()` für jede geladene und angefangene Zelle, Schritt 1 durchsucht (2r+1)² Zellquadrate (mit 4 Hash-Lookups je Quadrat: `m_index`, `m_loaded`, `m_pending`, `m_failed`) pro Anchor *und* Lookahead-Punkt, danach Sort + `unique` auf `wanted`; Schritt 3 iteriert alle Pending-Zellen, Schritt 5 alle geladenen Zellen und berechnet `nearer()`; am Ende werden die Stats über alle Pending-Zellen neu gezählt. Außerdem wird `SceneSerializer ser;` jedes Frame konstruiert (`:477`), auch wenn `ready` leer ist.
- **Code-Auszug**:
```cpp
update(world, std::vector<Anchor>{ cam }, budgetMs);
...
SceneSerializer ser;   // :477, ohne Prüfung auf ready.empty()
...
for (const auto& [k, root] : m_loaded)
{
    const CellManifest::Cell& cell = m_manifest.cells[m_index[k]];
    if (nearer(cell.x, cell.z) > m_manifest.unloadRadius) gone.push_back(k);
}
```
- **Lösung**: (1) `SceneSerializer` erst konstruieren, wenn `ready` nicht leer ist, bzw. als Member. (2) Early-out: Signatur der Anchors (Zellindex von `here` und `ahead` je Anchor + Radius-Scale) merken; solange unverändert und `m_pending` leer ist und seit dem letzten Lauf kein Entity zerstört wurde (`world.structureEpoch()`), Schritte 1 und 5 überspringen (nur nach Zellwechsel neu berechnen). (3) Scratch-Vektoren als Member (`m_wanted`, `m_ready`, `m_gone`). (4) Schritt 0 nur bei geänderter `structureEpoch`.
- **Erwarteter Effekt**: Schätzung: Pro Frame ~5 Allokationen und (2r+1)²·4 Hash-Lookups weniger; im Stillstand nahezu 0 Aufwand. Bei kleinem `loadRadius` (wenige Zellen) nur wenige µs – messen.
- **Risiko / Verifikation**: Mittel – Early-out darf Lade-/Entlade-Reaktion auf Radiusänderung durch Skripte (Manifest wird nur in `begin` gesetzt, daher ok) nicht verzögern; Zellen, die von anderen zerstört werden (Schritt 0-Kommentar), müssen weiter erkannt werden. Tests: bestehende Cell-Streamer-Tests (`tests/` mit `CellStreamer`), plus „Anchor steht still, Zelle extern zerstört → wird neu geladen“.

---

### SCN-115 – `CellStreamer`: unnötige Kopien und `operator[]` auf Maps
- **ID**: SCN-115
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/CellStreamer.cpp:387,487,501,579` (`m_index[k]`, `m_pending[k]`), `:418` (`std::string(bytes.begin(), bytes.end())`), `:563` (`p->created.insert`), `:564,571` (Hooks bekommen Vektoren)
- **Problem**: `m_index[k]`/`m_pending[k]` sind `operator[]` auf `unordered_map` – bei fehlendem Key wird still ein Default eingefügt (`m_index` → Slot 0 → falsche Zelle), `find`/`at` wäre korrekt und ausdrückt „muss existieren“. Bei Text-Cells wird die gesamte Datei nochmals als `std::string` kopiert (nur Worker, aber doppelter Spitzenspeicher), `p->created` sammelt alle Entities der Zelle zusätzlich zu den Slice-Listen.
- **Code-Auszug**:
```cpp
const CellManifest::Cell& cell = m_manifest.cells[m_index[k]];
const std::shared_ptr<Pending> p = m_pending[k];
json scene = text ? parseSceneText(std::string(bytes.begin(), bytes.end())) : parseSceneCbor(bytes);
```
- **Lösung**: `m_index.at(k)` bzw. gespeicherten Index in `wanted` mitführen, `parseSceneText(std::string_view)`-Overload (bzw. `std::string(std::move…)`-freie Variante) einführen, falls das Parsing-Interface das erlaubt.
- **Erwarteter Effekt**: Robustheit (+), geringer Speicher-Peak beim Laden; Laufzeit vernachlässigbar.
- **Risiko / Verifikation**: Gering. Tests der Stream-Zustandsmaschine.

---

### SCN-116 – AudioEngine: Mixer-Thread-Kleinigkeiten (`runEq`, doppelter try_lock, String-Bus pro Stimme)
- **ID**: SCN-116
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AudioEngine.cpp:97-113` (runEq), `:236-256` (envRead Handover), `:355` (`std::string busName`), `:921-925,930-943` (routeFor/routeAsset)
- **Problem**: (a) `runEq` entdenormalisiert nach jedem Aufruf *alle* `kMaxBands * channels` Zustände, auch die ungenutzten Bänder – bei kleinen Puffern (512-Frame-Chunks, `kChunkFrames`) ist das relativ viel; die Schleife kann auf `eq.bands.size()*ch` begrenzt werden. (b) `envRead` nimmt für Kurve und EQ denselben Mutex nacheinander per `try_lock` (Kommentar Z. 204 behauptet „its own pending slot“, geteilt wird aber `pendingMutex`) – ein einziger `try_lock` mit anschließendem Swap beider Slots genügt. (c) Jede Stimme speichert `std::string busName`; `routeFor` und `routeAsset` bauen pro Play-Aufruf Strings (`std::function`-Lambda in `resolveBus`) und `m_impl->buses.find(busName)` hasht den String noch einmal.
- **Code-Auszug**:
```cpp
for (HE::BiquadState& s : state)
{
    if (std::fabs(s.z1) < 1.0e-20) s.z1 = 0.0;
    if (std::fabs(s.z2) < 1.0e-20) s.z2 = 0.0;
}
```
- **Lösung**: (a) Schleife über `state.begin() + eq.bands.size()*ch`; (b) beide Pending-Flags unter einem `try_lock` abarbeiten; (c) Bus als `BusData*`/Id statt String in `ActiveSound` halten (Namensvergleich nur in `removeBus`).
- **Erwarteter Effekt**: Sehr klein; nur wenn EQ-Voices zahlreich sind. Nicht ohne Messung angehen.
- **Risiko / Verifikation**: Gering. Audio-Render-Tests (`readMixedFrames`) vor/nach vergleichen.

---

### SCN-117 – `AnimationEval`: dupliziertes Wrapping, `composeBoneMatrices` alloziert `model` pro Aufruf
- **ID**: SCN-117
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/AnimationBlendSystem.cpp:20-26,116,120`; `src/HE_Scene/src/AnimationEval.cpp:86-88`; `src/HE_Scene/src/AnimationStateMachineSystem.cpp:364-365,512-513`; `src/HE_Scene/src/AnimationEval.cpp:118-130`
- **Problem**: Die Regel „wrap in [0,d) mit Korrektur für negative Reste“ existiert in vier Kopien (`wrapTime` im Blend-System, `advancePlayback`, zwei Stellen im StateMachine-System), und `blend.playbackTime` wird in `advancePlayback` gewrappt und danach noch einmal mit `std::fmod(t, clip->duration)` (Z. 116/120) pro Clip. `composeBoneMatrices` legt pro Aufruf einen `model`-Vektor an (nur Preview/Tests, nicht Hot Path).
- **Code-Auszug**:
```cpp
float w = std::fmod(t, duration);
if (w < 0.0f) w += duration;
```
- **Lösung**: Eine gemeinsame `wrapPlayhead(float t, float duration)` in `AnimationEval.h` (inline), überall verwenden; `composeBoneMatrices` mit optionalem `model`-Scratch-Parameter oder `thread_local`.
- **Erwarteter Effekt**: Weniger Code, kein Performanceeffekt.
- **Risiko / Verifikation**: Gering; Tests der Wrapping-Fälle (negative Speed, Dauer 0).

---

### SCN-118 – AntiCheat: Kleinkram (`check()` hasht pro Aufruf, lineare Report-Suche, `m_buffer` Erase vorn)
- **ID**: SCN-118
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AntiCheat/AntiCheatHost.cpp:296-310` (check), `:370-377` (findReport); `src/HE_Scene/src/AntiCheat/AntiCheatService.cpp:541-548` (findReport), `:160,252,261,269` (Observation.rule); `src/HE_Scene/src/AntiCheat/AntiCheatTelemetry.cpp:122-129` (Erase vorn); `src/HE_Scene/include/HorizonScene/AntiCheat/AntiCheatTelemetry.h:130`
- **Problem**: (a) `AntiCheatHost::check()` ist laut Kommentar ein per-Hit-Aufruf, hasht aber bei jedem Aufruf `rule` in `m_checkLogged.insert(rule)`, nur um einmal ein Debug-Log zu schreiben; (b) `findReport` ist eine lineare Suche über eine nach `id` aufsteigend sortierte `std::deque` (id wird mit `++m_nextReportId` vergeben) und wird im `pump()` je Report sowie von Script-Readern (`reportRule/Detail/...`) wiederholt aufgerufen; (c) `Observation.rule` wird mit `kindName(kind)` (`const char*`) zu `std::string` kopiert – bei „IntegrityMismatch“ (17 Zeichen) Heap-Allokation; (d) `m_buffer.erase(m_buffer.begin(), …)` auf `std::vector<std::string>` verschiebt alle Strings.
- **Code-Auszug**:
```cpp
if (m_checkLogged.insert(rule).second)
    HE_LOG_DEBUG(AntiCheat, "check '%s': no rule table in this build, passes", rule.c_str());
...
for (const Report& r : m_reports) { if (r.id == id) return &r; }
```
- **Lösung**: (a) Den Insert nur ausführen, wenn Debug-Logging aktiv ist (`HE_LOG_ENABLED(AntiCheat, Debug)`) oder `return true` vor dem Insert bei leerer Regeltabelle; (b) `std::lower_bound` auf `m_reports` nach `id`; (c) `std::string_view`/`const char*` in `Observation` oder `kindName` statisch halten; (d) `std::deque<std::string>` für `m_buffer`.
- **Erwarteter Effekt**: Vernachlässigbar bis klein (kalter Code, `kMaxReports` begrenzt). Nur mitnehmen, wenn die Dateien ohnehin angefasst werden.
- **Risiko / Verifikation**: Gering.

---

### SCN-119 – `CellSplit`: Entity-IDs als `json::dump()`-Strings; Fixpunkt-Schleife quadratisch
- **ID**: SCN-119
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/CellSplit.cpp:100` (idKey), `:313-322,371,417,425,504,517,523-529,546,563`, Fixpunkt `:511-532`, `std::function`-Rekursion `:332,364,393,489,570`
- **Problem**: Jede ID-Suche erzeugt per `v.dump()` einen neuen `std::string` und hasht ihn (mehrere hunderttausend Mal bei großen Szenen); die Schleife `for (bool changed = true; changed;)` geht in jeder Runde über alle Entities mit `idKey(...)` und mehr (`remove_if` mit `idKey(c)` je Kind) – O(Entities × Tiefe der Ordnerkette). `splitSceneIntoCells` ist eine Editor-/Offline-Operation (nicht pro Frame), daher niedrige Schwere. Außerdem wird `std::function` verwendet, ohne `<functional>` einzubinden (kommt transitiv).
- **Code-Auszug**:
```cpp
std::string idKey(const json& v) { return v.dump(); }
...
for (bool changed = true; changed;)
{
    changed = false;
    for (const json& e : *ents) { ... const std::string k = idKey(e["uuid"]); ...
```
- **Lösung**: IDs einmalig in `IdPair` (existiert schon: `asIdPair`/`IdPairHash`, `CellSplit.cpp:178-194`) umsetzen und alle Maps (`byId`, `nodeOfEntity`, `gone`, `moved`, `childrenLeft`) auf `IdPair` umstellen; Fixpunkt durch Bottom-up-Zählen der verbleibenden Kinder ersetzen (ein Durchlauf in umgekehrter Topologie). `#include <functional>` ergänzen.
- **Erwarteter Effekt**: Schätzung: Split großer Szenen (100k Entities) von Sekunden auf Bruchteile; kein Laufzeitgewinn im Spiel.
- **Risiko / Verifikation**: Mittel – Dateiformat und Reihenfolge der erzeugten Cell-Files müssen byte-identisch bleiben (Golden-Test: Split einer Beispielszene, Ausgabe gegen den bisherigen Stand diffen; Legacy-Zahl-IDs in `asIdPair` (`[hi, lo]` vs. „legacy number“, Z. 99) beachten).

---

### SCN-120 – `AnimationNotify`: Scan aller Notifies pro Segment und Entity
- **ID**: SCN-120
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationNotify.cpp:33-82` (emitSegment), `:92-143`
- **Problem**: Für jede spielende Entity mit Notifies läuft jedes Frame `emitSegment` über die komplette Notify-Liste des Clips (lineares Filtern mit `inSpan`), auch wenn die Zeitspanne (typisch 1/60 s) fast nie ein Ereignis enthält; bei Wrap/Lap-Spannen kommen weitere Segmente dazu. Das `pending`-Vektor-Heap-Objekt entsteht nur, wenn tatsächlich ein Ereignis trifft (erstes `push_back`), ist also kein Dauerbefund.
- **Code-Auszug**:
```cpp
std::vector<Pending> pending;
for (const AnimationNotify& n : notifies)
{
    if (n.duration > 0.0f) { ... }
    else if (inSpan(n.time)) pending.push_back({ n.time, Kind::Fire, &n.name });
}
```
- **Lösung**: Notifies beim Laden nach `time` sortieren (nur Reihenfolge der Ausgabe ist ohnehin durch `stable_sort` kanonisch) und per `lower_bound` auf das Intervall `[lo, hi]` einschränken; `Begin/End` für Zustände mit `duration > 0` bräuchten zusätzlich einen Sortierschlüssel für `time+duration` oder getrennte Liste.
- **Erwarteter Effekt**: Klein; Gewinn nur bei Clips mit vielen Notifies. Nicht ohne Messung umsetzen.
- **Risiko / Verifikation**: Mittel – Reihenfolge-/Randfallsemantik (`closedFrom`, Rewind) ist subtil; bestehende Notify-Tests (Wrap, Rewind, 64-Runden-Guard) müssen unverändert bestehen.

---

### SCN-121 – `resolveConfigIfNeeded` kopiert den geparsten Graphen
- **ID**: SCN-121
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationStateMachineSystem.cpp:213-240`
- **Problem**: `graph = std::move(parsed)` (Z. 222) wird richtig bewegt, danach aber `sm.resolvedGraph = graph;` (Z. 240) als Kopie zugewiesen, und `graph` danach nie wieder gebraucht – ein kompletter Deep-Copy aller States/Transitions/Strings pro Auflösung. Läuft nur bei `configDirty`/Asset-Wechsel (selten), daher gering.
- **Code-Auszug**:
```cpp
sm.resolvedGraph       = graph;
```
- **Lösung**: `sm.resolvedGraph = std::move(graph);` (die Kopie ist durch das spätere Lesen von `sm.resolvedGraph` nicht nötig).
- **Erwarteter Effekt**: Einmalig pro Auflösung ein Deep-Copy weniger.
- **Risiko / Verifikation**: Keines; kompiliert unverändert, Tests laufen lassen.

---

## Gelesen

Ja, alle 20 Dateien des Slice vollständig gelesen (AnimationBlendSystem, AnimationEval.cpp/.h, AnimationIk, AnimationNotify, AnimationPose, AnimationPreview, AnimationStateMachineSystem, AnimationSystem, AnimatorHost, AntiCheat/{Host,Service,Telemetry}, AudioEngine (1250 Z.), CameraRigController, CameraShake, CellPhysics, CellSplit, CellStreamer, Components/TransformComponent.cpp); zusätzlich gezielt `TransformHierarchy.cpp:300-440`, `AudioEngine.h`-Kommentare und Aufrufer per grep geprüft.

## Scene Slice 2 – EngineApi.cpp, EntityActive/EntityHost/EntityVisibility, EnvironmentPush, FloatingOrigin, FlyCameraController, FoliagePaint, FoliageSystem, GameInstanceHost

## Zusammenfassung

Der Slice ist funktional sauber und sehr gut kommentiert; die Probleme sind weniger Logikfehler als Kosten an den Rändern. Der größte Hebel ist die Skript-Grenze in `EngineApi.cpp`: jeder Registry-Aufruf läuft über `std::function`, einen `std::vector<Value>` für Argumente und einen für das Ergebnis, mit einem 280-Byte-`Value` (drei `std::string`, zwei `std::vector`). Der Self-Default-Wrapper kopiert diese Argumentliste bei jedem unverdrahteten Entity-Pin ein zweites Mal. Das ist pro Tick und pro Aufruf aus HorizonCode, Lua und Python relevant. Daneben gibt es mehrere O(n)-Pfade pro Aufruf oder Frame ohne Dirty-Flag (`EntityHost::tick`, `findByName`, Outliner-Zeilen, `FoliageSystem`), synchrone I/O auf dem Update-Thread (Prefs, Watch-Polling) und eine vollständige Foliage-Neuverteilung pro Mal-Frame im Editor. Funde: 1 Hoch, 9 Mittel, 8 Niedrig. Alle Zeilenangaben sind gegen den aktuellen Stand verifiziert; Größenordnungen sind Schätzungen und unter "Unsicher / Messung nötig" aufgeführt.

---

### SCN-201 – Skript-Dispatch: zwei Vektor-Allokationen und ein 280-Byte-`Value` pro API-Aufruf
- **ID**: SCN-201
- **Kategorie**: B / C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:5958-6017` (Leser `aF/aI/aS/aV3`), `:6021-6029` und fast alle Zeilen bis `:8580` (Registry), `src/HE_Core/include/HorizonCode/HorizonCode.h:93-124` (`Value`)
- **Problem**: Jede Registry-Zeile ist ein `[](Ctx&, const std::vector<Value>&) -> std::vector<Value>`. Der Aufrufer baut einen Argumentvektor (Heap), der Rückgabewert ist wieder ein `std::vector<Value>` (Heap, `VV{ Value::ofFloat(...) }`). `Value` hat 280 Byte (hand nachgerechnet, passt zu `sizeof`): drei `std::string` (`s`, `keyTypeName`, `typeName`), zwei `std::vector<Value>` (`items`, `keys`) und vier Vektorfelder, die nur für einen Typ benutzt werden. `Value::ofX` konstruiert und kopiert diesen Block, `aS()` kopiert Strings pro Argument (`return k < a.size() ? a[k].s : std::string()`). Ein `transform.getPosition` aus einem Tick-Graph bewegt so 12 Byte Nutzdaten über mindestens zwei Heap-Allokationen und ca. 560 Byte Kopierarbeit. Die Registry ist der einzige Weg für Lua/Python/HorizonCode, das trifft also jeden Skript-Frame.
- **Code-Auszug**:
```cpp
t.push_back({ "transform.getPosition", "Transform", false, {{"entity", P::Int}}, {{"position", P::Vec3}}, "HE::api::transform::getPosition",
    [](Ctx& c, const VV& a){ return VV{ v3(transform::getPosition(c, (Entity)aI(a, 0))) }; } });
```
- **Lösung**: (1) Billig und verhaltenserhaltend: `Value` verschlanken, indem Container-/Typnamen-Zustand in einen selten genutzten Unterblock wandert (`std::unique_ptr<ContainerPayload>`), das Skalar-`Value` schrumpft dann auf ca. 100 Byte und `ofX` bleibt allokationsfrei. (2) `aS()` als `const std::string&` mit statischem leerem String zurückgeben (und die C++-Funktionen nehmen ohnehin `const std::string&`), das spart die Stringkopie pro Argument. (3) Optional und größerer Umbau: ein zweiter, typisierter Fast-Path für die häufigsten Zeilen (Getter/Setter mit Int/Float/Vec3), den der HorizonCode-Interpreter und die Codegen bevorzugen (`cppCall` existiert bereits als Feld `"HE::api::..."`, der generierte C++-Code umgeht die Registry also schon).
- **Erwarteter Effekt**: Schätzung: 2 bis 4 Heap-Allokationen und etwa 0,3 bis 1 KB Kopie weniger pro Aufruf; sichtbar erst bei hunderten Aufrufen pro Frame. Messen mit einem Graph, der 1000 `getPosition` pro Tick ausführt.
- **Risiko / Verifikation**: `Value` ist ABI-nah (Hc-Codegen `HorizonCodeGenSupport.h` dupliziert Teile, Kommentar in `HorizonCode.h:99-103`). Tests: `test_engine_api` (Arity pro Zeile), die HorizonCode-Paritätstests, Save-Roundtrip (`valueToJson/valueFromJson`).

### SCN-202 – Self-Default-Wrapper kopiert die komplette Argumentliste und verdoppelt die `std::function`-Ebene
- **ID**: SCN-202
- **Kategorie**: C / B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:8488-8527` (Entity), `:8550-8577` (Widget)
- **Problem**: Der Post-Pass umhüllt jede Zeile mit erstem Parameter `entity` (rund 60) in ein weiteres `std::function`, das die Original-`invoke` per Move einfängt. Der Kommentar sagt, der Pin sei bewusst meist unverdrahtet ("for every read of its own speed"), der Pfad `a[0].i == 0` ist also der Normalfall für Self-Aufrufe. Dort passiert `VV withSelf = a;`: tiefe Kopie aller Argumente (je 280 Byte plus String-Heap) nur, um Element 0 zu überschreiben. Danach `entity::self(c)` mit Runtime-Lookup. Zusätzlich zwei verschachtelte `std::function`-Aufrufe pro API-Aufruf.
- **Code-Auszug**:
```cpp
VV withSelf = a;
withSelf[0] = Value::ofInt(static_cast<int>(me));
return inner(c, withSelf);
```
- **Lösung**: Die Substitution an die Aufrufstelle verlegen, an der `a` ohnehin beschreibbar ist (ApiFn::invoke nimmt `const VV&`; eine zweite Signatur `invoke(Ctx&, VV&)` oder ein `selfDefault`-Flag, das der Dispatcher selbst auswertet). Minimal: `inner` mit einem veränderbaren Argumentvektor aufrufen (`VV withSelf(a.begin(), a.end())` ist dieselbe Kopie, deshalb besser den Aufrufer `VV` per Wert übergeben lassen, wo er es ohnehin gebaut hat). Alternativ den Flag `params[0].selfDefault` (existiert schon) im Dispatcher prüfen, dann entfallen beide Wrapper-Lambdas.
- **Erwarteter Effekt**: Schätzung: eine Allokation plus Kopie von ca. 300 Byte pro Argument weniger bei jedem Self-Aufruf; ein indirekter Aufruf weniger.
- **Risiko / Verifikation**: Das Verhalten ist durch Tests belegt (Kommentar nennt `test_engine_api`); die Reihenfolge der Substitution im Dispatcher (Lua/Python/HorizonCode teilen `ApiFn::invoke`) muss erhalten bleiben. Test: Entity-Pin leer lassen, Aufruf aus Klasse und aus Text-Skript (Warnpfad `me == 0`).

### SCN-203 – `EntityHost::tick`: jeden Frame alle gebundenen Entities validieren, kopieren und ticken, ohne Ereignis- oder Dirty-Pfad
- **ID**: SCN-203
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EntityHost.cpp:228-264`, `src/HE_Scene/include/HorizonScene/EntityHost.h:110,151-155`
- **Problem**: Pro Frame: (a) alle Einträge von `m_byEntity` (`std::unordered_map`, knotenbasiert, cache-unfreundlich) durchlaufen und `registry().valid()` prüfen, nur um zerstörte Entities zu finden; (b) alle Instance-IDs in `m_tickScratch` kopieren (hier immerhin ohne Neuallokation); (c) `alive()` + `fireTick()` für jede Instanz, egal ob die Klasse überhaupt ein Tick-Ereignis besitzt. Der Kommentar begründet das Reaping mit "every path that removes an entity is covered": das geht auch ereignisgetrieben (`registry.on_destroy<>()` bzw. ein Entity-Destroy-Hook in `HorizonWorld::destroyEntity`), dann kostet ein Frame ohne Zerstörung nichts.
- **Code-Auszug**:
```cpp
for (const auto& [raw, inst] : m_byEntity)
    if (!m_world->registry().valid(static_cast<Entity>(raw)))
    { dead.push_back(inst); ... }
...
for (const auto& [raw, inst] : m_byEntity) m_tickScratch.push_back(inst);
for (const HorizonCode::InstanceId inst : m_tickScratch)
    if (m_runtime->alive(inst)) m_runtime->fireTick(inst, dt);
```
- **Lösung**: (1) Reaping über `on_destroy` einer ohnehin immer vorhandenen Komponente (`EntityIdComponent` ist bereits mit Signalen verdrahtet, siehe `HorizonWorld.cpp:30-32`) in eine kleine Todesliste schreiben; `tick` arbeitet nur diese Liste ab. (2) `m_byEntity` als flacher Vektor `{raw, inst}` halten (`instanceOf` mit sortiertem Vektor oder `flat_map`), ein einziger linearer Durchlauf für Tick. (3) Instanzen ohne Tick-Handler beim Binden markieren und nicht in die Tick-Liste aufnehmen (braucht Auskunft von `Runtime`, siehe Cross-Modul).
- **Erwarteter Effekt**: Schätzung: bei 1.000 Instanzen etwa 20 bis 60 µs pro Frame für Reaping/Kopie; der Tick selbst bleibt. Wächst linear mit Entity-Klassen-Instanzen (Projektile, NPCs).
- **Risiko / Verifikation**: Reihenfolge der Ticks und "Spawned während Tick tickt ab nächstem Frame" müssen erhalten bleiben. Tests zu EntityHost (Spawn/Destroy während Tick, Destruct-Ereignis nach Outliner-Löschung).

### SCN-204 – Lineare Suchen pro Skript-Aufruf: `findByName`, `mainCameraEntity`, `envOf`
- **ID**: SCN-204
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:346-353` (`findByName`), `:2378-2389` (`mainCameraEntity`, von allen 20 `camera::*`-Funktionen und `rigOf` genutzt), `:2587-2593` (`envOf`, von allen `env::get*/set*`)
- **Problem**: `entity.findByName` iteriert alle `NameComponent`s mit `std::string`-Vergleich pro Aufruf, O(n) pro Aufruf, und "Find By Name im Tick" ist das typische Skript-Muster. `mainCameraEntity` iteriert bei jedem Kamera-Getter/Setter (mehrere pro Frame bei einer Kamerasteuerung) die Kameras, `envOf` die `EnvironmentComponent`s. Beide Views sind klein, kosten aber pro Aufruf Pool-Lookup plus Iteration, und es gibt keinen Dirty-Flag oder Cache.
- **Code-Auszug**:
```cpp
auto view = c.world->registry().view<NameComponent>();
for (auto [e, nc] : view.each())
    if (nc.name == name) return (Entity)e;
```
- **Lösung**: `findByName`: Namensindex in `HorizonWorld` (`unordered_multimap<string, entity>`), gepflegt über `on_construct/on_update/on_destroy<NameComponent>` genau wie bereits der Id-Index (`HorizonWorld.cpp:30-32`); "erste Übereinstimmung in Pool-Reihenfolge" wird durch kleinste Pool-Position oder Entity-ID beim Mehrfachtreffer erhalten. Kamera/Environment: pro Frame im `Ctx` oder in `HorizonWorld` die Main-Camera- und Env-Entity cachen und per `on_construct/on_destroy` oder Versionszähler ungültig machen.
- **Erwarteter Effekt**: Schätzung: `findByName` von O(n) auf O(1); bei 10.000 benannten Entities ca. 50 bis 150 µs pro Aufruf gespart. Kamera/Env: klein, aber ohne Risiko.
- **Risiko / Verifikation**: Doppelte Namen (Auswahlregel) und `on_update` bei Umbenennung im Inspector (schreibt `nc.name` direkt, ohne Signal; dort müsste `registry.patch<NameComponent>` genutzt werden oder der Index verifiziert beim Treffer). Test: umbenennen, zerstören, doppelte Namen.

### SCN-205 – Foliage: `cachedInstances` hält eine dritte Kopie aller Matrizen, die nur gezählt wird
- **ID**: SCN-205
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/FoliageSystem.cpp:180-184,224-235`, `src/HE_Scene/include/HorizonScene/Components/FoliageComponent.h:40-45`
- **Problem**: Bei jeder Neuverteilung entstehen drei `mat4`-Arrays zu je N×64 Byte: `cachedInstances` (Welt), `local` (transient) und `store->local` (nach Buckets sortiert). Außerhalb der Szene-Tests wird `cachedInstances` nur für `.size()` gelesen (`TerrainTools.cpp:1596,1599`, `InspectorPanel.cpp:3517`, `EditorApplication.cpp:7563`); der Renderer liest ausdrücklich `store`. Bei 200.000 Instanzen (die Warnschwelle im Code) sind das 12,8 MB nur für Zählen. Zusätzlich `reserve(count)` auch bei starker Maske, wo nur ein Bruchteil überlebt.
- **Code-Auszug**:
```cpp
foliage.cachedInstances.reserve(static_cast<size_t>(count));
...
local.push_back(m);
...
m[3] = glm::vec4(worldPos, 1.0f);
foliage.cachedInstances.push_back(m);
```
- **Lösung**: `cachedInstances` durch `uint32_t placedCount` ersetzen und die Welt-Matrizen bei Bedarf aus `store->local` + Terrain-Origin ableiten (Tests, die die Welt-Matrizen vergleichen, `tests/test_foliage.cpp:78,146,232,244,260`, bekommen einen Hilfs-Accessor, der `store->local` zurückrechnet; Bit-Gleichheit ist gegeben, da laut Kommentar `m[3]` nur um `origin` verschoben wird). `local` direkt in das sortierte Ziel schreiben, indem zuerst nur Zellen-IDs plus Index gesammelt werden, oder `local` nach dem Sortieren sofort freigeben (`std::vector<glm::mat4>().swap(local)`), damit der Spitzenverbrauch nicht drei Kopien beträgt.
- **Erwarteter Effekt**: Schätzung: Spitzen-RAM der Neuverteilung ca. ein Drittel geringer, dauerhaft minus N×64 Byte pro Layer; weniger Cache-Druck beim Scatter.
- **Risiko / Verifikation**: Public-Feld der Komponente (Editor, Tests, evtl. MCP/Serializer: `grep` zeigt nur die genannten Stellen, aber Hot-Reload-Module könnten es lesen). Tests: `test_foliage` (Anzahl, Bit-Gleichheit Maske vs. uniform, Origin-Verschiebung).

### SCN-206 – Foliage-Malen: komplette Neuverteilung des Layers in jedem Frame mit gedrückter Maustaste
- **ID**: SCN-206
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/FoliagePaint.cpp:116` (`fol.dirty = true`), `src/HE_Scene/src/FoliageSystem.cpp:143-235`, Aufrufer `src/HE_Editor/TerrainTools.cpp:737-756`
- **Problem**: Jede Pinselbewegung setzt das gesamte Layer dirty; `FoliageSystem::update` verwirft Instanzen und Store und verteilt O(count) neu, inklusive `terrainHeightAt` (bei `seed != 0` ein fbm pro Kandidat), `FoliagePaint::sample` (bilinear) und Wang-Hash, danach Counting-Sort und Bucket-Bounds. Das passiert synchron im Update-Thread pro Frame, solange gemalt wird. Der Code warnt selbst ab 200.000 Instanzen vor "multi-second editor freeze". Die Maske ändert aber nur ein Rechteck (`x0..x1`, `z0..z1` sind in `paint` schon berechnet), und Kandidat i hängt nur von `seed` und `i` ab, nicht von seinen Nachbarn.
- **Code-Auszug**:
```cpp
if (!foliage.dirty) continue;
foliage.dirty = false;
foliage.cachedInstances.clear();
foliage.store.reset();
++foliage.revision;
```
- **Lösung**: Stufe 1 (einfach): Während eines Strichs das Neuverteilen drosseln (z. B. höchstens alle 100 ms oder erst bei Maustaste-los, Vorschau nur über die Debug-Anzeige). Stufe 2: Dirty-Rechteck in der Maske mitführen (`dirtyMinX..dirtyMaxZ`); der Scatter ist deterministisch pro Kandidat, es lassen sich also nur Kandidaten neu bewerten, deren (lx, lz) im Rechteck liegen, und der Bucket-Store inkrementell aktualisieren. Stufe 2 ist aufwendig, Stufe 1 reicht meistens.
- **Erwarteter Effekt**: Schätzung: bei 100.000 Instanzen mehrere ms pro Frame während des Malens, bei 500.000 mehrere hundert ms. Messen mit `Foliage` unter einem 1 km-Terrain bei Dichte 0,1/m².
- **Risiko / Verifikation**: Gedrosselte Variante verzögert nur die Anzeige, nicht den Dateninhalt (Maske ist sofort aktuell). Test: Undo-Snapshot pro Strich (`snapshotNow`), `test_foliage`-Maskenfälle bleiben unverändert.

### SCN-207 – Spawn-Pfad: Wegwerf-`HorizonWorld`, doppeltes `resolveClassAsset`, Blob-Kopie pro Spawn
- **ID**: SCN-207
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EntityHost.cpp:129-156` (spawn), `:88-117` (bind), `:310-335` (`inheritedComponents`), `:337-412` (`defaultComponents`)
- **Problem**: Pro `spawn` einer Klasse: `loadAsset(classPath)` (String-Hash), `inheritedComponents` kopiert den ganzen `componentBlob` als `std::vector<uint8_t>` (`return asset.componentBlob;`), bei Klassen ohne eigenen Blob folgt `resolveClassAsset` und für jeden Vorfahren ein `loadAsset(ancestor)`; dann `bind()` macht `loadAsset(classPath)` nochmals und ruft `resolveClassAsset` erneut auf. Gibt es keinen vererbten Blob, baut `defaultComponents` jedes Mal eine komplette `HorizonWorld` (Konstruktor legt Entity, Komponenten, `reserveComponentStorage()` mit 12 Pools und das Widget-/Script-Subsystem an, `HorizonWorld.cpp:21-51`), fügt Komponenten hinzu, serialisiert und verwirft alles. Bei Projektilen, die pro Schuss gespawnt werden, ist das eine Spawn-Hitch-Quelle.
- **Code-Auszug**:
```cpp
HorizonWorld scratch;
const Entity root = scratch.createEntity(baseClass.empty() ? "Entity" : baseClass);
...
SceneSerializer ser;
return ser.serializeSubtree(scratch, root);
```
- **Lösung**: (1) `defaultComponents` ergebnisweise cachen, Schlüssel `baseClass` (Ergebnis hängt nur von `baseClass` ab, ist also reine Funktion; `static std::unordered_map<std::string, std::vector<uint8_t>>`, beim Hot-Reload/Projektwechsel leeren). (2) `inheritedComponents` als `const std::vector<uint8_t>&`/`std::span` auf den Blob zurückgeben, solange der Pool nicht umzieht (der Kommentar warnt vor Pointer-Invalidierung: dann pro Klasse einen `shared_ptr<const vector>` im Aufrufer halten, der Blob wird sowieso nur an `instantiatePrefab` gereicht). (3) `ResolvedClass` einmal pro Spawn lösen und an `bind` durchreichen (`bind(entity, classPath, const ResolvedClass&)`).
- **Erwarteter Effekt**: Schätzung: das Cachen von `defaultComponents` entfernt je Spawn eine komplette Welt-Konstruktion samt Serialisierung (vermutlich dreistellige µs bis ms); zweiter Resolve spart Pool-Lookups und Vektorkopien.
- **Risiko / Verifikation**: Cache-Invalidierung bei Änderung der Engine-Klassentabelle (`engineClassIsA`) ist statisch; sonst keine. Tests: `test_entity_host`/Create-Object-Tests, Spawn von Player-Character-Klasse ohne eigenen Blob.

### SCN-208 – `fs.watch`: bis zu 32 Verzeichnisscans mit `stat` pro Eintrag auf dem Update-Thread
- **ID**: SCN-208
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:3022-3042` (`stampOf`), `:3081-3102` (`readChildren`), `:3151-3214` (`pollWatches`); Grenzen `include/HorizonScene/EngineApi.h:1750-1756` (`kWatchIntervalSeconds = 1.0`, `kMaxWatches = 32`, `kMaxWatchEntries = 4096`)
- **Problem**: `pollWatches(dt)` wird aus dem Frame aufgerufen. Einmal pro Sekunde läuft pro Watch ein `directory_iterator` mit je einem `std::filesystem::status` + `file_size` + `last_write_time` pro Eintrag, also bis zu 3 Syscalls × 4096 × 32 = rund 390.000 Syscalls in einem Frame, danach wird eine `std::map<std::string, WatchStamp>` mit Heap-Knoten pro Eintrag neu aufgebaut. Der Frame, in dem das Intervall fällig wird, hitcht (blockierende I/O im Update-Thread). Die Queue-Helfer sind ebenfalls linear (`queueChange` dedupliziert O(n) gegen bis zu 256, `takeChange` ruft `erase(begin())`).
- **Code-Auszug**:
```cpp
for (WatchEntry& w : watches())
{
    const WatchStamp now = stampOf(w.path);
    ...
    std::map<std::string, WatchStamp> fresh;
    if (!readChildren(w.path, fresh)) { ... }
```
- **Lösung**: Das Scannen auf einen Hintergrund-Thread (ein bestehender `JobSystem`-Worker oder der HTTP-Worker-Stil) verlegen, der nur Änderungsereignisse in `changeQueue()` (hinter dem Mutex) einstellt; oder native Watcher (`inotify`/`ReadDirectoryChangesW`/FSEvents) pro Plattform. Billig: die Watches reihum verteilen (ein Watch pro Frame statt alle auf einmal), `directory_entry::status()/file_size()` aus dem Iterator nutzen (spart auf manchen Plattformen den zweiten Syscall) und `changeQueue` als `std::deque<std::string>` mit Set für das Dedup.
- **Erwarteter Effekt**: Schätzung: Hitch von möglicherweise zig ms (kaltes Dateisystem, Netzlaufwerk: Sekunden) pro Sekunde entfällt; nur relevant, wenn ein Projekt `fs.watch` benutzt (Standardfall: keine Watches, dann ist `pollWatches` ein früher Rücksprung).
- **Risiko / Verifikation**: Semantik "ein Ereignis pro Pfad und Drain", "ein Verzeichnis-Zeitstempel zählt nicht" und der Degradationspfad `tooBig` müssen gleich bleiben. Die vorhandenen fs.watch-Tests mit manipulierten mtimes (falls vorhanden; sonst neu schreiben) absichern.

### SCN-209 – `prefs.set*` schreibt bei jedem Aufruf die gesamte Datei synchron
- **ID**: SCN-209
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:4747-4753` (`store`), `:4771-4774,4776-4783`
- **Problem**: Jeder `setString/setNumber/setBool/remove/clear` ruft `store()`: `doc().dump(2)` (Pretty-Print), `create_directories`, `ofstream` öffnen/truncaten/schließen. Ein Einstellungsdialog, der beim Ziehen eines Sliders pro Frame `prefs.setNumber` ruft (sehr naheliegend), macht 60 Dateischreibzugriffe pro Sekunde auf dem Game-Thread. Der Kommentar nennt die Begründung (Absturzsicherheit), die ein Write-Behind mit Flush am Frame-Ende und beim Beenden ebenso erfüllt. Außerdem schreibt `fs::writeText` nicht atomar (truncate vor dem Schreiben), im Gegensatz zu `save::write`, das temp+rename nutzt: ein Absturz mitten im Schreiben zerstört die Prefs.
- **Code-Auszug**:
```cpp
void setString(Ctx&, const std::string& key, const std::string& value)
{ doc()[key] = value; store(); }
```
- **Lösung**: `store()` setzt nur `s_dirty = true`; ein `flush()` läuft entweder einmal pro Sekunde über die Frame-Schleife (ähnlich `pollWatches`) oder bei Shutdown/Fokusverlust, und schreibt atomar (temp + rename wie in `save::write`). Wert nur schreiben, wenn er sich wirklich ändert (`doc()[key] == value` → kein `dirty`).
- **Erwarteter Effekt**: Schätzung: pro Aufruf ein Datei-Open/Write/Close weniger (typisch 50 bis 500 µs, auf Netz-/Cloud-Sync-Ordnern ms); bei Slider-Ziehen 60× pro Sekunde.
- **Risiko / Verifikation**: Daten gehen bei hartem Absturz zwischen Flushes verloren (Zeitfenster ≤ 1 s), das ist eine Verhaltensänderung und gehört dem Projekt zur Entscheidung. Alternative ohne Verhaltensänderung: nur das "kein Update bei gleichem Wert" und atomares Schreiben. Tests: `prefs`-Roundtrip, Shutdown-Flush.

### SCN-210 – Outliner: `subtreeVisibility` (rekursiv, alle 7 Komponenten) und `isEntityActive` (Eltern-Kette) pro sichtbarer Zeile und Frame
- **ID**: SCN-210
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EntityVisibility.cpp:28-64`, `src/HE_Scene/src/EntityActive.cpp:13-27`; Aufrufer `src/HE_Editor/OutlinerPanel.cpp:1232,1562`
- **Problem**: Der Outliner zeichnet nur sichtbare Zeilen (Clipping), ruft aber pro Zeile und Frame `subtreeVisibility`, das den ganzen Teilbaum der Zeile durchläuft, solange keine sichtbare Komponente gefunden wird. Eine zugeklappte Gruppe mit vielen Kindern ohne Renderable (Trigger, Collider, leere Ordner) oder mit ausgeblendetem Inhalt kostet je Frame O(Teilbaum) × 7 `try_get`. `isEntityActive` läuft pro Zeile die Eltern-Kette hoch (bis 256 Hops, je zwei Lookups). Zusätzlich hat `setSubtreeVisible/subtreeVisibility` keine Tiefenbegrenzung, im Gegensatz zu `isEntityActive` (der Kommentar dort setzt den Baum als zyklenfrei voraus; `reparentEntity` verhindert Zyklen, aber eine korrupte Szene würde hier den Stack sprengen).
- **Code-Auszug**:
```cpp
for (entt::entity child : h->children)
{
    const Visibility cv = subtreeVisibility(reg, child);
    if (cv == Visibility::Visible) return cv;
    if (cv == Visibility::Hidden)  v = Visibility::Hidden;
}
```
- **Lösung**: Im Outliner den Teilbaum-Status nur neu berechnen, wenn sich etwas geändert hat (Versionszähler, den `setEntityVisible`/Komponenten-Änderungen bumpen, oder einmal pro Frame ein Bottom-up-Durchlauf über die sichtbaren Wurzelzeilen mit Ergebnis-Cache `unordered_map<entity, Visibility>`, der am Frame-Ende geleert wird). Für die Aktiv-Kette: Eltern-Status beim Zeilenabstieg mitreichen statt pro Zeile neu hochzulaufen (der Outliner geht ohnehin von oben nach unten). Iterativ mit explizitem Stack statt Rekursion.
- **Erwarteter Effekt**: Schätzung: relevant nur bei großen, zugeklappten Teilbäumen; dort von O(n) pro Frame auf O(1) pro Zeile. Messen mit einer Szene mit 20.000 Entities unter wenigen Gruppen.
- **Risiko / Verifikation**: Zeilenhöhe bleibt konstant (Projektregel). UI-Test `test_outliner_ui` mit Hidden/Visible-Mischung; Cache nur frame-lokal, damit keine Staleness.

### SCN-211 – `water.*`-Abfragen bauen pro Aufruf einen frischen `WaterIndex`
- **ID**: SCN-211
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:1574-1580` (`surfaceNow`), Nutzer `:1583-1601,1622-1640`; `src/HE_Scene/src/WaterQuery.cpp:33-82`; `PhysicsWorld.cpp:703` hält bereits einen eigenen, pro Physikschritt aktualisierten Index
- **Problem**: `water.surfaceAt/depthAt/flowAt/disturb` bauen jedes Mal `WaterIndex::build(world)`: `view<TerrainComponent>`, pro Landschaft mit Wasser `worldMatrixOf` (Hierarchie-Walk), `glm::inverse`, 3 Spaltenlängen, danach `std::vector<Entry>` (Heap) und `std::sort`. Viele schwimmende Objekte, die pro Frame ihre Wasserhöhe per Skript abfragen, zahlen das je Objekt. Das Grid wird nicht neu gescannt (`ensureBounds` nur in `regions()`), die Kosten sind also moderat.
- **Code-Auszug**:
```cpp
const HE::water::WaterIndex index = HE::water::WaterIndex::build(*c.world);
return index.surfaceAt(x, z, c.physics ? c.physics->simTime() : 0.0, out);
```
- **Lösung**: Den Index des `PhysicsWorld` (oder einen im `Ctx` gehaltenen, einmal pro Frame via `refresh` aufgefrischten) wiederverwenden; der Kommentar im Header sagt selbst "Call it once per physics step, and before a batch of queries". Nur Ersatz, wenn `c.physics` fehlt (Editor-Vorschau).
- **Erwarteter Effekt**: Schätzung: eine Allokation, eine Sortierung und eine 4×4-Inversion pro Landschaft und Aufruf weniger; nur messbar bei hunderten Abfragen pro Frame.
- **Risiko / Verifikation**: Der Kommentar nennt als Grund "cannot be left holding the component pointers of a landscape that a script has just deleted"; ein pro Frame aufgefrischter Index hat dasselbe Problem innerhalb des Frames. Eine Version/`signal`-Prüfung auf Terrain-Pools (on_destroy) oder `refresh` bei Strukturänderung nötig. Test: Landschaft im selben Frame löschen und abfragen.

### SCN-212 – `json.*`-Getter parsen den gesamten Text bei jedem Aufruf
- **ID**: SCN-212
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:4675-4713` (`getString/getNumber/getBool/has/count`), `:4655-4672` (`setAt`)
- **Problem**: Wer 6 Felder aus derselben JSON-Antwort (z. B. HTTP-Body) liest, parst sie 6×; jeder `setX` parst, ändert und serialisiert komplett. Jeder Aufruf kopiert zusätzlich den Text (`aS()` in der Registry, SCN-201).
- **Code-Auszug**:
```cpp
const nlohmann::json doc = parse(text);
if (doc.is_discarded()) return fallback;
const nlohmann::json* v = walk(doc, path);
```
- **Lösung**: Ein-Eintrags-Cache des zuletzt geparsten Textes (Länge + Hash/Inhalt vergleichen, 64-Bit-FNV über `text` kostet weniger als ein Parse) im Namespace; `thread_local`, falls Skripte parallel laufen. `walk` baut außerdem pro Schritt `std::string key`/`num` per `substr`; mit `std::string_view` entfällt das.
- **Erwarteter Effekt**: Schätzung: Faktor 3 bis 10 bei mehrfachem Zugriff auf dasselbe große JSON; sonst nichts.
- **Risiko / Verifikation**: Rein intern; Test: abwechselnd zwei Texte, leerer Text, ungültiger Text.

### SCN-213 – Input-Snapshot: pro Frame Namensvektor und `unordered_set<std::string>`, Action-Suche linear mit Stringvergleich
- **ID**: SCN-213
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:5740-5757` (`Snapshot::keys`), `:5762-5778` (`pushSdlSnapshot`), `:5909-5922` (`findAction`)
- **Problem**: `pushSdlSnapshot` läuft jeden Frame über alle Scancodes (`n` ca. 512), ruft für jede gedrückte Taste `SDL_GetScancodeName` und legt `std::string` in einen `std::vector<std::string>` ab, `setKeysDown` leert dann das `unordered_set<std::string>` (Knoten freigeben) und fügt neu ein (Knoten allokieren). `keyDown(name)` hasht den Namen pro Aufruf; `actionDown/Pressed/...` suchen linear mit `a.name == name` über alle Actions. Alles im Normalfall ein paar gedrückte Tasten und ein paar Dutzend Actions, also klein, aber es ist ein Pro-Frame-Allokationsmuster ohne Notwendigkeit.
- **Code-Auszug**:
```cpp
for (int sc = 0; sc < n; ++sc)
    if (ks[sc]) { const char* name = SDL_GetScancodeName((SDL_Scancode)sc); if (name && name[0]) down.emplace_back(name); }
...
s.keys.clear(); for (const auto& n : names) s.keys.insert(n);
```
- **Lösung**: Gedrückte Tasten als `std::bitset<SDL_SCANCODE_COUNT>`/`std::array<bool>` halten; `keyDown(name)` löst den Namen über `SDL_GetScancodeFromName` (oder eine statische Name→Scancode-Map, einmal gebaut) in den Index auf. Die Action-Liste als `unordered_map<string, ActionState>` oder sortierter Vektor.
- **Erwarteter Effekt**: Schätzung: 0 Allokationen pro Frame statt ca. 2 bis 8 bei gedrückten Tasten; Nutzen klein, Code wird nicht komplizierter.
- **Risiko / Verifikation**: Namensauflösung muss groß-/kleinschreibungs- und Aliasverhalten der bisherigen Strings (SDL-Namen wie "Left Shift") exakt erhalten. Test: `input.keyDown` mit allen vom Editor geschriebenen Namen.

### SCN-214 – Save-Pfad: JSON → String → JSON, tiefe Kopie der Entity-Blobs und synchrones Pretty-Print
- **ID**: SCN-214
- **Kategorie**: C / E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:539-563` (`saveState`: `j.dump()`), `:5310-5318` (`setEntityState`: erneutes `parse`), `:5319-5324` (`entityState`: `dump()`), `:571-584` (`applySavedState`: erneutes `parse`), `:5147-5157` (`write`: `j["entities"] = active()->entities`, `dump(2)`)
- **Problem**: Jede Entity wird beim Speichern als `json` gebaut, zu String serialisiert, wieder geparst und gespeichert; beim Laden umgekehrt (`dump` → `parse`). `write()` kopiert das gesamte `entities`-Objekt tief, nur um es in ein neues Wurzelobjekt zu hängen, und schreibt es dann pretty-printed auf dem Game-Thread. Bei 1.000 gespeicherten Entities sind das tausende Knoten-Allokationen und ein Frame-Hitch beim Autosave.
- **Code-Auszug**:
```cpp
return save::setEntityState(uuid, j.dump());          // 562
nlohmann::json j = nlohmann::json::parse(json, ...);  // 5314
j["entities"] = active()->entities;                   // 5157
```
- **Lösung**: Intern eine zweite Schnittstelle `setEntityState(uuid, nlohmann::json&&)` / `const nlohmann::json* entityStateJson(uuid)` ergänzen, die C-ABI-Variante (String) bleibt als dünne Hülle. `write()`: `entities` per `std::swap` in das Ausgabeobjekt heben und nach `dump` zurücksetzen, oder die Teile getrennt serialisieren. `dump(2)` → `dump()` für Spielstände (Größe, Zeit) oder Pretty-Print optional.
- **Erwarteter Effekt**: Schätzung: etwa halbierte Speicher-/Ladezeit der Entity-Zustände, kein zusätzliches Vollkopie-Spitzen-RAM.
- **Risiko / Verifikation**: Dateiformat und Schlüsselreihenfolge (nlohmann sortiert Objekte alphabetisch) müssen byte-identisch bleiben, wenn die Doku "save → load → save byte-identical" verspricht (Kommentar `:200-203`). Test: Roundtrip save→load→save vergleichen.

### SCN-215 – Globaler Prozess-Zustand in Funktions-Statics, ungleich synchronisiert
- **ID**: SCN-215
- **Kategorie**: F / E
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:2747` (Debug-Queue), `:3054-3057` (Watches), `:4475-4477` (Timer), `:4845` (aktiver Spielstand), `:5335-5338` (Szenen-Requests/Zonen), `:5695` (Player-Tabelle), `:5750` (Input-Snapshot); gesichert nur `http` (`g_mtx`)
- **Problem**: Mehr als zehn unabhängige Singletons ohne Besitzer (Welt/Session) und ohne Sperre, während der Kommentar bei `datetime::localParts` (`:4789`) von "script threads" im Plural spricht, und der HTTP-Teil ausdrücklich Mutex und Worker hat. Zusätzlich bleibt Zustand zwischen Editor-Play-Sessions erhalten (nur `clearWatches`, `cancelAll`, `clear` je Gruppe, ein einzelner Reset-Punkt fehlt). Das ist vor allem ein Wartbarkeits-/Korrektheitsrisiko; Performance-Auswirkung gering.
- **Code-Auszug**:
```cpp
std::vector<Timed>& queue() { static std::vector<Timed> q; return q; }
std::vector<WatchEntry>&  watches()     { static std::vector<WatchEntry> w;  return w; }
```
- **Lösung**: Eine `struct ScriptApiState` bündeln (Timer, Watches, Debug-Queue, Input, Player, Szenen-Requests, Save), vom Host pro Session erzeugt und über `Ctx` erreichbar, mit einer `reset()`-Methode; die freien Funktionen ohne `Ctx` (`timer::after`, `input::keyDown`, ...) holen sie über einen einzelnen `thread_local`/globalen Pointer. Das ist ein größerer Umbau und nur sinnvoll, falls mehrere Welten oder Threads vorkommen; ansonsten dokumentieren "nur Main-Thread".
- **Erwarteter Effekt**: Keiner in der Performance; Reduktion von Zustandslecks zwischen Sessions und Race-Risiko.
- **Risiko / Verifikation**: Hoch für den vollständigen Umbau (öffentliche API im Header, C-ABI-Tabellen), daher nur nach Entscheidung. Unsicher, ob wirklich mehrere Threads Skripte ausführen (siehe Messung/Klärung).

### SCN-216 – Kleinigkeiten in Pro-Frame-Systemen: FlyCamera, EnvironmentPush, FloatingOrigin
- **ID**: SCN-216
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/FlyCameraController.cpp:92`, `:15-19` (findCamera); `src/HE_Scene/src/EnvironmentPush.cpp:8-57`; `src/HE_Scene/src/FloatingOrigin.cpp:44-55`
- **Problem**: (a) `FlyCameraController::update` setzt `t.dirty = true` jeden Frame, auch ohne Maus- oder Tastenbewegung, und `findCamera` iteriert pro Frame alle Kameras; ein dauerhaft schmutziger Transform hält die Kamera in jedem Frame im Propagations-Pfad. (b) `makeEnvironmentSettings` kopiert eine Struktur mit ca. 70 Feldern pro Frame feldweise, und ändert dabei die Komponente (`env.timeOfDay`, `moonPhase`): ein "make"-Name mit Nebenwirkung; die Felderliste existiert parallel in `EnvironmentComponent`, in `IRenderer::EnvironmentSettings` und in den `HE_ENV_FIELDS_*`-X-Listen des API, jede neue Einstellung muss vier Stellen treffen. (c) `shiftWorldOrigin` subtrahiert den Shift von jedem `worldMatrix` (`:46-47`) und ruft direkt danach `invalidateWorldMatrices`, dessen Kommentar sagt, dass der nächste Durchlauf ohnehin alles neu berechnet; die handgeschriebene Subtraktion ist nur für Leser zwischen Shift und Propagation sinnvoll. Das Ereignis ist selten (einmal pro Kilometer).
- **Code-Auszug**:
```cpp
t.dirty = true;               // FlyCameraController.cpp:92
f.driven = true;
```
- **Lösung**: (a) `t.dirty |= (f.dx != 0 || f.dy != 0 || glm::dot(move, move) > 0.0f);`. (b) Aufteilen in `advanceDayNight(env, dt)` (mutiert) und `toSettings(const EnvironmentComponent&)`; mittelfristig dieselbe X-Liste (`HE_ENV_FIELDS_*`) auch für das `EnvironmentSettings`-Mapping verwenden. (c) Nichts tun oder Schleife streichen, falls kein Leser zwischen Shift und `propagateTransforms` existiert (zu prüfen).
- **Erwarteter Effekt**: Gering; (a) spart bei ruhender Kamera die Propagation und eventuelle Folgearbeit an `dirty`.
- **Risiko / Verifikation**: (a) Der Flug-Controller schreibt `rotation` auch bei dt>0 ohne Eingabe nicht neu; die Änderung ist verhaltensgleich. Tests für den Fly-Controller (Pitch-Clamp, Sprint) und Floating-Origin-Test (`worldMatrix` nach Shift) beibehalten.

### SCN-217 – Foliage: Einstellungs-Hash pro Frame und Layer, `coverage()` scannt die ganze Maske pro UI-Frame
- **ID**: SCN-217
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/FoliageSystem.cpp:36-56,130-136`; `src/HE_Scene/src/FoliagePaint.cpp:147-153`; Aufrufer `src/HE_Editor/TerrainTools.cpp:1599`, `InspectorPanel.cpp:3526`
- **Problem**: (a) `FoliageSystem::update` hasht pro Frame und Layer bytewise (FNV-1a, ca. 70 Byte) alle Darstellungs-Einstellungen, um `revision` hochzuzählen, weil Felder direkt (Inspector) beschrieben werden und kein Dirty-Flag existiert. Das ist robust und billig (wenige Layer), aber O(Layer) pro Frame ohne Änderung. (b) `FoliagePaint::coverage` summiert die komplette Maske (bis 4096² = 16,7 MB) bei jedem UI-Zeichnen, solange das Foliage-Panel offen ist; bei Standard 128² trivial. (c) `sortIntoBuckets` reserviert `store->buckets` nicht (`push_back` pro nicht-leerer Zelle; Obergrenze `min(cellCount, n)` bekannt).
- **Code-Auszug**:
```cpp
const uint64_t key = settingsKeyOf(foliage);
if (key != foliage.settingsKey) { foliage.settingsKey = key; ++foliage.revision; }
```
- **Lösung**: (a) Lassen, solange Layer-Anzahl klein; sonst Setter/`registry.patch` im Inspector. (b) Coverage in `FoliageComponent` cachen und zusammen mit Maskenänderungen (nur in `paint/fill/clear/ensure`, alles in einer Datei) aktualisieren, oder im Panel alle 0,25 s neu rechnen. (c) `store->buckets.reserve(std::min(cellCount, n))`.
- **Erwarteter Effekt**: (a)/(c) vernachlässigbar; (b) bei 4096² ca. 5 bis 10 ms pro Panel-Frame (Schätzung).
- **Risiko / Verifikation**: Gering; (b) braucht einen Test, dass `coverage` nach `paint`/`fillMask`/`clearMask` stimmt.

### SCN-218 – `EngineApi.cpp` ist ein 9.210-Zeilen-Monolith mit schweren Includes
- **ID**: SCN-218
- **Kategorie**: F / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:1-97` (Includes), gesamte Datei
- **Problem**: Eine Übersetzungseinheit enthält 40+ Gruppen (Physik, Wasser, Audio, Netz, HTTP, SQLite, PDF-Druck, Dialoge, Save, JSON, Zeit ...) und zieht `sqlite3.h`, `SDL3`, `nlohmann/json.hpp`, `Net/HttpsClient.h`, `AntiCheatHost.h`, `NetGameSession.h` und ca. 30 Komponenten-Header ein. Jede Änderung an einer Gruppe baut die ganze Datei neu; der Registry-Block (`:6021-8580`) ist 2.560 Zeilen `t.push_back({...})` plus eine zweite, parallele Tabelle `kNames` (ca. 480 Einträge, Kommentare bis `:8444`), die per doppelter Schleife (`strcmp` jeder Zeile gegen jeden Eintrag, ca. 800 × 480 einmalig beim Start) zusammengeführt wird. Namen und Zeile sind getrennt und können driften (der Fallback `fn.displayName = fn.id` verdeckt vergessene Namen).
- **Code-Auszug**:
```cpp
for (auto& fn : t)
{
    fn.displayName = fn.id; // fallback: never null, worst case the id shows
    for (const auto& [id, name] : kNames)
        if (std::strcmp(fn.id, id) == 0) { fn.displayName = name; break; }
}
```
- **Lösung**: Pro Gruppe eine Datei (`EngineApi_Physics.cpp`, `_Net.cpp`, `_Db.cpp`, `_Save.cpp`, ...), jede mit ihrer eigenen `registerXxx(std::vector<ApiFn>&)`-Funktion, in der die Zeilen auch ihren Anzeigenamen als Feld tragen (Aggregat-Initialisierung `{ id, "Get Name", ... }`); die Post-Pässe (Self-Default) bleiben zentral. Das ändert kein Laufzeitverhalten und halbiert Inkrementbuild-Zeiten bei Änderungen an einer Gruppe.
- **Erwarteter Effekt**: Build-Zeit; Lesbarkeit; keine Laufzeit.
- **Risiko / Verifikation**: Reihenfolge der Registry-Zeilen (`find` nimmt die erste bei doppelter ID, Kommentar `:8779`, und Editor-Paletten zeigen Registry-Reihenfolge) muss erhalten bleiben; `test_engine_api` (Eindeutigkeit der IDs, `cppCall`-Invariante) deckt das ab.

---

## Gelesen

Ja, alle zehn Dateien des Slice vollständig gelesen: `EngineApi.cpp` (Zeilen 1 bis 9210 in Blöcken), `EntityActive.cpp`, `EntityHost.cpp`, `EntityVisibility.cpp`, `EnvironmentPush.cpp`, `FloatingOrigin.cpp`, `FlyCameraController.cpp`, `FoliagePaint.cpp`, `FoliageSystem.cpp`, `GameInstanceHost.cpp`; zusätzlich gezielt (nur per Suche und kurzen Ausschnitten) `HorizonCode.h` (`Value`), `WaterQuery.cpp`, `EntityHost.h`, `FoliageComponent.h`, `HorizonWorld.cpp`, `OutlinerPanel.cpp` und `TerrainTools.cpp` zur Verifikation der Aufrufer.

## Scene Slice 3 – GameReplication, HcCodegen, HiddenShell, Animation-/Audio-/Camera-/Cell-/Collision-/AntiCheat-Header, Components/*

## Zusammenfassung

Der Slice besteht zu etwa 55 % aus `HcCodegen.cpp` (Build-/Export-Zeit-Generator plus Toolchain-Probe, kein Frame-Pfad), zu 10 % aus `GameReplication.cpp` (Netzwerk-Tick) und sonst aus Headern und reinen Daten-Komponenten. Die Komponenten sind überwiegend schlank und nutzen bereits Dirty-Flags oder Revisionszähler (`MeshComponent::dirty`, `MaterialComponent::dirty`, `FoliageComponent::revision`, `AnimatorStateMachineComponent::configDirty`). Die relevanten Funde liegen deshalb in drei Bereichen:
- Per-Frame-Pfade ohne Änderungserkennung: Client-Interpolation, Server-Snapshots, `AudioSystem::updateSpatial`.
- Pro-Entity-Heap für Daten, die aus einem geteilten Asset abgeleitet sind (State-Machine-Graph, Blend-Spaces, Masken, Partikel-Config, 2,5 KB Zufallsgenerator).
- Die vom Codegen erzeugte Laufzeit (String-Dispatch, `std::vector<hc::Value>` pro Engine-Call) und eine Datenrasse in der cmake-Auflösung.

Funde: 2 Hoch, 9 Mittel, 7 Niedrig (18 gesamt). Alle Zeilennummern wurden gegen den aktuellen Stand der Dateien geprüft. Es wurde nichts gemessen. Alle Effekte sind Schätzungen.

---

### SCN-301 – Client-Interpolation schreibt jeden Frame jede Entity und setzt `dirty`, auch im Stillstand
- **ID**: SCN-301
- **Kategorie**: D (ECS ohne Dirty-Flag) / C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/GameReplication.cpp:932-981` (Schleife), `:954-955` (Position), `:979` (`dirty`)
- **Problem**: `advanceInterpolation` läuft pro Frame über alle `m_interp`-Einträge, also über alle replizierten Fremd-Entities. Pro Eintrag kostet das mindestens vier Lookups: `m_byNetId.find`, `reg.valid`, `try_get<TransformComponent>` und in `wireOrigin` ein weiteres `try_get<HierarchyComponent>` samt `rootEntity()`-Vergleich. Danach wird Position und Rotation neu berechnet und `tc->dirty = true` gesetzt, auch wenn `t == 1` ist und `previous == current` gilt. Das trifft statische Props und alles, was zwischen zwei Snapshots steht. Jede dieser Entities kostet damit in `propagateTransforms` (siehe `HierarchyComponent.h:6-9`: "skipping the subtrees that did not change") und in der Render-Extraktion jeden Frame Arbeit, obwohl sich nichts bewegt hat. Der Dirty-Mechanismus wird so für den gesamten Client unterlaufen. Zusätzlich werden `m_interp`-Einträge nur in `unregisterEntity` (`:273-285`) gelöscht. Wird eine Entity ohne Unregister zerstört, bleibt ein toter Eintrag, den die Schleife dauerhaft per `continue` überspringt.
- **Code-Auszug**:
```cpp
for (auto& [netId, st] : m_interp) {
    ...
    st.elapsed += dt;
    const float t = st.hasPrevious ? std::min(1.0f, st.elapsed / span) : 1.0f;
    tc->position = fromWire(glm::mix(st.previous.position, st.current.position, t),
                            wireOrigin(m_world, it->second));
    ...
    tc->dirty    = true;
}
```
- **Lösung**:
  1. Pro Eintrag ein `settled`-Flag führen. Es wird gesetzt, sobald `t >= 1` und die Pose einmal geschrieben wurde. Es wird gelöscht, wenn `applySnapshot` den Puffer verschiebt oder `applyBaseline` ihn setzt.
  2. Bei `settled` den Eintrag komplett überspringen. Dann entfällt `dirty`, und die vier Lookups gelten nur noch für bewegte Entities.
  3. Den Ursprungs-Offset (`wireOrigin`) einmal pro Frame bzw. pro Floating-Origin-Wechsel cachen (`world->origin()` ist pro Frame konstant). Die `const_cast`-Konstruktion in `:29` entfällt, wenn `wireOrigin` eine nicht-konstante Welt oder eine bereits geprüfte Registry bekommt.
  4. Zerstörte Entities (`!reg.valid`) aus `m_interp` entfernen (Schleife mit Iterator-erase).
  5. Alternativ `HierarchyComponent`/`EntityHandle` im `InterpState` ablegen, damit `m_byNetId.find` pro Frame wegfällt (Handle-Gültigkeit weiter über `reg.valid` prüfen).
  ```cpp
  if (st.settled) continue;
  ...
  if (t >= 1.0f) st.settled = true;
  ```
- **Erwarteter Effekt**: Schätzung: bei überwiegend ruhenden Entities (Props, Pickups, NPCs in Idle) entfallen fast alle Arbeitskosten dieser Funktion und der davon ausgelöste Transform-Propagate. Bei 1000 replizierten Entities sind das 4000 Hash-/Sparse-Set-Zugriffe pro Frame. Bei vollständig bewegter Szene kein Unterschied.
- **Risiko / Verifikation**: Das Verhalten bleibt gleich, wenn `settled` bei jedem neuen Sample zurückgesetzt wird. Eine Entity, die eine Skript-Änderung an Position erfährt, wird heute vom Interpolator überschrieben. Das wäre mit `settled` nur noch einmal der Fall. Zu prüfen ist, ob das gewollt ist ("Server ist Wahrheit"). Dann wäre das Verhalten leicht verschieden, und die Änderung braucht eine Entscheidung. Tests: GameReplication-Tests (Interpolation, Baseline, Wrap-Fall 179° → -179°) um "Position nach t=1 unverändert und `dirty` nicht erneut gesetzt" ergänzen.

---

### SCN-302 – Assetabgeleitete Daten werden pro Entity kopiert (State-Machine-Graph, Blend-Spaces, Masken, Partikel-Config)
- **ID**: SCN-302
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance (Speicher/Heap) + Vereinfachung
- **Ort**: `Components/AnimatorStateMachineComponent.h:21-23, 30, 33, 49, 76, 86-93`; `Components/AnimationLayerComponent.h:105-120`; `AnimationPose.h:223-235`; `Components/ParticleSystemComponent.h:49-51, 60-73`
- **Problem**: Jede Entity mit State Machine hält eine private Kopie des geparsten Graphen (`HE::AnimatorStateMachineGraph resolvedGraph` mit States, Transitions, Strings). Dazu kommen `unordered_map<string,float> params`, zwei `std::string` (`currentStateName`, `transitionTarget`) und einen `deque` mit je einer eigenen Kopie jedes referenzierten `BlendSpace`. Dazu kommt eine ungenutzte `LegacyConfig` mit zwei Vektoren und einer Map, die in jeder Instanz mitgetragen wird. `AnimationLayerComponent` legt pro Entity `vector<vector<float>> resolvedMasks` plus eigene `blendSpaces` an. `ParticleSystemComponent` kopiert die `ParticleEmitterConfig` pro Emitter. Die Kommentare nennen den Zweck ("same discipline as ParticleSystemComponent::resolvedConfig", "assets shared by many entities"), aber der Cache ist pro Komponente statt pro Asset. Bei 200 Crowd-Charakteren mit gleichem State-Machine-Asset werden Graph und Blend-Spaces 200 Mal geparst und gehalten, und der Cache wird bei jeder Asset-Änderung für jede Komponente einzeln über `configDirty`/`markConfigDirty` invalidiert. Der Zustand pro Frame (`params` als `unordered_map<string,float>`) bedeutet außerdem String-Hash pro Parameterzugriff (Lesezugriff durch Sync-Graph und Transition-Bedingungen).
- **Code-Auszug**:
```cpp
HE::AnimatorStateMachineGraph resolvedGraph;   // per entity
std::unordered_map<std::string, float> params;
HE::BlendSpaceCache blendSpaces;               // deque of copies, per entity
```
- **Lösung**:
  1. Einen geteilten, unveränderlichen Resolve-Cache pro Asset einführen: `std::shared_ptr<const ResolvedStateMachine>` (Graph + vorgeparste Blend-Spaces + Parameter-Indextabelle), geführt in einem System-Cache `unordered_map<UUID, {assetRevision, shared_ptr}>`. Die Komponente hält nur den `shared_ptr` und die Asset-Revision, mit der sie aufgelöst wurde.
  2. Invalidierung über eine Asset-Revision statt `configDirty` pro Komponente. Das ersetzt `markConfigDirty` und die "Belt and braces"-Doppelprüfung der Masken (`AnimationLayerComponent.h:108-114`).
  3. Laufzeit-Parameter als `std::vector<float>` mit Index aus der geteilten Tabelle (Name → Index einmal beim Graph-Resolve). Namensbasierter Zugriff (`animator.setParam`) wandelt einmal in einen Index um.
  4. `LegacyConfig` in eine separate, selten benutzte Komponente auslagern oder nach dem Migrationslauf entfernen (`registry.remove`) – oder `std::unique_ptr<LegacyConfig>`, damit nicht jede Instanz Vektoren und Maps trägt.
  5. Gleiches Muster für `resolvedConfig` des Partikel-Systems. Bei diesem ist zu beachten, dass `RandomRange`-Knoten beim Resolve einmal gewürfelt werden (Kommentar `:44-48`). Das bleibt gewahrt, wenn der gewürfelte Wert Teil des geteilten Resolve-Ergebnisses ist; gewollt ist heute vermutlich "einmal pro Emitter" – sonst bekäme jeder Emitter gleiche Werte. Dort deshalb nur den unveränderlichen Teil teilen.
- **Erwarteter Effekt**: Schätzung: Speicher pro animierter Entity sinkt um den Graph/Blend-Space-Anteil (je nach Asset einige KB bis zehn KB und mehrere Heap-Blöcke); weniger Cache-Misses in der Komponenten-Iteration, weil die Komponente kompakter wird. Parse-/Resolve-Kosten bei Asset-Reload sinken von N auf 1.
- **Risiko / Verifikation**: Mittel bis hoch (öffentliche Komponenten, Serializer, Editor-Panels, `AnimationStateMachineSystem.cpp`, außerhalb dieses Slices). Zuerst Speicher und Parse-Zeit mit 200 Instanzen messen. Danach schrittweise umbauen, erst Graph, dann Blend-Spaces. Tests: bestehende State-Machine-/Blend-Space-Tests plus "zwei Entities, ein Asset, Asset editieren → beide re-resolven genau einmal".

---

### SCN-303 – Host-Snapshot: pro Client alle Entities, 3 Lookups je Entity, Samples pro Client neu berechnet
- **ID**: SCN-303
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `GameReplication.cpp:679-772` (`sendSnapshots`), speziell `:701-730`, `:751-764`
- **Problem**: Alle `tickHz` (typisch 30 Hz) läuft für jeden Client die Schleife über alle `m_byNetId`. Pro Entity gibt es `reg.valid`, `try_get<NetworkComponent>`, `try_get<TransformComponent>`, einen Distanztest und `toWire` + `wireOrigin` (weiteres `try_get<HierarchyComponent>`). Die Wire-Samples hängen nicht vom Client ab, sondern nur der Relevanz-Test. Sie werden aber für jeden Client neu berechnet. Der Vektor `relevant` wird pro Client und Tick neu angelegt (kein `reserve`). Pro Datagramm entsteht ein `BitWriter` (vermutlich mit eigenem Heap-Puffer, in diesem Slice nicht sichtbar). Es gibt keine Delta- oder Dirty-Logik: Entities, die sich nicht bewegt haben, werden trotzdem 30 Mal pro Sekunde gesendet. `replicateTransform` ist nur ein manueller Schalter (`NetworkComponent.h:41`). Kosten wachsen mit O(Clients × Entities).
- **Code-Auszug**:
```cpp
for (const ConnectionId conn : m_net->connections()) {
    ...
    std::vector<std::pair<std::uint32_t, Sample>> relevant;
    for (const auto& [netId, entity] : m_byNetId) {
        if (!reg.valid(entity)) continue;
        auto* nc = reg.try_get<NetworkComponent>(entity);
        auto* tc = reg.try_get<TransformComponent>(entity);
        ...
        s.position = toWire(tc->position, wireOrigin(m_world, entity));
```
- **Lösung**:
  1. Einmal pro Tick eine Tabelle `{netId, entity-Position (vec3), Sample, relevanceRadius²}` bauen (ein Durchlauf, ein `reserve`). Danach pro Client nur noch Distanztest über das flache Array.
  2. Optional: Quantisierte Samples vergleichen, nur bei Änderung gegenüber dem letzten pro Client gesendeten Stand senden (Delta/Dirty). Das ändert das Wire-Verhalten und braucht eine Client-seitige Anpassung (Extrapolationsende). Ist ein Protokolländerungs-Thema, nicht nur Aufräumen.
  3. `BitWriter` und `relevant` als Member-Scratchpad wiederverwenden.
- **Erwarteter Effekt**: Schätzung: bei 8 Clients und 2000 Entities 8×2000×3 Lookups weniger (48 000 pro Tick, 1,4 M pro Sekunde); Schritt 1 allein bringt ungefähr den Faktor Client-Anzahl für den Lookup-Anteil.
- **Risiko / Verifikation**: Schritt 1 und 3 bytegleich auf dem Draht. Test mit bestehender Snapshot-Test-Suite (Reihenfolge der Entities im Snapshot muss gleich bleiben – `unordered_map`-Reihenfolge ist nicht definiert, siehe Kommentar `:594-596`; nach netId sortieren, falls Tests darauf bauen). Schritt 2 nur mit Protokollversion und Regressionstest "Entity bleibt nach Stillstand stehen".

---

### SCN-304 – `AudioEngine::play` kopiert den kompletten PCM-/Ogg-Puffer pro Aufruf
- **ID**: SCN-304
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonScene/AudioEngine.h:105-115, 123-125, 225-228, 288-293` (`startSound(const std::vector<uint8_t>& bytes, ...)`)
- **Problem**: Der Header dokumentiert selbst: "PCM16 copies the samples into the voice … Both copy because the asset lives in ContentManager's dense storage, which the next loadAsset() may relocate". Jeder `play`/`playSpatial` (Schuss, Schritt, UI-Klick) kopiert damit die gesamte Clip-Datenmenge auf dem Aufrufer-Thread (Update-/Spiel-Thread) und allokiert dafür. Der Kommentar bei `pauseSound` (`:225-228`) beziffert es selbst: "a multi-megabyte PCM copy per press". Eine Musikspur mit mehreren MB wird bei jedem Start einmal kopiert, ein oft ausgelöster Effekt bei jedem Auslösen.
- **Code-Auszug**:
```cpp
// PCM16 copies the samples into the voice (as the raw overload below); Vorbis copies only the Ogg bytes ...
// Both copy because the asset lives in ContentManager's dense storage ...
uint64_t play(const AudioAsset& clip, float volume = 1.0f, ...);
```
- **Lösung**:
  1. `AudioAsset::audioData` als `std::shared_ptr<const std::vector<uint8_t>>` (bzw. unveränderlichen, referenzgezählten Puffer) halten. Die Voice hält eine Referenz statt einer Kopie. Das behebt auch die Relokations-Gefahr, die der Grund der Kopie ist.
  2. Alternativ im Mixer-Backend (`miniaudio` `ma_audio_buffer_ref`) auf die geteilte Referenz zeigen und die Lebensdauer über den `shared_ptr` im Voice-Impl sichern.
  3. Gleichzeitig die Strings: `busName` ist `const std::string&`, `routeFor` baut pro Play einen neuen `std::string`. Das ist klein, aber im selben Pfad.
- **Erwarteter Effekt**: Schätzung: pro Sound-Start entfällt ein Memcpy der Clip-Größe (für 1 s Stereo 44,1 kHz PCM16 = 176 KB, für lange Clips MB) und eine Allokation; reduziert Hitches bei Gewehrfeuer/Effekt-Spam.
- **Risiko / Verifikation**: `AudioAsset` ist in `ContentManager/Assets.h` (anderes Modul/Slice) – Änderung der Asset-Struktur betrifft Serializer/Importer. Test: bestehende Audio-Tests (Headless `readMixedFrames`), zusätzlich "Asset während Wiedergabe neu laden" bleibt hörbar identisch und stürzt nicht ab.

---

### SCN-305 – `AudioSystem::updateSpatial` ruft pro Frame für jede Quelle `setSoundPosition` auf, ohne Änderungserkennung
- **ID**: SCN-305
- **Kategorie**: D / B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonScene/AudioSystem.h:51-78`
- **Problem**: Die View `AudioSourceComponent, TransformComponent` wird jeden Frame vollständig durchlaufen, und jede räumliche Quelle bekommt `engine.setSoundPosition(...)`, auch wenn sie sich nicht bewegt hat oder `handle` längst beendet ist. Die Engine-Implementierung (miniaudio-Wrapper) macht dafür sehr wahrscheinlich einen Handle-Lookup (Hash) und eine Sperre gegen den Audio-Thread (in diesem Slice nicht sichtbar, unsicher). Es gibt weder "Position geändert"-Prüfung noch Rückmeldung, wenn der Sound fertig ist; `src.handle` bleibt auf dem toten Wert. Außerdem wird `t.position` benutzt, also die lokale Position (`TransformComponent.h:20`, `worldMatrix` separat `:29`), obwohl für gekindete Entities die Weltposition gemeint ist. Eine Quelle unter einem verschobenen Parent klingt dann an der falschen Stelle. Das gilt analog für den Listener. Das ist ein Korrektheitsrisiko, kein reiner Performancepunkt, aber durch den Fund in `AudioSystem.h:61-67` direkt sichtbar (unsicher, ob alle Quellen bewusst Root-Level sind).
- **Code-Auszug**:
```cpp
auto srcView = reg.view<AudioSourceComponent, TransformComponent>();
for (auto [entity, src, t] : srcView.each()) {
    if (!src.spatial || src.handle == 0) continue;
    engine.setSoundPosition(src.handle, t.position.x, t.position.y, t.position.z);
}
```
- **Lösung**:
  1. Letzte gesendete Position in der Komponente (`glm::vec3 lastPos`) halten, nur bei Änderung (> Epsilon) senden. Nach Möglichkeit `isPlaying(handle)` selten prüfen oder in `AudioEngine` ein Rückruf/Reaping einführen, damit `handle` auf 0 gesetzt wird.
  2. Statt `t.position` die Weltposition aus `worldMatrix[3]` verwenden. Damit das stimmt, muss die Transform-Propagation vor dem Audio-Update gelaufen sein (Reihenfolge in `GameApplication.cpp:3294` / `EditorApplication.cpp:3950` prüfen).
  3. Der `break` in der Listener-Schleife ist korrekt, aber `view.each()` + `break` kann durch `view.front()`/`*view.begin()` ersetzt werden (Lesbarkeit).
- **Erwarteter Effekt**: Schätzung: bei 100 statischen Quellen 100 Engine-Aufrufe pro Frame weniger; Wirkung hängt von den Kosten von `setSoundPosition` ab (messen).
- **Risiko / Verifikation**: Schritt 1 verhaltensgleich. Schritt 2 ändert hörbares Verhalten für gekindete Quellen (Fehlerkorrektur) – mit einem Test belegen: Quelle als Kind eines verschobenen Parents, erwartete Weltposition im Engine-Mock.

---

### SCN-306 – `FoliageComponent` hält Instanzen doppelt (`cachedInstances` als `vector<mat4>` und `store`)
- **ID**: SCN-306
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance (Speicher)
- **Ort**: `Components/FoliageComponent.h:38, 45, 50`
- **Problem**: `cachedInstances` speichert jede Instanz als Welt-`mat4` (64 Byte). Dazu hält die Komponente das bereits sortierte, terrain-lokale `store`. Laut Kommentar liest der Renderer nur `store`; `cachedInstances` bedient nur "editor read-outs and the scatter tests" (`:41-43`). Bei Grasfeldern mit Hunderttausenden Instanzen sind das mehrere zehn MB, die in jedem Spiel-Build ungenutzt bleiben. `densityMask` (`maskRes²` Bytes) und `cachedInstances` werden außerdem bei jedem Komponenten-Move/-Copy (Undo-Snapshot, Szenen-Clone) mitkopiert. Das gleiche Muster gilt für `std::vector<glm::mat4>` im Play-Snapshot des Editors (Cross-Modul, unsicher).
- **Code-Auszug**:
```cpp
std::vector<glm::mat4> cachedInstances;
std::shared_ptr<const FoliageStore> store;
```
- **Lösung**:
  1. `cachedInstances` nur im Editor/Test-Build halten oder als Funktion `instancesWorld(const FoliageStore&, const glm::mat4& terrainWorld)` bei Bedarf aus dem `store` ableiten.
  2. Mindestens `cachedInstances` nach dem Scatter leeren, wenn kein Editor-Konsument sie braucht, und die Tests auf die Ableitungsfunktion umstellen.
- **Erwarteter Effekt**: Schätzung: 64 B × Instanzanzahl pro Foliage-Layer (z. B. 500 000 Instanzen ≈ 32 MB) weniger RAM.
- **Risiko / Verifikation**: Editor-Panels und Scatter-Tests lesen das Feld (Aufrufer in `FoliageSystem.cpp`, Tests; nicht in diesem Slice). Mit `grep cachedInstances` alle Leser prüfen, Verhalten pro Test absichern.

---

### SCN-307 – `ParticleSystemComponent` trägt einen `std::mt19937` (≈ 2,5 KB) inline
- **ID**: SCN-307
- **Kategorie**: C / F
- **Schweregrad**: Mittel
- **Typ**: Performance (Speicher/Cache) + Header-Gewicht
- **Ort**: `Components/ParticleSystemComponent.h:5, 42, 49, 60-73`
- **Problem**: `std::mt19937 rng { 42 }` hat 624 × 4 Byte Zustand. Jede Komponente ist dadurch über 2,5 KB groß, plus `resolvedConfig`, `LegacyConfig`, `std::vector<Particle>`. Die ECS-Pool-Elemente werden bei Swap/Move beim Entfernen von Komponenten (entt: swap-and-pop) mit diesem Brocken bewegt. Eine Iteration über alle Emitter streift pro Element ≥ 2,5 KB. Der Header zieht `<random>` in jede Datei, die die Komponente sieht (Serializer, Extractor, Editor-Panels).
- **Code-Auszug**:
```cpp
std::vector<Particle> particles;
std::mt19937          rng { 42 };
```
- **Lösung**: Durch einen kleinen Zähler-basierten Generator ersetzen (z. B. PCG32 oder xorshift, 8-16 Byte). Die Folge ändert sich dadurch, was bei deterministischen Replays oder Tests mit festen Seeds sichtbar sein kann. Falls bitgleiche Ergebnisse gebraucht werden, `mt19937` hinter `std::unique_ptr` legen (lazy beim ersten Emit).
- **Erwarteter Effekt**: Schätzung: Komponentengröße sinkt von ≈ 2,7 KB auf ≈ 0,3 KB; weniger Cache-Druck in Partikel-Update und -Extraktion; `<random>` fällt aus den Include-Ketten.
- **Risiko / Verifikation**: Das Ergebnis einer Partikelsimulation mit festem Seed ändert sich bei Generatorwechsel. Prüfen, ob Tests oder Replays auf konkrete Werte vergleichen (`grep -rn "rng" tests/`), sonst Variante mit `unique_ptr`.

---

### SCN-308 – Generierter Event-Dispatch: String-Kette pro Event, `RunState` mit `hc::Value` pro Aufruf
- **ID**: SCN-308
- **Kategorie**: B / C
- **Schweregrad**: Mittel
- **Typ**: Performance (Laufzeit des generierten Codes)
- **Ort**: `src/HE_Scene/src/HcCodegen.cpp:3488-3527` (Emission von `fireEvent`), `:3270-3283` (RunState), `:3534-3563` (Engine-Hooks)
- **Problem**: Der Generator emittiert für jede Klasse `fireEvent(const std::string& name, int elem, const hc::Value& arg)` als `if (name == "A") … else if (name == "B") …`. Wer per Name feuert (Runtime-Seam, Netzwerk, Bind-Event), zahlt O(#Events) `std::string`-Vergleiche pro Aufruf; bei Basisklassen kommt der Vergleichslauf der Eltern hinzu. Pro Aufruf wird ein `RunState rs;` auf dem Stack angelegt. Es enthält `hc::Value eventArg` (Value trägt laut Nutzung `items`, `keys`, `s`, `typeName`, also mehrere Container/Strings), und `rs.eventArg = arg` kopiert das Argument vollständig. Für die Engine-Hooks (`OnTick`, `OnHit` …) umgeht der Generator das bereits (direkte Overrides `:3534-3563`), nur die Value-Boxing-Schritte bleiben (`rs.eventArg = hc::toValue(arg)`, `:3555`). Der Generator hat die Gegenseite schon gelöst: Emit/Bind-Events werden zu `hc::EventId` internalisiert (`:3329-3337`), der Dispatch geht aber trotzdem über Strings.
- **Code-Auszug**:
```cpp
c += std::string("    ") + (first ? "if" : "else if") + " (name == " + strLit(name) + ")\n    {\n";
...
if (m_usesEventArg) c += "    rs.eventArg = arg;\n";
```
- **Lösung**:
  1. `fireEvent` zusätzlich als `fireEventId(EventId, …)` mit `switch` über die internalisierten Ids erzeugen und die Runtime auf Ids umstellen (die Id-Tabelle existiert, siehe `kEvt_…`).
  2. Alternativ den Namensvergleich über einen vorab generierten, nach Länge/erstem Zeichen gestaffelten Schalter (`switch (name.size())`) beschleunigen – weniger invasiv, aber weniger Wirkung.
  3. `RunState::eventArg` als `const hc::Value*` statt Wertkopie führen (Argument lebt während des Aufrufs).
- **Erwarteter Effekt**: Schätzung: pro Event-Dispatch ein Wert-Copy und eine Handvoll String-Vergleiche weniger; relevant für Klassen mit 10+ Events und vielen Instanzen (z. B. OnTick für Hunderte Entities). Vor der Umsetzung messen, wie viele Dispatches pro Frame wirklich über den Namenspfad laufen.
- **Risiko / Verifikation**: Hoch für Semantik-Parität: der Generator hat einen "Parity-Harness" (Kommentare zu `hcgen_C_math_ops`, "traced parity text"). Jede Änderung der Emission muss dessen Tests bestehen. Nur mit Messung (Profil mit `fireEvent` im Hot-Path) angehen.

---

### SCN-309 – Generierte Engine-Calls: `std::vector<hc::Value>` und Namens-Dispatch pro Aufruf
- **ID**: SCN-309
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance (Laufzeit des generierten Codes)
- **Ort**: `HcCodegen.cpp:2248-2274` (`emitValueCall`), `:1885-1898` (pure EngineCall), `:2707-2712`, `:1815-1823` (`hc::getProperty(m_ctx, elem, "name")`), `:2403-2410`
- **Problem**: Jeder Engine-Call im generierten C++ packt seine Argumente in einen frischen `std::vector<hc::Value>` (Heap), ruft `hc::callApi(m_ctx, "api.name", args)` über den Namen (Hash- oder Tabellensuche zur Laufzeit) und entpackt das Ergebnis aus einem weiteren Vector. Pure Calls werden "on every data-out read" dispatcht (`:1889-1897`, `std::vector<hc::Value>{...}` als Ausdruck). `getProperty`/`setProperty`/`getExternal` laufen ebenfalls per Namensstring. Damit sind genau die häufigsten Knoten in Gameplay-Graphen (Transform lesen/schreiben, Distanz, Input) heap- und string-lastig. Dass der Generator Direktaufrufe zwischen kompilierten Klassen bereits typisiert emittiert (`directFn`, `directVar`), zeigt, dass das Muster für Engine-APIs fehlt.
- **Code-Auszug**:
```cpp
std::string args = "std::vector<hc::Value>{";
...
return fromValueCall("hc::callApi(m_ctx, " + strLit(n.s) + ", " + args + ")", (size_t)outIdx, out);
```
- **Lösung**:
  1. `HE::api::ApiFn` auf eine aufgelöste Funktionsadresse/Id erweitern, die der Generator beim Erzeugen kennt (er ruft `HE::api::find(n.s)` schon, `:1332`). Dann `static const hc::ApiId kApi_n42 = hc::apiId("name");` pro Call (analog zu `kEvt_…`) und `hc::callApi(m_ctx, kApi_n42, args)`.
  2. Argumente in ein `std::array<hc::Value, N>` oder `hc::ArgPack` mit Small-Buffer legen statt `std::vector`.
  3. Häufige Standard-APIs (Transform get/set) direkt typisiert über einen Fast-Path (`hc::getPosition(m_ctx, ref)`) emittieren.
- **Erwarteter Effekt**: Schätzung: pro Engine-Call eine bis zwei Heap-Allokationen und eine String-Suche weniger; relevant bei Tick-Graphen mit vielen Calls pro Frame und Instanz. Messen: Profil eines kompilierten Tick-Graphen.
- **Risiko / Verifikation**: Die Dispatch-Zählung ist Teil der Parität ("its dispatch count is part of the trace (§3.4)", `:1855`). Ersetzen des Lookups ändert die Anzahl der Dispatches nicht, aber jede Änderung an der Emission braucht den Parity-Test. API-Registry liegt außerhalb dieses Slices (`EngineApi.h/.cpp`).

---

### SCN-310 – Datenrasse auf den globalen cmake-Cache in `HcCodegen.cpp`
- **ID**: SCN-310
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektheit)
- **Ort**: `HcCodegen.cpp:4222-4276` (`g_bundledCmakeDir`, `g_resolvedCmake`, `g_cmakeResolved`, `resolveCmake`, `setBundledCmakeDir`)
- **Problem**: Der Kommentar in `setBundledCmakeDir` (`:4271-4272`) räumt selbst ein, dass der Editor die Funktion "possibly while an export worker is inside resolveCmake()" aufruft. `g_bundledCmakeDir` (ein `std::filesystem::path`), `g_resolvedCmake` (`std::string`) und `g_cmakeResolved` (`bool`) sind aber ungesicherte Globals. Gleichzeitiges Schreiben (UI) und Lesen/Schreiben (Worker) auf `std::string`/`path` ist undefiniertes Verhalten und kann bei Pfadwechsel crashen. Die Absicht (nur geänderter Pfad leert den Cache) schützt nur den Gleichheitsfall.
- **Code-Auszug**:
```cpp
std::filesystem::path g_bundledCmakeDir;
std::string g_resolvedCmake;
bool        g_cmakeResolved = false;
```
- **Lösung**: Alle drei hinter einen `std::mutex` legen (kurzer Lock um `resolveCmake` und `setBundledCmakeDir`; die Prozessaufrufe in `cmakeAnswers` außerhalb des Locks, Ergebnis danach unter Lock speichern) und `resolveCmake` einen Wert statt `const std::string&` zurückgeben lassen (die Referenz auf das Global wird sonst nach einem Reset zum Dangling-Zugriff).
  ```cpp
  std::string resolveCmake() { std::lock_guard l(g_mx); ... return g_resolvedCmake; }
  ```
- **Erwarteter Effekt**: Kein Performance-Effekt; beseitigt eine seltene, schwer reproduzierbare Absturzquelle im Export-Pfad.
- **Risiko / Verifikation**: Niedrig. Test: zwei Threads, einer `setBundledCmakeDir` mit wechselnden Pfaden, einer `toolchainAvailable()`; TSan-Lauf.

---

### SCN-311 – Schwere Header: Game-Logic-Dispatcher als Header-only mit großen Includes
- **ID**: SCN-311
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Build-Zeit)
- **Ort**: `AnimationNotifySystem.h:1-9`, `CollisionSystem.h:1-9`, `AntiCheat/AntiCheatEvents.h:2-10`, `AudioSystem.h:1-9`, `Components/ParticleSystemComponent.h:1-6`, `AnimationPose.h:1-9`, `CellStreamer.h:1-2`
- **Problem**: Mehrere "Dispatcher"-Strukturen sind komplett im Header implementiert und ziehen `HorizonWorld.h`, `PhysicsWorld.h`, `ScriptEngine.h`, `HorizonCodeRuntime.h`, `ScriptContext.h`, `ContentManager.h` herein. Jede Datei, die nur `AnimationNotifySystem::dispatch` aufruft, kompiliert damit die gesamte Welt-, Physik- und Script-API mit. `AudioSystem.h` inkludiert `ContentManager.h` und `AudioEngine.h` nur für zwei Funktionen; `ParticleSystemComponent.h` zieht `<random>` und das ganze `ParticleGraph.h`; `CellStreamer.h` zieht `JobSystem/JobSystem.h`. Alle Dispatcher wiederholen dasselbe Gerüst "Script-Instanz, HorizonCode-Instanz, Sync-Graph" mit dem Default-Argument `const HcInstanceMap& = {}`. Das konstruiert in jedem Aufruf eine temporäre `unordered_map` (auf MSVC allokiert der Default-Konstruktor den Sentinel-Knoten). Zudem existiert `InstanceMap` als identischer Alias in drei Strukturen.
- **Code-Auszug**:
```cpp
static void dispatch(HE::NotifyQueue& queue, HorizonWorld& world, ScriptContext* scripts,
                     const InstanceMap& instances, HorizonCode::Runtime* runtime = nullptr,
                     const HcInstanceMap& hcInstances = {}, AnimatorHost* animators = nullptr)
```
- **Lösung**: Dispatcher nach `.cpp` verlagern (`AnimationNotifySystem.cpp`, `CollisionSystem.cpp`, `AntiCheatEvents.cpp`; im Header bleiben Vorwärtsdeklarationen), `AudioSystem` ebenso. `InstanceMap`/`HcInstanceMap` in einen gemeinsamen kleinen Header (`ScriptInstanceMaps.h`). Der Default `= {}` durch einen Überlad ohne Parameter oder `const HcInstanceMap* = nullptr` ersetzen.
- **Erwarteter Effekt**: Schätzung: spürbar kürzere inkrementelle Builds (Änderungen an `ScriptEngine.h`/`HorizonCodeRuntime.h` triggern weniger Dateien). Laufzeit kaum.
- **Risiko / Verifikation**: Niedrig, rein mechanisch. Auswirkung per `-ftime-trace`/Include-Zählung (`clang -H`) vorher/nachher belegen.

---

### SCN-312 – `AnimatorHost` hält drei Hash-Maps mit demselben Schlüssel
- **ID**: SCN-312
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `AnimatorHost.h:96-103` (`m_byEntity`, `m_assetOf`, `m_paramsOf`), `:86-90` (`instanceOf`)
- **Problem**: Pro Entity gibt es drei parallele `unordered_map<uint32_t, …>` (Instanz-Id, Asset-Uuid, Parameternamen als `vector<string>`). `bind`/`fireUpdate` (pro Frame und animierter Entity, laut Kommentar `:57-61`) und `instanceOf` (pro Notify) machen mehrere Lookups mit identischem Schlüssel; `unbind` muss alle drei konsistent löschen.
- **Code-Auszug**:
```cpp
std::unordered_map<uint32_t, HorizonCode::InstanceId> m_byEntity;
std::unordered_map<uint32_t, HE::UUID> m_assetOf;
std::unordered_map<uint32_t, std::vector<std::string>> m_paramsOf;
```
- **Lösung**: Eine Map `unordered_map<uint32_t, Binding>` mit `struct Binding { InstanceId id; HE::UUID asset; std::vector<std::string> params; }` (oder Parameter-Indizes aus SCN-302). Ein Lookup, ein Erase.
- **Erwarteter Effekt**: Schätzung: zwei Hash-Lookups pro `fireUpdate`/`bind`-Aufruf weniger; weniger Konsistenzfehler-Potenzial.
- **Risiko / Verifikation**: Niedrig; `.cpp` liegt außerhalb dieses Slices. Vorhandene Animator-Host-Tests nutzen.

---

### SCN-313 – Frame-Flags, die ein Pass pro Frame zurücksetzen muss (`finalizedThisFrame`, `appliedThisFrame`)
- **ID**: SCN-313
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance + Vereinfachung
- **Ort**: `Components/AnimationLayerComponent.h:126`, `Components/IkComponent.h:118`; Gegenstelle `src/PoseFinalize.cpp:434-440` (`poseBeginFrame`, nicht Teil dieses Slices)
- **Problem**: Beide Komponenten tragen ein `bool …ThisFrame`, das `poseBeginFrame` zu Beginn der Animationsphase in einer eigenen vollständigen View-Iteration (zwei Views) auf `false` setzt. Das ist ein zusätzlicher Pass über alle Layer- und IK-Komponenten pro Frame, nur um doppelte Pose-Treiber zu erkennen.
- **Code-Auszug**:
```cpp
bool finalizedThisFrame = false;   // AnimationLayerComponent / IkComponent
```
- **Lösung**: Statt Bool einen `uint32_t finalizedFrame` führen und gegen einen von der Welt/dem System gezählten `frameIndex` vergleichen (`if (c.finalizedFrame == frame) …; c.finalizedFrame = frame;`). `poseBeginFrame` und die Rücksetz-Passes entfallen.
- **Erwarteter Effekt**: Schätzung: zwei View-Iterationen pro Frame weniger; praktisch relevant erst bei vielen Charakteren.
- **Risiko / Verifikation**: Niedrig; Reihenfolge-Test mit Entity, die zwei Treiber trägt (heute geloggt, `PoseFinalize.cpp:458`).

---

### SCN-314 – Entity-weise Heap in Basis-Komponenten (`HierarchyComponent`, `NameComponent`, `MaterialComponent`, `LODComponent`)
- **ID**: SCN-314
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance (Speicher)
- **Ort**: `Components/HierarchyComponent.h:14-17`, `Components/NameComponent.h:5`, `Components/MaterialComponent.h:26, 34`, `Components/LODComponent.h:16`
- **Problem**: Jede Entity mit Kindern hat einen `std::vector<entt::entity> children`. Jeder Name ist ein `std::string` (heap ab 16 Zeichen, "Rock_Large_Mossy_03_LOD0" liegt schon darüber). `MaterialComponent` hat zwei Vektoren (`paramOverrides` mit je einem `std::string name`, `slotOverrides`), `LODComponent` einen Vektor. Bei 100 000 Szenen-Entities (Streaming-Zellen) summiert sich das auf viele kleine Allokationen und verschlechtert Cache-Lokalität beim Traversieren (`propagateTransforms` läuft über `children`).
- **Code-Auszug**:
```cpp
struct HierarchyComponent { entt::entity parent = entt::null; std::vector<entt::entity> children; };
struct NameComponent { std::string name; };
```
- **Lösung**: Nur mit Messung angehen. Optionen: Geschwister als intrusive verkettete Liste (`firstChild`, `nextSibling`, `prevSibling` als `entt::entity`, 12 Byte, keine Allokation, O(1) Reparent), Namen in einer String-Tabelle (`uint32_t nameId`) oder `SmallString<24>`. Das ändert APIs (`children` wird in Editor und Serializer direkt gelesen).
- **Erwarteter Effekt**: Schätzung: eine Allokation pro Eltern-Entity und pro langem Namen weniger. Nur relevant in sehr großen Szenen; vor Umbau Heap-Profil einer Zelle mit 50 000 Entities ziehen.
- **Risiko / Verifikation**: Hoch (Hierarchie-API wird überall genutzt, Serialisierung); kein Umbau ohne Messdaten. Nach Umbau Round-Trip-Tests der Szene-Serialisierung und Reihenfolge der Kinder (`SceneSerializer`-Tests).

---

### SCN-315 – `AnimationNotifyEvent` trägt `std::string name` pro gefeuertem Event
- **ID**: SCN-315
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `AnimationNotify.h:21-32`, `AnimationNotifySystem.h:66-69, 99-101`
- **Problem**: Jedes Event kopiert den Notify-Namen in einen eigenen `std::string`. Namen bis 15 Zeichen sind SSO (z. B. "footstep_l"), längere (`"Attack_Hit_Window_Start"`) allozieren pro Auslösung. Danach ruft der Dispatcher `callOnAnimationNotify(inst, ev.name)` und HorizonCode-`fire*` mit dem String, ggf. wird dort erneut kopiert oder hashed. Das passiert pro Frame und Entity, die Notifies feuert.
- **Code-Auszug**:
```cpp
struct AnimationNotifyEvent { uint32_t entity = 0; std::string name; Kind kind = Kind::Fire; };
```
- **Lösung**: `const std::string*` oder `std::string_view` auf den Namen im `AnimationNotify` des Clips (Clip-Asset lebt mindestens bis zum Dispatch im selben Frame, der Queue-Kommentar `:30-31` garantiert "filled this frame, emptied this frame"). Oder Notify-Namen beim Asset-Laden internieren (`uint32_t nameId`) und erst an der Skript-Grenze in Text auflösen.
- **Erwarteter Effekt**: Schätzung: eine mögliche Allokation pro Event weniger; gering, weil Events selten sind.
- **Risiko / Verifikation**: Lebensdauer des Clip-Assets ist die einzige Gefahr (Asset-Reload im selben Frame). Wenn nicht sicher, bei `std::string` bleiben. Kein Eingriff ohne Profil.

---

### SCN-316 – `HcCodegen.cpp`: Emitter, Probe-Doppeldurchlauf, Link-Suche in O(Links)
- **ID**: SCN-316
- **Kategorie**: A / D / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung (primär)
- **Ort**: `HcCodegen.cpp:1119-1130` (`execLinkFrom`/`dataLinkTo`), `:3833-3862` (Probe-Pass), `:1455-1466` (`std::function`-DFS), `:290-330` (`valueLit`, `Value item = src[i]`), ganze Datei 4917 Zeilen
- **Problem**:
  - Die Datei vermischt vier Aufgaben: Typ-/Enum-Header-Emission, Klassen-Emitter (`ClassEmitter`, ≈ 2300 Zeilen), Build-Manifest und Toolchain-Verwaltung (SDK-Suche, cmake/vswhere, Paketmanager-Installation, `buildDylib`, ab `:4062`). Letzteres hat mit Szenenlogik nichts zu tun.
  - `execLinkFrom` und `dataLinkTo` durchsuchen für jedes Statement/jede Eingabe linear alle Links (`for (const auto& l : m_g.links)`). Aufrufer sind `chain`, `input`, `targetOf`, `slotRef`. Das ist O(Knoten × Links) je Klasse – bei Graphen mit mehreren Tausend Links spürbar im Editor-Klick "Generate".
  - Der Probe-Durchlauf (`:3833-3862`) emittiert jede Klasse komplett (Header und Impl-String), um nur die öffentliche Oberfläche zu bekommen, und wirft das Ergebnis weg. Danach läuft der echte Durchlauf. Die Emission wird damit verdoppelt.
  - `valueLit` kopiert pro Array-Element den ganzen `Value` (`Value item = src[i]`), obwohl nur `isArray`/`container`/`type` geändert werden.
  - `checkCycles` nutzt rekursives `std::function` und `unordered_map` mit `operator[]` in der Schleife (legt Einträge an).
- **Code-Auszug**:
```cpp
const Link* execLinkFrom(int nodeId, int pin) const {
    for (const auto& l : m_g.links) if (l.srcNode == nodeId && l.srcPin == pin) return &l;
    return nullptr; }
```
- **Lösung**:
  1. Beim Konstruktor des `ClassEmitter` einmal `unordered_map<uint64_t(node<<32|pin), const Link*>` für Exec-out-Links und Data-in-Links bauen.
  2. Toolchain-Teil (ab `:4062`) in `HcToolchain.cpp` auslagern; `HiddenShell` gehört mit dorthin.
  3. Probe-Pass: Oberflächenberechnung (`m_publicFn`, `publicMethodSigs`, `variableMembers`) aus `run()` herausziehen (`buildNames()` genügt dafür), ohne die Bodies zu emittieren.
  4. `Value item` als Referenz plus lokale Typ-Overrides (`valueLit` mit Parameter statt Kopie).
- **Erwarteter Effekt**: Schätzung: Generate-Zeit im Editor sinkt bei großen Projekten, Faktor bis ~2 durch den Probe-Pass; Lesbarkeit durch Aufteilung deutlich besser. Kein Laufzeiteffekt im Spiel.
- **Risiko / Verifikation**: Die Ausgabe muss bytegleich bleiben (Parity-/Snapshot-Tests der generierten Dateien, `hcgen_*`-Fixtures). Für Schritt 3 vorher prüfen, dass `buildNames()` allein die Tabellen vollständig füllt (die Warnungen des Probe-Passes werden ohnehin verworfen).

---

### SCN-317 – Toolchain-Aufrufe starten pro Aufruf mehrere Prozesse
- **ID**: SCN-317
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `HcCodegen.cpp:4314-4322` (`commandExists`), `:4462-4497` (`chooseGenerator`, 2 Prozesse unter Windows), `:4553-4600` (`probeToolchain`), `:4877` (`buildDylib` ruft `chooseGenerator` erneut), `:4226-4234` (`cmakeAnswers`)
- **Problem**: `probeToolchain` und jeder `buildDylib`-Lauf rufen `chooseGenerator` auf, was unter Windows `vswhere` und `cmake -E capabilities` startet (zwei Prozesse mit `cmd.exe /c`), jedes Mal neu. `commandExists` startet eine Shell pro Prüfung. Alles synchron über `popen`, also auf dem aufrufenden Thread. Die Aufrufer sind Worker laut Kommentar (`:4148-4151`), aber die Ergebnisse (VS-Instanz) ändern sich nur bei Installation.
- **Code-Auszug**:
```cpp
runStreaming(vswhere + " -products * -prerelease ..." , nullptr, listed);
runStreaming(cmake + " -E capabilities", nullptr, caps);
```
- **Lösung**: `GeneratorChoice` einmal pro Session cachen (invalidieren bei `setBundledCmakeDir` und explizitem "Recheck"). Mit dem Mutex aus SCN-310 koppeln.
- **Erwarteter Effekt**: Schätzung: pro Generate/Build 2 Prozessstarts (je ≈ 100-500 ms unter Windows) gespart.
- **Risiko / Verifikation**: Cache-Invalidierung beim Installieren eines Visual Studios (`installToolchain`) beachten. Test: nach `installToolchain` wird neu geprobt.

---

### SCN-318 – Kleinigkeiten (Container, Casts, Versionstabellen)
- **ID**: SCN-318
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `GameReplication.cpp:26-31, 144-149, 849-854`; `CellStreamer.h:289`; `CameraRigController.h:62-89`; `AudioEngine.h:226-232`
- **Problem**:
  - `wireOrigin` nimmt `const HorizonWorld*` und macht dann `const_cast<HorizonWorld*>(world)->registry()`. Das ist ein Const-Bruch, nur um `try_get` aufzurufen.
  - `m_pendingInputs` (64 Elemente) ist ein `std::vector` mit `erase(begin())` (`:148`) und `remove_if`-erase (`:849`). Ein `std::deque` oder Ring passt besser zur FIFO-Semantik.
  - `std::unordered_map<Key, bool> m_failed` ist ein Set (`CellStreamer.h:289`).
  - `CameraRigController::update` hat zwei Überladungen, nur um die alte Signatur zu erhalten; ein einziger Aufrufpunkt mit Aggregat genügt, sobald alle Aufrufer migriert sind.
- **Code-Auszug**:
```cpp
const auto* h = const_cast<HorizonWorld*>(world)->registry().try_get<HierarchyComponent>(e);
...
m_pendingInputs.erase(m_pendingInputs.begin());
```
- **Lösung**: `registry()` const-Überlast in `HorizonWorld` bereitstellen (oder `const entt::registry&` übergeben), `std::deque`/Ringpuffer für Inputs, `unordered_set<Key>` für `m_failed`.
- **Erwarteter Effekt**: Kosmetisch; kleine Verbesserung in Lesbarkeit und bei 64-Element-Verschiebungen pro Eingabe.
- **Risiko / Verifikation**: Niedrig.

---

## Gelesen

Ja, alle 56 Dateien des Slices wurden vollständig gelesen, darunter `HcCodegen.cpp` (1-4917) und `GameReplication.cpp` (1-981). Ergänzend wurden nur zur Verifikation `PoseFinalize.cpp:430-460`, `TransformComponent.h` (Feldzeilen) und die Aufrufstellen von `AudioSystem::updateSpatial` gelesen.

## Scene Slice 4 – Components (ReplicatedVars…WeatherComponent), EngineApi.h … SequenceSystem.h (Kürzel SCN)

## Zusammenfassung

Slice 4 besteht fast nur aus Headern (70 Dateien, ca. 10 900 Zeilen, davon `EngineApi.h` 3 094 und `PhysicsWorld.h` 933). Die Header sind inhaltlich sehr gut dokumentiert; die Komponenten selbst sind überwiegend schlanke POD-Structs mit Dirty-Flags (`TransformComponent`, `TerrainComponent`, `SkeletalMeshComponent`). Die eigentlichen Kosten stecken in den dazugehörigen Systemen, deren Signaturen hier deklariert sind. Ich habe die jeweilige `.cpp` nur punktuell als Beleg gelesen (die `.cpp`-Dateien liegen in anderen Slices; dort bitte gegenprüfen). Auffällig sind (a) zwei per-Frame-Pfade ohne Dirty-Tracking mit Heap pro Entity (`PropertyReplicator::update`, `LODSystem::update`), (b) mehrere kleinere per-Frame-Allokationen in Script-/Input-/Rope-Pfaden und (c) ein unbegrenzt wachsender Log-Merker im RPC-Router. Funde: 2 Hoch, 9 Mittel, 9 Niedrig. Alle Performance-Schätzungen sind Schätzungen ohne Messung.

---

### SCN-401 – PropertyReplicator::update baut pro Frame und Entity Namen-/Wert-Listen neu auf
- **ID**: SCN-401
- **Kategorie**: C, D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/Net/PropertyReplicator.h:117` (`update()`), `:184-194` (`Table`), `:202-203` (`collect`); Beleg `src/HE_Scene/src/Net/PropertyReplicator.cpp:356-452`, `:92-124`
- **Problem**: Auf dem Host läuft `update()` jeden Frame. Für jede replizierte Entity (View über `NetworkComponent`, kein Dirty-Flag) wird `collect()` aufgerufen. Das füllt drei frische Vektoren (`std::vector<std::string> names`, `std::vector<bool> notify`, `std::vector<Value> current`) und kopiert dabei jeden Variablennamen (String) und jeden Wert (`Value` mit evtl. Strings/Containern). Danach wird `names != t.names` (String-Vergleich je Eintrag) geprüft und pro Property mit `std::find` über `names` nach dem Namen gesucht (O(n²) je Entity). Zusätzlich allokiert `joinedConnections()` jeden Frame einen Vektor, und die Schleife über `m_tables` prüft jeden Frame per `m_rep->entityOf()` (Hash-Lookup) alle Tabellen. Das ist Heap-Verkehr pro Entity und Frame, selbst wenn sich nichts geändert hat (der Normalfall).
- **Code-Auszug**:
```cpp
// PropertyReplicator.cpp:397-400, 410, 426
std::vector<std::string> names;
std::vector<bool>        notify;
std::vector<Value>       current;
collect(entity, names, notify, current);
if (names != t.names) { broadcastTableFor(nc.netId); continue; }
...
const auto at = std::find(names.begin(), names.end(), t.names[i]);
```
- **Lösung**:
  1. Namen/Notify nicht pro Frame neu kopieren: die Deklaration ändert sich nur, wenn eine Variable deklariert wird (`ReplicatedVarsComponent::declare`) oder der HorizonCode-Klassenwechsel/Hot-Reload passiert. Einen `declRevision`-Zähler (uint32) in `ReplicatedVarsComponent` (bei `declare` erhöhen) und einen Zähler/Hash der Runtime-Replikationsliste (`replicatedVariablesOf(inst)`) in `Table` merken; `names` nur bei Änderung neu aufbauen.
  2. Pro Frame nur die Werte vergleichen: `collectValuesInto(entity, t, scratchValues)` mit wiederverwendetem Member-Vektor `m_scratchValues` (kein Namens-Lookup, Positionen sind bereits durch die Tabelle fixiert). Bei unveränderter Deklaration entfällt das `std::find`.
  3. `joinedConnections()` einmal in einen wiederverwendeten Member-Vektor schreiben; die `m_tables`-Reap-Schleife nur bei geänderter `GameReplication`-Registrierung/`structureEpoch` ausführen.
  4. Kleine Skizze: `if (t.declRev != currentDeclRev(entity)) { rebuildNames(...); broadcastTableFor(...); continue; } m_rep->readValues(entity, t, m_scratchValues); for (i...) if (!valuesEqual(m_scratchValues[i], t.values[i])) ...`
- **Erwarteter Effekt** (Schätzung): pro Frame keine Heap-Allokationen mehr im Normalfall (drei Vektoren + N Strings je Entity). Bei einigen Hundert replizierten Entities mit je mehreren Variablen im Bereich von zehn bis hundert µs pro Frame auf dem Host; nur im Multiplayer-Host relevant.
- **Risiko / Verifikation**: Die Reihenfolge der Indizes (Wire-Format, „insertion order ist load-bearing“, siehe `ReplicatedVarsComponent.h:13-19`) darf sich nicht ändern. Die Erkennung „Deklaration geändert“ muss alle bisherigen Auslöser abdecken (Skript deklariert mitten in der Session, Hot-Reload, Typänderung). Absicherung: `tests/test_net_property_replication.cpp` (Deklaration nach Join, Typwechsel, Reorder) erweitern um „späte Deklaration“ und „Hot-Reload entfernt Variable“.

---

### SCN-402 – LODSystem::update läuft jeden Frame über alle LOD-Entities und komponiert jedes Mal die Eltern-Kette
- **ID**: SCN-402
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/LODSystem.h:10`; Beleg `src/HE_Scene/src/LODSystem.cpp:11-62`, `src/HE_Scene/src/TransformHierarchy.cpp:321-367`
- **Problem**: Der View `LODComponent, MeshComponent` wird jeden Tick komplett iteriert, ohne Dirty-Flag oder Early-out. Pro Entity wird `HE::worldPositionOf` → `worldMatrixOf` aufgerufen: das sammelt die komplette Parent-Kette (mit `registry.valid` + `try_get` je Stufe) und multipliziert Matrizen von oben nach unten; erst danach wird `glm::distance` zur Kamera gebildet. Terrain-Chunks (hunderte statische Kinder), Foliage-/Prop-LODs und alle statischen Meshes werden damit jeden Frame vollständig neu ausgewertet, obwohl sich weder ihre Position noch (bei stehender Kamera) die Distanz geändert hat. Die Dokumentation von `worldMatrixOf` nennt diesen Aufrufer ausdrücklich als messbaren Anteil (`TransformHierarchy.cpp:331-333`, „baseline §4.5“).
- **Code-Auszug**:
```cpp
// LODSystem.cpp:11-30
auto view = registry.view<LODComponent, MeshComponent>();
for (auto [entity, lod, mesh] : view.each())
{
    if (lod.levels.empty()) continue;
    const glm::vec3 pos = HE::worldPositionOf(world, entity);
    const float dist = glm::distance(cameraPos, pos);
```
- **Lösung**:
  1. Frame-kohärenter Early-out auf Weltebene: `LODSystem` merkt sich `lastCameraPos`, `world.structureEpoch()` und einen von `propagateTransforms` gepflegten „Transform-Änderungs-Zähler“ (oder `lastPropagateStats(world).changed`). Ist die Kamera um weniger als `eps` bewegt, die Epoche gleich und nichts transformiert/dirty, wird das System übersprungen (Ergebnis ist identisch, weil `lod.current` bereits stimmt). `eps` muss kleiner als der kleinste Abstand zu einer LOD-Grenze gewählt werden oder die Prüfung nutzt eine Distanz-Hysterese nur als Beschleuniger (exakt identisch: bei Kamerabewegung läuft die volle Schleife).
  2. Alternativ/zusätzlich: Weltposition aus `TransformComponent::worldMatrix[3]` lesen, wenn die Entity `localCacheValid` hat und der Tick nach `propagateTransforms` läuft; nur für frisch erzeugte Entities (`!localCacheValid`) den Pfad `worldPositionOf` nehmen. Das braucht eine Reihenfolgeprüfung in `SceneSystems::tickWorld` (LOD läuft am Ende, Propagate nicht davor; siehe Kommentar `LODSystem.cpp:20-28`).
  3. Optional: Distanz quadriert vergleichen (`maxDistance²`), spart `sqrt`/`glm::distance` je Entity; verändert aber `<=`-Grenzfälle numerisch minimal, daher nur mit Test.
- **Erwarteter Effekt** (Schätzung): bei ruhender Kamera und statischer Szene nahezu 100 % der LOD-Kosten (mehrere mat4-Multiplikationen pro Entity und Frame); bei laufender Kamera Schritt 2 deutlich billiger pro Entity. Bei 200 k-Entity-Szenen (siehe `docs/world-streaming-baseline-2026-10-06.md`) wahrscheinlich im ms-Bereich.
- **Risiko / Verifikation**: Das Verhalten „Chunk-Raster wird in demselben Tick richtig gelevelt“ (Kommentar `LODSystem.cpp:20-28`) darf nicht regressieren; Early-out darf nie eine neu erzeugte Entity überspringen (Epochen-Prüfung). Tests: `tests/test_lod.cpp`, `tests/test_transform_propagation.cpp`; neuer Test „Kamera still + Entity bewegt via Gizmo (ohne `dirty`) → LOD korrekt“, da viele Schreiber `dirty` nicht setzen (`TransformComponent.h:36-39`). Messen vor Umsetzung (Profiler-Scope „LOD“).

---

### SCN-403 – GameReplication::sendSnapshots: O(Clients × Entities) pro Tick, Samples pro Client neu berechnet
- **ID**: SCN-403
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/GameReplication.h:306` (`sendSnapshots`), `:344-348` (`m_byNetId`, `m_interp`); Beleg `src/HE_Scene/src/GameReplication.cpp:679-730`
- **Problem**: Je Snapshot-Tick (30 Hz) und je verbundenem Client wird `m_byNetId` (ein `unordered_map`, knotenbasiert) komplett iteriert; pro Eintrag `reg.valid`, `try_get<NetworkComponent>`, `try_get<TransformComponent>`, `wireOrigin(...)` und `toWire(...)`. Die Wire-Quantisierung (Position/Rotation) ist client-unabhängig und wird trotzdem für jeden Client neu berechnet; nur der Relevanz-Test (Distanz) ist clientspezifisch. Der Vektor `relevant` wird pro Client ohne `reserve` aufgebaut (wiederholtes Wachsen).
- **Code-Auszug**:
```cpp
// GameReplication.cpp:701-729
for (const ConnectionId conn : m_net->connections())
{
    ...
    std::vector<std::pair<std::uint32_t, Sample>> relevant;
    for (const auto& [netId, entity] : m_byNetId)
    {
        if (!reg.valid(entity)) continue;
        auto* nc = reg.try_get<NetworkComponent>(entity);
        auto* tc = reg.try_get<TransformComponent>(entity);
        ...
        s.position = toWire(tc->position, wireOrigin(m_world, entity));
```
- **Lösung**:
  1. Einmal pro Tick eine Tabelle `m_tickSamples` (reservierter Member-Vektor aus `{netId, Sample, relevanceRadius², position}`) über `m_byNetId` bauen, dann pro Client nur noch filtern und die Teilmenge in die Datagramme schreiben.
  2. `relevant` und die Tick-Tabelle als Member halten (`clear()` statt neu allokieren).
  3. Mittelfristig `m_byNetId` als dichten Vektor (netId ist ein fortlaufender Zähler `m_nextNetId`) statt `unordered_map` führen.
- **Erwarteter Effekt** (Schätzung): Aufwand sinkt von Clients×Entities×(3 Lookups + Quantisierung) auf Entities×(3 Lookups + Quantisierung) + Clients×Entities×(ein Distanztest). Bei 8 Clients ca. Faktor 3–6 weniger Arbeit im Sendepfad.
- **Risiko / Verifikation**: Bytes auf der Leitung müssen identisch bleiben (Reihenfolge der Entities im Datagramm hängt bisher an der Hash-Iterationsreihenfolge – nicht garantiert, Tests dürfen also nicht darauf bauen). `tests/test_game_replication.cpp` (Budget-/Split-Tests) muss grün bleiben.

---

### SCN-404 – RopeTrailSystem: pro Frame und Rope eine Vektor-Kopie, Matrix-Inverse und Hash; Trail-Geometrie als Wert pro Frame
- **ID**: SCN-404
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/RopeTrailSystem.h:38-39` (`resolveControlPoints`), `:58-59` (`buildTrailGeometry`), `:70` (`geometryHash`); Beleg `src/HE_Scene/src/RopeTrailSystem.cpp:63-87`, `:293-318`
- **Problem**: Der Header verspricht „a rope nobody touched costs one hash per frame“ (`:67-69`). Tatsächlich kostet sie pro Frame: eine vollständige Kopie von `rope.controlPoints` (`std::vector<glm::vec3> pts = rope.controlPoints;` – Heap-Allokation), bei Attachments zusätzlich `glm::inverse(worldMatrixOf(...))` (Parent-Kette + 4×4-Inverse), je Attachment `findByEntityId` + `worldPositionOf` (weitere Kettenwanderung), dann `owned.find(entityKey(e))` und `cm.getStaticMesh(...)` (zwei Lookups) bevor überhaupt `hash == builtHash` geprüft wird. Zusätzlich liefert `buildTrailGeometry` laut Header-Kommentar jeden Frame durch den Extractor ein `MeshData` **by value** (mehrere Vektoren) pro Trail.
- **Code-Auszug**:
```cpp
// RopeTrailSystem.cpp:66, 76, 300-317
std::vector<glm::vec3> pts = rope.controlPoints;           // Heap pro Rope und Frame
const glm::mat4 toLocal = glm::inverse(HE::worldMatrixOf(world, entity));
...
const std::vector<glm::vec3> pts = resolveControlPoints(world, e, rope);
const uint64_t hash = geometryHash(rope, pts);
...
if (ours && hash == rope.builtHash) continue;
```
- **Lösung**:
  1. `resolveControlPoints(world, e, rope, std::vector<glm::vec3>& out)` mit einem pro `update()` wiederverwendeten Scratch-Vektor (wie `m_tickScratch` in `EntityHost.h:155`); der alte Wert-Rückgabe-Overload bleibt als dünner Wrapper für Editor/Tests.
  2. Billigen Vorab-Test: Hat das Rope keine Attachments, ist der Hash eine reine Funktion der Komponente → Hash über `rope.controlPoints` direkt (ohne Kopie) bilden. `glm::inverse` nur berechnen, wenn ein Attachment tatsächlich vorhanden **und** dessen Weltposition sich gegenüber dem letzten Frame geändert hat (letzte Attachment-Weltpositionen in der Komponente merken).
  3. `buildTrailGeometry(trail, cameraPos, MeshData& out)` mit wiederverwendetem Puffer im Extractor (`out.clear()` behält Kapazität).
- **Erwarteter Effekt** (Schätzung): eine Heap-Allokation und mehrere Hash-/Chain-Lookups pro Rope und Frame weniger. Bei wenigen Ropes vernachlässigbar, bei Hunderten (Ketten, Seile) spürbar.
- **Risiko / Verifikation**: Der Hash muss bitgleich bleiben (er wird in Szenen nicht gespeichert, ist aber im Test `tests/test_rope_trail.cpp` gepinnt). Rebuild-Auslösung für bewegte Attachments darf nicht entfallen: Test „Attachment bewegt sich → Mesh wird ersetzt“ und „Rope unberührt → kein `replaceStaticMesh`“.

---

### SCN-405 – Entity-Id-Index: zwei Hash-Knoten pro Entity, ohne `reserve`
- **ID**: SCN-405
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/HorizonWorld.h:300-301`; Beleg `src/HE_Scene/src/HorizonWorld.cpp:408-424`, `:449-478`
- **Problem**: Jede Entity erzeugt über die `on_construct<EntityIdComponent>`-Signale einen `unordered_multimap`-Knoten (`m_idIndex`) **und** einen `unordered_map`-Knoten (`m_idIndexed`), also zwei Heap-Allokationen plus Rehashes (kein `reserve`), zusätzlich zu `NameComponent` (String), `HierarchyComponent::children` (Vektor) und `UUID::generate()`. Bei Szenen-Loads mit 200 k Entities (siehe `docs/world-streaming-baseline-2026-10-06.md`) sind das 400 k kleine Allokationen allein für den Index. `m_idIndexed` existiert nur, weil das Update-Signal den alten Schlüssel nicht liefert.
- **Code-Auszug**:
```cpp
// HorizonWorld.h:300-301
std::unordered_multimap<HE::UUID, Entity> m_idIndex;
std::unordered_map<Entity, HE::UUID>      m_idIndexed;
```
- **Lösung**:
  1. `m_idIndexed` durch einen Vektor `std::vector<HE::UUID> m_idOf` ersetzen, indiziert mit `entt::to_entity(e)` (entt-Entities sind dichte Indizes, der Vektor wird bei `onEntityIdSet` bei Bedarf vergrößert). Spart einen Knoten pro Entity und macht `unindexEntityId` zu einem Array-Zugriff.
  2. Für `m_idIndex` eine flache Hash-Map (z. B. `entt::dense_map` oder `std::vector` + offene Adressierung) verwenden; das Mehrfach-Halter-Verhalten (zwei Entities, eine UUID) bleibt über Mehrfacheinträge erhalten.
  3. `reserveEntities(size_t n)` an `HorizonWorld` anbieten und von den Scene-Loadern (n = Länge des `entities`-Arrays) aufrufen; ruft `m_registry.storage<...>().reserve(n)` und `m_idIndex.reserve(n)`.
- **Erwarteter Effekt** (Schätzung): beim Laden großer Szenen 30–50 % weniger Allokationen im Index-Pfad und weniger Rehash-Spitzen; Lookup `findByEntityId` bleibt O(1).
- **Risiko / Verifikation**: Semantik „zwei Halter einer ID bleiben beide auffindbar“ (`HorizonWorld.h:51-52`) muss erhalten bleiben. Tests: `tests/test_scene_entity_ids.cpp` (Kollisionen, Zerstörung des ersten Halters). Load-Zeit mit `SceneSerializer::lastLoadTiming()` vorher/nachher messen.

---

### SCN-406 – Jeder HE::api-Aufruf der Interpreter-/Registry-Pfade allokiert zwei `std::vector<Value>`
- **ID**: SCN-406
- **Kategorie**: B, C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/EngineApi.h:3040` (`ApiFn::invoke`), `:3050` (`find`), `:3090-3092` (`designTimeCallApi`); Beleg `src/HE_Scene/src/EngineApi.cpp:6021-6033`, `:8773-8795`
- **Problem**: Jede Registry-Zeile ist `std::function<std::vector<Value>(Ctx&, const std::vector<Value>&)>`. Jeder Aufruf aus dem HorizonCode-Interpreter, aus Lua/Python-Gruppen (`horizon.<group>.<fn>`) und aus generiertem C++ (`hc::callApi(ctx, "<id>", …)`) baut also (a) einen Argument-Vektor, (b) einen Ergebnis-Vektor (`VV{ Value::ofFloat(...) }`) und geht (c) über `std::function`-Indirektion und einen String-Lookup `find(const std::string&)` (Hash über den Id-String pro Aufruf, sofern der Aufrufer die Zeile nicht cacht). Für reine Mathe-Zeilen (`math.sin`, `math.clamp`, `str.length`) ist das ein Vielfaches der eigentlichen Arbeit, und genau diese Zeilen stehen in heißen Graph-Schleifen. Der Header-Kommentar `EngineApi.h:39-44` sagt selbst „no std::function … on the boundary“, die Registry tut aber das Gegenteil.
- **Code-Auszug**:
```cpp
// EngineApi.h:3040
std::function<std::vector<Value>(Ctx&, const std::vector<Value>&)> invoke;
// EngineApi.cpp:6033
[](Ctx& c, const VV& a){ return VV{ Value::ofString(entity::getName(c, (Entity)aI(a, 0))) }; }
```
- **Lösung**:
  1. Zusätzlich zu `invoke` einen optionalen, allokationsfreien Pfad anbieten: `using InvokeInto = void(*)(Ctx&, const Value* args, size_t n, Value* out)` (Funktionszeiger statt `std::function`, Ergebnis in vom Aufrufer bereitgestellten kleinen Puffer; `results.size()` ist je Zeile statisch bekannt). Zeilen ohne `invokeInto` fallen auf `invoke` zurück.
  2. Den Interpreter und `hc::callApi` so umstellen, dass sie `const ApiFn*` einmal pro Node cachen (Pointer ist laut `EngineApi.cpp:8775-8778` stabil) statt pro Aufruf per String zu suchen.
  3. Zunächst nur die reinen Mathe-/String-Gruppen (`math`, `str`, `time`, `random`) migrieren; dort ist der Gewinn am größten und die Thunks trivial.
- **Erwarteter Effekt** (Schätzung): für reine Zeilen Faktor 3–10 weniger Overhead pro Aufruf; für schwere Zeilen (Physik, Content) nicht relevant. Gesamteffekt hängt davon ab, wie viele API-Knoten pro Frame laufen (unsicher / messen mit Profiler-Scope um `invoke`).
- **Risiko / Verifikation**: Verhalten „Missing/extra args are tolerated“ (`EngineApi.h:3038-3039`) und die Parity zwischen Interpreter und Codegen muss gleich bleiben. Absicherung: die Invarianten-Tests in `tests/test_engine_api.cpp` (eine Callee-Zeile je Registry-Zeile) und die Codegen-Parity-Tests.

---

### SCN-407 – `apiCtx(L)` baut bei jedem Lua-Aufruf ein ~1,2 KB großes `Ctx` mit drei `std::function`-Kopien
- **ID**: SCN-407
- **Kategorie**: C, B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/EngineApi.h:60-218` (`struct Ctx`), Aufrufer `src/HE_Scene/src/ScriptContext.cpp:82-108`, `:145`, `:164` …
- **Problem**: `Ctx` enthält rund 35 `std::function`-Member (Fenster, Tray, Menü, Autostart …, je 32 Byte, zusammen > 1 KB). `apiCtx()` initialisiert das Struct bei jedem Aufruf (`HE::api::Ctx c{}`), **kopiert** dabei `createObject`, `destroyObject` und `requestQuit` (`std::function`-Kopien, bei größeren Captures mit Heap-Allokation) und zerstört alles wieder. Das passiert z. B. bei jedem `horizon.setPosition`/`setRotation` eines Skripts pro Frame.
- **Code-Auszug**:
```cpp
// ScriptContext.cpp:85-108
HE::api::Ctx c{};
c.world    = getWorld(L);
...
c.createObject  = hs.createObject;     // std::function-Kopie
c.destroyObject = hs.destroyObject;    // std::function-Kopie
...
if (const std::function<void()>* quit = getQuit(L)) c.requestQuit = *quit;
```
- **Lösung**:
  1. Host-Callbacks aus `Ctx` herausziehen in ein einmal pro Session gebautes `struct HostHooks` und in `Ctx` nur `const HostHooks* hooks` halten; die zeilen-spezifischen Zugriffe (`c.hooks->setWindowTitle`) ändern sich mechanisch. Alternativ `Ctx` pro `ScriptContext` einmal bauen (Member `m_apiCtx`, bei `setHostServices`/`setQuitHandler` neu) und per Referenz weiterreichen.
  2. Übergang: `apiCtx` liefert eine `const Ctx&` auf den gecachten Wert; Zeilen, die `Ctx&` mutieren wollen (`c.self`), bekommen eine billige Kopie nur dort.
- **Erwarteter Effekt** (Schätzung): pro Lua-Schreibaufruf mehrere hundert ns bis ~1 µs; bei vielen Skript-Instanzen mit Transform-Writes pro Frame messbar.
- **Risiko / Verifikation**: „Ctx wird mit positioneller Aggregat-Initialisierung gebaut, neue Member ans Ende“ (`EngineApi.h:112-117`) – die beiden Applikationen müssen mit angepasst werden. Messen (Mikrobenchmark 100 k `horizon.setPosition`) vor und nach; `tests/test_scripting.cpp`, `tests/test_engine_api.cpp`.

---

### SCN-408 – PlayerHost::tick erzeugt pro Frame Event-Namen-Strings und eine neue Aktions-Zustandsliste
- **ID**: SCN-408
- **Kategorie**: C, B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/PlayerHost.h:233-241` (`ActionInfo`), `:258` (`fireInputEvent`), `:271` (`m_actions`); Beleg `src/HE_Scene/src/PlayerHost.cpp:289`, `:337-425`; Gegenstück `EngineApi.h:2943-2949` (`setActions(std::vector<ActionState>)`)
- **Problem**: Für jede Axis-/Axis2D-Aktion wird **jeden Frame** `HE::inputEventAxis(a.name)` aufgerufen (neuer `std::string` aus `"Input." + name + ".Axis"`), und `Runtime::fireEvent(inst, event, …)` sucht den String in der Event-Tabelle (und `dispatchToListeners` hasht ihn erneut), selbst wenn kein Graph einen Handler hat. Außerdem wird pro Frame ein `std::vector<ActionState>` angelegt, jedes `ActionState` bekommt eine Kopie von `a.name` und der Vektor wird per Move in `setActions` ersetzt (`actions() = std::move(states)` gibt den alten Vektor samt Strings frei). In `tick` wird weiterhin pro Spieler `const std::string who = …` gebaut (bei > 1 Spielern Heap-Allokation pro Frame).
- **Code-Auszug**:
```cpp
// PlayerHost.cpp:352-358, 388, 423-425
std::vector<HE::api::input::ActionState> states;
states.reserve(m_actions.size());
for (const ActionInfo& a : m_actions) {
    HE::api::input::ActionState st;
    st.name = a.name;
    ...
    fireInputEvent(i, HE::inputEventAxis(a.name), HorizonCode::Value::ofFloat(v));
...
HE::api::input::setActions(std::move(states));
```
- **Lösung**:
  1. `ActionInfo` um die drei Event-Namen (`evAxis`, `evAxis2D`, `evPressed`, `evReleased`) erweitern, einmal in `begin()` füllen; `fireInputEvent` bekommt `const std::string&` darauf.
  2. `m_states` als Member-Vektor halten (Größe = `m_actions.size()`, Namen einmal setzen); pro Frame nur `down/pressed/released/x/y` zurücksetzen und `setActions` durch `publishActions(const std::vector<ActionState>&)` (Kopie der Zahlen, Namen unverändert) ersetzen.
  3. Axis-Events nur feuern, wenn ein Handler existiert oder sich der Wert geändert hat – **nur** wenn das beobachtbare Verhalten das erlaubt: Graphen nutzen `Input.<Action>.Axis` laut Kommentar als „Movement-Pump“ (`PlayerHost.cpp:381-383`), daher bleibt das Feuern pro Frame; nur die Namensbildung entfällt.
  4. `who` nur im Capture-Zweig bauen.
- **Erwarteter Effekt** (Schätzung): bei 10–20 Aktionen mehrere dutzend Allokationen pro Frame weniger; vor allem Cache- und Allokator-Entlastung im Game-Loop.
- **Risiko / Verifikation**: Reihenfolge der Events und die Aussage „stille Aktion wird als released mit Nullachse publiziert“ (`PlayerHost.cpp:345-348`) muss erhalten bleiben. Tests: `tests/test_player_host.cpp`, `tests/test_input_rebind.cpp`.

---

### SCN-409 – RpcRouter: `m_missingLogged` wächst unbegrenzt mit Client-gesteuerten Namen und wird linear durchsucht
- **ID**: SCN-409
- **Kategorie**: C, D (Speicher/Robustheit)
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Scene/include/HorizonScene/Net/RpcRouter.h:258-260`; Beleg `src/HE_Scene/src/Net/RpcRouter.cpp:523-534`, `src/HE_Scene/src/Net/PropertyReplicator.cpp:33-38`
- **Problem**: Für jeden eingehenden `CallServer`, dessen Funktion keine HorizonCode-Signatur hat, wird ein Schlüssel `std::to_string(netId) + ":" + fn` gebaut und per `alreadyLogged` linear in einem `std::vector<std::string>` gesucht und angehängt. `fn` stammt vom Client (bis 128 Zeichen, nur Länge geprüft). Ein Client, der eine eigene Entity besitzt, kann mit wechselnden Namen (bis 60 Aufrufe/s laut Rate-Limit, Standard `kMaxRpcPerSecond`) die Liste beliebig wachsen lassen; jeder Aufruf kostet dann O(n) und die Liste wird erst durch `clear()` am Sessionende frei. Dasselbe Muster (`alreadyLogged`) gilt für `m_localWriteLogged`/`m_doubleDeclareLogged` (dort Skript-gesteuert, harmloser) und `NetGameSession::m_refusedSpawnClasses`.
- **Code-Auszug**:
```cpp
// RpcRouter.cpp:528-529 + PropertyReplicator.cpp:33-37
const std::string key = std::to_string(netId) + ":" + fn;
if (!alreadyLogged(m_missingLogged, key)) HE_LOG_INFO(...);

bool alreadyLogged(std::vector<std::string>& seen, const std::string& key) {
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) return true;
    seen.push_back(key);
    return false;
}
```
- **Lösung**: Gemeinsamen Helfer `HE::OnceLog` (bounded set) einführen: `std::unordered_set<std::string>` mit Obergrenze (z. B. 256 Einträge); ab der Grenze wird nicht mehr gespeichert und nur noch ein einziges „further names suppressed“ geloggt. Die drei Vektoren und `alreadyLogged` durch diesen Helfer ersetzen.
- **Erwarteter Effekt**: begrenzter Speicher (Schutz vor Hostile-Client-Wachstum), O(1) statt O(n) je Aufruf; Logverhalten im Normalfall identisch.
- **Risiko / Verifikation**: Ausgabe „einmal pro (Entity, Name)“ muss bis zur Grenze identisch bleiben. Test: `tests/test_net_property_replication.cpp` / RPC-Tests um „300 verschiedene Namen → Speicher begrenzt, genau eine Unterdrückungsmeldung“ erweitern.

---

### SCN-410 – `prefs::set*` schreibt bei jedem Aufruf die komplette Datei synchron
- **ID**: SCN-410
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/EngineApi.h:2286-2303` (Doku: „written on every change“); Beleg `src/HE_Scene/src/EngineApi.cpp:4745-4750`, `:4766-4783`
- **Problem**: Jede `prefs.setString/setNumber/setBool/remove/clear`-Zeile ruft `store()`: `doc().dump(2)` (vollständiges, eingerücktes Serialisieren) plus `fs::writeText` (synchrones Schreiben der ganzen Datei) im Frame-Thread. Ein Skript, das während eines Slider-Drags oder pro Frame einen Wert setzt, erzeugt pro Aufruf einen Disk-Schreibvorgang. Die Datei wird dabei nicht atomar (temp + rename wie bei `save::write`) geschrieben; bei einem Absturz mitten im Schreiben ist die Datei beschädigt – das Gegenteil dessen, was der Kommentar („losing the user's settings to a crash“) verhindern soll.
- **Code-Auszug**:
```cpp
// EngineApi.cpp:4745-4750, 4766
void store() { if (!fs::writeText(kFile, doc().dump(2))) HE_LOG_WARN(...); }
void setString(Ctx&, const std::string& key, const std::string& value)
{ doc()[key] = value; store(); }
```
- **Lösung**: Dirty-Flag + verzögertes Schreiben: `setX` setzt nur `g_dirty = true` und merkt die Zeit; geschrieben wird (a) vom Host im Frame-Ende-Hook höchstens alle ~0,5 s (`fs::pollWatches`/`timer::poll` zeigen, dass es bereits einen Pro-Frame-Hook gibt), (b) beim Beenden/Play-Stop sicher (`prefs::flush()`), und (c) atomar über temp+rename. Die Zeile `prefs.save`-Semantik bleibt für Aufrufer unsichtbar.
- **Erwarteter Effekt** (Schätzung): Disk-I/O und JSON-Dump von pro Aufruf auf maximal zwei pro Sekunde; bei häufigem Schreiben von Einstellungen deutlicher Frame-Zeit-Gewinn.
- **Risiko / Verifikation**: Wer unmittelbar nach `setX` die Datei liest (Tests, `fs::readText("Prefs.json")`) sieht sonst den alten Stand → Flush-Punkte (Play-Stop, Shutdown, `fs`-Zugriff auf `Prefs.json`) festlegen und testen. Datenverlust-Fenster (Absturz) wird größer, aber durch atomares Schreiben bleibt die Datei konsistent.

---

### SCN-411 – Schwere Header: `HorizonWorld.h` und `EngineApi.h` ziehen ~3 900 Zeilen HE_Core in 190 bzw. 55 Dateien
- **ID**: SCN-411
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/include/HorizonScene/HorizonWorld.h:6-7,11,316-319`; `EngineApi.h:2-3`; `HcWatchEvents.h:2-3`; `Net/NetEvents.h:2-12`; `Net/NetGameSession.h:31-46`; `HorizonScene.h:1-60`
- **Problem**: `HorizonWorld.h` (inkludiert in ca. 190 Dateien in `src/` und `tests/`) inkludiert `HorizonCode/HorizonCode.h` (1 551 Zeilen), `HorizonCode/HorizonCodeRuntime.h` (886) und `UIWidget/WidgetManager.h` (1 441), weil `HorizonCode::Runtime`, `WidgetManager` und `HorizonCode::Graph` **by value** Member sind (`:316-323`). `EngineApi.h` (55 Dateien) zieht dieselben zwei HorizonCode-Header für `Ctx::runtime` (nur ein Zeiger) und `Value`. `HcWatchEvents.h` inkludiert `EngineApi.h` und `ScriptContext.h`, obwohl es nur Inline-Funktionen über Tokens enthält; `Net/NetEvents.h` und `Net/NetGameSession.h` inkludieren `ScriptEngine.h`, `AntiCheatHost.h`, `ProjectSettings.h` u. a. Jede Änderung an `WidgetManager.h` oder `HorizonCode.h` rebuildet damit den Großteil der Scene-Schicht, der Tests und beider Apps.
- **Code-Auszug**:
```cpp
// HorizonWorld.h:6-11, 316-319
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <UIWidget/WidgetManager.h>
...
HorizonCode::Runtime  m_ownScripts;
WidgetManager         m_ownWidgets;
```
- **Lösung**:
  1. In `HorizonWorld`: `std::unique_ptr<HorizonCode::Runtime> m_ownScripts` und `std::unique_ptr<WidgetManager> m_ownWidgets` (Konstruktor/Destruktor in der `.cpp`), `HorizonCode::Graph m_levelScript` ebenso; im Header nur Vorwärtsdeklarationen. `levelVariables()` (liefert eine `unordered_map<string, Value>`-Referenz, `:276`) in die `.cpp` verlegen.
  2. `EngineApi.h`: `Ctx::runtime` als `HorizonCode::Runtime*` bleibt Vorwärtsdeklaration; `Value`/`PinType` aus einem schlanken `HorizonCode/Value.h` (falls vorhanden/abtrennbar) statt der gesamten `HorizonCode.h`.
  3. `HcWatchEvents.h` und `NetEvents.h` auf Vorwärtsdeklarationen + `.cpp`-Implementierung umstellen oder nur die benötigten Header nennen.
- **Erwarteter Effekt** (Schätzung): spürbar kürzere Inkrementalbuilds (typisch 10–30 % weniger Parse-Last für Dateien, die nur `HorizonWorld` brauchen); keine Laufzeitwirkung.
- **Risiko / Verifikation**: Destruktionsreihenfolge (`m_ownScripts` muss vor den Widgets leben, Kommentar `:315`) bleibt zu beachten; Include-What-You-Use-Lauf bzw. ein voller Build von `he_tests`, `HorizonEditor`, `HorizonGame`. Hot-loaded GameLogic-DLLs: ABI/Layout von `HorizonWorld` ändert sich → neu bauen.

---

### SCN-412 – EntityHost::tick: zwei Durchläufe über eine `unordered_map` plus Snapshot pro Frame
- **ID**: SCN-412
- **Kategorie**: C, D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/EntityHost.h:110`, `:151-155`; Beleg `src/HE_Scene/src/EntityHost.cpp` (`EntityHost::tick`)
- **Problem**: Pro Frame wird `m_byEntity` (`unordered_map<uint32_t, InstanceId>`, knotenbasiert) zweimal komplett iteriert: einmal für den Reap (`registry().valid` je Eintrag) und einmal, um den Tick-Snapshot zu kopieren; danach pro Instanz `m_runtime->alive(inst)` und `fireTick`. Der Snapshot ist wiederverwendet (gut), der Reap ist aber O(Instanzen) pro Frame, obwohl ein Entity-Destroy-Signal (`registry.on_destroy`) den Fall in O(1) pro tatsächlichem Löschvorgang abdecken würde. Der Header selbst nennt die Wiederverwendung als Optimierung (`:153-155`).
- **Code-Auszug**:
```cpp
// EntityHost.cpp (tick)
for (const auto& [raw, inst] : m_byEntity)
    if (!m_world->registry().valid(static_cast<Entity>(raw))) { dead.push_back(inst); ... }
...
for (const auto& [raw, inst] : m_byEntity) m_tickScratch.push_back(inst);
```
- **Lösung**: `m_byEntity` als dichter Vektor `{entity, instance}` mit Index-Map nur für Lookups führen (Iteration cache-freundlich), Reap und Snapshot in einem Durchlauf erledigen (`valid()`-Prüfung und Kopie in dieselbe Schleife). Optional: `on_destroy`-Hook auf `ScriptComponent` markiert Instanzen als tot.
- **Erwarteter Effekt** (Schätzung): gering bei < 1 000 Instanzen; relevant erst bei großen Entity-Klassen-Szenen. Messen, bevor umgesetzt wird.
- **Risiko / Verifikation**: Ablauf „Reap vor Tick, Snapshot statt Live-Map“ ist sicherheitskritisch (Kommentar im Code); Reihenfolge der Ticks darf sich nicht ändern, falls Graphen darauf bauen (bisher hash-order, also nicht garantiert). Tests rund um `EntityHost` (Spawn/Destroy während Tick).

---

### SCN-413 – Input-Snapshot: pro Frame `vector<string>` + `unordered_set<string>` neu aufgebaut; Gamepad-Namen werden pro Abfrage geparst
- **ID**: SCN-413
- **Kategorie**: C, D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/EngineApi.h:2800-2838`; Beleg `src/HE_Scene/src/EngineApi.cpp:5738-5800`, `:5918-5931`
- **Problem**: `pushSdlSnapshot` läuft laut Header jeden Frame im Game: Es iteriert über alle SDL-Scancodes (ca. 500), baut einen `std::vector<std::string>` der gedrückten Tasten (Namen per `SDL_GetScancodeName`), und `setKeysDown` leert danach ein `std::unordered_set<std::string>` und fügt jeden Namen wieder ein (Knoten-Allokation pro gedrückter Taste und Frame). `keyDown(name)` hasht den Namen pro Abfrage; `gamepadButton`/`gamepadAxis` rufen pro Abfrage `SDL_GetGamepadButtonFromString` (String-Parsing). `findAction` in `actionDown/actionPressed/…` ist eine lineare Namenssuche über die Aktionsliste pro Abfrage.
- **Code-Auszug**:
```cpp
// EngineApi.cpp:5754-5757, 5762-5772
void setKeysDown(const std::vector<std::string>& names)
{ Snapshot& s = snap(); s.keys.clear(); for (const auto& n : names) s.keys.insert(n); }
void pushSdlSnapshot(float dx, float dy) {
    ...
    std::vector<std::string> down;
    for (int sc = 0; sc < n; ++sc)
        if (ks[sc]) { const char* name = SDL_GetScancodeName((SDL_Scancode)sc); ... down.emplace_back(name); }
```
- **Lösung**: Zustand als `std::bitset<SDL_SCANCODE_COUNT>`/`bool[512]` im Snapshot halten; `keyDown(name)` löst den Namen einmalig über eine statische `unordered_map<string_view, SDL_Scancode>` (beim ersten Aufruf gebaut) in einen Scancode auf. Gamepad-Namen analog in zwei kleine statische Tabellen. `setKeysDown(vector<string>)` bleibt als Kompatibilitäts-Pfad für Tests/Editor.
- **Erwarteter Effekt** (Schätzung): einige Allokationen und Hashes pro Frame weniger; klein, aber im Game-Loop konstant.
- **Risiko / Verifikation**: Namenssemantik („SDL scancode names“, Groß-/Kleinschreibung) exakt erhalten; Tests `tests/test_input_gamepad.cpp`, `tests/test_scripting.cpp`.

---

### SCN-414 – Poll-APIs der PhysicsWorld liefern `std::vector` pro Aufruf und Frame
- **ID**: SCN-414
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/PhysicsWorld.h:170`, `:738`, `:793`, `:806`, `:813-814`; Abfragen `:484`, `:532`, `:538`, `:560`
- **Problem**: `pollCollisionEnter/Exit`, `pollOverlapEnter/Exit`, `pollJointBroken`, `pollWaterEvents` geben jeweils einen neuen `std::vector` zurück und werden laut Doku „every frame“ aufgerufen. Bei Kontaktaktivität bedeutet das mehrere Heap-Allokationen pro Frame (sechs Vektoren) statt einer wiederverwendeten Liste. Dasselbe gilt für `overlapSphere/Box/Capsule`/`raycastAll` pro Skript-Abfrage (dort aber sachgerecht).
- **Code-Auszug**:
```cpp
// PhysicsWorld.h:793, 806, 813-814
std::vector<CollisionEvent> pollCollisionEnter();
std::vector<CollisionEvent> pollCollisionExit();
std::vector<CollisionEvent> pollOverlapEnter();
std::vector<CollisionEvent> pollOverlapExit();
```
- **Lösung**: Zusätzliche Überladung `void drainCollisionEnter(std::vector<CollisionEvent>& out)` (Tausch mit internem Puffer via `out.clear(); std::swap(out, m_events)`), vom `CollisionSystem` mit einem Member-Vektor aufgerufen; die Wert-Rückgabe bleibt als dünner Wrapper für Tests/Skripte.
- **Erwarteter Effekt** (Schätzung): bei Frames ohne Kontakte keiner (leere Vektoren allokieren nicht); bei Kontaktstürmen eine bis sechs Allokationen pro Frame weniger.
- **Risiko / Verifikation**: Semantik „drained, jedes Event genau einmal“ und Budget `eventsPerPoll` (`PhysicsBudget.h:35`) bleiben; `tests/test_physics_budget.cpp`.

---

### SCN-415 – Heap pro Entity bei Anlegen einiger Komponenten (Default-Member mit Allokation)
- **ID**: SCN-415
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `Components/SkeletalMeshComponent.h:11`, `Components/ScriptComponent.h:15`, `Components/RopeComponent.h:32`, `Components/UIButtonComponent.h:12`
- **Problem**: `SkeletalMeshComponent::boneMatrices = { glm::mat4(1.0f) }` allokiert bei **jeder** Konstruktion (auch beim Deserialisieren, wo der Vektor gleich danach ersetzt wird) 64 Byte auf dem Heap; derselbe Vektor wird beim Setzen durch den Animator ein zweites Mal neu dimensioniert. `ScriptComponent::properties` ist eine `unordered_map<string, ScriptPropValue>` pro Entity: libstdc++ allokiert für eine leere Map nichts, **MSVC allokiert beim Default-Konstruieren einen Sentinel-Knoten** (unsicher / messen auf Windows). `RopeComponent::controlPoints` hat einen Default mit zwei Punkten (Heap pro Rope, vertretbar), `UIButtonComponent::onClickFunction` ist ein `std::string` (SSO, kein Problem).
- **Code-Auszug**:
```cpp
// SkeletalMeshComponent.h:11
std::vector<glm::mat4> boneMatrices  = { glm::mat4(1.0f) };
// ScriptComponent.h:15
std::unordered_map<std::string, ScriptPropValue> properties;
```
- **Lösung**: `boneMatrices` leer lassen und die Konsumenten (`RenderExtractor`, Animationssysteme) „leer = Bind-Pose = eine Identität“ behandeln lassen, oder den Default lazy beim ersten Zugriff setzen; `ScriptComponent::properties` zu `std::vector<std::pair<std::string, ScriptPropValue>>` oder einem optionalen, erst bei Bedarf angelegten Container machen (Eigenschaftenzahl pro Skript ist klein).
- **Erwarteter Effekt** (Schätzung): bei 100 k Skelett-Entities beim Laden 100 k kleine Allokationen weniger; sonst gering.
- **Risiko / Verifikation**: Konsumenten, die `boneMatrices[0]` annehmen, würden bei leerem Vektor lesen – alle Zugriffe prüfen (Grep `boneMatrices`). Szenen-Roundtrip-Tests (`tests/test_scene_*.cpp`) und Extractor-Tests.

---

### SCN-416 – Zwei überlappende API-Schichten (ScriptApi und HE::api) und tote `cppCall`-Metadaten
- **ID**: SCN-416
- **Kategorie**: A, F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/include/HorizonScene/ScriptApi.h:19-204` gegenüber `EngineApi.h:224-324`, `:327-378`, `:1001-1018`, `:1029-1132`; `EngineApi.h:3031-3036` (`cppCall`); `:112-117` (Ctx-Positionsregel)
- **Problem**: `ScriptApi` (Doku: „single implementation behind the `horizon` module“) und `HE::api` (Doku: „single, engine-wide C++ gameplay API“) deklarieren dieselben Operationen (Name, Position/Rotation/Skalierung, `spawn/destroy`, UI-Text/Farbe/Position, ~25 Widget-Funktionen, Theme, Cursor). Die `HE::api`-Zeilen sind in 70 Fällen nur Weiterleiter an `ScriptApi::…` (`EngineApi.cpp:247-249`, `:634-637`, …); die Lua-Bindings rufen teils `ScriptApi`, teils `HE::api` (`ScriptContext.cpp:117-447`). Jede Operation existiert dadurch dreifach (Deklaration `ScriptApi`, Deklaration `HE::api`, Registry-Zeile) plus die Binding-Shims. `ApiFn::cppCall` ist laut Header-Kommentar „nothing emits it today“ (`:3033`), kostet aber einen String-Zeiger pro Zeile und beim Pflegen eine zusätzliche Redundanz, die nur ein Invarianten-Test absichert. Die Aussage in `EngineApi.h:3055-3058` („until ScriptApi is inverted onto HE::api“) bestätigt den Übergangszustand.
- **Code-Auszug**:
```cpp
// EngineApi.h:3036
const char* cppCall;   // "nothing emits it today"
// EngineApi.cpp:247
std::string getName(Ctx& c, Entity e) { return c.world ? ScriptApi::getName(*c.world, e) : std::string(); }
```
- **Lösung**: Die Inversion zu Ende führen (Lua/Python-Shims rufen ausschließlich `HE::api`, `ScriptApi.cpp` schrumpft auf interne Helfer oder verschwindet), und `cppCall` entfernen, bis der Codegen es tatsächlich benutzt (oder es per Makro nur in Test-Builds führen). Reihenfolge: zuerst Transform/Entity/UI (dünne Weiterleiter), dann Widgets.
- **Erwarteter Effekt**: weniger Redundanz (~500 Zeilen `ScriptApi.cpp` + Header), eine Stelle pro neuer Funktion; keine Laufzeitänderung.
- **Risiko / Verifikation**: Python-Backend (`PyScriptBackend`) und der CPython-Plugin-ABI-Pfad nutzen die gleichen Funktionen – Kompatibilität prüfen (`tests/test_python_scripting.cpp`, `tests/test_scripting_binding.cpp`, `tests/test_engine_api.cpp`).

---

### SCN-417 – Prefab-Index mit linearer Suche; Clipboard-Check parst bei jedem Menü-Frame das ganze JSON
- **ID**: SCN-417
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/SceneSerializer.h:303-317` (`PrefabRecordIndex::find`), `:424-427` (`componentKeyOfText`); Beleg `src/HE_Scene/src/SceneSerializer.cpp:3613-3626`
- **Problem**: (1) `PrefabRecordIndex::find` ist eine lineare Suche über `records`; der Kommentar sagt, der Inspector frage jeden Frame nach der Struktur (`:300-303`), `prefabStructureOf`/`prefabRemovedHereCount` rufen `find` pro Binding → O(n²) bei großen Prefabs. (2) `componentKeyOfText` ist „cheap enough to ask every frame a menu is open“ (`:425-426`), parst aber nach einem `find` auf den Envelope-Schlüssel das gesamte Clipboard als JSON; kopiert man eine Terrain-Komponente (Base64-Höhenfeld, mehrere MB), wird dieses Parsen pro Frame wiederholt, solange das Menü offen ist. Beides läuft nur im Editor.
- **Code-Auszug**:
```cpp
// SceneSerializer.h:311-316
const Record* find(const HE::UUID& uuid) const
{
    for (const Record& r : records)
        if (r.uuid == uuid) return &r;
    return nullptr;
}
// SceneSerializer.cpp:3617-3618
if (text.size() < 2 || text.find(kComponentEnvelopeKey) == std::string::npos) return {};
const json envelope = json::parse(text, nullptr, false);
```
- **Lösung**: `PrefabRecordIndex` bekommt beim Aufbau (`indexPrefabRecords`) eine `unordered_map<UUID, uint32_t>` (oder sortierten Index); `componentKeyOfText` liest nur die ersten ~200 Bytes des Textes per Mini-Scanner nach `"horizonComponent": "<key>"` und parst erst bei `importComponentText` voll; zusätzlich Ergebnis pro Clipboard-Inhalt (Hash/Länge + Zeiger) cachen.
- **Erwarteter Effekt**: Editor-Only; kleine ms-Spitzen bei großen Prefabs/Clipboards entfallen.
- **Risiko / Verifikation**: Mini-Scanner muss exakt dieselben Texte akzeptieren/ablehnen wie `json::parse` (Escapes, Reihenfolge der Keys) – mit Tests in `tests/test_scene_serializer*.cpp`, die die aktuelle Akzeptanzmenge pinnen; im Zweifel nur das Caching umsetzen.

---

### SCN-418 – Toter API-Code in `HorizonWorld`: `hasComponent`/`removeComponent` ohne Aufrufer, `decltype(component)`-Trick
- **ID**: SCN-418
- **Kategorie**: A, F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/include/HorizonScene/HorizonWorld.h:197-209`
- **Problem**: `hasComponent(Entity, auto&& componentType)` und `removeComponent(Entity, auto&& componentType)` erwarten eine **Instanz** der Komponente (`std::decay_t<decltype(componentType)>`) nur, um daraus den Typ abzuleiten – man muss also ein Objekt konstruieren, um zu fragen, ob eine Entity es hat. Ein Grep über `src/` und `tests/` findet für beide Funktionen **keinen einzigen Aufrufer**; `addComponent` hat ca. 620 Aufrufer. Die API ist damit toter Code, der außerdem unnatürlich zu benutzen wäre; `registry().all_of<T>(e)` bzw. `.remove<T>(e)` sind die tatsächlich verwendeten Wege.
- **Code-Auszug**:
```cpp
bool hasComponent(Entity entity, auto&& componentType)
{ return m_registry.all_of<std::decay_t<decltype(componentType)>>(entity); }
```
- **Lösung**: `hasComponent` und `removeComponent` löschen. Für `addComponent` ein typbasiertes `template<class T, class... A> T& add(Entity e, A&&... a)` ergänzen (`emplace<T>`), das auch die Referenz zurückgibt, und `addComponent` nur als dünnen Alias behalten.
- **Erwarteter Effekt**: weniger API-Fläche; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Grep in `vendor`-fremdem Projekt-/Plugin-Code und in GameLogic-Templates (`ProjectTemplates`) auf externe Nutzung; Build aller Targets.

---

### SCN-419 – Veraltete bzw. verrutschte Kommentare
- **ID**: SCN-419
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `Components/SequencePlayerComponent.h:66-68`; `HorizonWorld.h:181-183`; `Components/TerrainComponent.h:7-16`; `Net/NetGameSession.h:329-337`
- **Problem**:
  1. `SequencePlayerComponent.h:66-68` sagt, die Slot-Auflösung sei „a scan over every EntityIdComponent“ und deshalb nicht pro Frame – `HorizonWorld::findByEntityId` ist aber seit dem Id-Index O(1) (`HorizonWorld.h:40-47`, `SequenceEval.h:88-91`). Die Begründung für die „lazy, einmal“-Auflösung stimmt nicht mehr; die Vorsicht wegen eines noch streamenden Assets bleibt der eigentliche Grund.
  2. `HorizonWorld.h:181-183`: „environmentEntity() / weatherEntity() … (scans the registry)“ – tatsächlich liest `view<EnvironmentComponent>` das erste Element des Pools (`HorizonWorld.cpp:322-336`), also O(1).
  3. `TerrainComponent.h:7-8`: der Kommentar „Paintable landscape layers: two RGBA8 weightmap pages…“ steht über dem Text zu `kTerrainMaxResolution` und gehört zu `kTerrainWeightPages` (Zeile 17).
  4. `NetGameSession.h:329-337`: der Block „Hand the RUNTIME the door back into the session…“ beschreibt `installRpcHook()`/`setVariableSource`, steht aber vor `localCharacter()`; ein Leser bezieht ihn auf die falsche Funktion.
- **Code-Auszug**:
```cpp
// SequencePlayerComponent.h:66-68
// Slot → entity, resolved at play() and on bindSlot. Not per frame: the
// lookup is a scan over every EntityIdComponent.
```
- **Lösung**: Kommentare korrigieren bzw. an die richtige Deklaration verschieben; kein Code-Effekt.
- **Erwarteter Effekt**: weniger Fehlinformation beim Lesen (insbesondere Punkt 1 verleitet dazu, die Auflösung „aus Performancegründen“ nie zu wiederholen).
- **Risiko / Verifikation**: keines.

---

### SCN-420 – `HcCodegen.h`: `std::function` ohne `<functional>`, Toolchain-/Prozessverwaltung im Scene-Header
- **ID**: SCN-420
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/include/HorizonScene/HcCodegen.h:2-5`, `:64`, `:197-198`, `:211-212`
- **Problem**: Der Header benutzt `std::function` (`Options::onClass`, `installToolchain`, `buildDylib`), inkludiert aber nur `HorizonCode/HorizonCode.h`, `<filesystem>`, `<string>`, `<vector>`. Er kompiliert nur, weil `HorizonCode.h` transitiv `<functional>` bringt. Inhaltlich mischt der Header Codegen (`generate`), Toolchain-Erkennung/Installation (`probeToolchain`, `installToolchain` mit winget/brew/pkexec), Visual-Studio-Auswahl und GameLogic-Build (`gameLogicSpec`). Das ist Editor-/Tool-Infrastruktur, hat keine Abhängigkeit zum ECS, liegt aber in `HE_Scene/include/HorizonScene` und wird von `hc_codegen`, Editor und Tests inkludiert.
- **Code-Auszug**:
```cpp
// HcCodegen.h:2-5, 64
#include <HorizonCode/HorizonCode.h>
#include <filesystem>
#include <string>
#include <vector>
...
std::function<void(const std::string& label, size_t index, size_t count)> onClass;
```
- **Lösung**: `#include <functional>` ergänzen; mittelfristig Toolchain-Teil (`probeToolchain` … `gameLogicSpec`, ~150 Zeilen) in einen eigenen Header (`HorizonScene/HcToolchain.h` oder ins Tools-Modul) verschieben.
- **Erwarteter Effekt**: robustere Includes; klarere Modulgrenze.
- **Risiko / Verifikation**: Include-Pfade in `src/HE_Tools/hc_codegen`, `src/HE_Editor` und Tests (`tests/test_hc_codegen*.cpp`) anpassen; voller Build.

---

## Gelesen

Ja, alle 70 Dateien des Slice 4 (`docs/audit/_slices/Scene.txt`, Abschnitt `## slice 4`) wurden vollständig gelesen, `EngineApi.h` (3 094 Zeilen) in fünf aufeinanderfolgenden Blöcken ohne Auslassung; die zugehörigen `.cpp`-Dateien (`PropertyReplicator.cpp`, `RpcRouter.cpp`, `GameReplication.cpp`, `RopeTrailSystem.cpp`, `LODSystem.cpp`, `PlayerHost.cpp`, `EntityHost.cpp`, `ScriptContext.cpp`, `HorizonWorld.cpp`, Teile von `EngineApi.cpp`, `PhysicsWorld.cpp`, `TransformHierarchy.cpp`, `SceneSerializer.cpp`) wurden nur gezielt als Beleg gelesen, nicht vollständig.

## Scene Slice 5 – SplineCurve.h … ParticleSystem.cpp (Spline*, Terrain*, Timer, TransformHierarchy.h, UI*, Water*, Weather*, HorizonWorld, LOD, Movement, Navigation, Net/*, NotifyCollect.h, ParticleSystem)

## Zusammenfassung

Der Slice besteht zur Hälfte aus gut dokumentierten, reinen Deklarations-Headern (Water*, Terrain*, Spline*, UI*) und zur Hälfte aus Laufzeitcode (HorizonWorld, LOD/Movement/Navigation/Particle, Net-Replikation). Der Code ist insgesamt sauber und sehr gut kommentiert; die Dirty-Flag-Disziplin im Terrain/Water-Bereich ist vorbildlich (Region-Dirty, Revision, Hash). Die Funde konzentrieren sich auf die Per-Frame-Systeme: `PropertyReplicator::update` allokiert pro Frame und pro replizierter Entity mehrere Vektoren samt String-Kopien, `LODSystem`, `MovementSystem` und `NavigationSystem` iterieren jeden Frame alle Komponenten ohne Änderungserkennung, `ParticleSystem` führt pro Partikel einen Raycast aus, und `HorizonWorld::clear()` ist bei flachen Hierarchien quadratisch. Insgesamt 16 Funde: 1 Hoch, 7 Mittel, 8 Niedrig. Alle Laufzeitangaben sind Schätzungen; es lagen keine Messdaten vor.

---

### SCN-501 – PropertyReplicator::update allokiert pro Frame und Entity
- **ID**: SCN-501
- **Kategorie**: C (auch D)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/Net/PropertyReplicator.cpp:356-468 (Kern 360, 386-400, 410, 423, 465-466); Hilfsfunktion 92-124
- **Problem**: `update()` läuft jeden Frame auf dem Host (`NetGameSession::update` → `m_properties->update()`). Für JEDE replizierende Entity mit Tabelle wird `collect()` aufgerufen. Dabei entstehen pro Entity und Frame drei neue Vektoren (`names`, `notify` als `vector<bool>`, `current`), jeder Variablenname wird als `std::string` kopiert und jeder Wert (`HorizonCode::Value`, eine große Struktur mit String/Vektoren) kopiert, nur um anschließend per `names != t.names` (String-Vergleiche) festzustellen, dass sich nichts geändert hat. Zusätzlich wird die Variablenliste der Runtime (`replicatedVariablesOf(inst)`) pro Entity und Frame abgefragt und jeder Wert über den Namen (`getVariable(inst, rv.name)`, String-Lookup) geholt. Dazu kommen pro Frame: `joinedConnections()` (Vektor), ein zweiter Durchlauf über alle `m_tables` mit `entityOf()`, ein dritter Durchlauf über die `NetworkComponent`-View mit `m_tables.find`, und pro Änderung `noteCost(t.names[idx], …)` mit String-Hash. Es gibt keinen Dirty-Hinweis der Quelle (Runtime-Variable / `ReplicatedVarsComponent`), jede Entity wird jeden Frame komplett neu eingesammelt.
- **Code-Auszug**:
```cpp
for (auto [entity, nc] : m_world->registry().view<NetworkComponent>().each())
{
    ...
    std::vector<std::string> names;
    std::vector<bool>        notify;
    std::vector<Value>       current;
    collect(entity, names, notify, current);
    ...
    if (names != t.names) { broadcastTableFor(nc.netId); continue; }
```
- **Lösung**:
  1. Scratch-Puffer (`m_scratchNames/…`) als Member halten und in `collect` nur `clear()` aufrufen (Kapazität bleibt) – entfernt die Heap-Allokationen sofort, Verhalten identisch.
  2. Zweistufig vergleichen: zuerst nur die Deklaration prüfen, ohne Werte und Namen zu kopieren (Anzahl + Referenzen auf `rv.name` gegen `t.names`), erst bei Gleichheit die Werte in-place gegen `t.values` vergleichen (`getVariable` liefert dann nur noch einen Wert pro Variable; `valuesEqual` direkt auf dem Runtime-Wert).
  3. Entities ohne replizierte Variablen (leere Tabelle, `t.names.empty()` bei unveränderter Deklaration) in `Table` als `noVars` markieren und überspringen, bis eine Deklarationsänderung gemeldet wird (Runtime-Hook „Variable deklariert/Klasse neu geladen“, oder billiger Zähler `Runtime::replicatedRevision(inst)`).
  4. `conns` und die Tabellen-Aufräumschleife (379-384) nur bei Roster-/Netzid-Änderung statt jedes Frames ausführen.
  5. `m_costs` mit Index (`uint8_t idx` je Tabelle) statt `std::string`-Schlüssel führen.
- **Erwarteter Effekt**: Schätzung: bei 500 replizierten Entities mit je 3 Variablen entfallen ~2.000-3.000 Heap-Allokationen pro Frame (3 Vektoren + 3 Namen + ≥3 Values × 500); Host-Frametime-Anteil dieser Funktion sinkt vermutlich um eine Größenordnung. Messen mit HE_PROFILE_SCOPE um `m_properties->update()`.
- **Risiko / Verifikation**: Die Reihenfolge (HorizonCode-Variablen zuerst, dann `ReplicatedVarsComponent`, Dubletten übersprungen) und die Index-Semantik müssen erhalten bleiben; der Fall „Typ während der Session geändert“ (valueTypesMatch) und „Deklaration wächst mitten in der Session“ braucht Tests (bestehende Net-Tests unter `tests/` für PropertyReplicator, z. B. Tabelle neu senden bei neuer Variable). Die Dirty-Variante (Punkt 3) braucht einen Hook im Runtime und ist daher riskanter als 1+2.

### SCN-502 – LODSystem: Weltposition jeder LOD-Entity jeden Frame über die Elternkette
- **ID**: SCN-502
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/LODSystem.cpp:13-62 (Kern 33-35); src/HE_Scene/src/TransformHierarchy.cpp:321-367 (Aufrufer-Seite, Referenz)
- **Problem**: Die View `LODComponent, MeshComponent` wird jeden Frame vollständig durchlaufen. Pro Entity wird `worldPositionOf` aufgerufen: das läuft die Elternkette mit `try_get<TransformComponent>` + `try_get<HierarchyComponent>` pro Ebene hoch und multipliziert pro Ebene eine `mat4` (`cachedLocalMatrix`), nur um die Translation zu lesen. Es gibt keine Änderungserkennung: Kamera still und Entity unverändert ergibt dennoch dieselbe Rechnung jeden Frame. Terrain-Chunks (Hunderte) sind der Hauptfall. Der Kommentar nennt den Grund (frische Entities, `worldMatrix` noch veraltet); das rechtfertigt aber nicht, den Vollpfad für alle statischen Entities jeden Frame zu nehmen. `TransformHierarchy.cpp` erwähnt selbst „baseline §4.5“, dass dies messbar war.
- **Code-Auszug**:
```cpp
auto view = registry.view<LODComponent, MeshComponent>();
for (auto [entity, lod, mesh] : view.each())
{
    if (lod.levels.empty()) continue;
    const glm::vec3 pos = HE::worldPositionOf(world, entity);
    const float dist = glm::distance(cameraPos, pos);
```
- **Lösung**:
  1. Pro LOD-Entity die zuletzt berechnete Weltposition + `structureEpoch` cachen (Laufzeitfeld in `LODComponent`, nicht serialisiert). Neu berechnen nur wenn `structureEpoch` anders ist, `tc.dirty`/Wertvergleich der Kette anschlägt (derselbe Mechanismus wie `propagateTransforms`), oder die Entity frisch ist (Cache leer). Alternative: erst `propagateTransforms(world)` (inkrementell, nur gescannt) am Anfang von `LODSystem::update` aufrufen und dann `tc.worldMatrix[3]` lesen – das beseitigt auch die Sonderbehandlung.
  2. Zusätzlich: Bleibt die Kamera um weniger als der kleinste Abstand zur nächsten Schwelle (pro Entity `distToNextBoundary`) in Bewegung, die Entity überspringen.
  3. Mit quadrierten Distanzen (`maxDistance²` vorab) vergleichen, spart `sqrt` – nur zusammen mit 1 oder 2 sinnvoll, allein eine Mikro-Optimierung.
- **Erwarteter Effekt**: Schätzung: bei ruhiger Kamera und statischer Welt entfällt >90 % der Arbeit der Funktion; bei 1.000 Chunks/Props und Tiefe 2-3 sind das ~3.000 Sparse-Set-Lookups + 2.000 mat4-Produkte pro Frame. Messen (Profiler-Scope „LOD“ existiert in `SceneSystems.cpp:151`).
- **Risiko / Verifikation**: Frisch erzeugte Chunks (TerrainSystem baut am Anfang des Ticks) müssen im selben Tick korrekt eingestuft werden – genau der im Kommentar beschriebene Fehler (alle Chunks vom Ursprung aus gemessen). Test: bestehender Terrain-LOD-Test plus neuer Test „Entity im selben Tick erzeugt und verschoben, Level stimmt“. Floating-Origin (`shiftWorldOrigin`) muss den Cache invalidieren (`invalidateWorldMatrices`).

### SCN-503 – ParticleSystem: ein Physik-Raycast pro Partikel und Frame
- **ID**: SCN-503
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/ParticleSystem.cpp:164-194
- **Problem**: Bei `collisionEnabled` ruft `stepPool` für jedes lebende Partikel jeden Frame `physics->raycast(oldPos, delta, dist)` auf. Bei einem Emitter mit 1.000 Partikeln sind das 1.000 Jolt-Raycasts pro Frame, bei mehreren Emittern ein Vielfaches. Die meisten Partikel bewegen sich pro Frame nur Millimeter bis Zentimeter. Es gibt weder Budget noch Staffelung (alle N Frames) noch eine grobe Vorprüfung (z. B. Emitter-AABB gegen Broadphase).
- **Code-Auszug**:
```cpp
if (collide)
{
    const glm::vec3 delta = newPos - oldPos;
    const float     dist  = glm::length(delta);
    if (dist > 1e-6f)
    {
        const PhysicsWorld::RaycastHit hit = physics->raycast(oldPos, delta, dist);
```
- **Lösung**: (a) Pro Emitter ein Raycast-Budget pro Frame (z. B. `maxCollisionRays`, Round-Robin über den Pool: Partikel i wird nur geprüft, wenn `(i + frame) % k == 0`; das Segment wird dann auf `k·dt` verlängert, so dass kein Tunneling entsteht). (b) Vorprüfung: einmal pro Emitter und Frame die AABB der Partikel (oder Emitter-Radius) gegen die Physik prüfen (Overlap-Test); trifft sie nichts, werden alle Raycasts übersprungen. (c) Batch-API in `PhysicsWorld` (ein Lock/Query-Setup für viele Strahlen).
- **Erwarteter Effekt**: Schätzung: (a) mit k = 2-4 halbiert bis viertelt die Kosten; (b) lässt Partikel in freier Luft ganz kostenlos werden. Messen mit Szene „Regen/Funken 5 Emitter × 1.000 Partikel, Kollision an“.
- **Risiko / Verifikation**: Sichtbares Verhalten ändert sich leicht (Reflexion mit ≤ k Frames Verzögerung). Für exakte Erhaltung nur (b) und (c). Bestehende Particle-Tests (Reflexion/`killOnCollision`) prüfen; neuer Test mit Wand und schnellem Partikel (Tunneling).

### SCN-504 – ParticleSystem: Zustand, Konfig-Auflösung und Pool pro Emitter
- **ID**: SCN-504
- **Kategorie**: C (auch F)
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/ParticleSystem.cpp:63-98 (Auflösung), 107-142, 196-200; src/HE_Scene/include/HorizonScene/Components/ParticleSystemComponent.h:41-42 (Komponente, nur gelesen)
- **Problem**: (1) `resolveConfig` legt vor jedem Auflösen mit `HE::ParticleGraph::makeDefault()` einen Default-Graphen an (Heap), obwohl er direkt danach durch den geparsten ersetzt wird (74-81). (2) Jeder Emitter parst das Graph-JSON selbst; 100 gespawnte Treffer-Effekte (der Kommentar nennt „every few frames“) parsen dasselbe JSON 100-mal und evaluieren 100-mal. (3) `ParticleSystemComponent` enthält ein `std::mt19937 rng {42}` (~2,5 KB Zustand, 624-Wort-Initialisierung bei jeder Konstruktion/Kopie) plus die `LegacyConfig` (~150 B) inline; die Komponentenstride der View beträgt damit >3 KB und der Frame-Durchlauf über viele Emitter (nur `playing`, `emitAccumulator`, `particles` werden gelesen) trifft pro Emitter mehrere Cachelines. (4) `particles` wird nie mit `reserve(maxParticles)` vorbelegt (mehrfaches Wachsen beim ersten Füllen) und `erase(remove_if)` läuft jeden Frame auch wenn kein Partikel starb. (5) Der Kommentar „Remove dead particles (swap with back for O(1) removal)“ beschreibt etwas anderes als den Code (`remove_if` ist stabil und O(n)).
- **Code-Auszug**:
```cpp
HE::ParticleGraph graph = HE::ParticleGraph::makeDefault();
if (const ParticleGraphAsset* asset = cm.getParticleGraph(ps.particleAssetId);
    asset && !asset->nodeGraphJson.empty())
{
    HE::ParticleGraph parsed;
    if (HE::particleGraphFromJson(asset->nodeGraphJson, parsed))
        graph = std::move(parsed);
```
- **Lösung**: (1) `graph` erst im Fehler-/Fallback-Zweig mit `makeDefault()` belegen. (2) Geparsten Graphen pro Asset-ID cachen (`unordered_map<UUID, shared_ptr<const ParticleGraph>>` im System oder im `ContentManager`, invalidiert durch `markConfigDirty`/Asset-Revision); `evaluateParticleGraph` bleibt pro Emitter, weil es `ps.rng` verbraucht (RandomRange). (3) RNG durch einen kleinen Generator ersetzen (PCG32/xoshiro, 8-16 B) oder die `LegacyConfig` per `unique_ptr` auslagern (nur für Migration nötig); der RNG-Tausch ändert die Zufallsfolge. (4) `particles.reserve(min(maxParticles, 256))` beim ersten Emittieren; Erase nur, wenn ein Partikel in der Schleife starb (Flag). (5) Kommentar korrigieren.
- **Erwarteter Effekt**: Schätzung: (1)+(2) beseitigen pro neu gespawntem Effekt ein JSON-Parse + ~100 Heap-Allokationen; (3) senkt den Speicher pro Emitter um ~2,5 KB (bei 1.000 Emittern 2,5 MB) und verbessert die Cache-Dichte beim Durchlauf; (4) entfernt Reallokationen.
- **Risiko / Verifikation**: (3) ändert deterministische Partikelfolgen (Tests mit festem Seed 42, Particle-Graph-Editor-Vorschau „same seed same result“); daher als separaten Schritt und mit Test-Updates. (2) Cache-Invalidierung bei Asset-Edit (Particle Graph Editor) testen.

### SCN-505 – HorizonWorld::clear() ist bei flachen Hierarchien quadratisch
- **ID**: SCN-505
- **Kategorie**: D (auch C)
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/HorizonWorld.cpp:535-548 (clear), 496-511 (destroyEntity), 480-494 (destroyRecursive)
- **Problem**: `clear()` ruft für jedes Kind der Wurzel `destroyEntity(c)`. Jedes `destroyEntity` löscht die Entity per `erase(remove(...))` aus `children` der Wurzel (linear). Bei n Wurzelkindern sind das ~n²/2 Vergleiche; `createEntity` hängt alles zuerst an die Wurzel, daher sind flache Szenen (Prefab-Spawns, Foliage-/Cell-Entities, Tests) üblich. Bei 100.000 Wurzelkindern ≈ 5·10⁹ Elementvergleiche (Schätzung). Danach baut `clear()` zusätzlich einen `strays`-Vektor aller Entities und durchläuft sie noch einmal; `destroyRecursive` kopiert pro Knoten den `children`-Vektor (Heap-Allokation pro Entity mit Kindern). Da jede Entity einen `EntityIdComponent` hat, feuert zusätzlich pro Zerstörung der `on_destroy`-Hook (siehe SCN-506).
- **Code-Auszug**:
```cpp
const std::vector<Entity> children = rh->children;
for (Entity c : children)
    destroyEntity(c);
std::vector<Entity> strays;
for (auto e : m_registry.view<entt::entity>())
    if (e != m_rootEntity) strays.push_back(e);
```
- **Lösung**: In `clear()` zuerst `rh->children.clear()` (die Wurzel behält am Ende keine Kinder, dasselbe Ergebnis) und dann alles außer der Wurzel direkt zerstören – die zweite Schleife (`strays`) erledigt das bereits vollständig, die `destroyEntity`-Schleife ist damit überflüssig (der Built-in-Guard wird ohnehin umgangen, siehe Kommentar 547). Alternativ `m_registry.destroy(view.begin(), view.end())`-Bereich, wobei die Wurzel auszunehmen ist. In `destroyRecursive` die Kinder vom Ende her abbauen (`while(!ch.empty()){ c = ch.back(); ch.pop_back(); destroyRecursive(c);}`) statt zu kopieren; wenn die Kinder-Liste des Elternknotens anschließend ohnehin freigegeben wird, spart das die Kopie.
- **Erwarteter Effekt**: Schätzung: Szenenwechsel/PIE-Stop mit 100k flachen Entities von Sekunden/Minuten auf Millisekunden bis ~100 ms; Einsparung einer Heap-Allokation je Entity mit Kindern.
- **Risiko / Verifikation**: `m_structureEpoch`/`m_hierarchyDirty` müssen weiter gesetzt werden (Zeilen 548, 562); `fireLevelUnloaded`/Widgets/Level-Skript-Reihenfolge bleibt unberührt. Test: `clear()` mit 50k Wurzelkindern (Laufzeit), Sky + Sonne/Mond wird entfernt, Wurzel bleibt mit leerem `children`, `findByEntityId` danach leer.

### SCN-506 – Entity-ID-Index: zwei Hash-Knoten pro Entity
- **ID**: SCN-506
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/HorizonWorld.cpp:449-478 (+ 408-424 createEntity); src/HE_Scene/include/HorizonScene/HorizonWorld.h:294-301 (nur gelesen)
- **Problem**: Jede Entity erzeugt zwei Heap-Knoten: einen in `m_idIndex` (`unordered_multimap<UUID, Entity>`) und einen in `m_idIndexed` (`unordered_map<Entity, UUID>`). Dazu läuft pro Konstruktion/Update/Zerstörung ein Signal-Delegat (`on_construct`, `on_update`, `on_destroy`), und `unindexEntityId` macht `equal_range` + lineare Suche über die Halter derselben ID. Beim Laden großer Szenen (100k Entities) sind das 200k kleine Allokationen und doppeltes Hashing (UUID 128 Bit); `createEntity` ruft zusätzlich `UUID::generate()` und kopiert den Namen (`NameComponent{ name }`, Heap bei > SSO) sowie legt `HierarchyComponent` mit einem Kinder-Vektor an. Die Rückwärtsabbildung dient nur dazu, bei `on_update` die alte ID zu kennen.
- **Code-Auszug**:
```cpp
m_idIndex.emplace(id, entity);
m_idIndexed[entity] = id;
...
const auto holders = m_idIndex.equal_range(indexed->second);
```
- **Lösung**: Die Rückwärtsabbildung als dichten Vektor über den Entity-Index führen (`std::vector<HE::UUID> m_indexedId` indexiert mit `entt::to_entity(e)`, Null-UUID = nicht indexiert): kein Knoten pro Entity, O(1) ohne Hash. Für `m_idIndex` `reserve()` beim Szenenladen (Anzahl bekannt) bzw. eine offene Adressierung (flat map) verwenden; die seltenen Duplikate (zwei Halter derselben ID) in einer kleinen Nebenliste führen, so dass der Normalfall eine einfache `unordered_map<UUID, Entity>` ist. `createEntity` mit `const std::string&` + `std::move`-Überladung für Namen.
- **Erwarteter Effekt**: Schätzung: Halbierung der Allokationen beim Laden/Spawnen (−1 Knoten/Entity), weniger Fragmentierung; messbar im Scene-Load-Profil (nicht gemessen).
- **Risiko / Verifikation**: Die Semantik „zwei Halter derselben ID bleiben findbar“ (Kommentar 469) und „Null-ID wird nicht indexiert“ müssen bleiben. Tests: bestehende World-ID-Tests (Prefab zweimal, collab), plus Fall „emplace_or_replace mit anderer ID“ und „Entity-Index wird wiederverwendet (Generation)“.

### SCN-507 – MovementSystem: pro Mover und Frame Physik-Aufruf und Komponentenabfragen ohne Änderungsprüfung
- **ID**: SCN-507
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/MovementSystem.cpp:54-158 (Kern 110-119, 130-132, 156-157)
- **Problem**: Die View `MovementComponent, TransformComponent` wird komplett jeden Frame durchlaufen, auch für Entities, die ruhen (`moveInput == 0`, `lookYaw == 0`). Für jede wird (a) `reg.all_of<CharacterControllerComponent>` und `physics->setCharacterVelocity(id, …)` aufgerufen, also pro Frame ein Aufruf in die Physik-Schicht (ID-Lookup) mit dem identischen Wert wie im Vorframe, (b) `reg.try_get<RootMotionComponent>(e)` ausgewertet, obwohl das Ergebnis nur im `orientToMovement`-Zweig gebraucht wird (rmc/`rootOwnsYaw` bei ausgeschaltetem `orientToMovement` ungenutzt), (c) `moveInput`/`lookYaw` zurückgesetzt. Idle-NPCs (hunderte) kosten somit jeden Frame zwei Sparse-Set-Lookups und einen Physikaufruf.
- **Code-Auszug**:
```cpp
if (physics && reg.all_of<CharacterControllerComponent>(e))
{
    const auto& cc = reg.get<CharacterControllerComponent>(e);
    physics->setCharacterVelocity(static_cast<uint32_t>(e),
                                  glm::vec3(planar.x, cc.velocity.y, planar.z));
}
...
const auto* rmc = reg.try_get<RootMotionComponent>(e);
const bool rootOwnsYaw = rmc && rmc->mode != RootMotionComponent::Mode::Off
                      && rmc->options.extractYaw;
```
- **Lösung**: (a) `rmc`/`rootOwnsYaw` in den `if (mv.orientToMovement …)`-Zweig verschieben (reine Umordnung, identisches Verhalten). (b) Zuletzt geschriebene planare Geschwindigkeit in einem Laufzeitfeld (`MovementComponent::lastPlanar`, nicht serialisiert) halten und `setCharacterVelocity` nur bei Änderung oder bei `physics`-Neuaufbau aufrufen; der Zustand „einmal 0 schreiben beim Stoppen“ bleibt erhalten. (c) Optional: eine Entity mit `moveInput == 0 && lookYaw == 0 && lastPlanar == 0` früh per `continue` verlassen.
- **Erwarteter Effekt**: Schätzung: für 200 idle Charaktere ~400 Lookups + 200 Physikaufrufe pro Frame weniger; (a) allein ist kostenlos und sofort umsetzbar.
- **Risiko / Verifikation**: (b) setzt voraus, dass die Physik die gesetzte Geschwindigkeit hält (Kommentar in NavigationSystem 623-624 sagt „A character keeps the velocity it was last given“) und dass nach einem Neuaufbau des Charakters (Streaming, Respawn) neu geschrieben wird – Invalidierung bei `hasCharacter`-Wechsel. Test: Charakter stoppt (Velocity 0 einmal), Sprung (Y bleibt), Charakter wird neu erzeugt.

### SCN-508 – NavigationSystem: fehlgeschlagene Pfadsuche wird jeden Frame wiederholt
- **ID**: SCN-508
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/NavigationSystem.cpp:644-664 (Repath), 422-494 (planPath)
- **Problem**: Schlägt `planPath` fehl (Ziel/Agent außerhalb der NavMesh, Insel ohne Verbindung), bleibt `agent.hasPath == false` und `agent.moving == true`; im nächsten Frame wird erneut `planPath` aufgerufen: zwei `findNearestPoly` (je Box-Query über die BV-Tree), im Insel-Fall zusätzlich `findPath` mit 2048 Knoten. Der Kommentar (629-634) beschreibt genau dieses Szenario („the repath fails on every frame from then on“) und behebt nur das Gleiten, nicht die Kosten. Ein Verfolger, dessen Ziel außerhalb der Mesh steht, kostet so jeden Frame eine volle Suche, auch wenn das Ziel sich nicht bewegt hat.
- **Code-Auszug**:
```cpp
if (!agent.hasPath && !planPath(agent, worldPos, *query, id, glm::vec3(world.origin())))
{
    releaseCharacter();
    continue;
}
```
- **Lösung**: Nach einem Fehlschlag in einem Laufzeitfeld (`NavAgentComponent::nextPlanTime` bzw. `failedTarget` + Zähler) merken und erst neu planen, wenn (a) sich `targetPos` um mehr als `kRepathDistance` gegenüber dem fehlgeschlagenen Ziel bewegt hat, oder (b) ein Cooldown (z. B. 0,25-0,5 s) abgelaufen ist (der Agent bewegt sich selbst, daher kann sich die Start-Polygon ändern). Das Log bleibt gedrosselt wie bisher.
- **Erwarteter Effekt**: Schätzung: Faktor 15-30 weniger Suchen für gestrandete Agenten (bei 60 FPS und 0,25-s-Cooldown).
- **Risiko / Verifikation**: Reaktionszeit des Agenten, sobald das Ziel wieder auf der Mesh liegt, wird um bis zu den Cooldown verzögert; (a) ohne Cooldown erhält das Verhalten fast exakt. Test: Ziel off-mesh → kein `findPath` pro Frame (Zähler), dann Ziel zurück auf Mesh → Pfad innerhalb Cooldown.

### SCN-509 – NavigationSystem::update: Mehrfach-Durchläufe und doppelte Kettenwanderung
- **ID**: SCN-509
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/NavigationSystem.cpp:576-591 (autoStart), 599-602 (Warnung), 655, 764-778 (Transform-Bewegung)
- **Problem**: (1) Mit `physics` wird jeden Frame die komplette `NavAgentComponent`-View nur für das einmalige Latchen von `autoStart` durchlaufen. (2) Ohne NavMesh folgt jeden Frame eine zweite View nur für die Warnung. (3) Für bodenlose Agenten (Transform-Pfad) wird `worldPositionOf` (Kettenwanderung) einmal in 655 und dann in `localPositionForWorld` (`worldMatrixOf(parent)` + `glm::inverse`, 379-380) erneut pro Frame berechnet; jeder Agent mit Elternteil kostet also zwei Ketten und eine 4×4-Inversion pro Frame.
- **Code-Auszug**:
```cpp
for (auto [e, agent] : reg.view<NavAgentComponent>().each())
{
    if (!agent.autoStart || agent.autoStarted) continue;
    agent.autoStarted = true;
```
- **Lösung**: (1) autoStart in die Hauptschleife (639-642) einfalten bzw. nur bei „Play gestartet“-Flanke ausführen (Zähler/Flag der App, das `physics != nullptr` bereits signalisiert); (2) Warnung aus der ersten View-Iteration herleiten (zählt `moving` ohnehin); (3) `localPositionForWorld` eine Überladung mit bereits bekannter Parent-Weltmatrix geben (einmal `worldMatrixOf(parent)` und daraus `worldPos`, spart die zweite Kette); die Inversion nur berechnen, wenn der Parent nicht Identität ist.
- **Erwarteter Effekt**: Schätzung: gering (je ~N Iterationen); relevant erst bei tausenden Agenten. Reine Vereinfachung/Mikroeinsparung.
- **Risiko / Verifikation**: autoStart-Reihenfolge („before any early return“, Zeile 568-570) muss erhalten bleiben; Test: autoStart-Agent startet im ersten Tick auch ohne baked NavMesh (Warnung kommt).

### SCN-510 – Replikatoren: joinedConnections() pro Entity und dreifach duplizierte Hilfsfunktionen
- **ID**: SCN-510
- **Kategorie**: A (auch C)
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/Net/SpawnReplicator.cpp:61-68, 114-115, 48-51; src/HE_Scene/src/Net/PropertyReplicator.cpp:33-38, 55-58, 80-88; src/HE_Scene/src/Net/RpcRouter.cpp:50-55, 69-72, 108-116
- **Problem**: In `bindSceneEntities` wird in der Schleife über alle Kandidaten für jede Entity `joinedConnections()` (liefert einen neuen `vector` per Wert) aufgerufen; bei 5.000 Szenen-Entities sind das 5.000 Vektor-Allokationen beim Hosten bzw. beim Streamen einer Zelle. Außerdem existieren drei fast identische Kopien von `isAuthority()`, `joinedConnections()` und `alreadyLogged()` (Linearsuche in `vector<string>`) in den drei Replikatoren.
- **Code-Auszug**:
```cpp
for (const ConnectionId conn : joinedConnections())
    sendBind(conn, netId, uuid);
```
- **Lösung**: `joinedConnections()` vor der Schleife einmal bestimmen (bleibt gültig, es werden keine Verbindungen verändert). Die drei Hilfen in einen kleinen gemeinsamen Header (`Net/ReplicatorCommon.h`: `isAuthority(NetRole)`, `joinedConnections(NetSession*, const JoinedFilter&)`, `LogOnce`) auslagern. Die Varianten sind verhaltensgleich (Spawn: `!m_joined || m_joined(conn)`; Property/Rpc: ohne Filter alle Verbindungen).
- **Erwarteter Effekt**: Schätzung: −N Allokationen beim Host-Start; deutlich weniger Duplikat-Code.
- **Risiko / Verifikation**: Gering; bestehende Net-Tests (Bind/Spawn/Join) genügen.

### SCN-511 – PropertyReplicator: Tabellen-Broadcast sendet doppelt, Probe kodiert jeden Wert zweimal
- **ID**: SCN-511
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/Net/PropertyReplicator.cpp:337-341 (broadcastTableFor), 247-335 (sendTableFor, insb. 261-271, 296-333)
- **Problem**: `broadcastTableFor` ruft `sendTableFor(netId, conn)` je Verbindung auf. Jeder Aufruf führt `collect` + Probe-Kodierung (`BitWriter probe; writeValue(probe, …)` je Variable, danach nochmals in `writeTable`) aus. Ändert sich die Namensliste (`listChanged`), schickt der erste Aufruf die Tabelle an ALLE verbundenen Clients (318); die folgenden Aufrufe für Verbindung 2…N sehen die Liste nun als unverändert und senden sie an ihre Verbindung erneut (331). Clients 2…N erhalten die Tabelle zweimal (zuverlässiger Kanal, `handleTable` ist idempotent, daher kein Fehler, aber doppelter Verkehr und N× Collect/Probe).
- **Code-Auszug**:
```cpp
void PropertyReplicator::broadcastTableFor(std::uint32_t netId)
{
    for (const ConnectionId conn : joinedConnections())
        sendTableFor(netId, conn);
}
```
- **Lösung**: `sendTableFor` in zwei Teile trennen: `buildTable(netId) → optional<Table>` (collect + Filter, einmal) und `sendTable(conn|alle)`. `broadcastTableFor` baut einmal und sendet mit `sendTo(joined, …)`; Probe und Schreiben in einem Durchgang (Wert in den Writer schreiben, bei Fehlschlag Writer-Position zurücksetzen oder Vorprüfung per `isReplicableType` + Strukturcheck ohne Bits).
- **Erwarteter Effekt**: Schätzung: halbiert den Traffic dieses (seltenen) Ereignisses für N ≥ 2 Clients und senkt die CPU-Kosten bei Join-Wellen (Tabellen aller Entities × Clients).
- **Risiko / Verifikation**: Reihenfolge/Inhalt der Nachrichten ist testrelevant (Join-Sequenz Welcome → Binds → Tabellen → Baseline). Test: zwei Clients, Variable mitten in der Session deklariert → jeder Client empfängt genau eine Tabelle.

### SCN-512 – RpcRouter::accept: Schlüssel-String pro Aufruf, unbegrenzt wachsende Log-Liste
- **ID**: SCN-512
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/Net/RpcRouter.cpp:523-534 (Schlüssel), 443-446 (Signatur), 345 (args), 50-55 (alreadyLogged)
- **Problem**: Für jeden CallServer an ein Lua/Python/Native-Ziel (`!sig.found`) wird `std::to_string(netId) + ":" + fn` gebaut (Heap) und linear in `m_missingLogged` (`vector<string>`) gesucht. Die Liste wächst um einen Eintrag je (Entity, Funktion) über die ganze Session und wird nur in `clear()` geleert; bei vielen gespawnten Entities mit Lua-Handlern wächst sie unbegrenzt und die Suche wird O(n). Zusätzlich liefert `functionSignatureOf` eine Struktur mit Vektoren per Wert und `handleRpc` legt `std::vector<Value> args(argc)` an (große `Value`-Objekte); bei durch `kRpcRateWindowSec` begrenzter Rate unkritisch.
- **Code-Auszug**:
```cpp
const std::string key = std::to_string(netId) + ":" + fn;
if (!alreadyLogged(m_missingLogged, key))
    HE_LOG_INFO(Replication, "CallServer '%s' on net id %u: no HorizonCode signature ...
```
- **Lösung**: `std::unordered_set<uint64_t>` mit `hash(netId, fn)` (z. B. `netId * 0x9E37… ^ std::hash<string>{}(fn)`) statt String-Schlüssel; Obergrenze (z. B. 256 Einträge, danach nur noch Zähler), oder den Logeintrag je Funktionsname statt je Entity.
- **Erwarteter Effekt**: Schätzung: Entfernt eine Allokation je RPC und das unbeschränkte Wachstum; praktisch gering, solange RPC-Raten klein bleiben.
- **Risiko / Verifikation**: Nur Loghäufigkeit ändert sich; kein Testbedarf außer „Hinweis erscheint einmal pro Name“.

### SCN-513 – NetGameSession: doppelter `dropConnection`-Aufruf und kleine Redundanzen
- **ID**: SCN-513
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Scene/src/Net/NetGameSession.cpp:1060-1067, 891-912, 1022-1025/1124-1130
- **Problem**: In der Schleife über `m_rejected` steht `if (m_rpc) m_rpc->dropConnection(conn);` zweimal (Zeile 1064 und 1066), die zweite mit falsch eingerücktem, verwaistem Kommentar (Rest eines Merge). `dropConnection` ist idempotent (`m_rate.erase`), der zweite Aufruf ist reiner Ballast. `reapSeveredLinks` läuft jeden Frame auf dem Host mit O(Spieler × Verbindungen) und `update()` liest `steady_clock::now()` bis zu zweimal pro Frame (Browser + Announcer); beides vernachlässigbar.
- **Code-Auszug**:
```cpp
if (m_rpc) m_rpc->dropConnection(conn);
// A connection id the transport reuses must inherit no rate history.
if (m_rpc) m_rpc->dropConnection(conn);
```
- **Lösung**: Zeile 1065-1066 löschen und den Kommentar vor Zeile 1064 stellen. Optional `nowMs` einmal pro `update()` bestimmen.
- **Erwarteter Effekt**: Keiner messbar; Lesbarkeit.
- **Risiko / Verifikation**: Keines; kompilieren und Net-Tests.

### SCN-514 – ValueWire: Temporär-`Value` je Container-Element, tote Zweige im JSON-Pfad
- **ID**: SCN-514
- **Kategorie**: C (auch F)
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/Net/ValueWire.cpp:222-233, 134-139, 345-353
- **Problem**: `readValueAt`/`readScalarPayload` konstruieren pro Element ein vollwertiges `Value` (`Value e;`, `Value k;`, `Value f;`) – eine große Struktur mit Strings, Vektoren und mehreren glm-Typen – und verschieben es dann in `items`. Bei großen Float-Arrays (≤ 65.535 Elemente) bedeutet das viele große Konstruktionen/Verschiebungen; Allokationen entstehen nur bei Strings/Strukturen. Im JSON-Pfad ist der Zweig `case PinType::Struct` in `appendScalarJson` unerreichbar (Struct wird vorher in `appendJson` behandelt), und `Ref`/`Struct`/`Exec` führen alle zu `"null"` (zwei getrennte Fälle).
- **Code-Auszug**:
```cpp
Value e;
if (!readScalarPayload(r, e, type, depth)) return false;
out.items.push_back(std::move(e));
```
- **Lösung**: `out.items.emplace_back()` und direkt in `out.items.back()` lesen (bei Fehler `pop_back`). Dann `Ref/Exec/Struct` in `appendScalarJson` zu einem Fall zusammenfassen und den toten Struct-Zweig entfernen.
- **Erwarteter Effekt**: Schätzung: gering; spart pro Element eine Move-Konstruktion einer großen Struktur. Unsicher / messen (Arrays dieser Größe werden selten repliziert).
- **Risiko / Verifikation**: Fehlerpfad (halb gelesenes Element) muss `items` konsistent lassen; Test mit abgeschnittenem Datagramm (bestehende Malformed-Tests).

### SCN-515 – Schwere Includes in öffentlichen Headern
- **ID**: SCN-515
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: TransformHierarchy.h:3 (`entt/entt.hpp`, 31 Includer), UIInputSystem.h:2 (`entt/entt.hpp`, ungenutzt), UISystem.h:3, WaterQuery.h:5, src/NotifyCollect.h:8, WaterLake.h:3-4 (`TerrainSculpt.h`, `HorizonWorld.h`), WaterMesh.h:2 und TerrainMeshGenerator.h:2 (`ContentManager/Assets.h`), WeatherAudio.h:2 (`WeatherComponent.h`), UICursorSDL.h:2 (`SDL3/SDL.h`), SplineCurve.h:2-3
- **Problem**: Mehrere Header ziehen die komplette entt-Bibliothek, `Assets.h` oder `HorizonWorld.h` nur für einen Typnamen (`entt::entity`, `StaticMeshAsset` als Rückgabetyp, `Entity`). `UIInputSystem.h` benutzt keinen einzigen entt-Typ. `entt/entity/fwd.hpp` (vorhanden unter `src/HE_Scene/vendor/entt/entity/fwd.hpp`, enthält `enum class entity`) genügt für `entt::entity` und `entt::registry&`. Funktionsdeklarationen mit `StaticMeshAsset` als Rückgabetyp brauchen nur eine Vorwärtsdeklaration.
- **Code-Auszug**:
```cpp
// UIInputSystem.h
#include <entt/entt.hpp>
#include <cstdint>
```
- **Lösung**: `UIInputSystem.h`: Include entfernen. `TransformHierarchy.h`, `UISystem.h`, `WaterQuery.h`, `NotifyCollect.h`: `<entt/fwd.hpp>`; die .cpp-Dateien, die entt-Operationen brauchen, binden `entt.hpp` selbst ein. `TerrainMeshGenerator.h`: `struct StaticMeshAsset;` (alle Aufrufer binden `Assets.h` bereits für den Rückgabewert). `WaterLake.h`: `TerrainSculpt::Result` per Vorwärtsdeklaration ist nicht möglich (Rückgabe per Wert in Deklaration geht, aber Aufrufer brauchen den Typ) – dort nur `HorizonWorld.h` gegen `class HorizonWorld; using Entity = entt::entity;` tauschen, falls `Entity` ein Alias ist.
- **Erwarteter Effekt**: Schätzung: spürbar kürzere Kompilierzeiten bei Änderungen an diesen Headern; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Reines Compile-Risiko (fehlende Includes in Einbindern). Gesamtbuild (`HorizonEditor`, `he_tests`, `HorizonGame`) auf allen Plattformen.

### SCN-516 – NavMesh-Bake blockiert den aufrufenden (Editor-)Thread
- **ID**: SCN-516
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/NavigationSystem.cpp:106-163 (collectStaticGeometry), 166-395 (bake), 91-103 (movesAtRuntime)
- **Problem**: `collectStaticGeometry` und `bake` laufen synchron im Aufrufer (Editor-Panel): `HE::propagateTransforms(world)`, dann pro Mesh `movesAtRuntime` (drei `try_get` je Vorfahre, O(Meshes × Tiefe)), Geometrie in `vector<float>/vector<int>` kopieren, danach Recast-Pipeline bis zur Grenze `kMaxGridCells = 4096² = 16,7 M` Zellen (Heightfield mit Span-Zeigern: hunderte MB). Der Code selbst verweist auf „the editor simply stops responding“ und `HE_LOG_SLOW_SCOPE(250 ms)`. Editor-only, nicht pro Frame, daher niedrig.
- **Code-Auszug**:
```cpp
constexpr long long kMaxGridCells = 4096ll * 4096ll;
...
rcHeightfield* hf = rcAllocHeightfield();
```
- **Lösung**: Bake als Job (JobSystem) mit eigener `rcContext` und Fortschrittsanzeige/Abbruch ausführen; Geometriesammlung (World-Zugriff) bleibt im Hauptthread, `bake(nmc.geometry)` ist bereits ein reiner Funktionsaufruf auf Kopien und lässt sich auslagern. Ergebnis (`navMesh`/`navQuery`) im Hauptthread übernehmen.
- **Erwarteter Effekt**: Editor bleibt während des Bakes bedienbar; Gesamtdauer unverändert.
- **Risiko / Verifikation**: Lebensdauer von `nmc` während des Bakes (Undo, Szenenwechsel); Ergebnis nur anwenden, wenn die Komponente noch existiert. Test: Bake im Hintergrund + Szenenwechsel.

---

## Gelesen

Ja, alle Dateien des Slice 5 vollständig gelesen (HorizonWorld.cpp, LODSystem.cpp, MovementSystem.cpp, NavigationSystem.cpp, Net/NetGameSession.cpp, Net/PropertyReplicator.cpp, Net/RpcRouter.cpp, Net/SpawnReplicator.cpp, Net/ValueWire.cpp, NotifyCollect.h, ParticleSystem.cpp sowie alle Header von SplineCurve.h bis WeatherSystem.h); zusätzlich punktuell TransformHierarchy.cpp (321-386), SceneSystems.cpp (100-155) und ParticleSystemComponent.h zur Verifikation.

## Scene Slice 6 – PhysicsWorld.cpp … SceneJsonParse.cpp

## Zusammenfassung

Der Slice (PhysicsWorld, PlayerHost/-Settings, PoseFinalize/PoseSource, PropertyAnimationSystem, RootMotion*, RopeTrailSystem, SceneJsonParse) ist insgesamt sorgfältig und gut kommentiert. Gefunden wurden vor allem Skalierungsprobleme bei vielen Entities: `PhysicsWorld::step` besucht pro Schritt jeden Body (auch statische und schlafende), der Contact-Listener nimmt für jedes Kandidaten-Paar einen globalen Mutex, und Collider-Shapes werden pro Entity neu gebaut, nie geteilt. `RopeTrailSystem` und die Pose-/Root-Motion-Pfade arbeiten ohne Dirty-Flags und mit Heap-Allokationen pro Entity und Frame. Insgesamt 21 Funde: 4 Hoch, 11 Mittel, 6 Niedrig. `PlayerSettings.cpp` ist unauffällig (kein Hot Path, kein Fund). Es gibt keine Messdaten; Schätzungen sind als solche gekennzeichnet.

---

### SCN-601 – step(): Write-back-Schleife besucht jeden Body, auch statische und schlafende
- **ID**: SCN-601
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:3312-3406 (Snapshot, Reap, Write-back), 1002-1021 (`writeBackWorldPose`)
- **Problem**: Pro Physik-Schritt wird die komplette `entityToBody`-Map (`std::unordered_map`, knotenbasiert, cache-unfreundlich) in einen Vektor kopiert und durchlaufen. Dort stehen pro Body `reg.valid`, `TryGetBody` und der Motion-Type-Check. Die Schleife ist O(alle Bodies), obwohl meist nur wenige aktiv sind (in einem Level überwiegen statische Bodies, die nur wegen des „Reap" besucht werden). Dynamische **schlafende** Bodies werden nicht übersprungen: nur `Static` und gehaltene Bodies fallen raus. Für sie laufen `reg.all_of<CharacterControllerComponent>`, `try_get<TransformComponent>`, `localPositionForWorld` (Parent-Walk), bei Kindern zusätzlich `worldPoseOf(parent)` samt `quat_cast`/`normalize`, dazu `glm::eulerAngles` (Trigonometrie). Am Ende steht `transform.dirty = true` (Z. 1020). Damit wird jeder schlafende Body jeden Schritt als geändert markiert, und die Transform-Propagation und alles, was auf `dirty` hört, arbeiten für ruhende Objekte. Der Kommentar bei Z. 3300-3303 nennt selbst „ein Fünftel eines 20 000-Body-Schritts".
- **Code-Auszug**:
```cpp
m_impl->stepBodies.clear();
m_impl->stepBodies.reserve(m_impl->entityToBody.size());
for (const auto& entry : m_impl->entityToBody)
    m_impl->stepBodies.emplace_back(entry.first, entry.second);
for (const auto& [entityId, bodyId] : m_impl->stepBodies) {
    ... if (!reg.valid(entity)) { removeEntity(entityId); continue; }
    ... if (body->GetMotionType() == JPH::EMotionType::Static) continue;
    ... writeBackWorldPose(world, entity, *transform, ...);   // setzt transform.dirty = true
```
- **Lösung**:
  1. Write-back nur für aktive Bodies: Jolt liefert die Liste (`PhysicsSystem::GetActiveBodiesUnsafe(EBodyType::RigidBody)` und `GetNumActiveBodies`; Signatur gegen die Vendor-Version prüfen). Die Entity-ID kommt aus `body->GetUserData()`, die Map-Iteration entfällt. Wichtig: ein Body, der in diesem Schritt einschläft, war im Schritt davor aktiv, seine Endpose ist also schon geschrieben.
  2. Reap von Entities, die gelöscht wurden, nicht mehr pro Schritt über alle Bodies: `reg.on_destroy<RigidBodyComponent>()` bzw. `<TerrainComponent>` / `<CharacterControllerComponent>` setzt nur ein Flag oder merkt die ID (kein ContentManager-Zugriff im Listener nötig). Alternative mit weniger Risiko: den vollen Reap-Durchlauf nur alle N Schritte oder wenn `reg.storage<RigidBodyComponent>().size() < entityToBody.size()` ausführen.
  3. `dirty` nur setzen, wenn sich Position/Rotation tatsächlich ändern (Vergleich mit dem Alt-Wert).
- **Erwarteter Effekt**: (Schätzung) Bei 20 000 Bodies, davon wenige aktiv, fällt der Schritt-Overhead außerhalb von `Update()` von O(20 000) auf O(aktive). Für schlafende Szenen vermutlich Faktor 10-100 in diesem Abschnitt. Messen.
- **Risiko / Verifikation**: Verhaltensänderung: Skripte, die die Transform eines schlafenden Physik-Bodies direkt ändern, wurden bisher jeden Schritt von der Jolt-Pose überschrieben; nachher bleibt ihre Änderung bis zum Aufwecken stehen. Tests: `tests/` mit Physik-Sync (kinematisch, geparentet, Character+Proxy), Entity-Löschen mitten im Spiel (Raycast darf keine tote ID liefern), schlafender Body mit Transform-Änderung. Reihenfolge-Kommentar bei Z. 3393-3400 (Parent/Kind) bleibt gültig.

---

### SCN-602 – Contact-Listener: globaler Mutex pro Kandidaten-Paar und pro Contact
- **ID**: SCN-602
- **Kategorie**: E
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:214-224 (`OnContactValidate`), 236-264 (`OnContactAdded`), 266-280 (`OnContactRemoved`), 454-462
- **Problem**: `OnContactValidate` wird von allen Jolt-Workern für jedes neue Body-Paar aufgerufen und nimmt **immer** `m_mutex`, bevor überhaupt geprüft wird, ob `m_noCollidePairs` leer ist (der Normalfall ohne Joints). Das ist ein einziger Lock über alle Worker, den der Kommentar „uncontended in practice" (Z. 226-228) nur für die Schreibseite meint. `OnContactAdded`/`OnContactRemoved` serialisieren zusätzlich auf demselben Mutex und legen pro Contact einen `unordered_map`-Knoten an bzw. löschen ihn (Heap pro Kontakt in dichten Haufen). Mit `threads > 1` skaliert das Narrow-Phase-Ergebnis dadurch nicht.
- **Code-Auszug**:
```cpp
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_noCollidePairs.empty() &&
        m_noCollidePairs.count(bodyPairKey(b1.GetID(), b2.GetID())) != 0)
        return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
}
```
- **Lösung**:
  1. Validate lock-frei vorfiltern: `std::atomic<uint32_t> m_noCollideCount` (in `setPairCollides` unter Lock mitgeführt). Bei 0 sofort `AcceptAll...` ohne Lock. Verhalten identisch, da `setPairCollides` nie während `Update()` läuft (so dokumentiert).
  2. Für den Fall mit Joints: eigener `std::shared_mutex` oder ein während `Update()` unveränderliches Snapshot-Set (Jolt-Callbacks laufen nur innerhalb von `Update`, Schreiber nur davor/danach), dann ganz ohne Lock lesen.
  3. Added/Removed: pro Worker thread-lokale Puffer (`thread_local` oder Array pro Job-Index) und nach `Update()` in `step()` zusammenführen, statt jeden Contact unter dem globalen Lock zu verbuchen. Optional `m_active` als flache Hash-Map (offene Adressierung) statt Knoten-Map.
- **Erwarteter Effekt**: (Schätzung) Für (1) praktisch kostenlos und entfernt den Lock aus dem häufigsten Callback; für (3) spürbar in Szenen mit tausenden Kontakten pro Schritt und 4+ Threads. Messen mit `PhysicsWorld::stepStats` und einer Pile-Szene (`docs/physics-threading-baseline.md` nennt die Referenzszene).
- **Risiko / Verifikation**: (1) trivial. (3) betrifft die Ereignis-Reihenfolge: `take()` sortiert bereits nach Entity-Paar (Z. 438), die Ausgabe bleibt deterministisch, der Ref-Count (enter bei erstem, exit beim letzten Sub-Shape-Contact) muss zusammenführbar bleiben. Bestehende Collision-Event-Tests (Enter/Exit, Overlap, Limit/Carry-Backlog) müssen unverändert grün sein.

---

### SCN-603 – Collider-Shapes werden pro Entity neu gebaut und nie geteilt
- **ID**: SCN-603
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:1308-1446 (`buildColliderShape`), 1119-1161 (`buildMeshShape`), 1166-1209 (`buildConvexHullShape`), 1719, 1769
- **Problem**: `buildBodyFor` ruft für jede Entity `buildColliderShape` auf, das für Mesh/ConvexHull alle Vertices in `JPH::VertexList`/`Array<Vec3>` kopiert und eine neue `MeshShape` (BVH-Aufbau) bzw. Hull (Quickhull) erzeugt. 500 identische Häuser oder Felsen (gleiches Mesh, gleiche Skalierung) ergeben 500 identische Shapes im RAM und 500 Aufbauzeiten beim Laden und beim Streaming. Jolt-Shapes sind unveränderlich und referenzgezählt, also frei teilbar. Der Hull-Pfad versucht bei Überschreitung des 256-Punkte-Limits bis zu 3 Mal (`{1e-3, 0.05, 0.25}`) einen kompletten Aufbau aus allen (nicht dedupierten) Punkten. Auch Box/Sphere/Capsule erzeugen pro Entity ein neues Heap-Objekt.
- **Code-Auszug**:
```cpp
out.result = wantsMesh ? buildMeshShape(*mesh, worldScale)
                       : buildConvexHullShape(*mesh, worldScale, entityId);
...
for (const float tolerance : { 1.0e-3f, 0.05f, 0.25f }) {
    JPH::ConvexHullShapeSettings settings(points);
```
- **Lösung**: Shape-Cache in `Impl`, Schlüssel `(kind, meshAssetId, scale bitweise, ggf. Winding-Flip, halfExtents/radius/height bei Primitiven)` auf `JPH::Ref<JPH::Shape>`. `buildColliderShape` liefert bei Treffer `ShapeResult` aus dem Cache (Refcount). Der Cache wird in `clear()` geleert; alternativ schwache Einträge (nur halten, solange `GetRefCount() > 1`). Für den Hull-Pfad vorher Punkte dedupen (gequantisiert) und die Toleranzleiter beginnen lassen, wo sie bei gleichem Asset zuletzt erfolgreich war.
- **Erwarteter Effekt**: (Schätzung) Ladezeit und RAM für wiederholte Meshes sinken ungefähr um den Faktor der Instanzzahl; bei Streaming-Cells mit vielen identischen Props deutlich. Messen: Zeit und Speicher von `initialize()`/`addEntities` mit N identischen Mesh-Collidern.
- **Risiko / Verifikation**: Geteilte Shape ist beobachtbar identisch, solange `mustBeStatic`/Fallback-Entscheidungen mit im Cache-Wert stehen (Fehlerfälle nicht cachen oder mit Fehlerstatus). Pro Body unterschiedliche Daten (Material, UserData) liegen in `BodyCreationSettings`, nicht in der Shape. Wenn Content hot-reloaded wird (Mesh ersetzt), muss der Cache über die Asset-ID plus Revision invalidiert werden. Tests: zwei Entities, gleiches Mesh, gleiche und verschiedene Skalierung, gespiegelte Skalierung (Winding).

---

### SCN-604 – RopeTrailSystem: pro Frame Kopie, Hash und Aufräum-Sweep für jedes Seil, ohne Dirty-Flag
- **ID**: SCN-604
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/RopeTrailSystem.cpp:283-364, 63-87, 266-281, 47-56
- **Problem**: `update()` läuft jeden Frame (SceneSystems.cpp:147) und tut für **jede** `RopeComponent`, auch für unveränderte:
  1. `resolveControlPoints` kopiert `rope.controlPoints` (Heap-Allokation pro Seil und Frame, Z. 66), selbst wenn keine Attachments existieren und die Kopie unverändert zurückgegeben wird.
  2. `geometryHash` hasht bytewise (FNV-1a, 1 Multiplikation pro Byte) alle Punkte plus Parameter jeden Frame.
  3. Bei Attachments: `worldMatrixOf` + `glm::inverse` + 2× `findByEntityId` + 2× `worldPositionOf` pro Seil und Frame.
  4. `owned.find` und `cm.getStaticMesh(it->second)` (Hash-Lookup im ContentManager) pro Seil und Frame.
  5. Danach läuft immer der Sweep über **alle** `owned`-Einträge (`reg.valid` + `try_get`), obwohl sich die Seilmenge nur selten ändert.
  Bei Rebuild (z. B. ein Seil, das an eine bewegte Entity gebunden ist, also jeden Frame) kopiert `toStaticMesh` vier Vektoren aus `mesh`, das danach nicht mehr gebraucht wird (Z. 331, 339).
- **Code-Auszug**:
```cpp
for (auto [e, rope] : reg.view<RopeComponent>().each()) {
    const std::vector<glm::vec3> pts = resolveControlPoints(world, e, rope);   // Kopie
    const uint64_t hash = geometryHash(rope, pts);                              // bytewise FNV
    ...
    if (ours && hash == rope.builtHash) continue;
```
- **Lösung**:
  1. `resolveControlPoints` für den Fall ohne Attachments als Referenz auf `rope.controlPoints` liefern (z. B. `const std::vector<glm::vec3>&` plus optionaler Scratch-Vektor in einer wiederverwendeten Struktur, nur bei Attachment befüllt).
  2. Hash wortweise (64-bit XOR/Multiply pro 8 Byte) oder nur über geänderte Eingaben: Komponenten-Revision, die der Inspector/Setter hochzählt (`rope.revision`), plus beim Attachment den Vergleich der Endpunkte statt Neuhash aller Punkte.
  3. Sweep nur bei Strukturänderung: `reg.on_destroy<RopeComponent>().connect` setzt lediglich ein `bool g_ropesChanged` (kein ContentManager-Zugriff, womit der Einwand im Kommentar Z. 345-350 nicht greift), oder `owned.size() != storage.size()`. Entity-Handle-Recycling bleibt über den ID-Vergleich im Sweep abgedeckt.
  4. `toStaticMesh(HE::spline::MeshData&&)` mit `std::move` der vier Vektoren.
- **Erwarteter Effekt**: (Schätzung) Pro statischem Seil sinken die Kosten von Kopie + ca. `12·N` Hash-Schritten + 2 Hash-Lookups auf nahezu 0 pro Frame. Relevant ab dutzenden Seilen/Kabeln (Leitungen, Brücken); bei einzelnen Seilen im Mikrobereich. Messen mit dem `RopeTrail`-Profil-Scope.
- **Risiko / Verifikation**: Revisionszähler müssen von allen Schreibern (Serializer, Undo, Kollaborations-Sync, Skript-API) bedient werden; sonst veraltete Geometrie. Daher zuerst nur (1), wortweiser Hash, (3) und (4), die Verhalten exakt erhalten. Tests: Rope mit/ohne Attachment, Wechsel der Control Points, Entity-Handle-Recycling, `releaseWorld`.

---

### SCN-605 – Pose-Pipeline: mehrere Heap-Vektoren pro Entity und Frame
- **ID**: SCN-605
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PoseFinalize.cpp:149-156, 178-180, 311, 408-409, 502; src/HE_Scene/src/PoseSource.cpp:66-68, 93-95, 130-139
- **Problem**: Für jede animierte Entity und jeden Frame werden neu angelegt: `model` (`vector<mat4>` in `poseFinalize`), pro Layer `layerTRS`, ggf. `refTRS`, `out` (danach `pose = std::move(out)`, also ein Allokations-/Freigabe-Zyklus pro Layer), in `makePoseSource` für Blend Spaces `clips`, `durations`, `weights` und `order`, in `PoseSource::evaluate` nochmal `order` (die Sortierung `blendSpaceEvalOrder` läuft zweimal), `poses` (vector<vector<JointTRS>>, ein Vektor je aktivem Sample, je Skelettgröße zeroinitialisiert), `deltas` und `haveDelta` (`vector<bool>`). Bei IK kommen `toSolve`, `chain`, `chainWeights` hinzu. Bei 100 Joints à 40 Byte sind das viele kleine bis mittlere malloc/free pro Entity und Frame, plus Memset.
- **Code-Auszug**:
```cpp
std::vector<JointTRS> layerTRS;
...
std::vector<JointTRS> out;
applyLayerPose(pose, layerTRS, ref, maskWeights, layer.weight, layer.mode, rootJoint, out);
pose = std::move(out);
...
std::vector<std::vector<JointTRS>> poses(space->samples.size());
```
- **Lösung**: Eine `PoseScratch`-Struktur (Vektoren für `model`, `layerTRS`, `refTRS`, `out`, `poses`, `deltas`, `order`, IK-Listen), pro Treiber-`update()` einmal angelegt (oder `thread_local`) und durch `poseFinalize`/`PoseSource::evaluate` gereicht; Vektoren nur `clear()`/`resize()`. `order` einmal in `makePoseSource` berechnen und in `PoseSource` ablegen (entfällt in `evaluate`). `haveDelta` als `std::array`/`std::unique_ptr<bool[]>` oder in `RootMotionDelta` integrieren. `applyLayerPose` in-place bzw. Ping-Pong zwischen zwei Scratch-Puffern.
- **Erwarteter Effekt**: (Schätzung) Bei einigen hundert animierten Charakteren sinkt die Allokationsrate von ca. 10-30 auf ca. 0 pro Entity und Frame; messbar vor allem in Crowd-Szenen und durch geringere Speicherfragmentierung. Messen mit `AnimationStateMachine`-Scope und Allocator-Zähler.
- **Risiko / Verifikation**: Schnittstellenänderung von `poseFinalize`/`evaluate` (drei Treiber). Ergebnisbits müssen identisch bleiben (Kommentar Z. 497-500: „same bits"): vorhandene Animationstests ausführen (Layer, Additive, Blend Space, IK).

---

### SCN-606 – PlayerHost::tick: Strings, Event-Namen und Zustandsvektor pro Aktion und Frame
- **ID**: SCN-606
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PlayerHost.cpp:286-315 (`who`), 363-425, 388, 398, 409-410, 425
- **Problem**: Jeden Frame werden (a) pro Spieler `who` als `std::string` gebaut, bei mehr als einem Spieler mit zwei Allokationen, obwohl sie nur in den seltenen Fällen `Captured`/`Cancelled` gebraucht wird; (b) pro Aktion und Spieler die Event-Namen per String-Konkatenation erzeugt (`HE::inputEventAxis(a.name)`, `inputEventAxis2D`, `inputEventPressed`, `inputEventReleased`), an `fireInputEvent(const std::string&)` übergeben und dort im Runtime per String-Hash aufgelöst, danach noch einmal für den Pawn; (c) `std::vector<ActionState> states` mit `reserve` neu angelegt, `st.name = a.name` kopiert (Heap bei Namen >15 Zeichen) und per `setActions(std::move(...))` das alte Vektorpaket samt Strings freigegeben. Achsen-Events feuern absichtlich jeden Frame (Kommentar Z. 383), die Namenserzeugung dazu aber nicht.
- **Code-Auszug**:
```cpp
const std::string who = m_players.size() > 1 ? "Player " + std::to_string(i + 1) + ": " : "";
...
fireInputEvent(i, HE::inputEventAxis(a.name), HorizonCode::Value::ofFloat(v));
...
std::vector<HE::api::input::ActionState> states;
states.reserve(m_actions.size());
```
- **Lösung**: `ActionInfo` bekommt beim `begin()` vorberechnete Felder `evAxis`, `evAxis2D`, `evPressed`, `evReleased` (nur je nach `kind`). `fireInputEvent` nimmt sie ohne Neubau. `who` erst in den `case`-Zweigen erzeugen. `states` als Member behalten, Elemente in-place aktualisieren (`name` nur beim `begin()` setzen) und per `setActions(const std::vector&)` oder Swap übergeben. Die Text-Script-Aufrufe (Z. 390, 401, 416) siehe Cross-Modul.
- **Erwarteter Effekt**: (Schätzung) Eliminiert ca. 2-4 Heap-Allokationen und ebenso viele String-Hashes pro Aktion und Frame, bei 20-60 Aktionen und 60 FPS ein paar tausend Allokationen pro Sekunde. Im Mikrosekundenbereich pro Frame; niedriger Gewinn, aber trivial und riskoarm.
- **Risiko / Verifikation**: Eventnamen müssen byteidentisch zu `HE::inputEventAxis()` & Co. sein. Bestehende Input-/Player-Tests prüfen Event-Zustellung (Pause, UIOnly, Rebind, Multi-Player).

---

### SCN-607 – addEntity: `removeEntityImpl` immer, dadurch O(Joints) pro Spawn; lineare Pending-Joint-Listen
- **ID**: SCN-607
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:2533 (`addEntity`), 2307-2346 (`destroyJointsInvolving`), 608-629, 648-654, 2348-2418 (`resolvePendingJoints`), 2550
- **Problem**: `addEntity` ruft bedingungslos `removeEntityImpl(entityId, Rebuild)`. Das läuft in `destroyJointsInvolving`, das **alle** Joints linear durchsucht (Z. 2319-2321), außerdem acht Hash-Erases (Character, Water, Owed …). `addEntities` vermeidet das bereits mit `hasPhysics` (Kommentar Z. 2588-2591), `addEntity`/`addEntityTree` nicht. Projektile, Pickups und Effekte, die einzeln gespawnt werden, zahlen den Scan pro Spawn, in Szenen mit vielen Joints (Ragdolls, Ketten). Zusätzlich: `queueJoint`/`queuePatientJoint`/`unqueueJoint` sind lineare Scans über `pendingJoints`, und `resolvePendingJoints` läuft bei jedem erfolgreichen `addEntity` und macht pro „patient"-Eintrag `findByEntityId` plus vier Hash-Lookups, also O(P) pro Spawn, solange gestreamte Zellen mit wartenden Joints nicht geladen sind (diese Einträge werden nie aufgegeben).
- **Code-Auszug**:
```cpp
removeEntityImpl(entityId, JointFate::Rebuild);
bool built = buildBodyFor(world, entityId);
...
for (const auto& [owner, record] : m_impl->joints)
    if (owner == entityId || record.partner == entityId)
        owners.push_back(owner);
```
- **Lösung**: (1) In `addEntity` wie in `addEntities` nur bei `hasPhysics(entityId)` entfernen. Das ist äquivalent, denn ein Joint-Eintrag existiert nur, wenn beide Bodies existieren; hat die Entity keinen Body, gibt es auch keinen Joint, der sie nennt. (2) Reverse-Index `bodyEntity → owners` (oder `unordered_multimap<partner, owner>`) für `destroyJointsInvolving`. (3) `pendingJoints` zusätzlich als `unordered_set<uint32_t>` indizieren; patient-Einträge nach Partner-UUID (`unordered_multimap<UUID, entity>`) bündeln, so dass ein Spawn nur die Wartenden dieses Partners prüft.
- **Erwarteter Effekt**: (Schätzung) (1) macht Einzel-Spawns in jointreichen Szenen von O(J) auf O(1); (2)/(3) nur relevant bei vielen Joints bzw. vielen wartenden Zellen-Joints. Messen: 1000× `addEntity` bei 500 Joints.
- **Risiko / Verifikation**: (1) gering; Test „addEntity zweimal = Replace" und „Joint-Partner wird neu gebaut" sichern es ab. (2)/(3) berühren die Rebuild-/Wait-Semantik, daher nur mit den vorhandenen Joint-Tests (Chain, Streaming-Wait, Drop vs. Rebuild).

---

### SCN-608 – `purgeEntity` pro entferntem Body: O(aktive Kontakte) unter dem globalen Lock
- **ID**: SCN-608
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:371-400, 3054, 2705-2745 (`removeEntityTree`)
- **Problem**: Jeder `destroyBodyFor` ruft `contactListener.purgeEntity`, das `m_active` komplett linear durchläuft und alle vier Event-Queues filtert, unter `m_mutex`. Der Kommentar (Z. 368-370) nimmt „Gameplay-Rate" an. Ein Streaming-Unload (`removeEntityTree` mit `requeueJoints`) entfernt aber tausende Bodies in einem Frame: M Bodies × |m_active| Kontakte. Bei 5000 Bodies und einigen 10 000 aktiven Kontakten sind das 10⁸ Vergleiche in einem Frame, während der Mutex gehalten wird.
- **Code-Auszug**:
```cpp
for (auto it = m_active.begin(); it != m_active.end(); )
    if (it->second.event.entityA == entityId || it->second.event.entityB == entityId)
        it = m_active.erase(it);
    else ++it;
```
- **Lösung**: Batch-Variante `purgeEntities(const std::unordered_set<uint32_t>&)` mit einem einzigen Durchlauf über `m_active` und die Queues; `removeEntityTree`/`clear`-ähnliche Massenpfade sammeln die IDs und rufen sie einmal am Ende. `destroyBodyFor` bekommt dafür einen Parameter bzw. eine Variante ohne Purge. Alternativ Reverse-Index `entity → Schlüssel` in `ActiveContact`, falls Einzelentfernungen häufig sind.
- **Erwarteter Effekt**: (Schätzung) Zellen-Unload mit vielen Kontakten von O(M·C) auf O(M+C). Messen mit einem Streaming-Unload in einer Szene mit großem Haufen.
- **Risiko / Verifikation**: Ereignis-Semantik gleich (alle Kontakte der entfernten Entities verschwinden ohne Exit-Events); „carry"-Bookkeeping in `drop()` bleibt. Test: Unload eines Baums mit aktiven Kontakten, danach `pollCollisionExit` ohne tote Entity.

---

### SCN-609 – `initialize()` fügt Bodies einzeln in die Broad Phase statt als Batch
- **ID**: SCN-609
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:2460-2467, 1809 (`CreateAndAddBody`), 2606-2617 (Batch in `addEntities`), 2498
- **Problem**: `addEntities` nutzt `AddBodiesPrepare/Finalize` (Kommentar: „one subtree per layer to merge", Z. 2606-2609). `initialize()` ruft dagegen `buildBodyFor(world, id)` ohne Batch, also `CreateAndAddBody` je Body mit inkrementellem Tree-Insert, und baut danach mit `OptimizeBroadPhase()` den gesamten Tree sowieso neu. Bei zehntausenden Bodies (der Code-Kommentar spricht von 65 536) ist das die teuerste Phase des Szenenstarts.
- **Code-Auszug**:
```cpp
for (uint32_t entityId : bodies)
    buildBodyFor(world, entityId);          // batch == nullptr -> CreateAndAddBody
...
m_impl->physicsSystem.OptimizeBroadPhase();
```
- **Lösung**: In `initialize()` ein `BodyBatch` verwenden: `buildBodyFor(world, id, &batch)` für `bodies`, danach einmal `AddBodiesPrepare`/`AddBodiesFinalize(..., EActivation::Activate)` (statische Bodies aktiviert Jolt nicht), dann `OptimizeBroadPhase()` wie bisher. Der Code dafür existiert in `addEntities` bereits; in eine Hilfsfunktion `commitBatch()` ziehen und in beiden Pfaden nutzen.
- **Erwarteter Effekt**: (Schätzung) Deutlich kürzerer Szenenstart bei großen Szenen. Messen: `initialize()` bei 20 000 Bodies, vorher/nachher.
- **Risiko / Verifikation**: Die Tree-Struktur ändert sich, damit potenziell die Reihenfolge von Kontakt-Callbacks und Raycast-Gleichstand-Auflösung; Determinismus-Tests und Snapshots mit festen Seeds prüfen (Jolt-Determinismus gilt pro Aufbau, nicht zwischen zwei Aufbauarten). Joint-Phase muss nach dem Batch-Commit laufen (tut sie ohnehin als zweite Phase).

---

### SCN-610 – applyWater: Container-Allokationen pro Schritt und ein 360-Zeilen-Monolith
- **ID**: SCN-610
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:3765-4124, bes. 3811-3817, 3878-3880, 4003-4006, 4070, 4120-4121
- **Problem**: Pro Schritt mit Wasser: je Region ein `AllHitCollisionCollector` (Heap-Array), `waterSeen` (`unordered_set`) wird neu befüllt, `waterOwedNext`/`waterTrackNext`/`waterTrack` sind `unordered_map<uint32_t,…>` mit einem Knoten-`new` pro nassem Body und Schritt (Insert in `…Next`, Swap, Clear gibt Knoten frei). Dazu `std::sort` + `unique` über alle Kandidaten und zweifach `reg.try_get<RigidBodyComponent>` je Kandidat (Z. 3843 und 4033). Die Funktion enthält drei Lambdas (`raiseRings`, `pushEvent`, `allowSleepAgain`) und ist ca. 360 Zeilen lang (Kategorie A).
- **Code-Auszug**:
```cpp
impl.waterTrackNext[c.entity] = Impl::WaterTrack{ share, factor * share };
...
impl.waterOwed.swap(impl.waterOwedNext);
impl.waterTrack.swap(impl.waterTrackNext);
```
- **Lösung**: Kandidaten sind nach (owed, entity) sortiert; `waterOwed`/`waterTrack` als nach Entity sortierte flache Vektoren (`vector<pair<uint32_t,T>>`, `std::lower_bound`) oder offene Adressierung führen, `waterSeen` durch Marker-Generation im Track-Eintrag ersetzen. Collector-Vektor wiederverwenden (Member). Die Funktion in `gatherWaterCandidates`, `serveWaterBodies`, `emitWaterExits` und die Ripple-Logik in eine eigene Klasse/Datei trennen (Vereinfachung).
- **Erwarteter Effekt**: (Schätzung) Nur in Szenen mit Wasser und vielen schwimmenden Bodies relevant; entfernt O(nasse Bodies) Heap-Operationen pro Schritt. Messen mit `budgetStats().buoyancyMs`.
- **Risiko / Verifikation**: Die Reihenfolge der Events und Ringe ist durch die Sortierung nach Entity bereits deterministisch (Z. 4109); beim Wechsel der Container muss sie erhalten bleiben. Wasser-Tests (Eintritt/Austritt, Owed-Budget, Held-Body, Entity-Löschen) laufen lassen.

---

### SCN-611 – Character-Schleife: Neuaufbau und Sortierung pro Schritt, `operator[]`-Inserts, voller Pose-Walk für die Rotation
- **ID**: SCN-611
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:3411-3414, 3441-3449, 3477-3488, 3501, 3591, 3616-3619
- **Problem**: (a) `stepCharacters` wird jeden Schritt aus der Map neu gebaut und, mit Budget (Standard offenbar aktiv), per `std::stable_sort` sortiert, wobei der Komparator pro Vergleich zwei `owedOf.find` macht. Solange niemand etwas schuldet, ist der Komparator konstant `false` und die Sortierung ein reiner Leerlauf. (b) `characterOwed[entityId]` (Z. 3501) legt für **jeden** Character einen Eintrag mit 0 an (Kommentar Z. 693 sagt „Absent = nothing owed"); danach läuft am Ende zusätzlich eine Schleife über alle Einträge (Z. 3616-3619). (c) `holdNeeded()` (ruft u. a. die `std::function` `holdProbe`) wird pro Character statt einmal pro Schritt ausgewertet. (d) Für `MoveKinematic` wird `worldPoseOf(world, entity).rotation` berechnet: kompletter Hierarchie-Walk, Matrix-Zerlegung, `quat_cast`, `normalize`, nur um die Rotation zu erhalten.
- **Code-Auszug**:
```cpp
std::stable_sort(m_impl->stepCharacters.begin(), m_impl->stepCharacters.end(),
                 [&owedOf](uint32_t a, uint32_t b) { const auto ia = owedOf.find(a), ib = owedOf.find(b); ... });
...
float& owed = m_impl->characterOwed[entityId];
...
const glm::quat q = worldPoseOf(world, entity).rotation;
```
- **Lösung**: (a) Sortieren nur, wenn `characterOwed` Einträge > 0 hat (Zähler `owedPositive`); Ergebnis identisch (stabile Sortierung mit konstantem Komparator ändert nichts). (b) Nur bei tatsächlicher Schuld einfügen (`find` statt `[]`), bei 0 erasen. (c) `holdNeeded()` einmal vor der Schleife auswerten und nach einem `insert` in `heldCharacters` neu. (d) Rotation einmalig pro Schritt aus dem Matrixpfad ableiten oder den vorhandenen Rotationspfad (nur Rotation, kein `decomposeWorld` mit Skalierung/Determinante) nutzen.
- **Erwarteter Effekt**: (Schätzung) Für wenige Characters vernachlässigbar, für KI-Massen (hunderte CharacterVirtuals) O(n log n) Hash-Lookups pro Schritt weniger. Niedrig bis mittel; messen.
- **Risiko / Verifikation**: Reihenfolge muss exakt gleich bleiben, wenn keine Schuld existiert: `stepCharacters` folgt dann der Map-Iterationsreihenfolge, also nur den Sortierschritt weglassen, nicht die Reihenfolge ändern. Tests: Budget-Deferral (Owed, kGuaranteedCharacters) und Region-Hold für Characters.

---

### SCN-612 – Poll-Queues verlieren bei jedem Poll ihre Kapazität
- **ID**: SCN-612
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:321-343, 428-452 (`take`), 1626-1633 (`pollWaterEvents`), 2952-2959 (`pollJointBroken`), 4952-4974
- **Problem**: `take()` tauscht die Queue mit einem leeren `result` (`result.swap(queue)`), danach hat die Queue Kapazität 0 und die Worker-Callbacks `push_back`en im nächsten Schritt wieder von vorn (mehrfaches Wachstum/Neuallokation unter dem globalen Lock). Dasselbe bei `pollWaterEvents`/`pollJointBroken`. Vier Event-Vektoren pro Schritt; der Aufrufer bekommt jedes Mal einen neuen `std::vector` zurück. Zusätzlich dupliziert `take` den Komparator aus `sortEvents` (Z. 285-290, siehe SCN-619).
- **Code-Auszug**:
```cpp
if (m_limit == 0 || queue.size() <= m_limit) {
    result.swap(queue);
    carried = 0;
    return result;
}
```
- **Lösung**: Die Poll-Funktionen mit Out-Parameter `void pollX(std::vector<CollisionEvent>& out)` (alt: `out.clear(); out.swap(queue)` plus `queue.reserve(out.capacity())`), oder den Puffer pro Poll zurückgeben und der Queue einen vorallokierten Ersatz geben. Die vorhandene Rückgabe-Variante kann als dünner Wrapper bleiben (API-kompatibel).
- **Erwarteter Effekt**: (Schätzung) Ein paar Allokationen pro Schritt weniger; klein, aber im Callback-Hot-Path unter Lock.
- **Risiko / Verifikation**: Öffentliche API in `PhysicsWorld.h` und Aufrufer (CollisionSystem, EngineApi) betroffen; die Wrapper-Lösung vermeidet Änderungen. Collision-Event-Tests (inkl. Limit/Backlog) laufen lassen.

---

### SCN-613 – PropertyAnimationSystem: Komponenten-Lookups pro Kanal statt pro Entity
- **ID**: SCN-613
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PropertyAnimationSystem.cpp:56-61, 63-129, 131-148
- **Problem**: `applyAt` ruft pro Kanal `applyChannel`, das jedes Mal `try_get<TransformComponent>` und `try_get<MaterialComponent>` (zwei Sparse-Set-Lookups) ausführt, auch bei reinen Transformkanälen. Ein Clip mit 9 Transformkanälen macht 18 Lookups pro Entity und Frame. Der `Visible`-Kanal löst sechs weitere `try_get`s aus. Es gibt kein „Wert unverändert"-Skip: auch ein gehaltener Wert setzt `tc->dirty`/`mc->dirty` und überschreibt Material-Assets (siehe Cross-Modul). `cm.getPropertyAnimClip(pa.clipId)` (Hash-Lookup) läuft zusätzlich pro Entity und Frame.
- **Code-Auszug**:
```cpp
void PropertyAnimationSystem::applyChannel(HorizonWorld& world, ContentManager& cm, entt::entity e,
                                           PropTarget target, float v)
{
    auto& reg = world.registry();
    TransformComponent* tc = reg.try_get<TransformComponent>(e);
    MaterialComponent*  mc = reg.try_get<MaterialComponent>(e);
```
- **Lösung**: In `applyAt` einmal `tc`/`mc` ermitteln und als `struct Targets{ TransformComponent* tc; MaterialComponent* mc; }` an `applyChannel` geben; die übrigen Typen (Camera, Mesh, Light …) nur lazy im Kanal-Zweig. `tc->dirty = true` einmal am Ende von `applyAt`, wenn mindestens ein Transformkanal gesetzt wurde. Optional: Wert gegen den bisherigen vergleichen und `dirty` nur bei Änderung setzen.
- **Erwarteter Effekt**: (Schätzung) Etwa 2-3× weniger Registry-Lookups pro animierter Entity; relevant bei vielen animierten Props (Türen, Plattformen, Lichter); sonst klein.
- **Risiko / Verifikation**: Verhaltensgleich, solange die Setzung von `dirty` unverändert bleibt. Bestehende PropertyAnimator-Tests (Step-Kanal `Visible`, Looping, Material) prüfen.

---

### SCN-614 – Begin-/End-Frame-Durchläufe für Per-Frame-Flags
- **ID**: SCN-614
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/PoseFinalize.cpp:434-440, 525-548; src/HE_Scene/src/RootMotionApply.cpp:83-87, 174-185; src/HE_Scene/src/SceneSystems.cpp:213, 217
- **Problem**: `tickAnimation` startet jeden Frame mit zwei vollständigen View-Durchläufen (`rootMotionBeginFrame`, `poseBeginFrame` mit zwei Views) nur um Flags zurückzusetzen, und endet mit zwei weiteren (`rootMotionEndFrame`, `poseEndFrame`). `poseEndFrame` ist reine Diagnose („Entity hat Layers, aber keinen Animator") und läuft trotzdem in Release jeden Frame über alle Layer- und IK-Komponenten. Bei Crowds mit `RootMotionComponent`/`IkComponent` an hunderten Entities sind das vier Komponenten-Scans pro Frame, ohne Nutzen für unkritische Fälle.
- **Code-Auszug**:
```cpp
for (auto [e, lc] : world.registry().view<AnimationLayerComponent>().each())
    lc.finalizedThisFrame = false;
```
- **Lösung**: Statt Bool-Flag ein Frame-Stempel: `uint32_t finalizedFrame`/`appliedFrame` im Component, `tickAnimation` zählt `frameId` hoch; „schon gemacht" ist `finalizedFrame == frameId`. Die Begin-Durchläufe entfallen vollständig. Für `rootMotionEndFrame` nur über Entities iterieren, die zuletzt Velocity geschrieben haben (kleine Menge in `RootMotionContext`). `poseEndFrame` nur in Editor-/Debug-Builds oder alle N Frames (Log ist ohnehin gedrosselt).
- **Erwarteter Effekt**: (Schätzung) Nur messbar bei vielen tausend solchen Komponenten; sonst im Rauschen. Dafür weniger Code im Kernpfad.
- **Risiko / Verifikation**: Das Flag ist Teil der Komponenten-Daten, daher Serialisierung und Editor-Inspector prüfen (nicht serialisiert?). Der Doppelfahrer-Fall („zwei Treiber, einer gewinnt") muss weiterhin warnen: Test mit Entity, die zwei Treiber trägt.

---

### SCN-615 – Fuß-IK: Raycast pro Fuß und Frame ohne Distanz-/Sichtbarkeits-Skip, Allokationen in `applyIk`
- **ID**: SCN-615
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PoseFinalize.cpp:275-430 (bes. 311, 333-337, 408-409); src/HE_Scene/src/PhysicsWorld.cpp:4375-4436 (`raycast`)
- **Problem**: Jede animierte Entity mit `IkComponent` und `feet` macht pro Fuß und Frame einen vollständigen Physik-Raycast (`physics->raycast`), unabhängig davon, ob die Entity sichtbar oder weit weg ist. Jeder Raycast bezahlt `QueryCharge` (zwei `steady_clock::now`, zwei Atomics) und wird dem **Physik-Budget** angerechnet (Z. 3426); viele IK-Charaktere verringern so den Anteil für Character-Schritte (Kopplung Animation → Physik-Budget). Zusätzlich je Aufruf `toSolve`, `chain`, `chainWeights` als Heap-Vektoren, `glm::inverse(entityWorld)` und ein zusätzlicher `worldMatrixOf` (RootMotion ruft ihn kurz davor schon, siehe SCN-617).
- **Code-Auszug**:
```cpp
const PhysicsWorld::RaycastHit hit =
    physics->raycast(footWorld + glm::vec3(0.0f, f.traceUp, 0.0f),
                     glm::vec3(0.0f, -1.0f, 0.0f), std::max(0.0f, f.traceUp + f.traceDown),
                     static_cast<uint32_t>(e));
```
- **Lösung**: Optionaler Distanz-/Sichtbarkeits-Cutoff: Ab Abstand X zur Kamera (oder wenn die Entity gecullt ist) das Foot-IK-Gewicht auf 0 ausblenden und nicht traceen; den bereits geglätteten Offset ausfaden. Raycasts über Frames verteilen (jeder Fuß alle 2 Frames, dazwischen interpoliert). Die Query nicht dem Physik-Budget zuschlagen (eigener Zähler). Vektoren in `PoseScratch` (siehe SCN-605).
- **Erwarteter Effekt**: (Schätzung) Für Crowds mit IK proportional zur Zahl der IK-Füße, die dadurch nicht mehr jeden Frame casten; hängt vom Spieldesign ab. Messen.
- **Risiko / Verifikation**: Ändert das sichtbare Verhalten (ferne Charaktere ohne IK). Muss als Option/Standard-Schwelle bewusst entschieden werden, die Editor-Vorschau (physics == null, Fuß-IK aus) bleibt unberührt.

---

### SCN-616 – Trails: Löschen am Vektoranfang und Hierarchie-Walk pro Trail und Frame
- **ID**: SCN-616
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/RopeTrailSystem.cpp:234-264, 293-294
- **Problem**: `stepTrail` altert alle Punkte (`p.age += dt`), löscht abgelaufene Punkte per `erase(begin, alive)` am Vektoranfang und kürzt auf `maxPoints` ebenfalls am Anfang. Beides ist O(Punkte) Verschiebung pro Frame und Trail (Ringpuffer/`deque` wären O(1)). `worldPositionOf` läuft für jeden Trail jeden Frame, auch wenn `emitting == false` und keine Punkte mehr vorhanden sind (Wert wird nur im `emitting`-Zweig gebraucht). Das Altern aller Punkte ließe sich durch einen Zeitstempel pro Punkt ersetzen.
- **Code-Auszug**:
```cpp
for (auto [e, trail] : reg.view<TrailComponent>().each())
    stepTrail(trail, HE::worldPositionOf(world, e), dt);
...
trail.points.erase(trail.points.begin(), alive);
```
- **Lösung**: `worldPositionOf` erst im `emitting`-Zweig aufrufen (Lambda/Callback übergeben); Punkte mit Geburtszeit statt Alter speichern (`age = now - born`, beim Bauen der Geometrie berechnet), dann entfällt die Alterungsschleife; Speicher als `std::deque` oder Ring.
- **Erwarteter Effekt**: (Schätzung) Klein; relevant nur bei vielen Trails mit hoher `maxPoints`.
- **Risiko / Verifikation**: `TrailComponent::Point` ist evtl. serialisiert/exponiert (Editor/Skript); Änderung der Punktdarstellung prüfen. `buildTrailGeometry` liest `age` direkt.

---

### SCN-617 – Root Motion: wiederholte Clip-Samples und Hierarchie-Walks pro Entity und Frame
- **ID**: SCN-617
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/RootMotion.cpp:89, 157, 67-78; src/HE_Scene/src/RootMotionApply.cpp:61, 114-133; src/HE_Scene/src/PoseFinalize.cpp:474
- **Problem**: Pro Entity und Frame: `findRootJoint` (lineare Stringvergleiche über das Skelett) wird in `rootMotionSampleClip` je Clip/Sample aufgerufen, in `poseFinalize` nochmal pro Entity; `extractRootMotion` sampelt den Root bei t=0 für `invRest` und `lockRootJoint` sampelt ihn wieder bei t=0 (`first`); beides sind konstante Werte pro Clip. In `rootMotionApply` laufen bis zu drei Hierarchie-Walks (`entityWorldYaw` → `worldMatrixOf`, `worldPositionOf`, `localPositionForWorld`), danach in `applyIk` ein weiterer `worldMatrixOf` plus `glm::inverse`.
- **Code-Auszug**:
```cpp
const glm::quat invRest = glm::inverse(sampleJoint(clip, joint, 0.0f).rotation);   // RootMotion.cpp:89
const JointTRS first = sampleJoint(clip, static_cast<uint32_t>(rootJoint), 0.0f);   // RootMotion.cpp:157
```
- **Lösung**: Root-Joint-Index im `RootMotionComponent` cachen, invalidiert wie `resolvedForMeshId` in `IkComponent`; Root-Frame-0-Pose und `invRest` einmal pro Clip im `AnimationClipAsset` (beim Laden/Cooking). In `rootMotionApply` die Weltmatrix einmal holen und Yaw und Position daraus ableiten. Der Aufwand pro Aufruf ist klein, die Summe pro Frame bei Crowds nicht.
- **Erwarteter Effekt**: (Schätzung) Klein (Prozentbereich der Root-Motion-Kosten). Nur mit Messung angehen; Hauptkosten sind `sampleClip` über das Skelett.
- **Risiko / Verifikation**: Cache-Invalidierung bei Mesh-/Clip-Wechsel und Hot-Reload; vorhandene Root-Motion-Tests (Wrap/Laps, Blend Space, Charakter-Controller-Modus).

---

### SCN-618 – Physik-Queries: Shape- und Set-Allokationen pro Aufruf
- **ID**: SCN-618
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:4188-4210 (`HEOverlapCollector`), 4450, 4473, 4498, 4513, 4531, 4553, 4603
- **Problem**: Jeder `sphereCast`/`boxCast`/`capsuleCast`/`overlap*`-Aufruf macht `new JPH::SphereShape/BoxShape/CapsuleShape` (Heap + Refcount). `HEOverlapCollector` pflegt für die Deduplizierung ein `unordered_set` (Knotenallokation je Treffer), `raycastAll` ebenso (`seen`). Skripte rufen solche Queries oft pro Frame und Entity auf. Die Zahl der Treffer ist meist klein.
- **Code-Auszug**:
```cpp
const JPH::Ref<JPH::Shape> sphere = new JPH::SphereShape(radius);
...
if (m_seen.insert(m_current).second) m_entities.push_back(m_current);
```
- **Lösung**: Statt `unordered_set` bei bis zu ~32 Treffern lineare Suche im bereits vorhandenen `m_entities` (kein Zusatzcontainer); Shapes in kleinem thread-lokalen Cache nach Parametern. Erst nach Messung (Skript-Last).
- **Erwarteter Effekt**: (Schätzung) Klein; nur bei hunderten Queries pro Frame sichtbar.
- **Risiko / Verifikation**: Reihenfolge der Treffer (Einfügereihenfolge) bleibt bei linearer Suche gleich.

---

### SCN-619 – PhysicsWorld.cpp: 5000-Zeilen-Datei, toter Code, doppelte Includes/Komparatoren
- **ID**: SCN-619
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:8 und 74 (`<unordered_set>` doppelt), 285-290 (`sortEvents`, nirgends benutzt), 438-440 (gleicher Komparator in `take`), 1999-2066 vs. 2960 ff., ganze Datei (5038 Zeilen)
- **Problem**: `HEContactListener::sortEvents` ist definiert und ungenutzt (`grep` findet nur die Definition); `take()` enthält denselben Komparator inline. `<unordered_set>` ist zweimal eingebunden. Die Datei mischt Layer-Filter, Listener, Shape-Bau, Joints, Character, Region-Hold, Wasser und Queries in einer Übersetzungseinheit (lange Compile-Zeit, schwer zu reviewen). `jumpCharacter(entityId)` schlägt Component und Character nach und ruft dann die Überladung, die beides noch einmal nachschlägt (Z. 4838-4871).
- **Code-Auszug**:
```cpp
static void sortEvents(std::vector<PhysicsWorld::CollisionEvent>& events)   // ungenutzt
```
- **Lösung**: `sortEvents` entfernen oder in `take()` verwenden (eine Quelle für den Komparator), Doppel-Include entfernen. Mittelfristig Aufteilung in `PhysicsWorld.cpp` (Kern/Step), `PhysicsJoints.cpp`, `PhysicsWater.cpp`, `PhysicsQueries.cpp`, `PhysicsShapes.cpp` mit gemeinsamer interner Header für `Impl`.
- **Erwarteter Effekt**: Lesbarkeit und inkrementelle Build-Zeit; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Rein mechanisch; Build und `he_tests` genügen.

---

### SCN-620 – PlayerHost: ungenutzte Includes, Durchreich-Wrapper, Start lädt jede HorizonCode-Klasse
- **ID**: SCN-620
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/PlayerHost.cpp:15, 17, 21-28, 79-97
- **Problem**: `<filesystem>` und `<unordered_set>` werden eingebunden, aber nicht verwendet (nur die Include-Zeilen treffen im Grep). `discoverAssets` im anonymen Namespace ist ein reiner Durchreich-Wrapper um `cm.discoverAssets`. Wichtiger: `begin()` ruft `resolveClassAsset` für **jede** HorizonCode-Klasse des Projekts (Kommentar Z. 72-78: das lädt alle Vorfahren), nur um PlayerController/PlayerCharacter zu finden. Bei großen Projekten kostet das beim Sessionstart Zeit und Speicher (Level-Graphen werden pro Klasse zusammengesetzt).
- **Code-Auszug**:
```cpp
for (const std::string& assetPath : classPaths) {
    HorizonCode::ResolvedClass rc = HorizonCode::resolveClassAsset(cm, assetPath);
    if (HorizonCode::engineClassIsA(rc.engineBase, "PlayerCharacter")) { ... }
```
- **Lösung**: Includes und Wrapper entfernen. Für den Start: `engineBase` pro Klasse aus einer leichten Metadatenquelle (Asset-Header oder `TypeRegistry`) lesen, oder `resolveClassAsset` erst nach billiger Vorprüfung aufrufen, und nur die Treffer vollständig auflösen.
- **Erwarteter Effekt**: (Schätzung) Einmalig beim Start; unsicher, ob messbar. Nur bei großen Class-Beständen relevant.
- **Risiko / Verifikation**: Die Vorprüfung muss auch abgeleitete Klassen finden (Kommentar Z. 88-91); Test mit Klasse → Klasse → PlayerController.

---

### SCN-621 – SceneJsonParse: Index-Vektor ohne `reserve`, zeichenweiser Scanner
- **ID**: SCN-621
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/SceneJsonParse.cpp:34-117 (bes. 96), 20-28
- **Problem**: `findEntities` (Text) füllt `elements` per `push_back` ohne `reserve`, während die CBOR-Variante vorab reserviert (Z. 225). Die Scannerschleife geht zeichenweise mit `isSpace` und `switch`; `skipString` sucht Anführungszeichen/Backslash byteweise. Das ist ein serieller Vorlauf vor dem parallelen Parse (Amdahl). Bei mehreren 100 MB Szenentext liegt er im Bereich einer Sekunde, im Vergleich zum DOM-Parse jedoch klein (unsicher, messen).
- **Code-Auszug**:
```cpp
if (inArray && depth == 2) { elements.push_back({ elemBegin, i + 1 }); }
```
- **Lösung**: `elements.reserve(text.size() / 512)` o. ä. als Heuristik; `skipString` mit `memchr`/zwei-Zeichen-Suche beschleunigen. Nur nach Messung weitergehen.
- **Erwarteter Effekt**: (Schätzung) Kleiner einstelliger Prozentanteil am Ladevorgang.
- **Risiko / Verifikation**: Verhalten gleich; bestehende `parseSceneText`-Tests (Fallback bei Sonderformen, doppelte Keys, Escapes) laufen lassen.

---

## Gelesen

Alle 13 Dateien des Slice vollständig gelesen: ja (PhysicsWorld.cpp 1-5038 in vier Blöcken, PlayerHost.cpp, PlayerSettings.cpp, PoseFinalize.cpp/.h, PoseSource.cpp/.h, PropertyAnimationSystem.cpp, RootMotion.cpp, RootMotionApply.cpp/.h, RopeTrailSystem.cpp, SceneJsonParse.cpp). Zusätzlich gezielt per Grep geprüft: `BlendSpaceCache` (ist eine `std::deque`, Zeigerstabilität bestätigt), Aufrufstellen in `SceneSystems.cpp`, Nutzung von `sortEvents` und `PropertyAnimationSystem::advance`.

## Scene Slice 7 – SceneSerializer, SceneSystems, ScriptApi, ScriptContext, Sequence*, Spline*, Terrain*

## Zusammenfassung

Der Slice (13 Dateien, ca. 10,6k Zeilen) ist insgesamt sauber und sehr gut kommentiert. Die Probleme konzentrieren sich auf drei Stellen. (1) Der Szenen-Serializer macht beim Speichern/Undo mehrfach tiefe JSON-Kopien, darunter Multi-MB-Base64-Strings des Terrains. Er parst beim Prefab-Sync dasselbe CBOR-Blob pro Instanz neu. Er hat mehrere quadratische Suchen über lineare `Binding`-Vektoren. (2) Die Lua-Brücke baut pro Aufruf einen schweren `HE::api::Ctx` (ca. 25 `std::function`-Member, drei davon kopiert) und allokiert `vector<Value>`. (3) Per-Frame-Systeme (`SequenceSystem::begin/apply`) iterieren ohne Dirty-Flag über alle `SkeletalMeshComponent` und allokieren pro Spieler/Frame. Die Terrain-Werkzeuge kopieren pro Pinselstrich ganze Felder (Smooth, Weightmap). Gefunden: 4 Hoch, 9 Mittel, 10 Niedrig. Alle Aussagen sind aus dem Code belegt. Messdaten gibt es nicht, Schätzungen sind als solche markiert.

---

### SCN-701 – buildSceneJson/buildSubtreeJson kopieren den gesamten JSON-Baum mehrfach (Undo-Snapshot)
- **ID**: SCN-701
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/SceneSerializer.cpp:1084-1093`, `:721`, `:2588-2597` (analog in `buildSubtreeJson`)
- **Problem**: Jede Entity-Ebene wird per Copy-Zuweisung statt per Move in den Baum gehängt. `comps` wird nach `eJson` kopiert, `eJson` nach `entities` kopiert, `entities` nach `scene` kopiert. Zusätzlich wird in `serializeComponents` `comps["terrain"] = tc;` kopiert. Ein Terrain mit `sculptHeightsB64` (1025² Floats = 4 MB, base64 ca. 5,6 MB), Layer-Weights und Wasser-Zellen wird dadurch bis zu vier Mal deep-kopiert (tc→comps, comps→eJson, eJson→entities, entities→scene). Bei 200k Entities kommen vier komplette Kopien aller nlohmann-Knoten hinzu (je Knoten ein `std::map`-Node plus Strings). `saveToMemory` ist der Undo-Pfad: `EditorUndo.cpp:10,42,84,118` ruft ihn pro Kommando auf. Auch `{ "points", pts }`/`{ "levels", levels }`/`{ "layers", ... }` in Initializer-Listen kopieren (nlohmann-Initializer-Lists kopieren immer).
- **Code-Auszug**:
```cpp
json comps = serializeComponents(registry, entity);
if (!comps.is_null())
    eJson["components"] = comps;          // Kopie
entities.push_back(eJson);                // Kopie
...
scene["entities"] = entities;             // Kopie des ganzen Arrays
...
comps["terrain"] = tc;                    // Kopie inkl. MB-Strings (Z.721)
```
- **Lösung**: Überall `std::move` setzen: `eJson["components"] = std::move(comps); entities.push_back(std::move(eJson)); scene["entities"] = std::move(entities); eJson["children"] = std::move(children); comps["terrain"] = std::move(tc);`. In `buildSubtreeJson` genauso (2588, 2593, 2595, 2597). Optional `entities.get_ref<json::array_t&>().reserve(view.size())`. Verhalten bleibt bit-identisch, weil nur Kopien durch Moves ersetzt werden.
- **Erwarteter Effekt** (Schätzung): Snapshot-Zeit und Spitzen-Speicher sinken bei großen Szenen um grob 30-60 % (Kopien dominieren neben dem CBOR-Encode). Bei Terrain-Szenen kommen ca. 20 MB weniger memcpy pro Snapshot dazu.
- **Risiko / Verifikation**: Sehr gering. `tests/test_scene_serializer*` (Round-Trip) plus ein Test, der die Bytes von `saveToMemory` vor/nach der Änderung vergleicht (Byte-Gleichheit des CBOR). Zeit messen mit `lastLoadTiming`/`HE_LOG_SLOW_SCOPE` an einer 100k-Entity-Szene.

### SCN-702 – syncPrefabInstances parst pro Instanz das CBOR neu und löst Bindings quadratisch auf (läuft bei jedem Speichern)
- **ID**: SCN-702
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:3650`, `:3734-3739`, `:3752`, `:3778`, `:3930-3931`, `:4030`, `:4116-4169`; Aufrufer `src/HE_Editor/EditorApplication.cpp:13604` (jeder Save), `13751/13858` (Open)
- **Problem**: `syncPrefabInstances` cached die Blob-Bytes pro Prefab-Asset (`blobs`), aber `syncPrefabInstance` ruft pro Instanz `json::from_cbor(assetBlob)` auf. 1000 Platzierungen desselben Prefabs bedeuten 1000 identische CBOR-Parses. Innerhalb einer Instanz gilt: `recordPresent(key)` ist eine lineare Suche über alle Records (aufgerufen pro Binding und pro Subtree-Entity), `hasBinding` ist `std::any_of` über `work.bindings` pro Record, und `instanceOf/templateOf` sind lineare Scans (`PrefabInstanceComponent.h:104-115`). Das ergibt O(R²) pro Instanz. Dazu kommt pro Record ein volles `serializeComponents(registry, e)` (ca. 47 `try_get` plus JSON-Aufbau) und `json merged = block;` (tiefe Kopie), auch wenn am Ende nichts zu tun ist (Kommentar Z.4026: „a sync that changes nothing also touches nothing"). Der „nichts hat sich geändert"-Normalfall zahlt also den vollen Preis.
- **Code-Auszug**:
```cpp
const json scene = json::from_cbor(assetBlob, /*strict=*/true, /*allow_exceptions=*/false); // pro Instanz
...
auto recordPresent = [&](const HE::UUID& key) {
    for (const json* r : records) if (entityKeyOf(*r) == key) return true; return false; };
...
const bool hasBinding = std::any_of(work.bindings.begin(), work.bindings.end(), ...);
const json iComps = fresh ? json::object() : serializeComponents(registry, e);
```
- **Lösung**: (1) Geparstes JSON pro Asset-Id in `syncPrefabInstances` cachen (`unordered_map<UUID, json>`) und eine `syncPrefabInstance(world, root, const json& scene, ...)`-Überladung nutzen; die bytes-Variante bleibt als Wrapper. (2) Pro Aufruf einmal `unordered_set<UUID> recordKeys` und `unordered_map<UUID, UUID>` für `templateEntity→instanceEntity` und umgekehrt aufbauen und `recordPresent`/`hasBinding`/`counterpartOf` darauf umstellen (Reihenfolge der Ergebnisse bleibt, da nur Lookups ersetzt werden). (3) Optional einen Content-Hash/Dirty-Check pro Instanz (Asset-Version + Override-Liste unverändert → überspringen); das ändert Verhalten nur dann, wenn Hand-Edits an der Instanz relevant wären, daher separat absichern.
- **Erwarteter Effekt** (Schätzung): Save-Zeit bei Szenen mit vielen Prefab-Instanzen sinkt von O(Instanzen × Blobgröße) auf O(Blobs + Instanzen × Records). Bei 1000 Instanzen mit je 50 Records vermutlich Faktor 5-20 für den Sync-Anteil des Speicherns. Messen.
- **Risiko / Verifikation**: Mittel, weil die Funktion komplex ist (Nested-Placements). Die vorhandenen Tests in `tests/test_scene_serializer*`/Prefab-Sync-Tests müssen unverändert grün bleiben. Zusätzlich ein Test „1000 Instanzen, Zeitbudget" und ein Vergleich `PrefabSyncReport` vorher/nachher.

### SCN-703 – Lua-Dispatch: schwerer `HE::api::Ctx` und zwei `vector<Value>` pro Aufruf
- **ID**: SCN-703
- **Kategorie**: C
- **Schweregrad**: Hoch (Hot Path Skripte; Einschränkung: ohne Messdaten)
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/ScriptContext.cpp:82-109` (`apiCtx`), `:834-861` (`lua_engine_dispatch`), `:142-170` (`setPosition/setRotation`); Ctx-Definition `include/HorizonScene/EngineApi.h:60-150`
- **Problem**: Jeder Lua-Aufruf in die Engine-API ruft `apiCtx(L)` auf. Der Ctx enthält ca. 25 `std::function`-Member (`requestQuit`, `createObject`, `destroyObject`, Window-/Tray-/Menu-Callbacks …). Davon werden `requestQuit`, `createObject` und `destroyObject` pro Aufruf aus `HostServices` kopiert (Heap-Allokation, wenn der Lambda-Capture größer als der SBO-Puffer ist). Die übrigen werden default-konstruiert und wieder zerstört. Dazu kommen 3 `lua_getfield(LUA_REGISTRYINDEX, "…")` mit String-Hash pro Aufruf (`getWorld/getPhysics/getContent`) plus `getQuit`. `lua_engine_dispatch` allokiert `args` (reserve) und `res` (`vector<Value>`; `Value` trägt `std::string s`, `typeName`, mehrere Vektoren, also ein dickes Objekt) pro Aufruf. Skripte, die pro Frame viele `horizon.transform.*`/`math.*`-Aufrufe machen, zahlen das alles pro Aufruf.
- **Code-Auszug**:
```cpp
HE::api::Ctx c{};  c.world = getWorld(L); ...
c.createObject  = hs.createObject;      // std::function-Kopie
c.destroyObject = hs.destroyObject;     // std::function-Kopie
if (const std::function<void()>* quit = getQuit(L)) c.requestQuit = *quit;   // Kopie
...
std::vector<HorizonCode::Value> args; args.reserve(fn->params.size());
const std::vector<HorizonCode::Value> res = fn->invoke(c, args);
```
- **Lösung**: (a) Den Ctx pro `ScriptContext` einmal vorbauen und nur die veränderlichen Zeiger (`world/physics/content`) nachziehen. Die Registry-Slots kosten dann nichts, und der Cache wird in `setPhysicsWorld/setContentManager/setHostServices/setQuitHandler` invalidiert; Lua holt ihn per `lua_touserdata` aus einem Upvalue des Closures statt aus der Registry. Oder `Ctx` bekommt `const Services*` statt der Callback-Member. (b) `args`/`res` in einen kleinen Stack-Puffer (`absl::InlinedVector`-artig, `std::array` bis 8) oder als `thread_local` wiederverwendeten Vektor halten. (c) `lua_horizon_setPosition/setRotation` (Z.142-170) bauen den Ctx ebenfalls pro Aufruf; sie profitieren vom Cache.
- **Erwarteter Effekt** (Schätzung): pro Aufruf einige 100 ns bis ca. 1 µs, plus 0-3 Heap-Allokationen. Bei 5000 Calls/Frame grob 1-5 ms. **Messen**: Mikro-Benchmark `horizon.transform.setPosition` in Schleife.
- **Risiko / Verifikation**: Mittel. Lebensdauer der Services (Dangling nach Scene-Wechsel, siehe Kommentar Z.1385-1392) muss erhalten bleiben. Cache bei `~ScriptContext`/`setHostServices` invalidieren. Tests: Lua-Script-Tests plus Test „Quit-Handler nach `setQuitHandler` sichtbar".

### SCN-704 – collectAssetRefs(world, onlyEntities) durchsucht für jeden Slice die ganze Welt
- **ID**: SCN-704
- **Kategorie**: D
- **Schweregrad**: Hoch (bei Weltstreaming), sonst Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/SceneSystems.cpp:244-319`; Aufrufer `src/HE_Game/src/GameApplication.cpp:1504-1505`
- **Problem**: Die Teilmengen-Variante läuft über alle ca. 20 Component-Views der gesamten Welt und filtert erst danach per `keep(e)` (Hash-Lookup in einem frisch gebauten `unordered_set<uint32_t>`). Wird sie pro Streaming-Slice (`onlyEntities`) aufgerufen, ist der Aufwand O(Slices × Weltgröße) statt O(Slices × Slicegröße). Zusätzlich wird für jeden Slice ein neues `unordered_set` aufgebaut und `seen` neu allokiert.
- **Code-Auszug**:
```cpp
for (auto [e, c] : reg.view<MeshComponent>().each())  if (keep(e)) add(c.meshAssetId);
...  // 20 weitere Views, alle über die ganze Welt
const std::unordered_set<uint32_t> only(onlyEntities.begin(), onlyEntities.end());
```
- **Lösung**: Für die Teilmenge direkt über `onlyEntities` iterieren und pro Entity `reg.try_get<T>(e)` für jeden Typ aufrufen (O(|only| × 20)). Die Welt-Variante bleibt wie sie ist. Dafür die Typ-Liste einmal als generische Funktion `forEachRef(const Entity&, add)` ausdrücken (z. B. ein Template `visitRefs(reg, e, add)`) und für beide Varianten wiederverwenden; das ersetzt die 20 Zeilen Views durch eine Typliste. Reihenfolge der Ergebnisse (first-seen) ändert sich bei der Teilmengen-Variante (View-Reihenfolge vs. Slice-Reihenfolge). Prüfen, ob Aufrufer auf Reihenfolge angewiesen sind (vermutlich nicht, es ist ein Set).
- **Erwarteter Effekt** (Schätzung): bei Zellstreaming mit n Slices Faktor Weltgröße/Slicegröße, bei 200k Entities und 1000er Slices bis zu zwei Größenordnungen weniger Arbeit beim Nachladen.
- **Risiko / Verifikation**: Gering/Mittel (Reihenfolge). Test: gleiche Menge (nicht Reihenfolge) für Teilmenge und Voll-Welt vergleichen; vorhandene `collectAssetRefs`-Tests.

### SCN-705 – Laden: `c.value("key", json())` kopiert pro Feld ein JSON (Heap pro Entity)
- **ID**: SCN-705
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:1168-1170` (transform), `:1186`, `:1196`, `:1258-1260`, `:1290`, `:1307`, `:2012`, `:2154-2156`, `:2169-2186`, `:1856` (`c.value("layers", json::array())`), u. a.
- **Problem**: `json::value(key, json())` liefert das JSON **by value**. Bei einem Array wie `position` wird dadurch das ganze Array (3 Knoten plus Vektor-Allokation) kopiert, nur um es sofort in `jsonToVec3` zu zerlegen und zu verwerfen. Bei jeder Entity mit Transform sind das 3 tiefe Kopien. Material, Mesh, Light, UI usw. kommen dazu. Das ist reine Heap-Arbeit pro Entity beim Szenen-Load, bei Undo-Restore und beim Streaming (`applyAdditiveJson`).
- **Code-Auszug**:
```cpp
t.position = jsonToVec3(c.value("position", json()), t.position);
...
m.meshAssetId = jsonToUuid(c.value("asset", json()));
```
- **Lösung**: Hilfsfunktion `const json& field(const json& obj, const char* key)` mit `static const json kNull`, gibt `obj.find(key)` oder `kNull` zurück (keine Kopie). `jsonToVec3/jsonToUuid/...` nehmen bereits `const json&`. Mechanisch ersetzen: `jsonToVec3(field(c,"position"), t.position)`. Für das `layers`-Array `c.find("layers")` verwenden. Semantik identisch: nicht-Array → Fallback.
- **Erwarteter Effekt** (Schätzung): pro Entity 5-15 Heap-Allokationen weniger. Beim Laden großer Szenen einige Prozent der Build-Zeit (`buildMs`). Nach SCN-706 und SCN-701 messen.
- **Risiko / Verifikation**: Gering. Round-Trip-Tests plus Alt-Format-Tests (der Header `HAsset.h`/CLAUDE.md verlangt Test für alte Layouts).

### SCN-706 – applyComponents: bis zu 47 `contains()`-Lookups in `std::map` pro Entity
- **ID**: SCN-706
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance + Vereinfachung
- **Ort**: `SceneSerializer.cpp:1161-2192`, `:1117-1131`, `:2744-2836`
- **Problem**: `applyComponents` ist eine 1030 Zeilen lange Kette von `if (comps.contains("x")) { ... comps["x"] ... }`. Jede Entity macht ca. 47 String-Lookups (`contains`) in einer `std::map<string,json>`, und jedes Treffer-Feld macht erneut `comps["x"]` (zweiter Lookup). `serializeComponents` ist spiegelbildlich 47 `try_get`. Zusätzlich gibt es drei hand-gepflegte Schlüssellisten (`isKnownComponentKey`, `HE_SCENE_COMPONENT_KEYS`, die Ketten selbst). Der Kommentar bei 3011-3023 benennt die Drift-Probleme selbst.
- **Lösung**: Statt 47 Lookups: über `comps.items()` iterieren und per statischer `unordered_map<string_view, void(*)(registry&, Entity, const json&)>` dispatchen (eine Funktion pro Komponente, aus dem bestehenden Block herausgelöst). Die Reihenfolge der Anwendung muss gleich bleiben (z. B. `terrain` hängt von nichts ab, aber `prefab`/`navmesh` evtl.); sonst feste Reihenfolge per Tabelle. Danach kann `isKnownComponentKey` aus derselben Tabelle abgeleitet werden. Das ist ein größerer Umbau, daher nur mit Tests empfohlen.
- **Erwarteter Effekt** (Schätzung): kleiner als SCN-705; vor allem Wartbarkeit (eine Tabelle statt drei Listen) und einige Prozent Load-Zeit.
- **Risiko / Verifikation**: Mittel (Reihenfolge, `warnUnknownComponents`). `test_scene_serializer` deckt „jeder Key" ab.

### SCN-707 – applyPrefabJson: Stale-Bereinigung O(n·m) mit linearem `std::find`
- **ID**: SCN-707
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:2706-2723`
- **Problem**: `remove_if` über alle `children` der Welt-Root (Kommentar bei 2400: „it can hold a hundred thousand"), und für jedes Kind ein `std::find` in `stale`. Bei einem Prefab mit 2000 Entities in einer Szene mit 100k Root-Kindern sind das ca. 2·10⁸ Vergleiche pro Instanziierung (Kopieren/Einfügen, Undo-Redo, Collab). `applyAdditiveJson` löst dasselbe Problem schon sauber über den Tail der Liste (2463-2482).
- **Code-Auszug**:
```cpp
rc.erase(std::remove_if(rc.begin(), rc.end(), [&stale](Entity e)
         { return std::find(stale.begin(), stale.end(), e) != stale.end(); }), rc.end());
```
- **Lösung**: Entweder wie im Additiv-Pfad nur den Tail der Root-Liste ab `rootChildrenBefore` (vor der Schleife merken) betrachten, oder `stale` in ein `unordered_set<Entity>` legen (O(n+m)). Ergebnis (erhaltene Reihenfolge) bleibt identisch.
- **Erwarteter Effekt** (Schätzung): von O(n·m) auf O(n+m). Bei großen Szenen von Sekunden-Bruchteilen auf <1 ms.
- **Risiko / Verifikation**: Gering. Prefab-Instanziierungstests, plus Test „großes Root-Children + 1000er Prefab".

### SCN-708 – prefabInstancesBinding / addedHereUnder: O(Entities × Instanzen × Bindings)
- **ID**: SCN-708
- **Kategorie**: D
- **Schweregrad**: Mittel (Editor, Outliner-Rebuild)
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:4252-4263`, `:4325-4342`, `:4382-4389`; Aufrufer `src/HE_Editor/OutlinerPanel.cpp:670-674`; Datenstruktur `Components/PrefabInstanceComponent.h:104-115`
- **Problem**: `prefabInstancesBinding(world, entity)` iteriert alle `PrefabInstanceComponent`s der Welt und ruft jeweils das lineare `templateOf` über deren `bindings` auf. `addedHereUnder` ruft das für jedes Kind jedes Instanz-Roots auf, `prefabAddedHereCount` und `prefabRemovedHereCount` laufen zusätzlich pro Outliner-Knoten einer Instanz. Das ist O(E_unter_Instanzen × I × B) beim Outliner-Rebuild (Kommentar im Outliner: er läuft bei Strukturänderung). `prefabRemovedHereCount` ruft zudem pro Binding `findByEntityId`.
- **Lösung**: Pro Rebuild einmal einen Reverse-Index `unordered_map<UUID instanceEntity, small_vector<Entity root>>` aus allen `PrefabInstanceComponent`s aufbauen (O(I·B)) und `prefabInstancesBinding` darüber beantworten; an der Outliner-Stelle als Parameter durchreichen (API-Erweiterung `PrefabBindingIndex`). Die vorhandenen Einzel-Funktionen bleiben als Wrapper bestehen.
- **Erwarteter Effekt** (Schätzung): Outliner-Rebuild in Szenen mit vielen Prefab-Instanzen von quadratisch auf linear. Messen mit 500 Instanzen × 50 Kindern.
- **Risiko / Verifikation**: Gering. Outliner-UI-Tests (`tests/test_outliner_ui.cpp`) plus ein Test mit dem Index gegen die alte Funktion.

### SCN-709 – ScriptContext::destroyInstance durchsucht die gesamte Cooldown-Map
- **ID**: SCN-709
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/ScriptContext.cpp:1537-1544`, Struktur `include/HorizonScene/ScriptContext.h:351-364`
- **Problem**: Jedes `destroyInstance` iteriert die ganze `m_callbackReportNext`-Map, um die Einträge dieser Instanz zu entfernen. Beim Szenen-Teardown oder Massen-Despawn (n Instanzen) ist das O(n·m), und die Map enthält pro fehlerhaftem Callback einen Eintrag. Normalerweise ist die Map klein (nur fehlschlagende Callbacks), im Fehlerfall aber (ein defektes `onUpdate` auf 5000 Instanzen) wird sie groß.
- **Code-Auszug**:
```cpp
for (auto it = m_callbackReportNext.begin(); it != m_callbackReportNext.end(); )
    it = (it->first.id == id) ? m_callbackReportNext.erase(it) : std::next(it);
```
- **Lösung**: Early-out `if (m_callbackReportNext.empty()) return;` (billig, deckt den Normalfall ab), und für den Fehlerfall zweistufig `unordered_map<InstanceId, small array of (cb, next)>` oder die Map nach `id` gruppieren. Die einfache Variante (Early-out) reicht vermutlich.
- **Erwarteter Effekt**: Normalfall null Kosten; Fehlerfall O(1) pro Instanz statt O(m).
- **Risiko / Verifikation**: Gering. Test: Destroy mit Report-Cooldown, danach neu fehlschlagen → wird sofort gemeldet.

### SCN-710 – SequenceSystem::begin löscht `sequencePosed` auf allen Skeletal-Meshes, jeden Frame, auch ohne Sequenz
- **ID**: SCN-710
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/SequenceSystem.cpp:563-567`, `:699`, `:716-729`; Aufrufer `SceneSystems.cpp:222`
- **Problem**: `begin()` läuft über **alle** `SkeletalMeshComponent` (typisch einige hundert bis tausend Charaktere), um ein Bool zu löschen, bevor es auf `if (!ctx) return;` bzw. auf „keine SequencePlayer in der Szene" prüft. Das ist eine View ohne Dirty-Flag über die dichte Komponenten-Liste, und `sequencePosed` ist nur in Szenen mit laufender Sequenz überhaupt je `true`. Danach iteriert `begin` über alle `SequencePlayerComponent`, `apply` nochmal (`:700`), und `locksPlayerInput` (`:765`) pro Aufruf wieder.
- **Code-Auszug**:
```cpp
for (auto [e, smc] : reg.view<SkeletalMeshComponent>().each()) smc.sequencePosed = false;
if (!ctx) return;
```
- **Lösung**: Flag „es wurde im letzten Frame etwas geclaimt" als Zähler in `reg.ctx()` oder als `bool` im `SequenceContext` halten und die Löschschleife nur ausführen, wenn der Zähler > 0 war (nach dem Löschen auf 0 setzen, beim Claim in `begin` inkrementieren). Alternativ: kein Löschen, sondern ein Frame-Stempel (`uint32_t sequencePosedFrame`), den die anderen Treiber gegen den aktuellen Frame vergleichen. Das erfordert Änderungen in den drei Skeletal-Treibern (außerhalb dieses Slices). Die Zähler-Variante ist lokal und verhaltensgleich.
- **Erwarteter Effekt** (Schätzung): O(#SkeletalMesh) pro Frame → O(1) in Szenen ohne Sequenzen. Bei 1000 Charakteren grob 10-30 µs/Frame (Cache-Misse auf großen Komponenten). **Messen**.
- **Risiko / Verifikation**: Gering. Sequenz-Tests (`tests/test_sequence*`), insbesondere „Claim wird beim Stop gelöscht".

### SCN-711 – SequenceSystem: Heap-Allokationen pro Spieler und Frame
- **ID**: SCN-711
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `SequenceSystem.cpp:104-150` (`startSounds`), `:712` + `SequenceEval.cpp:73-102` (`evaluate`), `:660-662` + `:716-729` (`skeletalPoseAt` doppelt)
- **Problem**: (a) `startSounds` baut pro Audio-Track und Frame einen `starts`-Vektor mit `std::to_string(i)` (je ein String, ggf. Heap) und parst die Indizes per `std::stoul` zurück. Eine Notify-Namens-Rundreise nur, um den Index zu transportieren; `HE::NotifyQueue fired` und `starts` werden pro Aufruf neu angelegt. (b) `SequenceEval::evaluate` gibt ein `Result` mit `std::vector<PropertyWrite> writes` zurück, das pro Spieler und Frame wächst (push_back ohne reserve). (c) `skeletalPoseAt` (Slot-Lookup, `try_get`, `getAnimationClip`, `getSkeletalMesh`, `activeSection`-Scan) läuft pro Skeletal-Track doppelt (begin + apply). (d) `fireEvents`/`startSounds` laufen jeden Frame für alle Tracks, auch wenn der Span `[tPrev,tEnd]` keine Events enthält (`collectNotifySpan` entscheidet das erst nach dem Aufbau).
- **Code-Auszug**:
```cpp
for (size_t i = 0; i < tr.audio.size(); ++i)
    starts.push_back({ std::to_string(i), tr.audio[i].start, 0.0f });
...
const SequenceAudioSection& s = tr.audio[std::stoul(ev.name)];
```
- **Lösung**: (a) Für Audio-Sektionen direkt die Span-Regel auf `tr.audio[i].start` anwenden (eine kleine `spanContains(start, seq.duration, tPrev, tEnd, includeStart)`-Funktion, die `collectNotifySpan` bereits intern nutzt), ohne Namen-Rundreise. (b) `evaluate` bekommt einen `Result&`-Out-Parameter (`thread_local`/Member, `writes.clear()` statt neu allokieren). (c) Das Ergebnis von `skeletalPoseAt` aus `begin` in einem Scratch-Vektor pro Spieler für `apply` mitgeben. (d) Früh aussteigen, wenn der Track keine Events im Zeitfenster hat.
- **Erwarteter Effekt** (Schätzung): 3-10 Heap-Allokationen pro Spieler/Frame weniger, solange eine Sequenz spielt. Eher klein pro Frame, aber im Cutscene-Frame (viele Tracks) relevant.
- **Risiko / Verifikation**: Mittel für (a), weil die Span-Regel (Laps, geschlossener Ursprung) exakt gleich bleiben muss: Tests mit rückwärts/loop/Lap-Fällen aus den Sequenz-Tests übernehmen und gegen die alte Implementierung als Referenz vergleichen.

### SCN-712 – TerrainSculpt::apply (Smooth) kopiert das gesamte Höhenfeld pro Pinselstich
- **ID**: SCN-712
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/TerrainSculpt.cpp:243-246`, `:274-290`
- **Problem**: `before` ist eine Kopie von `tc.sculptHeights`. Bei Auflösung 1025 sind das 4 MB Allokation plus Kopie **pro Dab**, obwohl der Pinsel nur ein kleines Rechteck `[x0..x1]×[z0..z1]` verändert. Beim Ziehen des Smooth-Pinsels sind das viele Dabs pro Sekunde.
- **Code-Auszug**:
```cpp
const std::vector<float> before = (op == Op::Smooth) ? tc.sculptHeights
                                                     : std::vector<float>{};
```
- **Lösung**: Nur das Rechteck plus 1 Vertex Rand kopieren (`(w+2)*(h+2)` Floats, in einen wiederverwendeten `thread_local`-Puffer), und `before[...]` über Rect-lokale Indizes lesen. Das Ergebnis ist identisch, weil außerhalb des Rechtecks (+Rand) nie gelesen wird.
- **Erwarteter Effekt** (Schätzung): ca. 4 MB memcpy+Alloc pro Dab → wenige KB. Bei interaktivem Smoothen auf großen Terrains ein spürbarer Frame-Zeit-Gewinn (Größenordnung 1 ms/Dab bei 1025²).
- **Risiko / Verifikation**: Gering. Test: Smooth-Dab auf zufälligem Feld, Ergebnis bit-identisch zur alten Version (auch an Gitterrändern).

### SCN-713 – Weightmap: jede Pinselbewegung baut und lädt die komplette Textur neu
- **ID**: SCN-713
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/TerrainPaint.cpp:169-201`, `:132`; Verbraucher `TerrainSystem.cpp:396-420` (nicht Teil des Slices, zur Einordnung gelesen)
- **Problem**: `paint()` setzt nur ein globales `weightsDirty`. Der nächste Tick macht daraufhin `computeAverageLayerWeights(tc)`, `buildWeightTexture` (`usesSecondPage` scannt per `std::any_of` die komplette zweite Page; danach Kopie der gesamten `layerWeights`, bei zwei Pages row-weises Zusammenbauen), `cm.replaceTexture` und `InvalidateTexture`. Bei Standard 512² sind das 1 MB pro Page, bei `weightRes` bis 4096 bis zu 64 MB pro Page, **pro Frame während des Malens**. Anders als beim Sculpt (`regionDirty` in `TerrainSculpt.cpp:20-33`) gibt es kein Dirty-Rect für die Weights.
- **Lösung**: Dirty-Rect (`weightsDirtyX0/Z0/X1/Z1`) analog zu `markRegionDirty` führen, in `paint()` setzen und im System für Teil-Upload (`UpdateTextureRegion`-artige Backend-API, falls vorhanden; sonst nur das Rect neu zusammensetzen und die Durchschnittsberechnung inkrementell). `usesSecondPage` als gecachtes Bool im Component pflegen (`paint()` setzt es bei `layer>=4`; `fillLayer` ebenso) statt jedes Mal zu scannen. Der erste Schritt (gecachtes Flag, Rect für `computeAverageLayerWeights`) ist lokal und verhaltensgleich; der Teil-Upload braucht Renderer-Unterstützung.
- **Erwarteter Effekt** (Schätzung): bei 512² ca. 1-3 ms/Frame weniger beim Malen, bei 2048²+ deutlich mehr. **Messen** (Backends unterscheiden sich).
- **Risiko / Verifikation**: Mittel (Renderer-API). `tests/test_terrain*` für Paint plus Test, dass `buildWeightTexture` nach Teil-Update identisch zum Vollbau ist.

### SCN-714 – TerrainHeightmap::importPixels: Taps pro Zeile neu berechnet, Verzweigungen pro Sample
- **ID**: SCN-714
- **Kategorie**: D
- **Schweregrad**: Niedrig (nur Import, einmalig)
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/TerrainHeightmap.cpp:35-61`, `:142-161`
- **Problem**: `axisTaps(xi, ..., tx)` hängt nur von `xi` ab, wird aber für jede Zeile `zi` neu berechnet (res² statt res Aufrufe, je mit `std::floor/ceil` und `push_back`). `GreyReader::channel` verzweigt pro Sample auf `bytesPerChannel` und rechnet bei RGB 3 Mal. Beim Downsampling (4k → 513) sind das ca. 17 Mio. `grey()`-Aufrufe.
- **Lösung**: Die Taps aller Spalten einmal vor der Zeilenschleife in einen `vector<vector<Tap>>` (oder flach mit Offsets) vorberechnen. Die Verzweigung auf `bytesPerChannel`/`channels` per Template-Parameter `GreyReader<Bytes, Channels>` auflösen oder vorab in einen `vector<float>` Grauwerte (wie `greyFromTexture` schon macht) konvertieren und darauf samplen. Ergebnis identisch.
- **Erwarteter Effekt** (Schätzung): Importzeit großer Heightmaps -30 bis -60 %. Einmalige Editor-Aktion, daher Niedrig.
- **Risiko / Verifikation**: Gering. Bestehende `test_terrain_heightmap`-Tests plus Bit-Vergleich der Ausgabe.

### SCN-715 – ID-Maps ohne `reserve` beim Laden großer Szenen
- **ID**: SCN-715
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:2292`, `:2376`, `:2607` (`idMap`), `:2524` (`buildSubtreeJson`)
- **Problem**: `unordered_map<UUID, Entity> idMap` wächst bei 200k Entities über ca. 17 Rehash-Schritte; in `sliceForAdditiveLoad` (`:3199`) wird `at.reserve(n)` dagegen schon gemacht, die Absicht ist also bekannt.
- **Lösung**: `idMap.reserve(scene["entities"].size())` an den drei Stellen, `idMap.reserve(entities.size())` in `buildSubtreeJson`.
- **Erwarteter Effekt**: einige Prozent der Build-Zeit bei sehr großen Szenen (Schätzung).
- **Risiko / Verifikation**: Keines.

### SCN-716 – buildRibbon/buildTube allokieren pro Aufruf rund zehn Vektoren
- **ID**: SCN-716
- **Kategorie**: C
- **Schweregrad**: Niedrig (Hot Path nur, falls pro Frame für Trails/Seile aufgerufen; Aufrufer liegen in RopeTrailSystem, außerhalb des Slices)
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/SplineGeometry.cpp:197-243`, `:254-301`, `:303-367`, `:369-381`
- **Problem**: `buildRibbon` legt `pts`, `frames`, `tangents` (in `buildFrames`), `width`, `normal` sowie die vier `MeshData`-Vektoren pro Aufruf neu an. Für kamera-ausgerichtete Trails ändert sich das Mesh jeden Frame, also pro Trail ca. 9 Allokationen/Frame. `interleave` kopiert das Ergebnis dann nochmals. `mesh.positions.insert(end, {…})` mit Initializer-List ist langsamer als direktes Schreiben nach `resize`.
- **Lösung**: Eine `RibbonScratch`-Struktur (Vektoren, wiederverwendet) als optionalen Parameter; `buildFrames` mit Out-Vektor. `MeshData` am Aufrufer wiederverwenden (`clear()` statt neu). Optional direkt interleaved ausgeben. Nur sinnvoll nach Messung im RopeTrailSystem.
- **Erwarteter Effekt**: pro Trail und Frame ca. 8-10 Allokationen weniger (Schätzung); nur relevant bei vielen Trails.
- **Risiko / Verifikation**: Gering. `test_spline_geometry`-Tests; Ergebnis bitgleich.

### SCN-717 – ScriptApi: wiederholter Boilerplate und String-Kopien in Gettern
- **ID**: SCN-717
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/ScriptApi.cpp:159-259`, `:47-54`, `:167-174`; `ScriptContext.cpp:251-263` vs `:371-383`
- **Problem**: 15 Funktionen wiederholen `auto& reg = world.registry(); entt::entity e = toEntity(id); if (!reg.valid(e)) return …; reg.try_get<T>(e)`. Es gibt bereits `transformOf` als Helfer, aber nur für Transform. `getName/getUIText` geben `std::string` by value zurück, und die Lua-Shims machen daraus gleich `.c_str()` (eine Kopie pro Aufruf; bei Namen <16 Zeichen kein Heap, sonst schon). `lua_horizon_setMaterialParam` und `lua_horizon_setUIMaterialParam` sind bis auf den Aufruf identisch (vec4 aus 4 Lua-Zahlen).
- **Lösung**: Ein `template<class T> T* componentOf(HorizonWorld&, uint32_t)` ersetzt die 15 Wiederholungen; eine `readVec4(L, first)`-Funktion für die beiden Material-Shims. Für die String-Getter `const std::string*`/`std::string_view` zurückgeben (Python-Backend nutzt dieselbe API, daher Signaturänderung koordinieren; die Python-Seite liegt außerhalb dieses Slices).
- **Erwarteter Effekt**: ca. 60 Zeilen weniger, weniger Kopien. Kleine Laufzeitwirkung.
- **Risiko / Verifikation**: Gering; die API-Signaturen sind von `HE_Python` genutzt (Cross-Modul). Python- und Lua-Script-Tests.

### SCN-718 – SceneSystems: doppelte Präzipitations-Logik und mehrfaches Suchen der WeatherComponent
- **ID**: SCN-718
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/SceneSystems.cpp:54-96`, `:138-140`, `:184-198`
- **Problem**: Die Berechnung „Regen/Schnee-Menge, isSnow, Cap aus WeatherComponent oder 20000" steht zweimal (in `pushGpuParticleParams` und `pushProfilerSceneCounters`, Letzteres mit `uint32_t cap = 20000; // same default as…`). Die erste `WeatherComponent` wird pro Frame in `tickWorld` (`:139`) und noch einmal in `pushGpuParticleParams` (`:79`) per `view.each() { …; break; }` gesucht. Alle drei Stellen müssen bei einer Änderung gleichzeitig angepasst werden.
- **Lösung**: Eine kleine `struct PrecipState { bool active; bool isSnow; float amount; int cap; float clock; }` plus `PrecipState readPrecip(HorizonWorld&)` und die drei Stellen darauf umstellen (eine Suche pro Frame).
- **Erwarteter Effekt**: Weniger Code, eine View-Suche statt drei.
- **Risiko / Verifikation**: Gering. Wetter-Tests (`test_weather*`).

### SCN-719 – removeComponentForKey/resetComponentForKey: String-Vergleichskette; weitere lineare Binding-Suchen
- **ID**: SCN-719
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SceneSerializer.cpp:2791-2820`, `:2859-2873`, `:3737-3747`
- **Problem**: Die Makros erzeugen eine Kette von 46 `if (key == "…")` pro Aufruf (bis zu 46 String-Vergleiche). Sie wird pro Komponente und Entity im Prefab-Sync aufgerufen. `anyOverrideOn`/`anyOverrideOnRecord` sind lineare Scans über `overrides`.
- **Lösung**: Die X-Macro-Liste zu einer `static const unordered_map<string_view, Handler>` aufbauen (gleiches Makro, anderer Ausdruck), die auch `isKnownComponentKey` und `removeComponentsAbsentFrom` bedient. Zusammen mit SCN-706 umsetzen.
- **Erwarteter Effekt**: gering; Hauptnutzen ist eine Quelle für die Schlüsselliste.
- **Risiko / Verifikation**: Gering; `test_scene_serializer` deckt jeden Key ab.

### SCN-720 – computeTerrainHeightField initialisiert den Vektor doppelt
- **ID**: SCN-720
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/TerrainMeshGenerator.cpp:72-94`
- **Problem**: `std::vector<float> heights(vertCount, 0.0f);` wird sofort nach `heights = tc.sculptHeights;` überschrieben (zwei Allokationen/Initialisierungen plus Kopie von bis zu 4 MB). `generateTerrainMesh` (`:106`) und `ensureHeights` rufen das auf.
- **Lösung**: `if (sculptHeights.size() == vertCount) return tc.sculptHeights;` am Anfang (NRVO/Kopie nur einmal) und erst danach den Nullvektor bzw. fBm-Schleife. Verhalten gleich. Für `generateTerrainMesh` bei vorhandenem Sculpt-Feld könnte die Funktion direkt `const std::vector<float>&` verwenden (Referenz statt Kopie).
- **Erwarteter Effekt**: eine 4-MB-Initialisierung weniger pro Aufruf.
- **Risiko / Verifikation**: Keines.

### SCN-721 – exportComponentText/serializeEntityComponents serialisieren die ganze Entity für eine Komponente
- **ID**: SCN-721
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:3504-3516`, `:3597-3611`, `:3613-3639`
- **Problem**: `exportComponentText` baut per `serializeComponents` alle ca. 47 möglichen Komponenten, nur um einen Schlüssel zu lesen. `serializeEntityComponents` (Collab-Pfad, pro Edit) serialisiert jedes Mal alle Komponenten, auch wenn nur eine geändert wurde. `componentKeyOfText` parst bei geöffnetem Menü jeden Frame den Clipboard-Text voll, wenn er den Envelope-String enthält (Cheap-Reject nur per `find`). `importComponentText` parst ihn danach nochmal.
- **Lösung**: `serializeComponents(registry, entity, std::string_view onlyKey = {})` (Filter pro Block) für `exportComponentText`. Einen Parse-Cache (letzter Text → Schlüssel) in `componentKeyOfText` oder im Aufrufer (`InspectorPanel.cpp:4041`). Kollaborations-Delta (nur geänderte Schlüssel) wäre eine Protokolländerung (außerhalb des Scopes).
- **Erwarteter Effekt**: klein (Editor-Aktion); sinnvoll bei Collab mit vielen Edits.
- **Risiko / Verifikation**: Gering; Clipboard-Tests.

### SCN-722 – adoptLegacyInstance: O(n²) und ganze Entity-Serialisierung pro Record
- **ID**: SCN-722
- **Kategorie**: D
- **Schweregrad**: Niedrig (Legacy-Pfad, einmalig pro alter Instanz)
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:2896-2972`
- **Problem**: Pro Record werden alle Records erneut nach Geschwistern mit gleichem Namen durchsucht (O(R²), jeweils mit `value("name", …)`-String-Kopien), und danach `serializeComponents(registry, e)` komplett. Läuft nur für Instanzen ohne Bindings (Altbestand).
- **Lösung**: Einmal vorab `unordered_map<pair<parentKey,name>, count>` aufbauen. Nur anfassen, wenn alte Szenen messbar langsam sind.
- **Erwarteter Effekt**: nur für alte Szenen mit großen Prefabs.
- **Risiko / Verifikation**: Gering. Legacy-Adoption-Tests.

### SCN-723 – Spline: redundante Kurvenauswertungen in `refineSpan`/`at()`
- **ID**: SCN-723
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/SplineCurve.cpp:32-54`; `SplineGeometry.cpp:93-105`
- **Problem**: `refineSpan` wertet `at(u)` für 3 Prüfpunkte aus und beim Teilen nochmal `at(um)` (identisch mit dem 0.5-Prüfpunkt), und die Kind-Aufrufe bewerten ihre Prüfpunkte neu. `CatmullRomSpan::at` macht pro Aufruf 8 Divisionen, deren Nenner (`t1-t0`, `t2-t1`, …) pro Span konstant sind.
- **Lösung**: Die Kehrwerte im Konstruktor vorberechnen und `at` mit Multiplikationen schreiben (Ergebnis kann sich im letzten Ulp ändern; Tests mit Toleranz prüfen), und den 0.5-Punkt aus der Prüfschleife an `refineSpan` weiterreichen. Ohne Messdaten ist das eine Mikro-Optimierung. Nur umsetzen, wenn Polyline-Erzeugung im Profil auftaucht.
- **Erwarteter Effekt**: gering.
- **Risiko / Verifikation**: Kleine numerische Abweichungen; Spline-Tests mit Toleranz.

### SCN-724 – startWorldScripts fragt `assetType` doppelt pro Entity
- **ID**: SCN-724
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/ScriptContext.cpp:1560-1561` und `:1607`
- **Problem**: `startWorldScripts` prüft `cm.assetType(sc.scriptAssetId) == HorizonCodeClass` und `startEntityScript` prüft es für dieselbe Entity nochmals. Ebenso `active.off(entity)` (einmal in der Schleife, dann nicht mehr in `startEntityScript`, hier unkritisch).
- **Lösung**: Interne Variante `startEntityScriptUnchecked(entity, sc, cm)` für `startWorldScripts`, oder die Schleifenprüfung entfernen und stattdessen `kInvalidInstance` ohne Zählung unterscheiden (gleiche Zähllogik: HorizonCode-Klassen dürfen nicht in `attempted`).
- **Erwarteter Effekt**: ein Hash-Lookup pro Script-Entity beim Start; vernachlässigbar.
- **Risiko / Verifikation**: Gering.

---

## Gelesen

Alle 13 Dateien des Slices wurden vollständig gelesen: ja (SceneSerializer.cpp 1-4562 in Blöcken, SceneSystems.cpp, ScriptApi.cpp, ScriptContext.cpp, SequenceEval.cpp, SequenceSystem.cpp, SplineCurve.cpp, SplineGeometry.cpp, TerrainGenerate.cpp, TerrainHeightmap.cpp, TerrainMeshGenerator.cpp, TerrainPaint.cpp, TerrainSculpt.cpp). Zur Einordnung wurden punktuell gelesen: `EngineApi.h` (Ctx), `ScriptContext.h`, `PrefabInstanceComponent.h`, `TerrainSystem.cpp:375-425`, `OutlinerPanel.cpp:640-675`, `EditorApplication.cpp:13369-13392, 13598-13606`.

## Scene Slice 8 – TerrainSystem, TransformHierarchy, UIInputSystem, UISystem, Water*, Weather* (src/HE_Scene/src)

## Zusammenfassung

Der Slice ist insgesamt sorgfältig gebaut: Transform-Propagation hat Dirty-Scan, Stats, Verify-Modus und Tests; Wasser und Terrain haben eigene Dirty-Rechtecke und Hash-Gates, und Kommentare begründen Entscheidungen. Die Schwachstellen liegen in den Per-Frame-Pfaden, die trotz sauberer Dirty-Logik an anderer Stelle jeden Tick über alle Chunks, Elemente oder Splines laufen und dabei Heap-Container anlegen. Das sind die Terrain-Tessellation, die Chunk-Material-Synchronisation, der UI-Extract und die Wasser-Spline-Synchronisation. Dazu kommen Wiederholungen im Wassercode (FNV-Hash, Punkt-Segment-Distanz, `hasArea` jeweils 3–5-fach) und node-basierte Maps in der Contour-Extraktion. Insgesamt 15 Funde: 2 Hoch, 8 Mittel, 5 Niedrig. Es gibt keine Messdaten; alle Aufwandsschätzungen sind Schätzungen.

---

### SCN-801 – updateTessellation läuft jeden Tick über alle Chunks, auch wenn Tessellation aus ist
- **ID**: SCN-801
- **Kategorie**: D (ECS ohne Dirty-Flag), C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/TerrainSystem.cpp:715-768 (aufgerufen pro Tick aus SceneSystems.cpp:150)
- **Problem**: Vor der Entscheidung, ob ein Terrain überhaupt Tessellation nutzt (`tessFactorOf(tc) <= 1u`, Zeile 779, Default `tessellationFactor = 1`), wird für JEDEN `TerrainChunkComponent` der Welt `HE::worldPositionOf` (Hierarchie-Walk plus Matrixmultiplikationen) berechnet. Außerdem werden pro Chunk ein `unordered_map`-Lookup für den Besitz-Check (750-752) und ein `push_back` in `byTerrain` (`unordered_map<uint32_t, vector<Near>>`, 742) gemacht. Ein 2048er-Terrain hat bis zu 32×32 = 1024 Chunks, und `byTerrain` plus ein Vektor pro Terrain werden jeden Frame neu allokiert. Bei ausgeschalteter Tessellation (Normalfall) ist das reine Verschwendung.
- **Code-Auszug**:
```cpp
for (auto [ce, cc] : reg.view<TerrainChunkComponent>().each())
{
    ...
    const float d = glm::distance(cameraPos, HE::worldPositionOf(world, ce));
    byTerrain[static_cast<uint32_t>(cc.terrain)].push_back({ ce, d });
}
```
- **Lösung**: (1) Erste Schleife über die Terrains (`view<TerrainComponent>`, wenige Einträge) und für jedes `active = tessFactorOf(tc) > 1 && D > 0` bestimmen. (2) Sind alle Terrains inaktiv, wird nur noch die Chunk-Liste aufgeräumt, die noch `cc.tessActive` oder `tessMeshId != UUID{}` hat. Dafür gibt es `g_tessMeshes` (leer im Normalfall); ist `g_tessMeshes` für die Welt leer und `active` leer, früh `return`. (3) Distanz erst berechnen, wenn das Terrain des Chunks aktiv ist. (4) `byTerrain` nur mit aktiven Terrains füllen (oder `thread_local`/Member-Scratch wiederverwenden).
- **Erwarteter Effekt**: Schätzung: bei Tessellation aus von O(Chunks) mit ~1000 Hierarchie-Walks pro Frame auf O(Terrains) (~0). Bei aktiver Tessellation bleibt der Aufwand, sinkt aber um die Allokationen.
- **Risiko / Verifikation**: Das Aufräumen von Chunks mit Fremd-`tessMeshId` (748-762, „copied component“) muss erhalten bleiben. Dafür die bestehenden Terrain-Tessellationstests in `tests/` laufen lassen (`grep -l tessellation tests/`); ein Test mit kopiertem Chunk ergänzen, falls keiner existiert.

### SCN-802 – Chunk-Material-Sync: O(Terrains × Chunks) jeden Tick, ungated
- **ID**: SCN-802
- **Kategorie**: D, C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/TerrainSystem.cpp:431-469
- **Problem**: Der Kommentar sagt ausdrücklich „Re-synced every tick (NOT gated on tc.dirty)“. Dabei wird pro Terrain (a) das `MaterialComponent` des Terrains per Wert kopiert (`const MaterialComponent src = ...`, inklusive des `paramOverrides`-Vektors mit String-Namen, also Heap-Kopien jeden Tick), (b) die ganze `view<TerrainChunkComponent>` durchlaufen und mit `cc.terrain != te` gefiltert. Das sind T×C Iterationen statt C. Dazu kommen pro Chunk `try_get<MaterialComponent>` und `same()` mit String-Vergleich je Override. Zusätzlich lebt `needMat` als Heap-Vektor pro Terrain und Tick.
- **Code-Auszug**:
```cpp
const MaterialComponent src = reg.get<MaterialComponent>(te);
...
for (auto [ce, cc] : reg.view<TerrainChunkComponent>().each())
{
    if (cc.terrain != te) continue;
    auto* dst = reg.try_get<MaterialComponent>(ce);
    ...
    if (same(*dst)) continue;
```
- **Lösung**: (1) Das Parent-Material nicht kopieren, sondern per `const MaterialComponent&` lesen. Das nachfolgende `emplace_or_replace` auf Chunks kann die Pool-Referenz invalidieren, deshalb zuerst alle Chunk-Updates (`dst`-Schreibzugriffe laufen auf einem anderen Pool-Eintrag, sind sicher) und erst danach die `needMat`-Emplaces ausführen, oder `src` nur im seltenen `needMat`-Fall kopieren. (2) Eine einzige Chunk-Schleife über die View, in der `cc.terrain` per kleinem Lookup (Vektor `(terrainEntity, const MaterialComponent*)`, T ist klein) auf die Quelle abgebildet wird. (3) Weiter reduzieren: Terrain-Material mit einem Hash/„revision“ der Quelle (materialAssetId + overrides) vergleichen und nur bei Änderung über die Chunks laufen. Dafür braucht das `MaterialComponent` einen Änderungsmarker, den Editor-Schreiber setzen; ohne den bleibt (2) der sichere Schritt.
- **Erwarteter Effekt**: Schätzung: (1)+(2) entfernen den T-fachen Faktor und die Heap-Kopien. Bei 1 Terrain × 1024 Chunks bleiben ~1000 `try_get`+Vergleiche pro Frame; (3) würde das auf ~0 bringen.
- **Risiko / Verifikation**: Das Verhalten „Material-Swap und Param-Overrides erreichen die Chunks ohne Rebuild“ muss erhalten bleiben (bestehender Test zum Landscape-Material suchen: `grep -ln "kDefaultTerrainMaterialId" tests/`). (3) hängt von zuverlässigem Dirty-Setzen aller Material-Schreiber (Inspector, MCP, Skript-API) ab; deshalb erst nach (1)+(2) und mit Messung.

### SCN-803 – updateTerrains: pro Dirty-Frame 4-MB-Höhenfeld-Kopie, Vektor-von-Vektoren und volle Chunk-View
- **ID**: SCN-803
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/TerrainSystem.cpp:504-535, 821-825; TerrainMeshGenerator.cpp:72-94 (computeTerrainHeightField)
- **Problem**: (a) Bei gesculptetem Terrain kopiert `computeTerrainHeightField` das gesamte `sculptHeights` (`heights = tc.sculptHeights`), bei 1025² sind das ~4 MB pro Brush-Frame, obwohl `field` nur gelesen wird (`const std::vector<float> field`). Dasselbe geschieht in `updateTessellation` (823). (b) `byCoord` ist ein `vector<vector<entity>>` mit `chunksPerSide²` (bis 1024) einzelnen Vektoren, die pro Dirty-Terrain und Frame allokiert werden, obwohl nur der erste Eintrag je Zelle benutzt wird (`byCoord[key].front()`). (c) Für jedes Dirty-Terrain läuft die komplette `view<TerrainChunkComponent>` (O(alle Chunks aller Terrains)). (d) Beim Löschen/Neubau werden alle LODs jedes Chunks generiert (`numLODs`× `generateTerrainChunkMesh`), das ist hier nötig, aber der Schritt (a) ist es nicht.
- **Code-Auszug**:
```cpp
const std::vector<float> field = computeTerrainHeightField(tc);
...
std::vector<std::vector<entt::entity>> byCoord(
    static_cast<size_t>(g.chunksPerSide) * g.chunksPerSide);
```
- **Lösung**: (a) Eine Variante `const std::vector<float>& heightFieldRef(const TerrainComponent&, std::vector<float>& scratch)` die bei `sculptHeights.size() == res*res` eine Referenz auf `tc.sculptHeights` liefert und nur sonst `scratch` füllt. Die Konsumenten nehmen schon `const std::vector<float>&`. Achtung: während `buildChunk` darf `tc.sculptHeights` nicht verändert werden (tut es nicht). (b) `std::vector<entt::entity> byCoord(n, entt::null)`; ein zweiter Treffer derselben Zelle (Duplikat) kommt in `toDestroy`. Das ändert das Verhalten nur bei Duplikaten, die bisher stehen blieben (Duplikat-Chunk wurde nie gelöscht); wenn das erhalten bleiben soll, Duplikate ignorieren statt löschen. (c) Chunks pro Terrain über die Hierarchie (`HierarchyComponent.children` des Terrains) statt View suchen.
- **Erwarteter Effekt**: Schätzung: (a) spart eine 4-MB-memcpy (~0,3–1 ms) pro Brush-Frame bei 1025²; (b) bis zu 1024 Allokationen weniger pro Frame.
- **Risiko / Verifikation**: (a) berührt TerrainMeshGenerator (anderes Modul/Slice). Tests mit Sculpt und Region-Rebuild laufen lassen (`grep -l "regionDirty" tests/`). Bei (b) das Duplikat-Verhalten explizit festlegen.

### SCN-804 – UISystem::extract: Layout jeden Frame, ancestors-Walks dreifach pro Element, mal Canvas-Anzahl
- **ID**: SCN-804
- **Kategorie**: D (kein Dirty-Flag), C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/UISystem.cpp:44-54, 87-128, 130-239; aufgerufen aus RenderExtractor.cpp:1586
- **Problem**: Pro aktivem Canvas wird die gesamte `view<UIElementComponent>` iterativ durchlaufen (161) und für jedes Element `owningCanvas` (Hierarchie-Walk mit `try_get` + `all_of` je Hop), `computeScreenRect` (rekursiv, pro Ebene drei `try_get/all_of`) und `nestingDepth` (nochmals ein Walk) aufgerufen. Das ergibt O(Canvases × Elemente × Tiefe) und bei der Rekursion für jedes Kind das Neuberechnen der kompletten Elternkette. Das Layout ist unabhängig von Element-Änderungen jeden Frame neu. `out` wird ohne `reserve` befüllt, danach `stable_sort` über alle Objekte. Der Text-Pfad erzeugt Glyph-Quads je Zeichen jeden Frame (`emitUITextGlyphs`, außerhalb des Slice, nicht gelesen).
- **Code-Auszug**:
```cpp
for (auto [elemEnt, elem] : reg.view<UIElementComponent>().each())
{
    const entt::entity owner = owningCanvas(reg, elemEnt);
    ...
    if (!computeScreenRect(reg, elemEnt, vpWidth, vpHeight, scaleX, scaleY, screenPos, screenSize))
        continue;
    const int sortLayer = HE::uiSortKey(elem.layer, nestingDepth(reg, elemEnt));
```
- **Lösung**: Stufe 1 (verhaltensgleich): Ein Durchlauf über die Elemente; pro Element einmal die Kette hochgehen und dabei Canvas, Tiefe und Rect gemeinsam bestimmen (z. B. eine `resolve(e)`-Funktion mit kleinem Cache `unordered_map<entity, Resolved>`/Vektor nach Entity-Index pro Aufruf, damit Kinder das Eltern-Rect wiederverwenden); der Canvas-Filter wird zur Zuordnung `owner → Canvas`, nicht zu einer Schleife über alle Canvases. Stufe 2: `out.reserve(letzte Größe)`. Stufe 3 (nur nach Messung): Layout-Cache mit Dirty-Flag auf `UIElementComponent` und Viewport-Größe.
- **Erwarteter Effekt**: Schätzung: Stufe 1 reduziert die Hierarchie-Zugriffe von ~3·Tiefe auf ~1·Tiefe pro Element und entfernt den Canvas-Faktor. Absolut nur spürbar bei UIs mit mehreren hundert Elementen.
- **Risiko / Verifikation**: Reihenfolge der Ausgabe (stabile Sortierung nach `layer`) und das Fallback-Canvas-Verhalten (`owner == null` → erster aktiver Canvas, Element unter inaktivem Canvas zeichnet nirgends) müssen exakt erhalten bleiben. `tests/test_ui_*` bzw. Tests zu mehreren Canvases laufen lassen; ein Test mit zwei Canvases (Doppel-Geometrie-Regression, siehe Kommentar 135-144) sollte existieren.

### SCN-805 – UIInputSystem: pro Frame `unordered_set`, doppelte Hierarchie-Walks, Button-Schleife schreibt immer
- **ID**: SCN-805
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/UIInputSystem.cpp:38-95; UIInputSystem.h:25 (`std::unordered_set<uint32_t> hovered`)
- **Problem**: Höchstens ein Element ist „hovered“ (Kommentar Zeile 63), trotzdem ist der Zustand ein `std::unordered_set<uint32_t>`; pro Frame wird ein zweites Set `nowHovered` gebaut (Heap-Node bei `insert`), dann `std::move`. Für jedes Element mit Button/Script ruft die Schleife `computeScreenRect` (rekursiv) und, bei Treffer, `nestingDepth`. Das passiert in jedem Frame, auch wenn die Maus sich nicht bewegt und sich nichts geändert hat. Die Button-State-Schleife (85-91) läuft über alle Buttons und schreibt `btn.state` unbedingt.
- **Code-Auszug**:
```cpp
std::unordered_set<uint32_t> nowHovered;
if (top != 0) nowHovered.insert(top);
for (uint32_t e : state.hovered)
    if (!nowHovered.count(e))
        outEvents.push_back({ e, PointerEvent::Type::HoverExit });
```
- **Lösung**: `state.hovered` zu `uint32_t hovered = 0;` ändern (0 = keiner, `top == 0` benutzt dieselbe Konvention). Hover-Wechsel: `if (top != state.hovered) { if (state.hovered) push(Exit); if (top) push(Enter); }`. Den Button-Loop gleich behalten, aber nur schreiben, wenn sich der Wert ändert (damit etwaige Änderungs-Beobachter/Undo-Snapshots ruhig bleiben). Optional: die Hit-Test-Schleife nur ausführen, wenn sich Mausposition, Viewport oder Layout geändert haben (heikel, da Layout nicht dirty-markiert ist; deshalb nur nach Messung).
- **Erwarteter Effekt**: Schätzung: eine Heap-Allokation pro Frame weniger, deutlich einfachere Hover-Logik. Hit-Test bleibt O(Elemente × Tiefe).
- **Risiko / Verifikation**: `InputState` ist öffentlich (Header, im Editor/Game verwendet); `grep -rn "\.hovered" src/ tests/` auf Verwender prüfen. Reihenfolge der Events (Exit vor Enter) bleibt erhalten. UI-Input-Tests (`grep -l UIInputSystem tests/`) prüfen.

### SCN-806 – Wasser-Spline-Sync: jeden Tick Vektor mit UUID-Lookups aller Splines, Byte-FNV über Control-Points, 2× worldMatrixOf je Lake
- **ID**: SCN-806
- **Kategorie**: D, C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/WaterLake.cpp:156-175, 835-883; aufgerufen aus TerrainSystem.cpp:479 (jeder `updateTerrains`-Aufruf, auch die vielen Editor-Aufrufe)
- **Problem**: Gibt es mindestens einen Spline-See, baut `syncSplines` jeden Aufruf `splines` (`vector<pair<UUID, Entity>>`, ohne `reserve`) mit `world.entityId(se)` für JEDE `SplineComponent` der Welt, obwohl nur wenige Splines Lakes sind. Danach wird je See `fingerprint()` gerechnet: zwei `worldMatrixOf` (Hierarchie-Walks) und ein FNV-1a, der BYTEweise über alle Control-Points läuft (`hashBytes`, 1 Multiplikation pro Byte; 12 B pro Punkt). Die lineare Suche nach der Spline über `splines` ist O(Seen × Splines).
- **Code-Auszug**:
```cpp
std::vector<std::pair<HE::UUID, Entity>> splines;
for (auto [se, sc] : reg.view<SplineComponent>().each())
    splines.emplace_back(world.entityId(se), se);
...
h = hashBytes(h, sc.controlPoints.data(), sc.controlPoints.size() * sizeof(glm::vec3));
```
- **Lösung**: (1) Splines nicht vorab sammeln, sondern pro See die Spline über `world`-UUID-Index auflösen, falls `HorizonWorld` einen UUID→Entity-Lookup hat (`grep -n "findEntity\|entityById" HorizonWorld.h`). Sonst `splines` nur mit Einträgen füllen, deren UUID in einer kleinen Menge der `sourceSpline`-Werte vorkommt (Menge aus den Bodies, typischerweise 1–5). (2) FNV auf 64-Bit-Wörter umstellen oder einen schnellen Hash (z. B. 8 Byte pro Schritt, oder `std::hash`-Kombination über Floats); der Hash ist runtime-only (`syncKey`, nicht serialisiert; `sanitize` setzt ihn auf 0), die Hash-Werte dürfen sich also ändern. (3) Die Transform-Matrizen nur dann abfragen, wenn der Spline oder seine Vorfahren dirty/neu propagiert sind (z. B. `TransformComponent` des Splines/Terrains hat `worldMatrix` aus der Propagation, `worldMatrixOf` ist die leichtere Variante des selben Ergebnisses).
- **Erwarteter Effekt**: Schätzung: ohne Seen null Kosten (früher Ausstieg bleibt). Mit einem See und einer großen Welt mit vielen Splines: von O(Splines) Lookups pro Tick auf O(Seen). Hash: ~8× weniger Multiplikationen.
- **Risiko / Verifikation**: Die Semantik „`syncKey == 0` → erster Blick nur merken“ nicht ändern. Tests zum Lake-Sync (`grep -ln "syncSplines\|lake::" tests/`) laufen lassen.

### SCN-807 – Wasseroberfläche: pro Tick Maps/Vektoren auch im Ruhezustand, Hash doppelt berechnet, volle Rasterscans
- **ID**: SCN-807
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/WaterSurface.cpp:189-225, 227, 268-270, 301-348; WaterMesh.cpp:461-473, 495
- **Problem**: (a) Der Ruhezustand („nearly every tick“, Kommentar 214-216) wird erst NACH dem Aufbau von `byBody` (`unordered_map`, 189-195) erkannt, dazu kommen pro Tick `byTerrain` (`unordered_map`), `orphans`, `terrains` und `mine` (Vektoren, 324-345). (b) Beim Rebuild wird `surfaceHash(lattice, ...)` zweimal berechnet: in `Pass::terrain` (270) und erneut in `buildSurface(tc, lattice, ...)` (WaterMesh.cpp:495). Der Hash läuft byteweise FNV über `l.v` und `l.g` (Gitter groß, 1 Multiplikation pro Byte). (c) `allBodyCells(tc)` (227) scannt bei jedem nicht-ruhenden Tick das ganze Raster (`res²`, bis `kMaxRes`), obwohl `dirtyCells` bekannt ist; in einem Brush-Drag ist das jeden Frame der Fall.
- **Code-Auszug**:
```cpp
const uint64_t hash = surfaceHash(lattice, body.level, opt);   // WaterSurface.cpp:270
...
out.hash = surfaceHash(lattice, level, opt);                    // WaterMesh.cpp:495
```
- **Lösung**: (a) Ruhe-Check vor dem Sammeln: wenn `!f.dirty` und für alle `Body`s im Feld schon eine Surface mit passendem `paramsKey`/`level` existiert, früh raus; praktisch: Zähler/Revisions-Vergleich pro Terrain (`WaterSurfaceComponent` je Terrain + `f.revision`) und `WaterSurface::update` erst nach `view<WaterSurfaceComponent>` bei Bedarf. Die Scratch-Container (`byTerrain`, `terrains`) in einen `thread_local`/statischen Scratch auslagern, oder `smallvector`. (b) `buildSurface(...)` bekommt den vorberechneten Hash als Parameter (oder `out.hash` aus dem Aufrufer) – verhaltensgleich. (c) Extents pro Body cachen (z. B. im `Field` mit `revision` als Schlüssel; `WaterIndex` macht das für die Bounds schon) oder nur innerhalb von `dirtyCells ∪ built` neu bestimmen.
- **Erwarteter Effekt**: Schätzung: (a) 2–4 Heap-Allokationen weniger pro Frame; (b) halbiert die Hash-Kosten pro Rebuild (bei 512²-Lattice ~1 MB gehashte Bytes → grob 1–2 ms, **messen**); (c) bei 1024² ~1M Zellen weniger pro Frame beim Brushen.
- **Risiko / Verifikation**: (b) trivial verhaltensgleich. (c) Extent-Cache muss bei jeder Zellenänderung (Delta/Undo, `setResolution`, `decodeCells`) ungültig werden – `revision` wird bei allen diesen Pfaden erhöht (`markRect/markAll/++revision`), aber `sanitize`/`decodeCells` prüfen. Wasser-Tests (`tests/test_water*`) ausführen.

### SCN-808 – extractContours: node-basierte Maps für dicht indizierte Kanten
- **ID**: SCN-808
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/WaterMesh.cpp:279-373
- **Problem**: `points` und `next` sind `unordered_map<int64_t,…>` und `seen` ein `unordered_set<int64_t>`, obwohl die Schlüssel dicht sind (`(j*w+i)*2 + {0,1}` < `2*w*h`). Pro Crossing ein Heap-Node, danach wird `starts` aus der Map kopiert und sortiert („the same lake always gives the same mesh“). `points[cur]` im Ringaufbau nutzt `operator[]`. Das läuft bei jedem Wasser-Rebuild, bei Lake-Drags jeden Frame, und in `largestPiece`, `keepUnderSheet`/`shapeBank` (indirekt über `buildLattice`+`shoreSegments`).
- **Code-Auszug**:
```cpp
std::unordered_map<int64_t, glm::dvec2> points;
std::unordered_map<int64_t, int64_t>    next;
...
std::sort(starts.begin(), starts.end());
```
- **Lösung**: `std::vector<glm::dvec2> points(2*w*h)`, `std::vector<int32_t> next(2*w*h, -1)` (oder `int64_t`) und `std::vector<uint8_t> seen`. Die Startkanten ergeben sich beim aufsteigenden Index-Durchlauf `for key in 0..2*w*h: if next[key] >= 0`, das ist exakt die sortierte Reihenfolge von `starts`, der Sort entfällt. Speicher: `2*w*h*(16+4+1)` Byte, bei 1000×1000-Lattice ~42 MB, deshalb für große Lattices entweder auf die Bounding-Box der Konturen beschränken oder bei `w*h` über einer Schwelle bei der Map bleiben. **Messen**, welcher der beiden Wege gewinnt.
- **Erwarteter Effekt**: Schätzung: 3–10× schneller bei kleinen/mittleren Seen (kein Node-Alloc pro Crossing); bei sehr großen, dünnen Lattices ggf. mehr Speicher als die Maps.
- **Risiko / Verifikation**: Ergebnisreihenfolge (Ringe und Startpunkte) muss bitgleich bleiben: Golden-Tests im Wasserbereich (`grep -ln "extractContours" tests/`); der Schlüssel→Reihenfolge-Beweis oben gilt, weil `starts` genau die Schlüssel mit `next`-Eintrag aufsteigend sortiert.

### SCN-809 – reshape(): zwei Polygon-Rasterisierungen und zwei volle Raster-Scans pro Drag-Frame
- **ID**: SCN-809
- **Kategorie**: D, C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/WaterLake.cpp:399-482; WaterField.cpp:534-609 (rasterizePolygon)
- **Problem**: Pro Frame, in dem ein Spline-Punkt gezogen wird, rasterisiert `reshape` das neue UND das alte Polygon (`newR`, `oldR`, 404-408; das alte Raster war im vorigen Frame als `newR` schon berechnet). Dazu `seen(n)` (res² Byte, zero-initialisiert) und eine Schleife über alle `n` Zellen nach `owner == id` (424-431), danach zwei volle Schleifen (472-474 nur über `before`, ok). `rasterizePolygon` prüft pro Zeile × 8 Subzeilen ALLE Polygonkanten (`for i in p.size()`), also O(Zeilen·8·Kanten) statt Active-Edge-Tabelle; für 256 Kantenpunkte und 500 Zeilen ~1 Mio. Kantentests je Raster, plus `std::sort` je Subzeile.
- **Code-Auszug**:
```cpp
const Raster oldR = b->polygon.empty() ? Raster{}
                                       : rasterizePolygon(b->polygon, tc.sizeX, tc.sizeZ, f.res);
...
std::vector<uint8_t> seen(n, 0);
for (size_t i = 0; i < n; ++i) { if (f.owner[i] != id) continue; ...
```
- **Lösung**: (1) Active-Edge-Liste in `rasterizePolygon` (Kanten nach min-y sortieren, pro Subzeile nur aktive Kanten); das Ergebnis `xs` bleibt identisch. (2) In `reshape` die Zellsuche auf die Bounding-Box von `oldR ∪ newR ∪ bodyCells` beschränken (bodyCells braucht selbst einen Scan; deshalb ggf. die Extents aus SCN-807 teilen) und `seen` über die Box statt über das ganze Raster führen. (3) Das alte Raster im `Body` (oder in `State` pro Lake) cachen; Speicher = Raster-Rechteck des Sees.
- **Erwarteter Effekt**: Schätzung: (1) 5–20× bei komplexen Polygonen; (2) spart die ~1M-Zellen-Schleife und den 1-MB-Alloc je Drag-Frame bei res 1024.
- **Risiko / Verifikation**: Rasterergebnis muss bit-identisch bleiben (Golden-Test für `rasterizePolygon` mit konkaven Polygonen/Löchern ergänzen, bevor (1) geändert wird). Die Fließkomma-Reihenfolge der `acc`-Addition ist pro Subzeile unverändert, solange `xs` sortiert bleibt.

### SCN-810 – Wasserpinsel: keepUnderSheet/shapeBank bauen Lattice und Distanzfelder pro Dab neu
- **ID**: SCN-810
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/WaterBrush.cpp:57-93, 116-286, 333-381
- **Problem**: Pro Dab (Brush-Frame) mit Dig: `saveRegion` (Heap-Kopie), `keepUnderSheet` mit `while (moved)`-Schleife, die in jeder Iteration die Lattice inklusive `terrainHeightAt`-Sampling neu baut (`buildLattice` 77), dann `shapeBank` mit bis zu 3 Pässen, je Pass wieder `buildLattice`, `shoreSegments`, zwei Vektoren `nearest`/`best` (Heap) über die Box, zwei Sweeps. Die Zahl der `keepUnderSheet`-Iterationen ist nicht beschränkt (nur „ends because it only puts back“). Der Code ist korrekt dokumentiert, aber die Kosten wachsen mit Radius²·Iterationen.
- **Code-Auszug**:
```cpp
for (bool moved = true; moved; )
{
    moved = false;
    Lattice lattice;
    const bool sheet = cells.valid() && buildLattice(tc, body, cells, lattice, clip);
```
- **Lösung**: Zuerst **messen** (Profiler-Scope um `dab()`; Brush-Radius groß, Raster 1024). Wenn `keepUnderSheet` häufig >2 Iterationen braucht: nur die vom Zurücksetzen betroffenen Gitterecken der Lattice neu bewerten (`g`-Werte stehen über `terrainHeightAt`; ein zurückgesetzter Vertex ändert nur die 4 umliegenden Terrain-Zellen), statt alles neu zu bauen. Scratch-Vektoren (`nearest`, `best`) aus der Pass-Schleife heben und mit `assign` wiederverwenden.
- **Erwarteter Effekt**: Unsicher; ohne Messung kein Wert. Die Scratch-Wiederverwendung spart bis zu 6 Allokationen pro Dab.
- **Risiko / Verifikation**: Das Ergebnis der Iteration (Fixpunkt) darf sich nicht ändern. Die Brush-Tests des Wassers (`grep -ln "water::brush" tests/`) sind der Maßstab; ein Test mit bewusst mehreren Iterationen (steiler Hang) sichern.

### SCN-811 – Dreifach duplizierte Hilfsfunktionen im Wassercode
- **ID**: SCN-811
- **Kategorie**: A, F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: WaterBrush.cpp:17, 136-142; WaterField.cpp:14-17, 91; WaterLake.cpp:20-25, 94-102, 156-161; WaterMesh.cpp:18-21, 135-141, 151-157; WaterSurface.cpp:35-38, 46-52 (alle src/HE_Scene/src)
- **Problem**: `hasArea(tc)` existiert fünfmal (einmal als `terrainHasArea`), `finite(float)` zweimal, der FNV-1a-Hash dreimal (`hashBytes` in WaterLake, `fnv` in WaterMesh, `mix` in WaterSurface; identische Konstanten), die Punkt-zu-Segment-Distanz dreimal (`distance`-Lambda in shapeBank, `distToSegment` in WaterLake, `segmentDistance` in WaterMesh). `Touched` (WaterField.cpp:64) und `Changes` (WaterLake.cpp:45) sind dieselbe Struktur; der Kommentar bestätigt das.
- **Code-Auszug**:
```cpp
uint64_t hashBytes(uint64_t h, const void* data, size_t n) { ... h ^= p[i]; h *= 1099511628211ull; }  // WaterLake
uint64_t fnv(uint64_t h, const void* data, size_t bytes) { ... h ^= p[i]; h *= 1099511628211ull; }     // WaterMesh
uint64_t mix(uint64_t h, const void* data, size_t bytes) { ... h ^= p[i]; h *= 1099511628211ull; }     // WaterSurface
```
- **Lösung**: Ein interner Header `src/HE_Scene/src/WaterDetail.h` (nicht installiert, `namespace HE::water::detail`) mit `inline` `hasArea`, `finite`, `fnv1a`, `segmentDistance`, `CellBox` (= Touched/Changes). Ersetzen der Kopien. Gleichzeitig bietet sich an, den Hash dort einmal zu verbessern (SCN-806/807).
- **Erwarteter Effekt**: Weniger Code (~60 Zeilen), eine Stelle für Hash-Änderungen. Keine Laufzeitänderung.
- **Risiko / Verifikation**: Rein mechanisch. `hasArea` hat in `WaterBrush.cpp:17` eine schwächere Variante (nur `> 0`, ohne `isfinite`); beim Vereinheitlichen prüfen, ob das Verhalten für NaN/Inf-Größen relevant ist (dann bewusst die strengere Variante nehmen und Test ergänzen).

### SCN-812 – 64K-Tabellen auf dem Heap für Body-Ids (pruneEmptyBodies, ensureBounds, sanitize)
- **ID**: SCN-812
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: WaterField.cpp:438 (`std::vector<bool> used(65536, false)`), 973; WaterQuery.cpp:97 (`std::vector<uint8_t> used(65536, 0)`)
- **Problem**: `pruneEmptyBodies` wird nach jedem `removeCircle`/`removePolygon` mit Änderung aufgerufen, also pro Radier-Dab; es legt jedes Mal eine 8-KB-Bitmenge an und scannt danach das ganze Raster. `ensureBounds` legt bei jeder ungültigen Cache-Stelle 64 KB an und nullt sie. Die Id-Obergrenze ist durch `bodies.size()`/`nextBodyId` meist klein.
- **Code-Auszug**:
```cpp
std::vector<bool> used(65536, false);
if (f.allocated())
    for (size_t i = 0; i < f.owner.size(); ++i)
        if (f.coverage[i] > 0) used[f.owner[i]] = true;
```
- **Lösung**: `used` auf die Anzahl der Bodies dimensionieren (Slot-Tabelle wie in `allBodyCells`: `slot[id]`), oder `std::bitset<65536>` auf dem Stack (8 KB) anlegen, damit kein Heap-Alloc entsteht. Beim Radieren zusätzlich nur das Rechteck des Dabs scannen (Bodies, die der Dab nicht berührt hat, sind nicht neu leer geworden).
- **Erwarteter Effekt**: Schätzung: klein; einige µs pro Dab bis 1024². Nur Hot-Path-Hygiene.
- **Risiko / Verifikation**: Das „nur Rechteck scannen“-Argument stimmt nur, wenn vorher keine leeren Bodies existierten (Aufräumen alter leerer Bodies gehört bisher zu dieser Funktion). Deshalb zuerst nur den Container tauschen.

### SCN-813 – WaterIndex::refresh/WaterIndex::build: pro Aufruf Entry-Vektor, Matrixinverse, quadratische Cache-Suche
- **ID**: SCN-813
- **Kategorie**: C, D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/WaterQuery.cpp:33-86; Aufrufer PhysicsWorld.cpp:3795 (jeder Wasser-Step), RenderExtractor.cpp:1202 (jeder Frame), EngineApi.cpp:1578
- **Problem**: Jeder Aufruf baut `std::vector<Entry> next` (Entry ~ 2×mat4 + mat3 + Cache, ~250 B) neu auf, berechnet `worldMatrixOf` und `glm::inverse(mat4)` je Wasser-Terrain, sucht den alten Eintrag linear (`for old in m_entries`, O(E²)) und sortiert. Pro Frame wird das im RenderExtractor mit einem frisch gebauten Index nochmals gemacht (nur für eine einzige Frage), zusätzlich zum Physik-Step.
- **Code-Auszug**:
```cpp
std::vector<Entry> next;
for (auto [entity, tc] : reg.view<TerrainComponent>().each())
{ ... e.localToWorld = HE::worldMatrixOf(world, entity); ... e.worldToLocal = glm::inverse(e.localToWorld);
```
- **Lösung**: `next` durch In-place-Update von `m_entries` ersetzen (Einträge anhand Entity wiederverwenden, Reihenfolge beibehalten, `reserve` auf alte Größe). Die Matrixinverse nur bei geänderter `localToWorld` neu berechnen (Bitvergleich mit dem alten Eintrag). Für den Underwater-Check einen eigenen leichten Pfad ohne vollständigen Index (nur Terrains mit Wasser, kein Sort, keine Cache-Felder). Wirkung ist klein bei 1–3 Terrains; wichtig wird das nur in Szenen mit sehr vielen Landscapes.
- **Erwarteter Effekt**: Unsicher / messen. Schätzung: <10 µs pro Frame bei 1 Terrain.
- **Risiko / Verifikation**: `Entry`-Cache-Schlüssel (`keyGrid` = Datenzeiger des Raster-Vektors) bleibt unverändert. Tests `test_underwater_camera.cpp`, Wasser-Physik.

### SCN-814 – WeatherAudio::update: String-Kopie und Bus-Lookup jeden Frame, mehrere unabhängige "erstes Weather-Component"-Suchen
- **ID**: SCN-814
- **Kategorie**: C, A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/WeatherAudio.cpp:30-33, 217, 227; WeatherSystem.cpp:83-85; SceneSystems.cpp:140
- **Problem**: `busFor` liefert pro Frame einen `std::string` (Kopie des Bus-Namens, bei längeren Namen Heap-Alloc) und macht einen `unordered_map<string,…>::count` (Hash über den Namen) auf dem Audio-Engine-Impl, obwohl sich `soundBus` selten ändert. `engine.isPlaying(handle)` wird je Layer (4×) und je Thunder-Stimme jeden Frame gerufen (Map-Lookup in `m_impl->sounds`; ob dies gegen den Audio-Thread synchronisiert wird, ist in AudioEngine.cpp zu prüfen, hier unsicher). Dasselbe Muster `for (auto [e, wc] : view<WeatherComponent>().each()) { w = &wc; break; }` steht in WeatherAudio, WeatherSystem und SceneSystems (3×).
- **Code-Auszug**:
```cpp
const std::string bus = busFor(engine, w->soundBus);
...
WeatherComponent* w = nullptr;
for (auto [e, wc] : reg.view<WeatherComponent>().each()) { w = &wc; break; }
```
- **Lösung**: `busFor` zu `const std::string& busFor(...)` mit Rückgabe von `wanted` oder einer `static const std::string kNone` machen (keine Kopie). Bus-Existenz nur prüfen, wenn der Voice-Start ansteht (`bed.handle == 0` oder neue Thunder-Stimme), nicht jeden Frame. Ein Helfer `WeatherComponent* firstWeather(entt::registry&)` (z. B. in WeatherSystem.h) ersetzt die drei Schleifen; er kann `view.front()`/`view.begin()` nutzen.
- **Erwarteter Effekt**: Schätzung: klein (eine String-Kopie und ein Hash-Lookup pro Frame). Vereinfachung ist der Hauptnutzen.
- **Risiko / Verifikation**: Referenz auf `static const std::string` ist thread-sicher ab C++11. `hasBus` darf nicht vor dem ersten Voice-Start zwischengespeichert werden, sonst bleibt ein später erzeugter Bus unbeachtet; deshalb nur bei Start prüfen, nicht cachen.

### SCN-815 – Terrain-Diagnose-Logging im Rebuild-Pfad und globale Welt-Zeiger-Maps
- **ID**: SCN-815
- **Kategorie**: F, A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: TerrainSystem.cpp:126-127, 157, 203-204, 591-615; WaterSurface.cpp:33; TransformHierarchy.cpp:131-140
- **Problem**: (a) Der „Diagnostic“-Block läuft bei jedem `rebuildAll` (also bei jedem vollen Rebuild) und sonst im 1-s-Takt: `std::minmax_element` über das gesamte Höhenfeld (bis 1M Floats) nur für eine INFO-Zeile, die per `std::to_string`-Kette gebaut wird. Außerdem ist `s_lastLog` eine einzige Funktions-Static für alle Terrains. Er ist als Fehlersuch-Diagnose gedacht (Kommentar „A landscape that stays flat after a sculpt shows up here“). (b) Vier Datei-Statics (`g_pendingTerrainColliders`, `g_tessMeshes`, `g_displacementGrey`, `g_owned`) sind nach `const HorizonWorld*` geschlüsselt. Dagegen speichert `TransformHierarchy.cpp` seinen Zustand im `registry.ctx()` (`stateOf`), so verschwindet er mit der Welt. `g_displacementGrey` wächst ohne Räumung mit jeder genutzten Displacement-Textur (Greymap jeder Textur bleibt im Speicher).
- **Code-Auszug**:
```cpp
static auto s_lastLog = std::chrono::steady_clock::time_point{};
...
const auto mm = std::minmax_element(field.begin(), field.end());
```
- **Lösung**: (a) Den Diagnoseblock hinter `HE_LOG_ENABLED(INFO)`/`HE_LOG_DEBUG` legen oder auf DEBUG senken und das Min/Max nur dann berechnen, wenn der Log-Level aktiv ist; `s_lastLog` pro Terrain führen oder entfernen. (b) Die Maps in einen `struct TerrainState` im `reg.ctx()` überführen (wie `TransformHierarchy`); damit entfallen Welt-Zeiger-Schlüssel und das Aufräumen bei Weltende. `g_displacementGrey` an das ContentManager-Ereignis „Asset entladen“ koppeln oder auf wenige Einträge begrenzen.
- **Erwarteter Effekt**: Schätzung: spart eine Feldüberlauf-Schleife (~1M Vergleiche) pro vollem Rebuild; (b) beseitigt die Gefahr, dass eine neue Welt an derselben Adresse alte Einträge erbt.
- **Risiko / Verifikation**: Log-Text ist von Tests/MCP-Diagnose evtl. erwartet (`grep -rn "TerrainSystem: rebuilt" src tests docs`). (b) benötigt Test für „zweite Welt“ (Kommentar 115-117 beschreibt die Anforderung bereits).

---

## Gelesen
Alle 14 Dateien des Slices (TerrainSystem, TransformHierarchy, UIInputSystem, UISystem, WaterBrush, WaterField, WaterLake, WaterMesh, WaterQuery, WaterRipples, WaterSurface, WaterTriangulate, WeatherAudio, WeatherSystem) vollständig gelesen: ja. WaterRipples.cpp und WaterTriangulate.cpp (Earcut-Portierung mit `std::deque`-Pool, einmalig pro Rebuild) ergaben keine Funde. Zusätzlich gezielt per Grep/Teilausschnitt geprüft: SceneSystems.cpp (Aufrufreihenfolge), RenderExtractor.cpp:1190-1215, PhysicsWorld.cpp:3785-3800, AudioEngine.cpp (hasBus/isPlaying), UIInputSystem.h (hovered), TerrainMeshGenerator.cpp (computeTerrainHeightField), WeatherComponent.h (kGrid), TerrainComponent.h (tessellationFactor).
