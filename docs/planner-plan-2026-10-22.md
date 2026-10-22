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

## Handoff (Stand P1, 2026-10-22)
**Fertig:** P0 (siehe `src/HE_Editor/Planner.{h,cpp}`, `test_planner.cpp`) und P1 ohne P1b.
- `PlannerImport.{h,cpp}`: Task Board -> Datenbank "Tasks". Alle Ids (Datenbank, Properties, Optionen, Views) sind aus
  Namen abgeleitet (`derivedId`), damit zwei Rechner dieselbe Datei erzeugen. Zeilen-Id = Task-Id. Erneuter Import: neue
  Tasks dazu, vorhandene nur wenn `task.modifiedMs > row.modifiedMs` (Planner-Edits gewinnen). Historie/Kommentare kommen
  nicht mit (bleiben in `TaskBoard.json`).
- `PlannerWorkspace.{h,cpp}` (ImGui-frei): alle Datenbanken des Projekts, ein Schreiber, `edit()` laedt bei fremd
  geaenderter Datei neu und wendet die Aenderung einmal erneut an, `poll()` nimmt Pulls auf, unlesbare Dateien werden
  gelistet und nie ueberschrieben.
- `PlannerPanel.{h,cpp}`: virtueller Tab `::Planner::`. Seitenleiste (Datenbanken, New Database, Import Task Board,
  Reload), Views als Reiter (Tabelle, Board, `+ Table`/`+ Board`, Rechtsklick: umbenennen/loeschen), New Row, Suche,
  Filter-Popup, Properties-Popup (sichtbar, umbenennen, loeschen mit Bestaetigung, neu), Tabelle mit Header-Sortierung und
  Clipper, Zellen-Editor je Typ, Board mit Drag zwischen Lanes. Edits werden gepostet und nach dem Zeichnen angewendet.
- Verdrahtung: `EditorUI.cpp` (Include, Ctrl+S-Fallback, Tab-Dispatch, `Window > Planner`, `MC::OpenPlanner`),
  `MacMenuBar.{h,mm}`, `EditorHelp.cpp` (`Planner/...`, `Window/Planner`), `scripts/editor_help_audit.py` (PlannerPanel.cpp
  im Bereich interface). Sitzungs-Wiederherstellung geht ohne Aenderung: `restoreOpenTabs` nimmt jeden Pfad mit ':' am Anfang.
**Geprueft (macOS, Release):** `test_planner` 15, `test_planner_workspace` 8, `test_planner_ui` 6 Faelle gruen (Import,
Tippen in Zelle, Sortieren, 3000 Zeilen mit Clipper, Karte in andere Lane ziehen); Frames in `HE_UI_DUMP_DIR` angesehen.
`EditorUI.cpp` und `MacMenuBar.mm` per `-fsyntax-only` geprueft, der Editor selbst nicht gebaut/gestartet. Help-Audit 0 offen.
**Nicht gemacht:** P1b (Task-Board-Fenster, Footer, MCP, Team-Hinweise auf den Planner umstellen, altes Fenster
entfernen); Seiten/Bloecke im Tab (P3; eine Zeile oeffnet noch keine Page); Relationen und Engine-Links nur lesbar
(Zuweisen per Drag aus Content Browser/Outliner fehlt); Zeitleiste/Kalender; Reihenfolge der Datenbanken (jetzt nach
Name); Windows/Linux nicht gebaut. Select-Zelle zeigt den Farbpunkt nur im Menue.
**Naechste Schritte:** 1. Editor bauen und den Tab einmal von Hand oeffnen (Window > Planner, Tab schliessen, Editor
neu starten: kommt er wieder?). 2. P1b: erst `TaskBoardPanel::board()`-Nutzer inventarisieren (Footer, MCP-Tools,
`TaskBoardTeam`, Chip-Spruenge in EditorUI ~2780), dann Adapter. 3. P2 (Engine-Link per Drag, Relationen, Zeitleiste).
