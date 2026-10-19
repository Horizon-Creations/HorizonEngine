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
  Checks, `idForPath` und `loadAsset(projMatId)`, schlugen fehl).
- **Nicht verifiziert:** keine echte Mehr-Rechner-Sitzung (zwei laufende `HorizonEditor`-Prozesse);
  der Test reproduziert die ContentManager-Mechanik unit-test-artig, nicht über eine reale
  Collab-Session/MCP.

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
  (tests/test_collab_controller.cpp:~815, per-Tab-Reihenfolge von Hand nachgebaut)
- **Verifiziert:** Test einzeln grün (siehe unten); `HorizonEditor` baut mit dem Fix sauber durch
  (bestätigt, dass `EditorApplication.cpp` kompiliert — die Datei selbst ist NICHT in `he_tests`
  gelinkt).
- **Nicht verifiziert:** `EditorApplication.cpp` läuft nicht in `he_tests`; der Test bildet die
  Frame-Reihenfolge nach, ist aber kein Lauf der echten Klasse. Keine echte Mehr-Rechner-Sitzung.

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
- **Verifiziert:** alle drei Fälle einzeln grün (siehe unten).
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

- `tests/test_hc_codegen_export_build.cpp:181` — dokumentiertes Baseline-Rauschen (siehe
  `docs/handoff-template.md`-Vorläufer-Handoffs dieses Themas, Schritt 1–3).
- `tests/test_input_gamepad.cpp` (mehrere Fälle, Zeilen 725–896) — ein echtes Xbox-Gamepad ist an
  NN-WS03 angeschlossen; dokumentiert in der Hive-Lektion zu Vulkan/D3D-Baseline-Rauschen.
- `tests/test_physics_threading.cpp:305` — SEH-Crash, dokumentiertes Baseline-Rauschen.

Zusätzlich gezielt nur die drei geänderten Testdateien gelaufen lassen, zur Bestätigung ohne das
Rauschen der übrigen Suite:

```
tests\he_tests.exe --source-file=*test_project_exporter.cpp --source-file=*test_collab_controller.cpp --source-file=*test_terrain_sync.cpp
[doctest] test cases:  17 |  17 passed | 0 failed | 5623 skipped
[doctest] assertions: 162 | 162 passed | 0 failed |
```

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

## Handoff

- **Topic / step:** 209, Schritt 4 von 4
- **Branch / commit:** `claude/collab-bugs-materialien-horizoncode-wasser` @ `0df9ea73` (gepusht: ja,
  bereits vor diesem Schritt)
- **Done:** Alle drei Collab-Bugs (Engine-Material-Referenz, HorizonCode-Host→Client-Sync,
  Wasser dig-bed) sind mit je einem roten→grünen `he_tests`-Fall verifiziert; voller `he_tests`-Lauf
  zeigt nur dokumentiertes, themenfremdes Baseline-Rauschen; `HorizonEditor` und `he_tests` bauen
  sauber im Vordergrund; `editor_help_audit.py --check` ist grün.
- **Files touched (dieser Schritt):** nur `docs/collab-bugs-verifikation-2026-10-15.md` neu
  angelegt — keine Code-Änderungen in diesem Schritt.
- **Verified:** siehe Abschnitte oben (Vollbau, voller Testlauf, gezielter Testlauf der drei
  Testdateien, editor_help_audit.py, CI-Lauf gestartet).
- **Not verified:** keine echte Mehr-Rechner-Sitzung (zwei tatsächlich laufende
  `HorizonEditor`-Prozesse/MCP-Clients) für keinen der drei Fehler — alle drei Tests bilden Host/
  Client-Paare über den In-Test-Harness (`makeMachine`/`Net` bzw. Hand-Nachbau der
  Editor-Frame-Reihenfolge) nach, nicht über echte Prozesse. `EditorApplication.cpp` (Fehler 2)
  läuft nicht in `he_tests` selbst. Kein visueller D3D/Vulkan-Vergleich für den Wasser-Fix (laut
  Thema explizit ausgeschlossen). Der neue CI-Lauf für `0df9ea73` war bei Abschluss dieses Schritts
  noch nicht fertig.
- **Decisions taken:** frischen CI-Lauf für HEAD `0df9ea73` gestartet, weil der einzige vorhandene
  CI-Lauf nur bis zum vorletzten Commit reichte und den Wasser-Fix nie gesehen hatte.
- **Decisions suggested, not taken:** keine.
- **Pending:** nichts Code-seitiges für dieses Thema; die Queen kann nach grünem CI-Lauf auf
  `0df9ea73` mergen. Eine echte Mehr-Rechner-Verifikation (zwei physische/MCP-gesteuerte
  Editor-Instanzen) bliebe ein optionaler Nachschlag, ist aber laut Thema nicht Bedingung für
  "fertig" (das Thema verlangt zwei-Editoren-*Tests*, nicht zwei-Editoren-*Prozesse*).
- **Traps:** `he_tests` überschreibt die echte `%APPDATA%`-Config — immer `APPDATA` auf ein
  Scratch-Verzeichnis setzen (hier `C:\hw209\scratch_appdata`). `test_input_gamepad.cpp` schlägt auf
  NN-WS03 real fehl, weil dort ein echtes Xbox-Pad angeschlossen ist — kein neuer Flake.
  `EditorApplication.cpp` ist nicht in `he_tests` gelinkt; ein Fix dort braucht immer auch einen
  `HorizonEditor`-Build zur Bestätigung.
- **Next steps:** 1. CI-Lauf 39290146357 abwarten (beobachtet die Hive). 2. Bei Grün: Merge nach
  `release/0.7.0` durch die Queen. 3. Optional, falls gewünscht: eine echte Zwei-Prozess-Collab-Sitzung
  (zwei `HorizonEditor`-Instanzen über MCP) für mindestens einen der drei Fälle als zusätzliche
  manuelle Bestätigung.
