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

## Handoff (Stand Seiten-Links, Unterseiten, Block-Ziehen, 2026-10-22)
**Fertig:**
- Modell: `BlockType::PageLink` ("page-link", Feld `Block::page`; auf anderen Bloecken bleibt ein "page"-Schluessel unbekannter Schluessel und
  wird zurueckgeschrieben), `Page::moveBlockNear(id, target, after)` (vor/nach einem Block auf dessen Ebene, Kinder gehen mit, nicht in den eigenen Teilbaum).
- Panel: Seiten-Link-Block (Auswahl der Seiten, solange er nirgends hinzeigt; dann der Titel als Link, x loest ihn; "Page not found" bei fehlender Ziel-Seite),
  "Turn into > Page link", `New Sub-page` (legt die Seite mit `parent` = diese Seite an und haengt einen Link-Block ans Ende; in der Seitenleiste
  eingerueckt), `Back` bei Unterseiten fuehrt zur Eltern-Seite, Bloecke ziehen: das Griff-Symbol (⋮) ist Drag-Quelle, jede Blockzeile Ziel
  (obere Haelfte = davor, untere = danach, Linie als Vorschau); ein Klick ohne Ziehen oeffnet weiter das Menue.
- Help: `Planner/New Sub-page`, `Turn into` ergaenzt.
**Tests:** `test_planner_ui` 20 Faelle (neu: Block ziehen, Unterseite + Zurueck, Seiten-Link waehlen und folgen), `test_planner` 17 (moveBlockNear, PageLink-Round-Trip,
fremder "page"-Schluessel), Workspace 16, Task-Board-Tests gruen, Audit 0 offen, `HorizonEditor` gebaut. Volllauf nicht wiederholt.
**Offen:** Pfeiltasten zwischen Bloecken; Block "Seite" fuer Zeilenseiten in der Auswahl (nur freie Seiten); Seiten per Ziehen in der Seitenleiste verschachteln;
Wochenansicht im Kalender; Zeitleiste ohne Zoom/Resize; Relation: Klick zur Zielzeile; Variante B fuer Tasks; Live-Zusammenarbeit; Editor nicht gestartet, Windows/Linux nicht gebaut.

## Handoff (Stand Relationen, Pfeiltasten, Zeitleiste, 2026-10-22)
**Fertig:**
- Relationen: in den Details und auf der Zeilenseite stehen die verknuepften Zeilen als Links unter dem Auswahlfeld; ein Klick oeffnet die Zielzeile
  (Datenbank wird gewechselt, Details offen). In Tabellenzellen bleibt es beim Auswahlfeld.
- Pfeiltasten im Block-Editor: Hoch auf der ersten, Runter auf der letzten Zeile eines Blocks geht zum Block darueber/darunter (Trenner, Engine-Objekt,
  Datenbank- und Seiten-Link-Bloecke werden uebersprungen); der Cursor kommt beim Hochgehen und bei Backspace ans Ende, beim Runtergehen an den Anfang.
- **Fehler behoben:** In einem Block getippter Text ging verloren, wenn man danach einen Block ANKLICKTE, der weiter oben steht (gemeinsamer Edit-Puffer,
  ImGuis "deactivated after edit" ist unzuverlaessig, wenn ein frueheres Feld im selben Frame den Fokus nimmt). Jetzt Puffer je Block, "verlassen" = es gibt
  getippten Text aus einem frueheren Frame und das Feld ist nicht mehr aktiv. Regressionstest dazu.
- Zeitleiste: Zoom (Days/Weeks/Months, Combo in der Toolbar, `setTimelineZoom` fuer Tests), rechter Rand eines Balkens ziehen aendert das End-Datum
  (mit End-Property; ohne End-Zelle beginnt es am Start).
**Tests:** `test_planner_ui` 23 Faelle (neu: Relation oeffnen, Pfeiltasten, Edit bleibt beim Anklicken, Resize + Zoom in der Zeitleiste), Audit 0 offen,
`HorizonEditor` gebaut, Task-Board-Tests gruen. Volllauf nicht wiederholt. Editor nicht gestartet, Windows/Linux nicht gebaut.
**Offen:** Kalender nur Monate (keine Wochenansicht); Seiten per Ziehen in der Seitenleiste verschachteln; Zeilenseiten in der Seiten-Link-Auswahl;
Zeitleiste: Start-Rand ziehen, Heute-Sprung; Variante B fuer Tasks (Planner als Wahrheit, Historie je Zeile); Live-Zusammenarbeit ueber den Team-Server;
Markdown-Export/CSV; MCP-Tools fuer den Planner.

## Handoff (Stand MCP-Tools, 2026-10-22)
**Fertig:** `McpToolsPlanner.cpp` (`registerPlannerTools`, Hooks `McpPlannerHooks` in `McpToolHooks.h`) und `PlannerMarkdown.{h,cpp}`.
Tools: `planner_overview`, `planner_read_database` (View-Filter/Sortierung, search, limit/offset), `planner_add_row`, `planner_update_row`,
`planner_delete_row`, `planner_create_database`, `planner_add_property`, `planner_read_page`, `planner_write_page` (Markdown, replace/append,
neue Seite mit title/parent, Zeilenseiten per database+row), `planner_delete_page`. Alles laeuft ueber den einen `Workspace` des Tabs
(`EditorApplication.cpp`: `m_mcpPlannerCtx` haelt den AppContext, den die Tasks-Datenbank zum Erreichen des Task Boards braucht), also zeigt der offene Tab
Aenderungen sofort; "Tasks" geht ueber den Board-Schreiber (Status-Aenderung = Verlaufseintrag, Titel = keiner). Werte als Namen (Select-Optionen, Daten
YYYY-MM-DD, Relationen per Titel/Id, Engine-Links als {kind,id|scene|panel,label}); ein Wert, der nicht passt, verweigert die ganze Aenderung und nennt die Felder
(Probelauf auf einer Kopie). Markdown-Dialekt: siehe Kopf von `PlannerMarkdown.h`.
**Tests:** `test_planner_markdown` 4, `test_mcp_tools_planner` 7 gruen; Planner-/Task-Board-Tests unveraendert gruen; `HorizonEditor` gebaut.
**Nicht geprueft:** Aufruf ueber einen echten MCP-Client gegen den laufenden Editor (Rezept: `docs/mcp-editor-integration-plan.md`); Volllauf nicht wiederholt.
**Offen:** Whiteboard (naechster Schritt, siehe unten); keine Planner-Tools fuer Ansichten (Filter/Sortierung anlegen), Datenbank/Seite umbenennen/loeschen.

## Whiteboard (Plan, 2026-10-22, noch nichts gebaut)
Wunsch (Connor): "Boards" als freie Flaeche wie ein Whiteboard, mit Nodes mit Text und Bildern.
**Was es ist:** ein dritter Dokumenttyp neben Datenbank und Seite: ein **Whiteboard** (unendliche Flaeche) mit Karten (Nodes) und Verbindungen (Edges).
Nicht zu verwechseln mit der Board-Ansicht einer Datenbank (Lanes).
**Datei:** `Config/Planner/Boards/<id>.json`, gleiche Regeln wie Seiten/Datenbanken (eine Datei je Board, UUIDs, verlustfrei, unbekannte Schluessel bleiben,
nie ueber unlesbare/fremd geaenderte Datei speichern). Inhalt: `nodes[]` {id, type, x, y, w, h, color, text, ...typspezifisch}, `edges[]` {id, from, to, label?, arrow?},
`groups`/Rahmen als Node-Typ `frame`. Sicht (Pan/Zoom) wird nicht in die Datei geschrieben.
**Node-Typen:** `text` (Karte mit Text, Markdown-Kurzform), `image` (Bild), `object` (Engine-Objekt: Asset/Szene/Entity/Panel, Chip + Sprung), `row` (Karte einer Datenbank-Zeile,
Klick oeffnet sie), `page` (Karte einer Seite), `frame` (beschriftete Gruppe, zieht Inhalt mit).
**Bilder (Entscheidung noetig):** (a) Texture-Assets des Projekts per UUID referenzieren (wie Engine-Links, kein Duplikat, geht mit Content Browser Drag); (b) hineingezogene/eingefuegte
Bilddateien nach `Config/Planner/Media/<hex>.png` kopieren (reist mit Git, LFS-Frage bei grossen Dateien). Empfehlung: beides, (a) bevorzugt, (b) fuer Screenshots/Moodboards;
Miniaturen ueber den vorhandenen `AssetThumbnailCache`/Texturpfad des Editors.
**Bedienung (Canvas im Tab):** Pan (Leertaste+Ziehen / Mittelmaustaste / Trackpad), Zoom (Strg+Rad / Pinch), Auswahl (Klick, Shift, Aufziehrahmen), Verschieben/Groesse per Griffe, Doppelklick
auf leere Flaeche = neue Textkarte, von Kartenrand auf andere Karte ziehen = Verbindung, Drop aus Content Browser/Outliner = Karte, Einfuegen aus Zwischenablage (Text/Bild),
Farben, Raster-Einrasten, Minimap. **Undo/Redo** gibt es im Planner noch nicht; fuer ein Whiteboard fast Pflicht (Befehlsstapel je Dokument).
**MCP:** `planner_read_board` / `planner_write_board` (Nodes/Edges als JSON, Auto-Layout fuer Nodes ohne Position), damit Claude Ideen als Board anlegen kann.
**Phasen:** W0 Modell + Datei + Workspace + Tests (ImGui-frei) | W1 Canvas: Pan/Zoom, Textkarten, Verschieben/Groesse, Verbindungen, Undo | W2 Bilder (Asset-Referenz, Drop, Einfuegen, Medien-Ordner) |
W3 Objekt-/Zeilen-/Seitenkarten, Rahmen | W4 MCP-Tools, Minimap, Export als PNG.

## Handoff (Stand Whiteboard W0, 2026-10-22)
**Entschieden (Connor):** Bilder beides: Texture-Assets des Projekts per UUID referenzieren (`Node::asset`) UND eingefuegte/hineingezogene Dateien nach
`Config/Planner/Media` kopieren (`Node::media`, Name = Inhalts-Hash + Endung, gleiches Bild zweimal = eine Datei).
**Fertig (W0, ImGui-frei):**
- `PlannerBoard.{h,cpp}`: `Board` mit `Node` (Typen text, image, object, row, page, frame; Position/Groesse in Board-Einheiten, Reihenfolge = Stapel, letzter oben)
  und `Edge` (from, to, label, arrow). Operationen: addNode, move/moveNodesBy/resize (Mindestgroesse 24), Text/Farbe, bringToFront/sendToBack, removeNode (nimmt Kanten mit),
  addEdge (nicht auf sich selbst, nicht doppelt, Enden muessen da sein), hitTest (Karten schlagen Rahmen), nodesIn (Auswahlrechteck), nodesInside (Rahmen nimmt Inhalt mit), bounds.
  Datei `Config/Planner/Boards/<hex>.json`, gleiche Regeln wie Seiten/Datenbanken (byte-identisch, unbekannte Typen/Schluessel bleiben, auch ein "page"-Schluessel an einem
  Text-Knoten; nie ueber unlesbare/fremd geaenderte Datei speichern). Typ-spezifische Schluessel gelten nur fuer ihren Typ.
- Medien: `importMediaFile/importMediaBytes` (png, jpg, jpeg, gif, bmp, webp, tga; max 64 MB; Temp + Rename), `mediaPath` (lehnt Pfadtricks ab), `listMedia`.
- `PlannerFile.h`: die gemeinsamen Datei-Regeln (aus `Planner.cpp`) fuer weitere Dokumenttypen.
- `Workspace`: `boards()`, `findBoard`, `createBoard`, `removeBoard`, `editBoard` (Pull unter dem offenen Board wird gelesen, Edit einmal darauf), `poll`, `unreadableBoards`.
**Tests:** `test_planner_board` 8 Faelle gruen (Modell, Datei, Unbekanntes, beschaedigte Datei, Medien, Workspace); Planner-Tests unveraendert gruen; `HorizonEditor` gebaut.
**Nicht gemacht:** keine UI (W1), keine MCP-Tools fuer Boards, kein Undo, keine Aufraeumfunktion fuer ungenutzte Medien.
**Naechste Schritte (W1):** Canvas im Tab: Seitenleiste "Boards", Pan/Zoom (Leertaste+Ziehen, Mittelmaus, Strg+Rad), Textkarten (Doppelklick leer = neu, Doppelklick Karte = Text bearbeiten),
Auswahl (Klick, Shift, Aufziehrahmen), Verschieben/Groesse per Griffe, Verbindungen vom Kartenrand, Farben, Loeschen; Zeichnen nur sichtbarer Karten; Undo/Redo je Board (Befehlsstapel).
Danach W2 (Bilder: Asset-Drop aus dem Content Browser, Einfuegen aus der Zwischenablage ueber `importMediaBytes`, Vorschau mit `AssetThumbnailCache`).

## Handoff (Stand Whiteboard W1, 2026-10-22)
**Fertig (W1):** der Canvas im Tab.
- `BoardHistory` (ImGui-frei, `PlannerBoard.{h,cpp}`): Undo/Redo als Schnappschuesse (Karten + Pfeile), 100 Schritte, je Board, nicht gespeichert; Wiederherstellen ist Zuweisung
  (idempotent, falls ein Speichern nach einem Pull wiederholt wird). `snapshotOf`/`restore`.
- `PlannerBoardView.{h,cpp}`: Toolbar (Titel, New Card, New Frame, Undo, Redo, Delete, 7 Farben + "keine", Fit, 100 %, Zoomanzeige, bei gewaehltem Pfeil Beschriftung + Arrow)
  und Canvas: Raster, Pan (Scroll, Mittelmaus, Leertaste+Ziehen), Zoom an der Maus (Strg/Cmd+Scroll, 0,1x bis 4x), Auswahl (Klick, Shift, Aufziehrahmen),
  Verschieben (ein Rahmen nimmt, was ganz in ihm liegt, mit), Groesse per Eckgriffe (nie unter 24), Pfeile per Ziehen von einem Punkt am Kartenrand auf eine andere Karte
  (nicht doppelt, nicht auf sich selbst), Pfeil anklicken = auswaehlen, Doppelklick leer = neue Karte (Cursor drin), Doppelklick Karte = Text in place, Delete/Backspace,
  Strg/Cmd+Z, +Shift oder +Y, Strg/Cmd+A, Esc. Nur sichtbare Karten werden gezeichnet. Karten der Typen image/object/row/page werden schon als Karte mit Beschriftung
  dargestellt (Bild-Vorschau und Anlegen dieser Typen kommen in W2/W3).
- `PlannerPanel`: Seitenleiste "Boards" (Liste, New Board, Rechtsklick Delete Board mit Bestaetigung), Board ersetzt Datenbank/Seite im Hauptbereich, unlesbare Boards rot gelistet.
  Jede Aenderung laeuft durch `Workspace::editBoard` (Pull unter dem offenen Board wird gelesen und die Aenderung einmal darauf angewendet).
**Fehler beim Bauen gefunden und behoben:** ein Ziehen, das im selben Frame beginnt, mass vom Start des vorigen Ziehens und galt als Verschiebung (Klick = leerer Undo-Schritt).
**Tests:** `test_planner_ui` 29 Faelle (neu 6 fuer das Whiteboard: Karte anlegen und tippen, Titel, Verschieben + Undo/Redo per Knopf und Taste + gespeicherte Datei, Eckgriffe,
Pfeile, Pfeil-Beschriftung, Loeschen mit Pfeilen + Undo, Scroll/Zoom an der Maus/Mittelmaus-Pan/Fit/100 %, Rahmen, Farben, 2000 Karten gezeichnet < 20, Board-Wechsel, Pull),
`test_planner_board` 9, Help-Audit 0 offen, `HorizonEditor` gebaut. Volllauf nicht wiederholt. Editor nicht gestartet (Gefuehl von Zoom/Pan mit Trackpad/Maus ungeprueft);
Windows/Linux nicht gebaut.
**Offen:** W2 Bilder (Asset-Drop aus dem Content Browser, Einfuegen aus der Zwischenablage ueber `importMediaBytes`, Datei-Drop, Vorschau mit `AssetThumbnailCache`/Textur aus `Config/Planner/Media`);
W3 Objekt-/Zeilen-/Seitenkarten anlegen (Drop aus Outliner, "Open" in Details/Seite als Karte), Rahmen-Titel, Ausrichten/Raster-Einrasten, Kopieren/Einfuegen/Duplizieren;
W4 MCP-Tools fuer Boards, Minimap, Export als PNG; Undo fuer Titel; Mehrfachauswahl-Groesse; Mausrad-Zoom-Gefuehl pruefen.

## Handoff (Stand Whiteboard W2)

Pictures on the whiteboard. Verified: `he_tests --test-case="planner*"` 84/84 (two new UI tests in
`tests/test_planner_ui.cpp`: paste + file drop; asset/entity drop), `HorizonEditor` builds, help audit clean.
**Not verified**: the picture actually drawn (no renderer in tests, so `pictureOf` returns nothing there), the
real system clipboard (`SDL_GetClipboardData` with image/png, jpeg, bmp, webp, in `PlannerPanel.cpp` drawBoardTab)
and a real OS file drop. Try all three in the editor first. macOS Finder/screenshot clipboards may only offer
TIFF/other formats, in which case paste falls through to text.

- Preview: `PlannerBoardView.cpp` `pictureOf` (Texture asset: `AssetThumbnailCache::image`; media: stb_image
  + `renderer->CreateImGuiTexture`, cached per name in `s_mediaTex`, destroyed in `reset()`), drawn letterboxed in
  `drawNode` with the caption below. Without a picture the card shows the old label ("Image (not shown): ...").
- Creating cards: `placeCard`, `placeMediaImage`, `placeLink` (Texture asset -> Image with UUID, anything else ->
  Object card), `paste` (picture beats text), size from the picture (`cardSizeFor`, long side 320).
- Content Browser/Outliner drop: right after `##canvas` via `TaskBoardPanel::acceptDroppedLinks`.
- OS file drop: `EditorApplication.cpp` (before the .heproj block) asks `PlannerBoardView::wantsFileDrop(path, x, y)`
  (picture extension + canvas drawn in the last 2 frames + inside it), then `dropFile`; the canvas imports it next frame.
- Test hook: `PlannerPanel::setClipboardForTest`.

Decisions: copies go to Config/Planner/Media (hash name, dedupes); Texture assets are never copied.
Next steps: W3 (create object/row/page cards from a menu, frame titles, align/snap, copy/paste/duplicate of cards),
W4 (MCP board tools, minimap, PNG export), unused-media cleanup, board title undo.

## Handoff (Stand Whiteboard W3)

Verified: `he_tests --test-case="planner*"` 86/86 (new: `test_planner_board.cpp` align/distribute/snap/copy-paste,
`test_planner_ui.cpp` "copy, paste and duplicate, align, snap, page and row cards, frame titles"), HorizonEditor
builds, help audit clean. Not verified: real editor feel (snap while dragging, the Add popup with many rows), row cards
(only the page card path is tested), Windows/Linux.

- Board model (`PlannerBoard.h/.cpp`): `Board::alignNodes/distributeNodes/snapNodes`, `BoardClip`, `copyNodes`, `pasteNodes` (new ids, arrows between copied cards kept).
- View (`PlannerBoardView.cpp`): second toolbar row (Add popup of pages and per-database rows, Duplicate, Align popup, Snap checkbox, colours, Fit, 100%); the arrow-label row is now its own line. Keys Cmd/Ctrl+C/X/V/D. Copy leaves the marker `HorizonEngine/board-cards` on the system clipboard (replaces a picture, so paste knows to use `s_clip`); `Services::setClipboardText`, test hook `setClipboardForTest(image, text, setText)`.
- Snap (20 units): the card in hand lands on the grid, the others keep their offset; resize snaps the corner.
- Double-click: Page/Row card opens it (`PlannerPanel::selectPage/openRowPage`, posted), Object card jumps (`TaskBoardPanel::jumpToLink`); frame titles are edited by double-click as before.
Open: pasted frames come out on top of cards (no sendToBack); align ignores what lies inside a frame; W4 (MCP board tools, minimap, PNG export), unused-media cleanup, board-title undo.

## Handoff (Stand Whiteboard W4)

Verified: `he_tests --test-case="planner*"` 88/88 (new: `test_mcp_tools_planner.cpp` whiteboard tool test; `test_planner_ui.cpp` minimap + Export PNG), HorizonEditor builds, help audit clean. I looked at an exported PNG (cards, text, arrow, label drawn). Not verified: the buttons in the real editor, pictures inside an export (media pictures use the CPU rasteriser's `registerTexture`; untested), minimap feel on a big board.

- MCP (`McpToolsPlanner.cpp`): `planner_create_board`, `planner_read_board`, `planner_add_board_card` (text/frame/page/row), `planner_update_board_card`, `planner_delete_board_card`, `planner_connect_board_cards`, `planner_delete_board_arrow`; `planner_overview` now lists `whiteboards`. No image cards or PNG export over MCP (export needs the UI's font atlas on the main thread).
- Minimap (`PlannerBoardView.cpp`, 180x120 bottom right, "Map" checkbox): click/drag centres the view; it takes the mouse there (`overMap`).
- Export: `exportPng` draws the board into its own `ImDrawList` and rasterises it with `tests/ImGuiSoftwareRaster` (already compiled into the editor for EditorUiShot), `hePngWrite`; file `<project>/Exports/Planner/<title>.png`, longest side <= 4096, notification with the path. Texture-asset pictures show their label.
- Fixed on the way: `n->id` was read after `sendToBack` moved the vector (a new frame selected / returned the wrong card), in `createNode` and the MCP tool.
Open: unused-media cleanup, board-title undo, pasted frames on top of cards, align ignores frame contents, MCP image cards / export, calendar week view, timeline start-edge drag, sidebar page nesting by drag, live collaboration, real-editor test and Windows/Linux builds.

## Handoff (Stand Redesign der Planner-Oberfläche)

Verified: `he_tests --test-case="planner*"` 89/89 and `*help*` 37/37, HorizonEditor builds, help audit clean; looked at headless shots (sidebar, table toolbar, whiteboard). Not verified: the pinch on a real trackpad (the test sets `EditorInput::detail::g_pinchFrame`; the hook itself is the other canvases' and unchanged), look and feel in the real editor.

- Icons: `PlannerIcons.h` (header only, glyphs drawn with ImDrawList, no files: the editor has no icon font and I did not download any). Add a glyph = one `case` in `drawIcon`.
- Whiteboard (`PlannerBoardView.cpp`): header with undo/redo, zoom -, 100%, +, fit, map, export at the right; floating tool plate (card, frame, add page/row, snap) at the top left; floating bar over the selection (colour popup, duplicate, align when 2+, delete). Pinch zooms at the cursor. Control names for tests unchanged, except the colour swatches now sit in the "Color" popup.
- Sidebar (`PlannerPanel.cpp`): section titles DATABASES / PAGES / BOARDS with a "+" each (same `note` names as the old buttons), rows with a glyph and an accent bar when open.
- Database: view tabs with a glyph per kind and an underline; "Add View" (+) popup replaces the four "+ X" buttons (tests: click "Add View", then "+ Timeline"); search field with a magnifier; Filter/Properties as glyph buttons (Filter shows the count and lights up while a filter is on). Pages: Add Block / New Sub-page as glyph buttons.
Not touched: the table cells, row drawer, calendar/timeline/board-lane drawing, page blocks, the Task Board footer.

## Handoff (Stand Timeline-Schritte, Tabellen/Timelines/Boards als eigene Dinge)

Verified: `he_tests --test-case="planner*"` 92/92 and `*help*`, HorizonEditor builds, help audit clean. Not verified: the real editor (drag across the days with a mouse, the new sidebar).

- Timeline (`PlannerPanel.cpp` drawTimeline/`createStep`/`dayGesture`): "New Step" (today, 3 days, title typed in place); the last lane makes a step by dragging across the days (a click = 3 days); a row without dates has its own lane, a drag gives it dates; double-click a title renames in place; segmented Days/Weeks/Months, Today, Settings popup (Start/End/Colour by).
- Sidebar by kind: TABLES, TIMELINES, BOARDS (kanban), CALENDARS, PAGES, WHITEBOARDS, each with a "+". One entry per view; the tooltip names its data. A "+" of a view kind opens "A <kind> of": New data (a timeline gets Start/End, a board a Status Select with three options, a calendar a Date column) or any data that exists (then it is one more view of the same rows, `newView`/`addConfiguredView`/`prepareData`). The title of an open view is the view's name; "Data: X" opens the data's name. Deleting: context menu Delete View (when the data has several views) or Delete Data (confirm; not for Tasks). The model is unchanged (Database + Views); only the vocabulary and entry points moved.
- Tests: `newTable(ctx)` helper (New Table + data:new); a click right after an assignee popup is lost until another click takes the focus back (ImGui blocks hovering while a popup is the nav window), see the "From the Team" test.
Open: linking beyond relations (a timeline step linking a table row on other data = Relation property; whiteboard cards already link rows/pages), page blocks that embed a view (exist) could use the new "data" choice, MCP tools still say "database".

## Handoff (Stand Tabs, New Step/Event/Card)

Verified: `he_tests --test-case="planner*"` 93/93 + `*help*`, HorizonEditor builds, help audit clean; looked at a headless shot of the timeline with tabs. Not verified: the real editor.

- Tab bar (`PlannerPanel.cpp` `drawTabs`): one tab per open table/timeline/board/calendar/page/whiteboard (`s_tabs`, folded from the selection each frame, so any way of selecting opens it once or switches); x closes the tab (not the last), "+" lists everything and opens-or-switches. The old per-data view tabs and "Add View" popup are gone; views are made from the sidebar (`New Timeline` ... then "data:<name>").
- New Step / New Event / New Card (primary button per view kind; all `note("New Row")`): `newDated` adds Start/End (timeline) or Date (calendar) columns to the data when it has none, so the button always shows something; the calendar also has a + in a hovered day (`cal-add:day:<iso>`) and double-click; a board card goes into the first lane. Tasks has no dates (the Task Board file has none): timeline/calendar are disabled for it in the "data" list and their New buttons are disabled.
Open: tabs are not saved across editor restarts; MCP tools still say "database".
