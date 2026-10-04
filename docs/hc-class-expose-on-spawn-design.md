# Expose on Spawn für HorizonCode-Klassen (Thema 138)

Stand: Schritt 1, Bestand und Design. Kein Code in diesem Schritt.

Vorbild ist Expose on Spawn für Widgets, `docs/widget-pre-construct-design.md` §6 (Thema 119, Schritt 5; Commits `515c5786`, `bba6d2cb`, `4fd668c8`). Dieses Dokument übernimmt dessen Regeln und beschreibt nur, was bei Klassen anders ist oder dazukommt. Wo nichts steht, gilt §6 unverändert.

## 1. Bestand

### 1.1 Was es für Widgets gibt

| Teil | Stelle | Wiederverwendbar für Klassen |
|---|---|---|
| Flag `Variable::exposeOnSpawn`, JSON `"spawn": true`, nur öffentliche Instanzvariablen (Loader verwirft es sonst) | `HorizonCode.h` (Variable), `HorizonCode.cpp` `variableToJsonObj`/`variableFromJsonObj` | ja, unverändert |
| `SpawnValue`/`SpawnValues` | `HorizonCode.h` | ja |
| `SpawnPin`, `spawnPinsOf(graph)` | `HorizonCode.h/.cpp` | Grundlage; für Klassen braucht es die Vererbungskette (§3.2) |
| `syncSpawnPins(g, nodeId, now, before)`: Leitungen und Inline-Werte folgen dem Pin über den Namen | `HorizonCode.cpp` | ja, mit Versatz für die festen Eingänge (§3.3) |
| Pins in `Node::params`, generisch gespeichert (`"params"` in `nodeToJsonObj`) | `HorizonCode.cpp` | ja |
| Interpreter: Pin wirkt, wenn verdrahtet oder mit Wert am Knoten (`pinDefaults`) | `Runner::execNode`, `CreateWidget` | ja, mit Versatz |
| `Runtime::setPublicVariable(id, name, value)`: Set-(Ref)-Regel, interpretiert und kompiliert | `HorizonCodeRuntime.cpp` | ja, unverändert |
| Werte setzen nach `registerInstance`, vor PreConstruct, unbekannte Namen mit Warnung überspringen | `WidgetManager::createWidget` | Muster; Klassen setzen an anderer Stelle (§4) |
| Editor-Spiegel `syncCreateWidgetPins` mit Cache (Hash des Graph-JSON, höchstens 2×/s), `onEdit(false)` in `buildModel`, `force` beim Asset-Wechsel | `HcGraphHost.cpp` | Muster; Hash muss die Kette abdecken (§5.2) |
| Häkchen in den Variablen-Details | `UIEditorPanel.cpp` (nur Widget-Editor) | nein, Klassen haben ein eigenes Panel (§5.1) |
| Mitumbenennen der Pins | `HcRename.cpp` `planGraph`/`apply`, Zweig `CreateWidget` | ja, um `CreateObject` erweitert (§5.4) |
| Codegen: ohne wirkende Pins die alte Zeile byte-gleich, sonst `hc::SpawnValues` + Überladung | `HcCodegen.cpp`, `HorizonCodeGenSupport.h` | ja (§6) |
| Tests | `tests/test_widget_expose_on_spawn.cpp` | Vorlage (§8) |

### 1.2 Welche HC-Klassentypen es gibt

Die Taxonomie in `engineClasses()` (`HorizonCode.cpp`) kennt `Object`, `Entity`, `PlayerCharacter` und `PlayerController`. Widgets laufen getrennt über den WidgetManager und sind schon erledigt. **Komponentenklassen gibt es nicht**: eine Entity-Klasse bringt eine Komponentenliste mit (Prefab-Blob), aber eine Komponente ist kein HC-Klassentyp und hat keine Variablen eines Graphen. Ein „Add Component" für HC-Klassen existiert also nicht und gehört nicht hierher (neue Klassentypen sind ausdrücklich ausgeschlossen).

### 1.3 Welche Knoten und Wege eine HC-Klasse erzeugen

| Weg | Erzeuger | Bekommt Spawn-Werte? |
|---|---|---|
| Knoten **Create Object** (`NodeType::CreateObject`, `s` = Klassen-Asset-Pfad) | Graph, Interpreter + Codegen | **ja**, der einzige Knoten dieses Themas |
| Registry-Zeilen `entity.spawnClass` / `entity.spawnClassRotated` (Knoten „Spawn Class", Lua, Python, C++) | `EngineApi.cpp` `spawnClassImpl` → `Ctx::createObject` | nein, die Klasse ist ein **String-Pin**, zur Entwurfszeit also unbekannt, es gibt nichts zu spiegeln. Wie `widget.create` bei Widgets. |
| Im Level platzierte Entities mit ScriptComponent | `EntityHost::begin`/`bindFor` | nein, kein Erzeuger. Das Gegenstück wäre Unreals „Instance Editable" pro platzierter Instanz, eigenes Thema. |
| Game Instance | App beim Start | nein, kein Erzeuger |
| PlayerController aus den Projekteinstellungen | PlayerHost | nein, kein Erzeuger |
| Client-Kopie eines replizierten Spawns | `SpawnReplicator` (SPAWN trägt Pfad + Pose) | nein, siehe §7.3 |

Hinter Create Object liegen zwei Erzeugungswege, beide in den App-Lambdas `g_host.createObject` (`EditorApplication.cpp` ~1574, `GameApplication.cpp` ~1124):

* **Entity-Klassen** (aufgelöste Basis ist `Entity` oder davon abgeleitet, EntityHost läuft): `EntityHost::spawn(path, parent, pos, rot)` baut Entity, Platzierung und Physik und ruft `bind()`. `bind()` legt die Instanz an (`addCompiled` oder `addLevels`), setzt `setOwnedEntity` und feuert dann **Construct und BeginPlay direkt hintereinander**. Von außen kommt man nicht dazwischen.
* **Objekt-Klassen** (alles andere): `addCompiled` (nur im Spiel, kompilierte Klasse zuerst) oder `addLevels(resolveClassAsset(...))`, danach `fireConstruct`. Kein BeginPlay, die Platzierung wird verworfen.

## 2. Was der Nutzer sieht

* In den Details einer Variablen im **Klassen-Editor** das Häkchen **Expose on Spawn**, dasselbe Flag wie bei Widgets. Ausgegraut für private Variablen und lokale Variablen. Nur im Klassen-Editor (`HorizonCodeClassPanel`), nicht für Level-Skripte und die Game Instance: die erzeugt kein Knoten.
* Jeder **Create Object** dieser Klasse (und jeder davon abgeleiteten Klasse, §3.2) bekommt einen Dateneingang pro freigegebener Variable, benannt und getypt wie die Variable, **hinter** Location und Rotation. Der Ausgang `Object` liegt dahinter.
* Ein Pin wirkt, wenn er verdrahtet ist oder am Knoten einen Wert trägt; sonst behält die neue Instanz ihren eigenen Vorgabewert (Make-Struct-Regel, §6.3 der Widget-Doku). Bool/Int/Float/Double/String haben ein Inline-Feld, das beim Spiegeln mit dem Vorgabewert der Variablen vorbelegt wird. Vektoren, Farben, Referenzen, Structs und Container wirken nur verdrahtet.
* Der Wert liegt an, bevor Construct läuft, und bei Entity-Klassen damit auch vor BeginPlay. Setzt Construct (oder BeginPlay) dieselbe Variable selbst, gewinnt das, wie bei Widgets und in Unreal.

Zwei Regeln auf einem Knoten, und das ist Absicht: **Location/Rotation wirken nur verdrahtet** (unverdrahtet heißt „wie die Klasse es vorsieht", seit es diese Pins gibt), **Spawn-Pins wirken verdrahtet oder mit Wert**. Der Unterschied ist sachlich: für die Platzierung gibt es kein Inline-Feld (Vec3), und „0,0,0" wäre ein Teleport; für eine Zahl ist ein getippter Wert ein gewollter Wert.

## 3. Pins

### 3.1 Layout und Abwärtskompatibilität

```
Create Object:  execIn 0 | execOut 1 | Location 2, Rotation 3, Spawn₀ 4, Spawn₁ 5 … | Object 4+N
```

* Spawn-Pins sind `Node::params`, in `signatureInto` hinter `{Location, Rotation}` an `dataIns` gehängt, genau wie bei Create Widget (dort ohne feste Eingänge davor).
* Ein Knoten ohne `params` (jeder Graph von vorher, und jeder Create Object einer Klasse ohne Häkchen) hat exakt das heutige Layout, der Ausgang bleibt Pin 4. Keine neue Migration.
* Die bestehende Migration in `fromJson` (Link **ab** Pin 2 eines Create Object → Pin 4) bleibt eindeutig: Pin 2 ist in jedem Layout, mit oder ohne Spawn-Pins, der Location-**Eingang**, und von einem Eingang geht nie ein Link aus. Ein Graph mit Spawn-Pins speichert seine `params` mit, sein Layout ist beim Laden also das, unter dem seine Links geschrieben wurden.
* `pinDefaults` sind nach **Dateneingangs-Index** geschlüsselt (`HorizonCode.h`, Node). Spawn-Pin `i` hat den Schlüssel `2 + i`. Interpreter, Codegen und Spiegel rechnen mit diesem Versatz.

### 3.2 Welche Variablen: Vererbung

Eine Klasse erbt Variablen (`ResolvedClass`, `HcClassResolve.h`); eine öffentliche Variable der Basis ist an der abgeleiteten Instanz per Set (Ref) erreichbar, also auch per Spawn-Wert. Deshalb:

* `spawnPinsOfClass(const ResolvedClass&)` (neu, neben `spawnPinsOf`): läuft über `rc.levels` **Wurzel zuerst**, die Klasse selbst zuletzt, und innerhalb jeder Ebene in Deklarationsreihenfolge. Aufgenommen wird eine Variable mit `exposeOnSpawn`, `access == 0`, `scope == 0`. Namen sind in der Kette eindeutig (der Editor verweigert eine Neudeklaration, `inheritedVariables`), Doppelte werden trotzdem defensiv nur einmal genommen, bei der nächsten Deklaration.
* Das Häkchen sitzt an der **Deklaration**. Eine abgeleitete Klasse kann es für eine geerbte Variable nicht setzen oder entfernen; der Klassen-Editor zeigt geerbte Variablen ohne das Häkchen.
* `spawnPinsOf(graph)` bleibt für Widgets wie es ist.

### 3.3 Namenskollision mit Location/Rotation

Der Pin-Spiegel und `remapLinksFromSnapshot` hängen Leitungen nach Pin-**Namen** um. Eine Klassenvariable `Location` oder `Rotation` mit Häkchen ergäbe zwei gleichnamige Eingänge, und eine Leitung am Spawn-Pin rutschte beim nächsten Spiegeln auf den festen Eingang. Bei Widgets gab es keine festen Daten-Eingänge, deshalb ist das neu.

Entscheidung: **`spawnPinsOfClass` lässt die Namen `Location` und `Rotation` aus.** Der Klassen-Editor graut das Häkchen für Variablen dieses Namens aus, mit dem Grund im Tooltip („Create Object hat schon einen Eingang dieses Namens; benenne die Variable um"). Wird eine Variable mit Häkchen später in einen der beiden Namen umbenannt, verschwindet ihr Pin beim nächsten Spiegeln sichtbar samt Leitung; das Flag selbst bleibt gespeichert. Ausgänge (`Object`) liegen in einem anderen Register und kollidieren nicht.

### 3.4 Spiegeln

`syncSpawnPins` bekommt den Index des ersten Spawn-Eingangs als Parameter (`firstSpawnDataIn`: 0 für Create Widget, 2 für Create Object) und akzeptiert beide Knotentypen. Es

* ersetzt nur die `pinDefaults` mit Schlüssel `≥ firstSpawnDataIn`; die festen Eingänge davor behalten ihre (heute hat Location/Rotation keine, weil Vec3 kein Inline-Feld hat, aber das soll nicht davon abhängen),
* schlüsselt Inline-Werte nach Namen um, wie heute,
* belegt neue und umgetypte Pins mit dem Vorgabewert vor, ein Pin mit dem **vorherigen** Vorgabewert folgt dem neuen, wie heute.

## 4. Laufzeit: wann der Wert gesetzt wird

### 4.1 Schnittstelle

Entscheidung zum Signatur-Ripple: **erweitert wird nur die HorizonCode-Seite, wie bei `createWidget`.**

* `HorizonCode::Context::createObject` und `Runtime::Services::createObject` bekommen als vierten Parameter `const SpawnValues& spawn` (leer = Vorgaben der Klasse). `Runtime::makeContext` reicht ihn durch.
* `HE::api::Ctx::createObject` (Registry, `spawnClassImpl`) und `ScriptContext::HostServices::createObject` (Lua/Python) **bleiben dreistellig**. Sie haben keine Werte zu geben (§1.3).
* In beiden Apps gibt es danach **eine** Implementierung mit vier Parametern (`refuseClientSpawn`, EntityHost-Weg, Objekt-Weg, PlayerCharacter-Anmeldung, alles darin). `svc.createObject` ist sie selbst; `g_host.createObject` ist eine dreistellige Weiterleitung mit `{}`. Damit bleibt es bei der einen Tür, die `NetGameSession.h` und `SpawnReplicator.h` beschreiben: jeder Spawn, egal aus welchem Frontend, läuft durch denselben Code.
* Verworfen: ein vierter Parameter auch in `api::Ctx`/`HostServices`. Das zöge Lua, Python, die Registry und `PyScriptBackend` mit, ohne dass einer davon Werte hätte. Verworfen auch: ein zweiter Member nur für HC (`createObjectWithSpawn`); das wären zwei Türen.
* Betroffene Tests: die Stellen, die `HorizonCode::Context::createObject` oder `Runtime::Services::createObject` belegen (`test_horizoncode_runtime`, `test_horizoncode_compiled`, `test_horizoncode_codegen`, `test_entity_host`, `test_net_game_session`, …; 14 Zuweisungen in `tests/` insgesamt, ein Teil davon `api::Ctx`, der bleibt).

### 4.2 Gesetzt wird über einen gemeinsamen Helfer

`Runtime::applySpawnValues(InstanceId, const SpawnValues&, const std::string& classPath)`: ruft `setPublicVariable` je Wert, überspringt einen unbekannten oder privaten Namen mit einer Warnung im Stil des Widgets („Create Object '<pfad>': '<name>' is no public variable of the class (renamed or made private?) — value skipped"). Auf das Häkchen prüft die Laufzeit nicht, wie bei Widgets (§6.4 dort): es steuert, welche Pins der Editor anbietet, und kompilierte Klassen brauchen kein neues Reflexionsfeld. `WidgetManager::createWidget` bleibt unverändert (keine Änderung des Widget-Verhaltens), darf den Helfer aber später nutzen.

### 4.3 Objekt-Klassen

In beiden App-Implementierungen zwischen dem Anlegen und Construct:

```
inst = addCompiled(...) | addLevels(resolveClassAsset(...))   ← Variablen mit Vorgaben geseedet
applySpawnValues(inst, spawn, assetPath)                    ← neu
fireConstruct(inst)
```

### 4.4 Entity-Klassen

`EntityHost::spawn` und `EntityHost::bind` bekommen einen optionalen Parameter `const HorizonCode::SpawnValues* spawn = nullptr`. `spawn()` reicht ihn an `bind()` durch; `bind()` setzt die Werte nach `addCompiled`/`addLevels` und `setOwnedEntity`, **vor** `fireConstruct` und damit vor `fireBeginPlay`:

```
EntityHost::spawn(path, parent, pos, rot, spawn):
  Entity aus der Komponentenliste, Platzierung, Physik    (wie heute)
  bind(entity, path, spawn):
    inst = addCompiled | addLevels; setOwnedEntity
    applySpawnValues(inst, *spawn, assetPath)              ← neu
    fireConstruct(inst); fireBeginPlay(inst)
```

`begin()` und `bindFor()` (im Level platzierte Entities) rufen `bind()` ohne Werte, ihr Verhalten ändert sich nicht. Die App-Implementierung reicht `&spawn` an `m_entityHost.spawn(...)`; die Anmeldung beim PlayerHost (`addCharacter`) kommt wie heute danach.

### 4.5 Interpreter

`Runner::execNode`, Fall `CreateObject`: vor dem Aufruf, im Kontext des Erzeugers, `SpawnValues` sammeln, für jeden Spawn-Pin `i` mit `inputLinked(n, 2+i) || n.pinDefaults.count(2+i)`, Wert `coerce(evalInput(n, 2+i, …), n.params[i].type)`. Location/Rotation wie heute nur nach Leitung. Dann `m_ctx.createObject(n.s, posPtr, rotPtr, spawn)`. Die Fehlermeldung bei `ref == 0` bleibt.

## 5. Editor

### 5.1 Häkchen

`LevelScriptPanel.cpp` `drawVariableDetails`, im öffentlichen Zweig neben Replicated und Save Game, nur wenn das Panel ein Klassen-Asset zeigt. Das weiß die Funktion heute nicht (Parameter: Graph, geerbte Variablen, ContentManager); die Panel-Art muss hineingereicht werden. Ausgegraut bei `access != 0` und bei den Namen `Location`/`Rotation` (§3.3), jeweils mit Grund im Tooltip. Private schalten löscht das Flag, wie im Widget-Editor. Hilfetext unter `Script Variable/Expose on Spawn` in `EditorHelp.cpp` (die Tooltip-Abdeckung ist vollständig und soll es bleiben).

### 5.2 Pin-Spiegel

`syncCreateWidgetPins` wird zu einem Spiegel für beide Knotentypen verallgemeinert (oder bekommt einen Bruder `syncCreateObjectPins`; die Wahl ist Schritt 3 überlassen, beide teilen den Cache-Mechanismus):

* Antwort pro Klassenpfad: `resolveClassAsset(content, path)` und `spawnPinsOfClass(rc)`.
* **Der Cache-Schlüssel muss die ganze Kette abdecken**, nicht nur das eigene `graphJson`: Hash über Pfad und Graph-JSON der Klasse und jedes Vorfahren (`rc.chain`) sowie den `baseClass`-String. Sonst änderte ein Häkchen an der Basis die Pins am Create Object der abgeleiteten Klasse nicht. Höchstens zweimal pro Sekunde pro Pfad wie bei Widgets, `force` beim Wählen der Klasse.
* Fehlendes Klassen-Asset: Knoten bleibt, wie er ist (Leitungen bleiben für eine Wiederherstellung).
* In `buildModel` zusammen mit dem Widget-Spiegel, Änderung meldet `onEdit(false)` (geändert, kein Undo-Punkt). Das gilt in jedem Graph-Host, auch in Widget- und Level-Graphen, die Create Object benutzen.
* Klassen-Picker in `drawCommonNodeDetails` (`case NT::CreateObject`): nach `n.s = c.path` sofort mit `force` spiegeln, wie beim Widget-Picker. Gleichnamige und gleich getypte Pins der alten und neuen Klasse behalten Leitung und Wert, der Rest fällt sichtbar weg. Hinweistext ergänzen („Its Expose on Spawn variables appear as inputs").

### 5.3 Löschen, Häkchen weg, privat, Typwechsel

* **Variable gelöscht, Häkchen entfernt, privat geschaltet, Klasse nicht mehr abgeleitet:** der Pin verschwindet beim nächsten Spiegeln, seine Leitung fällt sichtbar weg (`remapLinksFromSnapshot`), sein Inline-Wert mit. Wie bei Widgets.
* **Typwechsel:** Der Name bleibt, also hängt `remapLinksFromSnapshot` die Leitung heute an den umgetypten Pin, und `syncSpawnPins` setzt nur den Inline-Wert auf den neuen Vorgabewert zurück. Eine Leitung eines nun falschen Typs bliebe stehen, und der Codegen stolperte darüber. Für Create Object gilt deshalb: **bei Typwechsel (alles außer dem Namen in `sameSpawnParam` verschieden) fällt die Leitung an diesem Pin**, so wie Get/Set Variable bei einem Typwechsel ihre Leitung verlieren (`LevelScriptPanel.cpp`, `removePinLinks` nach Typwechsel). Umgesetzt als Option von `syncSpawnPins` (`dropWiresOnRetype`), für Create Object an.
* Create Widget hat dieselbe Lücke. Die Option dort einzuschalten ist eine Änderung des Widget-Verhaltens und damit nicht Teil dieses Themas; als Folgepunkt notiert (§9).
* **Umgesetzt in Schritt 3, strenger als oben:** `remapLinksFromSnapshot` behält bei gleich großer Pin-Region den Index („in-place rename“). Für Spawn-Pins hieß das: eine Variable gelöscht und eine andere angehakt (oder Klassenwechsel am Knoten) in einem Zug, und die alte Leitung rutschte still auf den neuen Pin. Unter `dropWiresOnRetype` bleibt eine Leitung deshalb nur an einem Pin **ihres Namens**. Ein echtes Umbenennen läuft vorher über `HcRename` (benennt den Pin selbst um), der Name passt dann.
* Jede Leitung, die der Spiegel an einem Create Object kappt, steht als Warnung im Log (Kategorie Editor) mit Pfad, Knoten, Pin-Name und Grund (Pin weg / Typwechsel). Bei Create Widget nicht, Folgepunkt.
* Der Spiegel liest das Klassen-**Asset**; der Klassen-Tab schreibt es beim Speichern. Ein neues Häkchen, ein Löschen oder Abhaken wirkt an den Create-Object-Knoten also erst nach dem Speichern der Klasse (passt zu §7.2).

### 5.4 Umbenennen

`HcRename.cpp`: der Zweig für `CreateWidget` in `planGraph` und `apply` wird auf `CreateObject` erweitert (`n.type == NT::CreateWidget || n.type == NT::CreateObject`, Test `contains(targetKeys, n.s)`, in `apply` den Pin-Namen umschreiben statt `n.s`).

* `targetKeys` sind Klassen-Asset-Pfade, die zu benennende Klasse **und jede davon abgeleitete** (`HcRenameSweep::classAndDescendants`, über `listHorizonCodeClasses`). Der Klassen-Picker von Create Object schreibt genau diese Pfade nach `n.s`; `classOfRefSource` behandelt Create Object schon so. Umbenennen einer Basis-Variablen benennt damit die Pins am Create Object jeder abgeleiteten Klasse mit um.
* Ohne diesen Zweig sähe der Spiegel den umbenannten Pin als neu und verlöre den getippten Wert.
* Umbenennen in `Location`/`Rotation`: siehe §3.3, der Pin fällt beim nächsten Spiegeln weg. Kein Blockieren des Umbenennens.
* **Auslöser (Schritt 3):** Bis dahin stieß ein Variablen-Umbenennen in keinem Panel den Projekt-Rename an, nur Funktionen taten das; der `CreateWidget`-Zweig war nur über Tests erreichbar. Jetzt: Umbenennen einer öffentlichen Instanzvariablen im Klassen-Tab (`drawVariableDetails`, `derivable`) zieht im eigenen Graph Create-Object-Pins (und Get/Set External auf Self) per `planGraph(Role::Declares, classAndDescendants)` nach und parkt `Member::Variable` in `s_pendingRename`, also derselbe Dialog wie bei Funktionen für den Rest des Projekts.
* **Live-Asset:** Weil der Spiegel das Asset liest und der Tab erst beim Speichern schreibt, hätte der Spiegel den gerade umbenannten Pin sofort auf den alten Namen zurückgedreht (Leitung und Wert weg). Deshalb wird **nur die Umbenennung** zusätzlich in den Live-`HorizonCodeClassAsset` geschrieben (nicht auf Platte), so wie der Dialog sie in die anderen Assets schreibt. Bricht jemand den Dialog ab, behalten fremde Graphen den alten Pin-Namen, und der Spiegel kappt dort beim nächsten Öffnen die Leitung, mit Log-Warnung.

## 6. Codegen

`HcCodegen.cpp`, Fall `CreateObject`:

* Spawn-Pin `i` wirkt, wenn `dataLinkTo(n.id, r.dataIn0 + 2 + i)` oder `n.pinDefaults.count(2 + i)`, dieselbe Regel wie der Interpreter.
* Weder Location, Rotation noch ein Spawn-Pin wirkt: exakt die heutige Zeile `hc::createObject(m_ctx, "<pfad>")`, byte-gleich (Paritätstext unverändert).
* Nur Location/Rotation: der heutige Block, byte-gleich.
* Sonst ein Block mit den benannten Vektoren wie heute und `hc::SpawnValues s<id>` (wie bei Create Widget, `toValueCall(input(n, 2+i, …), dataInType(n, 2+i), …)`), Aufruf der neuen Überladung `hc::createObject(m_ctx, "<pfad>", posPtr, rotPtr, s<id>)`.
* `HorizonCodeGenSupport.h/.cpp`: Überladung `createObject(const Context&, const char*, const float*, const float*, const SpawnValues&)`, nicht inline wie die bestehende, mit derselben Fehlermeldung. Die bestehende Funktion ruft `c.createObject(path, pos, rot, {})`.
* Kompilierte Zielklassen: `setPublicVariable` deckt sie ab (`CompiledVarInfo::access`), nichts Neues im generierten Klassencode.
* **Umgesetzt in Schritt 4:** Reihenfolge im Block wie im Interpreter, erst Location, dann Rotation, dann die Spawn-Werte in Pin-Reihenfolge (ein reiner EngineCall an einem dieser Pins macht die Reihenfolge in der Spur sichtbar). Die vierstellige Funktion ruft die fünfstellige mit `SpawnValues{}`, die Fehlermeldung steht nur dort. Parity-Fixtures `spawn_target` (Int, Float, String, Bool, Vec3, Enum, Struct angehakt, dazu Color und Float, die der Aufrufer liegen lässt; Construct kopiert in `seen*`) und `spawn_caller` (drei Create Object: nur Werte, Platzierung plus Wert, Pins ohne Wirkung), Pins gespiegelt wie im Editor (`spawnPinsOfLevels` + `syncSpawnPins`). Die createObject-Zeile der Paritäts-Spur nennt die übergebenen Werte, wie createWidget; leer bei allen anderen Fixtures, deren Spur also unverändert. Text-Test: alte Zeile byte-gleich bei Pins ohne Wirkung, alter Platzierungsblock bei refs_objects, kein Fallback.

## 7. Abwärtskompatibilität und Grenzen

### 7.1 Alte Graphen

* Kein `"spawn"` an einer Variablen: Variable byte-gleich gespeichert, keine Pins.
* Kein `params` am Create Object: Layout wie heute, Ausgang Pin 4, Migration Pin 2 → 4 unverändert, Interpreter und Codegen rufen exakt wie heute (Codegen-Text byte-gleich).
* Ein Graph, der schon `"spawn"` an einer Klassenvariablen trägt (möglich, weil der Loader das Flag nicht nach Graph-Art unterscheidet), wird erst durch den Spiegel des nächsten Öffnens zu Pins; das ist erwünscht.

### 7.2 Gespiegelter Stand

Wie bei Widgets (§6.8 dort): der Stand am Knoten ist der vom letzten Öffnen. Exportiert jemand nach einer Klassenänderung, ohne den Aufrufer zu öffnen, nehmen Interpreter und Codegen die gespeicherten Pins; was es nicht mehr gibt, überspringt die Laufzeit mit Warnung (§4.2). Das Folgen eines geänderten Vorgabewerts kennt nur die laufende Editor-Sitzung.

### 7.3 Netz

`refuseClientSpawn` steht vor dem Setzen: ein Client erzeugt keine replizierten Objekte und setzt also auch nichts. Die Client-Kopie eines vom Host gespawnten Objekts entsteht über `SpawnReplicator` aus Pfad und Pose, **ohne** Spawn-Werte; ihr Construct und BeginPlay sehen die Vorgabewerte. Eine freigegebene Variable, die auch **Replicated** ist, kommt mit der Property-Replikation nach (RepNotify feuert dann). Die Werte in der SPAWN-Nachricht mitzuschicken ist ein eigenes Thema; im Handbuch als Grenze nennen.

### 7.4 Lua, Python, Spawn Class

`entity.spawnClass(Rotated)`, Lua und Python erzeugen Klassen weiter ohne Werte. Im Handbuch nennen, wie bei Widgets.

## 8. Tests (für Schritte 2–4)

Vorlagen: `tests/test_widget_expose_on_spawn.cpp`, `tests/test_entity_host.cpp`, `tests/test_horizoncode_codegen.cpp` (Fixture in `tests/fixtures/hcodegen/fixtures.h`).

1. **Reihenfolge Objekt-Klasse:** ein Spawn-Wert ist in Construct sichtbar. Negativkontrolle: Werte nach `fireConstruct` gesetzt macht den Fall rot.
2. **Reihenfolge Entity-Klasse:** über `EntityHost::spawn` ist der Wert in Construct **und** BeginPlay sichtbar; `begin()`/`bindFor()` ohne Werte unverändert.
3. **Wirkt-Regel:** unverdrahteter Pin ohne Wert lässt den Vorgabewert stehen; Pin mit Wert am Knoten setzt ihn; Location/Rotation unverdrahtet bleiben „wie die Klasse es vorsieht", auch wenn Spawn-Pins wirken.
4. **Vererbung:** Pins der Basis erscheinen am Create Object der abgeleiteten Klasse, Wurzel zuerst; der Wert landet in der Basis-Variablen der Instanz.
5. **Unbekannt/privat:** übersprungen mit Warnung, kein Absturz, Instanz entsteht.
6. **Kompiliert:** kompilierte Zielklasse bekommt den Wert (`setPublicVariable`-Zweig).
7. **Spiegel:** Versatz 2 (Location/Rotation-Leitungen und Ausgangsleitung folgen), Umsortieren, Löschen (Leitung weg), Typwechsel (Leitung weg, Wert = neue Vorgabe), `Location`/`Rotation`-Name ausgelassen, Cache reagiert auf Änderung an der **Basis**.
8. **Alter Graph:** Create Object ohne `params`, Ausgang Pin 4, Migration Pin 2 → 4 greift weiter.
9. **Umbenennen:** Variable der Klasse und einer Basis benennt den Pin um, Inline-Wert bleibt.
10. **Codegen:** ohne wirkende Pins byte-gleiche Zeilen (beide heutigen Formen); mit Spawn-Pin baut das Fixture und ist paritätisch zum Interpreter.
11. **Create Widget unverändert:** die bestehenden Widget-Tests laufen ohne Änderung grün.

## 9. Aufteilung auf die Schritte und Folgepunkte

* **Schritt 2** (Kern): `spawnPinsOfClass`, `syncSpawnPins` mit Versatz und `dropWiresOnRetype`, `signatureInto`, Interpreter, `Context`/`Services`-Signatur, `Runtime::applySpawnValues`, `EntityHost::spawn/bind`, beide App-Implementierungen, Tests 1–8.
* **Schritt 3** (Editor): Häkchen in `drawVariableDetails`, Spiegel mit Ketten-Hash, Picker-`force`, `HcRename`, Hilfetext, Tests 7 und 9 auf Editor-Seite.
* **Schritt 4** (Codegen): Emitter, Überladung, Fixture, Test 10.
* **Schritt 5** (Doku): `docs/horizoncode-reference.md` (§1 Expose on Spawn um Klassen erweitern, Tabellenzeile Create Object), In-Engine-Handbuch `EditorDeps/Docs/he-docs.json` (Abschnitt HorizonCode-Klassen und Knoten Create Object, inkl. Grenzen §7.3/7.4), Tooltip von Create Object in `nodeTooltip`.
* **Folgepunkte, nicht in diesem Thema:** Typwechsel lässt bei Create Widget die Leitung stehen (§5.3); Spawn-Werte in der SPAWN-Nachricht (§7.3); Werte für `entity.spawnClass`/Lua/Python; „Instance Editable" für im Level platzierte Entities.
