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

## Handoff (Stand P2, 2026-10-22)
**Fertig:**
- Assignees in Tasks: mit angemeldetem Team Auswahl aus dem Roster (wie vorher), ohne Team getippte Namen wie in jeder Datenbank
  (nur auf dem Board gespeichert, `assign` mit Verlaufseintrag, niemand wird benachrichtigt).
- Relationen: Zelle mit Auswahlliste der Zeilen der Zieldatenbank (haken/enthaken, eine Zeile zeigt nie auf sich selbst);
  "Add Property" fragt bei Typ Relation nach der Zieldatenbank ("This database" oder eine andere).
- Zeitleiste (`ViewKind::Timeline`): `+ Timeline`, Start/End (Date-Properties) und "Colour by" (Select) im Toolbar, ein Tag je
  Spalte, Balken von Start bis End (ohne End ein Tag), Heute-Markierung, Zeilen ohne Start unten als "no start date", Balken
  ziehen verschiebt Start und End, Klick auf Balken/Titel oeffnet die Zeile. Zeilen per Clipper.
- Engine-Links in beliebigen Datenbanken: Zelle und Details sind generisch (Drop, Chips, Sprung), nicht nur in Tasks.
**Tests:** `test_planner_ui` 12 Faelle (neu: getippte Assignees, Relationszelle, Zeitleiste), Help-Audit 0 offen, `HorizonEditor`
gebaut. Alle anderen Task-Board-Tests gruen. **Nicht geprueft:** Editor nicht gestartet; Windows/Linux nicht gebaut.
**Offen:** Kalenderansicht; Zeitleiste ohne Zoom und ohne Resize der Balken; Relation zeigt nur Titel (kein Klick zur Zielzeile);
P3 (Seiten/Bloecke, eine Zeile oeffnet eine Page); Tasks-Spalten bleiben fest; Reihenfolge der Datenbanken nach Name.

## Handoff (Stand P3, 2026-10-22)
**Fertig:** Seiten und Bloecke.
- Modell (`Planner.{h,cpp}`): `Page::addBlockAfter/setBlockType/setBlockText/setBlockChecked/indentBlock/outdentBlock/moveBlockBy`
  (Ausruecken nimmt die juengeren Geschwister als Kinder mit).
- `Workspace`: `pages()`, `findPage`, `createPage(title, parent)`, `openRowPage(row, db)` (nur im Speicher, bis der erste Block
  da ist: `savePage` schreibt nichts fuer eine leere Seite ohne Titel), `removePage` (samt Unterseiten), `editPage` (Pull unter
  der offenen Seite wird gelesen, Edit einmal darauf neu angewendet), `poll` fuer Seiten, unlesbare Seiten gelistet und nie ueberschrieben;
  `remove(db)` nimmt die Zeilenseiten mit.
- `PlannerPanel`: Seitenleiste "Pages" (Baum nach `parent`, Rechtsklick Delete Page mit Bestaetigung, New Page), Seitenansicht mit
  Titel, Block-Editor (Enter = naechster Block, Strg/Umschalt+Enter Zeilenumbruch, Markdown-Kuerzel `# ## ### - * 1. [] > --- ```` auf
  einem Absatz, Enter auf leerem Listenpunkt beendet die Liste, Backspace im leeren Block loescht ihn), Block-Menue (⋮): Turn into, Add Block Below,
  Move Up/Down, Indent/Outdent, Delete Block; Engine-Objekt-Block (Drop, Chip, Sprung), Datenbank-Ansicht-Block (erste 8 Zeilen,
  Klick oeffnet die Zeilenseite), Drop auf die Seite haengt Engine-Objekte an. Jede Zeile: "Open Page" in den Details, Eigenschaften
  oben (`drawPropertyTable`, gemeinsam mit den Details), Back fuehrt zur Datenbank. Zeile loeschen loescht auch ihre Seite.
- Help: `Planner/New Page`, `Open Page`, `Add Block`, `Turn into`, `Add Block Below`, `Move Up/Down`, `Indent`, `Outdent`, `Delete Block`,
  `Delete Page`; Audit 0 offen.
**Tests:** `test_planner_ui` 16 Faelle (neu: Seite schreiben, Block-Menue, Zeilenseite erst mit Block gespeichert, Drop + Datenbank-Block),
`test_planner_workspace` 16, `test_planner` 16; Task-Board-Tests unveraendert gruen; `HorizonEditor` gebaut.
**Nicht geprueft:** Editor nicht gestartet (Schriftgroessen der Ueberschriften, Code-Schrift, Fokus per Tastatur im echten Editor);
Windows/Linux nicht gebaut; Volllauf von he_tests nach P3 nicht wiederholt.
**Offen:** Seiten-Links (Block "Seite") und Unterseiten per UI anlegen; Bloecke per Ziehen umordnen; Pfeiltasten zwischen Bloecken;
Kalenderansicht; Zeitleiste ohne Zoom/Resize; Relation: Klick zur Zielzeile; Variante B (Planner als Wahrheit fuer Tasks) mit Historie je Zeile;
Live-Zusammenarbeit ueber den Team-Server.

## Handoff (Stand Kalender, 2026-10-22)
**Fertig:** Kalenderansicht (`ViewKind::Calendar`): `+ Calendar`, Monat als Raster (Montag zuerst, immer sechs Wochen), Date/End/Colour by im
Toolbar, Previous/Today/Next mit Monatsname, Zeilen auf jedem Tag von Date bis End, "+N more" wenn es nicht passt, Ziehen auf einen anderen
Tag verschiebt Date und End, Doppelklick auf einen Tag legt eine Zeile mit diesem Datum an, Klick oeffnet die Zeile. `PlannerPanel::showCalendarMonth/
calendarYear/calendarMonth` fuer Tests. Zeilen werden jetzt beim Loslassen ohne Ziehen geoeffnet (Kalender, Zeitleiste), nicht beim Druecken;
sonst verschiebt sich das Raster unter dem Cursor mitten im Ziehen.
**Fehler gefunden und behoben:** Chips in der Links-Zelle der Tabelle bekamen keine Klicks (die Drop-Flaeche darunter nahm sie, `SetNextItemAllowOverlap`
fehlte); jetzt Test dafuer.
**Tests:** `test_planner_ui` 17 Faelle, Help-Audit 0 offen, `HorizonEditor` gebaut. Volllauf: 3 bekannte, vom Planner unabhaengige Fehlschlaege
(`test_hpak` Disk registry, zwei `EngineProfiler`-Faelle), alles andere gruen.
**Offen:** Kalender zeigt nur Monate (keine Wochenansicht), Zeitleiste ohne Zoom/Resize; Seiten-Links/Unterseiten per UI; Bloecke per Ziehen umordnen;
Relation: Klick zur Zielzeile; Variante B fuer Tasks; Live-Zusammenarbeit; Editor nicht gestartet, Windows/Linux nicht gebaut.
