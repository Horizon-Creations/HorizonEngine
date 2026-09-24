# Script-API-Doku: Bestandsaufnahme 24.09.2026 (Thema 79, Schritt 1)

**Stand:** `main` 7d49d44f, Website-Checkout `Website/HorizonEngineDocs` (f54ea04, Seiten vom 08.–19.09.).
**Grundlage:** die Registry, wie sie zur Laufzeit gebaut wird, nicht ein Grep über `EngineApi.cpp`.
Ein Grep nach `t.push_back({ "` findet 445 Zeilen; die Registry hat 582, weil viele Rows über
Helfer entstehen (`unary("math.sin", …)`, die generierten `env.*`-Felder).

Die Id-für-Id-Liste steht in **`docs/script-api-docs-coverage.md`** (generiert, 800 Zeilen). Dieses
Dokument ordnet sie ein und schlägt die Folgeschritte vor.

## Werkzeug (reproduzierbar)

| Datei | Zweck |
|---|---|
| `scripts/script_api_docs/dump_engine_api.cpp` + `.sh` | kompiliert gegen einen vorhandenen Build (`libHorizonScene.dylib`) und schreibt `HE::api::registry()` als JSON: Id, Gruppe, Kategorie, Display-Name, exec/pure, `isScriptGroup`, C++-Callee, Pins (Typ, Array, `selfDefault`) **und die Editor-Beschreibung aus `HcNodeDocs::engineCall(id)`** |
| `scripts/script_api_docs/registry.json` | Schnappschuss dieses Dumps (582 Rows, 7d49d44f). Nach Registry-Änderungen neu erzeugen |
| `scripts/script_api_docs/coverage.py` | gleicht den Dump mit der Website-Doku ab und schreibt `docs/script-api-docs-coverage.md` |

```sh
scripts/script_api_docs/dump_engine_api.sh /Users/connorjansen/VSCode/HorizonEngine/build   # aus einem Worktree
python3 scripts/script_api_docs/coverage.py
```

Die Abdeckungsstufen je Id: **ref** = Signatur + Rückgabe dokumentiert, **named** = die Id steht wörtlich
auf einer Doku-Seite (Prosa oder Beispiel), **catalog** = nur der Funktionsname in der Katalogtabelle
von `horizoncode-nodes.html#engine-call`, **missing** = nirgends. „catalog" ist eine Namensnennung ohne
Parameter, Rückgabe oder Beschreibung. Für eine Referenz zählt nur **ref**.

## Ergebnis in Zahlen

| Oberfläche | Umfang | mit Signatur dokumentiert |
|---|---:|---:|
| Registry-Ids (`horizon.<gruppe>.*`, Engine-Call-Knoten, `HE::api::` in C++) | 582 in 42 Gruppen | **0** direkt, 13 nur über einen flachen Zwilling |
| davon in Lua/Python als `horizon.<gruppe>.*` erreichbar | 570 in 38 Gruppen | 0 |
| flache `horizon.*`-Shims (Lua + Python, nicht in der Registry) | 35 | 13 |
| Lifecycle-Callbacks Lua | 18 | 4 |
| Lifecycle-/Event-Callbacks Python (Behavior, GameInstance, Widget) | 24 | 7 (Name genannt) |

Registry-Ids nach Stufe: **ref 13, named 39, catalog 96, missing 434.** Eine ehrliche Kennzahl für den
Roadmap-Eintrag wäre „Ids mit Signatur + Beschreibung / (582 + 35)". Sie liegt heute bei
**26 / 617 ≈ 4 %** (13 flache Funktionen, die 13 Registry-Ids über ihre Zwillinge mitgezählt). Die 35 %
auf der Website messen etwas anderes, die Seiten *um* die API herum (Lifecycle, Properties, Beispiele).
Die Roadmap wurde in diesem Schritt nicht angefasst.

### Je Gruppe

`script` = in `HE::api::isScriptGroup()`, also als `horizon.<gruppe>.*` in Lua/Python da.

| Gruppe | Ids | script | ref | named | catalog | missing |
|---|---:|:---:|---:|---:|---:|---:|
| env | 116 | ja | 0 | 3 | 7 | 106 |
| net | 44 | ja | 0 | 0 | 0 | 44 |
| physics | 33 | ja | 3 | 3 | 0 | 27 |
| math | 31 | ja | 0 | 1 | 11 | 19 |
| widget | 28 | ja | 0 | 0 | 3 | 25 |
| camera | 26 | ja | 0 | 0 | 6 | 20 |
| app | 24 | ja | 0 | 0 | 0 | 24 |
| audio | 18 | ja | 0 | 4 | 3 | 11 |
| entity | 18 | ja | 3 | 1 | 9 | 5 |
| input | 17 | ja | 0 | 8 | 0 | 9 |
| save | 17 | ja | 0 | 6 | 5 | 6 |
| anticheat | 15 | ja | 0 | 0 | 0 | 15 |
| fs | 13 | ja | 0 | 0 | 5 | 8 |
| time | 13 | ja | 0 | 3 | 3 | 7 |
| scene | 12 | ja | 0 | 3 | 9 | 0 |
| string | 12 | ja | 0 | 0 | 11 | 1 |
| ui | 12 | ja | 0 | 0 | 11 | 1 |
| datetime, http, prefs | je 9 | ja | 0 | 0 | 0 | 27 |
| animator | 8 | ja | 0 | 2 | 0 | 6 |
| json | 8 | ja | 0 | 0 | 0 | 8 |
| transform | 8 | **nein** | 6 | 0 | 0 | 2 |
| db | 7 | ja | 0 | 0 | 0 | 7 |
| locomotion, movement, nav, player, theme | je 6 | ja | 0 | 0 | 6 (player) | 24 |
| dialog, random, timer, window | je 5 | ja | 0 | 0 | 5 (random) | 15 |
| content, debug, particle | je 4 | ja | 0 | 4 (debug) | 0 | 8 |
| clipboard, print, process | je 3 | ja | 0 | 0 | 0 | 9 |
| material | 2 | **nein** | 0 | 1 | 1 | 0 |
| cursor | 1 | **nein** | 0 | 0 | 1 | 0 |
| log | 1 | **nein** | 1 | 0 | 0 | 0 |

Maßgeblich ist die generierte Tabelle in `docs/script-api-docs-coverage.md`; diese hier ist deren
Zusammenfassung.

**20 Gruppen kommen auf der Website überhaupt nicht vor:** net, app, anticheat, datetime, http, prefs,
json, db, locomotion, movement, nav, theme, dialog, timer, window, content, particle, clipboard, print,
process (zusammen 181 Ids). Das sind vor allem die HE-Apps-Gruppen (Welle 1–3) und Multiplayer.

## Befunde

### 1. Die Prämisse „jede Registry-Row ist in allen vier Frontends erreichbar" stimmt nicht ganz

`transform`, `material`, `cursor` und `log` (12 Ids) stehen **nicht** in `isScriptGroup()`
(`src/HE_Scene/src/EngineApi.cpp:7589`). Es gibt also kein `horizon.transform.setPosition` in Lua/Python,
nur den flachen Zwilling `horizon.setPosition`. `transform.getWorldPosition` und
`transform.setWorldPosition` haben auch keinen flachen Zwilling, sind also aus Lua/Python gar nicht
erreichbar. `material.*` geht nur über `get/setMaterialParam`, mit anderer Arität (`x[,y,z,w]` statt
einer Color). Die Referenz muss
je Id sagen, **wie** sie in welchem Frontend heißt, sonst dokumentiert sie Aufrufe, die es nicht gibt.

### 2. Die Website beschreibt eine veraltete API

- `horizoncode-nodes.html#engine-call`: „20 groups, ~250 functions". Tatsächlich 42 Gruppen, 582 Ids.
  Die Tabelle nennt unter Widget `create`, `destroy`, `show`, `hide`. Diese Rows sind seit bf88185b
  (26.08.) **entfernt**, der Knoten „Create Widget" und in Lua/Python `horizon.createWidget` ersetzen sie.
  Physics steht mit 3 Funktionen drin (real 33), Camera mit 6 (real 26), Input mit 8 (real 17).
- `scripting.html#api`: zählt 13 Namespaced-Gruppen auf, real sind es 38.
- `scripting-api.html#how-scripts-run`: „The standalone runtime that ships with an exported game does
  not yet drive script lifecycles". Veraltet, `GameApplication::startScripts()` / `startScriptsFor()`
  (`src/HE_Game/src/GameApplication.h:91,315`) starten Skripte im gepackten Spiel.
- `scripting-api.html#api` kündigt „Every horizon.* function" an und listet 13 von 35 flachen Funktionen.
  Es fehlen Material (`get/setMaterialParam`), alle 11 Entity-UI-Funktionen, die 7 Widget- und die
  2 Cursor-Funktionen.
- Die Editor-interne Knotenreferenz (`src/HE_Editor/HcNodeReference.cpp`) wird aus der Registry
  generiert und ist aktuell. Deshalb nimmt `scripts/build_docs_bundle.py` die Website-Seite
  `horizoncode-nodes.html` bewusst nicht ins Editor-Handbuch auf. Die Website selbst hat keine
  generierte Entsprechung.

### 3. Lifecycle-Callbacks: 4 von 18 dokumentiert

Dokumentiert sind nur `onStart`, `onUpdate`, `onCollisionEnter`, `onCollisionExit`. Es fehlen (Lua-Namen)
`onBeginOverlap`, `onEndOverlap`, `onInputPressed`, `onInputReleased`, `onInputAxis`, `onInputAxis2D`,
`onTimer`, `onUIEvent`, `onAnimationNotify`, `onAnimationNotifyBegin`, `onAnimationNotifyEnd`, `onRep`
(bzw. `onRep_<var>`), `onNetEvent`, `onCheatDetected`. In Python zusätzlich die Session-Callbacks
`on_connected`, `on_disconnected`, `on_player_joined`, `on_player_left`, `on_session_started`,
`on_session_ended` (aus `NetScriptEvent`, `PyScriptBackend.cpp:1470`). Ohne diese Liste sind `net`,
`anticheat`, `timer` und die Input-Actions nur halb benutzbar, denn ihre Ergebnisse kommen als Callback an.

### 4. Die Beschreibungstexte gibt es schon, für jede Id

`src/HE_Editor/HcNodeDocs.cpp` hat für **alle 582 Ids** eine Beschreibung (testgesichert durch
`test_hc_node_docs`, `env.*` aus der Feldliste generiert); der Dump schreibt sie ins Feld `doc`.
Die Texte sind für HorizonCode geschrieben (sie nennen Knoten beim Display-Namen, z. B. „For a
character use Move instead"), aber inhaltlich richtig und aktuell.

**Empfehlung für alle Folgeschritte:** die Website-Referenz aus `registry.json` **generieren**, nicht
582 Einträge von Hand schreiben. Handarbeit kommt nur dazu, wo der Generator nicht reicht: je Gruppe
eine Einleitung, Beispiele je Sprache, Hinweise zu Aritätsunterschieden und die Callbacks. Sonst ist
die Seite beim nächsten Registry-Ausbau wieder so veraltet wie der Katalog heute. Die Beschreibung
müsste dazu Display-Namen („Move") in Ids übersetzen oder neutral formuliert werden. Das ist ein
Punkt für Schritt 2, nicht für jede Gruppe neu.

### 5. Querschnitt, der vor der ersten Gruppenseite stehen muss

Jede Signatur hängt an diesen Regeln, sie gehören einmal zentral auf die Referenzseite:

- **Aufruf-ABI des Registry-Dispatchers** (`lua_engine_dispatch`, `ScriptContext.cpp:819`; Python
  `horizon._engineCall`): Parameter werden nach Pin-Typ gelesen und flach übergeben. Vec2 = 2 Zahlen,
  **Vec3 = 3**, Color/Vec4 = 4, Int/Enum/Ref = Integer, Struct = Table/Dict mit `__type`
  (`horizon.structs.<Name>()`), mehrere Ergebnisse = Lua-Mehrfachrückgabe / Python-Tupel.
  Die flachen Shims weichen davon ab (optionale Argumente, `raycast` liefert eine benannte Tabelle,
  `setMaterialParam(e, name, x[,y,z,w])`). Das ist `docs/rework-2026-07-deferrals.md` §1, und dort steht
  die Vec3-Spreizung noch als „vier Zahlen"; seit es `PinType::Vec3` gibt, sind es drei. Auch die
  Gruppenliste dort (ui/widget/physics nicht freigeschaltet) ist überholt, maßgeblich ist
  `isScriptGroup()` in `EngineApi.cpp:7589`.
  **Noch am Code zu prüfen:** ob fehlende Argumente in Lua einen Fehler werfen (`luaL_check*`) oder mit
  Defaults auffüllen, wie Array-Parameter/-Ergebnisse (`physics.raycastAll`) ankommen und ob der
  Python-Weg identisch ist.
- **`selfDefault`**: 109 Ids haben einen führenden `entity`-Parameter, bei dem 0 „der Aufrufer selbst"
  heißt (`EngineApi.cpp:7501`). Das spart in jedem Beispiel `self.entityId`, steht aber nirgends.
- **Frontend-Namen je Id**: HorizonCode = Display-Name („Set Position"), Lua/Python =
  `horizon.<gruppe>.<fn>` oder flacher Zwilling, C++ = `cppCall` (`HE::api::transform::setPosition(ctx, …)`)
  bzw. im Game-Logic-Modul über die Services-Tabelle.
- **Variadische Überschreibungen**: `net.callServer`, `net.callClient`, `net.callAllClients` sind in
  Lua/Python durch variadische Funktionen ersetzt (`ScriptContext.cpp:975`, Python `_netCall*`), die
  Registry-Signatur gilt dort nicht.
- **Benutzertypen**: `horizon.enums.<Name>.<Eintrag>` und `horizon.structs.<Name>()` sind nirgends
  dokumentiert.
- **Berechtigungen**: `dialog`, `clipboard`, `process`, `print`, `db`, `app.setAutostart` hängen an
  `perm::allowed` (Projektrechte). Ohne Recht passiert nichts, auch das gehört an jede betroffene Id.
- **exec vs. pure**: 582 Ids tragen `isExec`. Für Skripte heißt das Seiteneffekt vs. reine Abfrage,
  für HorizonCode Exec-Knoten vs. Datenknoten. Das Merkmal kommt in jede Zeile der Referenz.

## Vorschlag für die Folgeschritte

Die Gruppierung im Thema (Math/Transform, Physics, Input, …) kennt die HE-Apps-Gruppen nicht. Diese
Aufteilung deckt alle 582 Ids genau einmal ab:

| Schritt | Inhalt | Ids |
|---|---|---:|
| 2 | **Gerüst + Generator**: Querschnitt aus Befund 5 (am Code verifiziert), Seitenstruktur der Referenz, Generator `registry.json` → HTML im Stil der Doku-Seiten (inkl. Suche/`docs-index.json`), vollständige flache 35 + alle Callbacks; veraltete Stellen aus Befund 2 korrigieren | 35 flach + Callbacks |
| 3 | Math, String, Random, Time, Timer, DateTime, Debug, Log | 80 |
| 4 | Entity, Transform, Scene, Content, Save, File (fs), Prefs, JSON | 89 |
| 5 | Physics, Navigation, Movement, Locomotion, Player, Input | 74 |
| 6 | Animator, Particles, Audio, Camera, Material, Environment (116 davon als generierte Feldtabelle) | 174 |
| 7 | UI, Widget, Cursor, Theme, Window, Dialog, Clipboard, App, Process, Print, Database, HTTP | 106 |
| 8 | Multiplayer (net) + AntiCheat, mit den Session-/Rep-/Cheat-Callbacks | 59 |
| 9 | Kennzahl neu messen (`coverage.py`), Roadmap-Prozent mit Beleg nachziehen, Deploy nach Bestätigung | – |

Steht der Generator aus Schritt 2, schrumpfen 3–8 auf Einleitung, Beispiele und Sonderfälle je Gruppe.
Jeder Schritt sollte `coverage.py` am Ende neu laufen lassen. Dazu muss es lernen, die neue
Referenzseite als **ref** zu zählen: heute erkennt es Signaturen nur in der alten Tabelle
`scripting-api.html#api`.

## Nachtrag Schritt 2 (24.09.2026): am Code und am Build geprüft

Schritt 2 hat Befund 3 und 5 nicht übernommen, sondern nachgeprüft: im Quelltext und mit
Wegwerf-Proben, die gegen `libHorizonScene` des Builds 7d49d44f echte Lua- und Python-Aufrufe
ausführen. Sechs Punkte oben stimmen so **nicht**:

1. **`selfDefault` gilt in Lua/Python nicht.** Der Wrapper (`EngineApi.cpp:7501`) setzt „Self“
   über `entity::self(c)` ein, und der Lua-/Python-Ctx hat `self = 0`. Ergebnis: Warnung „Entity is
   empty and there is no calling object“, die 0 geht unverändert durch und meint die
   Szenen-Wurzel (`horizon.entity.getName(0)` liefert `"World"`). Skripte müssen immer
   `self.entityId` / `self.entity_id` übergeben. „Spart in jedem Beispiel `self.entityId`“ ist
   falsch.
2. **Callback-Namen.** `onUIEvent`, `onNetEvent` und `onRep` sind nur die `HE_SCRIPT_CALL`-Labels
   in `ScriptContext.cpp`. Die Methoden, die ein Skript definiert, heißen `onClick`,
   `onHoverEnter`, `onHoverExit`, `onConnected`, `onDisconnected(reason)`,
   `onPlayerJoined(player)`, `onPlayerLeft(player)`, `onSessionStarted`, `onSessionEnded` und
   `onRep_<var>(old)` (`ScriptEngine.cpp`). Dazu RPC-Methoden unter ihrem wörtlichen Namen. Es sind
   25 feste Namen je Sprache, alle in `horizon.Behavior`: Python hat **keine**
   GameInstance-/Widget-Klassen. Timer, Input-Actions, Netz- und Cheat-Ereignisse gehen an
   **alle** Skriptinstanzen. Kontakte, Notifies und UI-Zeiger gehen nur an die betroffene Entity.
   `onRep_` feuert nie auf dem Host.
3. **Berechtigungen.** `dialog` und `clipboard` hängen an keiner Berechtigung. Gegatet sind
   `process.run`, `process.openUrl`, `print.file`, `app.setAutostart` („Run other programs“),
   `print.toPdf`, `db.open` und `fs.*` mit absolutem Pfad ohne Dialog („Files outside the
   project“) sowie `http.get`/`http.post` („Network access“). Der Editor-Hinweis zu „Network
   access“ („Reserved: nothing reads this yet“, `ProjectSettingsPanel.cpp`) ist veraltet, denn
   `EngineApi.cpp:2849` liest das Recht.
4. **Array-Pins sind aus Lua/Python kaputt** (Engine-Bug, als Warnung im Hive gemeldet).
   `luaReadValue`/`luaPushValue` und `pyReadValue`/`pyAppendValue` ignorieren
   `ApiParam::isArray`. Betroffen sind 14 Rows: `physics.overlap*`/`raycastAll`/`pollJointBroken`,
   `animator.notifiesOf`/`layerNames`, `fs.list`, `save.list`/`fields`,
   `scene.loadedZones`/`available` und `process.run`. Probe: `overlapSphere` gibt `0` statt einer
   Liste zurück, `save.list()` gibt `""` zurück.
5. **C++.** `cppCall` (`HE::api::…`) ist die engine-interne Funktion. Ein `GameLogic`-Modul linkt
   nicht gegen die Engine und erreicht nur die `he::`-Service-Helfer (save, entity, physics,
   input, content, anticheat, net).
6. **Fehlende Argumente** werfen in Lua einen Fehler (`luaL_check*`); einzige Ausnahme ist `bool`,
   das still zu `false` wird. In Python werfen sie `IndexError`, auch bei `bool`. Überzählige
   Argumente werden in beiden Sprachen ignoriert. Neun HorizonCode-Ereignisse (On Http
   Response, On File Changed, On Menu Item, On Tray Item, On Row Bind, On Window Closed, On
   Dismissed, On Selection Changed, On Right Clicked) haben kein Lua/Python-Gegenstück.

Werkzeug, das Schritt 2 hinzugefügt hat:

| Datei | Zweck |
|---|---|
| `scripts/script_api_docs/gen_reference.py` | baut `HorizonEngineDocs/scripting-reference.html` aus `registry.json` + `overlay/`, dazu die GEN-Blöcke in `horizoncode-nodes.html#engine-call`, `scripting-api.html#api` und `scripting.html#api`. Prüft bei jedem Lauf `flat.json` gegen `kHorizonFuncs` und `callbacks.json` gegen die Quelltexte; `--check` für CI |
| `scripts/script_api_docs/overlay/` | Handinhalt, der jeden Lauf überlebt: `sections/conventions.html`, `flat.json`, `callbacks.json`, `notes.json` (je Id: `note`, `perm`, `script_sig`, `examples`), `groups/<gruppe>.html` (Einleitung) |
| `coverage.py` | zählt jetzt die Referenzzeilen als **ref** und zeigt dazu **hand**, also Ids mit Handinhalt. Das ist die Kennzahl für die Schritte 3–8 |

Ablauf nach einer Registry-Änderung: `dump_engine_api.sh` → `gen_reference.py` →
`build_docs_index.py` (im Website-Checkout) → `scripts/build_docs_bundle.py` → `coverage.py`.

## Was dieser Schritt nicht getan hat

- Keine Website-Datei geändert, nichts deployt, Roadmap unverändert.
- Keine Beschreibung inhaltlich gegen das Verhalten geprüft. Die `doc`-Texte stammen aus
  `HcNodeDocs.cpp` und sind nur so richtig wie dort.
- Die Stufe „catalog" beruht auf einer Namenssuche in der Katalogtabelle (auch „get/set Position" zählt
  für `getPosition`/`setPosition`). Sie ist grob, spielt für die Lücken aber keine Rolle: eine
  Katalognennung ist keine Dokumentation.

## Nachtrag Schritt 10 (24.09.2026): Website-Hälfte nachgeliefert

Der Engine-Teil von Schritt 2 (5c70123d) war vollständig, im Website-Checkout lag davon aber
nichts mehr: `scripting-reference.html` fehlte, die drei GEN-Seiten standen auf dem Stand vom
08.09. Der Checkout war zurückgesetzt worden, die Hand-Edits von Schritt 2 an
`scripting-api.html` und `scripting.html` überlebten nur im eingecheckten `he-docs.json` und sind
von dort wiederhergestellt (Bündel danach byte-gleich).

- Website-Commit **2b9c276** im Checkout `Website` (lokal auf `main`, **nicht gepusht, nicht
  deployt**). Er enthält die Referenzseite, die GEN-Blöcke, die Hand-Edits und den neu gebauten
  `docs-index.json` (179 Abschnitte, 16 Seiten).
- `gen_reference.py` schreibt jetzt zusätzlich auf jede Doku-Seite einen Sidebar-Link „Engine
  API", die Blätterkette Scripting API → Engine API → HorizonCode Nodes und die Zählung im
  Hero/Meta von `horizoncode-nodes.html` (dort stand noch „20 groups, ~250 functions"). Der
  GEN-Text des Katalogs sagt nicht mehr „exactly the same engine surface", sondern nennt die
  vier Gruppen ohne `horizon.<gruppe>.*`-Tabelle.
- Geprüft: `gen_reference.py --check` sauber, alle internen Links und Anker der 16 Seiten
  lösen auf, `site_check` ohne Befund, `docs_search` findet die Callbacks, `coverage.py`
  582/582 **ref**, 24 **hand**.

Wird der Checkout erneut zurückgesetzt: `gen_reference.py` stellt Referenzseite, GEN-Blöcke und
Navigation wieder her; die Hand-Edits in `scripting-api.html` (#how-scripts-run, #api,
#behavior) und `scripting.html#api` stehen nur im Website-Commit.

## Nachtrag Schritt 3 (24.09.2026): Math, String, Random, Time, Timer, DateTime, Debug, Log

Handinhalt für 80 Ids: acht Gruppen-Einleitungen (`overlay/groups/`), Notes an 20 Ids und
sieben Beispiele in Lua und Python (`overlay/notes.json`). Jede Aussage über Sonderfälle ist mit
einer Wegwerf-Probe gegen `libHorizonScene` des Builds 7d49d44f gemessen worden, nicht aus der
Beschreibung übernommen. Die sieben Beispiele liefen aus `notes.json` extrahiert in beiden Sprachen
mit ausgelösten Callbacks (onStart/onUpdate/onTimer/onInputPressed/onCollisionEnter) und
lieferten die erwarteten Ergebnisse. `coverage.py`: 582/582 **ref**, **hand** 24 → 104.

Zwei Engine-Bugs, dokumentiert und im Hive gemeldet, nicht behoben:

- **`datetime.*` ist nur auf 128 s genau.** Die Epoch-Sekunden laufen als `Float` (32 Bit) durch
  die Rows (`EngineApi.cpp:6147`). Probe: `now()` = 1790248832 gegen `os.time()` = 1790248840,
  `second(os.time())` = 32 statt 40. Minute/Sekunde/`%S` sind bis zu 64 s falsch.
- **`debug.line/sphere/box` haben `Color`-Pins für Positionen** (`EngineApi.cpp:6513`), also
  vier Zahlen je Position: `line` braucht 13 Argumente. Die Referenz zeigt das per `script_sig`.

Sechs Beschreibungen in `HcNodeDocs.cpp` waren nachweislich falsch und sind an der Quelle korrigiert
(damit auch im Editor-Tooltip): `math.round` (Hälften weg von null, nicht „nach oben"),
`math.mod` (Vorzeichen des Dividenden, −1 mod 360 = −1), `string.length/substring` (Bytes, nicht
Zeichen), `string.toUpper/toLower` (nur A–Z), `random.seed` (ohne Seed startet jeder Start mit
derselben Folge) und `debug.line` (auch ein gepacktes Spiel zeichnet, `GameApplication.cpp:2886`;
„Shipping“ ist nur ein Pak-Profil). `registry.json` neu gedumpt, Diff genau diese acht `doc`-Felder.

Generator: Die Gruppenzeile behauptete `C++: HE::api::<gruppe>::<function>(ctx, …)`. Für
`string` heißt der Namespace `str`, und math/random/time/timer/debug nehmen keinen `Ctx`. Die Zeile
liest den Namespace jetzt aus den Rows und nennt keine Parameter mehr.

**Falle für die Folgeschritte:** `scripts/build_docs_bundle.py` ohne Pillow schreibt die acht
Figuren in `EditorDeps/Docs/img/` in voller Größe neu. Danach `git checkout -- EditorDeps/Docs/img/`;
`--check` bleibt sauber, es prüft nur `he-docs.json`.

## Nachtrag Schritt 4 (24.09.2026): Entity, Transform, Scene, Content, Save, File, Prefs, JSON

Handinhalt für 89 Ids: sieben neue Gruppen-Einleitungen und eine erweiterte (`fs`), Notes an
35 Ids, fünf Beispiele in Lua und Python (`boss_gate`, `checkpoint`, `tutorial_hint`,
`settings_file`, `cave_stream`). Grundlage war eine Wegwerf-Probe gegen `libHorizonScene` des Builds
7d49d44f mit echtem `ContentManager` (Temp-Ordner, ein StaticMesh, ein SaveGame-Template samt
Struct), Sandbox-Wurzel und rund 100 Aufrufen aus beiden Sprachen. Die fünf Beispiele liefen
wortgleich zu `notes.json` in beiden Sprachen mit ausgelösten Callbacks (onStart, onUpdate,
onBeginOverlap, onInputPressed); geprüft wurden die Wirkungen (Entities weg, Save-Datei mit
level 3/kills 2 und Neuladen, `Prefs.json`, Settings-Datei, Scene-Requests in der Queue).
`coverage.py`: 582/582 **ref**, **hand** 104 → 180 (alle 89 Ids dieses Schritts).

**Befund 1 (transform & Co.) im Generator gelöst:** Jede Zeile einer Gruppe ohne
`horizon.<gruppe>.*` nennt jetzt ihren Weg aus Lua/Python: „Lua/Python only as flat
`horizon.getPosition`" (verlinkt auf die Flach-Tabelle, gebaut aus den `twin`-Feldern in
`flat.json`) oder „**not reachable from Lua/Python**" (`transform.getWorldPosition`,
`setWorldPosition`). Das gilt auch für `material.*` und `cursor.set*` (Schritt 6/7 erben es).
Script-Rows mit flachem Zwilling nennen ihn zusätzlich („or flat `horizon.spawn`", 20 Rows).

Engine-Bugs, dokumentiert (Callout bzw. Note) und im Hive gemeldet, **nicht behoben**:

- **`entity.spawn` legt keine Transform an** (`ScriptApi.cpp:89` ruft nur `createEntity`, das
  Name, Hierarchy und EntityId setzt). `setPosition/Rotation/Scale` auf einem gespawnten Entity
  sind still wirkungslos, `getPosition` gibt 0,0,0 zurück, `distance` misst vom Ursprung. Gilt
  auch für das flache `horizon.spawn` und den HorizonCode-Knoten. Beschreibung in `HcNodeDocs.cpp`
  („It has a transform") und der Testkommentar `test_scripting_binding.cpp:804` sagen das
  Gegenteil; die Beschreibung ist bewusst **nicht** umgeschrieben, weil sie die Absicht nennt.
- **`scene.loadAdditive` hat einen `Color`-Pin für die Position** (`EngineApi.cpp:7006`), aus
  Lua/Python also vier Zahlen; mit drei wirft Lua *bad argument #6*, Python `IndexError`. Per
  `script_sig` gezeigt, wie bei `debug.line`.
- **`fs.modified` ist bis 64 s falsch**, dieselbe Ursache wie `datetime.*` (Thema 89):
  `Value::ofFloat((float)…)` (`EngineApi.cpp:6544`). Probe: 1790250880 gegen `os.time()` 1790250870,
  also sogar in der Zukunft.
- **Prefs lecken im Editor zwischen Projekten:** `prefs::doc()` liest `Prefs.json` einmal pro
  Prozess (`static bool loaded`, `EngineApi.cpp:4168`), ohne Reset beim Projektwechsel. Probe mit
  zwei Sandbox-Wurzeln: nach dem Wechsel liefert `getString` den Wert von Projekt A, und der
  nächste Set schreibt A's Schlüssel in B's `Prefs.json`.
- **`entity.distance` misst lokal** (`EngineApi.cpp:325`, `ScriptApi::getPosition` =
  `TransformComponent::position`): für ein Kind-Entity der Abstand zum Elternteil-Ursprung, nicht
  zur Weltposition. Aus dem Code gelesen, nicht per Probe (die gespawnten Probe-Entities hatten
  keine Transform, siehe oben). Richtig wäre `HE::worldPositionOf`.

Grenzen (kein Bug, `PinType::Float` ist 32 Bit), in Einleitung/Notes: `prefs`/`json`/`save`-Zahlen
und `fs.size` gehen durch float32 (0.1 → 0.10000000149011612, 16777217 → 16777216,
`json.setNumber` schreibt `3.0`); JSON-Setter sortieren die Schlüssel alphabetisch, Schlüssel mit
Punkt sind nicht adressierbar, `has` ist bei `null` wahr. Verhalten: `fs.rename` überschreibt (im
Gegensatz zu `copy`), `fs.remove` löscht keine Ordner, `fs.watch` pollt nur im gepackten Spiel
(`GameApplication.cpp:2760`) und meldet nur an das *On File Changed* der GameInstance;
`save.setNumber` schneidet bei Int-Feldern Richtung null ab; `save.setStruct` braucht
`horizon.structs.<Name>()`; `save.delete` auf den aktiven Save lässt ihn aktiv;
`entity.self/selfObject` sind aus Lua/Python immer 0; Scene-Requests laufen am Anfang des
nächsten Frames vor den Skripten (`GameApplication.cpp:2881` vor `:2906`), im Editor-Play
wirken nur Zonen, `load`/`activate` loggen nur.

## Nachtrag Schritt 5 (24.09.2026): Physics, Navigation, Movement, Locomotion, Player, Input

Handinhalt für 74 Ids: sechs Gruppen-Einleitungen (`overlay/groups/physics|movement|locomotion|nav|player|input.html`),
Notes an 33 Ids (davon sechs `script_returns` für die Treffer-Rückgabe der Casts) und fünf
Beispiele in Lua und Python (`player_move`, `push_crate`, `kill_plane`, `patrol`, `inventory`).
Grundlage war eine Wegwerf-Probe gegen `libHorizonScene` des Builds 7d49d44f mit echter
`PhysicsWorld` (Boden, dynamische Kiste, Charakter mit Character Controller + Movement,
Kind-Entity, Scharnierpaar), gebackenem NavMesh mit Agent, zwischen den Messetappen
getickt wie `SceneSystems` (Movement → Navigation → Physik), dazu Input- und
Player-Zustand so gepusht, wie die Apps es tun. Rund 110 Aufrufe aus Lua, die Kernfälle
wiederholt aus Python, beide Sprachen gleich. Die fünf Beispiele liefen wortgleich zu
`notes.json` in beiden Sprachen mit ausgelösten Callbacks (onStart, onUpdate,
onInputPressed); geprüft wurden die Wirkungen (Geschwindigkeit 8/5/0 m/s und Sprung,
Kiste fliegt bei Yaw 0 nach −Z und bei Yaw 90 nach −X, Rücksetzen von y = −25, drei
Patrouillenrunden, Moduswechsel). `coverage.py`: 582/582 **ref**, **hand** 180 → 254.

Sechs Beschreibungen in `HcNodeDocs.cpp` waren nachweislich falsch und sind an der Quelle
korrigiert (Editor-Tooltip und Referenz); `registry.json` neu gedumpt, Diff genau diese sechs
`doc`-Felder:

- `input.mouseButton`: 1 ist **rechts**, 2 die Mitte (`EngineApi.cpp:5181`, Test
  `test_engine_api.cpp:1314`), die Beschreibung sagte umgekehrt.
- `movement.forwardAmount` / `rightAmount`: Meter pro Sekunde, nicht −1…1 (Probe: seitwärts
  mit 5 m/s ergibt `rightAmount` 5; `test_movement.cpp:141` erwartet genau das).
- `locomotion.move`: nicht immer Weltraum, sondern „Move Direction Is" des Movement-Components
  (World, für einen PlayerCharacter Camera, `EntityHost.cpp:385`); Länge auf 1 gekappt, Y
  verworfen, zwei Aufrufe pro Frame addieren sich.
- `locomotion.look`: Pitch „vom Kamera-Rig verbraucht" stimmt nicht, `lookPitch` liest niemand
  (`MovementSystem.cpp` löscht ihn, kein Leser im Code).
- `nav.moveTo`: eine Ablehnung **stoppt** einen laufenden Agenten (Probe: isMoving/hasPath
  false), die Beschreibung sagte, er laufe weiter. Der Header sagte es richtig.

Engine-Befunde, dokumentiert (Callout bzw. Note) und im Hive gemeldet, **nicht behoben**:

- **`input.scrollDelta` ist im Spiel und im Play-Modus immer 0.** `pushSdlSnapshot`
  (`EngineApi.cpp:5184`) setzt das Rad fest auf 0, und sonst ruft niemand `setMouse` mit
  einem Radwert; `Input::mouse().wheel` gäbe es. Umweg in der Doku: Mouse Wheel an eine
  Axis-Action binden.
- **`locomotion.look` verwirft den Pitch** (siehe oben), dokumentiert mit Verweis auf
  `camera.addYawPitch`.
- Die Array-Rows (`physics.overlap*`, `raycastAll`, `pollJointBroken`) liefern aus Lua/Python
  weiter Skalar-Nullen (Befund aus Schritt 2, erneut gemessen). Neu: `pollJointBroken` leert
  die Queue dabei trotzdem, ein Skript nimmt die Ereignisse also einem HorizonCode-Graphen weg.

Verhalten, gemessen und in Einleitung/Notes: `raycast` & Casts geben aus Lua/Python zehn Werte
zurück, das flache `horizon.raycast` eine Tabelle ohne `hit`/`layer` oder `nil`; ein Strahl,
der in einem Collider startet, trifft ihn mit Abstand 0 (vom eigenen Charakter aus also den
Charakter selbst); bei Sphere/Box/Capsule-Cast ist `point` die Formmitte beim Stopp, nicht der
Kontakt (`PhysicsWorld.cpp:2927`), und sie ignorieren Trigger; Layer-Maske 0 sieht nichts, −1
alles; `setPosition`/`…AndReset` nehmen **lokale** Positionen (Kind unter (10,0,0): (0,5,0) →
Welt (10,5,0)), `…AtPosition` Weltpunkte; Kräfte scheitern auf kinematischen Körpern, also auch
auf Charakteren; `setVelocity` auf einem Charakter mit Movement wird im nächsten Frame
überschrieben; `setGravity` gilt nicht für Charaktere; nur der Joint-Besitzer meldet
`hasJoint`, Typ außerhalb 0–4 wird Fixed; `nav.moveTo` verweigert einen Punkt 4 m über dem
Mesh, `remainingDistance` ist nach der Ankunft −1, nie 0, `setSpeed(−2)` wird 0 bei weiter
`isMoving` = true; Player-Refs sind HorizonCode-Objektreferenzen (Ganzzahlen), die Tabelle
prüft nichts; Tastennamen sind SDL-Scancode-Namen und groß/klein-sensitiv („w" nie wahr,
„Left Shift"), Gamepad-Namen nicht („A" = „a", „south" gibt es nicht); Actions per Polling
aus `onUpdate` sind einen Frame alt (`GameApplication.cpp:2906` vor `:2984`, Editor
`:3240` vor `:3290`).
