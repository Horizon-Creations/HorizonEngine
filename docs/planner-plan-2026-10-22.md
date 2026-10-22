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

## Handoff (Stand P0, 2026-10-22)
**Fertig und geprueft:** P0. `src/HE_Editor/Planner.{h,cpp}` (namespace `HE::Planner`, ImGui-frei):
`Database` (Properties, Zeilen, Views, typisierte Zellen, `query()`/`column()` mit Filter/Sort/Gruppe,
Kaskaden beim Loeschen), `Page` (Block-Baum, `moveBlock`), Dateien `Config/Planner/Databases|Pages/<hex>.json`
(`load*/save*/delete*/list*`), gleiche Schutzregeln wie TaskBoard (nie ueber unlesbare oder fremd
geaenderte Datei speichern, Temp+Rename, unbekannte Keys/Typen/Zellen bleiben). Zellen sind das JSON,
das in der Datei steht (`Cell.json`), Zugriff ueber `setText/setSelect/...`.
`TaskBoardJson.h` macht die JSON-Helfer aus `TaskBoard.cpp` nutzbar (Tuer am Dateiende, TaskBoard sonst unveraendert).
Tests: `tests/test_planner.cpp` (15 Faelle, 337 Pruefungen, gruen), dazu `test_task_board`, `_team`, `_ui`,
`_footer_ui`, `test_mcp_tools_taskboard` weiter gruen (he_tests im Release-Build, macOS). `editor_help_audit.py --check` ok.
**Entschieden:** eine Datei je Datenbank/Seite; Zeile = Page mit gleicher Id; Filter/Sort mit Property `{}` = Titel;
Select-Sortierung nach Option-Reihenfolge; nicht auswertbarer Filter blendet nichts aus.
**Nicht gemacht / nicht geprueft:** keine UI, kein Tab, kein Menue; Windows/Linux nicht gebaut; keine Reihenfolge der
Seitenleiste (Verzeichnis nach Id sortiert, Index-Datei kommt mit dem Tab); Zeilen-Zellen werden linear gesucht
(bei tausenden Zeilen eine Map pro Zeile); `TaskBoard.cpp` und `Planner.cpp` teilen JSON-Helfer nur ueber die Tuer.
**Naechste Schritte (P1):** 1. `PlannerPanel` + `::Planner::`-Tab nach Muster `GameInstancePanel`
(EditorUI.cpp ~2445/4262/4516, Menue ~3131, Sitzungs-Wiederherstellung suchen). 2. Tabellenansicht ueber `query()`,
Eigenschaftstypen-Editoren. 3. Import `TaskBoard.json` -> Datenbank "Tasks" mit festen Option-Ids (wiederholbar).
4. Hilfe-Eintraege in `EditorHelp.cpp`.
