# Collab-Bugs Verifikation (2026-10-15)

Topic 209, step 4. Vollbau, voller `he_tests`-Lauf und Audit für die drei in diesem Thema
behobenen Collab-Fehler, auf Branch `claude/collab-bugs-materialien-horizoncode-wasser` @
`0df9ea73` (HEAD, enthält alle drei Fixes: 9b762406, 89935d6d, 0df9ea73).

## Fehler 1: Engine-Material-Referenz wird zur Laufzeit nicht aufgelöst

- **Ursache:** `ContentManager::loadAssetFromMemory()` (src/HE_Core/src/ContentManager/ContentManager.cpp)
  registrierte ein per Pak-UUID gestreamtes Asset unter seinem eingebetteten META-Pfad (z. B.
  bare `Default.hasset`) statt unter dem adressierten Pak-Pfad (`Engine/Default.hasset`). Ein
  gepacktes Spiel/ein Collab-Client streamt ein Engine-Material normalerweise zuerst per UUID
  (MaterialComponent trägt nie einen Pfad) — dabei konnte der bare Key ein gleichnamiges
  Projekt-Asset verdecken, und eine reine Pfad-Auflösung für den echten `Engine/…`-Pfad schlug fehl.
- **Fix:** `mountPak()` zeichnet jetzt zusätzlich die Rückrichtung (UUID → adressierter Pfad) aus
  den `__asset_index__`-Einträgen auf; `loadAssetFromMemory()` bevorzugt diese beim Registrieren.
- **Testname:** `"Streaming an engine material by UUID first must not hijack a project material
  of the same name"` (tests/test_project_exporter.cpp:707)
- **Verifiziert:** Test einzeln grün (siehe unten); war laut Commit-Message rot vor dem Fix (beide
  Checks, `idForPath` und `loadAsset(projMatId)`, schlugen fehl) — selbst nicht reproduziert (kein
  Revert/Re-Test auf dem alten Stand in diesem Schritt, nur die Commit-Message als Quelle für "rot
  davor").
- **Nicht verifiziert:** **Dieser Test hat KEINEN zweiten Host/Client — das Thema verlangt "alle
  drei Szenarien mit zwei Editoren in einem Test grün", diese Bedingung ist für Fehler 1 damit NICHT
  erfüllt.** Der Test (gelesen, Zeilen 707–784) verwendet EINEN einzigen `ContentManager` (`cm`), der
  zuerst per `ensureResident(engineMatId)` ein per Pak-UUID gestreamtes Asset lädt und dann per
  `idForPath`/`loadAsset` auflöst — ein reiner ContentManager-/Pak-Mounting-Unit-Test ohne
  `CollabSession`, `CollabController` oder zweite Maschine. Keine echte Mehr-Rechner-Sitzung (zwei
  laufende `HorizonEditor`-Prozesse) wurde getestet.

## Fehler 2: Host-Änderung an HorizonCode-Graphen erreicht Clients nicht

- **Ursache:** `EditorApplication::updateAssetCollabSync` (src/HE_Editor/EditorApplication.cpp)
  diffte einen Content-Asset-Tab, beanspruchte dann bei Dirty das Lock, und ließ im selben Frame
  das Collab-Autosave den Dirty-Flag löschen — weil `CollabSession::requestLock` nur auf dem HOST
  sich selbst ohne Round-Trip gewährt (src/HE_Net/src/CollabSession.cpp:2548-2565). Der nächste
  Frame sah dann `owned==true` und `dirty==false` zugleich und überspeicherte den Diff: die Änderung,
  die das Lock erst ausgelöst hatte, ging nie als Delta raus. Die Client→Host-Richtung war davon
  nicht betroffen (ein Client sieht `owned==true` immer erst einen Round-Trip später, mit `dirty`
  noch gesetzt).
- **Fix:** Ein zweiter Diff direkt nach dem Claim, genau in dem Frame, in dem das Lock von
  "nicht gehalten" auf "gehalten" wechselt — vor dem Autosave.
- **Testname:** `"Live sync: the host's edit on a freshly claimed lock reaches the client"`
  (tests/test_collab_controller.cpp:782)
- **Verifiziert:** echtes Host/Client-Paar bestätigt (gelesen, Zeilen 782–830): ein `CollabController
  host` ruft `startHosting`, ein zweiter `CollabController client` ruft `joinSession("127.0.0.1",
  host.port(), …)` — eine reale `CollabSession`-Verbindung über echtes TCP auf localhost, kein
  reiner In-Memory-Doppelgänger. Test einzeln grün (siehe unten); `HorizonEditor` baut mit dem Fix
  sauber durch (bestätigt, dass `EditorApplication.cpp` kompiliert — die Datei selbst ist NICHT in
  `he_tests` gelinkt). War laut Commit-Message rot vor dem Fix — selbst nicht reproduziert (nur die
  Commit-Message als Quelle für "rot davor").
- **Nicht verifiziert:** `EditorApplication.cpp` läuft nicht in `he_tests`; der Test bildet
  `updateAssetCollabSync`s Pro-Tab-Frame-Reihenfolge von Hand nach (echte Host/Client-Session, aber
  kein Lauf der echten `EditorApplication`-Klasse). Keine echte Mehr-Rechner-Sitzung (zwei laufende
  `HorizonEditor`-Prozesse).

## Fehler 3: Wasser-Werkzeuge setzen dig bed nach Landscape-Edit zurück

- **Ursache:** `HE::water::lake::syncSplines` (src/HE_Scene/src/TerrainSystem.cpp:535) läuft pro
  Frame auf JEDER Maschine ungeschützt gegen Ownership — die Transform-Bewegung eines Peers (über
  den normalen, schnellen Transform-Kanal repliziert) sieht lokal wie eine eigene Bewegung aus. Bei
  einem Lake mit `digsBed == true` gräbt deshalb jede Maschine lokal neu (hebt ihren eigenen,
  ggf. veralteten `groundPatch` an und gräbt neu). Der `TerrainSync`-Settings-Patch mit dem frischen
  `groundPatch`/Polygon war auf 200 ms gedrosselt, langsamer als der Heights-Patch (33 ms), der das
  bereits gegrabene Terrain enthielt — ein Lift lief dadurch gegen bereits verändertes Terrain und
  hinterließ ein zu tiefes Loch, das nicht zum umliegenden Geländeniveau passte.
- **Fix:** `TerrainSync` (src/HE_Editor/TerrainSync.cpp/.h) hasht jetzt das eigene Grabungsbild eines
  Lake-Körpers (Polygon, groundPatch-Rect, Tiefe, Böschung) und schickt dessen Settings-Patch außer
  der Reihe, sobald sich der Hash ändert — dasselbe Muster wie bei einer geänderten
  Foliage-Layer-Anzahl (TerrainSync.cpp ~Zeile 462).
- **Testname:** `"Landscape sync: a client's own syncSplines must not re-dig a lake's bed from a
  stale patch"` (tests/test_terrain_sync.cpp, rot vor dem Fix laut Commit-Message: ein Oracle
  konvergiert zu falschen, zu tiefen Höhen; grün danach). Zwei begleitende Fälle
  (`"a lake's dig bed survives a landscape edit elsewhere"`, `"a settings patch on raised ground
  does not move the ground reference"`) decken die einfacheren, bereits korrekten Pfade ab.
- **Verifiziert:** alle drei Fälle einzeln grün (siehe unten); war laut Commit-Message rot vor dem
  Fix (ein Oracle konvergiert zu falschen, zu tiefen Höhen) — selbst nicht reproduziert (nur die
  Commit-Message als Quelle für "rot davor"). `host`/`client` laufen über `makeMachine()` +
  `Net({...})` (test_terrain_sync.cpp), ein In-Test-Harness mit zwei vollständigen
  `TerrainComponent`-Zuständen, aber ohne reale `CollabSession`/TCP — ein zweiseitiger Test, aber
  über ein leichteres Doppel als Fehler 2's echte Session.
- **Nicht verifiziert:** keine echte Mehr-Rechner-Sitzung; kein visueller Vergleich im laufenden
  Editor (kein D3D/Vulkan-Capture für dieses Thema vorgesehen — explizit ausgeschlossen).

## Vollbau

- `he_tests` (scoped Debug build, `C:\hw209`, konfiguriert gegen diesen Worktree-Pfad): im
  Vordergrund gebaut, `ninja: no work to do` — der Build war bereits auf dem sauberen HEAD
  `0df9ea73` aktuell.
- `HorizonEditor`: im Vordergrund gebaut, sauberer Relink + Deploy (83 Targets, u. a.
  `EditorApplication.cpp`, `TerrainSync.cpp`), keine Fehler.

## Voller `he_tests`-Lauf

Kommando: `tests\he_tests.exe` mit `APPDATA=C:\hw209\scratch_appdata` (vermeidet Überschreiben der
echten Editor-Config), scoped Debug build.

```
[doctest] test cases:   3498 |   3493 passed |  5 failed | 2142 skipped
[doctest] assertions: 628111 | 628099 passed | 12 failed |
```

Die 5 roten Fälle, keiner davon berührt Collab-/Material-/Wasser-Code:

- `tests/test_hc_codegen_export_build.cpp:181` — dokumentiertes Baseline-Rauschen; bereits in den
  hive_post-Ergebnissen von Schritt 1 (9b762406) und Schritt 3 (0df9ea73) dieses Themas als
  vorbestehend genannt, unabhängig vom hier gebauten Code.
- `tests/test_input_gamepad.cpp` (3 Testfälle, Zeilen 725–896) — ein echtes Xbox-Gamepad ist an
  NN-WS03 angeschlossen (dokumentiert in der Auto-Memory-Notiz `vulkan-d3d-baseline-noise.md`, nicht
  in einer Hive-Lektion). **Umgebungsabhängig, nicht deterministisch:** Schritt 3's eigener Lauf
  eine Stunde zuvor (`full_run2.log`, selbes Binary — `ninja: no work to do` auf demselben HEAD
  `0df9ea73` bestätigt das) zeigte NULL Gamepad-Fehler (nur 2 rote Fälle insgesamt: hc_codegen +
  physics_threading). Derselbe Build zeigte in diesem Schritt 3 rote Gamepad-Testfälle. Die
  Codebasis war zwischen beiden Läufen identisch — der Unterschied kommt vom angeschlossenen
  Controller (Reconnect/Zustandswechsel), nicht vom Code.
- `tests/test_physics_threading.cpp:305` — SEH-Crash, dokumentiertes Baseline-Rauschen (gleiche
  Quelle wie oben).

Zusätzlich gezielt nur die drei geänderten Testdateien gelaufen lassen, zur Bestätigung ohne das
Rauschen der übrigen Suite:

```
tests\he_tests.exe --source-file=*test_project_exporter.cpp --source-file=*test_collab_controller.cpp --source-file=*test_terrain_sync.cpp
[doctest] test cases:  17 |  17 passed | 0 failed | 5623 skipped
[doctest] assertions: 162 | 162 passed | 0 failed |
```

## Gezielter Collab- und Wasser-Testlauf

Explizit die Collab- und Wasser-/Lake-Testdateien gelaufen lassen (Dateinamen einzeln als
`--source-file`, nicht als Substring-Wildcard — der Worktree-Ordner selbst heißt
`claude-collab-bugs-materialien-horizonco` und enthält damit das Wort "collab"; ein
`--source-file=*collab*`-Wildcard matcht über den vollen Pfad fälschlich die GESAMTE Suite):

```
test_collab_docsync.cpp, test_collab_settle.cpp, test_collab_undo.cpp,
test_mcp_tools_collab.cpp, test_net_collab.cpp, test_collab_controller.cpp,
test_terrain_sync.cpp, test_water_lake.cpp, test_water_lake_api.cpp,
test_water_brush.cpp, test_water_field.cpp, test_project_exporter.cpp

[doctest] test cases:  41 |  41 passed | 0 failed | 5599 skipped
[doctest] assertions: 423 | 423 passed | 0 failed |
```

Nicht real gegen einen externen Server gelaufen (brauchen eine laufende `collab_server`-Instanz,
per Env-Var angebunden, sonst früh-returnend grün ohne die eigentliche Leitung zu prüfen):

- `test_mcp_tools_collab.cpp:230` "collab tools: host and join by session id through a real relay"
  (HE_TEST_RELAY)
- `test_net_relay.cpp:825` "Relay: against a real collab_server" (HE_TEST_RELAY)
- `test_net_relay.cpp:1291` "Relay fan-out: against a real collab_server" (HE_TEST_RELAY)
- `test_net_team.cpp:787/891/972` "Team: two editors / saved session tokens / profiles through a
  real collab_server" (HE_TEST_TEAM_SERVER)

Daneben zwei allgemeine (nicht collab-spezifische) Netz-Stack-Fälle, die auf NN-WS03 wegen
`HE_NET_LOOPBACK_ONLY` nur den Loopback-Pfad prüfen, nicht den dual-stack-Pfad: `test_net_discovery.cpp:941`,
`test_net_tcp.cpp:166/204`.

## `editor_help_audit.py --check`

```
TOTAL               1217     1217      0
help entries: 1969
```

Grün, 0 offene Controls.

## CI

- Vorheriger CI-Lauf (https://github.com/Horizon-Creations/HorizonEngine/actions/runs/39280784462,
  success) deckte nur bis Commit `89935d6d` (Schritt 2) ab — **nicht** den Wasser-Fix `0df9ea73`
  (Schritt 3), da dieser erst nach Start dieses Laufs gepusht wurde.
- Neuer Lauf für den aktuellen HEAD `0df9ea73` gestartet:
  https://github.com/Horizon-Creations/HorizonEngine/actions/runs/39290146357 (lief bei Abschluss
  dieses Schritts noch; die Hive beobachtet den neuesten Lauf dieses Themas und meldet grün/rot
  an die Queen).
- Lauf 39290146357 (`0df9ea73`): alle vier Jobs success, macOS eingeschlossen.
- Lauf https://github.com/Horizon-Creations/HorizonEngine/actions/runs/39302370729 (`dfd3fbed`):
  macOS rot in `tests/test_physics_budget.cpp:529f` (`charactersDeferred` 32 statt 0, `charactersStepped`
  32 statt 64). **Verdikt: vorbestehender Timing-Flake, keine Regression dieses Themas.**
  - `dfd3fbed` unterscheidet sich von `0df9ea73` nur um diese Doku-Datei (`git diff 0df9ea73 dfd3fbed
    --stat`); derselbe Code lief in 39290146357 auf macOS grün.
  - Die eigenen Commits des Themas (`git diff $(git merge-base release/0.7.0 HEAD)..HEAD --stat`)
    berühren `ContentManager`, `EditorApplication`, `TerrainSync` und Tests, nie `PhysicsWorld.cpp`,
    `PhysicsBudget.h` oder `test_physics_budget.cpp`.
  - Früherer Treffer auf der Basis: `release/0.7.0` Lauf 38909214550 (`7090180f`, Vorfahre von HEAD,
    PR-Lauf), macOS rot an `test_physics_budget.cpp:496f` (die Kontrollgruppe desselben Testfalls:
    gleiche zwei CHECKs auf `charactersDeferred`/`charactersStepped`). In den 21 jüngsten roten
    CI-Läufen anderer Zweige kommt der Test sonst nie vor (Suche nach `test_physics_budget.*Failed`).
  - Ursache: `PhysicsWorld.cpp` ~3544 stellt einen Charakter zurück, sobald `chargedMs + msSince(
    charactersFrom) >= budgetMs` (10 ms im Test) und schon `kGuaranteedCharacters` = 32 gelaufen sind.
    Der Test ist wanduhrabhängig; ein >10-ms-Stall des macOS-Runners mitten im Schritt (32 schaffen es,
    die übrigen 32 werden zurückgestellt) reicht, kein Code nötig. Zahlen passen: 32/32.
  - Keine Änderung am Test oder am Topic-Code. Nicht lokal wiederholt (kein Build-Ordner im Worktree;
    ein Vollbau von `he_tests` stand in keinem Verhältnis zu einem ohnehin eindeutigen Befund).

## Handoff

- **Topic / step:** 209, Schritt 4 von 4
- **Branch / commit:** `claude/collab-bugs-materialien-horizoncode-wasser` @ `0df9ea73` (gepusht: ja,
  bereits vor diesem Schritt)
- **Done:** Alle drei Collab-Bugs (Engine-Material-Referenz, HorizonCode-Host→Client-Sync,
  Wasser dig-bed) sind mit je einem (laut Commit-Message) roten→grünen `he_tests`-Fall gefixt und
  jetzt grün; voller `he_tests`-Lauf zeigt nur dokumentiertes, themenfremdes Baseline-Rauschen;
  `HorizonEditor` und `he_tests` bauen sauber im Vordergrund; `editor_help_audit.py --check` ist
  grün. **ABER:** die Thema-Bedingung "alle drei Szenarien mit zwei Editoren in einem Test grün" ist
  nur für Fehler 2 und 3 erfüllt — Fehler 1's Test hat keinen zweiten Host/Client (siehe oben, Fehler
  1 → Nicht verifiziert). Das Thema ist damit nach dem eigenen Fertig-Kriterium noch NICHT
  vollständig abgeschlossen.
- **Files touched (dieser Schritt):** nur `docs/collab-bugs-verifikation-2026-10-15.md` neu
  angelegt — keine Code-Änderungen in diesem Schritt.
- **Verified:** siehe Abschnitte oben (Vollbau, voller Testlauf, gezielter Testlauf der drei
  Testdateien, gezielter Collab+Wasser-Testlauf über 12 Testdateien, editor_help_audit.py,
  CI-Lauf gestartet).
- **Not verified:**
  - **Fehler 1 hat keinen Zwei-Editoren-Test** (siehe oben) — die Thema-Bedingung ist dafür nicht
    erfüllt. Das muss noch nachgeholt werden (neuer Test mit zwei `ContentManager`/Sessions, oder
    die Queen entscheidet, dass der bestehende Unit-Test für diesen Fehler ausreicht).
  - Keine echte Mehr-Rechner-Sitzung (zwei tatsächlich laufende `HorizonEditor`-Prozesse/
    MCP-Clients) für keinen der drei Fehler. `EditorApplication.cpp` (Fehler 2) läuft nicht in
    `he_tests` selbst.
  - "Rot vor dem Fix" wurde für keinen der drei Fehler in diesem Schritt selbst nachvollzogen
    (kein Revert + Re-Test) — die Aussage stützt sich auf die jeweilige Commit-Message.
  - Die HE_TEST_RELAY-/HE_TEST_TEAM_SERVER-gated Collab-Fälle (6 Testfälle, siehe oben) laufen ohne
    reale `collab_server`-Instanz nur ihren früh-returnenden Zweig, nicht die eigentliche Leitung.
  - Kein visueller D3D/Vulkan-Vergleich für den Wasser-Fix (laut Thema explizit ausgeschlossen).
  - Der neue CI-Lauf für `0df9ea73` war bei Abschluss dieses Schritts noch nicht fertig.
- **Decisions taken:** frischen CI-Lauf für HEAD `0df9ea73` gestartet, weil der einzige vorhandene
  CI-Lauf nur bis zum vorletzten Commit reichte und den Wasser-Fix nie gesehen hatte.
- **Decisions suggested, not taken:** keine.
- **Pending:** Fehler 1 braucht noch einen Test mit einem zweiten Host/Client (oder eine
  Entscheidung der Queen, dass der bestehende Single-ContentManager-Test als Nachweis reicht), bevor
  das Thema nach seinem eigenen Fertig-Kriterium geschlossen werden kann. Ansonsten nichts
  Code-seitiges; nach grünem CI-Lauf auf `0df9ea73` und Klärung von Fehler 1 kann die Queen mergen.
  Eine echte Mehr-Rechner-Verifikation (zwei physische/MCP-gesteuerte Editor-Instanzen) bliebe ein
  optionaler Nachschlag, ist aber laut Thema nicht Bedingung für "fertig" (das Thema verlangt
  zwei-Editoren-*Tests*, nicht zwei-Editoren-*Prozesse*).
- **Traps:** `he_tests` überschreibt die echte `%APPDATA%`-Config — immer `APPDATA` auf ein
  Scratch-Verzeichnis setzen (hier `C:\hw209\scratch_appdata`). `test_input_gamepad.cpp` schlägt auf
  NN-WS03 real fehl, weil dort ein echtes Xbox-Pad angeschlossen ist — kein neuer Flake.
  `EditorApplication.cpp` ist nicht in `he_tests` gelinkt; ein Fix dort braucht immer auch einen
  `HorizonEditor`-Build zur Bestätigung.
- **Next steps:** 1. Fehler 1 (`tests/test_project_exporter.cpp:707`): einen Zwei-Editoren-Test
  ergänzen (z. B. zwei `ContentManager`/`CollabSession` wie in `test_collab_controller.cpp:782`,
  die beide dasselbe gepackte Asset mounten/referenzieren) — oder die Queen entscheidet bewusst,
  dass der bestehende Unit-Test ausreicht, und das Thema wird trotzdem geschlossen. 2. CI-Lauf
  39290146357 abwarten (beobachtet die Hive). 3. Bei Grün und geklärtem Punkt 1: Merge nach
  `release/0.7.0` durch die Queen. 4. Optional: eine echte Zwei-Prozess-Collab-Sitzung (zwei
  `HorizonEditor`-Instanzen über MCP) für mindestens einen der drei Fälle als zusätzliche manuelle
  Bestätigung.
