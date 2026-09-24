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

## Was dieser Schritt nicht getan hat

- Keine Website-Datei geändert, nichts deployt, Roadmap unverändert.
- Keine Beschreibung inhaltlich gegen das Verhalten geprüft. Die `doc`-Texte stammen aus
  `HcNodeDocs.cpp` und sind nur so richtig wie dort.
- Die Stufe „catalog" beruht auf einer Namenssuche in der Katalogtabelle (auch „get/set Position" zählt
  für `getPosition`/`setPosition`). Sie ist grob, spielt für die Lücken aber keine Rolle: eine
  Katalognennung ist keine Dokumentation.
