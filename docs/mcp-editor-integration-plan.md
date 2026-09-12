# MCP-Integration im Editor: Objekte platzieren, bewegen, HorizonCode autoren

Plan für die Fernsteuerung des laufenden HorizonEditors durch externe
MCP-Clients (Claude Code, Claude Desktop, jede andere Instanz, die das Model
Context Protocol spricht): Entities anlegen, bewegen, umhängen, löschen,
Komponenten setzen, HorizonCode-Graphen bearbeiten. Das ist Schritt 1 des
Themas: Orientierung, Entscheidungen und der Zuschnitt der Folgeschritte.
Kein Feature-Code.

Branch: `claude/mcp-editor-integration`. Kein Merge nach main aus diesem Zweig
heraus.

Die Kernaussage vorweg, weil alles Weitere daran hängt: **der Editor hat heute
keinen zentralen Weg, auf dem eine Szenenänderung hereinkommt.** Drei Pfade
mutieren die Welt (UI-Handler, Collab-Remote-Handler, EngineApi-Registry), und
jeder ist einzeln an Undo und Collab verdrahtet oder eben nicht. Ein vierter
Pfad (MCP) daneben würde die Lücke vergrößern. Deshalb baut Schritt 2 zuerst
das Command-Gateway, und MCP wird sein erster externer Nutzer, nicht sein
Anlass für noch eine Sonderverdrahtung.

---

## 1. Was heute steht (Kartierung)

### 1.1 Frame-Reihenfolge, und wo ein externer Befehl landen kann

`EditorApplication::OnRender` (`src/HE_Editor/EditorApplication.cpp:1943`)
pumpt pro Frame in dieser Reihenfolge:

```
… Renderer-Settings, Play-Tick …
m_collab.update(nowMs)                         (:3508)  ← Transport → Session → Callbacks
m_git.update(nowMs)
if inSession: syncStructuralChanges()          (:3512)  ← Entity-Set-Diff → publishCreate/Destroy/Reparent
if inSession: updateAssetCollabSync(nowMs)               ← DocMirror-Diff der offenen Tabs
if inSession: followSelection + publishTransform + publishComponents  (:3520–3585, NUR m_selectedEntity)
setLocalPresence
AppContext ctx = makeContext(); EditorUI::render(ctx, dt)   (:3606)
saveOpenTabs()
```

Alles ist poll-getrieben und läuft auf dem Hauptthread. Ein externer Befehl
muss **nach** `m_collab.update` und **vor** `EditorUI::render` angewandt
werden: dann sieht ihn der Struktur-Diff im selben Frame, die Extraktion holt
die neue Transform, und die UI zeichnet den Stand, den der Client gerade
bestätigt bekommt. Das ist dieselbe Stelle, an der heute Peer-Edits ankommen.

### 1.2 Die drei Mutationspfade und ihre Undo-Verdrahtung

| Pfad | Wo | Undo außerhalb Session | Undo in Session | Publish an Peers |
|---|---|---|---|---|
| UI-Strukturbefehle (`duplicateSelectedEntity`, `copySelectedEntity`, `pasteEntityClipboard`, `deleteSelectedEntity`) | `EditorApplication.cpp:5943–6047` | `if (!m_isPlaying) m_undo.snapshotNow()` je Aufrufstelle | **keins** (`CollabUndo::Kind` kennt nur Transform/Asset) | indirekt über `syncStructuralChanges` (Entity-Set-Diff) |
| UI-Kontinuierlich (Gizmo, Inspector-Felder) | `InspectorPanel.cpp:159–160`, Gizmo | `capturePre/stashPre/commitPending` (Snapshot) | `m_collabUndo.recordTransform` aus dem Selektions-Baseline-Vergleich (`:3544–3562`) | `publishTransform`/`publishComponents`, **nur für `m_selectedEntity`** |
| Collab-Remote (`onRemoteCreate/Components/Destroy/Reparent/Transform`) | `EditorApplication.cpp:351–410, 660–680` | bewusst keins (fremde Edits) | keins | nein (Echo-Schutz über `m_structureKnown`) |
| EngineApi-Registry (`HE::api`, 580 Zeilen, `transform.setPosition` usw.) | `src/HE_Scene/src/EngineApi.cpp` | **keins** | **keins** | **nein** |
| Panels für Graph-Assets (Level Script, Klasse, Widget, Material, Partikel, Animator) | `LevelScriptPanel.cpp:1420` (Level Script: `undoSys->snapshotNow()`), Klassen-Panel `:1987` **`/*undo=*/nullptr`** | Level Script über den Szenen-Snapshot; Klassen-Panel hat **kein** Undo | `CollabUndo::Kind::Asset` (ganze Datei) | `DocMirror`-Diff je offenem Tab (`CollabDocSync`) |

`EditorUndo` (`src/HE_Editor/EditorUndo.h`) ist ein Ganzwelt-Snapshot-Stack
(CBOR, 64 Einträge). `restore` **remappt Entity-Handles**: jeder Handle, den
ein externer Client hält, ist nach einem Undo tot. `CollabUndo`
(`src/HE_Editor/CollabUndo.h`) ist ein Inverse-Op-Stack pro Nutzer, gültig nur
solange der Lock auf dem Subjekt gehalten wird (`dropUnowned`). Die beiden
schließen sich aus: außerhalb einer Session `m_undo`, in einer Session
`m_collabUndo` (`makeContext` reicht `.undoSys` nur außerhalb Play durch).

### 1.3 Collab: Publish und Locks sind an die Selektion gebunden

Der Punkt, den man leicht übersieht: `publishTransform`, `publishComponents`
und die CollabUndo-Baseline laufen pro Frame **ausschließlich für
`m_selectedEntity`** (`:3520–3585`). Ebenso hält `followSelection`
(`CollabController.h:864`) **genau einen** Lock und gibt den vorigen frei.

Für MCP heißt das: ein Edit an einer nicht selektierten Entity würde lokal
angewandt und **nie** veröffentlicht, und ein Lock, den MCP holt, würde der
nächste Klick des Nutzers wieder abgeben. Beides braucht das Gateway eigens
(siehe 2.4). Strukturänderungen sind die Ausnahme: `syncStructuralChanges`
diffst das Entity-Set und ist vollständig „by construction", ein zweites
Publish von Create/Destroy/Reparent wäre eine Duplikation.

Lock-API, wiederverwendbar: `requestLock(subject)` (asynchron, Host-Roundtrip),
`releaseLock`, `ownsLock`, `lockFor` (`CollabController.h:856–859`); Assets
über `requestAssetLock/ownsAssetLock/assetLockedByOther` (lazy, optimistisch,
Deny-Callback flippt den Tab read-only).

Netz-Identität: `subjectFor(handle)` leitet die Wire-ID aus der
`EntityIdComponent`-UUID ab (`CollabController.h:474`), `entityForNetId` geht
zurück. Das ist bereits die stabile Adresse, die MCP braucht.

Remote-Handler, die als Anwendungs-Code wiederverwendbar sind:

| Handler | Was er tut | Zeile |
|---|---|---|
| `onRemoteCreate` | `SceneSerializer::instantiatePrefab(world, blob, parent, preserveIds=true)` + `markSubtreeKnown` | `:351` |
| `onRemoteComponents` | `applyEntityComponents` + `tc->dirty = true` | `:379` |
| `onRemoteDestroy` | Selektion räumen, `m_structureKnown.erase`, `destroyEntity` | `:392` |
| `onRemoteReparent` | `reparentEntity` (0 = Root) | `:401` |
| `onRemoteTransform` | Position/Rotation/Scale + `dirty` | `:660` |

### 1.4 Serialisierung: CBOR ist JSON, und das ist die halbe MCP-Nutzlast

`SceneSerializer::serializeEntityComponents` (`SceneSerializer.cpp:2072`) ist
`json::to_cbor(serializeComponents(reg, e))`, also **exakt das JSON, das die
`.hescene` schreibt**, nur binär. `applyEntityComponents` (`:2086`) macht
`from_cbor` und ruft `applyComponents`, das nur die **vorhandenen** Schlüssel
überschreibt (Update, kein Replace). Folge: das MCP-JSON einer Entity ist
`json::from_cbor(serializeEntityComponents(e))`, und „setze Feld X von
Komponente Y" ist ein partieller Merge in dieses JSON, `to_cbor`,
`applyEntityComponents`. **Kein zweiter Serializer.** `isKnownComponentKey`
(`SceneSerializer.h:~70`) liefert die Liste gültiger Komponentenschlüssel für
das Tool-Schema.

Subtrees: `serializeSubtree`/`instantiatePrefab` (CBOR-Blob, Handles
kontiguierlich remappt, `preserveIds` für Collab).

Weltpositionen: `TransformComponent::worldMatrix` schreibt nur
`propagateTransforms`; in der Befehlsphase gilt `HE::worldPositionOf`/
`worldMatrixOf`, nie `tc.worldMatrix` (viermal in diesem Repo aufgetreten).

### 1.5 HorizonCode: Graph, Item-JSON, Adapter, Panel-Zustand

`HorizonCode::Graph` (`src/HE_Core/include/HorizonCode/HorizonCode.h:568`):
`nodes/links/variables/events`, `addNode(Node)` (vergibt `id` aus `nextId`),
`removeNode`, `connect(src, srcPin, dst, dstPin)` (validiert, ersetzt belegte
Exec-Out/Data-In), `connectWithConversion` (`:800`, spawnt Konvertierungsknoten),
`toJson/fromJson`, und **Item-Level**: `nodeToJson/nodeFromJson`,
`variableToJson/variableFromJson` (`:656–660`), in exakt der Form der
Dokument-Arrays. Links sind 4-int-Arrays ohne eigene ID.

Pin-Adressierung: unified Index `[execIns][execOuts][dataIns][dataOuts]`,
`HcGraphHost::pinRanges(n)` und `nodePins(n)` (`HcGraphHost.h:38–52`) liefern
Layout und Labels. **Ohne diese Information kann kein Client `connect`
benutzen**, also muss ein Tool sie pro Knoten liefern.

Knotenkatalog: `NodeType`-Enum (`HorizonCode.h:152 ff.`, u. a. `Event`,
`FunctionCall`, `Const*`, `EngineCall` mit `s = Registry-ID`), `engineEvents()`
und `engineClasses()` (`HorizonCode.cpp`), API-Zeilen aus `HE::api::registry()`.
Menü-Filter je Frontend: `HcGraphHost::MenuOpts` (`addExcluded`,
`apiGroups`).

Collab-Adapter: `CollabDocSync::forHorizonCodeGraph(g)` (`CollabDocSync.h:151`)
mit `upsert(Kind, id, json)`, `remove`, `reorder`, `afterApply` (Pin-
Reconcile). Das ist bereits der **Apply-Pfad** für eine Item-Änderung von
außen. Wendet MCP über den Adapter an, sieht der `DocMirror`-Diff des Panels
die Änderung im selben Frame als lokalen Edit und veröffentlicht sie an die
Peers, ohne dass MCP von Collab weiß.

Wo Graphen leben:

| Graph | Besitzer | Persistenz | Undo heute |
|---|---|---|---|
| Level Script | `HorizonWorld::levelScript()` (Teil der Szene) | `.hescene` | Szenen-Snapshot (`LevelScriptPanel.cpp:1420`) |
| Game Instance | `AppContext::gameInstanceGraph` + `commitGameInstance` | Projekt-Asset | Szenen-Snapshot-Revision |
| HorizonCode-Klasse | `ClassState` im Klassen-Panel (`LevelScriptPanel.cpp:1471`, `graph`, `collabMirror`, `dirty`, `path`) | `.hasset` (`AssetType::HorizonCodeClass`, `RuntimeAsset::graphJson`, `saveClassState :1622`) | **keins** (`:1987`) |
| Widget-Logik | `UIEditorPanel` (Scope `LogicGraph`) | `.hasset` (`AssetType::Widget`) | eigener Widget-Undo-Stack |

Ein Asset kann **offen** (Tab, Live-Dokument im Panel) oder **geschlossen**
(nur auf Platte) sein. Beides muss ein Tool bedienen, sonst hängt das
Verhalten davon ab, was der Mensch gerade angeklickt hat.

### 1.6 EngineApi-Registry

`HE::api::ApiFn` (`EngineApi.h:2001`): `id`, `category`, `isExec`,
`params/results` (typisiert), `cppCall`, `invoke(Ctx&, Values)`,
`displayName`. `registry()` wird an fünf Stellen introspiziert
(`ScriptContext.cpp:834`, `PyScriptBackend.cpp:775`, `HcEditorUtil.cpp:1294`,
`HcNodeReference.cpp:183`, `HcGraphHost.cpp:1207`). Die Tabelle ist direkt in
ein MCP-Tool-Schema übersetzbar.

Aber: die Registry liegt in **HE_Scene**, unterhalb von HE_Editor, und
`transform.setPosition` schreibt direkt in die Komponente (plus
Physik-Teleport). Sie weiß nichts von Undo, Locks oder Publish. Und die
meisten Exec-Zeilen (`scene.load`, `entity.spawnClass`, Physik) brauchen
Play-Services im `Ctx`, die im Edit-Modus nicht existieren. Ein generischer
„rufe Registry-Zeile X" wäre genau die Umgehung, die das Thema benennt.

### 1.7 Netz-Sockel

`HE::Net::TcpTransport` (`src/HE_Net/include/Net/TcpTransport.h`):
`listen(port)` / `connect`, `update()`/`poll()`/`send()`, nicht-blockierend,
Framing `[uint32 BE Länge][Payload]`, `kMaxFrameSize` 64 MiB, Hauptthread.
**Bindet aber `in6addr_any`/`INADDR_ANY`** (`Socket.cpp:217, 447` über
`socketCreateListenerDualStack`, `TcpTransport.cpp:37`). Für „nur lokal"
fehlt eine Loopback-Variante, das ist ein Touch-Point in HE_Net.
`LoopbackTransport::createPair()` deckt In-Prozess-Tests ab. Es gibt keinen
HTTP-/WebSocket-Server im Baum (nur `HttpClient/HttpsClient`).

### 1.8 Konfiguration, Opt-in-Vorbild, Tests

* Opt-in-Vorbild: `EditorConfig::CollabLanDiscovery` (`EditorApplication.h:77`),
  gelesen `:1108` (`getCustomConfigBool`), geschrieben `:7308`
  (`setCustomConfigEntry`), Checkbox `EditorSettingsPanel.cpp:441`, pro Frame
  in den Controller gedrückt (`:3500`). Persistenz in
  `~/Library/Application Support/HorizonEngine/config.json`
  (`GlobalState.cpp:121, 144`), plattformäquivalent auf Win/Linux. **Das ist
  der Ort für Token und Endpunkt-Datei, nie das Projektverzeichnis** (das
  landet in Git).
* Umgebungsvariablen-Muster: `HE_DUMP_*` (`EditorApplication.cpp:255 ff.`),
  `HE_COLLAB_OFFLINE`, `HE_COLLAB_DIRECTORY`.
* Jede neue Preferences-Checkbox braucht einen `EditorHelp`-Eintrag, sonst
  fällt der Deckungs-ctest (`editor_help_audit`, `scripts/editor_help_audit.py`).
* Tests: doctest, `tests/CMakeLists.txt` listet HE_Editor-Quellen **einzeln**
  (`:144–192`: `EditorUndo.cpp`, `CollabController.cpp`, `CollabUndo.cpp`,
  `CollabDocSync.cpp`, …). `EditorApplication.cpp` ist **nicht** im
  Testbinary. Neue `tests/test_*.cpp` registrieren sich über die
  `--source-file`-Schleife (`:490`) von selbst. Vorbilder:
  `test_collab_controller.cpp` (zwei Controller über echtes TCP,
  `pumpUntil`), `test_collab_undo.cpp` (Harness mit Handler-Lambdas),
  `test_collab_docsync.cpp`.
* Python-Seite: das Geschwister-Werkzeug `~/VSCode/horizon-web-mcp` läuft auf
  `mcp>=2.0.0` (`MCPServer`, nicht `FastMCP`); der System-Python hat das
  Paket nicht, das Shim braucht also ein venv oder `uv run`.

---

## 2. Entscheidungen

### 2.1 Transport: TCP-Server im Editor plus stdio-Shim

**Entscheidung:** Der Editor öffnet, wenn eingeschaltet, einen **lokalen
TCP-Listener auf 127.0.0.1** (OS-Port oder fester Port aus der Konfiguration)
und spricht darüber JSON-RPC 2.0. MCP-Clients erreichen ihn über ein dünnes
**stdio-Shim** `scripts/he_mcp.py` (Python, `mcp>=2`), das der Client als
Subprozess startet. Das Shim kennt **keine Tools**: es fragt den Editor
(`tools/list`) und registriert, was zurückkommt, jede Anfrage wird 1:1
durchgereicht. Neue Tools sind damit reine C++-Arbeit.

Verworfen, mit Grund:

* **Editor als stdio-Subprozess des Clients.** MCP-Clients starten Server als
  Kindprozesse. Der Editor ist eine GUI-App, die der Mensch startet und
  weiterbenutzt; sie kann nicht Kind eines Claude-Prozesses sein, und ein
  zweiter Editor-Prozess sähe eine andere Welt. Dazu die PATH-Falle: eine
  aus dem Finder gestartete .app erbt ein minimales PATH ohne Homebrew
  (siehe `ensureToolPathAugmented()`), ein aus ihr gestartetes Python fände
  sein `mcp`-Paket nicht.
* **Streamable-HTTP/SSE direkt in C++.** Es gibt keinen HTTP-Server im Baum;
  ihn zu schreiben, inklusive Session-Handling und Chunking, kostet mehr als
  das ganze Gateway, für einen Vorteil (kein Python), der lokal nicht zählt.
  Bleibt als späterer Ersatz des Shims offen (Schritt 7, optional).
* **MCP-Client tritt als Collab-Peer bei.** Verlockend, weil
  `CollabSession` schon Entity-Mutationen kennt. Aber der Client müsste ein
  vollständiger Peer sein (Snapshot-Empfang, BitStream-Binärprotokoll, Locks,
  Präsenz), hätte eine zweite Weltkopie, würde als Teilnehmer in der Liste
  stehen, und es ginge nur, wenn eine Session läuft. Die Wiederverwendung
  passiert stattdessen **innerhalb** des Editors: MCP-Befehle laufen durch
  dieselben Apply-Funktionen wie Peer-Edits.

Framing: das vorhandene `TcpTransport`-Framing (4-Byte-Länge, Payload =
UTF-8-JSON-RPC-Objekt). Das spart neuen Socket-Code, bringt die
Größenschranke gratis, und ist in Python drei Zeilen `struct.pack`. Notwendige
Ergänzung: `TcpTransport::listenLocal(port)` bzw. ein Bind-Parameter, weil
`listen` heute alle Interfaces bindet (1.7).

### 2.2 Protokoll und Tool-Schema

* JSON-RPC 2.0, eine Anfrage pro Frame-Batch: alle im Frame angekommenen
  Anfragen werden **nacheinander auf dem Hauptthread** ausgeführt, Antworten
  gehen im selben Frame raus. Kein Threading, keine Locks, keine
  Halbzustände zwischen Frames.
* Methoden auf Editor-Seite: `auth`, `tools/list`, `tools/call`, `ping`,
  `events/poll` (später für Benachrichtigungen; v1 nicht).
* Jedes Tool beschreibt sich selbst (`name`, `description`, `inputSchema` als
  JSON-Schema) in einer **Tool-Registry im Editor** (`McpToolRegistry`), aus
  der auch der Handbuch-Eintrag und die Tests lesen. Ein Tool ohne Schema
  gibt es nicht.
* Adressierung **nur über UUIDs** (`EntityIdComponent`), nie über Handles
  (1.2: Undo remappt Handles). Assets über content-relative Pfade.
* Fehler sind JSON-RPC-Fehler mit sprechendem Code: `not_found`,
  `locked_by_other`, `play_mode`, `builtin`, `invalid_payload`,
  `refused_by_policy`. Ein Client muss unterscheiden können, ob er es noch
  einmal versuchen soll.

### 2.3 Sicherheitsmodell

1. **Standard aus.** `EditorConfig::McpServerEnabled = false`, persistiert wie
   `CollabLanDiscovery`. Umgebungsvariable `HE_MCP=1` (+ `HE_MCP_PORT`) für
   Headless-Läufe und Tests, sonst nichts.
2. **Nur Loopback.** Bind auf `127.0.0.1` (und `::1`), nie auf ein Interface.
   Keine Port-Freigabe, kein LAN-Beacon, kein Directory-Eintrag.
3. **Token.** Beim Start des Listeners wird ein 32-Byte-Zufalls-Token
   erzeugt und mit Port und PID in eine Endpunkt-Datei
   `<Config-Dir>/mcp-endpoint.json` (Modus 0600) geschrieben; beim Stopp
   gelöscht. Erste Nachricht einer Verbindung muss `auth{token}` sein, alles
   andere → sofortiger Disconnect. Das Shim liest die Datei, der Mensch muss
   nichts eintragen. Ohne Token könnte jeder lokale Prozess die Szene
   verändern; mit Token nur einer, der die Datei des Nutzers lesen darf.
4. **Was ein Client darf:** genau die Tools der Registry, nichts darüber
   hinaus. Kein Datei-Zugriff, kein Shell, kein beliebiger Registry-Aufruf
   (2.7). Mutationen ausschließlich über das Gateway.
5. **Play-Modus:** Mutations-Tools werden mit `play_mode` abgelehnt (die UI
   verzichtet in PIE selbst auf Undo, eine Änderung wäre nach Stop weg und
   die Szene bliebe fälschlich dirty; siehe Kommentar `EditorApplication.cpp:5955`).
   Lesende Tools bleiben erlaubt.
6. **Builtins und Local-Only-Entities** (`isBuiltin`, `EnvironmentLightComponent`,
   `TerrainChunkComponent`): nicht anfassbar, `builtin`-Fehler.
7. **In einer Session:** MCP unterliegt denselben Locks wie ein Mensch
   (2.4). Ein Client ist nicht privilegierter als der Nutzer, der ihn
   gestartet hat.
8. **Sichtbarkeit:** solange der Listener läuft, zeigt der Editor das an
   (Status im Footer neben dem Collab-Status, Tooltip nennt Port und Anzahl
   Clients). Jede `tools/call`-Mutation landet als Zeile im Console-Log mit
   Präfix `MCP:`, damit „wer hat das getan" beantwortbar bleibt.
9. **Größen:** Frame-Limit 4 MiB für MCP (unter `kMaxFrameSize`), maximal 4
   gleichzeitige Verbindungen, mehr werden abgewiesen. Eine Rate-Begrenzung
   ist nicht nötig: ein Frame verarbeitet, was da ist, und der Client wartet
   auf die Antwort.

### 2.4 Das Command-Gateway (Kern, Schritt 2)

Ein einziger Eingang für Szenenänderungen, `HE::Ed::EditorCommands`
(`src/HE_Editor/EditorCommands.h/.cpp`), ImGui-frei, im Testbinary.

**Befehle** (Werttypen, alle mit Vorher/Nachher, damit jeder invertierbar ist):

| Befehl | Felder | Inverse |
|---|---|---|
| `CreateSubtree` | `parentUuid`, `blob` (CBOR-Subtree) oder `name` + `components`-JSON, `preserveIds`, → `rootUuid` | `DestroySubtree(rootUuid)` |
| `DestroySubtree` | `uuid`, aufgezeichnet: `blob`, `parentUuid`, Geschwister-Index | `CreateSubtree(blob, preserveIds=true)` |
| `Reparent` | `uuid`, `newParentUuid`, aufgezeichnet: `oldParentUuid` | `Reparent(old)` |
| `SetTransform` | `uuid`, `after[9]`, aufgezeichnet `before[9]` | `SetTransform(before)` |
| `SetComponents` | `uuid`, `patch` (partielles JSON), aufgezeichnet `beforeCbor` der betroffenen Schlüssel | `SetComponents(before)` |
| `DocEdit` | `assetPath`, `scope`, Liste `(Kind, id, beforeJson?, afterJson?)` | Liste vertauscht |
| `Batch` | Liste von Befehlen, ein Undo-Eintrag, atomar (bei Fehler wird rückwärts abgebaut) | rückwärts |

**Herkunft** `Origin { User, Remote, External }` steuert, was nach dem Apply
passiert:

| | Apply | Undo aufzeichnen | Publish | Lock prüfen |
|---|---|---|---|---|
| `User` (UI) | ja | ja | ja | ja (hat ihn über die Selektion) |
| `Remote` (Peer) | ja | **nein** | **nein** | nein |
| `External` (MCP) | ja | ja | ja | ja, eigener Lock-Satz |

Damit werden die fünf `onRemote*`-Lambdas zu `execute(cmd, Origin::Remote)`,
und die vier UI-Strukturbefehle zu `execute(cmd, Origin::User)`. Das ist der
eigentliche Vereinheitlichungs-Schritt, kein Nebeneffekt.

**Ablauf `execute`:**

1. Validieren: Entity existiert (UUID → Handle über `subjectFor`-Umkehr bzw.
   eigenen UUID-Index des Gateways), kein Builtin, kein Play-Modus (bei
   `External`), Payload lesbar (`isKnownComponentKey`).
2. Lock (nur in Session, nur `User`/`External`): `ownsLock(subject)`; sonst
   `requestLock` und **ablehnen mit `locked_by_other`, wenn ein anderer ihn
   hält**, bzw. mit `lock_pending`, wenn die Gewährung noch unterwegs ist.
   Optimistisch anwenden wie bei Assets wäre für Struktur-Befehle falsch: ein
   zurückgewiesener Destroy lässt sich nicht „read-only" machen. Der Client
   wiederholt nach einem Frame; das Tool sagt das im Fehlertext.
   MCP-Locks leben in einem eigenen Satz `m_externalLocks`, den
   `followSelection` nicht anfasst, und werden nach 2 s ohne weiteren Befehl
   auf dasselbe Subjekt freigegeben (kein Client darf Dinge dauerhaft sperren).
3. Vorher-Zustand einfangen (Blob/9 Floats/Komponenten-JSON), dann Apply über
   `WorldOps` (die heutigen Handler-Rümpfe, nach `EditorCommands.cpp`
   gezogen; `EditorApplication` behält nur Selektion und `m_structureKnown`
   über Callbacks).
4. Undo: hinter einem `IUndoSink`. Außerhalb Session der Snapshot-Sink
   (`EditorUndo::snapshotNow` **vor** dem Apply, wie heute; kontinuierliche
   Gesten bekommen `beginGesture/endGesture`, die auf `stashPre/commitPending`
   abbilden). In Session der Inverse-Op-Sink auf `CollabUndo`, das um die Kinds
   `Create`, `Destroy`, `Reparent`, `Components` **erweitert wird**: ohne das
   sind Struktur-Edits in einer Session weiterhin nicht rücknehmbar, und das
   wäre für MCP-Edits (die der Mensch nicht selbst gemacht hat) unzumutbar.
   Der Snapshot-Stack bleibt in v1, weil er außerhalb einer Session korrekt
   ist und die UI-Gesten darauf sitzen; das Gateway ist der Ort, an dem er
   später gegen Inverse-Ops getauscht werden kann, ohne Aufrufstellen
   anzufassen.
5. Publish (nur `User`/`External`, nur in Session): `SetTransform` →
   `publishTransform` **pro Befehl**, nicht über die Selektionsschleife;
   `SetComponents` → `publishComponents`; Struktur → **nichts**
   (`syncStructuralChanges` sieht es); `DocEdit` → nichts (der `DocMirror`-Diff
   des Panels sieht es, geschlossene Assets → `publishAsset` nach dem Save).
6. Observer: `onCommandApplied(cmd, origin)` für Selektion, Dirty-Flag,
   Notification, später MCP-Events.

**Was das Gateway bewusst nicht ist:** kein Ersatz für die kontinuierlichen
ImGui-Gesten in v1 (Gizmo, Inspector-Drags). Die bleiben auf
`capturePre/stashPre/commitPending`, weil sie mit dem Snapshot-Stack korrekt
sind. Schritt 2 zieht die **Strukturbefehle** und die **Remote-Handler**
hinein, das ist die Stelle, an der heute drei Verdrahtungen nebeneinander
liegen.

### 2.5 Scope v1: welche Tools zuerst

Lesend (Schritt 3):

* `scene.info` (Pfad, Dirty, Play-Modus, Session-Status, Entity-Anzahl)
* `entity.list` (Baum: uuid, name, parent, Komponentenschlüssel; Filter
  nach Name/Komponente)
* `entity.get(uuid)` (Komponenten-JSON aus 1.4, plus Weltposition über
  `worldPositionOf`)
* `entity.find(name)`
* `asset.list(type?)` (aus dem Content-Index), `component.keys`
* `editor.selection.get`

Mutierend (Schritt 3, alle durch das Gateway):

* `entity.create(name, parentUuid?, components?)`
* `entity.destroy(uuid)`, `entity.reparent(uuid, parentUuid)`
* `entity.set_transform(uuid, position?, rotation?, scale?)` (lokal, Euler
  Grad, wie die Komponente)
* `entity.set_components(uuid, patch)` (partieller Merge)
* `entity.duplicate(uuid)`
* `editor.selection.set(uuids)`, `editor.undo`, `editor.redo`
* `scene.save`

HorizonCode (Schritt 4): `hc.open(asset)`, `hc.get(asset)` (Graph-JSON +
pro Knoten die Pin-Liste), `hc.node_types`, `hc.add_node`, `hc.set_node`
(Upsert per Item-JSON), `hc.remove_node`, `hc.connect`, `hc.disconnect`,
`hc.set_pin_default`, `hc.add_variable`, `hc.set_variable`,
`hc.remove_variable`, `hc.save(asset)`.

Nicht in v1: Screenshot des Viewports (braucht Renderer-Readback; `HE_DUMP_*`
ist ein Prozess-Modus, nicht live), Material-/Partikel-/Animator-Graphen
(gleicher Adapter-Mechanismus, aber anderer Katalog), Asset-Import,
Terrain-Sculpting, Play-Steuerung, Ereignis-Push an den Client.

### 2.6 HorizonCode-Autoring: offenes und geschlossenes Asset

Ein `DocEdit` findet sein Ziel über eine **Dokument-Registry** im Editor: die
Panels melden ihre offenen Live-Graphen (dieselben `DocBindings`, die
`updateAssetCollabSync` schon einsammelt), keyed nach content-relativem Pfad
und `Scope`. Level Script = `HorizonWorld::levelScript()`, Game Instance =
`AppContext::gameInstanceGraph`.

* **Offen:** Apply über `CollabDocSync::IDocAdapter::upsert/remove` +
  `afterApply`; das Panel bekommt `dirty`; sein `DocMirror`-Diff publiziert
  in einer Session von selbst. Undo: `DocEdit`-Inverse im Gateway-Stack; das
  Level-Script-Panel bekommt zusätzlich seinen Szenen-Snapshot (wie heute bei
  jedem Edit), das Klassen-Panel hat damit erstmals ein Undo.
* **Geschlossen:** `ContentManager::loadAsset(path)` → `graphJson` →
  `fromJson` → dieselben Adapter-Operationen auf dem geladenen Graph →
  `toJson` → `saveAsset` (mit `AssetWriteLease`, in einer Session
  `publishAsset`). Auch hier ein `DocEdit`-Undo-Eintrag, dessen Inverse denselben
  Weg geht.
* `hc.connect` nimmt unified Pin-Indizes **oder** Pin-Labels; das Tool löst
  Labels über `nodePins` auf, weil ein Client Indizes nur aus `hc.get`
  kennt. Fällt `Graph::connect` durch, wird `connectWithConversion` versucht
  und das Ergebnis (inkl. eingefügtem Konvertierungsknoten) zurückgemeldet.
* `hc.add_node` für `EngineCall` prüft die Registry-ID und die `apiGroups`
  des Frontends (`MenuOpts`), damit MCP nichts einfügt, was das Menü
  verweigert.

### 2.7 EngineApi-Registry: Schema ja, Ausführung nein

* `api.list` liefert die Registry als Dokumentation (id, category, params,
  results, isExec, displayName). Das ist das, was ein Client braucht, um
  `EngineCall`-Knoten sinnvoll zu setzen.
* `api.call` nur für **pure Zeilen** (`isExec == false`) und ohne
  Welt-Nebenwirkung (Mathe, Vektor, String). Exec-Zeilen werden mit
  `refused_by_policy` abgelehnt; die vier, die im Edit-Modus Sinn ergeben
  (`transform.set*`), sind als `entity.set_transform` schon Gateway-Befehle.
  Ein Mapping `Registry-ID → Gateway-Befehl` bleibt möglich, wird aber nicht
  gebaut, bevor es einen zweiten Nutzer gibt.

---

## 3. Folgeschritte (Vorschlag für die Schritte 2 bis 7)

Der Chefchen hat das Thema in sieben Schritte zerlegt; sichtbar ist hier nur
Schritt 1. Die folgende Aufteilung ist mein Vorschlag, wie die Arbeit in
prüfbare Stücke fällt.

### Schritt 2: Command-Gateway

Ziel: `EditorCommands` mit den sieben Befehlen, `Origin`, `IUndoSink`
(Snapshot + Inverse), `CollabUndo` um Struktur-Kinds erweitert, die fünf
Remote-Handler und die vier UI-Strukturbefehle umgestellt. Verhalten des
Editors für den Menschen unverändert.

| Datei | Was |
|---|---|
| `src/HE_Editor/EditorCommands.h/.cpp` (neu) | Befehle, `execute`, `WorldOps`, UUID-Index, Lock-Satz, Observer |
| `src/HE_Editor/CollabUndo.h/.cpp` | `Kind::Create/Destroy/Reparent/Components`, Payloads (Blob, Parent, CBOR), `undoLabel` |
| `src/HE_Editor/EditorApplication.cpp:351–410, 660–680` | `onRemote*` → `execute(…, Remote)` |
| `src/HE_Editor/EditorApplication.cpp:5943–6047` | duplicate/copy/paste/delete → `execute(…, User)` |
| `src/HE_Editor/EditorApplication.cpp:3520–3585` | Transform-Baseline bleibt; Publish pro Befehl kommt aus dem Gateway dazu, Doppel-Publish über den Hash in `publishTransform` abgefangen |
| `src/HE_Editor/EditorApplication.h` | Member `EditorCommands m_commands`, Verdrahtung in `OnInit` |
| `src/HE_Editor/StructuralSync.h` | unverändert; `markSubtreeKnown` wird vom Gateway-Callback gerufen |
| `tests/CMakeLists.txt:144–192` | `EditorCommands.cpp` eintragen |
| `tests/test_editor_commands.cpp` (neu) | siehe unten |
| `tests/test_collab_undo.cpp` | neue Kinds |

Tests:

* Jeder Befehl: Apply, Undo, Redo auf einer `HorizonWorld` ohne Session,
  Weltzustand per UUID verglichen; nach Undo ist die alte UUID wieder da
  (preserveIds).
* `Batch`: Fehler in Befehl 3 baut 1 und 2 zurück, kein Undo-Eintrag.
* `Origin::Remote` zeichnet nichts auf und publiziert nichts (Zähler am
  Fake-Sink).
* Builtin, Play-Modus, unbekannte UUID, unbekannter Komponentenschlüssel →
  je der passende Fehlercode, Welt unverändert.
* Zwei `CollabController` über TCP (Muster `test_collab_controller.cpp`):
  `SetTransform`/`SetComponents` an einer **nicht selektierten** Entity
  kommt beim Peer an; `CreateSubtree` genau einmal (keine Duplikate über
  `syncStructuralChanges`); Lock beim Peer → `locked_by_other`; Undo eines
  Struktur-Befehls in der Session stellt beim Peer denselben Stand her.
* `CollabUndo`: `dropUnowned` wirft Struktur-Einträge, deren Lock weg ist.

### Schritt 3: Transport, Auth, Tool-Registry, Shim, Entity-Tools

| Datei | Was |
|---|---|
| `src/HE_Net/include/Net/TcpTransport.h`, `src/TcpTransport.cpp`, `Socket.h/.cpp` | `listen(port, loopbackOnly)` bzw. `socketCreateListenerLoopback` |
| `src/HE_Editor/McpBridge.h/.cpp` (neu) | Listener, Auth-Zustand pro Verbindung, JSON-RPC-Dispatcher, Frame-Pump `update(nowMs)`, Endpunkt-Datei |
| `src/HE_Editor/McpToolRegistry.h/.cpp` (neu) | Tool-Beschreibungen + Handler, `tools/list`-Schema |
| `src/HE_Editor/McpToolsEntity.cpp` (neu) | die Entity-/Scene-/Editor-Tools aus 2.5 über `EditorCommands` |
| `src/HE_Editor/EditorApplication.h/.cpp` | `EditorConfig::McpServerEnabled/McpPort`, Lesen `:1108`, Schreiben `:7308`, Pump nach `m_collab.update` (`:3508`) vor `EditorUI::render`, `HE_MCP`-Env |
| `src/HE_Editor/EditorSettingsPanel.cpp:441 ff.` | Checkbox + Port + Hinweistext (Token-Datei, nur lokal) |
| `src/HE_Editor/EditorHelp.cpp` | Einträge für die neuen Bedienelemente (Deckungs-ctest) |
| Footer/Statusleiste (`CollabPresenceBar` oder `EditorToolbar`) | „MCP: an, n Clients" |
| `scripts/he_mcp.py` (neu) | stdio-MCP-Server, liest Endpunkt-Datei, `tools/list`-Passthrough, Reconnect |
| `scripts/requirements-mcp.txt` (neu) | `mcp>=2.0.0` |
| `tests/CMakeLists.txt` | neue `.cpp` eintragen |
| `tests/test_mcp_bridge.cpp` (neu) | siehe unten |
| `tests/test_net_tcp.cpp` | Loopback-Bind |

Tests:

* Loopback-Listener ist von einer Nicht-Loopback-Adresse des Hosts nicht
  erreichbar (Verbindungsversuch auf die LAN-Adresse scheitert).
* Ohne `auth` als erste Nachricht: Disconnect; falsches Token: Disconnect;
  richtiges Token: `tools/list` liefert jedes Tool mit Schema.
* Endpunkt-Datei: entsteht beim Start mit Modus 0600 (POSIX), verschwindet
  beim Stopp, enthält Port/PID/Token.
* Ein Test-Client (in C++ über `TcpTransport::connect`) macht
  `entity.create` → `entity.set_transform` → `entity.get` → `editor.undo` →
  `entity.get`; Weltzustand und Fehlercodes werden geprüft; Play-Modus-Flag
  → `play_mode`.
* Fünfte Verbindung wird abgewiesen; ein 5-MiB-Frame wird abgewiesen, die
  Verbindung überlebt nicht, die anderen schon.
* Python: `scripts/tests/test_he_mcp.py` mit einem Fake-Editor-Socket
  (Framing, Auth, Passthrough, Reconnect nach Editor-Neustart).

### Schritt 4: HorizonCode-Tools

| Datei | Was |
|---|---|
| `src/HE_Editor/McpToolsHc.cpp` (neu) | Tools aus 2.5/2.6 |
| `src/HE_Editor/EditorCommands.h/.cpp` | `DocEdit`-Befehl, Dokument-Registry (offen/geschlossen), `AssetWriteLease` |
| `src/HE_Editor/LevelScriptPanel.cpp` (Class-Panel, `ClassState`), `UIEditorPanel.cpp`, Game-Instance-Panel | Live-Dokumente bei der Registry an-/abmelden (dieselben `DocBindings`) |
| `src/HE_Editor/HcGraphHost.h` | `pinRanges/nodePins` für die Antwort von `hc.get`; `MenuOpts` je Frontend abfragbar |
| `src/HE_Core/include/HorizonCode/HorizonCode.h` | evtl. `linkKey`-Export, sonst nichts |
| `tests/test_mcp_hc.cpp` (neu) | siehe unten |

Tests:

* Auf einem geschlossenen Klassen-Asset: `add_node` (Event + EngineCall) →
  `connect` per Label → `save` → Datei neu laden → Graph enthält beides;
  Undo → Datei ohne beides.
* Auf einem offenen Graph (Adapter direkt, ohne Panel): `set_node` upsertet,
  `DocMirror`-Diff danach liefert genau ein Delta (das Publish ist
  garantiert).
* `connect` mit Typ-Konflikt → Konvertierungsknoten wird gemeldet;
  unlösbarer Konflikt → `invalid_payload` ohne Änderung.
* `EngineCall` mit ID außerhalb der `apiGroups` des Frontends → abgelehnt.
* Parität: `hc.get` gibt für jeden Knoten genau die Pins, die
  `HcGraphHost::nodePins` liefert (Zähler und Labels).

### Schritt 5: Registry-Exposition, Selektion, Ereignisse

* `api.list`, `api.call` (pure), `component.keys` mit Feldskizze aus einem
  Beispiel-JSON je Komponente.
* `editor.selection.*` mit Mehrfachauswahl, falls bis dahin im Editor
  vorhanden, sonst einzeln.
* `events/poll`: Puffer der `onCommandApplied`-Ereignisse pro Verbindung
  (uuid, Befehl, Origin), damit ein Client sieht, was der Mensch parallel
  tut. Kein Push, nur Poll.

| Datei | Was |
|---|---|
| `src/HE_Editor/McpToolsApi.cpp` (neu) | `api.list`, `api.call` (nur `isExec == false`), `component.keys` |
| `src/HE_Editor/McpToolRegistry.h/.cpp` | Registrierung der neuen Tools, Schema aus `ApiFn::params/results` |
| `src/HE_Editor/EditorCommands.h/.cpp` | Ereignispuffer am Observer (256 Einträge, Überlauf-Marke) |
| `src/HE_Editor/McpBridge.h/.cpp` | `events/poll`, Cursor pro Verbindung |
| `src/HE_Editor/McpToolsEntity.cpp` | `editor.selection.set` für Mehrfachauswahl |
| `tests/test_mcp_bridge.cpp` | siehe unten |

Tests: `api.call` auf eine Exec-Zeile → `refused_by_policy`; pure Zeile
liefert das Ergebnis der Registry-`invoke`; Ereignispuffer hält 256 und
meldet Überlauf; zwei Verbindungen haben unabhängige Cursor.

### Schritt 6: Editor-UX, Handbuch, Sichtbarkeit

* Preferences-Abschnitt mit Erklärung, Statusanzeige, Console-Präfix
  `MCP:`, Notification beim ersten Client-Connect („Claude Code ist
  verbunden") und bei einer abgewiesenen Auth.
* Handbuch-Kapitel (DocsLibrary) „Editor fernsteuern" mit der
  Shim-Konfiguration für Claude Code (`claude mcp add`) und Claude Desktop.
* `EditorHelp`-Deckung grün, `editor_help_audit.py` grün.

| Datei | Was |
|---|---|
| `src/HE_Editor/EditorSettingsPanel.cpp` | Abschnitt „Fernsteuerung (MCP)": Schalter, Port, Pfad der Endpunkt-Datei, Hinweistext |
| `src/HE_Editor/EditorHelp.cpp` | Einträge für jedes neue Bedienelement |
| `src/HE_Editor/DocsLibrary.cpp` (+ Docs-Quelle) | Kapitel „Editor fernsteuern" |
| `src/HE_Editor/CollabPresenceBar.cpp` oder `EditorToolbar.cpp` | Statusanzeige „MCP: an, n Clients" mit Tooltip |
| `src/HE_Editor/McpBridge.cpp` | Notifications über `NotificationStore`, Console-Präfix |
| `tests/test_editor_help.cpp`, `tests/test_docs_library.cpp` | Deckung und Kapitel |

Tests: Help-Audit, `test_docs_library` mit dem neuen Kapitel, ein
`he_uishot`-Bild des Preferences-Abschnitts als Sichtprüfung.

### Schritt 7: Ende-zu-Ende, Doku, Website

* Ein echter Durchlauf mit Claude Code gegen den laufenden Editor (Mensch
  am Bildschirm): Würfel platzieren, verschieben, Level-Script-Knoten
  setzen, Undo im Editor sehen. Protokoll im Thread.
* Zwei-Instanzen-Durchlauf in einer Session: MCP auf dem Host, Peer sieht
  die Änderungen, Lock-Konflikt einmal provoziert.
* Optional: Streamable-HTTP direkt in C++ als Ersatz des Shims, nur wenn
  das Shim in der Praxis stört.

| Datei | Was |
|---|---|
| `docs/mcp-editor-integration-plan.md` | Abschluss-Stand, Abweichungen vom Plan |
| `README.md` | Absatz „Editor per MCP fernsteuern" mit der Shim-Zeile |
| `scripts/he_mcp.py` | Fehlermeldung bei fehlendem `mcp`-Paket mit Installationszeile |
| `Website/HorizonEngine/roadmap.json` (Geschwister-Repo, über `horizon-web`-MCP) | Roadmap-Eintrag, Devlog; Deploy bestätigen lassen |

Tests: die beiden Durchläufe oben sind Handtests mit Protokoll im Thread;
alles Automatisierbare ist in den Schritten 2 bis 6 abgedeckt.

---

## 4. Risiken und offene Punkte

* **Snapshot-Undo und Inverse-Undo teilen sich weiterhin nichts.** Das
  Gateway macht die Wahl zentral, aber solange die UI-Gesten am
  Snapshot-Stack hängen, kann ein MCP-Befehl außerhalb einer Session nur als
  Snapshot rückgenommen werden. Das ist korrekt, aber jede Rücknahme kostet
  einen Ganzwelt-Restore und macht Handles ungültig (deshalb UUIDs).
* **Asynchrone Lock-Gewährung.** In einer Session braucht ein MCP-Befehl auf
  ein noch nicht gehaltenes Subjekt einen Roundtrip; der Client bekommt
  `lock_pending` und wiederholt. Ein ungeduldiger Client sieht das als
  Fehler. Der Fehlertext muss das erklären.
* **Lock-Timeout gegen `dropUnowned`.** 2.4 gibt externe Locks nach 2 s
  ohne weiteren Befehl frei; `CollabUndo::dropUnowned` wirft jeden Eintrag,
  dessen Lock weg ist. Zusammen hieße das: jeder MCP-Edit in einer Session
  verliert sein Undo 2 s später. Schritt 2 muss sich entscheiden: entweder
  leben externe Locks, bis der Client sie freigibt oder die Verbindung
  trennt (Undo bleibt, ein Client kann Subjekte lange halten, der Mensch
  sieht das am Lock-Badge), oder der Timeout bleibt und Session-Undo für
  MCP-Edits gilt als nicht vorhanden. Mein Vorschlag ist das Erste, mit
  Freigabe beim Disconnect.
* **Struktur-Undo in einer Session** ist neu (`CollabUndo`-Erweiterung) und
  hat die Regel „gültig, solange der Lock gehalten wird". Ein Destroy gibt
  den Lock auf das zerstörte Subjekt frei; der Undo-Eintrag muss den Lock
  überleben, sonst ist ein Destroy nie rücknehmbar. Vorschlag: Destroy-
  Einträge behalten den Lock, bis der Eintrag vom Stack fällt.
* **Doppel-Publish von Transforms** für die selektierte Entity (Selektions-
  schleife + Gateway). `publishTransform` verwirft unveränderte Werte,
  deshalb harmlos, aber ein Test soll es belegen.
* **Panels müssen ihre Live-Dokumente anmelden.** Wer das für ein Panel
  vergisst, bekommt den Geschlossen-Pfad (Load/Save auf Platte) unter einem
  offenen Tab, und der Tab überschreibt beim nächsten Save. Die Registry
  muss deshalb beim Save des Panels prüfen, ob die Platte inzwischen eine
  neuere Version hat (Vergleich über den Asset-Stempel), und warnen.
* **Python-Abhängigkeit** (`mcp>=2`) auf der Nutzerseite. Das Shim muss ohne
  Editor-Build laufen (reines Skript) und beim Fehlen des Pakets eine klare
  Meldung mit Installationszeile geben.

---

## 5. Was am Ende tatsächlich gebaut wurde

Nachgetragen zum Abschluss des Themas. Der Plan oben ist von Schritt 1 und steht
unverändert da; dieser Abschnitt sagt, wo die Umsetzung ihm gefolgt ist und wo
nicht. Wer wissen will, was die Engine *kann*, liest diesen Abschnitt, nicht die
Schrittliste in Kapitel 3.

### 5.1 Steht

| Was | Wo |
|---|---|
| Command-Gateway, fünf Befehle, `Origin{User,Remote,External}`, Lock-Gate, zwei Undo-Senken | `src/HE_Editor/EditorCommands.h/.cpp` |
| TCP-Listener auf 127.0.0.1, JSON-RPC 2.0, Auth per Token, Frame-Limit 4 MiB, max. 4 Verbindungen | `src/HE_Editor/McpBridge.h/.cpp` |
| Werkzeug-Registry und Kernwerkzeuge (`ping`, `scene_info`) | `src/HE_Editor/McpToolRegistry.h/.cpp` |
| Entity-Werkzeuge: `entity_create`, `entity_destroy`, `entity_reparent`, `entity_set_transform`, `entity_set_components`, `entity_list`, `entity_get` | `src/HE_Editor/McpToolsEntity.cpp` |
| Dreizehn HorizonCode-Werkzeuge (lesend und autorenschaftlich) | `src/HE_Editor/McpToolsHc.cpp` |
| Asset-Werkzeuge: `asset_resolve`, `asset_list`, `asset_create`, `asset_delete`, `asset_move` — Pfad→UUID, Ordner lesen, autorierte Typen anlegen, Löschen mit Referenz-Scan davor, Verschieben mit Retarget | `src/HE_Editor/McpToolsAsset.cpp` |
| Was in einem frisch angelegten Asset jedes Typs steht — aus dem Create-Menü des Content Browsers herausgezogen, damit Panel und Werkzeuge dieselbe Datei schreiben | `src/HE_Editor/AssetStubWriter.h/.cpp` |
| 233 Werkzeuge aus der `HE::api`-Registry, Schema aus `params`, Beschreibung aus `NodeDocs::engineCall` | `src/HE_Editor/McpToolsApi.h/.cpp` |
| Ein/Aus-Schalter, Port, Zustandsanzeige, „Try Again" | Preferences → Editor → **Remote Control** |
| Fußzeilen-Anzeige mit Tooltip, Klick öffnet die Seite | `src/HE_Editor/McpStatusBar.h/.cpp` |
| Konsolenzeilen mit Präfix `MCP:` für Start, Stopp, Auth und jeden `tools/call` | `McpBridge.cpp:432` |

Zur **Namenskonvention**: die Werkzeuge heißen `scene_info`, nicht `scene.info`
wie in 2.5/2.6 geschrieben. Die Messages-API lässt für Tool-Namen nur
`^[a-zA-Z0-9_-]{1,64}$` zu, ein Punkt macht das Werkzeug beim Client
unbenutzbar (Entscheidung aus Schritt 4).

### 5.2 Das Sicherheitsmodell, wie es sich heute bedienen lässt

2.3 ist inhaltlich umgesetzt, mit zwei Abweichungen und einer Ergänzung.

* **Nur IPv4.** 2.3.2 nennt `127.0.0.1` *und* `::1`. Ein Socket kann nicht beide
  Loopbacks binden, und ein zweiter wäre eine zweite Angriffsfläche für nichts.
  Der Listener ist IPv4; ein Client muss wörtlich `127.0.0.1` verbinden,
  `localhost` kann auf `::1` auflösen und dann ins Leere laufen.
* **Keine Notification beim ersten Client.** 2.3.8 und Schritt 6 sehen eine
  vor. Gebaut ist stattdessen die Fußzeilenanzeige, die den Zustand *dauerhaft*
  zeigt statt einmal — bei etwas, das eine Sitzung lang läuft, ist eine
  Meldung, die man verpasst haben kann, die schwächere Antwort. Die
  Konsolenzeilen (`MCP: client N authenticated`) gibt es zusätzlich.
* **Ergänzung: der dritte Zustand.** `McpBridge::setEnabled` kehrt früh zurück,
  wenn sich der Wert nicht geändert hat. Ein `start()`, das scheitert (Port
  belegt, Endpunktdatei nicht schreibbar), wird vom Frameloop also nie
  wiederholt: die Einstellung sagt an, und es lauscht nichts. Preferences und
  Fußzeile zeigen diesen Zustand getrennt an („Enabled, but the listener is not
  up" / „Remote Control: not listening"), und der Knopf „Try Again" geht den
  einzigen Wiederholungspfad, den es gibt — aus und wieder an. Eine Checkbox,
  die hier nur angehakt dastünde, wäre eine Lüge über genau das, wofür sie da
  ist.

**Locks gelten auch für die HorizonCode-Werkzeuge.** Nachgereicht nach dem
Abschluss-Review. Die `hc_*`-Werkzeuge laufen nicht über `EditorCommands` — sie
schreiben über den `CollabDocSync`-Adapter — und hatten damit auch das
Lock-Gate aus 2.4/Punkt 7 nicht. Ein Fremd-Lock war für einen Menschen ein
bewusster Kompromiss (er sieht das Read-only-Banner), für einen entfernten
Client ist es stiller Datenverlust: der Edit landet lokal, wird nie
veröffentlicht (`publishDocDeltas` sendet nur, was wir halten) und stirbt beim
nächsten Ganzdatei-Update des Peers. Alle zehn schreibenden Werkzeuge fragen
jetzt in `openDoc` `McpHcHooks::lockedByOther(key)` und lehnen mit
`locked_by_other` ab; die drei lesenden sind unberührt, gemeinsam lesen ist der
Sinn einer Session. Bewusst **nur** die Fremd-Lock-Hälfte von
`EditorCommands::checkLock`: kein `lock_pending`, weil die Asset-Politik des
Editors optimistisch ist (`beginAssetEdit` — ein verlorener Asset-Edit wird von
der Platte nachgeladen, eine gelöschte Entity nicht) und weil hier kein eigener
Lock-Satz gehalten wird, auf dessen Gewährung man warten könnte. Der
Schlüssel wird dabei so übersetzt, wie `collabSyncKey` es tut: die beiden
editor-eigenen Graphen *sind* ihr Tab-Pfad in der Lock-Tabelle, ein
Klassen-Asset ist schon content-relativ.

Die Richtung ist einseitig und soll es bleiben: Preferences schreibt
ausschließlich `EditorConfig::McpServerEnabled`, der Frameloop schiebt das
jedes Frame in `setEnabled`. Der `McpBridge*` in `AppContext` ist für die UI
**lesend** — er beantwortet, was die Config nicht kann (läuft der Listener
wirklich, auf welchem Port, wie viele Clients).

`Restore Defaults` setzt den Schalter bedingungslos zurück, auch wenn gerade
ein Client verbunden ist. Ein „Restore Defaults", das einen offenen Listener
stehen ließe, wäre genau die Überraschung, gegen die der Standard-Aus existiert.

### 5.3 Was offen ist

> **Stand nach dem Folgethema (Schritt 5, 08.09.2026).** Die ersten beiden
> Punkte sind zu, der dritte zur Hälfte. Der Text der Punkte bleibt stehen, wie
> er geschrieben wurde; was ihn geschlossen hat, steht als `→` darunter und
> ausführlich in Kapitel 7.

* **`scripts/he_mcp.py` gibt es nicht.** Das ist die eine echte Lücke, und sie
  ist größer als sie aussieht: die Brücke spricht rohes JSON-RPC über einen
  gerahmten TCP-Socket, **nicht** MCP über stdio. Ein Client wie Claude Code
  kann sich damit heute nicht anhängen. Ein `claude mcp add`-Rezept im Handbuch
  wäre eine Anleitung für ein Programm, das es nicht gibt — deshalb steht
  keines darin. Das Shim braucht einen eigenen Schritt (reines Python,
  `mcp>=2`, liest Port und Token aus `<Config-Dir>/mcp-endpoint.json`, reicht
  `tools/list` und `tools/call` durch und kennt selbst keine Werkzeuge).
  * → **Erledigt.** `scripts/he_mcp.py` steht (Schritt 2, `58a9da3d`), samt 28
    Python-Tests gegen eine nachgebaute Brücke. Eine Abweichung von der Klammer
    oben: **kein `mcp>=2`**, sondern reine Standardbibliothek — die
    Chefchen-Entscheidung zu 6.2, begründet in 7.2. Ausgeliefert wird es neben
    dem Editor (`CMakeLists.txt`, `package_macos.sh`), und registriert wird es
    vom Knopf „Add to Claude" (Schritt 3, `55431c3a`).
* **Kein Handbuch-Kapitel „Editor fernsteuern".** Das Kapitel läge nicht in
  diesem Repo: `EditorDeps/Docs/he-docs.json` wird von
  `scripts/build_docs_bundle.py` aus `~/VSCode/Website/HorizonEngineDocs/*.html`
  erzeugt. Die drei Handbucheinträge der neuen Bedienelemente haben deshalb ein
  leeres `topic` — F1 landet in der generierten Settings-Referenz unter der
  Überschrift „Remote Control", was funktioniert; ein `topic` auf einen
  Website-Anker, den es noch nicht gibt, würde `test_editor_help` rot färben.
  Wenn das Kapitel geschrieben ist: Abschnitt in `collaboration.html`, Bundle
  neu bauen, Bundle hier committen, Website **nicht** deployen — und erst dann
  die `topic`-Felder füllen.
  * → **Erledigt, genau in dieser Reihenfolge.** Das Kapitel ist
    `collaboration.html`, Abschnitt `#remote-control` („Remote Control", nach
    „Notifications", mit Eintrag in der Seitenleiste). Bundle neu gebaut
    (`EditorDeps/Docs/he-docs.json`, 112 → 113 Abschnitte, hier committet),
    **Website nicht deployt** — das ist ein eigener, bestätigter Schritt des
    Menschen. Danach die `topic`-Felder: alle **fünf** Remote-Control-Einträge
    zeigen jetzt auf `collaboration#remote-control` (drei aus dem Vorthema, zwei
    aus Schritt 3). Der Weg über F1 ist unverändert die generierte
    Settings-Referenz; `topic` ist der Weiterlesen-Link daneben.
* **Nie gegen einen echten Client gelaufen.** Alles hier ist durch Tests über
  einen echten Socket belegt, aber der Ende-zu-Ende-Durchlauf aus Schritt 7
  (Mensch am Bildschirm, Würfel platzieren, Undo sehen) steht aus — und kann
  es auch, solange das Shim fehlt.
  * → **Halb.** Der Handschlag ist gelaufen: die Probe aus Schritt 4 startet
    über `claude mcp get` wirklich das Shim, und die Zeile wird nur grün, wenn
    der Zähler dieser Brücke dabei hochgeht (7.4). Der Rest des Durchlaufs —
    Mensch am Bildschirm, Würfel setzen, Undo sehen — steht weiter aus und ist
    aus einer Testsuite heraus auch nicht zu belegen. **Das bleibt offen** und
    ist der einzige Punkt aus 5.3, der es tut.
* Die Punkte aus Kapitel 4 bleiben, wo sie noch nicht durch Schritt 2
  entschieden wurden.

---

## 6. Das Shim und der Knopf: Entwurf

Nachgetragen als Schritt 1 des Folgethemas („MCP stdio-Shim + Add to
Claude-Knopf"). Schließt die erste Lücke aus 5.3 und den Nutzerwunsch daneben:
ein Punkt in Preferences, der den Server bei einer vorhandenen `claude`-CLI
registriert, und eine Anzeige auf der Seite **Tool Status**, die die Verbindung
tatsächlich prüft statt sie zu behaupten.

Alles hier ist Entwurf, kein Code. Was empirisch belegt ist, steht mit dem
Befehl dabei, der es belegt hat; was noch offen ist, steht in 6.9.

### 6.1 Was heute wirklich da ist (nachgemessen)

**Die Brücke.** `McpBridge` lauscht auf `127.0.0.1`, Port aus
`EditorConfig::McpPort` (0 = das System wählt). Rahmung ist
`[uint32 big-endian Länge][JSON]` (`src/HE_Net/include/Net/TcpTransport.h:15`),
Nutzlast rohes JSON-RPC 2.0. Die Endpunktdatei liegt unter
`GlobalState::userDataDir() / "mcp-endpoint.json"`
(`EditorApplication.cpp:6124`), Modus 0600, Inhalt
(`McpBridge.cpp:463`):

```json
{ "port": 51234, "pid": 4711, "token": "…64 Hex…",
  "protocolVersion": 1, "host": "127.0.0.1" }
```

**Der Handschlag.** Die erste Nachricht auf einer Verbindung MUSS
`{"jsonrpc":"2.0","id":1,"method":"auth","params":{"token":"…"}}` sein. Alles
andere — falscher Token, kaputtes JSON, eine andere Methode — schließt die
Verbindung ohne Antwort (`McpBridge.cpp:283`). Danach kennt der Dispatcher vier
Methoden: `ping`, `auth`, `tools/list`, `tools/call`
(`McpBridge.cpp:362`).

**Die Antwortformen sind schon MCP-Drahtform.** Das ist der Punkt, an dem das
ganze Shim klein wird. `tools/list` liefert `{ "tools": [ { name, description,
inputSchema } ] }`, `tools/call` liefert `{ content:[{type:"text",text}],
isError, structuredContent }` — beides genau so, wie MCP es haben will
(`McpToolRegistry.h:75`, `McpBridge.cpp:408`). Das Shim muss nichts übersetzen,
nur weiterreichen.

**Die Bedienoberfläche.** Preferences → Editor → **Remote Control** hat zwei
Zeilen (`EditorSettingsPanel.cpp:536` und `:597`): die Checkbox mit
Zustandszeile plus „Try Again", und den Port. Daneben, im selben Rail-Abschnitt
„Editor", liegt **Tool Status** (`Page::Status`, `drawStatusPage`
`EditorSettingsPanel.cpp:1807`) — eine Vier-Spalten-Tabelle Tool / State /
Detail / Fix, gespeist aus Hintergrundproben (`ctx.gitProbe`,
`ctx.toolchainProbe`). Dort gehört die Claude-Prüfung hin, nicht auf die
Remote-Control-Seite: die Seite existiert für genau die Frage „ist das Werkzeug
draußen da und tut es, was es soll".

**Prozesse starten.** `HE::Proc` (`src/HE_Core/include/Platform/Process.h`) ist
der richtige Weg und nicht der alte `popen`-Wrapper: argv-Vektor statt
Shell-String (Pfade mit Leerzeichen sind damit ein Nicht-Problem), getrennte
Ströme, ehrlicher Exit-Code, Timeout. `HE::Proc::which()` ruft
`augmentToolPath()` selbst auf und behandelt auf Windows `.exe`/`.cmd`/`.bat`
über `SearchPathW` (`Process.cpp:389`) — die zwei Fallen, um die
`resolveBrew()` seinerzeit herumgebaut wurde, sind hier also schon erledigt.

### 6.2 Die Entscheidung, die vorab fällt: welcher Python-Stack

Das Thema sagt „mcp>=2". Die API dazu ist geprüft und steht in 6.4 — aber die
Abhängigkeit selbst ist das größere Problem, und der Knopf hängt daran.

Gemessen auf dieser Maschine:

```
$ python3 -c "import mcp"     → ModuleNotFoundError: No module named 'mcp'
$ python3 -V                  → Python 3.14.5
```

Das ist der Normalfall, nicht die Ausnahme. Die drei anderen MCP-Server dieses
Nutzers laufen jeweils aus einem eigenen `.venv`. Der mitgelieferte CPython des
Editors hilft nicht: der ist ein eingebettetes Runtime-Zip mit `._pth` und ohne
`pip` und ohne `python3`-Executable. Und auf Windows heißt der Interpreter
`py`/`python`, `python3` gibt es dort auf PATH gar nicht.

Konsequenz: `claude mcp add horizon-editor -- python3 …/he_mcp.py` erzeugt auf
einer frischen Maschine einen Eintrag, der beim ersten Start
`ModuleNotFoundError` wirft. Der Knopf würde also zuverlässig einen kaputten
Eintrag anlegen — und die ehrliche Testanzeige daneben würde das brav melden.

Zwei Wege stehen zur Wahl:

**(a) `mcp>=2` wie im Thema.** Korrekte Bibliothek, weniger eigener Code, aber
das Shim braucht eine Installation, die der Editor nicht mitbringt. Der Knopf
müsste dann entweder ein `.venv` neben dem Editor anlegen und `pip install
mcp` fahren (Netz, Minuten, kann scheitern, braucht ein eigenes
Fortschrittsfenster wie `installToolchain`), oder der Nutzer installiert von
Hand — womit der Knopf nur noch die halbe Zusage hält.

**(b) Shim ohne Fremdabhängigkeit, nur stdlib.** MCP über stdio ist
zeilengetrenntes JSON-RPC 2.0 auf stdin/stdout. Ein reiner Durchreicher braucht
fünf Methoden: `initialize`, `notifications/initialized`, `ping`, `tools/list`,
`tools/call`. Die Brücke liefert `tools/list` und `tools/call` bereits in
MCP-Drahtform (6.1), das Weiterreichen ist damit fast wörtlich. Geschätzt 150
bis 200 Zeilen `json` + `socket` + `sys`, keine Installation, läuft mit jedem
Python ab 3.8.

**Empfehlung: (b).** Der Knopf verspricht „einmal drücken und es geht". Mit (a)
ist dieses Versprechen an einen `pip install` gekettet, den der Editor nicht
kontrolliert; mit (b) ist es wahr, sobald irgendein Python auf der Maschine
liegt. Die Bibliothek würde uns hier nichts abnehmen, was wir nicht ohnehin
durchreichen — ihr Wert liegt im Definieren eigener Werkzeuge, und das Shim
definiert ausdrücklich keine.

**Das ist eine Chefchen-Entscheidung**, weil das Thema wörtlich `mcp>=2`
vorgibt. Sie muss vor Schritt 2 fallen; alles andere in diesem Abschnitt gilt
für beide Wege.

Für den Fall, dass (a) gewählt wird, ist die API in 6.4 nachgeprüft — die
Zeilen aus dem Gedächtnis stimmen nur zur Hälfte.

### 6.3 `scripts/he_mcp.py`: Aufbau

Reiner Durchreicher, ohne eigene Werkzeuge. Sechs Teile:

1. **Endpunktdatei finden.** Reihenfolge:
   `--endpoint <pfad>` (argv) → `$HE_MCP_ENDPOINT` → die plattformübliche
   Ableitung von `userDataDir()`. Die Ableitung ist die Rückfallebene und nicht
   der Normalweg: der Editor kennt den Pfad und gibt ihn bei der Registrierung
   mit (6.6), damit eine Diskrepanz gar nicht erst entstehen kann. Für den
   Handbetrieb repliziert das Shim `GlobalState.cpp:117-125` trotzdem:
   `%APPDATA%\HorizonEngine`, `~/Library/Application Support/HorizonEngine`,
   `$XDG_CONFIG_HOME/HorizonEngine` bzw. `~/.config/HorizonEngine`, jeweils
   `+ /mcp-endpoint.json`.

2. **Verbinden und anmelden.** `socket.create_connection((host, port))` mit
   dem `host` aus der Datei (wörtlich `127.0.0.1` — `localhost` löst auf dem Mac
   zuerst auf `::1` auf und läuft ins Leere, deshalb steht das Feld überhaupt
   in der Datei). Dann als erstes Frame `auth` mit dem Token, und auf die
   Antwort warten. Kommt keine, sondern ein Verbindungsabbruch: Token ist alt,
   Datei neu lesen, einmal wiederholen.

3. **Rahmung.** `struct.pack(">I", len(payload)) + payload` beim Senden; beim
   Lesen vier Bytes Länge, dann exakt so viele Bytes (`recv` in einer Schleife,
   `recv` liefert Teilstücke). Das Gegenstück refüsiert über
   `kMaxFrameBytes = 4 MiB`; das Shim prüft dieselbe Grenze beim Lesen, damit
   ein defekter Präfix nicht in eine Riesenallokation läuft.

4. **stdio-Seite.** Eine Zeile lesen, JSON parsen, beantworten, `flush()`. Kein
   Threading: MCP über stdio ist von Haus aus sequentiell, und die Brücke
   beantwortet ohnehin ein Frame pro Frame auf dem Hauptthread. **Nichts außer
   JSON-RPC darf auf stdout landen** — jede Diagnose geht nach stderr, sonst
   ist der Strom vergiftet.

5. **Die fünf Methoden.**
   * `initialize` → beantwortet das Shim selbst mit
     `protocolVersion`, `capabilities:{tools:{}}`, `serverInfo:{name:
     "horizon-editor", version}`. Vorher muss die Verbindung zur Brücke stehen
     und `auth` durch sein (siehe 6.7 — daran hängt, ob der Test ehrlich ist).
   * `notifications/initialized` → schlucken, keine Antwort (Notification).
   * `ping` → `{}`.
   * `tools/list` → an die Brücke, Ergebnis unverändert zurück.
   * `tools/call` → an die Brücke, Ergebnis unverändert zurück.
   Alles andere → JSON-RPC-Fehler `-32601`, mit derselben id.

6. **`--selftest`.** Kein MCP, sondern: Datei lesen, verbinden, `auth`,
   `tools/list`, eine Zeile JSON nach stdout
   (`{"ok":true,"port":…,"tools":233}`), Exit 0. Bei jedem Fehler eine Zeile
   mit `"ok":false` und `"error"`, Exit 1. Das ist der Prüfstein, den der
   Editor drücken kann, ohne die `claude`-CLI zu bemühen — und das, was ein
   Mensch von Hand aufruft, wenn die Registrierung streitig ist.

**Wiederverbinden.** Der Editor startet neu → neuer Port, neuer Token, alter
Socket tot. Das Shim liest die Endpunktdatei bei jeder Anfrage neu, wenn der
Socket weg ist, und prüft `pid` mit `os.kill(pid, 0)`: existiert der Prozess
nicht mehr, ist die Datei verwaist (die Brücke löscht sie beim sauberen Stopp,
aber nicht nach einem Absturz). **Zwei laufende Editoren teilen sich eine
Endpunktdatei, der letzte Start gewinnt.** Das wird hier benannt und nicht
gelöst — pro Instanz eine Datei zu schreiben verlangt, dass der Client wählt,
und dafür gibt es noch keine Oberfläche.

### 6.4 Falls doch `mcp>=2`: die API, nachgeprüft

Gegen `mcp 2.0.0` in
`~/VSCode/horizon-web-mcp/.venv` verifiziert. Was aus 1.x im Kopf ist, stimmt
nicht mehr, und was in unserer Notiz stand, stimmt nur halb:

* `mcp.server.MCPServer` gibt es (der FastMCP-Nachfolger), ist für einen
  Durchreicher aber falsch: er registriert Werkzeuge statisch per Dekorator,
  unsere Liste kommt zur Laufzeit vom Editor.
* Richtig ist `mcp.server.lowlevel.Server` — und der hat **keine**
  `@server.list_tools()` / `@server.call_tool()`-Dekoratoren mehr. Stattdessen
  Konstruktorargumente:
  `Server(name, version=…, on_list_tools=…, on_call_tool=…)`, beides
  `async (ctx, params) -> ListToolsResult | CallToolResult`. Alternativ
  `add_request_handler(method, params_type, handler)`.
* Gefahren wird mit `mcp.server.stdio.stdio_server()` und
  `server.run(read, write, server.create_initialization_options())`.
* **Felder sind snake_case, aber die Aliase akzeptieren camelCase.** Geprüft:
  `Tool.model_validate({... "inputSchema": …})` funktioniert und landet in
  `.input_schema`; `CallToolResult.model_validate({"isError":…,
  "structuredContent":…})` ebenso; `model_dump(by_alias=True)` schreibt wieder
  camelCase heraus. Das heißt: auch auf Weg (a) wären die Antworten der Brücke
  ohne Umbau einlesbar.
* `ToolAnnotations` liegt in `mcp_types` und wird von `mcp.types`
  re-exportiert; beide Importe gehen.

### 6.5 Die `claude`-CLI finden

`HE::Proc::which("claude")` ist der Kern und deckt das Meiste ab: es ruft
`augmentToolPath()` (also `/opt/homebrew/bin`, `/usr/local/bin`) und behandelt
die Windows-Endungen. Auf dieser Maschine reicht das —
`/opt/homebrew/bin/claude` ist ein Symlink auf die npm-Installation.

Die `kToolPrefixes` kennen aber nur Homebrew, und die CLI kommt auch anders auf
die Platte. Deshalb nach dem `resolveBrew()`-Muster **zusätzlich** die
kanonischen Orte direkt prüfen, bevor aufgegeben wird:

* `~/.local/bin/claude` (nativer Installer)
* `~/.claude/local/claude` (lokale Installation)
* `~/.npm-global/bin/claude` und `$(npm prefix -g)/bin/claude`, wenn npm ohne
  Homebrew installiert wurde
* Windows: `%APPDATA%\npm\claude.cmd`

Reihenfolge: `which` zuerst (das ist, was der Nutzer im Terminal bekäme), dann
die Kandidaten. Gefunden oder nicht — die Antwort ist ein absoluter Pfad oder
leer, und leer heißt: der Knopf ist ausgegraut und sagt warum, statt einen
Befehl zu starten, der nicht existiert.

### 6.6 Der Knopf: welcher Befehl genau

```
claude mcp add horizon-editor -s user \
  -e HE_MCP_ENDPOINT=<abs. Pfad zu mcp-endpoint.json> \
  -- <python> <abs. Pfad zu he_mcp.py>
```

Vier Dinge daran sind Entscheidungen:

**`-s user`, nicht der Standard.** `claude mcp add` schreibt ohne Angabe in
`local` — die projektbezogene Konfiguration, gebunden an das aktuelle
Arbeitsverzeichnis. Ein aus dem Finder gestarteter Editor hat als
Arbeitsverzeichnis `/`. Der Eintrag landete dort, wo ihn niemand je sieht.
`-s user` ist die Konfiguration, die in allen Projekten gilt — genau das, was
die anderen Server dieses Nutzers benutzen (`claude mcp get hive` →
„Scope: User config").

**`-e HE_MCP_ENDPOINT=…`.** Der Editor kennt den Pfad exakt; das Shim müsste
ihn sonst raten (6.3). Ein Pfad mit Leerzeichen ist unkritisch, weil `HE::Proc`
argv übergibt und keine Shell-Zeile baut.

**Absolute Pfade.** Beide, Interpreter und Skript. Das hat einen Preis, der
benannt gehört: **wird die `.app` später verschoben, zeigt der Eintrag ins
Leere.** Die Testanzeige aus 6.7 findet das (Status „Failed to connect"), und
der Knopf schreibt den Eintrag beim nächsten Druck neu. Ein relativer Pfad
wäre schlechter, weil `claude` ihn gegen sein eigenes Arbeitsverzeichnis
auflöste.

**Doppelte Registrierung.** Gemessen:

```
$ claude mcp add he-probe-tmp -s user -- /bin/true      → "Added …", rc=0
$ claude mcp add he-probe-tmp -s user -- /bin/true      → "MCP server he-probe-tmp already exists in user config", rc=0
```

**Der Exit-Code ist in beiden Fällen 0.** Der Knopf darf sich also nicht auf rc
verlassen. Der saubere Weg ist `claude mcp remove horizon-editor -s user`
(Fehler wird ignoriert, wenn nichts da war) und danach `add` — damit ist der
Knopf idempotent *und* aktualisiert einen alten Eintrag mit falschem Pfad, was
ein „existiert schon, lasse ich" nicht täte. Er heißt deshalb sinnvollerweise
**„Add to Claude"** und schreibt, wenn schon einer da ist, kommentarlos neu.

**Wo liegt `he_mcp.py` zur Laufzeit?** Quelle bleibt `scripts/he_mcp.py`. Der
Editor sucht es unter `SDL_GetBasePath() / "he_mcp.py"` — dieselbe Grundlage
wie `Docs/`, `EngineContent/` und das gebündelte cmake. Damit das in beiden
Welten stimmt, zwei kleine Ergänzungen:

* `src/HE_Editor/CMakeLists.txt`: ein POST_BUILD-`copy_if_different` nach
  **beiden** Zielen, `$<TARGET_FILE_DIR:HorizonEditor>` und `${DEPLOY_EDITOR}`
  — genau das Doppelziel-Muster, das der `HE_BUNDLE_CMAKE`-Block bei `:338`
  schon benutzt. Der Entwicklerbaum ist damit abgedeckt.
* `scripts/package_macos.sh`, Abschnitt 6: eine `cp`-Zeile nach
  `Contents/Resources/`, neben denen für `Docs` und `EngineContent`.

**Der Interpreter.** Auf macOS/Linux `HE::Proc::which("python3")`, auf Windows
der Reihe nach `python3`, `python`, `py`. Wird keiner gefunden, ist der Knopf
ausgegraut mit derselben Begründung wie bei fehlender CLI. Fällt Weg (a) aus
6.2, kommt hier stattdessen der Interpreter aus dem angelegten `.venv` hin.

### 6.7 Die Testanzeige: was sie prüfen muss, damit sie nicht lügt

Auf **Tool Status**, im Idiom der Seite: `statusRow(Name, Level, Detail, Fix)`,
gespeist von einer Hintergrundprobe nach dem Muster von `HE::Sc::GitProbe`
(`src/HE_Scene/…/SourceControl/GitProbe.h`) — ein flaches Aggregat, einmal beim
Öffnen der Seite gefüllt, dazu ein „Recheck".

Vier Zeilen, weil die vier verschieden scheitern und verschiedene Antworten
haben:

| Zeile | Ok, wenn | Fix führt zu |
|---|---|---|
| `claude` CLI | gefunden (6.5) | Hinweis, wo man sie herbekommt |
| Python für das Shim | Interpreter gefunden (und, auf Weg (a), `import mcp` geht) | dito |
| Bei Claude registriert | `claude mcp get horizon-editor` findet den Eintrag | der „Add to Claude"-Knopf |
| Verbindung steht | siehe unten | Remote Control einschalten |

**Wie die letzte Zeile ehrlich wird.** Gemessen an einem absichtlich kaputten
Eintrag:

```
$ claude mcp get he-probe-tmp
  Status: ✘ Failed to connect
  Issue: CONNECTION_CLOSED: Connection closed
rc=0
$ claude mcp get hive
  Status: ✔ Connected
rc=0
```

Zwei Dinge folgen daraus:

* **Der Exit-Code ist auch hier nutzlos** — 0 in beiden Fällen. Geparst wird
  die `Status:`-Zeile, und die `Issue:`-Zeile wandert unverändert in die
  Detail-Spalte. Das ist die beste Fehlermeldung, die es gibt, weil sie von dem
  kommt, der es versucht hat.
* **`claude mcp get` startet den Server wirklich** und führt den
  MCP-Handschlag. Das ist ein echter Durchlauf und keine Existenzprüfung.

Damit „Connected" aber tatsächlich *diesen* Editor bedeutet, muss das Shim beim
`initialize` schon an der Brücke hängen: **verbindet sich das Shim erst beim
ersten `tools/list`, dann meldet ein Shim mit geschlossenem Editor brav
„Connected" und die Anzeige lügt genau da, wo sie es nicht darf.** Also:
`initialize` verbindet und authentifiziert zuerst und schlägt sonst fehl
(Prozess beendet sich mit Code ≠ 0, `claude` zeigt „Failed to connect").
Das ist die eine Stelle, an der die Reihenfolge im Shim eine
Sicherheitsaussage trägt.

**Der unabhängige Gegencheck.** Auch ein sauberes `Connected` sagt nicht, dass
das Shim *diese* Editor-Instanz erreicht hat (zwei Editoren, 6.3). Das weiß nur
die Brücke selbst: `McpBridge::clientCount()` steigt, während die Probe läuft.
Der Test kann also zusätzlich den Zählerstand vor und nach dem `claude mcp
get` vergleichen — steigt er, ist der Beweis vollständig und kostet nichts.
Ein sauberer Ausbau davon wäre ein Kennungsfeld im `auth`-Frame
(`params.client = "he_mcp/1"`), das die Brücke mitschreibt; das ist eine
Bridge-Änderung und deshalb hier nur als Option benannt.

**Threading.** `HE::Proc::run` ist synchron, und `claude mcp get`
gesundheitsprüft für Sekunden. Das Settings-Panel läuft auf dem ImGui-Thread.
Die Probe gehört also auf einen Worker, mit demselben Aufbau, den
`installToolchain(onLine)` und `startGitProbe()` schon haben: Zustand
„Checking" in der Tabelle, Ergebnis wird beim nächsten Frame gelesen. Der
„Add to Claude"-Knopf ebenso — er ist schnell, aber nicht garantiert schnell.

**Was daraus gebaut wurde (6.8, Punkt 5), mit zwei Abweichungen vom Entwurf.**
`src/HE_Editor/McpClaudeProbe.h/.cpp` (ImGui-frei wie `McpClientSetup`, geprüft
in `tests/test_mcp_claude_probe.cpp` gegen die oben gemessenen Transkripte) plus
vier Zeilen auf **Tool Status**: `Claude Code`, `MCP shim`, `Registered`,
`Claude connection`. Beide Abweichungen sind Korrekturen an 6.7, nicht
Auslassungen:

* **Der Gegencheck zählt Handschläge, nicht Clients.** 6.7 schlägt vor,
  `clientCount()` vor und nach dem `claude mcp get` zu vergleichen. Das kann
  nicht funktionieren: **ein `update()` leert die ganze Ereignisschlange**, und
  das Shim verbindet, authentifiziert und legt auf, bevor der nächste Frame
  beginnt — Verbindung und Abbruch fallen also in denselben Pump und der Zähler
  verlässt die Null nie. Stattdessen `McpBridge::authCount()`, monoton, plus
  `lastAuthClient()` aus dem `params.client`-Feld, das das Shim ohnehin schon
  schickt. Gelesen wird beides nur auf dem Frame-Thread, und erst drei Pumps
  nachdem der Worker zurückkam: die Antwort der CLI ist da, bevor dieser Prozess
  auf den Socket geschaut hat.
* **Nicht beim Start, sondern beim Öffnen der Seite.** Wie in 6.7 vorgesehen,
  aber es ist mehr als eine Terminfrage: der Check *ist* ein Verbindungsversuch.
  Er startet die CLI, die das Shim startet, das sich mit genau diesem Editor
  verbindet. Deshalb hat `AppContext` hier zwei Zustände statt des üblichen
  Null-Zeigers (`claudeProbe` null + `claudeProbing` false heißt „noch niemand
  hat gefragt"), und die Seite sagt ausdrücklich, dass dieser eine Check nicht
  lesend ist.

Dazu eine Zeile, die 6.7 nicht vorsieht und die das Einzige ist, was den Knopf
je zum zweiten Mal nötig macht: `Registered` wird **amber statt grün**, wenn der
Eintrag ein anderes Skript oder eine andere Endpunktdatei nennt als diese
Installation heute schreiben würde. Ein verschobenes `.app` startet sonst
nichts, und `claude mcp get` nennt das brav „registriert".

**Beide Pfade einmal wirklich gefahren** (echte CLI, echtes Shim, Eintrag
danach wieder entfernt), weil 6.7 nur die eine Hälfte gemessen hatte:

```
# Endpunktdatei existiert nicht  →  rc=0
  Status: ✘ Failed to connect
  Issue: -32000: cannot read the endpoint file /tmp/… (No such file or
         directory) — the editor writes it while Preferences > Editor >
         Remote Control is switched on
# Brücke läuft (Fake-Bridge aus tests/test_he_mcp.py)  →  rc=0
  Status: ✔ Connected
  Environment:
    HE_MCP_ENDPOINT=/tmp/…
```

Drei Dinge, die vorher nur angenommen waren: der `Environment:`-Block ist
**eingerückt unter der Überschrift**, nicht in der Zeile (deshalb sucht der
Parser die Variable im ganzen Text statt in einer Zeile); die `Issue:`-Zeile
trägt die Fehlermeldung **des Shims selbst** durch, was die beste
Diagnose ist, die die Seite haben kann; und die Brücke zählte während des
`claude mcp get` **genau einen** Handschlag — das ist der Gegencheck aus 6.7,
im Kleinen vorgeführt. Was damit weiterhin **nicht** verifiziert ist: dieselbe
Zeile mit dem echten Editor dahinter, also `authCount()` statt eines
Python-Zählers. Das ist der Durchlauf aus 6.8 Punkt 8 und braucht ein Fenster.

### 6.8 Was der Zuschnitt der Folgeschritte daraus macht

1. **Entscheidung 6.2** (Chefchen): stdlib oder `mcp>=2`. Alles Weitere hängt
   nur an der Interpreter-Zeile.
2. **`scripts/he_mcp.py`** nach 6.3, plus `--selftest`. Prüfbar ohne Editor:
   ein Python-Test kann die Brückenseite als Socket nachbauen (der Handschlag
   ist zwölf Zeilen) und das Shim gegen `tools/list` fahren.
3. **Auslieferung**: die zwei Kopierzeilen aus 6.6, und in `package_macos.sh`
   die Prüfliste am Ende (`ok Resources/…`) mitziehen, damit ein fehlendes
   Skript im DMG auffällt statt still zu fehlen.
4. **CLI- und Interpreter-Suche** nach 6.5 als eigenes, testbares Stück —
   kein ImGui darin.
5. **Probe + Tool-Status-Zeilen** nach 6.7, auf einem Worker.
6. **„Add to Claude"-Knopf** auf der Remote-Control-Seite nach 6.6.
7. **Handbucheinträge** für jedes neue Bedienelement in `EditorHelp.cpp`,
   `topic` leer wie bei den drei bestehenden Remote-Control-Einträgen
   (`EditorHelp.cpp:2161`). Ohne sie färbt sich `editor_help_audit` rot —
   das ist ein ctest (`tests/CMakeLists.txt:578`), kein Nice-to-have.
8. **Der Durchlauf aus 5.3**, den es bisher nicht geben konnte: Editor auf,
   Remote Control an, Knopf drücken, in einer Claude-Instanz einen Würfel
   setzen, im Editor Undo sehen.

### 6.9 Offen, absichtlich

> Fortgeschrieben in Schritt 5. Erledigtes steht als `→` unter seinem Punkt;
> was offen geblieben ist, steht in 7.6.

* **6.2 ist nicht entschieden.** Bewusst nicht selbst entschieden, weil das
  Thema `mcp>=2` wörtlich vorgibt und die Empfehlung dagegen steht.
  * → **Entschieden: Weg (b), reine Standardbibliothek.** Begründung und Folgen
    in 7.2.
* **Nur auf macOS gemessen.** Die `claude`-Ausgaben oben stammen aus einer
  einzigen Installation. Die `Status:`-Zeile ist mit Unicode-Häkchen dekoriert
  (`✔`/`✘`); der Parser sollte auf `Failed to connect` bzw. `Connected` im Text
  prüfen und nicht auf das Zeichen. Ob eine andere CLI-Version anders
  formatiert, ist ungeprüft — deshalb ist der Zählerstand-Gegencheck aus 6.7
  mehr als Zierde.
* **Zwei Editoren, eine Endpunktdatei.** Benannt in 6.3, nicht gelöst.
  * → **Weiter nicht gelöst, aber nicht mehr unsichtbar.** Die Zeile
    „Claude connection" wird nur grün, wenn der Handschlag-Zähler *dieser*
    Brücke währenddessen hochgeht; hält ein zweiter Editor die Datei, sagt die
    Zeile genau das (7.4). Wer die Datei bekommt, entscheidet weiter, wer zuerst
    einschaltet.
* **Kein Handbuch-Kapitel.** Unverändert der Punkt aus 5.3; ein `claude mcp
  add`-Rezept kann erst hinein, wenn das Skript ausgeliefert wird — dann aber
  wirklich, und in `collaboration.html`.
  * → **Geschrieben.** `collaboration.html#remote-control`, mit dem Rezept
    darin, weil das Skript jetzt ausgeliefert wird (7.5).

---

## 7. Das Shim und der Knopf: was gebaut wurde

Nachgetragen zum Abschluss des Folgethemas (Schritt 5, 08.09.2026), im selben
Verhältnis zu Kapitel 6 wie Kapitel 5 zu den Kapiteln davor: der Entwurf oben
bleibt stehen, hier steht, wo die Umsetzung ihm gefolgt ist und wo nicht.

### 7.1 Steht

| Was | Wo | Aus |
|---|---|---|
| stdio-Shim, nur Standardbibliothek, ab Python 3.8, kennt selbst keine Werkzeuge | `scripts/he_mcp.py` | `58a9da3d`, `ecfddf2e` |
| Testbrücke aus echtem Socket + echten Pipes, Shim als Unterprozess | `tests/test_he_mcp.py`, ctest `he_mcp_shim` | `58a9da3d` |
| CLI-, Interpreter- und Skriptsuche, Argumentvektoren, Auswertung — ImGui-frei | `src/HE_Editor/McpClientSetup.h/.cpp`, `tests/test_mcp_client_setup.cpp` | `55431c3a` |
| Knopf „Add to Claude" + „Look for Claude Again", auf einem Worker | `src/HE_Editor/EditorSettingsPanel.cpp:655` | `55431c3a` |
| Verbindungsprobe (`claude mcp get`, echter Handschlag) — ImGui-frei | `src/HE_Editor/McpClaudeProbe.h/.cpp`, `tests/test_mcp_claude_probe.cpp` | `356a6c9d` |
| Vier Zeilen auf **Tool Status**, `Recheck all` fasst sie mit | `EditorSettingsPanel.cpp:2100` | `356a6c9d` |
| Handschlag-Zähler der Brücke, für den Gegencheck | `src/HE_Editor/McpBridge.h:130` (`authCount`, `lastAuthClient`) | `356a6c9d` |
| Auslieferung neben dem Editor und ins `.app`, mit Prüfzeile im Packaging | `src/HE_Editor/CMakeLists.txt`, `scripts/package_macos.sh` | `55431c3a` |
| Handbuchkapitel, fünf Bedienelemente verlinkt | `collaboration.html#remote-control`, `EditorDeps/Docs/he-docs.json`, `EditorHelp.cpp:2161` | Schritt 5 |

### 7.2 Die Entscheidung zu 6.2, und was sie kostet

Weg **(b)**: das Shim benutzt `json`, `socket`, `struct`, `sys`, `os` — sonst
nichts. Kein `pip install`, kein Virtualenv, jedes Python ab 3.8.

Der Grund ist der Knopf. „Add to Claude" verspricht eine Registrierung, die
funktioniert; hinge sie an einem Paket, das der Editor nicht mitliefert, wäre
das ein Versprechen, das er nicht halten kann — ein frisch installiertes Python
ohne `mcp`-Modul ist der Normalfall, nicht die Ausnahme. Das im Thementext
vorgegebene `mcp>=2` war die Grobvorgabe, gegen die der geprüfte Befund aus 6.2
stand; die Entscheidung dagegen ist bewusst getroffen worden und nicht
weggerutscht.

Was es kostet: das Shim muss den Handschlag selbst beantworten (`initialize`,
`notifications/initialized`, `tools/list`, `tools/call`) statt ihn einem SDK zu
überlassen, und muss die Protokollversion des Clients zurückspiegeln. Das sind
die rund vierzig Zeilen in `McpServer` — der Preis dafür, dass die einzige
Voraussetzung „ein Python" heißt. 6.4 (die geprüfte `mcp`-2.x-API) bleibt im
Dokument, falls die Rechnung sich einmal umdreht.

### 7.3 Der Knopf: drei Dinge, die nicht offensichtlich sind

* **`-s user`, nicht der Standard.** `claude mcp add` schreibt sonst in die
  Konfiguration des *Arbeitsverzeichnisses* — und ein aus dem Finder
  gestarteter Editor hat „/" als solches. Der Eintrag landete, wo nie jemand
  hinsieht.
* **Immer neu schreiben, nie verweigern.** `claude mcp add` auf einen
  bestehenden Eintrag meldet „already exists" und beendet sich mit **Null**.
  Der Rückgabewert allein kann „registriert" also nicht von „alten, falschen
  Pfad stehengelassen" unterscheiden. Deshalb erst `mcp remove`, dann `mcp add`,
  unter genau einem Namen (`horizon-editor`).
* **Der Knopf ist nicht an den Schalter gekoppelt.** Registrieren schreibt eine
  Zeile in Claudes Konfiguration und fasst den Listener nicht an. Ihn zu
  sperren, solange Remote Control aus ist, würde nur ein Ritual beibringen, das
  es nicht gibt. Was zusammengehört, sagt der Hilfetext: der Schalter muss an
  sein, *wenn Claude fragt*.

### 7.4 Die Zeile, die nicht behaupten kann

Ausführlich in 6.7 unter „Was daraus gebaut wurde". Der Kern: `claude mcp get`
**startet** das Shim und führt den Handschlag wirklich aus, und grün wird die
Zeile nur, wenn zusätzlich der Handschlag-Zähler *dieser* Brücke währenddessen
hochgegangen ist. „Connected" allein sagt nur, dass irgendein Editor geantwortet
hat. Genau darum ist der Zähler und nicht `clientCount()` der Gegencheck: ein
`update()` leert die ganze Ereignisschlange, und das Shim verbindet,
authentifiziert und legt im selben Pump wieder auf.

### 7.5 Das Handbuch, in der Reihenfolge, die 5.3 vorschreibt

Der Abschnitt steht auf der Website-Seite `collaboration.html` als
`#remote-control` („Remote Control", zwischen „Notifications" und
„Message Reference", mit Eintrag in der Seitenleiste): Schalter und Port, die
Endpunktdatei und ihre drei Plattformpfade, das Shim und warum es überhaupt
existiert, der Befehl des Knopfes wörtlich, die vier Tool-Status-Zeilen mit dem,
was Grün jeweils *bedeutet*, und was ein Client darf (undoable, Locks gelten).

Danach `scripts/build_docs_bundle.py`: 112 → 113 Abschnitte, das Bundle liegt
committet in `EditorDeps/Docs/he-docs.json` — Pflicht, weil ein frischer Klon
und die CI ohne den Website-Checkout bauen müssen. Erst danach die
`topic`-Felder, alle fünf Remote-Control-Einträge in `EditorHelp.cpp` auf
`collaboration#remote-control`. Umgekehrt hätte `test_editor_help` rot gezeigt,
und zwar zu Recht.

**Die Website ist nicht deployt.** Der Text liegt im Geschwister-Checkout
`Website/HorizonEngineDocs/collaboration.html` und wartet dort auf den
Deploy-Schritt, den der Mensch bestätigt. Der Editor zeigt das Kapitel trotzdem
schon: er liest das Bundle, nicht die Seite.

Der Suchindex der Website (`HorizonEngineDocs/docs-index.json`) ist **bewusst
nicht** mitgeneriert: `deploy.py` baut ihn selbst, und die Datei stand zu dem
Zeitpunkt schon mit fremden Änderungen im Arbeitsverzeichnis — sie neu zu
schreiben hätte fremde Arbeit überschrieben, um etwas vorzuziehen, das der
Deploy ohnehin tut.

### 7.6 Was offen bleibt

* **Der Ende-zu-Ende-Durchlauf mit einem Menschen davor.** Der Handschlag ist
  belegt (7.4), das Setzen eines Würfels aus einer echten Claude-Sitzung und das
  Undo danach im Editor nicht. Aus einer Testsuite heraus ist es auch nicht zu
  belegen — es braucht jemanden, der zusieht.
* **Nur auf macOS gemessen.** Unverändert der Punkt aus 6.9: die
  `claude`-Ausgaben, aus denen die Probe liest, stammen aus einer Installation.
  Der Parser prüft auf die Wörter, nicht auf die Häkchen, und der Gegencheck
  fängt eine falsche Zuordnung ab — aber Windows und Linux sind ungeprüft.
* **Zwei Editoren, eine Endpunktdatei.** Sichtbar gemacht, nicht gelöst.
* **Die Website ist nicht deployt** (7.5).

## 8. Nachtrag: die Asset-Werkzeuge (Folgethema 29, Schritt 1)

Ein Nutzungstest aus einer echten Claude-Sitzung hat die Lücke benannt, die
alles davor unbenutzbar machte: Entities setzen und HorizonCode schreiben geht,
aber **kein Werkzeug konnte ein Asset benennen**. Jede Mesh-Referenz, jedes
Material, jede Klasse zum Spawnen wird über einen Pfad oder eine UUID
adressiert, und die bekam ein Client nirgends her.

Fünf Werkzeuge schließen das: `asset_resolve` (Pfad → UUID, Typ, geladen?),
`asset_list` (Ordner lesen, rekursiv, nach Typ gefiltert), `asset_create`,
`asset_delete`, `asset_move`.

### 8.1 Kein EditorCommands darunter, und das ist eine Entscheidung

Das Gateway kennt fünf Entity-Befehle und legt zu jedem einen Undo-Eintrag an.
Für Assets gibt es keinen: „Löschen ist die eine Content-Browser-Operation ohne
Undo" (`AssetRefScan.h`), und ein Rename ist ein Dateisystem-Move plus das
Umschreiben jedes Referrers auf der Platte. Dem Gateway das Invertieren
beizubringen hieße, ein Undo zu erfinden, das der Editor selbst nicht anbietet —
eine Verhaltensänderung im Kostüm einer Verkabelung. Also derselbe Hooks-Weg wie
bei `McpHcHooks`: die Prüfungen, die ein Mensch vom Content Browser geschenkt
bekommt, werden hier einmal gestellt, und die Absagen behalten die Drahtnamen
der Entity-Werkzeuge.

### 8.2 Die drei Regeln, die wichtiger sind als die Werkzeuge

* **Ein Leser lädt nicht.** `asset_resolve` und `asset_list` antworten aus dem
  META-Chunk der Datei, dem Pfad-Index und dem Header-Sniff-Cache. `loadAsset`
  würde den dichten Asset-Pool verschieben und jeden Zeiger ungültig machen, den
  der Editor gerade hält. Eine Frage darf die Antwort nicht verändern.
  * Dazu gehört die Reihenfolge: **die Platte zuerst, der Index als Rückfall.**
    `already_exists` schickt den Client auf „lösch das erst" — und ein Löschen
    entlädt nicht (so macht es der Content Browser auch), das alte Asset bleibt
    also unter dem freigewordenen Pfad resident. Über den Index gefragt käme die
    UUID einer Datei zurück, die es nicht mehr gibt.
* **Der Stub-Writer ist geteilt.** Er lag als Lambda im Create-Menü und war
  damit nur aus einem ImGui-Popup erreichbar. Jetzt `AssetStubWriter.h`, zwei
  Aufrufer, keine zweite Theorie davon, was in einem frischen Input Action
  steckt.
* **In einer Session sind Löschen und Verschieben Anfragen**, keine Taten: sie
  brechen jede Referenz auf den alten Namen, also entscheidet der Host und alle
  Maschinen bewegen sich gleichzeitig. Die Werkzeuge melden `applied: false,
  requested: true`, statt eine Änderung zu behaupten, die sie nicht gemacht
  haben.

### 8.3 Die Grenze

Das Registry-Kopfstück verspricht einem externen Client keinen Dateizugriff.
Diese Werkzeuge nehmen Pfade, also ist das Versprechen nur so gut wie
`checkPath`: jedes Argument ist content-relativ, wird über
`resolveAbsolutePath` aufgelöst und danach daraufhin geprüft, ob es wirklich
**in** einem bekannten Root *landet*. Ein absoluter Pfad wird als solcher
abgelehnt, nicht stillschweigend um den führenden Schrägstrich gekürzt: „/Materials/Rock.hasset"
und „/Users/jemand/.ssh/id_rsa" sind danach dieselbe Sorte String, und der
zweite käme als schlichtes „nicht gefunden" zurück.

### 8.4 Was bewusst offen bleibt

* **Ordner** werden weder gelöscht noch verschoben. Beides zieht einen ganzen
  Teilbaum mit, und die Liste dazu sieht nur der Content Browser.
* **`Engine/`** ist schreibgeschützt — dieselbe Sperre, die der Panel-Kontext
  `engineLocked` nennt.
* **Szenen anlegen** gehört Schritt 2 (`scene_create`): eine `.hescene` ist
  JSON, kein HAsset, und der Stub-Writer schriebe dort das Falsche. (Genau das
  tat der Content Browser bis dahin — siehe 9.4.)
* **Material-, Input- und Terrain-Inhalte** ändern die Werkzeuge nicht. Sie
  legen die Dateien an und bewegen sie; was drin steht, sind die Schritte 3 bis
  6 dieses Themas. (Terrain hat seine eigenen Werkzeuge seit Schritt 9, siehe
  Kapitel 10; Widgets seit Schritt 4, siehe Kapitel 11; Input seit Schritt 5,
  siehe Kapitel 12; Materialien seit Schritt 6, siehe Kapitel 13.)
* **Eine Material-Instanz** kann `asset_create` nicht anlegen: ein Stub ist immer
  ein Master, und eine Datei ohne Graph **und** ohne Eltern ist das eine, was
  eine Instanz nicht sein darf. Das macht `material_create_instance` (13.4).

### 8.5 Nebenbei gefunden

`HE::assetTypeName` (`Types/Enums.h`) kannte `BoneMask` und `BlendSpace` nicht —
genau der Durchfall, den der Kommentar über dem Switch vorhersagt, samt Warnung.
Beide meldeten sich gegenüber `content.typeName`, der C-ABI und allem, was
Assets nach Typnamen auflistet, als unbekannt. Ein Test hält Hin- und
Rückrichtung jetzt zusammen.

## 9. Nachtrag: die Szenen-Werkzeuge (Folgethema 29, Schritt 2)

`scene_save`, `scene_create`, `scene_open` — die Hälfte, ohne die alles, was
die Entity-Werkzeuge tun, mit der Sitzung stirbt. Ein Client konnte hundert
Objekte setzen und hatte keinen Weg, davon irgendetwas zu behalten, keinen Weg,
ein zweites Level anzufangen, und keinen Weg, zu einem vorhandenen zu wechseln.
`scene_info` sagte ihm, die Szene sei dirty, und bot nichts dagegen an.

### 9.1 Wieder kein EditorCommands darunter, und diesmal schärfer

Das Gateway kennt fünf Entity-Befehle und schreibt zu jedem einen Undo-Eintrag,
der ihn umkehrt. `openScene` **ersetzt die Welt und leert danach die
Undo-Historie** (`EditorApplication::openScene`) — es gibt nichts umzukehren,
und eine Inverse zu erfinden hieße, ein Undo zu erfinden, das der Editor selbst
nicht anbietet. Also dieselbe Bauform wie bei den Asset-Werkzeugen: die Prüfungen,
die ein Mensch von der Oberfläche geschenkt bekommt, werden hier einmal über
Hooks gestellt, und die Ablehnungen behalten die Drahtnamen der Entity-Werkzeuge.

`saveScene` und `openScene` in den Hooks sind die **eigenen Member** von
`EditorApplication`, keine Nachbauten: ein MCP-Save nimmt damit das
Szenen-Thumbnail auf und ein MCP-Open lädt die Asset-Referenzen vor und wärmt
die Material-Pipelines, genau wie das Datei-Menü. Beide geben jetzt `bool`
zurück — die UI-Aufrufer ignorieren das (Log und Titelleiste sagen es ihnen),
der Client am anderen Ende kann weder das eine noch das andere sehen.

### 9.2 Die eine Wache, die von Hand nachgebaut werden musste

Ein Mensch erreicht `openScene` nie direkt: Datei > Szene öffnen läuft über
`requestGuarded` (`EditorUI.cpp`), und das hebt bei einer dirty Szene die
Speichern-Nachfrage. Ein Client sieht diesen Dialog nicht. Also lehnt
`scene_open` eine dirty Szene mit `dirty` ab, solange er nicht
`discard_changes: true` sagt. Eine Stunde Platzierungsarbeit still wegzuwerfen
ist die Lüge, die hier am teuersten wäre, und sie ist für den Aufrufer
prinzipiell unsichtbar. `scene_open` speichert deshalb auch **nicht** von selbst
vorher: wer beides wollte, ruft `scene_save` und dann `scene_open` — ein
ungefragter Save dagegen ließe sich nicht zurücknehmen.

### 9.3 Play-Modus, alle drei

Nicht Vorsicht: Play-in-Editor läuft **in** `m_editorWorld`, nachdem der
Zustand in eine Temp-Datei geschnappt wurde (`setPlayMode`). Die Welt, die ein
Save während des Spielens schriebe, ist also die, in der der Spieler gerade
herumgelaufen ist — ein „Speichern", das drei Minuten Play-Physik über das
gebaute Level legt, ist Datenverlust, der wie ein Erfolg aussieht.

### 9.4 Nebenbei gefunden: „Neue Szene" im Content Browser war kaputt

Die Zeile `tryCreate("NewScene", ".hescene", HE::AssetType::Scene)` rief
`writeAssetStub` — und schrieb damit einen **binären HAsset-Container** an einen
`.hescene`-Pfad. `SceneSerializer::loadJSON` liest den als „not valid JSON",
`openScene` loggt einen Fehler und lässt eine leere Welt ohne Pfad stehen. Der
Content Browser bot also eine Datei zum Öffnen an, die er selbst erzeugt hatte
und die niemand öffnen konnte.

Gefixt über einen geteilten Schreiber, dasselbe Prinzip wie beim Stub-Writer:
`HE::Ed::writeEmptySceneFile` serialisiert eine **leere Welt** (statt ein
Literal hinzuschreiben), das Panel und `scene_create` rufen ihn beide. Eine
default-konstruierte `HorizonWorld` ist genau das, was Datei > Neue Szene
hinterlässt — Root-Entity und sonst nichts, kein Himmel, kein Licht, keine
Kamera. Ein Test hält beide Richtungen fest: der Stub an `.hescene` wird als
Szene abgelehnt, die geschriebene leere Szene öffnet.

### 9.5 Was bewusst offen bleibt

* **Additives Laden** (`openSceneAdditive`) ist ein Merge in die laufende Welt
  mit eigenen Physik- und Undo-Regeln, nicht Szenen-Persistenz.
* **Eine „neue leere Szene" ohne Datei** gibt es nicht. `scene_create` schreibt
  immer eine Datei; ein Werkzeug, dessen einzige Wirkung das Wegwerfen der
  offenen Szene ist, wäre eine Waffe ohne Ziel.
* **Keine Ablehnung in einer Collab-Sitzung.** Ein Mensch wird auch nicht
  abgelehnt, und eine Sperre, die MCP hat und das Datei-Menü nicht, wäre eine
  Verhaltensänderung als Klempnerei verkleidet. Der Sitzungszustand steht
  stattdessen im Ergebnis.
* **Die Endung ist nicht verhandelbar.** Fehlt sie, wird `.hescene` ergänzt
  (wer „Levels/Main" sagt, meint „Levels/Main.hescene"); jede *andere* Endung
  wird abgelehnt statt korrigiert.

## 10. Nachtrag: die Terrain-Werkzeuge (Folgethema 29, Schritt 9)

`terrain_info`, `terrain_heightmap`, `terrain_sculpt`, `terrain_paint` — vier
Werkzeuge für die eine Komponente, die `entity_get` und
`entity_set_components` nicht sinnvoll adressieren können.

### 10.1 Warum Terrain eigene Werkzeuge braucht

Die Nutzlast einer `TerrainComponent` sind zwei Blobs: `sculptHeightsB64`
(res² Floats, 263k davon bei der Auflösung, auf die der Chunk-Bauer schnappt)
und `layerWeightsB64` (weightRes² RGBA-Texel). Die generischen
Komponenten-Werkzeuge reichen die als Base64 heraus und nehmen sie so wieder
an — das ist keine Schnittstelle, sondern ihre Abwesenheit. Ein Client kann
daraus keine Höhe lesen, keine einzelne ändern, ohne das ganze Feld neu zu
kodieren, und „heb hier den Boden an" gar nicht erst ausdrücken.

Die vier sprechen deshalb das Vokabular des Landscape-Modus: eine
**Weltposition**, ein Pinselradius mit Falloff, eine Operation. Die Mathematik
ist `TerrainSculpt` (Höhen, neu) und `TerrainPaint` (Layer-Gewichte,
vorhanden) — dieselben Funktionen, aus denen die Pinsel des Editors gebaut
sind, damit ein Client keine Landschaft erzeugen kann, die der Editor von Hand
nicht auch erzeugt hätte.

### 10.2 Diesmal doch durch das Gateway

Anders als bei den Asset- und Szenen-Werkzeugen: eine Terrain-Änderung **ist**
eine Komponenten-Änderung an einer Entity, also genau das, was
`Command::setComponents` ist. Der Pinsel läuft auf einer **Kopie** der
Komponente, das Ergebnis geht als Komponenten-Patch durch das Gateway — und
damit sind Undo, das Publizieren in eine Collab-Sitzung, die
Play-Modus-Ablehnung und das Lock-Gate ohne eine zweite Kopie von irgendetwas
davon dabei.

### 10.3 Die eine Schreiboperation außerhalb des Gateways

`TerrainComponent` trägt Felder, die ausdrücklich nie serialisiert werden: die
UUID der Weightmap-Textur, das Chunk-Gitter, für das die Chunks zuletzt gebaut
wurden, und das Region-Dirty-Rechteck. Das Gateway baut die Komponente aus dem
Szenen-JSON neu, das die nicht tragen kann — sie landet also mit dem Default:
keine Weightmap-Textur (der nächste Tick registriert **eine zweite** und die
erste ist verloren), kein bekanntes Chunk-Gitter und `dirty`, also ein Neubau
aller 64+ Chunks für einen Pinselabdruck von zehn Metern. Nach dem Befehl
werden diese Laufzeitfelder deshalb auf die neue Komponente übertragen und das
Dirty-Rechteck auf die Pinselausdehnung gesetzt. Das ist ein Schreibzugriff auf
die Welt am Gateway vorbei und bewusst der kleinstmögliche: er berührt nichts,
was je eine Szenendatei, ein Undo-Eintrag oder ein Peer zu sehen bekäme. Ein
Test setzt die drei Felder vorher und hält fest, dass sie den Umweg überleben.

### 10.4 Eine Koordinate auf der ganzen Schnittstelle: Welt

Jedes x, z und jede Höhe ist Weltraum, auch die Zahlen, die aus
`terrain_heightmap` zurückkommen. Die Komponente speichert terrain-**lokal**,
die Pinsel nehmen lokal — umgerechnet wird also genau einmal pro Werkzeug, am
Rand, gegen `HE::worldPositionOf`. Nie gegen `tc.worldMatrix`: das Feld ist nur
so frisch wie das letzte `propagateTransforms`, und ein Terrain, das in
demselben Frame entstanden ist, antwortet mit der Identität. Lokale Koordinaten
herauszureichen wäre weniger Code und die schlechtere Schnittstelle gewesen: bei
einer Landschaft im Ursprung — und das sind die meisten — bleibt der Fehler bis
zu dem einen Projekt unsichtbar, in dem sie es nicht ist.

### 10.5 Der 2ⁿ+1-Schnapp, vorgezogen

`TerrainSystem::updateTerrains` schnappt `resolution` auf 2ⁿ+1 und resampelt
dabei das Höhenfeld, damit LOD0-Vertices exakt auf Quellgitterpunkten landen.
Passiert das **nach** einem Schreibvorgang, sind die Zahlen, die ein Client
gerade gesetzt hat, verschoben und der Gitterschritt, den er gemessen hat, ein
anderer. `TerrainSculpt::ensureHeights` nimmt den Schnapp deshalb vorweg, und
`terrain_info` meldet `resolutionSnapsTo`, solange er noch aussteht.

### 10.6 Was bewusst offen bleibt

* **Der interaktive Pinsel wurde nicht umgehängt.** `TerrainTools.cpp` ist
  dt-getaktet und strichgebunden (Flatten-Ziel und Ramp-Start werden beim
  Mausdruck erfasst); ein Umbau darauf ist eine Verhaltensänderung, die
  headless nicht nachweisbar ist. Die Blendfaktoren in `TerrainSculpt` sind
  bewusst dieselben, damit ein späterer Tausch eine Zeile ist.
* **Ramp** hat keine Entsprechung: der Pinsel läuft zwischen zwei Punkten, die
  ein Einzelabdruck nicht kennt.
* **Kein Massen-Schreiber** (`terrain_set_heights`). Ein Höhenfeld aus einem
  Zahlen-Array zu setzen ist die naheliegende Ergänzung, aber ein eigener
  Schritt: die Frage, was mit einem Array passiert, das nicht auf das Gitter
  passt, ist die ganze Arbeit daran.
* **`heightmapTexture`** wird vom Szenen-Schreiber nie ausgegeben und von
  nichts gesetzt (Phase-2-Platzhalter). Die Werkzeuge tragen es deshalb auch
  nicht mit — das gehört zum Serialisierer, nicht hierher.

## 11. Nachtrag: die Widget-Werkzeuge (Folgethema 29, Schritt 4)

`widget_tree`, `widget_types`, `widget_add`, `widget_remove`, `widget_move`,
`widget_set_properties`, `widget_set_anchor`, `widget_save` — acht Werkzeuge für
den Elementbaum eines UI-Widget-Assets. Datei: `src/HE_Editor/McpToolsWidget.cpp`,
Tests: `tests/test_mcp_tools_widget.cpp`.

### 11.1 Warum Widgets eigene Werkzeuge brauchen

Derselbe Grund wie beim Terrain, eine Schicht höher. Der ganze Inhalt eines
UI-Widget-Assets ist ein String, `UIWidgetAsset::treeJson`, und die
Asset-Werkzeuge können die Datei anlegen, verschieben und löschen — aber keinen
einzigen Knopf hineinsetzen. Einem Client das JSON zum Umschreiben zu geben wäre
der Base64-Fehler in hübscherer Kodierung: die Ids sind eine Nummerierung, die er
selbst führen müsste, Geschwisterreihenfolge ist Vektorreihenfolge, und ein
Umhängen, das nur `parentId` schreibt, lässt das Element dort stehen, wo es
zufällig stand (siehe `UIWidgetTree::moveElement`).

Die Werkzeuge sprechen deshalb die Sprache des Designers: eine Element-Id, ein
Elternteil, ein Platz unter den Geschwistern, ein Anker aus den sechzehn
UMG-Rechtecken, und Eigenschaften **beim Namen** aus genau der Tabelle, die das
Details-Panel zeichnet (`UIElement::allProperties`).

### 11.2 Zwei Orte, an denen ein Widget lebt — und immer nur einer davon

Hält ein Designer-Tab das Asset, ist der Baum **dieses Tabs** die Wahrheit: das
geladene Asset ist nur eine Kopie, die der Tab bei jedem Edit neu schreibt, und
das nächste Save des Menschen schreibt den Tab. Eine Mutation geht deshalb in den
LIVE-Baum (`McpWidgetHooks::liveTree`) und endet mit `markEdited` — Undo-Schnappschuss
und Dirty-Mark, genau was das Panel nach einem menschlichen Edit tut. Auf die
Platte kommt nichts: der Tab hat jetzt ungesicherte Änderungen, was der wahre
Zustand ist, und `widget_save` schreibt sie.

Hält kein Tab das Asset, gibt es keine zweite Kopie, mit der man kollidieren
könnte — dann bearbeiten die Werkzeuge `treeJson` über den ContentManager und
schreiben die Datei sofort.

Das ist eine **bewusste Abweichung** vom `McpHcHooks`-Präzedenzfall, der ein
Klassen-Asset ohne offenes Panel schlicht ablehnt. Die Ablehnung dort schützt vor
einer zweiten Kopie, die das Save des Menschen überschreibt; ohne offenen Tab kann
dieses Rennen nicht stattfinden, und ablehnen hieße, ein mit `asset_create`
angelegtes Widget könnte nie gefüllt werden. Der Preis ist, dass der Plattenweg
kein Undo hat — derselbe Preis, den die Asset-Werkzeuge schon zahlen. Jedes
mutierende Ergebnis sagt in `target` (`"editor"` / `"disk"`), welchen Weg es
genommen hat, damit ein Client nie raten muss, wo seine Änderung liegt.

### 11.3 Drei Fallen, an denen ein stiller Fehler entstanden wäre

* **`setPropAny` schreibt einen unbekannten Namen nirgendwohin** und sagt nichts
  dazu. Auf dieser Schnittstelle hieße das: ein Client glaubt, er habe „Text" auf
  einem Panel gesetzt, und sieht keinen Grund, noch einmal hinzusehen. Jeder Name
  wird deshalb vorher in `allProperties()` nachgeschlagen; ein Fehlschlag ist eine
  Ablehnung, die die vorhandenen Namen mitbringt.
* **`uiWidgetTypeFromName` antwortet auf alles Unbekannte mit `Panel`.** Ein
  ungeprüfter Typname wäre also stillschweigend ein Panel geworden. Der Name wird
  gegen die Registry geprüft.
* **`UIWidgetTree::add` setzt `parentId` nicht und fragt `acceptsChildren()`
  nicht** — das tut nur `moveElement`. Das Elternteil wird deshalb hier geprüft,
  `parentId` vor dem `add` geschrieben und ein gewünschter Platz unter den
  Geschwistern als `moveElement` nachgeschoben.

Dazu die Regel aus den anderen Dateien: erst alles lesen, dann schreiben. Eine
abgelehnte `widget_set_properties` hat **nichts** geschrieben, auch nicht die
gültigen Werte davor — ein halb angewendeter Aufruf ist der eine Fehler, aus dem
ein Client nicht herausfindet, weil er nicht weiß, welche Hälfte gelandet ist.

### 11.4 Nebenbei gefunden: was die Datei nicht mitträgt

Ein paar Eigenschaften sind Laufzeitzustand, den das Widget-Format bewusst nicht
persistiert — allen voran `Item Count` einer ListView („eine Anwendung, die mit
der Zeilenzahl des letzten Laufs wieder aufmacht, zeigt Zeilen für Daten, die sie
noch nicht geladen hat", `UIElements.h`). Setzen gelingt, liest sich als die
gewünschte Zahl zurück, und ist beim nächsten Laden weg.

Ein Client kann das nicht sehen. `widget_set_properties` fragt es deshalb selbst,
mit der einzigen Methode, die nicht vom Schreiber abdriften kann: Baum
serialisieren, zurücklesen, vergleichen. Was den eigenen Rundlauf nicht überlebt,
steht im Ergebnis unter `notPersisted` statt später entdeckt zu werden.

### 11.5 Was bewusst offen bleibt

* **Die Leinwand selbst** (`canvasWidth/Height`, `scaleMode`, `description`,
  `themeAsset`) wird gelesen, aber nicht geschrieben. Ein Stub kommt auf
  1920×1080 zur Welt, was der Normalfall ist; ein `widget_set_canvas` ist die
  naheliegende Ergänzung und gehört in denselben Schritt wie die
  Widget-Parameter.
* **Der Logikgraph** (`UIWidgetAsset::graphJson`) wird nicht angefasst. Das ist
  ein HorizonCode-Graph und gehört den `hc_`-Werkzeugen; er hängt nur an einem
  Dokumentschlüssel, den `McpHcDoc` heute nicht kennt.
* **Widget-Parameter, Animationen, Theme-Bindungen** (`UIWidgetTree::params`,
  `animations`, `UIElement::themeRoles`/`textKeys`) — jedes davon ist eine eigene
  Vokabel, keine Eigenschaft.
* **Der Plattenweg veröffentlicht nicht in eine Kollaborationssitzung.** Ein
  Element-Edit ist Sache von `CollabDocSync`, und dessen Adapter hängt am Spiegel
  eines offenen Tabs. In einer Sitzung arbeitet man am offenen Tab — der Weg, der
  dann ohnehin genommen wird.
* **Kein `widget_duplicate`.** Das Panel hat `duplicateSubtree`; als Werkzeug
  wäre es nützlich und ist eine eigene Frage (welche Ids kommen zurück, was
  passiert mit Namen), nicht ein Nebenprodukt dieses Schritts.

## 12. Nachtrag: die Input-Werkzeuge (Folgethema 29, Schritt 5)

`input_actions`, `input_mappings`, `input_bindable`, `input_action_set`,
`input_mapping_bind`, `input_mapping_unbind` — sechs Werkzeuge für die beiden
Asset-Typen, die entscheiden, ob eine Taste überhaupt etwas tut. Datei:
`src/HE_Editor/McpToolsInput.cpp`, Tests: `tests/test_mcp_tools_input.cpp`
(31 Testfälle).

### 12.1 Der eine Grund, der alles andere begründet: der Loader schweigt

Auch hier ist der Inhalt beider Assets je **ein** JSON-String
(`InputActionAsset::json`, `InputMappingContextAsset::json`), die
Asset-Werkzeuge können also die Dateien anlegen und bewegen und keine Taste
hineinschreiben. Aber der Grund für eigene Werkzeuge ist diesmal ein anderer und
ein schärferer als bei Widgets: **was der Loader mit einem Namen tut, den er
nicht kennt.**

`HE::applyInputMappingContext` **überspringt** ihn. Lautlos. Ein Kontext, in dem
jeder Tastenname falsch geschrieben ist, lädt ohne eine einzige Meldung, zeichnet
im Panel vollständig korrekt und ergibt ein Spiel, das die Tastatur ignoriert.
„Spacebar" statt „Space" ist keine Fehlermeldung, sondern eine Bindung, die in
der Datei steht, im Editor zu sehen ist und im Spiel nichts tut — und ein Client
kann das per Konstruktion nicht bemerken.

Deshalb wird die Vokabel **hier** geprüft, mit genau den Funktionen, die der
Loader aufruft: `SDL_GetScancodeFromName`, `SDL_GetGamepadButtonFromString`,
`HE::mouseButtonFromName`, `HE::axisSourceFromName`. Ein Treffer daneben ist eine
Absage, und `input_bindable` ist das Werkzeug, aus dem ein Client die erlaubten
Namen überhaupt erst liest (~240 Tastennamen mit Filter, die Pad-Namen mit dem
Etikett, das ein Mensch darauf erkennt, die fünf Maustasten, die zehn
Achsenquellen mit `isDelta`).

Der zugehörige Test ist der einzige, der die Behauptung dieser Werkzeuge
tatsächlich belegt: die geschriebene Datei wird über einen **zweiten**
ContentManager gelesen und dann in `applyInputMappingContext` auf ein echtes
`InputMapping` gegeben. Ein Rundlauf durch unseren eigenen Dekoder beweist
nichts, weil unser Dekoder nicht der ist, der schweigt.

### 12.2 Drei Fallen, an denen ein stiller Fehler entstanden wäre

* **Der Werttyp der Action entscheidet die Form ihrer Bindungen.** Eine
  Button-Action nimmt Tasten, Pad- und Maustasten (`mapAction`), eine Axis-Action
  Achsenzeilen (`mapAxis`), eine Axis-2D-Action sie pro Komponente
  (`mapAxis2D`). Eine Taste auf einer Axis-Action landet in einer Liste, die der
  Runtime für diese Action **nie liest**: vorhanden, im Editor sichtbar, tot.
  Also wird die Action zuerst aufgelöst (aus ihrer Datei) und die falsche Form
  abgelehnt, mit der richtigen im Absagetext.

* **Ein 2D-Eintrag muss `axesX` schreiben, auch wenn seine Y-Liste noch leer
  ist.** `"axes"` registriert eine EINdimensionale Abbildung, und `axis2DValue()`
  antwortet danach für immer 0,0. `decodeMapping` lässt `MapEntry::valueType` auf
  -1, und die Rückfallregel des Encoders („2D, wenn eine Y-Liste existiert")
  greift dann. Deshalb wird der Werttyp **jedes** Eintrags neu aufgelöst, bevor
  irgendetwas kodiert wird — nicht nur der des angefassten. Sonst degradiert ein
  Speichern, das einer ganz anderen Action galt, den 2D-Nachbarn im selben
  Kontext still auf 1D. Genau dieser Fall ist ein Test: ein handgeschriebener
  Kontext mit einem `axesX`-Eintrag, dann eine Bindung auf eine andere Action,
  danach muss `axesX` noch da stehen und `wouldBind` **zwei** Gruppen melden.

* **Eine Geräte-Zeile darf keine Tasten behalten.** Die Runtime liest die Paare
  einer Zeile, was auch immer `source` sagt — Tasten auf einer Stick-Zeile binden
  also unsichtbar weiter. Der Quellen-Auswahlkasten des Panels löscht sie
  deshalb (`applyDetected`); hier ist es eine Absage, weil das stille Wegwerfen
  der halben Anfrage genau das ist, was diese Schnittstelle nicht tun darf. Und
  umgekehrt: eine `Key`-Zeile ohne jede Taste oder Pad-Taste wirft der Loader
  weg, sie würde also aus einer Datei verschwinden, in die sie als geschrieben
  gemeldet wurde.

### 12.3 Der offene Tab wird abgelehnt, nicht beschrieben — Abweichung von 11.2

Das ist die bewusste Gegenentscheidung zu den Widget-Werkzeugen, und der Grund
ist, was die beiden Panels aufbewahren. `UIEditorPanel` hält pro Tab einen
Undo-Stapel, ein MCP-Edit kann dort also als Schnappschuss landen und ist von dem
eines Menschen nicht zu unterscheiden. `InputAssetPanel::PanelState` hält
**überhaupt kein Undo** — nur `dirty`. Es gibt dort also kein sicheres Landen:
in den Tab schreiben wäre eine Änderung, die der Mensch nicht zurücknehmen kann,
und die Datei hinter dem Tab schreiben wäre eine, die sein nächstes Speichern
still zurückdreht.

Also: bei **unsauberem** Tab eine Absage mit `dirty` und dem Satz, was zu tun
ist. Bei **sauberem** Tab (oder keinem) wird die Datei geschrieben und dem Tab
gesagt, er soll sie neu lesen — derselbe `reloadFromDisk`-Weg, den die Änderung
eines Kollaborationspartners nimmt, also die Antwort, die der Editor auf „die
Datei hat sich unter einem offenen Tab geändert" schon hat. Verlieren kann man so
in keiner der beiden Richtungen etwas, und ein `input_save` gibt es
folgerichtig **nicht**: diese Werkzeuge lassen nie etwas Ungespeichertes hinter
sich.

Die Nachschlagerichtung dafür musste dazu: MCP adressiert content-relativ, die
Tab-Zustände dieses Panels hängen am absoluten Pfad der Tableiste. Statt darauf
zu vertrauen, dass zwei Schreibweisen eines absoluten Pfades als Strings
zusammenfallen, trägt `PanelState` jetzt `relPath` und die beiden neuen Zugänge
(`isDirtyByContentPath`, `reloadByContentPath`) laufen als Suche darüber —
genau die Bauform, die `UIEditorPanel::stateByContentPath` aus demselben Grund
hat.

### 12.4 Ein Leser lädt nicht, auch hier nicht

Beide Nutzlasten sind ein kleiner JSON-Chunk, also lesen die Leser die **Datei**
(`HAsset::Reader` + `CHUNK_IACT`/`CHUNK_IMAP`) statt `loadAsset` zu rufen. Das
ist dieselbe Regel wie in 8.2 und aus demselben Grund: `loadAsset` registriert
das Asset, verschiebt den dichten Asset-Pool und macht jeden Zeiger ungültig, den
der Editor in dem Moment hält. So kann `input_actions` die Actions eines ganzen
Projekts auflisten, ohne anzufassen, was resident ist — und `input_mapping_bind`
kann den Werttyp der Action nachsehen, ohne sich damit den Zeiger auf den Kontext
unter den Füßen wegzuziehen. Geladen wird ausschließlich im Schreibpfad, genau
einmal, und danach folgt kein weiteres Laden mehr.

### 12.5 Was die Werkzeuge sagen, weil es sonst niemand sagt

* **Namenskollisionen.** Ein Mapping referenziert eine Action über ihren
  **Pfad**, Events und Bindungen laufen aber über den Dateistamm
  (`inputActionNameFromPath`). Zwei `IA_Fire` in verschiedenen Ordnern sind für
  die Runtime **eine** Action. `input_actions` meldet das als `duplicateNames`.
* **Tote Namen.** `input_mappings` meldet pro Eintrag `deadNames`: die Namen, die
  der Loader überspringen wird. Bei einer handgeschriebenen oder älteren Datei
  ist das die einzige Stelle, an der „steht in der Datei" von „funktioniert im
  Spiel" unterschieden wird.
* **Was ein Umtypen kaputt gemacht hat.** `input_action_set` meldet bei
  geändertem Werttyp jeden Mapping-Kontext, der diese Action bindet
  (`affectedMappings`): deren Bindungen haben ab sofort die falsche Form.
* **Verwaiste Einträge.** Ein Eintrag kann die Action überleben, die er nennt —
  und genau der ist der, den jemand loswerden will. `input_mapping_unbind`
  verlangt darum **nicht**, dass der Action-Pfad existiert; `input_mappings`
  meldet ihn als `actionMissing`.

### 12.6 Was bewusst offen bleibt

* **Kein Anlegen.** Eine Input-Action oder ein Kontext wird mit `asset_create`
  geboren (Typ `InputAction` / `InputMappingContext`, mit gültiger
  Minimal-Nutzlast aus dem geteilten Stub-Writer) — ein zweiter Weg dorthin wäre
  eine zweite Theorie davon, was in einer frischen Datei steckt.
* **Kein Ersetzen einer einzelnen Bindung.** Ändern ist `unbind` + `bind`, zwei
  Aufrufe. Das Panel hat dafür seine Bind-Knöpfe, die eine Zeile *an ihrem Platz*
  überschreiben (`BindSlot`); als Werkzeug wäre das ein dritter Adressraum
  (Liste, Index, Feld) für eine Operation, die aus zwei bestehenden zusammengeht.
* **Keine Reihenfolge.** Bindungen werden angehängt; die Reihenfolge innerhalb
  einer Liste bedeutet für die Runtime nichts (jede gesetzte Bindung löst aus),
  im Gegensatz zur Geschwisterreihenfolge eines Widgets.
* **Kein „Taste drücken zum Binden".** Das ist der ganze Sinn des Panels und
  braucht ein Fenster, eine Tastatur und einen Menschen davor.
* **Kein Veröffentlichen in eine Kollaborationssitzung.** Wie beim Plattenweg der
  Widget-Werkzeuge: der Fremd-Lock wird geprüft und abgelehnt, aber eine
  Item-Level-Publikation eines Input-Edits gibt es in `CollabDocSync` nicht.
* **`Engine/` ist schreibgeschützt** — dieselbe Sperre wie überall
  (`failEngineReadOnly`).

## 13. Nachtrag: die Material-Werkzeuge (Folgethema 29, Schritt 6)

`material_info`, `material_set_param`, `material_create_instance` — drei
Werkzeuge für die Assetsorte, die `asset_create` zwar anlegen kann, aber leer:
ein frisches Material ist ein Stub mit META-Chunk und sonst nichts. Kein Graph,
kein Shader, keine Parameter. Ein Client konnte ein Material an ein Mesh hängen
und hatte keinen Weg zu erfahren, was daran einstellbar ist, geschweige denn es
einzustellen.

Die Aufteilung ist die von `terrain_info`: **ohne** `path` die Liste, **mit**
`path` das eine Material vollständig. Ein viertes Werkzeug namens
`material_list` wäre dieselbe Frage mit zwei Antwortformen gewesen.

### 13.1 Der eine Grund, der alles andere begründet: wo ein Wert wirklich wohnt

Es gibt zwei Antworten, und die falsche schreibt eine Änderung, die später
zurückgenommen wird, ohne dass zum Zeitpunkt des Schreibens etwas zu sehen wäre:

* Auf einem **Master** ist der **Graph** die Wahrheit. Die Werte der Param-Knoten
  sind die Defaults, die die Codegen emittiert, und
  `MaterialEditorPanel::applyToMaterial` baut `shaderParamData` bei **jedem**
  Edit komplett daraus neu, ohne etwas vom alten Block zu behalten. Ein Wert, der
  nur in den Parameterblock geschrieben wurde, hält also genau so lange, bis ein
  Mensch einen Knoten verschiebt.
* Nur den Knoten zu schreiben ist derselbe Fehler gespiegelt.
  `ContentManager::regenerateMaterialFromGraph` schnappt den Parameterblock
  **nach Namen**, bevor die Codegen läuft, und legt ihn danach wieder darüber —
  Werte werden ohne Neukompilieren editiert, der gebackene Block darf also
  legitim von den Knoten-Defaults abweichen. Der Slot-Schreibvorgang muss daher
  **vor** dem Regenerieren passieren, sonst legt das Regenerieren den alten Wert
  über den neuen Default.
* Auf einer **Instanz** gibt es überhaupt keinen Graphen. Die Wahrheit ist der
  Parameterblock **plus** `instanceOverriddenParams`: ein dort genannter Slot
  behält den Wert der Instanz, jeder andere folgt beim nächsten
  `syncMaterialInstance` dem Eltern-Material. Einen Wert zu setzen und den
  Override **nicht** zu markieren ist ein Wert, den der nächste Sync frisst.

`material_set_param` schreibt deshalb auf einem Master **beides** — den
Param-Knoten **und** den Slot, in dieser Reihenfolge — und markiert auf einer
Instanz den Override. Der Test dazu hört nicht beim Zurücklesen des Assets auf:
er lässt die Codegen laufen, die das Panel laufen lässt
(`materialGraphFromJson` → `generateFragment` → `MatParamSlot::value`), und
prüft **beide** Hälften an demselben Aufruf. Eine Implementierung, die nur eine
davon schreibt, fällt an genau einer der beiden durch.

### 13.2 Der dritte Fall, und er wird gesagt statt verschwiegen

Ein Param-Knoten kann in einer **Material-Funktion** stehen, die der Graph
aufruft. Dann existiert der Slot, aber kein Knoten **dieses** Graphen trägt den
Namen: der Wert lässt sich setzen, die Renderer benutzen ihn, und der nächste
Edit im Material-Editor setzt ihn auf den Default der Funktion zurück. Das
Ergebnis sagt das (`graphDefaultUpdated: false`, `graphNodesUpdated: 0`, plus
ein Satz, was den Wert wieder wegnimmt), statt den Client es selbst
herausfinden zu lassen. `material_info` meldet dasselbe vorab als
`inGraph: false` pro Parameter.

### 13.3 Zwei Fallen, an denen ein stiller Fehler entstanden wäre

* **Eine ParamFloat-Range wohnt in `p[1]`/`p[2]`** — also in denselben vier
  Floats, in die der Wert geht. Geschrieben werden darum nur so viele
  Komponenten, wie die Art des Parameters trägt
  (`matParamKindComponents`): vier zu schreiben hätte die Range gefressen, die
  aus dem Feld einen Schieberegler macht, und niemand hätte das mit diesem
  Aufruf in Verbindung gebracht.
* **Mehrere Knoten können einen Namen teilen.** `paramSlot` fasst gleichnamige
  Param-Knoten zu **einem** Slot zusammen, ein Graph kann also drei davon
  haben. Einen zu aktualisieren und zwei stehenzulassen heißt: das nächste
  strukturelle Edit nimmt den, den die Codegen zuerst erreicht. Aktualisiert
  werden alle Knoten des Namens — aber nur die mit der **passenden Art**, denn
  der erste gesehene bestimmt die Art des Slots, und eine Farbe in das `p[]`
  eines Float-Knotens zu schreiben ist wieder die Range-Falle.

Die Slider-Range wird außerdem **durchgesetzt**: ein Wert außerhalb wird mit
`out_of_range` abgelehnt, nicht stillschweigend geklemmt. Geklemmt wäre ein Wert,
den niemand verlangt hat, als Erfolg gemeldet — und der Schieberegler im Editor
lässt sich auch nicht aus seiner Range ziehen.

### 13.4 Die Instanz, die `asset_create` nicht anlegen kann

„Ein Master-Material, viele Varianten" ist die eine Materialform, die kein Stub
sein kann: eine Instanz hat keinen eigenen Graphen, sie teilt den kompilierten
Shader des Elternteils (gleicher Quell-Hash → **derselbe** Pipeline-Cache-Eintrag,
kein Neukompilieren) und unterscheidet sich nur in den Werten, die sie
überschreibt. `material_create_instance` geht den Weg des Content Browsers
verbatim: mit `parentMaterialPath` registrieren, `syncMaterialInstance` die ganze
Parameterschicht vom Elternteil ableiten lassen, Zeiger **danach** neu holen,
speichern. Das Ergebnis trägt gleich die geerbten Parameter, damit der nächste
Aufruf ein `material_set_param` sein kann.

Das Eltern-Material darf unter `Engine/` liegen — aus einem Engine-Default
abzuleiten ist genau der Normalfall, für den ausgelieferte Defaults da sind. Das
neue Asset darf es nicht; ohne `path` landet das Kind dann im eigenen Content des
Projekts statt neben dem Elternteil.

### 13.5 Der offene Tab wird abgelehnt — wie bei Input, aber aus anderem Grund

12.3 lehnt den offenen Input-Tab ab, weil das Panel **kein** Undo hat. Der
Material-Editor hat eines (JSON-Schnappschüsse des Graphen) — und wird trotzdem
abgelehnt. Der Grund ist ein anderer: die Wahrheit des Tabs ist sein eigener
`State::graph`, und ein Edit dort hineinzusetzen heißt `applyToMaterial` — das
Regenerieren, der Inline-Cross-Compile-Check, die Preview-Invalidierung, die
Instanz-Propagation, der Collab-Mirror —, alles hinter einem `AppContext`, den
diese Datei nicht haben kann. Ein Hook, der das freilegte, wäre ein **zweiter**
Regenerierpfad, der mit dem ersten Schritt halten muss.

Also: ein offener Tab **mit** ungespeicherten Änderungen wird mit `dirty`
abgelehnt, ein **sauberer** bekommt dasselbe `reloadFromDisk`, über das auch die
Änderung eines Kollaborationspeers hereinkommt. Ein `material_save` gibt es
nicht — diese Werkzeuge lassen nichts Ungespeichertes hinter sich.

### 13.6 Ein Leser lädt nicht — die Liste jedenfalls nicht

Die **Listenform** beantwortet sich aus dem Header-Sniff
(`EditorAssetTypeCache`), lädt also nichts: eine Frage darf ihre eigene Antwort
nicht verändern, und ein `loadAsset` pro Material hieße den ganzen
Material-Pool verschieben und zu jeder Instanz ihr Elternteil nachladen. Ein
Material, das **schon** resident ist, darf dafür mehr sagen (`kind`, `parent`,
`paramCount`) — das ist die einzige Stelle, an der Master und Instanz hier ohne
Laden unterscheidbar sind.

Die **Einzelform** lädt, wie `widget_tree`, und sagt das in ihrer Beschreibung:
die Parameterschicht eines Materials steht nirgendwo sonst.

Der Lauf über den Content-Ordner selbst (Dotfile-Regel, Sortierung,
Obergrenze, Sniff statt Laden) ist mit diesem Schritt **ein** Helfer geworden,
`walkContentAssets` in `McpToolCommon` — vorher hatten die Input-Werkzeuge ihre
eigene Kopie. Der Ordner-Lauf von `asset_list` bleibt getrennt: der listet auch
Ordner und filtert über Typ*namen*, ist also eine andere Frage.

### 13.7 Was bewusst offen bleibt

* **Der Graph selbst.** Knoten hinzufügen, verdrahten, den Output umstellen —
  das ist das Material-Editor-Pendant zu den HorizonCode-Werkzeugen und ein
  eigener Schritt. Wer einen Parameter *haben* will, den es noch nicht gibt,
  braucht einen Menschen im Panel; `no_params` sagt das so.
* **Die PBR-Skalare** (`baseColor`, `metallic`, `roughness`, `opacity`,
  `doubleSided`). `material_info` meldet sie, gesetzt werden sie nicht: auf einer
  Instanz folgen sie dem Elternteil (`syncMaterialInstance` überschreibt sie bei
  jedem Sync), und auf einem Master sind sie der Legacy-Pfad neben dem Graphen.
  Ein Setter dafür wäre ein zweiter Weg zu etwas, das der Graph ohnehin besser
  ausdrückt.
* **Statische Switches einer Instanz.** Ein Switch-Override backt eine andere
  **Permutation** des Shaders (`instanceSwitchNames`/`…Values` → eigener
  Quell-Hash, eigener Pipeline-Eintrag), das ist keine Wertänderung.
  `material_info` meldet die vorhandenen Overrides, setzen kann man sie hier
  nicht.
* **Texturslots.** Welche Textur ein Texture-Sample-Knoten liest, steht im
  Knoten, nicht im Parameterblock — also Graph-Arbeit, siehe oben.
* **Kein Master anlegen.** Das ist `asset_create` mit Typ `Material`, aus
  demselben Grund wie bei Input (12.6): ein zweiter Weg dorthin wäre eine zweite
  Theorie davon, was in einer frischen Datei steckt.
* **Kein Veröffentlichen eines Wert-Edits in eine Kollaborationssitzung.** Wie
  beim Plattenweg der Widget-Werkzeuge und bei Input: der Fremd-Lock wird geprüft
  und abgelehnt, aber eine Item-Level-Publikation gibt es in `CollabDocSync`
  dafür nicht. Das Anlegen wird publiziert.
* **Keine Thumbnail-Invalidierung.** Der Save-Knopf des Panels ruft
  `AssetThumbnailCache::invalidate`, ein `material_set_param` nicht — das Kärtchen
  im Content Browser zeigt also bis zu seinem nächsten Staleness-Poll (ein, zwei
  Sekunden) das alte Bild. Kosmetisch, und der Preis dafür, dass diese Datei die
  Editor-Caches nicht kennt; ein Hook nur dafür wäre mehr Klempnerei als Nutzen.

---

## 14. Nachtrag: die Prefab-Werkzeuge (Folgethema 29, Schritt 7a)

`prefab_info`, `prefab_instantiate`, `prefab_save`, `prefab_instances` — vier
Werkzeuge für die einzige Einheit der Wiederverwendung, die der Editor kennt.

### 14.1 Die Abweichung vom Auftrag, und warum

Der Schritt war formuliert als „`prefab` in die Component-Whitelist aufnehmen
(SceneSerializer.cpp:2078) für echte Prefab-Instanzen statt Einweg-Kopien bei
`entity_create`". Beim Nachsehen stellte sich heraus, dass es **keine
Prefab-Komponente gab**, die man hätte eintragen können: Prefabs sind an jeder
Stelle des Editors Einweg-Kopien (`SceneSerializer::instantiatePrefab` prägt
frische Identitäten auf), und die Whitelist ohne Leseseite einzutragen wäre
genau der stille Verlust gewesen, vor dem der Kommentar an dieser Stelle warnt:
`checkComponentKeys` hätte `components.prefab` bei `entity_create` **angenommen**
und `applyComponents` hätte es wortlos fallen lassen.

Also zwei Dinge statt einem, in dieser Reihenfolge:

1. Prefabs überhaupt benutzbar machen — eigene Werkzeuge, kein neues Feld.
2. Die Verknüpfung als echte Komponente: geschrieben, gelesen, in der Whitelist,
   und mit einem Verbraucher (`prefab_instances`), der sie nicht zu einem toten
   Etikett macht.

Und ein eigenes `prefab_instantiate` statt `entity_create` mit einem
Prefab-Schlüssel: `entity_create` baut **eine** Entity aus Komponenten-JSON, das
der Client selbst schreibt. Ein Prefab ist das Gegenteil — ein ganzer authorierter
Teilbaum, den jemand schon richtig hingestellt hat. Die beiden über einen Aufruf
laufen zu lassen hieße, dass `components` und `path` sich gegenseitig
widersprechen können, und die Frage „was passiert mit den Komponenten, die der
Client mitschickt" hat keine Antwort, die nicht überrascht.

### 14.2 Warum ein Prefab überhaupt Werkzeuge braucht

`asset_create` lehnt den Typ ab, und zwar zu Recht (`AssetStubWriter`: „a prefab
with no PFAB payload is an empty file, not an empty prefab"). Die Nutzlast ist
CBOR. Ein Client konnte also sehen, dass `Prefabs/Lamp.hasset` existiert, und
damit nichts anfangen, während ein Mensch dieselbe Datei ins Viewport zieht.

`prefab_save` ist entsprechend der **einzige** Weg, ein Prefab-Asset anzulegen —
anders als bei Input und Material, wo `asset_create` die Datei macht und die
Fachwerkzeuge sie füllen. Der Unterschied ist nicht Geschmack: eine leere
Prefab-Datei ist keine leere Definition, sondern kaputt.

### 14.3 Die Platzierung ist EIN Kommando, und das Wichtige reitet im Blob

`prefab_instantiate` ist ein einziges `Command::create` durch das Gateway. Die
Position, die Rotation, die Skalierung, der Name und die Verknüpfung werden
**in den Wurzel-Datensatz des Blobs gepatcht, bevor** das Kommando läuft.

Der Grund ist der Undo-Stack. Ein zweites `Command::setTransform` hinterher wäre
ein zweiter Eintrag: ein Strg+Z würde das Prefab an seinen authorierten Platz
zurückstellen und in der Szene stehen lassen, statt es herauszunehmen. Dasselbe
für die Verknüpfung — nachträglich in die Welt geschrieben stünde sie in keinem
Undo-Eintrag, und das erste Redo brächte den Teilbaum ohne sie zurück.

Gepatcht wird **nur, was der Client geschickt hat**. Die authorierte Rotation
und Skalierung sind Teil dessen, was gespeichert wurde; sie zurückzusetzen, weil
ein Aufruf sie nicht erwähnt hat, wäre stilles Ent-Authorieren. Dieselbe Regel
befolgt das Drag-Drop im Viewport, das nur die Position überschreibt.

### 14.4 Die Wurzel eines Blobs ist der Datensatz OHNE `parent`

Das ist keine Konvention dieser Datei: `buildSubtreeJson` lässt den Schlüssel für
die Wurzel absichtlich weg („naming an outside parent would make applyPrefabJson
find no root and refuse everything"), und `applyPrefabJson` liest ihn genauso.
Daraus folgen zwei Ablehnungen, die es vorher nirgends gab:

* **Kein Datensatz ohne `parent`** → der Loader verweigert alles und gibt
  `entt::null` zurück. Als `invalid_payload` abgelehnt, mit dem Grund.
* **Zwei Datensätze ohne `parent`** → der Loader macht einen zur Wurzel und lässt
  den anderen **oben in der Szene stehen**, ohne dass irgendetwas das sagt. Das
  ist der unangenehmere Fall, weil er wie ein Erfolg aussieht.

`prefab_info` meldet beide Formen vorab (`rootCount`), statt eine Datei für in
Ordnung zu erklären, die der Schreiber gleich ablehnt.

### 14.5 Nichts wird geladen — auch nicht zum Platzieren

Die Nutzlast kommt aus dem PFAB-Chunk der Datei (`HAsset::Reader`), byteweise
dasselbe, was `loadAsset` in `PrefabAsset::data` legen würde. Zwei Gründe, und
der zweite ist der tragende: eine Frage darf ihre eigene Antwort nicht ändern
(dieselbe Regel wie bei den Input- und Material-Lesern), und ein
`PrefabAsset*` aus dem ContentManager ist ein Zeiger in einen dichten Vektor,
den das nächste Laden mitsamt der Strings ungültig macht, die er besitzt.

`prefab_save` ist die eine Ausnahme, und dort in der Reihenfolge des Outliners:
**erst schreiben, dann registrieren.** `saveAsset` prägt die Identität einer
frischen Datei, `registerRuntimeAsset` prägt nur eine, wenn keine da ist — so
stimmen der Pfad→UUID-Eintrag und die Datei überein.

### 14.6 Die Verknüpfung, und was sie ausdrücklich nicht ist

`PrefabLinkComponent` hält die UUID des Prefab-Assets, auf der **Wurzel** der
Platzierung (die Kinder sind Teil der Instanz, nicht eigene Instanzen).
Schlüssel `"prefab"` im Szenenformat — geschrieben, gelesen **und** in
`isKnownComponentKey`, alle drei, denn zwei von dreien sind der stille Verlust
oder der falsche Alarm.

Sie ist **keine Prefab-Vererbung**. Ein Edit am Asset erreicht die gesetzten
Instanzen nicht, es gibt keine Override-Verfolgung, nichts wendet beim Laden
etwas erneut an. Eine Platzierung bleibt eine Kopie; sie weiß jetzt nur, woher
sie kam. Alles darüber hinaus muss beantworten, was mit einer Entity passiert,
die ein Mensch nach dem Setzen bearbeitet hat, und das ist eine Entwurfsfrage mit
mehreren vertretbaren Antworten, kein Feld.

Drei Stellen, an denen sie sonst gelogen hätte:

* **Nicht in `instantiatePrefab` geschrieben.** Die Funktion bedient auch
  Einfügen, Duplizieren und den Create eines Peers — keines davon ist eine
  Prefab-Platzierung. Der Link wird deshalb im Aufrufer gesetzt: im Werkzeug und
  im Viewport-Drag-Drop. Ohne die drei Zeilen im Viewport wären ausgerechnet die
  Prefabs, die ein **Mensch** setzt, die, die `prefab_instances` nicht sieht.
* **`prefab_save` streift den Link der Wurzel ab.** Sonst trüge jede künftige
  Platzierung des NEUEN Prefabs die UUID des alten. Ein Link auf einem **Kind**
  bleibt, dort stimmt er: das Kind ist wirklich eine Platzierung eines anderen
  Prefabs, die in diesem hier sitzt.
* **UUID statt Pfad.** Ein Pfad in einer Szenendatei bricht beim Verschieben —
  und weil `AssetRefScan` Asset-Referenzen in einer `.hescene` als `[hi, lo]`
  **innerhalb** eines `components`-Blocks sucht, findet der Löschdialog eine
  Instanz jetzt von selbst. Dafür war keine Zeile nötig, nur die richtige
  Kodierung; der Test prüft es trotzdem, weil „von selbst" beim nächsten Umbau
  aufhören kann.

### 14.7 Was bewusst offen bleibt

* **Prefab-Vererbung.** Siehe oben: Edits am Asset propagieren nicht, es gibt
  keine Overrides, kein „Revert to Prefab". Das ist der große Punkt aus der
  Lückenliste des Masterplans und mehr als ein Nachtrag.
* **Kein Ersetzen eines bestehenden Prefabs.** `prefab_save` auf einen belegten
  Pfad ist `already_exists`. Überschreiben würde jede künftige Platzierung ändern
  und die schon gesetzten unberührt lassen — nichts, was aus Versehen passieren
  sollte.
* **Kein Undo für `prefab_save`.** Es ist eine Asset-Operation, wie bei den
  Asset-Werkzeugen (8.1): der Gateway spricht Szenenänderungen, und beim Anlegen
  einer Datei ändert sich in der Welt nichts.
* **`PrefabLinkComponent` steht nicht im Inspector.** Es gibt keine Zeile im
  Details-Panel, die „aus Prefabs/Lamp.hasset" anzeigt, und keinen Knopf, der
  zur Quelle springt. Der Wert ist da und über MCP lesbar; die UI dafür ist ein
  eigener kleiner Schritt.
* **Keine Thumbnail-Invalidierung nach `prefab_save`**, aus demselben Grund wie
  bei Material (13.7).

---

## 15. Nachtrag: die Typ-Werkzeuge (Folgethema 29, Schritt 7b)

`type_info`, `type_field_set`, `type_field_remove`, `type_enum_set`,
`type_enum_remove` — fünf Werkzeuge für die eigenen Typen eines Projekts.

### 15.1 Der Grund: `asset_create` legt eine leere Definition an

Ein Struct- oder Enum-Asset ist eine Definition, die danach **jedes** andere
Frontend spricht: HorizonCode-Pins und -Variablen, Lua-Tabellen und
Python-Dicts, der generierte C++-Header, Savegame-Felder. Die Datei anzulegen
konnte `asset_create` schon; was sie anlegt, ist eine Definition ohne Felder und
ohne Einträge, auf der sich nichts bauen lässt.

Die Nutzlast selbst hinzulegen wäre der Base64-Fehler mit hübscheren Zeichen:
`type` ist ein Integer-Enum, ein Container-Feld trägt **vier** gekoppelte Felder
(`isArray`, `container`, `keyType`, `keyTypeName`), und die Kodierung des
Vorgabewerts hängt am Typ des Feldes. Gesprochen wird deshalb die Sprache des
Type Editors: ein Feldname, ein Typ an seiner Beschriftung („Float", „Vec3",
„Enum"), ein Container an seinem Namen, ein Vorgabewert in der Form, die dieser
Typ hat.

### 15.2 Die vier gekoppelten Felder werden als eines geschrieben

`isArray` ist „ist es überhaupt ein Container", `container` sagt **welcher**, und
der Loader **repariert eine unzulässige Kombination stillschweigend**
(`structFromJson`: „a kind present means container, full stop"). Wer also zwei
der vier setzt und das dritte vergisst, bekommt ein Feld, das nicht das
bestellte ist, und keinen Fehler dazu. Ein `container`-Argument setzt alle vier.

Eine Zeile davon ist nicht offensichtlich: ein **Array** wird als
`isArray = true, container = None` geschrieben, nicht als `container = Array`.
Das ist die Legacy-Zeile, die `containerKindOf` als Array auflöst, und es ist
genau das, was `structToJson` schreibt („so an array field keeps the exact bytes
it had before containers existed").

### 15.3 Ein Speichern ist mehr als die Datei

`TypeAssetPanel::saveState` macht drei Dinge, und zwei davon auszulassen wäre
eine stille Abweichung vom Panel:

* Die Definition wird in `HE::TypeRegistry` **neu registriert** — daraus lesen
  jedes Typ-Dropdown im Editor, der Skript-Bootstrap, der Savegame-Seeder und
  die C++-Codegen. Nur die Datei zu schreiben heißt: der Editor zeigt weiter die
  Felder von gestern, und nichts auf dem Bildschirm legt nahe, warum.
* In einem **C++-Projekt** wird `Source/Generated/GameTypes.h` neu geschrieben.
  Sonst kompiliert Gameplay-Code gegen ein Struct, das das Asset nicht mehr ist,
  und der Fehler taucht später auf, ohne dass ihn jemand mit einem MCP-Aufruf in
  Verbindung bringt. Das ist der Hook `onTypesChanged`.
* Eine **Savegame-Vorlage** wird ausdrücklich **nicht** registriert. Auf der
  Platte ist sie ein `StructDef` (deshalb nehmen die Feld-Werkzeuge sie mit),
  aber ein Typ ist sie nicht, und sie zu registrieren hieße, sie in jedes
  Typ-Dropdown des Editors zu legen.

Die tragenden Tests fragen deshalb die **Registry**, nicht nur die Datei — und
einer fragt `makeDefaultValue`, also die Antwort, mit der eine Instanz dieses
Typs tatsächlich anfängt.

### 15.4 Die zwei Wachen des Panels, unterschiedlich streng

* **Ein Zyklus wird abgelehnt.** `structWouldCycle` ist, was der Save-Knopf des
  Panels prüft, bevor er schreibt: ein Struct, das sich (direkt oder über ein
  anderes) selbst enthält, kommt beim Seeden eines Defaults nie zum Ende.
* **Eine Namenskollision wird gemeldet, nicht abgelehnt** (`nameCollision: true`).
  Das Panel warnt und speichert ebenfalls, und das ist richtig: eine Datei muss
  benennbar sein, bevor jemand sie umbenennen kann.

Dazu eine, die es im Panel nur als Dropdown gibt und hier ausgesprochen werden
muss: ein Enum- oder Struct-Feld muss eine Definition nennen, **die es gibt**.
Ein Feld, das ins Leere zeigt, seedet jede Instanz mit einem leeren Wert, und
nirgendwo steht, warum.

### 15.5 Was bewusst offen bleibt

* **Kein Anlegen und kein Umbenennen.** Eine neue Definition ist `asset_create`
  mit Typ `StructType`/`EnumType`/`SaveGameTemplate`, ein neuer Name ist
  `asset_move` — aus demselben Grund wie bei Input (12.6) und Material (13.7).
* **Ein Feld umzubenennen ist Anlegen + Entfernen**, und das ist ehrlich so: der
  Rename-Retarget für Struct-Felder fehlt in der ganzen Engine (Savegame-Lücke,
  siehe das Masterplan-Logbuch). Weder das Panel noch diese Werkzeuge ziehen
  HorizonCode-Graphen, Savegame-Vorlagen oder Skripte nach, die das alte Feld
  nennen. Ein `type_field_rename` würde genau das versprechen.
* **Keine authorierten Start-Elemente für Container.** Ein Array-, Set- oder
  Map-Feld kann Elemente mitbringen (`defaultValue.items`/`keys`), gesetzt werden
  sie hier nicht — `type_info` meldet nur ihre Anzahl. Das ist eine eigene
  Vokabel („ein Wert je Element, in der Form des Elementtyps"), und das Panel
  bietet sie ebenfalls nur inline.
* **Kein Veröffentlichen in eine Kollaborationssitzung**, wie bei Input und
  Material: der Fremd-Lock wird geprüft und abgelehnt, publiziert wird nichts.

---

## 16. Befund: Animation und Partikel (Folgethema 29, Schritt 7c/7d — NICHT gebaut)

Der Schritt hatte vier Teile in fester Reihenfolge; (a) Prefabs und (b) Typen
sind gebaut (Abschnitte 14 und 15), (c) Animation und (d) Partikel nicht — die
Bitte aufzuhören kam vorher. Damit der nächste Durchgang nicht wieder bei null
kartiert, hier der Stand der Vorarbeit, ehrlich als das, was er ist: eine
Kartierung, kein Entwurf.

### 16.1 Was schon steht (nachgesehen, nicht vermutet)

| Asset | Chunk | Panel | Was das Panel anbietet |
|---|---|---|---|
| AnimatorStateMachine | `CHUNK_ASMG` (JSON) | `AnimatorStateMachineEditorPanel` | `isDirty`, `reloadFromDisk`, `save(ctx, assetPath)` |
| ParticleSystem | `CHUNK_PTGR` (JSON) | `ParticleGraphEditorPanel` | `isDirty`, `reloadFromDisk`, `save(ctx, assetPath)` |
| BlendSpace | `CHUNK_BLSP` (JSON) | `BlendSpacePanel` | `isDirty`, `reloadFromDisk`, `save(ctx, path)` |

Alle drei haben also schon die Paarung, auf der die Input-, Material- und
Typ-Werkzeuge stehen: ein Tab mit ungespeicherten Änderungen wird abgelehnt, ein
sauberer liest die Datei neu. Was **fehlt**, ist bei allen dreien dasselbe wie
beim Type-Panel vor Schritt 7b: die Adressierung ist die **absolute** Pfad des
Tab-Bars, MCP adressiert content-relativ. Das sind pro Panel die ~20 Zeilen
`stateByContentPath` + `isDirtyByContentPath`/`reloadByContentPath`, die
`InputAssetPanel` und (seit 7b) `TypeAssetPanel` schon haben.

### 16.2 Die Frage, die vor dem Entwurf zu beantworten ist

Für Material war es „wo wohnt ein Wert wirklich", für Input „der Loader
schweigt", für Typen „die Registry ist die Wahrheit, nicht die Datei". Für
Animation und Partikel ist sie noch nicht beantwortet, und ohne sie wären die
Werkzeuge eine JSON-Umschreibhilfe:

* **AnimatorStateMachine** ist ein Graph mit Zuständen, Übergängen **und
  Parametern**, und ein Übergang trägt Bedingungen, die diese Parameter beim
  NAMEN nennen. Die erste Frage ist deshalb, was mit den Bedingungen passiert,
  wenn ein Parameter verschwindet oder seinen Typ wechselt — dieselbe Klasse von
  Problem wie der fehlende Rename-Retarget bei Struct-Feldern (15.5).
* **Partikel-Graphen** sind ein Knotengraph wie der Material-Graph. Damit stellt
  sich sofort dieselbe Frage, die für Material bewusst **offen gelassen** wurde
  (13.7, „der Graph selbst"): Knoten hinzufügen und verdrahten ist ein eigener
  Schritt, kein Nachtrag. Ein realistisches Minimum wäre hier das, was
  `material_set_param` ist: die **Werte** vorhandener Knoten lesen und setzen,
  nicht die Struktur.
* **AnimationClip** ist gar kein authoriertes Asset, sondern importiert
  (`AssetStubWriter` lehnt den Typ ab, wie Mesh und Textur). „Minimal editierbar"
  kann dort also nicht heißen, was es bei den anderen beiden heißt — allenfalls
  Notifies und Marker, die in einem eigenen Chunk liegen.

### 16.3 Empfehlung für den nächsten Durchgang

Nicht als ein Schritt. `particle_*` (Werte vorhandener Knoten, Vorbild
`material_set_param`) und `animator_*` (Zustände, Übergänge, Parameter, mit einer
Antwort auf 16.2) sind zwei Schritte mit zwei verschiedenen Fallen, und
AnimationClip ist ein dritter, der wahrscheinlich mit „gar nicht, es ist ein
Import" endet. Wer zuerst die drei `ByContentPath`-Paare nachrüstet, hat für
beide danach dieselbe Grundlage wie Input, Material und Typen.

---

## 17. Animation und Partikel, gebaut (Schritt 7c/7d)

Der Befund aus Abschnitt 16 ist abgearbeitet. Achtzehn Werkzeuge, registriert in
drei Aufrufen (Partikel, Animator samt Blend Space, Clip), und die Empfehlung aus
16.3 hat sich in einem Punkt als falsch erwiesen — dazu 17.4.

Die Vorbedingung zuerst: die drei `ByContentPath`-Paare, die 16.3 verlangt, sind
nachgerüstet (`AnimatorStateMachineEditorPanel`, `ParticleGraphEditorPanel`,
`BlendSpacePanel`). Ohne sie kann kein Werkzeug fragen, ob ein Tab
ungespeicherte Änderungen hat: die Werkzeuge adressieren content-relativ, die
Tab-Zustände hängen am absoluten Pfad.

| Familie | Werkzeuge | Adresse eines Wertes |
|---|---|---|
| Partikel | `particle_info`, `particle_set`, `particle_slot_set` | der **Pin-Name** des Emitter Output |
| Animator | `animator_info`, `animator_state_set`/`_remove`, `animator_transition_set`/`_remove`, `animator_param_set`/`_remove` | Zustand und Parameter beim **Namen**, ein Übergang über das **Tripel** |
| Blend Space | `blendspace_info`, `blendspace_set`, `blendspace_sample_set`/`_remove` | ein Sample über seinen **Index** |
| Animation Clip | `clip_info`, `clip_notify_set`/`clip_notify_remove`, `clip_root_motion_set` | ein Notify über seinen **Index** |

### 17.1 Partikel: der Pin ist die Adresse, nicht der Knoten

Ein Partikel-Asset ist ein Knotengraph, und jeder authorierte Wert steckt in
einem Const-Knoten, der an einem der siebzehn Eingänge des Emitter Output hängt.
Einem Client dieses JSON zum Umschreiben zu geben, wäre derselbe Fehler wie
Base64: der Pin-**Index** ist On-Disk-Format (`ParticleGraph.h`: Pins dürfen nur
angehängt werden), eine Verbindung sind vier nackte Zahlen, und `type` ist ein
Anzeigename statt des Enums.

16.3 hatte „die Werte vorhandener Knoten" vorgeschlagen, Vorbild
`material_set_param`. Wörtlich genommen ist das ein Werkzeug, das auf einem
frischen Asset **nichts** tun kann: `ParticleGraph::makeDefault` ist genau ein
Knoten, der Emitter Output, mit unverbundenen Pins. Es gibt keinen Wert, den man
setzen könnte. Also ist der Pin die Adresse, buchstabiert wie die Slot-Liste des
Panels ihn buchstabiert („Emit Rate", „Start Color"), und gesetzt wird die
kleinste Änderung, die diesen Wert wahr macht:

* Pin unverbunden → ein Const-Knoten wird angelegt, gesetzt und verdrahtet (bei
  den beiden Farb-Pins ein Const Color, weil das Panel dort das hinlegt),
* Pin hängt an einem Const-Knoten, der **nur** diesen Pin speist → der Wert wird
  an Ort und Stelle geändert,
* Pin hängt an irgendetwas anderem — Random Range, Add, Lerp, oder ein Const,
  den sich ein zweiter Pin teilt → **abgelehnt**, unter Nennung des Knotens.

Der letzte Punkt ist die Grenze, die 16.3 gezogen hat, und sie ist die ehrliche:
einen Graphen umzuverdrahten ist ein anderes Werkzeug als einen Wert zu setzen,
und einem Autor still seinen Mathe-Knoten abzuhängen, um eine Konstante
hineinzuzwingen, wäre eine Änderung, die niemand bestellt hat.

Jede Antwort trägt zusätzlich die **ausgewertete** Emitter-Konfiguration, also
dasselbe POD, das die Simulation bekommt — was der Client wollte, war ein
Partikelverhalten, keine Graphänderung. Steckt eine Random Range darin, sagt die
Antwort dazu, dass sie einmal pro Auswertung würfelt und das laufende Spiel
eigenständig würfelt.

### 17.2 Animator: was mit einem Übergang passiert, wenn sein Parameter geht

Jede Familie musste eine Frage zuerst beantworten (16.2). Für die
Zustandsmaschine ist es diese, und die Antwort steht in
`AnimationStateMachineSystem::evalTransition`: ein Übergang, dessen Parameter
nicht in der Live-Map steht, liefert `false` — jeden Frame, für immer. Kein
Fehler, keine Logzeile, ein Übergang, der still nie wieder feuert. Daraus folgt:

* **`animator_param_remove` lehnt ab**, solange ein Übergang den Parameter nennt,
  und zählt diese Übergänge auf. `force` nimmt ihn trotzdem heraus, denn ein
  Sync-Graph oder ein Skript darf den Parameter zur Laufzeit schreiben, ohne dass
  er je als Default deklariert war — die Map ist offen. Abgelehnt wird, es aus
  Versehen zu tun.
* **`animator_state_set` kann umbenennen** und macht die Nacharbeit, die das
  Panel von Hand macht: jeder Übergangs-Endpunkt und `startState`, der den alten
  Namen nannte, wird mitgezogen. Ohne das blieben baumelnde Endpunkte zurück, die
  das System genauso still überspringt wie den fehlenden Parameter.
* **`animator_state_remove` kaskadiert**: die Übergänge, die den Zustand
  berühren, fallen weg, und `startState` wird geleert, wenn er dorthin zeigte.

Ein Übergang hat **keine Id** (`AnimatorStateMachineGraph.h` sagt das und sagt
auch, warum es noch nicht behoben ist). Die Kollaboration schlüsselt einen, indem
sie from/to/param hasht — also adressieren diese Werkzeuge ihn genauso, über das
Tripel. Ein passendes Tripel wird an Ort und Stelle geändert, alles andere
angehängt; erst das macht zwei Übergänge zwischen denselben zwei Zuständen auf
**verschiedenen** Parametern überhaupt adressierbar.

### 17.3 Der Blend Space gehört in dieselbe Familie

Weil ein Zustand statt auf einen Clip auf einen Blend Space zeigen kann und
`blendSpaceId` dabei **gewinnt**. Die Maschine authorieren zu können, ohne den
Raum authorieren zu können, in den sie blendet, wäre ein halbes Werkzeug. Samples
haben keine Id und zwei dürfen denselben Clip nennen, also ist der **Index** die
einzige ehrliche Adresse; `blendspace_info` meldet ihn mit.

Die Live-Invalidierung (`markConfigDirty`) macht nur die Zustandsmaschine, nicht
der Blend Space — weil `BlendSpacePanel::save` sie auch nicht macht. Ein
Werkzeug, das mehr ungültig macht als das Panel, wäre ein zweiter, leise anderer
Speicherpfad.

### 17.4 Animation Clip: 16.2 lag falsch, und das war prüfbar

16.2 vermutete, ein AnimationClip ende bei „gar nicht editierbar, es ist ein
Import" — `AssetStubWriter` lehnt ihn ab wie ein Mesh oder eine Textur, und
anlegen kann ihn niemand. Der Beleg dagegen ist ein Panel: der **Skeletal Mesh
Editor** authoriert die Notify-Timeline eines Clips und seinen
Root-Motion-Schalter, speichert beides mit `ContentManager::saveAsset`, und beide
liegen in einem eigenen Chunk (`CHUNK_ANOT`). Die authorierte Hälfte ist also
real, sie ist klein, und sie ist von außen genau so editierbar wie im Tab.

Die **Keyframes** bleiben draußen (`CHUNK_ANIM`). Die sind der Import: ein
Werkzeug, das einen Kanal schriebe, würde Animation neu authorieren, die ein
DCC-Werkzeug besitzt, und der nächste Re-Import würfe es kommentarlos weg.

Drei Dinge sind an dieser Familie anders als an den anderen:

* **Gelesen wird aus der Datei, geschrieben durch den geladenen Clip.** Die
  Leseseite holt aus `CHUNK_ANIM` nur die ersten zwei Felder und dazu
  `CHUNK_ANOT` — eine Frage nach einem Clip lädt also nie seine Keyframes, und
  ein Walk-Cycle mit 300 Kanälen ist megabyteweise Samples, die niemand bestellt
  hat. Die Schreibseite hat diese Wahl nicht: die Notify-Liste lebt im
  Clip-Asset, `saveAsset` schreibt die ganze Datei daraus, und das ist der Weg,
  den das Panel selbst nimmt. Genau deshalb prüfen die tragenden Tests nach jeder
  Änderung, dass die **Kanäle noch da sind**.
* **Kein Reload-Hook.** Der geladene Clip **ist** der Editierpuffer des Tabs
  (`getAnimationClipMutable`), und die Notify-Spur liest diese Liste jeden Frame.
  Auch die Simulation liest sie direkt (`AnimationNotify.cpp`), es gibt also
  nichts Aufgelöstes zu invalidieren.
* **Die Tab-Frage geht über den CLIP-Pfad.** `SkeletalMeshEditorPanel` schlüsselt
  seine ungespeicherten Clip-Änderungen nach dem Clip, nicht nach dem Tab (der
  zeigt ein Mesh), und `ContentAsset::path` ist content-relativ. Diese Familie
  braucht deshalb als einzige kein `ByContentPath`-Paar.

Eine Zeitangabe außerhalb des Clips wird **abgelehnt, nicht geklemmt**: ein
Notify hinter dem Ende feuert nie (die Feuerregel in `AnimationNotify.h`), und
ein Werkzeug, das ihn still nach innen schöbe, erfände einen Zeitpunkt, während
eines, das ihn draußen ließe, ein totes Ereignis schriebe und Erfolg meldete. Die
Ablehnung nennt die Länge des Clips, also die Zahl, die dem Client fehlte.

### 17.5 Was bewusst offen bleibt

* **Der Partikel-Graph selbst.** Knoten anlegen und verdrahten ist ein eigener
  Schritt, genau wie beim Material-Graphen (13.7). Was hier geht, sind Werte.
* **Kein Anlegen und kein Umbenennen von Assets**, wie bei Input (12.6), Material
  (13.7) und Typen (15.5): `asset_create` und `asset_move`. Für einen Clip gilt
  auch das nicht — er entsteht beim Import eines Skeletal Mesh.
* **`PropertyAnimClip`** ist ein anderes Asset mit einem anderen Chunk und ist
  nicht abgedeckt.
* **Keine Bone Masks.** `BoneMaskPanel` hat dieselbe Paarung wie die drei anderen
  Panels, und eine Maske ist das, was ein Layer einer Zustandsmaschine
  einschränkt — die nächstliegende vierte Familie in dieser Ecke.
* **Kein Veröffentlichen in eine Kollaborationssitzung**: der Fremd-Lock wird
  geprüft und abgelehnt, publiziert wird nichts.

---

## 18. Bauen und Einstellen (Folgethema 29, Schritt 12)

Fünf Werkzeuge, und sie schließen die zwei Lücken, die das Lückenaudit des
Themas unter P2 als Punkte 6 bis 9 geführt hat: **`project_package`** (Build ▸
Export Project), **`project_build`** (Build ▸ Build and Reload Game Logic),
**`project_build_status`** (was das Build-Fenster zeigt) sowie **`settings_get`**
und **`settings_set`** über die beiden Bereiche `project` und `editor`.

Vorher endete jedes der 79 anderen Werkzeuge bei einer Datei im Projekt. Keines
produzierte etwas, das ein Mensch starten kann, und ob ein HorizonCode-Graph
überhaupt übersetzbar ist, erfährt man zum ersten Mal beim Export — der außer
Reichweite lag. Mit diesen fünf sind es 84 (nachgezählt statt geschätzt: die
eindeutigen `t.name`-Literale über alle `McpTools*.cpp` plus
`McpToolRegistry.cpp`; die aus `HE::api::registry()` erzeugten Zeilen sind wie
in der Zählung von `mcp-integration-im-15` nicht mitgezählt, nur `api_list`).

### 18.1 Beides ist asynchron, und darum hängt der ganze Entwurf daran

Der Export läuft auf einem Worker (`ExportDialogPanel`), der Game-Logic-Compile
auf einem zweiten (`GameLogicBuildPanel`), und beide berichten in **ein**
Fenster (`BuildProgressDialog`), dessen Modell die einzige Aufzeichnung dessen
ist, was passiert ist.

* `project_package` und `project_build` antworten, sobald der Lauf **gestartet**
  ist — `started: true` und sonst nichts über Erfolg, weil in diesem Moment
  nichts über Erfolg bekannt ist. Ein Werkzeug, das hier blockierte, hielte die
  Frame-Schleife an: die Handler laufen auf dem UI-Thread, zwischen der
  Kollaborations-Pumpe und dem Rendern, und die Reload-Hälfte eines
  Game-Logic-Builds passiert absichtlich erst im **nächsten** Frame.
* `project_build_status` ist die Stelle, an der die Antwort ankommt. Pollen ist
  die vorgesehene Nutzung.

### 18.2 Das Fenster musste erst lesbar werden

`BuildProgressDialog` hatte von außen `running()`, `runKind()`, `isOpen()` und
`interpretedClasses()` — Schritte, Logtext und Ergebnis hatten **keinen**
Accessor. Ein Werkzeug hätte nur „läuft noch / läuft nicht mehr" sagen können,
nicht ob es geklappt hat. Neu ist deshalb `BuildProgressDialog::snapshot()`: eine
**Kopie** unter dem Mutex des Modells, weil der Worker aus einem anderen Thread
hineinschreibt. Das Log kommt ganz heraus und wird erst im Werkzeug beschnitten —
der Aufrufer ist der, der weiß, ob er die Fehlerzeile oder das Protokoll will.

`project_build_status` gibt darum den **Schwanz** des Logs zurück (Standard 100
Zeilen, hart bei 2000), filterbar nach Schritt und nach Schweregrad. `minSeverity: 2`
ist die Zeile des Compilers ohne die tausend, die funktioniert haben.

### 18.3 Ein Override gilt für DIESEN Lauf und nie für das Profil

`ExportDialogPanel::startFromProfile` nimmt das Profil **als Wert**.
`project_package` kopiert das gespeicherte `ExportProfile`, flickt die Kopie und
reicht die Kopie weiter; nichts wird zurückgeschrieben und `saveProject` läuft
nicht. Ein einmaliges „bau mir eine Linux-Kopie" darf nicht zu dem werden, was
das nächste Export Project des Menschen tut — und das Zurückschreiben ist etwas,
das der Dialog auf seinem Save-Knopf macht, mit einem Menschen davor.

Geprüft wird dabei die Zielplattform gegen die **Namen des Enums selbst**, nicht
über `exportPlatformFromName`: das bildet alles Unbekannte auf `Host` ab, ein
vertipptes „linx" wäre also ein vollständiger Host-Export in einen Ordner, der
nach einer anderen Plattform heißt, und niemand hätte es gesagt bekommen.

### 18.4 Ein Fenster, ein Lauf

`Kind` existiert genau dafür, dass ein Export und ein Game-Logic-Build einander
nicht die Knöpfe wegnehmen. Keines der beiden Werkzeuge startet etwas, solange
die andere Art das Fenster hält: beide lehnen mit `busy` ab und **nennen die
Art**, die hält — ein „busy" ohne das lässt einen Client am falschen Werkzeug
pollen.

### 18.5 Warum die Einstellungen EIN Paar Werkzeuge sind

Weil der Editor sie so zeigt. Es gibt keine Projekt-Einstellungs-Oberfläche, also
hängen die Projektseiten in den Preferences neben den Editor-Seiten
(`EditorSettingsPanel::Page`, mit genau dieser Begründung im Header). Ein Client,
der wissen müsste, zu welchem von zwei Werkzeugen eine Einstellung gehört, müsste
etwas wissen, das der Editor selbst nicht sichtbar macht. Der Bereich wird aus
dem Schlüssel abgeleitet.

### 18.6 Die Editor-Seite brauchte erst einen Katalog

Die Preferences schreiben ihre Einstellungen als `row("bloom", "Post-Processing",
widget)` in `EditorSettingsPanel.cpp`, und das ist Dokumentation, die nur ein
Mensch lesen kann, der auf ein Bedienelement schaut. Die zwei ehrlichen
Alternativen waren beide schlechter: einem Modell einen freien Config-Schlüssel
geben (nichts sagte ihm, dass `AntiAliasing` 0..4 nimmt, eine 7 würde
geschrieben und irgendwo stillschweigend geklemmt) oder es ein Panel parsen
lassen, das sich ohne Fenster nicht übersetzen lässt.

Also `EditorSettingsCatalog.h/.cpp`: dieselbe Liste als **Daten**, mit den
Bereichen, die die Widgets ohnehin erzwingen, und den Kategorienamen der Panels
wörtlich. Dafür sind `EditorConfig` und `EditorMode` aus `EditorApplication.h` in
ein eigenes `EditorConfig.h` gezogen worden — dieselbe Datei zieht ImGui, SDL,
den Renderer und die Physik herein, und das hielt die Einstellungen des Editors
aus allem heraus, was nicht der Editor ist, also auch aus dem Testbinary.

Drei Sorten Zeile, und nur die erste ist ein schlichtes Feld:

* ein Feld von `EditorConfig` (über Member-Zeiger gelesen und geschrieben),
* ein Feld, das **zusätzlich** irgendwohin reisen muss (`apply`): `MaxFps`
  erreicht die Frame-Taktung über `AppContext::setMaxFps`, und ein Schreiben,
  das nur die Struktur setzt, wäre eine Zahl in einer Datei, die bis zum
  nächsten Start nichts ändert;
* gar kein Feld von `EditorConfig` (`SettingStorage::External`): VSync lebt auf
  der Application, der Backend-Name auf dem AppContext. Die trägt der Aufrufer
  über eigene Hooks.

### 18.7 Persistenz wird gemeldet, nie angenommen

Ein Projekt-Schreiben ruft `ProjectManager::saveProject`, das Ergebnis sagt
`persisted`. Schlägt das fehl, ist die Antwort `write_failed` — mit dem Satz,
dass der Wert im Speicher steht: so zu tun, als sei nichts passiert, macht das
nächste Lesen unerklärlich.

Beim Editor lag die Sache anders: `config.json` wurde **ausschließlich** in
`OnShutdown` geschrieben, in einem Block von 52 Zeilen, der als einziger die
Schlüssel zu den Feldern kannte. Der ist jetzt
`EditorApplication::writeEditorConfig()` — dieselbe Stelle, von OnShutdown und
vom Werkzeug gerufen. Eine zweite Kopie dieser Zuordnung wäre die, die an dem Tag
aufhört zu passen, an dem jemand ein Feld hinzufügt. Ohne diesen Hook lautet die
ehrliche Antwort `persisted: false` mit dem Grund, statt einer Behauptung, die
erst nach einem Absturz auffliegt.

### 18.8 Drei Ablehnungen, die vor der Benutzung zu lesen sind

* **Die Kategorie `Remote Control` wird nie geschrieben.** Das ist die Seite, auf
  der diese Brücke eingeschaltet und ihr Port gewählt wird. Ein Werkzeug, das
  seinen eigenen Listener abschalten kann, ist im besten Fall nutzlos und im
  schlechtesten unerklärlich. Lesbar bleibt sie, damit ein Client den Zustand
  sieht, in dem er lebt. (Der Vorschlag stammt aus dem Audit-Nachtrag von
  `mcp-integration-im-15`.)
* **`name`, `path`, `id`, `scriptLanguage` und `appProject`** sind lesbar und
  nicht schreibbar. Die fünf sagen, was das Projekt IST — die Skriptsprache
  entscheidet, was überhaupt angelegt werden darf, das App-Flag, ob es eine Welt
  gibt —, und das unter einem Projekt voller Assets zu ändern ist keine
  Einstellung, sondern eine Umwandlung, die niemand geschrieben hat.
* **`project.startupScene` ist lesbar und nicht schreibbar**, und das ist kein
  Prinzip, sondern ein Befund: `ProjectManager::saveProject` ist ein
  Read-Modify-Write, der den Schlüssel des Manifests bewahrt und dieses Feld
  **nie** zurückschreibt. Ein Setter hätte die Struktur bewegt, `persisted: true`
  gemeldet (die Datei WURDE ja geschrieben) und den Wert beim nächsten Laden
  verloren — das schlechteste der drei möglichen Verhalten. Der Editor hat dafür
  auch keine Oberfläche. Was ein BUILD startet, ist das `startupScene` des
  Export-Profils, und das nimmt `project_package` als Argument.

**Keine Play-Mode-Sperre**, absichtlich. Ein Mensch kann die Preferences auch
während einer laufenden Vorschau öffnen, und eine Schranke, die MCP hat und die
Oberfläche nicht, wäre eine Verhaltensänderung im Gewand von Verkabelung. Wo ein
Wert eine LAUFENDE Simulation nur über einen Callback erreicht — die
Kollisionsmatrix —, wird der Callback gerufen.

### 18.9 Die Kollisionsmatrix passt in kein Schlüssel-Wert-Paar

Sechzehn Namen und ein 16×16-Dreieck. Statt einer Zeile je Zelle gibt es zwei
Schlüssel-**Formen**:

```
project.collisionLayers.name.<i>
project.collisionLayers.collides.<a>.<b>
```

`settings_get` liest beide Hälften ganz aus, damit ein Client nie raten muss,
welche Indizes es gibt: die 16 Namen mit ihren Schlüsseln und die Paare, die
**nicht** kollidieren (alles kollidiert, solange es nicht in `blockedPairs`
steht). Geschrieben wird über `setCollides`, das **beide** Zellen setzt — Jolt
verspricht nicht, in welcher Reihenfolge es fragt, und eine halb gefüllte Matrix
kollidiert in manchen Frames und in anderen nicht.

### 18.10 Was bewusst offen bleibt

* **Kein `project_open` / `project_create`.** Das steht im Audit als eigener
  Punkt (B) und hat eine eigene Falle — `dialog-static-leaks-across-projects` —,
  die vor dem Bauen untersucht gehört.
* **Keine Export-Profile anlegen, umbenennen oder löschen.** `settings_get` listet
  sie, `project_package` läuft eines, `project.activeExportProfile` wählt eines
  aus. Ein Profil zu erzeugen ist die Arbeit des Dialogs.
* **Kein Starten des fertigen Builds.** `project_build_status` nennt die
  Executable und ob diese Maschine sie ausführen könnte; ein Spiel zu starten ist
  etwas anderes als es zu bauen.
* **Kein `hc_check`.** Punkt 10 des Audits, und der teuerste: der `ClassSource`-
  Sammler steht inline im Export-Worker und müsste erst herausgelöst werden.
  Solange das so ist, ist `project_package` mit `compileHorizonCode: true` der
  einzige Weg, einen nicht übersetzbaren Graphen zu erfahren — und
  `project_build_status` meldet ihn unter `interpreted`.
* **`documentTypes`, `appIconName`-Auswahl und die Font-Maske jenseits von Griechisch
  und Kyrillisch** sind nicht abgedeckt.
