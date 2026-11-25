# Projekt schließen beendet, was es laufen ließ; ein Pin-Edit spielt keinen Sound (25.11.2026)

Zwei Editor-Fehler, eine gemeinsame Ursache: die Live-Vorschau eines App-Projekts
(GameInstance-Runtime, ihre Widgets, Timer und Sounds) hatte kein „Ende".

## Offene Entscheidung (User)

Bug 2 ist eine Verhaltensfrage, die Startsitzung hat nicht geantwortet; umgesetzt ist die
Vorgabe: Nach jeder Änderung (auch jedem Speichern eines Widgets/einer Klasse) ist die
App-Vorschau **still**, bis man „Restart Live Preview" drückt oder die Vorschau selbst etwas
abspielt. Alternative: Musik läuft über Edit-Neustarts weiter, nur neue Starts werden unterdrückt.

## Ursachen (Zeilen: Stand vor dem Fix, `639beb3f4`)

1. **Close Project / Projektwechsel ließ die App-Vorschau weiterlaufen.**
   `EditorUI::endProjectSession` (`src/HE_Editor/EditorUI.cpp:1269`, der Trichter für Close
   `:2595` und jeden Wechsel) räumte Tabs, Collab und Welt ab, aber nichts jenseits der Welt:
   kein `fireShutdown`, kein `timer::cancelAll`, kein `stopAll` (das gab es nur beim
   Play-Stopp, `EditorApplication.cpp:16443`). Der Loop-Sound aus OnInit lief im Hub weiter.
   `m_appUiStartedFor` (`EditorApplication.cpp:3317`) blieb auf dem Pfad stehen, also startete
   dasselbe App-Projekt beim Wiederöffnen seine Vorschau nie. Außerdem rief nur ein Wechsel
   `GitController::openProject` (`EditorApplication.cpp:2298`, schließt das alte Repo); Close
   Project rief `m_git.closeProject()` nie, der Git-Worker pollte das alte Repo hinter dem Hub.
2. **Sound am Play-Sound-Pin setzen spielte ihn ab.** Jeder Commit des GameInstance-
   Graphen setzt `m_appPreviewRestartPending` (`commitGameInstance`, `EditorApplication.cpp:15364`;
   MCP-Pfad gleich), und `restartAppPreview` (`:16450`) führte OnInit erneut aus. Play Sound in OnInit (Tamagochi:
   OnInit → Create/Show Widget → `audio.play`, Loop) startete den Clip, und weil der
   Neustart die alten Sounds nicht stoppte, stapelten sie sich.

## Fix (Commit auf `release/0.8.0`)

* `EditorApplication::stopAppPreview()`: die Abbauhälfte von `restartAppPreview`,
  jetzt inklusive `WeatherAudio::stop` + `m_audioEngine.stopAll()`.
* `EditorApplication::endProjectRuntime()`: Vorschau stoppen (nur wenn gestartet),
  `m_appUiStartedFor` leeren, Timer, Sounds, GameInstance-Graph leeren, `m_git.closeProject()`.
  Beendet zuerst Play. Aufgerufen über `AppContext::endProjectRuntime` in `endProjectSession`
  VOR `newScene`, damit OnShutdown die Widgets noch findet (die Welt besitzt sie), und
  noch einmal (dann No-op) oben im `onProjectLoaded`-Callback: File ▸ New Project über einem
  offenen Projekt und das Tutorial laden, ohne `endProjectSession` zu durchlaufen.
* `uiLive` und der Vorschau-Start in `OnRender` verlangen `m_projectLoaded`: ein
  fehlgeschlagener Wechsel lässt das alte Projekt im ProjectManager stehen.
* `HE::api::audio::setPlaybackHeld` (`EngineApi.h/.cpp`): solange gehalten, starten
  `play`/`playAt` nichts. `restartAppPreview(keepState=true)` (jeder Edit-Neustart)
  hält es um `fireInit`. „Restart Live Preview" (keepState=false) und alles, was die
  laufende Vorschau später tut (Button-Klick), spielt normal. Folge: nach einem Edit
  ist die Vorschau still, bis man sie neu startet. **Grenze:** gehalten wird nur, was
  OnInit synchron startet; ein Play Sound hinter Delay/Timer in OnInit spielt weiter
  bei jedem Edit (`shortcut:`-Kommentar in `restartAppPreview`).

## Verifiziert

* `he_tests --source-file=*test_engine_api.cpp`: 132/132, neuer Fall „a held playback gate
  starts nothing" (echter Clip, also kann die 0 nur vom Tor kommen). Audio-Fälle 148/148.
* `editor_help_audit.py --check` grün (keine neuen Bedienelemente).
* Live: `scripts/he_project_close_live.py ~/HorizonEngineProjects/Tamagochi/Tamagochi.heproj`
  (arbeitet auf einer Kopie; 4× PASS, auch „OnShutdown vor dem Leeren der Welt"). Neue
  Schalter `HE_CLOSE_PROJECT_AT_FRAME` / `HE_REOPEN_PROJECT_AT_FRAME` (`docs/headless-runs.md`,
  hinter `HE_FEATURE_TEST_HOOKS`). Zeuge = `Audio engine shutting down (N active voice(s))`
  + `Started ... sound`. Gegenprobe mit ausgeschaltetem Fix (einmal von Hand gebaut):

  | Lauf | ohne Fix | mit Fix |
  |---|---|---|
  | Close bei f200, Ende f400 | 1 Stimme | 0 |
  | Close f200, Reopen f300 | Vorschau startet nicht wieder | startet, 1 Stimme |
  | MCP `hc_set_pin_default` (leeren, Clip) | Sound #2 startet, 2 Stimmen | kein Start, 0 |

## Nicht verifiziert

* Git-Worker nach Close nur aus dem Code (Tamagochi ist kein Repo); kein Log im Worker.
* Spielprojekte live nicht gefahren (dort läuft im Edit-Modus kein GameInstance).
* New-Project/Tutorial-Pfad (Teardown im `onProjectLoaded`-Callback) nicht live gefahren.
* Nicht angefasst: Python-Host, JobSystem/Asset-Streaming in Flug, Panel-Statics.

## Handoff

Nächste Schritte: (1) Tamagochi `GameInstance.hcode` speichert `pitch` 0.0 am Play-Sound-Knoten
(vor dem Default-Fix gespeichert); `Started ... pitch 0.00` im Log. Klären, ob Pitch 0 stumm ist
und alte Graphen migriert werden sollen. (2) Falls gewünscht: Musik über Edit-Neustarts
weiterlaufen lassen statt still (Variante „Zustand erhalten"), dann `stopAll` im Edit-Neustart
durch ein Behalten der Vorschau-Stimmen ersetzen.
