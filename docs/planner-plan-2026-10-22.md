# Horizon Planner: Projektplanung im Notion-Stil (Entwurf)

Status 2026-10-22: Plan und Mockup (`docs/planner-mockup-2026-10-22.html`), nichts implementiert.

## Ziel
Eine Planungs-App fuer das Spiel: Datenbanken mit mehreren Ansichten (Tabelle, Board,
Zeitleiste, Kalender), Seiten mit Bloecken (GDD, Level-Konzepte, Playtest-Notizen),
Relationen zwischen allem, und der Vorteil gegenueber Notion: Eintraege zeigen auf echte
Engine-Objekte (Asset, Entity, Szene, Panel) und springen dorthin.

## Was schon da ist
`src/HE_Editor/TaskBoard.{h,cpp}` (915 Zeilen): git-freundliche Datei `Config/TaskBoard.json`
(UUIDs, ISO-Zeiten, verlustfrei, unbekannte Keys bleiben), `TaskLink` (Asset/Entity/Scene/Panel)
mit Orphan-Pruefung, Updates/Kommentare, Zuweisung. Dazu Panel, Footer, MCP-Tools und
Team-Benachrichtigungen (`docs/taskboard-team-interop-2026-10-15.md`). Das ist praktisch
schon eine Datenbank mit einem festen Schema.

## Architektur (Empfehlung)
1. **Datenmodell `HE::Planner`** (ImGui-frei, wie TaskBoard): Datenbank = Schema (Eigenschaften:
   Text, Auswahl, Mehrfachauswahl, Zahl, Datum, Person, Relation, Engine-Link, Checkbox) +
   Zeilen + gespeicherte Ansichten (Filter, Sortierung, Gruppierung, Spalten). Seite = Liste von
   Bloecken. Gleiche Regeln wie TaskBoard: UUIDs, verlustfrei, LF, Unbekanntes behalten.
2. **Ablage** `<Projekt>/Config/Planner/<db-uuid>.json` und `Pages/<uuid>.json`, eine Datei je
   Datenbank/Seite (weniger Merge-Konflikte als eine grosse Datei), reist mit Git/Team.
3. **Task Board wird eine Datenbank** (Migration: bestehende `TaskBoard.json` lesen, nicht
   loeschen; Footer, MCP, Team-Notices bleiben zunaechst am alten Modell, danach Adapter).
4. **UI als `PlannerPanel` in einem eigenen Voll-Tab** (Entscheidung 2026-10-22: Eigenbau im Editor,
   ersetzt das Task Board, bekommt den ganzen Tab-Platz statt eines andockbaren Fensters).
   Der Mechanismus existiert: virtuelle Tabs (`::Preferences::`, `::LevelScript::`, `::GameInstance::`).
   Neu: `PlannerPanel::kTabPath = "::Planner::"`, geoeffnet per `openVirtualTab("Planner", ...)`.
   Anzufassen (Muster von `GameInstancePanel`/`EditorSettingsPanel`, nicht Content Browser):
   - `EditorUI.cpp` ~2445 (Tab-Gating: virtueller Pfad zeichnet statt Viewport/Panels, early return),
     ~4262 (Dispatch zum Panel), ~4516 (Vollbild-Tab), Menue `Window > Planner` neben ~3131.
   - `EditorTabs`: virtuelle Pfade sind schon eigene Tabs ohne Datei, nichts zu aendern.
   - Tab-Wiederherstellung der Sitzung (Stelle suchen, wo `kTabPath`s gespeichert werden).
   - Kollaboration: eigenen Sync-Key `::Planner::` nur, wenn Live-Sync gewollt (P4+), vorher Git.
   Innen: links Seitenbaum/Datenbanken, Mitte Inhalt, rechts optional Eigenschaften einer Zeile.
   Zeilen-Clipping wie Outliner, Bloecke als ImGui-Editor (Slash-Menue).
   Hilfe-Eintraege in `EditorHelp.cpp` nicht vergessen (`editor_help_audit.py --check`).
4a. **Task Board ersetzen, nicht doppelt halten:** P1 liest `TaskBoard.json` in die Datenbank
   "Tasks" (Datei bleibt bis zur Abnahme unangetastet). Danach: `TaskBoardPanel`-Fenster entfaellt,
   `Window > Task Board` oeffnet den Planner-Tab auf der Tasks-Ansicht; Chip-Spruenge
   (`TaskBoardPanel::takeJump`, EditorUI ~2780) wandern in den Planner; Footer-Zaehler
   (`TaskBoardFooter`) liest die Tasks-Datenbank; `McpToolsTaskBoard` und Team-Notices
   (`TaskBoardTeam`) bekommen einen Adapter auf das neue Modell. Reihenfolge siehe Phasen.
5. **Spaeter eigenstaendig**: dieselbe Kernbibliothek in einer HE App (weltloser Modus, siehe
   he-apps-plan), damit Designer ohne den ganzen Editor planen koennen.
6. **MCP-Tools** (wie `McpToolsTaskBoard.cpp`): Claude/Hive kann Eintraege lesen und anlegen.

## Entschieden
- Eigenbau im Editor als Voll-Tab, ersetzt das Task Board (Connor, 2026-10-22).

## Noch offen
- Echtzeit-Zusammenarbeit: Git-Merge reicht anfangs (wie TaskBoard), Live-Sync ueber Team-Server spaeter.

## Phasen
| Phase | Inhalt | Ergebnis |
|---|---|---|
| P0 | Datenmodell + Datei + Tests (Round-trip, alte Dateien laden) | `Planner.{h,cpp}` |
| P1 | Planner-Tab (`::Planner::`), Tabellenansicht, Eigenschaftstypen, Filter/Sortierung, Task-Board-Import | nutzbar fuer Features/Bugs |
| P1b | Task Board abloesen: Menue, Chip-Spruenge, Footer, MCP-Adapter; altes Fenster entfernen | ein System statt zwei |
| P2 | Board- und Zeitleisten-Ansicht, Meilensteine, Relationen | Roadmap im Editor |
| P3 | Seiten mit Bloecken, eingebettete Datenbank-Ansichten, Engine-Objekt-Bloecke (Szenenvorschau) | GDD im Editor |
| P4 | Kalender, Vorlagen (Feature, Bug, Playtest), Team-Notices, MCP-Tools | Team-tauglich |
| P5 | Standalone als HE App, Export Markdown/CSV | optional |

## Risiken
Block-Editor in ImGui ist der aufwendigste Teil (Cursor, Auswahl, Undo): zuerst nur Absatz,
Ueberschrift, Liste, Checkliste, Engine-Link. Zeitleiste braucht eigenes Zeichnen (kein Widget).

## Handoff (Stand P1b, 2026-10-22)
**Entschieden (Variante A):** Die Aufgaben bleiben in `Config/TaskBoard.json` (TaskBoard/`TaskBoardPanel::edit` ist der eine
Schreiber); "Tasks" im Planner ist eine **Projektion**. Gruende: Footer, MCP-Tools und Team-Bruecke haengen an Task-Historie
(`TaskUpdate`), die in einer Zeile keinen Platz hat. Preis: Tasks hat feste Spalten (Status, Priority, Assignees, Description,
Links); `Config/TaskBoard.json` bleibt die Aufgabendatei. Variante B (Planner als Wahrheit) waere spaeter ein eigener Schritt.
**Fertig:**
- `PlannerTasks.{h,cpp}`: `refreshTasks()` (Board -> Zeilen, Schema fest) und `applyTasksEdit()` (Datenbank-Edit -> TaskBoard-Aufrufe:
  Status -> `setStatus` mit Update, Titel -> kein Update, Zuweisung -> `assign`, Links per `sameTarget`, neue Zeile = `add` mit Zeilen-Id,
  Zeile loeschen = `remove`; Spalten hinzufuegen/loeschen wird abgelehnt).
- `Workspace::External` (setExternal): Quelle fuer Zeilen, `apply` fuer Edits, `generation` zum Abfragen; die Tasks-Datei
  speichert nur Views (`Database::persistRows=false`); `remove(Tasks)` abgelehnt; unlesbare Datei wird nie ueberschrieben.
- `TaskBoardPanel` ist nur noch Datenkern + Link-Werkzeug: `tick()` (Team pumpen, Entity-Sprung, jede Frame aus EditorUI),
  `generation()` (steigt auch bei Edits), `reloadBoard/retrySave`, `linkChip/acceptDroppedLinks/jumpToLink`. Fenster, Karten,
  Details entfernt. `Jump::Panel` auf "Task Board"/"Planner" oeffnet den Planner-Tab.
- `PlannerPanel`: Zeilen-Details rechts (Beschreibung, Links als Chips mit x, Drop-Zone, Link Selection/Scene/Panel, Verlauf,
  Kommentar, Delete Row), Links-Zelle mit Drop-Ziel und Chips, Assignees-Auswahl aus dem Team (nur mit Team), "From the Team",
  Banner mit Reload Tasks/Retry Save, Spalten von Tasks gesperrt. Import-Knopf entfernt.
- `EditorUI`/`MacMenuBar`: `Window > Task Board` oeffnet Planner auf Tasks (`MC::OpenTaskBoard`), Footer-Klick oeffnet Tab mit der
  gewaehlten Aufgabe, `s_showTaskBoard` und `PanelOpenTaskBoard` entfernt (alte Configs ignorieren den Schluessel).
- Help: alte "Task Board/*"-Fenstereintraege raus, `Planner/*` dazu, Footer-Eintraege bleiben; Audit 0 offen.
**Tests:** `test_taskboard_ui.cpp` ist weg, seine Faelle sind in `test_planner_ui.cpp` portiert (Drops + Chips + Sprung + Orphan, Pull unter
offenem Board + Merge-Konflikt, Assignees nur mit Team + Benachrichtigung + From the Team, Lane-Drag, neue Zeile/Titel). Neu:
`PlannerTasks`/External-Faelle in `test_planner_workspace.cpp`. `test_taskboard_team`, `_footer_ui`, `test_mcp_tools_taskboard`,
`test_task_board` unveraendert gruen (Beweis, dass die Bruecke die Edits weiter sieht). `HorizonEditor` gebaut (Release, macOS).
**Nicht geprueft:** der Editor wurde nicht gestartet (Tab von Hand oeffnen/schliessen/Neustart, Footer-Klick, `Window > Task Board`
in der nativen Menueleiste); Windows/Linux nicht gebaut; die Handbuchquellen unter `EditorDeps/Docs` enthalten keinen Treffer fuer "Task Board" (die Website-Docs `HorizonEngineDocs/` nicht geprueft).
**Offen/Naechste Schritte:** 1. Editor einmal von Hand durchklicken (siehe oben). 2. Website-Docs (`HorizonEngineDocs/`) auf das Task-Board-Fenster pruefen.
3. P2: Relationen und Engine-Links in beliebigen Datenbanken per Drag zuweisen (fuer Tasks geht es schon), Zeitleiste, Kalender.
4. P3: Seiten/Bloecke, eine Zeile oeffnet dann eine Page. 5. Spaeter: Planner als Wahrheit (Variante B) mit Historie je Zeile.
