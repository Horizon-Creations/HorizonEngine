# Spielbarkeits-Audit 27.08.2026 — was noch fehlt, um ein echtes 3D-Spiel zu bauen

**Stand:** `main` 6b3b2539. **Grundlage:** der Code, nicht der Plan.

Sechs Auditoren haben je einen Bereich am Quelltext abgeklopft, jeweils mit der Frage, woran ein
Autor scheitert, der damit heute einen Shooter, ein Third-Person-Action-Spiel oder ein
Survival-Spiel bauen will. Danach hat je ein zweiter Agent jeden Fund zu widerlegen versucht.
76 Funde, 66 bestätigt, 10 präzisiert, 0 unwiderlegt stehengelassen. Zwölf Rohfunde tragen nach der
Gegenprüfung die Note Blocker; sie verdichten sich auf sechs Ursachen, weil vier Bereiche dasselbe
Loch von verschiedenen Seiten gemeldet haben.

Dieses Dokument ist der Nachfolger von `docs/gap-audit-2026-08-25.md` und überschneidet sich mit ihm
bewusst nur dort, wo ein Fund seither präziser oder schlimmer geworden ist.

---

## Umsetzungsstand

| Blocker | Stand |
|---|---|
| B1 Physikwelt nach dem Szenenstart eingefroren | **erledigt** (`eb32fe27`), `addEntity`/`removeEntity`/`setPosition`, dazu ein Reap in `step()` und `purgeEntity` im Kontakt-Listener |
| B2 keine Kollisionsgeometrie ausser Box, Kugel, Kapsel | **erledigt** (`eb32fe27`), Mesh, Convex Hull und Height Field; die Landschaft trägt jetzt Kollision |
| B3 zur Laufzeit nur eine leere Entity erzeugbar | **erledigt**, `entity.spawnClass` über den bestehenden Create-Object-Dienst, erreichbar aus allen vier Frontends |
| B4 Navigation aus keiner Sprache steuerbar | offen |
| B5 gepacktes Spiel entdeckt keine PlayerController-Klasse | **erledigt**, `__asset_types__` in der Pak; bestehende Exporte müssen einmal neu gebaut werden |
| B6 kein Partikel-Burst | offen |

Bei B1 und B2 hat sich eine Regel herausgeschält, die über die beiden Blocker hinaus gilt und
dreimal hintereinander übersehen wurde: **`PhysicsWorld` tauscht ausschliesslich Weltposen aus, die
Skript-Transform-API arbeitet lokal, und umgerechnet wird in `EngineApi`.** Wer eine Pose zwischen
den beiden bewegt, muss die Richtung nennen.

B3 hat einen zweiten Befund freigelegt, der grösser war als der Blocker selbst: die
Textsprachen-Frontends bauten ihren `Ctx` an fünf Stellen selbst und liessen dabei `audio`,
`entities` und `runtime` weg. Elf Audio-Zeilen und jede Runtime-Zeile waren aus Lua und Python
stumm, ohne dass es je aufgefallen wäre, weil ein neutraler Rückgabewert wie ein Ergebnis aussieht.
Seither baut genau eine Stelle pro Frontend den `Ctx`.

---

## 1. Wo die Engine steht

Die Autorenseite und die Weltseite sind reif, die Laufzeitseite ist es nicht. Ein Autor kann heute
eine Landschaft skulptieren, sie bepflanzen, sie beleuchten, sie mit Wetter und Tageszeit versehen,
Materialien als Graphen bauen, Charaktere importieren, sie über eine Zustandsmaschine animieren,
UI im Designer zusammensetzen, Logik in vier Sprachen schreiben, das Ganze mit anderen zusammen
bearbeiten und als selbstenthaltenes Paket exportieren, das beim Spieler startet und seine
Grafikeinstellungen mitbringt. Was er nicht kann, ist die Welt zur Laufzeit verändern: die
Physikwelt wird beim Szenenstart einmal gebaut und ist danach eingefroren, nichts kommt hinzu,
nichts verschwindet, nichts lässt sich versetzen. Deshalb gibt es kein Geschoss, keinen gespawnten
Gegner, kein aufsammelbares Objekt, keinen Respawn und keine tragfähige gestreamte Zone. Dazu
kommen zwei Lücken, die unabhängig davon jedes Außenlevel treffen: die Landschaft hat überhaupt
keinen Kollisionskörper, und Kollisionsformen gibt es nur als Box, Kugel und Kapsel.

Das exportierte Paket startet, aber es startet leer: weil eine `.hpak` keine Typinformation trägt,
findet der Spielstart keine PlayerController-Klasse, und damit läuft das `BeginPlay`, in dem ein
HorizonCode-Projekt laut eigener Dokumentation seinen Charakter erzeugt, nie an. Von den sechs
Blockern sind also drei erst im ausgelieferten Spiel sichtbar und im Editor unauffällig. Fundament
ist damit alles bis einschließlich B6, Ausbau erst das, was danach kommt. Bemerkenswert ist, wie oft
die teure Hälfte bereits gebaut und nur nicht angeschlossen ist: Recast, `EntityHost::spawn`,
`GameReplication`, der Splash-Screen, `PropertyAnimClip`, die halbe AudioEngine.

---

## 2. Was schon geht

Das ist der Teil, den man beim Planen am häufigsten falsch im Kopf hat. Alles hier wurde geprüft.
Wo ein Blocker eine Fähigkeit entwertet, steht die Einschränkung dabei.

| Fähigkeit | Stand | Beleg |
|---|---|---|
| Level-Streaming und Zonen | vollständige API: `load`, `loadAdditive`, `unloadZone`, `activate`, `showZone`/`hideZone`, `setZonePosition`, `loadedZones`, `zoneScene`, `availableScenes`, `hasPendingLevel`, als verzögerte Request-Queue. **Aber: gestreamte Zonen bekommen keine Physik (B1), und das Entladen gibt kein einziges Asset frei** | `EngineApi.h:548-608`, `GameApplication.cpp:735-865` |
| Voller Levelwechsel | neue Welt wird zuerst gebaut, Fehlschlag lässt die laufende unangetastet; Physik, Skripte, Hosts fahren neu hoch, App-UI überlebt den Wechsel. **Aber: der Editor führt ihn nicht aus** | `GameApplication.cpp:680-733` |
| Fester Physiktakt | Akkumulator mit `kFixedDt`, in Spiel und Editor identisch. **Aber: die Renderseite interpoliert nicht zwischen den Schritten** | `GameApplication.cpp:1284-1298`, `PhysicsWorld.h:161` |
| Character Controller | echter Jolt `CharacterVirtual` mit Stufen-Hochlaufen, Hangbegrenzung, Grounded-Rückschreiben, mitgezogenem kinematischem Proxy | `PhysicsWorld.cpp:459-476`, `:591`, `:630-638` |
| Movement-Schicht | Intent lebt genau einen Frame, Clamp statt Normalize, `orientToMovement` über den kürzesten Weg, Y bleibt der Physik | `MovementSystem.cpp:35-70`, `tests/test_movement.cpp` |
| Kollisions- und Trigger-Ereignisse | kommen im gepackten Spiel in allen drei Skript-Frontends aus einem Dispatch an, Blocking und Sensor sauber getrennt. **Aber: ein Trigger ohne RigidBody ist stumm** | `GameApplication.cpp:1307`, `CollisionSystem.h:38-84`, `PhysicsWorld.cpp:389` |
| Physik-Schreibseite | `addForce`, `addImpulse`, `addTorque`, `setVelocity`/`getVelocity`, `overlapSphere`, `sphereCast`, Raycast mit `ignoreEntity`, `setGravity`. **Aber: nur auf Körpern, die beim Szenenstart existierten** | `PhysicsWorld.h:47-134` |
| Recast/Detour | komplette Bake-Kette, echte Pfadsuche mit String-Pulling, Wegpunkt-Folgen, Wireframe im Viewport, ungewöhnlich gute Klartext-Diagnose, Zellenbudget gegen den Editor-Freeze, Terrain wird mitgebacken. **Aber: aus keiner Sprache steuerbar, und im Spiel läuft kein Agent an (B4)** | `NavigationSystem.cpp:240-390`, `:432-509` |
| Animations-Zustandsmaschine | States, Transitions, echter TRS-Crossfade mit slerp, Parse-Cache pro Asset, Sync-Graph läuft vor der Transition-Auswertung und in beiden Anwendungen, geht durch den Codegen | `AnimationStateMachineSystem.cpp:117-254`, `:130-136` |
| Animationsphase | läuft in beiden Anwendungen zuletzt, nach dem Gameplay und vor der Extraktion | `GameApplication.cpp:1373`, `EditorApplication.cpp:2354` |
| Latente Ausführung | `Delay` ist echt, zwei Uhren (skaliert und real), von beiden Anwendungen gepumpt | `HorizonCode.h:216-221`, `HorizonCodeRuntime.h:157-163` |
| Objekt-zu-Objekt-Kommunikation | `BindEvent`, `EmitEvent`, `CallExternal`, `GetExternal`, `SetExternal`, `Cast`, `GetGameInstance`, kein globales Register nötig | `HorizonCode.h:183-254` |
| GameInstance | szenenübergreifender Zustandsort, aus jedem Graphen erreichbar, seine öffentlichen Variablen von außen les- und schreibbar. **Aber: kein Tick darauf** | `GameInstanceHost.h:5-16` |
| Speicherstände | mehrere Slots, getippte Felder inklusive Structs und Containern, atomar geschrieben, im Nutzerprofil statt im Installationsordner, aus allen vier Sprachen | `EngineApi.cpp:2126-2165`, `GameApplication.cpp:375-388` |
| Audio-Engine | miniaudio mit Bussen, 3D-Position, Abstandsdämpfung, Panning, komplettes Transport (seek, pause, Cursor, Pitch, Loop), tickt im gepackten Spiel. **Aber: nur sieben Funktionen davon sind aus Skripten erreichbar** | `AudioEngine.cpp:97-308`, `EngineApi.cpp:2072-2092` |
| Partikelsystem | allgemeines System mit Node-Editor und Live-Vorschau, GPU-instanziert, pro Emitter gebackener Shader, Kollision gegen die Physik mit Abprall. **Aber: nicht auslösbar (B6), und D3D/Vulkan zeichnen es nicht** | `ParticleSystem.cpp:118-133`, `RenderExtractor.cpp:256-280` |
| Decals | existieren vollständig: Komponente, Extraktion, Metal-Pipeline, Inspector mit Textur-Slot, Handbucheintrag. Der `ssr-plan` führt sie zu Unrecht als offen | `DecalComponent.h`, `InspectorPanel.cpp:1190-1201` |
| Material zur Laufzeit | `material.setParam` schreibt einen Override pro Entity, nicht ins geteilte Asset, Aufblinken bei Treffer ist baubar | `RenderExtractor.cpp:246` |
| Asset-Streaming | echt asynchron auf einem Worker, Registrierungen pro Frame gedeckelt gegen Hitches. **Aber: es gibt nur den Weg hinein, nie einen hinaus** | `ContentManager.h:282-295`, `GameApplication.cpp:1269` |
| Export | Exe plus Engine-Bibliotheken plus `.hpak` plus `config.json`, echtes macOS-.app mit Ad-hoc-Signat, inkrementelles Packen, Texturkompression nach Ziel-GPU, Schlüssel im Binary statt in der hcfg | `ProjectExporter.cpp:409-450`, `:974-989` |
| Grafik beim Spieler | 24 Grafikschlüssel plus Fenster und Backend werden ausgeliefert und **jeden Frame** neu aus dem Config gelesen, sind also live umschaltbar, sobald jemand sie setzen kann | `ExportDialogPanel.cpp:118-170`, `GameApplication.cpp:1375-1440` |
| Logdatei beim Spieler | mit Rotation über drei Vorläufe und Ausweichen ins Nutzerverzeichnis bei schreibgeschütztem Installationsort | `GlobalState.cpp:45-90` |
| Patch- und DLC-Kanal | jede `.hpak` in `Mods/` überlagert den Basis-Pak, gleiche UUID ersetzt, und zwar vor dem Lesen der Startszene, also auch Levels | `GameApplication.cpp:287-297` |
| Registry-Reichweite | alles außer `widget` und `material` ist automatisch in Lua und Python, inklusive `player`, `locomotion`, `movement`, `physics`, `scene`, `save`, `app` | `EngineApi.cpp:2383-2400` |

Zwei Korrekturen zu verbreiteten Annahmen: auf Windows ist das Standard-Backend **OpenGL**, nicht
D3D (`GameApplication.cpp:54-59`), Partikel verschwinden dort also erst, wenn jemand bewusst
umschaltet. Und Lua und Python können sehr wohl besitzen und einen Charakter fahren
(`horizon.player.possess`, `locomotion.move`), sie bekommen nur keine Action-Ereignisse.

---

## 3. Die Blocker

Sortiert nach Wirkung, nicht nach Aufwand.

### B1. Die Physikwelt ist nach dem Szenenstart eingefroren

**Was fehlt.** `PhysicsWorld` hat drei Lücken, die dieselbe Ursache haben und zusammen gehören:
keine Möglichkeit, einen Körper nachzutragen, keine, einen zu entfernen, und keinen Weg vom
Transform zurück nach Jolt. Die öffentliche API der Klasse besteht aus `initialize`, `step`,
`clear` und Abfragen, mehr nicht.

**Woran der Autor scheitert.** Jede nach dem Sitzungsstart erzeugte Entity bleibt körperlos: kein
Kollidieren, kein Getroffenwerden, kein Trigger, kein `locomotion.move`, kein `isGrounded`. Also
kein Geschoss, kein Pickup, kein gespawnter Gegner, kein Trümmerteil, keine gespawnte Plattform.
Jede zerstörte Entity lässt ihren Körper für immer stehen, das Level füllt sich also im Spielverlauf
mit unsichtbaren Wänden, die weiter blocken, weiter von Raycasts getroffen werden und weiter
Kollisionsereignisse mit einer toten Entity-Id feuern. Und weil die Physikschleife den Transform nur
schreibt und nie liest, ist `transform.setPosition` auf allem, was einen Körper hat, wirkungslos:
kein Respawn, kein Checkpoint, kein Portal, kein Aufzug, keine skriptgesteuerte Plattform. Die beiden
Hälften zusammen heißen, dass es derzeit überhaupt keinen Weg gibt, einen gestorbenen Spieler wieder
ins Spiel zu bringen, neu spawnen liefert einen unbeweglichen Körper, versetzen wird überschrieben.
Additiv gestreamte Zonen sind aus demselben Grund unbegehbar, der Spieler fällt durch ihren Boden.

**Beleg.** `entityToBody` wird ausschließlich in `PhysicsWorld.cpp:403` gefüllt, `entityToCharacter`
in `:476`, beide innerhalb von `initialize()`; `initialize()` hat genau zwei Aufrufer,
`GameApplication.cpp:887` und `EditorApplication.cpp:5464`, beide beim Weltaufbau. `step()` ab
`:490` iteriert nur über die beiden Tabellen und scannt nie nach. `RemoveBody`/`DestroyBody` stehen
nur in `clear()` (`:1060-1071`), alles auf einmal. Der Body-Writeback (`:504-563`) und die
Character-Schleife (`:604-613`) schreiben ausschließlich Jolt nach Transform; `transform.setPosition`
landet in `ScriptApi.cpp:62-65` und setzt nur `t->position` plus `dirty`, und `dirty` wird in
`PhysicsWorld.cpp` nie gelesen. `HorizonWorld::destroyEntity` (`HorizonWorld.cpp:410-425`)
benachrichtigt niemanden, entt-Sinks sind nirgends verdrahtet. Der Additive-Zweig
(`GameApplication.cpp:784-830`) baut den Kontext sogar mit `nullptr` als PhysicsWorld und ruft
`startPhysics` nicht, der UnloadZone-Zweig (`:844-861`) fasst Jolt nicht an. Die Engine weiß es und
sagt es dem Skriptautor als Ablehnungsgrund (`PhysicsWorld.cpp:733-735`), und sie widerspricht sich
dabei selbst: `GameApplication.cpp:554-556` behauptet im Kommentar, ein Spawn aus dem BeginPlay eines
Controllers bekomme einen Body. `EntityHost::spawn` berührt die Physik mit keiner Zeile.

**Aufwand: M.** Ein `addBody`/`removeBody`-Paar plus ein `setPosition`, danach je eine Registry-Zeile.
In denselben Umbau gehört das harte Weltbudget: `kMaxBodies`, `maxBodyPairs` und
`maxContactConstraints` stehen alle drei auf 1024 (`PhysicsWorld.cpp:271`, `:278-279`), ohne
Projekteinstellung, ohne Umgebungsvariable, ohne Config-Zeile.

### B2. Es gibt keine Kollisionsgeometrie außer Box, Kugel und Kapsel, und die Landschaft trägt gar keine

**Was fehlt.** `ColliderShape` kennt genau drei Werte. Es gibt keinen Mesh-Collider, keinen Convex
Hull und kein Heightfield. Terrain bekommt überhaupt keinen Körper: keine Zeile im Repo hängt jemals
eine `RigidBodyComponent` an eine Terrain- oder Terrain-Chunk-Entity. Entities ohne
`ColliderComponent` bekommen eine Box aus `transform.scale * 0.5`.

**Woran der Autor scheitert.** Jedes Außenlevel. Der Charakter fällt durch die Landschaft oder steht
auf einer unsichtbaren Kiste, Projektile fliegen durch den Berg. Ein importiertes glTF-Haus ist
physikalisch ein Quader, man läuft nicht hinein; Treppen, Rampen, Felsen und Brücken müssen aus
Primitiven nachgestellt werden. Der komplette Landschafts-Editor mit Sculpting, vier Paint-Layern
und Chunk-LOD produziert damit heute Dekoration. Besonders schief: die Navigation sieht das Terrain
sehr wohl, `NavigationSystem.cpp:136-148` backt die LOD0-Dreiecke der Chunks in Recast. Der KI-Agent
läuft also über eine Landschaft, durch die der Spieler fällt.

**Beleg.** `Enums.h:181-186` (drei Formen), `PhysicsWorld.cpp:309-345` (Formbau plus Box-Fallback).
Suchen ohne Treffer über `src/` ohne vendor: `HeightField`, `MeshShapeSettings`,
`ConvexHullShapeSettings`, `TriangleShape`. Kein `RigidBodyComponent` in `TerrainSystem.cpp` oder
`TerrainMeshGenerator.cpp`; `terrainHeightAt` hat drei Aufrufer, `FoliageSystem.cpp:84`,
`EditorApplication.cpp:2545`, `TerrainTools.cpp:91`, keiner davon in der Physik. Nebenbefund: ein
`ColliderComponent` mit `shape = Box` auf einer Character-Entity wird still zur Default-Kapsel
0.7/0.3 (`PhysicsWorld.cpp:455-470`), obwohl der Kommentar Box und Sphere verspricht.

**Aufwand: L.** Heightfield-Shape aus `sculptHeights` pro Chunk ist der große Teilgewinn und wäre
für sich genommen M.

### B3. Zur Laufzeit lässt sich nur eine leere Entity erzeugen

**Was fehlt.** Der Mechanismus ist gebaut und liegt eine Registry-Zeile hinter der Skriptseite:
`EntityHost::spawn` instanziiert den kompletten Komponenten-Blob einer Klasse über
`instantiatePrefab` und setzt Position und Rotation vor `Construct`/`BeginPlay`. Erreichbar ist er
nur über den HorizonCode-Knoten `Create Object`. Was Skripte stattdessen haben, ist
`entity.spawn(parent, name)`, eine nackte Entity ohne Transform, Mesh, Collider und Logik. Es gibt
außerdem kein `addComponent`/`removeComponent`/`hasComponent` in der Registry und keinen
Skript-Aufrufer von `instantiatePrefab`.

**Woran der Autor scheitert.** In Lua, Python und C++ ist nichts mit Inhalt spawnbar: kein Geschoss
mit Mesh und Collider, kein Gegner, kein Loot-Drop, kein Effekt, kein Decal am Trefferpunkt, keine
Trümmer. Da die Projektsprache eine harte Einsprachigkeits-Entscheidung ist, heißt das: wer nicht
HorizonCode nimmt, kann keine Welt bevölkern. Und in HorizonCode ist das Ergebnis körperlos (B1).
Der übliche Ausweg, Objekte vorplatzieren und poolen, trägt hier ebenfalls nicht, weil ein gepooltes
Objekt sich nicht versetzen lässt (B1). Genau deshalb bleibt dieser Fund ein Blocker statt einer
Unbequemlichkeit.

**Beleg.** `EntityHost.cpp:112-166`; Aufrufer nur `EditorApplication.cpp:1228` und
`GameApplication.cpp:429`. `entity.spawn` ist `EngineApi.cpp:1702` auf `ScriptApi.cpp:89-96`.
`instantiatePrefab` (`SceneSerializer.cpp:1939`) hat außerhalb von `EntityHost.cpp:130` nur
Editor-Aufrufer (Viewport-Drop, Duplizieren, Einfügen, Thumbnails, Collab).

**Aufwand: M**, davon der Löwenanteil in der Frage, wie eine Klasse aus Lua und Python benannt wird.

### B4. Navigation ist aus keiner Sprache steuerbar, und im gepackten Spiel läuft sie nie an

**Was fehlt.** Es gibt keinen einzigen Skript-Einstiegspunkt: keine Gruppe `nav`, `agent` oder
`path` in der Registry, und auch keinen generischen Komponentenzugriff. `NavAgentComponent::targetPos`,
`moving` und `hasPath` sind nur über zwei ImGui-Knöpfe im Inspector schreibbar. Es fehlen
`setDestination`, `stop`, `isAtDestination`, ein reines `findPath`, ein `sampleNavMesh` und ein
`randomPointInRadius`; Detour kann all das.

**Woran der Autor scheitert.** Jede reaktive KI. Ein Gegner kann nicht zum Spieler gehen, nicht
patrouillieren, nicht fliehen. Schlimmer: im ausgelieferten Spiel bewegt sich überhaupt kein Agent,
denn `moving` wird nicht serialisiert, jeder Agent startet also mit `moving = false`, und die
Update-Schleife überspringt ihn in der ersten Zeile. Das NavMesh ist im Spiel totes Gewicht.

**Beleg.** Registry ohne `nav`-Gruppe; einzige Schreiber außerhalb des Systems sind
`InspectorPanel.cpp:783-788` (Go/Stop) und `SceneSerializer.cpp:1085-1092` (Laden).
`SceneSerializer.cpp:517-523` schreibt nur `targetPos`, `speed`, `stoppingDist`;
`NavigationSystem.cpp:421` ist `if (!agent.moving) continue;`.

**Eigener Fixpfad, den B1 nicht miterledigt.** `NavigationSystem::update` schreibt `tc.position`
direkt (`:497-508`), umgeht die Physik also grundsätzlich. Daraus folgen heute drei kaputte
Ausgänge und ein vierter, der noch verwirrender ist: ohne Körper ist der Gegner ein Geist, den kein
Raycast trifft; mit dynamischem Körper zuckt er und bleibt stehen; mit Character Controller
überschreibt der Writeback die Nav-Bewegung; mit statischem Körper läuft er sichtbar weiter,
während sein Collider für immer am Spawnpunkt steht (`PhysicsWorld.cpp:506-508`). Die Lösung ist
nicht `teleport`, sondern die Bewegung über `setCharacterVelocity` beziehungsweise
`MovementComponent` zu führen, also denselben Weg zu nehmen wie der Spielercharakter.

**Zwei Fallen im selben Umbau.** `NavAgentComponent.h:9` behauptet, der Pfadzustand werde beim
Ändern von `targetPos` zurückgesetzt; es gibt kein Feld, das das alte Ziel merkt, und gepfadet wird
nur bei `hasPath == false`. Ein Verfolger folgt also nie, auch nicht, wenn man das Ziel jeden Frame
neu setzt. Und der Agent dreht sich nicht (`rotation` kommt in der Datei nicht vor), füttert keine
Animationsparameter und weicht nicht aus, es gibt kein `dtCrowd` und kein RVO.

**Aufwand: M** für die API plus die Velocity-Route, **L**, wenn Ausweichen dazu soll.

### B5. Im gepackten Spiel entdeckt `discoverAssets` nichts, und damit entsteht kein PlayerController

**Was fehlt.** `PlayerHost::begin` baut über `ContentManager::discoverAssets` dreierlei: die
Aktionen, die Bindungen und die PlayerController-Klassen. `discoverAssets` selbst besteht aus zwei
Hälften, und im gepackten Spiel liefert keine etwas: `enumerateIds` liest `m_assetTypeIndex`, der
ausschließlich beim tatsächlichen Laden eines Assets geschrieben wird, und der anschließende
Verzeichnislauf geht über `<exeDir>/Content`, ein Verzeichnis, das der Exporter nie anlegt.
`mountPak` füllt nur UUID-nach-Mount und Pfad-nach-UUID, ohne Typinformation. Weder Input- noch
Klassen-Assets sind von einer Szenenkomponente referenziert, werden also auch nicht nachgezogen.

**Woran der Autor scheitert.** Nicht in erster Linie an der Eingabe, sondern daran, dass sein Spiel
in eine leere Welt startet. Die dritte Schleife in `begin` sucht die HorizonCodeClass-Assets, deren
aufgelöste Basis PlayerController ist (`PlayerHost.cpp:54`). Derselbe leere Rückgabewert heißt
also: im ausgelieferten Spiel wird überhaupt kein PlayerController instanziiert, kein `Construct`,
kein `BeginPlay`. Und genau dieses `BeginPlay` ist laut `PlayerHost.h:21-31` der Ort, an dem das
Spiel seinen Charakter per `Create Object` spawnt und ihn per `player.possess` übernimmt. Ein
ausgeliefertes HorizonCode-Spiel zeigt damit die Kulisse und sonst nichts.

Die Eingabehälfte ist echt, aber kleiner als sie aussieht. Weg ist alles, was die empfohlene
Eingabeschicht benutzt: benannte Actions und Axes, die `Input.*`-Ereignisse an PlayerController und
besessene Charaktere, die komplette Gamepad-Bindungsarbeit. Nicht weg ist das rohe Abfragen.
`HE::api::input::pushSdlSnapshot` und `setGamepad` laufen im gepackten Spiel jeden Frame
(`GameApplication.cpp:1203`, `:1230`, `:1237`), also funktionieren `input.keyDown`,
`input.mouseButton`, `input.mousePosition`, `input.mouseDelta`, `input.scrollDelta`,
`input.gamepadButton` und `input.gamepadAxis` im Build aus allen vier Frontends. Ein Lua- oder
Python-Spiel, das Tasten pollt statt Actions zu binden, ist bedienbar. Im Editor funktioniert
beides, dort ist die Content-Wurzel ein echtes Verzeichnis, der Fehler zeigt sich also erst nach
dem Packen.

**Beleg.** `PlayerHost.cpp:34-54`; `ContentManager.cpp:1919-1941`, `:1910-1917`, Schreiber des
Typindex nur `:508` und `:1772`; `mountPak` `:2199-2249`; Content-Wurzel `Application.cpp:50-52`;
`create_directories` im Exporter nur `440/449/450/771`; `streamMountedAssets` hat außer den Tests
keinen Aufrufer. Der Header benennt die Lücke selbst (`PlayerHost.h:45-47`). Für die Klassenhälfte:
`m_playerHost.begin` steht in `GameApplication.cpp:561`, und davor lädt nichts
HorizonCodeClass-Assets nach Typ; die beiden `loadAsset`-Stellen bei `:417` und `:451` liegen in
der `createObject`-Lambda und laufen erst zur Laufzeit, per Pfad. Das Gegenstück, das es richtig
macht, existiert bereits: `__type_index__` für Struct- und Enum-Assets, durchgeladen in
`GameApplication.cpp:301-326`.

**Aufwand: S.** Ein Typ-Feld im `__asset_index__` nach dem Muster von `__type_index__`; ein reines
`__input_index__` wäre genauso billig, deckte aber nur die kleinere Hälfte ab. Der Fix erwischt
beide Wirkungen, die Abnahme muss das ebenfalls tun: nicht "die Tasten reagieren", sondern "der
PlayerController wird instanziiert und sein `BeginPlay` läuft". Wer B5 als Eingabeproblem liest,
prüft nach dem Fix die Tastatur und übersieht die Klassenentdeckung.

### B6. Ein Effekt lässt sich nicht auslösen

**Was fehlt.** Drei Dinge zusammen. Es gibt keinen Burst und keinen One-Shot: die Emissionsschleife
fragt `looping` nie ab. Es gibt keinen Skript-Zugriff auf Partikel, keine Gruppe `particle`, `vfx`
oder `fx` in der Registry, `playing` ist eine Inspector-Checkbox. Und der einzige Skript-Hebel,
`entity.setVisible`, fasst nur `ps->visible` an, die Simulation läuft unsichtbar weiter.

**Woran der Autor scheitert.** Explosion, Mündungsfeuer, Blutspritzer, Einschlagsstaub, Funken beim
Hacken, also jeder Effekt, der einmal an einer Stelle passiert. Der Ausweichweg über
"Effekt-Entity spawnen" ist durch B3 zu, es bleibt nur, jeden Effekt vorab in die Szene zu stellen
und ihn nicht einmal einschalten zu können.

**Beleg.** `ParticleSystem.cpp:150-191` (Emissionsschleife ohne `looping`-Abfrage, Klemmung,
Fertig-Bedingung), `:200-207` (`playing`-Gate), `EngineApi.cpp:77` (`setVisible`),
`InspectorPanel.cpp:1512`. `looping = false` ist dabei nicht bloß wirkungslos, sondern in beide
Richtungen falsch: bei `interval > dt` meldet `stepPool` schon im ersten Frame fertig und schaltet
den Emitter ab, **bevor** ein einziges Partikel entstanden ist, bei `interval < dt` (also im
Burst-Fall) leert sich der Pool nie und der Emitter läuft ewig. `tests/test_particles.cpp:184-204`
besteht genau wegen der ersten Variante, der Test prüft nur `!playing`, nie ob überhaupt etwas
emittiert wurde.

**Aufwand: L**, weil zum Auslösen auch ein Vokabular gehört, das etwas taugt (siehe Abschnitt 4).
Nur `particle.play`/`stop`/`burst` plus ein korrektes `looping` wären M.

---

## 4. Die schmerzhaften Punkte

### Die Editor-Vorschau ist kein Beleg für das ausgelieferte Spiel

Drei unabhängige Divergenzen. Erstens führt der Editor von den sechs Szenen-Anforderungen nur vier
aus, ein voller Levelwechsel und `activate` landen im else-Zweig mit einer Warnung
(`EditorApplication.cpp:1863-1911`); der Ablauf Hauptmenü, Level, Game Over, Neustart ist im Editor
also nicht durchspielbar, jede Iteration daran kostet einen Export. Zweitens startet der PIE-Pfad
für additiv geladene Zonen keine Skripte, das Spiel dagegen schon (`GameApplication.cpp:818-820`
ruft `startScriptsFor` mit beiden Hosts, `bindFor` und `startScriptsFor` kommen in
`EditorApplication.cpp` gar nicht vor). Drittens liegt `SceneSystems::tickWorld` im Editor **vor**
dem Physikschritt (`:2233` vor `:2246`), im Spiel dahinter (`:1298` vor `:1368`), womit
MovementSystem und NavigationSystem in beiden Anwendungen auf verschiedenen Seiten des Schritts
laufen. Der Plan nennt als Zielreihenfolge Skripte, Physik, Kamera, Hosts, `tickWorld`,
`tickAnimation` (`docs/character-animation-rework-plan.md:250-252`); das Spiel hält sie ein, der
Editor nicht, und `SceneSystems.cpp:99-102` behauptet im Kommentar das Gegenteil. Aufwand: S für die
Reihenfolge, M für Zonen-Skripte und den Szenenwechsel in PIE.

### Es gibt keinen Sprung

Keine `locomotion.jump`, keine Coyote-Zeit, kein Sprungpuffer, kein Feld an
`CharacterControllerComponent` oder `MovementComponent`. Die Umgehung über `physics.setVelocity` mit
gesetztem Y funktioniert je nach Sprache und Anwendung verschieden: im gepackten Spiel geht sie aus
Lua und Python, weil der Physikschritt zwischen Skripten und MovementSystem liegt; aus HorizonCode
geht sie nie, weil der PlayerHost hinter dem Schritt tickt; im Editor geht sie gar nicht. Wer das
debuggt, sucht in seinem Skript, nicht im Tick-Diagramm. Ohne `MovementComponent` funktioniert sie
überall, weil `MovementSystem.cpp:16` dann nicht greift. Aufwand: M, und sie gehört mit der
Tick-Reihenfolge in denselben Handgriff.

### Zwischen zwei Physikschritten wird nicht interpoliert

Der Akkumulator läuft mit `kFixedDt = 1/60` (`GameApplication.cpp:1284-1298`), der Writeback
schreibt Jolt direkt in die `TransformComponent` (`PhysicsWorld.cpp:504-563`, `:604-613`), und der
Extractor nimmt, was dasteht. Es gibt kein Alpha, keinen vorigen Transform, kein `prevPosition`-Feld;
die einzige Interpolation im Repo ist die Snapshot-Interpolation des Netzwerkcodes
(`GameReplication.cpp:438`). Auf jedem Bildschirm, dessen Bildrate kein Vielfaches von 60 ist, also
bei 144, 120, 165 Hz oder ungebremst, springt damit jedes physikgetriebene Objekt sichtbar, weil es
nur alle 2,4 Bilder einen neuen Wert bekommt. Das trifft zuerst den Charakter und damit in beiden
Kameramodi die ganze Ansicht, während die Blickrichtung aus der Maus mit voller Bildrate läuft: das
Ruckeln ist gemischt, und das ist die unangenehmste Variante. Der feste Takt selbst ist richtig, es
fehlt nur seine Renderhälfte. Immerhin kein Export-Schock, der Editor tickt genauso. Aufwand: M, ein
zweiter Transform pro Körper plus ein Alpha im Extractor.

### Eine Entity hat keine Identität außer ihrem Namen

Keine Tags, keine Teams, keine Fraktionen, kein Besitzverhältnis im Gameplay-Sinn, und genau zwei
Kollisions-Layer, die nicht gewählt, sondern aus `RigidBodyType` abgeleitet werden
(`PhysicsWorld.cpp:49-53`, `:360-375`); weder `ColliderComponent` noch `RigidBodyComponent` haben ein
Layer- oder Maskenfeld, und selbst der Charakter-Sweep filtert fest auf `MOVING` (`:565-567`).
Dazu gibt es keine Weltabfrage außer `entity.findByName`: kein `findByTag`, kein `findAll`, kein
"alle Entities mit Komponente X", und `ForEach` in HorizonCode läuft über ein Array, nicht über die
Welt. Damit ist nicht ausdrückbar: "nur der Spieler löst diesen Trigger aus", "Geschosse treffen den
Schützen nicht", "Freund oder Feind", "alle Gegner in diesem Raum aufwecken", "alle Sammelobjekte
zählen". Der einzige Ersatz ist ein `Cast` auf die HorizonCode-Klasse; in Lua und Python bleibt der
Namensvergleich. Aufwand: M für Tags plus Weltabfrage, M für Layer und Maske.

### Es gibt keinen Ort für Spielregeln mit einem Tick

Die Klassentaxonomie kennt vier Klassen, keine Regel-Klasse (`HorizonCode.cpp:2171-2191`). Der
GameInstance ist zwar ein echter szenenübergreifender Zustandsort und von überall erreichbar, aber
`fireTick` hat repo-weit genau drei Aufrufer (`WidgetManager.cpp:405/409`, `EntityHost.cpp:194`,
`PlayerHost.cpp:183`), und weder die Level-Instanz noch der GameInstance ist einer davon. Punktestand,
Rundentimer, Siegbedingung und Wellen-Spawner müssen deshalb auf einer im Level platzierten Entity
sitzen, die niemand löschen darf und die per `Get Game Instance` plus `Call External` in die Regeln
delegiert. Ein Hauptmenü-Level ohne Spieler und ohne Entity hat gar keinen Ort mit Pro-Frame-Logik.
In Lua und Python gibt es überhaupt kein Skript ohne Entity. Aufwand: M.

### Ein Trigger ohne RigidBody ist stumm, und nichts sagt das

`PhysicsWorld::initialize` iteriert über `view<TransformComponent, RigidBodyComponent>` (`:308-309`).
Wer eine Collider-Komponente hinzufügt und "Is Trigger" anhakt, ohne einen RigidBody dazuzulegen,
bekommt keinen Körper, keine Warnung, keinen Log-Eintrag und keinen Hinweis im UI; auch das
F1-Handbuch erwähnt den Zusammenhang nicht (`EditorHelp.cpp:275-278`), er steht nur im
Header-Kommentar von `ColliderComponent.h:7-9`. Das ist die erste Interaktion, die jeder baut, und
sie scheitert lautlos. Der Präzedenzfall für den Fix steht im selben Menü drei Zeilen darüber:
"Camera Rig" fügt seine `CameraComponent` automatisch bei, mit exakt dieser Begründung
(`InspectorPanel.cpp:1745-1755`). Aufwand: S.

### Kein Startpunkt, kein Respawn-Baustein, nicht einmal als Konvention

Keine Marker-Komponente, kein Spawn-Volumen, keine Respawn-Funktion. Der Autor muss den Startpunkt
über einen verabredeten Entity-Namen suchen und die Weltposition selbst in `Create Object` stecken.
`PlayerHost.h:21-31` begründet die Abwesenheit ausdrücklich als Entscheidung ("where a body stands
is the game's decision"), die nachvollziehbar ist, nur gibt es kein Werkzeug, sie umzusetzen.
Aufwand: S, sobald B1 und B3 stehen.

### Verzögerung gibt es nur einmal gleichzeitig und ohne Abbruch, Timer gar nicht

Der einzige Zeitbaustein ist `Delay`, und erneutes Auslösen während des Wartens wird still verworfen
(`HorizonCode.h:216-221`). Kein Handle, kein Abbrechen, keine Wiederholung, kein benannter Timer;
die `time`-Kategorie hat sechs Zeilen und keine davon ist latent. Nachladezeit, Feuerrate,
Cooldown, ablaufender Buff, Rundenuhr, verzögerte Explosion sind damit in HorizonCode Ketten ohne
Abbruchmöglichkeit (eine Waffe, die man beim Waffenwechsel nicht stoppen kann), in Lua und Python
schreibt jeder Autor seine Zeitverwaltung neu. Aufwand: M.

### Animation: eine Spur, ein Clip, kein Rückkanal

Fünf Befunde mit derselben Wurzel, das Auswertungsmodell ist eine einzige globale Mischung zweier
Posen. Es gibt keine Blend Spaces, keine Layer, keine Bone-Masken und nichts Additives
(`AnimationEval.h:38-42`, `AnimationStateMachineSystem.cpp:215-243`); ein Ego-Shooter kann also nicht
mit dem Oberkörper zielen, während die Beine laufen. Es gibt kein Root Motion, die Figur driftet
über dem Boden, Tempo und Animation hängen nur über `maxSpeed` zusammen. Es gibt keine Notifies und
die Clip-Zeit ist aus keiner Sprache lesbar, also kein Schrittgeräusch, kein Trefferfenster, kein
Hülsenauswurf am richtigen Frame; die `animator`-Gruppe besteht aus `setParam`, `getParam`,
`getState`. Es gibt kein `animator.play`, der einzige Weg zu einer Animation führt über einen
Float-Parameter plus Transition. Und die Zustandsmaschine kann eine Bedingung pro Übergang, keinen
Exit Time, kein Any State, keine bool- oder Trigger-Parameter, und während eines Crossfades werden
Übergänge gar nicht geprüft (`AnimationStateMachineSystem.cpp:179-197`), eine eintreffende
Trefferreaktion wird also verschluckt. Als Kuriosum am Rande: `AnimatorBlendComponent` lässt sich
nicht einmal über den Inspector anlegen, das ist ausdrücklich so gewollt
(`InspectorPanel.cpp:1786-1791`), erreichbar ist sie nur über eine handgeschriebene `.hescene`.
Aufwand: XL für Layer und Blend Spaces, M je für Root Motion, Notifies und `play`, L für die FSM.

### Charakter- und Physikqualität: kein IK, kein Ragdoll, keine Constraints, keine Sockets

Kein Solver irgendeiner Art, die Bone-Matrizen entstehen rein vorwärtskinematisch und werden danach
von nichts mehr angefasst, also keine Füße auf schrägem Boden, keine zweite Hand an der Waffe, kein
Look-At, kein Aim-Offset. Kein Ragdoll und kein Übergang dorthin, und auch keine Joints: Hinge,
Point, Slider, SixDOF, Distance kann Jolt alles, die Engine benutzt nichts davon, also keine Tür mit
Scharnier, keine Kette, kein Seil, keine Radaufhängung. Keine Bone-Sockets, es gibt nicht einmal
eine API, die Weltmatrix eines Knochens zu **lesen**, das Wort `bone` kommt in `EngineApi.cpp` und
`EngineApi.h` kein einziges Mal vor; die Waffe in der Hand ist damit nicht befestigbar. Und der
Character Controller nutzt zwei Jolt-Fähigkeiten nicht, die fertig danebenliegen: die Shape wird nur
einmal gebaut, es gibt kein `SetShape` und damit kein Ducken, und `GetGroundVelocity` wird nirgends
gerufen, also steht der Spieler in der Luft, während die Plattform unter ihm wegfährt. Aufwand: S
für Ducken und bewegte Plattformen, M für Sockets, L für IK, XL für Ragdoll und Constraints.

### Ein Mesh hat genau ein Material, egal wie viele die Quelldatei hatte

`StaticMeshAsset` und `SkeletalMeshAsset` tragen je genau ein `materialPath`/`materialId`
(`Assets.h:77-117`), `MeshComponent` hat gar keine Materialreferenz
(`Components/MeshComponent.h:5-12`), und `RenderObject` kennt genau eine `materialAssetId` als
Override (`RenderObject.h:10-20`). Der Importer passt dazu: er hängt alle glTF-Primitive eines
Modells in einen einzigen Index-Buffer (`MeshImporter.cpp:29-45`, `:120-135`), die Untergliederung
der Quelldatei ist nach dem Import also weg, samt der Zuordnung Primitiv zu Material.

Das ist die andere Hälfte des Hauses aus B2. Dort ist das importierte glTF-Haus physikalisch ein
Quader, hier trägt es optisch ein einziges Material: Dach, Wand, Fenster und Tür teilen sich eine
Textur. Das gilt für jede gekaufte Requisite, jede Waffe, jedes Fahrzeug und jeden Charakter, und
ein Autor trifft es in derselben ersten Stunde wie B2, nämlich beim ersten importierten Modell. Der
Vor-Audit führt den Punkt als 4.2. Aufwand: XL, weil Submesh-Bereiche und eine Slot-Liste durch
Asset-Format, Importer, Serialisierung, Extractor, alle Backends und den Inspector müssen.

### Zur Laufzeit wird nie ein Asset freigegeben

`ContentManager::unloadAsset` hat repo-weit keinen Aufrufer außerhalb von Editor-Panels
(`ContentBrowserPanel.cpp:2444`, `:2631`, `MaterialEditorPanel.cpp:1531`, `:1571`), dem
Thumbnail-Cache und den Tests. Es gibt kein Speicherbudget und keine LRU-Räumung: der Header nennt
die AssetRef-Pins ausdrücklich als Voraussetzung für "safe LRU eviction" (`ContentManager.h:410`),
gebaut ist sie nicht. Die Renderer-Caches werden nur über `InvalidateMesh`/`InvalidateMaterial`
angefasst, und die ruft außerhalb des Editors niemand außer der Terrain-Regeneration
(`TerrainSystem.cpp:119`). `UnloadZone` (`GameApplication.cpp:843-862`) zerstört Entities und
Skriptinstanzen, fasst aber weder den ContentManager noch die GPU-Caches an.

Der Speicher einer gestreamten Welt wächst deshalb monoton. Zone laden, verlassen, entladen,
zurückkehren lässt RAM und VRAM jedes Mal steigen und nie fallen, womit die zentrale Fähigkeit für
"Welt im Großen" in langen Sitzungen unbrauchbar ist, und zwar genau auf der schwachen Hardware, um
die sich dieses Dokument an mehreren Stellen sorgt. Die nie freigegebene Audio-Stimme direkt
darunter ist kein eigener Fund, sondern der Sonderfall derselben Regel: gebaut ist der Weg hinein,
nicht der hinaus. Aufwand: S für ein `unloadZoneAssets`, das wenigstens die Assets einer entladenen
Zone zurückgibt, M für ein echtes Budget mit LRU auf den bestehenden Pins.

### Audio: die Engine kann mehr als das Spiel

Vier Punkte. Erstens werden Stimmen nie freigegeben, der Spezialfall der Regel eine Ebene darüber:
`AudioEngine` hat kein `update`, jeder `play`
legt eine `ActiveSound` mit einer **vollständigen PCM-Kopie** an, und aus der Tabelle verschwindet
sie nur durch explizites `stop`; nach 500 Schüssen liegen zig Megabyte tote Kopien und 500 lebende
Mixer-Objekte herum, und die 128er-Warnung im Code beschreibt den Normalfall, nicht den Fehlerfall
(`AudioEngine.cpp:22-27`, `:165`, `:239-248`). Zweitens ist die Skript-Oberfläche eine Teilmenge:
`setSoundVolume`, `setSoundPitch`, `pauseSound`, `resumeSound`, `seekSound` existieren, haben aber
keine Registry-Zeile und werden ausschließlich vom Editor-Vorschau-Transport benutzt; also keine
Musik-Überblendung, keine Pause bei Escape, kein Zurücklesen eines Bus-Pegels für den Regler im
Optionsmenü. Drittens nur unkomprimiertes WAV ohne Streaming, zehn Musikstücke sind rund 400 MB im
Paket und im RAM, wo sie als OGG 40 wären, und der Editor nimmt nichts anderes an. Viertens ist der
`AudioSourceComponent`-Block im Inspector reiner Anzeigetext: die Asset-UUID wird per `labelText`
gezeigt, es gibt kein Drop-Ziel und kein Auswahl-Popup, obwohl der Editor den Helfer an 13 anderen
Stellen benutzt, unter anderem für `WeatherComponent::thunderSound` (`InspectorPanel.cpp:1463-1466`
gegen `:513`). Ein knisterndes Lagerfeuer ist damit im Editor nicht autorierbar, Ton in der Welt geht
nur skriptgesteuert über `audio.playAt` mit einem Pfad. Aufwand: S für das Aufräumen und den
Drop-Slot, M für die restlichen Registry-Zeilen und OGG.

### Das Partikel-Vokabular reicht für keinen klassischen Effekt

Auch wenn B6 behoben ist, kann der Emitter zu wenig: alles entsteht aus **einem Punkt**, es gibt
keine Kugel-, Box-, Kegel-, Ring- oder Mesh-Form; eine Instanz besteht aus Position, Größe und
Lebenszeit, also keine Rotation, keine Zufallsfarbe, keine Zufallsgröße; und der Blend-Modus ist in
beiden Backends fest auf Alpha verdrahtet, es gibt **keine additive Mischung**, Feuer, Funken,
Mündungsfeuer und Magie sehen zwangsläufig wie graue Aufkleber aus. Dazu fehlen Flipbooks,
Tiefensortierung, weiche Partikel, Kurven, Emitter-Verzögerung und -Dauer, lokaler Raum und
Sub-Emitter, und `RandomRange` würfelt laut eigenem Kommentar nur einmal beim Laden. Belege:
`ParticleSystem.cpp:180`, `RenderWorld.h:90-94`, `MetalRenderer.mm:10545-10549`,
`ParticleGraph.h:14-18`, `:60-88`. Aufwand: XL.

### Partikel und Decals zeichnen nur Metal und OpenGL

`particleBatches` wird nur in `MetalRenderer.mm:14820` und `OpenGLRenderer.cpp:11692` verbraucht,
D3D11, D3D12 und Vulkan ignorieren sie stillschweigend; Decals gibt es nur auf Metal und dort nur im
Deferred-Tile-Modus, also nur auf Apple-GPUs. Entwarnung gegenüber der ersten Einschätzung: auf
Windows ist OpenGL das Standard-Backend, ein Spieler sieht seine Partikel also, unsichtbar werden
sie erst, wenn jemand bewusst auf D3D oder Vulkan umschaltet. Aufwand: L, gehört zur
Backend-Parität.

### Auslieferung: kein Absturz-Handler, kein Cross-Export, kein Produktauftritt

`CrashHandler::install()` wird repo-weit genau einmal gerufen, in `src/HE_Editor/main.cpp:9`. Das
`main.cpp` des Spiels ist sieben Zeilen lang und ruft es nicht, obwohl der POSIX-Zweig fertig ist,
ein ausgeliefertes Spiel verliert also auch auf macOS und Linux jeden Absturz, nicht nur auf Windows
(dort fehlt zusätzlich die Implementierung). Aufwand: S für die eine Zeile.
Cross-Platform-Export bietet vier Ziele an und liefert nur die eigene Runtime mit: die anderen
sucht der Exporter unter `../GameRuntimes/<Plattform>/`, ein Pfad, den die CI nie befüllt und den
kein Dokument erwähnt; nebenbei wird für jedes Ziel außer Host die HorizonCode-Übersetzung nach C++
übersprungen, und zwar nur mit einer Logzeile. Und das fertige Produkt heißt `HorizonGame.exe`,
trägt das Systemicon und behauptet Version 1.0 (`ProjectExporter.cpp:1029-1030`, `:331-341`), es gibt
kein Icon-, Versions- oder Herausgeberfeld im Export-Dialog. Aufwand: je M.

### Das ausgelieferte Spiel hat keine Laufzeit-Diagnose

Es gibt keine In-Game-Konsole, "console" hat in `src/HE_Game` keinen einzigen Treffer, dazu kein
FPS- oder Frametime-HUD und keinen Cheat- oder Debug-Schalter. Der komplette Profiler mit Capture,
Overview und Frame-Detail liegt in `HE_Editor` (`ProfilerPanel.cpp`, `ProfilerWidgets.cpp`), im
Spiel stehen nur die `HE_PROFILE_SCOPE`-Marker ohne jede Anzeige. Die Kommandozeile wird nicht
ausgewertet, `src/HE_Game/src/main.cpp` reicht `argc`/`argv` nur an `Run` weiter, es gibt also weder
ein `-console` noch ein `-dev`. Auf der Fehlerseite ergibt eine kaputte oder fehlende Startszene
eine Logzeile und ein schwarzes Fenster (`GameApplication.cpp:521-545`); Messageboxen gibt es nur
für den Renderer-Init und für Renderfehler (`Application.cpp:201`, `:279`). Ausdrücklich vorhanden
und deshalb hier zu nennen, damit niemand dort sucht: `debug.line`, `debug.sphere` und `debug.box`
funktionieren im gepackten Spiel (`GameApplication.cpp:1258-1260`).

Der Absturz-Handler aus Welle 1 deckt das nicht ab, er deckt den Absturz. Alles darunter bleibt
unsichtbar: meldet ein Tester "ruckelt bei mir" oder "hängt nach zehn Minuten", gibt es außer der
Logdatei kein Werkzeug, und die Frage, ob es an der Bildrate, am Speicher oder an einem Skript
liegt, ist beim Spieler nicht beantwortbar. Für eine Engine, deren Vorschau an drei Stellen anders
tickt als der Build und mit B5 an einer vierten anders lädt, ist das die falsche Stelle zum Sparen.
Aufwand: S für ein Frametime-HUD hinter einem Schalter, M für Konsole und
Kommandozeilen-Auswertung, die nebenbei `-windowed`, `-width` und den Netzwerk-Startweg mitbringt.

### Der Spieler kann nichts einstellen und nichts umbelegen

Es gibt keine Registry-Gruppe für Einstellungen, Grafik oder Fenster, `app` besteht aus `quit`.
`setVSync`, `setWindowMode` und `setMaxFps` existieren als Methoden der Anwendung und werden nur vom
Editor gerufen. Persistenz ist im Spiel bewusst abgeschaltet, und der Datei-Sandkasten sperrt auf
`<pref>/Saved`, ein Skript kann die exe-benachbarte `config.json` also nicht einmal von Hand
schreiben. Der Spieler kann sie im Texteditor ändern, das hält auch, aber ein Optionsmenü im Spiel
ist nicht baubar. Fast geschenkt wäre dabei die Grafik-Hälfte: `GameApplication.cpp:1375-1440` liest
die Werte **jeden Frame** neu, eine API, die den GlobalState setzt und die Datei schreibt, wirkte
ohne jede Renderer-Arbeit sofort, nur Auflösung, Fenstermodus und Backend brauchen einen Neustart.
Ebenso zu ist die Tastenbelegung: `PlayerHost` backt die Bindungen einmal in ein privates Feld, es
gibt kein Umbinden, kein Speichern, kein Zurücksetzen und nicht einmal eine Abfrage, welche Taste
gerade auf einer Action liegt, ein HUD kann also kein "Drücke E" anzeigen. Der Editor hat die
komplette Binding-UI. Aufwand: je M.

### Kein Splash, kein Ladebildschirm mit Fortschritt, keine Lokalisierung, kein Umlaut

`ApplicationConfig` hat ein fertiges Splash-Feld samt verstecktem Hauptfenster und Fortschrittstexten,
`GameApplication::GetConfig` belegt es nie; das Spiel zeigt beim Start also ein schwarzes Fenster.
Ein Ladebildschirm ist zur Hälfte baubar, `scene.load(pfad, hidden)` plus `hasPendingLevel` plus
`activate` plus `OnLevelLoaded` ergibt einen sauberen Anfang und ein Ende, aber es gibt keinen
Fortschrittswert: `asyncInFlightCount` und `asyncProgress` existieren und haben zwei Konsumenten,
den Profiler und den Material-Editor, keine Registry-Zeile. Lokalisierung existiert in keiner Form,
kein Katalog, keine Schlüssel, keine Sprachumschaltung, kein Font-Fallback. Und der Text-Renderer
des Spiels backt genau 96 Glyphen, ASCII 32 bis 127, jedes andere Byte wird still übersprungen
(`UIFont.cpp:22`, `:110`, `:237-238`), aus "Größe" wird "Gre". Das gilt für beide UI-Wege und auch
für ein importiertes Font-Asset. Immerhin ist es kein Export-Schock, weil die Designer-Vorschau
denselben Atlas benutzt. Aufwand: S für den Splash, M für UTF-8 und den Fortschritt, L für
Lokalisierung.

### Welt im Großen: Sichtbarkeit, Foliage, Terrain-Daten, Zonen im Editor

Sichtbarkeit endet beim Frustum-Cull plus Distanz-LOD: kein Occlusion Culling, kein Hi-Z, kein HLOD,
keine Impostors, und `MeshComponent` hat nicht einmal eine Zeichendistanz (die gibt es nur für
Lichter und Foliage), ein Mesh ohne `LODComponent` wird also in jeder Entfernung submittiert. In
einem Gebäude wird jedes Objekt hinter jeder Wand gezeichnet. Foliage sitzt zwingend auf derselben
Entity wie das Terrain (`FoliageSystem.cpp:36`), womit es pro Landschaft **genau eine** Sorte gibt,
gleichverteilt gestreut, ohne Maske, ohne Hang- oder Höhenfilter; Gras und Bäume und Steine
nebeneinander erzwingen mehrere Terrains. Die Terrain-Quelldaten liegen base64-kodiert in der
`.hescene` und können deshalb nicht streamen; bei der Inspector-Obergrenze von 512 Stützpunkten auf
10 km ist das ein Punkt alle 19,5 Meter. Zonen wiederum existieren im Editor gar nicht als Objekt:
kein Panel, keine Liste, keine Gizmos, kein Streaming-Volumen, eine Zonenposition ist eine Zahl in
einem Node. Und das 8-Licht-Fenster der Nicht-Metal-Backends ist **ungeordnet**, es sind nicht die
nächsten acht Lichter, sondern die acht mit der niedrigsten ECS-Position, und die springt beim
Laden einer Zone um, was sich als Flackern liest, nicht als Limit (`RenderExtractor.cpp:460-488`,
`LightPacking.cpp:7-19`). Aufwand: S für die Licht-Sortierung, L für Foliage-Layer und
Zonen-Autorenwerkzeuge, XL für Occlusion Culling.

### Ein Spielstand kann nur wiedergeben, was der Autor vorgesehen hat

`SaveStateComponent` speichert Transform und Sichtbarkeit, mehr nicht, und sagt selbst, dass eine
zur Laufzeit erzeugte Entity jeden Lauf eine neue UUID prägt und ihr Zustand deshalb nie
wiederhergestellt werden kann. Es gibt keinen Zerstört-Vermerk und keinen Entity-Iterator, über den
man einen bauen könnte (siehe Identität, oben). Aufgesammeltes liegt nach dem Laden wieder da,
zerstörte Kisten stehen, gebaute Strukturen sind weg. Ein Umweg existiert und ist zumutbar, Listen
zerstörter und zu respawnender UUIDs passen als Container-Feld in ein Save-Template, aber der
Respawn hängt dann an B1 und B3. Aufwand: L für einen echten Weltzustand.

### `PropertyAnimClip` ist ein toter Assettyp

Nicht nur nicht erzeugbar, sondern gar nicht ladbar: der Typ fehlt in **beiden**
Dispatch-Switches der ContentManager, während die übrigen 21 dort stehen; der einzige Weg an ein
Clip-Asset ist `registerPropertyAnimClip` aus C++, dessen einzige Aufrufer sieben Testfälle sind.
Selbst eine handgeschriebene `.pac` lädt niemand. Das Format kann außerdem nur 15 skalare Ziele,
kein FOV, keine Sichtbarkeit, keinen Ereignis-Track, keine Audiospur, keinen Kameraschnitt. Zur
Cutscene selbst: die Bausteine sind alle da, Kamerafahrt per Skript, Dialog über `ui.setText` und
`audio.play`, Zeitkontrolle über `time.setTimeScale`. Was fehlt, ist ausschließlich das Autorieren.
Aufwand: L, das ist der Sequencer.

---

## 5. Die auffälligen Punkte

- **Ein Kollisionsereignis sagt nur, WER.** `CollisionEvent` trägt zwei Entity-Ids, kein
  Kontaktpunkt, keine Normale, kein Impuls, während `RaycastHit` direkt darunter Punkt und Normale
  sehr wohl kennt (`PhysicsWorld.h:15-29`). Keine Einschlagsdekale am richtigen Ort, kein
  richtungsabhängiger Rückstoß, kein Sturzschaden nach Aufprallwucht. Notlösung ist ein zweiter
  Raycast im Handler, der danebenliegen kann.
- **Ein Skript kann einer Entity keine eigenen Daten anheften.** Kein `addComponent`, kein
  Schlüssel/Wert-Fach; Trefferpunkte und Munition brauchen eine HorizonCode-Klasse plus `Cast`. In
  Lua und Python fehlt sogar der Weg, das Skript einer getroffenen Entity überhaupt anzusprechen.
  Ausnahme: native C++-GameLogic bekommt `HorizonWorld&` und könnte über die header-inline
  `addComponent`-Templates eigene Structs anhängen, das ausgelieferte Scaffold kann den Header aber
  nicht einmal einbinden (siehe unten).
- **Keine Bildschirmeffekte für Treffer.** Keine Vignette, keine Farbkorrektur, keine chromatische
  Aberration, keine Bewegungsunschärfe, keine Tiefenschärfe, kein eingebauter Kamera-Shake. Baubar
  sind heute schon: Blutspritzer-Overlay und Treffer-Blitz als UI-Widget mit Textur und Tint, und
  ein Rückstoß über `camera.addYawPitch`. Beides findet niemand von selbst.
- **Keine Trails, Ribbons oder Beams.** Kein Streifen-Renderer irgendeiner Art, die
  `ribbon`-Treffer im Code sind alle Aurora-Bänder im Himmels-Shader. Also kein Tracer, keine
  Raketenspur, keine Schwertspur; der Umweg über `debug.line` sieht aus wie ein Debug-Werkzeug.
- **3D-Audio bleibt bei linearer Dämpfung stehen.** Kein Doppler (miniaudio kann es, es wird nur nie
  eine Geschwindigkeit gesetzt), kein Reverb, keine Verdeckung durch Wände, kein Tiefpass. In einem
  Shooter ist Verdeckung Spielinformation, nicht Kosmetik. Dazu zwei tote Regler, die im Inspector
  stehen und nirgends ankommen: `AudioSourceComponent::rolloffFactor` und
  `AudioListenerComponent::masterVolume`.
- **Decals sind nur Albedo und zur Laufzeit nicht erzeugbar.** `roughness` ist ausdrücklich
  reserviert, das G-Buffer-Normal wird nicht angefasst, nasses Blut geht nicht; und es gibt keine
  Lebensdauer und keine Mengenbegrenzung. Ein Pool aus vorplatzierten Decals mit `setVisible` ist
  der gangbare Umweg, sobald B1 steht.
- **Kein Retargeting.** Ein Kanal adressiert sein Ziel über einen rohen `jointIndex`, der aus der
  Reihenfolge des ersten Skins der Quelldatei stammt; die Knochen haben Namen, beim Sampling werden
  sie nie benutzt. Eine gekaufte Animationsbibliothek passt nicht auf die eigene Figur, und der
  Fehlerfall ist keine Meldung, sondern ein verdrehter Charakter.
- **Der Animations-Importer verschluckt Kanäle still.** Alles außer LINEAR wird übersprungen, ebenso
  Kanäle außerhalb des ersten Skins und Morph-Gewichte, ohne Warnung
  (`AnimationClipImporter.cpp:131-145`). Ein Clip mit Bezier-Kurven aus Blender kommt teilweise an,
  und der Autor sucht den Fehler in seiner Zustandsmaschine.
- **Der Spielstand weiß nicht, in welchem Level er entstand.** Ein Save ist
  `{ id, templateRef, fields, entities }`, keine Szenen- oder Zonen-Sektion, und beim Weltwechsel
  wird die Zonentabelle ersatzlos verworfen. In HorizonCode, Lua und Python ist das mit drei
  Aufrufen (`loadedZones`, `zoneScene`, `zonePosition`) plus einem Container-Feld nachbaubar, für ein
  reines C++-Projekt gar nicht.
- **Keine Zerstörbarkeit und keine physikalischen Trümmer.** Kein Fracture, keine Bruchstücke, keine
  Constraints. Visuelle Trümmer per Transform gehen, fallende nicht.
- **Beleuchtung im Großen ist vollständig dynamisch.** Kein Lightmap-Backen, keine Reflection Probes,
  keine Irradiance-Volumen. Das ist eine bewusste Entscheidung zugunsten von DDGI, heißt aber, dass
  es keinen Weg gibt, Beleuchtungskosten in die Bauzeit zu verschieben, und keine Rückfallebene auf
  schwacher Hardware. Dazu sind Schatten nach wie vor Konstanten (`kCascadeCount = 3`,
  `kShadowDistance = 250`).
- **Keine Grafikstufen, keine dynamische Auflösung, keine Bildratenbremse im Spiel.** 24 Einzelwerte
  ohne Preset-Begriff, kein `MaxFps`-Schlüssel im Export, obwohl der Limiter fertig ist; ein
  ausgeliefertes Spiel mit VSync aus läuft ungebremst. Und die Regler, die auf schwacher Hardware am
  meisten brächten, Schattenreichweite, Schattenauflösung, Texturqualität und Sichtweite, sind gar
  keine Schlüssel.
- **Ein Patch geht nur über den Mod-Schalter, und der muss beim ersten Export gesetzt gewesen sein.**
  Der Overlay-Mechanismus ist gut gebaut, er hängt nur an einem Flag, das im Shipping-Profil aus ist
  und in die ausgelieferte hcfg eingebrannt wird. Wer v1.0 ohne den Haken veröffentlicht hat, liefert
  v1.1 als Voll-Download. Es gibt außerdem keinen Updater und keine Versionsprüfung.
- **Gameplay-Replikation: die Bibliothek ist fertig, es fehlen vier Anschlüsse.** Keine
  Replikations-Komponente (überlebt also kein Speichern), keine `NetSession` für Gameplay, keine
  Registry-Gruppe, und kein Startweg, denn das Spiel wertet die Kommandozeile überhaupt nicht aus.
  Letzteres blockiert nebenbei auch `-windowed` und `-width`.
- **C++ als Projektsprache: das Scaffold kommt an die Welt nicht heran.** `IGameLogic::onStart`
  bekommt `HorizonWorld&` per Referenz und die `addComponent`-Templates sind header-inline, das
  Scaffold-CMake hängt aber nur `HE_Core/include` ein (`CppScaffoldTemplates.cpp:314`), während
  `HorizonWorld.h` in `HE_Scene/include` liegt und `entt` aus `HE_Scene/vendor` zieht. Der volle
  ECS-Zugriff ist also vorgesehen und real, aber ohne selbst nachgetragene Include-Pfade nicht
  einmal kompilierbar; die injizierte C-ABI-Tabelle kennt nur Savegames und `findEntityByName`.
- **Barrierefreiheit kommt nicht vor.** Keine Untertitel für Audio, keine UI- oder Textgröße für den
  Spieler, keine Farbenblindheits-Modi, kein Halten statt Drücken, keine Reduktion von
  Kamerawackeln, keine Screenreader-Schnittstelle; Suchen über `src/` ohne vendor nach `colorblind`,
  `uiScale`, `screenreader` und `subtitle` liefern im Laufzeitcode null Treffer. Das ist eine
  Weiche, keine Forderung: solange der Font-Atlas ohnehin auf UTF-8 umgebaut wird und eine
  Einstellungs-API entsteht, beides steht in Welle 4, kosten ein Untertitel-Kanal und eine UI-Skala
  fast nichts. Danach kosten sie einen Umbau. Für eine Konsolen-Zertifizierung ist
  Untertitel-Unterstützung außerdem Pflicht, nicht Kür.
- **Es gibt keine spielbare Projektvorlage.** Die fünf Presets (`ProjectHubPanel.h:18-31`) sind
  Ordnergerüste, im besten Fall plus "a furnished scene (sky, ground, cube, light)". Es gibt keine
  Third-Person- und keine First-Person-Vorlage mit fertigem PlayerController, Charakter,
  InputMappingContext, CameraRig und Startpunkt. Ein Anfänger muss den Spieler komplett selbst
  zusammensetzen: Controller-Klasse anlegen, Charakter im `BeginPlay` per `Create Object` spawnen,
  `player.possess` rufen, Actions und Mappings anlegen, das CameraRig setzen, eine Startposition
  erfinden. Eine Konvention dafür gibt es nicht und ein Beispiel auch nicht. Das Gewicht des Punktes
  liegt aber woanders: eine Vorlage, die die Engine selbst mitliefert und deshalb bei jedem Umbau
  mitlaufen muss, hätte B1, B3, B4 und B5 am ersten Tag aufgedeckt, statt sie zwölf Blocker lang
  liegen zu lassen, bis ein Audit sie findet. Sie ist damit die billigste Gegenmaßnahme gegen das
  Muster, das Abschnitt 7 beschreibt.

---

## 6. Empfohlene Reihenfolge

Nicht nach Schwere sortiert, sondern danach, was am meisten freischaltet, was woran hängt und was
billig ist und viel wegräumt.

**Welle 1, klein und räumt viel weg.**

1. **B5, Typ-Feld in den Pak-Index (S).** Zuerst, weil ein ausgeliefertes HorizonCode-Spiel ohne ihn
   in eine leere Welt startet: kein PlayerController, kein `BeginPlay`, also auch kein Charakter.
   Der Fehler bleibt im Editor unsichtbar, und der Fix folgt einem bestehenden Muster
   (`__type_index__`). Abnahmekriterium ist deshalb nicht "die Tasten reagieren", sondern "der
   PlayerController wird instanziiert und sein `BeginPlay` läuft"; wer nur die Eingabehälfte prüft,
   hakt den Punkt ab, während die Klassenentdeckung weiter nichts findet. Ein Tag Arbeit gegen einen
   Totalausfall.
2. **Absturz-Handler im Spiel scharf schalten (S).** Eine Zeile in `src/HE_Game/src/main.cpp`, und
   macOS und Linux haben ab sofort Absturzdumps beim Spieler. Der Windows-Zweig kann warten. Was er
   nicht abdeckt, steht in Abschnitt 4 unter der Laufzeit-Diagnose: unterhalb eines Absturzes hat
   das ausgelieferte Spiel kein Werkzeug, das ein Tester bedienen könnte.
3. **Trigger ohne RigidBody nicht mehr stumm (S).** Automatisch beilegen, wie es "Camera Rig" drei
   Zeilen darüber schon tut. Räumt die häufigste stille Fehlersuche des Anfängers weg.
4. **Tick-Reihenfolge im Editor angleichen (S).** Solange Vorschau und Build verschieden ticken, ist
   jede Messung an Bewegung und Sprung wertlos, und die halbe Sprung-Verwirrung verschwindet mit.
5. **Licht-Fenster nach Distanz sortieren (S).** Ein `partial_sort` gegen ein Flackern, das wie ein
   Bug aussieht.

**Welle 2, das Fundament.**

6. **B1, `addBody`/`removeBody`/`setPosition` (M).** Der größte Hebel im Dokument. Er schaltet in
   einem Zug frei: Spawnen mit Körper, Respawn und Checkpoints, Portale und Aufzüge, das Aufräumen
   toter Körper, begehbare gestreamte Zonen, physikalische Trümmer, und er ist die Voraussetzung
   dafür, dass B3 und B6 überhaupt etwas nützen. Das Körperbudget gleich mitkonfigurierbar machen.
7. **B3, Klassen-Spawn in die Registry (M).** Direkt danach, weil es B1 vervielfacht und der
   Mechanismus schon existiert: eine Zeile, die `EntityHost::spawn` erreichbar macht, plus
   `addComponent`, gibt Lua, Python und dem Codegen zum ersten Mal eine bevölkerbare Welt.
8. **B2, Heightfield für Terrain, danach Mesh-Collider (M, dann L).** Unabhängig von 6 und 7 und der
   erste Blocker, auf den ein Autor in der ersten Stunde stößt. Die Terrain-Hälfte zuerst, sie ist
   billiger und trifft jedes Außenlevel; Mesh und Convex Hull danach für importierte Levelgeometrie.
   In dieselbe erste Stunde fällt die optische Hälfte desselben Hauses, Submeshes und
   Material-Slots (XL). Die gehört nicht in diesen Handgriff, muss aber hier in der Planung stehen,
   sonst fehlt der einzige XL-Posten, den ein Autor schon beim ersten importierten Modell trifft.
9. **Sprung plus Ducken plus bewegte Plattformen (M, S, S).** Sobald 6 steht, sind das kleine
   Ergänzungen an derselben Datei, und sie sind der Unterschied zwischen "Charakter" und
   "Techdemo".
10. **Interpolation zwischen den Physikschritten (M).** Gehört zu 6, weil beide am Writeback
    hängen: ein zweiter Transform pro Körper plus ein Alpha im Extractor. Vorher sieht auf einem
    144-Hz-Bildschirm jede Bewegungsarbeit aus 9 schlechter aus, als sie ist, und jede Beurteilung
    von Spielgefühl misst das Ruckeln mit.
11. **Assets beim Zonen-Entladen freigeben (S, dann M).** Erst hier, weil es vorher niemand lange
    genug spielt, um es zu bemerken; danach ist es die Bedingung dafür, dass eine gestreamte Welt
    eine lange Sitzung übersteht. Ein `unloadZoneAssets` auf den bestehenden AssetRef-Pins ist der
    kleine Schritt, ein Budget mit LRU der große. Die Audio-Stimmen fallen in denselben Handgriff.

**Welle 3, die Welt wird lebendig.**

12. **B4, Nav-API plus Velocity-Route plus `moving` serialisieren (M).** Erst jetzt sinnvoll, weil
    der Agent über den Character Controller fahren soll, den 6 erst wirklich benutzbar macht. Die
    drei Teile gehören in einen Handgriff, sonst hat man eine API, die im Build nichts tut.
13. **Tags, Teams und eine Weltabfrage (M), danach Kollisions-Layer (M).** Hängt an nichts, wird
    aber von allem gebraucht, was nach 12 kommt: Freund und Feind, "nur der Spieler löst aus",
    Geschosse, die den Schützen verfehlen, Weltzustand speichern.
14. **B6, `particle.play`/`stop`/`burst` plus korrektes `looping` (M).** Nach 7, weil der schönere
    Weg der gespawnte Effekt ist. Das große Vokabular (Formen, additiv, Flipbooks) danach separat.
15. **Regel-Ort mit Tick (M) und Timer mit Handle (M).** Zusammen, weil Rundenuhr, Wellen und
    Cooldowns dieselbe Baustelle sind.

**Welle 4, Produkt statt Prototyp.**

16. **Einstellungs-API (M).** Billiger als sie aussieht, weil die Werte schon jeden Frame gelesen
    werden; danach ist ein Optionsmenü Skriptarbeit. Tastenbelegung im selben Zug.
17. **UTF-8 im Font-Atlas (M), Splash (S), Exe-Name, Icon und Version (M).** Die drei Dinge, die
    zwischen "Build" und "Produkt" stehen. Hier fällt zugleich die Entscheidung über
    Barrierefreiheit: ein Untertitel-Kanal und eine UI-Skala sind neben diesem Umbau und neben 16
    fast geschenkt und danach ein eigener Umbau.
18. **Audio aufräumen (S) und Drop-Slot (S), danach OGG und die fehlenden Registry-Zeilen (M).**
19. **Animation: Notifies und `animator.play` (je M), danach Root Motion (M), Layer und Blend Spaces
    zuletzt (XL).** In dieser Reihenfolge, weil die ersten beiden sofort Spielgefühl liefern und die
    letzten das Datenmodell umbauen.
20. **Nach Bedarf gezogen:** Sequencer und `PropertyAnimClip` ladbar machen (L), Foliage-Layer (L),
    Occlusion Culling (XL), IK (L), Ragdoll und Constraints (XL), Cross-Platform-Runtimes (M),
    Submeshes und Material-Slots (XL), Lokalisierung (L), Gameplay-Replikation anschließen (L).

Quer zu allen vier Wellen liegt die spielbare Projektvorlage aus Abschnitt 5. Sie ist kein Posten
in dieser Liste, sondern der Prüfstand für sie: wer sie mitliefert und mitpflegt, merkt jeden dieser
Punkte an dem Tag, an dem er entsteht, statt in einem Audit.

---

## 7. Was überrascht hat

**Positiv.**

- **Level-Streaming existiert vollständig.** Zonen laden, verstecken, positionieren, entladen, ein
  Hintergrund-Preload mit `activate` für nahtlose Übergänge, und die App-UI überlebt den
  Szenenwechsel. "Level-Streaming fehlt" wäre ein Falschbefund gewesen; was fehlt, ist die Physik
  darin und der Rückweg, denn das Entladen einer Zone gibt kein einziges Asset frei.
- **Die Gameplay-API ist deutlich breiter, als der Vor-Audit sagt.** Kräfte, Impulse, Drehmoment,
  `overlapSphere`, `sphereCast`, Gravitation, `timeScale`, Eingabemodi, `pointerOverUI`, `quit`,
  Speicherstände, und fast alles davon automatisch in Lua und Python.
- **Lua und Python können mehr als angenommen.** Die ganze `player`-Gruppe ist Skriptgruppe,
  `possess` und `locomotion.move` funktionieren, ein Lua-Skript kann heute einen Charakter fahren
  und besitzen. Nur Action-Ereignisse bekommt es nicht.
- **Die Fehlerdiagnose der Navigation ist vorbildlich.** Gedrosselte Klartext-Warnungen mit
  Koordinaten, ein Zellenbudget gegen den Editor-Freeze und eine Meldung, die die zu ändernde
  Einstellung beim Namen nennt. So sollte der Rest der Engine auch reden.
- **Der Character Controller ist echt.** Jolt `CharacterVirtual` mit Stufen, Hangbegrenzung und
  mitgezogenem Proxy, kein handbewegter Rigidbody. Genau der Punkt, an dem viele Engines einen Geist
  am Spawnpunkt stehen lassen.
- **Decals gibt es, entgegen dem Plan.** Und der Deferred-Plan führt sie sogar korrekt als erledigt,
  nur der SSR-Plan wurde zu Unrecht dafür verantwortlich gemacht.

**Negativ.**

- **Die teure Hälfte ist gebaut und nicht angeschlossen, und zwar reihenweise.** Recast komplett,
  aber ohne API und ohne serialisiertes `moving`. `GameReplication` komplett, ohne einen einzigen
  Aufrufer. `EntityHost::spawn` komplett, ohne Registry-Zeile. Der Splash-Screen komplett, im Spiel
  nie belegt. `PropertyAnimClip` komplett, in keinem Loader-Dispatch. Die halbe AudioEngine
  komplett, nur vom Editor benutzt. Das ist kein Zufall mehr, sondern ein Muster: gebaut wird
  bis zur Testbarkeit, angeschlossen wird beim ersten echten Anwendungsfall, und der kam nie. Genau
  deshalb wiegt die fehlende spielbare Projektvorlage schwerer als ihre Schwere: sie wäre der erste
  echte Anwendungsfall, sie wäre billig, und sie hätte B1, B3, B4 und B5 am ersten Tag gemeldet.
- **Kommentare, die aktiv lügen.** Das ist die Umkehrung der Lehre vom 25.08., wo die Lücken
  wenigstens ehrlich als "yet"-Kommentar im Code standen. `GameApplication.cpp:554-556` behauptet,
  ein Spawn im BeginPlay bekomme einen Body. `NavAgentComponent.h:9` behauptet, der Pfad werde beim
  Zielwechsel zurückgesetzt. `SceneSystems.cpp:99-102` behauptet, Movement lande vor dem
  Physikschritt. `EngineApi.cpp:263-264` behauptet, Physik und Hierarchie hörten den Transform ab.
  Alle vier sind falsch, und alle vier stehen an genau der Stelle, an der jemand den Fehler suchen
  würde.
- **Ein Test, der grün ist, weil das Feature kaputt ist.** `tests/test_particles.cpp:184-204` prüft,
  dass ein nicht-schleifender Emitter aufhört, und besteht, weil er sich im ersten Frame selbst
  abschaltet, bevor ein Partikel entstanden ist. Der Test prüft `!playing` und nie, ob überhaupt
  etwas emittiert wurde.
- **Die Editor-Vorschau ist an drei Stellen etwas anderes als das Spiel**, und keine davon meldet
  sich: verworfene Szenenwechsel, stumme Zonen-Skripte, andere Tick-Reihenfolge. Genau die Falle,
  die dieses Projekt schon mehrfach getroffen hat.
- **Navigation und Physik sehen verschiedene Welten.** Der NavMesh-Bake sammelt die Terrain-Chunks
  ein, die Physik nicht. Der Agent läuft über eine Landschaft, durch die der Spieler fällt. Zwei
  Systeme, dieselbe Geometrie, gegensätzliche Antwort.
- **Ein Text-Renderer mit 96 Glyphen.** Dass die Engine ihre eigene Entwicklersprache nicht
  darstellen kann, war der unerwartetste Einzelfund. Immerhin fällt es schon im Editor auf.

---

## 8. Wie dieses Dokument entstanden ist

Sechs Bereiche wurden getrennt am Code abgesucht, nicht an den Plänen. Jeder Fund ging danach an
einen zweiten Durchgang, dessen Auftrag war, ihn zu **widerlegen**: den Code zu finden, der es
doch kann. Das ist die Lehre aus dem Vorgänger-Audit vom 25.08.2026, in dem acht längst erledigte
Dinge als offen in den Plänen standen.

Zuletzt hat eine Vollständigkeitsprüfung das fertige Dokument gegen den Code gelesen. Sie hat eine
Behauptung gekippt, die an der prominentesten Stelle stand: B5 war als "die Eingabe ist leer, ein
Tag Arbeit" auf Platz eins der Reihenfolge geführt. Beides war falsch. Rohe `input.keyDown`-Abfragen
funktionieren im Build sehr wohl, und es fehlt nicht die Eingabe, sondern der PlayerController
selbst. Der Fix ist derselbe geblieben, das Abnahmekriterium nicht. Sechs weitere Befunde kamen
dazu, die zwischen den sechs Bereichen durchgefallen waren; zwei davon, die fehlende Interpolation
zwischen den Physikschritten und die nie freigegebenen Assets, tragen einen eigenen Eintrag in der
Reihenfolge, die deshalb 20 statt 18 Nummern hat.

Was daraus für den nächsten Leser folgt: eine Zahl in diesem Dokument ist belegt, eine Wirkung ist
eine Auslegung. Die Belege mit Datei und Zeile sind zweimal geprüft, die Sätze darüber, was ein
Befund für einen Spielautor bedeutet, einmal.
