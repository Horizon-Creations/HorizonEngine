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
