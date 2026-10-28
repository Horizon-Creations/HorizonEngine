# Modulbericht Editor – HorizonEditor (186 k LOC)

Pfad: `src/HE_Editor` · Teilberichte: 20 (`_parts/Editor_*.md`)

## Modulzusammenfassung

> **Abdeckungshinweise:** (1) Slice 1 wurde zweimal angesetzt: der erste Auditor hat den Umfang falsch gelesen (ganzes Verzeichnis per Grep); sein Bericht liegt als Ergänzung in `_parts/supplement/Editor_hotpath_grep.md` (nicht in den Zahlen unten), Slice 1 wurde als 1a (`Editor_1.md`) und 1b (`Editor_20.md`) vollständig neu gelesen. (2) `EditorHelp.cpp` Z. 1960–9372 (Datentabelle) wurde per Skript auf Eindeutigkeit/Abdeckung geprüft, nicht zeilenweise gelesen. (3) `HcNodeDocs.cpp` Z. 120–2080 (Doc-Tabelle) ebenso nur per Skript. (4) `EditorSelfUpdate.h` Z. 1–60 nur überflogen. (5) `EditorApplication.cpp`: Witness-Szenen (Z. 5321–10075) nur auf Duplikate/Struktur, nicht auf Bildlogik geprüft.

**362 Funde**: Hoch 35, Mittel 157, Niedrig 170 · Typ: Vereinfachung 116, Performance 210, Beides 36 · Kategorien: A 94, B 5, C 101, D 100, E 22, F 40

| Teilbericht | Hoch | Mittel | Niedrig |
|---|---|---|---|
| Editor_1.md | 0 | 7 | 11 |
| Editor_2.md | 2 | 6 | 7 |
| Editor_3.md | 3 | 8 | 9 |
| Editor_4.md | 2 | 11 | 8 |
| Editor_5.md | 0 | 5 | 6 |
| Editor_6.md | 0 | 4 | 8 |
| Editor_7.md | 1 | 5 | 5 |
| Editor_8.md | 1 | 11 | 11 |
| Editor_9.md | 3 | 8 | 7 |
| Editor_10.md | 2 | 9 | 9 |
| Editor_11.md | 2 | 9 | 11 |
| Editor_12.md | 2 | 8 | 7 |
| Editor_13.md | 0 | 9 | 13 |
| Editor_14.md | 4 | 9 | 10 |
| Editor_15.md | 3 | 9 | 11 |
| Editor_16.md | 2 | 11 | 7 |
| Editor_17.md | 2 | 9 | 8 |
| Editor_18.md | 5 | 9 | 7 |
| Editor_19.md | 0 | 3 | 9 |
| Editor_20.md | 1 | 7 | 6 |

### Hoch-Funde (Index)

- **EDT-201** – Level-Script-/GameInstance-Graph wird in einer Session jeden Frame vollständig serialisiert
- **EDT-202** – Netz-ID-Tabelle ist ein Vektor: O(N²) beim Seeding, lineare Suche pro Frame, Registry-Scan bei jedem Fehltreffer
- **EDT-301** – Content-Browser-Grid zeichnet alle Kacheln jeder Frame (kein Row-Clipping)
- **EDT-302** – DocsPanel zeichnet alle Sektionen einer Seite jede Frame
- **EDT-303** – `ContentBrowserPanel::render()` ist eine 4 400-Zeilen-Funktion mit ca. 60 Funktions-Statics
- **EDT-401** – `onProjectWriteRefused` ist nur unter `HE_HAVE_LIBSSH2` definiert, aber unbedingt benutzt
- **EDT-402** – `syncStructuralChanges()` baut jeden Frame das komplette Entity-Set neu auf
- **EDT-701** – Selektion: `roots()`/`addMany()` sind O(N²), `roots()` läuft pro Frame
- **EDT-801** – 16 Asset-Panels in 8 parallelen Handliste; Drift bereits sichtbar, dazu 16 Lookups je Tab und Frame
- **EDT-901** – Dropdown-Listen für String-Pins werden jeden Frame neu berechnet (Verzeichnisbaum-Scan, Header-Sniffing, JSON-Parse)
- **EDT-902** – `drawTypePicker` lädt bei geöffnetem Combo jeden Frame alle Klassen-Assets und parst deren Graph-JSON
- **EDT-903** – `GraphEditor::draw`: pro Knoten und Frame ~10 `std::function`-Aufrufe mit By-Value-Rückgaben, kein Viewport-Culling, Host-Lookups linear
- **EDT-1001** – `InputAssetPanel::render` scannt pro Frame den gesamten Content-Baum
- **EDT-1002** – Inline-Pin-Editor fragt pro Frame und Pin die Auswahllisten an (inkl. Verzeichnisscans)
- **EDT-1101** – Rekursiver Content-Scan (plus Datei-Header lesen) pro Frame, solange Combo/Popup/Submenü offen ist
- **EDT-1102** – Material-Canvas: Model-Adapter mit linearem `findNode` je Callback (O(N²)), Vektor-/String-Kopien und ~25 `std::function` pro Frame
- **EDT-1201** – `entityByUuid` ist ein linearer Scan mit `snprintf` + `std::string` pro Entity, obwohl `HorizonWorld::findByEntityId` O(1) existiert
- **EDT-1202** – `entity_list` serialisiert jede Entity vollständig (CBOR→JSON) *vor* Filter und Limit und weiter nach Erreichen des Limits
- **EDT-1401** – Partikel-Graph wird jeden Frame nach JSON serialisiert, um „Änderung“ zu erkennen
- **EDT-1402** – NotificationBar kopiert den gesamten Store (bis 200 Einträge mit je 6 Strings + `std::function`) in jedem Frame
- **EDT-1403** – Profiler-Timeline iteriert pro Frame alle Spans mehrfach und hasht Namen pro gezeichnetem Span
- **EDT-1404** – Outliner: Restarbeit O(N) pro Frame trotz Row-Clipping (u. a. `visibleRows` mit N Einträgen und Allokation)
- **EDT-1501** – Zwei `git`-Prozesse pro Frame im „Create branch“-Dialog
- **EDT-1502** – Spline-Panel: Polygon-Rasterisierung, O(res²)-Zählung und Kurven-Sampling in jedem Frame
- **EDT-1503** – Source Control „Tree view“: Baum und Zeilen jeden Frame neu aufgebaut, nicht geclippt
- **EDT-1601** – Collab-Strukturdiff baut jeden Frame ein Hash-Set aller Entities und scannt es dreimal
- **EDT-1602** – Orphan-Prüfung der Task-Links durchsucht pro Entity-Link die gesamte Welt mit `snprintf`
- **EDT-1701** – Sculpt-Brushes laufen pro Frame über das ganze Höhenraster
- **EDT-1702** – `typeNamePicker` scannt pro Feld und Frame beide Content-Roots
- **EDT-1801** – Palette scannt pro Frame den gesamten Content-Baum und liest jeden .hasset-Header
- **EDT-1802** – Hierarchie: `childrenOf()` (O(n)) pro Knoten und Frame, kein Row-Clipping
- **EDT-1803** – Zeichenliste des Canvas: Tiefenwalk mit linearem `find` pro Element und Frame
- **EDT-1804** – Eingebettete Widgets werden pro `WidgetRef` und Frame tief kopiert und neu gelayoutet
- **EDT-1805** – `EditorSelection::roots()` jeden Frame im Viewport, quadratisch in der Selektionsgröße
- **EDT-2001** – Cinematic-Preview rendert und schreibt/restauriert die Welt jeden Frame, auch im Stillstand

## Zusammenfassungen der Teilberichte

**Editor_1.md**: Die 16 Dateien (5283 Zeilen) sind insgesamt sauber und gut kommentiert; es gibt keine Funde mit Schwere Hoch. Die relevantesten Punkte sind (a) ein Logikfehler im Budget-Pfad von `AssetThumbnailCache::get`, der trotz vorhandener Disk-Cache-Datei eine synchrone GPU-Render-Runde auslösen kann, (b) ein nie invalidierter statischer Graph-Cache, der dem eigenen Kommentar ("Non-caching") widerspricht, und (c) mehrere Draw-/Frame-Pfade mit Dateisystemaufrufen oder Allokationen pro Frame (`AudioEditorPanel` Import-Zeile, `AppMetadataRows`, Asset-Autosave-Sammlung). Funde: 0 Hoch, 7 Mittel, 11 Niedrig. Alle Zeilenangaben wurden gegen die gelesenen Dateien geprüft; Messdaten liegen nicht vor, Schätzungen sind als solche gekennzeichnet.

---

**Editor_2.md**: Der Slice (11 Dateien, ca. 10.000 Zeilen) ist funktional sorgfältig und sehr gut kommentiert, aber `CollabController` ist ein 4667-Zeilen-Gott-Objekt (davon 1215 Kommentarzeilen in der .cpp und 908 von 1682 Zeilen in der .h), und an mehreren Stellen laufen pro Frame Operationen, die mit der Szenen- bzw. Dokumentgröße skalieren. Die schwersten Funde sind die lineare Netz-ID-Tabelle (`m_netIds`: O(N²) beim Seeding, lineare Suche pro Frame und pro Remote-Nachricht, Negativ-Treffer scannen die ganze Registry) und die Voll-Serialisierung des Level-Script-/GameInstance-Graphen pro Frame in einer Session. Dazu kommen blockierende Datei-I/O auf dem UI-Thread, die Datei vor dem Typ-Gate liest, Syscalls pro Frame im Collaboration-Fenster und viel Duplikation (Adapter, Batch-Approve/Deny, Teilnehmer-Lookup). Der Cinematic-Streifen ist unauffällig (Eigenlogik sauber getrennt und testbar); nur Row-Clipping fehlt und `draw()` ist sehr lang. Funde: 2 Hoch, 6 Mittel, 7 Niedrig. Nichts davon wurde gemessen; Schätzungen sind als solche gekennzeichnet.

---

**Editor_3.md**: Der Slice (19 Dateien, ca. 10 950 Zeilen) ist funktional sauber und sehr ausführlich kommentiert; die kleinen Logikklassen (`CollabSettle`, `CollabUndo`, `DeferredSlotFreeList`, `DocsLibrary`) sind ordentlich und getestet. Das Gewicht liegt bei zwei Panels: `ContentBrowserPanel.cpp` (5 062 Zeilen, davon `render()` ca. 4 400 Zeilen in einer Funktion) zeichnet Kacheln, Baum und Dialoge **ohne Clipping** und mit vielen Per-Frame-Allokationen und Dateisystemaufrufen. `DocsPanel` zeichnet jede Frame **alle** Sektionen einer Seite (der Node-Reference mit ca. 300 Sektionen) und führt die Volltextsuche jede Frame neu aus. Außerdem ein echter Defekt: `CppClassEditorPanel::reloadFromDisk` lädt nie neu. Insgesamt 20 Funde: 3 Hoch, 8 Mittel, 9 Niedrig. Alle Aufwandsangaben sind Schätzungen; es gab keine Messdaten.

**Editor_4.md**: `EditorApplication.cpp` (14 759 Zeilen) besteht zu rund einem Drittel (Zeilen 5321–10075) aus `dumpFrameHeadless()`, einer 4 750 Zeilen langen Sammlung von HE_DUMP_*-Testszenen. Zu einem weiteren Siebtel besteht sie aus `OnRender()` (3015–5143, 2 129 Zeilen) und `setupMcpTools()` (10414–11223). Die Frame-Schleife ist überwiegend sauber gegated (Dirty-Checks, statische getenv-Caches, Fenstertitel nur bei Änderung). Die echten Hot-Path-Kosten stecken in den Collab-/Team-Pfaden: Sie laufen jeden Frame und sind O(Entities) bzw. machen Syscalls und String-Kopien. Dazu kommen die Pro-Aufruf-Kosten von `makeContext()` und `callApi`. Gefunden wurden 21 Punkte: 2 Hoch, 11 Mittel, 8 Niedrig. Davon ist EDT-401 ein echter Build-Fehler für Builds ohne libssh2 (nicht durch Kompilieren verifiziert), alles andere ist Struktur oder Performance. Es wurde nichts gemessen. Alle Aufwandsangaben sind Schätzungen.

---

**Editor_5.md**: Der Slice besteht überwiegend aus Headern, Daten und kleinen, gut kommentierten Hilfsmodulen (Kamera, Dock-State, Command-Gateway, Guides-Bauteile). Es gibt keine eigentlichen Per-Frame-Hot-Paths in diesen Dateien selbst; die belegbaren Kosten liegen in dem, was die Header auslösen (`AppContext`-Neuaufbau, Struktur-Diff, Typ-Cache-Kaltstart) und in `EditorCommands.cpp` (unnötige Serialisierung, O(n)-UUID-Suche mit String-Bau pro Entity). Insgesamt 11 Funde: 0 Hoch, 5 Mittel, 6 Niedrig. Alle Zeilenangaben wurden gegen den Ist-Stand geprüft; Aussagen zu Dateien außerhalb des Slice (EditorApplication.cpp, EditorUI.cpp, TaskBoardPanel.cpp, ContentBrowserPanel.cpp) sind als solche gekennzeichnet und gezielt per grep/Ausschnitt verifiziert, nicht vollständig gelesen.

**Editor_6.md**: Der Slice ist insgesamt sauber und gut kommentiert. `EditorHelp.cpp` besteht zu ~9350 von 9800 Zeilen aus reinen Daten (1776 `Entry`, 155 `Area`, 16 `PanelTopic`, 41 Komponenten-Scopes); der Code-Teil (ab Zeile 9618) ist klein und bereits einmal auf einen Hash-Index umgestellt (`findKey`, Zeilen 9634-9651). Lookups sind hover-gated (`helpForLabel`/`helpForKey`), es gibt keine doppelten Schlüssel (per Skript geprüft: 1776 eindeutig, alle Einträge haben eine `Area`). Die relevanten Funde liegen bei `EditorMultiEdit` (Vollserialisierung pro Drag-Frame und pro Member, Row-Marks/Summary jeden Frame neu gebaut) und bei `EditorInput::detectTrackpad` (SDL-Aufruf mit Allokation pro Frame). Dazu ein sichtbarer Defekt in `EditorReference` (`##wx`-Suffix landet im Seitentitel). Insgesamt 12 Funde: 0 Hoch, 4 Mittel, 8 Niedrig.

**Editor_7.md**: Der Slice ist insgesamt sauber und gut kommentiert: die offensichtlichen Hot-Path-Fallen sind schon behandelt (Hover-Gate vor der Help-Suche in `EditorRowWidgets`, `favoritesContain` ohne Allokation, Cache der Reduce-Motion-Abfrage, einmaliger `capturePre()` pro Drag). Der einzige echte Skalierungsfehler ist die Selektionslogik (`EditorSelection::roots/contains/addMany`, O(N²) und `roots()` läuft pro Frame im Viewport). Danach folgen die Doppelpflege Settings-Katalog gegen Settings-Panel (Wertebereiche sind bereits gedriftet, Restore Defaults ist eine handgepflegte, lückenhafte Liste), der Gruppen-Gizmo mit pro-Frame-Aufbau über alle Mitglieder, der Footer-Status mit Strings/Vektoren pro Frame und die Shortcut-Lookups über String-IDs. Der Rest sind Kleinigkeiten. Funde: 1 Hoch, 5 Mittel, 6 Niedrig. Messdaten liegen nicht vor, alle Zahlen sind Schätzungen.

---

**Editor_8.md**: Der Slice (EditorUI.cpp 4438 Zeilen, ExportDialogPanel.cpp 1999, GitController/-CloneDialog, EditorWidgets, EditorUndo, ViewportNav, Footer-Bars, Kleinteile) ist funktional sorgfältig und gut kommentiert, leidet aber an zwei strukturellen Problemen: (1) Pflege-Listen, die in vielen parallelen Kopien gehalten werden (16 Asset-Panels in 8 Dispatch-Stellen, 13 Fenster-Toggles in 6 Listen, Mac- und ImGui-Menü) und bereits driften, (2) monolithische Funktionen (`renderEditor` ~3000 Zeilen, `startExport` ~780 Zeilen). Pro-Frame-Kosten sind meist klein, aber vermeidbar: String-Bau je Tab/Slot/Kachel, ungeclippte Listen in Picker-Popup und Clone-Dialog, 16 Hash-Lookups je Tab und Frame. Funde: 1 Hoch, 11 Mittel, 11 Niedrig (23 gesamt). Nichts davon wurde gemessen; Schweregrade beruhen auf Code-Lesung.

**Editor_9.md**: Der Slice ist insgesamt sauber und gut kommentiert; die Logik-Dateien (HcExtract, HcFallbackReport, HcGraphClipboard, GraphViewStore, GitHubSignInView) sind klein, ImGui-frei bzw. testbar und haben kaum Befunde. Die relevanten Probleme sitzen in den Per-Frame-Pfaden: (1) `HcEditorUtil` ruft in Draw-Callbacks Dateisystem-Scans und Asset-/JSON-Loads **jeden Frame** auf (Pin-Dropdowns, Typ-Picker), (2) `GraphEditor::draw` ruft pro Knoten und Frame ein Dutzend `std::function`-Callbacks mit by-value-Rückgaben (Vektoren/Strings) ohne Viewport-Culling, gegen einen Host mit linearem `findNode` (O(N²)), (3) mehrere Panels kopieren pro Frame `TypeRegistry`-Definitionen und `signatureOf`-Vektoren. Dazu kommt der Exec-Listener in `HcExecTrace`, der pro ausgeführtem Knoten Strings kopiert und hasht. Funde: 3 Hoch, 8 Mittel, 7 Niedrig (18 gesamt).

---

**Editor_10.md**: Der Slice ist insgesamt sauber geschrieben und gut kommentiert; die reine Logik (HcRename, HcPull, HcWatch, InputMappingModel, HeprojRegistration) ist ImGui-frei und testbar. Die echten Kosten liegen in zwei Draw-Pfaden, die pro Frame Dateisystem-Scans ausführen (`InputAssetPanel` und die Inline-Pin-Editoren der HorizonCode-Canvas), sowie in mehreren Per-Frame-Neuaufbauten (Klassen-Graph-Parse in `HcPullUi`, Watch-Fenster, `buildModel`-Lambdas mit linearem `findNode`). Daneben gibt es Duplikation (PinType→Name in fünf Kopien, davon eine tot und falsch; Node-Spawn-Code in `HcGraphHost` fünffach; drei identische Tabellen in `InputAssetPanel`) und einen kleinen Logikfehler (Capture wird beim Entfernen von Pad-/Maus-Zeilen nicht abgebrochen). Funde: 2 Hoch, 9 Mittel, 9 Niedrig. Alle Zeilenangaben wurden gegen den aktuellen Stand geprüft; Messdaten liegen nicht vor, Aufwandsschätzungen sind als solche markiert.

---

**Editor_11.md**: Slice 11 umfasst 10 Dateien (10 975 Zeilen): `InputMappingModel.h`, `InspectorPanel.{h,cpp}`, `InspectorPrefabKeys.cpp`, `LevelScriptPanel.{h,cpp}`, `MacMenuBar.{h,mm}`, `main.cpp`, `MaterialEditorPanel.cpp`. Der Code ist insgesamt sorgfältig kommentiert; echte Probleme sind selten Algorithmusfehler, sondern Arbeit im Draw-Pfad, die pro Frame wiederholt wird: rekursive Dateisystem-Scans in offenen Popups/Combos, ein quadratisches Model-Adapter-Muster im Material-Canvas, Lua-/Listen-Abfragen im Details-Panel und komplette Komponenten-Auflistungen pro Baumknoten. Strukturell dominieren eine 2 900 Zeilen lange Funktion (`renderForImpl`) mit 41-fach kopiertem Entfernen-Muster und drei parallel gepflegte Komponentenlisten. Es gibt 22 Funde: 2 Hoch, 9 Mittel, 11 Niedrig (nach Schweregrad sortiert, mit Ausnahme von EDT-1114, das als Mittel nachgeordnet steht). Alle Funde stammen aus Draw()-Code (Editor-Hot-Path), die Schweregrade berücksichtigen, ob der Pfad immer oder nur bei geöffneter Sektion/Popup läuft. Gemessen wurde nichts; wo die Wirkung von Datenmengen abhängt, ist das vermerkt.

**Editor_12.md**: Der Slice ist die MCP-Bridge des Editors (Socket, JSON-RPC, Tool-Registry, 11 Tool-Familien) plus ein paar Footer-/Viewport-Gizmos. Nichts davon läuft pro Frame im Draw-Pfad außer `McpBridge::update` (leer, solange niemand verbunden ist), `McpStatusBar` und die Kamera-Gizmos; der Hot-Path ist hier stattdessen der *Tool-Aufruf auf dem UI-Thread* (Bridge.h: „one call, on the main thread"). Die Qualität ist hoch (gute Fehlertexte, saubere Trennung ImGui-frei/testbar), die Probleme sind (a) zwei O(N)-Scans pro Aufruf in den Entity-Tools, die mit Batch/Szenengröße multiplizieren, (b) mehrfach duplizierte Gate-/Helferlogik über ≥11 Tool-Dateien und ein 1592-Zeilen-Header, der über `EditorApplication.h` in 78 Übersetzungseinheiten gezogen wird, (c) Frame-Thread-Dateisystemwalks, die entgegen den Kommentaren nicht durch das `limit` begrenzt sind. Funde: 2 Hoch, 8 Mittel, 7 Niedrig. Alle Funde sind statisch aus dem Code belegt; es wurde nichts gemessen oder gebaut.

---

**Editor_13.md**: Der Slice (10 Dateien, 10 464 Zeilen) sind die MCP-Tool-Handler fuer Input, Material, Particle, Prefab, Scene, Screenshot, Sequence, Settings, TaskBoard und Terrain. Nichts davon laeuft pro Frame im Draw-Pfad; jeder Handler ist ein einmaliger Aufruf eines externen Clients. Die Schwere ist deshalb durchgehend begrenzt: 0 Hoch, 9 Mittel, 13 Niedrig. Die Dateien sind sorgfaeltig kommentiert und konsistent aufgebaut. Die groesste Pflegelast ist Duplikation: dieselben Gate-Ketten (Play/Lock/Dirty/Engine), dieselben kleinen Helfer (`uuidJson`, `vec3Arg`, `failFor`, `lowered`) und dieselben Validierungsbloecke stehen in mehreren Dateien. Die groesste Laufzeit steckt in den Terrain-Tools (Komplettkopie plus CBOR/JSON/Base64-Rundlauf pro Brush-Dab) und in `commitGraph` (doppelte Codegen pro Material-Graph-Edit). Keine Messdaten vorhanden; Laufzeitschaetzungen sind gekennzeichnet.

**Editor_14.md**: Der Slice (34 Dateien, ~10,8k Zeilen) ist insgesamt sauber und gut kommentiert; der Outliner hat bereits Row-Clipping, die Notification-Schicht ist logisch sauber von ImGui getrennt. Die Hauptprobleme sind Arbeit, die pro Frame im UI-Thread läuft, obwohl sie nur bei Änderungen nötig wäre: ein JSON-Serialisieren des ganzen Partikel-Graphen pro Frame, eine tiefe Kopie des Notification-Stores pro Frame (auch bei geschlossenem Flyout), O(Spans)-Schleifen pro Frame im Profiler-Timeline und O(N)-Restarbeit im Outliner trotz Clipping. Dazu kommen ungeclippte Listen (Profiler-Scopes, Play-Report), ein `exists()`-Syscall pro bekanntem Projekt und Frame im Project Hub und einige Duplikate (Property-Validierung in den MCP-Widget-Tools, Preset-Listen, Farben). Funde: 4 Hoch, 9 Mittel, 10 Niedrig. Alle Zeilenangaben gegen den Ist-Stand geprüft; Laufzeitannahmen sind als Schätzung markiert, Messungen wurden nicht durchgeführt.

---

**Editor_15.md**: Der Slice (32 Dateien, ca. 10.860 Zeilen) ist insgesamt sauber gebaut und sehr gut kommentiert. Die meisten Panels cachen bereits sinnvoll (Statusmodell der Source-Control-Änderungen per `generation`, Root-Motion-Pfad, Szenenliste, Preview-Cache im Report-Dialog). Die relevanten Funde konzentrieren sich auf drei Muster: (1) Arbeit pro Frame, die nur bei Zustandsänderung nötig wäre (Prozessstart, Polygon-Rasterisierung, O(Auflösung²)-Zählung, Baumaufbau); (2) Listen ohne Clipping bzw. mit Heap-Allokation pro Zeile und Frame; (3) wiederholte Boilerplate (Seitenrümpfe in ProjectSettings, `stateByContentPath`, Gruppen-Strings im Sequencer). Funde: 3 Hoch, 9 Mittel, 11 Niedrig. Es wurde nichts gebaut oder gemessen; Aufwandsschätzungen sind Schätzungen.

**Editor_16.md**: Der Slice (24 Dateien, ca. 9.200 Zeilen: StaticMeshEditorPanel, StreamingDebugView, StructuralSync, TaskBoard*, Team*, TerrainSync) ist insgesamt sauber geschrieben: Datenmodell (`TaskBoard`) ohne ImGui, Row-Clipping in den Task-Karten, Throttling in `TerrainSync`, klare Trennung Controller/View. Die Kosten liegen fast ausschließlich in **Pro-Frame-Arbeit, die einen Zustand neu berechnet statt ihn zu cachen**: der Strukturdiff für Collab (O(Entities) mit Heap pro Frame), der Footer-Task-Zähler (sortiert pro Frame alle offenen Tasks und ruft `getenv`), die Team-Footer-Cluster (3× `onlineOutsideSession()` pro Frame, pro-Zelle-Rechtecke für Avatare), `TeamSyncController::update` (5 String-Konkatenationen pro Frame) und die Orphan-Prüfung der Task-Board-Links, die pro Entity-Link den ganzen Weltinhalt per `snprintf` durchsucht. Zusätzlich Wartbarkeit: duplizierte Panelliste, mehrfach vorhandener Case-insensitive-Namensvergleich, Test-Instrumentierung im Produktiv-Drawpfad. `TaskBoardTeam` ist im Editor noch nicht verdrahtet (laut `docs/taskboard-team-interop-2026-10-15.md` geplant), die dortigen Pro-Frame-Kosten sind daher nur prospektiv.

Funde: 2 Hoch, 11 Mittel, 7 Niedrig. Alle Laufzeitangaben sind Schätzungen aus dem Code, nicht gemessen.

---

**Editor_17.md**: Der Slice (16 Dateien, 8698 Zeilen) ist insgesamt sauber und gut kommentiert. Die relevanten Probleme sitzen an drei Stellen: (1) die Sculpt-Brushes in `TerrainTools.cpp` iterieren pro Frame über das gesamte Höhenraster (bis 1025², Smooth kopiert zusätzlich den ganzen Vektor); (2) `TypeAssetPanel` scannt pro Struct-/Enum-Feld und pro Frame rekursiv beide Content-Roots samt Header-Reads (`listAssets`), obwohl die Combo meist geschlossen ist; (3) mehrere Panels erzeugen pro Frame vermeidbare Kopien, Allokationen und volle Grid-Scans (ToolSetup-Probes, Water-`wetCells`, Tutorial-Sampling, Theme-Styleliste). Dazu kommen Duplikation (Brush-Kernel, `track`-Lambdas, Asset-Tab-Boilerplate) und einige Kommentar-/Encoding-Fehler.
Funde: 2 Hoch, 9 Mittel, 8 Niedrig (19 gesamt). Keine Quelldateien geändert.

---

**Editor_18.md**: Der Slice ist funktional gut durchdacht und ausführlich kommentiert, aber `UIEditorPanel.cpp` (7731 Zeilen, vier Zuständigkeiten in einer anonymen Namespace) rechnet und allokiert im Designer jeden Frame Dinge, die sich nur bei Edits ändern: ein Verzeichnisbaum-Scan samt Header-Lesen für die Palette, ein O(n²)-Hierarchiebaum ohne Clipping, ein O(n·Tiefe·n)-Aufbau der Zeichenliste (lineares `UIWidgetTree::find`) und tiefe Kopien eingebetteter Widgets pro `WidgetRef` und Frame. Im Viewport läuft `EditorSelection::roots()` (quadratisch) jeden Frame, und auf macOS wird pro Frame ein kompletter Szenenwalk nur für ein Menü-Label gemacht. `UITimelineMath.h`, `ViewportActions.*` (außer `anyHidden`) und `UndoHistoryPanel` sind sauber; dort nur Kleinigkeiten. Funde: 5 Hoch, 9 Mittel, 7 Niedrig (21 gesamt). Alle Laufzeit-Schätzungen sind ohne Messdaten und als solche markiert.

---

**Editor_19.md**: Der Slice ist insgesamt sauber und gut kommentiert. Der einzige echte Hot Path ist `ViewportToolbar::render` (läuft jeden Frame). Die teure Hilfe-Suche pro Zelle ist bereits per Hash-Index gelöst (`EditorHelp.cpp:9634`, Perf-Audit 3.2), sodass keine schweren Per-Frame-Kosten übrig sind. Die Funde betreffen vor allem eine 460-Zeilen-Funktion mit doppelt gepflegter Layout-Arithmetik, wiederholte Textmessung pro Frame, eine handgepflegte Duplikatliste der Show-Flags und schwere Header. `ViewportPick` läuft nur bei Klick (nicht pro Frame) und ist dort korrekt; ein möglicher Randfall (ungültige Entity im Snapshot) ist als "unsicher" markiert. Funde: 0 Hoch, 3 Mittel, 9 Niedrig (EDT-1901 bis EDT-1912), dazu vier Niedrig-Befunde zur CMakeLists im Abschnitt "Zusatzprüfung".

**Editor_20.md**: Der Code ist durchgehend sauber kommentiert und die Datenmodelle sind gut getrennt (AudioWave/AudioMix ohne AppContext, Peak-Pyramide, Bracket-Muster im Cinematic-Preview). Die Funde liegen vor allem in per-Frame-Arbeit ohne Änderungserkennung (Cinematic-Preview, EQ-Kurve, Bone-Mask-Baum, Build-Log), in dupliziertem Panel-Boilerplate (BlendSpace/BoneMask/Cinematic) und in einer sehr langen Funktion (`AudioWave::draw`, ca. 540 Zeilen). Ein echter Lesefehler auf einen ungültigen Zeiger steckt in `BoneMaskPanel::jointRow`. Anzahl Funde: 1 Hoch, 7 Mittel, 6 Niedrig. `CameraBookmarks.*` ist unauffällig (kleine, geschlossene Datenklasse, Encode/Decode robust). Nichts davon wurde gemessen; Schweregrade sind Schätzungen aus dem Code.

---

## Cross-Modul-Hinweise

**Editor_1.md**

- `src/HE_Scene/src/AudioEngine.cpp:594-612`: `busVoiceCount` ist O(Stimmen) pro Aufruf, `applyBusConfig` setzt jedes Mal alle Bus-Lautstärken neu (vgl. EDT-114). Ein `voiceCountsByBus()` (eine Iteration) wäre die passende Engine-API.
- `src/HE_Core/include/ContentManager/ContentManager.h:89`: `writeAssetTo(RuntimeAsset&, ...)` nimmt non-const, was EDT-106 (PCM-Kopie) erzwingt. Const-Überladung prüfen.
- `src/HE_Core/include/Types/Enums.h:85-113`: kein `Count`-Sentinel; `assetTypeFromName` und mehrere Switches hängen am letzten Enumerator (EDT-116).
- GraphEditor-Modell (`GraphEditor.h`, nicht Teil dieses Slices): `Model::pins`/`nodeIds`/`links` liefern Vektoren per Wert über `std::function`, was alle Aufrufer (Material/HorizonCode/Particle/ASM) pro Frame zu Allokationen zwingt (vgl. EDT-109). Eine `span`/Callback-basierte API wäre projektweit effizienter.
- FNV-1a-Duplikate in `src/HE_Editor/CollabDocSync.cpp`, `CollabPresenceBar.cpp`, `CollabController.cpp`, `TerrainSync.cpp`, `SceneFileStamp.h`, `src/HE_Core/include/Hpak/HpakFormat.h`, `src/HE_Core/src/Hpak/ProjectExporter.cpp`, `src/HE_Scene/src/Water*.cpp`, `RopeTrailSystem.cpp` (nur über Konstantensuche gefunden, nicht gelesen).
- `AppMetadataRows::previewSlot` (AppMetadataRows.cpp:103-109) hält `Preview::handle` als statische GPU-Textur; ein Projekt-/Renderer-Wechsel oder das Beenden gibt sie nicht frei (siehe Unsicher), ähnlich wie `AssetThumbnailCache::shutdown()` es für Tiles tut.

**Editor_2.md**

- `src/HE_Editor/EditorApplication.cpp:5061-5070`: Pro Frame mit selektierter Entity in einer Session wird `TerrainSync::serializeWithoutGrids(...)` ausgeführt (Heap-Vektor aller Komponenten), dann in `publishComponents` FNV-gehasht (CollabController.cpp:2665) und `netIdFor` (EDT-202) aufgerufen. Besser: nur bei geänderter Entity (Dirty-Flag/Revisionszähler der Komponenten oder `EditorCommands`-Hook) serialisieren. Unsicher/messen: Kosten hängen von Komponentenzahl ab.
- `src/HE_Editor/EditorApplication.cpp:4663-4667`: Presence-Selektion fremder Peers ruft pro Frame `entityForNetId` je selektierter Entity (siehe EDT-202, Negativ-Treffer scannen die Registry).
- `src/HE_Editor/EditorApplication.cpp:10241-10248, 10339-10343`: Belegstellen für EDT-201; dort ist auch die richtige Stelle für einen Revisionszähler/ein Throttling.
- `src/HE_Net/src/Socket.cpp:1577-1609`: `socketOwnsAddress` enumeriert bei jedem Aufruf alle Interfaces; für Aufrufer mit Frame-Frequenz fehlt ein Cache (siehe EDT-205). `SecureTransport::sessionFingerprint` (SecureTransport.cpp:654) rechnet bei jedem Aufruf einen HMAC.
- `src/HE_Editor/EditorApplication.h:10`: bindet `CollabController.h` ein (siehe EDT-208).
- `src/HE_Editor/BuildProgressDialog.cpp:546-615`: nutzt das Muster "ein Modell pro Frame, von `FooterWidth` und `DrawFooter` geteilt", das `CollabActivityBar` und vermutlich `CollabPresenceBar` (anderer Slice, nicht gelesen) nicht nutzen.
- `src/HE_Net/include/Net/CollabSession.h`: `sendAsset(const AssetUpdate&)` nimmt die Bytes per const-Referenz; ob intern nochmals kopiert wird (Frame-Aufbau/Chunking), wurde nicht geprüft.

**Editor_3.md**

- `src/HE_Core/src/ContentManager/ContentManager.cpp:1015-1029` (`isEngineDefaultPath`/`isEngineOverridePath`) und `:977-981` (`isEngineContentDevMode`: `getenv` + `atoi` je Aufruf) sind die eigentliche Ursache der Syscalls in EDT-305. Lexikalische Präfixprüfung und gecachte Dev-Mode-Flag wären auch für `ContentManager::resolveAbsolutePath` (mehrfach `std::filesystem::exists` pro Aufruf, `:983-1000`) sinnvoll.
- `src/HE_Editor/GitController.cpp:436-466` (`toRepoRelative`): normalisiert bei jedem Aufruf beide Pfade (`normaliseSlashes(absolutePath)` und `normaliseSlashes(s.root)`); `s.root` könnte einmal pro Status-Refresh normalisiert gehalten werden. Relevant, weil es pro Kachel und Baumknoten läuft.
- `src/HE_Editor/EditorHelp.cpp:9717-9736` (`panelForTopic`): linearer Scan über `kPanelTopics` mit String-Vergleichen, von DocsPanel pro Sektion und Frame aufgerufen (EDT-302). Ein `unordered_map<string_view, ...>` beim ersten Zugriff würde reichen.
- `AssetThumbnailCache::get` und `EditorAssetTypeCache::assetTypeOf/is` (nicht in diesem Slice) werden pro Kachel und Frame mit dem vollen Pfad-String aufgerufen; ihre Lookup-Kosten (Hash des Pfads, ggf. Mutex) entscheiden mit, wie viel EDT-301 spart.
- `HE::Net::Participant::avatar` (Header außerhalb des Slice): ein vorberechneter Hash würde EDT-312 lösen.
- `HcNodeReference.cpp:225`, `EditorReference.cpp:151`, `EditorGuides.cpp:614`: alle drei rufen `appendPage` je Seite auf (EDT-313).

**Editor_4.md**

- **HE_Editor/ViewportOverlays.cpp** (`appendColliderWireframes`, `appendSelectionMarkers`): läuft per Frame über alle Collider/Selektionen (Aufruf Z. 4344–4357). Dort auf Caching (nur bei Änderung neu bauen) prüfen. `show.colliders` ist aus dem Menü schaltbar, aber an.
- **HE_Editor/ViewportPanel.cpp:91–** (`appendGroundGrid`): Das Gitter ist bereits gecacht (`s_grid`, nur bei Kameraänderung neu). Pro Frame kommen aber zwei Kopien der ~2 000 Linien hinzu: `out.append(s_grid.lines())` und `m_lastDebugLines.assign(...)` (Z. 4744), dann kopiert vermutlich `SetDebugLines` noch einmal in der Backend-Schicht. Billig, aber ein "Static-Lines-Layer" im Renderer würde alle drei sparen (HE_Rendering).
- **HE_Core/ContentManager** (`pollHotReload` alle 1,5 s im UI-Thread, Z. 3435; periodisches `refreshContentFolder` auf einem `std::async`-Thread alle `ContentBrowserRefreshRate` Sekunden, Z. 3342): Der Hot-Reload-Poll stat't vermutlich alle geladenen Assets. Ein File-Watcher (inotify/ReadDirectoryChangesW/FSEvents) würde das ersetzen. Messen auf großen Projekten.
- **HE_Editor/TerrainSync.cpp:297–303** (`serializeWithoutGrids`): `GridsAside` verschiebt pro Aufruf Raster. Wenn die Raster per Move zur Seite gelegt werden, ist es billig, sonst (Kopie) wäre EDT-404 teurer als beschrieben.
- **HE_Editor/EditorUI.cpp, `EditorUI::unsavedAssetPaths()`, `tabHasUnsavedEdits`**: werden im `updateAssetCollabSync`-Schleifenkörper pro Tab und Frame aufgerufen (Z. 10252, 10341), Kosten dort prüfen.
- **HE_Scene/EngineApi.cpp**: `HE::api::fs::setSandboxRoot`/`perm::set` sind globale Setter und werden pro EngineCall gerufen (EDT-407). Ein `HE::api::Ctx`, der die Host-Funktionen als Zeiger statt als `std::function`-Kopien trägt, spart die fünf Kopien an jeder Aufrufstelle (Lua/Python/HC/MCP).
- **HE_Editor/EditorConfig.h**: Eine Feldtabelle (EDT-411) kann zugleich die Quelle für Preferences, MCP-Settings und die Hilfe-Audits sein.

**Editor_5.md**

- `src/HE_Editor/ContentBrowserPanel.cpp:1838-1856`: Beim Filtern/Suchen wird pro Datei und Frame `std::string name = f->name` kopiert und per `tolower` umgewandelt (Zeilen 1844-1845), und der Typfilter läuft über den gesamten Baum, nicht nur über sichtbare Zeilen (siehe EDT-504). Ein vorberechneter Kleinschreib-Name pro Datei (im Scan) würde Kopie und Schleife sparen; gehört in den Content-Browser-Slice.
- `src/HE_Editor/TaskBoardPanel.cpp:547-550,579,603`: nutzt `entityByUuid(…, HE::uuidToHex(l.id))` (siehe EDT-503), also Formatierung auf beiden Seiten.
- `src/HE_Editor/EditorApplication.cpp:10218-10221`: toter Code (`root`, `(void)root`) in `updateAssetCollabSync`.
- `src/HE_Editor/EditorUI.cpp:4119ff`: Tab-Dispatch per Prädikat-Kette (EDT-506).
- `src/HE_Editor/EditorApplication.cpp:11400-11420`: `syncStructuralChanges` (EDT-501) gehört in den Slice, der diese Datei abdeckt; dort die Umsetzung koordinieren.

**Editor_6.md**

- `src/HE_Scene/include/HorizonScene/SceneSerializer.h:135-136`: `serializeEntityComponents` hat keinen Komponentenfilter; für EDT-601 wäre eine Variante `(world, entity, span<const char*> componentKeys)` die eigentliche Lösung (hier außerhalb des Slice).
- `src/HE_Editor/InspectorPanel.cpp:~232-233` (Mehrfachauswahl): `const std::vector<std::string> common = s_multi.common;` und `partial` werden pro Frame kopiert, obwohl der Cache nur über `refreshMultiSelection` verändert wird und `renderFor` ihn nicht anfasst (eine Referenz würde reichen, muss aber gegen Refresh während der Iteration abgesichert werden – `s_multi.stale = true` wird erst nach der Schleife wirksam).
- `src/HE_Editor/ViewportPanel.cpp:1688-1712` (Marquee-Rahmen): pro Frame ein neues `std::unordered_set<uint32_t> seen` und `std::vector<Entity> found`, `meshBounds`-Lookup je Objekt; `EditorMarquee::encloses` (EditorMarquee.cpp:27-48) multipliziert pro Ecke `model` und dann `viewProj` (zwei Matrix-Vektor-Produkte). Vorab `mvp = viewProj * model` wäre ein Produkt je Ecke, ändert aber Float-Rundung minimal (Randfälle im Rahmen); nicht ohne Messung. Allokationen per `reserve` bzw. wiederverwendeten statischen Puffer vermeiden.
- `src/HE_Editor/EditorPanelState.h`: `AssetPanelState::operator[]/find` nehmen `const std::string&`; Aufrufer mit `const char*`/`string_view` bauen pro Frame ein temporäres `std::string`. Heterogener Lookup (`std::map<std::string, T, std::less<>>`) würde das vermeiden – Panels in anderen Slices prüfen.

**Editor_7.md**

- `src/HE_Editor/ViewportPanel.cpp:1535-1560` ruft jeden Frame `ctx.selection.roots(...)`, dann `erase(remove_if(... all_of<EditorLockComponent>))`; zusätzlich Collab-Filter. Siehe EDT-701/702. Der Vektor sollte pro Selektionsrevision gecacht oder wiederverwendet werden.
- `src/HE_Editor/ViewportActions.cpp:88,103,180,244,277` und `EditorApplication.cpp:11740` rufen `roots()` pro Aktion, profitieren von EDT-701 automatisch.
- `src/HE_Editor/ShortcutsPage.cpp:147` (`conflicts(a.id)` pro Zeile pro Frame), siehe EDT-706.
- `src/HE_Editor/SelfUpdateDialog.cpp:180-181,188` ruft `su.snapshot()` mehrfach pro Frame (je ein Mutex-Lock und Kopie von fünf Strings). Einmal pro Frame in eine lokale Variable lesen (Niedrig).
- `src/HE_Editor/EditorUI.cpp:3774` (`tabMark` + `CalcTextSize((tab.label + " *"))` pro Tab pro Frame, baut Strings) und `tabHasUnsavedEdits(tab.assetPath)` pro Tab pro Frame. In Slice des EditorUI prüfen.
- `src/HE_Editor/McpToolRegistry.h:1370` und `EditorSettingsCatalog.h:10-11` behaupten, der Katalog spiegle die Widget-Bereiche; stimmt nicht (EDT-703).
- `src/HE_Editor/EditorHelp.cpp:9634` (`findKey`) wird von jeder Toolbar-Zelle pro Frame aufgerufen (EDT-707); Implementierung (Tabelle sortiert? Hash?) im Slice von EditorHelp bewerten.

**Editor_8.md**

- `src/HE_Editor/EditorRowWidgets.cpp:429-444`: `Row::MixedScope` baut im Konstruktor eine neue `unordered_map` mit `fieldKey()`-Strings (Allokation je Eintrag) und `mixedMask` ruft pro Zeile `fieldKey(label)` (String-Bau) auf; bei Mehrfachauswahl läuft das für jedes Row-Widget jeden Frame. Normalisierte Keys einmal pro Selection-Änderung cachen.
- `src/HE_Editor/ContentBrowserPanel.cpp:834, 1686, 2045`: Aufrufer von `entryForAbsolutePath/folderHasChanges` (EDT-809); der Browser sollte den Repo-relativen Schlüssel pro Kachel beim Aufbau der Tile-Liste einmal bilden statt je Frame.
- `src/HE_Editor/ProjectHubPanel.cpp:303-` Kopie des Windows-Ordnerdialogs (EDT-814).
- `src/HE_Editor/HcEditorUtil.cpp:93` (`listAssets`): wird vom Picker beim Öffnen und vom Export (`ExportDialogPanel.cpp:1366, 1420`) aufgerufen; prüfen, ob es `EditorAssetTypeCache` nutzt und ob eine Typ-Index-Abfrage am `ContentManager` den Header-Sniff ersetzt.
- `src/HE_Editor/CinematicPanel.*`: hat keine `appendSnapshots`/Recovery-Schnittstelle, `AssetAutosave` deckt Cinematics damit vermutlich nicht ab (EDT-801).

**Editor_9.md**

- `src/HE_Core/include/GraphCommon/GraphModel.h:45-57`: `findNodeById` ist ein linearer Scan; wird von `HorizonCode::Graph::findNode` (`src/HE_Core/src/HorizonCode/HorizonCode.cpp:1228`) und damit von allen `Model`-Callbacks in `HcGraphHost.cpp:684-699` pro Knoten/Frame genutzt (Teil von EDT-903). Ein Index (id→Position) im Graph-Container oder eine Frame-Map im Host wäre die zentrale Abhilfe auch für Material-/Partikel-/Animator-Hosts.
- `src/HE_Editor/HcGraphHost.cpp:899-905` und `src/HE_Editor/UIEditorPanel.cpp:6523` (Host-`paramChoices`) liefern die Listen für EDT-901 ebenfalls pro Frame; dort prüfen, ob eigene Scans (Widget-Animationen/Elemente) stattfinden.
- `src/HE_Editor/EditorWidgets.cpp:121-190`: Asset-Picker mit Scan beim Öffnen (korrekt), aber Filter/Label pro Frame ohne Clipper (EDT-910). Ähnliche Muster in `MaterialEditorPanel.cpp:1323/1762`, `TypeAssetPanel.cpp:264`, `InputAssetPanel.cpp:704`, `LevelScriptPanel.cpp:1832/3031`, `InspectorPanel.cpp:3212`, `ExportDialogPanel.cpp:1366/1420`, `HcGraphHost.cpp:1817/2185/2207/2236` rufen `listAssets`/`listHorizonCodeClasses` auf – jeweils prüfen, ob im Draw pro Frame statt einmalig beim Öffnen.
- `src/HE_Editor/MaterialEditorPanel.cpp:135`: zweite `categoryColor`-Implementierung (EDT-913).
- `src/HE_Core/include/HorizonCode/HorizonCode.h:754-761`: Allokationsfreie Accessoren existieren; „HcCodegen and the editor panels still build full NodeSigs; they can adopt these too“ – weitere Panel-Aufrufer von `signatureOf` mit `grep` prüfen.

**Editor_10.md**

- `src/HE_Editor/HcEditorUtil.cpp:93-100, 105-127` (`listAssets`/`listScenes`): ungecachte rekursive Verzeichnis-Walks mit Header-Reads (`HAsset::readAssetTypeFromFile`), von vielen Draw-Pfaden aufgerufen (u. a. EDT-1001, EDT-1002, HcGraphHost-Combos, HcPullUi:286). Ein zentraler, über `EditorAssetTypeCache::invalidate*`/Datei-Watcher invalidierter Cache würde alle Aufrufer entlasten. Gehört in den Slice von HcEditorUtil.
- `src/HE_Editor/HcEditorUtil.cpp:1693-1790` (`drawPinDefaultEditor`): ruft die `choices`-Funktion vor `BeginCombo` (siehe EDT-1002); Signatur `ParamChoices` sollte in „hat Auswahl?“ und „Auswahl holen“ geteilt werden.
- `src/HE_Editor/HcEditorUtil.cpp:506-519` (`valueTypeName`) und `:383` (`pinTypeName`): weitere Kopien der Typnamen-Tabelle (EDT-1008).
- `src/HE_Editor/GraphEditor.cpp:283-360, 677`: ruft `model.pins`, `model.title`, `model.getPos`, … je Node und Frame; ein optionaler „NodeView“-Callback (liefert Pins, Titel, Farbe, Position in einem Aufruf mit einer Suche) würde das mehrfache `findNode` und die Signatur-Neuberechnung sparen (zu EDT-1004).
- `src/HE_Core/include/GraphCommon/GraphModel.h:40-62`: `findNodeById` ist linear; für Graphen >~200 Nodes wäre ein optionaler id→index-Cache im Graph (invalidiert bei add/remove) sinnvoll.
- `src/HE_Core/include/HorizonCode/HorizonCode.h:752-753`: `signatureOf` liefert vier Vektoren per Wert; viele Editor-Aufrufer brauchen nur Zählwerte (`signatureCountsOf`) oder eine einzelne PinDesc.
- `src/HE_Editor/UIEditorPanel.cpp:6594`, `LevelScriptPanel.cpp:1217`, `AnimatorStateMachineEditorPanel.cpp:764`: rufen `buildModel` je Frame (syncCreateWidgetPins + ~40 Lambdas + QuickSpawn-Vektor werden jeden Frame neu aufgebaut). Ein langlebiges Model mit aktualisierter `Host`-Referenz wäre billiger; nur messen, falls Frame-Zeit auffällt.

**Editor_11.md**

- `src/HE_Core/include/GraphCommon/GraphModel.h:44-64`: `findNodeById` ist linear, und wird von `MaterialGraph`, `ParticleGraph`, `HorizonCode::Graph` (und `GraphEditor`-Adaptern der anderen Editoren) pro Node und Frame benutzt. Ein Id→Index-Cache an zentraler Stelle (Rebuild bei Load/Undo/Mutation) würde EDT-1102 und die analogen Muster in `UIEditorPanel`/Particle-Panel gleichzeitig beheben (Slice-Auditoren für HE_Core/Editor-Graph-Panels informieren).
- `src/HE_Editor/HcEditorUtil.cpp:55-103` (`listAssets`/`scanAssetsInto`): Jeder Aufruf scannt Content- und Engine-Content-Baum rekursiv und liest Header; ein gemeinsamer, über `EditorAssetTypeCache`/Refresh invalidierter Index würde alle Picker (Mesh, Textur, Klassen, InputAction, Widget, Theme, Audio, Szenen: `HcEditorUtil.cpp:1850-1866`) beschleunigen. `listScenes` (Z.105-131) macht dasselbe ohne Extension-Filter vor dem Walk.
- `src/HE_Editor/GraphEditor.cpp:283-360`: Layout aller Nodes pro Frame ohne Sichtbarkeits-Culling (`d.pins = model.pins(nid)` für jede Node), ruft pro Node `model.title`, `headerColor` (siehe EDT-1102). Relevant für alle Graph-Panels.
- `src/HE_Editor/EditorUI.cpp`: Die Menüstruktur existiert doppelt (ImGui-Bar + `MacMenuBar.mm`, `Cmd`-Enum). Das ist im Header ausdrücklich als Wartungsfalle beschrieben (`MacMenuBar.h:15-20`). Eine gemeinsame Tabelle `{Menü, Label, Cmd, Shortcut, needsProject, gameOnly}` würde beide Pfade (ImGui + Cocoa) speisen und die Fehlerklasse "nur im ImGui-Menü vorhanden" beseitigen.
- `src/HE_Editor/EditorMultiEdit.*`: `EditorMultiEdit::state(world, entity)` serialisiert die komplette Entity (JSON) und wird pro Edit-Frame zweimal aufgerufen (`InspectorPanel.cpp:278, 326`); bei Verlinkung über viele Komponenten teuer. Auditor von `EditorMultiEdit.cpp` sollte prüfen, ob nur die betroffene Komponente (`only`) serialisiert werden kann.
- `src/HE_Core/src/Scripting/ScriptEngine.cpp:511-560` (`getScriptProperties`): Lua-Walk pro Aufruf, siehe EDT-1104; ein `ScriptEngine`-seitiger Cache pro Modul-Revision wäre die zentrale Lösung.
- `src/HE_Editor/InputMappingModel.cpp` (nicht in diesem Slice): `decodeMapping`/`encodeMapping`/`applyDetected` – im Header sind die Zeilenindizes `kAxisRowBaseY = 500` fest kodiert; ein Axis-Listenindex ≥ 500 würde fälschlich als Y-Liste gelesen (theoretisch, kein praktischer Fall). Auditor der .cpp prüfen lassen, ob eine Obergrenze erzwungen wird.

**Editor_12.md**

- `src/HE_Editor/EditorCommands.cpp:419-429` (`entityByUuid`): siehe EDT-1201; auch `TaskBoardPanel.cpp:547,550,579,603` ruft `HE::Ed::entityByUuid` in Panel-Draw-Code (Zeilen 547/550 vermutlich pro sichtbarer Zeile/Link) — dort wird derselbe O(E)-Scan pro Frame ausgeführt; mit dem Fix von EDT-1201 entfällt auch dieses Problem (Draw-Pfad, daher höher zu gewichten als im MCP-Fall).
- `src/HE_Editor/McpToolsInput.cpp:327`, `McpToolsMaterial.cpp:546,1643`, `McpToolsParticle.cpp:402`, `McpToolsPrefab.cpp:322,711`, `McpToolsSequence.cpp:145,592`, `McpToolsType.cpp:522`: weitere Aufrufer von `walkContentAssets` (EDT-1203) und eigene uuid→Pfad-Walks (EDT-1207); Gate-Duplikate (EDT-1208) in Input, Material, Particle, Prefab, Scene, Sequence, Type, Widget; lokale `pinTypeName`/`uuidJson`/`lowered` in Material, Particle, Prefab, Settings, Type (EDT-1211).
- `src/HE_Editor/InputAssetPanel.*`, `TypeAssetPanel.*`, `ParticleGraphEditorPanel.*`, `AnimatorStateMachineEditorPanel.*`, `BlendSpacePanel.*`, `SkeletalMeshEditorPanel.*`: jedes Panel liefert dasselbe Quartett `isDirty / isDirtyByContentPath / reloadByContentPath / appendDirtyPaths` (MaterialEditorPanel.h:35-40 verweist ausdrücklich auf „InputAssetPanel's pair"). Ein gemeinsames Interface (`IAssetTabHost`) würde die Verdrahtung in `setupMcpTools` und die Bestätigungsdialoge beim Beenden vereinheitlichen.
- `src/HE_Scene/src/SceneSerializer.cpp:3011` (`isKnownComponentKey`): für EDT-1202 wäre eine billige `hasComponentKey`-Prüfung die passende Ergänzung.
- `src/HE_Editor/EditorApplication.h:46-48`: Include-Kette für EDT-1209; Destruktionsreihenfolge-Kommentar bei `:838-861`.
- `scripts/he_mcp.py` (Shim, nicht gelesen): Rahmengröße 4 MiB wird in Bridge.h/Registry.h erwähnt; EDT-1204 sollte mit dem Shim abgestimmt werden, wenn `dump(2)` entfällt.

**Editor_13.md**

- `src/HE_Editor/McpToolCommon.cpp:197-237` (`walkContentAssets`): jeder Katalog-Aufruf scannt den gesamten Content-Baum rekursiv, sortiert und sniffed alle `.hasset`. Ein durch `ContentManager`s Content-Change-Zaehler invalidierter Index wuerde alle Kataloge (asset_list, material_info, input_*, sequence_info, prefab_info, particle_info) beschleunigen. Siehe EDT-1309.
- `src/HE_Core/src/ContentManager/ContentManager.cpp:790-818` (`regenerateMaterialFromGraph`): Rueckgabetyp `void`, bricht bei leerem GLSL still ab; ein `bool`/Overload mit vorberechnetem `MatShaderGen` wuerde EDT-1303 ermoeglichen.
- `src/HE_Editor/EditorApplication.cpp:10782-10785`: der Terrain-`regenerate`-Hook ruft `TerrainSystem::updateTerrains(*m_editorWorld, ...)` fuer die gesamte Welt nach jedem Dab; mit EDT-1301 (mehrere Dabs pro Aufruf) laesst sich das ebenfalls reduzieren. Ob `updateTerrains` nur dirty Chunks anfasst, ist dort zu pruefen.
- `McpToolsAnimator.cpp`, `McpToolsAsset.cpp`, `McpToolsType.cpp`, `McpToolsEntity.cpp`: tragen dieselben Kopien von `uuidJson`/`readPayload`/`failFor`/`vec3Arg` (siehe EDT-1308); die Gate-Kette (EDT-1307) kommt vermutlich auch in `McpToolsWidget.cpp`/`McpToolsClip.cpp` vor.
- `src/HE_Editor/McpToolRegistry.h`: die Hook-Structs `McpInputHooks`, `McpMaterialHooks`, `McpParticleHooks`, `McpSequenceHooks`, `McpPrefabHooks` tragen gleichartige `isPlaying/lockedByOther/isDirty/reloadFromDisk`-Felder (Basis `McpAssetWriteHooks` bietet sich an).
- `EditorCommands.cpp:406-417` (`uuidOf`) formatiert die UUID per `snprintf`, obwohl `HE::uuidToHex` existiert (TaskBoard nutzt es).

**Editor_14.md**

- `src/HE_Core/src/Diagnostics/GlobalState.cpp:403-407` – `removeKnownProject` mit Wert-Aliasing durch `std::remove` (siehe EDT-1420).
- `src/HE_Core/include/Diagnostics/EngineProfiler.h:210, 301-302` und `EngineProfiler.cpp:323` – `liveSnapshot()` gibt `m_live` per Wert zurück; der Ring ist ein `vector` mit `erase(begin())` pro Frame (O(240)); `std::deque`/Ringpuffer plus `span`-Zugriff wäre billiger. `ProfThreadTimeline::spans` hält bis zu ~250k Spans pro Lane ohne Sortierung nach Start (Voraussetzung für EDT-1403).
- `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:21-26` – `childrenOf` ist O(n) pro Aufruf; mehrere Aufrufer (MCP-Widget-Tools, WidgetManager.cpp:5926) rufen es pro Knoten auf.
- `src/HE_Editor/GraphEditor.h` – `GraphEditor::Model` liefert Vektoren/Strings per Wert pro Callback (betrifft Material-, HorizonCode- und Particle-Editor, siehe EDT-1412).
- `src/HE_Scene/src/EntityVisibility.cpp:51-65` – `subtreeVisibility` läuft bei vollständig verborgenem Teilbaum über den ganzen Teilbaum, wird aber vom Outliner pro sichtbarer Zeile und Frame aufgerufen (OutlinerPanel.cpp:1561-1562).

**Editor_15.md**

- `src/HE_SourceControl/src/GitCli.cpp:437-449`: `isValidBranchName` und `branchExists` sind synchrone Prozessaufrufe. Sie sollten in der API als „blockierend“ gekennzeichnet bzw. als asynchrone Variante über `GitService` angeboten werden (siehe EDT-1501). `GitCli::run` mit 10-s-Timeout wird sonst leicht aus UI-Code aufgerufen.
- `src/HE_Scene/src/WaterField.cpp:196-203` (`Field::wetCells`) zählt O(res²) bei jedem Aufruf; ein mitgeführter Zähler pro Body beim Schreiben von `coverage` würde den Aufruf O(1) machen (EDT-1502). `src/HE_Scene/src/WaterLake.cpp:673-690` (`whyNot`) rasterisiert in einer reinen Diagnosefunktion das Polygon.
- `src/HE_Editor/EditorPanelState.h` (`AssetPanelState`): Die Suche nach `relPath` fehlt und ist in mindestens sechs Panels kopiert (EDT-1514).
- `src/HE_Editor/EditorShortcuts.cpp`: `conflicts()`/`label()` sollten für Zeilenweise-Aufrufe pro Frame günstig sein (EDT-1509); nicht in diesem Slice gelesen.
- `src/HE_Editor/AppMetadataRows.*` (`iconPreviewTexture`, `drawIconFileRow`): Wird in `drawApplicationPage` jeden Frame gerufen; Dateisystemzugriffe/Texturaufbau dort wurden nicht geprüft.
- `src/HE_Editor/ViewportPanel.cpp` (`selectionBox`): Wird von jedem offenen Secondary-Viewport zusätzlich zum Hauptfenster pro Frame berechnet (SecondaryViewportPanel.cpp:93); Frame-weiter Cache wäre denkbar.
- `src/HE_Editor/GitController` / `HE_SourceControl::RepoStatus`: `status()` liefert eine Referenz mit festem Ort; `source`-Zeiger-Vergleiche (SourceControlPanel.cpp:340, :1957) tragen deshalb nichts bei. Falls sich `generation` je zurücksetzen kann, wäre eine Projekt-ID im Status die richtige Quelle.

**Editor_16.md**

- `src/HE_Editor/EditorCommands.cpp:419-428` (`entityByUuid`/`uuidOf`): O(Entities) mit `snprintf` pro Entity – wird vom Task Board, den MCP-Tools und `EditorApplication` benutzt (siehe EDT-1602). Ein `UUID`-Overload wäre modulweit nützlich.
- `src/HE_Editor/EditorApplication.cpp:4915-4925`: baut pro Frame ein `TeamSyncController::Project` (Path-Kopie + 3 Strings); `EditorApplication.cpp:11400-11560` (`syncStructuralChanges`) ist der eigentliche Verursacher von EDT-1601.
- `src/HE_SourceControl/include/SourceControl/TeamSyncService.h:183-189`: `status()/lastResults()/history()/lost()` liefern Wert-Kopien; ein Revisionszähler fehlt (EDT-1610).
- `src/HE_Scene/include/HorizonScene/HorizonWorld.h:106`: `cellManifestJson()` ohne Revisionszähler (EDT-1614).
- `src/HE_Editor/EditorUI.cpp:291-325` (`docsPanelOpener`) – Quelle der doppelt gepflegten Panelliste (EDT-1615); `EditorUI.cpp:3502` ruft `footerOnlineWidth` pro Frame.
- `src/HE_Editor/MeshMaterialSlots.*` (nicht im Slice): `MS::tableOf(*mesh)` kopiert pro Frame die Slot-Tabelle (`StaticMeshEditorPanel.cpp:472`); Größe/Kosten dort prüfen.
- `TaskBoardTeam`/`TaskTeamLink` sind im Editor nicht verdrahtet (`setTeam`/`setLink` nur in `tests/`, Plan in `docs/taskboard-team-interop-2026-10-15.md`): bis dahin ist der Team-Code in `TaskBoardPanel.cpp` (Zeilen 972-1022, 1145-1179) Totlast im Produktivbuild.

**Editor_17.md**

- `src/HE_Editor/UIEditorPanel.cpp:878, 7097, 7378` und `ExportDialogPanel.cpp:1366` rufen ebenfalls `HcEditorUtil::listAssets` (rekursiver Scan + Header-Reads); falls dort pro Frame statt nur in geöffneten Combos aufgerufen wird, dasselbe Problem wie EDT-1702 (nicht geprüft; Aufgabe für die Slices dieser Dateien). Ein gemeinsamer gecachter `listAssets` (Content-Revision) würde alle beheben (`src/HE_Editor/HcEditorUtil.cpp:93`).
- `src/HE_Scene/src/WaterField.cpp:196` (`Field::wetCells`) ist O(res²) und wird auch von `EngineApi.cpp:1565` (Skript/MCP) gerufen; ein Zellzähler pro Body im Field würde Editor und API entlasten (EDT-1705).
- `src/HE_Core/src/Types/TypeRegistry.cpp:160-168, 222-231`: `getEnum/getStruct` liefern Kopien, `nameCollides` scannt linear; ein `shared_ptr<const Def>`-Zugriff bzw. ein Namensindex würde auch `McpToolsType.cpp:320` entlasten.
- `src/HE_Editor/InputAssetPanel.*` ist laut Kommentar eine weitere Kopie des Asset-Tab-Boilerplates (EDT-1717).
- `src/HE_Editor/EditorApplication.cpp:12323, 12339`: `gitInstallLog`/`toolchainInstallLog` geben den Log by value zurück (Kopie unter Mutex pro Aufruf); ein `string_view`-Snapshot mit Versionszähler wäre günstiger.

**Editor_18.md**

- `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:16-26` und `src/HE_Core/include/.../GraphModel.h:45-70` (`findNodeById`): lineare Suche; `UIWidgetTree::find` ist die Wurzel von EDT-1802/1803 und der Layout-Funktionen (`uiElementRect` ruft rekursiv `find` auf Eltern, `UIWidgetTree.cpp:239-258`). Ein Id→Index-Lookup (Ids sind dichte Ints) in `UIWidgetTree` würde auch Runtime-Layout (`WidgetManager`) beschleunigen. `childrenOf` und `removeSubtree` (`UIWidgetTree.cpp:21-52`) haben O(n²) bei großen Bäumen (`removeSubtree` mit `std::find` in `doomed` pro Element).
- `src/HE_Editor/EditorSelection.cpp:75-102`: `contains()` linear, `roots()` quadratisch (EDT-1805). Weitere Aufrufer (Outliner, Gizmo, MCP) profitieren von einem Set/Revisionszähler.
- `src/HE_Editor/HcEditorUtil.cpp:93-100`: `listAssets` als Dateisystem-Scan wird in mehreren Panels im Draw benutzt (hier Palette; `grep listAssets` für weitere Aufrufer außerhalb des Slice); ein zentraler, invalidierbarer Asset-Listen-Cache wäre die saubere Lösung.
- `src/HE_Editor/EditorUI.cpp:1991` (macOS): `entityActionState(ctx).primaryLocked` pro Frame (EDT-1811).
- `src/HE_Core/include/ContentManager/Assets.h:144-152`: `boundsMin/boundsMax` existieren, werden vom Editor nicht genutzt (EDT-1812). `UIElement::properties()` liefert by-value-Vektoren (EDT-1809); vermutlich auch in anderen Panels genutzt.
- `src/HE_Editor/ViewportOverlays.cpp:155`: zweiter, nie invalidierter Mesh-Box-Cache (EDT-1812) – auch relevant für `SecondaryViewportPanel` (nutzt `ViewportPanel::selectionBox`, dessen `sceneSnapshot` bei jedem Aufruf aus dem Menü extrahiert, falls der Scene-Frame noch nicht lief: `ViewportPanel.cpp:709-721`).

**Editor_19.md**

- src/HE_Editor/EditorToolbar.cpp:46-97 (`cellImpl`): `cellTinted` (107-111) reicht `helpKey` nicht durch; dadurch zwingt es Aufrufer zu handgebauten Zellen (siehe EDT-1904).
- src/HE_Editor/ViewportPanel.cpp:72-90 (`showFlagFields`) ist die eigentliche Quelle der Show-Flag-Liste; ein Gruppenfeld dort würde EDT-1903 lösen.
- src/HE_Editor/ViewportPanel.cpp:958: `SetViewMode` wird jeden Frame gepusht; Backends sollten ein Setzen des gleichen Werts als No-Op behandeln (nicht geprüft, in `src/HE_Rendering/.../*Renderer.*` nachsehen).
- src/HE_Editor/CMakeLists.txt: siehe "Gelesen" und die Hinweise unten.

**Editor_20.md**

- `src/HE_Editor/EditorUI.cpp:1063-1077`: `appendSnapshots`-Liste ist handgepflegt und enthält `CinematicPanel` nicht (siehe EDT-2003); gleiche Pflege-Liste existiert für `isDirty` (`:992`), `save` (`:1048`), `forget` (`:1117`), `reloadFromDisk` (`:1211`) und das Tab-Dispatching (`:4161`). Eine Registrierungstabelle würde alle fünf Listen ersetzen.
- `src/HE_Core/src/Audio/AudioEdit.cpp:307-316`: `BiquadCoeffs::magnitudeDb` verwendet `std::complex<double>`/`std::polar`/`log10` pro Aufruf; für die EQ-Anzeige (EDT-2004) wäre eine Variante mit vorab berechnetem `cos(w)`, `cos(2w)` ohne Komplexzahlen deutlich billiger.
- `src/HE_Editor/AudioEditorPanel.cpp` (nicht Teil dieses Slice): ruft `AudioWave::buildPeaks` auf; bei sehr langen Clips (siehe Unsicher) bitte prüfen, ob der Aufruf synchron im UI-Thread geschieht und ob das Dekodieren/Peak-Bauen über das `JobSystem` laufen könnte.
- `src/HE_Scene/src/TransformHierarchy.cpp:415`: `propagateTransforms` ist dirty-fähig (`structureEpoch`), geht aber im Zweifel auf `propagateTransformsFull`; `CinematicPreview::restore` setzt `tc->dirty = true`, was bei jedem Frame mindestens einen Teilpass auslöst (EDT-2001).

## Unsicher / Messung nötig

**Editor_1.md**

- EDT-107: tatsächliche Dauer von `decodeToPcm16` + `buildPeaks` + `analyze` für typische Ambience-Loops (1-10 Min.) im Release-Build messen, bevor ein Job-Umbau begründet wird.
- EDT-105/106: Frame-Zeit von `updateAssetAutosave` mit z.B. 5 offenen schmutzigen Tabs und 50-MB-PCM messen (Hitch bei Autosave-Tick vs. durchschnittliche Allokationslast).
- EDT-101: Wie oft tritt der Fall "Upload-Budget leer, Render-Budget frei" in der Praxis auf? Mit einem Zähler für `RenderAssetThumbnail`-Aufrufe vs. vorhandene `.hthumb` messen.
- `AssetThumbnailCache::image()` (Zeilen 566-621): Laden + `toDisplayRgba` + Upload eines 4K-Bildes läuft ungedrosselt synchron im Frame (bewusst laut Header). Beim Wechsel zwischen Seiten mit vielen großen Bildern und der 5 s-Räumung (`kImageIdleSec`) mögliche Re-Upload-Hitches; messen, ob ein Budget oder Hintergrund-Decode nötig ist.
- `AppMetadataRows`: `s_previews` gibt die GPU-Textur beim Projekt- oder Renderer-Wechsel nicht frei (nur bei geändertem Schlüssel); ob `DestroyImGuiTexture` am Ende noch einen gültigen Renderer hat, wurde nicht geprüft (potentielles Dangling-Handle bei Renderer-Wechsel).
- ASM-Panel: Transitionen referenzieren Zustände über Namen (Strings). Doppelte Zustandsnamen oder freie Texteingaben in "From/To" (Zeilen 659-662) erzeugen mehrdeutige/baumelnde Links; `findStateByName` liefert dann den ersten Treffer. Ob die Laufzeit (AnimationStateMachineSystem) ebenfalls nach Namen auflöst, wurde nicht geprüft; eine ID-basierte Referenz wäre eine Datenformat-Änderung (Loader-Kompatibilität nötig).
- EDT-114: Ob `AudioEngine`-Aufrufe aus dem UI-Thread intern gelockt sind (kein Lock in `applyBusConfig` gesehen), wurde nicht geprüft.

**Editor_2.md**

- Tatsächliche Kosten von `diffInto`/`hasLocalChanges` pro Frame für Level Script/GameInstance (EDT-201): Scoped-Timer in `publishDocDeltas`, Graphen mit 50/500/2000 Nodes.
- Ab welcher Entity-Zahl `seedNetIds` (EDT-202) spürbar wird: Test mit 10k/50k/100k Entities; Schätzung oben ist reine Rechnung (N²/2 Paarvergleiche).
- `GetAdaptersAddresses`-Dauer auf Windows (EDT-205); Linux/macOS vermutlich im µs-Bereich.
- Wie oft `m_pendingOps` im Alltag groß wird (EDT-206); die 128er-Grenze ist nur eine Obergrenze.
- Zeitgewinn durch Datei-Lesen mit einem `read` statt `istreambuf_iterator` (EDT-204): Implementierungsabhängig (libstdc++/libc++/MSVC), mit großem Mesh messen.
- Ob `Sequencer::targetName` (SequencerTimeline.h, anderer Slice) pro Aufruf einen `std::string` baut; falls ja, ist der Label-Anteil in EDT-210 größer.
- Wirkung der Row-Clipping-Änderung im Cinematic-Streifen (EDT-210) bei realen Sequenzen; wahrscheinlich nur bei Extremfällen relevant.

**Editor_3.md**

- Tatsächliche Frame-Kosten pro Kachel (EDT-301) hängen von `AssetThumbnailCache::get` und `EditorAssetTypeCache` ab; ohne Profil kein Faktor angebbar. Zuerst mit 2 000/5 000 Dateien profilieren.
- Wie stark `fs::relative` auf den Zielplattformen kostet (EDT-305): libstdc++ und MSVC rufen `weakly_canonical`; libc++ (macOS) verhält sich ähnlich, aber nicht identisch. Messung mit `strace -c` / Process Monitor / `dtruss`.
- Die Größe der Node-Reference-Seite (EDT-302: "ca. 300 Sektionen" laut Kommentar `DocsPanel.cpp:1081-1086`) und der Anteil der CPU-Zeit in `splitWords` gegenüber ImGui-Item-Submission.
- Reale Textmenge von `he-docs.json` samt generierten Seiten (EDT-313/314); 500 KB stammt aus dem Kommentar in `DocsLibrary.h:223-225`.
- Ob die Log-Flood-Szenarien (EDT-308/311) in der Praxis die Konsole offen halten; Verhalten bei 5 000 Zeilen + aktiver Suche messen.
- EDT-307: Soll ein Reload ungespeicherte lokale Änderungen verwerfen? Kommentar sagt ja ("anything unsaved here is stale"), Entscheidung beim Fix bestätigen.

**Editor_4.md**

- EDT-402: Wie viele Entities sind in realen Sessions typisch, und wie lange dauert `syncStructuralChanges` tatsächlich pro Frame? Mit `HE_PROFILE_SCOPE_N("StructSync")` messen.
- EDT-404: Kosten von `serializeWithoutGrids` für Terrain-/Foliage-/Skeletal-Entities, abhängig davon, ob `GridsAside` verschiebt oder kopiert.
- EDT-406: ob die 60 `[this]`-Lambdas im SBO der verwendeten `std::function` liegen (libstdc++: bis 16 Byte, MSVC: bis 32 Byte, libc++: bis 24 Byte). Wenn ja, keine Heap-Kosten, dann nur die Konstruktion.
- EDT-414: Ob die Backends bei unveränderten Settings ihre Pipelines/Targets neu bauen. Falls sie selbst vergleichen, ist der Push pro Frame wertlos teuer nur im Aufbau der Structs.
- Joint-Overlay (Z. 4370–4461): pro Joint zwei `worldMatrixOf` (Parent-Kette) und `findByEntityId` pro Frame. Bei tausenden Joints und aktivem `show.joints` messen.
- Rope-/Trail-Guides und `appendRootMotionPreview`/`appendLookAtPreview` (Z. 2858–2966) allokieren einen `std::vector<glm::vec3> path` pro Frame für ein Entity. Der Kommentar rechtfertigt das. Nur relevant bei vielen Frames mit großer Auswahl.
- Datenrennen-Verdacht (nicht verifiziert): `startToolchainProbe()` (Z. 2324) und `startGitProbe()` (Z. 2396) werden aus UI-Callbacks mitten im Frame gestartet, in dem der Dialog noch `ctx.toolchainProbe`/`ctx.gitProbe` (Zeiger auf `m_toolchainProbe`/`m_gitProbe`) liest, während der Worker das Objekt neu zuweist. Das Flag wird zwar vorher auf `false` gesetzt, aber der Zeiger im laufenden Frame bleibt gültig. Prüfen, ob der Dialog das Ergebnis nach dem Klick im selben Frame noch liest.
- `healStuckImGuiKeys` (Z. 2975): einmal pro Sekunde 512 Scancodes mit `SDL_GetKeyFromScancode`. Vermutlich vernachlässigbar.

**Editor_5.md**

- EDT-501: tatsächliche Kosten von `syncStructuralChanges` pro Frame bei großen Szenen in einer Session (Zeitmessung um die Funktion, Allokationszähler).
- EDT-504: Dauer der Kaltstart-Sniffs nach `invalidateAll()` bei 5–10 k Assets auf Windows (Defender) und auf Netzlaufwerken; ob der Refresh-Worker den Typ ohnehin schon kennt.
- EDT-505: reale Kosten von `makeContext()` (Konstruktion + Destruktion von ~60 `std::function`); vermutlich < 10 µs.
- EDT-502: Anteil der Gateway-Aufrufe im Alltag; heute nur MCP, daher Nutzen erst bei Migration der UI-Pfade groß.
- EDT-510: Speicher/Startzeit der Guides-Seiten tatsächlich messen, bevor die Suchtext-Doppelung angefasst wird.

**Editor_6.md**

- EDT-601: tatsächliche Kosten von `state()` je Entity (Serialisierung + CBOR→JSON) und Konstruktionskosten von `SceneSerializer`; Frame-Zeit beim Drag mit 200/1000 Members.
- EDT-603: Kosten von `SDL_GetTouchDevices` pro Aufruf unter Windows (Win32-Pfad evtl. mit Enumeration).
- EDT-607: Index-Aufbau und Relokations-Startkosten von `kEntries` (Startup-Profil, erster Hover-Frame).
- EDT-612: Ob `Help::find`/`Scope` jemals außerhalb des UI-Threads aufgerufen wird.

**Editor_7.md**

- EDT-701: tatsächliche Kosten von `roots()` bei Select All (5 000 / 20 000 Entities) im Viewport-Frame messen; ebenso ob Tiefe > 1 in realen Szenen vorkommt.
- EDT-702: Zeit von `worldMatrixOf` pro Mitglied (Tiefe) und `glm::inverse` bei 100/1 000 Mitgliedern im Drag.
- EDT-705: Allokationen pro Frame im idle Footer (Allokations-Zähler/Tracy) bestätigen; absoluter Gewinn vermutlich < 20 µs.
- EDT-706: Zeitanteil von `pressed()`-Summe pro Frame (vermutlich ~2–5 µs).
- EDT-707: Suchstrategie von `Help::findKey` (nicht in diesem Slice) – wenn Hash/Binärsuche, ist der Fund noch kleiner.
- EDT-704: Ist das Nicht-Zurücksetzen der Render-Einstellungen in "Restore Defaults" gewollt? Produktentscheidung.
- EDT-709: Dauer der Erstsynthese des Chime/Tick (geschätzt 1–3 ms) messen.
- `EditorSettingsPanel.cpp` Status-Seite: Allokationen pro Frame nur relevant, falls die Seite Dauer-Hotspot beim Profiling wird.

**Editor_8.md**

- Kosten von 16 `AssetPanelState::dirty`-Lookups je Tab und Frame (EDT-801/806) bei realistisch 10-30 Tabs.
- Snapshot-Größe und Speicherbedarf von bis zu 64 Welt-Snapshots in `EditorUndo` (EDT-818).
- Dauer des rekursiven `.hescene`-Scans (EDT-811) bei Projekten mit `.git`/`Export`-Ordnern; Thread-Sicherheit von `ContentManager`/`syncPrefabInstances`, falls das Serialisieren in den Worker wandert.
- Ob `HcEditorUtil::listAssets` den Header-Sniff-Cache nutzt (EDT-808).
- Ob das Fehlen von `SkeletalMeshEditorPanel` in `reloadAssetTabFromDisk` und von `CinematicPanel` in `appendAssetSnapshots` Absicht ist (EDT-801).
- Verhalten, wenn `Row`-Aufrufer `assetDropSlot` mit `idSuffix == nullptr` rufen (EDT-807); nicht gesucht.

**Editor_9.md**

- EDT-903: tatsächlicher Anteil von Callbacks/`findNode` an der Frame-Zeit bei realen Graphen (z. B. 200/1000 Knoten); Profil mit Tracy/`perf` vor Umbau.
- EDT-901/902: Kosten von `listScenes`/`listAssets` je Projektgröße (Dateianzahl, Netzlaufwerk/Windows-Defender); Messung der Frame-Zeit mit offenem Combo bzw. 10 `scene.*`-Knoten.
- EDT-906: Anteil des Exec-Listeners an der Interpreter-Laufzeit im Play-Modus (Microbenchmark mit Schleifengraph).
- EDT-907: ob `ContentManager` einen Revisionszähler pro Asset bereitstellt (nötig für Cache-Invalidierung).
- EDT-916: ob der Compiler `std::fill` + `clear()` in `wipe()` entfernt (nur per Disassembly der Release-Build-Binärdatei prüfbar).
- EDT-915: Größe typischer Installer-Logs (winget/brew) und Threading des `gitInstallLog`-Getters.

**Editor_10.md**

- EDT-1002: Wie viele Pins pro Frame tatsächlich `drawPinInlineEditor` erreichen (Viewport-Culling in GraphEditor?) und wie teuer `listScenes`/`listAssets` in einem realen Projekt sind.
- EDT-1001: Kosten von `listAssets` bei großem `Content/`-Baum (Disk-Cache-Effekte); ein Frame-Zeitprofil mit offenem Input-Mapping-Tab.
- EDT-1004: Ab welcher Node-Zahl der O(N²)-Anteil sichtbar wird (Schätzung 300-500+).
- EDT-1006: Ob `HorizonCode::Runtime` einen Versionszähler für den angehaltenen Run bietet (Voraussetzung für verlustfreies Caching).
- EDT-1007: Vorfilter per Substring-Check – Verhalten bei JSON-escapten Zeichen im Namen (Serializer-Einstellungen von `HorizonCode::toJson` prüfen).
- EDT-1003: Aufwand eines `resolveClassAsset` pro Frame (hängt von Kettenlänge/Graphgröße).
- HcGraphHost `buildModel` pro Frame: Kosten der ~40 `std::function`-Zuweisungen sind je nach STL-Implementierung (SBO-Größe: libstdc++ 16 B, libc++ 24 B, MSVC 56 B) unterschiedlich; Lambdas mit zwei Referenz-Captures passen überall, `quickSpawns` mit Capture `[&h, t]` ebenfalls – nicht gemessen.

**Editor_11.md**

- EDT-1102: tatsächliche Frame-Kosten des quadratischen `findNode`-Musters bei 300/1 000 Nodes (Profiler-Panel); ggf. ist `GraphEditor::draw` selbst der Engpass.
- EDT-1101: Dauer des Walks mit warmem/kaltem Dateisystem-Cache bei einem 5 000-Asset-Projekt; wie oft `loadAsset` in der Script-Combo tatsächlich neu von der Platte liest (vermutlich nur beim ersten Öffnen, da residente Assets gecacht werden).
- EDT-1104: Verhalten von `ScriptEngine::loadScript` bei Syntaxfehler (wird der Fehler jedes Frame neu erzeugt und geloggt?) – nicht gelesen.
- EDT-1108: Wie oft `RenderMaterialPreview` bei `previewAnimated` Pipelines pro Frame neu bildet (Hash-Cache laut Kommentar) – nicht gemessen.
- EDT-1110: Thread-Sicherheit von `HE::MaterialShaderLibrary` (statische Instanz in `regenerateMaterial`), falls der Cross-Compile in einen Job verlegt wird.
- EDT-1113 (c): ob `AudioEngine::setSoundAttenuation` einen Mutex nimmt oder eine Voice neu parametriert.
- EDT-1113 (b): Kosten von `HE::spline::Curve` (Arc-Length-Tabelle) bei vielen Kontrollpunkten – `SplineCurve.cpp` nur ab Zeile 60-85 gelesen.
- EDT-1118: Auswirkung von zwei `trackEdit()`-Aufrufen auf dasselbe Item in `EditorUndo::stashPre/commitPending` (nicht gelesen).
- `LevelScriptPanel.cpp:2301-2326` `previewMeshBounds`: statischer Cache `s_cache` wird nie invalidiert (Mesh-Reload, Asset-Ersatz mit gleicher UUID, Projektwechsel) und wächst unbegrenzt; Speicher klein, aber veraltete Bounding Boxes möglich. Verhalten ändern ist trivial (Cache beim Projektwechsel/Content-Refresh leeren), aber ob es praktisch auftritt, nicht verifiziert.
- `MaterialEditorPanel.cpp:2651` (`InputTextMultiline` mit dem gesamten GLSL als `std::string`, ReadOnly): ImGui layoutet bei Multiline-InputText den gesamten Text pro Frame; bei 100 KB+ Shader ggf. messbar. Alternative wäre ein `ImGuiListClipper` über Zeilenoffsets. Keine Messung vorhanden.

**Editor_12.md**

- Größe der `tools/list`-Antwort: wie viele `api_*`-Tools werden aus `HE::api::registry()` erzeugt (Z. 633-637) und wie viele Bytes/Tokens kostet das pro Client-Kontext? Wenn das Hunderte Tools sind, wäre eine gestufte Liste („api_list" statt je ein Tool) der größte Hebel bei den Modell-Kosten; hier nicht bewertbar.
- Typische Szenen-/Projektgrößen für EDT-1202/1203/1207 (Entities, `.hasset`-Dateien) und reale Dauer eines `entity_list`, `animator_info`-Details und `asset_list` rekursiv.
- Wirkung von EDT-1209 auf die Buildzeit: `-ftime-trace` an einem typischen Panel-TU (Anteil `nlohmann/json.hpp` und `IRenderer.h`).
- EDT-1205: realistische Timeout-Wahl, weil `nowMs` Frame-Zeit ist und der Editor bei Idle-Throttling selten tickt.
- EDT-1210: ob Projekte legitim Symlinks im Content-Verzeichnis haben (macOS `/var`↔`/private/var`, Windows-Junctions).
- Build-Log-Größe in EDT-1206 (nur aus der Tool-Beschreibung „thousands of lines" geschlossen).

**Editor_13.md**

- Tatsaechliche Kosten eines Terrain-Dabs (EDT-1301) und der Gewinn eines Teil-Patches: mit 1025²-Landschaft und 100 Aufrufen messen; ebenso `HorizonWorld`-Konstruktor (EDT-1321).
- `computeTerrainHeightField` fuer prozedurale Landschaften (EDT-1302) bei hoher Aufloesung und vielen Oktaven.
- Codegen-Zeit eines grossen Material-Graphen (EDT-1303) und Verhaeltnis zu JSON-Parse/Serialisierung.
- Ob die MCP-Bridge Handler auf dem UI-Thread oder einem Worker ausfuehrt (bestimmt, ob EDT-1301/1306/1309 Frame-Hitches oder nur Antwortzeit verursachen) und ob Handler nebenlaeufig laufen (EDT-1316, `s_seq`).
- Aktualitaet der `m_diskRegistry` fuer `pathForId` (EDT-1306).
- `uuidJson`-Ausgabeformat bei `uint64` > 2^53 (EDT-1308).
- Graph-Groessen in der Praxis fuer EDT-1313.

**Editor_14.md**

- Reale Restkosten der Outliner-Zeilenschleife bei 50k Entities nach dem Clipping (EDT-1404, Stufe 2 nur nach Messung).
- Kosten von `SceneSerializer::prefabInstancesBinding` / `prefabAddedHereCount` im Outliner-Rebuild für große Prefab-Instanzen (OutlinerPanel.cpp:668-675); evtl. O(Instanzen x Entities).
- `HE::subtreeVisibility` und `isEntityActive` pro sichtbarer Outliner-Zeile (Z.1232, 1561): Worst Case bei großen komplett verborgenen Teilbäumen O(Teilbaum) pro Frame; Normalfall bricht beim ersten sichtbaren Kind ab.
- Anzahl Spans und tatsächliche Frame-Zeit des Timeline-Tabs bei realen Captures (EDT-1403).
- Wirkung der `ParticleGraph`-JSON-Serialisierung pro Frame (EDT-1401) bei typischen Graphgrößen (vermutlich klein, aber Allokationen pro Frame).
- Ob der Windows-COM-Ordner-Dialog (EDT-1420) einen funktionalen Grund hat (Parenting/Verhalten gegenüber SDL).
- Ob `HcCodegen.h` in ProjectHubPanel.cpp für den Typ `ctx.toolchainProbe` zwingend ist (EDT-1419).

**Editor_15.md**

- EDT-1504: Ob `RenderWorldPreview`/`RenderSkeletalPreview` eine Slot-Textur über Frames stabil halten und wie teuer ein Frame mit drei offenen Secondary-Viewports in einer realen Szene ist. Nur mit Profiler und Backend-Matrix entscheiden.
- EDT-1502: Reale Kosten von `whyNot` und `wetCells` bei Wassergitter 1024²/4096²; Zeiten im Profiler (Panel nur im Spline-Modus).
- EDT-1511: Anteil der reinen Serialisierung an der Autosave-Spitze bei großen Szenen; die Auslagerung des Schreibens lohnt nur, wenn I/O dominiert.
- EDT-1510: Tatsächliche Hash- und Lesezeit bei Szenendateien von ca. 12 MB (FNV-1a byteweise); Messung mit `remember()` nach `Save`.
- EDT-1508: Obergrenze von `GitController::recentCommits()` und Anzahl Remote-Branches in realen Repos.
- EDT-1509: Implementierung von `EditorShortcuts::conflicts()` (außerhalb des Slice) und Anzahl der Aktionen.
- ProjectSettingsPanel.cpp:596-597: `uiSetFontWeightBold(...)`/`uiSetFontScripts(...)` werden als Abfrage jeden Frame mit Seiteneffekt-Namen aufgerufen („Asking for what is already live answers yes“). Ob der Aufruf bei einer Änderung etwas anstößt (Rebake-Versuch pro Frame) wurde nicht geprüft.
- SceneRecoveryDialog.cpp:125-126: `Escape` wird neben dem Button ausgewertet; ob ImGui das Modal zusätzlich selbst per Escape schließt (und `deferRecovery` dann doppelt oder nie läuft), wurde nicht getestet.
- SequencerPanel.cpp:450-460: `previewActors` schreibt jeden Frame für alle Actors (Kommentar begründet das); bei sehr vielen Actors (> 1.000, ungeclippte Actor-Liste in `drawActors` :401-416) wäre das messbar, ist aber ein Randfall.

**Editor_16.md**

- EDT-1601: tatsächliche ms/Frame von `syncStructuralChanges` bei 10k/100k Entities in einer Session; ob `markHierarchyDirty` alle Hierarchieänderungen abdeckt.
- EDT-1602: Hitch-Größe bei realer Szenengröße; Anzahl typischer Entity-Links.
- EDT-1603/1604/1606: absolute Kosten bei realistischen Boards (< 100 Tasks wahrscheinlich unter 50 µs/Frame; Gewinn überwiegend bei sehr großen Boards).
- EDT-1608: reale Vertex-/CPU-Zeit der Avatar-Rechtecke im Footer; ob Run-Length oder Textur-Pfad lohnender ist.
- EDT-1611: ImGui-Kosten bei 120k Segmenten (Vertex-Puffer-Obergrenze, `ImDrawIdx` 16-bit → ImGui splittet Draw-Lists ggf.); Fenster-Verhalten bei 16-bit-Indizes prüfen.
- EDT-1612/1613: Serialisierungsgröße der Landscape-Entity, `memcmp`-Durchsatz auf Zielhardware, ob Dirty-Signale aller Werkzeuge (Pinsel, Fill Layer, Heightmap-Import, MCP, Undo) verlässlich gesetzt werden.
- `StaticMeshEditorPanel::drawMeshView` rendert die Vorschau jeden Frame neu (`RenderWorldPreview`); ob der Renderer bei unveränderter Kamera/Szene cached, liegt außerhalb des Slice (Backend).
- `StreamingDebugView::drawJobs`: Kosten und Locking von `ThreadPool::stats()` unbekannt (Quelle `HE_Core/JobSystem`).

**Editor_17.md**

- EDT-1701: tatsächliche ms pro Frame beim Sculpten bei res 513/1025 (Release) und Anteil Smooth-Vollkopie; Einfluss von `updateTerrains` (Chunk-Rebuild) dürfte dominieren, wenn der Brush groß ist.
- EDT-1709: Kosten von ~20 k `AddRectFilled` im Fit-Modus eines 4K-Bildes; Verfügbarkeit eines Repeat-Samplers in allen sechs Backends (`CreateImGuiTexture`).
- EDT-1705: Standard-Rastergröße des Wassers und typische Body-Anzahl.
- EDT-1704: Größe typischer Content-Bäume beim Tutorial (Sandbox ist klein, daher in der Praxis gering).
- `TextureViewerPanel::load` lädt Texturen vollständig im UI-Thread (`cm.loadAsset`, `toDisplayRgba` kopiert Level 0, 16k-Texturen ≈ 1 GB, Raw-Datei komplett eingelesen und dekodiert); nicht gemessen, bei sehr großen Bildern vermutlich spürbarer Freeze. Kanalwechsel erzeugt pro Klick eine volle CPU-Maskenkopie und neue GPU-Textur (`drawCanvas:320-333`).
- `ToolSetupDialog`: Größe von `GitProbe`/`GitInstallPlan` nicht ausgemessen, Kopierkosten daher geschätzt.

**Editor_18.md**

- Kosten von `HcEditorUtil::listAssets` bei realen Projekten (EDT-1801): Anzahl .hasset, OS-Cache.
- Anteil von `drawEmbeddedTree`/`laidEmbeddedCopy` an der Canvas-Zeit bei Seiten mit 20–30 Komponenten (EDT-1804).
- Kosten von `loadAsset(path)` für bereits geladene Assets (EDT-1808), `uiThemeValueFor` pro Property (EDT-1807).
- Dauer der PreConstruct-Sandbox pro Commit (`runDesignTimePreConstruct`, EDT-1821).
- Dateischreibzeit von `saveAsset` im Live-Preview (EDT-1810).
- Tiefe des Undo-Stacks von `EditorUndo` (EDT-1819) und Anzahl Splines/Collider in typischen Szenen (EDT-1815).
- Ob `UIWidgetTree::elements` je `nullptr` enthält (EDT-1816).
- `anyHidden`-Walk-Kosten auf macOS bei großen Szenen (EDT-1811).

**Editor_19.md**

- Tatsächliche Kosten von `ViewportToolbar::render` pro Frame (Anzahl `CalcTextSize`-Aufrufe, EDT-1902/1912) mit dem Profiler-Panel messen, bevor gecacht wird.
- Ob `pick` mit ungültigen Entities im Snapshot (gelöscht, aber noch im zuletzt extrahierten `RenderWorld`) vorkommt: Dann gibt `pick` (ViewportPick.cpp:41-67) eine ungültige Entity als Treffer zurück, weil der Lock-Check nur bei `reg.valid(owner)` läuft. Aufrufer ViewportPanel.cpp:1759/1783 auf Nachprüfung lesen (nicht vertieft).
- Ob `RenderObject::transform` immer affin ist (für `glm::affineInverse`, EDT-1907).
- Ob `streamingCells` absichtlich im Show-Popup fehlt (EDT-1903).
- Ob `SetViewMode` pro Frame im Backend billig ist (ViewportPanel.cpp:958).
- Größe von `EditorDeps` und `DEPLOY_GAME` (CMake-Post-Build-Kosten).

**Editor_20.md**

- EDT-2001: Realer Frame-Anteil von `RenderWorldPreview` + Extraktion + `propagateTransforms` im Stillstand; ob Slot-0-Textur zwischen Frames stabil bleibt.
- EDT-2004: Kosten von `magnitudeDb` × Spalten × Bänder pro Frame (6 Bänder, 600 Spalten im Kopf geschätzt).
- EDT-2006: Ob bei realen Rigs (150-300 Gelenke) die O(J²)-Kindersuche gegenüber den ImGui-Widgetkosten überhaupt messbar ist.
- EDT-2010: `AddLine` (AA) vs. `AddRectFilled` für die Wellenformspalten; Wirkung des Overview-Caches.
- `AudioWave::buildPeaks` (`AudioWaveformView.cpp:18-84`) läuft in einem Durchgang über alle Samples. Bei einem 20-Minuten-Stereo-Clip (≈ 115 M Frames) sind das einige hundert Millisekunden, wenn der Aufrufer es im UI-Thread macht (nicht in diesem Slice zu sehen).
- `BuildProgressDialog::s_log` wächst unbegrenzt (`:209`, `Build::log`); bei sehr langen Compiler-Ausgaben ist ein Deckel (z. B. letzte 50 000 Zeilen) ratsam, aber Verhalten (Vollständigkeit der MCP-Auskunft über `snapshot()`) wäre betroffen.
- `BoneMaskPanel.cpp:155-165`: Für Knoten mit Kindern zeichnet `TreeNodeEx(..., "%s", j.name)` den Gelenknamen, danach zeichnet `jointRow` ihn nochmals (`TextUnformatted`). Ob das Absicht ist (zweiter Name hinter der Checkbox) oder doppelt angezeigt wird, ließ sich ohne Laufzeit nicht klären (UI-Dump mit `HE_UI_DUMP_DIR` ansehen).
- `CinematicPanel.cpp:407-427`: Die Akteurzeile kennt weder Umbruch noch Clipping; bei vielen Akteuren läuft sie aus dem Fenster. Ob das praktisch vorkommt, ist offen.
- `CinematicPanel` ohne `appendSnapshots` (EDT-2003) ist evtl. gewollt; bitte mit dem Autor bestätigen.


---
# Funde (vollständig, nach Teilbericht)

## Editor Slice 1a – AnimatorStateMachineEditorPanel, AppMetadataRows, AssetAutosave, AssetRecoveryDialog, AssetStubWriter, AssetThumbnailCache, AudioEditorPanel, AudioMixerPanel

## Zusammenfassung

Die 16 Dateien (5283 Zeilen) sind insgesamt sauber und gut kommentiert; es gibt keine Funde mit Schwere Hoch. Die relevantesten Punkte sind (a) ein Logikfehler im Budget-Pfad von `AssetThumbnailCache::get`, der trotz vorhandener Disk-Cache-Datei eine synchrone GPU-Render-Runde auslösen kann, (b) ein nie invalidierter statischer Graph-Cache, der dem eigenen Kommentar ("Non-caching") widerspricht, und (c) mehrere Draw-/Frame-Pfade mit Dateisystemaufrufen oder Allokationen pro Frame (`AudioEditorPanel` Import-Zeile, `AppMetadataRows`, Asset-Autosave-Sammlung). Funde: 0 Hoch, 7 Mittel, 11 Niedrig. Alle Zeilenangaben wurden gegen die gelesenen Dateien geprüft; Messdaten liegen nicht vor, Schätzungen sind als solche gekennzeichnet.

---

### EDT-101 – Thumbnail-Cache: bei erschöpftem Upload-Budget wird die vorhandene Disk-Cache-Datei übergangen und gerendert
- **ID**: EDT-101
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AssetThumbnailCache.cpp:662-678, 722-729
- **Problem**: Level 1 (Disk-Cache lesen) läuft nur, wenn `s_uploadsLeft > 0` (8 pro Frame). Ist das Budget aufgebraucht, fällt `get()` ohne Prüfung der Datei in Level 2 und rendert — sofern `s_rendersLeft > 0` (2 pro Frame) — den Tile neu per `RenderAssetThumbnail` (synchroner GPU-Readback bzw. Asset-Load), obwohl eine gültige `.hthumb` existiert. Beim Öffnen eines Ordners mit z.B. 300 bereits gecachten Assets rendern also in den ersten Frames bis zu 2 Tiles pro Frame unnötig neu, und `writeFile` überschreibt danach die vorhandene Datei. Für `Scene` hat der Autor genau diese Falle erkannt (Zeilen 684-693), für alle anderen Typen nicht.
- **Code-Auszug**:
```cpp
if (!file.empty() && s_uploadsLeft > 0) { ... readFile(...) ... return e.texture; }
// Level 2: render it.
if (s_rendersLeft <= 0) return nullptr;
...
--s_rendersLeft;
const bool produced = ... s_renderer->RenderAssetThumbnail(*s_content, kind, renderId, kThumbSize, pixels);
```
- **Lösung**: Ist `file` gesetzt, aber `s_uploadsLeft <= 0`, sofort `return nullptr` (Tile versucht es im nächsten Frame wieder, Icon-Fallback bleibt). Alternativ billig prüfen, ob die Datei existiert, und nur dann warten. Skizze:
```cpp
if (!file.empty())
{
    if (s_uploadsLeft <= 0) return nullptr;      // retry next frame; never render past a cache we did not consult
    ... readFile ...
}
```
  Der Szenen-Sonderfall (689-693) vereinfacht sich dadurch (der Zweig `s_uploadsLeft > 0` entfällt).
- **Erwarteter Effekt**: Schätzung: vermeidet bis zu 2 GPU-Renders/Frame während der ersten Frames eines gecachten Ordners; spürbar bei Ordnern mit vielen Meshes/Materialien (ms-Bereich pro Render, abhängig vom Backend).
- **Risiko / Verifikation**: Tiles ohne Cache-Datei werden weiter gerendert, aber erst, wenn das Upload-Budget im Frame nicht erschöpft ist; die Füllzeit eines uncached Ordners kann sich leicht verlängern. Test: `tests/` hat vermutlich Content-Browser-Thumbnail-Tests (nicht geprüft); neuen Test mit Fake-Renderer (Zähler `RenderAssetThumbnail`) und >8 vorab geschriebenen `.hthumb` ergänzen.

### EDT-102 – `loadFunctionGraph`: statischer Graph-Cache wird nie invalidiert (Kommentar widerspricht Code)
- **ID**: EDT-102
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektheit/Speicher)
- **Ort**: src/HE_Editor/AssetThumbnailCache.cpp:152-169
- **Problem**: Der Kommentar nennt den Loader "Non-caching" und grenzt ihn vom Cache des Material-Editors ab, "whereas a tile is generated once". Tatsächlich ist `graphs` eine Funktions-`static std::map`, aus der nie gelöscht wird (kein Eintrag bei `invalidate()`, `clear()`, `setContext()`/Projektwechsel). Folgen: (1) Wird eine verschachtelte Material-Funktion `F` bearbeitet, nutzt jeder später gerenderte Tile einer Funktion `G`, die `F` aufruft, weiter die alte `F`-Version bis zum Editor-Neustart; (2) bei einem Projektwechsel liefert derselbe `relPath` im neuen Projekt den Graphen des alten; (3) der Cache wächst monoton. Zusätzlich lädt `s_content->loadAsset(relPath)` die Funktion und entlädt sie nie wieder.
- **Code-Auszug**:
```cpp
static std::map<std::string, HE::MaterialGraph> graphs;
if (auto it = graphs.find(relPath); it != graphs.end()) return &it->second;
const HE::UUID id = s_content->loadAsset(relPath);
...
return &(graphs[relPath] = std::move(g));
```
- **Lösung**: Den Cache aus der Funktion in den Dateiscope heben (`s_fnGraphs`, `std::unordered_map`, Zeiger bleiben bei Node-Containern stabil), und (a) in `makeMaterialFunctionScratch` zu Beginn jeder Tile-Erzeugung leeren (Tile wird nur einmal pro Version erzeugt, der Cache muss nur während eines Aufrufs die Zeiger stabil halten), oder (b) in `clear()`/`setContext()`/`invalidate()` leeren. Variante (a) entspricht dem Kommentar ("non-caching") und ist die einfachste.
- **Erwarteter Effekt**: Behebt veraltete Tiles und unbegrenztes Wachstum; Performance neutral (Tile-Erzeugung ist selten).
- **Risiko / Verifikation**: `materialFunctionScratch` ist öffentlich und "für Tests" gedacht; vorhandene Tests (vermutlich in `tests/test_material_graph.cpp`, nur nach Namen zu suchen, nicht ganz lesen) laufen lassen. Neuer Test: Funktion F ändern (neuer JSON), Tile von G erneut erzeugen, generierten GLSL vergleichen.

### EDT-103 – `AudioEditorPanel`: Import-Zeile eines Raw-Tabs ruft pro Frame `std::filesystem::relative` auf
- **ID**: EDT-103
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AudioEditorPanel.cpp:524-546, 1102-1115 (zusätzlich 735-736)
- **Problem**: Solange ein Tab mit einer rohen .wav/.ogg geöffnet ist, ruft `render()` jeden Frame `writeTargetFor()` auf. Darin läuft `std::filesystem::relative(src.parent_path(), t.root, ec)` (intern `weakly_canonical` auf beiden Pfaden, also Dateisystem-Syscalls) sowie zweimal `isEngineDefaultPath`, und danach wird `target` (String mit `relDir / stem`) neu gebaut. Das Ergebnis ändert sich nur, wenn sich der Pfad oder die Projektwurzel ändert.
- **Code-Auszug**:
```cpp
const WriteTarget wt = writeTargetFor(ctx, assetPath);   // pro Frame
...
t.relDir = std::filesystem::relative(src.parent_path(), t.root, ec);
```
- **Lösung**: `WriteTarget` samt `target`-String im `State` cachen (neues Feld `WriteTarget wt; std::string wtKey;`), Schlüssel = `contentRoot()` + `engineContentRoot()` + Dev-Mode-Flag; nur bei Schlüsseländerung neu berechnen. `runExtract` ruft `writeTargetFor` weiter direkt (einmalige Aktion).
- **Erwarteter Effekt**: Schätzung: spart pro Frame 2-4 Syscalls plus ca. 4-6 String-Allokationen, solange ein Raw-Tab sichtbar ist; spürbar v.a. auf Windows/Netzlaufwerken.
- **Risiko / Verifikation**: Gering. Die Import-Zeile muss nach Projektwechsel korrekt bleiben (Schlüssel enthält die Roots). Manuell: Raw-.wav öffnen, Projekt wechseln, Zielpfad prüfen.

### EDT-104 – `AppMetadataRows`: Stat-Syscalls und String-Schlüssel pro Frame im Draw
- **ID**: EDT-104
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AppMetadataRows.cpp:121-131, 135-146, 198-207
- **Problem**: Solange Project Settings > Application oder der Export-Dialog sichtbar ist, laufen pro Frame: `fs::is_regular_file(resolveProjectFile(...))` für Icon-Datei und Splash-Bild (je 1 Syscall plus Pfadkonstruktion), und `iconPreviewTexture` baut bei jedem Aufruf (pro Vorschau-Größe, also zweimal bei gleichzeitigem Settings+Export) einen Schlüssel aus 5 Strings, `std::to_string` und `fs::last_write_time` (weiterer Syscall), nur um zu erkennen, dass sich nichts geändert hat. Der Thumbnail-Cache derselben Slice (`kStatIntervalSec = 1.5`) löst genau dieses Problem mit gedrosseltem Re-Stat.
- **Code-Auszug**:
```cpp
const auto mtime = file.empty() ? fs::file_time_type{} : fs::last_write_time(file, ec);
const std::string key = p.appIconName + "|" + p.appIconColor + "|" + file.string() + "|"
                      + std::to_string(static_cast<long long>(mtime.time_since_epoch().count())) + "|" + std::to_string(px);
```
- **Lösung**: Pro Zeile/Slot den Dateistatus ("exists", "mtime") nur alle ~1 s neu abfragen (`ImGui::GetTime()` als Takt, Werte im `Preview`-Slot bzw. in einer kleinen Struct `FileProbe{ std::string path; double at; bool exists; time mtime; }` halten). Den Schlüssel nur neu bilden, wenn eine der Eingaben (Name/Farbe/Pfad-String/mtime) gewechselt hat — Name/Farbe/Pfad lassen sich billig als einzelne Felder statt als zusammengesetzter String vergleichen.
- **Erwarteter Effekt**: Schätzung: ca. 3-4 Syscalls und ca. 8 String-Allokationen pro Frame weniger, solange die Seite offen ist. Absolut klein, aber exakt die Klasse "Dateisystem im Draw" aus BRIEF_EDITOR.
- **Risiko / Verifikation**: Eine extern ersetzte Icon-Datei erscheint bis zu 1 s später. Kein UI-Test nötig; Sichtprüfung. Hinweis: `Preview::handle` und `tex` sind redundant (`tex` ist aus `handle` ableitbar, Zeilen 100-101, 173-174).

### EDT-105 – Asset-Autosave: Dirty-Liste samt `std::function`/Strings wird jeden Frame neu gebaut
- **ID**: EDT-105
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AssetAutosave.cpp:240-290 (Aufrufer EditorApplication.cpp:13704-13709, EditorUI.cpp:1061-1077), Panel-Seite z.B. AnimatorStateMachineEditorPanel.cpp:149-163, AudioEditorPanel.cpp:619-633
- **Problem**: `updateAssetAutosave` läuft laut Header "one call per frame". Pro Frame ruft `EditorUI::appendAssetSnapshots` 15 Panels ab; jedes Panel iteriert seine States und legt für jede schmutzige Datei einen `AssetSnapshotSource` an: `resolveSavePath(relPath)` (String-Allokation), ein `std::function` mit Capture (`[cm,&st]`, 16 Byte: je nach STL im SBO oder auf dem Heap), `out` ohne `reserve`. Danach baut `update()` jeden Frame `keyed` (reserve), ein `unordered_set<std::string>` (Allokationen je Key), ruft `keyFor` (liefert `std::string` per Wert, d.h. Kopie des gecachten Keys) und iteriert `m_live`. Die eigentliche Arbeit (Schreiben) passiert nur alle 60 s; das Prunen ist die einzige Frame-pflichtige Aufgabe, und die muss nicht 60x pro Sekunde laufen. Bei vielen offenen schmutzigen Tabs sind das einige zig Heap-Allokationen/Frame.
- **Code-Auszug**:
```cpp
std::vector<HE::Ed::AssetSnapshotSource> dirty;
EditorUI::appendAssetSnapshots(ctx, dirty);   // pro Frame
m_assetAutosave.update(nowMs, dirty);
...
std::unordered_set<std::string> dirtyKeys;     // pro Frame in update()
```
- **Lösung**: In `updateAssetAutosave` drosseln: Sammlung/Prune nur, wenn seit dem letzten Lauf >= ~1000 ms vergangen sind (oder eine Tab-Dirty-Generation sich geändert hat), `m_live.empty()` und kein Panel dirty ist der billige Early-out. Zusätzlich `keyFor` als `const std::string&` zurückgeben und `normalised(m_root)` in `configure()` einmal berechnen (derzeit pro `relativeOf`-Aufruf, also pro Datei und Tick, 2x `weakly_canonical`, Zeile 156).
- **Erwarteter Effekt**: Schätzung: Allokationen pro Frame bei N schmutzigen Tabs von ca. 3N auf ca. 0 im Durchschnitt; bei 0 schmutzigen Tabs bleibt es bei 15 leeren Panel-Iterationen (vernachlässigbar).
- **Risiko / Verifikation**: Das Prunen verzögert sich bis ~1 s: Stürzt der Editor in dieser Sekunde nach einem Save ab, bietet er eine bereits gespeicherte Kopie als "ungesichert" an (das Recovery-Dialog meldet `changedSince`). Alternativ ohne Verhaltensänderung: nur SBO-sicher machen (kleinere Captures) und `reserve`. Tests: vorhandene AssetAutosave-Tests (Fake-Uhr, laut Header) um den gedrosselten Pfad erweitern.

### EDT-106 – `AudioEditorPanel::appendSnapshots` kopiert beim Autosave das komplette PCM
- **ID**: EDT-106
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AudioEditorPanel.cpp:625-631
- **Problem**: Der Schreiber legt `AudioAsset copy = *a;` an und übergibt ihn an `writeAssetTo(RuntimeAsset&, ...)`. `copy` enthält `audioData` (laut Kommentar des Panels "tens of megabytes"), d.h. jede Autosave-Runde eines schmutzigen Audio-Tabs kopiert die gesamte Audio-Payload im UI-Thread, obwohl sich nur der kleine Edit-Chunk (`AudioEdit`) geändert hat. (Das ASM-Panel kopiert dagegen ein kleines JSON-Asset, unkritisch.)
- **Code-Auszug**:
```cpp
AudioAsset copy = *a;
return cm->writeAssetTo(copy, dest);
```
- **Lösung**: `ContentManager::writeAssetTo` auf `const RuntimeAsset&` umstellen (Signatur in ContentManager.h:89 ist non-const; vermutlich nur aus Bequemlichkeit) und `*a` direkt übergeben. Falls der Writer intern mutiert: Edit-Chunk-Pfad (`saveAsset` schreibt "samples exactly as loaded") nutzen. Als Zwischenlösung die Nutzdaten kurz verschieben (`std::swap`) statt kopieren ist riskanter, weil der Zeiger im ContentManager lebt.
- **Erwarteter Effekt**: Schätzung: spart je Autosave-Tick einen memcpy der Clip-Größe (10-100 MB, einige bis einige 10 ms Frame-Hitch) pro schmutzigem Audio-Tab.
- **Risiko / Verifikation**: Const-Korrektheit von `writeAssetTo` und seinen Unterklassen-Writern prüfen (unsicher, nicht gelesen); Test: Asset mit Edit speichern, Snapshot schreiben und byte-identisch zu `saveAsset` vergleichen.

### EDT-107 – `AudioEditorPanel`: Dekodierung, Peaks und Analyse laufen synchron im ersten Frame des Tabs
- **ID**: EDT-107
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AudioEditorPanel.cpp:159-183, 222-297, 695-722
- **Problem**: Beim ersten Frame eines Tabs werden im UI-Thread nacheinander `AudioEngine::decodeToPcm16` (Vorbis, ganze Datei), `AW::buildPeaks` und `analyze()` (Vollpass über alle Samples) ausgeführt. Der Kommentar in `analyze` benennt selbst "tens of millions of samples per clip". Für lange Ambience-Loops (der Zweck des Tabs) ist das ein spürbarer Freeze beim Öffnen des Tabs; die Tab-Ansicht kann während dieser Zeit weder neu zeichnen noch reagieren. Peaks und Analyse sind reine Funktionen über unveränderliche PCM-Daten und eignen sich für den Job-System-Worker.
- **Code-Auszug**:
```cpp
const AudioAsset* clip = clipOf(ctx, st);                 // decodeToPcm16 on first call
if (!st.peaksDone)    { st.peaks    = AW::buildPeaks(wclip); st.peaksDone    = true; }
if (!st.analysisDone) { st.analysis = analyze(*clip);        st.analysisDone = true; }
```
- **Lösung**: Decode+Peaks+Analyse als ein Job auf dem JobSystem (`std::future`/Atomic-Flag im State), Panel zeigt "Analysing…" bis fertig. PCM ist unveränderlich (siehe Kommentar 156-158), also kein Locking der Samples nötig; State muss nur den Tab-Abbruch (`forget`) abfangen (Handle auf Job halten, Ergebnis verwerfen). Als Minimalschritt: Peaks und Analyse in einem Durchlauf fusionieren (zwei Vollpässe → einer).
- **Erwarteter Effekt**: Schätzung/Messung nötig: Dekodierung eines 60 s-Stereo-Ogg liegt im Bereich 10-50 ms, Analyse+Peaks einer 10-Minuten-Datei zig ms; mit Job entfällt der Frame-Hitch.
- **Risiko / Verifikation**: Moderat (Nebenläufigkeit im Editor). Vorher messen (siehe "Unsicher"). Test: `analyze` ist nach Auslagerung in Header/anonymen Namespace testbar; vorhandene Audio-Panel-Tests (falls vorhanden) mit synchronem Stub-Executor laufen lassen.

### EDT-108 – Thumbnail-Cache: Re-Stat aller Tiles im selben Frame und doppelte Typ-/Hash-Lookups pro Tile und Frame
- **ID**: EDT-108
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/AssetThumbnailCache.cpp:623-656, 903-916, 566-586
- **Problem**: (1) `lastStat` startet bei `-1e9`, alle sichtbaren Tiles stat'en im ersten Frame und danach synchron alle 1.5 s im selben Frame: bei 300 sichtbaren Tiles etwa 600 Syscalls (`last_write_time` + `file_size` in `sourceStampOf`, Zeilen 906-908) in einem Frame, regelmäßig. (2) Jeder `get()`-Aufruf (pro sichtbarem Tile und Frame) macht `thumbnailKindOf` (EditorAssetTypeCache-Lookup, Pfad-Hash) *und* `s_entries[absPath]` (zweiter Pfad-Hash) auch für bereits fertige Tiles, bevor `state == Ready` geprüft wird.
- **Code-Auszug**:
```cpp
ThumbnailKind kind;
if (!thumbnailKindOf(absPath, kind)) return nullptr;   // Hash 1 (+ evtl. Disk-Sniff)
Entry& e = s_entries[absPath];                          // Hash 2
```
- **Lösung**: (a) Beim Anlegen eines Eintrags `lastStat = now - jitter` (z.B. `hash(absPath) % 1.5s`) setzen, damit die Re-Stats über das Intervall verteilt werden; zusätzlich ein einziger Syscall statt zwei (`stat`-basiert, z.B. plattformneutral nicht trivial → mtime+size via `fs::directory_entry` wäre ebenfalls 1 Syscall; sonst belassen). (b) Zuerst `s_entries.find(absPath)`; nur wenn nicht gefunden die Typfrage stellen und einen Eintrag anlegen. Dann kostet ein fertiger Tile einen Lookup und keinen Typ-Lookup.
- **Erwarteter Effekt**: Schätzung: glättet einen periodischen Syscall-Burst im ms-Bereich (Windows/Netzwerklaufwerke stärker); spart ca. 1 Hash pro Tile und Frame. Ohne Messdaten klein.
- **Risiko / Verifikation**: Kein Verhaltensunterschied außer zeitlicher Streuung der Aktualisierung (max. 1.5 s). Der Kommentar bei 627-633 (Typ vor Eintrag, um Map-Einträge für Skripte zu vermeiden) bleibt erfüllt, wenn bei Nicht-Fund zuerst der Typ geprüft wird.

### EDT-109 – State-Machine-Canvas: Modell-Lambdas bauen pro Frame Vektoren und Strings
- **ID**: EDT-109
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/AnimatorStateMachineEditorPanel.cpp:450-485, 525-600
- **Problem**: Pro Frame: `m.nodeIds` legt einen neuen `vector<int>` an, `m.pins` liefert pro Aufruf einen neuen `vector<Pin>` mit zwei Elementen (identisch für alle Knoten), `m.links` ruft pro Transition zweimal `findStateByName` (lineare String-Vergleiche, O(T·S)), und `drawNodeBody` baut pro Knoten zwei `std::string` (`clipState`, `spaceState`) nur zum Ausgeben. Außerdem werden `findStateById` pro Callback linear gesucht (`getPos`, `setPos`, `title`, `drawNodeBody` je Knoten: O(S²) pro Frame). Bei typischen Graphen (10-50 Zustände) unkritisch; mit 200 Zuständen aber messbar.
- **Code-Auszug**:
```cpp
m.pins = [](int) -> std::vector<GraphEditor::Pin> { return { {0,"In",...}, {0,"Out",...} }; };
const HE::AnimationState* from = findStateByName(st.graph, t.fromState);
const std::string clipState = (s->clipId == HE::UUID{}) ? std::string("(no clip)") : ...
```
- **Lösung**: `clipState`/`spaceState` als `const char*` (kein `std::string`); `pins` liefert eine `static const std::vector`-Kopie bzw. (API-Änderung in GraphEditor) einen `span`; in `links` einmal pro Frame eine `unordered_map<string,int>` name→id aufbauen (oder Index-Lookup per `reserve`d Vector); `findStateById` per `unordered_map<int, size_t>` oder über Index, den GraphEditor schon liefert.
- **Erwarteter Effekt**: Schätzung: wenige Allokationen/Frame bei kleinen Graphen, O(S²)→O(S) bei großen; erst ab ca. 100 Zuständen relevant.
- **Risiko / Verifikation**: Gering; `GraphEditor::Model` ist gemeinsame API (siehe Cross-Modul). UI-Test in `tests/test_*_ui.cpp` für den Panel-Pfad (falls vorhanden) laufen lassen.

### EDT-110 – State-Machine-Seitenleiste: feste `char`-Puffer kürzen Namen still, Transitionsliste ist nicht geclippt
- **ID**: EDT-110
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Editor/AnimatorStateMachineEditorPanel.cpp:534-549, 633-638, 650-678, 700-706
- **Problem**: Alle Namensfelder werden über `char buf[64/128]` + `snprintf` in ein Eingabefeld gespiegelt: pro Transition und Frame drei `snprintf` (From/To/Param). Ist ein Name länger als der Puffer (63 bzw. 127 Zeichen), wird er beim ersten Tastendruck still auf die Pufferlänge gekürzt und in den Graph zurückgeschrieben (Datenverlust). Außerdem werden alle Transitionen gezeichnet (7 Widgets + `PushID` je Zeile), ohne Row-Clipping aus CLAUDE.md Abschnitt 4, d.h. 100 Transitionen = 700 Widgets pro Frame.
- **Code-Auszug**:
```cpp
char fb[64], tb[64], pb[64];
std::snprintf(fb, sizeof(fb), "%s", t.fromState.c_str());
...
if (ImGui::InputText("From##t", fb, sizeof(fb)))  { t.fromState = fb; structuralEdit = true; }
```
- **Lösung**: `ImGui::InputText(label, &t.fromState)` via `misc/cpp/imgui_stdlib.h` (wird in AppMetadataRows/AudioMixerPanel bereits genutzt) — keine Puffer, keine Kürzung, keine Kopien. Zeilenhöhe der Transitionszeile konstant halten und mit `ImGuiListClipper` zeichnen (Konvention aus `OutlinerPanel.cpp`); bei `IsItemDeactivatedAfterEdit` für Textfelder ist die `structuralEdit`-Semantik unverändert.
- **Erwarteter Effekt**: Behebt stilles Kürzen; spart pro Transition und Frame 3 `snprintf`; Clipping begrenzt Widgetzahl bei großen Maschinen.
- **Risiko / Verifikation**: Das Eingabefeld "From/To" ist Freitext und kann auf nicht existierende Zustände zeigen (siehe "Unsicher"); Verhalten ändert sich dadurch nicht. Test: Zustandsname > 64 Zeichen setzen, Edit auslösen, Name bleibt vollständig.

### EDT-111 – State-Machine-Panel: duplizierte Reload-Logik und doppelter Asset-Lookup
- **ID**: EDT-111
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/AnimatorStateMachineEditorPanel.cpp:73-86, 186-202, 230-242
- **Problem**: `reloadFromDisk` und `reloadByContentPath` setzen identisch `loaded=false; dirty=false; collabMirror={}; collabSyncMirror={}` (der Kommentar in 234 nennt sie "the same three lines", es sind vier). Kommt ein dritter Mirror hinzu, muss man beide Stellen ändern. In `stateFor` wird `getAnimatorStateMachine(st.assetId)` zweimal aufgerufen (75, 81), mit zwei getrennten `if`-Initialisierungen.
- **Code-Auszug**:
```cpp
st->loaded = false; st->dirty = false; st->collabMirror = {}; st->collabSyncMirror = {};   // 2x
```
- **Lösung**: `static void markForReload(State& st)` einführen und aus beiden Funktionen aufrufen; in `stateFor` `const auto* asset = ...getAnimatorStateMachine(...)` einmal holen und beide JSONs daraus lesen. `reloadByContentPath` kann zudem `reloadFromDisk` nicht ersetzen (anderer Schlüssel), nur den Teil nach dem Lookup teilen.
- **Erwarteter Effekt**: Weniger Duplikat, kein messbarer Laufzeitgewinn.
- **Risiko / Verifikation**: Praktisch null; vorhandene MCP-Animator-Tests (Hooks `McpAnimatorHooks`) laufen lassen.

### EDT-112 – `AudioEditorPanel::render` ist eine ca. 630 Zeilen lange Funktion
- **ID**: EDT-112
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/AudioEditorPanel.cpp:653-1286
- **Problem**: `render()` vereint Lazy-Load, Voice-Verfolgung, Toolbar (ca. 150 Zeilen), linke Info-Spalte (ca. 215 Zeilen), Wellenform-Pane, Tastaturkürzel und den verzögerten Extract. Die Info-Spalte besteht aus Abschnitten (Format/Levels/Silence/Loop seam/Cut/Volume curve/Mixer bus/Preview/Import), die nur `clip`, `an`, `editable`, `st` lesen. Hohe Zyklomatik, schwer testbar; Nebenabhängigkeiten (z.B. `syncTrimView` zweimal, 745 und 928, weil Reihenfolge implizit wichtig ist) sind nur über Kommentare dokumentiert.
- **Code-Auszug**:
```cpp
void render(AppContext& ctx, const std::string& assetPath, const ImVec2& pos, const ImVec2& size)
{ ... }   // Zeilen 653-1286
```
- **Lösung**: In anonyme Funktionen zerlegen: `followVoice(st, ...)`, `drawToolbar(ctx, st, ...)`, `drawInfoColumn(ctx, st, ...)` (mit Unterfunktionen je `SeparatorText`), `drawWavePane(...)`, `handleShortcuts(...)`. Ein gemeinsames Kontext-Struct (`struct Frame { AppContext& ctx; State& st; const AudioAsset& clip; AudioAsset* editable; bool canEdit, editsLocked, audioReady; }`) vermeidet lange Parameterlisten.
- **Erwarteter Effekt**: Lesbarkeit/Testbarkeit; keine Laufzeitänderung.
- **Risiko / Verifikation**: Rein mechanisch; ImGui-ID-Stack und `WrapText`-Scopes (CLAUDE.md: vor `EndChild()` freigeben) beim Verschieben erhalten. Test: bestehende UI-Tests mit Pixel-Dump (`HE_UI_DUMP_DIR`) vor/nach vergleichen.

### EDT-113 – `AudioEditorPanel`: Footer-Texte werden jeden Frame formatiert und mit Umbruch vermessen
- **ID**: EDT-113
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/AudioEditorPanel.cpp:1168-1197, 949-1088 (viele `formatTime/formatFrames/formatGain`-Aufrufe), 1066
- **Problem**: Pro Frame: `AW::readout(...)` (String), `snprintf` in einen 320-Byte-Hint, zwei `ImGui::CalcTextSize(..., wrap_width)` über den langen Hinttext (Umbruchsberechnung ist O(Textlänge) pro Aufruf), mehrere `AW::formatTime(...)` Temp-Strings in der Infospalte, `std::string bus = editable->edit.bus;` (Kopie). Der Hint hängt nur von (Dauer, framesPerPx·canvasW, curveMode, trackpadPointer) ab.
- **Code-Auszug**:
```cpp
const float whereH = ImGui::CalcTextSize(where.c_str(), nullptr, false, wrapW).y;
const float hintH  = ImGui::CalcTextSize(hint, nullptr, false, wrapW).y;
```
- **Lösung**: `hint` und `hintH` im State cachen, Schlüssel = (`canvasW`, `fpp`, `curveMode && canEdit`, `trackpad`, `durationSec`). `where` ändert sich mit Hover/Playhead — das bleibt pro Frame. `formatTime`-Strings der statischen Spalten (`an.durationSec`) einmal beim Analysieren erzeugen.
- **Erwarteter Effekt**: Schätzung: 1-2 Textlayouts und ca. 5 String-Allokationen pro Frame weniger; im Mikrosekundenbereich, nur der Vollständigkeit wegen (Tab ist nur im Vordergrund aktiv).
- **Risiko / Verifikation**: Gering; Layoutunterschiede sichtbar im UI-Dump. Nur umsetzen, wenn ein Profiler Editor-UI-Zeit auf diesem Tab zeigt.

### EDT-114 – `AudioMixerPanel`: Engine-Abgleich und Voice-Zählung jeden Frame, `std::function`-Parameter, Flag-lastiges `drawStrip`
- **ID**: EDT-114
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Editor/AudioMixerPanel.cpp:58-64, 81-91, 103-105, 240, 255, 274-298; src/HE_Scene/src/AudioEngine.cpp:594-612 (Cross-Modul)
- **Problem**: (1) `audio.applyBusConfig(cfg)` läuft bei offenem Fenster jeden Frame (Master- + je Bus `createBus`/`setBusVolume`/`setBusEq` mit jeweiliger Map-Suche und EQ-Vergleich); (2) `audio.busVoiceCount(name)` iteriert pro Strip alle `m_impl->sounds` mit String-Vergleich, d.h. O(Busse × Stimmen) pro Frame; (3) `listenFor` ist ein linearer Suchlauf, wird pro Bus mehrfach aufgerufen (255, 294, in `applyListenState` doppelt): O(B²); (4) `drawStrip` erhält zwei `const std::function&`, die pro Strip pro Frame aus Lambdas konstruiert werden (Heap-Allokation möglich je nach Capture-Größe/STL), plus neun Parameter inkl. nullable `solo` und Flags `removable`/`eqLit` — Master- und Bus-Strip unterscheiden sich in Wahrheit nur durch drei Eigenschaften.
- **Code-Auszug**:
```cpp
audio.applyBusConfig(cfg);                       // jeden Frame
StripResult drawStrip(const char* id, const char* title, float* volume, bool* muted, bool* solo,
                      int voices, bool removable, const std::function<void(float)>& onVolume,
                      const std::function<void()>& onListen, bool eqLit = false)
```
- **Lösung**: `applyBusConfig` nur nach Änderung aufrufen (Projekt gewechselt, Bus hinzugefügt/entfernt, `commit`, MCP-seitig geänderte Konfiguration über eine Versionszähler-Abfrage am `AudioBusConfig`) oder zumindest `busVoiceCount` einmal pro Frame in ein `unordered_map<string,int>`/`vector` füllen (eine Iteration über `sounds`). `onVolume/onListen` als Template-Parameter oder `function_ref`; `StripSpec{ title, id, removable, eqLit }` statt Flag-Argumente. `s_listen` als `unordered_map<string, BusListen>` oder Index parallel zu `cfg.buses`.
- **Erwarteter Effekt**: Schätzung: Mikrosekunden; nur bei offenem Mixer-Fenster. Eher Vereinfachung als Performance.
- **Risiko / Verifikation**: `applyBusConfig` wird laut Kommentar bewusst jeden Frame ausgeführt, damit MCP-seitige Bus-Änderungen sofort wirken; ein Dirty-Mechanismus am `AudioBusConfig` muss alle Schreibpfade abdecken (unsicher, nicht geprüft). Tests: bestehende Mixer-/AudioBusConfig-Tests, plus MCP-Pfad manuell.

### EDT-115 – `AssetRecoveryDialog`: kopiert die komplette Offer-Liste jeden Frame
- **ID**: EDT-115
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/AssetRecoveryDialog.cpp:81, 111-127, 203-219
- **Problem**: `const std::vector<AssetRecoveryEntry> offers = *ctx.assetRecoveryOffers;` kopiert jeden Frame, solange der Dialog offen ist, die Liste (je Eintrag 4 Strings). Die Zeilenschleife selbst sammelt die Aktion in `act`/`actKey` und braucht keine Kopie; nur "Restore All"/"Delete All" (203-219) iterieren, während die Callbacks die Quellliste verändern. Zusätzlich pro Zeile pro Frame: `std::filesystem::path(...).filename().string()` und `savedAtText` (localtime_r + strftime) sowie zwei Durchläufe über die Liste (Höhenberechnung und Zeichnen).
- **Code-Auszug**:
```cpp
// A copy: the buttons remove entries from the vector it points at.
const std::vector<HE::Ed::AssetRecoveryEntry> offers = *ctx.assetRecoveryOffers;
```
- **Lösung**: Mit `const auto& offers = *ctx.assetRecoveryOffers;` zeichnen (Aktionen laufen ohnehin nach der Schleife) und nur in den "All"-Zweigen eine lokale Kopie (oder Key-Liste) anlegen. `name`/`when` je Eintrag beim ersten Zeichnen cachen (z.B. Felder in `AssetRecoveryEntry`, oder `displayName` in `pending()` befüllen). Hinweis: Nach `ctx.restoreAssetRecovery` im "Restore"-Zweig (156-161) wird `offers.size()` nicht mehr benutzt, also kein Dangling.
- **Erwarteter Effekt**: Vernachlässigbar (seltener modaler Dialog); Hauptgewinn ist Einfachheit.
- **Risiko / Verifikation**: Referenz auf `*ctx.assetRecoveryOffers` darf nach einem Callback nicht weiterbenutzt werden (Iterator-Invalidierung); daher Zeilenschleife unverändert lassen und die "All"-Zweige kopieren. Kein bestehender Test sichtbar (Dialog ist ImGui); mit `HE_UI_DUMP_DIR` prüfen.

### EDT-116 – `AssetStubWriter`: `if`-Kette statt `switch`, `assetTypeFromName` mit hart codierter Obergrenze
- **ID**: EDT-116
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/AssetStubWriter.cpp:62-121, 192-205
- **Problem**: `writeAssetStub` prüft `type` in acht aufeinanderfolgenden `if`-Blöcken (mit gleichem Muster "Konstanter JSON-String → `addChunk`"), während `isCreatableAssetType` im selben File ausdrücklich ohne `default` per `switch` arbeitet, damit neue Typen bewusst klassifiziert werden. Beim Stub-Schreiben fehlt diese Absicherung: ein neuer erstellbarer Typ, der einen Pflicht-Chunk braucht, wird ohne Compilerwarnung ohne Chunk geschrieben. `assetTypeFromName` läuft linear mit `std::string == const char*` bis zum hart codierten `AssetType::Sequence` (Zeile 199); das Enum-Ende wird so nur per Kommentar abgesichert, ein neues Enum-Mitglied nach `Sequence` bricht lautlos (Enums.h:113 ist aktuell das letzte).
- **Code-Auszug**:
```cpp
if (type == HE::AssetType::InputAction) { const char* json = "{\"valueType\":\"Button\"}"; w.addChunk(HAsset::CHUNK_IACT, json, std::strlen(json)); }
...
for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(HE::AssetType::Sequence); ++i)
```
- **Lösung**: Kleine Hilfsfunktion `addJson(w, chunk, std::string_view)`, dann `switch (type)` ohne `default` mit je einem `case`; leere Fälle explizit (`case Material: ... break;`). Für den Namen→Typ-Lookup einen `AssetType::Count`-Sentinel in `Enums.h` einführen (gemeinsam mit den beiden dort bereits dokumentierten Switches) oder eine einmal gebaute `static const std::unordered_map<std::string, AssetType>` verwenden.
- **Erwarteter Effekt**: Compiler-gestützte Vollständigkeit; Laufzeit unerheblich (MCP/Create-Menü).
- **Risiko / Verifikation**: Das Enum liegt in HE_Core (Cross-Modul). Test: Round-Trip-Test, der für jeden `isCreatableAssetType`-Typ `writeAssetStub` + Laden prüft (laut Header-Kommentar existiert bereits so ein Tool-Test, nicht geprüft); `assetTypeFromName(assetTypeName(t)) == t` für alle `t`.

### EDT-117 – `AssetThumbnailCache`: duplizierte Box-Filter-Schleifen und Dummy-`ThumbnailKind`
- **ID**: EDT-117
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/AssetThumbnailCache.cpp:109-137, 251-284, 781-813, 305-310
- **Problem**: (1) `makeTextureThumbnail` und `captureToTile` enthalten nahezu denselben Box-Filter mit gedeckelten Taps (`sx0/sx1/stepX/acc/taps`), nur Quellformat/Zeilenrichtung/Alpha-Handling unterscheiden sich. (2) `thumbnailKindOf(absPath, kind)` füllt `kind` für sechs Typen (Texture, Font, Particle, Widget, Scene, MaterialFunction→Material) mit einem Platzhalter `ThumbnailKind::Material`, der laut Kommentaren "unused" ist; die Funktion kombiniert so zwei Fragen ("hat der Typ ein Tile?" und "welche Renderer-Art?"). (3) `makeFontThumbnail` legt `static const char kSample[]` an, kopiert es in eine `std::string` und dekodiert es mit `uiUtf8Decode` zeichenweise.
- **Code-Auszug**:
```cpp
case HE::AssetType::Texture: out = ThumbnailKind::Material; return true;   // "kind unused"
static const char kSample[] = "Aa"; ... const std::string sample = kSample;
```
- **Lösung**: Gemeinsamen `boxDownscale(src, srcStride/format, rect, out)`-Helfer extrahieren (Lambda für Pixel-Fetch), Typ-Dispatch als `enum class TileSource { Renderer, Cpu, ViewportCapture }` plus separates `rendererKindOf`, und `std::string_view`/Konstante statt `static`+Kopie.
- **Erwarteter Effekt**: Weniger Code und weniger Fehlerstellen (der Texturpfad musste schon einmal wegen "Thema 92" korrigiert werden); Laufzeit gleich.
- **Risiko / Verifikation**: Tests existieren laut Header für `textureThumbnail`/`captureToTile` ("Public for tests"); Pixel-für-Pixel-Gleichheit vor/nach Refactoring prüfen.

### EDT-118 – Kleinkram: FNV-1a mehrfach kopiert, ungenutzte Includes, schwere Header
- **ID**: EDT-118
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/AssetAutosave.cpp:174-176 und AssetThumbnailCache.cpp:896-898 (gleiche Konstanten), AudioEditorPanel.cpp:24,30-31 (`<cctype>`, `<iterator>`, `<limits>`), AudioEditorPanel.h:2-4, AnimatorStateMachineEditorPanel.h:2-4
- **Problem**: (1) Dieselbe FNV-1a-64-Schleife steht inline in beiden Dateien (Konstanten `1469598103934665603ull`/`1099511628211ull` kommen laut Suche in mind. 14 Dateien vor, u.a. `CollabDocSync.cpp`, `SceneFileStamp.h`, `HpakFormat.h`). (2) In `AudioEditorPanel.cpp` werden `<cctype>`, `<iterator>`, `<limits>` nicht verwendet (kein `isalnum`, `std::distance`, `numeric_limits` im File; per Grep geprüft); `<cfloat>` wird für `FLT_MIN` gebraucht. (3) `AudioEditorPanel.h` und `AnimatorStateMachineEditorPanel.h` ziehen `EditorUI.h` und `<imgui.h>` nur für `AppContext` bzw. `ImVec2` (Rückgabe-/Parametertypen) in jeden Includer; `AssetAutosave.h` für `AssetSnapshotSource`.
- **Code-Auszug**:
```cpp
std::uint64_t h = 1469598103934665603ull;
for (unsigned char c : rel) { h ^= c; h *= 1099511628211ull; }
```
- **Lösung**: Eine gemeinsame `constexpr std::uint64_t fnv1a64(std::string_view)` in einem kleinen Core-Header (HE_Core, `Types/Hash.h` o.ä.; vorher prüfen, ob bereits ein Helfer existiert) und in den Editor-Dateien verwenden. Ungenutzte Includes entfernen. In den Panel-Headern `AppContext` vorwärts deklarieren (`struct AppContext;`) und `ImVec2` über `<imgui.h>` beibehalten oder `struct ImVec2;` nur, wenn der Header nicht inline benutzt; `AssetSnapshotSource` bräuchte ein kleines `AssetSnapshotSource.h`.
- **Erwarteter Effekt**: Schätzung: etwas kürzere Kompilierzeit durch kleinere Header; weniger Duplikate.
- **Risiko / Verifikation**: Hash-Ergebnis muss bitgleich bleiben (Dateinamen im Cache/Autosave hängen daran): Test mit festen Eingaben (`fileNameFor("a/b")`, `AssetAutosave::keyFor`). Include-Bereinigung per Build aller Targets prüfen.

---

## Gelesen

Dateien vollständig gelesen (alle 16, ja): AnimatorStateMachineEditorPanel.cpp (858 Z.), AnimatorStateMachineEditorPanel.h, AppMetadataRows.cpp, AppMetadataRows.h, AssetAutosave.cpp, AssetAutosave.h, AssetRecoveryDialog.cpp, AssetRecoveryDialog.h, AssetStubWriter.cpp, AssetStubWriter.h, AssetThumbnailCache.cpp (980 Z.), AssetThumbnailCache.h, AudioEditorPanel.cpp (1288 Z.), AudioEditorPanel.h, AudioMixerPanel.cpp, AudioMixerPanel.h; keine Datei ausgelassen. Fremddateien nur punktuell (EditorApplication.cpp:13690-13715, EditorUI.cpp:1055-1100, AudioEngine.cpp:474-624, Enums.h:85-113, ContentManager.h:89) zur Belegung.

## Editor Slice 2 – CinematicTimeline.*, Collab* (ActivityBar, Controller, DocSync, EntityOps, Panel)

## Zusammenfassung

Der Slice (11 Dateien, ca. 10.000 Zeilen) ist funktional sorgfältig und sehr gut kommentiert, aber `CollabController` ist ein 4667-Zeilen-Gott-Objekt (davon 1215 Kommentarzeilen in der .cpp und 908 von 1682 Zeilen in der .h), und an mehreren Stellen laufen pro Frame Operationen, die mit der Szenen- bzw. Dokumentgröße skalieren. Die schwersten Funde sind die lineare Netz-ID-Tabelle (`m_netIds`: O(N²) beim Seeding, lineare Suche pro Frame und pro Remote-Nachricht, Negativ-Treffer scannen die ganze Registry) und die Voll-Serialisierung des Level-Script-/GameInstance-Graphen pro Frame in einer Session. Dazu kommen blockierende Datei-I/O auf dem UI-Thread, die Datei vor dem Typ-Gate liest, Syscalls pro Frame im Collaboration-Fenster und viel Duplikation (Adapter, Batch-Approve/Deny, Teilnehmer-Lookup). Der Cinematic-Streifen ist unauffällig (Eigenlogik sauber getrennt und testbar); nur Row-Clipping fehlt und `draw()` ist sehr lang. Funde: 2 Hoch, 6 Mittel, 7 Niedrig. Nichts davon wurde gemessen; Schätzungen sind als solche gekennzeichnet.

---

### EDT-201 – Level-Script-/GameInstance-Graph wird in einer Session jeden Frame vollständig serialisiert
- **ID**: EDT-201
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CollabDocSync.cpp:127-219` (diffInto, hasLocalChanges); Aufrufer `src/HE_Editor/EditorApplication.cpp:10241-10248`, `10339-10353` (fremder Slice, nur zur Belegung)
- **Problem**: `updateAssetCollabSync` läuft pro Frame, sobald eine Session aktiv ist. Für die Tabs `LevelScriptPanel::kTabPath` und `GameInstancePanel::kTabPath` gibt es kein Dirty-Flag. Gehalten: `publishDocDeltas` setzt `alwaysDiff` und ruft jeden Frame `diffInto`. Nicht gehalten: `hasLocalChanges` wird jeden Frame aufgerufen. Beide Pfade rufen `capture()` für alle 7 `Kind`s, also `nodeToJson` für jeden Node, jede Variable, jeden Kommentar, und vergleichen/ersetzen anschließend die Strings. Der Kommentar bei `publishDocDeltas` (EditorApplication.cpp:10319-10322) sagt selbst, dass das "a few hundred-node graphs ... milliseconds of pure JSON" kostet und deshalb nicht pro Frame laufen darf; `alwaysDiff` tut genau das für diese zwei Tabs. Solange der Tab offen ist, ist es reine Verschwendung (Idle-Frames).
- **Code-Auszug**:
```cpp
// CollabDocSync.cpp:205-207
for (int k = 0; k < kKindCount; ++k)
{
    const auto current = capture(doc, static_cast<Kind>(k));
// EditorApplication.cpp:10339-10342
const bool alwaysDiff = (key == LevelScriptPanel::kTabPath || key == GameInstancePanel::kTabPath);
const bool dirty = alwaysDiff || EditorUI::tabHasUnsavedEdits(absPath);
```
- **Lösung**: (1) Billigen Änderungszähler an den zwei Dokumenten einführen: `HorizonCode::Graph` bekommt ein `uint64_t revision`, das die Editier-Funktionen (GraphEditor/`HcGraphHost::Host::onEdit`, MCP `hc.endEdit`) erhöhen; alternativ hängt `EditorApplication` einen Zähler an `onEdit`. (2) `hasLocalChanges`/`diffInto` nur ausführen, wenn sich der Zähler seit dem letzten Diff geändert hat (Zähler im `DocMirror` merken: `uint64_t seenRevision`). (3) Ohne Zähler als Zwischenlösung: Diff nur alle ~250 ms drosseln (`nowMs - lastDiffMs`), da Deltas ohnehin gebündelt werden. 
- **Erwarteter Effekt**: Schätzung: bei einem Graphen mit einigen hundert Nodes spart das pro Frame etwa 0,1-2 ms CPU plus Tausende kurzer String-Allokationen im Idle; nur in Sessions mit offenem Tab. Messen mit Scoped-Timer um `diffInto`.
- **Risiko / Verifikation**: Die Revisionszähler müssen an allen Mutationsstellen erhöht werden (MCP-Tools, Undo/Redo, Remote-Apply); ein vergessener Pfad würde Edits nicht senden. Das Throttling-Variante ist risikoärmer (nur Latenz). Absichern über `tests/test_collab_docsync.cpp` plus einen Test "Edit ohne Zähler-Bump wird trotzdem gefunden" (bei Throttling) bzw. je Mutationspfad.

---

### EDT-202 – Netz-ID-Tabelle ist ein Vektor: O(N²) beim Seeding, lineare Suche pro Frame, Registry-Scan bei jedem Fehltreffer
- **ID**: EDT-202
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CollabController.cpp:2702-2783` (netIdFor, entityForNetId, forgetNetId, seedNetIds), `2801-2812` (publishDestroy); `CollabController.h:1590` (`m_netIds`)
- **Problem**: `m_netIds` ist `std::vector<std::pair<uint64_t,uint32_t>>`. `netIdFor(handle)` durchsucht linear nach dem Handle, `entityForNetId(net)` linear nach der Net-ID. Konsequenzen: (a) `seedNetIds()` ruft für jede Entity `netIdFor`, das jedes Mal alle bisher eingetragenen Paare vergeblich durchläuft: N²/2 Vergleiche (10k Entities: ca. 5·10⁷; 100k: ca. 5·10⁹, also ein mehrsekündiger Freeze beim Join/Hosten). (b) Jeder Frame mit selektierter Entity in einer Session ruft `subjectFor` und `publishComponents` (beide `netIdFor`) linear auf (EditorApplication.cpp:5010, 5066), dazu `entityForNetId` je Remote-Transform (Callback `onTransform`, bis 30 Hz) und je fremder Selektion in der Presence-Schleife (EditorApplication.cpp:4666). (c) Bei einem Fehltreffer (unbekannte Entity, z. B. Presence-Selektion auf eine Entity, die lokal noch nicht existiert) scannt `entityForNetId` die gesamte Registry (`reg.view<EntityIdComponent>().each`) und cached das Negativergebnis nicht, also wiederholt sich das jeden Frame. (d) `forgetNetId` ist `remove_if` über alles.
- **Code-Auszug**:
```cpp
std::uint64_t CollabController::netIdFor(std::uint32_t entityHandle)
{
    for (const auto& [net, handle] : m_netIds)
        if (handle == entityHandle) return net;
...
void CollabController::seedNetIds()
{
    m_netIds.clear();
    m_world->registry().view<entt::entity>().each([&](auto e) {
        netIdFor(static_cast<std::uint32_t>(entt::to_integral(e)));
    });
```
- **Lösung**: (1) Zwei Hash-Maps statt Vektor: `std::unordered_map<uint32_t,uint64_t> m_netByHandle; std::unordered_map<uint64_t,uint32_t> m_handleByNet;`, Einfügen mit `emplace` (erster Eintrag gewinnt, wie bisher die lineare Suche). (2) `seedNetIds` reserviert (`registry.size()`) und trägt direkt ein, ohne vorherige Suche. (3) Die UUID-zu-NetId-Ableitung (`lo & ~(1<<63)`, Zeilen 2735-2736 und 2755-2756 sind dupliziert) in eine freie Funktion `netIdOf(const EntityIdComponent&)` ziehen. (4) Fehltreffer: statt Registry-Scan pro Aufruf einmalig pro Frame/Registry-Änderung eine komplette `handleByNet`-Aktualisierung (z. B. über `registry.on_construct<EntityIdComponent>()`-Observer, der neue Entities einträgt) oder zumindest ein kurzlebiges Negativ-Cache (Net-ID, Frame-Nummer). (5) `forgetNetId` über beide Maps.
- **Erwarteter Effekt**: Schätzung: Seeding von O(N²) auf O(N) (Sekunden → Millisekunden bei 50k+ Entities); pro-Frame-Lookups von O(N) auf O(1). Bei Szenen unter ca. 2k Entities nicht spürbar.
- **Risiko / Verifikation**: Heutiges Verhalten bei doppelten Einträgen (gleiche Net-ID, anderes Handle nach Handle-Recycling) muss bewusst festgelegt werden: `forgetNetId` entfernt aktuell alle Einträge mit dieser Net-ID. Tests: `tests/test_collab_controller.cpp` (seedNetIds/entityForNetId bereits genutzt) um Fälle "Entity gelöscht und Handle recycelt", "unbekannte Net-ID, die später entsteht" (Negativ-Cache darf nicht veralten) und einen Seed-Zeitbudget-Test mit 20k Entities ergänzen.

---

### EDT-203 – CollabDocSync::diffInto: doppelte JSON-Kopien, O(n²)-Suche, tote Bedingung, Vollserialisierung pro Remote-Batch
- **ID**: EDT-203
- **Kategorie**: A / C / D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Editor/CollabDocSync.cpp:127-200`, `221-283`
- **Problem**: (1) `const auto current = capture(...)` ist const, daher kopiert `for (...) known.emplace(id, json)` (Zeile 197) jede JSON-Zeichenkette ein zweites Mal und baut die Map jedes Mal komplett neu (`known.clear()`), auch wenn sich nur ein Item geändert hat. (2) Die Entfernungserkennung `std::find(ids.begin(), ids.end(), id)` (Zeile 161) und das `remove_if` mit `find` (Zeilen 186-188) sind O(n²); bei UI-Widgets mit mehreren tausend Elementen sind das Millionen Vergleiche pro Diff. (3) Tote Bedingung: innerhalb von `if (ids != mirror.order[k])` ist `ids == mirror.order[k]` immer falsch, `!(sameSet && ids == mirror.order[k])` also immer wahr; `sameSet` ist zudem nur ein Größenvergleich und irreführend benannt. Außerdem wird `Delta d` samt `joinIds(ids)` (String-Bau) vor der Entscheidung gebaut und bei `onlyAppended` verworfen. (4) `applyDeltas` ruft nach jedem erfolgreichen Batch `capture()` für den ganzen Dokumentbestand (Zeilen 269-281), d. h. ein einzelner Remote-Knoten-Drag (bis 30 Hz) kostet O(Dokument) JSON-Serialisierung.
- **Code-Auszug**:
```cpp
// :161
if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
// :192
if (!(sameSet && ids == mirror.order[k]) && !onlyAppended)
    out.push_back(std::move(d));
// :196-198
known.clear();
for (const auto& [id, json] : current) known.emplace(id, json);
```
- **Lösung**: `current` nicht-const lassen; nach der Schleife `known` per `std::move(json)` befüllen (`known.reserve(current.size())`). Für die Entfernungsprüfung die bereits vorhandene Map nutzen: zuerst alle `current`-IDs in ein `std::unordered_set` (oder markiere in `known` mit einem Generationszähler), dann `mirror.order[k]` gegen das Set prüfen. `onlyAppended` berechnen, bevor `Delta d`/`joinIds` gebaut wird; die tote Teilbedingung und `sameSet` entfernen. Für `applyDeltas`: Mirror nur für die tatsächlich angewendeten Items nachziehen (JSON neu aus dem Dokument holen, weil `upsert` normalisieren kann) statt das gesamte Dokument.
- **Erwarteter Effekt**: Schätzung: halbiert Allokationen/Kopien pro Diff, macht Entfernungsprüfung linear; pro Remote-Batch O(geänderte Items) statt O(Dokument). Wirkung bei Graphen ab einigen hundert Items messbar.
- **Risiko / Verifikation**: Die Serialisierung nach `upsert` kann vom empfangenen JSON abweichen; deshalb Mirror aus dem Dokument neu lesen und nicht aus `d.json` übernehmen (sonst "Ping-Pong"-Diffs). `tests/test_collab_docsync.cpp` deckt Diff/Apply/Reorder ab; ergänzen: reine Reorder-Fälle, "nur angehängt", "Item entfernt und neu eingefügt", Echo-Freiheit nach Apply.

---

### EDT-204 – publishAsset liest die ganze Datei, bevor das Typ-Gate entscheidet; `istreambuf_iterator`, Kopie statt Move; dreifach dupliziert
- **ID**: EDT-204
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CollabController.cpp:3136-3181` (publishAsset), `4417-4421` (publishAssetCreate), `4587-4604` (catchUpJoiner)
- **Problem**: (1) Der Hook feuert laut Kommentar (3143-3147) bei jedem `ContentManager::saveAsset`, auch bei Importen von Meshes/Texturen. `publishAsset` liest die komplette Datei in einen Vektor (3138-3141) und prüft erst danach `assetTypeTravels(assetTypeOf(fullPath))` (3149-3150). Bei einer Session ohne Large-Asset-Sync (Normalfall) wird jede große `.hasset`-Datei also auf dem UI-Thread komplett gelesen und anschließend verworfen. `publishAssetCreate` macht es richtig (Gate vor dem Lesen, Zeilen 4414-4415). (2) `std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {})` liest Byte für Byte ohne Vorab-Reserve (typisch deutlich langsamer als ein einziger `read`) und reallokiert exponentiell. (3) `up.bytes = bytes;` (Zeile 3178) kopiert die Daten, `bytes` wird danach nicht mehr gebraucht; der Header (CollabController.h:196-199) rechnet ohnehin mit dreifacher Residenz einer Datei. (4) `catchUpJoiner` liest für jeden Joiner alle angefassten Dateien synchron im Frame (Callback `onParticipantJoined`, aus `update()`).
- **Code-Auszug**:
```cpp
std::ifstream f(fullPath, std::ios::binary);
if (!f) return;
std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
...
up.bytes    = bytes;
```
- **Lösung**: Hilfsfunktion `static bool readFileBytes(const std::string& path, std::vector<uint8_t>& out)` (seekg/tellg + `out.resize` + ein `read`) einführen und an den drei Stellen verwenden. In `publishAsset` den Typ-Gate vor das Lesen ziehen (`isSyncableAsset` → Typ → dann lesen). `up.bytes = std::move(bytes)`; `size` vorher merken (steht bereits danach, deshalb `const size_t size = bytes.size()` vor dem Move).
- **Erwarteter Effekt**: Schätzung: bei einem 100-MB-Import in einer Session ohne Large-Asset-Sync entfällt ein Lesevorgang von einigen hundert ms bis Sekunden auf dem UI-Thread; Einlesen mit einem `read` ist typischerweise um ein Mehrfaches schneller als der Iterator-Weg. Messen mit einem großen Mesh.
- **Risiko / Verifikation**: Reihenfolge `!f`/`bytes.empty()` vs. Gate ändert nur den Zeitpunkt stiller `return`s, kein beobachtbares Verhalten. `tests/test_collab_controller.cpp` (Asset-Publish-Fälle) laufen lassen; neuen Test ergänzen: "nicht reisender Typ → Datei wird nicht geöffnet" (z. B. über einen nicht lesbaren Pfad, der trotzdem lautlos zurückkehrt).

---

### EDT-205 – Collaboration-Fenster: Interface-Enumeration und HMAC bei jedem Frame
- **ID**: EDT-205
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CollabPanel.cpp:917-931` und `967-972`; `CollabController.cpp:2557-2568`; `src/HE_Net/src/Socket.cpp:1577-1609`, `1521-1575`; `src/HE_Net/src/SecureTransport.cpp:654-677`
- **Problem**: Solange das Fenster als Host offen ist, ruft `DrawCollabWindow` jeden Frame `HE::Net::socketOwnsAddress(seen)` auf. Das ruft `socketLocalAddresses()`, das `getifaddrs` (Linux/macOS) bzw. `GetAdaptersAddresses` (Windows, mit 16-KB-Puffer; auf Windows im Bereich mehrerer Millisekunden üblich, unsicher/messen) ausführt und für jede Adresse `inet_pton` plus String-Bau macht. Zusätzlich `sessionFingerprint()` pro Frame: bis 16 `m_peers.find` plus HMAC-Berechnung plus String-Bau. Beide Ergebnisse ändern sich während der Session praktisch nicht.
- **Code-Auszug**:
```cpp
const bool ours   = HE::Net::socketOwnsAddress(seen);
...
if (const std::string fp = collab->sessionFingerprint(); !fp.empty())
```
- **Lösung**: Panel-lokal cachen: `static std::string s_cachedSeen; static bool s_cachedOurs;` und neu berechnen, wenn `seen != s_cachedSeen` oder alle ~5 s. Fingerprint im Controller beim Wechsel auf `Status::Joined/Hosting`/Peer-Established einmal berechnen und als `std::string m_fingerprint` ablegen (leer bis Handshake fertig; deshalb bei leerem Cache jeden Frame erneut versuchen, danach nicht mehr).
- **Erwarteter Effekt**: Schätzung: entfernt einen Syscall (Linux/macOS einige µs bis zehn µs, Windows potenziell ms) und einige Allokationen pro Frame, nur bei offenem Fenster.
- **Risiko / Verifikation**: Änderung der lokalen Interfaces (VPN an/aus) wird erst mit dem Cache-Intervall sichtbar; rein kosmetisch. Kein Test nötig; per Profiler-Zähler (Aufrufe pro Sekunde von `socketLocalAddresses`) prüfen.

---

### EDT-206 – Request-Zeilen der Host-Queue bauen pro Frame Pfade, Strings und durchsuchen Teilnehmer/Entity-Teilbäume
- **ID**: EDT-206
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CollabPanel.cpp:1040-1126`, `1128-1235`, `1253-1296`; `CollabController.cpp:3940-3985` (opDisplayName, opHeldBy)
- **Problem**: Bis zu 128 offene Anfragen (`kMaxPendingOps`, Zeile 3875) werden jeden Frame ungeclippt gezeichnet. Pro Zeile: `opDisplayName` (bei Entity: `entityForNetId` inkl. möglichem Registry-Scan, siehe EDT-202; bei Datei `std::filesystem::path(...).filename().string()` mit Allokationen), `opHeldBy` (für Entities ein Stack-DFS über den ganzen Teilbaum mit `netIdFor` + `lockFor` je Knoten; für Dateien `assetLockInfo` mit FNV über den Pfad), `std::filesystem::path(op.newPath).filename().string()` (Zeile 1074), ein Teilnehmer-Linearscan je Requester (1104-1115) und je Gruppe (1184). Zusätzlich pro Frame `std::vector<char> drawn`, `std::vector<size_t> group` je Zeile, `std::string headline`/`title`. Die Ergebnisse (Anzeigenamen, Halter) ändern sich nur bei Lock-/Welt-Änderungen. Das widerspricht der Konvention "Row-heavy panels draw only visible rows" (CLAUDE.md Abschnitt 4).
- **Code-Auszug**:
```cpp
const std::string name = collab->opDisplayName(op);
...
const std::string heldBy = collab->opHeldBy(op);
...
std::filesystem::path(op.newPath).filename().string().c_str()
```
- **Lösung**: (1) `PendingAssetOp` um gecachte Felder `displayName`/`newDisplay` erweitern, beim Einreihen (`queueAssetOp`) berechnen. (2) `opHeldBy` nur für sichtbare Zeilen (`ImGui::IsRectVisible`/`ImGuiListClipper`, Zeilenhöhe konstant machen) auswerten und höchstens alle ~250 ms pro Zeile auffrischen. (3) Bündel-Gruppierung einmal beim Ändern von `m_pendingOps` berechnen statt jeden Frame. (4) Teilnehmer-Lookup über Hilfsfunktion `participantById` (siehe EDT-211).
- **Erwarteter Effekt**: Schätzung: bei wenigen Anfragen kaum messbar; bei einer Bündel-Anfrage mit 100 Dateien (typischer Multi-Delete) entfallen pro Frame ca. 100× Pfad-/Lock-Lookups und viele kleine Allokationen. Nur Host mit offenem Fenster.
- **Risiko / Verifikation**: Gecachter Name einer Entity kann veralten (Umbenennung); beim Rendern nur neu auflösen, wenn `entityForNetId` greift. UI-Test nach Muster `tests/test_*_ui.cpp` mit `HE_UI_DUMP_DIR` für Bündel-Zeilen ergänzen.

---

### EDT-207 – CollabController ist ein Gott-Objekt mit 280- und 610-Zeilen-Funktionen
- **ID**: EDT-207
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/CollabController.cpp:196-477` (startHosting mit ca. 150-Zeilen-Lambda, 327-474), `1053-1661` (wireCallbacks), gesamte Datei 4667 Zeilen; `CollabController.h:46-1682`
- **Problem**: Eine Klasse vereint: Netzwerk-Stack-Aufbau, Port-Mapping/Directory-Worker, LAN-Discovery, lokale Identität/Avatar (Config + Datei), Asset-Locking, Asset-Operationen mit Host-Inbox, Revisionsordnung, Netz-ID-Tabelle, Transform-/Komponenten-Throttling, Settle/Catch-up und einen Per-Peer-Rate-Limiter (~100 Zeilen plus 40 Zeilen Doku). `startHosting` enthält den kompletten Router-/Directory-Registrierungsablauf als Inline-Lambda (Mapping-Ergebnisse → Texte → Pinhole → HTTPS-Registrierung); `wireCallbacks` ist eine einzige Funktion mit 20 Lambdas, in denen `onLockChanged`/`onLockDenied` jeweils über 100 Zeilen mit mehrfachen Early-Returns und verschachtelten Schleifen haben. Das erschwert Tests (der Limiter z. B. ist nur über den Controller prüfbar) und Lesbarkeit.
- **Code-Auszug**:
```cpp
m_registerFuture = std::async(std::launch::async, [endpoint, sid, port16, displayName] {
    RegisterResult r;
    ...  // ca. 150 Zeilen bis Zeile 474
```
- **Lösung**: Schrittweise, ohne Verhaltensänderung: (1) Das Lambda als freie Funktion `RegisterResult registerWithDirectory(endpoint, sid, port, displayName)` in `CollabDirectory.cpp` (zusammen mit `remedyFor`, `releaseNetworkResources`, `pumpDirectory` und den Konstanten). (2) `RemoteNoteLimiter` (budget/summary/mutex, 5 Funktionen) als eigene kleine Klasse mit Unit-Test. (3) Identity (`identityStorage`, `setLocal*`, `resampleSquareRgba`) in `CollabIdentity.cpp`. (4) `wireCallbacks` pro Callback in benannte Member (`onLockChangedImpl`, `onLockDeniedImpl`, `onAssetUpdatedImpl`) zerlegen; das gemeinsame Muster "Pfad in m_heldAssetLocks / m_pendingLockReleases suchen" in Helfer (`bool contains(vec, path)`, `bool eraseFirstMatching(...)`).
- **Erwarteter Effekt**: Rein strukturell: bessere Lesbarkeit/Testbarkeit, kleinere TUs, schnellere inkrementelle Builds. Keine Laufzeitänderung.
- **Risiko / Verifikation**: Mechanisches Verschieben; `tests/test_collab_controller.cpp`, `test_net_collab.cpp`, `test_mcp_tools_collab.cpp` müssen unverändert grün bleiben. Public-API der Klasse bleibt (Header-Kompatibilität).

---

### EDT-208 – CollabController.h wird über EditorApplication.h in ca. 100 Übersetzungseinheiten gezogen
- **ID**: EDT-208
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/CollabController.h:17-36` (Includes), `src/HE_Editor/EditorApplication.h:10`
- **Problem**: `CollabController.h` inkludiert `Net/CollabSession.h` (1211 Zeilen), `Net/LanBeacon.h`, `Net/PortMapper.h`, `Net/RelayFanout.h`, `Net/RelayTransport.h`, `<future>`, `<mutex>`, `<unordered_map>`. `EditorApplication.h` inkludiert den Controller direkt; `EditorApplication.h` selbst wird von rund 100 Dateien in `src/` und `tests/` eingebunden (grep). Jede dieser TUs parst den Netzwerk-Stack mit, obwohl die meisten nur `ctx.collab->inSession()` brauchen. Der Header hat selbst Pimpl-Ansätze (`class SceneStateProvider`, `unique_ptr<NetSession>`), hält aber `LanBeacon::Announcer/Browser`, `PortMapper::MappingHandle`, `std::future<RegisterResult>` und `RemoteNoteBudget` per Wert.
- **Code-Auszug**:
```cpp
#include <Net/CollabSession.h>
#include <Net/LanBeacon.h>
#include <Net/PortMapper.h>
#include <Net/RelayFanout.h>
#include <Net/RelayTransport.h>
```
- **Lösung**: In `EditorApplication.h` `class CollabController;` vorwärts deklarieren und `m_collab` als `std::unique_ptr<CollabController>` halten (oder die Member hinter ein Pimpl `struct Impl` legen: Directory-/LAN-/Limiter-Zustand). Die 18 Dateien, die `CollabController` wirklich benutzen, inkludieren den Header selbst (tun sie bereits per `#include "CollabController.h"`). Alternativ Teilung in `CollabController.h` (kleines Interface) und `CollabControllerImpl.h`.
- **Erwarteter Effekt**: Schätzung: spürbar kürzere Inkrementalbuilds des Editors, wenn `CollabSession.h`/Net-Header geändert werden (ca. 80 TUs weniger betroffen). Kein Laufzeiteffekt. Messen mit `-ftime-trace` bzw. `/d1reportTime`.
- **Risiko / Verifikation**: `m_collab` wird per Wert verwendet (`m_collab.update(...)` etc. in EditorApplication.cpp); Umstellung auf Zeiger ändert viele Aufrufstellen (mechanisch). `AppContext::collab` ist bereits ein Zeiger. Build aller Targets (Editor, `he_tests`) prüfen.

---

### EDT-209 – CollabDocSync-Adapter duplizieren Link-/Upsert-Logik; `applyOrder` ist O(n²); zwei handgeschriebene Zahlen-Parser
- **ID**: EDT-209
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/CollabDocSync.cpp:493-514`, `581-601`, `625-634`, `661-665`, `683-684`, `697-708`, `721-730` (Link-Code ×3), `305-326` (applyOrder), `41-60`, `526-550` (Parser)
- **Problem**: `HcAdapter`, `MatAdapter` und `ParticleAdapter` implementieren dasselbe Link-Handling dreimal: `linkJson` (String aus 4 Ints; in `ParticleAdapter::enumerate` inline), Upsert "parse vier Ints, `find_if` gleiche Endpunkte, sonst `push_back`", Remove "`find_if` über `linkKey`". `applyOrder` sucht für jede gewollte ID linear im Vektor (O(n²)); bei UI-Bäumen mit tausenden Elementen und jedem Reorder-Delta spürbar. `splitIds` und `parseFourInts` sind zwei Eigenbau-Parser für Ganzzahlen, die `std::from_chars` ersetzen könnte.
- **Code-Auszug**:
```cpp
for (std::size_t i = 0; i < items.size(); ++i)
{
    if (taken[i] || idOf(items[i]) != want) continue;
```
- **Lösung**: Template-Hilfsfunktionen `template<class L> std::string linkJson(const L&)`, `template<class V,class L> bool upsertLink(V& links, const L&)`, `template<class V> bool removeLinkByKey(V& links, int64 id)` (alle drei Link-Typen haben `srcNode/srcPin/dstNode/dstPin`). `applyOrder`: `unordered_map<int64,size_t>` einmal aufbauen. Parser per `std::from_chars` mit den bestehenden Obergrenzen.
- **Erwarteter Effekt**: ca. 60-80 Zeilen weniger; Reorder-Apply von O(n²) auf O(n). Gering.
- **Risiko / Verifikation**: Reihenfolge-Semantik von `applyOrder` (Unerwähnte ans Ende, relative Reihenfolge erhalten) muss identisch bleiben; Randfälle: doppelte IDs im Reorder-Delta, unbekannte IDs. Absichern mit den Reorder-Tests in `tests/test_collab_docsync.cpp`.

---

### EDT-210 – Cinematic-Streifen: kein Row-Clipping, pro Frame String-/Vektor-Allokationen, `draw()` mit 416 Zeilen
- **ID**: EDT-210
- **Kategorie**: D / A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/CinematicTimeline.cpp:532-948` (draw), `354-391` (buildRows), `606-850`
- **Problem**: (1) Die Zeilenschleife übergibt jede Zeile an ImGui (`PushID`, `Selectable`, `BeginPopupContextItem`, Items), unabhängig davon, ob die Zeile im sichtbaren Bereich des Child-Fensters liegt; CLAUDE.md verlangt für Row-heavy-Panels Clipping. Bei vielen Aktoren × Tracks (hunderte Zeilen) wächst der Aufwand linear. (2) Pro Frame und Zeile: `std::vector<Row>` in `buildRows`, `std::string name` mit `+= "  (missing)"` (Zeile 627-629), `trackLabel(tr) + "##cin_track"` (Zeile 662), pro Section `assetLabel` → `std::function` → `std::string`-Kopie (Zeile 750). (3) `buildRows` ist O(B·T): für jede Binding `findBinding` (Zeile 364, quadratisch in B) und eine Schleife über alle Tracks, danach für jeden Track erneut `findBinding` (Zeile 382); `draw` ruft `findBinding` pro Gruppenzeile ein weiteres Mal (Zeile 616). (4) `draw()` ist eine 416-Zeilen-Funktion mit bis zu sechs Verschachtelungsebenen (Zeilen → Items → Section/Key/Flag → Hit-Behandlung) und gemischter Eingabe-, Zeichen- und Drag-Logik.
- **Code-Auszug**:
```cpp
for (int r = 0; r < static_cast<int>(rows.size()); ++r)
{
    const Row& row = rows[r];
    const float rowY = rowsTop + M.rowH * static_cast<float>(r);
    ...
    if (ImGui::Selectable((trackLabel(tr) + "##cin_track").c_str(), selected, 0, ...
```
- **Lösung**: Zeilenhöhe ist konstant (`M.rowH`): `firstRow/lastRow` aus `ImGui::GetScrollY()` und Child-Höhe berechnen und nur diese Zeilen zeichnen (Playhead/Lane-Hintergrund unverändert). `buildRows` mit einer einmal gebauten `slot → bindingIndex`-Map (kleiner `unordered_map` oder sortierter Vektor) auf O(B+T). Labels in einem kleinen Per-Frame-Puffer (`char[]` + `snprintf`) oder gecacht pro Track; `assetName` nur für sichtbare Sections. `draw()` in `drawGroupRow`, `drawTrackRow`, `drawItem` und `applyDrag` zerlegen.
- **Erwarteter Effekt**: Schätzung: bei üblichen Sequenzen (unter 30 Zeilen) nicht messbar; bei großen Cinematics weniger ImGui-Submits und Allokationen. Hauptnutzen ist Lesbarkeit.
- **Risiko / Verifikation**: Item-Selektion/Drag darf nicht von Clipping abhängen (der Drag-Block ab Zeile 855 arbeitet mit Indizes und bleibt unberührt). `tests/test_cinematic_timeline.cpp` (headless über `buildRows`/`draw`) und `tests/test_ui_shot.cpp` als Regression; bildliche Prüfung per `HE_UI_DUMP_DIR`.

---

### EDT-211 – Duplizierte Muster im Controller: Batch-Approve/Deny, Pending-Op-Ersetzen, Teilnehmer-Lookup, FNV-1a
- **ID**: EDT-211
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `CollabController.cpp:4176-4214` (approveAssetOpBatch/denyAssetOpBatch), `3921-3929` und `4028-4035` (OurOp ersetzen/anhängen), Teilnehmer-Lookups `481-488`, `1227-1232`, `3286-3287`, `3322-3327`, `3612-3617`, `3749-3750`, `3804-3809`, `4384-4385`, `CollabPanel.cpp:1106, 1184, 1259`; FNV-1a `134-136`, `2316-2318`, `2849-2855`, `CollabDocSync.cpp:63-68`
- **Problem**: `approveAssetOpBatch` und `denyAssetOpBatch` sind bis auf das letzte `approveOne`/`denyOne` identisch (Herausnehmen der Batch-Zeilen). Das Muster "gleiche (op,path) ersetzen requestId sonst anhängen" steht zweimal (`requestEntityOp`, `requestOrPerformAssetOp`). "Teilnehmer per ID suchen" steht ca. elfmal als Schleife mit leicht unterschiedlichem Fallback. FNV-1a ist viermal handgeschrieben.
- **Code-Auszug**:
```cpp
std::vector<PendingAssetOp> taken;
for (auto it = m_pendingOps.begin(); it != m_pendingOps.end();)
{
    if (it->batchId == batch && it->batchOwner == owner) { taken.push_back(*it); it = m_pendingOps.erase(it); }
    else ++it;
}
```
- **Lösung**: `std::vector<PendingAssetOp> takeBatch(owner, batch)` privat (`stable_partition` + move) und die zwei Methoden darauf; `void trackOurOp(op, path, id, display)` für das Ersetzen; `const Participant* participantById(ParticipantId) const` im Controller (und von den Panels nutzbar); `constexpr uint64_t fnv1a(string_view)` in einem kleinen Header (Net- oder Editor-Util).
- **Erwarteter Effekt**: ca. 70 Zeilen weniger, ein Ort für Teilnehmer-Fallbacks. Keine Laufzeitänderung.
- **Risiko / Verifikation**: `assetSubject` normalisiert `\\` zu `/` innerhalb der Schleife (andere FNV-Varianten nicht), also die Hash-Funktion generisch und die Normalisierung separat halten; Hash-Werte sind Teil des Wire-Formats und dürfen sich nicht ändern. Tests: `test_collab_controller.cpp` (Batch-Fälle), `test_net_collab.cpp`.

---

### EDT-212 – CollabActivityBar: redundanter Zweig, doppelte String-Bildung pro Frame, implizite Aufrufreihenfolge, UTF-8-Schnitt
- **ID**: EDT-212
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/CollabActivityBar.cpp:41-46`, `64-100`, `105-127`, `141-166`, `168-220`
- **Problem**: (1) `if (s_batch.count == 0) { s_batch = {}; s_batch.since = now; } else s_batch.since = now;` setzt in beiden Zweigen `since = now`; `s_batch = {}` ist redundant (der Zustand ist bei `count == 0` bereits Standard). (2) `FooterWidth` und `DrawFooter` rufen jeweils `queueText` (baut `std::to_string`-Text, zwei Vektor-`size()`-Aufrufe) und `batchText`/`elide` (Teilstring-Kopien) pro Frame, also doppelt; im Gegensatz dazu hält `BuildProgressDialog.cpp:546` ein gemeinsames Modell pro Frame. (3) `collect()` (Ableeren der Controller-Puffer) läuft nur innerhalb von `FooterWidth`; wer nur `DrawFooter` aufruft, sammelt nie. Die Abhängigkeit steht nirgends dokumentiert. (4) `elide()` schneidet nach Bytes (`substr(0, 33)`) und kann ein UTF-8-Mehrbytezeichen (Umlaute in Datei-/Personennamen) in der Mitte teilen, was ungültiges UTF-8 an ImGui gibt (Darstellung als "?").
- **Code-Auszug**:
```cpp
if (s_batch.count == 0) { s_batch = {}; s_batch.since = now; }
else                     s_batch.since = now;
...
s = s.substr(0, kMaxLabelChars - 1) + "\xE2\x80\xA6";  // U+2026
```
- **Lösung**: Zweig zu `s_batch.since = now;` vereinfachen. Einen `struct Line { std::string text; bool personal; float alpha; }` pro Frame einmal in `FooterWidth` bauen und in `DrawFooter` wiederverwenden (oder `collect()` + Text-Bau in eine `prepare(ctx)`-Funktion mit Frame-Zähler). `elide` am letzten UTF-8-Lead-Byte (`(c & 0xC0) == 0x80` rückwärts überspringen) kürzen.
- **Erwarteter Effekt**: vernachlässigbar bei Laufzeit (ein paar kleine Allokationen pro Frame, nur in Session); Korrektheit bei Nicht-ASCII-Namen.
- **Risiko / Verifikation**: `elide` ändert sichtbaren Text nur bei Mehrbyte-Zeichen; kleiner Unit-Test mit "Jörg"-Länge am Limit.

---

### EDT-213 – Lineare Vektoren und String-Schlüssel auf Nachrichtenpfaden
- **ID**: EDT-213
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `CollabController.cpp:4477-4479` (noteSessionTouched), `2883-2887`, `1496-1501`, `1533`, `1546-1548` (m_heldAssetLocks/m_pendingLockReleases `std::find`), `2992-2996`, `3020` (revisionKey), `4272-4281` (acceptRemoteCreate); `CollabController.h:1527`, `1591`, `1604`
- **Problem**: `noteSessionTouched` (pro Doc-Delta-Publish und -Empfang, bis ca. 30 Hz) sucht linear in `m_sessionTouched` (wächst mit jeder angefassten Datei der Session). `m_heldAssetLocks`/`m_pendingLockReleases` sind kleine Vektoren; unkritisch, aber mehrfach linear durchsucht. `acceptRevision` baut pro empfangenem Frame `to_string(from) + '\n' + path` als Schlüssel (eine Allokation je Delta). `acceptRemoteCreate` testet bei vergebenem Namen bis zu 998 Kandidaten per `filesystem::exists` auf dem Host-UI-Thread; ein Peer, der in einer Schleife denselben Namen anlegt, löst das wiederholt aus (es existiert ein serverseitiger Rate-Limit-Grund `RateLimited`; Wirkung nicht geprüft).
- **Code-Auszug**:
```cpp
if (std::find(m_sessionTouched.begin(), m_sessionTouched.end(), relativePath) == m_sessionTouched.end())
    m_sessionTouched.push_back(relativePath);
```
- **Lösung**: `m_sessionTouched` als Vektor behalten (Reihenfolge wird beim Catch-up gebraucht) plus `std::unordered_set<std::string> m_sessionTouchedSet` für den Existenztest. `m_seenRevision` mit Key `std::pair<ParticipantId,std::string>` (eigener Hash) oder `unordered_map<ParticipantId, unordered_map<string,uint32_t>>` (löscht auch `forgetRevisionsFrom` ohne Präfixscan). `acceptRemoteCreate`: Kandidatenzahl auf ca. 50 begrenzen oder einmal das Verzeichnis listen.
- **Erwarteter Effekt**: gering (Zeichenketten-Vergleiche bei hunderten Einträgen, eine Allokation je Delta). Nur Sauberkeit/Skalierung.
- **Risiko / Verifikation**: `acceptRemoteCreate`-Begrenzung ändert das Verhalten bei vielen vergebenen Namen (Vorschlag fehlt dann) und ist daher optional. Tests `test_collab_controller.cpp` (Revisionsordnung, Catch-up).

---

### EDT-214 – Kleinigkeiten im Cinematic-Teil: Magic-Character-Vergleich, doppelte Dauer-Initialisierung
- **ID**: EDT-214
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/CinematicTimeline.cpp:447-453`, `108-121`
- **Problem**: `kindColour` entscheidet über den ersten Buchstaben des Gruppennamens (`g[0] == 'T'` / `'M'`), eine unsichtbare Kopplung an die Benennung in `Sequencer::targetGroup` (Umbenennung "Transform" → "Position" bricht die Farbe still). In `addTrack` steht `if (seq.duration <= 0.0f) seq.duration = kDefaultDuration;` in beiden Zweigen (Zeilen 116 und 120).
- **Code-Auszug**:
```cpp
const char* g = Sequencer::targetGroup(tr.channel.target);
if (g[0] == 'T') return IM_COL32(120, 180, 255, 255);
if (g[0] == 'M') return IM_COL32(190, 130, 255, 255);
```
- **Lösung**: Farbe direkt am `PropTarget` bestimmen (kleine `switch`-Funktion oder `Sequencer::targetGroupId(target)` als Enum) bzw. `targetGroup` in einen Enum-Rückgabewert umstellen. Den Dauer-Fallback einmal vor dem `if (kind == CameraCut)`-Zweig setzen.
- **Erwarteter Effekt**: Robustheit, 2 Zeilen weniger.
- **Risiko / Verifikation**: `Sequencer::targetGroup` wird im SequencerTimeline-Teil weiterverwendet (anderer Slice); dort nur ergänzen, nicht ändern. `tests/test_cinematic_timeline.cpp`.

---

### EDT-215 – Kommentare erzählen Fehlerhistorie statt Absicht
- **ID**: EDT-215
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `CollabController.cpp` (1215 von 4667 Zeilen sind Kommentare, neunmal "used to"), z. B. `3085-3096`, `2172-2175`, `1663-1670`; `CollabController.h` (908 von 1682 Zeilen), z. B. `79-96`, `182-199`, `1153-1165`; `CollabPanel.cpp:371-392`
- **Problem**: Viele Kommentare dokumentieren, was früher falsch war ("This list used to spell out `.hmat`, ...", "used to be one line of small red text", Anekdoten wie "418 stale forwards"). Das ist wertvoll als Commit-/Doc-Information, verdeckt aber im Code den eigentlichen Vertrag und macht die Datei schwer überfliegbar; die Funktionen selbst sind dadurch teils nur noch mit Scrollen verständlich. CLAUDE.md fordert "explains the why", nicht die Chronik.
- **Code-Auszug**:
```cpp
// This list used to spell out `.hmat`, `.huiw`, `.hcode`, `.hpart`, `.hasm`,
// `.hinput`; the engine writes none of those, so it matched NOTHING in a real
// project ...
```
- **Lösung**: Pro Block den heute gültigen Vertrag in 2-4 Zeilen belassen und die Fehlergeschichte in `docs/` (z. B. `docs/collab-sync-audit-*.md`, existiert laut EditorApplication.cpp:10237) bzw. Commit-Nachricht verweisen. Nur dort kürzen, wo ohnehin angefasst wird (keine Massenänderung).
- **Erwarteter Effekt**: Lesbarkeit; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Keines für Verhalten; keine Information löschen, die nirgends anders steht.

---

## Gelesen

Ja, alle 11 Dateien des Slice vollständig gelesen: `CinematicTimeline.cpp/.h`, `CollabActivityBar.cpp/.h`, `CollabController.cpp` (Zeilen 1-4667) und `.h` (1-1682), `CollabDocSync.cpp/.h`, `CollabEntityOps.h`, `CollabPanel.cpp` (1-1334) und `.h`; zur Belegung zusätzlich gezielt `EditorApplication.cpp` (Zeilen 1040-1080, 4640-4700, 4995-5085, 10195-10400), `Socket.cpp` (1500-1610), `SecureTransport.cpp` (640-680), `EditorRowWidgets.cpp` (490-540) und `CinematicPanel.cpp` (880-938).

## Editor Slice 3 – CollabPresenceBar … DocsPanel

## Zusammenfassung

Der Slice (19 Dateien, ca. 10 950 Zeilen) ist funktional sauber und sehr ausführlich kommentiert; die kleinen Logikklassen (`CollabSettle`, `CollabUndo`, `DeferredSlotFreeList`, `DocsLibrary`) sind ordentlich und getestet. Das Gewicht liegt bei zwei Panels: `ContentBrowserPanel.cpp` (5 062 Zeilen, davon `render()` ca. 4 400 Zeilen in einer Funktion) zeichnet Kacheln, Baum und Dialoge **ohne Clipping** und mit vielen Per-Frame-Allokationen und Dateisystemaufrufen. `DocsPanel` zeichnet jede Frame **alle** Sektionen einer Seite (der Node-Reference mit ca. 300 Sektionen) und führt die Volltextsuche jede Frame neu aus. Außerdem ein echter Defekt: `CppClassEditorPanel::reloadFromDisk` lädt nie neu. Insgesamt 20 Funde: 3 Hoch, 8 Mittel, 9 Niedrig. Alle Aufwandsangaben sind Schätzungen; es gab keine Messdaten.

## Funde

### EDT-301 – Content-Browser-Grid zeichnet alle Kacheln jeder Frame (kein Row-Clipping)
- **ID**: EDT-301
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ContentBrowserPanel.cpp:1644-1759` (Ordner-Kacheln), `1895-2327` (Datei-Kacheln); verwandt `813-884` (Baum)
- **Problem**: CLAUDE.md Abschnitt 4 verlangt, dass Row-Heavy-Panels nur sichtbare Zeilen zeichnen. Das Grid iteriert dagegen über `displayFolder->subfolders` und `gridFiles` komplett und baut pro Kachel: `BeginGroup`, drei `PushStyleColor`, `ImageButton`, `AssetThumbnailCache::get(fullPath)` (String-Hash), `pickAssetVisual` (Typ-Cache-Lookup, bei Unbekannt `tolower`-Kopie der Extension), Git-Lookup (`entryForAbsolutePath` -> `toRepoRelative` mit mehreren String-Allokationen), Collab-Schlüssel (`collabKeyFor` -> `std::string`), `isSelected` (linear über die Selektion), Label-Kürzung per `CalcTextSize`-Schleife und `std::filesystem::path(...).stem().string()`. Das passiert für jede Datei im Ordner, auch wenn nur ca. 30 sichtbar sind. Bei einem Ordner mit mehreren tausend Assets (die Suche liefert ebenfalls eine flache Trefferliste) ist das der dominante Per-Frame-Kostenpunkt des Editors, sobald das Panel sichtbar ist. Das Kommentar in `GitController.cpp:440` ("this runs per visible tile") ist bereits falsch: es läuft pro Kachel überhaupt.
- **Code-Auszug**:
```cpp
for (auto* file : gridFiles)
{
    if (col > 0 && col < columns) ImGui::SameLine();
    ...
    const bool isSel = isSelected(file->fullPath);
    ImGui::BeginGroup();
    ImGui::PushID(file->fullPath.c_str());
    ...
    ImTextureID thumb = reinterpret_cast<ImTextureID>(AssetThumbnailCache::get(file->fullPath));
```
- **Lösung**: Kachelhöhe ist konstant (`k_cellSize` + eine Textzeile, Ordner- und Dateikachel gleich). Ordner und Dateien zu einer Item-Liste zusammenführen (Ordner zuerst), Zeilenzahl `rows = ceil(n / columns)`, Zeilenhöhe = Kachelhöhe + `ItemSpacing.y`, dann `ImGuiListClipper clipper; clipper.Begin(rows, rowH); while (clipper.Step()) for (row...) for (c in 0..columns) drawTile(items[row*columns+c]);`. `SameLine` nur innerhalb einer Zeile. `s_revealScrollPath` (Scroll-zu-Kachel) braucht dann `ImGui::SetScrollY(rowIndex * rowH)` statt `SetScrollHereY` in der Kachel, weil die Kachel evtl. nicht gezeichnet wird. Die Kachel-Zeichnung in eine Lambda/Funktion `drawFileTile(...)` auslagern (siehe EDT-304), dann wird Clipping fast mechanisch.
- **Erwarteter Effekt**: (Schätzung) Kosten pro Frame sinken von O(Dateien im Ordner) auf O(sichtbare Kacheln), also bei 2 000 Dateien grob Faktor 50 bei diesem Teil. Die tatsächliche Frame-Zeit vor/nach muss gemessen werden.
- **Risiko / Verifikation**: Mittel. Scroll-zu-Kachel (Reveal), Shift-Bereichsselektion (arbeitet über `gridFiles`, nicht über gezeichnete Kacheln, bleibt korrekt), Drag-Quelle/-Ziel und Tastatur-Enter müssen weiter funktionieren. Es gibt keinen UI-Test für das Panel; vor dem Umbau einen `test_content_browser_ui.cpp` nach Vorbild von `test_outliner_ui.cpp` mit 5 000 synthetischen `HE::File` anlegen (Scroll, Selektion, Reveal).

### EDT-302 – DocsPanel zeichnet alle Sektionen einer Seite jede Frame
- **ID**: EDT-302
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/DocsPanel.cpp:965-1018` (`drawPage`), `309-393` (`drawRuns`/`splitWords`), `992-993`
- **Problem**: `drawPage` ruft für jede Sektion `drawBlock` auf allen Blöcken auf, ohne zu prüfen, ob die Sektion im sichtbaren Bereich liegt. Die Node-Reference ist eine Seite mit ca. 300 Sektionen (Kommentar `1081-1086`), jede mit NodePreview, Pins-Block, Absätzen und Tabellen. Pro Absatz baut `drawRuns` jede Frame `splitWords()` neu (eine `std::string` pro Wort, `substr`-Kopien), ruft pro Wort `CalcTextSize`, `PushStyleColor` und `TextUnformatted`. Zusätzlich pro Sektion und Frame `lib.topicOf(s_page, i)` (zwei String-Konkatenationen) und `panelForTopic(topic)` (linearer Scan über `kPanelTopics`, `EditorHelp.cpp:9717-9736`). Das Fenster selbst wird dabei nur teilweise per ImGui-Clipping weggeschnitten (Draw-Befehle, nicht die CPU-Arbeit).
- **Code-Auszug**:
```cpp
for (int i = 0; i < static_cast<int>(page.sections.size()); ++i)
{
    ...
    const std::string topic = lib.topicOf(s_page, i);
    if (const HE::Ed::Help::PanelTopic* pt = HE::Ed::Help::panelForTopic(topic); pt && s_panelOpener)
    ...
    for (const docs::Block& b : sec.blocks) drawBlock(ctx, b);
```
- **Lösung**: (1) Pro Sektion die Höhe der letzten Frame merken (`std::vector<float> s_secHeight`, zurückgesetzt bei Seitenwechsel). Liegt `[cursorY, cursorY + h]` komplett außerhalb von `[scrollY, scrollY + visibleH]` und ist die Sektion nicht das Scroll-Ziel (`s_scrollTo == i`), dann `ImGui::Dummy(ImVec2(0, h))` statt Zeichnen. Gemessene Höhen werden nach dem ersten Voll-Durchlauf gefüllt; bis dahin zeichnen wie bisher. (2) `splitWords` einmalig beim Laden/Seitenwechsel cachen (z. B. `mutable std::vector<Word>` je `Block`, oder `std::unordered_map<const Block*, std::vector<Word>>`) bzw. `Word::text` als `std::string_view` in `Run::text`. (3) `topicOf`/`panelForTopic` einmal pro Seite berechnen (`std::vector<const PanelTopic*>`).
- **Erwarteter Effekt**: (Schätzung) Aus O(alle Blöcke der Seite) pro Frame wird O(sichtbare Sektionen); auf der Node-Reference-Seite mehr als eine Größenordnung. Auf kleinen Seiten kaum spürbar.
- **Risiko / Verifikation**: Mittel. Der Sprung zu einer Sektion (`s_scrollFrames = 3`) hängt von den echten Höhen ab, deshalb Ziel-Sektion und alles darüber in den ersten Frames nicht überspringen. Bei Breitenänderung des Fensters die Höhen verwerfen. Verifikation mit `tests/test_ui_shot.cpp` (rendert den Reader headless): Screenshot vorher/nachher an gleicher Scrollposition vergleichen.

### EDT-303 – `ContentBrowserPanel::render()` ist eine 4 400-Zeilen-Funktion mit ca. 60 Funktions-Statics
- **ID**: EDT-303
- **Kategorie**: A
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/ContentBrowserPanel.cpp:668-5060`
- **Problem**: Eine einzige Funktion enthält Baum, Breadcrumb, Suche, Grid, Drag-Move, Kontextmenü, ca. 9 modale Dialoge und die Tastatursteuerung. Der Zustand liegt in ca. 60 `static`-Locals mitten im Funktionskörper (z. B. `709`, `726-738`, `774-782`, `985-1000`, `1445-1504`). Alle Dialoge wiederholen dasselbe Skelett: `if (s_openX && !ctx.contentRefreshPending && !ctx.contentRefreshDone) OpenPopup; SetNextWindowSize; pinDialogToEditorWindow; BeginPopupModal(NoTitleBar|NoResize|NoMove) ... else if (!target.empty() && !s_openX) { clear; cancelReferenceScan(); }` (`3659-3663`, `3979-4122`, `4130-4338`, `4344-4442`, `4448-4494`, `4501-4614`, `4621-4711`, `4718-4799`, `4807-4884`). Die "Escape-Falle" (Zustand räumen, wenn das Modal ohne Button schließt) ist fünfmal handkopiert. Zusätzlich hält die Funktion drei `shared_lock`s (`673-675`) über die ganze Länge inklusive Modalen und enthält deshalb den Kommentar zum Selbst-Deadlock (`2927-2934`). Das Datei-Statik-Zustandsmodell macht Tests unmöglich.
- **Code-Auszug**:
```cpp
auto [contentFolder, contentLock] = ctx.globalState->lockContentFolder();
auto [engineFolder, engineLock]   = ctx.globalState->lockEngineFolder();
auto [sourceFolder, sourceLock]   = ctx.globalState->lockSourceFolder();
...
static std::vector<std::string> s_deleteAssetTargets;
static bool                     s_openDeleteAssetPopup = false;
```
- **Lösung**: Schrittweise zerlegen, ohne Verhalten zu ändern: (1) `struct BrowserState` (Selektion, Navigation, Dialog-Zustände) in einer anonymen Namespace-Instanz statt verstreuter Statics. (2) Ein kleiner Helfer `ModalDialog` (`open(flag)`, `begin(id, width)` inkl. `pinDialogToEditorWindow`, `onDismissed(lambda)`) ersetzt die 9 Skelette. (3) Je Dialog eine freie Funktion (`drawDeleteAssetDialog(ctx, state)` usw.), Kontextmenü und Create-Menü (`drawCreateAssetItems`) ebenso. (4) Danach lässt sich die Dialog-Logik außerhalb des Ordner-Locks ausführen (Locks nur um Baum/Grid halten), was die Deadlock-Sonderbehandlung (`2927-2950`) überflüssig macht. Reihenfolge: erst Dialoge, dann Grid (siehe EDT-301), zuletzt Baum.
- **Erwarteter Effekt**: Keine Laufzeitwirkung; deutlich bessere Lesbarkeit (geschätzt 800-1 000 Zeilen weniger Boilerplate) und Voraussetzung für EDT-301/304/306.
- **Risiko / Verifikation**: Mittel (reines Refactoring, aber viele Zustandsabhängigkeiten, z. B. `s_ctxMenuItem` über mehrere Frames). Vorher ein UI-Test (siehe EDT-301), danach `scripts/editor_help_audit.py --check` (Help-Scopes/Labels dürfen nicht wandern).

### EDT-304 – Content-Browser-Suche kopiert Trefferliste und Map jede Frame
- **ID**: EDT-304
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ContentBrowserPanel.cpp:1787-1869`
- **Problem**: Der Cache für aktive Filter ist korrekt gedacht, aber bei einem Treffer wird er per Wertkopie gelesen: `gridFiles = s_cachedHits; foundIn = s_cachedFoundIn;`. `foundIn` ist `unordered_map<const File*, std::string>`; jede Frame werden also alle Hit-Strings (relative Pfade) und alle Map-Knoten neu allokiert. Bei einigen tausend Treffern sind das tausende Heap-Allokationen pro Frame. Zusätzlich wird im Nicht-Filter-Fall `gridFiles` jede Frame neu gefüllt (`1791-1793`), inklusive `cbCollapsedIntoHeader` (siehe EDT-306). Im Miss-Fall wird in `collect` pro Datei ein `name`-Kopie lowercased und anschließend `s_cachedHits = gridFiles; s_cachedFoundIn = foundIn;` noch einmal kopiert.
- **Code-Auszug**:
```cpp
if (s_cachedNeedle == s_searchText && ... && s_cachedSourceVersion == sourceVersion)
{
    gridFiles = s_cachedHits;
    foundIn   = s_cachedFoundIn;
}
```
- **Lösung**: Statt Kopien per Referenz arbeiten: `const std::vector<const HE::File*>* shown = &s_cachedHits; const auto* foundInMap = &s_cachedFoundIn;` bzw. im Nicht-Filter-Fall `displayFolder->files` ohne Zwischenvektor, wenn kein Source-Root aktiv ist (`cbCollapsedIntoHeader` liefert dort immer false). Den Cache-Key als eigenes `struct SearchCache` mit `bool matches(...)` bündeln. `foundIn` als `std::vector<std::string>` parallel zu den Treffern (Index statt Map) oder `std::string_view` auf einen Pfad-Pool ersetzen.
- **Erwarteter Effekt**: (Schätzung) Bei aktiver Suche mit n Treffern entfallen ca. 2n Allokationen pro Frame.
- **Risiko / Verifikation**: Niedrig. Lebensdauer: die Zeiger bleiben nur bis zum nächsten Content-Refresh gültig, deshalb nicht über Frames halten (der Cache-Key prüft bereits alle drei Versionszähler). Test: Suche tippen, Refresh während Suche auslösen.

### EDT-305 – Dateisystem-Syscalls im Draw-Pfad (`isReadOnlyGround`, `engineRelativePath`)
- **ID**: EDT-305
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ContentBrowserPanel.cpp:97-103`, `765-773`, `3049`, `3564-3582`, `4926`, `4213-4214`; `src/HE_Core/src/ContentManager/ContentManager.cpp:977-981`, `1015-1029`
- **Problem**: `engineRelativePath` ruft `fs::relative(fullPath, engineContentCacheDir(), ec)` auf; `std::filesystem::relative` löst beide Pfade über `weakly_canonical` auf (mehrere `stat`-Aufrufe). `isReadOnlyGround` kombiniert das mit `isEngineContentDevMode()` (`getenv` + `atoi`), `isEngineDefaultPath` und `isEngineOverridePath` (je ein weiteres `fs::relative`). Aufgerufen wird das (a) **jede Frame**, sobald das Panel Fokus hat und eine Selektion existiert (`4926`: `const bool onEngineGround = isReadOnlyGround(s_selectedItem);`, nur durch `!IsAnyItemActive()` geschützt), (b) jede Frame bei offenem Kontextmenü für das Menüziel (`3049`) und für jede selektierte Datei samt `is_directory`-Syscall (`3564-3582`, bei 500 selektierten Assets ca. 2 000 Syscalls pro Frame), (c) im Pattern-Rename-Dialog pro Ziel und Frame `exists`+`equivalent` (`4213-4214`).
- **Code-Auszug**:
```cpp
if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
    !ImGui::IsAnyItemActive() && !s_selection.empty())
{
    const bool onEngineGround = isReadOnlyGround(s_selectedItem);
```
- **Lösung**: (1) `isReadOnlyGround` nur bei Bedarf aufrufen: `onEngineGround` lazy innerhalb der `IsKeyPressed(Delete/F2)`-Zweige berechnen (`ImGui::IsKeyPressed(...) && !isReadOnlyGround(...)` ist bereits kurzgeschlossen, wenn die Reihenfolge der Operanden getauscht wird). (2) Das Ergebnis für Menüziel/Selektion beim Öffnen des Menüs einmal berechnen (`s_ctxMenuInfo`), nicht in jeder Frame des offenen Menüs. (3) Mittelfristig in HE_Core `isEngineDefaultPath`/`isEngineOverridePath` und `engineRelativePath` auf rein lexikalische Präfixprüfung (normalisierte Pfade, `lexically_relative`) umstellen; die Pfade sind bereits absolut und normalisiert (vgl. Kommentar `GitController.cpp:440`). `isEngineContentDevMode()` einmal beim Start cachen. (4) Pattern-Rename: Plan nur bei Eingabeänderung neu berechnen.
- **Erwarteter Effekt**: (Schätzung) Entfernt im Normalfall (Selektion + Fokus) ca. 6-10 Syscalls pro Frame; bei offenem Menü mit großer Selektion tausende. Wirkung auf Netzwerklaufwerke/Windows (Defender) deutlich größer als auf lokalem Linux.
- **Risiko / Verifikation**: (1) und (2) niedrig. (3) berührt Verhalten bei Symlinks/Case-Insensitivität (macOS/Windows); `fs::relative` löst Symlinks auf, ein lexikalischer Vergleich nicht. Vor der Umstellung Test mit Projekt hinter Symlink (siehe Kommentar `GitController.cpp:445-455`). Messen mit `strace -c`/Process Monitor.

### EDT-306 – Source-Root: `.h`/`.cpp`-Zusammenfassung ist O(n²) mit Allokationen pro Vergleich
- **ID**: EDT-306
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ContentBrowserPanel.cpp:1766-1782`, `1789-1793`
- **Problem**: `cbCollapsedIntoHeader` läuft für jede Datei jeder Frame; für `.cpp`-Dateien wird pro Vergleichspartner `cbIsHeaderExt` (kopiert und lowercased die Extension) und zweimal `std::filesystem::path(...).stem().string()` ausgewertet. In einem Source-Ordner mit n Dateien sind das O(n²) Pfad-/String-Allokationen pro Frame, nur im C++-Projekt, aber dort dauerhaft.
- **Code-Auszug**:
```cpp
const std::string stem = std::filesystem::path(f->name).stem().string();
for (auto* g : owner->files)
    if (cbIsHeaderExt(g->extension) &&
        std::filesystem::path(g->name).stem().string() == stem)
        return true;
```
- **Lösung**: Einmal pro Ordner und Content-Version ein `std::unordered_set<std::string>` der Header-Stems aufbauen (Stem per `name.substr(0, name.rfind('.'))`, Extension per ASCII-Vergleich ohne Kopie) und `cbCollapsedIntoHeader` auf einen Set-Lookup reduzieren. Das Ergebnis `gridFiles` zusammen mit dem Suchcache (EDT-304) pro (Ordner, Versionen) halten. Die dreifach duplizierten Extension-Helfer vereinheitlichen (EDT-316).
- **Erwarteter Effekt**: (Schätzung) O(n²) -> O(n) und einmalig statt pro Frame.
- **Risiko / Verifikation**: Niedrig. Verhalten ändert sich nicht; Kontrolle am Source-Root mit Paaren `Foo.h/Foo.cpp`, `Lone.cpp`, `Foo.hpp/Foo.cc`.

### EDT-307 – `CppClassEditorPanel::reloadFromDisk` setzt nur ein Flag, das nie ausgewertet wird
- **ID**: EDT-307
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/CppClassEditorPanel.cpp:274-287`, `79-117`, `119-122`
- **Problem**: `reloadFromDisk` setzt `header.loaded = false` / `source.loaded = false` mit dem Kommentar "the next frame re-read it". `stateFor()` kehrt aber früh zurück, sobald `st.resolved` gesetzt ist (`94`), und `loadBuf` (der einzige Ort, der die Datei liest) wird nur im ersten Zweig aufgerufen. `loaded` wird ausschließlich in `bufDirty` gelesen. Folge: Ein Collab-Peer-Update lädt den Editor nie nach (veraltete Anzeige), und nach dem Aufruf liefert `bufDirty` für beide Hälften `false`, so dass lokale ungespeicherte Änderungen weder im Quit-Guard noch bei `save()` erscheinen. Vergleich: `BlendSpacePanel.cpp:313`, `StaticMeshEditorPanel.cpp:356` prüfen `!st.loaded` im Render-Pfad.
- **Code-Auszug**:
```cpp
State& stateFor(const std::string& canonical)
{
    State& st = s_states[canonical];
    if (st.resolved) return st;       // <- loaded wird hier nie geprüft
```
- **Lösung**: In `stateFor` nach dem Resolve-Block `if (!st.header.loaded) loadBuf(st.header); if (!st.source.loaded) loadBuf(st.source);` ausführen (`loadBuf` setzt `loaded = true`), oder `reloadFromDisk` direkt `loadBuf` aufrufen lassen. Einfachste Variante: `resolved`-Zweig auf die Pfadauflösung beschränken und das Laden separat nach `loaded` steuern.
- **Erwarteter Effekt**: Kein Performance-Effekt; behebt einen Korrektheitsfehler (stille Datenverwechslung im Collab-Betrieb).
- **Risiko / Verifikation**: Niedrig. Test: Datei auf Platte ändern, `reloadFromDisk` aufrufen, nächster `render` muss den neuen Text zeigen und `isDirty` false melden; Variante mit vorher ungespeicherten Edits (Entscheidung nötig: verwerfen, wie der Kommentar behauptet).

### EDT-308 – Console: Trim per `vector::erase(begin)` und vollständiger `rebuildVisible` bei jedem Zufluss
- **ID**: EDT-308
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ConsoleView.cpp:268`, `413-415`, `432-455`, `419-426`, `676`
- **Problem**: `s_lines` ist ein `std::vector<Line>` mit Obergrenze 5 000. `Line` enthält sieben `std::string`. Läuft die Konsole voll (typisch bei einem Log-Spam, genau dem Fall, den die Zähler-Faltung abmildern soll), verschiebt jedes Trimmen per `erase(begin, ...)` alle 5 000 Elemente (je 7 String-Moves). Jede Frame mit Zufluss setzt `s_visibleDirty`, und `rebuildVisible` geht dann über alle 5 000 Zeilen; bei aktiver Suche mit `containsNoCase` (`std::search` mit Lambda-Vergleich, byteweise Kleinschreibung) über jeden `text`. Das passiert auch bei geschlossenem Fenster nicht (nur im Draw), aber bei offenem Fenster mit Dauer-Logging in jeder Frame.
- **Code-Auszug**:
```cpp
if (s_lines.size() > k_maxLines)
    s_lines.erase(s_lines.begin(),
                  s_lines.begin() + static_cast<std::ptrdiff_t>(s_lines.size() - k_maxLines));
```
- **Lösung**: `std::deque<Line>` (der Zufluss-Puffer ist bereits eins) oder Ringpuffer für `s_lines`; `s_visible` als Index-Liste mit `id`-Offsets so führen, dass ein Trim vorne nur einen Basis-Offset verschiebt. Für den Zufluss einen inkrementellen Pfad: neue Zeilen gegen Filter/Suche prüfen und an `s_visible` anhängen, statt alles neu zu bauen; Vollrebuild nur bei Änderung von Filter/Suche/Trim. Den Suchtext einmal klein schreiben und `Line::text` in der Sink schon klein vorhalten (oder `strcasestr`).
- **Erwarteter Effekt**: (Schätzung) Bei Log-Flood Wegfall von ca. 35 000 String-Moves und einem 5 000-Zeilen-Durchlauf pro Frame.
- **Risiko / Verifikation**: Niedrig bis Mittel. Chips- und Kategorie-Zähler werden nach Trim "recounted, never decremented" (Kommentar `430-431`); beim inkrementellen Pfad ist ein Trim ein Vollrebuild. Testen mit `test_ui_shot.cpp` und `ConsoleView::resetForTest()` plus synthetischem Flood.

### EDT-309 – Console: `std::deque`-Konstruktion (Heap) bis zu dreimal pro Frame, auch bei leerem Puffer
- **ID**: EDT-309
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ConsoleView.cpp:352-359`; Aufrufer `ConsolePanel.cpp:74`, `ConsoleView.cpp:685`, `1020`
- **Problem**: `drainPending()` legt zuerst `std::deque<Line> taken;` an und tauscht dann unter dem Lock. libstdc++ und MSVC allokieren für eine default-konstruierte `deque` bereits Map und ersten Chunk. Gepumpt wird in `DrawConsoleWindow` vor `Begin`, noch einmal in `draw()` und in `stats()`. Bei offenem Fenster also bis zu zwei, bei geschlossenem eine nutzlose Allokation pro Frame (die Panel-Funktion kehrt bei `!open` früh zurück, daher nur bei offenem Fenster).
- **Code-Auszug**:
```cpp
bool drainPending()
{
    std::deque<Line> taken;
    {
        std::lock_guard<std::mutex> lock(s_pendingMutex);
        taken.swap(s_pending);
    }
    if (taken.empty()) return false;
```
- **Lösung**: Erst unter dem Lock `if (s_pending.empty()) return false;` prüfen, danach erst `taken` anlegen. Zusätzlich das zweite `pump()` in `draw()` streichen (der Aufrufer pumpt bereits vor `Begin`; `draw()` wird in Tests auch ohne Panel genutzt, daher `pump()` dort behalten und im Panel entfernen, oder einen `s_pumpedThisFrame`-Zähler nutzen).
- **Erwarteter Effekt**: Kleine, aber konstante Einsparung (1-2 Allokationen pro Frame).
- **Risiko / Verifikation**: Niedrig.

### EDT-310 – Console: pro sichtbarer Zeile und Frame wird die Copy-Zeile gebaut, Breite per `CalcTextSize`
- **ID**: EDT-310
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ConsoleView.cpp:786`, `825-826`, `847`, `915`
- **Problem**: `row.clear(); appendRow(line, row);` kopiert für jede sichtbare Zeile jede Frame den kompletten Text (plus `std::to_string` bei Wiederholungen), obwohl `row` nur im Kontextmenü-Eintrag "Copy Line" gebraucht wird. Außerdem wird `ImGui::CalcTextSize(line.msg.c_str())` pro sichtbarer Zeile und Frame berechnet, um Badge und Link zu platzieren.
- **Code-Auszug**:
```cpp
const Line& line = s_lines[...];
row.clear();
appendRow(line, row);
```
- **Lösung**: `appendRow` erst im `BeginPopupContextItem`-Zweig (bzw. Lambda `copyLine`) aufrufen. Textbreite pro Zeile cachen (`mutable float msgW` + Font-Größen-Key) oder erst berechnen, wenn `repeat > 1 || leads`.
- **Erwarteter Effekt**: ca. 30-60 Stringkopien und `CalcTextSize`-Aufrufe weniger pro Frame (Schätzung).
- **Risiko / Verifikation**: Niedrig.

### EDT-311 – Console: Log-Sink läuft unter dem Log-Mutex, `Line` speichert Text mehrfach
- **ID**: EDT-311
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ConsoleView.cpp:45-75`, `124-236`, `377-379`
- **Problem**: Der Sink wird mit gehaltenem Log-Mutex aufgerufen (Kommentar `126`) und macht pro Record `localtime_r` + `strftime` + `snprintf` und mindestens sechs String-Allokationen (`time`, `msg`, `text`, `loc`, `hcKey`, `scriptName`; `text` dupliziert `msg`, Prefix und `loc`). Bei Logging-Last auf mehreren Threads verlängert das die Zeit unter dem globalen Log-Lock. In `drainPending` wird pro Zeile (bei gleichem Level/Kategorie wie die letzte) ein `sscanf` für die "(previous message repeated ...)"-Notiz ausgeführt, obwohl ein Präfixvergleich (`msg[0] == '('`) genügt.
- **Code-Auszug**:
```cpp
line.msg.assign(at, static_cast<std::size_t>(end - at));
line.text = prefix;
line.text += line.msg;
```
- **Lösung**: Im Sink nur Rohdaten kopieren (Zeit als `uint64_t unixMillis`, `msg`, Level, Kategorie, optional Datei/Zeile); `time`, `text` und `loc` erst beim Drain im UI-Thread (oder lazy beim ersten Zeichnen) bauen. `Line::text` nur für Suche/Kopieren, aus `msg` + Metadaten ableitbar, daher nicht speichern oder erst bei aktiver Suche materialisieren. Vor `sscanf` auf `msg.rfind("(previous message repeated", 0) == 0` prüfen.
- **Erwarteter Effekt**: (Schätzung) Geringere Verweildauer im Log-Lock und ca. 30-40 % weniger Speicher je Zeile.
- **Risiko / Verifikation**: Niedrig. Die Sink-Regel "kurz, nie loggen" bleibt erhalten.

### EDT-312 – Avatar: 16-KB-FNV-Hash pro Aufruf und Frame statt einmal pro Bildwechsel
- **ID**: EDT-312
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CollabPresenceBar.cpp:54-65`, `81-107`, `359`, `704`, `770`, `801`
- **Problem**: `avatarTexture()` hasht bei **jedem Aufruf** den vollen RGBA-Puffer (`kAvatarSize = 64` -> 64×64×4 = 16 KB, byteweise FNV-1a mit serieller Multiplikationskette), nur um zu entscheiden, ob neu hochgeladen werden muss. Aufgerufen wird es pro Footer-Gesicht (bis 4), pro Viewport-Marker (bis 7) und pro Roster-Zeile, wobei der Footer und die Roster-Liste in derselben Frame zusammen gezeichnet werden können. Ca. 10-20 Hashes × 16 KB pro Frame, solange eine Session läuft. Außerdem wird ein fehlgeschlagener Upload (`entry.handle == nullptr`) in jeder Frame erneut versucht (Zeile 96-106: Hash gleich, aber `entry.handle` null -> `CreateImGuiTexture` jede Frame).
- **Code-Auszug**:
```cpp
const std::uint64_t hash = hashPixels(rgba);
CachedAvatar& entry = s_avatars[key];
if (entry.handle && entry.hash == hash && entry.size == size)
    return static_cast<ImTextureID>(...);
```
- **Lösung**: Den Hash dort berechnen, wo die Pixel ankommen (Roster-Eintrag/`Identity`, einmal pro Empfang) und als `avatar.hash` mitführen; `avatarTexture` vergleicht dann nur den 64-Bit-Wert. Alternativ eine Generationszahl am `Participant::avatar`. Für Fehlschläge `entry.failedHash` merken und nicht erneut versuchen, bis sich der Hash ändert.
- **Erwarteter Effekt**: (Schätzung) ca. 0,2-0,4 ms CPU pro Frame bei vollem Footer + Markern entfällt; kein Effekt ohne Session.
- **Risiko / Verifikation**: Niedrig. `HE_Net::Participant`/`Identity` liegen außerhalb des Slice (Header-Änderung nötig). Die Garantie "Teilnehmer-ID wiederverwendet -> altes Bild" bleibt, solange der Hash am Bild hängt.

### EDT-313 – `Library::appendPage` baut den gesamten Suchindex bei jedem Seitenzugang neu
- **ID**: EDT-313
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/DocsLibrary.cpp:387-431`; Aufrufer `EditorGuides.cpp:610-615`, `EditorReference.cpp:151`, `HcNodeReference.cpp:225`; `DocsPanel.cpp:202-242`
- **Problem**: `appendPage` ruft am Ende `buildIndex()` auf, das alle Seiten und Sektionen (ca. 500 KB Bundle-Text plus Node-Reference plus Guides) neu in Kleinbuchstaben kopiert. Die Installer rufen es in Schleifen pro Seite auf (Guides: eine Seite je Eintrag in `all`; EditorReference: je Bereich). Das ergibt N vollständige Rebuilds, quadratisch in Seitenzahl × Textmenge, alles synchron im UI-Thread beim ersten Öffnen des Readers (`ensureLoaded` aus `open()`/`openTopic()`, also auch beim ersten F1). Zusätzlich dupliziert `Index::pageTitle` den Seitentitel in jede Sektion (`427-428`).
- **Code-Auszug**:
```cpp
for (Docs::Page& p : all)
{
    ...
    lib.appendPage(std::move(p));   // -> buildIndex() über die ganze Bibliothek
}
```
- **Lösung**: `appendPage` indexiert nur die neue/ersetzte Seite (`m_index.resize(m_pages.size()); m_index[slot] = buildPageIndex(page)`). `buildIndex()` bleibt für `loadFromJson`. `pageTitle` einmal pro Seite (`std::vector<std::string> m_pageTitleLower`). Optional `ensureLoaded` auf einen Worker auslagern oder hinter einer "Lade..."-Anzeige halten.
- **Erwarteter Effekt**: (Schätzung) Erster Öffnen-Stall um den Faktor ca. N (Seitenzahl) kürzer; einmalig, nicht pro Frame.
- **Risiko / Verifikation**: Niedrig. `tests/test_docs_library.cpp` deckt Replace-by-id ab; einen Test ergänzen, dass `search` nach `appendPage` zweier Seiten identische Treffer wie vorher liefert.

### EDT-314 – DocsPanel: Volltextsuche läuft jede Frame, Snippets für alle Treffer, versteckte Seiten werden mitdurchsucht
- **ID**: EDT-314
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/DocsPanel.cpp:1024-1034`, `1481`; `src/HE_Editor/DocsLibrary.cpp:497-576`, `559-564`
- **Problem**: `drawResults(s_query)` ruft `lib.search(query)` in **jeder Frame** auf, solange das Suchfeld nicht leer ist, nicht nur bei Eingabe. `search` bewertet jede Sektion aller Seiten mit bis zu fünf `std::string::find`-Durchläufen über deren Volltext (insgesamt ca. 1 MB) pro Token und ruft `snippetAround` (Allokationen: Teilstring, `tidy`) für **jeden** Treffer auf, bevor auf `maxHits = 40` gekürzt wird. Die Panel-Seite filtert anschließend die versteckten Bundle-Kapitel (`ownPage`) wieder heraus (`1031-1032`), sie wurden also vorher mitbewertet und mit Snippets versehen.
- **Code-Auszug**:
```cpp
std::vector<docs::Hit> hits = lib.search(query);
hits.erase(std::remove_if(hits.begin(), hits.end(),
    [](const docs::Hit& h){ return !ownPage(h.page); }), hits.end());
```
- **Lösung**: (1) Ergebnis in der Panel-Schicht cachen: `static std::string s_lastQuery; static std::vector<Hit> s_hits;` und nur bei geänderter Eingabe oder geänderter Library-Version neu suchen. (2) In `Library::search` das Snippet erst nach `sort`/`resize` für die verbleibenden Treffer berechnen. (3) Seitenfilter als Parameter in `search` (`std::function<bool(int page)>` oder `int firstPage`), damit versteckte Seiten gar nicht bewertet werden; das spart auch den späteren `erase`.
- **Erwarteter Effekt**: (Schätzung) Von O(Textmenge) pro Frame auf O(Textmenge) pro Tastendruck; bei 1 MB Text grob 0,5-2 ms pro Frame gespart (messen).
- **Risiko / Verifikation**: Niedrig. `test_docs_library.cpp` prüft Ranking; das Verhalten bleibt gleich, solange Snippet-Berechnung nach dem Sortieren erfolgt (Reihenfolge hängt nur von `score`, `page`, `section` ab).

### EDT-315 – DocsPanel: Navigation wird jede Frame neu zusammengebaut
- **ID**: EDT-315
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/DocsPanel.cpp:1171-1228`, `564`, `1198-1200`
- **Problem**: `navGroups()` erzeugt jede Frame `std::vector<NavGroup>` mit kopierten Titel-Strings und Seitenindizes, inklusive `Guides::pageIds()`-Durchlauf, `lib.pageIndex(id)` (linear über alle Seiten, String-Vergleich) pro Guide und `std::find` pro Seite. Dazu in `drawLangTabs` pro Variante und Frame `"Documentation/" + v.label` und `snprintf`.
- **Code-Auszug**:
```cpp
for (const NavGroup& g : navGroups())
```
- **Lösung**: `navGroups()` einmal nach `ensureLoaded()` berechnen (`static std::vector<NavGroup> s_nav; static size_t s_navPages`) und bei geänderter `lib.pages().size()` neu bauen. Help-Key pro Variante im `Block::Variant` vorhalten.
- **Erwarteter Effekt**: Gering (einige Dutzend Allokationen pro Frame).
- **Risiko / Verifikation**: Niedrig.

### EDT-316 – Duplizierte Extension-/Zustandshelfer in ContentBrowser und CppClassEditor
- **ID**: EDT-316
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/ContentBrowserPanel.cpp:1099-1100`, `1397`, `1766-1768`, `1833`, `1845`; `src/HE_Editor/CppClassEditorPanel.cpp:17-32`, `34-45`; Kachelstil `ContentBrowserPanel.cpp:1654-1667` vs. `1910-1923`
- **Problem**: Dieselbe "lowercase + vergleiche mit .h/.hpp/..."-Logik existiert dreimal (`lowerExt`/`isHeaderExt`/`isSourceExt` im CppClassEditor, `cbLowerExt`/`cbIsHeaderExt`/`cbIsSourceExt` im ContentBrowser, ein weiterer Inline-Lambda in der Reveal-Logik); jede Variante kopiert die Extension. `tolower`-Schleifen auf Strings stehen fünfmal im Content Browser. Der selektierte/nicht selektierte Button-Stil und die Label-Kürzung (`1741-1755` vs. `2295-2310`) sind für Ordner- und Dateikachel kopiert. `readTextFile` liest per `istreambuf_iterator` Zeichen für Zeichen.
- **Code-Auszug**:
```cpp
auto cbLowerExt = [](const std::string& e){ std::string s=e; for(auto&c:s) c=(char)::tolower((unsigned char)c); return s; };
auto cbIsHeaderExt = [&](const std::string& e){ std::string s=cbLowerExt(e); return s==".h"||s==".hpp"||s==".hh"||s==".hxx"; };
```
- **Lösung**: Ein gemeinsamer Header (`EditorPathUtil.h`) mit `bool isCppHeaderExt(std::string_view)` / `isCppSourceExt(std::string_view)` (ASCII-case-insensitive, ohne Allokation), `asciiLower(std::string)`; `drawTileFrame(isSel, ...)` und `truncatedLabel(...)` als gemeinsame Funktionen für Ordner- und Dateikachel; `readTextFile` per `seekg/tellg` + `read`. Bei `AssetImporter`/`Importer::isImportableSource` auf vorhandene Helfer prüfen.
- **Erwarteter Effekt**: Weniger Code (geschätzt 80-120 Zeilen), keine Laufzeitwirkung außer EDT-306.
- **Risiko / Verifikation**: Niedrig.

### EDT-317 – Content Browser: Breadcrumb-DFS und Baum-Rekursion per `std::function` jede Frame
- **ID**: EDT-317
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ContentBrowserPanel.cpp:813-884`, `1163-1179`, `1016-1027`
- **Problem**: Der Breadcrumb ermittelt die Elternkette jede Frame per rekursiver DFS (`std::function`) über den ganzen Baum ab Root, obwohl `s_gridFolder` meist unverändert ist. `renderTree` ist ebenfalls eine rekursive `std::function`, die pro Knoten `ImGui::PushID(fullPath.c_str())` (String-Hash), `std::find` in `s_revealExpand` und `folderHasChanges` (mit `toRepoRelative` = zwei bis drei String-Allokationen) ausführt; der Baum ist nicht geclippt (geöffnete Knoten zählen vollständig).
- **Code-Auszug**:
```cpp
std::function<bool(const HE::Folder*)> findPath = [&](const HE::Folder* cur) -> bool {...};
findPath(&cbRootFolder(s_selectedRootKind));
```
- **Lösung**: Breadcrumb-Kette nur bei Navigation oder Versionswechsel neu berechnen und als `std::vector<const HE::Folder*> s_crumbs` halten (oder aus `s_gridFolderPath` per Pfadsegmente und `Folder::parent`, falls vorhanden). Rekursion in eine freie Funktion/Lambda mit `auto&` (Y-Kombinator) umbauen; `folderHasChanges` für den Baum pro Knoten nur bei geöffnetem/sichtbarem Knoten (`ImGui::IsRectVisible`) auswerten.
- **Erwarteter Effekt**: Gering bis mittel bei sehr großen Projektbäumen (Schätzung).
- **Risiko / Verifikation**: Niedrig; Folder-Zeiger werden bei Refresh ungültig, deshalb Cache an die drei Versionszähler binden (wie bei `s_gridFolder`, `997-1030`).

### EDT-318 – Content Browser: temporärer Vektor in Range-for, Label-Kürzung O(n²)
- **ID**: EDT-318
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/ContentBrowserPanel.cpp:2350-2351`, `1741-1749`, `2295-2304`
- **Problem**: `for (const std::string& moveSrc : s_pendingMoveDst.empty() ? std::vector<std::string>{} : s_pendingMoveSrcs)` ist ein Ternary aus Prvalue und Lvalue und kopiert deshalb bei nicht leerem Ziel den gesamten Vektor (Lebensdauer ist zwar korrekt, aber unnötig und schwer zu lesen); der Zweig läuft jede Frame. Die Label-Kürzung baut in jeder Schleifeniteration `label + "..."` neu (eine Allokation je entferntem Zeichen) und ruft `CalcTextSize` dafür auf: bei langen Namen O(n) Allokationen pro Kachel und Frame.
- **Code-Auszug**:
```cpp
while (!label.empty() &&
       ImGui::CalcTextSize((label + "...").c_str()).x > labelW)
    label.pop_back();
```
- **Lösung**: `if (!s_pendingMoveDst.empty()) for (const std::string& moveSrc : s_pendingMoveSrcs)`. Label-Kürzung: Binärsuche über die Länge oder `ImFont::CalcWordWrapPositionA` / `CalcTextSizeA` mit einmalig berechneter Breite von "..." und `const char* text_end`-Variante ohne String-Kopie; Ergebnis pro (Dateiname, Zellbreite) im Tile-Cache halten, dann entfällt es bei Clipping (EDT-301) ohnehin für unsichtbare Kacheln.
- **Erwarteter Effekt**: Klein; zusammen mit EDT-301 vernachlässigbar.
- **Risiko / Verifikation**: Niedrig.

### EDT-319 – `CollabUndo`: doppelter Push-Code, `erase(begin)`, Kopie des Eintrags bei Undo/Redo
- **ID**: EDT-319
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/CollabUndo.cpp:15-49`, `95-101`, `190`, `204`
- **Problem**: `recordTransform` und `recordAsset` wiederholen die drei Zeilen von `push()` (push_back, `m_redo.clear()`, Kappen) statt `push()` zu benutzen. `undo()`/`redo()` kopieren den Eintrag (`Entry e = m_undo.back();`), inklusive der Byte-Vektoren (`beforeBytes`, `afterBytes`, `subtree`; bei Asset-Einträgen zwei komplette Dateikopien), obwohl er danach entweder zurückgeschoben (Erfolg) oder verworfen wird. `m_undo.erase(m_undo.begin())` bei 64 Einträgen verschiebt je einmal pro Aktion bis zu 63 große `Entry`-Objekte (nicht relevant, aber `std::deque` wäre eindeutiger).
- **Code-Auszug**:
```cpp
Entry e = m_undo.back();
m_undo.pop_back();
if (!applyEntry(e, /*useBefore=*/true)) return false;
m_redo.push_back(std::move(e));
```
- **Lösung**: `Entry e = std::move(m_undo.back()); m_undo.pop_back();`. Bei Fehlschlag verhält sich der Code heute so, dass der Eintrag verloren ist (der Return erfolgt ohne `push_back`); das bleibt so, ist also gleichwertig. `recordTransform`/`recordAsset` rufen `push(std::move(e))`. `m_undo` zu `std::deque<Entry>` für `pop_front`.
- **Erwarteter Effekt**: Vermeidet das Duplizieren großer Asset-Puffer bei jedem Undo (selten); hauptsächlich Lesbarkeit.
- **Risiko / Verifikation**: Niedrig. `tests/test_collab_undo.cpp` deckt Undo/Redo und Kappung ab.

### EDT-320 – `DeferredHandleRetireList` ist eine Überabstraktion über `DeferredSlotFreeList`
- **ID**: EDT-320
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/DeferredHandleRetireList.h:30-89`, `src/HE_Editor/DeferredSlotFreeList.h`
- **Problem**: Um geparkte Handles hinter einem Serial freizugeben, wird eine Slot-Tabelle mit Indirektion genutzt: `retire` allokiert einen Slot, parkt ihn, und `reclaim` räumt den gesamten Freiliste-Inhalt aus (`while (m_slots.alloc(slot))`), prüft jeden Slot auf `Handle{}`, destroyed und gibt alle Slots wieder zurück (`m_scratch`). Das ist O(freie + geparkte Slots) pro `reclaim`, das bei jedem Frame mit Pending aufgerufen wird, und braucht ein Scratch-Array. Die Aufgabe ist funktional ein `std::vector<{Handle, serial}>` mit einmaligem Durchlauf und "kept"-Kompaktierung, genau wie `DeferredSlotFreeList::reclaim` es bereits macht.
- **Code-Auszug**:
```cpp
m_scratch.clear();
int slot = -1;
while (m_slots.alloc(slot)) { ... m_scratch.push_back(slot); }
for (int s : m_scratch) m_slots.release(s);
```
- **Lösung**: `struct Parked { Handle h; std::uint64_t serial; }; std::vector<Parked> m_parked;` mit `retire` = `push_back`, `reclaim` = Durchlauf, Destroy bei `serial <= completed`, Kompaktierung. Die Tests `test_deferred_handle_retire_list.cpp` bleiben gültig (gleiche öffentliche API).
- **Erwarteter Effekt**: Weniger Code (ca. 40 Zeilen), `reclaim` O(geparkte); die Zahl geparkter Handles ist klein (Kommentar: höchstens einer je Frame in Flight), also kein messbarer Performance-Gewinn.
- **Risiko / Verifikation**: Niedrig; vorhandene Tests (`test_deferred_handle_retire_list.cpp`) laufen lassen.

## Gelesen

Alle 19 Dateien des Slice 3 vollständig gelesen (ja), `ContentBrowserPanel.cpp` in fünf Blöcken (1-1300, 1301-2300, 2301-3250, 3251-4150, 4151-5062); nur ergänzend gezielt nachgeschlagen: `ContentManager.cpp:975-1030`, `GlobalState.cpp:132-141`, `GitController.cpp:430-490`, `EditorHelp.cpp:9717-9736`, `EditorReference.cpp`/`EditorGuides.cpp` (Aufrufstellen von `appendPage`).

## Editor Slice 4 – src/HE_Editor/EditorApplication.cpp

## Zusammenfassung

`EditorApplication.cpp` (14 759 Zeilen) besteht zu rund einem Drittel (Zeilen 5321–10075) aus `dumpFrameHeadless()`, einer 4 750 Zeilen langen Sammlung von HE_DUMP_*-Testszenen. Zu einem weiteren Siebtel besteht sie aus `OnRender()` (3015–5143, 2 129 Zeilen) und `setupMcpTools()` (10414–11223). Die Frame-Schleife ist überwiegend sauber gegated (Dirty-Checks, statische getenv-Caches, Fenstertitel nur bei Änderung). Die echten Hot-Path-Kosten stecken in den Collab-/Team-Pfaden: Sie laufen jeden Frame und sind O(Entities) bzw. machen Syscalls und String-Kopien. Dazu kommen die Pro-Aufruf-Kosten von `makeContext()` und `callApi`. Gefunden wurden 21 Punkte: 2 Hoch, 11 Mittel, 8 Niedrig. Davon ist EDT-401 ein echter Build-Fehler für Builds ohne libssh2 (nicht durch Kompilieren verifiziert), alles andere ist Struktur oder Performance. Es wurde nichts gemessen. Alle Aufwandsangaben sind Schätzungen.

---

## Funde (nach Schweregrad)

### EDT-401 – `onProjectWriteRefused` ist nur unter `HE_HAVE_LIBSSH2` definiert, aber unbedingt benutzt
- **ID**: EDT-401
- **Kategorie**: F (Build-Fehler / #ifdef-Struktur)
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung (Korrektheit)
- **Ort**: EditorApplication.cpp:69, 536, 2412–2435, 2670
- **Problem**: Die Funktion wird in Zeile 69 unbedingt deklariert und in `OnInit` (Z. 536) unbedingt benutzt. Ihre Definition liegt aber im `#ifdef HE_HAVE_LIBSSH2`-Block, der in Zeile 2412 beginnt und in Zeile 2670 endet. Das ist der Block der EngineContent-SFTP-Funktionen. Ohne libssh2 gibt es eine `static` Funktion, die benutzt, aber nie definiert wird. Das ergibt eine Warnung und danach einen Linker-Fehler. CLAUDE.md nennt einen Build ohne SFTP-Credentials/libssh2 "normal und supported". Die Funktion hat keinen SFTP-Bezug. Der Fehler stammt aus Commit 5319767 ("Lock the project's files while an export runs"), der die Definition zwischen die SFTP-Funktionen gesetzt hat. In dieser Umgebung wurde nicht kompiliert. Verifiziert ist nur per Grep, dass es keine zweite Definition gibt.
- **Code-Auszug**:
```cpp
static void onProjectWriteRefused(const char* what);   // 69, unbedingt
HE::ProjectWriteGate::install(&BuildProgressDialog::exportRunning, &onProjectWriteRefused);  // 536, unbedingt
#ifdef HE_HAVE_LIBSSH2                                   // 2412
static void onProjectWriteRefused(const char* what)     // 2418, Definition nur hier
```
- **Lösung**: `#ifdef HE_HAVE_LIBSSH2` von Zeile 2412 hinter das Ende von `onProjectWriteRefused` (nach Zeile 2435) verschieben. `onContentSyncFailure` und alles danach bleiben im Block. Danach `#include <atomic>`/`<mutex>` prüfen, falls der Header sie nur transitiv liefert.
- **Erwarteter Effekt**: Build ohne libssh2 linkt wieder. Reine Korrektur.
- **Risiko / Verifikation**: Sehr gering. Verifikation: CMake ohne libssh2 konfigurieren (`HE_HAVE_LIBSSH2` nicht gesetzt) und `HorizonEditor` linken.

### EDT-402 – `syncStructuralChanges()` baut jeden Frame das komplette Entity-Set neu auf
- **ID**: EDT-402
- **Kategorie**: D
- **Schweregrad**: Hoch (in einer Session; sonst unkritisch)
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:4976 (Aufruf), 11400–11618 (Funktion), konkret 11416–11419, 11529–11532, 11594–11598
- **Problem**: In einer Collab-Session läuft jeden Frame `syncStructuralChanges()`. Es legt ein neues `std::unordered_set<Entity> current` über alle Entities der Welt an, wobei jede Einfügung eine Heap-Allokation ist. Danach laufen `newSubtreeRoots`, ein Durchlauf über `m_structureKnown` (mit `m_structureParent.count` pro Entity), `recordMissingParents`, `reparentedSince` und eine Löschprüfung `current.count(e)` für jedes bekannte Entity. Pro Frame sind das einige Hash-Operationen pro Entity und eine große Allokation. Bei 50 000–200 000 Entities (Streaming-Szenen, Foliage-Instanzen als Entities, Terrain-Chunks sind ausgenommen) sind das vermutlich mehrere Millisekunden pro Frame. Es skaliert linear mit der Szene, obwohl sich im Normalfall nichts geändert hat. Der Kommentar "complete by construction" begründet die Diff-Methode, aber nicht, warum sie jeden Frame voll laufen muss.
- **Code-Auszug**:
```cpp
std::unordered_set<Entity> current;
reg.view<entt::entity>().each([&](auto e) {
	if (!isLocalOnly(e)) current.insert(e);
});
...
for (const Entity e : m_structureKnown)
	if (!current.count(e)) gone.push_back(e);
```
- **Lösung**: Den Diff nur ausführen, wenn sich die Struktur geändert haben kann.
  1. Dirty-Flags über `reg.on_construct<EntityIdComponent>()` und `on_destroy<EntityIdComponent>()` setzen. Alternativ ein Zähler in `HorizonWorld::createEntity/destroyEntity`. Dazu die vorhandene Hierarchie-Dirty-Information (`markHierarchyDirty`, Hierarchie-Revision) für Umhängungen nutzen.
  2. `if (!structDirty && now - lastFullDiff < 1000 ms) return;` Der volle Diff läuft als Sicherheitsnetz weiterhin einmal pro Sekunde. Er behält damit die Garantie "vollständig, auch bei direkter `registry().create`".
  3. `current` als Member halten (`clear()` behält die Buckets) oder die Prüfung ganz ohne Set über `reg.valid(e)` für die `gone`-Schleife lösen. `valid` genügt, weil `isLocalOnly` bei toten Entities irrelevant ist.
- **Erwarteter Effekt**: Im Ruhezustand von O(N) pro Frame auf O(1). Schätzung: einige hundert µs bis mehrere ms pro Frame gespart bei großen Szenen. Messen mit `HE_PROFILE_SCOPE_N`.
- **Risiko / Verifikation**: Mittel. Ein Pfad, der Entities ohne die Flags erzeugt, würde um bis zu 1 s verzögert gepublisht. Die Retry-Logik (kSendRetryFrames = 60) hängt an Frames, nicht an Sekunden, und müsste entsprechend angepasst werden. Es gibt keinen Test für das Verhalten (die Regeln liegen in StructuralSync.h und sind getestet, die Frame-Schleife nicht). Ein Test, der N Frames ohne Änderung laufen lässt und Create/Destroy/Reparent einzeln auslöst, wäre nötig. Dafür müsste die Schleife in eine testbare Klasse wandern (z. B. `StructuralSyncPass` neben StructuralSync.h).

### EDT-403 – Pro Frame: `projectRoot()` (Dateisystem-Stat) und mehrere String-Kopien für Team-Sync, auch ohne Team-Server
- **ID**: EDT-403
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:4906–4927, 10077–10084
- **Problem**: In `OnRender` wird jeden Frame `TeamSyncController::Project project` gebaut. Dabei werden `projectRoot()` aufgerufen, das ein `std::filesystem::is_regular_file(p)` macht (Syscall) und einen neuen String liefert, sowie `sc.teamServer()`, `sc.team` und `sc.project` kopiert. Das passiert unabhängig davon, ob ein Team-Server konfiguriert ist. Davor kopiert `m_git.setTeamServer(sc.teamServer(), sc.team, sc.project)` dieselben Strings noch einmal. `projectRoot()` wird außerdem in `collabSyncKey` pro Tab und Frame benutzt (siehe EDT-405). Der Wert ändert sich nur beim Projektwechsel.
- **Code-Auszug**:
```cpp
TeamSyncController::Project project;
project.root       = projectRoot();      // is_regular_file() pro Frame
project.teamServer = sc.teamServer();
project.team       = sc.team;
project.project    = sc.project;
for (const std::string& rel : m_teamSync.update(nowMs, project, m_isPlaying))
```
- **Lösung**: Den Projektroot in `setOnProjectLoaded` (und nach `saveProject`) in ein Member `m_projectRootCached` schreiben. `projectRoot()` gibt dann eine `const std::string&` zurück. Wenn `!sc.teamServer()` und der Controller idle ist, den `Project`-Aufbau überspringen oder `Project` als Member halten und nur bei geänderter `ProjectData`-Revision neu füllen. `setTeamServer` nur bei Änderung aufrufen (Vergleich vor der Kopie, oder die Parameter als `string_view`).
- **Erwarteter Effekt**: Ein Syscall und etwa 4 kleine Allokationen/Kopien pro Frame weniger. Schätzung: wenige µs, bei langsamen Netzlaufwerken als Projektordner deutlich mehr (Stat auf einer Netzfreigabe kann ms dauern).
- **Risiko / Verifikation**: Gering. Der Cache muss bei Projektwechsel/-neuanlage invalidiert werden (`gameInstancePath()` hat dasselbe Muster, ebenfalls stat pro Aufruf). Wenn das `.heproj`-Verzeichnis zur Laufzeit ersetzt wird (Datei wird zu Verzeichnis), wäre der Cache stale. Das ist praktisch nie der Fall.

### EDT-404 – Ausgewählte Entity wird jeden Frame der Session komplett nach CBOR serialisiert
- **ID**: EDT-404
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:5004–5072 (konkret 5061–5070)
- **Problem**: Solange die Session aktiv ist und etwas ausgewählt ist, ruft `OnRender` jeden Frame `TerrainSync::serializeWithoutGrids(*m_editorWorld, selected)` auf. Das legt `GridsAside` an (Raster beiseite schieben und zurück), serialisiert alle Komponenten der Entity per `serializeEntityComponents` in einen `std::vector<uint8_t>` (Heap) und übergibt ihn an `publishComponents`, das den Blob hasht und verwirft, wenn er unverändert ist. Der Kommentar ("Serializing one entity is cheap") ist für ein Light plausibel. Für ein Terrain/Foliage/Skeletal-Entity mit großen Komponenten (Listen, Pfade, Overrides) ist es das nicht. Es läuft auch, wenn niemand etwas ändert.
- **Code-Auszug**:
```cpp
const std::vector<std::uint8_t> comps =
	HE::Ed::TerrainSync::serializeWithoutGrids(*m_editorWorld, selected);
if (!comps.empty())
	m_collab.publishComponents(static_cast<std::uint32_t>(entt::to_integral(selected)), comps);
```
- **Lösung**: Nur serialisieren, wenn sich etwas geändert haben kann: `m_undo.revision()` hat sich bewegt oder die Auswahl hat gewechselt, oder ein Inspector-Widget hat die Entity als editiert markiert (`noteEntityEdited` existiert bereits für Prefabs). Dazu als Rückfall eine Drossel auf z. B. 10 Hz (`nowMs - lastComponentsMs >= 100`). Der Hash in `publishComponents` bleibt als zweite Sicherung.
- **Erwarteter Effekt**: Statt 60× pro Sekunde nur bei Änderungen bzw. 10× pro Sekunde. Schätzung: je nach Entity 20–500 µs pro Frame eingespart. Messen.
- **Risiko / Verifikation**: Mittel. Änderungen, die keine Undo-Revision erzeugen (Gizmo-Drag, Slider vor dem Loslassen), würden von der Revision nicht erfasst. Darum zusätzlich die Drossel statt nur der Revision. Das Live-Gefühl für Peers bleibt bei 10 Hz erhalten. Tests: vorhandene Collab-Tests (`tests/`) auf Live-Komponenten-Sync prüfen. Neuer Test: ein Komponentenwert wird geändert, ohne dass Revision/Flag angefasst wird, und kommt innerhalb des Throttles beim Peer an.

### EDT-405 – `updateAssetCollabSync` macht pro Frame und Tab Stat, Stringbau, Hash-Sets und Kopien
- **ID**: EDT-405
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance + Vereinfachung
- **Ort**: EditorApplication.cpp:10195–10315, 10086–10134 (`collabSyncKey`)
- **Problem**: Die Funktion läuft jeden Frame in der Session. Sie
  1. baut mit `makeContext()` einen kompletten `AppContext` (siehe EDT-406), obwohl `ctx` nur beim Autosave eines Tabs in Zeile 10261 gebraucht wird,
  2. hält `const std::string& root` (Z. 10218), das nie benutzt wird (`(void)root;` in Z. 10221),
  3. legt pro Frame ein `std::unordered_set<std::string> openRel` an und ruft für jeden offenen Tab `collabSyncKey()` auf. Das macht für C++-Klassen und Ordner `projectRoot()` (Stat, siehe EDT-403), `projectRelativeAssetPath` (String-Bau), `isSyncableAsset` und `EditorAssetTypeCache::assetTypeOf(tabPath)`. Der Schlüssel eines Tabs ändert sich nur, wenn sich der Tab-Pfad ändert,
  4. kopiert `m_collab.heldAssetLocks()` in einen Vektor,
  5. fügt am Ende alle `openRel` in `m_docMirrorPaths` ein (String-Kopien pro Tab und Frame).
- **Code-Auszug**:
```cpp
const std::string& root = contentManager().contentRoot();
AppContext ctx = makeContext();
(void)root;
std::unordered_set<std::string> openRel;
for (const auto& tab : m_tabs)
{
	const std::string key = collabSyncKey(tab.assetPath);
```
- **Lösung**: (a) `ctx` erst vor dem `EditorUI::saveAsset` bauen und `root` löschen. (b) Die Tab→Key-Abbildung in `AppContext::EditorTab` oder einer `unordered_map<string,string>` cachen und bei Änderung des Tab-Pfads (oder Projektwechsel) invalidieren. (c) `openRel` und `m_docMirrorPaths` als Member halten und nur verändern, wenn sich die Tab-Liste ändert (die Signatur aus `saveOpenTabs` kann dafür benutzt werden). (d) Rate begrenzen: Delta-Publikation ist nötig, aber Lock-Verwaltung und Aufräumen (`heldAssetLocks`, `forgetAssetEditSession`) nur bei Tab-Änderung oder alle 250 ms.
- **Erwarteter Effekt**: Bei 10–30 offenen Tabs spart das pro Frame zehn bis hundert kleine Allokationen und ein paar Stats. Schätzung: 10–100 µs pro Frame, nur in Sessions.
- **Risiko / Verifikation**: Mittel. Die Lock-Freigabe ("Tab geschlossen, Lock zurück") darf nicht verzögert werden, sonst blockiert man Peers. Mit Tab-Revision-Trigger plus Zeitgrenze von 250 ms ist das ausreichend. `tests/test_collab*.cpp` prüfen. Die Funktion ist dort vermutlich nicht abgedeckt, weil `EditorApplication` nicht in `he_tests` liegt.

### EDT-406 – `makeContext()` baut bei jedem Aufruf ~60 `std::function`s und ruft `prune`
- **ID**: EDT-406
- **Kategorie**: B / C
- **Schweregrad**: Mittel
- **Typ**: Performance + Vereinfachung
- **Ort**: EditorApplication.cpp:12005–12429 (Aufrufer u. a. 5095, 10219, 4843ff; in MCP-Hooks 10719, 10803, 10808 …)
- **Problem**: `makeContext()` wird mindestens einmal pro Frame aufgerufen (Z. 5095 `EditorUI::render`), in Sessions ein zweites Mal (EDT-405) und bei jedem MCP-Widget-/Task-Aufruf. Jeder Aufruf macht `m_selection.prune(registry)`, setzt vier Bridge-Pointer neu und baut ein Aggregat mit rund 60 `std::function`-Membern (jeweils `[this]`-Lambda, passt ins SBO, also vermutlich keine Heap-Allokation, aber 60 Konstruktionen, Typ-Erasure-Manager und beim Verlassen 60 Destruktoren). Dazu kommen String-/Vektor-Kopien nicht, weil die Datenmember Referenzen sind. Die Lambdas sind über die Lebenszeit der Anwendung konstant, nur die Wertfelder (`isPlaying`, `sceneDirty`, `projectLoaded`, Probe-Pointer, `undoSys`) ändern sich.
- **Code-Auszug**:
```cpp
return AppContext{
	.imguiReady          = m_imguiReady,
	.quit                = [this]{ m_quitConfirmed = true; Quit(); },
	.toggleProfilerCapture = [this]{ toggleProfilerCapture(); },
	... // ~60 weitere std::function-Felder
```
- **Lösung**: `AppContext m_ctx` als Member einmalig in `OnInit` mit allen Callbacks füllen. Pro Frame nur die Wertfelder aktualisieren (`refreshContext()`), und `makeContext()` gibt `m_ctx` zurück (Referenz). Alternativ die Callbacks in ein `AppContext::Callbacks`-Teilstruct auslagern, das einmal gebaut und per Zeiger referenziert wird. Der Teilbau wäre eine reine Umstrukturierung ohne Verhaltensänderung.
- **Erwarteter Effekt**: Pro Aufruf etwa 60 Funktions-Konstruktionen und -Destruktionen weniger. Schätzung: 1–3 µs pro Aufruf, ein paar µs pro Frame. Wichtiger ist die Lesbarkeit, weil die 420 Zeilen aus einer Funktion verschwinden, die nur Verdrahtung ist. Als reine Performance-Maßnahme ohne Messung nicht priorisieren.
- **Risiko / Verifikation**: Mittel. `AppContext` enthält Referenzen auf Member (`editorConfig`, `selection`, `tabs`), die bei einer langlebigen Instanz gültig bleiben müssten. Einige Panels könnten den Context kopieren und dabei annehmen, dass er frisch ist (z. B. `isPlaying` als Wert). Prüfen, ob `isPlaying`, `sceneDirty` usw. Werte oder Referenzen sind. Die UI-Tests (`tests/test_*_ui.cpp`) bauen eigene Contexts und decken den Pfad nicht ab. Ein Smoke-Lauf mit `HE_HIDDEN_WINDOW=1` ist nötig.

### EDT-407 – `callApi` (EngineCall-Knoten) setzt pro Aufruf Sandbox-Pfad, Berechtigungen und kopiert vier `std::function`s
- **ID**: EDT-407
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:334–353 (`apiCtx`), 1978–2022 (`svc.callApi`)
- **Problem**: Jeder EngineCall-Knoten in einem laufenden Graphen (PIE oder App-Vorschau, pro Frame potenziell hunderte) macht Folgendes:
  - `find(id)` per String-Lookup,
  - `std::filesystem::path(projPath).parent_path() / "Saved"` plus `.string()` (drei Allokationen) für `setSandboxRoot`,
  - `perm::Grants` neu füllen und `perm::set(g)`,
  - `apiCtx(...)`: kopiert `createObject`, `destroyObject`, `requestQuit`, `windowSize`, `requestRedraw` (fünf `std::function`-Kopien) in ein frisches `Ctx`.
  Sandbox-Wurzel und Grants ändern sich nur bei Projektwechsel oder wenn der Nutzer die Projekteinstellungen ändert. Der Kommentar ("setSandboxRoot is a cheap string assign") stimmt für die Zuweisung, aber nicht für die vorher gebaute Pfadkette.
- **Code-Auszug**:
```cpp
const std::string& projPath = m_projectManager.currentProject().path;
if (!projPath.empty())
	HE::api::fs::setSandboxRoot(
		(std::filesystem::path(projPath).parent_path() / "Saved").string());
...
HE::api::perm::set(g);
HE::api::Ctx c = apiCtx(m_editorWorld.get(), m_physicsWorld.get(), &contentManager(), self);
```
- **Lösung**: Sandbox-Root und Grants in `setOnProjectLoaded` und beim Speichern der Projekteinstellungen setzen, sowie einmal beim Play-Start (dort wird `setSandboxRoot` ohnehin schon gesetzt, Z. 12686). `callApi` ruft dann nur noch `apiCtx`. Für `Ctx` die Host-Funktionen per `const HostCtxParts*` oder `std::shared_ptr`/Referenz teilen, statt sie zu kopieren (falls `HE::api::Ctx` das zulässt, sonst Cross-Modul-Hinweis).
- **Erwarteter Effekt**: 3–4 Allokationen und 5 Funktionskopien weniger pro EngineCall. Schätzung: etwa 0,3–1 µs pro Aufruf. Relevant erst bei tausenden Aufrufen pro Frame (Schleifen im Graph). Messen mit einem PIE-Szenario.
- **Risiko / Verifikation**: Gering bis mittel. Wenn jemand `allowFiles` zur Laufzeit im Projekt-Dialog ändert, muss der Cache aktualisiert werden. Die Berechtigungen stehen ausdrücklich nur in `perm::Grants`. Ein Test mit geänderten Projekteinstellungen bei laufendem PIE fehlt.

### EDT-408 – `dumpFrameHeadless()` ist 4 750 Zeilen Testszenen in der Produktions-Übersetzungseinheit
- **ID**: EDT-408
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:5321–10075 (1 Funktion), Includes 24–120 für diese Zwecke
- **Problem**: Ein Drittel der Datei sind rund 30 Witness-Szenen (SKYTEST, MATERIALTEST, SECTIONTEST, ICONTEST, OCCLUSIONTEST, DOFTEST, SSRTEST, WATERTEST, WATERDROP, TEXARRAY, TEXBOMB, FOLIAGETEST, AUTOLAND, WATERLAKE, WATERBRUSH, LAKEEDIT, MOUNTAINTEST, THUMB, UITEST, ROPETEST, SELBOXTEST, LIGHTGIZMOTEST, SPLINETEST, SCENEIMAGE, MCPGIZMO …), jede mit eigener Verzweigung auf `std::getenv("HE_DUMP_…")`. Es sind 122 getenv-Aufrufe in der Datei. Sie sind nur für Screenshots/CI relevant, ziehen aber MaterialGraph, Terrain, Water, Foliage, Physics, Rope, Spline und Cinematic-Header in diese TU (Includes 70–140 gehören zum großen Teil dazu) und machen sie zur teuersten Übersetzungseinheit des Editors. Jede Änderung an einem Witness kompiliert auch `OnRender` neu und umgekehrt. Die Funktion ist nicht unit-testbar und schwer zu lesen (ein Block "sky test" beeinflusst Kamera und Environment für alle folgenden Blöcke).
- **Code-Auszug**:
```cpp
void EditorApplication::dumpFrameHeadless()
{ ... 4 750 Zeilen ...
	if (const char* st = std::getenv("HE_DUMP_SKYTEST"); st && *st && m_editorWorld) { ... }
	if (const char* mt = std::getenv("HE_DUMP_MATERIALTEST"); mt && *mt && m_editorWorld) { ... }
```
- **Lösung**:
  1. Neue Datei `EditorDumpWitnesses.cpp` (+ kleiner Header) mit `namespace HE::Ed::Dump`. Jedes Witness wird eine freie Funktion `void addWaterLakeWitness(DumpScene&, const char* mode)` mit einem kleinen `DumpScene`-Kontext (World, ContentManager, Renderer, Kamera, Selection, Undo).
  2. Eine Tabelle `{ "HE_DUMP_WATERLAKE", &addWaterLakeWitness }` statt 30 hintereinander stehender `if`-Blöcke.
  3. `dumpFrameHeadless` bleibt als Orchestrator: Settings → Witnesses → Settle → Capture → Nachwitnesses (GPUTIME, SCENEIMAGE, MCPGIZMO).
  4. Die Witness-Datei nur in Editor-Targets mit `HE_DUMP`-Option oder immer, aber in einer eigenen TU.
- **Erwarteter Effekt**: `EditorApplication.cpp` schrumpft um etwa 32 % (auf ca. 10 000 Zeilen), das Inkrement-Build von OnRender-Änderungen wird schneller (Schätzung, nicht gemessen), die Witnesses werden einzeln lesbar und wiederverwendbar (siehe EDT-409).
- **Risiko / Verifikation**: Gering für die Funktionalität, mittel für den Aufwand. Verhalten muss bit-genau bleiben, weil `scripts/he_shot.py` und die dokumentierten Baselines (Thema-Nummern in den Kommentaren) auf die Env-Namen und Reihenfolge der Blöcke bauen. Verifikation: pro Witness ein Referenzbild vor/nach dem Umzug (`scripts/he_shot.py`) und `md5`-Vergleich der "dump counters"-Zeile. Die Reihenfolge ist bedeutsam (z. B. `HE_DUMP_SKYTEST` setzt die Kamera vor allen anderen Blöcken, `WarmupMaterials` liegt bei Z. 8624 zwischen Szenenaufbau und Re-Authoring).

### EDT-409 – Wiederholter Boilerplate in den Witnesses (Kamera-Basis ×9, Landscape ×8, Sphere ×2, diffPercent ×2, Checkerboard ×4 …)
- **ID**: EDT-409
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp, u. a. 5913–5915, 6098–6100, 6155–6157, 6197–6199, 6231–6233, 9205–9207, 9363–9365, 9462–9464, 9547–9549 (Kamera-Vorwärtsvektor); 6466–6469, 6751–6754, 7185–7191, 7331–7334, 7457–7460, 7660–7663, 7784–7787, 8236–8239, 8568–8574 (Landscape bei y=300); 5885–5906 und 6066–6086 (UV-Sphere); 9888–9895 und 9977–9984 (`diffPercent`); 8820–8830, 8908–8914, 8950–8956 (Checkerboard-Composite); 7797–7810 und 8255–8267 (Bank-Höhenfeld); 8166–8168, 7920–7922, 8542–8544, 8171–8173 (`HE_DUMP_WATERHIDEGROUND/SHEET`)
- **Problem**: Der gleiche Code steht an vielen Stellen kopiert. Das gilt für die Kamera-Basis aus Pitch/Yaw (9 Mal wortgleich), die Landscape-Entity mit `position.y = 300`, `resolution`, `heightScale = 0`, `seed = 0`, `dirty = true` (8 Mal), die UV-Sphere mit `segU = 48, segV = 24` (2 Mal), den Bild-Diff in Prozent (2 Mal) und das Composite über Schachbrett (4 Mal). Für das Bank-Höhenfeld steht im Code sogar der Hinweis "same shape, so the two witnesses agree". Das ist ein Kopierzwang, den nur ein Helfer lösen kann. `envF`-Lambdas gibt es in drei Varianten (5508, 7496, 8566).
- **Code-Auszug**:
```cpp
const float cp = std::cos(m_editorCamera.pitch()), sp = std::sin(m_editorCamera.pitch());
const float cy = std::cos(m_editorCamera.yaw()),   sy = std::sin(m_editorCamera.yaw());
const glm::vec3 camFwd(cp * sy, sp, -cp * cy);
```
- **Lösung**: Helfer in der neuen Witness-Datei (EDT-408): `glm::vec3 EditorCamera::forward()` (existiert privat, wird hier mit Hinweis nachgebaut: "EditorCamera::forward is private" in Z. 5911; stattdessen öffentlich machen), `Entity makeFlatLandscape(world, name, size, res, y)`, `StaticMeshAsset makeUvSphere(segU, segV, r)`, `double diffPercent(a, b)`, `void writeOverCheckerPpm(path, rgba, w, h)`, `void hideTerrainChunks(world)`, `float envF(name, default)`.
- **Erwarteter Effekt**: Etwa 350–450 Zeilen weniger. Kein Laufzeiteffekt. Weniger Fehler bei neuen Witnesses.
- **Risiko / Verifikation**: Gering, wenn mechanisch umgesetzt. Referenzbilder pro Witness wie bei EDT-408. Die Landscape-Parameter unterscheiden sich im Detail (`resolution` 33/65/129, `water.res`), also nicht alles in einen Helfer mit Defaults pressen, sondern nur die Entity-Anlage.

### EDT-410 – `OnRender()` ist 2 129 Zeilen lang und enthält lokale Typen, Lambdas und statische Zustände
- **ID**: EDT-410
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:3015–5143, darin 3674–3772 (lokale Structs `PlayPhysicsInputs`/`PlayPoseWatch`, `static PlayPoseWatch s_playPose`, Lambda `samplePhysicsInputs`), 4329–4756 (Debug-Overlays), 3128–3272 (Input-Snapshot und Szenen-Requests), 4864–5093 (Collab/MCP/Team-Pumpe)
- **Problem**: Die Frame-Schleife ist eine einzige Funktion mit vielen Zuständen (`simulating`, `uiLive`, `stepping`, `gameDt`). Einzelne Abschnitte hängen über lokale Statics zusammen (`s_playPose` wird in Zeile 3726 deklariert, aber erst in 4125–4162 neu geschrieben). Das macht die Ordnungsbedingung ("Vergleich oben, Cache unten") unsichtbar und führt zu den langen erklärenden Kommentaren. Zwei Pfade (`renderer()`-Block und Collab-Block) liegen inkonsistent eingerückt (Z. 4964–4974). Eine Instanzvariable wäre richtig, weil `static` zwischen mehreren `EditorApplication`-Instanzen (Tests, Hub) geteilt würde.
- **Code-Auszug**:
```cpp
struct PlayPoseWatch { uint32_t entity = 0u; bool valid = false; glm::vec3 position{}; glm::vec3 rotation{}; PlayPhysicsInputs build{}; };
static PlayPoseWatch s_playPose;
```
- **Lösung**: Aufteilen in private Methoden in Frame-Reihenfolge: `pumpTransportAndBreakpoints()`, `pumpPlayInput()`, `syncAuthorEditsToPhysics()` (mit `PlayPoseWatch m_playPose` als Member und die Typen in einen anonymen Namespace), `tickWorldAndPhysics()`, `tickWidgetsAndPointer()`, `buildDebugOverlays()`, `pumpCollabAndMcp()`, `pushRendererSettings()`. Die Reihenfolge bleibt exakt, es gibt nur Methodenaufrufe statt Blöcke. Danach ist jede Phase einzeln profilierbar (`HE_PROFILE_SCOPE_N` pro Methode).
- **Erwarteter Effekt**: Keine Laufzeitänderung (bis auf Inlining). Lesbarkeit, Reviewbarkeit und die Möglichkeit, die Phasen in Tests zu treiben (z. B. `syncAuthorEditsToPhysics` mit einer Fake-Physikwelt).
- **Risiko / Verifikation**: Gering, solange keine Logik wandert. Die Reihenfolge (physics → camera → animation → compare) ist dokumentiert und muss erhalten bleiben. Vor/nach: PIE-Smoke (Play, Gizmo-Verschieben einer Kiste, Stop) mit `HE_HIDDEN_WINDOW=1`.

### EDT-411 – Config laden und speichern: ~120 Schlüssel zweimal von Hand aufgelistet
- **ID**: EDT-411
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:1500–1636 (laden) und 14298–14417 (`writeEditorConfig`)
- **Problem**: Für jedes Feld in `EditorConfig` gibt es zwei Zeilen mit demselben Schlüsselstring. Der Kommentar in Z. 14287–14291 sagt, die Funktion sei herausgezogen worden, weil "a second copy of that mapping is one that stops matching the first". Es gibt aber weiterhin zwei Kopien (Laden und Speichern), die sich nur in `getCustomConfig*`/`setCustomConfigEntry` unterscheiden. Ein Beispiel für die Gefahr: `RewardsReducedMotion` wird beim Laden geklemmt (Z. 1539), beim Speichern nicht. Beim Laden steht `KeepCPUAssetsInfoAcknoleged` als Altlast. Neue Felder in nur einer Richtung zu vergessen, bleibt unbemerkt, bis ein Nutzer einen Neustart macht.
- **Code-Auszug**:
```cpp
m_editorConfig.BloomEnabled   = globalstate.getCustomConfigBool("BloomEnabled",   m_editorConfig.BloomEnabled);   // 1598
globalstate.setCustomConfigEntry("BloomEnabled", m_editorConfig.BloomEnabled);                                  // 14380
```
- **Lösung**: Eine konstexpr-Tabelle in `EditorConfig.h` oder `EditorConfigIo.cpp`:
  `struct Field { const char* key; std::variant<bool EditorConfig::*, int EditorConfig::*, float EditorConfig::*, std::string EditorConfig::*> member; }`, plus `forEachField(cfg, visitor)`. Laden und Speichern sind dann je eine Schleife (`std::visit`). Die Sonderfälle (Legacy-Key, Clamp von RewardsReducedMotion, Kamera, ShowFlags, Shortcuts, Graphics-Rückgabe) bleiben davor/danach. Die Tabelle kann zusätzlich vom MCP-Settings-Tool und `settings_set` genutzt werden (siehe Cross-Modul).
- **Erwarteter Effekt**: Etwa 200 Zeilen weniger, Laden und Speichern können nicht mehr auseinanderlaufen. Kein Laufzeiteffekt (einmalig).
- **Risiko / Verifikation**: Gering. Unit-Test: Round-Trip `EditorConfig` → JSON → `EditorConfig` (alle Felder auf Nicht-Default setzen) und Vergleich. Ein Test für das Laden einer alten `config.json` ohne neue Schlüssel (Defaults bleiben) schützt die Abwärtskompatibilität.

### EDT-412 – `setupMcpTools()` ist 810 Zeilen mit zehn gleichen Hook-Lambdas
- **ID**: EDT-412
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:10414–11223; `lockedByOther`: 10588, 10641, 10796, 10823, 10843, 10871, 10889, 10916, 10943, 10974, 10988; `publishCreate`: 10655, 10769, 10857, 10874; `onAssetAppeared`: 10725, 10772, 10860, 10877; `isPlaying`: 10429, 10458, 10634, 10755 … ; `entityCount`: 10431–10439 und 10757–10762; Zeit in ms: 4881, 5100, 11250, 11310
- **Problem**: Jede Tool-Familie bekommt eine eigene Hook-Struktur. Die Lambdas `isPlaying`, `lockedByOther` (`m_collab.assetLockedByOther(rel)`), `publishCreate` und `onAssetAppeared` sind jeweils zeichengleich, bis zu elfmal. Die Entity-Zählung (`view<entt::entity>().each(++n)`, O(N)) steht doppelt. Ein neues Tool-Set muss die Familie von Hand ergänzen. Die gleichzeitige Pflegeliste ("Mcp-Tool-Registry") ist damit verteilt auf 800 Zeilen. Der Millisekunden-Zeitstempel `duration_cast<milliseconds>(steady_clock::now()…)` ist vier Mal ausgeschrieben.
- **Code-Auszug**:
```cpp
widget.lockedByOther = [this](const std::string& rel) { return m_collab.assetLockedByOther(rel); };
input.lockedByOther  = [this](const std::string& rel) { return m_collab.assetLockedByOther(rel); };
material.lockedByOther = [this](const std::string& rel) { return m_collab.assetLockedByOther(rel); };
...
```
- **Lösung**: Gemeinsame Helfer in einem anonymen Namespace oder als Lambdas am Funktionskopf: `auto isPlaying = [this]{...}; auto lockedByOther = ...; auto publishCreate = ...; auto appeared = ...;`, dann `widget.lockedByOther = lockedByOther;`. Alternativ eine `McpCommonHooks`-Basisstruktur in `McpTools*.h`, die die Familien erben. `static std::uint64_t nowMs()` als freie Funktion (auch für EDT-403/405). Für `entityCount` `reg.storage<entt::entity>().size()` verwenden (O(1), `free_list`-abhängig, prüfen) oder `registry.alive()`. Die Familien in je eine Methode (`registerAssetFamily`, `registerParticleFamily` …) teilen.
- **Erwarteter Effekt**: Etwa 100–150 Zeilen weniger, eine Stelle für das gemeinsame Verhalten. Die Entity-Zählung wird O(1) (nur bei Statusabfragen, nicht pro Frame).
- **Risiko / Verifikation**: Gering. Bestehende MCP-Tests (`tests/test_mcp_*.cpp`) laufen gegen die Hooks, nicht gegen `EditorApplication`. Daher ein Smoke-Lauf mit `HE_MCP=1` und einem scripted Client (`scripts/he_mcp_multiclient.py`) nach der Umstellung.

### EDT-413 – Play-Start/-Stop schreibt und liest den Szenen-Snapshot über eine feste Temp-Datei
- **ID**: EDT-413
- **Kategorie**: E / C
- **Schweregrad**: Mittel
- **Typ**: Performance + Robustheit
- **Ort**: EditorApplication.cpp:12640–12651 (Save beim Play-Start), 12984–12999 (Load beim Stop); `saveToMemory`/`loadFromMemory` existieren (SceneSerializer.h:118–119) und werden in Z. 13408 schon benutzt
- **Problem**: Beim Drücken von Play wird die gesamte Welt synchron im UI-Thread binär in `temp_directory_path() / "he_play_snapshot.hescene_bin"` geschrieben und beim Stop wieder gelesen. Das ist blockierende Disk-I/O für etwas, das nur im Prozess gebraucht wird. Bei großen Szenen (dutzende MB) kann das spürbar sein. Außerdem ist der Pfad fest: Zwei laufende Editor-Instanzen (zwei Projekte, oder ein Editor plus ein CI-Dump-Lauf) überschreiben gegenseitig ihren Snapshot, und beim Stop würde eine Instanz die Welt der anderen laden. Auf Systemen mit schreibgeschütztem/vollem Temp schlägt Play fehl, obwohl nichts davon nötig wäre.
- **Code-Auszug**:
```cpp
const std::filesystem::path snapshot =
	std::filesystem::temp_directory_path() / "he_play_snapshot.hescene_bin";
...
if (!serializer.save(*m_editorWorld, snapshot, SerializeFormat::Binary))
...
if (!serializer.load(*m_editorWorld, snapshot, SerializeFormat::Binary))
```
- **Lösung**: `std::vector<uint8_t> m_playSnapshot` als Member. Beim Play `serializer.saveToMemory(*m_editorWorld, m_playSnapshot)`, beim Stop `serializer.loadFromMemory(*m_editorWorld, m_playSnapshot)` und danach `m_playSnapshot.clear(); shrink_to_fit()`. Alternativ bei Speicherdruck eine Datei mit PID im Namen. Das Binärformat bleibt dasselbe, falls `saveToMemory` dasselbe Format wie `SerializeFormat::Binary` schreibt (prüfen: `saveToMemory` wird für Undo-Snapshots benutzt).
- **Erwarteter Effekt**: Play-Start/-Stop ohne Datei-I/O (Schätzung: bei 50 MB Szene 50–300 ms eingespart je Richtung, abhängig vom Datenträger). Wegfall der Instanz-Kollision.
- **Risiko / Verifikation**: Mittel. Wenn `saveToMemory` ein anderes Layout als `save(..., Binary)` hat, muss `loadFromMemory` es lesen können (gleiches Paar benutzen). Hinweis auf CLAUDE.md: Szenen werden verlustfrei serialisiert. Test: `Save→Play→Edit→Stop`-Round-Trip mit allen Komponenten (existierende `tests/test_scene*` ergänzen, ein Test "play snapshot round trip" über `saveToMemory`/`loadFromMemory`).

### EDT-414 – Renderer-Einstellungen: dieselben ~100 Zeilen in `OnRender` und `dumpFrameHeadless`, und ein Push von 12 Settings pro Frame
- **ID**: EDT-414
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: EditorApplication.cpp:3528–3635 (`OnRender`), 5339–5498 (`dumpFrameHeadless`)
- **Problem**: Beide Blöcke bauen Bloom, SSAO, DoF, MotionBlur, AA, GI, SSR, GI-Reflections, RenderPath, Occlusion aus `m_editorConfig` auf und wenden die `HE_DUMP_*`-Overrides an. Die Override-Logik ist doppelt (einmal mit `static const char*`-Cache, einmal mit `getenv` pro Aufruf). Wer ein neues Setting ergänzt, muss beide pflegen. Dazu: `OnRender` ruft pro Frame zwölf `renderer()->Set*Settings(...)` auf. Ob die Renderer-Backends auf "unverändert" prüfen und die Pipelines nicht neu bauen, ist hier nicht sichtbar (siehe Unsicher).
- **Code-Auszug**:
```cpp
renderer()->SetBloomSettings(IRenderer::BloomSettings{ m_editorConfig.BloomEnabled, ... });
renderer()->SetSSAOSettings(IRenderer::SSAOSettings{ ... });
...
renderer()->SetGIReflectionSettings(gr);
```
- **Lösung**: Eine Methode `void pushRendererSettings(IRenderer& r, const EnvOverrides& ov)`, die beide Pfade aufrufen. Die Overrides werden einmal beim Start in ein `EnvOverrides`-Struct geparst (optional-Felder). Pro Frame nur pushen, wenn sich `m_editorConfig` gegenüber dem letzten Push geändert hat (ein `RendererSettingsSnapshot` mit `operator==`, gebaut aus den Config-Feldern). Der Frame-Push bleibt eine Zeile.
- **Erwarteter Effekt**: Etwa 120 Zeilen weniger. Pro Frame zwölf Setter-Aufrufe gespart, vermutlich mikroskopisch (µs). Nur wegen der Wartbarkeit angehen.
- **Risiko / Verifikation**: Gering. Die Kommentare in Z. 3604–3611 betonen, dass die Overrides bei jedem Frame gelten müssen. Das bleibt erhalten, wenn der Snapshot die Overrides schon enthält. Verifikation: `he_shot.py` mit `HE_DUMP_GI`, `HE_DUMP_SSR`, `HE_DUMP_RENDERPATH` vor/nach.

### EDT-415 – `saveOpenTabs()` baut jeden Frame einen Signatur-String
- **ID**: EDT-415
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:5097 (Aufruf), 13242–13250
- **Problem**: Jeden Frame wird `sig` aus allen Tab-Pfaden plus `std::to_string(m_activeTab)` zusammengesetzt und mit `m_lastTabSig` verglichen. Bei 20 Tabs mit je 100 Zeichen sind das 2 KB String-Bau pro Frame. Danach schreibt eine Änderung die gesamte Config (`writeConfig`) synchron im UI-Thread.
- **Code-Auszug**:
```cpp
std::string sig;
for (const auto& t : m_tabs) sig += t.assetPath + "|";
sig += std::to_string(m_activeTab);
if (sig == m_lastTabSig) return;
```
- **Lösung**: 64-Bit-Hash über die Pfade und den Index statt eines Strings (`m_lastTabHash`). Noch besser: `EditorUI`/`EditorTabs` zählen eine `tabRevision` hoch, wenn sie Tabs ändern. Das gleiche Signal löst EDT-405.
- **Erwarteter Effekt**: Pro Frame null Allokationen (Schätzung: 1–3 µs). Der Config-Write bleibt.
- **Risiko / Verifikation**: Sehr gering. Hash-Kollision hätte nur zur Folge, dass ein Speichern ausfällt. Ein Test über die `EditorTabs`-Funktionen existiert nicht für diesen Pfad.

### EDT-416 – Texture-Hot-Reload invalidiert pro geänderter Textur alle Materialien
- **ID**: EDT-416
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:3476–3481
- **Problem**: Für jede geänderte Textur wird `contentManager().enumerateIds(HE::AssetType::Material)` neu aufgebaut (Vektor) und alle Materialien werden invalidiert. Bei einem `git pull` mit 200 geänderten Texturen und 5 000 Materialien sind das 200 Vektor-Allokationen und 1 Mio. `InvalidateMaterial`-Aufrufe auf dem UI-Thread in einem Poll. Das Ergebnis ist nach dem ersten Mal identisch.
- **Code-Auszug**:
```cpp
case HE::AssetType::Texture:
	for (const HE::UUID& matId : contentManager().enumerateIds(HE::AssetType::Material))
		renderer()->InvalidateMaterial(matId);
	break;
```
- **Lösung**: Flag `bool texturesChanged` im Switch setzen, nach der Schleife einmal über alle Materialien invalidieren. Besser: nur die Materialien invalidieren, die die geänderte Textur referenzieren (`MaterialAsset::textureIds`, `graphTextureIds`), wenn die Reverse-Map billig verfügbar ist (Cross-Modul).
- **Erwarteter Effekt**: Von O(Texturen × Materialien) auf O(Materialien) pro Poll.
- **Risiko / Verifikation**: Gering. Verhalten identisch. Test: `pollHotReload` mit zwei geänderten Texturen, `InvalidateMaterial` für jedes Material höchstens einmal (Fake-Renderer).

### EDT-417 – Kleine Pro-Frame-Allokationen in Collab-/Prefab-/Landscape-Pfaden
- **ID**: EDT-417
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:5085–5091 (Selektion für Presence), 13422–13436 (`recordPrefabEdits`, Z. 3293 aufgerufen), 4874–4880 (`m_retargetJobs`), 4545–4549 (Landscape-Vorschau), 4626–4637 (Ringe)
- **Problem**:
  - `m_collab.active()` legt pro Frame einen `std::vector<uint64_t> selection` an (reserve + `subjectFor` pro Eintrag), obwohl sich die Auswahl selten ändert.
  - `recordPrefabEdits()` baut jeden Frame `current` (UUID-Vektor der Auswahl, inkl. `try_get<EntityIdComponent>` pro Eintrag), selbst wenn sich die Revision nicht bewegt hat. Der Vektor wird nur gebraucht, um `m_prefabEditLastSelection` zu aktualisieren.
  - `m_retargetJobs.erase(remove_if(... wait_for(0s)))` läuft jeden Frame, auch wenn die Liste leer ist (billig, aber nutzlos).
  - Die Landscape-Vorschau (nur bei offenem "Neue Landschaft"-Formular) allokiert pro Frame `hpre` und bewertet `terrainHeightAt` für bis zu 33×33 Punkte (mit Oktaven).
  - Presence-Ringe: 3 × 28 `cos/sin` pro Teilnehmer und Frame.
  Jeder Punkt für sich ist klein. Bei 1 000+ ausgewählten Entities (Auswahl-Rahmen) wachsen die ersten beiden linear.
- **Code-Auszug**:
```cpp
std::vector<std::uint64_t> selection;
selection.reserve(m_selection.size());
for (Entity sel : m_selection.entities())
	selection.push_back(m_collab.subjectFor(static_cast<std::uint32_t>(entt::to_integral(sel))));
m_collab.setLocalPresence(p, r, selection);
```
- **Lösung**: Die Presence-Auswahl nur neu berechnen, wenn sich eine `m_selection.revision()` oder die Entity-Menge geändert hat (Member-Vektor, `clear()` + wiederverwenden). `recordPrefabEdits`: Auswahl nur bei Änderung der Selektion/Revision aktualisieren (Selection-Revision oder Größenvergleich). `m_retargetJobs`: `if (!m_retargetJobs.empty())`. Landscape-Vorschau: Höhenraster in Member cachen, Schlüssel aus den `newTerrain`-Parametern. Ringe: statische 28er Sin/Cos-Tabelle (`constexpr`-fähig).
- **Erwarteter Effekt**: Pro Frame wenige Allokationen weniger, bei großen Auswahlen mehr. Schätzung: unter 10 µs im Normalfall.
- **Risiko / Verifikation**: Gering. Beim Selektionsvergleich muss auch Entity-Löschung (Handle-Wiederverwendung) erkannt werden: `prune` in `makeContext` ändert die Auswahl, daher die Revision dort mitzählen.

### EDT-418 – `OnEvent`: redundante Backend-Weiche und doppelte Text-Edit-Tastentabelle
- **ID**: EDT-418
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:14435–14455, 14550–14578, 14598–14669
- **Problem**: (1) Die `switch (m_backend)` ruft in jedem Zweig (OpenGL, Vulkan, Metal, D3D11, D3D12) `ImGui_ImplSDL3_ProcessEvent(&event)`. Nur `default` tut nichts. Der Kommentar der D3D-Zweige widerspricht dem Code ("Win32 backend … handles Win32 messages … but we still forward SDL events"). (2) Die Taste-für-Taste-Behandlung eines fokussierten Labels (Z. 14550–14578) ist eine Teilmenge der Behandlung eines Textfelds (Z. 14598–14669). Die Cases für Links/Rechts/Home/End/Up/Down/Ctrl+A/Ctrl+C stehen zweimal.
- **Code-Auszug**:
```cpp
case HE::RendererBackend::OpenGL: case HE::RendererBackend::Vulkan: case HE::RendererBackend::Metal:
	consumed = ImGui_ImplSDL3_ProcessEvent(&event); break;
case HE::RendererBackend::D3D11: case HE::RendererBackend::D3D12:
	consumed = ImGui_ImplSDL3_ProcessEvent(&event); break;
```
- **Lösung**: (1) `bool consumed = ImGui_ImplSDL3_ProcessEvent(&event);` ohne Weiche (der Software-Backend-Fall prüfen: `default` hat bisher nicht weitergeleitet, `HE::RendererBackend::Software` könnte absichtlich ausgenommen sein; dann `if (m_backend != Software)`). (2) Eine Funktion `bool routeTextEditKey(WidgetManager&, const SDL_KeyboardEvent&, bool allowEdit)`, die für beide Fälle die gemeinsame Tabelle benutzt und `allowEdit == false` die Schreibtasten auslässt.
- **Erwarteter Effekt**: Etwa 40 Zeilen weniger. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Gering bis mittel. Das Verhalten bei Software-Backend und Tastatur-Routing ist nur manuell testbar. `tests/test_ui_widgets.cpp` testet das WidgetManager-Verhalten, nicht diese Weiche.

### EDT-419 – Testszenen-Fehler: `HE_DUMP_LAKEWATERRES` wird sofort überschrieben
- **ID**: EDT-419
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung (Fehler in Testcode)
- **Ort**: EditorApplication.cpp:8245–8254
- **Problem**: Z. 8249–8250 setzen `ltc.water.res` aus `HE_DUMP_LAKEWATERRES`. Vier Zeilen später setzt Z. 8254 unbedingt `ltc.water.res = 128;`. Die Umgebungsvariable hat damit keine Wirkung, obwohl der Kommentar (8245–8246) sie als Teil des Messaufbaus für den 1-km-Fall nennt (`HE_DUMP_LAKESIZE=1000 HE_DUMP_LAKERES=1025 HE_DUMP_LAKEWATERRES=256`). Die dort gemessenen "drag"-Zeiten gelten damit für 128 statt 256 Wasserzellen.
- **Code-Auszug**:
```cpp
if (const char* lw = std::getenv("HE_DUMP_LAKEWATERRES"); lw && std::atoi(lw) > 1)
	ltc.water.res = static_cast<uint32_t>(std::atoi(lw));
ltc.heightScale = 0.0f;
ltc.seed        = 0;
ltc.dirty       = true;
ltc.water.res   = 128;
```
- **Lösung**: Zeile 8254 durch `if (ltc.water.res == 0 /* kein Env */) ltc.water.res = 128;` ersetzen oder vor die Env-Zeilen ziehen (Default zuerst, Override danach).
- **Erwarteter Effekt**: Die LAKEEDIT-Messreihe "drag" mit `HE_DUMP_LAKEWATERRES` wird gültig.
- **Risiko / Verifikation**: Sehr gering. Bestehende Baselines mit der Variable gesetzt wären vorher nicht gültig gewesen. Dokumentierte Zahlen (docs, Thema 188) prüfen.

### EDT-420 – Acht fast gleiche Worker-Thread-Muster (Probes/Installer)
- **ID**: EDT-420
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:2324–2339 (Toolchain), 2349–2372 (Claude), 2396–2410 (Git), 2529–2623 (SFTP), 2630–2669 (Manifest), 2672–2689 (Router), 2693–2725 (Git-Identität), 2734–2777 (Git-Install), 2783–2811 (Toolchain-Install); Joins in `OnShutdown` 14132–14171
- **Problem**: Jedes Muster: `if (m_xThread.joinable()) m_xThread.join(); m_xChecked.store(false); m_xThread = std::thread([this]{ result = probe(); m_xChecked.store(true); });` plus ein Join in `OnShutdown`. Jedes Muster braucht Member (Thread, Atomics, Ergebnis). Einzelne Varianten haben Cancel-Flags oder Mutex-Logs. Neue Probes (wie `startClaudeProbe`) kopieren das Muster. Die Shutdown-Liste muss von Hand vollständig sein, sonst terminiert `std::thread` in `~thread`. Beim erneuten Recheck aus der UI (Callback aus dem Frame, in dem `ctx.toolchainProbe` noch gelesen wird) wird das Ergebnis-Member vom Worker beschrieben, während die UI den Zeiger noch hält (siehe Unsicher).
- **Code-Auszug**:
```cpp
if (m_toolchainThread.joinable()) m_toolchainThread.join();
m_toolchainChecked.store(false, std::memory_order_release);
m_toolchainThread = std::thread([this] { ...; m_toolchainChecked.store(true, std::memory_order_release); });
```
- **Lösung**: Kleine Vorlage `template<class T> class ProbeJob { void start(std::function<T()>); bool done() const; const T* result() const; ~ProbeJob() { join(); } }` (Ergebnis erst nach `done`, Join im Destruktor, optionale Cancel-Flag). Das ersetzt je Probe vier Member und eine Zeile in `OnShutdown`. `std::jthread` mit `std::stop_token` (C++20) erledigt das Join und das Abbrechen.
- **Erwarteter Effekt**: Etwa 100 Zeilen weniger, Shutdown-Liste entfällt, die Datenrennen-Frage wird an einer Stelle gelöst.
- **Risiko / Verifikation**: Gering bis mittel. Die Probes haben unterschiedliche Lebensdauern (`m_gitThread` wird aus anderen Worker-Threads re-probed). Beim Umstellen die Reihenfolge der Joins in `OnShutdown` erhalten (Kommentare dort sind sicherheitsrelevant).

### EDT-421 – Kleinigkeiten: doppelte Includes, unbenutzte Variable/Parameter, irreführender Parametername
- **ID**: EDT-421
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:144 und 155 (`<algorithm>` doppelt), 140 und 166 (`<SDL3/SDL.h>` doppelt), 10218/10221 (`root` + `(void)root`), 13964–13968 (`GameLogicDeltaTime(float rawDt)` mit `(void)rawDt`), 1027 (`onRemoteTransform` nimmt `subject`, bekommt aber einen lokalen Entity-Handle, siehe CollabController.cpp:1457–1461)
- **Problem**: Reine Aufräumpunkte. Besonders der Parametername in Z. 1027 führt in die Irre: Weiter oben (Z. 660, 4665) sind "Subjects" uuid-abgeleitete 64-Bit-IDs, die über `entityForNetId` aufgelöst werden. Hier wird derselbe Name für den schon aufgelösten 32-Bit-Handle benutzt und per `static_cast<entt::id_type>` gecastet. Das Verhalten ist korrekt (nur der Name ist falsch), aber ein Leser verdächtigt einen Fehler.
- **Code-Auszug**:
```cpp
m_collab.onRemoteTransform([this](std::uint64_t subject, const float pos[3], ...) {
	const auto e = static_cast<entt::entity>(static_cast<entt::id_type>(subject));
```
- **Lösung**: Doppelte `#include`s entfernen, `root`/`(void)root` löschen, den Parameter in `handle` umbenennen (Signatur im Header bleibt, nur der Lambda-Parametername). `GameLogicDeltaTime` ist vermutlich eine virtuelle Überschreibung, dann den Parameter unbenannt lassen (`float /*rawDt*/`).
- **Erwarteter Effekt**: Lesbarkeit. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Keines.

---

## Gelesen

Ja, `src/HE_Editor/EditorApplication.cpp` wurde vollständig gelesen (Zeilen 1–14 759) in Blöcken von 650–1 000 Zeilen, weil das Lesewerkzeug 1 500-Zeilen-Blöcke wegen des Token-Limits abgelehnt hat. Die Witness-Abschnitte 5321–10075 wurden gelesen, aber wegen ihrer Gleichförmigkeit nur auf Duplikate, Fehler und Struktur hin analysiert, nicht auf die Korrektheit ihrer Bildlogik. Es wurde kein Quellcode geändert und nichts kompiliert oder gemessen.

## Editor Slice 5 – EditorApplication.h … EditorGuides.h (16 Dateien, ~4 200 Zeilen)

## Zusammenfassung
Der Slice besteht überwiegend aus Headern, Daten und kleinen, gut kommentierten Hilfsmodulen (Kamera, Dock-State, Command-Gateway, Guides-Bauteile). Es gibt keine eigentlichen Per-Frame-Hot-Paths in diesen Dateien selbst; die belegbaren Kosten liegen in dem, was die Header auslösen (`AppContext`-Neuaufbau, Struktur-Diff, Typ-Cache-Kaltstart) und in `EditorCommands.cpp` (unnötige Serialisierung, O(n)-UUID-Suche mit String-Bau pro Entity). Insgesamt 11 Funde: 0 Hoch, 5 Mittel, 6 Niedrig. Alle Zeilenangaben wurden gegen den Ist-Stand geprüft; Aussagen zu Dateien außerhalb des Slice (EditorApplication.cpp, EditorUI.cpp, TaskBoardPanel.cpp, ContentBrowserPanel.cpp) sind als solche gekennzeichnet und gezielt per grep/Ausschnitt verifiziert, nicht vollständig gelesen.

## Funde

### EDT-501 – Collab-Struktur-Diff baut jeden Frame ein komplettes `unordered_set<Entity>` und hält vier knotenbasierte Hash-Container
- **ID**: EDT-501
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorApplication.h:923-936` (Container); Verbraucher `EditorApplication.cpp:11400-11420`, `11529-11532`, `11595-11600` (nur Ausschnitte gelesen)
- **Problem**: Solange eine Collab-Session läuft (`EditorApplication.cpp:4976`: `if (m_collab.inSession()) syncStructuralChanges();`), wird pro Frame über alle Entities ein neues `std::unordered_set<Entity> current` aufgebaut (ein Heap-Knoten pro Entity, kein `reserve`), danach mehrfach über `m_structureKnown` iteriert (`m_structureParent.count(e)`, `current.count(e)`). Dazu kommen drei weitere knotenbasierte Maps mit demselben Schlüssel (`m_structureParent`, `m_structureUnsendable`, `m_structureUndeletable`). Bei einigen tausend Entities sind das tausende Allokationen/Frees und Hash-Lookups pro Frame, obwohl sich in aller Regel nichts geändert hat.
- **Code-Auszug**:
```cpp
std::unordered_set<Entity> current;
reg.view<entt::entity>().each([&](auto e) {
    if (!isLocalOnly(e)) current.insert(e);
});
...
for (const Entity e : m_structureKnown)
    if (reg.valid(e) && !m_structureParent.count(e)) ...
```
- **Lösung**: (1) `current` als Member behalten und pro Frame `clear()` + `reserve(reg.storage<entt::entity>().size())`, damit die Bucket-Arrays nicht neu wachsen (Knoten bleiben, aber die Rehash-Kosten entfallen); besser (2) gar keine Menge bauen: `reg.valid(e)` für `m_structureKnown` reicht für "Destroyed", und "Created" kann über die Differenz `registry.size()` gegen `m_structureKnown.size()` plus einen Early-Out (Anzahl und Entity-Storage-Revision unverändert → nur Reparent-Check) abgekürzt werden. (3) Die drei Nebencontainer zu einer `unordered_map<Entity, StructInfo{parent, unsendableFrames, undeletableFrames}>` zusammenführen (ein Lookup statt drei).
- **Erwarteter Effekt**: Schätzung: 1–3 Größenordnungen weniger Allokationen/Frame in Sessions mit großen Szenen; im Solo-Betrieb kein Effekt. Messen mit Szene ≥ 5 000 Entities und aktiver Session.
- **Risiko / Verifikation**: Die Regeln liegen in `StructuralSync.h` und sind getestet (`tests/` Struktur-Sync-Tests suchen); jede Änderung muss "Echo-Schutz" (`forgetStructureOf`) und die Retry-Zähler erhalten. Early-Out nur, wenn Storage-Größe UND ein Struktur-Revisionszähler unverändert sind, sonst werden Reparents verpasst.

### EDT-502 – `EditorCommands` serialisiert Undo-Daten, die nur eine von zwei Senken liest
- **ID**: EDT-502
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorCommands.cpp:257-288` (applyCreate), `290-306` (applyDestroy), `363-402` (applyComponents); `EditorCommands.h:174-178`
- **Problem**: `execute()` füllt `Recorded` immer vollständig (`serializeSubtree`, `serializeEntityComponents` je zweimal), obwohl das Ergebnis ausschließlich in `if (sink) sink->recordCommand(rec, origin)` (Zeile 250) landet und `SnapshotUndoSink::recordCommand` die Default-No-op-Implementierung hat (nur `CollabUndoSink` liest `rec`). Außerhalb einer Session (Solo, `sink == m_snapshotSink`) und bei `Origin::Remote` (`sink == nullptr`) ist die Serialisierung vollständig verschwendet. Zusätzlich wird in `applyComponents` der Zustand nach dem Schreiben ein zweites Mal serialisiert (Zeile 382 und erneut 399 für `publishComponents`), obwohl `rec.afterComponents` identisch ist.
- **Code-Auszug**:
```cpp
rec.beforeComponents = serializer.serializeEntityComponents(*m_world, cmd.target);
...
rec.afterComponents = serializer.serializeEntityComponents(*m_world, cmd.target);
...
m_hooks.publishComponents(..., serializer.serializeEntityComponents(*m_world, cmd.target));
```
- **Lösung**: `execute()` bestimmt `sink` bereits vor dem Apply; ein `bool wantRecord = sink && sink != m_snapshotSink` (oder eine virtuelle `bool IUndoSink::needsRecord() const`) an die `apply*`-Funktionen durchreichen und die Serialisierung nur dann ausführen. In `applyComponents` für `publishComponents` `rec.afterComponents` wiederverwenden (bei `!wantRecord` einmal lokal serialisieren). Der Destroy-Fall bleibt in Sessions nötig, weil `rec.subtree` dort die Undo-Daten ist.
- **Erwarteter Effekt**: Schätzung: pro MCP-Create/Destroy/SetComponents ein bis zwei vollständige Subtree-/Komponenten-Serialisierungen weniger (bei großen Prefabs Millisekunden). Aktuell gehen nur MCP-Tools durch das Gateway (Kommentar `EditorApplication.h:880-885`), daher selten; wächst, sobald UI und Remote-Handler migriert werden.
- **Risiko / Verifikation**: Bestehende Gateway-Tests (`tests/` zu EditorCommands/CollabUndo) müssen unverändert grün bleiben; neuer Test: Solo-Pfad ruft nie `serializeSubtree` (Zähler im Test-Hook) und `publishComponents` erhält denselben Blob wie bisher.

### EDT-503 – `entityByUuid`: O(n) mit `snprintf` + `std::string`-Allokation pro Entity
- **ID**: EDT-503
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorCommands.cpp:406-429`; Aufrufer `TaskBoardPanel.cpp:547,550,579,603` (Orphan-Check), `McpToolsEntity.cpp:206,443,573`
- **Problem**: Die Suche formatiert für jede Entity die UUID neu in einen `std::string` (`uuidOf`: `snprintf` + Konstruktion) und vergleicht Strings, statt die gesuchte UUID einmal zu parsen und die zwei `uint64` zu vergleichen. Der Header begründet den fehlenden Index zwar ("Aufruf einmal pro Request"), aber `TaskBoardPanel::refreshOrphans` ruft sie pro Entity-Link und Sekunde (`TaskBoardPanel.cpp:520-526` throttled auf 1 s) auf, also O(Links × Entities) Allokationen pro Sekunde.
- **Code-Auszug**:
```cpp
reg.view<const EntityIdComponent>().each([&](auto e, const EntityIdComponent&) {
    if (found != entt::null) return;
    if (uuidOf(world, e) == uuid) found = e;
});
```
- **Lösung**: Überladung `Entity entityByUuid(HorizonWorld&, const HE::UUID&)` die direkt `c.id.hi/lo` vergleicht; die String-Variante parst einmal (`HE::uuidFromHex` existiert, genutzt in `TaskBoardPanel.cpp:675`) und delegiert. `TaskBoardPanel` kann dann `l.id` direkt übergeben statt `HE::uuidToHex(l.id)`. Außerdem Schleife mit `break` beenden (entt `each` kann nicht abbrechen; stattdessen `for (auto [e, c] : view.each())` mit `return e`).
- **Erwarteter Effekt**: Schätzung: 20–50× schneller pro Suche, keine Heap-Allokationen. Messen mit 5 000 Entities × 20 Entity-Links.
- **Risiko / Verifikation**: Verhalten identisch, solange die String-Variante exakt dasselbe Format akzeptiert wie `uuidOf` erzeugt (32 Zeichen, Kleinbuchstaben). `uuidFromHex` akzeptiert evtl. Großbuchstaben/Bindestriche; dann würden bisher erfolglose Eingaben plötzlich matchen. Test: bestehende MCP-Entity-Tests + je ein Fall für ungültige/Großschreibung.

### EDT-504 – Kalter Asset-Typ-Cache liest nach jedem Content-Refresh synchron Dateiheader im Draw-Pfad
- **ID**: EDT-504
- **Kategorie**: E / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorAssetTypeCache.cpp:18-40`; `EditorAssetTypeCache.h:5-15`; Aufrufer `ContentBrowserPanel.cpp:1361`, `1851` (nur Ausschnitt gelesen); Invalidierung `ContentBrowserPanel.cpp:2976,3844`, `EditorApplication.cpp:830`, `EditorUI.cpp:770`
- **Problem**: `invalidateAll()` leert den Cache bei jedem Content-Refresh. Danach öffnet `sniffHeader` im UI-Thread pro unbekanntem Pfad eine `std::ifstream` (Open + 32-Byte-Read). Besonders kritisch ist `ContentBrowserPanel.cpp:1851`: der Typfilter läuft über den ganzen (rekursiven) Dateibaum, nicht nur über sichtbare Zeilen. Der Kommentar dort ("costs one header read per asset ONCE") stimmt nur bis zur nächsten Invalidierung. Auf einem Projekt mit tausenden Assets (oder Netzlaufwerk/Antivirus unter Windows) ergibt das einen Frame-Hänger direkt nach jedem Refresh.
- **Code-Auszug**:
```cpp
HE::AssetType sniffHeader(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    ...
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
```
- **Lösung**: Der Refresh läuft bereits auf einem Worker (`m_contentRefreshFuture`, `EditorApplication.h:1226`). Dort den Typ beim Scan mitlesen (die Datei wird ohnehin per `directory_iterator` angefasst) und per `prime(path, type)` in den Cache schreiben bzw. den Cache nicht leeren, sondern pro Eintrag über `(mtime,size)` validieren. Alternativ `invalidateAll()` nur als Generationszähler erhöhen und Einträge lazy neu validieren.
- **Erwarteter Effekt**: Schätzung: Wegfall von N synchronen Opens im Frame nach jedem Refresh; Größenordnung ms bis 100 ms bei 5–10 k Dateien. **Unsicher / messen**, siehe unten.
- **Risiko / Verifikation**: Cache-Korrektheit nach Löschen/Umbenennen (genau der Bug, den die Datei dokumentiert) muss erhalten bleiben; `tests/` zum Typ-Cache (Suche `AssetTypeCache`) um "Datei ersetzt durch anderen Typ" ergänzen. Thread-Sicherheit: Cache ist bewusst UI-only und lock-frei; Priming vom Worker bräuchte eine Übergabeliste, die der UI-Thread einträgt.

### EDT-505 – `AppContext`/`makeContext()`: ~60 `std::function`-Lambdas und ~100 Referenz-/Feldinitialisierungen pro Aufruf, zweimal pro Frame in Sessions
- **ID**: EDT-505
- **Kategorie**: A / C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Editor/EditorApplication.h:92-586` (AppContext), `EditorApplication.cpp:12005-12429` (makeContext, nur Anfang/Struktur gelesen); Aufrufe `EditorApplication.cpp:5095` (jeder Frame), `10219` (`updateAssetCollabSync`, jeder Frame in Session, `:4977`)
- **Problem**: `AppContext` ist ein God-Struct mit rund 60 `std::function`-Feldern und Dutzenden Referenzen. `makeContext()` (≈ 410 Zeilen, 61 `[this`-Lambdas gezählt) baut es bei jedem Aufruf komplett neu, obwohl sich fast nichts ändert. In einer Session passiert das zweimal pro Frame, weil `updateAssetCollabSync` sich einen eigenen Kontext baut, der zudem nur zur Weitergabe dient (`const std::string& root = ...; (void)root;`, Zeilen 10218-10221 sind toter Code). Jedes `std::function` mit `[this]`-Capture passt in den Small-Buffer (kein Heap), die Kosten sind also nur Konstruktion/Destruktion ≈ 4–6 KB, eher niedrig; dafür ist die Struktur der eigentliche Komplexitätstreiber (jedes neue Feature fügt hier ein Feld plus Lambda hinzu).
- **Code-Auszug**:
```cpp
AppContext ctx = makeContext();

(void)root;
std::unordered_set<std::string> openRel;
```
- **Lösung**: Kurzfristig `updateAssetCollabSync(nowMs, ctx)` den Frame-Kontext aus `OnRender` weiterreichen (eine Konstruktion/Frame) und die toten Zeilen 10218/10221 entfernen. Mittelfristig `AppContext` in einen langlebigen Member aufteilen (Callbacks einmal in `OnInit` binden) und pro Frame nur die veränderlichen Werte (`isPlaying`, `sceneDirty`, `recoveryOffer`, `toolchainProbe`, …) aktualisieren; Callback-Gruppen als Interfaces (`IProjectActions`, `IGitActions`) statt 60 Einzel-`std::function`.
- **Erwarteter Effekt**: Schätzung: einige µs pro Frame, nicht messbar relevant; Nutzen ist Vereinfachung/Wartbarkeit. **Messen**, bevor mehr als die Kurzfrist-Variante umgesetzt wird.
- **Risiko / Verifikation**: `makeContext()` ruft außerdem `m_selection.prune(...)` auf (Zeile 12011); wird der Aufruf in `updateAssetCollabSync` entfernt, muss das Prune vorher im Frame schon gelaufen sein (ist es, `OnRender` ruft es früher auf Zeile 5095 – vor dem Weiterreichen prüfen, dass die Reihenfolge Zeile 4977 vs. 5095 stimmt, denn `updateAssetCollabSync` läuft VOR dem UI-Kontext).

### EDT-506 – Asset-Tab-Dispatch: Kette von bis zu 19 `isXAsset(path)`-Prädikaten, jedes mit eigenem Path-Hash und Map-Lookup
- **ID**: EDT-506
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/EditorAssetTypeCache.cpp:30-45`, `EditorAssetTypeCache.h:22-25`; Aufrufer `EditorUI.cpp:1245`, `4119-4150ff` (19 Treffer `Panel::is…Asset(` in EditorUI.cpp)
- **Problem**: Der aktive Tab fragt pro Frame in einer `else if`-Kette nacheinander, ob der Pfad "mein Typ" ist; jede Abfrage hasht den Pfad-String (`std::unordered_map<std::string,…>::find`) erneut. Die Information ist ein einziger `AssetType`-Wert.
- **Code-Auszug**:
```cpp
else if (MaterialEditorPanel::isMaterialAsset(tabPath) || MaterialEditorPanel::isMaterialFunctionAsset(tabPath)) ...
else if (UIEditorPanel::isWidgetAsset(tabPath)) ...
else if (HorizonCodeClassPanel::isClassAsset(tabPath)) ...
```
- **Lösung**: Einmal `const auto type = EditorAssetTypeCache::assetTypeOf(tabPath);` und `switch (type)` (oder eine Tabelle `AssetType → render-Funktion`). Die `isXAsset`-Funktionen bleiben für andere Aufrufer erhalten.
- **Erwarteter Effekt**: Schätzung: bis zu ~18 Hashes/Lookups pro Frame weniger (Sub-µs-Bereich); überwiegend Lesbarkeit.
- **Risiko / Verifikation**: Reihenfolge der Kette darf sich nicht ändern, wo ein Prädikat mehrere Typen akzeptiert (z. B. `isMaterialAsset || isMaterialFunctionAsset`, Partikel-Graph); Aufruf aus `EditorUI.cpp` liegt außerhalb dieses Slice.

### EDT-507 – `EditorCamera`: Basisvektoren werden mehrfach neu berechnet; dreifach wiederholter Bewegungs-Check
- **ID**: EDT-507
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorCamera.cpp:14-41`, `146-204`, `221-226`, `98-116`
- **Problem**: `forward()` (4 Trig-Aufrufe + `normalize`) wird von `right()`, `up()`, `viewMatrix()` jeweils neu aufgerufen: `viewMatrix()` rechnet `forward()` dreimal und `upReference()` zweimal (`up()` → `right()` → `forward()`). `update()` ruft bei Pan `right()` und `up()` (also `forward()` dreimal), bei Fly-Move zusätzlich `forward()` direkt. `currentPreset()` rechnet für jede der 6 Presets erneut `sin`/`cos`. Außerdem steht `in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f` dreimal in `update()`.
- **Code-Auszug**:
```cpp
return glm::lookAt(m_position, m_position + forward(), up());
...
if (in.orbit && (in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f))
if (in.look  && (in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f))
if (in.pan   && (in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f))
```
- **Lösung**: `struct Basis { vec3 f, r, u; }` und `Basis basis() const` (eine `forward()`-Berechnung); `viewMatrix`, Pan und Fly-Move nutzen `basis()`. In `update()` `const bool moved = in.mouseDelta != glm::vec2(0.0f);`. `currentPreset()` bleibt wegen Yaw-Wrap (Back: yaw = π vs. −π) beim Vektorvergleich; die Presets könnten als `constexpr`-Tabelle der Vorwärtsvektoren vorliegen.
- **Erwarteter Effekt**: Schätzung: wenige hundert ns pro Frame; reine Aufräumarbeit, kein messbarer Gewinn zu erwarten.
- **Risiko / Verifikation**: Float-Ergebnisse bitgleich, solange dieselben Ausdrücke verwendet werden; Tests für die Kamera/Presets (`tests/`, Suche `EditorCamera`) laufen lassen.

### EDT-508 – `EditorApplication.h`: Icon-Liste vierfach gepflegt, schwerer Header in 81 Dateien
- **ID**: EDT-508
- **Kategorie**: F / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `EditorApplication.h:475-497` (`AppContext::CbIcons`), `1383-1405` (`m_icon*`), `EditorApplication.cpp:1712` (Ladetabelle), `12372` (Kopie in `makeContext`); Includes `EditorApplication.h:2-59`
- **Problem**: Jeder neue Asset-Typ-Icon braucht Einträge an vier Stellen (Struct-Feld, Member, Ladeliste, Kopie in `makeContext`); 20 `ImTextureID`-Paare sind derselbe Datensatz zweimal. Der Header zieht ca. 45 Includes (`HorizonScene/*`, `ContentSync`, `Net`, `Mcp*`, `GitController`, …) nach, und wird in 81 Dateien (`grep -l`) des Editors eingebunden; jede Änderung an einem dieser Header kompiliert den halben Editor neu.
- **Code-Auszug**:
```cpp
struct CbIcons { ImTextureID folder = 0; ImTextureID material = 0; ... } cbIcons;   // AppContext
...
ImTextureID m_iconFolder = 0; ImTextureID m_iconMaterial = 0; ...                   // EditorApplication
```
- **Lösung**: `CbIcons` als frei stehendes Struct definieren und `EditorApplication` genau eine Instanz `m_cbIcons` halten; `AppContext` bekommt eine Referenz/Kopie (`ctx.cbIcons = m_cbIcons`) statt 20 Einzelkopien; Ladetabelle als `{ "Folder.png", &m_cbIcons.folder }`. Für die Includes: `AppContext` und `EditorConfig`-artige Daten in eigene Header (`AppContext.h`), die Panels nur diese einbinden; Mitglieder wie `m_gameServicesBinding`, `m_netServices` etc. hinter ein Pimpl.
- **Erwarteter Effekt**: Schätzung: spürbar kürzere inkrementelle Builds, 40–60 Zeilen weniger Pflegecode; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Rein mechanisch; Build aller Ziele (`HorizonEditor`, `he_tests`) reicht. Das Pimpl-Vorhaben ist größer und sollte separat geplant werden.

### EDT-509 – Veraltete/verrutschte Kommentare und Kodierungsfehler in `EditorApplication.h`
- **ID**: EDT-509
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `EditorApplication.h:36`, `665-677`, `832-836`, `EditorApplication.cpp:10218-10221`
- **Problem**: (a) Zeile 36 enthält Mojibake: `// the multiplayer session (plan Â§5.6)` (UTF-8 als Latin-1 gelesen/gespeichert; gemeint `§5.6`). (b) Der Kommentarblock "Per-project open-tab persistence … restoreOpenTabs runs on project load; saveOpenTabs …" (665-667) steht vor `applyAssetBytes` und gehört zu `saveOpenTabs`/`restoreOpenTabs` (676-677). (c) Der Kommentar "The local MCP listener. Pumped once per frame…" (832-835) steht über `m_mcpCameras`, der Member `m_mcp` folgt erst Zeile 861. (d) `m_hubProjectName/m_hubProjectDir` werden als Roh-`char*` + Größe durch `AppContext` gereicht (519-522), obwohl `std::string` genügte (nur Hub-Dialog, daher Niedrig).
- **Code-Auszug**:
```cpp
#include <HorizonScene/Net/NetGameSession.h>        // the multiplayer session (plan Â§5.6)
```
- **Lösung**: Zeichen korrigieren, Kommentarblöcke an die zugehörigen Deklarationen verschieben, toten `root`/`(void)root` entfernen.
- **Erwarteter Effekt**: Nur Lesbarkeit.
- **Risiko / Verifikation**: Keines (Kommentare). Datei als UTF-8 speichern.

### EDT-510 – Guides: eager gebauter, doppelt gehaltener Text und Mehrfachkopien beim Seitenbau; ungenutztes `numbers()`
- **ID**: EDT-510
- **Kategorie**: C / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/EditorGuides.cpp:57-71`, `101-110`, `150-197`, `588-616`; `EditorGuides.h:102`; Aufruf `DocsPanel.cpp:240`
- **Problem**: (a) `install()` baut beim Laden der Docs alle vier Tracks (~1 700 Zeilen Inhalt in `Guides/*.cpp`) sofort; `section()` legt zusätzlich den gesamten Suchtext (`s.text`) als zweite Kopie jedes Strings an. Das ist Einmalarbeit, aber der Speicher ist dauerhaft doppelt belegt. (b) `bullets({...})`, `table(..., {{...}})` und `steps({...})` nehmen `std::vector<std::string>`/`initializer_list`-Elemente, die `const` sind: jede Literal-Zeichenkette wird beim Aufbau mehrfach kopiert (Literal → `std::string` in der Initializer-Liste → Kopie in den Vektor → Move in die Zelle). Nur Einmalkosten beim Docs-Laden. (c) `Guides::numbers()` (`EditorGuides.cpp:79-82`, `.h:102`) hat keinen Aufrufer (kein `numbers({` im Repo außer dem unabhängigen `McpToolsApi::numbers`).
- **Code-Auszug**:
```cpp
Docs::Block numbers(std::vector<std::string> items)
{
    return list(Docs::BlockKind::Numbers, std::move(items));
}
```
- **Lösung**: (a) Suchtext lazy beim ersten Suchen bauen, oder `Section::text` ganz weglassen und die Suche über die Blöcke laufen lassen (nur wenn der Speicher stört). (b) Nur bei nachgewiesenem Startzeit-Beitrag ändern. (c) `numbers()` entfernen oder bewusst als API für die Content-Tracks stehen lassen (Header-Kommentar nennt es Teil der "whole surface a track needs").
- **Erwarteter Effekt**: Schätzung: wenige hundert KB Speicher und < 10 ms einmalig. **Messen**, bevor angefasst wird.
- **Risiko / Verifikation**: Die Suche ist getestet (`tests/test_editor_help.cpp:900,970` ruft `Guides::install` und prüft Seiten-IDs); jede Änderung an `Section::text` braucht einen Test, dass Callout-/Flow-Texte weiter gefunden werden (das ist die dokumentierte Falle des Moduls).

### EDT-511 – `EditorDockState`: Haltezeit in Frames statt Zeit; `isGraphicsRow` baut pro Aufruf temporäre Liste
- **ID**: EDT-511
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorDockState.cpp:54-57,158`; `src/HE_Editor/EditorGraphics.cpp:55-62`
- **Problem**: (a) `kHoldMaxFrames = 60 * 90` ("90 Sekunden") ist frameabhängig: bei unbegrenzter FPS (vsync aus) endet die Haltezeit nach wenigen Sekunden, bei 30 FPS nach 3 Minuten; Kommentar und Verhalten weichen dann ab. Gleiches gilt für `kHoldSettleFrames = 12`. (b) `isGraphicsRow` (wird in `EditorSettingsPanel.cpp:244` pro gezeichneter Zeile aufgerufen) legt pro Aufruf eine `initializer_list<RowSet>` an und vergleicht bis zu 11 Strings per `strcmp`; ohne Allokation, aber unnötig kompliziert gegenüber einem `constexpr`-Array der Keys.
- **Code-Auszug**:
```cpp
constexpr int kHoldMaxFrames = 60 * 90;
...
for (const RowSet& r : { qualityRows(), postProcessingRows(), reflectionsAndGiRows() })
```
- **Lösung**: (a) Zeitbasis über `ImGui::GetTime()` (`s_heldSince`) statt Frame-Zähler; `kHoldSettleFrames` bleibt frameweise (es geht um ImGui-interne Fokus-Latenz in Frames). (b) Ein gemeinsames `constexpr std::array<const char*, 11> kAllRows` und `std::any_of`; die drei `RowSet`-Funktionen als Slices darauf.
- **Erwarteter Effekt**: Verhaltenskonstanz der Haltezeit über FPS-Werte; (b) vernachlässigbar.
- **Risiko / Verifikation**: `tests/test_editor_dock_state.cpp` und `tests/test_editor_graphics.cpp` treiben beides headless; für (a) Test mit simulierter Zeit statt Frames anpassen (der Test arbeitet vermutlich mit Frame-Zählern: vorher prüfen).

## Gelesen
Alle 16 Dateien des Slice vollständig gelesen (EditorApplication.h in Blöcken bis Zeile 1462); die Verifikation von Aufrufern erfolgte nur über gezielte grep-/Ausschnitt-Lesungen in EditorApplication.cpp, EditorUI.cpp, TaskBoardPanel.cpp, ContentBrowserPanel.cpp, DocsPanel.cpp (nicht vollständig gelesen, da außerhalb des Slice).

## Editor Slice 6 – EditorHelp, EditorInput(+Mac), EditorMarquee, EditorMultiEdit, EditorPanelState, EditorReference

## Zusammenfassung
Der Slice ist insgesamt sauber und gut kommentiert. `EditorHelp.cpp` besteht zu ~9350 von 9800 Zeilen aus reinen Daten (1776 `Entry`, 155 `Area`, 16 `PanelTopic`, 41 Komponenten-Scopes); der Code-Teil (ab Zeile 9618) ist klein und bereits einmal auf einen Hash-Index umgestellt (`findKey`, Zeilen 9634-9651). Lookups sind hover-gated (`helpForLabel`/`helpForKey`), es gibt keine doppelten Schlüssel (per Skript geprüft: 1776 eindeutig, alle Einträge haben eine `Area`). Die relevanten Funde liegen bei `EditorMultiEdit` (Vollserialisierung pro Drag-Frame und pro Member, Row-Marks/Summary jeden Frame neu gebaut) und bei `EditorInput::detectTrackpad` (SDL-Aufruf mit Allokation pro Frame). Dazu ein sichtbarer Defekt in `EditorReference` (`##wx`-Suffix landet im Seitentitel). Insgesamt 12 Funde: 0 Hoch, 4 Mittel, 8 Niedrig.

## Funde

### EDT-601 – Multi-Edit serialisiert pro Drag-Frame die ganze Entity N+2 mal
- **ID**: EDT-601
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorMultiEdit.cpp:10-19`, `:75-104`; Aufrufer `src/HE_Editor/InspectorPanel.cpp:276-279`, `:326-327`
- **Problem**: `state()` serialisiert JEDE Komponente der Entity per `SceneSerializer` nach CBOR und parst sie sofort wieder zu `json`. Bei einem Drag (Maus gedrückt, `inputMayEditThisFrame()`) passiert das jeden Frame für den Primary zweimal (vor/nach den Widgets) und in `propagate()` zusätzlich einmal pro Member (`state(world, member)`), nur um zu prüfen, ob Komponente und Pfad existieren. Bei Strg+A in einem großen Level (Kommentar InspectorPanel:141-148 nennt "tausend") sind das ~1000 Voll-Serialisierungen samt JSON-Parse pro Frame, solange ein Wert gezogen wird. Zusätzlich wird eine `SceneSerializer`-Instanz pro Aufruf angelegt (Kosten der Konstruktion unsicher).
- **Code-Auszug**:
```cpp
for (Entity member : members) {
    ...
    json mine = state(world, member);          // ALLE Komponenten, CBOR -> json
    json patch = json::object();
    for (const Change& c : changes) {
        const auto comp = mine.find(c.component);
        if (comp == mine.end()) continue;
        if (!comp->contains(c.path)) continue;
        (*comp)[c.path] = c.value;
        patch[c.component] = *comp;
    }
```
- **Lösung**: (1) `propagate` nur dann ausführen, wenn `changes` nicht leer ist (ist bereits so) und die Member-Zustände nur für die betroffenen Komponenten holen: eine `state(world, e, componentNames)`-Variante, falls `SceneSerializer` einen Komponentenfilter anbietet oder ergänzt bekommt (Cross-Modul, siehe unten). (2) Während des Drags `propagate` höchstens einmal pro Frame, aber die "after"-Aufnahme des Primary nur wenn der Frame tatsächlich ein Widget geändert hat (ImGui-`changed`-Rückgabe von `renderFor`) statt bei jedem Frame mit gedrückter Maus. (3) Alternativ nur beim Loslassen anwenden und während des Drags auf einem Teil der Member vorschauen (Verhaltensänderung, daher nicht ohne Abstimmung).
- **Erwarteter Effekt**: (Schätzung) bei 100+ gewählten Entities spürbar flüssigeres Ziehen im Details-Panel; bei 1-5 Entities kaum messbar. Messen: Frame-Zeit beim Ziehen mit 200/1000 Members.
- **Risiko / Verifikation**: Funktioniert nur, wenn die Teil-Serialisierung dasselbe JSON liefert wie `state()`; `tests/test_editor_multi_edit.cpp` deckt `diff`/`propagate` ab, ergänzen um Fall "Member ohne Komponente / kürzeres Array". Die "changed"-Variante (2) ändert Verhalten, wenn ein Widget den Wert ohne `changed`-Flag modifiziert (z. B. Gizmo-Buttons, Strukturaktionen) – daher mit `structural` aus `renderFor` kombinieren.

### EDT-602 – `rowMarks`/`describe` werden jeden Frame für jede gemeinsame Komponente neu gebaut
- **ID**: EDT-602
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorMultiEdit.cpp:110-117`, `:183-198`, `:200-248`; Aufrufer `src/HE_Editor/InspectorPanel.cpp:292-302`
- **Problem**: `s_multi.mixed` wird sauber gecacht (Refresh alle 30/240 Frames), aber die daraus abgeleiteten Strukturen nicht: pro Frame und pro Komponente läuft `rowMarks` (Neuaufbau einer `unordered_map<string,unsigned>`) und `describe` (Neuaufbau eines Strings samt `spelled`/`elementName`-Temporären). Beide rufen für jeden `Mixed` der betreffenden Komponente `tokensOf()` auf (kopiert den `json_pointer`, baut ein `vector<string>` durch `back()/pop_back()` + `reverse`), und beide iterieren pro Komponente über die komplette `mixed`-Liste (O(Komponenten × Leaves) pro Frame). Alles ist reine Funktion von (`mixed`, `component`) und ändert sich nur beim Refresh.
- **Code-Auszug**:
```cpp
const Row::MixedScope mixedRows(
    key ? EditorMultiEdit::rowMarks(s_multi.mixed, key)
        : std::unordered_map<std::string, unsigned>{});
...
const std::string differs = EditorMultiEdit::describe(s_multi.mixed, key);
```
- **Lösung**: Im `MultiSelectionCache` pro Komponente ein Paar `{marks, differs}` ablegen, beim Refresh (`refreshMultiSelection`) einmal aus `c.mixed` berechnen (`std::unordered_map<std::string, Cached>`), und im Frame nur noch nachschlagen. `MixedScope` auf eine Referenz auf die gecachte Map umstellen statt Kopie/Move einer frischen Map.
- **Erwarteter Effekt**: (Schätzung) pro Frame mehrere hundert kleine Allokationen weniger bei vielen abweichenden Feldern; bei wenigen `mixed`-Leaves vernachlässigbar.
- **Risiko / Verifikation**: Gering; Ergebnis ist identisch, solange der Cache bei `stale`/Revision/Periode zusammen mit `mixed` invalidiert wird (derselbe Pfad). Bestehende Tests für `rowMarks`/`describe` unverändert nutzbar.

### EDT-603 – `detectTrackpad()` ruft auf Win/Linux jeden Frame `SDL_GetTouchDevices` auf (Heap-Allokation + Lock)
- **ID**: EDT-603
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorInput.cpp:9-45`, `:59-63`; Aufrufer `src/HE_Editor/EditorUI.cpp:605` (jeden Frame) plus 7 weitere Stellen (`ViewportPanel.cpp:1150`, `EditorViewportNav.cpp:82`, `StaticMeshEditorPanel.cpp:168/515/548`, `AudioEditorPanel.cpp:1185/1203`)
- **Problem**: Auf einem Desktop ohne Akku und ohne Touch-Gerät (der Normalfall bei Windows/Linux-Workstations) bleibt `sawTrackpad` dauerhaft `false`, also läuft die Probe bei JEDEM `trackpadPointer`-Aufruf neu: `SDL_GetTouchDevices` liefert ein frisch allokiertes Array (`SDL_free` danach). Das passiert mindestens einmal pro Frame (EditorUI) und zusätzlich in jedem Panel-Aufruf. Der Kommentar rechtfertigt das Dauerprobieren nur für macOS (Lazy-Registrierung beim ersten Touch).
- **Code-Auszug**:
```cpp
static bool sawTrackpad = false;
if (!sawTrackpad)
{
    int count = 0;
    if (SDL_TouchID* ids = SDL_GetTouchDevices(&count))
    { ... SDL_free(ids); }
}
```
- **Lösung**: Die Probe drosseln (z. B. nur wenn sich `SDL_GetTicks()` um >= 1000 ms geändert hat) oder ereignisgesteuert machen (bei `SDL_EVENT_FINGER_DOWN`/`SDL_EVENT_FINGER_MOTION` bzw. Touch-Device-Hotplug einmal neu prüfen). Das Ergebnis des Frames außerdem in `beginFrame()` einmal berechnen und die Panels nur `detail::g_trackpadActive` lesen lassen (siehe `trackpadActive()`); `trackpadPointer(ctx)` müsste dann nur noch `resolveTrackpad` mit dem Cache aufrufen.
- **Erwarteter Effekt**: (Schätzung) ein paar Mikrosekunden und eine Allokation pro Aufruf gespart; klein, aber ein Dauerkostenpunkt ohne Nutzen. Messen mit SDL-Profil unter Windows.
- **Risiko / Verifikation**: Gedrosselte Erkennung verzögert das automatische Umschalten um bis zu eine Sekunde; Test: `resolveTrackpad` ist bereits rein testbar, Drosselung per Hand prüfen (Magic Trackpad anstecken, Auto-Modus).

### EDT-604 – `EditorReference`: "##"-Suffix landet im Seitentitel (sichtbarer Defekt)
- **ID**: EDT-604
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorReference.cpp:38-47` (labelOf), `:99-101`; betroffene Daten `src/HE_Editor/EditorHelp.cpp:1195`, `:1199`, `:1202`, `:1206`, `:1211`
- **Problem**: `labelOf` nimmt bei leerem `title` den Teil nach dem letzten `/` des Schlüssels – inklusive ImGui-ID-Suffix. Fünf Einträge haben `title == ""` und ein `##wx`-Suffix (`Weather/Fog Density##wx`, `Wind Speed##wx`, `Rain##wx`, `Snow##wx`, `Wetness##wx`); ihre Abschnitte heißen in der generierten Referenz daher "Fog Density##wx" usw., ebenso fließt es in `sec.text` (Suche) ein. Gleiche Behandlung fehlt in `Help::find`-Gegenstück `visible()` (EditorHelp.cpp:9627), das nur im Anonymous-Namespace existiert.
- **Code-Auszug**:
```cpp
const std::size_t slash = key.rfind('/');
return slash == std::string::npos ? key : key.substr(slash + 1);
```
- **Lösung**: Nach dem Slash-Split den Teil ab `##` abschneiden (`label.substr(0, label.find("##"))`) – am besten über eine geteilte Funktion `Help::visibleLabel()` (die vorhandene `visible()` exportieren), damit Lookup und Referenz dieselbe Regel nutzen. Der Anker (`sec.id`) darf das Suffix behalten, er muss mit `referenceTopic` übereinstimmen.
- **Erwarteter Effekt**: Korrekte Titel im Handbuch; keine Performance-Wirkung.
- **Risiko / Verifikation**: Gering. `tests/test_editor_help.cpp` um Prüfung erweitern: kein `sec.title` enthält "##". Anker-Auflösung (F1) bleibt unverändert.

### EDT-605 – `referenceTopic()` wird für jedes angezeigte Tooltip-Frame neu berechnet
- **ID**: EDT-605
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorRowWidgets.cpp:603-607`; `src/HE_Editor/EditorHelp.cpp:9738-9792`
- **Problem**: Solange ein Tooltip sichtbar ist, läuft pro Frame `referenceTopic(e->key)`: `areaOf` scannt linear alle 155 Präfixe (String-View-Vergleich), danach bei Komponenten-Keys zusätzlich bis zu 41 Scopes, dann zwei `std::string`-Allokationen (`anchor`, Rückgabe). Das Ergebnis wird nur gebraucht, wenn F1 gedrückt wird bzw. um den Hinweis "F1 documentation" anzuzeigen.
- **Code-Auszug**:
```cpp
static std::string s_topic;
s_topic = HE::Ed::Help::referenceTopic(e->key);
```
- **Lösung**: Pro Tooltip-Wechsel (Vergleich `e != s_lastQueued`) einmal berechnen und `s_topic` behalten; oder `bool hasReference(key)` ohne String-Bau für die Anzeige und `referenceTopic` erst bei `IsKeyPressed(F1)`.
- **Erwarteter Effekt**: (Schätzung) wenige Mikrosekunden/Frame, nur während ein Tooltip steht. Niedrig.
- **Risiko / Verifikation**: Keines; `s_lastQueued` existiert schon (RowWidgets:475).

### EDT-606 – `findKey` ohne Hover-Gate in Toolbar, `helpMarker` und Asset-Slot (Doppel-Lookup)
- **ID**: EDT-606
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorToolbar.cpp:56`; `src/HE_Editor/EditorRowWidgets.cpp:524-531`; `src/HE_Editor/EditorWidgets.cpp:135-136`
- **Problem**: Drei Stellen hashen jeden Frame den Schlüssel, ohne dass das Item gehovert ist: Toolbar-Zelle (`findKey(helpKey)` pro Zelle), `helpMarker` (jede Marker-Zeile) und der Asset-Slot, der erst `findKey(helpKey)` aufruft und dann `helpForKey(helpKey)` nochmals (zweiter Hash, der erste nur für die Verzweigung). Bei ~60 Zellen und ~25 ns/Hash ist das ~1,5 µs/Frame – das Performance-Audit (Kommentar EditorHelp.cpp:9637-9641) hat den Hauptteil bereits beseitigt.
- **Code-Auszug**:
```cpp
if (helpKey && HE::Ed::Help::findKey(helpKey))
    helpForKey(helpKey);
else if (ImGui::IsItemHovered())
```
- **Lösung**: Asset-Slot: einmal `const Entry* e = findKey(...)` und `helpForEntry(e)`. Toolbar/`helpMarker`: Entry-Zeiger pro Call-Site cachen (`static const Entry* e = findKey("...")`), da die Tabelle konstant ist; oder Lookup erst bei `hovered`, wobei die Weiche "Help- vs. Plain-Tooltip" nur beim Hover relevant ist.
- **Erwarteter Effekt**: Mikrosekunden; reine Konsolidierung. Nur zusammen mit anderem Umbau lohnend.
- **Risiko / Verifikation**: `helpMarker` zeigt "(?)" nur bei existierendem Eintrag – bei Hover-Gating darf diese Entscheidung nicht verschoben werden. Gering.

### EDT-607 – Hash-Index von `findKey` wird lazy beim ersten Hover gebaut; Startkosten ungemessen
- **ID**: EDT-607
- **Kategorie**: D / F
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorHelp.cpp:9643-9648`, Tabelle `:25-9372`
- **Problem**: Die Tabelle `kEntries` ist `constexpr` (71 KB Zeigerdaten in `.data.rel.ro`, mit ~8,9 k Relokationen unter PIE/macOS-Rebase, plus ~370 KB Strings in `.rodata`). Der Index (`unordered_map<string_view, const Entry*>`, 1776 Knoten-Allokationen, jeder Key wird gehasht) entsteht beim ersten `findKey`, also im ersten Frame mit Hover bzw. in der ersten Toolbar-Zelle – ein Mini-Hitch im ersten Frame, plus ~1776 kleine Heap-Knoten über die gesamte Laufzeit. Die Struktur ist ansonsten richtig (kein linearer Scan, `try_emplace` erhält Ersttreffer-Semantik).
- **Code-Auszug**:
```cpp
static const std::unordered_map<std::string_view, const Entry*> s_index = [] {
    std::unordered_map<std::string_view, const Entry*> m;
    m.reserve(std::size(kEntries));
    for (const Entry& e : kEntries) m.try_emplace(e.key, &e);
    return m;
}();
```
- **Lösung**: Nur falls Messung (Startup-Zeitstempel, erster Frame) eine nennenswerte Größe zeigt: statt Hash-Map ein `std::vector<const Entry*>` einmal nach Key sortieren (`stable_sort`, damit der Ersttreffer gewinnt) und per `lower_bound` suchen (1 Allokation, cache-freundlicher), oder den Index zur Build-Zeit erzeugen (gperf/constexpr-Sortierung in einem `consteval`-Block). Alternativ den Index im Editor-Start im Hintergrund-Job aufbauen.
- **Erwarteter Effekt**: (Schätzung, unsicher) Index-Aufbau ~100-300 µs einmalig, ~100 KB Heap weniger bei vector-Variante. Nicht ohne Messung umsetzen.
- **Risiko / Verifikation**: Binärsuche ist pro Lookup langsamer als Hash (log2 1776 ≈ 11 String-Vergleiche), also nur wenn Hover-Gating gilt; `tests/test_editor_help.cpp` prüft Eindeutigkeit/Ersttreffer bereits.

### EDT-608 – `EditorReference::install`: unnötige Temporäre und Duplikat-Strings beim Aufbau
- **ID**: EDT-608
- **Kategorie**: C / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/EditorReference.cpp:52-55` (`whereOf`), `:87-90`, `:99-103`, `:147-148`; Aufruf `src/HE_Editor/DocsPanel.cpp:236`
- **Problem**: (a) Der Komparator der `stable_sort` baut pro Vergleich zwei `std::string(a.area->group)`/`std::string(a.entry->key)`-Temporäre (Gruppen > 15 Zeichen und Keys > 15 Zeichen allokieren): bei 1776 Zeilen O(n log n) Vergleiche. (b) `whereOf(e, area)` ignoriert `e` (ungenutzter Parameter) und gibt nur `area.group` zurück. (c) Der Body liegt dreimal vor (`kEntries`-Literal, `Paragraph`-Run, `sec.text`), `sec.text` hängt zusätzlich title/eyebrow/key an: ~330 KB Body-Text → ~1 MB Heap in der Docs-Library. (d) `pageTitle[a->page] = a->title` wird je Eintrag überschrieben statt je Seite einmal.
- **Code-Auszug**:
```cpp
const int g = std::string(a.area->group).compare(b.area->group);
return g != 0 ? g < 0 : std::string(a.entry->key) < std::string(b.entry->key);
```
- **Lösung**: `std::string_view` im Komparator; `whereOf` entfernen und `area->group` direkt nutzen; `pageTitle.try_emplace`. Für (c) den Suchtext nur aus Titel + Body bilden, falls die Suche sonst nichts braucht (`sec.eyebrow` und Key sind für Treffer beim Nutzer nützlich – Absicht prüfen) oder `Docs::Section::text` lazy beim ersten Suchen füllen.
- **Erwarteter Effekt**: (Schätzung) einmaliger Aufbau beim Öffnen der Docs, ~ms und wenige hundert KB; Niedrig.
- **Risiko / Verifikation**: Sortierreihenfolge muss identisch bleiben (string_view-Vergleich ist lexikografisch wie `std::string`). Tests: `test_editor_help` (Seitenstruktur).

### EDT-609 – Kleine Ineffizienzen in `EditorMultiEdit` (`propagate`, `mixed`, `diffInto`)
- **ID**: EDT-609
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/EditorMultiEdit.cpp:28`, `:78`, `:85-92`, `:175-177`
- **Problem**: (a) `patch[c.component] = *comp;` kopiert den gesamten Komponenten-JSON für JEDEN Change, auch wenn mehrere Changes in dieselbe Komponente gehen (letzte Kopie enthält alle, die früheren sind Verschwendung). (b) `std::find(skip.begin(), skip.end(), member)` pro Member: O(n·|skip|); in einer Collab-Session mit vielen gehaltenen Entities quadratisch. (c) `mixed()` prüft `seen` mit linearem `std::any_of` über das wachsende Ergebnis: O(L²) für L Leaves. (d) `diffInto` beginnt jede Rekursionsebene mit `before == after` (tiefer Vergleich) – für jeden Unterbaum, der sich unterscheidet, wird der Vergleich auf jeder Ebene wiederholt (O(Tiefe·Größe)).
- **Code-Auszug**:
```cpp
(*comp)[c.path] = c.value;
patch[c.component] = *comp;
```
- **Lösung**: (a) Schreibzugriffe auf `*comp` sammeln und `patch[c.component] = std::move(*comp)` nach der Change-Schleife je Komponente einmal setzen (Changes nach Komponente gruppieren oder `patch.contains` prüfen und auf `patch[c.component]` direkt schreiben). (b) `skip` einmal in ein sortiertes Vektor/`unordered_set` kopieren (oder dem Aufrufer erlauben, es sortiert zu liefern). (c) `std::set<std::pair<std::string,std::string>>`/Hash-Set über (component, path.to_string()) oder zuerst sortieren + `unique`; Reihenfolge "erste Sichtung" beachten. (d) Gleichheitstest nur an Blättern, Objekt/Array-Zweig ohne Vorab-Vergleich (Ergebnis identisch, da gleiche Teilbäume keine Changes erzeugen).
- **Erwarteter Effekt**: Nur bei sehr großen Selektionen/Leave-Mengen messbar (Schätzung); sonst Lesbarkeit.
- **Risiko / Verifikation**: (d) ändert die Aufrufzahl, nicht das Ergebnis – `tests/test_editor_multi_edit.cpp` deckt `diff`/`mixed` ab; Reihenfolge der `mixed`-Ausgabe muss erhalten bleiben (Header-Vertrag "in der Reihenfolge des Referenz-Zustands").

### EDT-610 – `EditorHelp`: tote Area-Regeln und handgepflegte Parallellisten
- **ID**: EDT-610
- **Kategorie**: F / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorHelp.cpp:9411`, `:9486`, `:9521`, `:9538`, `:9540`, `:9556`, `:9557`, `:9578`, `:9582` (tote Regeln); `:9604-9615` (`kComponentScopes`); `:9378-9395` (`kPanelTopics`)
- **Problem**: Neun `kAreas`-Präfixe (`"hub."`, `"settings."`, `"material."`, `"input."`, `"hc."`, `"terrain."`, `"env."`, `"profiler."`, `"collab."`) treffen keinen einzigen der 1776 Schlüssel (per Skript über den Tabellen-Quelltext geprüft; im Code sind Strings mit denselben Präfixen nur Settings-Ids (`EditorSettingsCatalog.cpp:96/261`) und HorizonCode-Node-Namen (`HcNodeDocs.cpp`), keine Help-Keys). Sie kosten jeden `areaOf`-Aufruf einen unnötigen Vergleich und wecken den Eindruck, es gäbe diese Einträge. `kComponentScopes` (41 Namen) muss manuell mit den `"<Komponente>/…"`-Keys in sync gehalten werden – eine neue Komponente ohne Eintrag landet in keiner Area (der Test meldet das, aber erst nach dem Build). Zudem baut `areaOf` einen separaten statischen `vector<Area>` (Zeilen 9764-9770) nur um die Scopes in ein `Area`-Format zu bringen.
- **Code-Auszug**:
```cpp
{ "hub.",      "editor-interface", "Editor Interface", "Project Hub" },
...
{ "collab.",   "editor-collab",    "Collaboration & Source Control", "Collaboration" },
```
- **Lösung**: Tote Regeln löschen (nach Gegenprobe `grep` auf `"hub."` usw. in `src/HE_Editor` und in `tests/`). Optional: `kComponentScopes` als `constexpr Area`-Tabelle direkt anlegen statt `const char*[]` + Laufzeit-Vector (ein Datenblock weniger, kein `static`-Init).
- **Erwarteter Effekt**: Weniger Rauschen in Reviews; Mikrovorteil bei `areaOf`.
- **Risiko / Verifikation**: Gering; `tests/test_editor_help.cpp` (prüft Area je Key) muss weiter grün sein. Falls Plugins/Projekte (nicht im Repo) diese Präfixe verwenden, vorher mit dem Team klären.

### EDT-611 – Doppelt gepflegte Help-Texte über Scopes
- **ID**: EDT-611
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorHelp.cpp` – Schlüsselpaare mit byte-gleichem Body: `Viewport Menu/Unlock` = `World Outliner/Unlock`; `Project General/{Show a splash while starting, Splash image, Subtitle, Browse...}` = `Export/…`; `Application/Browse...` = `Export/Browse...` = `Project General/Browse...`; `Application/Icon file` = `Export/Icon file`; `Sequencer/Delete Key` = `UI Timeline/Delete Key`; `UI Variable/Rotation##vdef` = `Script Variable/Rotation##vdef`
- **Problem**: Acht Gruppen identischer Texte werden an mehreren Stellen gepflegt; bei Änderungen driftet eine Kopie (die Datei warnt selbst vor Drift Tooltip/Handbuch). Die Literale werden vom Linker meist zusammengeführt, kosten also keinen Speicher, nur Wartung.
- **Code-Auszug**: (Zitat nicht möglich ohne ganze Einträge; Belege per Duplikat-Suche, siehe Ort)
- **Lösung**: Nur wenn die Texte auch dauerhaft gleich bleiben sollen: `constexpr const char*`-Konstanten im Anonymous-Namespace (`kSplashBody` …) und in beiden Einträgen referenzieren. Alternativ einen Alias-Mechanismus in `find()` (Scope A fällt auf Scope B zurück) – mehr Komplexität, nicht empfohlen.
- **Erwarteter Effekt**: Pflege; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Gering; Wortlaut-Regressionen werden von `test_editor_help` nicht erfasst, daher beim Zusammenführen Text exakt belassen.

### EDT-612 – `scopes()` ist ein prozessweiter Vektor, `find()` nutzt aber `thread_local` für den Puffer
- **ID**: EDT-612
- **Kategorie**: E / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorHelp.cpp:9620-9624`, `:9667`
- **Problem**: Der Kommentar bei `thread_local std::string scoped` begründet, dass ein Lookup "von anderswo als dem UI-Thread" den Puffer nicht überschreiben soll. Der Scope-Stack, den derselbe Aufruf liest, ist aber ein gewöhnliches `static std::vector` ohne Synchronisation: ein Lookup aus einem Nicht-UI-Thread (z. B. MCP-Bridge, Report-Dialog-Worker) während der UI-Thread `Scope` pusht/poppt wäre ein Data Race. Die Absicht des Kommentars wird so nur halb erfüllt.
- **Code-Auszug**:
```cpp
std::vector<const char*>& scopes()
{
    static std::vector<const char*> s;
    return s;
}
```
- **Lösung**: Entweder `static thread_local` (jeder Thread hat seinen Stack, Lookups ohne Scope fallen auf unscoped zurück – bisheriges Verhalten im UI-Thread unverändert) oder den Kommentar anpassen ("UI-Thread only", wie `EditorPanelState.h` es schreibt) und das `thread_local` am Puffer streichen. Ein Gegencheck, ob irgendwo außerhalb des UI-Threads `Help::find` aufgerufen wird, steht aus (311 Aufrufstellen in `src/HE_Editor`, nicht einzeln geprüft).
- **Erwarteter Effekt**: Konsistenz/Klarheit; kein Performance-Effekt (`thread_local` bei `scopes()` kostet einen TLS-Zugriff pro Lookup).
- **Risiko / Verifikation**: `thread_local` verändert Verhalten, wenn ein Hintergrund-Thread bisher absichtlich die Scopes des UI-Threads gelesen hat (unwahrscheinlich). Test: vorhandene UI-Tests (`test_*_ui.cpp`).

## Gelesen
Ja, vollständig gelesen: `EditorHelp.h`, `EditorInput.h/.cpp`, `EditorInputMac.mm`, `EditorMarquee.h/.cpp`, `EditorMultiEdit.h/.cpp`, `EditorPanelState.h/.cpp`, `EditorReference.h/.cpp` sowie der Code-Teil von `EditorHelp.cpp` (Zeilen 1-40 und ab 9372 bis Ende, d. h. Lookup, Scope, Area, PanelTopic). Von den Dateneinträgen von `EditorHelp.cpp` habe ich Zeilen 1-1960 gelesen; Zeilen 1960-9372 (reine `Entry`/`Area`-Datenzeilen) habe ich nur skriptgestützt ausgewertet (Eindeutigkeit der Keys, Area-Abdeckung, Duplikat-Bodies, Titel mit "##", Textlängen, tote Präfixe), nicht Zeile für Zeile gelesen.

## Editor Slice 7 – EditorRewards … EditorTransformGizmo (src/HE_Editor)

Dateien: EditorRewards.{h,cpp}, EditorRowWidgets.cpp, EditorSelection.{h,cpp}, EditorSelfUpdate.{h,cpp}, EditorSettingsCatalog.{h,cpp}, EditorSettingsPanel.{h,cpp}, EditorShortcuts.{h,cpp}, EditorSystemMotion.{h,cpp,mm}, EditorTabs.{h,cpp}, EditorTheme.{h,cpp}, EditorToolbar.{h,cpp}, EditorTransformGizmo.{h,cpp}.

## Zusammenfassung

Der Slice ist insgesamt sauber und gut kommentiert: die offensichtlichen Hot-Path-Fallen sind schon behandelt (Hover-Gate vor der Help-Suche in `EditorRowWidgets`, `favoritesContain` ohne Allokation, Cache der Reduce-Motion-Abfrage, einmaliger `capturePre()` pro Drag). Der einzige echte Skalierungsfehler ist die Selektionslogik (`EditorSelection::roots/contains/addMany`, O(N²) und `roots()` läuft pro Frame im Viewport). Danach folgen die Doppelpflege Settings-Katalog gegen Settings-Panel (Wertebereiche sind bereits gedriftet, Restore Defaults ist eine handgepflegte, lückenhafte Liste), der Gruppen-Gizmo mit pro-Frame-Aufbau über alle Mitglieder, der Footer-Status mit Strings/Vektoren pro Frame und die Shortcut-Lookups über String-IDs. Der Rest sind Kleinigkeiten. Funde: 1 Hoch, 5 Mittel, 6 Niedrig. Messdaten liegen nicht vor, alle Zahlen sind Schätzungen.

---

### EDT-701 – Selektion: `roots()`/`addMany()` sind O(N²), `roots()` läuft pro Frame
- **ID**: EDT-701
- **Kategorie**: D
- **Schweregrad**: Hoch (bei großen Selektionen, z. B. Select All; sonst unauffällig)
- **Typ**: Performance
- **Ort**: `EditorSelection.cpp:75-97` (roots), `:99-102` (contains), `:55-63` (addMany), `:48-53` (setMany). Aufrufer pro Frame: `ViewportPanel.cpp:1535` (`ctx.selection.roots(...)` vor dem Gizmo), `EditorApplication.cpp:12011` (`prune`, pro Frame in `makeContext`).
- **Problem**: `contains()` ist eine lineare Suche über `m_entities`. `roots()` ruft sie für jeden Vorfahren jedes Mitglieds auf (`if (contains(p))`), also O(N · Tiefe · N). Der Viewport ruft `roots()` jeden Frame auf, sobald etwas selektiert ist, und danach noch `erase(remove_if(...))` auf einem frisch allokierten Vektor. Edit ▸ Select All (`EditorUI.cpp:1845`, `ViewportActions::selectAll`) erzeugt Selektionen mit allen Entities. Bei 5 000 Entities mit Hierarchietiefe ~3 sind das größenordnungsmäßig 50–75 Mio. Vergleiche pro Frame (Schätzung, messen). `addMany()` ruft `contains(e)` je Element und ist ebenfalls O(N²), dort aber nur einmalig pro Geste (Hitch beim Select All / Shift-Range).
- **Code-Auszug**:
```cpp
for (Entity e : m_entities) {
    ...
    for (int steps = 0; steps < kMaxDepth; ++steps) {
        ...
        p = h->parent;
        if (contains(p)) { covered = true; break; }   // linear in N
    }
```
```cpp
for (Entity e : range) {
    if (e == entt::null || contains(e)) continue;     // linear in N
    m_entities.push_back(e);
}
```
- **Lösung**:
  1. In `roots()` einmal pro Aufruf ein Lookup bauen und dort fragen: `std::unordered_set<Entity> sel(m_entities.begin(), m_entities.end());` (oder bei kleinem N weiter linear: `if (m_entities.size() <= 16)` Schnellpfad), dann `sel.count(p)`. Komplexität O(N · Tiefe).
  2. `addMany`/`setMany` genauso: lokales Set der vorhandenen Mitglieder, nur neue anhängen. `setMany` braucht das `++m_revision` danach nicht mehr doppelt (der Kommentar "harmless" deutet es an); eine Erhöhung genügt.
  3. Optional: das Set als Member halten und in allen Mutatoren pflegen, dann ist auch `contains()` (vom Outliner pro sichtbarer Zeile genutzt) O(1). Das ist invasiver; Schritt 1+2 reichen.
  4. Optional: das Ergebnis von `roots()` im Viewport pro Selektions-Revision cachen; Vorsicht, Hierarchie-Änderungen ändern `m_revision` nicht, der Cache müsste also auch auf Reparenting reagieren. Ohne Messung nicht empfohlen.
- **Erwarteter Effekt**: Schätzung: Selektion mit einigen tausend Einträgen von zweistelligen Millisekunden pro Frame auf unter 1 ms; kleine Selektionen unverändert.
- **Risiko / Verifikation**: Reihenfolge der Ausgabe (Selektionsreihenfolge) und die Zyklus-Kappung `kMaxDepth` müssen erhalten bleiben. `tests/test_editor_selection.cpp` deckt `roots` ("leaves out a member whose ancestor is selected") und Ranges ab. Ergänzen: Test mit 5 000 Mitgliedern und tiefer Kette plus ein Zyklus-Fall; Revisionszähler-Erwartung für `setMany` prüfen (Tests dürfen nicht auf den doppelten Increment bauen).

---

### EDT-702 – Gruppen-Gizmo: pro Frame O(N·Tiefe) Neuaufbau, pro Drag-Frame N Inversen
- **ID**: EDT-702
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `EditorTransformGizmo.cpp:223-261` (Neuaufbau ohne Drag), `:302-316` (Anwenden pro Member), `:339-361` (`movable`-Vektor), `:71-78` (`parentWorldOf`).
- **Problem**: Solange eine Mehrfachselektion existiert und nicht gezogen wird, läuft jeden Frame für jedes Mitglied `HE::worldMatrixOf(world, e)` (komponiert die Hierarchie bis zur Wurzel, laut Kommentar bewusst nicht aus dem Cache gelesen) nur für den Zentroid. Dazu kommt in `manipulate(...)` pro Frame ein neuer `movable`-Vektor, und im ViewportPanel noch `roots()` plus ein weiterer Vektor. Während des Drags wird für jedes Mitglied jeden Frame `parentWorldOf` (wieder eine Hierarchiekomposition) berechnet und `glm::inverse` (allgemeine 4x4-Inverse) darauf angewendet, obwohl sich die Eltern der Wurzel-Mitglieder während des Drags nicht ändern (Mitglieder, deren Vorfahre selektiert ist, sind per `roots()` ausgeschlossen).
- **Code-Auszug**:
```cpp
for (const Entity e : members)
    if (registry.all_of<TransformComponent>(e)) {
        primaryWorld = HE::worldMatrixOf(world, e);   // jeden Frame, N mal
        centroid += glm::vec3(primaryWorld[3]);
```
```cpp
const glm::mat4 local = glm::inverse(parentWorldOf(world, m.entity)) * worldNow;   // jeden Drag-Frame, N mal
```
- **Lösung**:
  1. `GroupMember` um `glm::mat4 invParentStart` erweitern, beim Drag-Start (Zeile 272-278) einmal füllen; im Drag-Loop nur noch multiplizieren. Spart N Hierarchiekompositionen und N Inversen pro Frame (Verhalten identisch, solange kein Elternteil während des Drags von außen bewegt wird; ein Peer-Edit mitten im Drag wäre der einzige Unterschied, vorher wurde er "live" nachgeführt).
  2. Den Zentroid im Nicht-Drag-Fall nur neu berechnen, wenn sich Selektion oder eine Transform geändert hat (Revision der Selektion + Dirty-Flag der Mitglieder). Ohne Messung zunächst nur Schritt 1.
  3. `movable` als wiederverwendeten `static thread_local std::vector<Entity>` oder Member halten statt pro Frame zu allokieren (Niedrig).
- **Erwarteter Effekt**: Schätzung: bei 1 000 Mitgliedern Drag-Frame von mehreren ms auf unter 1 ms. Nicht-Drag-Frames: nur bei großen Gruppen messbar.
- **Risiko / Verifikation**: Das Verhalten bei sich während des Drags ändernden Elternmatrizen (Collab, Skripte) ändert sich; entscheiden, ob das gewollt ist. Tests: `tests/test_viewport_gizmo.cpp`, `tests/test_gizmo_pick.cpp` (Gruppen-Drag mit verschachtelter Hierarchie ergänzen und gegen die bisherige Ausgabe vergleichen).

---

### EDT-703 – Settings-Katalog und Settings-Panel sind zwei Wahrheiten; Wertebereiche sind bereits gedriftet
- **ID**: EDT-703
- **Kategorie**: A (Duplikation) / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `EditorSettingsCatalog.cpp:66-463` gegen `EditorSettingsPanel.cpp:303-949`; Behauptung im Header `EditorSettingsCatalog.h:10-11` und `McpToolRegistry.h:1370-1371` ("the ranges the widgets enforce by construction").
- **Problem**: Jede Einstellung ist doppelt beschrieben (Katalog für MCP, Panel für die UI) und ein drittes Mal in `EditorHelp.cpp` plus die Hilfetexte im Katalog. Die Bereiche widersprechen sich bereits: die MCP-Seite lässt Werte zu, die die UI-Slider nie erlauben, und die Doku behauptet das Gegenteil. Beispiele (Katalog / Panel): Render Scale 0.25–2.0 (`:354-356`) / 0.5–2.0 (`Panel:383`), Bloom Threshold 0–10 / 0–4 (`:365`, `Panel:416`), AO Radius 0.01–10 / 0.05–2 (`:373`, `:425`), AO Intensity 0–1 / 0–2 (`:376`, `:426`), DoF Focus Distance 0.1–1000 / 0.1–200 (`:385`, `:438`), Focus Range 0–1000 / 0–100, Camera Speed 0.1–100 / 1–50 (`:86`, `Panel:868`), Stick/Trigger Deadzone 0–0.9 / 0–0.5 (`:96-101`, `Panel:901-902`), SSR Intensity 0–2 / 0–1 (`:407`, `Panel:475`), GI Light Radius 0–10 / 0.05–3 (`:426`, `Panel:499`), GI Indirect Intensity 0–5 / 0–3 (`:423`, `Panel:498`), Content-Browser-Refresh 1–600 / ≥0 (`:108`, `Panel:948`). Auch inhaltlich: Katalog "Keep CPU Assets … saves a re-read" (`:104-107`) gegen Panel "Not implemented — CPU asset data is always kept" (`Panel:935-944`), Render Scale ohne Metal-Hinweis im Katalog.
- **Code-Auszug**:
```cpp
// Katalog
t.push_back(floatRow("postProcess.bloomThreshold", "Bloom Threshold", ..., 0.0, 10.0, ...));
// Panel
Row::sliderFloat("Bloom Threshold", &cfg.BloomThreshold, 0.0f, 4.0f, "%.2f");
```
- **Lösung**:
  1. Katalog als einzige Quelle der Bereiche/Labels/Hilfetexte behandeln: im Panel eine kleine Hilfe `const SettingDesc& S(const char* key)` und `Row::sliderFloat(S("postProcess.bloomThreshold").label, &cfg.BloomThreshold, S(...).minValue, S(...).maxValue, "%.2f")`. Dadurch verschwinden die Literale im Panel; Zeilen mit Backend-Gates bleiben im Panel.
  2. Alternativ (kleiner): nur einen Test ergänzen, der alle Katalog-Bereiche gegen die im Panel verwendeten prüft (z. B. über einen Test-Hook, der die Slider-Aufrufe aufzeichnet).
  3. Vorher entscheiden, welche Seite recht hat (die Panel-Werte sind die geprüften UI-Grenzen, die Katalog-Werte sind vermutlich "großzügig geraten").
- **Erwarteter Effekt**: keine Laufzeit; weniger Pflegeaufwand und ein ehrlicher MCP-Vertrag. Löst das Muster aus `EditorRewards.h:383-390` ("in six places") zu einem Teil.
- **Risiko / Verifikation**: Strengere MCP-Bereiche brechen bestehende Clients, die Werte jenseits der UI-Grenzen setzen (`tests/test_mcp_tools_settings.cpp` "out-of-range number is refused" prüfen). Hilfetexte stehen im Audit-Skript `scripts/editor_help_audit.py --check`; Label-Änderungen müssen dort bestehen.

---

### EDT-704 – "Restore Defaults" ist eine handgepflegte, lückenhafte Kopie der Defaults
- **ID**: EDT-704
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `EditorSettingsPanel.cpp:2405-2480`
- **Problem**: ~45 Zuweisungen mit hartkodierten Werten (`UiFontScale = 1.0f`, `EditorCameraSpeed = 6.0f`, `CollabMaxAssetMB = 64`, …), die `EditorConfig.h` spiegeln und bei jedem neuen Feld nachgezogen werden müssen. Der Button steht auf jeder Settings-Seite (auch Viewport Quality, Editor Renderer), setzt aber u. a. `RenderPath`, `OcclusionCulling`, `AntiAliasing`, `AASharpness`, `RenderScale`, `SpecularAA*`, DoF, Motion Blur, SSR, GI/GI-Refl, Gamepad-Deadzones, `PointerInput`, `MaxFps` nicht zurück, obwohl diese auf den Seiten stehen, auf denen der Button erscheint. Ob das Absicht ist, ist nicht dokumentiert (unsicher). Zusätzlich: `EditorConfig.h` ist die eigentliche Default-Quelle (z. B. `AntiAliasing = 1`, `RenderScale = 1.0f`).
- **Code-Auszug**:
```cpp
cfg.UiFontScale       = 1.0f;
cfg.EditorCameraSpeed = 6.0f;
...
cfg.BloomThreshold    = 1.0f;
```
- **Lösung**: Defaults aus einer frischen `EditorConfig const defaults{};` lesen und über den Katalog (`SettingDesc::pb/pi/pf/ps`, Zeilen mit `row == seitenzeilen`) zurücksetzen: `for (d : editorSettingCatalog()) if (d.storage==Config && d.writable && onThisPage(d)) copyMember(cfg, defaults, d);`. Sonderfälle (`CollabSyncLargeAssets` bei gesperrter Session, `McpServerEnabled=false`, `CollabRelayAddress.clear()`, Kamera-Flugspeed anwenden) bleiben als wenige explizite Zeilen. Das beseitigt auch die Doppelpflege mit `EditorConfig.h`.
- **Erwarteter Effekt**: keine Laufzeit; ~50 Zeilen weniger, kein Driften mehr, und die Seiten verhalten sich konsistent.
- **Risiko / Verifikation**: Beobachtbares Verhalten ändert sich (mehr Felder werden zurückgesetzt). Daher Produktentscheidung nötig, ob nur die sichtbare Seite oder alles zurückgesetzt wird. Absicherung: neuer UI-/Logiktest "Restore Defaults auf Seite X setzt exakt die Felder der Seite X auf `EditorConfig{}`", plus die Sonderfälle (Session-Sperre, MCP aus).

---

### EDT-705 – Footer-Status: Strings, Vektoren und Datumsparsing in jedem Frame
- **ID**: EDT-705
- **Kategorie**: C
- **Schweregrad**: Mittel (absolut klein, aber garantiert pro Frame; messen)
- **Typ**: Performance
- **Ort**: `EditorRewards.cpp:1450-1470` (`composeIdle`), `:1610-1724` (`drawFooterStatus`), `:696-707` (`progressOf` → `dayBefore` `:510-521` → `parseDay` `:490-500`), `:220-232` (`Feed::look` → `lineFor`), `:1424-1431` (`todayCached`). Aufruf `EditorUI.cpp:3601`.
- **Problem**: Der Editor zeichnet jeden Frame (siehe `EditorRewards.h:181-183`). Pro Frame entstehen: ein `IdleText` mit `std::vector<Piece>` aus bis zu 7 `std::string`s plus `whole` (Konkatenation, `std::to_string` für beide Zähler), bei zu schmalem Footer ein zweites `composeIdle`, 3–5 `CalcTextSize` über diese Strings, `progressOf(...)` mit `dayBefore(today)` (`substr`/`stoi`/`snprintf`, ein String), und `Feed::look` erzeugt `lk.line` per `lineFor` (bei "Committed and pushed" Heap, sonst SSO). Das passiert auch, wenn Rewards deaktiviert sind (`composeIdle` läuft trotzdem für "Ready"). Der Text ändert sich aber nur bei Änderung von Zählerstand, Tag, Idle-Text oder Footerbreite.
- **Code-Auszug**:
```cpp
IdleText idle = composeIdle(idleTextIn, showCounters ? pr : Progress{}, ...);
if (idle.counters && ImGui::CalcTextSize(idle.whole.c_str()).x > winW / 3.0f)
    idle = composeIdle(idleTextIn, Progress{}, plain[0], plain[1]);
```
- **Lösung**:
  1. `todayCached()` zusätzlich `dayBefore(day)` mitcachen (ein-Sekunden-Cache existiert schon), `progressOf` bekommt `yesterday` übergeben.
  2. `IdleText` als `static` zwischenspeichern mit Schlüssel `(idleTextIn-Zeiger/Inhalt, p.any, builds.shown, streak.shown, showCounters)`; neu bauen nur bei Schlüsseländerung; `idleW` und der Überlauf-Test (`> winW/3`) nur bei Schlüssel- oder Breitenänderung neu messen.
  3. `Feed::look` liefert `const std::string&`/`std::string_view` auf einen im Feed gehaltenen `m_line`-Cache, der bei `take()` gesetzt wird, statt pro Aufruf zu bauen.
- **Erwarteter Effekt**: Schätzung: ~10–15 Heap-Allokationen und einige `CalcTextSize` pro Frame weniger; absolut wenige µs. Wert liegt eher in der Disziplin "idle Frame allokiert nichts".
- **Risiko / Verifikation**: Cache-Invalidierung bei Schriftgröße/DPI-Änderung (`CalcTextSize`-Ergebnis) und beim Sprachwechsel des Idle-Texts. `tests/test_editor_rewards.cpp` (ImGui-freie Kern-Funktionen) bleibt gültig; für die Pixelausgabe ein UI-Frame-Dump mit `HE_UI_DUMP_DIR` vergleichen.

---

### EDT-706 – Shortcut-Lookup über String-IDs: doppelte lineare Suche + Heap pro Aufruf; `conflicts()` O(n²) pro Frame
- **ID**: EDT-706
- **Kategorie**: D
- **Schweregrad**: Mittel (Niedrig für die reinen `pressed()`-Kosten, Mittel wegen der Shortcuts-Seite)
- **Typ**: Performance
- **Ort**: `EditorShortcuts.cpp:134-139` (`find`), `:141-147` (`chord`), `:161-195` (`pressed`), `:206-220` (`conflicts`); Aufrufer: 32 `pressed`-Stellen und 45 `label`-Stellen im Editor, `ShortcutsPage.cpp:147` (`conflicts(a.id)` pro Zeile pro Frame).
- **Problem**: `pressed(id)` ruft `find(id)` (lineare Suche über 31 Einträge, je `string_view`-Konstruktion über `strlen`) und danach `chord(id)`, das `std::string(id)` für die `unordered_map` baut (Heap bei IDs > 15 Zeichen, z. B. `"viewport.snapToGround"`, `"file.saveSceneAs"`) und intern erneut `find(id)` ausführt. Alle ~32 Shortcuts werden jeden Frame abgefragt. `conflicts(id)` ruft für jede Tabellenzeile erneut `chord(other.id)` auf; die Shortcuts-Seite macht das für jede der 31 Zeilen in jedem Frame: ~1 000 `find` + Hash + Allokationen pro Frame, solange die Seite offen ist. `chordToText`/`chordFromText` (`:222-251`) laufen dagegen nur beim Laden/Speichern (150 Tasten × `GetKeyName` pro Chord, okay).
- **Code-Auszug**:
```cpp
ImGuiKeyChord chord(std::string_view id)
{
    const auto& ov = overrides();
    if (const auto it = ov.find(std::string(id)); it != ov.end()) return it->second;
    const Action* a = find(id);
    return a ? a->defaultChord : ImGuiKey_None;
}
```
- **Lösung**:
  1. `pressed(const Action&)`/`chord(const Action&)`-Overloads; die zentrale Form arbeitet auf dem Index in der Tabelle. Aufrufer lösen die ID einmal auf (`static const Action* kSave = EditorShortcuts::find("file.save");`) oder nutzen eine `enum class Id`.
  2. Effektive Chords in einem `std::array<ImGuiKeyChord, N>` parallel zur Tabelle halten (bei `setChord/reset/decode` aktualisieren); `overrides()` wird zur Persistenzliste. Dann ist `chord()` ein Array-Zugriff.
  3. `conflicts()` auf dem Array: O(n) pro Aufruf ohne Allokation; alternativ die Konflikte der Seite einmal pro Revision (Änderung der Overrides) berechnen.
  4. Mindestens: `unordered_map` mit transparentem Hash (`std::string_view`-Lookup), um `std::string(id)` zu sparen.
- **Erwarteter Effekt**: Schätzung: `pressed()`-Summe pro Frame von einigen µs auf < 1 µs; Shortcuts-Seite pro Frame von ~1 000 Lookups auf ~31.
- **Risiko / Verifikation**: Die API (`string_view`-IDs, die Strings in Configs/Menüs) bleibt kompatibel, wenn die Overloads nur ergänzt werden. `tests/test_editor_shortcuts.cpp` und `tests/test_shortcuts_ui.cpp` prüfen Rebinding, Konflikte, Encode/Decode.

---

### EDT-707 – Toolbar-Zelle: Help-Lookup und Textmessung pro Zelle und Frame, drei Kopien der Zellen-Layoutlogik
- **ID**: EDT-707
- **Kategorie**: D / A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `EditorToolbar.cpp:47-97` (`cellImpl`, Zeile 56 `findKey`, Zeile 86 `CalcTextSize`), `:37-41` (`cellWidth` misst dasselbe Label bereits), `:352-392` (`Bar::item`, `itemTinted`, `readout` mit identischem Cursor-/Breiten-/Zentrier-Code).
- **Problem**: Jede Zelle mit `helpKey` macht pro Frame `Help::findKey(helpKey)` (die Tabelle in `EditorHelp.cpp:9634` ist groß; Suchkosten unbekannt, siehe "Unsicher"), obwohl das Ergebnis konstant ist. Das Label wird pro Zelle zweimal gemessen: in `Bar::item → cellWidth` und in `cellImpl` (`labelW`). `item`/`itemTinted`/`readout` wiederholen den Block `w = cellWidth; if(!m_first) m_cursor += kSegGap; x = ...; m_cursor += w; m_first=false;` und die Mittellinie-Berechnung aus `cellImpl`.
- **Code-Auszug**:
```cpp
const HE::Ed::Help::Entry* help = helpKey ? HE::Ed::Help::findKey(helpKey) : nullptr;
...
const float labelW   = label ? ImGui::CalcTextSize(label).x : 0.0f;
```
- **Lösung**: (1) `Bar::advance(label) → {x, w}` als private Hilfe für alle drei Zellenarten. (2) `cellImpl` bekommt die bereits berechnete Labelbreite (oder `w` und `labelW` aus `Bar`) übergeben. (3) `findKey` nur auslagern, wenn die Messung zeigt, dass es relevant ist (z. B. `static` Zeiger-Cache pro Aufrufstelle, aber `helpKey` ist ein Zeiger auf Literale; ein `unordered_map<const char*, Entry*>` könnte genügen).
- **Erwarteter Effekt**: Schätzung: ~20–40 Zellen pro Frame; Gewinn im niedrigen µs-Bereich; Hauptnutzen ist weniger Code.
- **Risiko / Verifikation**: Pixelgenaue Layoutänderungen ausschließen (Rundung `std::floor` in der gleichen Reihenfolge lassen); UI-Tests, die die Toolbars rendern (`tests/test_*_ui.cpp`) vor/nach per `HE_UI_DUMP_DIR` vergleichen.

---

### EDT-708 – `Row::`-Helfer: Feldschlüssel-String pro Zeile und Frame bei Mehrfachselektion
- **ID**: EDT-708
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `EditorRowWidgets.cpp:205-218` (`fieldKey`), `:429-444` (`MixedScope`, `mixedMask`), `:238` + `:301`-`:338` (jede Zeile ruft `mixedMask` zwei- bis dreimal), `:377-383` (`comboZ` baut Vektor).
- **Problem**: `mixedMask(label)` springt bei fehlendem Scope billig zurück, sobald aber ein `MixedScope` mit Feldern aktiv ist (Details-Panel bei Mehrfachselektion), baut jeder Aufruf `fieldKey(label)` (ein `std::string`, Zeichen für Zeichen mit `tolower`) und sucht in einer `unordered_map`. Die Wrapper (`sliderFloat` usw.) rufen `mixedMask` einmal selbst und `row()` einmal; `dragFloat3` sogar zweimal. Der `MixedScope`-Konstruktor baut zusätzlich pro Feld pro Frame einen Schlüssel. `comboZ` allokiert im gemischten Fall pro Frame einen `std::vector`.
- **Code-Auszug**:
```cpp
const unsigned m = mixedMask(label);
return row(label, [&]{ return axisDragN("##v", v, 3, speed, min, max, fmt, m); });
```
- **Lösung**: `mixedMask` einmal in `row()` auswerten und in den `body` durchreichen (z. B. `row(label, [&](unsigned mixed){...})`); `fieldKey` auf `thread_local std::string` ohne Neuallokation umstellen oder einen Hash über die normalisierten Zeichen berechnen (Schlüssel `uint64_t`) statt Strings in der Map.
- **Erwarteter Effekt**: Schätzung: bei Mehrfachselektion ~100+ Kleinallokationen pro Frame weniger; Einzelselektion unverändert.
- **Risiko / Verifikation**: `fieldKey`-Normalisierung (Teil vor `##`, nur alnum, lowercase) muss identisch bleiben (Hash-Kollisionen vermeiden, Schlüssel 64 Bit). Tests: Details-Panel-/Multi-Edit-UI-Tests vorhanden (`tests/test_*_ui.cpp`), Mixed-Anzeige "—" und "(mixed)" prüfen.

---

### EDT-709 – Ton-Synthese: Duplikate, Neuberechnung in `previewDragCues`, Erstberechnung im Save-Frame
- **ID**: EDT-709
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `EditorRewards.cpp:824-848` (`importPopPcm16`) gegen `:951-964` (`glide`); `:1000-1024` (`chimePcm16`) gegen `:745-759` (`toPcm16`); `:1221-1245` (`previewDragCues`) gegen `:1137-1147` (`playDragCue`-Cache); `:1122-1135` (`playTone` baut PCM beim ersten Abspielen).
- **Problem**: `importPopPcm16` hat die Gleit-Synthese ein zweites Mal von Hand geschrieben (nur mit `min(t, kGlideSec)`); `chimePcm16` normalisiert nicht über `toPcm16` (bewusst, aber als eigener Schleifenkörper). `previewDragCues` synthetisiert alle 5 Cues bei jedem Klick neu und mixt sie in einen neuen Puffer, obwohl `playDragCue` dieselben Cues cached. `playTone` erzeugt beim ersten Abspielen eines Tons die Samples synchron im Frame der Aktion (z. B. Chime: ~18 500 Samples × 2 Noten mit `sin`+`exp`; Schätzung 1–3 ms), obwohl die Header-Doku (`EditorRewards.h:313-318`) verspricht, dass die Rückmeldung "nie die Aktion verzögert" (dort bezogen auf das Öffnen des Geräts).
- **Code-Auszug**:
```cpp
if (pcm[i].empty()) pcm[i] = tonePcm16(t, kRate);   // im Frame des Speicherns
```
- **Lösung**: `importPopPcm16` über `glide(...)` ausdrücken (mit angepasster Gleitdauer-Parameter); `previewDragCues` aus dem Cache der Cues bedienen; Töne in `keepUiAudio` (läuft schon im eigenen Frame, wenn der Ton "möglich" wird) vorab erzeugen, sobald `uiSoundPossible` und das Gerät offen ist.
- **Erwarteter Effekt**: Schätzung: einmalig 1–3 ms weniger beim ersten Save/Build-Ton; 4 Duplikat-Schleifen weniger.
- **Risiko / Verifikation**: Ausgabe muss bitidentisch bleiben (`tests/test_editor_rewards.cpp` pinnt Eigenschaften der Töne; zusätzlich Hash der PCM-Bytes vor/nach Refactoring vergleichen). Glide-Parameter von `importPop` nicht ändern.

---

### EDT-710 – Rewards: unbegrenzt wachsende Tab-Liste, `normalPath` pro Content-Browser-Kachel
- **ID**: EDT-710
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `EditorRewards.cpp:377-402` (`SaveMarks::update`, `m_tabs` wird nie bereinigt), `:1341-1350` (`importHighlight`), `:404-407` (`normalPath`), Aufrufer `ContentBrowserPanel.cpp:1948`.
- **Problem**: `SaveMarks::m_tabs` bekommt für jeden je gezeichneten Tab-Schlüssel einen Eintrag (String + Zustand) und löscht nie; bei vielen geöffneten/geschlossenen Assets wächst die Liste, und die lineare Suche mit String-Vergleich läuft pro Tab und Frame (`EditorUI.cpp:3774`). `importHighlight` ruft, solange Marken existieren (bis 30 s nach einem Import), pro sichtbarer Kachel und Frame `normalPath(fullPath)` (`std::filesystem::path`, `lexically_normal`, `generic_string`: mehrere Allokationen).
- **Code-Auszug**:
```cpp
return s_fresh.strength(normalPath(fullPath), now);   // pro Kachel und Frame
```
- **Lösung**: `SaveMarks::update` durch einen `endFrame(openKeys)`-Schritt ergänzen, der nicht mehr vorhandene Tabs entfernt (oder Einträge ohne Aktivität nach X Sekunden); `importHighlight` zuerst billig prüfen, ob der Dateiname (`filename`) irgendeiner Marke passt, bevor normalisiert wird, oder die Marken beim Import zusätzlich über den rohen Pfad indizieren.
- **Erwarteter Effekt**: Schätzung: vernachlässigbar im Normalfall; begrenzt Wachstum und verhindert ein Hotspot-Szenario mit 1 000 sichtbaren Kacheln nach einem Import.
- **Risiko / Verifikation**: Marken-Zeitfenster (`kImportWindowSec`) und Check-Mark-Dauer (`kTabCheckSec`) nicht verändern. `tests/test_editor_rewards.cpp` deckt `FreshImports`/`SaveMarks` ab; Test für "Tab geschlossen → Eintrag entfernt, neu geöffneter Tab zeigt keinen alten Haken" ergänzen.

---

### EDT-711 – Settings-Panel Kleinkram: `toggleFavorite`, `prefScopeFor`, Status-Seite
- **ID**: EDT-711
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `EditorSettingsPanel.cpp:84-100` (`toggleFavorite`), `:164-171` (`prefScopeFor`), `:1667-1696` + `:1746-1755` (`statusRow` mit `std::function`-Temporären), `:303-1090` (Row-Lambdas).
- **Problem**: (a) `toggleFavorite` parst die Favoritenliste mit `std::stringstream`/`getline`, obwohl `QuickSettingsFavorites.h` bereits einen allokationsfreien Parser für dieselbe Liste hat; zwei Implementierungen derselben Grammatik (leere Tokens, Doppelkommas). (b) `prefScopeFor` baut pro sichtbarer Zeile und Frame ein temporäres `std::string` (`cache.find(cat)` mit `const char*` auf `std::map<std::string,…>`; für "Global Illumination" (19 Zeichen) Heap) – dabei ist die Kategorie ein Literal. (c) Die Status-Seite baut pro Frame ~15 `std::string`-Details und pro Fix-Zeile ein `std::function`; die Seite ist selten offen.
- **Code-Auszug**:
```cpp
static std::map<std::string, std::string> cache;
auto it = cache.find(cat);          // temporärer std::string pro Aufruf
```
- **Lösung**: (a) `toggleFavorite` in `QuickSettingsFavorites.h` neben `favoritesContain` (mit `string_view`-Tokenizer). (b) Cache als `std::unordered_map<const char*, std::string>` (Zeiger der Literale als Schlüssel) oder `std::map<std::string, std::string, std::less<>>` mit transparenter Suche, oder die Zeilen übergeben direkt den Scope-String. (c) nur bei Messung anfassen.
- **Erwarteter Effekt**: Schätzung: wenige Allokationen pro Frame bei geöffneter Preferences-Seite; Hauptgewinn weniger Doppelcode.
- **Risiko / Verifikation**: Reihenfolge und Format von `QuickSettingsFavorites` bleiben (config.json-Kompatibilität). Vorhandener Test für `favoritesContain` um `toggle` erweitern (leere Tokens, Duplikate).

---

### EDT-712 – Kleinere Beobachtungen (kein eigener Fund)
- **Ort / Problem / Lösung**: kurz gebündelt, alle Niedrig/Vereinfachung:
  - `EditorSelfUpdate.cpp:82` `Session::set(const std::function<void(Snapshot&)>&)` erzeugt für jede Statusänderung ein `std::function`; kalt, kann ein Template sein. Kein Handlungsbedarf.
  - `EditorTabs.cpp:22-43` `sameAsset` macht pro Vergleich zweimal `fs::exists` + `fs::equivalent` (Syscalls), und `find()` ruft das für jeden Tab (`:51-54`); nur beim Öffnen von Tabs, nicht im Frame. Wenn `openOrFocus` einmal pro Frame aus einer Schleife aufgerufen würde (nicht gefunden), O(Tabs) Dateisystemzugriffe; derzeit nur Aktionen.
  - `EditorRewards.cpp:1049-1059` `tallyMoment` schreibt bei jedem Build/Commit synchron `config.json` im UI-Thread (im Frame der Aktion); selten, aber Disk-I/O auf dem Edge, den das Header-Versprechen "never delays the action" betrifft.
  - `EditorRewards.h:1-430`: ~430 Zeilen Designdokumentation im Header; wirkt sich nicht auf Kompilierzeit aus, gehört aber nach `docs/` (laut CLAUDE.md-Konvention für Designaufzeichnungen).

---

## Gelesen

Alle Dateien des Slice vollständig gelesen (ja): EditorRewards.cpp (1–1731) und .h (1–902), EditorRowWidgets.cpp, EditorSelection.{h,cpp}, EditorSelfUpdate.cpp und .h (Zeilen 1–60 des Headers nur überflogen, Kopfkommentar), EditorSettingsCatalog.{h,cpp}, EditorSettingsPanel.cpp (1–2490) und .h, EditorShortcuts.{h,cpp}, EditorSystemMotion.{h,cpp,mm}, EditorTabs.{h,cpp}, EditorTheme.{h,cpp}, EditorToolbar.{h,cpp}, EditorTransformGizmo.{h,cpp}. Zusätzlich gezielt gelesen: Aufrufer in ViewportPanel.cpp, EditorApplication.cpp, ShortcutsPage.cpp, SelfUpdateDialog.cpp, EditorUI.cpp, ContentBrowserPanel.cpp, QuickSettingsFavorites.h (nur Zitate, nicht ausgewertet).

## Editor Slice 8 – EditorUI … GitController

## Zusammenfassung
Der Slice (EditorUI.cpp 4438 Zeilen, ExportDialogPanel.cpp 1999, GitController/-CloneDialog, EditorWidgets, EditorUndo, ViewportNav, Footer-Bars, Kleinteile) ist funktional sorgfältig und gut kommentiert, leidet aber an zwei strukturellen Problemen: (1) Pflege-Listen, die in vielen parallelen Kopien gehalten werden (16 Asset-Panels in 8 Dispatch-Stellen, 13 Fenster-Toggles in 6 Listen, Mac- und ImGui-Menü) und bereits driften, (2) monolithische Funktionen (`renderEditor` ~3000 Zeilen, `startExport` ~780 Zeilen). Pro-Frame-Kosten sind meist klein, aber vermeidbar: String-Bau je Tab/Slot/Kachel, ungeclippte Listen in Picker-Popup und Clone-Dialog, 16 Hash-Lookups je Tab und Frame. Funde: 1 Hoch, 11 Mittel, 11 Niedrig (23 gesamt). Nichts davon wurde gemessen; Schweregrade beruhen auf Code-Lesung.

## Funde

### EDT-801 – 16 Asset-Panels in 8 parallelen Handliste; Drift bereits sichtbar, dazu 16 Lookups je Tab und Frame
- **ID**: EDT-801
- **Kategorie**: A (auch D)
- **Schweregrad**: Hoch
- **Typ**: Beides
- **Ort**: `src/HE_Editor/EditorUI.cpp:978-997` (tabHasUnsavedEdits), `1004-1026` (unsavedAssetPaths), `1033-1056` (saveAsset), `1061-1078` (appendAssetSnapshots), `1084-1093` (collabDocsFor), `1102-1124` (discardPanelState), `1196-1216` (reloadAssetTabFromDisk), `4111-4186` (Render-Dispatch), Nutzung pro Frame `3769`
- **Problem**: Jede Panel-Klasse wird in acht Funktionen von Hand aufgezählt. Die Kommentare räumen das Risiko selbst ein ("a panel missing here silently keeps its edits"). Die Listen sind bereits uneinheitlich: `appendAssetSnapshots` (1061-1078) enthält `CinematicPanel` nicht, obwohl `saveAsset` (1048), `unsavedAssetPaths` (1018) und `tabHasUnsavedEdits` (992) es führen; das heißt Cinematic-Tabs bekommen vermutlich keine Crash-Recovery-Kopie (`CinematicPanel.h` hat keine `appendSnapshots`). Der Kommentar nennt "the same fourteen panels", die Liste hat 15 Einträge. `reloadAssetTabFromDisk` kennt `SkeletalMeshEditorPanel` nicht (evtl. gewollt, nirgends begründet). Zusätzlich läuft `tabHasUnsavedEdits` in der Tab-Leiste für jeden Tab jeden Frame (Zeile 3769) und ruft 16-mal `AssetPanelState::dirty(path)` auf (`EditorPanelState.h:46`, je ein String-Hash-Lookup auf demselben Pfad). Bei 20 Tabs sind das ~320 Hash-Lookups pro Frame. Der Render-Dispatch (4111-4186) probiert bis zu 18 `isXAsset(path)`-Prädikate nacheinander pro Frame für den aktiven Tab.
- **Code-Auszug**:
```cpp
// EditorUI.cpp:981-996
return ScriptEditorPanel::isDirty(assetPath)        ||
       CppClassEditorPanel::isDirty(assetPath)      ||
       MaterialEditorPanel::isDirty(assetPath)      || ... SkeletalMeshEditorPanel::isDirty(assetPath);
// EditorUI.cpp:1061 ff.: appendAssetSnapshots – ohne CinematicPanel
```
- **Lösung**: Eine einzige Tabelle `static const AssetPanelOps kAssetPanels[]` mit den Funktionszeigern bzw. Lambdas je Panel (`isAsset`, `isDirty`, `appendDirty`, `save`, `appendSnapshots`, `collabDocs`, `forget`, `reload`, `render`; nicht unterstützte Operationen = `nullptr`). Alle acht Funktionen werden zu Schleifen über die Tabelle; ein neues Panel wird an genau einer Stelle eingetragen und eine fehlende Fähigkeit (Cinematic: Snapshots) fällt im Review als `nullptr` auf. Pro Frame: `tabHasUnsavedEdits` mit einem Panel-Index pro Tab cachen (Tabelle liefert beim Öffnen das Panel; `tab.panel`), dann 1 statt 16 Lookups; der Dispatch wird `kAssetPanels[tab.panel].render(...)`, Invalidierung über die vorhandenen Versionszähler von `EditorAssetTypeCache`.
- **Erwarteter Effekt**: Wartbarkeit deutlich besser, Fehlerklasse "Panel vergessen" entfällt. Performance: von ~16·N auf ~N Hash-Lookups je Frame (Schätzung, wenige µs bis zweistellige µs; messen).
- **Risiko / Verifikation**: Reihenfolge der Prüfung im Dispatch ist semantisch (Raw-Dateien vor `ScriptEditorPanel`-Fallthrough, Kommentare 4173-4184); die Tabelle muss diese Reihenfolge erhalten. Vor dem Umbau Test, der pro Panel `isDirty/save/forget` über die Tabelle ansteuert (z.B. Erweiterung von `tests/test_*_ui.cpp`); für Cinematic zunächst klären, ob fehlende Recovery Absicht ist (dann `nullptr` mit Kommentar).

### EDT-802 – `renderEditor` ist eine ~3000-Zeilen-Funktion mit ~40 Lambdas und zwei Menü-Implementierungen
- **ID**: EDT-802
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorUI.cpp:1286-4320`; Mac-Zweig `1994-2138`, ImGui-Menü `2147-2562`
- **Problem**: Eine Funktion enthält Guard-Zustand (statische Locals 1299-1327), Datei-Dialog-Logik, Menü (zweifach: nativ-Mac und ImGui), Footer, Tab-Leiste, Shortcuts, Lock-Banner, Panel-Dispatch und Dock-Layout. Die Enablement-Regeln der Menüeinträge stehen doppelt (ImGui-Zweig prüft `ctx.projectLoaded` je Eintrag, der Mac-Switch prüft sie in jedem `case` neu; Kommentar 1996 nennt das selbst). Drei fast identische Dialog-Lambdas (`triggerOpenScene`, `triggerAddSceneAdditive`, `triggerSaveSceneAs`, 1360-1387) unterscheiden sich nur in Op/Funktion.
- **Code-Auszug**:
```cpp
auto triggerOpenScene = [&]() { s_pendingFileOp = PendingFileOp::OpenScene;
    SDL_DialogFileFilter filters[] = { { "Horizon Scene", "hescene" } };
    const std::string dir = sceneDialogDir(); SDL_ShowOpenFileDialog(fileDialogCb, ...); };
// triggerAddSceneAdditive: identisch bis auf s_pendingFileOp
```
- **Lösung**: In freie Funktionen/Klassen schneiden: `EditorGuard` (Unsaved-Modal, Collab-Modal, `requestGuarded`), `EditorMenuBar` (eine Tabelle `{Cmd, label, shortcutId, enabledFn, actionFn}`, aus der ImGui-Menü und Mac-Switch erzeugt werden), `EditorFooter`, `EditorTabBar`, `EditorShortcuts`. Die drei Dialog-Lambdas zu `openSceneDialog(PendingFileOp, bool save)` zusammenführen.
- **Erwarteter Effekt**: Lesbarkeit/Testbarkeit; kein Laufzeiteffekt (Schätzung 0).
- **Risiko / Verifikation**: Hoch bei einem Schnitt in einem Zug (viele `static`-Locals teilen Zustand). Schrittweise, je Block ein Commit; Absicherung durch `tests/test_*_ui.cpp` und `scripts/editor_help_audit.py --check` (Help-Scopes hängen an der Reihenfolge der Aufrufe).

### EDT-803 – Fenster-Toggles in sechs parallelen Listen
- **ID**: EDT-803
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `EditorUI.cpp:299-313` (docsPanelOpener), `353-369` (s_panelPrefs), `1943-1955` (Mac setToggleState), `2102-2118` (Mac Cases), `2481-2512` (ImGui Window-Menü), `4347-4378` (renderOverlays)
- **Problem**: Jedes der 13 View-Panels (Profiler, Environment, Collab, Team, SourceControl, Console, AudioMixer, UndoHistory, Watch, TaskBoard, Scene 2-4) taucht mit Titel, Flag und Config-Key in allen sechs Stellen auf. Ein neues Panel vergessen heißt: kein "Show me", keine Persistenz oder kein Mac-Menüeintrag.
- **Code-Auszug**:
```cpp
{ "Performance Profiler", "PanelOpenProfiler", &s_showProfiler },   // s_panelPrefs
{ "Performance Profiler", &s_showProfiler },                        // docsPanelOpener
MacMenuBar::setToggleState(MC::ToggleProfiler, s_showProfiler);     // + Switch + ImGui-Menü
```
- **Lösung**: Eine Tabelle `struct ViewPanel { const char* title; const char* configKey; bool* open; MC cmd; void(*draw)(AppContext&, bool&); }`; `docsPanelOpener`, `updatePanelVisibility`, Mac-Sync, Window-Menü und `renderOverlays` iterieren darüber.
- **Erwarteter Effekt**: Weniger Pflegeaufwand, ca. -80 Zeilen. Kein Laufzeitgewinn.
- **Risiko / Verifikation**: Menü-Reihenfolge und Trenner (nur `!appProj`) müssen erhalten bleiben; die Draw-Signaturen sind uneinheitlich (`DrawTeamWindow` ist lokal). Help-Audit-Skript ausführen.

### EDT-804 – Footer: hand gepflegte Breitenkette, je Widget quadratisch wachsender Ausdruck
- **ID**: EDT-804
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `EditorUI.cpp:3430-3595`
- **Problem**: Acht Blöcke (Team, Bell, Presence, Online, Sync, Activity, MCP, Build) rechnen die x-Position je als `Fenster − fps − teamSlot − bell − presence − … − Padding − 16 − (bell>0?16:0) − …`. Der Kommentar (3445-3448) beschreibt: "inserting a widget here means editing EVERY block to its left". Der Fix mit `actW` (3539-3544) zeigt, dass das schon einmal zu einem Sprungfehler führte.
- **Code-Auszug**:
```cpp
ImGui::SameLine(ImGui::GetWindowWidth() - fpsW - teamSlot - bellW - presenceW - onlineW - syncW - actW - mcpW - buildW
    - ImGui::GetStyle().WindowPadding.x - 16.0f - (bellW > 0.0f ? 16.0f : 0.0f) - ... );
```
- **Lösung**: Slots rechts nach links in ein kleines Array: `struct Slot{ float w; std::function<void()> draw; }` (oder `inplace_function`; Lambdas leben nur im Frame). Dann eine Schleife: `x = right; for (s : slots) if (s.w>0) { x -= s.w; SameLine(x); s.draw(); x -= 16; }`. Alle `FooterWidth` einmal pro Frame, die Reihenfolge steht an einer Stelle.
- **Erwarteter Effekt**: Footer-Code ~165 auf ~60 Zeilen, neue Widgets ohne Folgeedits.
- **Risiko / Verifikation**: Pixelposition muss identisch bleiben (Offsets: Gap 16 nur zwischen vorhandenen Nachbarn, Padding am Rand). UI-Shot-Vergleich mit `HE_DUMP_UI` vorher/nachher bei verschiedenen Kombinationen.

### EDT-805 – Edit-Menü entscheidet über Undo/Redo anders als die "eine Tür" darüber
- **ID**: EDT-805
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (kleine Verhaltensangleichung)
- **Ort**: `EditorUI.cpp:1256-1283` vs. `2228-2254`
- **Problem**: Der Kommentar bei `sessionUndoActive` (1248-1255) sagt, Footer, Chord, Mac-Menü und Edit-Menü gehen durch dieselbe Tür. Das Edit-Menü berechnet aber `collabUndoActive` selbst (ohne `!ctx.isPlaying`) und im Nicht-Session-Zweig `canUndo = ctx.undoSys && ctx.undoSys->canUndo()` ohne die `&& ctx.undo`-Prüfung von `canUndoNow`. Folge: In einer Session während Play ist der Eintrag aktiv, der Footer-Button nicht.
- **Code-Auszug**:
```cpp
const bool collabUndoActive = ctx.collab && ctx.collab->inSession() && ctx.collabUndo;   // 2228
return !ctx.isPlaying && ctx.collab && ctx.collab->inSession() && ctx.collabUndo;        // 1260
```
- **Lösung**: Edit-Menü auf `sessionUndoActive`, `canUndoNow`, `canRedoNow`, `undoNow`, `redoNow` umstellen; nur das Label aus `collabUndo->undoLabel()` bleibt im Menü.
- **Erwarteter Effekt**: Ein Entscheidungspunkt weniger, konsistentes Verhalten.
- **Risiko / Verifikation**: Menü ist während Play in einer Session künftig deaktiviert (heute aktiv). Test: UI-Test mit `ctx.collab` (Fake) und `isPlaying=true`.

### EDT-806 – Tab-Leiste baut je Tab und Frame Strings
- **ID**: EDT-806
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `EditorUI.cpp:3766-3787`
- **Problem**: Pro Tab und Frame: `std::string suffix`, `tab.label + suffix + "###tab_" + path` (mindestens eine Heap-Allokation, bei absoluten Pfaden sicher), und nach dem Speichern zusätzlich zwei `CalcTextSize` mit Temporär-Strings (Zeilen 3778-3781, 3797). Dazu `tabHasUnsavedEdits` (siehe EDT-801). Bei 15-25 offenen Tabs sind das einige Dutzend Allokationen pro Frame, nur um dieselbe ID zu hashen.
- **Code-Auszug**:
```cpp
const std::string shown = tab.label + suffix
    + "###tab_" + (tab.assetPath.empty() ? std::string("scene") : tab.assetPath);
```
- **Lösung**: In `EditorTab` einen vorgefertigten `std::string imguiId` ("###tab_…") cachen (beim Anlegen/Umbenennen setzen) und das Label per `snprintf` in einen wiederverwendeten `thread_local char[512]` bauen (`label`, `" *"`, `imguiId`). Die Blank-Breite für das Check-Mark-Padding einmal pro Tab cachen oder nur bei `mark.check` berechnen (ist es bereits; bleibt).
- **Erwarteter Effekt**: ~0 Allokationen pro Frame statt N (Schätzung); absolut klein, aber läuft in jedem Frame mit Projekt.
- **Risiko / Verifikation**: ID muss byte-identisch bleiben, sonst verlieren Tabs ihren Zustand (ImGui-Identität). `tabs`-Anlage hat mehrere Aufrufer (`EditorTabs::openOrFocus`), daher den Cache lazy füllen.

### EDT-807 – `assetDropSlot`: pro Slot und Frame vier String-Allokationen und ein Asset-Lookup
- **ID**: EDT-807
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorWidgets.cpp:100-104`, `113`, `120`, `198`
- **Problem**: Wird in jedem Details-Abschnitt für jeden Asset-Slot jeden Frame aufgerufen. `shown` (Kopie des Asset-Namens), `popupId = "##pickasset_" + idSuffix` (12 Zeichen Präfix, also meist Heap), `(shown + "##" + idSuffix).c_str()` und bei `showClear` `"Clear##" + idSuffix` werden je Frame neu gebaut; `resolveAsset` macht je Frame den Typ-Getter-Lookup in `ContentManager`. Außerdem ist `idSuffix` ohne Default und wird in Zeile 113/120/198 ungeprüft in `std::string + const char*` benutzt, während 125 und 163 `idSuffix ? idSuffix : ""` schreiben; ein `nullptr` wäre UB.
- **Code-Auszug**:
```cpp
const std::string popupId = std::string("##pickasset_") + idSuffix;
if (ImGui::Button((shown + "##" + idSuffix).c_str()))
```
- **Lösung**: Ein `char buf[256]` per `snprintf`/`ImFormatString` für Button-Label und `popupId` (ImGui braucht popupId nur für `OpenPopup`/`BeginPopup`: `ImGui::GetID(idSuffix)` einmal und `OpenPopup(ImGuiID)` verwenden). `shown` als `const char*` auf den Asset-Namen statt Kopie (die Getter liefern Zeiger auf stabile Assets). `idSuffix` zu `const char*` mit Pflicht-Assert oder Normalisierung am Anfang.
- **Erwarteter Effekt**: ~3-4 Allokationen weniger pro Slot und Frame; bei einer Details-Ansicht mit 20 Slots ~80 Allokationen pro Frame (Schätzung).
- **Risiko / Verifikation**: Popup-ID muss konsistent zwischen `OpenPopup` und `BeginPopup` sein (beide umstellen). UI-Tests der Details-Panels.

### EDT-808 – Asset-Picker-Popup ohne Clipping und ohne Filter-Cache
- **ID**: EDT-808
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `EditorWidgets.cpp:123-130`, `153-190`
- **Problem**: CLAUDE.md fordert Row-Clipping für zeilenreiche Panels. Hier läuft pro Frame, solange das Popup offen ist, eine Schleife über *alle* Assets des Typs mit `assetMatchesQuery` und `(a.label + "##" + a.path)` je Eintrag (Allokation + Hash), nur sichtbare Zeilen werden gezeichnet (ImGui clippt, spart aber die Vorarbeit nicht). Bei Projekten mit tausenden Materialien/Meshes ist das ein spürbarer Frame-Kosten-Block. Außerdem: `HcEditorUtil::listAssets` (`HcEditorUtil.cpp:93`) sniffed beim Öffnen die Header aller `.hasset` (Kommentar 122-124); ob es `EditorAssetTypeCache` nutzt, wurde nicht geprüft.
- **Code-Auszug**:
```cpp
for (const auto& a : s_pickerItems) {
    if (!HcEditorUtil::assetMatchesQuery(a.label, a.path, s_pickerQuery)) continue;
    if (ImGui::Selectable((a.label + "##" + a.path).c_str()) ...
```
- **Lösung**: Gefilterte Indexliste `s_pickerShown` nur neu berechnen, wenn sich `s_pickerQuery` ändert (Vergleich mit Vorwert), dann `ImGuiListClipper` über diese Liste; Selectable-Label per `ImGui::PushID(idx)` + `Selectable(a.label.c_str())` statt String-Konkatenation.
- **Erwarteter Effekt**: Popup-Frame-Kosten O(sichtbare Zeilen) statt O(alle Assets) (Schätzung).
- **Risiko / Verifikation**: ID-Wechsel von `label##path` auf `PushID(idx)` ändert Fokus/Scroll-Identität nicht relevant (Popup schließt bei Auswahl). Manuell testen: Suche tippen, Auswahl, "(none)".

### EDT-809 – `GitController::toRepoRelative` allokiert pro Kachel und Frame mehrfach
- **ID**: EDT-809
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/GitController.cpp:436-482`; Aufrufer `ContentBrowserPanel.cpp:834, 1686, 2045`
- **Problem**: Der Header (193-195) verspricht "cheap enough per visible tile". Je Aufruf: `normaliseSlashes(absolutePath)` (Kopie), `normaliseSlashes(s.root)` (bei jedem Aufruf neue Kopie derselben Konstante), `abs.substr(...)` (dritte Allokation), bei Alias-Pfad nochmal. Das sind 3-5 Heap-Allokationen je sichtbarer Datei- und Ordnerkachel und Frame; bei 500 Kacheln ~2000 Allokationen pro Frame.
- **Code-Auszug**:
```cpp
const std::string abs = normaliseSlashes(absolutePath);
...
std::string rel = stripPrefix(normaliseSlashes(s.root));
```
- **Lösung**: Normalisiertes Root und Alias einmal in `update()` ablegen (dort läuft schon die `generation`-Prüfung, Zeile 356-371): `m_rootNorm`. Pro Aufruf keine Normalisierung, wenn der Pfad kein `'\\'` enthält (unter macOS/Linux immer der Fall; `find_first_of` prüft nur). `RepoStatus::find` nimmt `const std::string&` (`RepoStatus.h:106`), also ein `thread_local std::string scratch` für `rel` statt `substr`.
- **Erwarteter Effekt**: ~0 Allokationen pro Aufruf (Schätzung); Content-Browser-Frames mit Git-Badges spürbar billiger bei großen Ordnern (messen).
- **Risiko / Verifikation**: Verhalten bei Symlink-Alias und Windows-Backslashes erhalten. Es gibt vermutlich Tests zu `entryForAbsolutePath`; sonst einen ergänzen (Root `/repo`, Pfad `/repo-other/x` darf nicht matchen, Alias-Fall).

### EDT-810 – `startExport`: ~780 Zeilen, davon ein ~280-Zeilen-Worker-Lambda
- **ID**: EDT-810
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/ExportDialogPanel.cpp:1215-1997` (Lambda `1714-1991`, `ExportSettings`-Befüllung `1521-1626`)
- **Problem**: Eine Funktion mischt Szenen-Serialisierung, HorizonCode-Quellensammlung, Runtime-Suche, Step-Plan, ~60 Zeilen Feld-für-Feld-Kopie `ctx.projectManager->currentProject().X` (30-mal derselbe Ausdruck) und einen Worker mit Codegen, Datei-Schreiben und Berichtstext. Die Einrückung (Block `{` in `{`) ist ein Rest aus einer früheren Verschachtelung.
- **Code-Auszug**:
```cpp
es.allowFiles     = ctx.projectManager->currentProject().allowFiles;
es.allowProcesses = ctx.projectManager->currentProject().allowProcesses; ... (30x)
```
- **Lösung**: Extrahieren: `collectScenes(...)`, `collectHcSources(...)`, `makeExportSettings(const ProjectData&, …)`, `runHcCodegen(...)` (liefert `hcMsg/hcFatal/esEff.horizonCodeGenLib`) und ein kleines Worker-Objekt. `const ProjectData& proj = pm->currentProject();` einmal. `buildGameConfigJson` (148-229) kann mit einer Tabelle `{key, &EditorConfig::field}` die 35 `put`-Zeilen ersetzen (nur wenn alle Typen JSON-konvertierbar sind).
- **Erwarteter Effekt**: Lesbarkeit; keine Laufzeitänderung.
- **Risiko / Verifikation**: Der Worker liest `s_exportHcStop` (Static) aus dem Thread (1739, 1765, 1780, 1893); beim Extrahieren als Wert capturen. Es gibt Export-Tests im Repo prüfen (`tests/` nach `ProjectExporter`), sonst Smoke-Export mit Golden-`project.hcfg`.

### EDT-811 – Export durchsucht den gesamten Projektordner rekursiv (zweimal), auf dem UI-Thread
- **ID**: EDT-811
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `ExportDialogPanel.cpp:490-503` (`open`), `1297-1340` (`startExport`)
- **Problem**: Beide Stellen laufen mit `recursive_directory_iterator` über das ganze Projektverzeichnis (inklusive `.git`, `Export/`-Ausgabe, LFS-Cache, Build-Verzeichnisse) nur um `.hescene` zu finden. `startExport` lädt und serialisiert dort zusätzlich synchron jede Szene auf dem UI-Thread (JSON-Parse + `saveToMemory`); der Kommentar 1211-1214 räumt das Einfrieren ein. Die Suchschleife ist als Code doppelt vorhanden. Die Startszene wird außerdem zweimal geladen (1281 und im Scan 1316, Kommentar bei 1290 nennt es gewollt).
- **Code-Auszug**:
```cpp
std::filesystem::recursive_directory_iterator sit(projectRoot2, skip_permission_denied, ec2);
... if (regular && sit->path().extension() == ".hescene") { HorizonWorld w2; ser2.load(w2, ...) ... }
```
- **Lösung**: Gemeinsamer Helfer `std::vector<fs::path> findProjectScenes(root)`: `sit.disable_recursion_pending()` für Verzeichnisse `.git`, `Export`, `Cache`/`Intermediate` (konkrete Namen aus `ProjectManager` übernehmen) oder auf die vom `ContentManager` bereits geführte Registry zurückgreifen (`scanContentDirectory` kennt alle Assets). Das Laden/Serialisieren in den Worker-Thread verschieben, wo es nur `ContentManager` für `syncPrefabInstances` braucht (Kommentar sagt UI-Thread, Thread-Sicherheit von `ContentManager` vorher klären).
- **Erwarteter Effekt**: Menü-Öffnen und Export-Start nicht mehr proportional zur Gesamtgröße des Ordners (bei Projekten mit Export-Ausgabe oder `.git` mit vielen Objekten potenziell Hunderte ms; messen). UI-Freeze beim Start entfällt nur bei Verschiebung in den Worker.
- **Risiko / Verifikation**: Szenen außerhalb des Ausschlusses müssen weiter gefunden werden (heute wird alles gefunden, auch Szenen in `Export/`; das ist vermutlich unerwünscht, aber ein Verhaltensunterschied). Verschiebung in den Worker ist riskant (Thread-Sicherheit von `ContentManager`/`SceneSerializer`). Test: Projekt mit Szenen in Unterordnern und in `Export/`.

### EDT-812 – Worker-Lambda kopiert alle Szenenbinärdaten und `ExportSettings`
- **ID**: EDT-812
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `ExportDialogPanel.cpp:1714-1717`, `1731`
- **Problem**: Die Capture-Liste `[es, contentDir, projName, sceneName, outDir, sceneBinary, extraScenes, gameInstanceJson, hcCompile, hcSources, …]` kopiert `extraScenes` (alle Szenen als CBOR-Binärdaten), `sceneBinary`, `hcSources` (alle Graphen) und `es`; die Originale werden danach nie mehr gelesen. In der Lambda folgt noch `ExportSettings esEff = es;` (Zeile 1731), also eine zweite Kopie.
- **Code-Auszug**:
```cpp
s_exportThread = std::thread([es, contentDir, projName, sceneName, outDir, sceneBinary, extraScenes,
                              gameInstanceJson, hcCompile, hcSources, ...]()
```
- **Lösung**: Init-Captures mit `std::move`: `[es = std::move(es), sceneBinary = std::move(sceneBinary), extraScenes = std::move(extraScenes), hcSources = std::move(hcSources), …]`; `esEff` direkt als `ExportSettings esEff = std::move(es)` (Lambda ist nicht `mutable`, daher `mutable` ergänzen oder `es` in ein lokales `shared_ptr` legen).
- **Erwarteter Effekt**: Einmaliger Export-Start spart eine Kopie aller Szenen (bei Projekten mit mehreren hundert MB Szenen merkbar; Schätzung).
- **Risiko / Verifikation**: Lambda muss `mutable` werden. Keine Verhaltensänderung; Smoke-Export.

### EDT-813 – Clone-Dialog: Repo-Liste ohne Clipping, Filter und Meta-Strings je Frame
- **ID**: EDT-813
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/GitCloneDialog.cpp:124-134`, `419-448`
- **Problem**: Für jedes Repo im Konto (kann hunderte betragen) pro Frame: `containsNoCase` (kopiert die Nadel in einen `std::string`, `std::search` mit `tolower` je Zeichen), `std::string meta = … + updatedAt.substr(0,10)` (zwei Allokationen), `CalcTextSize`, `PushID`, `Selectable`; danach Zeile 447 nochmal ein linearer Scan zum Finden von `chosen`. Läuft nur, solange der Dialog offen ist (einmaliger Dialog), daher Niedrig.
- **Lösung**: Filterliste (Indizes) nur bei Änderung von `s_filter` oder `repoList` neu bauen, `ImGuiListClipper`, `meta` per `snprintf` in einen Stack-Puffer, Nadel einmal klein schreiben. `chosen` als Index merken.
- **Erwarteter Effekt**: Kleiner Gewinn, nur bei Konten mit sehr vielen Repos spürbar.
- **Risiko / Verifikation**: Gering. Manuell mit gemockter Liste (500 Einträge).

### EDT-814 – Windows-Ordnerauswahl (`IFileOpenDialog`) dreifach kopiert
- **ID**: EDT-814
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `EditorUI.cpp:3150-3197`, `GitCloneDialog.cpp:202-248`, `ProjectHubPanel.cpp:303-` (außerhalb des Slice)
- **Problem**: ~45 Zeilen COM-Boilerplate je Kopie; die Non-Windows-Variante (`SDL_ShowOpenFolderDialog`) ebenfalls dreifach, jedes Mal mit eigenem Ergebnis-Slot.
- **Lösung**: `EditorWidgets::pickFolder(SDL_Window*, const char* startDir, std::function<void(std::string)>)` (oder Ergebnis-Slot-Klasse) in `EditorWidgets.cpp`, mit der Windows- und SDL-Variante an einer Stelle.
- **Erwarteter Effekt**: ca. -100 Zeilen, ein Ort für Bugfixes (z.B. Längenprüfung `len <= hubProjectDirSize` in EditorUI.cpp:3173 vs. `sizeof(s_parentDir)` in GitCloneDialog:225).
- **Risiko / Verifikation**: Plattformabhängig; nur unter Windows testbar (COM), Linux/macOS per SDL-Pfad manuell.

### EDT-815 – Export-Modal: Dateisystem- und Pfadarbeit je Frame, solange es offen ist
- **ID**: EDT-815
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `ExportDialogPanel.cpp:719-721`, `731`, `757`, `1035-1049`, `1000-1002`
- **Problem**: Je Frame: `SDL_GetBasePath()` zweimal plus `std::filesystem::path`-Konstruktion, `bundleKey = platform + "/" + flavor`, bei fehlender Runtime `resolveRuntimeDir(...).lexically_normal().string()` und `BuildProgressDialog::interpretedClasses()` (liefert Kopie von Headline und Klassenliste) mit `HcFallbackReport::describe` pro Zeile. Der Cache `s_exportBundleCache` löst nur `findRuntimeBundle`, nicht die übrigen Pfade.
- **Lösung**: Im Cache-Treffer-Pfad zusätzlich den normalisierten Anzeigetext (`bundleText`, `missingText`) speichern und mit ausgeben; `interpretedClasses()` nur bei `Build::finish` abholen und im Panel halten.
- **Erwarteter Effekt**: Nur während des offenen modalen Dialogs; vernachlässigbar außer bei sehr tiefen Pfaden.
- **Risiko / Verifikation**: Gering.

### EDT-816 – Publish-Dialog zeichnet das gesamte Log als `InputTextMultiline` unter Mutex
- **ID**: EDT-816
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EngineContentPublishDialog.cpp:233-237`
- **Problem**: Das ganze Log (bei Tausenden Dateien viele KB) wird jeden Frame an `InputTextMultiline` gegeben, während `s_logMutex` gehalten wird; der Worker blockiert in `appendLog` (Zeile 92), solange gezeichnet wird. ImGui zählt dafür den gesamten Text pro Frame durch. Außerdem `const_cast<char*>(s_log.c_str())` auf einen `std::string`.
- **Lösung**: Log als `std::vector<std::string>` und `ImGuiListClipper` im Child-Fenster, Mutex nur zum Kopieren neuer Zeilen (Cursor-Index) halten. Alternativ `TextUnformatted` in `BeginChild` (hat einen Clipping-Fastpath).
- **Erwarteter Effekt**: Nur Dev-Tool; vermeidet lange Frames bei großen Publish-Läufen.
- **Risiko / Verifikation**: Auswahl/Copy im Log geht mit dem Child verloren (`InputText` erlaubte Strg+C); ein Copy-Button ergänzen.

### EDT-817 – `EngineContentSyncBar`: `status()` zweimal je Frame, mit Kopie und Filename-Berechnung
- **ID**: EDT-817
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `EngineContentSyncBar.cpp:337` und `356`, `313-329`
- **Problem**: `FooterWidth` und `DrawFooter` rufen je `EngineContentSync::instance().status()` (liefert `DownloadQueueStatus` per Wert, enthält `currentRelativePath`) und bauen `currentLabel`/`countText` jeweils neu. Auch im Leerlauf (`!st.active`) wird die Statuskopie zweimal je Frame gezogen. Kann zudem zwei verschiedene Zustände sehen (Breite aus Aufruf 1, Zeichnung aus Aufruf 2); die Footer-Kette in EditorUI.cpp hat dasselbe Problem schon einmal erlebt (Kommentar 3539-3543).
- **Lösung**: Einmal pro Frame im Footer `status` holen und beide Funktionen mit `const DownloadQueueStatus&` aufrufen (oder die Bar cached ein `Frame{width,label,count}` zwischen `FooterWidth` und `DrawFooter`).
- **Erwarteter Effekt**: Eine Statuskopie (und bei Aktivität ein Label-Bau) weniger je Frame; verhindert Breitenabweichung.
- **Risiko / Verifikation**: Gering; Schnittstelle (Header) ändern oder intern cachen, damit `EditorUI.cpp` unverändert bleibt.

### EDT-818 – `EditorUndo`: irreführender Header, Vollkopie in `stashPre`, `vector::erase(begin)` und 64 Weltsnapshots
- **ID**: EDT-818
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `EditorUndo.h:17-21`, `122`; `EditorUndo.cpp:154-160`, `180-187`
- **Problem**: (a) Der Header-Kommentar sagt `undo.capturePre(); // before the widget runs, every frame (cheap)`. `capturePre` serialisiert die ganze Welt (`saveToMemory`); die realen Aufrufer rufen es nur auf Maus-Press (`InspectorPanel.cpp:870-878`, `EditorTransformGizmo.cpp:98`, `TerrainTools.cpp:1805`). Der Kommentar lädt dazu ein, es pro Frame aufzurufen. (b) `stashPre` kopiert `m_scratch` in `m_pending` (Zeile 157), obwohl `capturePre` den Scratch ohnehin beim nächsten Aufruf überschreibt. (c) `pushUndo` ruft `m_undoStack.erase(m_undoStack.begin())` bei Überlauf (64 `Entry`-Moves, billig, aber `std::deque` wäre passender). (d) Bis zu 64 Voll-Snapshots der Welt im Speicher (Größe nur nach Messung bewertbar).
- **Code-Auszug**:
```cpp
m_pending      = m_scratch;      // Vollkopie des Weltsnapshots
if (m_undoStack.size() > kMaxEntries) m_undoStack.erase(m_undoStack.begin());
```
- **Lösung**: Header-Text korrigieren ("serialisiert die ganze Welt, nur auf Press aufrufen"). In `stashPre` `m_pending = std::move(m_scratch); m_scratch.clear();` – setzt voraus, dass kein Aufrufer zweimal `stashPre` nach einem `capturePre` macht (Gizmo ruft `capturePre(); stashPre()` zusammen, Details-Panel getrennt, prüfen). Stack-Container auf `std::deque`. Speicher: Snapshot-Größe und Anzahl messen; ggf. Byte-Budget statt fester 64.
- **Erwarteter Effekt**: Eine Weltkopie weniger je Edit-Start (Schätzung: Größe des CBOR-Snapshots, bei großen Szenen MBs); Header-Fix verhindert Fehlgebrauch.
- **Risiko / Verifikation**: Mittel für den Move-Umbau (Scratch-Wiederverwendung). Test: zwei Edit-Sessions hintereinander ohne dazwischenliegendes `capturePre`.

### EDT-819 – `EditorViewportNav`: Diagnose-Logging im produktiven Eingabepfad
- **ID**: EDT-819
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorViewportNav.cpp:274-304`
- **Problem**: Ein "Self-diagnostic" wird in jedem `gather`-Aufruf ausgewertet (6 `IsKeyDown`/`IsMouseDown`-Abfragen) und loggt per `HE_LOG_INFO` alle 60 Frames, solange W/S oder RMB gehalten werden, also bei jedem Kameraflug. Das war für eine Feldmeldung gedacht ("WASD stopped working after joining a session on Windows"), läuft aber dauerhaft und füllt das Console-Log.
- **Lösung**: Hinter `HE_NAV_DIAG`/Umgebungsvariable (einmal gelesen wie in `EditorUiShot.cpp:liveAfterRender`) oder Log-Level `Debug` stellen; oder nach Behebung der Meldung entfernen. Die Eingabestate-Abfrage wird dabei nur im Diagnosefall ausgewertet.
- **Erwarteter Effekt**: Weniger Log-Rauschen; Mikro-Gewinn pro Frame.
- **Risiko / Verifikation**: Keines funktional.

### EDT-820 – `GitController`: Wiederholte Guards, Kopien pro Frame in `setTeamServer`
- **ID**: EDT-820
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `GitController.cpp:494-502`, `189-227`, `160-204`
- **Problem**: (a) `setTeamServer` wird laut Header (225) jeden Frame aufgerufen; `m_teamServerTeam = on ? team : std::string();` (497-498) legt je Aufruf ein Temporär an und kopiert (Heap, sobald >15 Zeichen), obwohl sich der Wert fast nie ändert. (b) Fünf Methoden wiederholen `if (!mayModify() …) return; if (HE::ProjectWriteGate::refuse("…")) return;`. (c) Die Reward-Arming-Sequenz (`m_service.busy()` / `m_syncWatch.requested(kSyncCommit | (push && !remoteUrl().empty() ? kSyncPush : 0))`) steht identisch in `requestCommitAll` (169-172) und `requestCommitStaged` (200-203).
- **Lösung**: (a) `if (m_teamServer == on && m_teamServerTeam == team && m_teamServerProject == project) return;` am Anfang (bei `!on` genügt `m_teamServerTeam.empty()` Prüfung). (b) kleine private Methode `bool mayRewrite(const char* what)`. (c) `armSyncWatch(bool push)`.
- **Erwarteter Effekt**: Zwei Heap-Allokationen weniger je Frame, -15 Zeilen.
- **Risiko / Verifikation**: Gering; bestehende Source-Control-Tests laufen lassen.

### EDT-821 – `render()` kopiert den Projektpfad jeden Frame; Fenster-Pins vor geschlossenen Popups
- **ID**: EDT-821
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `EditorUI.cpp:614-621`; `2671`, `2737`, `3119-3122`; `ExportDialogPanel.cpp:549-551`; `EngineContentPublishDialog.cpp:193-194`; `GitCloneDialog.cpp:309-310`
- **Problem**: (a) `const std::string project = (… ) ? ctx.projectManager->currentProject().path : std::string();` kopiert den Pfad jeden Frame (Heap bei >15 Zeichen) nur zum Vergleich mit `s_graphViewProject`. (b) Mehrere Modale rufen `SetNextWindowSize`/`pinDialogToEditorWindow` (und im New-Project-Fall zwei `PushStyleVar`) in jedem Frame, auch wenn das Popup zu ist; `BeginPopupModal` verwirft die NextWindow-Daten dann (`vendor/imgui/imgui.cpp:13224`), es ist also korrekt aber nutzlos. Der Kommentar 2623-2625 begründet für „About“ das Gegenteil (Hazard) und gilt nur, weil die dortige Bedingung davor steht; Stil uneinheitlich.
- **Lösung**: (a) `const std::string& project = … ? currentProject().path : kEmpty;` (statisches leeres `std::string`). (b) Pin und Style-Pushes nur in `if (ImGui::IsPopupOpen(id))` ziehen, einheitlich für alle fünf Dialoge.
- **Erwarteter Effekt**: Eine Allokation und ~10 ImGui-Calls weniger je Frame; praktisch nicht messbar, Konsistenzgewinn.
- **Risiko / Verifikation**: `IsPopupOpen` mit String-ID hasht im aktuellen Fenster; im Root-Kontext aufrufen (siehe Kommentar `Export` 63-65).

### EDT-822 – Test-Rasterizer in Editor-Target
- **ID**: EDT-822
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorUiShot.cpp:19-21`
- **Problem**: Produktions-Editor inkludiert `"../../tests/ImGuiSoftwareRaster.h"` (Kommentar: "linked into HorizonEditor for these witnesses"). Der Release-Editor zieht damit eine Testbibliothek und deren Include-Pfad mit; `liveAfterRender` kostet pro Frame zwei statische Env-Prüfungen und einen Funktionsaufruf auch ohne gesetzte Variablen.
- **Lösung**: Hinter eine CMake-Option (`HE_EDITOR_UI_SHOT`, Default ON in Debug/Dev-Builds) stellen, `tests/ImGuiSoftwareRaster.*` in ein eigenes kleines Target auslagern, das Editor und `he_tests` linken.
- **Erwarteter Effekt**: Sauberere Abhängigkeiten, kleinerer Release-Editor (Schätzung; Größe unbekannt).
- **Risiko / Verifikation**: Skripte (`scripts/he_team_presence_shots.py`) brauchen den Release-Editor mit Rasterizer; CMake-Flag entsprechend dokumentieren.

### EDT-823 – Kleinkram: schwere Header, Duplikate, veraltete Kommentare
- **ID**: EDT-823
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: mehrere
- **Problem**:
  - `EditorWidgets.h:2-7` inkludiert `<Types/Enums.h>`, `<Types/UUID.h>`, `<unordered_map>` in jedes Panel (für `AssetDrop` und `MixedScope::m_fields`); `GitController.h:11-23` zieht GitHubApi/GitService/RepoStatus/EditorRewards, `<thread>`, `<mutex>` in alles, was `EditorApplication.h` (AppContext) einbindet. Mittel durch Forward-Declarations/PIMPL reduzierbar (`MixedScope` hält den Zustand per `std::unique_ptr<Impl>` oder Zeiger).
  - `severityOf(line)` in `GameLogicBuildPanel.cpp:99-104` und dasselbe Lambda in `ExportDialogPanel.cpp:1853-1855`; `editorBaseDir()` (`GameLogicBuildPanel.cpp:91-95`) ist `SDL_GetBasePath() ? path : {}` aus `ExportDialogPanel.cpp:719/1450/1001` als vierte Kopie. Zusammen nach `BuildProgressDialog` (`severityOfLine`, `editorBaseDir`).
  - Dasselbe `std::thread` + `atomic running/finished` + Reaper-Muster in `ExportDialogPanel.cpp:259-264`, `GameLogicBuildPanel.cpp:76-85`, `EngineContentPublishDialog.cpp:68-77` mit je eigener Join-Funktion in `EditorUI::joinPending*`; ein `BackgroundJob`-Helfer würde die drei Join-Regeln bündeln.
  - Veraltet: `GameInstancePanel.h:11` ("Togglable from the View menu", ist Window-Menü/Tab); `EngineContentSyncBar.h:11-12` ("Only declared when HE_HAVE_LIBSSH2", ist unbedingt deklariert); `ExportDialogPanel.cpp:5`+`EditorUI.cpp:1058` "fourteen panels"; Lambda-Parameter `(void)ctx` in `EngineContentPublishDialog::Draw` und `SyncBar::FooterWidth` (ungenutzt).
  - Muster `HE_LOG_WARN(Editor, "%s", ("…" + s).c_str())` (EditorWidgets.cpp:84, EditorUI.cpp:1442, ExportDialogPanel.cpp:342 u.a.) baut einen `std::string` für ein printf-Format, das `%s` selbst könnte; nur Warn-/Fehlerpfad.
  - `FrustumLines.h:206` benutzt `3.14159265358979f / 360.0f` statt `glm::radians(fovDeg) * 0.5f`.
  - `ExportDialogPanel.cpp:97-100` `exportBackendChoices` ist ein Ein-Zeilen-Wrapper um `HE::BackendRules::choicesFor`, der bei offenem Combo je Frame einen `std::vector` allokiert.
- **Lösung**: wie jeweils genannt; jede Position separat und billig.
- **Erwarteter Effekt**: Weniger Rekompilierzeit (Header, Schätzung), weniger Duplikat.
- **Risiko / Verifikation**: Gering; Rekompilieren und Tests.

## Gelesen
Alle 26 Dateien des Slice vollständig gelesen: ja (EditorUI.cpp/.h, EditorUiShot.cpp/.h, EditorUndo.cpp/.h, EditorViewportNav.cpp/.h, EditorWidgets.cpp/.h, EngineContentPublishDialog.cpp/.h, EngineContentSyncBar.cpp/.h, EnvironmentPanel.cpp/.h, ExportDialogPanel.cpp/.h, FrustumLines.h, GameInstancePanel.h, GameLogicBuildPanel.cpp/.h, GitCloneDialog.cpp/.h, GitController.cpp/.h); außerhalb des Slice nur gezielte Stichproben (`EditorPanelState.h`, `EditorRowWidgets.cpp`, `PlayErrorReveal.h`, `RepoStatus.h`, `ProjectHubPanel.cpp`, vendor `imgui.cpp:13217`).

## Editor Slice 9 – GitHubSignIn* · GitMissingDialog · GraphEditor* · GraphViewStore · Guides/* · Hc* (EditorUtil, ExecTrace, Extract, ExtractUi, ExtractUiBind, FallbackReport, GraphClipboard)

## Zusammenfassung

Der Slice ist insgesamt sauber und gut kommentiert; die Logik-Dateien (HcExtract, HcFallbackReport, HcGraphClipboard, GraphViewStore, GitHubSignInView) sind klein, ImGui-frei bzw. testbar und haben kaum Befunde. Die relevanten Probleme sitzen in den Per-Frame-Pfaden: (1) `HcEditorUtil` ruft in Draw-Callbacks Dateisystem-Scans und Asset-/JSON-Loads **jeden Frame** auf (Pin-Dropdowns, Typ-Picker), (2) `GraphEditor::draw` ruft pro Knoten und Frame ein Dutzend `std::function`-Callbacks mit by-value-Rückgaben (Vektoren/Strings) ohne Viewport-Culling, gegen einen Host mit linearem `findNode` (O(N²)), (3) mehrere Panels kopieren pro Frame `TypeRegistry`-Definitionen und `signatureOf`-Vektoren. Dazu kommt der Exec-Listener in `HcExecTrace`, der pro ausgeführtem Knoten Strings kopiert und hasht. Funde: 3 Hoch, 8 Mittel, 7 Niedrig (18 gesamt).

---

### EDT-901 – Dropdown-Listen für String-Pins werden jeden Frame neu berechnet (Verzeichnisbaum-Scan, Header-Sniffing, JSON-Parse)
- **ID**: EDT-901
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/HcEditorUtil.cpp:1749-1757` (Aufruf), `:1805-1889` (`engineParamChoices`), `:63-131` (`scanAssetsInto`, `listScenes`); Aufrufer `src/HE_Editor/HcGraphHost.cpp:899-905`
- **Problem**: `drawPinDefaultEditor` ermittelt die Optionsliste *vor* `BeginCombo`, also jeden Frame für jeden sichtbaren, unverdrahteten String-Pin, der eine Liste hat. Je nach Parameter bedeutet das: `listScenes` = rekursiver `recursive_directory_iterator` über das ganze Projektverzeichnis; `listAssets` = rekursiver Scan beider Content-Roots plus Datei-Öffnen jedes `.hasset` (Header lesen) plus `file_size`; `save.*` = `loadAsset` + `structFromJson` (JSON-Parse). Der Canvas ruft das über `drawPinInlineEditor` für jeden Knoten im Graphen auf (kein Culling, s. EDT-903). Ein Graph mit zehn `scene.load`/`audio.play`-Knoten macht damit pro Frame zehn komplette Projekt-Scans. Dass das ein bekanntes Muster ist, zeigt der Kommentar in `EditorWidgets.cpp:121-126` („scanned once, when the popup opens … wasteful sixty times a second“) – hier fehlt genau diese Vorsicht.
- **Code-Auszug**:
```cpp
const std::vector<std::string> opts = choices ? choices(n, pd.name)
                                              : std::vector<std::string>{};
if (opts.empty()) { ImGui::InputText("##pd", &v.s); ... }
...
const bool open = ImGui::BeginCombo("##pd", ...);
if (open) { for (const std::string& o : opts) ... }
```
- **Lösung**: (1) Liste nur holen, wenn der Combo geöffnet ist; die Frage „gibt es überhaupt eine Liste“ (Text-Box vs. Dropdown) über eine billige Funktion `engineParamHasChoices(node, param)` beantworten (reine String-Vergleiche, kein I/O). (2) Die teure Liste in einem Editor-weiten Cache halten (Schlüssel: Asset-Typ bzw. „scenes“, invalidiert über die vorhandene `contentRefreshPending`/`EditorAssetTypeCache::invalidate`-Mechanik oder einen Generationszähler des Content-Browsers). Skizze:
```cpp
if (ImGui::BeginCombo("##pd", cur)) {
    const auto& opts = choiceCache().get(n, pd.name, choices);  // einmal pro Öffnen/Invalidierung
    ...
}
```
- **Erwarteter Effekt**: (Schätzung) Je nach Projektgröße von mehreren ms bis zu dutzenden ms pro Frame auf Graphen mit Engine-Call-Knoten dieser Parameter → praktisch 0 ms im Normalbetrieb.
- **Risiko / Verifikation**: Cache-Invalidierung bei Asset-Anlage/Umbenennung/Löschen muss stimmen (sonst veraltete Auswahl). Die Textbox-Entscheidung (`opts.empty()`) muss bei „Widget ohne Animationen“ (Host-`paramChoices` leer) erhalten bleiben, siehe Kommentar `:1750-1753`; dort bleibt „leere Liste → Textfeld“ ein Sonderfall, der eine Frage an den Host braucht. Test: UI-Test mit Knoten `scene.load` vor/nach Anlegen einer Szene; Profil (Zeit pro Frame) mit 50 `.hasset` / 5000 Dateien.

### EDT-902 – `drawTypePicker` lädt bei geöffnetem Combo jeden Frame alle Klassen-Assets und parst deren Graph-JSON
- **ID**: EDT-902
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/HcEditorUtil.cpp:576-583`, `:490-502` (`listClasses`), `:475-488` (`classInfoForPath`), `:524-587`
- **Problem**: Solange das Typ-Dropdown offen ist (Variablen-, Parameter-, Struct-Feld-Typen), läuft pro Frame `listClasses(cm, …)`: zweimal `listAssets` (HorizonCodeClass + Widget; je zwei Roots, rekursiver Scan + Header-Open pro `.hasset`), danach für **jede** gefundene Klasse `cm->loadAsset(path)` + `HorizonCode::fromJson(graphJson)` (volles JSON-Parsen des Graphen), nur um öffentliche Funktionen/Variablen zu extrahieren – die der Picker gar nicht verwendet (er zeigt nur `c.label` und `c.path`). Zusätzlich kopieren `reg.enums()` / `reg.structs()` pro Frame die Definitionslisten (`:554`, `:564`), und `lc(s)` allokiert pro Eintrag und Frame einen String (`:542`).
- **Code-Auszug**:
```cpp
for (const auto& c : listClasses(cm, nullptr, nullptr))
    if (hit(c.label) && ImGui::Selectable(c.label.c_str(), type == P::Ref && *className == c.path))
```
- **Lösung**: Im Picker `listHorizonCodeClasses(cm)` + `listAssets(…Widget)` verwenden (nur Label/Pfad, kein Parse) statt `listClasses`; beim Öffnen (`ImGui::IsWindowAppearing()`) einmal in einen `static std::vector<ClassRef>` schreiben (wie `s_pickerItems` in `EditorWidgets.cpp:126`) und bei geöffnetem Popup nur noch filtern. `enums()`/`structs()` ebenfalls beim Öffnen einmal kopieren. Der Query-Lowercase einmal pro Frame, Labels vorab kleingeschrieben mitspeichern.
- **Erwarteter Effekt**: (Schätzung) Bei 100 Klassen/Widgets: pro offenem Frame 100 Asset-Loads + JSON-Parses entfallen → Combo stotterfrei; Größenordnung 5–50 ms/Frame gespart (messen).
- **Risiko / Verifikation**: Niedrig. Neu angelegte Klassen erscheinen erst beim nächsten Öffnen (akzeptabel). Prüfen: `tests/` für Picker-Verhalten (Suchfilter `assetMatchesQuery` ist testbar); manuell Combo öffnen, Frametime vorher/nachher.

### EDT-903 – `GraphEditor::draw`: pro Knoten und Frame ~10 `std::function`-Aufrufe mit By-Value-Rückgaben, kein Viewport-Culling, Host-Lookups linear
- **ID**: EDT-903
- **Kategorie**: B/C/D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/GraphEditor.cpp:283-405` (Layout), `:671-914` (Zeichnen), `src/HE_Editor/GraphEditor.h:196-302` (`Model`); Host: `src/HE_Editor/HcGraphHost.cpp:684-699`, `src/HE_Core/include/GraphCommon/GraphModel.h:45-57`
- **Problem**: Der Canvas ist der Hot Path des Visual-Scripting-/Material-/Partikel-/Animator-Editors. Pro Knoten und Frame: `model.getPos`, `model.pins` (liefert `std::vector<Pin>` mit `std::string label` je Pin – ein Heap-Block pro Label > SSO plus der Vektor), `nodeIsReroute`, `nodeBodyHeight` (3×: `:356`, `:85`, `:876`), `title` (liefert `std::string`; 2× pro compact-Knoten, auch für Reroutes `:677`), `headerColor`, `nodeOutline`, plus je unverdrahtetem Input-Pin `pinHasInlineEditor`/`pinInlineEditorWidth` (siehe EDT-904). **Alle** Knoten werden gelayoutet und mit allen Primitiven gezeichnet, auch die weit außerhalb des Canvas-Rects; Kanten ebenfalls (Bezier-Tessellierung pro Link). Der Host (`HcGraphHost`) löst jede ID mit `graph.findNode(id)` auf, das ein linearer Scan über `graph.nodes` ist → pro Frame O(N · Callbacks · N), also bei 1000 Knoten ~10⁷ Vergleiche; `m.links` macht zusätzlich pro Link ein `findNode` (O(L·N)).
- **Code-Auszug**:
```cpp
for (int nid : ids) {
    Drawn d; ... model.getPos(nid, gx, gy);
    d.pins = model.pins(nid);
    d.reroute = model.nodeIsReroute && model.nodeIsReroute(nid);
    ...
    d.gw = d.compact ? compactGraphWidth(model.title(nid), d.pins) : kNodeW;
```
```cpp
m.getPos = [&graph](int id, float& x, float& y){ if (const HC::Node* n = graph.findNode(id)) { ... } };
```
- **Lösung**: (a) Nach dem Layout pro Knoten das Screen-Rect gegen `origin..origin+size` (plus Rand für Pin-Kreise/Outline) testen; außerhalb liegende Knoten nur als `Drawn` ohne Zeichnen/Body/Inline-Editoren/Tooltip behalten (Pins für Link-Anker bleiben). Zeichnen der Links nur, wenn Bounding-Box der Bezier-Kontrollpunkte das Rect schneidet. (b) Titel/Pin-Labels einmal im Layout in `Drawn` ablegen und im Zeichen-Loop wiederverwenden (statt 2. Aufruf). (c) Hostseitig: `GraphModel.h::findNodeById` durch eine pro Frame gebaute `unordered_map<int,const Node*>`/sortierten Index ersetzen oder ein `Model::nodeIndex`-Hook; mindestens im Host eine Frame-lokale Map aufbauen. (d) Längerfristig `pins` als `std::span`/Referenz auf gecachte Pin-Listen (invalidiert über Graph-Edit-Counter) statt Vektoren by value.
- **Erwarteter Effekt**: (Schätzung) Bei großen Graphen (>300 Knoten) Frame-Zeit des Graph-Panels von O(N²)-dominiert auf grob O(sichtbare Knoten); typischer Gewinn bei gezoomtem Ausschnitt 3–10×. Bei <50 Knoten kaum messbar → vorher messen.
- **Risiko / Verifikation**: Mittel. Culling darf Interaktion nicht verändern: Hit-Tests (`pinAt`, `hoverNodeNow`, Box-Select, `frameNodes`) laufen über `nodes` – die Liste muss vollständig bleiben, nur das Zeichnen wird übersprungen; Inline-Editor-Widgets außerhalb des Canvas dürfen ImGui-State nicht verlieren (aktives Textfeld, das aus dem Bild gepannt wird!). Tests: bestehende Graph-/UI-Tests (`tests/test_*_ui.cpp` mit Graph-Canvas), Draw-List-Vertexzahl vor/nach, Pan mit aktivem InputText.

### EDT-904 – `signatureOf` (4 Heap-Vektoren) bis zu 5× pro unverdrahtetem Input-Pin und Frame
- **ID**: EDT-904
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/HcEditorUtil.cpp:1645-1653` (`dataInIndexOf`), `:1669-1691`, `:1693-1788`; Aufrufer im Canvas `src/HE_Editor/GraphEditor.cpp:325-329`, `:375-378`, `:817`, `:835`, `:857`
- **Problem**: `dataInIndexOf` ruft `HorizonCode::signatureOf(n)` auf, das laut `HorizonCode.h:754-761` vier Vektoren allokiert. Pro unverdrahtetem Daten-Input-Pin ruft der Canvas im Layout `pinHasInlineEditor` + `pinInlineEditorWidth` und im Zeichen-Loop nochmals `pinHasInlineEditor` + `pinInlineEditorWidth` + `drawPinDefaultEditor` → 5 Signaturbildungen pro Pin und Frame. Die Header-Kommentare nennen die allokationsfreien Varianten (`signatureCountsOf`, `dataPinDescOf`) bereits für Hot Paths. Zusätzlich `pinDefaultsToSelf` → `HE::api::find(n.s)` (String-Lookup) pro Engine-Call-Pin und Frame (`:1663`).
- **Code-Auszug**:
```cpp
const HorizonCode::NodeSig s = HorizonCode::signatureOf(n);
const int dataIn0 = (int)(s.execIns.size() + s.execOuts.size());
```
- **Lösung**: `dataInIndexOf` auf `signatureCountsOf(n)` + `dataPinDescOf(n, true, di, out)` umstellen (gleiche Antworten, keine Allokation). Alternativ: im Canvas-Layout das Ergebnis (`hasEditor`, `width`) pro Pin einmal in `Drawn` ablegen und im Zeichen-Loop wiederverwenden (halbiert die Aufrufe zusätzlich).
- **Erwarteter Effekt**: (Schätzung) Pro Pin 5×4 Heap-Allokationen weniger pro Frame; bei 500 Pins ≈ 10 000 Allokationen/Frame gespart.
- **Risiko / Verifikation**: Niedrig, solange `dataPinDescOf` dieselbe Pin-Reihenfolge wie `signatureOf().dataIns` liefert (Test: für alle `NodeType`s `dataInIndexOf` alt vs. neu vergleichen; `tests/test_horizoncode*`).

### EDT-905 – Selektions- und Link-Suche im Canvas linear: O(N·S) pro Frame, O(L·N) für Kanten
- **ID**: EDT-905
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/GraphEditor.cpp:674-675`, `:571-585`, `:1213-1237`, `:431-434`, `:110-116`, `:980`, `:1067`
- **Problem**: (a) Pro Knoten im Zeichen-Loop `std::find` über `st.selection`. Nach Strg+A (`:1305`, `st.selection = ids`) ist S = N → N² Vergleiche pro Frame, solange die Selektion besteht. (b) Der Link-Loop löst Quelle und Ziel jeder Kante mit dem linearen `findNode`-Lambda (O(N)) auf, danach `findPin` linear, dann zwei weitere lineare Pin-Scans (Farbe, `isExec`) – O(L·(N+P)) pro Frame; derselbe Block wird beim Wire-Doppelklick wiederholt. (c) `for (int rid : st.selection.empty() ? std::vector<int>{ hn } : st.selection)` kopiert wegen des Ternary-Prvalue die gesamte Selektion (`:980`, `:1067` – letzteres in jedem Frame eines Drag!).
- **Code-Auszug**:
```cpp
const bool sel = st.selected == n.id ||
    std::find(st.selection.begin(), st.selection.end(), n.id) != st.selection.end();
...
const Drawn* sn = findNode(l[0]);
const Drawn* dn = findNode(l[2]);
```
```cpp
for (int sid : st.selection.empty() ? std::vector<int>{ st.dragNode } : st.selection)
```
- **Lösung**: Einmal pro Frame nach dem Layout `std::unordered_map<int,int> indexOf` (id → Index in `nodes`) bauen und `findNode` darauf umstellen; Selektion pro Frame in ein `unordered_set<int>` (oder `std::vector<char>` über Node-Index) umsetzen, wenn `selection.size() > ~8`; in `Drawn` die Pins schon mit Farbe/`isExec` des Ausgangs indexierbar machen (`findPin` liefert Index statt Pointer, dann `pins[i].color/isExec` direkt). Range-for über `const std::vector<int>& src = st.selection.empty() ? single : st.selection` mit vorab angelegtem `single`.
- **Erwarteter Effekt**: (Schätzung) Bei 1000 Knoten + Strg+A: ~10⁶ → ~10³ Operationen für die Selektion; Link-Loop von O(L·N) auf O(L).
- **Risiko / Verifikation**: Niedrig; Reihenfolge der Zeichnung unverändert (Index der `nodes`-Liste nach Raise-Sortierung bauen, **nach** `stable_sort` `:418-429`). Test: Graph-UI-Test mit Select-All, Link-Doppelklick (Treffer-Link identisch).

### EDT-906 – Exec-Listener von `HcExecTrace` kopiert/hasht pro ausgeführtem Knoten Strings und liest die Uhr
- **ID**: EDT-906
- **Kategorie**: C/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/HcExecTrace.cpp:72-82` (`tabKeyFor`), `:84-114`, `:116-131` (`glowOf`), `:162-167` (`hasBreakpoint`), `:195-198`
- **Problem**: `attach` hängt Listener in den Interpreter, die bei **jedem** ausgeführten Exec-Knoten laufen (Play-Modus, Widget-Previews). `recordHit` → `nowSeconds()` (steady_clock-Aufruf) → `recordHitAt` → `tabKeyFor(runtimeKey)` (liefert `std::string` by value: Kopie des Keys bzw. Konstruktion aus `kTabPath`, plus `rfind("level:")`) → `s_hits[std::string]` (Hash des Pfadstrings, Map-Lookup) → `lastRun[nodeId]` (zweiter Hash). Auch der Break-Prädikat-Pfad kopiert (`hasBreakpoint` → `tabKeyFor`), sobald ein Breakpoint existiert (die „common case“-Abkürzung gilt nur bei leerer Menge). Auf der Zeichenseite fragt der Canvas `glowOf` pro Knoten und Frame (je 1× `steady_clock::now()` + String-Hash) und `hasBreakpoint`/`pausedNodeOf` (Kopie + String-Vergleich) – das ist O(N) Uhr-Aufrufe pro Frame.
- **Code-Auszug**:
```cpp
GraphHits& g = s_hits[tabKeyFor(runtimeKey)];
g.lastRun[nodeId] = atSeconds;
```
```cpp
float glowOf(const std::string& tabKey, int nodeId) { return glowAt(tabKey, nodeId, nowSeconds()); }
```
- **Lösung**: `tabKeyFor` als `const std::string&`/`std::string_view`-Variante (Rückgabe auf `runtimeKey` selbst bzw. auf statische Strings) und Zeitstempel einmal pro Frame statt pro Knoten (`glowAt` mit Frame-Zeit aus `ImGui::GetTime()`/Frame-Cache). Für den Listener: letzten `(classKey-Zeiger/Hash) → GraphHits*` cachen (Interpreter ruft meist Folgen aus derselben Klasse auf), `lastRun` ggf. als `vector<float>` indiziert nach Node-ID oder Frame-Zähler statt Zeit-Hashmap.
- **Erwarteter Effekt**: (Schätzung) Pro Exec-Knoten 1–2 Allokationen + 2 Hashes + Uhr-Syscall (vDSO) weniger; relevant nur bei schweren Graphen im Play-Modus. Unsicher/messen: Anteil an der Interpreter-Zeit.
- **Risiko / Verifikation**: Niedrig. Tests existieren (`HcExecTrace` ist laut Header „ImGui-free on purpose: this is what the test drives“); `glowAt` mit explizitem Zeitpunkt bleibt API-kompatibel.

### EDT-907 – `HcExtractUi` parst/kopiert pro Frame (Klassen-JSON, `structs()`, `classVariables` zweimal)
- **ID**: EDT-907
- **Kategorie**: C/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/HcExtractUi.cpp:81-105` (`extractStructOf`), `:212`, `:242-257`, `:360-379`; `src/HE_Editor/HcExtract.cpp:77-87`, `:113-163`
- **Problem**: `drawBindEventHelper` ruft jeden Frame `extractStructOf(targetClassPath)` → `cm->loadAsset` + `resolveClassAsset` (parst Klasse und alle Vorfahren aus JSON) bzw. `fromJson` bei Widgets, solange ein BindEvent-Knoten selektiert ist; der Kommentar „One parse per class and frame, the price HcPullUi pays“ benennt es – der Cache gilt nur *innerhalb* eines Frames. `drawDetails` kopiert pro Frame `TypeRegistry::structs()` (alle StructDefs) auch bei geschlossenem Combo (`:212`), `getStruct` (`:243`, Kopie), und `classVariables(g)` doppelt (`:256` und intern in `rows()` `:116`), jeweils mit vollen `Variable`-Kopien (inkl. `defaultItems`, `structDefaults`). `rows()` ist O(Felder × Map-Einträge) mit `def.findField` pro Paar. `listNote`/`listTooltip`/`drawVariableLine` (`:360-389`) bauen pro sichtbarer Variablenzeile und Frame `extractedTo`-Vektor bzw. `describe`-String, selbst wenn `g.extract.empty()` (Reihenfolge der Guards in `listNote` prüft `extractedTo` vor `g.extract.empty()`).
- **Code-Auszug**:
```cpp
if (s_path == classPath && s_frame == ImGui::GetFrameCount()) return s_struct;
...
const HC::ResolvedClass rc = HC::resolveClassAsset(*cm, classPath);
```
```cpp
const std::vector<HE::StructDef> all = HE::TypeRegistry::instance().structs();
```
- **Lösung**: `extractStructOf` nach (Pfad, Asset-Revision/Dirty-Zähler) cachen statt (Pfad, Frame); `structs()` erst im `if (open)`-Zweig holen; `listNote` zuerst `g.extract.empty()` prüfen (billigster Test zuerst); `classVariables` einmal berechnen und an `rows()` übergeben (Überladung `rows(g, def, vars)`); in `classVariables` nur Name/Typ/Scope kopieren, wenn `decl` sonst nicht gebraucht wird (Struktur `ClassVar` hält die ganze `Variable`).
- **Erwarteter Effekt**: (Schätzung) Parse pro Frame für selektierte BindEvent-Knoten entfällt (JSON-Parse mehrerer KB/Frame); die übrigen Kopien sind klein, aber pro Variablenzeile multipliziert.
- **Risiko / Verifikation**: Mittel für den Cache (Invalidierung bei Speichern/Umbenennen der Ziel-Klasse; `ContentManager` hat vermutlich einen Asset-Zähler – vor Umsetzung prüfen). Tests: `HcExtract` ist reine Logik und in `he_tests` abgedeckt; UI: BindEvent auf Klasse, Struct der Klasse ändern → Anzeige aktualisiert.

### EDT-908 – `TypeRegistry`-Definitionen werden per Frame by value kopiert (`getEnum`, `enums()`, `structs()`)
- **ID**: EDT-908
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/HcEditorUtil.cpp:1034-1037` (`ConstEnum`-Knotenkörper), `:1506-1507`, `:1585-1586`, `:1420`, `:554`, `:564`, `:627`; `src/HE_Editor/HcExtractUi.cpp:212`, `:418`
- **Problem**: `drawLiteralNodeBody` (Knotenkörper jedes `ConstEnum`-Knotens, also N-mal pro Frame im Canvas) kopiert `HE::EnumDef` (Name + Vektor aller Einträge mit Strings) per `getEnum(…, def)`; ebenso `drawStructDefaultEditor` (pro Struct-Feld mit Enum-Typ eine `EnumDef`, plus `StructDef` einmal) und `drawArraySlotsEditor` (pro Array-Element eine `EnumDef`-Kopie!). Bei Enums mit 30 Einträgen ≈ 31 Allokationen pro Knoten/Frame. Die Picker kopieren zusätzlich komplette Listen.
- **Code-Auszug**:
```cpp
HE::EnumDef def;
if (!HE::TypeRegistry::instance().getEnum(n.typeName, def) || def.entries.empty()) ...
const HE::EnumEntry* cur = def.findValue((int)n.f[0]);
```
- **Lösung**: Eine Lesezugriffs-API ohne Kopie anbieten (`const EnumDef* TypeRegistry::findEnum(path)`, stabil bis zur nächsten Registrierung, oder `shared_ptr<const EnumDef>`), im Draw darüber arbeiten. Für Picker-Listen: erst im geöffneten Zweig holen (siehe EDT-902). Falls die Registry thread-safe über Kopie sein muss: Frame-Cache (`unordered_map<path, EnumDef>` mit Generationszähler).
- **Erwarteter Effekt**: (Schätzung) Je Enum-Knoten und Frame zig Allokationen weniger; spürbar nur bei vielen Enum-Literalen/langen Enum-Listen.
- **Risiko / Verifikation**: Niedrig–Mittel (Lebensdauer des Zeigers bei Hot-Registrierung durch Type-Asset-Edit im selben Frame). Test: `tests/` für TypeRegistry + UI-Test Enum-Knoten.

### EDT-909 – Suchmenü/Engine-API-Palette: pro Eintrag und Frame mehrere String-Allokationen, ungeclippt, doppelter Registry-Durchlauf
- **ID**: EDT-909
- **Kategorie**: C/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/HcEditorUtil.cpp:1203-1233` (`searchMenuItem`), `:1235-1264` (`searchMenuEnd`), `:1291-1303` (`apiRowMatches`), `:1306-1367`
- **Problem**: Während die Add-Node-Palette offen ist, läuft pro Frame für jede Registry-Zeile (Hunderte): `apiRowMatches` mit bis zu drei `low()`-Strings (`displayName`, `id`, `category`) wenn die Query nicht leer ist; `std::string(shown) + "##" + fn.id` (Allokation); in `searchMenuItem` `label.substr(...)` + `std::transform` (2. Allokation), `s_nav.keys.push_back(label)` (3.) und bei `searchMenuEnd` zweimal lineares `indexOf` mit String-Vergleichen. `engineApiCategories` wiederholt denselben Durchlauf nochmals. Es gibt kein Clipping (`ImGuiListClipper`), jeder Eintrag wird submitted – das widerspricht der Konvention in CLAUDE.md §4 („Row-heavy panels draw only visible rows“), wenn auch durch das Ranking-Konzept (alle Einträge müssen sich registrieren) erschwert.
- **Code-Auszug**:
```cpp
std::string shown = label.substr(0, label.find("##"));
std::transform(shown.begin(), shown.end(), shown.begin(), ...tolower...);
const int score = rankMatch(shown, s_nav.query);
...
s_nav.keys.push_back(label);
```
- **Lösung**: Lowercase-Anzeigenamen/ID/Kategorie einmal beim Registry-Aufbau (oder lazy statisch) cachen und mit `string_view` arbeiten; Ranking unabhängig vom Zeichnen auf einer vorberechneten Liste (Query ändert sich selten → nur bei `queryDirty` neu ranken, sonst gespeicherte Reihenfolge zeichnen und mit Clipper nur sichtbare Zeilen). `keys` als `vector<const char*>`/Index statt Kopie.
- **Erwarteter Effekt**: (Schätzung) Bei ~500 Einträgen ~1500–2500 kleine Allokationen/Frame gespart; Palette bleibt bei Tippen flüssig. Gering, da nur bei offenem Popup.
- **Risiko / Verifikation**: Mittel – Tastaturlogik (Highlight nächster Frame, `swallowEnter`, `enterQueued`) ist subtil; vor Umbau Tests aus `tests/` für `searchMenu*` (falls vorhanden; sonst zuerst Characterization-Test schreiben).

### EDT-910 – Asset-Picker filtert/bildet Labels pro Frame ohne Clipper (`assetMatchesQuery` allokiert dreimal)
- **ID**: EDT-910
- **Kategorie**: C/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/HcEditorUtil.h:44-68` (`assetMatchesQuery`); Aufrufer `src/HE_Editor/EditorWidgets.cpp:162-190` (außerhalb des Slice, nur Beleg)
- **Problem**: Pro Asset und Frame (Popup offen): `lower(label)`, `lower(path)`, Konkatenation `hay`, `lower(query)` und je Term ein `substr` → ≥5 Allokationen. Der Aufrufer baut zusätzlich `a.label + "##" + a.path` für jedes Item und submitted alle Treffer ohne `ImGuiListClipper`. Bei 5000 Texturen/Meshes sind das >25 000 Allokationen/Frame, obwohl nur ~10 Zeilen sichtbar sind.
- **Code-Auszug**:
```cpp
const std::string hay = lower(label) + '\n' + lower(path);
const std::string q   = lower(query);
...
if (hay.find(q.substr(start, i - start)) == std::string::npos) return false;
```
- **Lösung**: `ClassRef` beim Scan um ein vorab kleingeschriebenes `hay` erweitern; Query einmal pro Änderung in Terme zerlegen (`vector<string_view>`); gefilterte Indexliste nur bei Query-Änderung neu bilden und mit `ImGuiListClipper` zeichnen; Label-ID über `PushID(index)` statt String-Konkatenation.
- **Erwarteter Effekt**: (Schätzung) Popup-Frame von O(N) Allokationen auf O(sichtbar); bei großen Projekten die Differenz zwischen Ruckeln und flüssig.
- **Risiko / Verifikation**: Niedrig; `assetMatchesQuery`-Semantik (alle Terme, case-insensitiv, leere Query = alles) hat laut Header-Kommentar einen Test; beibehalten (zusätzliche Überladung statt Signaturänderung).

### EDT-911 – `GraphEditor::draw` ist eine ~1300 Zeilen lange Funktion mit verflochtenem Zustand
- **ID**: EDT-911
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/GraphEditor.cpp:119-1436`, `src/HE_Editor/GraphEditor.h:100-192` (`State` mit ~30 Feldern)
- **Problem**: Layout, Eingabe-Auflösung (Pins, Knoten, Quick-Spawn, Box-Select, Wire-Doppelklick), Zeichnen und Shortcuts stehen in einer Funktion mit Lambdas, die über Referenzen aufeinander aufbauen (`consumed`, `hoverNodeNow`, `interact`, `kbOwned`, …). Reihenfolgeabhängigkeiten (z. B. „AFTER the loop“ Kommentar `:916-925`, der einen echten Bug dokumentiert) sind nur durch Kommentare gesichert. Das erschwert auch die Optimierungen aus EDT-903/905 (Culling, Index). Außerdem Flag-Zustände in `State` (`dragNode`, `linkSrcNode`, `boxSel`, `panning`, `dragOffNode`, `cueMenu`, …) statt eines expliziten Gesture-Enums – Bedingungen wie `st.linkSrcNode == 0 && st.dragNode == 0 && !st.boxSel` wiederholen sich ~10×.
- **Code-Auszug**:
```cpp
if (!consumed && st.linkSrcNode == 0 && st.dragNode == 0 && !st.boxSel &&
    ImGui::IsMouseClicked(ImGuiMouseButton_Left))
```
- **Lösung**: In Phasen-Funktionen zerlegen, die ein `Frame`-Struct (`origin, mouse, nodes, links, flags`) bekommen: `layoutNodes`, `drawGrid/Links/Nodes`, `resolvePinGrab`, `resolveNodePress`, `handleQuickSpawn`, `handleLinkDrag`, `handleBoxSelect`, `handleShortcuts`, `drawPopups`. `State` um `enum class Gesture { None, NodeDrag, LinkDrag, BoxSelect, Pan }` ergänzen und `idle()` als Helfer statt der Dreifach-Bedingung.
- **Erwarteter Effekt**: Besser testbar/lesbar; Voraussetzung für Culling. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Mittel (Verhaltensregression bei Eingabe-Reihenfolge). Zuerst Characterization-Tests für Gesten (Pin-Grab außen, Overlap-Klick, Doppelklick-Wire, Q/F/Home) in `tests/test_*graph*_ui.cpp`, dann mechanisches Extrahieren ohne Logikänderung.

### EDT-912 – Typ-Label-Zeilen: pro Variablenzeile und Frame Vektor + Strings + `std::filesystem::path` für Stem
- **ID**: EDT-912
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/HcEditorUtil.cpp:663-673` (`typePartName`), `:699-743` (`typeLabelParts`, `addTypeLabel`), `:790-837` (`variableRow`), `:530-534`, `:617-619`, `:1376-1392` (`castTargetLabel`, `assetNodeTitle`)
- **Problem**: Jede gezeichnete Variablenzeile baut `std::vector<LabelPart>` mit bis zu 5 `std::string` (`"Array<"` wird per `std::string(word) + "<"` konkateniert) und konstruiert für Enum/Struct/Ref ein `std::filesystem::path` (Allokation, Parsing) nur zum Extrahieren des Stems; dazu `CalcTextSize` pro Teil. `assetNodeTitle`/`castTitle` (vermutlich im `title`-Callback des Canvas pro Knoten und Frame) tun dasselbe. Für fixe Wörter reichen `const char*`/`string_view`. Ein manueller Stem-Helfer existiert bereits in `HcExtract::structLabel` (`HcExtract.cpp:105-111`) – derselbe Zweck ist so in mindestens drei Varianten implementiert.
- **Code-Auszug**:
```cpp
if (const std::string stem = std::filesystem::path(typeName).stem().string(); !stem.empty())
    return stem;
```
- **Lösung**: Gemeinsamer `std::string_view stemOf(std::string_view path)` (Suche nach letztem `/`/`\\` und `.`) in einem kleinen Header, ohne Allokation; `LabelPart` mit `std::string_view`/`const char*` für feste Wörter; `variableRow` zeichnet über ein kleines `std::array<Part,5>`.
- **Erwarteter Effekt**: (Schätzung) Je Zeile 5–8 Allokationen weniger; nur sichtbar bei langen Variablenlisten.
- **Risiko / Verifikation**: Niedrig; Test `tests/test_hc_*` auf `typeLabel`-Ausgabe („Map<String, Bool>“ etc.) bleibt gültig.

### EDT-913 – Doppelte Pflege: Pin-Glyphen und Typnamen-Tabellen in drei Dateien
- **ID**: EDT-913
- **Kategorie**: A/F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/GraphEditor.cpp:750-794` vs. `src/HE_Editor/HcEditorUtil.cpp:179-222` (Glyphen); `HcEditorUtil.cpp:136-156` (`tooltipTypeName`), `:506-519` (`valueTypeName`), `src/HE_Editor/HcExtract.cpp:19-23` (`kNames[]`); `HcEditorUtil.cpp:47-55` (`pinTypeCombo`); `src/HE_Editor/GraphEditor.cpp:15-35` vs. `MaterialEditorPanel.cpp:135`
- **Problem**: Die vier Pin-Glyphen (Dreieck/2×2-Gitter/3-Punkt/2-Spalten) sind in Canvas und Tooltip handkopiert („The same four shapes the canvas draws“), jede Proportionsänderung muss doppelt erfolgen. Drei Typnamen-Tabellen, von denen `valueTypeName` laut eigenem Kommentar (`:670-671`) schon einmal „{Exec:}“ für Enum-Keys lieferte; `HcExtract::kNames` und `pinTypeCombo` (`(int)type - 1`) hängen an der Enum-Reihenfolge von `PinType` (stiller Bruch bei neuem Enum-Wert). `categoryColor` existiert in `GraphEditor.cpp` und `MaterialEditorPanel.cpp`.
- **Code-Auszug**:
```cpp
static const char* kNames[] = { "Exec", "Float", "Bool", "Int", "String", ... "Double" };
const size_t i = (size_t)t;
```
- **Lösung**: `GraphEditor::drawPinGlyph(ImDrawList*, ImVec2 center, float r, bool exec, Container, ImU32 color, ImU32 keyColor)` in `GraphEditor.h` exportieren und von Tooltip/`variableRow` aufrufen; ein `pinTypeName()` (existiert, `HcEditorUtil.h:219`) als einzige Quelle nutzen (`valueTypeName`, `tooltipTypeName`, `kNames` ersetzen; `static_assert` auf `PinType`-Anzahl). `categoryColor` als `constexpr`-Tabelle/`switch` auf Hash und eine Definition.
- **Erwarteter Effekt**: Weniger Drift-Risiko, ~80 Zeilen weniger.
- **Risiko / Verifikation**: Niedrig; Pixel-Vergleich mit UI-Shot (`HE_UI_DUMP_DIR`) vor/nach.

### EDT-914 – Mehrfache Titel-/Textmessung und Callback-Aufrufe pro Knoten (Layout vs. Zeichnen)
- **ID**: EDT-914
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/GraphEditor.cpp:91-102`, `:359`, `:374`, `:677`, `:719`, `:804`, `:15-35`
- **Problem**: `CalcTextSize` für denselben Pin-Label wird im Layout (`:374`, nur für Input-Labels) und im Zeichnen (`:804`) wiederholt, Titel wird für compact-Knoten zweimal vom Host geholt (`:359`, `:677`) und einmal vermessen (`:93`) plus nochmals (`:719`); `model.title` wird sogar für Reroutes bezogen (`:677`) und nicht verwendet. `categoryColor` baut `std::string` und vergleicht bis zu 17-mal String-Literale hintereinander.
- **Code-Auszug**:
```cpp
const std::string title = model.title(n.id);
if (n.reroute) { ... continue; }
```
- **Lösung**: `title` und `labelW[i]` in `Drawn` ablegen (Layout), Zeichnen liest daraus; `title` erst nach dem Reroute-Zweig holen; `categoryColor` über statische `switch`-Tabelle/`string_view`-Vergleich.
- **Erwarteter Effekt**: Klein; Teil der Bereinigung aus EDT-903.
- **Risiko / Verifikation**: Niedrig.

### EDT-915 – `GitMissingDialog`: pro Frame Plan-Kopie, Satzbau und vollständige Log-Kopie während der Installation
- **ID**: EDT-915
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/GitMissingDialog.cpp:26-45`, `:58-69`, `:142-156`
- **Problem**: `s_plan = ctx.gitProbe->install` kopiert pro Frame und Aufruf die komplette `GitInstallPlan` (Strings); `routeSentence` und `gitInstallLaunch` bauen pro Frame neue Strings/`optional`; `ctx.gitInstallLog()` liefert eine Kopie des gesamten Installer-Logs pro Frame während der Installation (potenziell zehntausende Zeichen), die dann als ein einziges `TextUnformatted` mit Wrap gelayoutet wird (O(Länge) pro Frame). Zwei Remedies (Git + LFS) können im selben Frame gezeichnet werden. Selten sichtbar (git fehlt/Installation läuft) → niedrig.
- **Code-Auszug**:
```cpp
if (ctx.gitProbe) s_plan = ctx.gitProbe->install;
...
const std::string log = ctx.gitInstallLog();
```
- **Lösung**: Plan nur bei Änderung kopieren (Zeiger/Revision vergleichen) bzw. per `const&` verwenden, solange `gitProbe` existiert; Log-Getter mit Revisionszähler oder `std::string_view`/Zeilenliste + `ImGuiListClipper`.
- **Erwarteter Effekt**: Gering; verhindert Ruckeln bei langem winget/brew-Output.
- **Risiko / Verifikation**: Niedrig; `gitInstallLog` ist ein `std::function` des AppContext (Threadsicherheit des Logs beachten, falls vom Worker beschrieben).

### EDT-916 – `GitHubSignIn`: mehrfach Mutex + String-Kopien pro Frame; `wipe()` ohne Garantie gegen Dead-Store-Elimination
- **ID**: EDT-916
- **Kategorie**: C/F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/GitHubSignIn.cpp:37-41`, `:243-290`, `:410-413`, `:429-513`, `:517-569`
- **Problem**: (a) `drawAccountRow` ruft `account()` bis zu 3×, `accountLogin()`, `accountError()` (je Lock + Stringkopie) und `flowVisible()` → `currentView()` (konstruiert `View` mit mehreren Strings, ruft `s_device->prompt()` kopierend) pro Frame; `pump()` läuft pro Frame bis zu 3× (`Draw`, `drawAccountRow`, `drawInline`). Alles klein, aber vermeidbar durch einen Snapshot pro Frame. (b) `wipe()` überschreibt per `std::fill` und `clear()` – dass der Compiler den Store nicht entfernt, ist nicht garantiert (anders als `SecureZeroMemory`/`explicit_bzero`/`memset_s`); außerdem liegen durch `std::move(token)` in Lambda/`runSave(std::string token)` weitere Kopien vor, die nicht überschrieben werden, wenn SSO/Heap umkopiert wurde. Das Token ist laut Header-Kommentar „wiped“, die Zusicherung ist schwächer als dokumentiert (unsicher, ohne Disassembly nicht belegbar).
- **Code-Auszug**:
```cpp
void wipe(std::string& s) { std::fill(s.begin(), s.end(), '\0'); s.clear(); }
```
- **Lösung**: Plattform-Funktion (`SecureZeroMemory`/`explicit_bzero`/`memset_s` bzw. `volatile`-Schleife) in einem Helfer; View/Account einmal pro Frame in `pump()` in einen `static Snapshot` schreiben und alle Zeichenfunktionen daraus lesen.
- **Erwarteter Effekt**: Performance vernachlässigbar; Robustheit der Token-Hygiene.
- **Risiko / Verifikation**: Niedrig. Verifizieren per Release-Disassembly/Test, ob der Store entfernt wird (unsicher).

### EDT-917 – Kleinere algorithmische Schwächen in Aktionspfaden (Clipboard, Extract, Breakpoint-JSON)
- **ID**: EDT-917
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/HcGraphClipboard.cpp:24-45`, `src/HE_Editor/HcExtract.cpp:128-148`, `:187-220`, `src/HE_Editor/HcExecTrace.cpp:201-211`
- **Problem**: `HcClipboard::copy` ist O(S·N) (`g.findNode` linear je ID) plus O(L·S) (`inSel` linearer `any_of` je Link); `paste` serialisiert über JSON hin und zurück (korrekt, aber doppelt kopiert). `HcExtract::autoMapByName` berechnet `lower(cv.decl.name)` und `lower(f.name)` innerhalb der Variablen-Schleife je Feld (O(F·V) Allokationen). `saveBreakpointsJson` kopiert `s_breakpoints` in eine `std::map` nur zum Sortieren. Alles Benutzeraktionen, keine Frame-Pfade.
- **Code-Auszug**:
```cpp
auto inSel = [&](int id) { return std::any_of(tmp.nodes.begin(), tmp.nodes.end(), ...); };
for (const HC::Link& l : g.links) if (inSel(l.srcNode) && inSel(l.dstNode)) ...
```
- **Lösung**: `unordered_set<int>` der kopierten IDs; `lower(f.name)` vor die innere Schleife heben; Sortierung nur der Schlüssel statt der ganzen Map.
- **Erwarteter Effekt**: Nur bei sehr großen Selektionen (>1000 Knoten) messbar.
- **Risiko / Verifikation**: Niedrig; bestehende Clipboard-/Extract-Tests (`tests/`) decken Semantik.

### EDT-918 – Guides: Prosa als C++-Initializer-Listen, teils redundant formuliert
- **ID**: EDT-918
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/Guides/GuidesCharacter.cpp:20-813`, `GuidesFirstSteps.cpp`, `GuidesShipping.cpp`, `GuidesWorld.cpp`; Installation `src/HE_Editor/DocsPanel.cpp:240`, `src/HE_Editor/EditorGuides.cpp:587-612`
- **Problem**: Alle Guide-Seiten werden beim Laden der Docs-Bibliothek (nicht pro Frame, nur wenn die Library geladen wird) als Baum aus `std::vector<Docs::Block>`/`std::string` gebaut; Braced-Init-Listen kopieren jedes `Block` (inklusive geschachtelter `std::vector<Block>` bei `tip/note/warn`) einmal – einmalig, aber bei ~900 Zeilen Prosa Compile-Zeit- und Startkosten. Inhalte sind mehrfach geschrieben: „Additive is local-space only“ (`GuidesCharacter.cpp:275-278` und `:537-543`), „IK has no script nodes“ (`:283-285`, `:707-710`), „Foot IK needs a running game“ (`:286-288`, `:628-635`), „A layer poses from a clip or a blend space, never from a state machine“ (`:279-282`, `:568-570`). Die Datei warnt selbst (`:10-15`), dass veraltete „What does not exist yet“-Sätze das Handbuch verschlechtern – Duplikate verdoppeln dieses Risiko.
- **Code-Auszug**:
```cpp
secs.push_back(section("limits", "Honestly", "What to watch", {
    bullets({ "A layer poses from one clip or one blend space, never from a state " ... }),
}));
```
- **Lösung**: Gemeinsame „Einschränkungen“-Bullets als benannte Konstanten/Hilfsfunktion (`limitAdditiveLocalSpace()`) teilen; mittelfristig Guide-Texte als Datendatei (Markdown/JSON, wie die Website-Kapitel) laden statt in C++ einzukompilieren – verkürzt die Compile-Zeit der vier Dateien deutlich. Beim Installieren `reserve`, `std::move` (vorhanden) beibehalten.
- **Erwarteter Effekt**: (Schätzung) Kleine Einsparung bei Compile- und Start-/Docs-Ladezeit; bessere Pflegbarkeit.
- **Risiko / Verifikation**: Niedrig; Seiten-IDs/Anker (`guides-*`) müssen stabil bleiben (F1-Cross-Links). Prüfen per `scripts/editor_help_audit.py --check` und einem Test, der alle Seiten-IDs/Links auflöst.

---

## Gelesen
Alle 26 Dateien des Slice wurden vollständig gelesen: ja (`GraphEditor.cpp` in vier Blöcken, `HcEditorUtil.cpp` in vier Blöcken, alle Guides-Dateien komplett); zusätzlich gezielte Greps in `HcGraphHost.cpp`, `EditorWidgets.cpp`, `GraphModel.h`, `HorizonCode.h`, `DocsPanel.cpp`, `EditorGuides.cpp` zur Verifikation von Aufrufern.

## Editor Slice 10 – HcGraphHost … InputMappingModel (31 Dateien, 10 964 Zeilen)

## Zusammenfassung

Der Slice ist insgesamt sauber geschrieben und gut kommentiert; die reine Logik (HcRename, HcPull, HcWatch, InputMappingModel, HeprojRegistration) ist ImGui-frei und testbar. Die echten Kosten liegen in zwei Draw-Pfaden, die pro Frame Dateisystem-Scans ausführen (`InputAssetPanel` und die Inline-Pin-Editoren der HorizonCode-Canvas), sowie in mehreren Per-Frame-Neuaufbauten (Klassen-Graph-Parse in `HcPullUi`, Watch-Fenster, `buildModel`-Lambdas mit linearem `findNode`). Daneben gibt es Duplikation (PinType→Name in fünf Kopien, davon eine tot und falsch; Node-Spawn-Code in `HcGraphHost` fünffach; drei identische Tabellen in `InputAssetPanel`) und einen kleinen Logikfehler (Capture wird beim Entfernen von Pad-/Maus-Zeilen nicht abgebrochen). Funde: 2 Hoch, 9 Mittel, 9 Niedrig. Alle Zeilenangaben wurden gegen den aktuellen Stand geprüft; Messdaten liegen nicht vor, Aufwandsschätzungen sind als solche markiert.

---

### EDT-1001 – `InputAssetPanel::render` scannt pro Frame den gesamten Content-Baum
- **ID**: EDT-1001
- **Kategorie**: D (auch E: blockierende I/O im Draw)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/InputAssetPanel.cpp:704 (Implementierung `HcEditorUtil::listAssets`: src/HE_Editor/HcEditorUtil.cpp:63-100)
- **Problem**: Jeder Frame eines geöffneten Input-Mapping-Tabs ruft `listAssets` auf, nur um die Action-ComboBox zu füllen. `listAssets` führt zwei `recursive_directory_iterator`-Läufe (Content + EngineContent) aus und öffnet pro `.hasset` den Header (`readAssetTypeFromFile`) plus `file_size`. Das Ergebnis wird nur gebraucht, wenn die Combo (Zeile 742) offen ist – und auch dann nur in dem einen Frame-Block. Die Liste wird sogar berechnet, wenn der Tab keinen einzigen Entry hat. Der Cache `EditorAssetTypeCache` wird hier nicht genutzt.
- **Code-Auszug**:
```cpp
const auto actions = HcEditorUtil::listAssets(ctx.contentManager, HE::AssetType::InputAction);
...
if (ImGui::BeginCombo("##action", shown.c_str()))
{
    for (const auto& a : actions)
```
- **Lösung**: Den Aufruf in den `if (ImGui::BeginCombo(...))`-Block verschieben (Liste nur im Frame mit offenem Popup bauen). Für flüssiges Verhalten bei offenem Popup optional in `PanelState` cachen und beim Öffnen (`ImGui::IsWindowAppearing`/Combo-Open-Flanke) neu laden.
```cpp
if (ImGui::BeginCombo("##action", shown.c_str()))
{
    const auto actions = HcEditorUtil::listAssets(ctx.contentManager, HE::AssetType::InputAction);
    ...
```
- **Erwarteter Effekt**: Schätzung: eliminiert pro Frame O(Anzahl .hasset) Header-Reads und Directory-Walks; bei großen Projekten (tausende Assets) im Bereich mehrerer ms bis zweistelliger ms pro Frame, solange der Tab sichtbar ist. Messen.
- **Risiko / Verifikation**: Sehr gering; Verhalten identisch, solange die Liste im Combo-Frame frisch gebaut wird. Sichtprüfung der Combo; `tests/` hat kein Input-Panel-UI-Test (ggf. `test_*_ui.cpp` nach Muster ergänzen).

### EDT-1002 – Inline-Pin-Editor fragt pro Frame und Pin die Auswahllisten an (inkl. Verzeichnisscans)
- **ID**: EDT-1002
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/HcGraphHost.cpp:896-907 (Aufrufkette: HcEditorUtil.cpp:1693 `drawPinDefaultEditor`, Zeile ~1763 `choices(n, pd.name)`, `engineParamChoices` ab 1805)
- **Problem**: `drawPinInlineEditor` übergibt ein Lambda, das `h.paramChoices` und `HcEditorUtil::engineParamChoices(node, param, h.content)` aufruft. `drawPinDefaultEditor` ruft dieses Lambda für jeden ungebundenen String-Pin im Draw (nicht erst bei geöffneter Combo) und baut einen `std::vector<std::string>`. Für die Parameter `scene`, `class`, `widgetAsset`, `themeAsset`, `asset` (audio.*) ist die Antwort ein voller Verzeichnis-Walk (`listScenes`: rekursiv über das Projekt-Root; `listAssets`: Content + EngineContent mit Header-Reads), und das für jeden sichtbaren Pin jedes Frames. Die Liste wird erst gebraucht, wenn die Combo aufgeht (`if (open)`), davor nur `opts.empty()` für die Widget-Wahl.
- **Code-Auszug**:
```cpp
HcEditorUtil::drawPinDefaultEditor(*n, pin, committed,
    [&h](const HC::Node& node, const std::string& param)
    {
        std::vector<std::string> c;
        if (h.paramChoices) c = h.paramChoices(node, param);
        if (c.empty()) c = HcEditorUtil::engineParamChoices(node, param, h.content);
        return c;
    });
```
- **Lösung**: (1) `engineParamChoices` in zwei Stufen teilen: `bool hasParamChoices(node, param)` (reine Namens-/Row-Prüfung, billig) und die Listenerzeugung. Der Draw braucht nur die Bool-Antwort für Combo-vs-Textbox, die Liste nur nach `BeginCombo == true`. (2) Alternativ die Antworten mit einem kurzen TTL-Cache (z. B. 1-2 s, Invalidierung bei Asset-Änderung) in `HcEditorUtil` halten. Variante (1) ist exakt verhaltensgleich.
- **Erwarteter Effekt**: Schätzung: pro Frame entfallen N_sichtbare_Pins × (Verzeichnis-Walk). Bei einem Graphen mit mehreren `scene.load`/`spawnClass`-Nodes in einem größeren Projekt typischerweise ms-Bereich pro Frame; wirkt auf jede Graph-Ansicht (Level Script, Class, Widget, Animator). Messen.
- **Risiko / Verifikation**: Niedrig bei Variante (1), solange `hasParamChoices` exakt dieselbe Verzweigungslogik wie die Listenfunktion nutzt (sonst Combo/Textbox-Wechsel). Test: `tests/` zu HcEditorUtil (engineParamChoices) um die Äquivalenz „Liste leer ⇔ !hasParamChoices“ ergänzen. Ob GraphEditor außerhalb des Viewports liegende Nodes überspringt, ist unklar (`GraphEditor.cpp:337-340` baut `Drawn` für alle Ids) – bei Culling wäre der Effekt kleiner. Unsicher / messen.

### EDT-1003 – `HcPullUi::sourceVariables` parst den Klassen-Graph jeden Frame neu
- **ID**: EDT-1003
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/HcPullUi.cpp:53-66
- **Problem**: Der „Cache“ ist nach `ImGui::GetFrameCount()` geschlüsselt und gilt damit nur innerhalb eines Frames – bei jedem neuen Frame wird `loadClassGraph` (→ `loadAsset`, `resolveClassAsset` über die ganze Vererbungskette inkl. JSON-Parse, bzw. `HC::fromJson` für Widgets) erneut ausgeführt. Das passiert für jede im Detailpanel sichtbare Variable mit Quelle „Creator“/„Reference“, jeden Frame. Zusätzlich wird `s_vars` per Wert in `out` kopiert.
- **Code-Auszug**:
```cpp
if (s_path != classPath || s_frame != ImGui::GetFrameCount())
{
    HC::Graph g;
    s_ok = HcGraphHost::loadClassGraph(s_targets.content, classPath, g);
    s_vars = s_ok ? HcPull::publicVariables(g) : std::vector<HcPull::SourceVar>{};
```
- **Lösung**: Cache nach (classPath, Änderungsstempel) statt nach Frame: z. B. ein Zeitfenster (0,5 s wie `syncCreateWidgetPins`) oder ein Versions-/Hash-Wert der Asset-JSON (`graphJson` der geladenen Klasse; für Vererbung die Kette). `out` als `const std::vector<SourceVar>&` zurückgeben statt zu kopieren.
- **Erwarteter Effekt**: Schätzung: spart pro Frame einen Chain-Resolve + JSON-Parse (typisch 0,1-1 ms je nach Graphgröße) solange das Panel mit einer solchen Variable offen ist.
- **Risiko / Verifikation**: Mittel-gering: Ein TTL-Cache zeigt Änderungen an der Quellklasse mit ≤ TTL Verzögerung. Hash der Quell-JSONs vermeidet das. `HcPull`-Tests (`tests/` HcPull) decken die Entscheidungslogik, nicht den Cache; manuell prüfen: Variable in Quellklasse umbenennen → Pull-Zeile aktualisiert sich.

### EDT-1004 – `buildModel`-Lambdas: lineares `findNode` pro Node und Link, `signatureOf`-Vektoren pro Node und Frame
- **ID**: EDT-1004
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/HcGraphHost.cpp:684-697 (`nodeIds`, `getPos`, `title`, `headerColor`, `pins`, `links`), 193-203 (`pinRanges`), 223-254 (`nodePins`); Aufrufer GraphEditor.cpp:283, 320, 337-340, 359, 677
- **Problem**: `HC::Graph::findNode` ist laut `GraphModel.h:40` eine lineare Suche. GraphEditor ruft pro Frame für jeden Node `getPos`, `pins`, `title` (zweimal bei Compact-Nodes), `headerColor`, `nodeOutline` auf – jeweils mit `findNode`: O(N²) pro Frame. Das `links`-Lambda macht zusätzlich pro Link ein `findNode(l.srcNode)`: O(L·N). Außerdem baut `nodePins` pro Node `HC::signatureOf` (4 `std::vector<PinDesc>` auf dem Heap) plus den Ausgabevektor ohne `reserve`; `pinRanges` kopiert dafür ebenfalls die komplette Signatur, obwohl `HC::signatureCountsOf` existiert (wird in 950/977 bereits genutzt).
- **Code-Auszug**:
```cpp
m.links = [&h, &graph]{ std::vector<std::array<int,4>> ls; ls.reserve(graph.links.size());
    for (const auto& l : graph.links) { const HC::Node* s = graph.findNode(l.srcNode);
        if (s && s->subgraph == h.currentGraph) ls.push_back({ ... }); }
    return ls; };
```
- **Lösung**: (a) Im `links`-Lambda einmal eine `std::unordered_map<int,int>` (oder sortierter Vektor) id→subgraph bauen: O(N+L). (b) `pinRanges` auf `signatureCountsOf` umstellen (ohne Allokation). (c) `nodePins`: `out.reserve(total)`. (d) Optional: pro Frame einen id→Node*-Index in `buildModel` aufbauen (gültig bis zur ersten strukturellen Änderung; Mutationen im Frame passieren nur in Callbacks/Menüs, daher Index nach Mutation verwerfen oder nur für die reinen Lese-Lambdas nutzen).
- **Erwarteter Effekt**: Schätzung: bei 300-500 Nodes sinkt die Lookup-Arbeit von ~10⁵-10⁶ Vergleichen pro Frame auf ~10³; die Heap-Allokationen pro Node und Frame (4 Vektoren) halbieren sich. Nur relevant bei großen Graphen. Messen (Profiler an einem 500-Node-Level-Script).
- **Risiko / Verifikation**: (d) ist heikel wegen Zeiger-Invalidierung bei `addNode` (siehe die Kommentare 1348-1350 / 289-292); (a)-(c) sind lokal und risikolos. `tests/` für HcGraphHost (pinRanges/nodePins Äquivalenz mit `signatureCountsOf`) ergänzen.

### EDT-1005 – `canConnect` kopiert den gesamten Graphen
- **ID**: EDT-1005
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/HcGraphHost.cpp:706-708 (Aufrufer GraphEditor.cpp:1139-1141)
- **Problem**: Beim Ziehen eines Wires wird für jeden Pin, den der Cursor betritt, `HC::Graph scratch = graph;` – eine tiefe Kopie aller Nodes (Strings, `params`, `pinDefaults`-Maps), Links, Variablen, Events, Kommentare – angelegt, nur um `connect` probeweise auszuführen. Der Kommentar nennt „once per pin the wire enters“; bei großen Graphen ist das trotzdem ein spürbarer Hitch pro Pin-Wechsel.
- **Code-Auszug**:
```cpp
m.canConnect = [&h, &graph](int oN, int oP, int iN, int iP){
    HC::Graph scratch = graph;
    return hostConnect(h, scratch, oN, oP, iN, iP); };
```
- **Lösung**: Entweder eine reine Prüffunktion in HorizonCode (`Graph::canConnect`, die ohne Mutation arbeitet – `connect` selbst enthält die Typregeln), oder zumindest einen Mini-Graphen mit den zwei beteiligten Nodes + Links der beiden Pins kopieren. Für `adoptForEachElementType/adoptRerouteType/connectWithConversion` (mutieren) wäre die Alternative: Ergebnis pro (oN,oP,iN,iP) im Drag cachen, bis sich der Graph ändert – Pin-Paare werden beim Hovern wiederholt abgefragt.
- **Erwarteter Effekt**: Schätzung: Kopierkosten ∝ Graphgröße (µs bis ms); verschwindet bei Cache/Probe-Variante.
- **Risiko / Verifikation**: Mittel: die Entscheidung muss exakt mit `hostConnect` übereinstimmen (Kommentar 700-705: „cannot drift“). Der Cache-pro-Paar-Ansatz erhält das Verhalten exakt. Test: HcGraphHost-Unit-Test mit Paaren (ForEach/Reroute/Conversion), verglichen mit Scratch-Kopie.

### EDT-1006 – Watch-Fenster baut Snapshot und alle Strings jeden Frame; ungeclippt, `lower()` je Zeile
- **ID**: EDT-1006
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/HcWatchPanel.cpp:102-103, 111, 36-45, 184-221; src/HE_Editor/HcWatch.cpp:219-225, 301-307, 309-399
- **Problem**: Solange das Fenster offen ist, wird pro Frame `HcWatch::variableRows` (Game Instance live) bzw. `HcWatch::build` (angehaltener Run) ausgeführt: `rt.variablesSnapshot()` kopiert die komplette `unordered_map<string,Value>`, danach wird über eine `std::map` sortiert und jeder Wert zu `Row{name,type,value}` formatiert (viele kurze Strings, bei Arrays/Structs/Refs mit `TypeRegistry::getStruct`-Kopien der `StructDef`). In `drawRows` wird nicht geclippt, und `passesFilter` ruft bei aktivem Filter für jede Zeile `lower(r.name)` (Kopie) auf. Im Break-Zustand ändert sich nichts zwischen den Frames, dennoch wird alles neu gebaut („Nothing here is kept between frames“, HcWatch.h:25).
- **Code-Auszug**:
```cpp
const std::vector<HcWatch::Row> live = HcWatch::variableRows(*rt, rt->gameInstance());
drawRows(live, lower(s_filter), "##liveVars");
...
const HcWatch::Snapshot snap = HcWatch::build(*rt, (size_t)s_runIndex);
```
- **Lösung**: (1) Im angehaltenen Zustand den Snapshot cachen, bis sich der Run ändert (Step/Continue ersetzt den Run – dafür braucht es einen Zähler/Handle im Runtime, sonst Cache nach `(runIndex, nodeId, level, instance)`). (2) Im Live-Zustand auf ~10 Hz drosseln (Zeit-Gate) und beim Filtern `ImGuiListClipper` nach dem Filter nutzen. (3) `lower(name)` nur einmal pro Row beim Bauen speichern oder Vergleich ohne Allokation (`std::search` mit `tolower`-Prädikat).
- **Erwarteter Effekt**: Schätzung: pro Frame entfallen O(Variablen) Allokationen/Formatierungen; relevant bei Game-Instance mit vielen/großen Containern oder Structs. Messen.
- **Risiko / Verifikation**: Drosselung ändert das Live-Verhalten leicht (≤100 ms Latenz). Cache im Break-Zustand ist verhaltensgleich, sofern Invalidierung korrekt ist (unsicher, ob `Runtime` einen Versionszähler hat – prüfen). `HcWatch` hat Tests (`build`/`formatValue`), die unverändert bleiben.

### EDT-1007 – `HcRenameSweep::scan`: Vollparse des Projekts auf dem UI-Thread, zweimal pro Klasse, Szenen vierfach kopiert
- **ID**: EDT-1007
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/HcRenameSweep.cpp:30-50, 92-106, 131-171; HcRenameDialog.cpp:107-144
- **Problem**: Nach jedem Rename ruft `requestAfterRename` synchron `classAndDescendants` (löst für **jede** Klasse `resolveClassAsset` über die Kette auf) und danach `scan` auf, das für jede Klasse, jedes Widget, jeden Animator-Sync-Graph `loadAsset` + `HC::fromJson` ausführt – die Klassen also ein zweites Mal. `listHorizonCodeClasses` wird dabei zweimal (Dateisystem-Scan) aufgerufen. Szenen werden komplett gelesen (`stringstream` → `str()` Kopie → `json::parse`), `scene["levelScript"].dump()` und erneut per `fromJson` geparst. Alle geladenen Assets bleiben als Nebeneffekt im ContentManager. Die allermeisten Graphen erwähnen den alten Namen gar nicht.
- **Code-Auszug**:
```cpp
std::stringstream ss; ss << f.rdbuf();
scene = nlohmann::json::parse(ss.str(), nullptr, false);
...
return HorizonCode::fromJson(scene["levelScript"].dump(), out);
```
- **Lösung**: (1) Vorfilter: nur parsen, wenn der JSON-Text `t.oldName` als Teilstring enthält (jeder Treffer in `planGraph` hängt an `n.s`, `pullVar`, Event-/Variablennamen oder `params[].name`, die alle als String in der JSON stehen). (2) `json::parse(f)` direkt aus dem Stream, `levelScript` bei Bedarf als Substring bzw. ohne `dump()`-Roundtrip übergeben. (3) `classAndDescendants` und `scan` über dieselbe Klassenliste/Chain-Auflösung laufen lassen (einmalige Liste, einmaliger Resolve). (4) Optional als Job (JobSystem) mit Fortschritt statt UI-Thread-Blocking.
- **Erwarteter Effekt**: Schätzung: bei Projekten mit hunderten Klassen/Widgets/Szenen Sekunden-Stall → Bruchteil; Vorfilter spart >90 % der Parses.
- **Risiko / Verifikation**: Vorfilter-Risiko: Namen mit JSON-escapten Zeichen (`"`, `\`, nicht-ASCII, falls Serializer `\uXXXX` schreibt) – im Zweifel auf den escapten Namen filtern oder den Filter nur bei reinen `[A-Za-z0-9_ ]`-Namen anwenden. Tests: `tests/` zu HcRename/Sweep um Fälle mit Sonderzeichen-Namen erweitern. Unsicher / messen: tatsächliche Projektgrößen.

### EDT-1008 – PinType→Name-Tabelle fünffach; `HcGraphHost::pinTypeName` ist tot und falsch
- **ID**: EDT-1008
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/HcGraphHost.cpp:205-221 (+ Header HcGraphHost.h:41-42); HcWatch.cpp:51-70 (`scalarTypeName`); HcPull.cpp:17-38 (`typeWord`, indexbasiert); HcEditorUtil.cpp:383 (`pinTypeName`→`tooltipTypeName`), HcEditorUtil.cpp:506-519 (`valueTypeName`)
- **Problem**: Dieselbe Abbildung steht in fünf Kopien. `HcGraphHost::pinTypeName` hat keinen Aufrufer außerhalb (Grep über `src` und `tests`: nur Definition/Deklaration) und liefert für `Vec3`, `Vec4`, `Transform` – die im Enum (`HorizonCode.h:50-51`) existieren – „Exec“. `HcPull::typeWord` indiziert ein Namensarray mit `(size_t)t` und hängt damit an der Enum-Reihenfolge (Kommentar fehlt, kein `static_assert`). `valueTypeName` in HcEditorUtil kennt `Transform` nicht. Jede neue PinType-Variante muss an fünf Stellen nachgezogen werden – genau der Drift, den das Projekt sonst vermeiden will.
- **Code-Auszug**:
```cpp
const char* pinTypeName(PT t)
{
    switch (t) { case PT::Float: return "Float"; ... case PT::Struct: return "Struct";
                 default: return "Exec"; }
}
```
- **Lösung**: Eine einzige Funktion `const char* HorizonCode::pinTypeDisplayName(PinType)` (oder die vorhandene `tooltipTypeName` in HcEditorUtil als Quelle) mit vollständigem `switch` **ohne** `default` (damit `-Wswitch` neue Varianten meldet); die vier anderen Stellen rufen sie auf. `HcGraphHost::pinTypeName` löschen (Header + Definition).
- **Erwarteter Effekt**: ~60 Zeilen weniger, ein latenter Anzeigefehler (Vec3/Vec4/Transform) entfällt.
- **Risiko / Verifikation**: Gering. Beachten: die Varianten unterscheiden sich in der Schreibweise („Ref“→„Object“ überall gleich?). Vor der Zusammenführung Ausgabe aller PinTypes der vier Tabellen vergleichen; Test, der über alle Enum-Werte iteriert (vorhandene HcWatch-/HcPull-Tests laufen lassen).

### EDT-1009 – Node-Spawn-Code in `HcGraphHost` mehrfach kopiert; `drawPinDragMenu` über 320 Zeilen
- **ID**: EDT-1009
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/HcGraphHost.cpp:1069-1081, 1458-1463, 1589-1594, 2320-2326 (Engine-Call-Node bauen; zusätzlich HcNodeReference.cpp:185-190); 1210-1219, 1612-1622, 2134-2145, 2351-2359 (Get/Set-Variable-Node an Variable binden); 1341-1661 (`drawPinDragMenu`), 1055-1334 (`drawAddMenuTail`)
- **Problem**: Das Befüllen eines `EngineCall`-Nodes (`s`, `hasArg`, `params`, `results`) steht fünfmal, das Übertragen einer `Variable` auf einen Get/Set-Node (`propType`, `isArray`, `container`, `keyType`, `keyTypeName`, `typeName`) viermal – die Kommentare „The container half travels with the type…“ wurden dafür bereits mehrfach kopiert. Das ist fehleranfällig (die Quick-Pick-Variante schreibt `{}` als vierten `params`-Wert, die anderen nicht). Die Funktionen `drawPinDragMenu`/`drawAddMenuTail` sind eine Kette gleichförmiger Abschnitte mit denselben Mustern (`matches`, `searchMenuItem`, `addNode`, `wire`, `created`, `CloseCurrentPopup`), zum Teil als einzeilige Monster (Zeilen 1435-1437).
- **Code-Auszug**:
```cpp
nn->s = fn.id; nn->hasArg = fn.isExec;
nn->params.clear(); nn->results.clear();
for (const auto& p : fn.params)  nn->params.push_back({ p.name, p.type, p.isArray });
for (const auto& r : fn.results) nn->results.push_back({ r.name, r.type, r.isArray });
```
- **Lösung**: Zwei kleine Helfer im anonymen Namespace: `HC::Node* spawnEngineCall(Graph&, const ApiFn&, pos, subgraph)` (liefert id) und `void bindToVariable(HC::Node&, const HC::Variable&)`; ggf. `HcEditorUtil`/HorizonCode bietet `makeEngineCallNode(const ApiFn&)`, das dann auch HcNodeReference nutzt. Danach die Menüabschnitte als `drawRefMembers()`, `drawStructFields()`, `drawGenericNodes()`, `drawEngineCalls()`, `drawVariables()`, `drawFunctions()` herausziehen.
- **Erwarteter Effekt**: Schätzung: −80 bis −120 Zeilen, ein Ort für Container-/Typregeln.
- **Risiko / Verifikation**: Gering-mittel: reine Umstrukturierung; auf Abweichungen achten (Quick-Pick vs. Add-Menü bei `params` mit/ohne `typeName`). Vorhandene HcGraphHost-/Menü-Tests laufen lassen; falls keiner den Add-Menü-Pfad abdeckt: Snapshot-Test der erzeugten Nodes.

### EDT-1010 – `InputAssetPanel::render` ist ein 750-Zeilen-Monolith mit dreifach kopierten Tabellen und Magic-Ints
- **ID**: EDT-1010
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/InputAssetPanel.cpp:464-1213 (Tabellen: 775-794, 803-829, 844-867; valueType-Ternaries: 346-348, 439-441, 479-480, 759-760; Kopfzeile 9)
- **Problem**: (1) Keys-, Pad- und Maus-Tabelle sind dieselbe Struktur (3 Spalten, TableSetup, Schleife, PushID, Bind-Zelle, Remove-Button) in drei Kopien. (2) Der Wert `valueType` ist ein `int` mit den Bedeutungen 0/1/2/-1/-2; die Abbildung „json → int“ steht zweimal (479, 759), „int → "Axis2D"/"Axis"/"Button"“ zweimal (346, 439), und die Entscheidung „bietet Buttons/Axes an“ steht in `InputMappingModel.cpp:140-141` und in `InputAssetPanel.cpp:1039-1041` doppelt. (3) Die `include`-Zeile 9 trägt zwei verschmolzene Kommentare („danger buttons for deletion     // shared per-tab state map“). (4) `render` enthält `drawAxisTable` (~130 Zeilen) und den Add-Binding-Popup (~120 Zeilen) als Lambdas.
- **Code-Auszug**:
```cpp
st.valueType = HE::inputActionIsAxis2D(a->json) ? 2
             : HE::inputActionIsAxis(a->json)   ? 1 : 0;   // 479
...
e.valueType = HE::inputActionIsAxis2D(a->json) ? 2
            : HE::inputActionIsAxis(a->json)   ? 1 : 0;    // 759
```
- **Lösung**: `enum class ActionType { Unresolved=-1, Missing=-2, Button=0, Axis=1, Axis2D=2 }` in `InputMappingModel.h` samt `actionTypeOf(json)`, `actionTypeName(type)` und `offers(type)`-Helfern (testbar, ImGui-frei); `drawBindingTable(const char* id, std::vector<std::string>&, rowDrawer)` für die drei Listen; `drawAxisTable`/`drawAddBindingPopup` als freie Funktionen in einem eigenen Anonymous-Block.
- **Erwarteter Effekt**: Schätzung: −120 Zeilen, `render` unter 250 Zeilen; die Typregel („welche Bindings bietet die Action an“) wird testbar.
- **Risiko / Verifikation**: Gering; Dateiformat unverändert (`makeInputActionJson`). `tests/` für InputMappingModel erweitern (Typ→Angebot). UI per `HE_UI_DUMP_DIR`-Dump vor/nach vergleichen.

### EDT-1011 – Capture wird beim Entfernen einer Pad-/Maus-Zeile nicht abgebrochen
- **ID**: EDT-1011
- **Kategorie**: F (Logikfehler, klein)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektheit)
- **Ort**: src/HE_Editor/InputAssetPanel.cpp:685-688 (Regel), 795 (Keys: ok), 830-831 (Pad), 868-869 (Maus)
- **Problem**: Der Kommentar bei 685 sagt, jede strukturelle Änderung müsse eine laufende Capture verwerfen, weil sie auf `(entry, sub)` zeigt. Beim Entfernen eines Keys (795), einer Achse (1032-1033) und eines Entries (1207) geschieht das; beim Entfernen eines Gamepad-Buttons und eines Maus-Buttons nicht. Ist gleichzeitig ein Bind für `PadButton`/`MouseButton` mit höherem Index armed, landet der nächste Druck in der verschobenen Zeile.
- **Code-Auszug**:
```cpp
if (removeKey >= 0) { e.keys.erase(...); st.dirty = true; cancelCaptureForThisAsset(); }
...
if (removePadButton >= 0)
{ e.gamepadButtons.erase(e.gamepadButtons.begin() + removePadButton); st.dirty = true; }
```
- **Lösung**: `cancelCaptureForThisAsset();` an beiden Stellen ergänzen – oder, besser, in einem lokalen `eraseAt(vec, idx)`-Helfer kapseln, der `dirty` setzt und die Capture abbricht (löst gleichzeitig die Duplikation aus EDT-1010).
- **Erwarteter Effekt**: Verhindert Fehlbindung; keine Performance-Wirkung.
- **Risiko / Verifikation**: Sehr gering. Test: ImGui-frei nicht prüfbar; manuell: Pad-Zeile 2 Bind armieren, Zeile 1 entfernen → Capture muss disarmed sein.

### EDT-1012 – Add-Menü: viele Kleinallokationen pro Frame bei offenem Popup
- **ID**: EDT-1012
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/HcGraphHost.cpp:1060-1062, 1067-1068, 1113-1121, 314-327, 1150, HcGraphShortcuts.cpp:37-42
- **Problem**: Solange das Add-Menü offen ist, läuft pro Frame: `matches` lowercased bei jedem Aufruf **zwei** Strings (`lower(name)`, `lower(cat)`); für jede der ~10 Kategorien wird die komplette `nodeRegistry()` durchlaufen und pro Node `std::string(HC::nodeCategory(t)) != cat` (Allokation) gebildet; `apiGroups` wird als Vektor kopiert; `offerableFunctions` kopiert komplette `HC::Node`-Objekte (mit `params`, `results`, `pinDefaults`) und wird in Add- und Drag-Menü je Frame erneut aufgerufen; `hintFor` ist linear. Außerdem allokiert `aliasHit` je Aufruf eine lowercase-Kopie des Alias-Strings.
- **Code-Auszug**:
```cpp
if (std::string(HC::nodeCategory(t)) != cat) continue;
if (!matches(HC::nodeDisplayName(t), cat) && !aliasHit(t, q)) continue;
```
- **Lösung**: `std::strcmp(HC::nodeCategory(t), cat) != 0` (Include `<cstring>` ist schon da); Registry einmal nach Kategorie gruppieren (statisch, `std::unordered_map<const char*,…>` oder sortierter Vektor); `matches(name)` mit vorab kleingeschriebenem Namen; `offerableFunctions` als Vektor von Zeigern zurückgeben, solange kein `addNode` zwischen Aufbau und Nutzung passiert (Kommentar 289-292 beachten: Zeiger nur für `variables`, nicht für `nodes` sicher → dort bei Kopie bleiben, aber nur `s`/`params` kopieren).
- **Erwarteter Effekt**: Schätzung: von ~Tausenden kleinen Allokationen pro Frame (Registry-Größe × Kategorien) auf wenige; nur beim offenen Menü relevant, daher Niedrig.
- **Risiko / Verifikation**: Gering. Reihenfolge der Einträge muss erhalten bleiben (Registry-Reihenfolge).

### EDT-1013 – `NodeDocs::engineCall`: lineare Suche über ~526 Zeilen, String-Kopie pro Aufruf
- **ID**: EDT-1013
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/HcNodeDocs.cpp:2192-2237, 18 (Tabelle `kDocs`, 526 Einträge); Aufrufer HcEditorUtil.cpp:325, HcNodeReference.cpp:204
- **Problem**: `engineCall` vergleicht `id` gegen jeden Eintrag (`id == r.id` baut je einen `string_view` über `strlen`), gibt eine `std::string`-Kopie zurück, und `hasEngineCall` baut den Text nur, um `empty()` zu prüfen (für `env.*` inklusive Zusammenbau mit Weather-Satz). Der Aufruf kommt aus dem Tooltip-Draw (HcEditorUtil.cpp:325) und aus dem Reference-Build (pro `ApiFn` einmal: O(n²) = 526 × Registry).
- **Code-Auszug**:
```cpp
for (const Row& r : kDocs)
    if (id == r.id) return r.doc;
```
- **Lösung**: Tabelle alphabetisch sortieren und per `std::lower_bound` suchen (mit `static_assert`/Test auf Sortierung), Rückgabetyp `std::string_view` für die statischen Zeilen (nur der `env.*`-Fall braucht einen String), `hasEngineCall` ohne Textbau. Der Registry-Reihenfolge-Kommentar (Zeile 13) gilt dann nur noch für die Dokumentation; die Reference-Seite liest nach Registry, nicht nach Tabelle.
- **Erwarteter Effekt**: Schätzung: µs-Bereich; nur beim Hovern/Reference-Build. Niedrig.
- **Risiko / Verifikation**: Gering; `tests/test_hc_node_docs.cpp` (Drift-Test über `explicitId`) deckt die Tabelle ab. Zeilen 120-2080 der Tabelle wurden nur stichprobenartig gelesen (siehe „Gelesen“).

### EDT-1014 – `HcPull`/`HcPullUi`: `StructDef`-Kopien und String-Bau im Per-Frame-Pfad
- **ID**: EDT-1014
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/HcPull.cpp:46-54, 124-133, 135-192; src/HE_Editor/HcPullUi.cpp:310-311, 323, 345-346, 379, 409
- **Problem**: `fieldOf`, `fittingMembers` und die Member-Combo rufen `TypeRegistry::getStruct(name, def)` – eine Kopie der gesamten `StructDef` (Felder, Strings) – pro Aufruf. In `drawSection` läuft pro Frame: `getStruct` für die Member-Combo (346, auch wenn die Combo zu ist), `HcPull::check` (baut `label`, `path`, Statustext, ruft `fieldOf`/`fitOf`), und bei offener Variable-Combo pro Variable zweimal `fitOf` + zweimal `fittingMembers` (311, 323). Bei `memberMissingVar` zusätzlich `structsWithMember` über alle Structs pro Frame (409).
- **Code-Auszug**:
```cpp
HE::StructDef def;
HE::TypeRegistry::instance().getStruct(chosen->typeName, def);   // pro Frame
const std::string shown = ...;
if (ImGui::BeginCombo("Member", shown.c_str())) {
```
- **Lösung**: `def` erst innerhalb von `BeginCombo` laden; `fittingMembers` einmal je Variable berechnen und wiederverwenden; falls `TypeRegistry` einen Pointer-/Referenz-Getter bietet, diesen verwenden (Lebensdauer beachten). `structsWithMember` nur beim Öffnen der Combo oder bei Änderung von `pullMember` neu rechnen.
- **Erwarteter Effekt**: Schätzung: wenige µs-Zehnerbereiche pro Frame; Detailpanel-only, daher Niedrig.
- **Risiko / Verifikation**: Gering; HcPull-Tests decken Entscheidungslogik.

### EDT-1015 – Zwei abweichende Kopien der „darf diese Klasse geschrieben werden?“-Regel
- **ID**: EDT-1015
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/HcPullUiBind.cpp:20-28 vs. src/HE_Editor/HcRenameDialog.cpp:47-64
- **Problem**: Der Kommentar in HcPullUiBind.cpp („The rename dialog's rule“) behauptet Gleichheit, die Regeln weichen aber ab: Rename prüft zusätzlich `AnimatorStateMachineEditorPanel::isDirty` und liefert den Namen des Lock-Besitzers; PullUiBind prüft zusätzlich `UIEditorPanel::isDirtyByContentPath` und liefert eine generische Meldung. Sie können weiter auseinanderdriften (neues Panel mit Graph-Tabs → muss an zwei Stellen eingetragen werden).
- **Code-Auszug**:
```cpp
if (UIEditorPanel::isDirty(relPath) || UIEditorPanel::isDirtyByContentPath(relPath) ||
    HorizonCodeClassPanel::isDirty(relPath))
    return "open with unsaved changes";
```
- **Lösung**: Eine gemeinsame Funktion `std::string assetWriteBlockedBecause(AppContext&, const std::string& relPath)` (z. B. in `HcRenameDialog.h` oder `HcEditorUtil`), beide Aufrufer nutzen sie.
- **Erwarteter Effekt**: Eine Quelle für die Regel; verhindert, dass ein neues Graph-Panel nur halb eingetragen wird.
- **Risiko / Verifikation**: Gering; die vereinigte Regel ist strenger (Vereinigungsmenge) – bewusstes Verhalten prüfen.

### EDT-1016 – `HcRename`: `signatureOf` für reine Zählung, lineare Suchen in der Sweep-Schleife
- **ID**: EDT-1016
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/HcRename.cpp:21-25, 27-32, 56-87, 194-200, 244
- **Problem**: `targetPin` baut eine volle `NodeSig` (4 Vektoren) nur für zwei `size()`-Werte (`signatureCountsOf` existiert); `animationPinsOf` macht für jeden EngineCall-Node eine `HE::api::find`-Suche und eine `NodeSig`; `wiredInto` durchläuft für jede Abfrage alle Links; `litUse` ist linear in der Zahl der Literale; `dedupe` ist O(n²). Multipliziert mit der Sweep-Schleife über alle Projekt-Graphen (EDT-1007).
- **Code-Auszug**:
```cpp
const HorizonCode::NodeSig s = HorizonCode::signatureOf(n);
return (int)s.execIns.size() + (int)s.execOuts.size();
```
- **Lösung**: `signatureCountsOf` verwenden (`execIns + execOuts`); bei vielen Treffern einen `dstNode→srcNode`-Index pro Graph (Hash) für `wiredInto` aufbauen; `animationPinsOf` nur ausführen, wenn der Node-String `s` einen Animation-Parameter hat (vorab statisch aus der Registry ableiten).
- **Erwarteter Effekt**: Schätzung: gering; nur während eines Renames.
- **Risiko / Verifikation**: Gering; `HcRename`-Tests vorhanden.

### EDT-1017 – Add-Binding-Popup: ungeclippte ~260 Zeilen, `lower()` je Zeile und Frame
- **ID**: EDT-1017
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/InputAssetPanel.cpp:302-312, 1144-1172, 237-260, 288-296
- **Problem**: `matchesFilter` kleinschreibt bei **jedem** Aufruf beide Strings; für ~240 Tastennamen + Gamepad/Maus/Achsen-Einträge ergibt das pro offenem Popup-Frame ≥ 500 Allokationen plus `label + "##kb"`-Konkatenation je Zeile. `allKeyNames()` ist als „klein genug“ dokumentiert, aber die Kleinschreibung hätte man dort einmalig mitbauen können. Ähnlich bauen `padButtonCombo`/`mouseButtonComboCell` den Anzeigenamen pro Frame und Zeile (`std::string shownName`) und `keyBindCells` ruft `SDL_GetScancodeFromName` pro Key-Zeile und Frame.
- **Code-Auszug**:
```cpp
return lower(label).find(lower(filter)) != std::string::npos;
```
- **Lösung**: Filter einmal pro Frame kleinschreiben; `allKeyNames()` als Paar (Name, Lowercase) halten; bei leerem Filter `ImGuiListClipper`. Für `SDL_GetScancodeFromName` das Ergebnis pro Zeile nur bei `InputTextWithHint`-Änderung neu berechnen (Dirty-Flag je Row, nicht serialisiert).
- **Erwarteter Effekt**: Schätzung: spürbar nur bei offenem Popup auf schwachen Maschinen.
- **Risiko / Verifikation**: Gering.

### EDT-1018 – Hilfsfunktionen mehrfach kopiert; statischer Zustand an Node-IDs gebunden
- **ID**: EDT-1018
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `stem()`: HcPull.cpp:26-28 (typeWord) und 201-204 (describe), HcPullUi.cpp:27-33, HcWatch.cpp:74-81 (insgesamt ~12 Kopien in HE_Editor/*.cpp); `lower()`: HcGraphHost.cpp:403-408, HcWatchPanel.cpp:36-40, InputAssetPanel.cpp:306-309; statischer Zustand: HcGraphHost.cpp:159 (`s_pick`), 1742-1744 (`s_newName`/`s_newFor`), HcPullUi.cpp:404 (`s_structType`), HcWatchPanel.cpp:30-34
- **Problem**: „Pfad → Dateiname ohne Endung“ und „ASCII-lowercase“ sind in jeder Datei neu geschrieben; die Varianten verhalten sich unterschiedlich (HcWatch lässt führenden Punkt stehen: `dot > 0`; HcPull/HcPullUi nicht). `drawEventPicker` hält den Eingabepuffer „neuer Event-Name“ in einer Funktions-Static, die nur über `n.id` zurückgesetzt wird – zwei Tabs mit gleicher Node-ID teilen den Text.
- **Code-Auszug**:
```cpp
static std::string s_newName;
static int s_newFor = 0;
if (s_newFor != n.id) { s_newName.clear(); s_newFor = n.id; }
```
- **Lösung**: `HE::Ed::pathStem()` und `HE::Ed::asciiLower()` in einen kleinen Header (EditorWidgets/HcEditorUtil oder `Core/StringUtil`) und überall verwenden. Event-Namen-Puffer in den Host-/Panel-Zustand (`GraphTabs`/State) verlagern.
- **Erwarteter Effekt**: Weniger Drift, ~60 Zeilen weniger.
- **Risiko / Verifikation**: Gering; Randverhalten der Varianten (führender Punkt) vorher vereinheitlichen und testen.

### EDT-1019 – `HeprojRegistration`: synchrone Datei-I/O beim Editorstart und viel Abstraktion für einen Einmal-Check
- **ID**: EDT-1019
- **Kategorie**: A / E
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Editor/HeprojRegistration.cpp:218-235 (`plan`), 466-490 (`observe`), 540-550 (`upToDate`), 513 (`writeFileAtomic`), 718-747 (`startAtStartup`)
- **Problem**: `startAtStartup` führt `plan()` (also `observe`) auf dem Haupt-Thread vor dem ersten Frame aus. Unter Linux liest das mehrere Dateien komplett (mimeapps.list ×2, .desktop-Vorlage, zwei PNGs byte-weise per `sameBytes` zweimal, `readFile` über `ostringstream` mit Zusatzkopie). Nur das Schreiben läuft im Worker. Unter Windows sind es ~6 Registry-Lesevorgänge. Die Implementierung (Backend- + Registry-Interface, 230-Zeilen-Header, 780 Zeilen) ist für einen einmaligen, nichtkritischen Start-Check groß; sie ist allerdings absichtlich testbar gebaut. Kleinigkeit: `write` überschreibt die `.desktop`-Datei immer (513), die anderen nur bei Unterschied.
- **Code-Auszug**:
```cpp
const Plan p = plan(*backend, enabled);   // Haupt-Thread, Datei-I/O
...
g_worker = std::thread([backend, p] { ... apply(*backend, p) ... });
```
- **Lösung**: Das ganze `plan + apply` im Worker laufen lassen (Ergebnis `LeftAlone` per Callback/Future melden); `sameBytes` zuerst über `file_size` vergleichen; `readFile` ohne `ostringstream`. Den Abstraktionsgrad nicht ändern (Tests hängen daran).
- **Erwarteter Effekt**: Schätzung: wenige ms Startzeit (Disk-Cache-abhängig); Niedrig.
- **Risiko / Verifikation**: Gering-mittel: `StartupReport` ist heute synchron nutzbar (der Aufrufer reagiert auf `LeftAlone` sofort) – Aufrufer anpassen. `tests/test_heproj_*` prüfen.

### EDT-1020 – Kleinigkeiten
- **ID**: EDT-1020
- **Kategorie**: C / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: HcGraphHost.cpp:491-509 und 486; HcNodeReference.cpp:162-206, 213-223; InputMappingModel.cpp:13-26; InputAssetPanel.cpp:493; HcRenameDialog.cpp:159-172
- **Problem**: (a) `syncCreateWidgetPins` kopiert alle 500 ms pro Create-Widget-Node `w->graphJson` (`const std::string json = w->graphJson;`) nur um zu hashen; `static cache` wächst unbegrenzt (Einträge gelöschter Widgets bleiben). (b) `NodeReference::install` legt ~hundert bis tausend `Docs::Section` ohne `reserve` an und berechnet pro Section `signatureOf`; einmalig, daher irrelevant. (c) `decodeMapping`: drei identische „Array von Strings“-Schleifen (`keys`, `gamepadButtons`, `mouseButtons`) → ein Lambda `readStrings(j, name, out)`. (d) `ImGui::Begin(("##inputasset_" + assetPath).c_str(), …)` konkateniert pro Frame einen String; (e) `HcRenameDialog::Draw` fragt pro Frame für alle Entries `blockedBecause` (mehrere Map-Lookups + Strings) und baut drei Vektoren – gewollt („asked again every frame“), aber für große Reports (hunderte Entries) verzichtbar.
- **Code-Auszug**:
```cpp
const std::string json = w->graphJson;
const std::size_t hash = std::hash<std::string>{}(json);
```
- **Lösung**: (a) Hash direkt über `w->graphJson` (`fromJson` lädt nichts nach, der Zeiger bleibt gültig), Cache-Eviction für nicht mehr referenzierte Pfade; (c) Lambda; (d)/(e) `Begin`-ID cachen bzw. `blockedBecause` nur alle 0,5 s neu auswerten.
- **Erwarteter Effekt**: Gering.
- **Risiko / Verifikation**: Gering; zu (a) vorher prüfen, dass zwischen `getWidget` und Hash kein `loadAsset` mehr passiert.

---

## Gelesen

Vollständig gelesen: alle Dateien des Slice mit Ausnahme von `src/HE_Editor/HcNodeDocs.cpp`, von dem nur Zeilen 1-120 und 2080-2244 (Kopf, Tabellenende, Env-Felder, alle Funktionen) sowie per Skript Duplikat-IDs (keine) geprüft wurden; die ~2000 Tabellenzeilen dazwischen sind reine Daten (`{ id, doc }`) und wurden nicht einzeln gelesen. Zusätzlich punktuell gelesen (außerhalb des Slice, zur Belegung): HcEditorUtil.cpp (listAssets, drawPinDefaultEditor, engineParamChoices), GraphEditor.cpp (Model-Aufrufer), GraphViewStore.h, GraphModel.h, HorizonCode.h (PinType, NodeSig), InputMappingModel.h, EditorAssetTypeCache.cpp.

## Editor Slice 11 – InputMappingModel.h … MaterialEditorPanel.cpp

## Zusammenfassung

Slice 11 umfasst 10 Dateien (10 975 Zeilen): `InputMappingModel.h`, `InspectorPanel.{h,cpp}`, `InspectorPrefabKeys.cpp`, `LevelScriptPanel.{h,cpp}`, `MacMenuBar.{h,mm}`, `main.cpp`, `MaterialEditorPanel.cpp`. Der Code ist insgesamt sorgfältig kommentiert; echte Probleme sind selten Algorithmusfehler, sondern Arbeit im Draw-Pfad, die pro Frame wiederholt wird: rekursive Dateisystem-Scans in offenen Popups/Combos, ein quadratisches Model-Adapter-Muster im Material-Canvas, Lua-/Listen-Abfragen im Details-Panel und komplette Komponenten-Auflistungen pro Baumknoten. Strukturell dominieren eine 2 900 Zeilen lange Funktion (`renderForImpl`) mit 41-fach kopiertem Entfernen-Muster und drei parallel gepflegte Komponentenlisten. Es gibt 22 Funde: 2 Hoch, 9 Mittel, 11 Niedrig (nach Schweregrad sortiert, mit Ausnahme von EDT-1114, das als Mittel nachgeordnet steht). Alle Funde stammen aus Draw()-Code (Editor-Hot-Path), die Schweregrade berücksichtigen, ob der Pfad immer oder nur bei geöffneter Sektion/Popup läuft. Gemessen wurde nichts; wo die Wirkung von Datenmengen abhängt, ist das vermerkt.

## Funde

### EDT-1101 – Rekursiver Content-Scan (plus Datei-Header lesen) pro Frame, solange Combo/Popup/Submenü offen ist
- **ID**: EDT-1101
- **Kategorie**: D (auch E: I/O im Draw)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:3206-3219`, `src/HE_Editor/LevelScriptPanel.cpp:3031-3046`, `src/HE_Editor/MaterialEditorPanel.cpp:1317-1327` (Aufrufer-Implementierung: `src/HE_Editor/HcEditorUtil.cpp:55-92`, `93-103`)
- **Problem**: `HcEditorUtil::listAssets()` läuft per `recursive_directory_iterator` über Content- UND Engine-Content-Baum, liest von jeder `.hasset` den Header (`readAssetTypeFromFile`) und ruft `file_size` auf. Drei Stellen rufen das innerhalb des Draw-Codes eines offenen Popups auf, also jeden Frame, solange es offen ist:
  1. Script-Komponente, Klassen-Combo: zusätzlich `loadAsset(c.path)` für JEDE Klasse im Projekt, nur um die Selectable-ID zu vergleichen (die Klassen werden dadurch alle geladen).
  2. HorizonCode-Klassen-Tab, Base-Class-Popup: Scan + pro Klasse `resolveClassAsset(...)` (Ancestor-Kette lesen) pro Frame.
  3. Material-Canvas, Kontextmenü "Set Texture": Scan nach allen Texturen pro Frame.
  Dagegen macht derselbe Panel-Code in `drawPreviewSubjectPicker` (MaterialEditorPanel.cpp:1755-1768) es richtig ("Scanned once per popup-open, not per frame … silly at 60 Hz") – das Muster ist also bekannt, aber nicht überall angewendet.
- **Code-Auszug**:
```cpp
// InspectorPanel.cpp:3212
for (const auto& c : HcEditorUtil::listHorizonCodeClasses(ctx.contentManager))
{
	const HE::UUID id = ctx.contentManager ? ctx.contentManager->loadAsset(c.path) : HE::UUID{};
// MaterialEditorPanel.cpp:1323
for (const auto& a : HcEditorUtil::listAssets(ctx.contentManager, HE::AssetType::Texture))
	if (ImGui::MenuItem((a.label + "##" + a.path).c_str(), nullptr, n->s == a.path))
```
- **Lösung**: Gemeinsamen Mechanismus für "Liste beim Öffnen einfrieren" einführen, z.B. in `HcEditorUtil`: `const std::vector<ClassRef>& cachedAssets(ContentManager*, AssetType, bool refresh)`, wobei `refresh = ImGui::IsWindowAppearing()` (Popup) bzw. beim `BeginMenu`-Öffnen. Für die Script-Combo die ID erst beim Klick laden (`loadAsset` nur wenn `ImGui::Selectable` true liefert) und die Selected-Prüfung über `c.path == curPath` statt über die ID machen. Für das Base-Class-Popup die `wouldCycle`-Auflösung einmalig beim Öffnen in einen `std::vector<bool>` schreiben. Bei Texturen/Meshes zusätzlich Zeilen-Clipping (`ImGuiListClipper`), siehe EDT-1111. Besser noch: ein einziger asset-type-indexierter Cache im `ContentManager`/`EditorAssetTypeCache`, den Refresh/Import invalidiert.
- **Erwarteter Effekt** (Schätzung): Pro offenem Popup Wegfall eines kompletten Verzeichnisbaum-Walks mit N Header-Reads pro Frame; bei Projekten mit mehreren tausend Assets der Unterschied zwischen sichtbarem Stocken (>10 ms/Frame, Festplatten-/Cache-abhängig) und ~0. Reihenfolge der Einträge und Anzeige bleiben gleich.
- **Risiko / Verifikation**: Neu angelegte Assets erscheinen erst beim nächsten Öffnen – entspricht dem Verhalten des Mesh-Pickers. Manuell: Popup öffnen, Asset im Content Browser anlegen, Popup schließen/öffnen. Messen mit dem Profiler-Panel (Frame-Zeit bei offenem Popup, Projekt mit >2000 Assets).

### EDT-1102 – Material-Canvas: Model-Adapter mit linearem `findNode` je Callback (O(N²)), Vektor-/String-Kopien und ~25 `std::function` pro Frame
- **ID**: EDT-1102
- **Kategorie**: D (auch C, B)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:1090-1117`, `135-148`, `1024-1075`; Aufrufer `src/HE_Editor/GraphEditor.cpp:283, 320, 340, 356, 677, 703, 728, 876` (außerhalb des Slice, nur zur Bestätigung gelesen)
- **Problem**: `GraphEditor::draw` ruft pro Frame und pro Node `model.getPos`, `model.pins`, `model.nodeBodyHeight`, `model.title`, `model.headerColor` (teils mehrfach). Jede dieser Lambdas macht `st.graph.findNode(id)`, und `findNode` ist eine lineare Suche (`GraphCommon/GraphModel.h:44-57`, Kommentar: "no index is kept"). Ergebnis: ca. 5–6 · N²/2 Vergleiche pro Frame. Dazu kommen: `m.nodeIds` und `m.links` bauen jeden Frame neue Vektoren, `m.title` liefert einen `std::string` (für FunctionCall zusätzlich `std::filesystem::path(...).stem()`), `categoryColor(const char*)` konstruiert pro Node und Frame einen `std::string` und vergleicht bis zu 10 Mal, `matNodePins` baut pro Node einen neuen `std::vector<Pin>` (Landscape-/Output-/FunctionCall-Nodes zusätzlich weitere Vektoren und `loadFunctionGraph`). Die Model-Struktur mit ~25 capturing `std::function` wird jedes Frame neu aufgebaut (Captures >16 Byte → Heap).
- **Code-Auszug**:
```cpp
m.getPos = [&st](int id, float& x, float& y){ if (const MatGraphNode* n = st.graph.findNode(id)) { x = n->x; y = n->y; } };
m.title  = [&st](int id){ const MatGraphNode* n = st.graph.findNode(id); return n ? matNodeTitle(*n) : std::string(); };
m.headerColor = [&st](int id){ const MatGraphNode* n = st.graph.findNode(id);
	return categoryColor(n ? HE::matNodeDesc(n->type).category : ""); };
ImU32 categoryColor(const char* cat) { const std::string c = cat; if (c == "Material") ...
```
- **Lösung**: (1) Zu Beginn von `drawMaterialCanvas` einmal eine Tabelle `id → MatGraphNode*` bauen (`std::unordered_map<int, MatGraphNode*>` oder, weil `nodeIds` ohnehin zuerst läuft, ein sortierter `std::vector<std::pair<int,MatGraphNode*>>` bzw. ein Index-Vektor `ids[i]`); alle Lambdas lesen daraus. Alternativ `MaterialGraph::findNode` O(1) machen (Id→Index-Cache, bei Laden/Undo neu aufbauen – das betrifft auch HorizonCode/ParticleGraph, siehe Cross-Modul). (2) `categoryColor` auf `strcmp` bzw. eine `constexpr`-Tabelle `{const char*, ImU32}` umstellen, oder die Farbe im `MatNodeDesc` hinterlegen. (3) `m.nodeIds`/`m.links` unverändert lassen, aber `reserve`-Puffer im State halten (`st.idScratch`) statt neu zu allozieren, sobald `GraphEditor::Model` Referenzen zulässt (API-Änderung, optional).
- **Erwarteter Effekt** (Schätzung): Bei 300 Nodes ~250k Vergleiche/Frame weniger, bei 1000 Nodes ~2,5 Mio.; dazu 3N–5N kleine Heap-Allokationen pro Frame. Für kleine Graphen (<50 Nodes) vernachlässigbar → deshalb "messen" (Profiler-Panel mit großem Landscape-Material).
- **Risiko / Verifikation**: Die Tabelle muss pro Frame (nicht pro State) gebaut werden, weil Callbacks (`removeNode`, `connect`, Paste) die Node-Liste ändern; Zeiger aus der Tabelle dürfen nach einer Mutation nicht mehr benutzt werden (die bestehenden Lambdas holen heute pro Aufruf frisch). Tests: `tests/test_material_graph.cpp` deckt Graph-Logik ab, aber nicht den Canvas; Sichtprüfung mit `HE_UI_DUMP_DIR` bzw. `scripts/he_uishot.py`.

### EDT-1103 – 2 900-Zeilen-Funktion `renderForImpl`, 41× Kopiervorlage, drei parallele Komponentenlisten
- **ID**: EDT-1103
- **Kategorie**: A
- **Schweregrad**: Mittel (Wartbarkeit; kein Laufzeitproblem)
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:845-3710` (`renderForImpl`), Wiederholungen z.B. `1425, 1439, 1478, 1511, … 3674`; Listen: `src/HE_Editor/InspectorPrefabKeys.cpp:226-273`, `InspectorPanel.cpp:3765-3847` (`kAddGroups`), die `if (auto* x = registry.try_get<T>(entity))`-Kette
- **Problem**: Jede Komponente ist ein eigener Block `if (auto* x = try_get<T>) { if (componentHeader("Label", removable, removed)) {…} if (removed) { if (undo) undo->snapshotNow(removeLabel.c_str()); registry.remove<T>(entity); } }`. Das Entfernen-Muster steht 41-mal wörtlich im Code. Daneben existieren drei Listen, die dieselbe Komponentenmenge beschreiben und manuell synchron zu halten sind: die Header-Sektionen in `renderForImpl` (43 `componentHeader("…")`-Aufrufe), `kComponentKeys` (Label → Scene-Key, 44 Einträge, `InspectorPrefabKeys.cpp`) und `kAddGroups` (Label + `has/add`, `InspectorPanel.cpp`). Die Prefab-Sync-Tabelle wird zwar durch einen Test gegen `SceneSerializer::isKnownComponentKey` gehalten, aber nichts hält die drei Listen gegeneinander. Mehr als eine Bildschirmseite Funktionskörper pro Komponente macht Review und Verzweigung (collect/only/removeMatching/quiet in jeder Sektion) schwer lesbar.
- **Code-Auszug**:
```cpp
if (removed) { if (undo) undo->snapshotNow(removeLabel.c_str()); registry.remove<TransformComponent>(entity); }   // 1425
if (removed) { if (undo) undo->snapshotNow(removeLabel.c_str()); registry.remove<Transform2DComponent>(entity); } // 1439
```
- **Lösung**: Schrittweise, ohne Verhaltensänderung: (a) Hilfs-Template `template<class T> void removeIfAsked(...)`/Makro-freie Lambda `finishSection<T>(removed)`; die 41 Zeilen werden zu `endSection<TransformComponent>()`. (b) Pro Komponente eine kleine freie Funktion `void drawLight(Ctx&, LightComponent&, Tracker&)` in eigenen Translation Units (`InspectorSections_*.cpp`: Animation, Physics, Environment, UI), `renderForImpl` bleibt eine Tabelle `{label, key, removable, has<T>, draw}`. (c) Aus derselben Tabelle `kComponentKeys` und die Add-Menü-Zeilen (`kAddGroups`) ableiten; nur Gruppe/"needsSkeleton" bleibt Zusatzdaten. Das passt auch zur Anforderung in CLAUDE.md (Panel kompakt, zentrale Pflege-Listen).
- **Erwarteter Effekt**: Weniger Fehlerquellen beim Hinzufügen einer Komponente (heute 3 Stellen + `EditorHelp.cpp`); Kompilierzeit pro Änderung sinkt (die Datei ist 4 060 Zeilen).
- **Risiko / Verifikation**: Hoch für Regressionen bei Verschieben der ImGui-ID-Struktur (`PushID`, `##`-Suffixe, `Help::Scope`-Reihenfolge, `trackEdit`-Positionen). Absicherung: `python3 scripts/editor_help_audit.py --check`, vorhandene `tests/test_*_ui.cpp` mit `HE_UI_DUMP_DIR` vor/nach vergleichen (Bitmap-Diff), plus Test für `listComponents`-Reihenfolge (heute implizit durch Code-Reihenfolge).

### EDT-1104 – Script-Sektion: Lua-Property-Abfrage und Script-Lookup pro Frame
- **ID**: EDT-1104
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:3241-3290` (Implementierung `src/HE_Core/src/Scripting/ScriptEngine.cpp:511-560`)
- **Problem**: Solange die Script-Komponente einer ausgewählten Entity aufgeklappt ist, läuft pro Frame: `isScriptLoaded(moduleName)` (String-Hash), `getScriptProperties(moduleName)` (`lua_rawgeti`, `lua_next` über die Property-Tabelle, pro Property `ScriptPropDef` mit `std::string`, Rückgabe als `std::vector` per Wert) und je Property `s->properties.find(def.name)`. Die Property-Definitionen ändern sich nur, wenn sich der Quelltext ändert. Außerdem kompiliert `loadScript(...)` im Draw (L3249), falls das Modul nicht geladen ist (einmalig, aber auf dem UI-Thread; bei Fehler im Skript jedes Frame erneut, weil `isScriptLoaded` dann false bleibt – unsicher, hängt von `loadScript`).
- **Code-Auszug**:
```cpp
if (!ctx.propScriptEngine->isScriptLoaded(s->moduleName))
	ctx.propScriptEngine->loadScript(s->moduleName, asset->sourceCode);
auto defs = ctx.propScriptEngine->getScriptProperties(s->moduleName);
```
- **Lösung**: Statischen Cache `{moduleName, sourceCode.size()/hash, std::vector<ScriptPropDef>}` im Panel halten (oder in `EditorApplication` neben `propScriptEngine`); Neuberechnung nur wenn Modulname oder Quelltext-Hash (bzw. ContentManager-Revision des Scripts) sich ändern. Fehlgeschlagenes `loadScript` ebenfalls merken (negativer Cache pro Hash), damit nicht jedes Frame neu kompiliert wird.
- **Erwarteter Effekt** (Schätzung): Pro Frame ein Lua-Tabellen-Walk + N String-Allokationen weniger; sichtbar nur bei Skripten mit vielen Properties.
- **Risiko / Verifikation**: Hot-Reload eines Scripts muss den Cache ungültig machen (Hash des `sourceCode` genügt). Test: Script ändern, Inspector offen lassen, Properties müssen nachziehen.

### EDT-1105 – Entity-Picker-Liste (Name + UUID aller Entities) viermal kopiert, pro Frame O(Entities)
- **ID**: EDT-1105
- **Kategorie**: A / D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:1859-1875` (IK Look Target), `2468-2484` (Camera Rig Target), `2645-2673` (Rope Attach), `2993-3016` (Joint Target)
- **Problem**: Vier fast identische Blöcke bauen bei jedem Frame, an dem die jeweilige Sektion offen ist, `std::vector<const char*> names` und `std::vector<HE::UUID> ids` über alle benannten Entities (`registry.view<NameComponent>()`, `all_of<TransformComponent>`, `world.entityId(e)`). Bei einer Szene mit 10–100 k Entities sind das pro Frame zehn- bis hunderttausende Registry-Zugriffe und zwei wachsende Vektoren (kein `reserve`), obwohl die Liste nur bei geöffneter Combo gebraucht wird (nur dann werden die Namen gezeichnet; der Combo-Vorschauwert braucht nur den aktuellen Namen).
- **Code-Auszug**:
```cpp
std::vector<const char*> names{ "(none)" };
std::vector<HE::UUID>    ids{ HE::UUID{} };
for (auto [e, name] : registry.view<NameComponent>().each())
{
	if (e == entity || e == world.rootEntity()) continue;
	if (!registry.all_of<TransformComponent>(e))  continue;
	const HE::UUID id = world.entityId(e);
```
- **Lösung**: Eine Hilfsfunktion `bool entityCombo(HorizonWorld&, Entity self, const char* label, HE::UUID& target, const char* noneText)` in `EditorWidgets` (oder lokal), die (1) die Vorschau aus `world.findByEntityId(target)` ermittelt (ein Lookup) und (2) die Liste nur in `BeginCombo(...)`-Rumpf iteriert, mit `ImGuiListClipper` für große Szenen und optionaler Suche. Das ersetzt vier Kopien und liefert Clipping nach CLAUDE.md Abschnitt 4.
- **Erwarteter Effekt** (Schätzung): Pro Frame mit offener Sektion in großen Szenen mehrere ms weniger; Code −60 Zeilen.
- **Risiko / Verifikation**: Reihenfolge der Einträge (View-Reihenfolge) muss gleich bleiben; ImGui-Combo-Verhalten (Auswahl-Commit, `snapshotNow`) beibehalten. Test: Rope/Joint in Szene mit gesetztem Ziel öffnen, Wert bleibt; UI-Test mit Dump.

### EDT-1106 – Komponentenbaum der Klassen-Tab ruft für jede Entity pro Frame `listComponents` (kompletter Details-Body im Collect-Modus)
- **ID**: EDT-1106
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/LevelScriptPanel.cpp:2599-2605`, `2623-2640`
- **Problem**: In `drawComponentsBody` läuft pro Frame (1) `listComponents` für die fokussierte Entity (L2602) und (2) pro Baumknoten `listComponents(…, comps)` (L2639) plus Kopie der Kinderliste (`const std::vector<Entity> kids = hier ? hier->children : …`) und eines `std::string label`. `listComponents` ruft `renderForImpl` mit `collect` auf, d.h. es werden alle ~43 `try_get`-Sektionen und je `componentHeader` (inkl. `Help::setScope`, `std::string removeLabel`-Bau, EditorUndo-Context-Konstruktion) durchlaufen. Das Ergebnis ändert sich nur bei strukturellen Änderungen (Add/Remove/Paste/Reset, diese sind alle dem Panel bekannt: `st.dirty = true`/Rückgabewert von `renderFor`). Dasselbe gilt für `refreshMultiSelection` (InspectorPanel.cpp:171-176), das aber gecached ist.
- **Code-Auszug**:
```cpp
std::vector<std::string> comps;
InspectorPanel::listComponents(ctx, *st.compWorld, e, comps);
```
- **Lösung**: Pro Entity ein `{std::vector<std::string> comps; uint64_t structuralRev;}` im `ClassState` halten; `structuralRev` wird bei Add/Remove (Rückgabewert `renderFor`, `addComponentMenu`, `removeComponent`, `Add Child`, `Remove`) erhöht. Alternativ: `listComponents` selbst auf eine schlanke Funktion `componentLabels(registry, entity)` umstellen, die ohne ImGui-Kontext und ohne `Help::Scope`/`removeLabel` auskommt (siehe EDT-1103: bei tabellengetriebenem Design ist das ein Einzeiler).
- **Erwarteter Effekt** (Schätzung): Linear in Anzahl Baumknoten; bei typischen Klassen (3–10 Entities) gering, bei großen Hierarchien merklich. Ein Messen im Class-Tab mit 50+ Kindern lohnt sich.
- **Risiko / Verifikation**: Vergessene Invalidierung zeigt veraltete Baumzeilen. Absicherung: Fallback-Revalidierung alle 30 Frames (wie `MultiSelectionCache`), UI-Test "Komponente im Details entfernen → Baum aktualisiert".

### EDT-1107 – Mehrfachauswahl: pro Frame Kopien, O(n·m)-Suchen und Collab-Lookups für alle Mitglieder
- **ID**: EDT-1107
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:250-251`, `257`, `351-357`, `121-139`, `316-321`
- **Problem**: `renderMultiSelection` ist ausdrücklich für Ctrl+A mit Tausenden Entities ausgelegt (Kommentar L141-149), hat aber pro Frame noch lineare bzw. quadratische Anteile: (a) `const std::vector<std::string> common = s_multi.common; … partial = s_multi.partial;` kopiert beide Listen jeden Frame; (b) `membersToSkip(...)` läuft jeden Frame über alle Mitglieder (`registry.all_of<Terrain>`, in Session zusätzlich `subjectFor` + `lockFor` pro Mitglied); (c) `targets` (L351-357) wird jeden Frame berechnet, obwohl es nur im Add-Component-Popup gebraucht wird, mit `std::find(skip…)` pro Mitglied – in einer Collab-Session ist `skip` nahezu die ganze Auswahl, also O(n²) (1000 Mitglieder → ~10⁶ Vergleiche/Frame); (d) beim strukturellen Entfernen `std::find(held…)` in einer Schleife über alle Mitglieder (selten, nur bei Klick).
- **Code-Auszug**:
```cpp
const std::vector<std::string> common  = s_multi.common;
const std::vector<std::string> partial = s_multi.partial;
...
for (Entity e : members)
	if (std::find(skip.begin(), skip.end(), e) == skip.end()) targets.push_back(e);
```
- **Lösung**: (a) `const auto&` statt Kopie (Vorsicht: `s_multi` wird in der Schleife über `stale` nur markiert, nicht neu aufgebaut → Referenz bleibt gültig; sonst `refreshMultiSelection` erst nach der Schleife). (b)+(c) `held` zusammen mit dem `MultiSelectionCache` ablegen (neu berechnen bei Revision-Wechsel/`stale`/Periode, in Session zusätzlich bei Lock-Änderung), `skip`/`held` als `std::unordered_set<Entity>` bzw. sortierter Vektor + `binary_search`; `targets` erst innerhalb von `if (ImGui::BeginPopup(...))` berechnen.
- **Erwarteter Effekt** (Schätzung): Bei 1000+ ausgewählten Entities in Session spürbarer (ms-Bereich); ohne Session nur die Kopien.
- **Risiko / Verifikation**: Lock-Wechsel durch Peers muss `held` ungültig machen, sonst werden Edits an inzwischen fremd-gesperrte Entities propagiert (Korrektheit!). Daher nur zusammen mit Lock-Revision cachen oder `held` weiterhin pro Frame, aber O(n) ohne Kopien. Test: `tests/test_*collab*` bzw. neuer UI-Test mit Mock-Collab.

### EDT-1108 – Material-Vorschau: bei animiertem Node-Preview pro Frame Graph-Kopie + Shader-Generierung + String-Swaps
- **ID**: EDT-1108
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:2385`, `2399-2464`, `2469-2519`
- **Problem**: `previewAnimated` (Material liest die Engine-Uhr) setzt `previewDirty` in jedem Frame. Läuft gleichzeitig ein Per-Node-Preview (`previewNodeId != 0`) oder fehlt dem Asset noch ein gebackener Shader (`noBakedShader`), so wird in jedem Frame: `HE::MaterialGraph pg = st.graph;` (Deep-Copy), Output-Verdrahtung neu aufgebaut, `HE::generateFragment(pg, loader)` ausgeführt, `customShaderFragGlsl`, `shaderParamData`, `graphTexturePaths`, `graphLayerNames` in `orig*` kopiert, überschrieben und zurückgeschoben (dann `matGlslUsesTime` zweimal über den GLSL-Text). Das Ergebnis ist nur vom Graphen und `previewNodeId` abhängig, nicht von der Uhr (die Uhr geht als Parameter `previewClock` in den Renderer). Der Material-Function-Zweig (L2469-2519) hat dasselbe Muster (Scratch-Graph + `generateFragment` bei jeder Neuzeichnung, inkl. Animation).
- **Code-Auszug**:
```cpp
if (st.previewAnimated) st.previewDirty = true;
...
HE::MaterialGraph pg = st.graph;
...
const HE::MatShaderGen pgen = outId ? HE::generateFragment(pg, loader) : HE::MatShaderGen{};
```
- **Lösung**: `MatShaderGen` für das Preview im State cachen (`st.previewGen`, `st.previewGenKey` = (Graph-Revision bzw. `undoPos`+`pushUndo`-Zähler, `previewNodeId`, `isFunction`)). Bei reiner Zeit-Animation nur `RenderMaterialPreview` aufrufen und `previewAnimated` aus gecachten `usesTime`-Flags nehmen (einmal pro neuem Shader berechnen). Das Swap in/out der Asset-Felder kann bei cached Gen entfallen, wenn der Renderer die Quelle als Parameter nimmt (API-Änderung, optional) – ohne die API-Änderung wenigstens `std::swap` statt Kopie+Move.
- **Erwarteter Effekt** (Schätzung): Pro Frame bei aktiver Zeit-Animation + Node-Preview ein Graph-Copy + Codegen weniger (abhängig von Graphgröße: ~0,1–several ms); gilt nur für den Fall "animiert und Node-Preview/ungebackenes Asset".
- **Risiko / Verifikation**: Cache-Schlüssel muss jede Graph-Änderung erfassen, auch Peer-Edits (`peerEdited`) und Live-Drag (`liveEdit`, ändert Positionen, nicht Shader – unkritisch). Test: Node-Preview an, Node verschieben/verbinden, Preview muss aktualisieren.

### EDT-1109 – Preview-Mesh-Picker: kein Zeilen-Clipping, pro Frame Lowercase/Konkatenation/`isLoaded` für alle Meshes
- **ID**: EDT-1109
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:1746-1753`, `1780-1806`
- **Problem**: Die gecachte Liste (`s_engineMeshes/s_projectMeshes`) wird zwar nur beim Öffnen geladen, aber `section(...)` iteriert pro Frame über ALLE Einträge: `matches(m.label)` lowercased jedes Label (Kopie + `transform`), `ctx.contentManager->isLoaded(m.path)` (String-Hash) pro Eintrag, `m.label + "##" + m.path` baut je Zeile einen String, `formatBytes` einen weiteren. Nur ~20 Zeilen sind sichtbar. CLAUDE.md Abschnitt 4 fordert Row-Clipping für zeilenlastige Panels.
- **Code-Auszug**:
```cpp
for (const auto& m : list)
{
	if (!matches(m.label)) continue;
	const bool resident = ctx.contentManager && ctx.contentManager->isLoaded(m.path);
	if (HcEditorUtil::searchMenuItem(m.label + "##" + m.path))
	...
	const std::string size = formatBytes(m.bytes);
```
- **Lösung**: Beim Öffnen und bei Query-Änderung eine gefilterte Index-Liste (`std::vector<int>`) aufbauen (Label vorab in `labelLower` speichern); in `section` `ImGuiListClipper` über die gefilterte Liste verwenden und erst für sichtbare Zeilen `isLoaded`/Strings berechnen. Die Section-Header und Zeilenhöhe bleiben konstant (eine Zeile = `GetTextLineHeightWithSpacing`). Selbes Muster für den Texture-Picker aus EDT-1101.
- **Erwarteter Effekt** (Schätzung): Bei 5 000 Meshes ~10 000 Strings/Frame weniger, solange das Popup offen ist.
- **Risiko / Verifikation**: `searchMenuItem` hebt "bestes Match" hervor (Enter nimmt es) – das Clipping darf dessen Logik (`HcEditorUtil::searchMenuItem`) nicht aushebeln; vor Änderung `HcEditorUtil::searchMenuItem` lesen. UI-Test mit vielen Dummy-Meshes.

### EDT-1110 – Rekompilierung/Serialisierung beim Commit synchron im UI-Thread
- **ID**: EDT-1110
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:196-254`, `302-311`, `2704-2711`
- **Problem**: Jede abgeschlossene Bearbeitung (`structuralEdit || paramEdit || panelEdit || commentEdit`) ruft `applyToMaterial` → `regenerateMaterial` (volle `generateFragment`, `materialGraphToJson`, `syncMaterialInstancesOf`, Metal-Cross-Compile über `MaterialShaderLibrary`) und danach `pushUndo` (nochmals `materialGraphToJson` + String-Vergleich + Push eines Voll-Snapshots, bis 64 Stück) – alles synchron auf dem UI-Thread. Das JSON wird pro Commit zweimal erzeugt (`mat->nodeGraphJson` und Snapshot). `commentEdit` (Kommentarbox verschoben) löst dabei ebenfalls vollständige Shader-Regenerierung und Cross-Compile aus, obwohl der Kommentar den Shader nicht ändert (Kommentar im Code selbst: "persist (no shader change)").
- **Code-Auszug**:
```cpp
if ((structuralEdit || paramEdit || panelEdit || commentEdit) && assetOk && !st.isInstance)
{
	applyToMaterial(st, ctx);
	pushUndo(st);
```
- **Lösung**: (1) `commentEdit` ohne Shader-Regenerierung behandeln: nur `nodeGraphJson` aktualisieren + `dirty`, Undo-Push. (2) JSON einmal erzeugen und in `regenerateMaterial` sowie `pushUndo` wiederverwenden (z.B. `regenerateMaterial` gibt/übernimmt `std::string json`). (3) Cross-Compile-Check (`s_checkLib.fragment`) optional in einen Job auslagern (`JobSystem`) und das Ergebnis im nächsten Frame abholen. (1) und (2) sind rein lokal.
- **Erwarteter Effekt** (Schätzung): Komfort bei großen Materialien (Commit-Latenz); kein Per-Frame-Effekt.
- **Risiko / Verifikation**: (1) ändert Verhalten nur, wenn Kommentare das Shader-Ergebnis beeinflussen – laut Code nicht; prüfen mit Test, der `generateFragment` mit/ohne Kommentare vergleicht. (3) braucht Thread-Sicherheit der Shader-Library (siehe „Unsicher“).

### EDT-1111 – Material-Sektion: Mesh-Section-Tabelle und Strings pro Frame kopiert
- **ID**: EDT-1111
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:2056-2107`
- **Problem**: Solange die Material-Sektion offen ist, wird `slots = HE::meshSectionsOf(*sm)` (Vektor von `MeshSection` mit Strings) jedes Frame neu gebaut, dazu pro Slot `std::string emptyText` mit `substr`/Konkatenation. Der Kommentar begründet die Kopie ("the picker may load, and a load moves every mesh pointer") – die Begründung trägt aber nur für Zeiger auf das Asset, nicht für den Aufbau pro Frame.
- **Code-Auszug**:
```cpp
std::vector<MeshSection> slots;
...
slots = HE::meshSectionsOf(*sm);
...
std::string emptyText = "(mesh's own)";
```
- **Lösung**: Pro Entity gecachter Satz `{meshId, revision} → slots` (Cache im Panel mit der ContentManager-Revision oder `meshAssetId` + Anzahl Sections als Schlüssel) und `emptyText` nur erzeugen, wenn der Slot-Picker tatsächlich gezeichnet wird (er wird immer gezeichnet, aber Texte lassen sich als `char[96]` per `snprintf` ohne Heap bauen).
- **Erwarteter Effekt** (Schätzung): Einige Allokationen pro Frame; nur bei Meshes mit mehreren Slots.
- **Risiko / Verifikation**: Gering; Cache-Invalidierung bei Mesh-Reload (Asset-Pointer ändert sich, `meshAssetId` nicht) beachten.

### EDT-1112 – Komponenten-Header: String-Bau und Help-Key pro Header und Frame
- **ID**: EDT-1112
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:908`, `913-917`, `930`
- **Problem**: In `componentHeader` (läuft pro Komponente und Frame): `removeLabel = std::string("Remove ") + label;` (Heap-Allokation ab "Remove " + >8 Zeichen, z.B. "Animator State Machine"), `snprintf("Component/%s")` + `helpForKey(key)` (Hash-Lookup). Beides nur im (seltenen) Klick-/Hover-Fall nötig: `removeLabel` wird erst bei `removed` gelesen, und Hilfe nur bei Hover.
- **Code-Auszug**:
```cpp
removeLabel = std::string("Remove ") + label;
...
std::snprintf(key, sizeof(key), "Component/%s", label);
EditorWidgets::helpForKey(key);
```
- **Lösung**: `removeLabel` erst beim Entfernen bauen (der Aufrufer kennt `label` nicht, aber `componentHeader` kann `const char* removeLabelFor` speichern, das auf `label` zeigt, und der Aufrufer baut den String); bei EDT-1103 (Tabelle) entfällt das Problem ohnehin. Hilfe nur bei `ImGui::IsItemHovered()` (falls `helpForKey` das nicht intern tut – prüfen).
- **Erwarteter Effekt** (Schätzung): 1 Heap-Allokation und 1 Hash pro offener Komponente und Frame; vernachlässigbar, aber trivial zu beheben.
- **Risiko / Verifikation**: Gering. `editor_help_audit.py --check` muss weiter grün sein.

### EDT-1113 – Mehrfach pro Frame aufgebaute Hilfsobjekte: `CollisionLayerNames`, Spline-`Curve`, Audio-Attenuation
- **ID**: EDT-1113
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:441-457` + `2848-2849`, `3137-3138`; `2728-2732`; `3428-3447`
- **Problem**: (a) `CollisionLayerNames(ctx)` kopiert pro Aufruf 16 Layer-Namen (`cfg.layerName(i)` liefert `std::string` per Wert; bei leerem Namen `"Layer " + to_string(i)`) in neue Strings – bei Rigid Body UND Character Controller, pro Frame. (b) Der Spline-Abschnitt baut pro Frame `HE::spline::Curve(*spline).length()` (Kopie der Kontrollpunkte, Spans, ggf. Längentabellen; Implementierung `SplineCurve.cpp:60-85`) nur für eine Textzeile. (c) Audio Source: `setSoundAttenuation(...)` wird jeden Frame an die Audio-Engine gegeben, solange `a->handle != 0` (ob die Engine lockt: nicht geprüft), die Falloff-Kurve mit 64 `attenuationGain`-Aufrufen wird jedes Frame neu berechnet.
- **Code-Auszug**:
```cpp
names[i] = cfg.layerName(i);          // 441-457, 16 Strings pro Konstruktion
HE::spline::Curve(*spline).length()   // 2732
ctx.audioEngine->setSoundAttenuation(a->handle, a->attenuation, a->innerRange, a->range, a->rolloffFactor); // 3446
```
- **Lösung**: (a) Statischer Cache `{revision of collisionLayers, names[], items[]}` oder lazy: Namen erst bei geöffneter Combo bauen (`Row::combo` bekommt `const char*`-Array; Alternative `BeginCombo` + Selectable aus `cfg.layerName`). (b) Länge nur bei Änderung der Punktliste neu berechnen (Hash über `controlPoints`/`closed` oder `SplineComponent`-Dirty). (c) `setSoundAttenuation` nur aufrufen, wenn sich einer der vier Werte gegenüber dem letzten Aufruf geändert hat (Werte im Widget-Zweig `trackEdit`/`IsItemEdited` bündeln) – Verhalten bleibt bei Änderungen identisch.
- **Erwarteter Effekt** (Schätzung): Je ein paar Mikrosekunden; Summe klein.
- **Risiko / Verifikation**: (c) ändert das Verhalten, wenn die Audio-Engine das Attenuation-Modell sonst überschreibt (z.B. beim Neustart des Voice) – prüfen.

### EDT-1114 – Klassen-Tab: Input-Action-Scan alle 3 s auf dem UI-Thread, Datei-I/O je InputAction
- **ID**: EDT-1114
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/LevelScriptPanel.cpp:1828-1848`, `3071-3080`
- **Problem**: Für Player-Klassen (`isPlayer`) wird der Katalog `scanInputActions` alle 3 Sekunden komplett neu gebaut: `listAssets(InputAction)` (rekursiver Walk über Content + Engine-Content mit Header-Read jeder `.hasset`, siehe EDT-1101) und danach pro Action `HAsset::Reader::open` (liest alle Chunks!) + JSON-Sniff. Das passiert synchron im Draw des Tabs, solange der Tab sichtbar ist, und wiederholt sich unabhängig davon, ob sich etwas geändert hat.
- **Code-Auszug**:
```cpp
if (st.eventsScanTime < 0.0 || (isPlayer && now - st.eventsScanTime > 3.0))
{
	...
	st.inputActions = isPlayer ? scanInputActions(ctx.contentManager) : std::vector<InputActionRef>{};
```
- **Lösung**: Die Liste nur dann neu bauen, wenn sich der Content geändert hat: ContentManager/`EditorAssetTypeCache` hat bereits Invalidierung bei Refresh/Import/Löschen/Umbenennen (`EditorAssetTypeCache.h`, Kommentar in MaterialEditorPanel L1867-1869). Alternativ nur beim Fokus-Gewinn des Tabs und beim Öffnen des Add-Menüs scannen (Lazy Evaluation: die Liste wird nur im Add-Menü gebraucht, `drawAddMenu`). Die Typ-Sniffs (`IsAxis2D`) pro Datei im Cache nach Dateigröße+mtime ablegen.
- **Erwarteter Effekt** (Schätzung): Wegfall periodischer Frame-Spitzen (alle 3 s) im Tab; Größe hängt vom Projekt ab ("messen").
- **Risiko / Verifikation**: Neue InputActions müssen weiter im Add-Menü erscheinen: Test, in dem eine Action während offenem Tab angelegt wird.

### EDT-1115 – `LevelScriptPanel` – globaler Zustand `g` mit Kontext-Swap und 230-Zeilen-`drawGraphBody`
- **ID**: EDT-1115
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/LevelScriptPanel.cpp:126-190`, `1392-1620`, `540-553`, `1663-1722`
- **Problem**: Alle Helfer (`drawVariables`, `drawFunctions`, `drawNodeDetails`, …) greifen auf eine globale `LSState g` zu; ein Wechsel zwischen Level-Script/Game-Instance/Klassen-Tab wird durch `std::move` des gesamten Zustands in/aus `std::map<std::string, LSState>` realisiert. Der Kommentar rechtfertigt das mit "ein einziger Ort, an dem der Zustand gewählt wird", aber es ist ein verstecktes Singleton: Funktionen können nicht isoliert getestet werden, und `forget(...)` muss `g.graphFor` manuell vergleichen (L2801). `drawGraphBody` hat 14 Parameter (davon 5 mit Default) und wird in drei Rollen verwendet. Das Kommentarblock-Verhältnis (z.B. L1447-1471 für drei Zeilen Code) erschwert die Lesbarkeit; die Event-Listen (`kEvents`) stehen als `static const` in den `render`-Funktionen.
- **Code-Auszug**:
```cpp
std::map<std::string, LSState> s_graphStates;
LSState g;
...
if (!g.graphFor.empty()) s_graphStates[g.graphFor] = std::move(g);
```
- **Lösung**: Mittelfristig `LSState&` als Parameter bzw. ein `struct GraphFrame { HC::Graph& graph; LSState& g; Host& … }`, das `drawGraphBody` aufbaut; `s_graphStates[key]` liefert direkt eine stabile Referenz (std::map-Knoten sind stabil), kein Move nötig. Damit entfallen Swap und das Vergessen-Sonderfall. Zusätzlich die 14 Parameter in eine `struct GraphBodyDesc` bündeln.
- **Erwarteter Effekt**: Lesbarkeit; keine Laufzeit-Änderung.
- **Risiko / Verifikation**: Mittel – viele Aufrufstellen (>100 Zugriffe auf `g.`). Mechanisch ersetzbar; Tests: vorhandene HorizonCode-Panel-UI-Tests (`tests/test_*hc*`) vor/nach.

### EDT-1116 – `drawFunctions` O(F·N) und String-Bau pro Zeile/Frame in der Sidebar
- **ID**: EDT-1116
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/LevelScriptPanel.cpp:515-537`, `348-358`, `474-486`
- **Problem**: Für jede FunctionEntry-Node wird die gesamte Node-Liste erneut nach gleichnamigen Funktionen durchsucht (`same`), und alle Nodes jedes Frames mindestens einmal durchlaufen, nur um FunctionEntries zu finden. `label` wird je Funktion und Frame als `std::string` aus drei Teilen gebaut; `varRow` ruft `HcExtractUi::listNote(...)` (liefert `std::string`) pro Variable und Frame. Unclipped-Listen (Variablen, Events, Funktionen).
- **Code-Auszug**:
```cpp
for (const auto& n : graph.nodes) {
	if (n.type != NT::FunctionEntry) continue;
	int same = 0;
	for (const auto& o : graph.nodes) if (o.type == NT::FunctionEntry && o.s == n.s) ++same;
```
- **Lösung**: Pro Frame einmal die FunctionEntry-Nodes in einen `std::vector<const HC::Node*>` einsammeln (oder ein `std::unordered_map<std::string,int>` der Namenszähler), dann `same` per Lookup; Labels per `snprintf` in einen Stack-Puffer. Bei Graphen mit tausenden Nodes (Level-Script) ist der Durchlauf über alle Nodes ohnehin der Hauptanteil – dann Funktionsliste im `LSState` cachen und bei `edited` neu aufbauen.
- **Erwarteter Effekt** (Schätzung): Bei Graphen mit >1000 Nodes und >10 Funktionen merkbar; sonst klein.
- **Risiko / Verifikation**: Gering; Test mit Doppelnamen ("duplicate name!").

### EDT-1117 – Material-Add-Menü: zehn Registry-Durchläufe mit String-Konstruktion pro Frame
- **ID**: EDT-1117
- **Kategorie**: D / C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:1366-1370`, `1395-1433`, `1436-1462`
- **Problem**: Solange das Add-Node-Popup offen ist, ruft `emitCategory` für jede der 10 Kategorien einmal `matNodeRegistry()` ab und konstruiert pro Node zweimal `std::string(d.category)`/`std::string(lastCat)` zum Vergleichen, `matches()` ruft zweimal `lower()` (Kopie + `transform`) pro Node, dazu ein Tail-Pass mit weiteren `std::string(d.category)`-Vergleichen, und pro MaterialFunction `std::filesystem::path(fn->path).stem().string()` (+ ein `enumerateIds`). Etwa ~150 Nodes × 10 Kategorien.
- **Code-Auszug**:
```cpp
for (const auto& d : HE::matNodeRegistry())
{
	if (std::string(d.category) != cat || !listed(d)) continue;
	if (std::string(lastCat) != cat)
```
- **Lösung**: Einmalig (static) eine nach Kategorie gruppierte Index-Liste der Registry bauen (`std::vector<std::pair<const char*, std::vector<const MatNodeDesc*>>>`), Kategorienvergleich per `strcmp`; Kleinbuchstaben-Varianten der Namen/Kategorien vorberechnen. Funktionsnamen beim Popup-Öffnen (`IsWindowAppearing`) in einer Liste cachen.
- **Erwarteter Effekt** (Schätzung): ~3 000 kleine String-Konstruktionen pro Frame im offenen Menü entfallen.
- **Risiko / Verifikation**: Gering; Reihenfolge der Kategorien (`kCatOrder` + Tail) beibehalten.

### EDT-1118 – `trackEdit()` außerhalb von `if` bei Light-Range/Spot-Angle (irreführende Einrückung, doppelter `stashPre`/`commitPending`)
- **ID**: EDT-1118
- **Kategorie**: A / B
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:2574-2577`, `1276-1278`
- **Problem**: Die Zeilen
  `if (l->type != LightType::Directional) Row::dragFloat("Range", …); trackEdit();` und
  `if (l->type == LightType::Spot) Row::dragFloat("Spot Angle", …); trackEdit();`
  führen `trackEdit()` unabhängig von der Bedingung aus. Wird das Widget nicht gezeichnet, bezieht sich `IsItemActivated()`/`IsItemDeactivatedAfterEdit()` auf das zuletzt gezeichnete Item (Intensity bzw. Range) und ruft `stashPre`/`commitPending` ein zweites Mal auf; im besten Fall ein No-op, im ungünstigen Fall ein doppelter Undo-Eintrag oder überschriebener Pre-State (hängt von `EditorUndo::stashPre` ab: unsicher, nicht geprüft). Außerdem: im Weather-Zweig `if (registry.all_of<WeatherComponent>(entity)) { if (auto* w = registry.try_get<…>)` – doppelte Abfrage, die zweite Verzweigung ist immer wahr.
- **Code-Auszug**:
```cpp
if (l->type != LightType::Directional)
	Row::dragFloat("Range", &l->range, 0.1f, 0.0f, 10000.0f); trackEdit();
if (l->type == LightType::Spot)
	Row::dragFloat("Spot Angle", &l->spotAngle, 0.5f, 1.0f, 179.0f); trackEdit();
```
- **Lösung**: Klammern setzen: `if (…) { Row::dragFloat(…); trackEdit(); }`. Weather: `if (auto* w = registry.try_get<WeatherComponent>(entity)) { … }`, die äußere `all_of` entfällt.
- **Erwarteter Effekt**: Korrektheit/Lesbarkeit; keine Performance.
- **Risiko / Verifikation**: Gering; Undo-Test: Light Directional Intensity ziehen → genau ein Undo-Schritt.

### EDT-1119 – Material-Lösch-/Weitere Frame-Kosten: `drawParamConstPanel` und `drawInstanceOverridePanel` kopieren Gruppen-Strings und suchen linear
- **ID**: EDT-1119
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:910-923`, `2046-2067`, `2033-2037`
- **Problem**: `groupOf` liefert `std::string` per Wert; im Instanz-Panel wird `groupOf(i) != grp` für alle G×P Paare pro Frame mit je einer String-Kopie ausgewertet, `ovIndex(nm)` ist linear über die Overrides (zweimal pro Zeile). Bei 100 Parametern und 10 Gruppen sind das ~1 000 String-Kopien und ~10 000 String-Vergleiche pro Frame. Im Master-Panel (`drawParamConstPanel`) wird zusätzlich jedes Frame ein `std::vector<std::string> groupOrder` und zwei Zeigerlisten aufgebaut.
- **Code-Auszug**:
```cpp
auto groupOf = [&](size_t i) -> std::string
{ return i < inst.graphParamGroups.size() ? inst.graphParamGroups[i] : std::string(); };
...
if (groupOf(i) != grp) continue;
```
- **Lösung**: `groupOf` als `const std::string&` zurückgeben (statisches leeres `kEmpty`), `ovIndex` einmal in ein `std::vector<char> overridden(inst.graphParamNames.size())` vorberechnen (eine Schleife mit `unordered_set`), die Gruppen-Reihenfolge nur bei Änderung der Parameteranzahl neu bauen. Wegen der fehlenden Clip-Nutzung (alle Zeilen werden gezeichnet) ggf. `ImGuiListClipper` pro Gruppe, wenn Instanzen mit >200 Parametern vorkommen (unsicher).
- **Erwarteter Effekt** (Schätzung): Gering, nur bei vielen Parametern.
- **Risiko / Verifikation**: Gering.

### EDT-1120 – macOS-Menü: `setProjectLoaded` schreibt jeden Frame ~35 `NSMenuItem.enabled`; `setToggleState` linear
- **ID**: EDT-1120
- **Kategorie**: D / B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MacMenuBar.mm:506-510`, `521-532`, `536-563`
- **Problem**: `setProjectLoaded` hat im Gegensatz zu `setAppProject`, `setToggleState` und `setItemTitle` keine Änderungsprüfung: Jeden Frame werden alle `s_projectItems` (rund 35) per ObjC-Setter beschrieben (`it.enabled = …`); AppKit markiert das Menü ggf. als neu zu zeichnen (der Kommentar in `setAppProject` nennt genau dieses Problem für `hidden`). `setToggleState` durchsucht pro Aufruf `s_toggleItems` (~45 Einträge) linear; bei ~45 Aufrufen pro Frame (EditorUI) sind das ~2 000 Vergleiche plus ObjC-Property-Reads (`t.item.state`).
- **Code-Auszug**:
```cpp
void setProjectLoaded(bool loaded)
{
	s_projectLoaded = loaded;   // a few dozen items; cheap enough to set every frame
	for (NSMenuItem* it : s_projectItems) it.enabled = loaded ? YES : NO;
}
```
- **Lösung**: `if (s_installed && loaded == s_projectLoaded) return;` – Achtung: `install()` ruft `setProjectLoaded(false)` bei initial `s_projectLoaded = false` und muss den ersten Durchlauf erzwingen (z.B. `std::optional<bool>` oder Flag `s_projectStateApplied`). Toggle-Lookup: `std::unordered_map<uint64_t, NSMenuItem*>` (Schlüssel `cmd<<32|arg`) oder gecachtes `bool s_lastState`-Feld im `ToggleItem`, um den ObjC-Read zu sparen (`if (t.last == on) return;`).
- **Erwarteter Effekt** (Schätzung): ~80 ObjC-Messages pro Frame weniger; nur macOS. Ob AppKit bei gleichem Wert wirklich neu zeichnet: nicht gemessen.
- **Risiko / Verifikation**: Gering; die Änderung muss berücksichtigen, dass der Menübaum außerhalb (Hub/Projektwechsel) nicht ohne Zutun zurückgesetzt wird. Manuell auf macOS prüfen.

### EDT-1121 – Schwere Header für kleine Abhängigkeiten
- **ID**: EDT-1121
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/InputMappingModel.h:2`, `src/HE_Editor/LevelScriptPanel.h:2`, `src/HE_Editor/InspectorPanel.h:2`, `src/HE_Editor/MacMenuBar.h:3-4`
- **Problem**: `InputMappingModel.h` zieht `<Application/InputMapping.h>` (und damit `Input.h`, `<SDL3/SDL.h>`, `<unordered_map>`, `<algorithm>`) nur für das Enum `AxisSource` (Kommentar sagt: "free of ImGui", aber nicht von SDL). `LevelScriptPanel.h` inkludiert das volle `<imgui.h>` für `ImVec2` (Vorwärtsdeklaration geht nicht für Wertparameter; `const ImVec2&` genügt jedoch nicht ohne Typ – hier wäre `struct ImVec2;` ausreichend, da nur Referenzen verwendet werden). `InspectorPanel.h` zieht `HorizonScene/HorizonWorld.h` (entt-Header) für `Entity`/`HorizonWorld`. Jede Translation Unit, die diese Header inkludiert (Tests, MCP-Tools, EditorUI), bezahlt dafür.
- **Code-Auszug**:
```cpp
#include <Application/InputMapping.h>   // AxisSource
#include <imgui.h>        // LevelScriptPanel.h, nur für ImVec2&
```
- **Lösung**: `AxisSource` in einen kleinen Header `Application/InputAxisSource.h` auslagern (`enum class AxisSource : unsigned char` o.Ä.), `InputMapping.h` inkludiert ihn. `LevelScriptPanel.h`: `struct ImVec2;` statt `<imgui.h>`. `InspectorPanel.h`: `using Entity = entt::entity;` über schlankeren Header, falls vorhanden; sonst belassen.
- **Erwarteter Effekt** (Schätzung): Kürzere Kompilierzeiten, v.a. `tests/test_inputmapping.cpp`.
- **Risiko / Verifikation**: Gering; Build aller Targets (`he_tests`, `HorizonEditor`).

### EDT-1122 – Eingabeprüfung `inputMayEditThisFrame` iteriert alle ImGui-Tasten pro Frame
- **ID**: EDT-1122
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:88-105`
- **Problem**: Pro Frame in der Mehrfachauswahl werden alle `ImGuiKey_NamedKey_*` (~150 Tasten) per `IsKeyDown` abgefragt, bevor entschieden wird, ob serialisiert werden muss. Einfacher und billiger: `io.KeysData`-Scan ist nicht nötig, wenn `io.WantTextInput`, `io.InputQueueCharacters`, oder `ImGui::IsAnyItemActive()` schon Aktivität anzeigen; für das Panel genügt in der Praxis `ImGui::IsAnyItemActive() || mouse || key-event-this-frame` (`io.KeysDown`-Änderung via `io.InputQueueCharacters`/`ImGui::IsKeyPressed`).
- **Code-Auszug**:
```cpp
bool key = io.InputQueueCharacters.Size > 0;
for (int k = ImGuiKey_NamedKey_BEGIN; !key && k < ImGuiKey_NamedKey_END; ++k)
	key = ImGui::IsKeyDown(static_cast<ImGuiKey>(k));
```
- **Lösung**: Entweder `ImGui::IsAnyItemActive()` ergänzen und die Schleife nur ausführen, wenn kein Item aktiv ist (häufiger Fall: nichts passiert → Schleife läuft trotzdem ganz). Alternativ Schleife durch ImGuis `io.KeyMods != 0 || ImGui::IsKeyPressed(any)` ersetzen. Wirkung ohne Messung sehr klein – nur aufnehmen, wenn die Funktion am Frame-Profil auffällt.
- **Erwarteter Effekt**: Vernachlässigbar (<10 µs).
- **Risiko / Verifikation**: Tastaturbedienung der Slider (Cursortasten bei fokussiertem Widget) muss weiter propagieren: UI-Test mit Tastatur-Edit (Pfeile in `DragFloat` per Ctrl+Click-Eingabe).

## Gelesen

Ja, alle 10 Dateien des Slice wurden vollständig gelesen (`InputMappingModel.h`, `InspectorPanel.cpp/.h`, `InspectorPrefabKeys.cpp`, `LevelScriptPanel.cpp/.h`, `MacMenuBar.h/.mm`, `main.cpp`, `MaterialEditorPanel.cpp`); `main.cpp` ergab keinen Fund (22 Zeilen, einmalige Initialisierung). Zur Bestätigung einzelner Funde wurden nur gezielte Ausschnitte außerhalb des Slice gelesen (`HcEditorUtil.cpp` listAssets, `GraphEditor.cpp` Layout-Schleife, `GraphModel.h`, `ScriptEngine.cpp::getScriptProperties`, `CollisionLayers.cpp::layerName`, `SplineCurve.cpp` Konstruktor, `InputMapping.h` Includes).

## Editor Slice 12 – MaterialEditorPanel.h, McpBridge*, McpCameraGizmos*, McpClaudeProbe*, McpClientCameras.h, McpClientSetup*, McpLockBook.h, McpStatusBar*, McpToolCommon*, McpToolRegistry*, McpToolsAnimator/Api/Asset/Batch/Build/Clip/Collab/Entity/Hc

## Zusammenfassung

Der Slice ist die MCP-Bridge des Editors (Socket, JSON-RPC, Tool-Registry, 11 Tool-Familien) plus ein paar Footer-/Viewport-Gizmos. Nichts davon läuft pro Frame im Draw-Pfad außer `McpBridge::update` (leer, solange niemand verbunden ist), `McpStatusBar` und die Kamera-Gizmos; der Hot-Path ist hier stattdessen der *Tool-Aufruf auf dem UI-Thread* (Bridge.h: „one call, on the main thread"). Die Qualität ist hoch (gute Fehlertexte, saubere Trennung ImGui-frei/testbar), die Probleme sind (a) zwei O(N)-Scans pro Aufruf in den Entity-Tools, die mit Batch/Szenengröße multiplizieren, (b) mehrfach duplizierte Gate-/Helferlogik über ≥11 Tool-Dateien und ein 1592-Zeilen-Header, der über `EditorApplication.h` in 78 Übersetzungseinheiten gezogen wird, (c) Frame-Thread-Dateisystemwalks, die entgegen den Kommentaren nicht durch das `limit` begrenzt sind. Funde: 2 Hoch, 8 Mittel, 7 Niedrig. Alle Funde sind statisch aus dem Code belegt; es wurde nichts gemessen oder gebaut.

---

### EDT-1201 – `entityByUuid` ist ein linearer Scan mit `snprintf` + `std::string` pro Entity, obwohl `HorizonWorld::findByEntityId` O(1) existiert
- **ID**: EDT-1201
- **Kategorie**: D (auch C)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: Aufrufer in diesem Slice: `McpToolsEntity.cpp:206`, `:443`, `:573`; `McpToolsApi.cpp:199` (über `hooks.entityByUuid`); Implementierung (Cross-Slice) `src/HE_Editor/EditorCommands.cpp:419-429`; vorhandener Index `src/HE_Scene/include/HorizonScene/HorizonWorld.h:53` (`findByEntityId`, `m_idIndex` Z. 300)
- **Problem**: Jede uuid-Auflösung eines MCP-Tools formatiert die ID *jeder* Entity der Welt als Hex-String (`snprintf` + `std::string`-Allokation, wenn > SSO) und vergleicht Strings; es gibt keinen Early-Exit (`view.each` läuft nach dem Treffer weiter, der Lambda kehrt nur sofort zurück). Jeder `entity_*`-Aufruf löst 1–2 uuids auf, `api_*`-Tools lösen *jeden* String-Int-Parameter so auf, und `batch` (bis 100 Operationen) multipliziert das: 100 Operationen × E Entities × ein `snprintf`. Das läuft auf dem UI-Thread. Die Welt führt bereits einen `unordered_multimap<UUID, Entity>`-Index (`findByEntityId`), der hier nicht genutzt wird.
- **Code-Auszug**:
```cpp
// EditorCommands.cpp:424
reg.view<const EntityIdComponent>().each([&](auto e, const EntityIdComponent&) {
    if (found != entt::null) return;
    if (uuidOf(world, e) == uuid) found = e;
});
```
- **Lösung**: Den Hex-String einmal parsen (`uuidFromHex` existiert in `TaskBoard.h:88`; sonst 2×`strtoull` über je 16 Zeichen, Länge 32 prüfen) und `world.findByEntityId(id)` aufrufen; `entt::null` bei Parsefehler. Kein neuer Cache nötig. Skizze:
```cpp
Entity entityByUuid(HorizonWorld& w, const std::string& s) {
    HE::UUID id; if (!parseHex32(s, id)) return entt::null;
    return w.findByEntityId(id);
}
```
- **Erwarteter Effekt**: Schätzung: von O(E) mit Allokation/`snprintf` je Entity auf O(1). Bei 50 000 Entities grob 5–20 ms je Auflösung → < 1 µs; ein 100er-Batch fiele von Sekunden auf Mikrosekunden. (Schätzung, nicht gemessen.)
- **Risiko / Verifikation**: Bei doppelten IDs (beschädigter Merge) lieferte der alte Scan den ersten in View-Reihenfolge, `findByEntityId` „einen von beiden" (HorizonWorld.h:50-52 dokumentiert das) – für MCP-Adressierung unkritisch. Groß-/Kleinschreibung: `uuidOf` gibt Kleinbuchstaben aus; der alte String-Vergleich war case-sensitiv, der Parser sollte beides akzeptieren oder ebenfalls streng sein (Verhalten erhalten = streng). Der Index wird über Registry-Signale gepflegt: Test in `tests/` für „Entity in demselben Frame erzeugt, dann `entity_get`" ergänzen (Index wird synchron über Signale gepflegt, aber das ist der Fall, an dem der Scan nie scheitern konnte).

---

### EDT-1202 – `entity_list` serialisiert jede Entity vollständig (CBOR→JSON) *vor* Filter und Limit und weiter nach Erreichen des Limits
- **ID**: EDT-1202
- **Kategorie**: D (auch C)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `McpToolsEntity.cpp:145-174` (`componentsOf`, `entitySummary`), `:331-362` (Schleife); Folgeaufrufe `:391`, `:685`, `:710`
- **Problem**: `entitySummary` ruft pro Entity `SceneSerializer::serializeEntityComponents` (alle Komponententypen via `try_get`), dann `json::from_cbor`, dann ein JSON-Array der Schlüssel – nur um danach `name` zu vergleichen oder `components` nach einem Schlüssel zu durchsuchen. Der Namensfilter (`nameContains`) bräuchte nur `NameComponent`; selbst ohne Filter wird nach Erreichen von `limit` (Z. 360 `++truncated; continue;`) weiter für *alle* restlichen Entities komplett serialisiert, nur um `truncated` zu zählen. Bei einer Szene mit 20 000 Entities × ~8 Komponenten sind das 20 000 CBOR-Encodes + Decodes + JSON-Bäume pro Aufruf auf dem UI-Thread, bei Standardlimit 200 liefert der Aufruf aber nur 200 Zeilen. Zusätzlich: `entity_set_components` serialisiert die Zielentity zweimal (Z. 685 und 710), `entity_get` einmal – das ist okay, aber die Rückgabe von `entity_set_components` ist ein weiterer voller Roundtrip.
- **Code-Auszug**:
```cpp
// McpToolsEntity.cpp:337-361
for (const Entity e : all) {
    if (e == world->rootEntity()) continue;
    json summary = entitySummary(*world, e);          // volle Serialisierung
    ...
    if (!needle.empty()) { std::string name = summary["name"]...; tolower... }
    if (static_cast<int>(out.size()) >= limit) { ++truncated; continue; }
```
- **Lösung**: (1) Reihenfolge umdrehen: zuerst billige Prädikate (uuid leer? `NameComponent` lowercase-Substring) direkt am Registry prüfen, erst für Treffer `entitySummary` bauen. (2) Für den `component`-Filter eine billige Prüfung „hat Entity Komponente mit Schlüssel K" anbieten (z. B. `SceneSerializer::hasComponentKey(world, e, key)` neben `isKnownComponentKey`, `SceneSerializer.cpp:3011`) statt Voll-Serialisierung; solange es sie nicht gibt, nur für den Fall `component` nicht leer voll serialisieren. (3) Nach `limit` für `truncated` nur noch die billigen Prädikate auswerten (Zähler), nie `entitySummary`. (4) `std::vector<Entity> all` (Z. 331) mit `reserve(registry.storage<entt::entity>().size())` bauen, oder direkt iterieren – der Kommentar begründet die Kopie mit Pool-Erzeugung durch `try_get`; die entfällt, wenn die billigen Prädikate vorab laufen und nur Treffer serialisiert werden (dann aber `all` beibehalten, solange `entitySummary` im Loop läuft).
- **Erwarteter Effekt**: Schätzung: Kosten fallen von O(E · Komponenten) auf O(E) billige Vergleiche + O(limit) Serialisierungen; bei großen Szenen Faktor 10–100 (nicht gemessen).
- **Risiko / Verifikation**: Ausgabeformat (`entities`, `truncated`) muss bit-identisch bleiben, inkl. dass Entities ohne uuid übersprungen werden und `truncated` nur gefilterte Treffer zählt. `tests/` hat vermutlich MCP-Entity-Tests (nach `entity_list` greppen); einen Test ergänzen, der `nameContains` + `limit` + `component` kombiniert und `truncated` prüft. Gefahr: Die `components`-Liste in der Zusammenfassung stammt aus dem Serializer; wer sie billiger baut, muss dieselbe Schlüsselmenge liefern (sonst driftet `entity_list` von `entity_get` ab) – deshalb die Voll-Serialisierung für Treffer beibehalten.

---

### EDT-1203 – Rekursive Verzeichniswalks auf dem UI-Thread laufen komplett (Sammeln + Sortieren) bevor `limit` greift; zwei Kopien der Walk-Logik
- **ID**: EDT-1203
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `McpToolCommon.cpp:197-241` (`walkContentAssets`), `McpToolsAsset.cpp:299-333` (`asset_list`, rekursiv); Kommentar-Behauptung `McpToolCommon.h:72-75`, `McpToolsAsset.cpp:39-41`
- **Problem**: Beide Funktionen laufen den *gesamten* Baum, sammeln alle Pfade (`absPaths.push_back`, `found.emplace_back`), sortieren alles und wenden erst danach das Limit an. Der Header begründet die Kappung ausdrücklich damit, dass „MCP handlers run on the editor's frame thread – a full walk of a large project would stall it", aber das Limit begrenzt nur die Ergebnisgröße, nicht den Walk. Bei `asset_list` mit `type`-Filter wird zudem pro Datei `typeNameOf` (Header-Sniff, bei Cache-Miss Dateizugriff) bis zum Limit aufgerufen. Außerdem sind die Dotfile-/Disable-Recursion-Regeln dreimal ausgeschrieben (Common:215-222, Asset:307 + :317-322).
- **Code-Auszug**:
```cpp
// McpToolCommon.cpp:213-228
for (; !ec && it != end; it.increment(ec)) { ... absPaths.push_back(...); }
std::sort(absPaths.begin(), absPaths.end());
// danach erst: if (found.size() >= limit) { truncated = true; break; }
```
- **Lösung**: (a) `asset_list` die Walk-Logik von `walkContentAssets` mitbenutzen lassen (eine Funktion `collectEntries(root, recursive, includeFolders)`), (b) die Walk-Ergebnisse pro Projekt cachen und über den vorhandenen Datei-Watcher/„Content refresh" invalidieren (der Editor kennt `onAssetAppeared/onAssetGone/onAssetMoved`, Hooks in `McpAssetHooks`), (c) wenn ein Cache nicht gewünscht ist: Sortierung ist nötig für Determinismus, also bleibt der Walk – dann wenigstens `reserve` und dokumentieren, dass das Limit keine Stall-Garantie ist. Mindestlösung: Kommentar korrigieren und `walkContentAssets`/`asset_list` zusammenführen.
- **Erwarteter Effekt**: Schätzung: Mehrfach-Aufrufe (`animator_info`, `clip_info`, `material_info`, `asset_list` …) auf großen Projekten (10⁴–10⁵ Dateien) vom 10–100-ms-Stall auf ~0 mit Cache; Vereinfachung: eine Walk-Implementierung statt zwei.
- **Risiko / Verifikation**: Determinismus (sortierte Reihenfolge) muss bleiben. Cache-Invalidierung bei Änderungen von außen (Git-Pull, externer Editor) – ohne Watcher bleibt nur der ungecachte Walk; „unsicher / messen". Tests: `tests/` zu `asset_list`/`walkContentAssets` mit Dotfiles, verschachtelten Ordnern, Limit.

---

### EDT-1204 – `tools/call`: Ergebnis wird kopiert, mit `dump(2)` pretty-gedruckt und zusätzlich als `structuredContent` ein zweites Mal gesendet
- **ID**: EDT-1204
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `McpBridge.cpp:444-477`, `:494-500` (`sendJson`); `McpToolsBatch.cpp:244-254`
- **Problem**: `const ToolResult r` verhindert Move: `json payload = r.isError ? ... : r.content;` kopiert den ganzen Ergebnisbaum (z. B. `hc_get` mit 200 Knoten samt Pins, `entity_get`). Danach `payload.dump(2)` (eingerückt, ~30–40 % größer als kompakt – und für ein Modell Tokens), dann `structuredContent = std::move(payload)`, und `sendJson` serialisiert das gesamte Objekt noch einmal und escaped dabei den Text-String ein zweites Mal. Auf der Leitung liegt jedes Ergebnis also zweimal (Pretty-Text + kompaktes Struct). Das drückt gegen `kMaxFrameBytes` (4 MiB, Bridge.h:63) früher als nötig. Im Batch: `slot["result"] = r.content;` (Z. 254) kopiert ebenfalls, `r` ist `const`.
- **Code-Auszug**:
```cpp
// McpBridge.cpp:454-460
json payload = r.isError ? json{ {"code", r.errorCode}, {"message", r.errorMessage} } : r.content;
json content = json::array({ json{ {"type","text"}, {"text", payload.dump(2)} } });
...
{ "structuredContent", std::move(payload) },
```
- **Lösung**: `ToolResult r = ...` (nicht const) und `json payload = r.isError ? ... : std::move(r.content);` — im Batch `slot["result"] = std::move(r)`-Variante entsprechend (r nicht const machen). Optional: `dump()` statt `dump(2)` (kompakt; MCP-Clients/Modelle lesen beides, spart Bytes und Tokens) – das ändert den sichtbaren Text, daher separat entscheiden.
- **Erwarteter Effekt**: Schätzung: ein Deep-Copy weniger pro Aufruf (O(Ergebnisgröße)); mit kompaktem `dump` ~25–40 % kleinere Antworten. Wirkt nur bei großen Antworten spürbar.
- **Risiko / Verifikation**: Der Move ist verhaltensneutral, solange `r` danach nicht mehr gelesen wird (`r.isError`, `r.imageBytes` werden nach dem Payload-Bau gelesen – `isError` vor dem Move zwischenspeichern; `imageBytes` sind eigene Member). `dump(2)`→`dump()` ändert den Text-Block; Tests, die den Text vergleichen (Bridge-Tests über echten Socket, `tests/` nach `structuredContent`/`"text"` greppen), vorher prüfen.

---

### EDT-1205 – Nicht authentifizierte Verbindungen haben kein Timeout; `Client::connectedMs` wird nie gelesen
- **ID**: EDT-1205
- **Kategorie**: E / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `McpBridge.h:134-138`, `McpBridge.cpp:237`, `:227-236`
- **Problem**: Jeder Client bekommt `connectedMs = nowMs`, das Feld wird aber nirgends ausgewertet (`grep connectedMs` trifft nur Deklaration und die eine Zuweisung). Eine Verbindung, die nie `auth` sendet, bleibt für immer in `m_clients`; vier solche Sockets (lokaler Prozess, kein Token nötig) füllen `kMaxClients = 4` und sperren damit alle echten Clients aus, bis der Editor die Bridge aus-/einschaltet. Das Token schützt die Befehle, nicht die Slots. Zugleich ist das Feld toter Code.
- **Code-Auszug**:
```cpp
// McpBridge.cpp:237
m_clients[ev.conn] = Client{ /*authed=*/false, nowMs };
// McpBridge.h:137  std::uint64_t connectedMs = 0;   // nie gelesen
```
- **Lösung**: In `update()` nach der Event-Schleife über `m_clients` iterieren (max. 4 Einträge) und unauthentifizierte Verbindungen älter als z. B. 5 s trennen (`disconnect` + `erase` + `notifyClientGone`, derselbe Pfad wie bei falschem Token). Alternativ das Feld entfernen, wenn man den Zustand bewusst akzeptiert.
- **Erwarteter Effekt**: Kein Performance-Gewinn; Robustheit (Slot-Verhungern) bzw. weniger toter Zustand.
- **Risiko / Verifikation**: Ein Test über echten Socket (Bridge ist ImGui-frei, laut Header testbar): verbinden, nichts senden, `update(now + 6 s)` → `connectionCount()==0`. Achtung: Frame-Zeit steht nur still, wenn der Editor idle-throttled; `nowMs` ist Frame-Zeit, ein sehr langsamer Start könnte legitime Clients treffen – großzügiges Timeout wählen.

---

### EDT-1206 – `project_build_status` kopiert den kompletten Build-Log (unter dem Lock des Build-Fensters) und filtert erst danach
- **ID**: EDT-1206
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `McpToolRegistry.h:1296-1349` (`McpBuildStatus`, `buildStatus`), `McpToolsBuild.cpp:378-404`
- **Problem**: Der Hook liefert `McpBuildStatus` *by value* inklusive `std::vector<McpBuildLogLine> log` (Kommentar Z. 1297-1298: „a copy taken under the window's own lock"). Die Beschreibung des Tools sagt selbst, ein Toolchain-Lauf drucke „thousands of lines". Der Aufruf ist als Polling gedacht (`running`/`finished`), also wird bei jedem Poll auf dem UI-Thread der gesamte Log (alle Strings) kopiert, währenddessen der Lock gehalten wird, den der Build-Worker zum Schreiben braucht; danach filtert das Tool auf die letzten ≤ 2000 Zeilen (`kept` ist ein zweiter Vektor, nochmals Kopien).
- **Code-Auszug**:
```cpp
// McpToolsBuild.cpp:378, 389-402
McpBuildStatus st = hooks.buildStatus();
...
std::vector<McpBuildLogLine> kept;
for (const McpBuildLogLine& l : st.log) { if (...) continue; kept.push_back(l); }
if (kept.size() > maxLines) kept.erase(kept.begin(), kept.end() - maxLines);
```
- **Lösung**: Hook-Signatur um die Filter erweitern: `buildStatus(const McpBuildLogFilter& f)` (`withLog`, `maxLines`, `minSeverity`, `step`), der Editor-Hook filtert unter dem Lock rückwärts vom Ende und kopiert nur die ≤ maxLines Treffer. Das Tool behält `logLinesMatched`, indem der Hook die Trefferzahl mitliefert.
- **Erwarteter Effekt**: Schätzung: Kopieraufwand von O(Loggröße) auf O(maxLines) pro Poll, kürzere Lock-Haltezeit; bei Logs mit 10⁴–10⁵ Zeilen merklich.
- **Risiko / Verifikation**: Änderung der Hook-Signatur berührt die Verdrahtung in `EditorApplication::setupMcpTools` und Tests mit synthetischem `McpBuildStatus`. `logLinesMatched`/`logTruncated` müssen identisch bleiben (Test mit mehr Zeilen als `maxLines`, mit `minSeverity`, mit `step`).

---

### EDT-1207 – Uuid→Pfad-Auflösung durch Vollwalk mit Datei-Öffnung pro Asset (`RefNames`), dasselbe Muster in Prefab/Sequence
- **ID**: EDT-1207
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `McpToolsAnimator.cpp:91-121` (`RefNames`), genutzt `:456`, `:608`, `:980`, `:1132`; gleiches Muster außerhalb des Slice: `McpToolsPrefab.cpp:711-712`, `McpToolsSequence.cpp:145-148`
- **Problem**: Um eine gespeicherte uuid (Clip/BlendSpace einer State/Sample) in einen Pfad zurückzuübersetzen, läuft `RefNames::build` `walkContentAssets` (bis 2000 Assets, Walk + Sort + Header-Sniff je Datei) und öffnet danach *jede* Datei, um die uuid im META-Chunk zu lesen (`assetUuidOfFile`), und baut daraus eine `std::map` mit Pair-Key. Das passiert bei jedem Detail-Aufruf (`animator_info`, `blendspace_info`) und bei jedem Set-Tool, dessen Ergebnis einen Zustand mit Referenzen zurückgibt (Z. 608-620, 1132-1137), auf dem UI-Thread. „Built ONCE per call" bedeutet: einmal pro Aufruf, nicht einmal pro Projekt.
- **Code-Auszug**:
```cpp
// McpToolsAnimator.cpp:109-115
for (const ContentAsset& a : walkContentAssets(*m_content, {AnimationClip, BlendSpace}, 2000, truncated)) {
    const HE::UUID id = HE::AssetRefs::assetUuidOfFile(a.abs);   // Datei öffnen
    if (!(id == HE::UUID{})) m_byId.emplace(std::make_pair(id.hi, id.lo), a.rel);
}
```
- **Lösung**: Ein projektweiter, lazy gebauter Index `uuid → relPath` neben `EditorAssetTypeCache` (gleiche Invalidierung wie `EditorAssetTypeCache::invalidate`, gespeist von `onAssetAppeared/Gone/Moved`), den Animator/Prefab/Sequence/… teilen. Für die Set-Tools zusätzlich: die gerade aus den Argumenten aufgelösten Pfade direkt zurückgeben statt den Index zu befragen (der Aufrufer kennt `clip`-Pfad schon).
- **Erwarteter Effekt**: Schätzung: Detailaufruf von O(Assets + Dateiöffnungen) auf O(1) nach dem ersten Aufruf; ein weniger Walk-Duplikat in drei Dateien.
- **Risiko / Verifikation**: Veraltete Einträge (Datei extern verschoben) → `missing: true` muss weiterhin erscheinen; Tests mit gelöschtem Clip. Cache-Invalidierung sorgfältig (Asset-Move über Collab-Broadcast). „Unsicher / messen", wie groß typische Projekte sind.

---

### EDT-1208 – Gleiche Tor-Logik (Pfad, Engine-readonly, Play-Mode, Lock, Dirty-Tab) in ≥ 11 Tool-Dateien und 15 Hook-Structs mit denselben vier `std::function`s
- **ID**: EDT-1208
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `McpToolsAnimator.cpp:206-235`, `McpToolsClip.cpp:177-204`, `McpToolsHc.cpp:422-478`, `McpToolsAsset.cpp:146-154`; ebenso (nicht Teil des Slice) Input, Material (2×), Particle, Prefab, Scene, Sequence, Type, Widget (`grep '"play_mode"\|"locked_by_other"'`: je 1–2 Treffer in 12 Dateien). Hook-Structs: `McpToolRegistry.h:358-1169` (`McpAssetHooks`, `McpWidgetHooks`, `McpInputHooks`, `McpMaterialHooks`, `McpPrefabHooks`, `McpTypeHooks`, `McpParticleHooks`, `McpAnimatorHooks`, `McpClipHooks`, `McpSequenceHooks`, …)
- **Problem**: Jede Familie hat ihre eigene `Doc`/`openDoc`-Kopie mit denselben Prüfungen in derselben Reihenfolge und nur leicht abgewandelten Fehlertexten, und jedes Hook-Struct deklariert wieder `isPlaying`, `lockedByOther`, `isDirty`, `reloadFromDisk` (50 Treffer dieser Namen in der Header-Datei) samt wiederholtem Kommentar „Same question, same shape and same optimistic asset policy as McpHcHooks::lockedByOther". `McpToolCommon.h` begründet `checkPath` ausdrücklich mit „two copies of a boundary are two boundaries" – bei den Gates gilt dasselbe, nur wurde dort nicht zusammengeführt.
- **Code-Auszug**:
```cpp
// Animator:206-235 ≈ Clip:177-204 ≈ Hc:425,464 ...
if (p.engine) { d.failure = failEngineReadOnly(p.rel); return d; }
if (h.isPlaying && h.isPlaying()) { d.failure = ToolResult::fail("play_mode", ...); return d; }
if (h.lockedByOther && h.lockedByOther(p.rel)) { ... "locked_by_other" ... }
if (h.isDirty && h.isDirty(p.rel)) { ... "dirty" ... }
```
- **Lösung**: (1) `struct McpAssetGate { std::function<bool()> isPlaying; std::function<bool(const std::string&)> lockedByOther, isDirty, reloadFromDisk; }` in `McpToolCommon.h`; die Familien-Hooks erben/enthalten es (`struct McpAnimatorHooks : McpAssetGate {…}`). (2) `std::optional<ToolResult> gateWrite(const McpAssetGate&, const PathCheck&, GateText)` in `McpToolCommon.cpp`, wobei `GateText` die familienspezifische Begründung für `play_mode`/`dirty` liefert (die Texte unterscheiden sich fachlich). (3) Dann pro Familie nur noch Parsen + `gateWrite`. Die Verdrahtung in `EditorApplication::setupMcpTools` füllt das Gate einmal statt zehnmal.
- **Erwarteter Effekt**: Vereinfachung: grob 300–400 Zeilen weniger über 12 Dateien, ein Ort für die Reihenfolge der Prüfungen. Keine Laufzeitwirkung.
- **Risiko / Verifikation**: Fehlercodes und -texte sind ein Teil der Tool-Schnittstelle (Modelle lesen sie): Codes exakt erhalten, Texte pro Familie parametrisieren. Tests, die `play_mode`/`locked_by_other`/`dirty` pro Familie prüfen (`tests/` nach diesen Codes greppen), laufen unverändert. Reihenfolge der Prüfungen (Engine → Play → Lock → Dirty; bei Hc: Play → Key → Lock) darf nicht kippen, sonst ändern sich Refusal-Prioritäten.

---

### EDT-1209 – `McpToolRegistry.h` (1592 Zeilen, schwere Includes) wird über `McpBridge.h` → `EditorApplication.h` in 78 Übersetzungseinheiten gezogen
- **ID**: EDT-1209
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `EditorApplication.h:46` (`#include "McpBridge.h"`), `McpBridge.h:41` (`#include "McpToolRegistry.h"`), `McpToolRegistry.h:24-36` (Includes `Renderer/IRenderer.h`, `Scripting/ScriptTypes.h`, `Types/*`, `<nlohmann/json.hpp>`)
- **Problem**: `EditorApplication.h` wird von 78 Dateien eingebunden (`grep -l`), und bringt damit `nlohmann/json.hpp`, `IRenderer.h` (nur für `EditorCameraOverride` in den Screenshot-Hooks) und ca. 1100 Zeilen reine Hook-Struct-Deklarationen in jede davon, obwohl nur `EditorApplication.cpp` (`setupMcpTools`) und die `Mcp*.cpp`-Dateien sie brauchen. Der Header ist Prosa-schwer (Block-Kommentare über Hunderte Zeilen), was Lesbarkeit hilft, aber Parsezeit kostet.
- **Code-Auszug**:
```cpp
// EditorApplication.h:46
#include "McpBridge.h"
// McpBridge.h:41
#include "McpToolRegistry.h"
```
- **Lösung**: (1) `McpToolRegistry.h` in `McpTool.h` (ToolResult, McpTool, McpToolRegistry, base64 – ohne Renderer/Scripting/Hook-Structs) und `McpToolHooks.h` (alle `Mcp*Hooks`, Registrierungsfunktionen) aufteilen; (2) `EditorApplication` hält `std::unique_ptr<HE::Ed::McpBridge>` mit Forward-Declaration statt Wert-Member (Bridge-Kommentar `EditorApplication.h:838-861` verlangt nur die Destruktionsreihenfolge relativ zu `m_mcpCameras` – ein `unique_ptr` an derselben Stelle behält sie); `McpClaudeProbe.h`/`McpLockBook.h` bleiben leicht. (3) Prosa-Blöcke in `docs/` verschieben und im Header per Verweis kürzen.
- **Erwarteter Effekt**: Schätzung: weniger Parse-Arbeit in 78 TUs (json.hpp dominiert); Größenordnung 1–3 % der Editor-Gesamtbuildzeit bei inkrementellen Änderungen am Header, nicht gemessen.
- **Risiko / Verifikation**: Reines Include-/Ownership-Refactoring, aber der `unique_ptr` verlagert die Konstruktion nach `EditorApplication.cpp`; Tests, die `McpBridge` direkt nutzen (Socket-Tests), sind unberührt. Mit `-ftime-trace` an einem typischen Panel-TU messen (unsicher / messen).

---

### EDT-1210 – `checkPath` prüft die Konfinierung nur lexikalisch; Symlinks innerhalb der Content-Wurzel werden nicht aufgelöst
- **ID**: EDT-1210
- **Kategorie**: F (Korrektheit/Sicherheit, außerhalb des Perf-Rasters)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `McpToolCommon.cpp:162-180` (`lexically_normal`, `isUnder`), Kommentar `:165-167`; Gegenbeispiel `McpClientSetup.cpp:195` nutzt `weakly_canonical`
- **Problem**: Der Kommentar nennt die Prüfung „the check that actually keeps the promise: resolution is symbolic, so a path can come back looking fine and still point outside". Implementiert ist aber nur `std::filesystem::path(absRaw).lexically_normal()` und ein Präfixvergleich der Pfadelemente. Ein Symlink (oder Junction) im Content-Ordner, der aus dem Projekt zeigt, besteht die Prüfung; `asset_delete`/`asset_move`/`asset_list` wirken dann auf Dateien außerhalb des Projekts (soweit das Ziel `.hasset`-Namen/Header-Sniffs durchlässt). Das Versprechen „no file access" in `McpToolRegistry.h:5-7` ist damit schwächer als dokumentiert.
- **Code-Auszug**:
```cpp
// McpToolCommon.cpp:163
std::filesystem::path abs = std::filesystem::path(absRaw).lexically_normal();
...
if (!isUnder(abs, contentRoot) && !(engineRoot.empty() ? false : isUnder(abs, engineRoot)))
```
- **Lösung**: Nach dem lexikalischen Test `weakly_canonical(abs, ec)` bilden (und ebenso für die Roots einmalig) und den Präfixvergleich auf die kanonischen Pfade wiederholen; `p.abs` als kanonische Form zurückgeben. Kosten: wenige `lstat`-Aufrufe pro Tool-Aufruf.
- **Erwarteter Effekt**: Keine Performance; schließt die Lücke zwischen Kommentar und Verhalten.
- **Risiko / Verifikation**: Auf macOS ist `/var` ein Symlink auf `/private/var`: die Roots müssen *ebenfalls* kanonisiert werden, sonst lehnt jeder Pfad ab. Windows: Junctions/8.3-Namen testen. Unsicher: ob Projekte legitime Symlinks im Content nutzen (dann Verhalten ändern, gegebenenfalls Option). Test: Symlink in temporärem Content auf Verzeichnis außerhalb → `asset_resolve` muss `invalid_path` liefern.

---

### EDT-1211 – Lokale Kopien gemeinsamer Hilfen (`strArg`, `objectSchema`, `lowered`, `pinTypeName`, `uuidJson`)
- **ID**: EDT-1211
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `McpToolsEntity.cpp:95-100`, `:234-243` und `McpToolsHc.cpp:363-368`, `:379-388` (Kopien von `McpToolCommon.cpp:18`, `:51`); `lowered` in `McpToolsApi.cpp:44`, `McpToolsBuild.cpp:29`, `McpToolsHc.cpp:203` (+ `McpToolsSettings.cpp:43`); `pinTypeName` in `McpToolsApi.cpp:51` und `McpToolsHc.cpp:92` (identische Tabelle für `HorizonCode::PinType`; weitere in Material:707, Type:76); `uuidJson` in `McpToolsAnimator.cpp:79`, `McpToolsAsset.cpp:78` (+ Material:97, Particle:236, Prefab:113)
- **Problem**: Entity und Hc binden `McpToolCommon.h` nicht ein und definieren `strArg`/`objectSchema` selbst (anonymer Namespace, identische Semantik); `lowered` existiert viermal, `uuidJson` fünfmal (Animator nutzt `static_cast<uint64_t>`, Asset nicht – gleiches JSON). Zwei Pin-Namenstabellen für denselben Enum. Divergenz-Risiko genau der Art, die `McpToolCommon.h` für Grenzfälle beschreibt.
- **Code-Auszug**:
```cpp
// McpToolsEntity.cpp:95 und McpToolsHc.cpp:363 – identisch zu McpToolCommon.cpp:18
std::string strArg(const json& args, const char* key) { if (!args.is_object()) return {}; ... }
```
- **Lösung**: `McpToolCommon.h` einbinden, lokale Kopien löschen; `lowered`, `uuidJson`, `pinTypeName(HorizonCode::PinType)` nach `McpToolCommon` (bzw. `HcNodeDocs`/Hc-Header) heben.
- **Erwarteter Effekt**: Vereinfachung, ca. 60–80 Zeilen; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Reines Löschen/Umhängen; Kompilierung prüft (Mehrdeutigkeit zwischen lokaler und Common-Variante tritt nur auf, solange beide sichtbar sind). Bestehende Tests genügen.

---

### EDT-1212 – Kleine O(n²)-Stellen und Allokationen in den HorizonCode-/API-Tools
- **ID**: EDT-1212
- **Kategorie**: D / C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `McpToolsHc.cpp:533-545` (`droppedLinks`), `:996`, `:1073`, `:1077-1079` + `:1104-1106` (`idsBefore`), `:353-359` + `:739` (`apiAllowed` je Zeile), `:722-725`/`:741-744` (`lowered` je Kandidat); `McpToolsApi.cpp:586-589` (`lowered` je Zeile)
- **Problem**: `droppedLinks` prüft jeden alten Link per `any_of` gegen alle neuen Links: O(L²) plus vollständige Kopie von `d.g->links` vor jeder Mutation (`before`). `hc_connect` kopiert außerdem alle Knoten-IDs und sucht jeden Knoten mit `std::find` in dieser Liste (O(N²)). `apiAllowed` baut für jede Registry-Zeile einen neuen `std::vector<const char*>`; `hc_node_types`/`api_list` allokieren pro Kandidat 2–3 lowercase-Strings.
- **Code-Auszug**:
```cpp
// McpToolsHc.cpp:538
const bool still = std::any_of(g.links.begin(), g.links.end(), [&b](const HC::Link& l){ ... });
// :1105
if (std::find(idsBefore.begin(), idsBefore.end(), n.id) == idsBefore.end())
```
- **Lösung**: Links nach dem Muster „sort + `set_difference`" oder über `unordered_set<uint64>` (Schlüssel `CollabDocSync::linkKey`) vergleichen; für neue Knoten reicht die Differenz `nextId` vorher/nachher (Ids werden fortlaufend vergeben, `n.id = d.g->nextId` Z. 873) oder `unordered_set`. `apiAllowed`: Gruppenvektor einmal pro Dokument bauen; `lowered`-Treffer vorberechnen oder `strcasestr`-artigen Vergleich ohne Allokation.
- **Erwarteter Effekt**: Schätzung: nur bei Graphen mit mehreren tausend Links/Knoten merkbar; sonst im Rauschen (Editor-only, pro Tool-Aufruf, nicht pro Frame).
- **Risiko / Verifikation**: Ausgabe (`replacedLinks`, `spawnedNodes`) muss gleiche Reihenfolge behalten (Reihenfolge der alten `links` bzw. `nodes`). Unsicher / messen, ob es relevante Graphgrößen gibt.

---

### EDT-1213 – Registry: `tools/list` wird bei jedem Aufruf neu gebaut, lineares `find`, doppelte Handler-Felder, Hooks-Kopie je API-Zeile
- **ID**: EDT-1213
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `McpToolRegistry.cpp:96-101` (`find`), `:113-125` (`listPayload`), `McpToolRegistry.h:109-117` (`handler` + `handlerCtx`, `invoke`), `McpToolRegistry.cpp:69-77`; `McpToolsApi.cpp:462` (`[row, hooks]`, Kopie von `McpApiHooks` pro Zeile), `:633-637`
- **Problem**: Neben den ~120 handgeschriebenen Tools registriert `registerApiTools` eine Tool-Instanz je pure `HE::api`-Zeile (Anzahl nicht gemessen, vermutlich hunderte). `listPayload` kopiert dann pro Aufruf alle Schemas in ein neues JSON; `find` vergleicht linear Strings (in `batch` bis 100×). Jedes API-Tool trägt eine eigene Kopie von drei `std::function`s (`McpApiHooks`). Zwei Handler-Felder pro Tool (`handler`, `handlerCtx`) erfordern den Wrapper in `add`, obwohl nur Batch/Screenshot (und `add` selbst) `handlerCtx` nutzen (`grep`: 3 Dateien).
- **Code-Auszug**:
```cpp
// McpToolRegistry.cpp:96
for (const auto& t : m_tools) if (t.name == name) return &t;
// McpToolsApi.cpp:462
t.handler = [row, hooks](const json& args) -> ToolResult { ...
```
- **Lösung**: `unordered_map<std::string, size_t>` neben `m_tools` (Registrierung ist einmalig); `listPayload` einmal bauen und cachen (Invalidierung in `add`/`clear`); `McpApiHooks` als `shared_ptr` capturen; langfristig nur `handlerCtx` (alle Lambdas bekommen den ungenutzten ctx-Parameter) – niedriger Nutzen, nur bei Gelegenheit.
- **Erwarteter Effekt**: Schätzung: marginal (Mikrosekunden–niedrige Millisekunden pro `tools/list`); der eigentliche Hebel ist die Antwortgröße (siehe „Unsicher").
- **Risiko / Verifikation**: Cache muss bei `registry.add` nach dem ersten `tools/list` (z. B. spätes `registerBatchTool`) invalidiert werden; Test „Reihenfolge der Tools in tools/list" beibehalten (Batch als letztes).

---

### EDT-1214 – Kommentar `McpTool::mutates` beschreibt ein Verhalten, das es nicht gibt
- **ID**: EDT-1214
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `McpToolRegistry.h:99-101`; tatsächliche Verwendung `McpBridge.cpp:478`, `McpToolsBatch.cpp:257`
- **Problem**: Der Kommentar sagt „Mutating tools are refused while play-in-editor runs, and are the ones written to the console log". `grep '\->mutates\|\.mutates'` findet nur die Log-Zeile in der Bridge und im Batch; ein Play-Mode-Refuse existiert dort nicht (jede Familie prüft selbst, siehe EDT-1208, und die Entity-Tools über das Gateway). Wer sich auf den Kommentar verlässt, geht von einer zentralen Sperre aus, die fehlt.
- **Code-Auszug**:
```cpp
// McpToolRegistry.h:99
// Mutating tools are refused while play-in-editor runs, and are the ones
// written to the console log. Reading tools are neither.
bool mutates = false;
```
- **Lösung**: Kommentar auf „wird im Konsolen-Log vermerkt" kürzen — oder die zentrale Sperre tatsächlich in `McpBridge::dispatch` implementieren und die zehn Familien-Prüfungen aus EDT-1208 darauf stützen (Verhaltensänderung: Entity-Tools lieferten dann denselben Code, aber Hc/Asset-Familien hätten ihre eigenen Texte nicht mehr).
- **Erwarteter Effekt**: Weniger Irreführung.
- **Risiko / Verifikation**: Nur Kommentar: keins. Zentrale Sperre: Tests der einzelnen `play_mode`-Texte.

---

### EDT-1215 – `McpClaudeProbe.cpp` und `McpClientSetup.cpp` duplizieren `commandLine` und Trim-Helfer
- **ID**: EDT-1215
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `McpClaudeProbe.cpp:12-17` ↔ `McpClientSetup.cpp:15-20` (`commandLine`, zeichengleich); `McpClaudeProbe.cpp:19-25` (`trimmed`) ↔ `McpClientSetup.cpp:50-59` (`firstLine` mit eigener Trim-Logik)
- **Problem**: Die Probe-Datei inkludiert bereits `McpClientSetup.h`, definiert aber `commandLine` neu (mit dem Kommentar „Same transcript shape the 'Add to Claude' button prints"). `trimmed` und das Trim in `firstLine` sind dieselbe Operation.
- **Code-Auszug**:
```cpp
std::string commandLine(const std::filesystem::path& exe, const std::vector<std::string>& args)
{ std::string line = exe.string(); for (const std::string& a : args) { line += ' '; line += a; } return line; }
```
- **Lösung**: `commandLine` (und `trimmed`) in `McpClientSetup.h` als `inline`/freie Funktion deklarieren und in der Probe wiederverwenden.
- **Erwarteter Effekt**: ~15 Zeilen weniger; keine Laufzeitwirkung (läuft einmal pro Klick/Probe, Worker-Thread).
- **Risiko / Verifikation**: Keins; Tests `McpClientSetup`/`McpClaudeProbe` mit synthetischen `Proc::Result` bleiben gültig.

---

### EDT-1216 – Statusleiste und Kamera-Labels bauen pro Frame Strings, solange die Bridge läuft
- **ID**: EDT-1216
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `McpStatusBar.cpp:38-55`, `:77-87` (`FooterWidth`), `:89-97` (`DrawFooter`); `McpCameraGizmos.cpp:38-43`, `:127` (`labelFor` je Kamera je Frame)
- **Problem**: `FooterWidth` und `DrawFooter` rufen je `stateOf` + `textFor` (neuer `std::string`, im Busy-Fall `snprintf`) – dasselbe Ergebnis zweimal pro Frame, nur solange die Bridge läuft (Standard: aus, `State::Off` kehrt vorher zurück). `drawViewportLabels` baut pro Kamera pro Frame `labelFor(id)` (`snprintf` + `std::string`) und ruft `CalcTextSize`. Beides ist sehr klein (≤ 4 Clients) und nur aktiv, wenn die Funktion benutzt wird.
- **Code-Auszug**:
```cpp
// McpCameraGizmos.cpp:127
const std::string label = labelFor(id);
const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
```
- **Lösung**: Optional: Footer-Text pro Frame einmal cachen (statische Struktur mit `count`/`state` als Schlüssel) und `labelFor` in einen kleinen `char[16]`-Puffer schreiben lassen. Nur sinnvoll, wenn man ohnehin hier arbeitet.
- **Erwarteter Effekt**: Nicht messbar; nur der Vollständigkeit halber (Schätzung: < 1 µs pro Frame).
- **Risiko / Verifikation**: Keins; die Kommentare nennen `FooterWidth` und `DrawFooter` als synchron zu haltende Paarung (McpStatusBar.cpp:34-37) – ein Cache würde genau das sicherstellen.

---

### EDT-1217 – `MaterialEditorPanel.h` zieht `EditorUI.h`, `imgui.h`, `CollabDocSync.h`, `AssetAutosave.h` für Vorwärtsdeklarierbares
- **ID**: EDT-1217
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MaterialEditorPanel.h:2-7`
- **Problem**: Das Interface braucht `AppContext&` (Forward-Declaration `struct AppContext;` steht bereits in `EditorUI.h:9`), `ImVec2` (nur in `render`), `CollabDocSync::DocBindings` (ein `std::vector<DocBinding>`, Rückgabe *by value* → vollständiger Typ nötig) und `HE::Ed::AssetSnapshotSource` (nur als Vector-Referenz → Forward-Declaration reicht). Es hat nur vier Includer (`ContentBrowserPanel.cpp`, `EditorApplication.cpp`, `EditorUI.cpp`, `MaterialEditorPanel.cpp`), daher geringer Hebel.
- **Code-Auszug**:
```cpp
#include "CollabDocSync.h"
#include "EditorUI.h"
#include <imgui.h>
...
#include "AssetAutosave.h"
```
- **Lösung**: `struct AppContext;`, `struct ImVec2;`/`<imgui.h>` bleibt wegen Wertparameter `const ImVec2&` (Referenz → Forward-Declaration genügt), `namespace HE::Ed { struct AssetSnapshotSource; }`; nur `CollabDocSync.h` bleibt. Auch das Paar `isDirty`/`isDirtyByContentPath` und `reload…` existiert wortgleich in `InputAssetPanel` (laut Kommentar Z. 30-34) – siehe Cross-Modul-Hinweise.
- **Erwarteter Effekt**: Vernachlässigbar (4 TUs).
- **Risiko / Verifikation**: Prüfen, dass die vier Includer die Typen anderweitig einbinden (Compile-Fehler zeigen es sofort).

---

## Gelesen

Ja, alle 27 Dateien des Slice 12 vollständig gelesen (MaterialEditorPanel.h, McpBridge.cpp/.h, McpCameraGizmos.cpp/.h, McpClaudeProbe.cpp/.h, McpClientCameras.h, McpClientSetup.cpp/.h, McpLockBook.h, McpStatusBar.cpp/.h, McpToolCommon.cpp/.h, McpToolRegistry.cpp/.h (beide Hälften), McpToolsAnimator.cpp, McpToolsApi.cpp/.h, McpToolsAsset.cpp, McpToolsBatch.cpp, McpToolsBuild.cpp, McpToolsClip.cpp, McpToolsCollab.cpp, McpToolsEntity.cpp, McpToolsHc.cpp); zusätzlich gezielt gelesen: `EditorCommands.cpp:400-430`, `EditorApplication.cpp:11355-11398`, `EditorSettingsPanel.cpp:762-791`, `HorizonWorld.h` (Id-Index).

## Editor Slice 13 – McpToolsInput.cpp … McpToolsTerrain.cpp

## Zusammenfassung

Der Slice (10 Dateien, 10 464 Zeilen) sind die MCP-Tool-Handler fuer Input, Material, Particle, Prefab, Scene, Screenshot, Sequence, Settings, TaskBoard und Terrain. Nichts davon laeuft pro Frame im Draw-Pfad; jeder Handler ist ein einmaliger Aufruf eines externen Clients. Die Schwere ist deshalb durchgehend begrenzt: 0 Hoch, 9 Mittel, 13 Niedrig. Die Dateien sind sorgfaeltig kommentiert und konsistent aufgebaut. Die groesste Pflegelast ist Duplikation: dieselben Gate-Ketten (Play/Lock/Dirty/Engine), dieselben kleinen Helfer (`uuidJson`, `vec3Arg`, `failFor`, `lowered`) und dieselben Validierungsbloecke stehen in mehreren Dateien. Die groesste Laufzeit steckt in den Terrain-Tools (Komplettkopie plus CBOR/JSON/Base64-Rundlauf pro Brush-Dab) und in `commitGraph` (doppelte Codegen pro Material-Graph-Edit). Keine Messdaten vorhanden; Laufzeitschaetzungen sind gekennzeichnet.

## Funde

### EDT-1301 – Terrain-Brush: Komplettkopie und Serialisierungs-Rundlauf pro Dab
- **ID**: EDT-1301
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsTerrain.cpp:506-512, 549-555, 199-227` (sculpt); `:682-687, 725-731` (mountain); `:809-816, 866, 874-882` (paint)
- **Problem**: Jeder `terrain_sculpt`/`terrain_mountain`/`terrain_paint`-Aufruf kopiert das gesamte `TerrainComponent` (Hoehenfeld bis `kTerrainMaxResolution`², Weightmaps, Wasser). Danach serialisiert `componentsOf` die komplette Entity-Komponente nach CBOR und parst sie nach JSON (inklusive Dekodierung der Base64-Blobs der alten Daten). Die neuen Daten werden wieder Base64-kodiert, `Command::setComponents` serialisiert erneut nach CBOR, und das Gateway laedt sie ueber den Scene-Loader (wieder Base64-Dekodierung); fuer Undo kommt vermutlich noch ein Vorher-Zustand dazu. `terrain_paint` haelt zusaetzlich zwei Vorher-Kopien der Weightmaps (`before`, `before2`) nur fuer den Vergleich. Ein Client, der ein Gelaende mit 100 Dabs formt, zahlt das 100-mal; bei 1025² Hoehen sind das ~4 MB pro Kopie.
- **Code-Auszug**:
```cpp
TerrainComponent work = *r.tc;                       // 506
...
json terrain = componentsOf(*r.world, r.entity)["terrain"];   // 549
terrain["sculptHeightsB64"] = SceneSerializer::encodeBase64(...);  // 553
...
const std::vector<std::uint8_t> before  = work.layerWeights;  // 815
const std::vector<std::uint8_t> before2 = work.layerWeights2; // 816
```
- **Lösung**: (1) Statt `componentsOf` des ganzen Entity-JSON nur das `terrain`-Patch-Objekt mit den tatsaechlich geaenderten Feldern (`sculptHeightsB64` + `resolution`, bzw. Weightmaps) an `setComponents` geben, falls der Gateway Teil-Patches merged (zu pruefen, siehe Risiko). (2) `TerrainPaint::paint` / `TerrainSculpt::apply` melden bereits `changed`; fuer Paint ein `changedTexels` im Ergebnis ergaenzen, dann entfallen die beiden Vorher-Kopien. (3) Optional mehrere Dabs pro Aufruf (`dabs: [...]`) akzeptieren, damit der Rundlauf einmal pro Stroke statt einmal pro Dab anfaellt.
- **Erwarteter Effekt**: Schaetzung: Faktor 2-4 weniger Kopien/Base64-Arbeit pro Dab; (3) senkt die Kosten linear mit der Dab-Anzahl. Messen mit einer 1025²-Landschaft und 100 Aufrufen.
- **Risiko / Verifikation**: Das Verhalten (ein Undo-Schritt pro Aufruf, `changed`-Zaehlung, Carry-over der Runtime-Felder) muss exakt bleiben. Pruefen, wie `Command::setComponents` Teil-Patches behandelt (`replace` vs. `merge`), sonst gehen Felder verloren. Bestehende Terrain-MCP-Tests unter `tests/` (per `grep -l terrain_sculpt tests/`) als Absicherung; Test fuer "Undo stellt Hoehenfeld bytegenau wieder her" ergaenzen.

### EDT-1302 – `terrain_info` berechnet das komplette Hoehenfeld nur fuer Min/Max
- **ID**: EDT-1302
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsTerrain.cpp:274-289`
- **Problem**: Pro Landschaft und pro Aufruf wird `computeTerrainHeightField(tc)` ueber das ganze Raster ausgewertet (bei nicht gesculpteter Landschaft fBm mit `octaves` Schichten pro Vertex) und nur `min_element`/`max_element` daraus gelesen. Ohne `uuid` laeuft das fuer alle Landschaften der Szene. Ein Client pollt `terrain_info` typischerweise vor und nach jeder Aenderung.
- **Code-Auszug**:
```cpp
const std::vector<float> field = computeTerrainHeightField(tc);
float mn = 0.0f, mx = 0.0f;
if (!field.empty())
{
    mn = *std::min_element(field.begin(), field.end());
    mx = *std::max_element(field.begin(), field.end());
}
```
- **Lösung**: Min/Max in einem Durchlauf (`std::minmax_element`) ist nur Kosmetik. Wirksam: bei `sculptHeights.size() == res*res` Min/Max direkt ueber `sculptHeights` ohne Neuberechnung; fuer die rein prozedurale Landschaft das Ergebnis in `TerrainComponent` cachen (Invalidierung bei Parameteraenderung) oder `heightRange` als optional markieren (`includeRange: true`).
- **Erwarteter Effekt**: Schaetzung: bei 1025² und 8 Oktaven mehrere Mio. Noise-Auswertungen pro Landschaft und Aufruf; mit Cache/Abkuerzung O(1) bzw. ein linearer Pass. Messen.
- **Risiko / Verifikation**: Ergebnis muss identisch bleiben: gleiche Quelle wie die Chunk-Erzeugung (`computeTerrainHeightField`). Test: `heightRange` fuer sculpted und prozedural vor/nach der Aenderung vergleichen.

### EDT-1303 – `commitGraph`: Shader-Codegen laeuft zweimal pro Graph-Edit
- **ID**: EDT-1303
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:1414-1449` (Aufrufer: add_node, remove_node, connect, disconnect, set_pin_default, set_node)
- **Problem**: Der "Trockenlauf" ruft `HE::generateFragment` auf und verwirft das Ergebnis (nur `gen.glsl.empty()` wird geprueft). Danach schreibt der Code `materialGraphToJson(g)` in das Asset, und `regenerateMaterialFromGraph` (`ContentManager.cpp:790-818`) parst diesen JSON erneut und ruft `generateFragment` ein zweites Mal (inklusive der Funktions-Loader-Aufrufe). Jeder einzelne Verbindungs-/Knoten-Edit eines Clients zahlt zweimal Codegen plus Graph-Serialisierung/Parsen. Ein Client, der ein Material mit 30 Knoten und 40 Links aufbaut, ruft das ~70-mal.
- **Code-Auszug**:
```cpp
const HE::MatShaderGen gen = HE::generateFragment(g, loader, nullptr);
if (gen.glsl.empty()) return ToolResult::fail(...);
...
cm.regenerateMaterialFromGraph(m.id);   // 1441: generateFragment erneut
```
- **Lösung**: `ContentManager::regenerateMaterialFromGraph` liefert heute `void` und bricht intern bei leerem GLSL still ab. Variante A: Rueckgabewert `bool` (false = Codegen leer, Asset unveraendert) und den Trockenlauf streichen; da die Funktion bei leerem GLSL vor jeder Schreibaktion zurueckkehrt, bleibt "Refusal ist No-op" erhalten, solange `nodeGraphJson` erst bei `true` gesetzt wird (Reihenfolge in der ContentManager-Funktion anpassen oder den Graph-JSON dort uebergeben). Variante B: Overload `regenerateMaterialFromGraph(id, const MatShaderGen&)`, der das vorberechnete `gen` anwendet.
- **Erwarteter Effekt**: Schaetzung: halbe Codegen-Zeit pro Graph-Edit; dazu entfaellt ein JSON-Parse. Messen mit einem grossen Master-Material.
- **Risiko / Verifikation**: `regenerateMaterialFromGraph` wird auch beim Laden aufgerufen (Cross-Modul, `HE_Core`); Signaturaenderung muss kompatibel bleiben (Rueckgabewert ignorierbar). Tests: `tests/` mit `material_add_node`/`material_connect` (Grep nach den Tool-Namen) und ein Test, der einen Graph ohne Output-Node (Codegen leer) uebergibt und prueft, dass Datei und Asset unveraendert bleiben.

### EDT-1304 – `material_add_node` und `material_set_node` duplizieren ~90 Zeilen Validierung
- **ID**: EDT-1304
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:1763-1843` (add) und `:2767-2888` (set)
- **Problem**: Beide Handler pruefen `s` (FunctionCall-Pfad, Textur-Pfad), `p` (Array 1-4 Zahlen, `paramCount == 0`), `min/max` (nur ParamFloat, beide, `min < max`), `group/tooltip` (nur Param-Knoten) und `position` ([x, y]) mit praktisch identischem Code und Meldungen. Aenderungen (z. B. neue Knotenart mit eigenem `s`-Format) muessen an zwei Stellen erfolgen; der Beleg dafuer ist, dass die Fehlertexte schon leicht auseinanderlaufen ("wired with material_connect" vs. "... or material_set_pin_default", `set_node` kennt zusaetzlich die Output-Sonderfaelle).
- **Code-Auszug**:
```cpp
if (nt == HE::MatNodeType::FunctionCall) { ... checkPath(*cm, s, true, "s") ... }   // 1765
if (HE::matNodeSamplesTexture(nt) && !s.empty()) { ... }                           // 1780
if (haveS && nt == HE::MatNodeType::FunctionCall) { ... }                          // 2769 (Kopie)
if (haveS && HE::matNodeSamplesTexture(nt) && !s.empty()) { ... }                  // 2784 (Kopie)
```
- **Lösung**: Eine `struct NodeEditArgs { bool haveS; std::string s; std::vector<float> p; ...; }` plus `bool parseNodeEditArgs(ContentManager&, const json&, HE::MatNodeType, ToolResult& failure, NodeEditArgs& out)`. Add und Set rufen sie auf; nur die Unterschiede (Output-Regeln, `type`-Pruefung, Default-Position) bleiben im jeweiligen Handler. Zusaetzlich `nodeIdArg` (2146) auch fuer `id` in remove/set verwenden (1923-1927, 2718-2722) und die doppelte "Id ist eine Comment-Box"-Schleife (1931-1935, 2726-2730) in `findNodeOrFail` ziehen.
- **Erwarteter Effekt**: ca. -90 Zeilen, eine Wahrheit fuer Argumentformen (Schaetzung).
- **Risiko / Verifikation**: Fehlertext und Fehlercode muessen identisch bleiben (Clients parsen Codes). Bestehende MCP-Material-Tests laufen lassen; Snapshot der Fehlermeldungen vorher/nachher diffen.

### EDT-1305 – Terrain: Brush-Rahmen viermal kopiert, `terrain_lake`-Handler mit 180 Zeilen und fuenf Aktionen
- **ID**: EDT-1305
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsTerrain.cpp:506-512 / 557-568`, `:682-687 / 733-742`, `:809-813 / 884-894`, `:984-989 / 1123-1131`; Lake-Handler `:957-1138`
- **Problem**: Jedes Mutations-Tool wiederholt (a) die "Work-Kopie + keepWeightmap/keepBuiltRes/keepBuiltCps/wasDirty/dirty=false"-Einleitung und (b) das Fuellen von `CarryOver` plus `writeBack`. Jede neue Runtime-Eigenschaft von `TerrainComponent`, die erhalten bleiben muss, muss viermal nachgezogen werden. Zusaetzlich ist `terrain_lake` ein einziger Handler, der per String-Vergleich `action == "create"/"reshape"/"dig"/"level"` verzweigt und die Parameter jeweils selbst liest (Flag-Parameter-Muster).
- **Code-Auszug**:
```cpp
TerrainComponent work = *r.tc;
const HE::UUID      keepWeightmap = work.weightmapTextureId;
const std::uint32_t keepBuiltRes  = work.builtRes;
const std::uint32_t keepBuiltCps  = work.builtChunksPerSide;
const bool          wasDirty      = work.dirty;
work.dirty = false;
```
- **Lösung**: `struct TerrainEdit { TerrainComponent work; CarryOver keep; bool wasDirty; }` mit `TerrainEdit begin(const Terrain&)` und `ToolResult commit(EditorCommands&, Terrain&, TerrainEdit&, json patch, bool weightsDirty, const McpTerrainHooks&)`, das `fullRebuild`/`regionDirty`/Dirty-Rechteck aus `work` ableitet. Den Lake-Handler in `lakeCreate/lakeReshape/lakeDig/lakeLevel/lakeRemove` aufteilen (je ~25 Zeilen) und nur Plan/Validierung gemeinsam halten.
- **Erwarteter Effekt**: ca. -60 Zeilen im Rahmen, Handler < 60 Zeilen (Schaetzung).
- **Risiko / Verifikation**: `terrain_paint` setzt `weightsDirty = true`, `regionDirty = false` und lässt `work.dirty` nicht zurueck (kein `work.dirty = false` in Zeile 809-813) - dieser Unterschied muss im Helfer erhalten bleiben. Terrain-MCP-Tests und Undo-Test vor/nach.

### EDT-1306 – `sequence_info`/`sequence_write` bauen pro Aufruf einen Index ueber alle Clips und Sounds des Projekts
- **ID**: EDT-1306
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsSequence.cpp:137-152, 157-165`
- **Problem**: Sobald die Sequenz auch nur einen Asset-Verweis enthaelt, laeuft `assetPaths`: `walkContentAssets` (rekursiver Dateibaum, Sortierung, Header-Sniff aller `.hasset`, Limit 5000) und dann pro Treffer `AssetRefs::assetUuidOfFile` (Datei oeffnen). Das passiert bei jedem `sequence_info` und jedem `sequence_write` (via `stateJson`), obwohl meist nur wenige UUIDs aufzuloesen sind. `ContentManager::pathForId(UUID)` existiert (ContentManager.h:73) und liefert genau diese Rueckrichtung als reinen Lookup ueber die Disk-Registry.
- **Code-Auszug**:
```cpp
for (const ContentAsset& a : walkContentAssets(
         content, { HE::AssetType::AnimationClip, HE::AssetType::Audio }, 5000, truncated))
{
    const HE::UUID id = HE::AssetRefs::assetUuidOfFile(a.abs);
    if (id != HE::UUID{}) out[id] = a.rel;
}
```
- **Lösung**: `refs` (bereits per `sequenceAssetRefs` gesammelt) einzeln mit `content.pathForId(id)` aufloesen und `unresolved` fuellen, wenn leer; `assetPaths` und `PathIndex` entfallen. Zusaetzliche Typpruefung (Clip/Audio) nur falls noetig ueber `EditorAssetTypeCache::assetTypeOf`.
- **Erwarteter Effekt**: O(#Verweise) statt O(#Assets im Projekt) pro Aufruf; Schaetzung: bei Projekten mit mehreren tausend Assets Sekundenbruchteile bis Sekunden gespart. Messen.
- **Risiko / Verifikation**: Pruefen, ob die Disk-Registry (`m_diskRegistry`) aktuell ist, wenn Dateien gerade neu erzeugt wurden und ob Engine-Content ("Engine/...") identisch aufgeloest wird; sonst waere `unresolvedAssets` ein Verhaltenswechsel. Test: Sequenz mit Clip aus Projekt und aus Engine-Content, frisch kopierte Datei ohne Refresh.

### EDT-1307 – Gate-Kette (Engine/Play/Lock/Dirty) und `no_project`-Pruefung in jedem Tool-Modul neu geschrieben
- **ID**: EDT-1307
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `McpToolsInput.cpp:248-275` (openDoc), `McpToolsMaterial.cpp:268-297` (openMat), `McpToolsParticle.cpp:169-197` (openEmitter), `McpToolsSequence.cpp:539-564` (openSequence); `no_project` in `McpToolsMaterial.cpp:1016, 1507, 1742, 1916, 2191, 2312, 2493, 2711, 3567` (9 Handler, wortgleicher Text)
- **Problem**: Vier Module implementieren dieselbe Abfolge `p.engine → failEngineReadOnly; isPlaying → play_mode; lockedByOther → locked_by_other; isDirty → dirty` mit je eigenen, nur im Satzbau abweichenden Meldungen, jeweils mit einem eigenen Hook-Struct (`McpInputHooks`, `McpMaterialHooks`, `McpParticleHooks`, `McpSequenceHooks`, ...), die dieselben vier `std::function`-Felder tragen. Ein neues Gate (z. B. "Asset ist schreibgeschuetzt durch Source Control") muesste an allen Stellen ergaenzt werden. Das Material-Modul wiederholt ausserdem 9-mal die `contentRoot().empty()` → `no_project`-Abfrage.
- **Code-Auszug**:
```cpp
if (p.engine) { d.failure = failEngineReadOnly(p.rel); return d; }
if (h.isPlaying && h.isPlaying()) { d.failure = ToolResult::fail("play_mode", ...); return d; }
if (h.lockedByOther && h.lockedByOther(p.rel)) { ... "locked_by_other" ... }
if (d.openDirty) { ... "dirty" ... }
```
- **Lösung**: In `McpToolCommon` eine `struct McpWriteGate { isPlaying, lockedByOther, isDirty, ... }` (die Hook-Structs erben oder enthalten sie) und `ToolResult checkWriteGate(const McpWriteGate&, const PathCheck&, const char* tabName, const char* playWhy)`; die kontextspezifische Begruendung (warum Play-Mode hier zaehlt, welcher Tab) bleibt als Parameter. Fuer `no_project` eine `requireProject(ContentManager&, const char* what)`-Funktion, die auch in `openMat`/`openGraphForEdit` aufgerufen wird.
- **Erwarteter Effekt**: ca. -120 Zeilen ueber den Slice (Schaetzung); einheitliche Codes/Texte.
- **Risiko / Verifikation**: Texte sind Teil der Client-Schnittstelle (das Modell liest sie). Parametrisierte Begruendungen muessen die heutigen Saetze erhalten; Tests, die `"play_mode"`/`"dirty"` pruefen, laufen lassen (Grep nach den Codes in `tests/`). Die Hook-Structs liegen in `McpToolRegistry.h` (ausserhalb des Slice).

### EDT-1308 – Kleine Helfer in bis zu fuenf Dateien kopiert
- **ID**: EDT-1308
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `uuidJson`: `McpToolsMaterial.cpp:97`, `McpToolsParticle.cpp:236`, `McpToolsPrefab.cpp:113` (ausserdem `McpToolsAnimator.cpp:79`, `McpToolsAsset.cpp:78`); `vec3Arg`/`vec3Prop`: `McpToolsScreenshot.cpp:70-92`, `McpToolsPrefab.cpp:126-148`; `vec3Json`: `McpToolsScreenshot.cpp:94`, `McpToolsTerrain.cpp:177`; `failFor(CmdError, ...)`: `McpToolsPrefab.cpp:70-110`, `McpToolsTerrain.cpp:59-98`; `failNoProject`: `McpToolsSettings.cpp:23`, `McpToolsTaskBoard.cpp:112`; `lowered`: `McpToolsMaterial.cpp:1529`, `McpToolsSettings.cpp:43`; Entity-UUID als Hex: `McpToolsSequence.cpp:58-65` und `EditorCommands.cpp:406-417`; Link-JSON: `linkJson` (`McpToolsMaterial.cpp:2012`), `linkJsonOf` (`:2627`) plus Inline-Kopien `:1249`, `:1956`; `readPayload`: `McpToolsInput.cpp:118`, `McpToolsParticle.cpp:123` (ausserdem Animator, Type).
- **Problem**: Dieselben Funktionen mit gleichem Verhalten (Prefab-Kommentar 119-125 begruendet die Kopie ausdruecklich mit "nur acht Zeilen"; ueber den ganzen Editor sind es aber Dutzende). `uuidJson` weicht schon ab (Particle castet nach `uint64_t`, die anderen nicht): bei Zahlen > 2^53 koennte ein Client unterschiedliche Darstellungen sehen (unsicher, wie `json` `uint64` ausgibt - pruefen). In Material existieren `linkJson` und `linkJsonOf` im selben File mit identischem Koerper.
- **Code-Auszug**:
```cpp
json linkJson(const HE::MatGraphLink& l)   { return json{ {"srcNode", l.srcNode}, ... }; }   // 2012
json linkJsonOf(const HE::MatGraphLink& l) { return json{ {"srcNode", l.srcNode}, ... }; }   // 2627
```
- **Lösung**: `uuidJson`, `vec3Arg`, `vec3Prop`, `vec3Json`, `failNoProject`, `lowered` nach `McpToolCommon.h/.cpp` (dort liegen schon `strArg`, `intArg`). Die uuid-Hex-Funktionen (`uuidHex`/`uuidFromHex`) existieren bereits als `HE::uuidToHex`/`HE::uuidFromHex` (TaskBoard nutzt sie, `McpToolsTaskBoard.cpp:35, 128`) - `uuidOf` und Sequence darauf umstellen. Der Material-Duplikat `linkJsonOf` wird gestrichen. `failFor` als gemeinsame Funktion mit einer kleinen Tabelle (`CmdError` → Satzvorlage) und familienspezifischem `what`/Zusatzsatz.
- **Erwarteter Effekt**: ca. -150 Zeilen, ein Ort fuer Fixes (Schaetzung).
- **Risiko / Verifikation**: `uuidJson`-Formatunterschied zuerst klaeren; `HE::uuidToHex` muss dieselbe 32-stellige Kleinbuchstaben-Form liefern (`%016llx%016llx`) - gegen `uuidOf` testen. Kleine Unit-Tests auf `McpToolCommon`.

### EDT-1309 – Katalog-Listen oeffnen jede Datei zweimal bzw. parsen den vollen Inhalt nur fuer Zaehler
- **ID**: EDT-1309
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `McpToolsInput.cpp:550-576` (input_actions), `:737-751` (input_mappings); `McpToolsParticle.cpp:399-419` (particle_info); `McpToolsSequence.cpp:589-612` (sequence_info); `McpToolsPrefab.cpp:317-329` (prefab_info); gemeinsame Walk-Funktion `McpToolCommon.cpp:197-237` (ausserhalb des Slice)
- **Problem**: `walkContentAssets` durchlaeuft bei jedem Aufruf den gesamten Content-Baum (`recursive_directory_iterator`, Sortierung, `EditorAssetTypeCache::assetTypeOf` fuer jede `.hasset`, bis das Limit der gewuenschten Typen erreicht ist). Danach oeffnet jeder Katalog-Handler jedes Treffer-File erneut (`readPayload`/`HAsset::Reader`/`assetUuidOfFile`) und parst bei Particle und Sequence den vollstaendigen Graph/Dokument-JSON, nur um `nodeCount/linkCount` bzw. `bindingCount/trackCount` auszugeben. Bei 200 Katalogeintraegen sind das 200 Datei-Opens und 200 JSON-Parses, auf dem Thread, der die MCP-Anfrage bedient (je nach Bridge: UI-Thread? unsicher, siehe unten).
- **Code-Auszug**:
```cpp
for (const ContentAsset& a : found)
{
    std::string payload;
    ParticleGraph g = ParticleGraph::makeDefault();
    if (readPayload(a.abs, payload) && !payload.empty())
    { ParticleGraph parsed; if (particleGraphFromJson(payload, parsed)) g = std::move(parsed); }
    list.push_back(json{ { "path", a.rel }, { "nodeCount", ... }, { "linkCount", ... } });
}
```
- **Lösung**: (1) `ContentAsset` um die beim Sniff ohnehin gelesene UUID erweitern (Prefab/Sequence brauchen `assetUuidOfFile` nur dafuer). (2) Fuer die Zaehler entweder `limit` kleiner defaulten (z. B. 50) oder die Zaehlung als Opt-in (`counts: true`) anbieten; ansonsten sind Pfad + Typ guenstig. (3) In `walkContentAssets` den Verzeichnis-Scan mit `ContentManager::contentChangeCounter` (laut `ContentManager.h` "bewegt sich, wenn Dateien erscheinen koennen") cachen.
- **Erwarteter Effekt**: Schaetzung: Katalogaufrufe von O(Dateien im Projekt) auf O(Treffer) bzw. O(1) bei Cache-Treffer. Messen mit einem Projekt mit >5000 Assets.
- **Risiko / Verifikation**: Cache-Invalidierung (externe Dateiaenderungen, Collab-Pull) muss stimmen, sonst zeigen Kataloge veraltete Listen. Ob die Bridge die Handler auf dem UI-Thread ausfuehrt, ist in diesem Slice nicht sichtbar (`McpBridge`/`McpToolRegistry` pruefen).

### EDT-1310 – Textur-Knotenlisten und der 80-Faelle-`nodeTypeName`-Switch
- **ID**: EDT-1310
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:813-821` (nodeDetail), `:957-964` (nodeJson), `:1198-1206` (graph_info), `:1597-1607` (node_types), Gegenbeispiel `:1780`/`:2784` (`HE::matNodeSamplesTexture(nt)`); Switch `:619-705`
- **Problem**: Die acht Textur-Sample-Typen sind viermal von Hand als `case`-Liste aufgezaehlt, obwohl `HE::matNodeSamplesTexture` schon existiert und in add/set_node benutzt wird. Ein neuer Sample-Knoten muss hier an vier Stellen und im Registry-Sweep-Test ergaenzt werden. `nodeTypeName` ist eine zweite, manuell gepflegte 80-zeilige Namenstabelle neben `HE::matNodeRegistry()` (Kommentar 617-618 nennt den Grund: der Test faengt Vergessenes ab).
- **Code-Auszug**:
```cpp
case HE::MatNodeType::TextureSample:
case HE::MatNodeType::NormalMapSample:
case HE::MatNodeType::TextureArraySample:
...                                         // 4x kopiert
```
- **Lösung**: Statt der Case-Listen `if (HE::matNodeSamplesTexture(n.type))` (in `nodeDetail` und `nodeJson` direkt; in `node_types` bleibt der Bomb-Unterschied, dort `isBombSample(type)` ergaenzen). Den Enum-Namen langfristig als Feld in `MatNodeDesc` fuehren (Cross-Modul) und `nodeTypeName` streichen.
- **Erwarteter Effekt**: ca. -50 Zeilen, ein Ort fuer neue Knoten (Schaetzung).
- **Risiko / Verifikation**: Pruefen, dass `matNodeSamplesTexture` genau diese acht Typen deckt (Definition lesen, ausserhalb des Slice). Registry-Sweep-Test (`tests/test_material_graph.cpp`, nur per Grep lesen) bleibt als Absicherung.

### EDT-1311 – Particle: Output-Knoten-Suche viermal inline, obwohl `outputNode()` existiert; Hinweis wird ueberschrieben
- **ID**: EDT-1311
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsParticle.cpp:241-246` (outputNode), Kopien `:542-543`, `:575-577`, `:634-636`, `:714-716`; Hinweis `:475-484`
- **Problem**: `outputNode(const ParticleGraph&)` ist nur als const-Variante vorhanden; die mutierenden Handler suchen den Knoten per Hand (viermal dieselbe Schleife, in `particle_set` sogar dreimal hintereinander, obwohl `reset`/`set` den Output-Knoten nicht entfernen). Zusaetzlich: in `particle_info` wird `result["note"]` fuer "kein Emitter Output" gesetzt und danach bei `randomised` ohne Pruefung ueberschrieben (Zeilen 475 und 482), sodass die wichtigere Warnung verloren geht.
- **Code-Auszug**:
```cpp
ParticleGraphNode* out = nullptr;
for (ParticleGraphNode& n : e.graph.nodes)
    if (n.type == ParticleNodeType::EmitterOutput) { out = &n; break; }
```
- **Lösung**: Mutierende Overload `ParticleGraphNode* outputNode(ParticleGraph&)`; in `particle_set` den Pointer einmal holen und nach `removeNode`/`addNode` ueber `outId` erneut mit `findNode` aufloesen (Pointer koennen nach `addNode` ungueltig sein - deshalb sind die Neu-Suchen vermutlich so entstanden; `findNode(outId)` ist O(n) und genauso einfach). Beide Hinweise anhaengen (`note += ...`).
- **Erwarteter Effekt**: -12 Zeilen, ein Hinweis weniger verloren.
- **Risiko / Verifikation**: Verhalten identisch bis auf den zusaetzlich erhaltenen Hinweis; Test: Graph ohne EmitterOutput und mit RandomRange liefert beide Hinweise.

### EDT-1312 – Material: `Mat` kopiert 15 Vektoren pro Aufruf, `set_param` kopiert danach zweimal alles, um einen Wert zu lesen
- **ID**: EDT-1312
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:134-211, 3163-3164, 3293-3298`
- **Problem**: `openMat` kopiert das gesamte Asset (Graph-JSON, Param-Namen, Typen, Min/Max, Daten, Gruppen, Tooltips, Overrides, Switches, Texturen, Layer) in `Mat`; die Begruendung (dichter Pool wird beim naechsten Load bewegt) ist korrekt. `material_set_param` kopiert aber danach `Mat fresh = m;` (kopiert alles erneut) und `copyOut(fresh, *a2)` (ueberschreibt alles), nur um `paramJson(...)["value"]` fuer einen Slot zu erzeugen.
- **Code-Auszug**:
```cpp
Mat fresh = m;
if (const MaterialAsset* a2 = cm->getMaterial(m.id)) copyOut(fresh, *a2);
const int newSlot = fresh.slotOf(name);
if (newSlot >= 0) out["value"] = paramJson(fresh, ...)["value"];
```
- **Lösung**: Hilfsfunktion `json readParamValue(const MaterialAsset&, int slot)` die direkt aus `shaderParamData`/`graphParamTypes` liest (es sind nur `paramData`, `paramTypes` noetig), ohne `Mat`.
- **Erwarteter Effekt**: Schaetzung: einige KB Kopien weniger je Aufruf, vernachlaessigbar ohne Messung; Wert liegt in der Lesbarkeit.
- **Risiko / Verifikation**: `paramJson` und die Direktfassung muessen dieselbe Bool/Float/Vec-Darstellung erzeugen; vorhandene `material_set_param`-Tests.

### EDT-1313 – Material-Graph-Abfragen sind O(Knoten × Pins × Links)
- **ID**: EDT-1313
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:793-806, 906-921, 358-367, 2106-2122`
- **Problem**: `linkInto`/`connectedInputs` laufen linear ueber alle Links und werden in `nodeJson` pro Eingangs-Pin (`sourceOf`), in `chainInto` pro Schritt und beim Output-Summary aufgerufen. `paramJson` durchsucht pro Parameter alle Knoten (`inGraph`). `reaches` baut `std::set<int>` und scannt bei jedem Pop alle Links (O(V·E)).
- **Code-Auszug**:
```cpp
for (const HE::MatGraphLink& l : g.links)
    if (l.dstNode == node && l.dstPin == pin) return &l;
```
- **Lösung**: Nur wenn gemessen relevant: einmalig eine `std::unordered_map<uint64_t, const MatGraphLink*>` (Key `dstNode<<32|dstPin`) und fuer `inGraph` ein `std::set<std::string>` der effektiven Param-Namen pro `materialJson` aufbauen. Fuer typische Materialien (< 100 Knoten) ist der Gewinn klein.
- **Erwarteter Effekt**: unsicher / messen; erst bei Graphen mit > 500 Knoten sichtbar.
- **Risiko / Verifikation**: Reihenfolge der Ausgabe darf sich nicht aendern (die Links-Reihenfolge bestimmt, welcher bei doppelten `dst` gewinnt - `connect` erzwingt einen pro Pin).

### EDT-1314 – `openGraphForEdit` prueft Pfad/Typ doppelt; `material_node_types` laedt jede Funktion
- **ID**: EDT-1314
- **Kategorie**: A / E
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:1357-1376, 1642-1674`
- **Problem**: `openGraphForEdit` ruft `checkPath` und `assetTypeOf`, danach ruft `openMat` beides noch einmal. Der Zweck (eigene Funktions-Meldung) laesst sich in `openMat` ueber einen Parameter oder einen `functionRefusal`-Text erreichen. `material_node_types` ruft pro gelistetem Funktions-Asset `cm->loadAsset(a.rel)` (laedt und registriert bis zu 80 Funktionen im ContentManager, was den Pool bewegt) und parst jeden Graph, nur um die Pin-Beschreibungen zu liefern; die Beschreibung (Zeile 1491) gibt das offen zu.
- **Code-Auszug**:
```cpp
const HE::UUID id = cm->loadAsset(a.rel);
const MaterialFunctionAsset* fn = id == HE::UUID{} ? nullptr : cm->getMaterialFunction(id);
```
- **Lösung**: `openMat(..., Refuse::FunctionWithGraphEditText)`; fuer `node_types` die Funktionsgraphen ueber die FILE-Chunk-Variante lesen (wie Input/Particle: `HAsset::Reader`, kein Load), das haelt die Aussage "eine Frage veraendert ihre Antwort nicht" ein.
- **Erwarteter Effekt**: ein Datei-Open und ein Sniff weniger pro Edit; node_types ohne Pool-Seiteneffekt (Schaetzung: klein).
- **Risiko / Verifikation**: Chunk-Name/Format fuer MaterialFunction-Graphen im Reader pruefen (CHUNK-Konstante in `HAsset.h`). Test: `material_node_types` darf `isLoaded` nicht aendern.

### EDT-1315 – Prefab: toter `blob`-Puffer, doppelte CBOR-Umwandlung; `prefab_instances` mit hartem 500er-Limit
- **ID**: EDT-1315
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsPrefab.cpp:154-163, 229-230, 457-458, 586-615, 706-713`
- **Problem**: `Prefab::blob` wird in `openPrefab` aus dem Chunk kopiert und nur fuer `json::from_cbor` gebraucht; kein Aufrufer liest `p.blob` (Grep: nur Zeilen 229-230). Zusaetzlich wird in `prefab_instantiate` der gepatchte `tree` wieder nach CBOR serialisiert. In `prefab_save` wird `serializeSubtree` → `from_cbor` → ggf. Re-`to_cbor` ausgefuehrt, obwohl nur Zaehler/Root gebraucht werden. `prefab_instances` begrenzt die Pfadsuche hart auf 500 Prefabs (`walkContentAssets(..., 500, ...)`); ein Projekt mit mehr Prefabs erhaelt fuer die uebrigen `"prefab": ""`, obwohl sie existieren (kein `truncated`-Hinweis).
- **Code-Auszug**:
```cpp
p.blob = chunk->data;
p.tree = json::from_cbor(p.blob, true, false);   // blob wird danach nicht mehr gelesen
...
walkContentAssets(*cm, { HE::AssetType::Prefab }, 500, truncated)   // truncated wird ignoriert
```
- **Lösung**: `blob` aus der Struct entfernen und `from_cbor(chunk->data)` direkt (Iteratorpaar) aufrufen. Beim 500er-Limit `truncated` auswerten und im Ergebnis `prefabPathsTruncated: true` melden oder die Zuordnung ueber `cm->pathForId` loesen (siehe EDT-1306).
- **Erwarteter Effekt**: eine Kopie des Prefab-Blobs weniger (Schaetzung: klein); das Limit behebt eine stille Fehlinformation.
- **Risiko / Verifikation**: Nur additive Ausgabefelder; Test mit > 500 Prefabs entfaellt, sinnvoll als Unit-Test mit Limit-Konstante.

### EDT-1316 – Screenshot: PNG wird beim Ausliefern kopiert
- **ID**: EDT-1316
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsScreenshot.cpp:573-604`
- **Problem**: `png` ist `const std::vector<uint8_t>`, daher kopiert `r.imageBytes = png;` bis zu ~2,5 MB (Inline-Limit) vor dem Senden. Das Roh-RGBA (`rgba`, bis 3840×2160×4 = 33 MB) wird pro Aufruf neu allokiert (`std::vector` lokal). `generatedBaseName` nutzt ein nicht-atomares `static unsigned s_seq` (Zeile 401); ob zwei Bridge-Threads Handler parallel ausfuehren, ist in diesem Slice nicht erkennbar (unsicher).
- **Code-Auszug**:
```cpp
const std::vector<std::uint8_t> png = hePngEncode(...);
...
r.imageBytes  = png;
```
- **Lösung**: `std::vector<std::uint8_t> png = ...;` und `r.imageBytes = std::move(png);` (nach dem `png.size()`-Check). `rgba` als wiederverwendeten Member im `ScreenshotState` halten (`reserve` bei Groessenwechsel). `s_seq` zu `std::atomic<unsigned>`, falls Handler nebenlaeufig laufen koennen.
- **Erwarteter Effekt**: ein Memcpy von <= 2,5 MB weniger je Screenshot; `rgba`-Wiederverwendung spart bis zu 33 MB Allokation je Aufruf (Schaetzung).
- **Risiko / Verifikation**: `png.size()` vor dem Move in `result["pngBytes"]` (Zeile 577) bereits gelesen - Reihenfolge beibehalten. Kein Verhaltensunterschied.

### EDT-1317 – Input: Value-Type-Ableitung viermal, Pairs-Lambda doppelt, Tastenliste pro Aufruf neu aufgebaut
- **ID**: EDT-1317
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsInput.cpp:149-151, 537-538, 565-566, 629-630`; `:348-359`; `:197-204, 470-472`; `:671-672`
- **Problem**: Der Ausdruck `inputActionIsAxis2D(p) ? kVtAxis2D : inputActionIsAxis(p) ? kVtAxis : kVtButton` steht an vier Stellen (einer davon als `valueTypeOfActionFile`). `collectDeadNames` definiert zwei fast gleiche Lambdas (`add`/`addPad`), die sich nur im Praedikat unterscheiden. `allKeyNames()` allokiert pro `input_bindable`-Aufruf ~240 `std::string`, obwohl die Liste konstant ist. `input_action_set` liest bei Retype die Kontexte mit hartem Limit 200 (`findInputAssets(..., 200, ...)`) ohne `truncated` auszuwerten; bei mehr Kontexten bleibt `affectedMappings` unvollstaendig.
- **Code-Auszug**:
```cpp
const int vt = HE::inputActionIsAxis2D(d.payload) ? kVtAxis2D
             : HE::inputActionIsAxis(d.payload)   ? kVtAxis : kVtButton;
```
- **Lösung**: `int valueTypeOfPayload(const std::string& json)` einmal definieren und an den vier Stellen aufrufen. Ein Lambda `addDead(field, name, pred)` mit `bool(*)(const std::string&)`. `allKeyNames()` als `static const std::vector<std::string>` (Funktions-Static, thread-sicher in C++11). `truncated` bei `affectedMappings` melden.
- **Erwarteter Effekt**: -15 Zeilen, ~240 Allokationen weniger je `input_bindable` (Schaetzung).
- **Risiko / Verifikation**: Reine Umformung; Ausgabe identisch. SDL-Namenstabelle ist zur Laufzeit konstant. Vorhandene Input-MCP-Tests.

### EDT-1318 – Settings: Drei-Zustands-Rueckgabe `setCollisionLayerKey`, doppelter Katalog-Lookup, Schreiben ohne Aenderung
- **ID**: EDT-1318
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsSettings.cpp:381-429, 685-704, 706-714, 728-730`
- **Problem**: `setCollisionLayerKey` kodiert drei Ausgaenge ueber `bool`-Rueckgabe, `handled`-Out-Parameter und `err`: "nicht meine Form" (false, handled=false), "meine Form, Wert falsch" (false, handled=true), "gesetzt" (true). Der Aufrufer verzweigt darauf in einem `if / else if / else`-Gebilde. `findProjectSetting(key)` wird in `settings_set` zweimal ausgefuehrt (692 und 729) und linear gesucht. Das Manifest wird auch dann geschrieben, wenn `changed == false` (Kommentar 706-708 begruendet es); das ist eine Dateischreibung pro "unveraendertem" Aufruf (Schaetzung: vernachlaessigbar).
- **Code-Auszug**:
```cpp
bool handled = false;
if (setCollisionLayerKey(*proj, key, value, err, changed, handled)) touchedLayers = true;
else if (handled) return ToolResult::fail("invalid_argument", err);
else { const ProjSetting* d = findProjectSetting(key); ... }
```
- **Lösung**: `enum class LayerKeyResult { NotMine, Rejected, Applied }` als Rueckgabe, `err`/`changed` als Out-Parameter. Den gefundenen `ProjSetting*` einmal halten und fuer die Wertausgabe wiederverwenden. Optional: `saveProject` nur bei `changed` (Verhaltensaenderung - nicht ohne Absprache).
- **Erwarteter Effekt**: bessere Lesbarkeit, -1 Lookup.
- **Risiko / Verifikation**: Nur die Rueckgabeform aendern; Settings-MCP-Tests.

### EDT-1319 – Scene: Ordner-Anlage doppelt, `checkPlaying` gibt Zeiger auf Scratch zurueck
- **ID**: EDT-1319
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsScene.cpp:54-63, 171-172, 250-251, 333-334, 187-201, 272-280`
- **Problem**: `ToolResult* checkPlaying(const McpSceneHooks&, ToolResult& scratch)` liefert einen Zeiger auf einen vom Aufrufer bereitgestellten Scratch zurueck; jeder Aufrufer braucht drei Zeilen (`ToolResult scratch; if (ToolResult* r = ...) return *r;`). Die "Elternordner anlegen"-Sequenz (`exists(parent)`, `create_directories`, Fehlertext) steht in `scene_save` und `scene_create` fast wortgleich.
- **Code-Auszug**:
```cpp
ToolResult scratch;
if (ToolResult* r = checkPlaying(*h, scratch)) return *r;
```
- **Lösung**: `std::optional<ToolResult> checkPlaying(const McpSceneHooks&)` (oder `bool playing` plus gemeinsame `failPlayMode()`); `ToolResult ensureParentDir(const PathCheck&)` fuer den Ordner-Block (Rueckgabe "ok" oder "failed").
- **Erwarteter Effekt**: -20 Zeilen, einfacher Aufrufstil.
- **Risiko / Verifikation**: Reine Umformung; Scene-MCP-Tests.

### EDT-1320 – TaskBoard: Status-/Prioritaets-Parsing dreifach, fehlende Includes
- **ID**: EDT-1320
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsTaskBoard.cpp:251-258, 304-309, 1-9, 154-161`
- **Problem**: Das Muster "Name parsen, `Other` ablehnen, Fehlertext" steht fuer `status` in `task_create` und `task_update` und fuer `priority` in `task_create`. `std::function` und `std::int64_t` werden benutzt (`runEdit`, Zeile 266-268), aber `<functional>`/`<cstdint>` nicht eingebunden - es kompiliert nur ueber transitive Includes (F). `taskAnswer` ruft bei jeder Antwort `teamJson`, das `team->members()` kopiert; fuer jede Task-Aenderung ein Vektor-Copy (Schaetzung: unbedeutend).
- **Code-Auszug**:
```cpp
if (!sw.empty() && (!HE::taskStatusFromName(sw, status) || status == HE::TaskStatus::Other))
    return ToolResult::fail("invalid_payload", "'status' must be open, in-progress, blocked or done");
```
- **Lösung**: `bool parseStatusArg(const json&, const char* key, TaskStatus&, ToolResult&)` bzw. fuer Prioritaet; fehlende Includes ergaenzen.
- **Erwarteter Effekt**: -10 Zeilen, robuster Build.
- **Risiko / Verifikation**: Texte unveraendert lassen; Taskboard-MCP-Tests.

### EDT-1321 – Terrain: tote Bedingungen im Heightmap-Tool, Wegwerf-`HorizonWorld` im Lake-Tool
- **ID**: EDT-1321
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/McpToolsTerrain.cpp:394-400, 429-430, 1113-1119`
- **Problem**: `n` wird in Zeile 394 auf 2..128 geklemmt, danach pruefen Zeile 399-400 `(n > 1)` und Zeile 429-430 `heights.empty()` - beide Bedingungen sind immer wahr/falsch (tote Zweige). Im Lake-Tool wird fuer jeden Aufruf ein eigenes `HorizonWorld scratch` angelegt, dort ein `TerrainComponent` kopiert (`emplace<TerrainComponent>(se, work)`) und serialisiert, um das `terrain`-JSON zu erhalten; die anderen drei Tools holen es aus der echten Entity und patchen nur Felder (`componentsOf(*r.world, r.entity)`). Die Kosten von Konstruktor/Destruktor einer `HorizonWorld` sind nicht bekannt (unsicher / messen).
- **Code-Auszug**:
```cpp
const float stepX = (n > 1) ? (maxX - minX) / static_cast<float>(n - 1) : 0.0f;   // n >= 2
...
{ "min", heights.empty() ? 0.0f : mn },                                           // nie leer
...
HorizonWorld scratch;
scratch.registry().emplace<TerrainComponent>(se, work);
```
- **Lösung**: Tote Bedingungen entfernen. Fuer Lake dieselbe Methode wie in sculpt (Patch auf dem echten Entity-JSON mit `sculptHeightsB64`, Water-Felder) oder eine kleine `terrainToJson(const TerrainComponent&)` in `SceneSerializer` anbieten (Cross-Modul).
- **Erwarteter Effekt**: -6 Zeilen; Lake spart eine Welt-Allokation (unsicher).
- **Risiko / Verifikation**: Lake schreibt auch `water` (das nur im Serializer liegt) - der Patch-Weg muss dieselben Felder liefern; Lake-MCP-Tests (Create/Reshape/Dig/Level/Remove) vorher/nachher vergleichen.

### EDT-1322 – `limit`-Behandlung: sechs Varianten
- **ID**: EDT-1322
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `McpToolsInput.cpp:550, 737`; `McpToolsMaterial.cpp:544, 1638-1640`; `McpToolsParticle.cpp:403`; `McpToolsPrefab.cpp:317-318`; `McpToolsSequence.cpp:593`
- **Problem**: Jeder Katalog klemmt `limit` anders: `std::max(1, intArg(...))` (Input, Material), manuelle `args.contains`+`is_number_integer` (Material node_types), `if (limit <= 0) limit = 200` (Prefab), gar nicht (Particle, Sequence). `walkContentAssets` mit `limit <= 0` liefert dann eine leere Liste mit `truncated = true` ohne Fehler.
- **Code-Auszug**:
```cpp
walkContentAssets(*cm, { HE::AssetType::ParticleSystem }, intArg(args, "limit", 200), truncated);
```
- **Lösung**: `int limitArg(const json&, int def)` in `McpToolCommon` (klemmt auf [1, 5000]) und ueberall benutzen.
- **Erwarteter Effekt**: einheitliches Verhalten, -10 Zeilen.
- **Risiko / Verifikation**: Aenderung des Verhaltens fuer `limit <= 0` bei Particle/Sequence (heute: leere Liste); fuer Clients unproblematisch, aber als Aenderung vermerken.

## Gelesen

Alle zehn Dateien des Slice vollstaendig gelesen: ja (McpToolsInput, Material, Particle, Prefab, Scene, Screenshot, Sequence, Settings, TaskBoard, Terrain); zur Verifikation nur gezielt gelesen: `McpToolCommon.cpp:18-45, 197-237`, `ContentManager.h:60-95`, `ContentManager.cpp:770-818`, `EditorCommands.cpp:400-420`, `EditorApplication.cpp:10775-10790`.

## Editor Slice 14 – McpToolsType.cpp … ProjectPreflight.h

## Zusammenfassung

Der Slice (34 Dateien, ~10,8k Zeilen) ist insgesamt sauber und gut kommentiert; der Outliner hat bereits Row-Clipping, die Notification-Schicht ist logisch sauber von ImGui getrennt. Die Hauptprobleme sind Arbeit, die pro Frame im UI-Thread läuft, obwohl sie nur bei Änderungen nötig wäre: ein JSON-Serialisieren des ganzen Partikel-Graphen pro Frame, eine tiefe Kopie des Notification-Stores pro Frame (auch bei geschlossenem Flyout), O(Spans)-Schleifen pro Frame im Profiler-Timeline und O(N)-Restarbeit im Outliner trotz Clipping. Dazu kommen ungeclippte Listen (Profiler-Scopes, Play-Report), ein `exists()`-Syscall pro bekanntem Projekt und Frame im Project Hub und einige Duplikate (Property-Validierung in den MCP-Widget-Tools, Preset-Listen, Farben). Funde: 4 Hoch, 9 Mittel, 10 Niedrig. Alle Zeilenangaben gegen den Ist-Stand geprüft; Laufzeitannahmen sind als Schätzung markiert, Messungen wurden nicht durchgeführt.

---

### EDT-1401 – Partikel-Graph wird jeden Frame nach JSON serialisiert, um „Änderung“ zu erkennen
- **ID**: EDT-1401
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/ParticleGraphEditorPanel.cpp:295-304 (Änderungs-Quellen: 557-566, 137-152, 180-191, 105-113)
- **Problem**: Der Kommentar sagt „Re-evaluate only on an actual graph change“ und im State „not every frame“, der Code serialisiert aber den ganzen Graphen in jedem gezeichneten Frame (nlohmann-Dump, viele Heap-Allokationen) und vergleicht den String mit dem letzten. Bei jeder Änderung wird zusätzlich der komplette String nach `lastEvaluatedJson` kopiert (auch pro Frame während eines Node-Drags, weil `setPos` das JSON ändert). Läuft für jeden offenen Particle-Tab.
- **Code-Auszug**:
```cpp
const std::string curJson = HE::particleGraphToJson(st.graph);
if (curJson != st.lastEvaluatedJson)
{
    st.previewConfig     = HE::evaluateParticleGraph(st.graph, st.previewRng);
    st.lastEvaluatedJson = curJson;
```
- **Lösung**: Eine Revisionszahl statt Text-Vergleich. (1) `uint64_t graphRevision` in `State`, `lastEvaluatedRevision` daneben. (2) Erhöhen an den Stellen, die den Graphen ändern: `structuralEdit` (Z.557), `st.geState.liveEdit` (Z.566), Laden in `stateFor`, `reloadFromDisk`/`reloadByContentPath` (setzen `loaded=false`, also Neuladen -> Revision hoch), Undo-Restore und Collab-Apply (`CollabDocSync::forParticleGraph` schreibt in `st->graph`). (3) Nur bei `revision != lastEvaluatedRevision` evaluieren; die Evaluation hängt nur an Node-Parametern/Links, nicht an Node-Positionen, daher kann Positions-Drag (liveEdit) die Revision ausnehmen. Alternative ohne Eingriff in alle Schreibpfade: ein Revisionszähler direkt in `HE::ParticleGraph` (HE_Core), der in `connect/addNode/removeNode/...` hochzählt; Parameteränderungen über `n->p` im Panel zählen dann explizit hoch.
- **Erwarteter Effekt**: Entfällt ein JSON-Dump pro Frame und offenem Tab (Schätzung: je nach Graphgröße einige 10 bis 100 µs plus ~100+ Allokationen pro Frame); Preview-Neuberechnung bleibt gleich.
- **Risiko / Verifikation**: Verhalten muss exakt gleich bleiben: jede Änderungsquelle (Undo, Collab-Remote-Edit, Reload) muss die Revision bewegen, sonst veraltet die Preview. Test: Preview-Config nach Undo, nach `reloadFromDisk` und nach Collab-Apply gleich wie bei der JSON-Variante (siehe `tests/` zu ParticleGraph/Collab; ggf. neuer Test, der `evaluateParticleGraph`-Aufrufzähler prüft).

### EDT-1402 – NotificationBar kopiert den gesamten Store (bis 200 Einträge mit je 6 Strings + `std::function`) in jedem Frame
- **ID**: EDT-1402
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/NotificationBar.cpp:56-81 (Kommentar 40-42), NotificationStore.cpp:164-168, NotificationStore.h:222
- **Problem**: `refresh()` ruft pro Frame `ctx.notifications->snapshot()` auf, das `std::vector<Notification>` per Wert kopiert (`return m_entries;`). Jede `Notification` trägt `title,text,detail,assetPath,key,handledBy` plus `vector<NoteAction>` (Labels und `std::function` mit Captures). Der Kommentar rechtfertigt das mit „couple of dozen rows“, aber `k_maxEntries = 200`; und die Kopie wird auch gebraucht, wenn der Flyout zu ist: Die Fußzeile braucht nur `unseen`, `worstUnseen`, `newestProblemMs`. Zusätzlich hält die Kopie den Store-Mutex, den Worker-Threads (Log-Brücke, Scan, Sync) beim Posten brauchen.
- **Code-Auszug**:
```cpp
s_cache.entries = ctx.notifications->snapshot();
for (const Notification& n : s_cache.entries) { ... }
```
```cpp
std::vector<Notification> NotificationStore::snapshot() const
{ std::lock_guard<std::mutex> lock(m_mutex); return m_entries; }
```
- **Lösung**: (1) Im Store eine `std::atomic<uint64_t> m_version` führen, die bei jeder Mutation von Zeilen (`post`, `resolve`, `markAllSeen`, `clear`, `settle`, `runAction`, Eviction) hochgezählt wird (nicht in `holdToast`/`dismissToast`, die die Liste der Bell nicht ändern). (2) `struct Summary { size_t unseen; NoteLevel worstUnseen; uint64_t newestProblemMs; size_t count; }` und `Summary summary() const` (eine Schleife unter Lock, keine Kopie) für `FooterWidth`/`DrawFooter`. (3) `snapshot()` nur aufrufen, wenn `s_open || s_pinned` (Flyout offen) und `version != cache.version`. Alternative ohne API: Summary-Felder im Store beim Mutieren mitpflegen.
- **Erwarteter Effekt**: Im Normalzustand (Flyout zu) null Allokationen und eine kurze Lock-Zeit pro Frame statt bis zu ~1400 Heap-Allokationen bei vollem Store (Schätzung: 200 Einträge x ~7 Strings/Vektoren); weniger Lock-Konkurrenz mit Worker-Threads.
- **Risiko / Verifikation**: Gleiches sichtbares Verhalten (Zähler, Farbe, Klingeln). Bestehende Tests `tests/test_notification_store.cpp`, `test_notification_popups_ui.cpp` abdecken; ergänzen: Summary == Ableitung aus Snapshot nach beliebiger Aktionsfolge (post/merge/evict/markAllSeen/resolve).

### EDT-1403 – Profiler-Timeline iteriert pro Frame alle Spans mehrfach und hasht Namen pro gezeichnetem Span
- **ID**: EDT-1403
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/ProfilerWidgets.cpp:331-335, 386-388, 405-416, 83-92; ProfilerPanel.cpp:81, 157-177
- **Problem**: `TimelineView` läuft pro Frame (a) über alle Spans aller Lanes für `lo/hi` (Capture-Ausdehnung), (b) pro Lane nochmals über alle Spans für `maxDepth`, (c) ein drittes Mal pro Lane über alle Spans mit `spanRect` (kein Culling per Sortierung/Binärsuche), und ruft für jeden sichtbaren Span `ScopeColor` (FNV-Hash über den Namen, `fmod`, HSV-Konversion) plus `drawTextClipped` (PushClipRect/PopClipRect pro Span) auf. Pro Lane sind laut ProfilerPanel.cpp:81 bis zu ~250 000 Spans möglich. `lo/hi` und `maxDepth` ändern sich nur, wenn `refreshLanes`/Load neue Daten liefert (höchstens alle 0,25 s bzw. gar nicht). Ein Profiler, der selbst Millisekunden pro Frame verbraucht, verfälscht das, was er misst.
- **Code-Auszug**:
```cpp
for (const ProfThreadTimeline& lane : lanes)
    for (const ProfThreadSpan& s : lane.spans) { lo = std::min(lo, s.startNs); hi = std::max(hi, s.endNs); }
...
uint32_t maxDepth = 0;
for (const ProfThreadSpan& s : lane.spans) maxDepth = std::max(maxDepth, s.depth);
...
const ImU32 col = ScopeColor(s.name);
```
- **Lösung**: (1) `lo/hi` und `maxDepth` je Lane einmal beim Befüllen berechnen (in `refreshLanes` bzw. nach `LoadedCapture::load`) und in einer kleinen Struktur neben `g_cache.lanes` ablegen (`struct LaneStats { uint64_t lo, hi; uint32_t maxDepth; }`); `TimelineView` bekommt sie als Parameter. (2) Spans je Lane beim Cachen nach `startNs` sortieren (stabil, pro Tiefe) und im Draw per `std::lower_bound` auf `view.startNs` einsteigen und bei `startNs > view.endNs` abbrechen; Sub-Pixel-Spans (Breite < 1 px) zusammenfassen oder überspringen. (3) `ScopeColor` in eine kleine `unordered_map<const char*, ImU32>` (Zeigeridentität reicht, die Namen sind interniert) oder direkt als Feld im gecachten Span ablegen.
- **Erwarteter Effekt**: Von O(Spans) auf O(sichtbare Spans) pro Frame (Schätzung: bei 4 Lanes x 100k Spans einige ms pro Frame gespart; messen mit dem Profiler selbst bzw. `HE_PROFILE_SCOPE`).
- **Risiko / Verifikation**: Reihenfolge der Zeichnung (Überdeckung) ändert sich, wenn nach Start sortiert wird; Hover-Treffer („letzter gewinnt“) bleibt gleich, solange die Zeichenreihenfolge pro Tiefe erhalten bleibt. `tests/test_profiler_layout.cpp` deckt `spanRect/zoom` ab; Culling-Test ergänzen (gleiche gezeichnete Menge wie ungefilterte Schleife).

### EDT-1404 – Outliner: Restarbeit O(N) pro Frame trotz Row-Clipping (u. a. `visibleRows` mit N Einträgen und Allokation)
- **ID**: EDT-1404
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/OutlinerPanel.cpp:1100-1102, 1133-1183, 1154, 1819-1837, 1114-1130
- **Problem**: Die Zeilenschleife läuft für jeden Cache-Knoten (50k Entities = 50k Iterationen/Frame). Auch für geclippte Zeilen: `visibleRows.push_back` (Z.1154), `rowId()` (ImHashData), `GetStateStorage()->GetInt` (binäre Suche in der Storage), `enterPath` (zwei Vektor-Zugriffe). Dazu reserviert `visibleRows.reserve(s_outlinerCache.size())` pro Frame einen Block von N*4 Byte (200 KB bei 50k; über der mmap-Schwelle vieler Allocatoren, also u. U. ein mmap/munmap pro Frame), obwohl `visibleRows` nur im Shift-Klick-Zweig (Z.1819-1837) gebraucht wird. Das Doc `world-streaming-baseline-2026-10-06.md` §3.3 nennt den Outliner bei 50k als ~1/4 der Frame-Zeit, das Clipping hat nur das TreeNodeEx/Text-Messen entfernt.
- **Code-Auszug**:
```cpp
std::vector<Entity> visibleRows;
visibleRows.reserve(s_outlinerCache.size());
...
visibleRows.push_back(node.entity);
```
- **Lösung**: Stufe 1 (trivial, verhaltensgleich): `visibleRows` nur füllen, wenn ein Shift-Klick möglich ist (`ImGui::GetIO().KeyShift && ImGui::IsMouseClicked(0)`; der Klick wird erst innerhalb der Schleife erkannt, daher alternativ: beim Shift-Klick in der Schleife nur `shiftRangeTarget` setzen und die Menge der sichtbaren Zeilen im Folge-Pass (oder im selben Frame durch einen zweiten, schlanken Durchlauf über Cache + Fold-Zustand) berechnen), oder als `static` Member mit `clear()` (Kapazität bleibt, keine Allokation). Stufe 2 (größer, nur nach Messung): Sichtbare Zeilen als flache Liste (Index, Tiefe) nur bei `cacheRebuilt`/Fold-Änderung/Filter-Änderung neu aufbauen und pro Frame nur den Bereich `[firstVisible, lastVisible]` per Prefix-Summe der konstanten Zeilenhöhe iterieren; Fold-Zustand liegt dann im eigenen Set statt in ImGui-Storage.
- **Erwarteter Effekt**: Stufe 1: keine 200-KB-Allokation und 50k push_backs pro Frame (Schätzung: 0,1-0,5 ms bei 50k). Stufe 2: Outliner-Kosten pro Frame proportional zur sichtbaren Zeilenzahl statt zur Szenengröße (Schätzung: mehrere ms bei 50k; unsicher, messen).
- **Risiko / Verifikation**: Stufe 2 greift tief in ID-Stack/Fold-Verhalten ein (Kommentar 1034-1049 beschreibt die Abhängigkeit von TreeNodeEx-IDs). `tests/test_outliner_ui.cpp` mit `setRowClipping(false)` als Referenz (Frame-Vergleich) beibehalten; zusätzlich Shift-Range-Test über geclippte und gefaltete Bereiche.

### EDT-1405 – Outliner-Filter läuft pro Frame über alle Knoten, mit zwei neuen Vektoren
- **ID**: EDT-1405
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/OutlinerPanel.cpp:936-960; OutlinerFilter.cpp:170-179, 192-222
- **Problem**: Solange ein Suchtext oder Typfilter aktiv ist, werden pro Frame `rows` (N Einträge) und `shown` (N Einträge, in `apply`) allokiert und für jeden Knoten `reg.valid`, `nameMatches` (case-insensitive `std::search` mit `tolower` pro Zeichenvergleich) und `kind.test` (`any_of`) ausgewertet. Der Kommentar begründet das mit „a component added in Details does not dirty the hierarchy“, aber der Name ändert sich nur mit dem Hierarchie-Cache, der Suchtext nur bei Tastendruck; nur der Typtest hängt am Registry-Zustand.
- **Code-Auszug**:
```cpp
std::vector<OutlinerFilter::Row> rows;
rows.reserve(s_outlinerCache.size());
...
rows.push_back({ node.depth, match });
...
shown = OutlinerFilter::apply(rows);
```
- **Lösung**: (1) Namens-Match je Knoten nur neu bestimmen, wenn sich `s_searchText` oder der Cache ändert (Bitset/`vector<uint8_t>` `nameHit`). (2) Typtest im gleichen Scheibenverfahren wie `tally` (Z.735-754) über mehrere Frames pro 2048 Knoten aktualisieren und das Ergebnis in `vector<uint8_t> typeHit` halten (zeitlich versetzte Anzeige um wenige Frames; Kommentar dokumentiert, dass dies für die Chip-Zähler schon so läuft). (3) `rows/shown` als `static` Puffer mit `clear()` wiederverwenden. (4) Das Nadel-`tolower` einmal pro Frame statt pro Zeichenvergleich (Nadel vorab kleinschreiben, Name nur einseitig).
- **Erwarteter Effekt**: Bei 50k Entities mit aktivem Filter spart es pro Frame ~2 Allokationen von 50k Elementen und 50k Such-/Typtests (Schätzung 1-4 ms; messen).
- **Risiko / Verifikation**: Sichtbare Änderung: Typfilter reagiert auf hinzugefügte Komponenten ggf. 1-2 Frames später (Details-Panel-Edits). Wenn das nicht akzeptabel ist, nur (1), (3), (4) umsetzen (verhaltensgleich). Test in `tests/test_outliner_ui.cpp` (Filter + Komponente nachträglich hinzufügen) und `test_outliner_filter*.cpp` falls vorhanden.

### EDT-1406 – Outliner-Cache: Namenskopie je Knoten, `std::function`-Rekursion, Rebuild bei jeder Strukturänderung
- **ID**: EDT-1406
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/OutlinerPanel.cpp:614-690 (Rekursion 648-685), 1311-1320, 462-469, 553-578
- **Problem**: Jeder Rebuild (jede `isHierarchyDirty()`, also jedes Erzeugen/Löschen/Umhängen, auch Spawns während Play) kopiert für jeden der N Knoten den Namen in `OutlinerNode::name` (Heap-Allokation ab >15 Zeichen), ruft pro Knoten bis zu vier Registry-Lookups plus für Knoten unter Prefab-Instanzen `prefabInstancesBinding(...)` auf (Kosten dort unklar) und rekursiert über ein `std::function` (Indirektion pro Knoten, Heap für die große Capture). Zusätzlich zwei weitere `std::function`-Rekursionen (`setBranch`, `note`) und ein per Lambda rekursiver Widget-Baum, bei dem für jeden Knoten wieder alle Elemente gescannt werden (Z.556-562, O(n²) für `hasChildren`).
- **Code-Auszug**:
```cpp
std::function<bool(Entity, int, bool, bool)> collect = [&](...) -> bool { ... };
...
OutlinerNode node{ entity, name ? name->name : "(unnamed)", depth, hier && !hier->children.empty() };
```
- **Lösung**: (1) Statt `std::string name` einen `const std::string*`/Index nicht möglich (Zeiger nicht stabil bei Storage-Wachstum); stattdessen Namen erst bei Bedarf (nur sichtbare Zeilen und Filter) aus dem Registry holen: `NameComponent` pro sichtbarer Zeile ist O(1) und braucht keine Kopie im Cache; Filter-Matches können über den Registry-Zugriff erfolgen. (2) `collect` als private statische Funktion/Lambda mit explizitem Stack (`std::vector<Frame>`) oder `auto self`-Technik ohne `std::function`. (3) `drawElem` des Widget-Panels: Kinder je Element einmal in `unordered_map<int, vector<const UIElement*>>` bündeln (`UIWidgetTree::childrenOf` ist selbst O(n)).
- **Erwarteter Effekt**: Rebuild ohne N Namens-Allokationen und ohne Funktionsaufruf-Overhead pro Knoten (Schätzung: Rebuild bei 50k um 30-50 % schneller; unsicher/messen). Relevant vor allem, wenn Gameplay im Play-Modus pro Frame spawnt.
- **Risiko / Verifikation**: Verhalten identisch; Namensänderung muss weiterhin angezeigt werden (heute über Dirty-Flag beim `renameEntity`; ohne Namen im Cache sogar robuster). `tests/test_outliner_ui.cpp`; Messung mit `HE_PROFILE_SCOPE_N("OutlinerPanel::render")` vor/nach.

### EDT-1407 – `OutlinerPanel::render` ist eine ~1370-Zeilen-Funktion mit statischem Zustand und eingebetteten Lambdas
- **ID**: EDT-1407
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/OutlinerPanel.cpp:512-1886 (statische Variablen 631-632, 698-701, 732-735, 764-765, 1055-1056)
- **Problem**: Eine Funktion enthält Cache-Aufbau, Kind-Sweep, Suchleiste + Chip-Layout, Filter, Statuszeile, Row-Clipping, Zeilen-Zeichnung, Drag&Drop, ein 140 Zeilen langes Kontextmenü, Rename-Eingabe, Badges und Shift-Range. Zustand lebt in ~12 function-local `static`s, was Mehrfach-Instanzen (zwei Outliner) und Tests erschwert und Datenflüsse (z. B. `cacheRebuilt`, `skipBelowDepth`, `pathDepth/pushedDepth`) schwer nachvollziehbar macht.
- **Code-Auszug**:
```cpp
static std::vector<OutlinerNode> s_outlinerCache;
static HorizonWorld*             s_lastWorld = nullptr;
...
static std::string s_searchText;
static int         s_typeFilter = OutlinerFilter::kAllKinds;
```
- **Lösung**: `struct OutlinerState` (Cache, Rename, Suche, Kind-Zähler, Row-Steps) in einen anonymen Namespace heben und in kleine Funktionen zerlegen: `rebuildCache`, `sweepKindCounts`, `drawHeader` (Suche + Chips), `drawStatusLine`, `drawRows` (Clipping), `drawRowContextMenu`, `drawRowBody` (Icon/Name/Badges), `resolveShiftRange`. Die Funktionen sind bereits durch Kommentarblöcke abgegrenzt; reine Verschiebung, keine Logikänderung.
- **Erwarteter Effekt**: Lesbarkeit/Testbarkeit; keine Laufzeitänderung (Inlining ändert nichts). Erleichtert EDT-1404/1405/1406.
- **Risiko / Verifikation**: Gering, wenn die ImGui-ID-Struktur unverändert bleibt (keine zusätzlichen `PushID`s). `tests/test_outliner_ui.cpp` Frame-Vergleich (BMP-Dump) als Absicherung; `editor_help_audit.py --check` muss weiter grün sein.

### EDT-1408 – Outliner-Kopfbereich: Chip-Layout, Statuszeile und `selectedPath` bauen pro Frame Vektoren/Strings neu
- **ID**: EDT-1408
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/OutlinerPanel.cpp:814-931 (Chips), 975-1003 (Statuszeile), 1114-1130 (`selectedPath`), 865-872 (`place`)
- **Problem**: (a) Chips: pro Frame `std::vector<Chip>`, `std::vector<int> lineOf`, bis zu 22 mal `CalcTextSize` doppelt (einmal in `chipLabelW`, einmal beim Zeichnen) plus `snprintf` pro Chip, obwohl sich Zähler/Breite nur bei neuem Sweep-Ergebnis oder Breitenänderung ändern. (b) Statuszeile: bei genau einem selektierten Entity wird pro Frame die Ahnenkette in einen `vector<string>` kopiert (bis 64 Strings) und danach in einer Doppelschleife (`from`/`i`) der Pfad-String neu gebaut und mit `CalcTextSize` gemessen, bis er in die Breite passt (O(Tiefe²) Messungen im schmalen Panel). (c) `selectedPath`: `std::unordered_set<Entity>` mit Knotenallokation je Vorfahre, jedes Frame, auch ohne Selektion und ohne offene Guide-Linien; nach 256 selektierten Entities wird abgebrochen (Guides dann unvollständig). (d) Lambda `place` ignoriert den Parameter `w` (`(void)w;`).
- **Code-Auszug**:
```cpp
std::vector<std::string> names;
for (Entity e = ctx.selection.primary(); ... names.size() < 64;) { ... names.push_back(nc ? nc->name : std::string("(unnamed)")); ... }
for (size_t from = 0; from < names.size(); ++from) { std::string t = ...; ... CalcTextSize(t.c_str()).x <= room ...}
```
- **Lösung**: Statuszeile und Chips in kleinen Structs cachen, Schlüssel: (primary-Entity, Hierarchie-Revision, `room`/`avail` gerundet, Anzahl-Version `s_kindCounts`). `selectedPath` als `static std::vector<Entity>` (sortiert, `binary_search`) oder wiederverwendetes `unordered_set` mit `clear()` (Buckets bleiben), nur befüllen, wenn `!ctx.selection.entities().empty()`. Pfadkürzung per Binärsuche über `from` (wie `elideFront` in NotificationBar) statt linear mit Neuaufbau. `place` auf zwei Parameter reduzieren.
- **Erwarteter Effekt**: Entfernt einige Dutzend Allokationen und Textmessungen pro Frame (Schätzung < 0,1 ms; geringer Gewinn, aber hohe Frequenz).
- **Risiko / Verifikation**: Cache-Invalidierung bei Umbenennung des Vorfahren (Name im Pfad) beachten; Hierarchie-Cache wird dabei neu gebaut, daher Schlüssel `cacheRebuilt`-Zähler. `test_outliner_ui.cpp` (Statuszeile/Chip-Screenshots).

### EDT-1409 – Profiler-Tabellen „Scopes“ und „Frame Detail/CPU scopes“ zeichnen alle Zeilen ohne Clipper
- **ID**: EDT-1409
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/ProfilerPanel.cpp:520-548, 658-667, 697-716
- **Problem**: Die Scopes-Tabelle erzeugt je Aggregat ~9 Widgets (Dummy, TextColored mit `ScopeColor`-Hash, Text, `ProgressBar`, sechs formatierte Texte) für alle Zeilen jeden Frame; die Frame-Detail-Liste baut je Scope einen `std::string label(depth*2, ' ')` (Heap) und zeichnet Text + `ProgressBar`; ein einzelner Frame kann tausende Scopes haben. Die Hitch-Liste ebenso. Das Panel läuft mit `ImGuiTableFlags_ScrollY`, `ImGuiListClipper` wird nirgends verwendet.
- **Code-Auszug**:
```cpp
for (size_t i = f.scopes.size(); i-- > 0;)
{
    const ProfScopeSample& sc = f.scopes[i];
    std::string label(static_cast<size_t>(sc.depth) * 2, ' ');
    label += sc.name ? sc.name : "?";
    ImGui::Text("%s", label.c_str());
```
- **Lösung**: `ImGuiListClipper` (Zeilenhöhe konstant) um die Schleifen von `DrawScopes` (Tabelle: `clipper.Begin(agg.size())`), `DrawFrameDetail` (CPU-Scopes, mit Rückwärts-Index) und `DrawHitches`; Label ohne Allokation: `ImGui::Text("%*s%s", depth*2, "", name)`. `maxSelf` einmal beim Cachen berechnen (Z.503-504 läuft pro Frame über alle Aggregate).
- **Erwarteter Effekt**: Kosten proportional zu ~30 sichtbaren Zeilen statt Anzahl der Scopes (Schätzung: bei 500 Scopes von ~4500 auf ~300 Widgets pro Frame).
- **Risiko / Verifikation**: Der Clipper verlangt gleiche Zeilenhöhen: ProgressBar-Höhe ist in beiden Listen konstant (Standardhöhe); mit dem Screenshot-Test des Panels prüfen (`test_profiler*_ui` falls vorhanden, sonst `HE_UI_DUMP_DIR`).

### EDT-1410 – Project Hub: `std::filesystem::exists()` pro bekanntem Projekt und Frame, plus Pfad-/String-Bau pro Zeile
- **ID**: EDT-1410
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/ProjectHubPanel.cpp:459-466
- **Problem**: Jeden Frame wird für jeden Eintrag der Recent-Liste ein Dateisystem-Zugriff gemacht (`stat`), dazu `path`-Konstruktion, `stem().string()` und der Label-String `name + "\n" + known[i]`. Liegt ein Projekt auf einer getrennten Netzfreigabe oder externer Platte, blockiert ein einzelnes `stat` den UI-Thread (Hub zeichnet ohne Idle-Throttle, 60 Hz). Die Liste ist ungeclippt.
- **Code-Auszug**:
```cpp
std::filesystem::path p(known[i]);
std::string name = p.stem().string();
...
bool exists = std::filesystem::exists(p);
```
- **Lösung**: Cache `struct KnownRow { std::string label; bool exists; }` in einem file-static Vektor; Neuaufbau, wenn sich `known` (Größe/Inhalt, z. B. per Hash oder Version in `GlobalState`) ändert oder alle ~2 s (`ImGui::GetTime()`) für die `exists`-Aktualisierung. Optional `ImGuiListClipper` (feste Zeilenhöhe 40 + Separator).
- **Erwarteter Effekt**: Pro Frame null Syscalls und null Allokationen in der Liste; kein UI-Hänger durch langsame Datenträger (Schätzung: bei 10-20 Projekten 10-20 `stat` pro Frame entfallen).
- **Risiko / Verifikation**: Anzeige eines neu gelöschten/neu erscheinenden Projekts bis 2 s verzögert. Nach „Remove from list“ und nach Create/Open Cache invalidieren. Manueller Test im Hub; `ProjectLaunchOpen`-Tests unberührt.

### EDT-1411 – MCP-Widget-Tools: Property-Validierung doppelt, Kinderlisten O(n²)
- **ID**: EDT-1411
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/McpToolsWidget.cpp:653-681 (widget_add) und 855-875 (widget_set_properties); 337-356, 473, 729-731
- **Problem**: Der Block „Property-Tabelle holen, je Eintrag Zeile suchen, bei Fehlschlag Namensliste bauen, `propValueFromJson`, `pending` füllen“ steht zweimal fast wörtlich im Code (je ~25 Zeilen). Außerdem: `elementSummary` ruft pro Element `tree.childrenOf(e.id)` auf, das alle Elemente scannt (`UIWidgetTree.cpp:21-26`), sodass `widget_tree` O(n²) ist; `widget_remove` sammelt Nachfahren ebenfalls per `childrenOf`-Scan je Knoten; `elementArg` baut bei jedem Fehler eine ID-Liste (nur im Fehlerfall, ok).
- **Code-Auszug**:
```cpp
const std::vector<UIPropDesc> table = e->allProperties();
...
const auto row = std::find_if(table.begin(), table.end(),
    [&](const UIPropDesc& pd) { return pd.name == it.key(); });
```
- **Lösung**: Gemeinsame Hilfsfunktion `bool readProps(const UIElement&, const json& props, std::vector<std::pair<std::string,UIPropValue>>& out, ToolResult& fail)` im anonymen Namespace; beide Handler rufen sie auf. Für die Baumausgabe einmal eine Kind-Tabelle (`unordered_map<int, vector<int>>`) in `widget_tree` bauen und an `elementSummary` reichen (`UIWidgetTree` bleibt unverändert; optional `childrenOf` dort mit Cache).
- **Erwarteter Effekt**: ~25 Zeilen weniger Code, ein Ort für Fehlermeldungen; `widget_tree` linear statt quadratisch (nur bei großen Widgets spürbar; Tool-Aufruf, nicht Frame-Hot-Path).
- **Risiko / Verifikation**: Fehlertexte müssen byte-gleich bleiben (Tests der MCP-Tools, `tests/test_mcp_*widget*`); vor dem Refactor Snapshot der Fehlermeldungen ziehen.

### EDT-1412 – Partikel-Graph-Tab: GraphEditor-Callbacks und Preview allokieren pro Frame Vektoren/Strings
- **ID**: EDT-1412
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/ParticleGraphEditorPanel.cpp:333-353, 409-445, 490-497
- **Problem**: Das `GraphEditor::Model` wird jeden Frame aus sieben `std::function`s gebaut, die selbst allokieren: `nodeIds` gibt einen neuen `vector<int>` zurück, `links` einen neuen `vector<array<int,4>>`, `pins(id)` pro Node und Aufruf einen `vector<Pin>`, `title(id)` einen `std::string`. In der Preview wird `std::vector<ParticlePreviewInstance> instances` pro Frame neu allokiert (`reserve`) und `slot`-Lambda baut je Frame zwei `std::string`s („(default)“, „set“, „(missing)“) für eine reine Anzeige.
- **Code-Auszug**:
```cpp
m.pins = [&st](int id) -> std::vector<GraphEditor::Pin> { ... std::vector<GraphEditor::Pin> pins; ... return pins; };
...
const std::string state = (target == HE::UUID{}) ? std::string("(default)") : ...
```
- **Lösung**: (1) `instances` und eventuelle Pins als `static`/State-Member mit `clear()`. (2) `state` als `const char*`. (3) `GraphEditor::Model` auf Ausgabe in vorhandenen Puffer umstellen (`void nodeIds(std::vector<int>&)`, `const std::vector<Pin>& pins(int)` mit Cache je Node-Typ, da `particleNodeDesc(type)` statisch ist) – das ist ein Cross-Modul-Eingriff in `GraphEditor.h` (Material/HorizonCode-Editor nutzen dasselbe).
- **Erwarteter Effekt**: Kleine Zahl Allokationen pro Frame und Node (Schätzung: bei 20 Nodes ~60-100 Allokationen/Frame weniger); Gewinn klein, aber es betrifft jeden Graph-Editor.
- **Risiko / Verifikation**: Änderung am Interface von `GraphEditor::Model` berührt Material- und HorizonCode-Editor; zuerst (1)/(2) lokal umsetzen. Test: vorhandene GraphEditor-/UI-Tests.

### EDT-1413 – PlayReportPanel: Log-Mutex über die ganze Zeichnung gehalten, 2000 `TextWrapped` ungeclippt
- **ID**: EDT-1413
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/PlayReportPanel.cpp:42-93
- **Problem**: `std::lock_guard<std::mutex> lk(*ctx.playLogMutex)` wird direkt nach `Begin` genommen und bis zum Funktionsende (inklusive aller ImGui-Aufrufe, Clipboard und `EndChild`) gehalten; Logger-Sink-Threads (Zeile `appendPlayLog`) blockieren in der Zeit. Darunter laufen bis zu 2000 `ImGui::TextWrapped` pro Frame (Umbruchberechnung je Eintrag), zweimal über das Log (Zähler, Liste), ohne Clipper.
- **Code-Auszug**:
```cpp
std::lock_guard<std::mutex> lk(*ctx.playLogMutex);
int errors = 0, warnings = 0;
for (const auto& e : *ctx.playLog) (e.level == HE::LogLevel::Warning ? warnings : errors)++;
```
- **Lösung**: Beim Öffnen des Panels (oder bei Änderung von `playLog->size()`) eine Kopie/Gliederung unter kurzem Lock ziehen (gefilterte Index-Liste, Zähler) und das Lock vor dem Zeichnen freigeben. Für die Liste `ImGuiListClipper` nutzen; wegen variabler Zeilenhöhe (Wrap) entweder `TextUnformatted` mit festem Umbruchverzicht (Panel läuft ohnehin im Scroll-Child) oder Höhen cachen.
- **Erwarteter Effekt**: Keine Blockade von Worker-Threads mehr; Panel-Kosten unabhängig von Log-Länge (nur bei geöffnetem Report, nach Play; daher Mittel statt Hoch).
- **Risiko / Verifikation**: Verhalten gleich, solange die Kopie bei Änderung erneuert wird (Log wächst nur nach Play-Ende nicht mehr; praktisch statisch). Manueller Test: Play mit >2000 Warnungen.

### EDT-1414 – Profiler-Overview baut pro Frame 4 Vektoren und sortiert 240 Frames
- **ID**: EDT-1414
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/ProfilerPanel.cpp:266-309, 408; EngineProfiler.h:210 und :301-302
- **Problem**: `liveSnapshot()` kopiert den Ring (240 Frames, `m_live` per Wert) und baut `deltas` (double), `bars` (float, identisch zu `deltas`), `cpuSpark`, `gpuSpark`, danach `computePercentiles` (Sortierung), `computeFpsLows` und `computeHistogram` – alles pro Frame. Bei 240 Elementen gering, aber redundant (`bars` ist nur `deltas` in float).
- **Code-Auszug**:
```cpp
const std::vector<ProfLiveFrame> live = prof.liveSnapshot();
deltas.reserve(live.size()); bars.reserve(live.size());
```
- **Lösung**: Statistik nur bei neuem Frame (Frame-Index vergleichen) neu berechnen, Vektoren als `static` wiederverwenden; `bars` weglassen und `FrameGraph` einen Funktor/`span<const double>` geben.
- **Erwarteter Effekt**: Einige Allokationen pro Frame weniger (Schätzung < 50 µs); bewusst niedrige Priorität.
- **Risiko / Verifikation**: Gering; visuell identisch. Messen, bevor Aufwand investiert wird.

### EDT-1415 – Notification-Flyout zeichnet alle Zeilen jedes Frames und eliden Pfade neu
- **ID**: EDT-1415
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/NotificationBar.cpp:379-501, 263-285
- **Problem**: Solange der Flyout offen ist (Maus über der Glocke), werden bis zu 200 Zeilen mit `CalcTextSize`, `ageText`-`snprintf`, `elideFront` (Binärsuche mit je einem `CalcTextSize`, dazu `std::string` pro Pfad) und eigener Wrap-Logik gebaut; das Child scrollt auf 320 px, aber nichts wird geclippt. Zeilen haben variable Höhe (Wrap), daher ist ein einfacher `ImGuiListClipper` nicht direkt möglich.
- **Code-Auszug**:
```cpp
const std::string shown = elideFront(n.assetPath, std::max(40.0f, wrapPos - ImGui::GetCursorPosX() - copiedW));
```
- **Lösung**: Gekürzte Pfade je Eintrag-ID in einem kleinen Cache (Schlüssel: id + gerundete Breite) halten; optional Zeilen außerhalb des Child-Clip-Rect (`ImGui::IsRectVisible` nach Höhenschätzung) überspringen und eine gemessene Höhe cachen.
- **Erwarteter Effekt**: Nur während der Flyout offen ist; Gewinn ist gering, wenn der Store typischerweise < 30 Einträge hat (Schätzung).
- **Risiko / Verifikation**: Niedrig; nur umsetzen, falls Messungen mit vollem Store einen Hänger zeigen.

### EDT-1416 – `OutlinerFilter`: `kPriority` hängt an handgezählten Indizes in `kKinds`
- **ID**: EDT-1416
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/OutlinerFilter.cpp:44-71, 77-100, 104-130, 138-143, 170-190
- **Problem**: Drei parallele Tabellen (`kKinds`, `kLooks`, `kPriority`) sind nur durch Reihenfolge und Zahlen verbunden; der Kommentar warnt selbst „a kind added in the middle of that list moves every one after it“. `static_assert` prüft nur die Größe von `kLooks`, nicht die Gültigkeit von `kPriority`. Außerdem duplizieren `nameMatches` und `matchSpan` die `std::search`-Logik inklusive Lambda.
- **Code-Auszug**:
```cpp
const int kPriority[] = { 3, // Camera
    4, // Light  ...
```
```cpp
const auto it = std::search(name.begin(), name.end(), needle.begin(), needle.end(), [&](char a, char b) { return lower(a) == lower(b); });
```
- **Lösung**: `enum KindId : int { All, Mesh, SkeletalMesh, Camera, ... , Count }` und `kPriority` als `KindId`-Array; `static_assert(Count == sizeof(kKinds)/sizeof(kKinds[0]))`. `nameMatches(name, needle)` als `needle.empty() || matchSpan(name, needle).pos != npos` (die zusätzliche Frühprüfung `needle.size() > name.size()` steckt schon in `matchSpan`).
- **Erwarteter Effekt**: Weniger Wartungsrisiko, ~10 Zeilen weniger; keine Laufzeitänderung.
- **Risiko / Verifikation**: Gering; vorhandener Test für `kindAt/primaryKind/nameMatches` genügt, ggf. `static_assert` für Priorität (jeder Index in `[1,Count)` höchstens einmal).

### EDT-1417 – MCP-Type-Tools: `type_info` liest jede Datei; `writeDef` lädt die Datei ein zweites Mal
- **ID**: EDT-1417
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/McpToolsType.cpp:522-548, 272-297, 126-138
- **Problem**: `type_info` ohne Pfad öffnet pro Definition (bis 200) die `.hasset`, kopiert den Payload und parst das JSON nur, um `fieldCount`/`entryCount` zu liefern. `writeDef` ruft `content.loadAsset(d.rel)` auf, nachdem `openDef` dieselbe Datei bereits per `HAsset::Reader` gelesen hat. `Def` trägt gleichzeitig `StructDef` und `EnumDef` (beide konstruiert) und ein Dummy-`ToolResult::ok(json::object())` als `failure`-Sentinel.
- **Code-Auszug**:
```cpp
std::string payload;
readPayload(a.abs, a.type, payload);
...
HE::TypeRegistry::structFromJson(payload, def);
```
- **Lösung**: Wo das `TypeRegistry` die Definition bereits kennt (`registerStruct/Enum` beim Laden), Zähler aus der Registry lesen statt Datei zu parsen; sonst Ergebnis cachen (Datei-mtime). `Def` als `std::variant<StructDef, EnumDef>` oder `std::optional<ToolResult> failure`.
- **Erwarteter Effekt**: Tool-Aufruf mit ~200 Dateien statt 200 Lese-/Parse-Vorgängen; kein Frame-Pfad, daher Niedrig.
- **Risiko / Verifikation**: Die Datei ist die Wahrheit (Kommentar 147-152: Lesen darf nichts registrieren); Registry-Lösung darf nur bei bereits geladenen Typen greifen. Tests der MCP-Typ-Tools.

### EDT-1418 – Duplizierte Konstanten: `levelColor` zweimal, Amber-Warnfarbe fünfmal im Profiler
- **ID**: EDT-1418
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: NotificationBar.cpp:87-95 und NotificationPopups.cpp:27-37; ProfilerPanel.cpp:448, 457, 468, 622, 753 (`ImVec4(0.95f, 0.75f, 0.2f, 1.0f)`); ProfilerPanel.cpp:338-340 und ProfilerWidgets.cpp:22-24 (`kGood/kWarn/kBad`)
- **Problem**: Die drei Level-Farben stehen wortgleich in zwei Dateien (der Kommentar in NotificationPopups sagt selbst „the same three the bell and the centre use“). Die FPS-Kachelfarbe in `DrawOverview` wiederholt die Budget-Farben aus `ProfilerWidgets.cpp` als Literale.
- **Code-Auszug**:
```cpp
case NoteLevel::Critical:  return ImVec4(0.95f, 0.36f, 0.33f, 1.0f);
case NoteLevel::Important: return ImVec4(1.00f, 0.72f, 0.35f, 1.0f);
```
- **Lösung**: `HE::Ed::levelColor(NoteLevel)` in `NotificationStore.h` (oder `EditorTheme`) als `constexpr`/`inline`; im Profiler `HE::Ed::Theme::Warn/Good/Bad` (oder Konstanten in `ProfilerWidgets.h`) verwenden.
- **Erwarteter Effekt**: Eine Quelle für Farben; keine Laufzeitänderung.
- **Risiko / Verifikation**: Keine; Screenshot-Tests (`test_notification_popups_ui`) bleiben unverändert.

### EDT-1419 – Toter Code und überflüssige Includes
- **ID**: EDT-1419
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: ProfilerWidgets.cpp:437-457 und .h:62-64 (`Sparkline`); ProfilerLayout.h:138-150 (`niceTickStep`); ProfilerPanel.cpp:110 (Parameter `lastRefreshField`), 981 (`if (f) { /* ... */ }`); ProfilerWidgets.cpp:114-116 (`PushFont(nullptr)`); OutlinerPanel.cpp:865-872 (Parameter `w`); ParticleGraphEditorPanel.cpp:3-5, 8; PlayReportPanel.cpp:3-4, 11; ProjectHubPanel.cpp:11-12, 20
- **Problem**: (a) `ProfilerWidgets::Sparkline` wird nirgends in `src/` aufgerufen und dupliziert die Linienschleife aus `StatTile`; `niceTickStep` wird nur von `tests/test_profiler_layout.cpp` genutzt (die Timeline zeichnet keine Zeitachse). (b) `cacheIsStale(prof, now, g_cache.lastRefresh)` bekommt immer dasselbe Feld als Parameter; Z.981 hat einen leeren `if`-Zweig, der nur einen Kommentar enthält. (c) `PushFont(nullptr)`/`PopFont()` um `AddText` hat keine Wirkung (`dl->AddText` nutzt den Draw-List-Font; der Kommentar sagt „gets the accent and the space“, was der Code nicht tut). (d) In `ParticleGraphEditorPanel.cpp` sind `EditorInput.h` (kein `EditorInput::` im File), `<cstdio>`, `<cstdint>` ungenutzt. (e) `PlayReportPanel.cpp` inkludiert `EditorWidgets.h` zweimal; der Kommentar an `EditorHelp.h` („AppContext (playLog…)“) beschreibt ein anderes Include. (f) `ProjectHubPanel.cpp`: `HcCodegen.h` (nur im Kommentar erwähnt; Typ `toolchainProbe` kommt vermutlich über `EditorApplication.h`; prüfen), `TutorialPanel.h`/`DocsPanel.h` nur im `__APPLE__`-Zweig benutzt.
- **Code-Auszug**:
```cpp
ImGui::PushFont(nullptr);
dl->AddText(ImVec2(p0.x + 8.0f, p0.y + 4.0f + lineH * 1.0f), u32(accent), value);
ImGui::PopFont();
```
- **Lösung**: Unbenutztes löschen bzw. in die Testdatei verschieben (`niceTickStep` nur, wenn keine Achse geplant ist); `PushFont(nullptr)` entfernen oder den gewünschten großen Font tatsächlich übergeben (`ctx.fontHeading` weiterreichen); Includes ins `#ifdef __APPLE__` verschieben.
- **Erwarteter Effekt**: Kleinere Header/Compile-Zeit, weniger Verwirrung.
- **Risiko / Verifikation**: Gering; `HcCodegen.h`-Entfernung per Build prüfen. `grep` auf `ProfilerWidgets::Sparkline` in `tests/` vor dem Löschen.

### EDT-1420 – Project Hub: Ordner-Dialog doppelt implementiert; `removeKnownProject` mit Referenz auf eigenes Element
- **ID**: EDT-1420
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ProjectHubPanel.cpp:303-360, 531-534; src/HE_Core/src/Diagnostics/GlobalState.cpp:403-407
- **Problem**: (a) Unter Windows ein ~40-zeiliger COM-`IFileOpenDialog`-Block, sonst `SDL_ShowOpenFolderDialog` (asynchron mit `dialogBridge`); der Windows-Pfad blockiert den Frame (`pDlg->Show`) und hat eigenes Pufferhandling. SDL3 stellt `SDL_ShowOpenFolderDialog` auch unter Windows bereit. (b) `ctx.globalState->removeKnownProject(known[ctx.hubRemoveIndex])` übergibt eine Referenz auf ein Element von `knownProjects`, und `removeKnownProject` ruft `std::remove(kp.begin(), kp.end(), path)` mit genau dieser Referenz als Wert auf. Beim Verschieben überschreibt `std::remove` das referenzierte Element (Value-Aliasing); bei eindeutigen Einträgen funktioniert das zufällig, bei doppelten Einträgen kann ein falscher Eintrag fallen.
- **Code-Auszug**:
```cpp
ctx.globalState->removeKnownProject(known[ctx.hubRemoveIndex]);
```
```cpp
kp.erase(std::remove(kp.begin(), kp.end(), path), kp.end());
```
- **Lösung**: (a) Windows-Zweig entfernen und überall den SDL-Pfad nutzen (Grund für die Sonderbehandlung prüfen: HWND-Parenting, `FOS_FORCEFILESYSTEM`). (b) Vor dem Aufruf kopieren: `const std::string victim = known[ctx.hubRemoveIndex]; removeKnownProject(victim);` oder in `GlobalState` den Parameter by value nehmen.
- **Erwarteter Effekt**: (a) -40 Zeilen plattformspezifischer Code, (b) kein latenter Aliasing-Bug.
- **Risiko / Verifikation**: (a) Windows-Verhalten manuell testen (Dialog-Parent, Ergebnis-Pfad); (b) Test in `tests/` für `GlobalState::removeKnownProject` mit Aliasing-Aufruf ergänzen.

### EDT-1421 – Create-Presets: zwei parallele Listen, `createPreset` mit doppeltem `switch`, kopierte Tabelle
- **ID**: EDT-1421
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/OutlinerPanel.cpp:59-165, 169-201, 1903-1926
- **Problem**: `drawCreateMenu` (Kommentar: „one list, so the two can never drift apart“) und `kPresetTable` (für die macOS-Leiste) sind zwei handgepflegte Listen derselben Einträge; `createPreset` hat zwei `switch`-Blöcke (Name, dann Komponenten) über dieselbe Enum; `entityPresetTable` kopiert `kPresetTable[i].row` zur Laufzeit in ein weiteres static Array.
- **Code-Auszug**:
```cpp
static const EntityPresetRow* rows = [] { static EntityPresetRow r[std::size(kPresetTable)]; for (...) r[i] = kPresetTable[i].row; return r; }();
```
- **Lösung**: Eine `constexpr`-Tabelle `{ label, group, Preset, defaultName }`; `drawCreateMenu` iteriert die Tabelle (Untermenüs nach `group` gruppieren), `entityPresetTable` liefert direkt ein `EntityPresetRow`-Array (Zeilen als eigenes `constexpr`-Array oder Tabelle mit `EntityPresetRow` als erstem Member, gecastet). `createPreset` setzt den Namen aus der Tabelle.
- **Erwarteter Effekt**: Eine Quelle für Menü, macOS-Menü und Namen; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Menü-Reihenfolge und Hilfe-Scope („New Entity“) müssen gleich bleiben; `editor_help_audit.py --check` (Literal-Labels wandern in eine Tabelle: der Audit erkennt sie evtl. nicht mehr, daher vorher prüfen).

### EDT-1422 – Kleine Per-Frame-Allokationen in MeshMaterialSlots, PanelSpotlight, PreviewPick
- **ID**: EDT-1422
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: MeshMaterialSlots.cpp:133-137 und 70-78; PanelSpotlight.cpp:120-135, 182-200, 233-247; PreviewPick.cpp:47
- **Problem**: `MeshMaterialSlots::draw` baut pro Slot und Frame `emptyText` als `std::string` (bei „(missing: path)“ Heap) und ruft einen `std::function`-Slot-Callback auf. `Session::push` kopiert eine ganze `Table` pro Undo-Schritt und entfernt bei Überlauf per `erase(begin())` (O(64)). `KeepClear::update` allokiert pro Frame (solange nicht gleitend) `avoid`, `candidates` (ohne `reserve`) und sucht 1-n Fenster per `FindWindowByName` (String-Hash); `focusedPanel` schreibt in einen `static std::string` mit `substr`. `PreviewPick::pick` invertiert die Matrix pro Kandidat und Klick.
- **Code-Auszug**:
```cpp
std::string emptyText;
if (!sec.materialPath.empty()) emptyText = "(missing: " + sec.materialPath + ")";
```
- **Lösung**: `emptyText` nur bei fehlendem Pfad bauen und sonst `const char*`-Literal; `std::deque<Table>` für das Undo-Fenster; in `KeepClear` Puffer als Member; `PreviewPick::Candidate` um die vorberechnete `invWorld` ergänzen, wenn Kandidatenlisten pro Klick groß werden.
- **Erwarteter Effekt**: Vernachlässigbar im Einzelnen (Schätzung); nur der Vollständigkeit halber.
- **Risiko / Verifikation**: Keine; Verhalten identisch.

### EDT-1423 – Schwere Header in leichten Schnittstellen
- **ID**: EDT-1423
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: OutlinerFilter.h:2 (`HorizonScene/HorizonWorld.h`), NotificationStore.h:2 (`Diagnostics/Log.h`), ProfilerWidgets.h:24-25 (`EngineProfiler.h`, `ProfilerStats.h`), ParticleGraphEditorPanel.h:3-7 (`EditorUI.h`, `AssetAutosave.h`, `CollabDocSync.h`)
- **Problem**: `OutlinerFilter.h` braucht nur `entt::registry` und `Entity`, zieht aber die ganze Welt-Definition; `NotificationStore.h` (über `EditorApplication.h` fast überall) inkludiert den Logger nur für `HE::Log::Record` in privaten Deklarationen; `ProfilerWidgets.h` zieht Profiler-Header für Signatur-Typen.
- **Code-Auszug**:
```cpp
#include <HorizonScene/HorizonWorld.h>   // OutlinerFilter.h
#include <Diagnostics/Log.h>             // NotificationStore.h
```
- **Lösung**: Vorwärtsdeklarationen (`struct Record;` in `HE::Log`, `entt/entity/fwd.hpp`) und Includes in die `.cpp` verschieben; Auswirkungen auf Tests per Compile prüfen.
- **Erwarteter Effekt**: Kürzere Kompilierzeit bei Änderungen an `HorizonWorld.h`/`Log.h` (Schätzung, nicht gemessen).
- **Risiko / Verifikation**: Build-Fehler durch fehlende transitive Includes in abhängigen Dateien; reiner Compile-Check.

---

## Gelesen

Alle 34 Dateien des Slice wurden vollständig gelesen: ja (OutlinerPanel.cpp in zwei Blöcken 1-1000 und 1000-1948; keine Datei ausgelassen). Zusätzlich punktuell geprüft: `GlobalState.cpp` (removeKnownProject), `UIWidgetTree.cpp` (childrenOf), `EntityVisibility.cpp`, `EntityActive.cpp`, `EngineProfiler.h` (Live-Ring).

## Editor Slice 15 – ProjectSettingsPanel.cpp … SplineTool.h

## Zusammenfassung

Der Slice (32 Dateien, ca. 10.860 Zeilen) ist insgesamt sauber gebaut und sehr gut kommentiert. Die meisten Panels cachen bereits sinnvoll (Statusmodell der Source-Control-Änderungen per `generation`, Root-Motion-Pfad, Szenenliste, Preview-Cache im Report-Dialog). Die relevanten Funde konzentrieren sich auf drei Muster: (1) Arbeit pro Frame, die nur bei Zustandsänderung nötig wäre (Prozessstart, Polygon-Rasterisierung, O(Auflösung²)-Zählung, Baumaufbau); (2) Listen ohne Clipping bzw. mit Heap-Allokation pro Zeile und Frame; (3) wiederholte Boilerplate (Seitenrümpfe in ProjectSettings, `stateByContentPath`, Gruppen-Strings im Sequencer). Funde: 3 Hoch, 9 Mittel, 11 Niedrig. Es wurde nichts gebaut oder gemessen; Aufwandsschätzungen sind Schätzungen.

## Funde

### EDT-1501 – Zwei `git`-Prozesse pro Frame im „Create branch“-Dialog
- **ID**: EDT-1501
- **Kategorie**: E
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/SourceControlPanel.cpp:1752-1758 (Aufruf), src/HE_SourceControl/src/GitCli.cpp:437-449 (Implementierung)
- **Problem**: Solange das modale „Create branch“-Fenster offen ist und das Namensfeld nicht leer ist, werden in jedem Frame synchron `GitCli::isValidBranchName` (`git check-ref-format --branch`) und `GitCli::branchExists` (`git rev-parse --verify`) aufgerufen. Beide laufen über `run(root, {...}, 10000)`, also Prozessstart mit bis zu 10 s Timeout im UI-Thread. Unter Windows (Prozessstart ca. 10–30 ms, Defender-Scan) bedeutet das sichtbares Ruckeln beim Tippen; bei einem hängenden `git` friert der Editor bis zu 10 s ein, und zwar pro Aufruf.
- **Code-Auszug**:
```cpp
if (!empty && !git->projectRoot().empty())
{
    nameOk = HE::Sc::GitCli::isValidBranchName(git->projectRoot(), s_branchName);
    taken  = nameOk && HE::Sc::GitCli::branchExists(git->projectRoot(), s_branchName);
}
```
- **Lösung**: Ergebnis cachen und nur neu fragen, wenn sich `s_branchName` ändert. Ein Eintrag `{ std::string checkedName; bool nameOk, taken; }` reicht. Zusätzlich `taken` ohne Prozess beantworten: `git.branches()` liegt im `GitController` schon als Liste vor (wird in `drawBranchesTab` benutzt), also `std::find` über die lokalen Branches. Nur `check-ref-format` bleibt ein Prozess, dann einmal pro Änderung, besser mit kleinem Debounce (z. B. erst 150 ms nach dem letzten Tastendruck) oder über den GitService-Worker.
```cpp
static std::string s_checked; static bool s_ok = false;
if (s_checked != s_branchName) { s_checked = s_branchName; s_ok = isValidBranchName(...); }
const auto& l = git->branches(); const bool taken = s_ok && std::find(l.begin(), l.end(), s_branchName) != l.end();
```
- **Erwarteter Effekt**: Von 2 Prozessstarts pro Frame auf 1 pro Tastendruck (Schätzung: Frame-Zeit im Dialog von 20–60 ms auf < 1 ms in Ruhe).
- **Risiko / Verifikation**: `branchExists` prüft `refs/heads/<name>`; die Liste `branches()` muss dieselben Namen enthalten (lokal, kein `origin/`-Präfix). Nach externem Branch-Anlegen ist die Liste so aktuell wie der Status-Poll, das ist für diesen Dialog ausreichend. Prüfen mit einem Test, der einen vorhandenen Branch und einen ungültigen Namen („a..b“) eingibt; zwischen Frames darf kein Prozess starten (Zähler im Test-Stub).

### EDT-1502 – Spline-Panel: Polygon-Rasterisierung, O(res²)-Zählung und Kurven-Sampling in jedem Frame
- **ID**: EDT-1502
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/SplineTool.cpp:288-302 (lakeSection), :355, :440 (worldLength), :474-477; Kostenquellen: src/HE_Scene/src/WaterLake.cpp:638-690, src/HE_Scene/src/WaterField.cpp:196-203
- **Problem**: `renderPanel()` läuft jeden Frame, solange die Quick-Settings im Spline-Modus sind. Dabei passiert pro Frame:
  - `L::whyNot(world, terrain, spline)` (nur bei geschlossenem, noch nicht in einen See verwandeltem Spline): baut `polygonOf(...)` und ruft `rasterizePolygon(poly, sizeX, sizeZ, res)` auf, also eine volle Rasterisierung auf dem Wassergitter (Standard 256², bis 4096²), nur um einen Hinweistext zu erzeugen. Dazu `linkOf` zweimal (Schleife über alle Terrains) und `landscapeUnder` (je Terrain `glm::inverse(worldMatrixOf(...))`).
  - Bei einem See: `tc->water.wetCells(body->id)` zählt über das gesamte `coverage`-Array (256² = 65 k bis 4096² = 16,7 M Einträge, je mit `owner[i]`-Vergleich).
  - `worldLength()`: `Curve`-Aufbau, `curve.sample(16)` (Heap-Vektor) und zwei Matrix-Vektor-Multiplikationen pro Sample, nur für die Textzeile „N points, X m“.
- **Code-Auszug**:
```cpp
const std::string why = L::whyNot(world, terrain, spline);       // Zeile 302, Rasterisierung
...
ImGui::TextDisabled("%u cells of water", tc->water.wetCells(body->id));  // Zeile 355
...
worldLength(world, active)                                        // Zeile 440
```
- **Lösung**: Ein kleiner Cache im Tool (`struct LakeInfo { uint64_t key; std::string why; uint32_t wet; float length; }`), Schlüssel aus `ctx.undoSys->revision()` plus Entity-UUID plus `SplineComponent`-Punktanzahl. Der Gizmo-Drag ändert die Control Points ohne neue Undo-Revision, daher zusätzlich einen billigen Hash der Punkte bzw. einen Zähler, den `moveSelectedPoint` erhöht, in den Schlüssel nehmen. Neu berechnen nur bei Schlüsselwechsel, im Drag höchstens alle ~100 ms. `wetCells` kann alternativ im `Field` mitgezählt werden (dort wo `coverage` geschrieben wird), das ist die sauberere Lösung, berührt aber HE_Scene.
- **Erwarteter Effekt**: Bei 256² etwa 0,1–0,5 ms/Frame, bei 1024²+ mehrere ms/Frame gespart (Schätzung, nicht gemessen). Das Panel ist nur im Spline-Modus aktiv, die Kosten wachsen aber mit der Landschaftsauflösung.
- **Risiko / Verifikation**: Cache-Invalidierung ist die Gefahr (veralteter „why“-Text, Create-Button zu lange gesperrt/freigegeben). Absichern mit einem UI-Test (`tests/test_*_ui.cpp`-Stil): Spline schließen, Punkt verschieben, Hinweis wechselt innerhalb eines Frames nach Drag-Ende; See anlegen, Zellenzahl aktualisiert sich. Wo es ohne Messung unklar ist: erst mit dem Profiler an einer 2048²-Landschaft messen.

### EDT-1503 – Source Control „Tree view“: Baum und Zeilen jeden Frame neu aufgebaut, nicht geclippt
- **ID**: EDT-1503
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/SourceControlPanel.cpp:653-713 (DirNode/insertRow/drawDirNode/drawRows), :924-935
- **Problem**: Der Baummodus ist der Standard (`s_treeView = true`, Zeile 69/1648). In `drawRows` wird pro Frame und Sektion ein `DirNode root` mit `std::map<std::string, DirNode>` aufgebaut: pro Datei und Verzeichnisebene ein `r.path.substr(...)` (Allokation) plus Map-Knoten-Allokation, bei Single-Child-Ketten zusätzlich String-Konkatenation in `drawDirNode`. Danach wird jede Zeile gezeichnet (`TreeNodeEx` mit `DefaultOpen`, pro Zeile ca. 6–10 ImGui-Aufrufe, `PushID(path.c_str())` hasht den Pfad), ohne `ImGuiListClipper`. Der Flat-Modus clippt dagegen sauber (Kommentar Zeile 707: „a freshly imported project can have thousands of untracked rows“), also trifft der Kommentar im Standardmodus nicht zu. Bei 5.000 neuen Dateien sind das im Baummodus pro Frame mehrere zehntausend Allokationen plus 5.000 gezeichnete Zeilen.
- **Code-Auszug**:
```cpp
if (s_treeView)
{
    DirNode root;
    for (const ChangeRow& r : rows) insertRow(root, r);
    for (const auto& [name, sub] : root.dirs) drawDirNode(name, sub, rowFn);
```
- **Lösung**: (1) Den Baum in `rebuildModel` bauen, also nur bei neuer `generation`/Filter/Suche, und im `Model` pro Sektion ablegen. (2) Den Baum zu einer flachen „sichtbaren Zeilen“-Liste (Verzeichniskopf oder Datei, Einrückung) abflachen, Auf-/Zuklappen als eigener Zustand (`std::unordered_set<std::string>` geschlossener Verzeichnisse pro Sektion), und diese Liste mit demselben `ImGuiListClipper` zeichnen wie den Flat-Modus. Die Zeilenhöhe ist konstant (`GetFrameHeight`), das Clipping ist daher ohne Layout-Änderung möglich. Einfacher Zwischenschritt: nur (1) umsetzen, das nimmt die Allokationen heraus, und Verzeichnisköpfe zuklappbar lassen.
- **Erwarteter Effekt**: Pro Frame von O(n·Tiefe) Allokationen und n gezeichneten Zeilen auf O(sichtbare Zeilen) ohne Allokation (Schätzung: bei 5.000 Dateien von mehreren ms auf < 0,2 ms).
- **Risiko / Verifikation**: Sichtbares Verhalten muss gleich bleiben: DefaultOpen, Single-Child-Ketten falten, Reihenfolge Verzeichnisse vor Dateien, Zustand bleibt beim Rebuild erhalten (heute hält ImGui den Zustand per ID im `TreeNodeEx`). Ein Zwischenstand, der `TreeNodeEx` beibehält, kann den Clipper nicht benutzen; dann nur (1). Prüfen per UI-Test mit 3.000 synthetischen `RepoStatus`-Einträgen und `HE_UI_DUMP_DIR`-Frames für Baum und Flat.

### EDT-1504 – Vorschau-Render pro Frame auch ohne jede Änderung (Scene 2–4, Skeletal Mesh Editor)
- **ID**: EDT-1504
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/SecondaryViewportPanel.cpp:179-181, :255-257; src/HE_Editor/SkeletalMeshEditorPanel.cpp:627-658
- **Problem**: Jedes offene Secondary-Viewport-Fenster (bis zu 3) ruft jeden Frame `RenderWorldPreview` auf; der Kommentar im selben File nennt das selbst „a synchronous GPU round trip“. Der Skeletal-Mesh-Editor ruft ebenso jeden Frame `RenderSkeletalPreview` auf und wertet davor `evaluateClipPoseLocked` aus, auch wenn der Clip pausiert ist, die Kamera ruht und nichts bearbeitet wurde. Zusätzlich liegt `boneMatrices` als lokaler `std::vector<glm::mat4>` (Zeile 627), also eine Heap-Allokation pro Frame.
- **Code-Auszug**:
```cpp
void* tex = ctx.renderer->RenderWorldPreview(*ctx.contentManager, *ctx.world, ...
    ov, glm::vec3(0.0f), env, &viewProj, /*slot=*/static_cast<uint32_t>(index + 1));
```
```cpp
std::vector<glm::mat4> boneMatrices; // empty = bind pose
AnimationPreview::evaluateClipPoseLocked(*mesh, *clip, st.clipTime, opt, boneMatrices);
```
- **Lösung**: Pro Pane einen Schlüssel aus (Kamera-Pose, Größe, Welt-/Szenen-Revision bzw. `EditorUndo::revision()`, Env, Grid, `clipTime`, Orbit-Werte, `showSkeleton`) halten. Bei unverändertem Schlüssel das letzte `tex` weiterzeichnen. Das setzt voraus, dass `IRenderer` die Slot-Textur zwischen Frames stabil hält (für `slot` ist das laut Parameter plausibel, aber **nicht verifiziert**). Weltänderungen ohne Revision (Animation läuft im Editor außerhalb von Play, Wasser, Partikel) würden dann „einfrieren“; deshalb pro Pane ein Schalter „Live“ oder ein Refresh-Intervall (z. B. 10 Hz) als Rückfall. `boneMatrices` als Member in `State` halten (`clear()` statt neu allokieren).
- **Erwarteter Effekt**: Mit drei offenen Panes bis zu 3 Vorwärts-Pässe und GPU-Syncs pro Frame weniger (Schätzung, abhängig von Szene; Messung nötig).
- **Risiko / Verifikation**: Hoch für Korrektheit der Anzeige (sichtbar veraltete Vorschau). Nur mit Messung und Backend-Test auf allen Backends (Metal, GL, Vulkan, D3D) umsetzen; „unsicher / messen“.

### EDT-1505 – Skeleton-Baum: Kinderlisten, Strings und Zeichnung jedes Frame, ungeclippt
- **ID**: EDT-1505
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/SkeletalMeshEditorPanel.cpp:381-391, :186-201
- **Problem**: Pro Frame wird `std::vector<std::vector<int>> children(N)` neu gebaut (N+ Allokationen) und rekursiv gezeichnet. `drawBoneNode` erzeugt pro Gelenk `label` (Kopie oder `"Joint " + to_string`) und danach `label + "##joint" + std::to_string(idx)` (2–3 weitere Allokationen). Alle Knoten sind `DefaultOpen`, also werden bei 150–800 Gelenken (Humanoid mit Fingern/MetaHuman) jeden Frame alle Knoten gezeichnet, ohne Clipping.
- **Code-Auszug**:
```cpp
std::vector<std::vector<int>> children(mesh->skeleton.size());
...
const bool open = ImGui::TreeNodeEx((label + "##joint" + std::to_string(idx)).c_str(), flags);
```
- **Lösung**: Kinderlisten einmal pro geladenem Mesh in `State` cachen (Schlüssel `meshId` + `skeleton.size()`), idealerweise als CSR-Array (`firstChild/nextSibling` oder `std::vector<int> offsets/indices`) statt vector-of-vectors. Label mit `ImGui::TreeNodeEx(reinterpret_cast<void*>(idx), flags, "%s", name)` (Pointer-ID) statt String-Bau. Bei großen Skeletons zusätzlich nur sichtbare Knoten zeichnen (abgeflachte Liste plus Clipper wie bei EDT-1503).
- **Erwarteter Effekt**: ca. 3 Allokationen × N pro Frame entfallen (Schätzung 0,1–0,5 ms bei 300 Gelenken).
- **Risiko / Verifikation**: Gering. Beim Wechsel des Mesh-Assets Cache invalidieren (Kommentar im File weist auf verschobene Asset-Pointer hin, deshalb nur nach ID/Größe schlüsseln, nie Pointer halten). Sichtprüfung der Baumstruktur, Zustand des Auf-/Zuklappens bleibt per ID erhalten.

### EDT-1506 – Footer-Status baut jeden Frame Strings und Tooltip-Text
- **ID**: EDT-1506
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/SourceControlPanel.cpp:1932-2052
- **Problem**: `DrawFooterStatus` läuft laut eigenem Kommentar jeden Frame, solange ein Projekt offen ist. Die Zählungen (`dirtyCount` usw.) sind per `generation` gecacht, aber `label` und `tip` werden jeden Frame neu gebaut: Kopie von `st.branch`, mehrere `std::string`-Konkatenationen, `std::to_string`, Kopie von `git->lastError()`, Tooltip-Text auch ohne Hover. Das sind ca. 10–15 Allokationen pro Frame, dauerhaft, auch wenn das Source-Control-Fenster nie geöffnet wird. Außerdem ist der „Identitäts“-Teil des Cache-Schlüssels (`s_source != &st`, Zeile 1957) wirkungslos, da `git->status()` eine Referenz auf ein Member (`m_service.status()`) ist und die Adresse nie wechselt.
- **Code-Auszug**:
```cpp
label = branch + "   ";
...
label += std::to_string(dirty) + (dirty == 1 ? " change" : " changes");
tip = "Branch: " + branch;
...
const bool clicked = ImGui::Button(label.c_str());
```
- **Lösung**: `label` und `tint` im vorhandenen `static`-Cache mitführen (Schlüssel `generation`, `isRepo`, `detached`, `branch`, `ahead/behind`) und `tip` nur im `IsItemHovered()`-Zweig bauen. Der tote Identitätsvergleich kann entfallen oder durch eine Projekt-ID ersetzt werden (siehe „Unsicher“).
- **Erwarteter Effekt**: ca. 10–15 Allokationen/Frame weniger; messbar klein (Schätzung < 20 µs), aber dauerhaft im Frame-Loop.
- **Risiko / Verifikation**: Gering. Der Tooltip-Inhalt enthält `git->lastError()` und ändert sich ohne neue `generation`; deshalb dort weiter frisch bauen (nur bei Hover). Vergleich der Texte vor/nach in einem kleinen Test mit gestubtem `GitController`.

### EDT-1507 – `rebuildModel`: Allokationen und Datei-I/O pro Tastendruck im Filterfeld
- **ID**: EDT-1507
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/SourceControlPanel.cpp:223-257, :291-344
- **Problem**: Der Model-Cache ist an `search` und `filter` gekoppelt. Jeder Tastendruck im Filterfeld löst `rebuildModel` über alle schmutzigen Dateien aus, und das berechnet jedes Mal für jede Datei neu: `abs = st.root + "/" + path`, `categoryOf` (mit `lowerCopy(rel.substr(dot))`, zweimal Allokation) und für `.hasset` `EditorAssetTypeCache::assetTypeOf(abs)` (Header-Sniff, bei kaltem Cache Dateizugriff im UI-Thread). Die Kategorie hängt aber weder von Filter noch Suche ab. Zusätzlich `lowerCopy(path)` pro Datei für den Such-Needle und `into.push_back(r)` kopiert `ChangeRow` mit vier Strings.
- **Code-Auszug**:
```cpp
const std::string abs = st.root + "/" + path;
const Cat cat = categoryOf(abs, path);
...
if (!needle.empty() && lowerCopy(path).find(needle) == std::string::npos) continue;
```
- **Lösung**: Zweistufig: Stufe 1 (nur bei neuer `generation`): `std::vector<ChangeRow> all` mit `cat`, `name/dir`, vorberechnetem `lowerPath` und `catCounts`. Stufe 2 (bei Filter/Suche): nur Indizes filtern und nach Sektion einordnen (`std::vector<uint32_t>` oder `const ChangeRow*`). Das beseitigt Allokationen und I/O beim Tippen. Die Extension-Listen als `constexpr std::string_view`-Arrays statt `std::string`-Vergleichen.
- **Erwarteter Effekt**: Tippen im Filter bei 10 k Änderungen von ca. 40 k Allokationen (+ evtl. Datei-I/O) auf nahezu allokationsfrei (Schätzung).
- **Risiko / Verifikation**: Gering bis mittel; `ChangeRow`-Zeiger müssen über die Lebensdauer des Models stabil sein (Vektor nicht mehr verändern, nachdem Zeiger verteilt sind). Sortierreihenfolge (`byPath`) beibehalten. Test: dieselbe Ausgabe wie bisher für gemischte Suchbegriffe/Filter.

### EDT-1508 – Branches-/History-Tab: O(R·L) mit Allokationen pro Frame, ungeclippte Listen
- **ID**: EDT-1508
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/SourceControlPanel.cpp:1143-1166 (Branches), :978-1086 (History)
- **Problem**: `drawBranchesTab` bildet pro Frame für jeden Remote-Branch `shortName` (Allokation bei jedem Eintrag mit `/`, also praktisch allen) und sucht ihn linear in den lokalen Branches (O(R·L)). Bei großen Repos (hunderte Remote-Branches) sind das hunderte Allokationen und zehntausende Stringvergleiche pro Frame. Alle Branch-Zeilen werden ungeclippt gezeichnet. `drawHistoryTab` zeichnet alle `recentCommits()` ungeclippt, pro Commit `PushID(string)`, `snprintf`, `std::set<std::string>::count` und `CalcTextSize`; aufgeklappte Commits zeichnen bis zu 200 Dateizeilen ebenfalls ungeclippt. Die Obergrenze von `recentCommits()` habe ich nicht verifiziert.
- **Code-Auszug**:
```cpp
const std::string shortName = slash == std::string::npos ? rb : rb.substr(slash + 1);
const auto& locals = git.branches();
if (std::find(locals.begin(), locals.end(), shortName) == locals.end()) remoteOnly.push_back(&rb);
```
- **Lösung**: `remoteOnly` als Cache im Controller bzw. im Panel (Schlüssel: Größe/Revision von `branches()` und `remoteBranches()`, bzw. `st.generation`), mit `std::string_view` auf den Teil nach dem ersten `/` und einem `std::unordered_set<std::string_view>` der lokalen Namen (O(R+L)). Listen mit `ImGuiListClipper` zeichnen (konstante Zeilenhöhe in Branches; in History hat die Zeile konstante Höhe `rowH`, ausgenommen aufgeklappte Commits, dort mit Positions-Tabelle oder Clipper nur über zugeklappte Bereiche).
- **Erwarteter Effekt**: Allokationsfrei und O(R+L) statt O(R·L); nur sichtbare Zeilen gezeichnet (Schätzung, nur bei großen Repos spürbar).
- **Risiko / Verifikation**: Gering (Branches), mittel (History-Clipper wegen variabler Höhe). Test mit 500 Remote-Branches; Ergebnis der Liste „Only on the server“ identisch.

### EDT-1509 – ShortcutsPage: Allokationen und `conflicts()` für jede Zeile in jedem Frame
- **ID**: EDT-1509
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/ShortcutsPage.cpp:100-107, :118-166
- **Problem**: Bei gesetztem Filter baut `matches` pro Aktion und Frame `needle` (Kopie), `hay` (drei Konkatenationen inkl. `label(a.id)`) und lowercased beide. Unabhängig vom Filter wird pro sichtbarer Zeile und Frame `conflicts(a.id)` aufgerufen (liefert `std::vector<const Action*>`, vermutlich eine Suche über alle Aktionen, also O(n²) pro Frame; Implementierung in EditorShortcuts.cpp liegt außerhalb dieses Slice), `label(a.id)` ein weiteres Mal als `std::string`, dazu `isDefault(a.id)` bis zu dreimal. Alles ungeclippt (ca. 80–150 Aktionen).
- **Code-Auszug**:
```cpp
std::string needle = s_shortcutFilter, hay = std::string(a.label) + ' ' + a.category + ' ' + label(a.id);
...
const std::vector<const Action*> clash = conflicts(a.id);
std::string text = capturing ? std::string("Press keys\xe2\x80\xa6") : label(a.id);
```
- **Lösung**: Needle einmal pro Frame (oder nur bei Eingabe) lowercasen; Treffer pro Aktion nur neu bestimmen, wenn sich Filtertext oder Bindungen geändert haben (Zähler `EditorShortcuts::revision()`), Ergebnis als `std::vector<char>`/Bitset cachen. `conflicts()` einmal für alle Aktionen vorab berechnen (Map chord → Anzahl/Liste, O(n)) und pro Zeile nur nachschlagen. Hier ist der Hebel kleiner Aufwand, der Gewinn aber auf eine selten offene Seite beschränkt.
- **Erwarteter Effekt**: Von ca. 3–5 Allokationen pro Zeile und O(n²) pro Frame auf O(n) ohne Allokation (Schätzung; unsicher, solange `conflicts()` nicht gelesen wurde).
- **Risiko / Verifikation**: Gering. `tests/test_shortcuts_ui.cpp` deckt die Seite bereits headless ab; um Rot-Markierung bei Konflikten und Filter ergänzen.

### EDT-1510 – Szenen-Stamp: FNV-Hash über die ganze Szenendatei synchron im UI-Thread
- **ID**: EDT-1510
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/SceneFileStamp.h:25-39, :47-57, :71-87; Aufrufer src/HE_Editor/SceneDiskWatch.cpp:26-30, :51-55, :121, :128
- **Problem**: `remember()` liest und hasht die komplette Szenendatei, und zwar nach jedem Öffnen, jedem Speichern, jedem Szenenwechsel und nach jeder Antwort im Dialog, alles im UI-Thread. `changed()` (hinter dem 1-s-Poll, wenn Größe/mtime abweichen) hasht ebenfalls komplett. Der Header-Kommentar beziffert große Szenen auf „a dozen megabytes“. FNV-1a Byte für Byte hat eine serielle Abhängigkeitskette (xor+mul, ca. 4 Takte/Byte): 12 MB entsprechen grob 12–20 ms Hitch pro Aufruf (Schätzung, nicht gemessen), plus Lesezeit. Unmittelbar nach dem eigenen Speichern liest `remember()` die gerade geschriebene Datei noch einmal.
- **Code-Auszug**:
```cpp
for (std::streamsize i = 0; i < n; ++i)
{
    h ^= static_cast<unsigned char>(buf[i]);
    h *= 1099511628211ull;
}
```
- **Lösung**: (a) Hash auf 8-Byte-Wörter umstellen (z. B. ein 64-Bit-Multiplikations-Mix pro Wort, 4–8× schneller) oder `XXH3`, falls bereits im Projekt vorhanden. (b) Hashen im `remember()` nach eigenem Speichern ganz vermeiden: Der Writer kennt die Bytes bereits; ein Hash des Schreibpuffers (oder Größe+mtime plus „hash lazily“) genügt, der Hash wird erst gebraucht, wenn Größe/mtime später abweichen. (c) Alternativ `remember()` und `changed()` auf einem Worker rechnen und nur das Ergebnis im Frame abholen.
- **Erwarteter Effekt**: Beim Speichern/Öffnen großer Szenen Hitch von ~10–20 ms auf < 3 ms (Schätzung).
- **Risiko / Verifikation**: Hashwechsel ändert keinen persistierten Zustand (nur im Speicher), daher risikoarm. Bei Variante (b) darf sich die Semantik „unveränderter Inhalt gilt nicht als Änderung“ nicht verschieben; vorhandene Tests zum Stamp (git-touch mit identischen Bytes) müssen weiter grün sein.

### EDT-1511 – Autosave serialisiert die Szene synchron im Frame
- **ID**: EDT-1511
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/SceneAutosave.cpp:100-172, vor allem :135 (`writer(tmp.string())`); Kommentar src/HE_Editor/SceneAutosave.h:68-69
- **Problem**: `update()` läuft pro Frame und ruft bei Fälligkeit (Standard alle 60 s, Mindestintervall 10 s) den `Writer` (`SceneSerializer::save`) im UI-Thread auf. Zusätzlich folgen im selben Frame `create_directories`, `remove`, `rename` und das Schreiben des Manifests. Die Kosten wachsen mit der Szenengröße (große Landschaften sind Base64-kodiert, also MB groß); das Header-Kommentar räumt ein, dass die Serialisierung selbst „der Hitch“ werden kann, und begrenzt nur das Intervall.
- **Code-Auszug**:
```cpp
removeQuiet(tmp);
if (!writer(tmp.string()))
```
- **Lösung**: Zweistufig: Serialisierung in einen Speicherpuffer im UI-Thread (konsistenter Schnappschuss, `SceneSerializer::saveToMemory` existiert, siehe SplineTool.cpp:379), Dateischreiben, `rename` und Manifest auf einem Worker. Das teilt die Kosten auf, ohne ein Datenrennen auf der Welt zu riskieren. Wie viel der Gesamtkosten die reine Serialisierung ist, ist nicht gemessen.
- **Erwarteter Effekt**: Hitch alle 60 s verkleinert um den I/O-Anteil (Schätzung; bei langsamen Platten/Netzlaufwerken deutlich).
- **Risiko / Verifikation**: Mittel: Reihenfolge „Szene vor Manifest“ und Atomizität müssen erhalten bleiben (Tests der Timing-Regeln mit Fake-Clock existieren laut Header, das Muster auf Writer-Seite erweitern); `clear()`/`adoptPending()` während laufendem Worker-Write absichern (Join oder Generationszähler). „Unsicher / messen“.

### EDT-1512 – `SequencerTimeline::draw`: eine 415-Zeilen-Funktion mit doppeltem Key-Code
- **ID**: EDT-1512
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/SequencerTimeline.cpp:384-799 (insbesondere :589-640 vs. :673-708)
- **Problem**: `draw()` ist ca. 415 Zeilen lang und mischt Zoom, Ruler, Wertachse, Zeilen, Keys, Kurve, Drag, Löschen und Playhead. Dope-Sheet- und Kurvenansicht enthalten fast identische Schleifen über die Keys (Sichtbarkeits-Check, `keyButton`, Press-Behandlung mit identischen sieben Zuweisungen, `hit.remove`, Doppelklick-Einfügen) und zwei fast gleiche Double-Click-Blöcke. Pro Key werden außerdem `PushClipRect/PopClipRect` um jede Raute/jeden Punkt gesetzt (Zeilen 620-622 und 692-695), obwohl für die ganze Spur ein Clip genügt. Zusätzlich iterieren beide Schleifen über alle Keys, obwohl `times` sortiert ist.
- **Code-Auszug**:
```cpp
if (hit.pressed)
{
    if (!picked) out.selectionChanged = true;
    view.trackSel = i;
    view.keySel   = k;
    if (view.playhead != ch.times[k]) out.playheadMoved = true;
    view.playhead  = std::clamp(ch.times[k], 0.0f, duration);
    view.scrubbing = false;
    view.keyArmed = true;
    view.dragging = false;
}
```
- **Lösung**: Hilfsfunktionen `selectKey(View&, Result&, int track, int key, const PropertyAnimChannel&, float duration, bool picked)` und `addKeyAtPointer(...)` extrahieren; `draw()` in `drawRuler/drawValueGrid/drawTrackRows/drawCurve/handleKeyDrag/applyRemovals` aufteilen. Einen Clip-Rect pro Lane statt pro Key. Für große Keyzahlen die sichtbare Spanne per `std::lower_bound` auf `times` bestimmen. Rein strukturell, Verhalten identisch.
- **Erwarteter Effekt**: Deutlich weniger Code und eine Stelle für Key-Auswahl-Semantik. Performance-Gewinn nur bei sehr vielen Keys/Draw-Commands (unsicher, messen).
- **Risiko / Verifikation**: Gering, wenn die Reihenfolge der Zustandsänderungen erhalten bleibt. Der headless UI-Test für den Streifen (laut SequencerTimeline.h) deckt Drag, Doppelklick und Wheel ab und ist die Absicherung.

### EDT-1513 – Statische Gruppen als Strings: strcmp und Erstbuchstaben-Vergleich
- **ID**: EDT-1513
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/SequencerTimeline.cpp:38-56, :289-292; src/HE_Editor/SequencerPanel.cpp:155-162, :253-263
- **Problem**: `targetGroup()` liefert Literal-Strings; Aufrufer vergleichen mit `std::strcmp` (Spurzähler, Add-Track-Popup) und `groupColour` prüft nur `targetGroup(t)[0] == 'T'`. Dadurch erhalten „Camera“ und „Visibility“ stillschweigend die Material-Farbe, obwohl der Kommentar „one cool tint per group“ zwei Gruppen nennt; der Zähler im Panel führt „camera FOV, visibility“ als „other“. Die Zählung läuft jeden Frame über alle Kanäle.
- **Code-Auszug**:
```cpp
return targetGroup(t)[0] == 'T' ? kGroupXform : kGroupMat;
...
if (std::strcmp(group, "Transform") == 0)     ++transformTracks;
```
- **Lösung**: `enum class TargetGroup { Transform, Material, Camera, Visibility }` mit `targetGroupOf(PropTarget)` und `groupName(TargetGroup)`; Farbe per `switch`. Ein `constexpr`-Array `kTargetGroup[kTargetCount]` ersetzt die Switches.
- **Erwarteter Effekt**: Weniger fehleranfällig, kein Stringvergleich pro Frame.
- **Risiko / Verifikation**: Gering; Farben der Gruppen Camera/Visibility entscheiden (heute Material-Farbe, Verhalten beibehalten oder bewusst ändern). Bestehender Test der Strip-Funktionen genügt.

### EDT-1514 – `stateByContentPath` in sieben Panels kopiert
- **ID**: EDT-1514
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/SequencerPanel.cpp:478-508; gleiche Form in BlendSpacePanel.cpp:252, AnimatorStateMachineEditorPanel.cpp:213, TypeAssetPanel.cpp:314, UIEditorPanel.cpp:6839, MaterialEditorPanel.cpp:1934
- **Problem**: Jedes Asset-Panel implementiert dieselbe Suche („über `s_states.forEach`, `st.relPath == contentPath`“) plus `isDirtyByContentPath`/`reloadByContentPath`; das Sequencer-Panel kommentiert selbst „Same walk as BlendSpacePanel's“. `AssetPanelState` kennt den Schlüssel `relPath` nicht, daher die Kopien.
- **Code-Auszug**:
```cpp
s_states.forEach([&](const std::string&, PanelState& st) {
    if (!found && st.relPath == contentPath) found = &st;
});
```
- **Lösung**: In `AssetPanelState<T>` (EditorPanelState.h) eine Methode `T* findIf(Pred)` bzw. `T* findByRelPath(const std::string&)` (für Typen mit `relPath`-Feld, per `requires`) anbieten und `isDirtyByContentPath/reloadByContentPath` als Schablone dort ablegen. Die Panels behalten nur dünne Weiterleitungen.
- **Erwarteter Effekt**: Minus ca. 15–20 Zeilen pro Panel, einheitliches Verhalten.
- **Risiko / Verifikation**: Gering. Die MCP-Pfade, die diese Funktionen nutzen, über vorhandene Tests (Mcp-Tests) laufen lassen.

### EDT-1515 – ProjectSettingsPanel: Seitenrümpfe und `commit`-Boilerplate wiederholt
- **ID**: EDT-1515
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ProjectSettingsPanel.cpp:61-69 (Helfer `openProject`), :114-119, :176-180, :342-347, :515-520 (vier Kopien), sowie ca. 60 Stellen `commit |= ImGui::IsItemDeactivatedAfterEdit();` (z. B. :661-662, :750-753, :936-940, :1093-1110, :1294-1350)
- **Problem**: Vier Seiten (Permissions, Collision Layers, Application, Fonts) wiederholen den „No project is open“-Block wörtlich, obwohl `openProject()` in Zeile 61 genau das tut. Die Seiten zerfallen außerdem in zwei Persistenzpfade (`saveProject` vs. `commitSettings`/`saveProjectSettings`) mit je eigener Fehlermeldung. Das Muster „Row-Widget, dann `commit |= IsItemDeactivatedAfterEdit()`“ steht ca. 60-mal.
- **Code-Auszug**:
```cpp
if (!ctx.projectManager || ctx.projectManager->currentProject().path.empty())
{
    ImGui::TextDisabled("No project is open.");
    return;
}
ProjectData& p = ctx.projectManager->currentProject();
```
- **Lösung**: Die vier Seiten auf `ProjectData* pp = openProject(ctx); if (!pp) return;` umstellen. Eine kleine Hilfe `struct Commit { bool v=false; void edit(bool changedOrDeactivated); }` bzw. ein Lambda `after()`, das `v |= ImGui::IsItemDeactivatedAfterEdit()` kapselt, oder `EditorWidgets::Row::...` um eine Variante erweitern, die den Wert zurückgibt. Optional die Seiten in eine Tabelle `{Page, title, draw}` fassen (Dispatch-`switch` in `render` und `kNavGroups` spiegeln sich heute).
- **Erwarteter Effekt**: ca. 40–60 Zeilen weniger, weniger Abweichung zwischen den Seiten.
- **Risiko / Verifikation**: Gering. `editor_help_audit.py --check` muss weiter grün sein (Labels unverändert lassen).

### EDT-1516 – ProjectSettingsPanel: kleine Pro-Frame-Arbeit (Backend-Liste, `projectRoot()`, Icon-Suche, Config-Kopie)
- **ID**: EDT-1516
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/ProjectSettingsPanel.cpp:772-777 (Backend-Namen), :650-651 (`projectRoot()`), :378-392 (Icon-Suche), :841-843 (Config-Kopien), :222-225 (Layer-Namen)
- **Problem**: Alles nur auf der jeweils sichtbaren Seite, aber pro Frame:
  - Seite „Window & Backend“: `std::vector<const char*> names` wird neu gebaut, dazu dreimal `BackendRules::choicesFor(...)` und ein `std::find_if` mit `strcmp` je Eintrag (O(n²)).
  - Seite „General“: `ctx.projectManager->projectRoot()` liefert vermutlich einen `std::string` per Wert, nur um `s_sceneListRoot != root` zu prüfen.
  - Seite „Application“: bei ungültigem Icon-Namen wird jeden Frame bis zu `uiIconCount()` (laut Kommentar ca. 2.000) Mal `strstr` ausgeführt.
  - `drawGraphicsPage` kopiert `EditorConfig` zweimal (`scratch`, `before`; ca. 280 Zeilen Struct, vier `std::string`-Member) und vergleicht per `same()`.
  - Seite „Collision Layers“: 16 Mal `kDefaults.layerName(i)` (`std::string`) plus Kopie `name` pro Frame.
- **Code-Auszug**:
```cpp
std::vector<const char*> names = { "(platform default)" };
for (const char* platform : { "macOS", "Windows", "Linux" })
    for (const char* b : HE::BackendRules::choicesFor(platform))
        if (std::find_if(names.begin(), names.end(), ...
```
- **Lösung**: Backend-Liste als `static const` einmal bauen (Funktions-lokales Static). Icon-Vorschläge nur neu berechnen, wenn sich `p.appIconName` ändert (kleiner Cache `std::string lastQuery; std::string matches;`). `projectRoot()` nur bei Projektwechsel vergleichen (oder `const std::string&` zurückgeben). EditorConfig-Kopie ist nur ca. 8 Allokationen groß und deshalb bewusst niedrig bewertet.
- **Erwarteter Effekt**: Kleine, dafür dauerhafte Einsparung (Schätzung < 0,1 ms/Frame auf diesen Seiten).
- **Risiko / Verifikation**: Gering. Backend-Liste ist plattformstatisch, aber `HE::BackendRules` könnte projektabhängig werden; das beim Static bedenken.

### EDT-1517 – Spline-Pick: Sample-Vektor und doppelte Matrixmultiplikation pro Frame
- **ID**: EDT-1517
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/SplineEdit.cpp:59-123 (vor allem :84-86, :93-97), :276-291 (`hover`); src/HE_Editor/SplineTool.cpp:57-69
- **Problem**: `hover()` ruft bei jeder Mausbewegung über dem Viewport `pick()` auf. `pick()` baut `HE::spline::Curve`, erzeugt per `curve.sample(16)` einen neuen `std::vector<glm::vec3>` (Spans × 16 + 1 Einträge) und multipliziert dann pro Sample zweimal (erst `model * v`, dann in `project()` `viewProj * v`). Dieselbe Kurve wird im selben Frame vom Overlay (Debug-Linien, nicht in diesem Slice) und von `worldLength()` im Panel nochmals gesampelt. `HE::worldMatrixOf(world, active)` wird pro Frame in `updateInViewport`, `hover`, `guides` und `worldLength` jeweils neu komponiert.
- **Code-Auszug**:
```cpp
const std::vector<glm::vec3> samples = curve.sample(kSamplesPerSpan);
...
const bool curOk = project(view, glm::vec3(model * glm::vec4(samples[i], 1.0f)), cur);
```
- **Lösung**: Einmal `const glm::mat4 mvp = view.viewProj * model;` bilden und pro Sample nur `mvp * vec4(s,1)` rechnen (identisches Ergebnis bis auf Float-Rundung, `project()` bekommt eine Überladung mit Clip-Koordinate). Sample-Puffer als wiederverwendeter Member der Tool-Klasse oder `thread_local`; `Curve::sample(int, std::vector<vec3>& out)` falls in HE_Scene ergänzbar. Weltmatrix einmal pro Frame im Tool cachen.
- **Erwarteter Effekt**: Bei Splines mit 50–200 Punkten ca. 800–3.200 Samples × 2 Mat-Vek-Produkte pro Frame; Halbierung dieser Arbeit plus eine Allokation weniger (Schätzung, nur bei Hover relevant, daher Niedrig).
- **Risiko / Verifikation**: Float-Rundungsdifferenzen können einzelne Pick-Grenzfälle um Subpixel verschieben; bestehende Pick-Tests in `tests/` (Spline-Edit) prüfen mit Toleranz.

### EDT-1518 – SplineTool: Klick-Zustand überlebt `reset()` (veralteter „armed“-Klick)
- **ID**: EDT-1518
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/SplineTool.cpp:111-119, :126-131, :168-196
- **Problem**: Der Klick-Zustand (`s_pressArmed`, `s_dragged`, `s_pressPos`) liegt als Funktions-Statics in `updateInViewport`, während alles übrige Tool-Gedächtnis file-static ist und von `reset()` gelöscht wird. Verlässt man den Modus oder bricht ein Frame-Gap die Tool-Kette (Zeile 126-131), während `s_pressArmed == true` ist, löscht `reset()` ihn nicht. Beim nächsten Eintritt in den Spline-Modus mit nicht gedrückter Maustaste wird der alte Druck als „Release“ gewertet und ein Klick an der veralteten `s_pressPos` ausgeführt (Punkt hinzufügen). Das ist ein Randfall, aber zugleich ein Beleg für unnötig verteilten Zustand.
- **Code-Auszug**:
```cpp
static bool   s_pressArmed = false;
static bool   s_dragged    = false;
static ImVec2 s_pressPos{};
...
if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { s_pressArmed = false; ... s_tool.click(...)
```
- **Lösung**: Die drei Werte in `SplineEdit::Tool` (oder eine kleine `ClickState`-Struct neben `s_tool`) verschieben und in `Tool::reset()` löschen; die ImGui-freie Klasse ist ohnehin testbar.
- **Erwarteter Effekt**: Einheitlicher Zustand, Randfall beseitigt.
- **Risiko / Verifikation**: Gering. Test: Maus drücken, Modus wechseln, loslassen, Modus wieder aktivieren, es darf kein Punkt entstehen.

### EDT-1519 – ReportIssueDialog: URL-Länge per vollständigem Encode, Worker-Ausgänge dupliziert
- **ID**: EDT-1519
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Editor/ReportIssueDialog.cpp:159-211 (`fits`, Binärsuche), :304-326 (Key bauen), :844, :531-559 (Worker-Ausgänge)
- **Problem**: (a) `fits()` ruft `urlEncode(body)` auf und nutzt nur `.size()`; jede Probe der Binärsuche baut dazu den Body neu und encodiert ihn komplett (bis ca. 9 Proben × bis zu mehreren zehn KB). Das Ergebnis ist rein zählbar. (b) Solange der Preview-Baum offen ist, werden pro Frame der Schlüsselstring `key` (alle Felder konkateniert) und eine Kopie des Body `std::string body = p.body` angelegt. (c) `currentReport()` kopiert beim Neuberechnen die bis zu 400 Logzeilen (`r.logLines = chosenLogLines()`). (d) Im Submit-Worker wiederholt sich nach jedem Fehlerausgang `s_workerRunning.store(false, ...)` viermal.
- **Code-Auszug**:
```cpp
const auto fits = [&](const std::string& body)
{
    return prefix.size() + urlEncode(body).size() <= maxUrlLength;
};
```
- **Lösung**: `urlEncodedSize(std::string_view)` (zählt 1 oder 3 pro Byte) statt `urlEncode(...).size()`; Key über einen Hash/Revisionszähler der Textfelder statt String-Konkatenation; Preview-Body als `ImGuiInputTextFlags_ReadOnly` auf einem `char*`-Puffer mit `InputTextMultiline(label, buf, size, ...)` oder per `TextUnformatted` in einem Child ohne Kopie; im Worker ein kleiner RAII-Guard (`struct RunningGuard { ~RunningGuard(){ s_workerRunning.store(false, std::memory_order_release); } };`), der alle Ausgänge abdeckt.
- **Erwarteter Effekt**: Weniger Allokationen beim Tippen mit offener Vorschau und weniger Wiederholung im Worker. Kein Hot Path, daher Niedrig.
- **Risiko / Verifikation**: Gering. Die reine Logik `buildBody/buildIssueUrl` ist laut Header unit-getestet; ein Test, der `urlEncodedSize(x) == urlEncode(x).size()` für Multibyte-/Sonderzeichen prüft, genügt.

### EDT-1520 – SelfUpdateDialog: Session und Help-Scope vor dem „geschlossen“-Return, Acceptance-Treiber im Produktionspfad
- **ID**: EDT-1520
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Editor/SelfUpdateDialog.cpp:153-171, :186-188, :56-76, :279-302
- **Problem**: `Draw()` erzeugt (`session()` legt per `make_unique` die `Session` samt `productBackend()` an) und baut den `Help::Scope` in jedem Frame, bevor geprüft wird, ob der Dialog überhaupt offen ist (Zeile 186). Die Session wird also beim ersten Frame angelegt, auch wenn der Nutzer nie „Check for Updates“ öffnet. Solange der Dialog offen ist, wird `su.snapshot()` pro Frame als Kopie mit mehreren Strings gezogen (und beim Öffnen zweimal). Außerdem sind der E2E-Akzeptanz-Treiber (`E2E`-Struct, Env-Variablen, `|| driving` in drei Button-Bedingungen) direkt mit der UI-Logik verwoben.
- **Code-Auszug**:
```cpp
HE::Ed::Help::Scope helpScope("Self-Update");
EditorSelfUpdate::Session& su = session();
...
if (!s_isOpen) return;
```
- **Lösung**: `session()` und den Scope erst nach `if (!s_isOpen && !s_openRequested && !drive.active) return;` anlegen bzw. nutzen. Den E2E-Treiber in eine kleine Hilfsklasse (eigene `.cpp` oder `#ifdef HE_SELFUPDATE_E2E`) mit `bool pressesDownload()`, `bool pressesRestart()` auslagern, damit die Draw-Funktion nicht an drei Stellen `|| driving` trägt.
- **Erwarteter Effekt**: Kein Session-Aufbau für Nutzer, die den Dialog nie öffnen (Kosten von `productBackend()` nicht gelesen, daher Niedrig); schlankere Draw-Funktion.
- **Risiko / Verifikation**: Gering, solange die E2E-Skripte (`scripts/selfupdate_editor_e2e.py`) weiter dieselben Pfade erreichen; E2E-Lauf vor dem Merge.

### EDT-1521 – Log-Aufrufe mit vorgebautem String statt Format-Argumenten
- **ID**: EDT-1521
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Editor/SceneAutosave.cpp:127, :140, :148, :162, :170; src/HE_Editor/SkeletalMeshEditorPanel.cpp:94, :96, :158
- **Problem**: Die Aufrufe lauten `HE_LOG_WARN(Editor, "%s", ("text " + path + ...).c_str())`: Der String wird immer gebaut, auch wenn der Level gefiltert wird (Zeile 170 ist `HE_LOG_DEBUG`, hier also vermutlich unnötig). Das Makro nimmt printf-Argumente (siehe `HE_LOG_WARN(Editor, "Report Issue: %s", why.c_str())` in ReportIssueDialog.cpp:536).
- **Code-Auszug**:
```cpp
HE_LOG_DEBUG(Editor, "%s", ("SceneAutosave: snapshot written to " + live.string()).c_str());
```
- **Lösung**: `HE_LOG_DEBUG(Editor, "SceneAutosave: snapshot written to %s", live.string().c_str());` (bzw. `live.c_str()` mit `native()` unter POSIX; unter Windows `string()` wegen `wchar_t`).
- **Erwarteter Effekt**: Winzig (Pfade, selten); vor allem konsistenter Stil.
- **Risiko / Verifikation**: Keines.

### EDT-1522 – Schwere Header, Kommentare und Reste
- **ID**: EDT-1522
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: siehe Liste
- **Problem**: Sammelposten ohne Verhaltensänderung:
  - `EditorApplication.h` wird für den Typ `AppContext` in kleinen Dialogen inkludiert (SceneRecoveryDialog.cpp:2, SelfUpdateDialog.cpp:4, ReportIssueDialog.cpp:10, SceneDiskWatch.cpp:3, ScriptEditorPanel.cpp:3, SkeletalMeshEditorPanel.cpp:6, SecondaryViewportPanel.cpp:2); Header ziehen `imgui.h`, `AssetAutosave.h` und `EditorUI.h` nach (ScriptEditorPanel.h:4-5, SequencerPanel.h:5, SkeletalMeshEditorPanel.h:2-6, SecondaryViewportPanel.h ist vorbildlich). Der Compile-Zeit-Effekt wurde nicht gemessen.
  - Veraltete Kommentare: SceneAutosave.h:21-22 („the recovery dialog is the next step on the board“) und :54-55 („the future dialog“), obwohl SceneRecoveryDialog existiert; dort und in SceneAutosave.cpp:74-75 steht auch „.hproject“ statt `.heproj`. ScriptEditorPanel.cpp:77-83 trägt zwei gestapelte Kommentarblöcke zur selben Funktion. ReportIssueDialog.cpp:970-972 beschreibt einen entfernten Tooltip.
  - Toter Code: SourceControlPanel.cpp:831-835 (leerer `if`-Zweig mit Kommentar), `drawRow(..., const std::string& root)` mit `(void)root;` (:575, Aufruf :925), zwei fast identische `cell`-Lambdas (:505-516, :523-534), dreimal das Muster `std::optional<WrapText> wrap; wrap.emplace(); ... wrap.reset();` (:890-891, :966-967, :1101-1102), dessen Zweck ein innerer `{ }`-Block mit `WrapText wrap;` ebenso erfüllt. `beginModalSizing()` wird pro Frame für vier geschlossene Popups aufgerufen (:1718, :1801, :1850, :1885).
  - Doppelte/ungenutzte Includes: `<cstdio>` zweimal in ReportIssueDialog.cpp:6 und :22, `<cstdio>` in SceneRecoveryDialog.cpp:6 ungenutzt. `formatValue` hat zwei identische Zweige für Rotation und FOV (SequencerTimeline.cpp:62-69). SecondaryViewportPanel.cpp:73 und :151 enthalten dasselbe `restoreView(...)`-Triple.
- **Code-Auszug**:
```cpp
if (ImGui::InputTextWithHint("##scsearch", "Filter changes…", s_search, sizeof(s_search)))
{
    // model rebuilds next draw: the cache key includes the text
}
```
- **Lösung**: `AppContext` in einen leichten Forward-Header (`AppContextFwd.h`) oder `struct AppContext;` nutzen, wo nur Referenzen verwendet werden (Implementierungen brauchen weiterhin die Definition; Gewinn vor allem in Headern). Reste entfernen, Kommentare aktualisieren.
- **Erwarteter Effekt**: Lesbarkeit; Compile-Zeit (Schätzung, messen).
- **Risiko / Verifikation**: Keines bis gering. `editor_help_audit.py --check` unberührt lassen.

### EDT-1523 – `recoveryDirForProject` erkennt Ordner über `has_extension()`
- **ID**: EDT-1523
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/SceneAutosave.cpp:70-79
- **Problem**: Die Entscheidung „Datei oder Ordner“ lautet `fs::is_regular_file(p, ec) || p.has_extension()`. Wird der Projektordner selbst übergeben und heißt er z. B. `My.Game` oder `Project.v2`, wird `parent_path()` genommen und die Recovery-Daten landen im falschen Verzeichnis (neben dem Projektordner). Der zweite Operand ist bei existierender Datei redundant und bei Ordnern mit Punkt falsch.
- **Code-Auszug**:
```cpp
if (fs::is_regular_file(p, ec) || p.has_extension()) p = p.parent_path();
```
- **Lösung**: Existierende Pfade per `is_regular_file`/`is_directory` entscheiden; nur wenn der Pfad nicht existiert, als Fallback die Endung `.heproj` prüfen (`p.extension() == ".heproj"`).
- **Erwarteter Effekt**: Korrekte Recovery-Pfade für Ordner mit Punkt im Namen.
- **Risiko / Verifikation**: Gering. Es gibt Tests zum Autosave laut Header; Fall „Ordnername mit Punkt“ und „nicht existierender `.heproj`-Pfad“ ergänzen.

## Gelesen

Alle 32 Dateien des Slice wurden vollständig gelesen (ProjectSettingsPanel.cpp/.h, QuickSettingsFavorites.h, ReportIssueDialog.cpp/.h, SceneAutosave.cpp/.h, SceneDiskWatch.cpp/.h, SceneFileStamp.h, SceneRecoveryDialog.cpp/.h, ScriptEditorPanel.cpp/.h, SecondaryViewportPanel.cpp/.h, SelfUpdateDialog.cpp/.h, SequencerPanel.cpp/.h, SequencerTimeline.cpp/.h, ShortcutsPage.cpp/.h, SkeletalMeshEditorPanel.cpp/.h, SourceControlPanel.cpp/.h, SplineEdit.cpp/.h, SplineTool.cpp/.h); zur Verifikation wurden zusätzlich gezielt Ausschnitte aus GitCli.cpp, WaterField.cpp, WaterLake.cpp, EditorConfig.h, EditorGraphics.cpp und per Grep EditorPanelState.h gelesen.

## Editor Slice 16 – StaticMeshEditorPanel … TerrainSync

## Zusammenfassung

Der Slice (24 Dateien, ca. 9.200 Zeilen: StaticMeshEditorPanel, StreamingDebugView, StructuralSync, TaskBoard*, Team*, TerrainSync) ist insgesamt sauber geschrieben: Datenmodell (`TaskBoard`) ohne ImGui, Row-Clipping in den Task-Karten, Throttling in `TerrainSync`, klare Trennung Controller/View. Die Kosten liegen fast ausschließlich in **Pro-Frame-Arbeit, die einen Zustand neu berechnet statt ihn zu cachen**: der Strukturdiff für Collab (O(Entities) mit Heap pro Frame), der Footer-Task-Zähler (sortiert pro Frame alle offenen Tasks und ruft `getenv`), die Team-Footer-Cluster (3× `onlineOutsideSession()` pro Frame, pro-Zelle-Rechtecke für Avatare), `TeamSyncController::update` (5 String-Konkatenationen pro Frame) und die Orphan-Prüfung der Task-Board-Links, die pro Entity-Link den ganzen Weltinhalt per `snprintf` durchsucht. Zusätzlich Wartbarkeit: duplizierte Panelliste, mehrfach vorhandener Case-insensitive-Namensvergleich, Test-Instrumentierung im Produktiv-Drawpfad. `TaskBoardTeam` ist im Editor noch nicht verdrahtet (laut `docs/taskboard-team-interop-2026-10-15.md` geplant), die dortigen Pro-Frame-Kosten sind daher nur prospektiv.

Funde: 2 Hoch, 11 Mittel, 7 Niedrig. Alle Laufzeitangaben sind Schätzungen aus dem Code, nicht gemessen.

---

### EDT-1601 – Collab-Strukturdiff baut jeden Frame ein Hash-Set aller Entities und scannt es dreimal
- **ID**: EDT-1601
- **Kategorie**: D (auch C)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/StructuralSync.h:46-57, 94-99, 104-118`; Aufrufer `EditorApplication.cpp:4976` (`if (m_collab.inSession()) syncStructuralChanges();`), `EditorApplication.cpp:11416-11420, 11529-11532, 11540-11543, 11595`
- **Problem**: Solange eine Collab-Session läuft, wird pro Frame (a) `std::unordered_set<Entity> current` neu befüllt (ein Heap-Knoten je Entity), (b) `newSubtreeRoots` macht je Entity zwei `count()`-Lookups plus `try_get<HierarchyComponent>`, (c) `recordMissingParents` macht je bekannter Entity `valid` + `parents.count`, (d) `reparentedSince` je bekannter Entity `valid` + `find` + `try_get`. Das sind ~6 Hash-Operationen und N Allokationen pro Frame, obwohl sich in 99 % der Frames weder Entity-Menge noch Hierarchie ändern. Die Hierarchie hat bereits ein Dirty-Signal (`m_editorWorld->markHierarchyDirty()` wird in `EditorApplication.cpp` beim Reparent gesetzt).
- **Code-Auszug**:
```cpp
// EditorApplication.cpp:11416
std::unordered_set<Entity> current;
reg.view<entt::entity>().each([&](auto e) { if (!isLocalOnly(e)) current.insert(e); });
// StructuralSync.h:54
for (const StructEntity e : current)
    if (!known.count(e) && !isNew(structParentOf(reg, e))) roots.push_back(e);
```
- **Lösung**: (1) Änderungen über entt-Signale erfassen (`on_construct<EntityIdComponent>` / `on_destroy` sowie `on_update<HierarchyComponent>`) in einen kleinen „pending“-Satz und den Diff nur ausführen, wenn der Satz nicht leer ist oder `hierarchyRevision` sich geändert hat. (2) Als Zwischenschritt: `current` als wiederverwendeten Member-`std::vector<Entity>` (sortiert, `reserve`) bzw. `entt::sparse_set` halten statt pro Frame zu allokieren, und `reparentedSince` nur laufen lassen, wenn die Hierarchie-Revision sich bewegt hat. Die Regeln in `StructuralSync.h` (unabhängig von Reihenfolge) bleiben unverändert und weiter testbar.
- **Erwarteter Effekt**: Bei 10k–100k Entities in einer Session Größenordnung 1–5 ms/Frame weniger UI-Thread-Zeit sowie wegfallender Allokations-Churn (Schätzung, messen).
- **Risiko / Verifikation**: Signale dürfen kein Ereignis verlieren (Undo, Prefab-Instanziierung, Remote-Create ohne Signal, `isTerrainGenerated` ändert sich ohne Entity-Änderung). Absichern: bestehende Tests für `StructuralSync.h` beibehalten; neuer Test, der eine Zufallsfolge aus create/destroy/reparent/undo einmal mit dem Voll-Diff und einmal mit dem Signal-Pfad fährt und identische Nachrichtenlisten fordert. Vorher mit Tracy/Profiler-Scope in `syncStructuralChanges` messen.

---

### EDT-1602 – Orphan-Prüfung der Task-Links durchsucht pro Entity-Link die gesamte Welt mit `snprintf`
- **ID**: EDT-1602
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TaskBoardPanel.cpp:518-557` (Entity-Zweig 540-550); Verursacher `src/HE_Editor/EditorCommands.cpp:419-428` (`entityByUuid`), `TaskBoardPanel.cpp:596-610, 575-580`
- **Problem**: `refreshOrphans` läuft bei offenem Task-Board jede Sekunde (`s_orphansNext = now + 1s`) und ruft pro Entity-Link `entityByUuid(world, uuidToHex(l.id))`. `entityByUuid` iteriert ALLE Entities mit `EntityIdComponent` und ruft je Entity `uuidOf` (= `snprintf` + `std::string`-Allokation) und vergleicht Strings; es bricht nicht ab (`found` wird nur geprüft, die Iteration läuft weiter). Kosten = Links × Entities × (snprintf + malloc) jede Sekunde auf dem UI-Thread – ein regelmäßiger Hitch bei großen Szenen. Dasselbe Muster steckt in `jumpTo` und `resolvePendingSelect`.
- **Code-Auszug**:
```cpp
// EditorCommands.cpp:421-427
reg.view<const EntityIdComponent>().each([&](auto e, const EntityIdComponent&) {
    if (found != entt::null) return;          // iteriert trotzdem weiter
    if (uuidOf(world, e) == uuid) found = e;  // snprintf + malloc je Entity
});
```
- **Lösung**: (1) `entityByUuid` auf UUID-Vergleich ohne String umstellen: Overload `entityByUuid(world, const UUID&)`, Vergleich `c.id == id` direkt auf der Komponente, frühes Beenden per klassischer `for`-Schleife über `view.each()`-Iterator. Der String-Overload parst einmal via `uuidFromHex` und delegiert. (2) In `refreshOrphans` pro Durchlauf alle Entity-Links sammeln und in EINEM Weltdurchlauf in ein `unordered_set<UUID>` der Ziel-IDs prüfen (O(Entities + Links) statt Produkt). (3) Optional: Prüfintervall bei großen Welten auf 2–5 s anheben bzw. nur bei geänderter Szenenrevision.
- **Erwarteter Effekt**: Von O(L·E) mit Allokation auf O(E) ohne Allokation; bei 20 Links und 50k Entities von ~1 M `snprintf` pro Sekunde auf ~50k Vergleiche (Schätzung, messen).
- **Risiko / Verifikation**: Verhalten identisch, solange UUID-Vergleich äquivalent zum Hex-Stringvergleich ist (Hex ist lowercase 32-stellig, `uuidFromHex` akzeptiert auch Großbuchstaben – dort war der alte String-Vergleich strenger; Aufrufer übergeben nur eigene `uuidToHex`-Werte). `entityByUuid` ist von MCP-Tools mitbenutzt: `tests/test_mcp_tools_taskboard.cpp` plus Tests rund um `EditorCommands` laufen lassen.

---

### EDT-1603 – Footer-Aufgabenzähler sortiert pro Frame alle offenen Tasks und liest dreimal `getenv`
- **ID**: EDT-1603
- **Kategorie**: D (auch C)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TaskBoardFooter.cpp:56-94, 320-327`; `TaskBoardPanel.cpp:190-217, 294-300`
- **Problem**: `DrawFooter` läuft in jedem Editor-Frame (Footer ist immer sichtbar, `EditorUI.cpp:3428`). Pro Frame: `board()` (kopiert `projectRoot()` als `std::string`, vergleicht), `author()` (Wert-Rückgabe; ist `s_author` leer – der Normalfall ohne Team – folgen bis zu drei `getenv`-Aufrufe, je ein linearer Scan der Umgebung), `summarize` → `openCount`, `openCountFor`, und `rankedOpen`: `std::vector<const Task*>` + `std::stable_sort` (zusätzlicher Temp-Puffer) + `std::vector<UUID>`, nur um `ids.front()` zu verwenden, danach `board.find(front)` (nochmals linear) und Titelkopie; anschließend `layoutFor` mit `snprintf`, `CalcTextSize` und `"Next: " + title`. `DrawFooter` ruft `author()` einmal pro Frame, `summarize` verwendet es; bei offener Liste doppelt.
- **Code-Auszug**:
```cpp
const Summary s = summarize(*b, TaskBoardPanel::author());   // :325
// summarize:
const std::vector<HE::UUID> ids = rankedOpen(board, me);
if (!ids.empty()) if (const Task* t = board.find(ids.front())) ...
```
- **Lösung**: (1) In `summarize` den besten Task per `std::min_element` mit demselben Komparator finden (liefert bei Gleichstand das erste Element, also dasselbe wie das Front-Element des `stable_sort`) – O(n) ohne Allokation, kein zweites `find`. `rankedOpen` bleibt für `openList` unverändert. (2) `author()` das Ergebnis des Umgebungsvariablen-Lookups einmal in `s_author`-Fallback cachen (statisch, Berechnung beim ersten Aufruf). (3) Das Summary samt Layout-Strings cachen, geschlüsselt auf `(s_store.generation, board-Änderungszähler, author, maxWidth, FontSize)`; ein Änderungszähler lässt sich in `edit()`/`loadFromDisk()` hochzählen.
- **Erwarteter Effekt**: Footer-Kosten fallen von O(n log n) + ~5–8 Allokationen + 3 `getenv` auf O(1) im Normalfall (Schätzung; bei < 50 Tasks absolut klein, aber jeden Frame in jedem Projekt).
- **Risiko / Verifikation**: Reihenfolge muss exakt der von `rankedOpen` entsprechen: Test, der `min_element`-Treffer mit `rankedOpen(...).front()` auf zufälligen Boards vergleicht. Vorhandene Footer-Tests (`tests/test_*taskboard*`) laufen lassen.

---

### EDT-1604 – Task-Liste/Spalten: bis zu 12 Vollscans der Tasks pro Frame, `matches()` allokiert bei aktivem Filter
- **ID**: EDT-1604
- **Kategorie**: D (auch C)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TaskBoardPanel.cpp:436-450, 459-463, 1050-1074, 1077-1111, 965`
- **Problem**: Das Row-Clipping spart nur das Zeichnen der Karte (`drawCard` → `IsRectVisible`), nicht die Auswahl: `drawList` zählt je Spalte `columnOf && matches` über alle Tasks (4 Scans), dann nochmals je Spalte zum Zeichnen (4 Scans); `drawColumns` scannt 4×; `drawToolbar` zählt einmal. `matches()` ruft bei nicht-leerem Filter `lower(s)` (Kopie des Strings, Allokation) für Titel, Beschreibung und jedes Link-Label und jeden Assignee – pro Task, pro Scan, pro Frame. Bei 500 Tasks mit Filter sind das rund 8 × 500 × (2–6) Allokationen pro Frame.
- **Code-Auszug**:
```cpp
auto has = [&](const std::string& s) { return lower(s).find(lowerFilter) != std::string::npos; };
...
for (int c = 0; c < 4 && !b.tasks.empty(); ++c) {
    int n = 0;
    for (const Task& t : b.tasks) n += columnOf(t) == c && matches(t, f);
    ...
    for (Task& t : b.tasks) if (columnOf(t) == c && matches(t, f)) drawCard(ctx, b, t, w);
}
```
- **Lösung**: Einmal pro Frame (oder nur bei Änderung von Filter/`generation`/Board-Revision) vier Indexlisten `std::vector<int> lane[4]` aufbauen (Member-Vektoren, wiederverwendet) – Zählen und Zeichnen nutzen dieselben Listen. `matches` ohne Kopie: Teilstring-Suche case-insensitive mit `std::search` und einem `tolower`-Prädikat auf `string_view`, dann entfällt die Allokation; der Filter wird nur einmal kleingeschrieben. Alternativ pro Task einen gecachten Suchstring (`title\0description\0labels`) halten.
- **Erwarteter Effekt**: Aus 8–12 Scans mit Allokationen werden 1 Scan ohne Allokation; spürbar erst bei vielen hundert Tasks oder langen Beschreibungen (Schätzung).
- **Risiko / Verifikation**: Reihenfolge innerhalb der Lane bleibt Dateireihenfolge. Die Listen dürfen nicht über `post()`-Edits hinweg dangling werden (Edits laufen erst nach `End()`, Liste daher pro Frame neu bauen). UI-Test `test_*taskboard*_ui` und `lastDrawStats()` beibehalten.

---

### EDT-1605 – Details-Pane ruft pro Frame `fs::relative` (Dateisystem-Syscalls) nur für ein `BeginDisabled`
- **ID**: EDT-1605
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TaskBoardPanel.cpp:1248-1249` (Funktion `projectRelative` 487-496)
- **Problem**: Im Details-Bereich eines ausgewählten Tasks wird jeden Frame `projectRelative(s_store.root, ctx.currentScenePath)` berechnet; es nutzt `fs::relative`, das intern `weakly_canonical` aufruft (mehrere `stat`/`readlink`-Syscalls je Pfadkomponente, auf Windows zusätzlich Pfad-Normalisierung). Das Ergebnis dient nur dazu, den „Link Scene“-Button zu (de)aktivieren und beim Klick den Pfad zu liefern. Blockierende I/O im Draw-Pfad, auf Netzlaufwerken/Windows-Defender-Scans teuer.
- **Code-Auszug**:
```cpp
const std::string scene = projectRelative(s_store.root, ctx.currentScenePath);
ImGui::BeginDisabled(scene.empty());
```
- **Lösung**: Das Ergebnis pro `(root, currentScenePath)` cachen (statische Strings `s_lastSceneAbs`/`s_lastSceneRel`; neu berechnen, wenn sich `ctx.currentScenePath` ändert) oder nur bei Klick berechnen und für das Disable `!ctx.currentScenePath.empty()` verwenden (die „außerhalb des Projekts“-Ablehnung dann beim Klick mit Hinweis).
- **Erwarteter Effekt**: Entfällt dauerhafte Syscall-Last pro Frame, solange ein Task ausgewählt ist (Schätzung: je nach FS 20–200 µs/Frame; messen).
- **Risiko / Verifikation**: Wenn nur das Disable vereinfacht wird, ändert sich das Verhalten für Szenen außerhalb des Projekts (Button wäre aktiv). Cache-Variante ist verhaltensgleich. Test: Link Scene bei Szene innerhalb/außerhalb des Projekts.

---

### EDT-1606 – Chips: pro Karte und Frame Pfadauflösung, Thumbnail-Lookup, Typ-Sniff und mehrfache String-Bauten
- **ID**: EDT-1606
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TaskBoardPanel.cpp:697-753, 759-809, 828-942, 1196-1207`
- **Problem**: Für jeden sichtbaren Chip, jeden Frame: `chipText` (String-Kopie, `uuidToHex(...).substr(0,8)`, `fs::path(...).stem()`), in `chipVisual` `assetAbsPath` (= `pathForId` + `resolveAbsolutePath`, zwei String-Allokationen), `AssetThumbnailCache::get(abs)` (Hash über den Pfadstring), `EditorAssetTypeCache::assetTypeOf(abs)` (weiterer Pfad-Hash) bzw. für Szenen `sceneAbs` mit `fs::path::lexically_normal()`. In `drawCard` zusätzlich Kopie des Titels (`const std::string title = ...`, 886) und `std::to_string` für die Orphan-Zahl. Im Details-Bereich wird `chipText` je Link doppelt berechnet (1199 und in `chip()` 761).
- **Code-Auszug**:
```cpp
const std::string abs = l.orphaned ? std::string{} : assetAbsPath(ctx, l.id);
...
if (void* t = AssetThumbnailCache::get(abs)) { ... }
switch (EditorAssetTypeCache::assetTypeOf(abs)) { ...
```
- **Lösung**: `ChipVisual` (Textur, Glyph, Tint) und `chipText` pro Link in einem kleinen Cache halten (`unordered_map<UUID asset/scene-key, CachedChip>`), invalidiert über `s_store.generation`, Thumbnail-Cache-Revision bzw. alle ~0,5 s – analog zu `refreshOrphans`. Den Titel für `RenderTextEllipsis` nicht kopieren (`const char* p = t.title.empty() ? "(untitled)" : t.title.c_str()`). `chipText` in `drawDetails` einmal berechnen und an `chip()` durchreichen.
- **Erwarteter Effekt**: Pro sichtbarem Chip ~4–6 Allokationen und 2–3 Hash-Lookups weniger je Frame (Schätzung; sichtbare Karten sind wenige, daher Mittel statt Hoch).
- **Risiko / Verifikation**: Thumbnails können nachgeladen werden; Cache darf nur „kein Thumbnail“ nicht dauerhaft festschreiben (Ablaufzeit oder `AssetThumbnailCache`-Revision). UI-Test mit `chipRect()`.

---

### EDT-1607 – Team-Footer: `onlineOutsideSession()` wird 3–4× pro Frame berechnet, Namen werden pro Frame kopiert
- **ID**: EDT-1607
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TeamView.cpp:353-393, 396-399, 401-407, 475, 446, 454`; `TeamController.cpp:808-829`; Aufrufer `EditorUI.cpp:3502`
- **Problem**: Bei angemeldetem Team (Footer ist immer da) wird `team.onlineOutsideSession()` pro Frame in `footerOnlineWidth`, `stepJoins` (jeder der drei Footer-Einstiege ruft es, aber nur einmal pro Frame aktiv), `drawFooterOnline` und – bei geöffnetem Menü – `drawOnlineMenuWindow` aufgerufen. Jeder Aufruf baut `std::vector<std::string>` aller Session-Teilnehmernamen (Kopien) und einen `vector<const TeamMember*>`; `onlineOutside` ist O(Mitglieder × Teilnehmer) mit `sameName` (tolower pro Zeichen). `drawFooterOnline` kopiert zusätzlich pro Frame `m.name` pro Gesicht in `s_stackNow` (`std::string` in `StackFace`), `stepJoins` ist O(M²) (`memberNamed` + `inStack` pro Gesicht).
- **Code-Auszug**:
```cpp
float footerOnlineWidth(const TeamController& team)
{ return onlineLayout(team.onlineOutsideSession().size()).width; }   // :396
...
const std::vector<const HE::Net::TeamMember*> online = team.onlineOutsideSession();  // :405
```
- **Lösung**: Pro Frame einmal berechnen und cachen: `struct OnlineCache { int frame; std::vector<const TeamMember*> list; }` in `TeamView.cpp`, befüllt von `stepJoins` (läuft ohnehin zuerst) bzw. einer Funktion `onlineThisFrame(team)`, die über `ImGui::GetFrameCount()` guarded; `footerOnlineWidth`, `drawFooterOnline`, Menü lesen daraus. Ohne Session (`m_collab.inSession()` false) die Namensliste gar nicht bauen (steht schon so im Code, aber `onlineOutside` bekommt trotzdem einen leeren Vektor). `StackFace` auf `const TeamMember*` bzw. Index statt `std::string` umstellen oder Namen per `string_view` in den Mitgliedern referenzieren (Zeiger gelten laut Header nur bis zum nächsten `update()`, `s_stackLast` überlebt einen Frame – daher `team.revision()` mitführen oder Name nur bei Gesichtswechsel kopieren).
- **Erwarteter Effekt**: Von ~3 Listenbauten + N Stringkopien auf 1 Listenbau pro Frame; absolut klein bei < 10 Mitgliedern, aber dauerhaft (Schätzung).
- **Risiko / Verifikation**: `tests/test_team_ui.cpp` (headless, Probe-Seam) deckt Layout und Slide ab; Reihenfolge der Liste muss gleich bleiben.

---

### EDT-1608 – Avatare werden pro Frame als bis zu 1.024 Einzelrechtecke gezeichnet
- **ID**: EDT-1608
- **Kategorie**: D (GPU/CPU-Vertices)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TeamView.cpp:684-730` (Schleife 699-711); Aufrufer 215, 437, 523, 634, 951, 1219
- **Problem**: Jedes Avatar-Gesicht mit Bild erzeugt `cells × cells` `AddRectFilled` (bei 19 px Footer-Höhe ~12×12 = 144, im Profil ~48 px = 32×32 = 1.024), jeweils mit eigener Quellpixel-Berechnung und zwei `std::floor`-Paaren; im Footer pro Frame bis zu 5 Gesichter (eigenes + 4 Online) plus Slide, im Team-Fenster ein Gesicht pro Mitglied. Der Kommentar nennt als Grund „this file has no renderer“. Das Ergebnis ändert sich nur, wenn sich das Avatarbild (oder die Größe) ändert.
- **Code-Auszug**:
```cpp
for (int y = 0; y < cells; ++y)
    for (int x = 0; x < cells; ++x) {
        ... dl->AddRectFilled(a, b, IM_COL32(px[0], px[1], px[2], px[3]));
```
- **Lösung**: Pro `(Mitglied, avatarId/Revision, cells)` die Zellfarben einmal in einen `std::vector<ImU32>` vorberechnen (Cache `unordered_map<key, vector<ImU32>>`) – spart Sampling und Floor-Rechnung. Besser: horizontale Läufe gleicher Farbe zu einem Rechteck zusammenfassen (Run-Length je Zeile; Avatare haben große Flächen), oder – wo ein Renderer verfügbar ist (Editor, nicht Test) – das 64×64-RGBA einmal in eine ImGui-Textur laden (`ctx`-Renderer, vgl. `AssetThumbnailCache`) und mit `AddImageRounded` zeichnen.
- **Erwarteter Effekt**: Zeichenvertices im Footer von ~4k auf wenige Dutzend (Schätzung; bei mehreren Online-Mitgliedern mit Bild ca. 0,1–0,3 ms CPU/Frame, messen).
- **Risiko / Verifikation**: Optik muss pixelgleich bleiben (kein Interpolationsunterschied) – daher Cache/Run-Length statt Textur zuerst; Screenshot-Vergleich über `HE_UI_DUMP_DIR` in `test_team_ui`.

---

### EDT-1609 – `TeamSyncController::update` baut pro Frame einen langen Schlüsselstring und faltet Strings mehrfach
- **ID**: EDT-1609
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TeamSyncController.cpp:14-18, 96-132`; Aufrufer `EditorApplication.cpp:4915-4925`
- **Problem**: In Projekten im Team-Server-Modus läuft `update` jeden Frame: `folded(m_team.team())` und `folded(project.team)` (zwei String-Kopien), danach `key = root.generic_string() + "\n" + folded(team) + "\n" + folded(project) + "\n" + folded(member) + "\n" + host + ":" + to_string(port)` (~8 Allokationen), nur um `key != m_key` zu prüfen. Der Aufrufer baut zusätzlich pro Frame ein `TeamSyncController::Project` (`projectRoot()` als `std::filesystem::path`-Kopie, drei Strings).
- **Code-Auszug**:
```cpp
if (!haveSignIn || folded(m_team.team()) != folded(project.team)) { ... }
const std::string key = project.root.generic_string() + "\n" + folded(project.team) + ...
if (key != m_key || !m_sync.running()) { m_key = key; restart(project); }
```
- **Lösung**: Felderweise vergleichen statt Schlüssel bauen: `m_project.root == project.root && sameName(team) && sameName(project) && sameName(member) && host/port gleich` mit einem allokationsfreien Case-insensitive-Vergleich (`std::equal` + `tolower`, derselbe wie `sameName` in `TeamController.cpp:52`). Alternativ den Schlüssel nur neu bauen, wenn `m_team.revision()` bzw. Projekteinstellungsrevision sich ändert. Den `Project`-Aufbau im Aufrufer ebenfalls auf wenn-geändert umstellen.
- **Erwarteter Effekt**: ~10 Heap-Allokationen/Frame weniger in Teamprojekten (Schätzung; klein, aber trivial zu beheben).
- **Risiko / Verifikation**: Gleichheitssemantik des Schlüssels exakt nachbilden (Root case-sensitiv, Namen case-insensitiv, Host/Port exakt); `tests/test_team_sync_ui.cpp` und die TeamSync-Tests.

---

### EDT-1610 – `TeamSyncView::draw` kopiert pro Frame Status, Ergebnisse, Verlauf und „Lost“-Liste und zeichnet sie ungeclippt
- **ID**: EDT-1610
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TeamSyncView.cpp:58, 61, 107, 123, 143-165`; API `HE_SourceControl/include/SourceControl/TeamSyncService.h:183-189`
- **Problem**: `status()`, `lastResults()`, `history()` und `lost()` geben Wert-Kopien zurück (Mutex + tiefe Kopie von Vektoren mit Strings und verschachtelten `merges`-Vektoren) und werden bei sichtbarem Source-Control-Fenster jeden Frame aufgerufen. `history()` kann lang werden (Server-Verlauf), jede Revision erzeugt einen `TreeNodeEx` samt `snprintf`, `agoText` (mit `system_clock::now()` + `to_string`) – ohne Clipper, obwohl die Zeilenhöhe konstant ist. `stateLine()` baut ebenfalls pro Frame mehrere Strings.
- **Code-Auszug**:
```cpp
const auto history = svc.history();            // :143, Kopie pro Frame
for (const auto& v : history) { ... agoText(v.time) ... TreeNodeEx(label, flags) ...
```
- **Lösung**: Im Service eine Revisionsnummer (`std::uint64_t revision()` atomar) anbieten und die View hält lokale Kopien, die nur bei geänderter Revision neu geholt werden (`static`/Member in der View). Für `history` (und `results`) `ImGuiListClipper` bzw. einen gedeckelten Anzeigeumfang (z. B. neueste 50 + „… N weitere“) verwenden; `agoText` höchstens 1×/s neu rendern.
- **Erwarteter Effekt**: Entfernt pro Frame Mutex-Kontention mit dem Sync-Thread und mehrere Heap-Kopien (Schätzung; relevant nur bei offenem Fenster und langem Verlauf).
- **Risiko / Verifikation**: Revision muss bei jeder Änderung der vier Datensätze steigen (sonst veraltete Anzeige). Der Service liegt in `HE_SourceControl` (Cross-Modul); Test `test_team_sync_ui.cpp`.

---

### EDT-1611 – UV-Ansicht zeichnet bis zu 120.000 Linien pro Frame, gemeinsame Kanten doppelt, ohne Culling
- **ID**: EDT-1611
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/StaticMeshEditorPanel.cpp:130-134, 213-233`
- **Problem**: Der Wireframe wird im Immediate-Mode jeden Frame neu aufgebaut: je Dreieck drei `AddLine`, jeder Index über `readUV` (Branch cooked/loose, Bounds-Check), bei Meshes ab ~40k Dreiecken sofort am Cap `kMaxUvSegments = 120000` (480k Vertices/Frame in ImGuis CPU-Pfad). Kanten, die zwei Dreiecke teilen (bei Manifold-Meshes fast alle), werden doppelt gezeichnet; Dreiecke außerhalb des Clip-Rects werden nicht verworfen, obwohl Zoom/Pan das meiste aus dem Bild schiebt. Zusätzlich wird über das Cap hinaus abgeschnitten (UI zeigt „first N edges“), d. h. bei großen Meshes ist die Darstellung unvollständig.
- **Code-Auszug**:
```cpp
for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    if (drawn + 3 > kMaxUvSegments) { truncated = true; break; }
    ... readUV(...) x3 ...
    dl->AddLine(p0, p1, edge); dl->AddLine(p1, p2, edge); dl->AddLine(p2, p0, edge);
```
- **Lösung**: Einmal pro Mesh (in `State`, gesetzt neben `statsDone`) eine deduplizierte Kantenliste `std::vector<std::array<float,4>>` bzw. UV-Paare aufbauen (Kanten über `(min(i,j),max(i,j))`-Hash/Sortierung eindeutig machen, sortieren nach Index). Pro Frame: nur Kanten, deren AABB das Canvas schneidet, per einem `PathLineTo`-freien Batch (`AddLine` bleibt, aber ca. halbe Anzahl, kein `readUV`). Das Cap kann dann sinnvoll auf Kanten statt Dreiecksseiten bezogen und deutlich höher angesetzt werden, oder ab einer Grenze die Linie zu einem Raster-Preview (Textur) gerendert werden.
- **Erwarteter Effekt**: Etwa halbe Linienzahl plus Culling bei hohem Zoom; die Cache-Liste entfernt die Indexauflösung pro Frame (Schätzung; messen mit 100k-Dreiecke-Mesh).
- **Risiko / Verifikation**: Optisch identisch (gleiche Linien, einmal gezeichnet); halbtransparente Kanten (Alpha 190) würden bisher an gemeinsamen Kanten doppelt aufaddieren – Aussehen ändert sich dort leicht (Kanten wirken heller/dunkler). Per Screenshot prüfen.

---

### EDT-1612 – `TerrainSync`: Settings-Hash über volle Serialisierung → CBOR-Parse → Json-Umbau → CBOR alle 200 ms je Landscape
- **ID**: EDT-1612
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance / Beides
- **Ort**: `src/HE_Editor/TerrainSync.cpp:297-322, 385-403, 261-292, 522, 615`
- **Problem**: `collect` (pro Frame in der Session) ruft alle `kSettingsIntervalMs = 200` ms je Landscape `settingsBlob`: `serializeWithoutGrids` (alle Komponenten der Entity per `serializeEntityComponents` nach CBOR), danach `json::from_cbor` des gerade erzeugten Blobs, Filtern auf `terrain`/`foliage`, erneutes `json::to_cbor`, dann `fnv1a` Byte für Byte. Das alles nur, um zu erkennen, dass sich nichts geändert hat. Beim Empfang (`applyKeepingGrids`) wird der Blob zweifach geparst (`from_cbor` für das `contains`-Prüfen, danach nochmals in `replaceEntityComponents`/`applyEntityComponents`), und anschließend läuft `settingsBlob` erneut für den Hash (522); bei jedem Wiederanwenden eines Pending-Patches in `applyRemote` (601-607) ebenfalls.
- **Code-Auszug**:
```cpp
const std::vector<std::uint8_t> all = serializeWithoutGrids(world, e);
const json comps = json::from_cbor(all, true, false);   // gerade erst erzeugt
...
return json::to_cbor(out);
```
- **Lösung**: (1) Direkt nur die Komponenten `TerrainComponent` und `FoliageComponent` ohne Grids serialisieren (Serializer-Einstiegspunkt für einzelne Komponentenschlüssel, oder `GridsAside` und anschließend nur zwei Schlüssel schreiben), ohne den Umweg über Parse/Neuaufbau. (2) Für den Änderungstest statt Serialisierung einen Hash über die kleinen POD-Felder der beiden Komponenten (Reihe in `shapeDiffers`/`restoreFoliage` existiert schon) bzw. ein Komponenten-`revision`/`dirty`-Signal; serialisiert wird nur bei Änderung. (3) In `applyKeepingGrids` den ersten `from_cbor` vermeiden (Schlüsselprüfung über den bereits geparsten Baum oder indem der Serializer meldet, welche Komponenten er ersetzt hat).
- **Erwarteter Effekt**: Entfernt 5 Voll-Serialisierungen/s je Landscape samt zwei JSON-Bäumen (Schätzung; abhängig von Anzahl Komponenten der Landscape-Entity, messen).
- **Risiko / Verifikation**: Der Hash muss alle Felder abdecken, die heute im Blob stehen, sonst gehen Änderungen verloren – daher Test: Feld ändern → Patch kommt an, nichts ändern → keiner. Wire-Format (Settings-Payload) bleibt unverändert; Test mit zwei `TerrainSync`-Instanzen als Peers (vorhandene Terrain-Sync-Tests).

---

### EDT-1613 – `TerrainSync`: Vollvergleich der Grids alle 33 ms, doppelte Spiegelkopien, `m_pending` bis ~256 MB und Patch-Kopie in `emit`
- **ID**: EDT-1613
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TerrainSync.cpp:332-347, 349-361, 410-506` (memcmp 469), `TerrainSync.h:58-64, 102-108`
- **Problem**: Pro Landscape und Intervall (33 ms) wird jedes der vier Grids per `memcmp` komplett gegen den Spiegel verglichen (Heights bis 1025²×4 B ≈ 4,2 MB, Weights/Weights2 je 1 MB, Mask 0,25 MB: etwa 6,5 MB je Durchlauf, ≈ 200 MB/s Lesebandbreite × 2 für Spiegel) und der Spiegel hält zusätzlich eine komplette Kopie aller Grids (Speicher ×2). `emit` kopiert jedes Patch (bis `kMaxPatchBytes` = 1 MiB) in `m_pending`, obwohl `p` danach nicht mehr gebraucht wird; `kMaxPending = 256` begrenzt nur die Anzahl, nicht die Bytes (Worst Case ~256 MiB bei ausbleibendem Echo).
- **Code-Auszug**:
```cpp
else if (v.bytes > 0 && std::memcmp(v.data, mg.data(), v.bytes) != 0) { ... }   // :469
m_pending.push_back({ e, p });                                                   // :355
if (m_pending.size() > kMaxPending) m_pending.pop_front();
```
- **Lösung**: (1) Der Header begründet bewusst „nicht fragen, welches Werkzeug es war“; trotzdem die bereits vorhandenen Dirty-Signale der Komponenten (`t->dirty`, `regionDirty` + `dirtyMin/Max`, `weightsDirty`, `fol->dirty`) als **Hinweis** nutzen: nur wenn ein Flag oder ein Revisionszähler seit dem letzten Vergleich bewegt wurde, wird der Vollvergleich ausgeführt, sonst höchstens alle 1–2 s als Sicherheitsnetz (fängt Änderungen ohne Flag, z. B. MCP/Undo). (2) In `emit` das Patch in `m_pending` verschieben und `send` auf der gespeicherten Referenz aufrufen (bei Misserfolg wieder entfernen), sonst nur bei `m_expectEcho` kopieren. (3) `m_pending` zusätzlich nach Gesamtbytes begrenzen (z. B. 32 MiB).
- **Erwarteter Effekt**: Ruhe-Frames ohne Pinsel kosten fast nichts statt ~0,3–1 ms/Intervall je Landscape; spart eine ≤1-MiB-Kopie pro gesendetem Patch (Schätzung, messen: Memcmp-Zeit auf Zielhardware).
- **Risiko / Verifikation**: Hohes Risiko bei Variante (1): Konsistenz zwischen Peers hängt am Vollvergleich; ein Flag, das ein Werkzeug nicht setzt, würde Änderungen verzögern (bis zum Sicherheitsnetz). Daher Zwei-Peers-Test mit Pinsel, Fill-Layer, Heightmap-Import, Undo und MCP-Änderung; Sicherheitsnetz-Intervall konservativ lassen. (2) und (3) sind risikoarm.

---

### EDT-1614 – `StreamingDebugView`: pro Frame Entity-Zählung per Vollscan und Vollstring-Vergleich des Manifests
- **ID**: EDT-1614
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/StreamingDebugView.cpp:38-53, 62-135, 368-379, 469-475`
- **Problem**: (a) `drawScene` zählt die Entities der offenen Szene mit `view<entt::entity>().each(++n)` (O(N)) jeden Frame, solange der Streaming-Tab sichtbar ist; entt kennt die Zahl bereits. (b) `manifestOf` wird pro Frame vom Viewport-Overlay und vom Tab aufgerufen und vergleicht jedes Mal `text != s_text` (Voll-`memcmp` des Manifest-JSON, bei vielen Zellen hunderte KB) statt eines Änderungszählers. (c) `appendCellLines` baut pro Frame einen `anchors`-Vektor, ruft `m->around(...)` und berechnet 96 `cos/sin`-Paare je Ring und Anker; `drawCells` ruft `around` mit denselben Ankern nochmals. (d) `drawJobs` kopiert `pool.stats()` (Struct mit Lane-Array) und `drawAssets` baut `asyncInFlightPaths(12)` als `vector<string>` pro Frame.
- **Code-Auszug**:
```cpp
ctx.world->registry().view<entt::entity>().each([&](auto) { ++n; });   // :374
const std::string& text = world.cellManifestJson();
if (text != s_text) { ... }                                             // :43-44
```
- **Lösung**: (a) `registry().storage<entt::entity>().in_use()` (bzw. `.size() - free_list()` je nach entt-Version) verwenden. (b) In `HorizonWorld` einen `cellManifestRevision()`-Zähler in `setCellManifestJson` pflegen und `manifestOf` danach keyen; ersatzweise `size()` und Zeigeridentität zuerst vergleichen. (c) Ring-Einheitskreis (96 Punkte) einmal als `static const` Tabelle; `around`-Ergebnis pro Frame teilen (Overlay und Tab laufen im selben Frame). (d) nur wenn Tab sichtbar (ist so), Rest klein – belassen.
- **Erwarteter Effekt**: O(N)→O(1) für (a); (b) spart bei großen Manifesten einen Vollvergleich pro Aufruf; insgesamt klein (Schätzung).
- **Risiko / Verifikation**: Niedrig. entt-Version prüfen (API-Name), `tests` zu `StreamingDebugView` bzw. `CellManifest` laufen lassen.

---

### EDT-1615 – Panelliste in `TaskBoardPanel` dupliziert `docsPanelOpener` und ist bereits veraltet
- **ID**: EDT-1615
- **Kategorie**: A / F (Pflege-Liste)
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/TaskBoardPanel.cpp:355-361` (`kPanels`), 552, 1260; Gegenstück `src/HE_Editor/EditorUI.cpp:291-325`
- **Problem**: Die gültigen Panel-Link-Ziele sind in `kPanels` handgepflegt und müssen mit `docsPanelOpener` übereinstimmen (Kommentar: „the ones EditorUI's docsPanelOpener knows how to raise“). Sie weichen bereits ab: `docsPanelOpener` kennt „Team“ und „Scene 2/3/4“, `kPanels` nicht – diese Panels lassen sich nicht verlinken. Ein neues Panel muss an zwei Stellen eingetragen werden.
- **Code-Auszug**:
```cpp
const char* const kPanels[] = { "Scene", "World Outliner", "Details", ..., "Undo History", "Watch", "Task Board", };
```
- **Lösung**: Eine gemeinsame Tabelle (z. B. `EditorPanels.h`: `constexpr std::array<std::string_view,N> kLinkablePanels`), aus der sowohl `docsPanelOpener` (Toggle-/Always-Listen) als auch die Task-Board-Auswahl lesen; oder `docsPanelOpener` eine Abfragefunktion `bool knowsPanel(const char*)` bereitstellen lassen, die `kPanels` ersetzt.
- **Erwarteter Effekt**: Keine Laufzeitänderung; verhindert weiteres Auseinanderlaufen.
- **Risiko / Verifikation**: Reihenfolge im Menü „Link Panel“ ändert sich ggf.; UI-Test `chipRect` für Panel-Links. Bestehende Task-Dateien mit Panel-Links bleiben gültig (nur Erweiterung).

---

### EDT-1616 – Case-insensitiver Namensvergleich mehrfach implementiert
- **ID**: EDT-1616
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/TeamController.cpp:52-59` (`sameName`), `TaskBoardTeam.cpp:24-30` (`sameTaskMember`), `TeamSyncController.cpp:14-18` (`folded`), `TaskBoardTeam.cpp:233-242` und `TeamView.cpp:342-347`
- **Problem**: Dieselbe Regel („Mitgliedsnamen sind case-insensitiv wie auf dem Teamserver“) existiert in drei Varianten (Vergleich, Falten in Kopie, `tolower`-Schleife für Map-Schlüssel). `TeamController::sameMemberName` ist bereits die öffentliche Variante, `TaskBoardTeam` nutzt sie aber nicht (ein anderer Tool-Layer). Die Varianten sind nicht identisch (Länge, `unsigned char`-Cast, Locale), was Fehler bei Nicht-ASCII-Namen begünstigt.
- **Code-Auszug**:
```cpp
// TeamController.cpp:52
bool sameName(const std::string& a, const std::string& b) { ... std::tolower(...) ... }
// TaskBoardTeam.cpp:24
bool sameTaskMember(std::string_view a, std::string_view b) { ... std::tolower(...) ... }
```
- **Lösung**: Eine Funktion `HE::Net::sameMemberName(string_view, string_view)` (am besten neben `TeamClient`/`RelayTransport::isValidId`, dort gilt dieselbe Regel serverseitig), von allen genutzt; `folded` nur noch für Map-Keys (`online[key]`) durch einen case-insensitiven Hash/Vergleich ersetzen oder das Falten dort belassen.
- **Erwarteter Effekt**: Weniger Code, eine Stelle für spätere Unicode-Regeln.
- **Risiko / Verifikation**: Gleiche Semantik (ASCII-`tolower`) beibehalten; Team-Tests (`test_team_ui`, TaskBoard-Team-Tests mit `FakeTaskTeam.h`).

---

### EDT-1617 – TaskBoard-Speicherpfad: Board-Kopie inkl. Datei-Bytes, doppelter Parse, O(n²) in `afterEdit`
- **ID**: EDT-1617
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TaskBoardPanel.cpp:231-273` (Kopie 236-238, 261); `TaskBoard.cpp:836-913` (probe 863-873), `TaskBoardTeam.cpp:160-197` (170-173), `TaskBoard.h:179, 254`
- **Problem**: Bei verbundenem Team kopiert `edit()` das ganze `TaskBoard` (`before = *b`) – inklusive des privaten Members `m_diskBytes` (kompletter Dateiinhalt) – nur um in `afterEdit` neue Updates zu erkennen. `afterEdit` ist O(Tasks²): für jede Task `before.find(t.id)` (linear) und ein neues `unordered_set<UUID> had` pro Task. `saveTaskBoard` parst bei jedem Speichern die Datei auf der Platte vollständig (`probe.fromJson(onDisk)`), obwohl `onDisk == board.m_diskBytes` (bereits von diesem Board gelesen/geschrieben) im Normalfall eine bekannte Gültigkeit hat; zusätzlich liegt der Dateiinhalt doppelt im Speicher (`m_diskBytes` + `onDisk`). `TaskBoard::find` und alle Setter sind linear in der Taskzahl.
- **Code-Auszug**:
```cpp
TaskBoard before;  if (tell) before = *b;                   // Panel :237-238
const Task* old = before.find(t.id);                        // afterEdit :170 (in Schleife über after.tasks)
std::unordered_set<UUID> had; ...                           // :171
const TaskBoard::LoadResult r = probe.fromJson(onDisk);     // TaskBoard.cpp :866
```
- **Lösung**: (1) Für `afterEdit` statt Board-Kopie nur eine Karte `UUID → (updates.size(), assignees)` vor dem Edit erfassen (Updates sind „oldest first“ und append-only, neue Updates = Index ≥ alte Größe; bei Reload gegen die Disk-Version entsprechend neu erfassen). (2) `m_diskBytes` aus `TaskBoard`-Kopien ausschließen (eigener Typ für Disk-Zustand) oder Board als `shared_ptr<const>` snapshotten. (3) `saveTaskBoard`: Wenn `onDisk == m_diskBytes`, ein Flag `m_diskParsable` (beim Laden gesetzt) statt erneutem Parse nutzen. (4) Optional `unordered_map<UUID,size_t>` Index für `find` ab einigen hundert Tasks.
- **Erwarteter Effekt**: Edit-/Save-Kosten (nicht pro Frame) von mehreren Vollkopien/Parses auf einen; für typische Boards (< 100 Tasks) vernachlässigbar.
- **Risiko / Verifikation**: Neue Update-Erkennung nur per Größe setzt voraus, dass Updates nie eingefügt/umsortiert werden (ein Pull kann die Liste umbauen → in dem Fall ID-basiertes Verfahren behalten). `tests/test_mcp_tools_taskboard.cpp` plus Team-Notice-Tests; Test für Save-over-conflict-Datei bleibt Pflicht (Verhalten „refuses unparsable file“).

---

### EDT-1618 – `TaskBoardTeam::update` baut bei wartendem Outbox-Eintrag jeden Frame Mitgliederliste und Map
- **ID**: EDT-1618
- **Kategorie**: C / E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TaskBoardTeam.cpp:220-253`; Aufrufer `TaskBoardPanel.cpp:282-290, 386`
- **Problem**: `pumpTeam()` läuft jeden Frame (auch bei geschlossenem Fenster). Ist die Verbindung da und `m_outbox` nicht leer – etwa weil ein Empfänger offline ist (Einträge bleiben bis `kOutboxTtlMs` = 15 min) –, wird pro Frame `m_link->members()` (Wert-Rückgabe, Kopie) abgerufen, ein `unordered_map<string,bool>` aufgebaut (Schlüssel gefaltet = Kopie je Mitglied) und jeder Outbox-Eintrag nochmals gefaltet. Ebenso `pumpTeam` selbst (`s_team->self()` als String-Kopie pro Frame). Heute nicht live: `TaskBoardPanel::setTeam` wird im Editor-Code nirgends aufgerufen (nur Tests), die Verdrahtung steht laut `docs/taskboard-team-interop-2026-10-15.md` unter „Next steps“ aus.
- **Code-Auszug**:
```cpp
if (m_link->connected() && !m_outbox.empty()) {
    std::unordered_map<std::string, bool> online;
    for (const TaskTeamMember& m : m_link->members()) { std::string key = m.name; ... online[key] = m.online; }
```
- **Lösung**: Vor der Verdrahtung beheben: Outbox-Verarbeitung nur im Takt (z. B. alle 250 ms oder bei Roster-Änderung `revision()`), `members()` als `const&`/Revisions-gekeyter Cache im Link, Map durch lineare Suche über das kleine Roster ersetzen (`sameTaskMember`), `self()` cachen. `poll()` bleibt pro Frame (billig).
- **Erwarteter Effekt**: Verhindert, dass die geplante Verdrahtung pro Frame ~10 Allokationen einführt.
- **Risiko / Verifikation**: `kAssignSettleMs`-Timing hängt an `nowMs`, nicht an Frames – gedrosselter Aufruf ändert nur Latenz (≤ Takt). Tests mit `FakeTaskTeam.h` (feste Uhr) anpassen.

---

### EDT-1619 – Test-Instrumentierung (Rect-Maps, Probe-Vektoren) läuft im Produktiv-Drawpfad mit
- **ID**: EDT-1619
- **Kategorie**: F / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/TaskBoardPanel.cpp:114-118, 390-393, 840-842, 453-457`; `TaskBoardFooter.cpp:134-136, 277, 286`; `TeamView.cpp:30, 88-103, 445, 446`
- **Problem**: Pro gezeichneter Karte wird `s_cardRects[id]` (unordered_map-Insert) und `s_chipRects[id]` (Vektor) gefüllt, jeden Frame geleert und neu aufgebaut (`clear()` behält Buckets, aber jeder Insert allokiert Knoten); `drawRows` im Footer-Overlay macht dasselbe für Tick/Titel. Das dient laut Header „for the UI test“. `TeamView::probe` ist korrekt über `s_probe` abgeschaltet.
- **Code-Auszug**:
```cpp
s_cardRects[id] = Rect{ pos.x, pos.y, pos.x + width, pos.y + m.h };
std::vector<Rect>& chipRects = s_chipRects[id];
chipRects.clear();
```
- **Lösung**: Wie bei `TeamView::setProbe` ein Opt-in: `static bool s_recordRects` (von Tests per `reset()`/`enableProbe(true)` gesetzt); die Maps nur füllen, wenn aktiv. Alternativ ein `std::vector<std::pair<UUID,Rect>>` mit `reserve` statt Map.
- **Erwarteter Effekt**: Minus ~2 Map-Inserts und 1 Vektor-Allokation pro sichtbarer Karte und Frame (Schätzung).
- **Risiko / Verifikation**: Bestehende UI-Tests müssen das Probe aktivieren (kleine Testanpassung); `lastDrawStats()` bleibt unberührt.

---

### EDT-1620 – Kleinkram: unbenutzte Parameter/Variablen, schwere Includes, `std::regex` für Dateinamen
- **ID**: EDT-1620
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `StaticMeshEditorPanel.cpp:280, 312` (`org` nur mit `(void)org;`); `TaskBoardPanel.cpp:828, 1114, 1325` (`TaskBoard& b` in `drawCard`/`drawDetails` ungenutzt, `(void)b;`); `TeamView.cpp:75-80, 1138-1149` (`Form& form` ungenutzt, `(void)form;`); `StreamingDebugView.cpp:22, 211-218` (`<regex>`, `<iterator>`, `<fstream>`); `TeamView.h:29`; `TaskBoard.h:6` (`<functional>`); `TaskBoardTeam.h:6-8`
- **Problem**: (a) Tote Variable/Parameter mit Unterdrückung statt Entfernung. (b) `StreamingDebugView` bindet `<regex>` (sehr schwerer Header, langsame Compile-Zeit) nur ein, um `cell_-?\d+_-?\d+\.hescene` zu erkennen; ein Dateiname dieser Form ist mit `rfind/strtol` in ~10 Zeilen prüfbar (einmalige Aktion, daher nur Compile-/Binärgröße). (c) `TeamView.h` zieht `TeamController.h` (inkl. `NotificationStore.h`, `Net/TeamClient.h`, `<filesystem>`, `<deque>`) in jede Einbindung, obwohl Deklarationen von `TeamController`/`Status` per Vorwärtsdeklaration reichen würden (`Status` ist ein verschachteltes Enum; dafür `statusText(...)` auf `int`/eigenes Enum-Alias umstellen oder den Enum auslagern).
- **Code-Auszug**:
```cpp
const ImVec2 org = ImGui::GetCursorScreenPos();   // StaticMesh :280
(void)org;                                         // :312
static const std::regex kCellFile(R"(cell_-?\d+_-?\d+\.hescene)");   // StreamingDebugView :211
```
- **Lösung**: Parameter/Variablen entfernen (Aufrufer in `drawList`/`drawColumns` mit anpassen), Regex durch Handparser ersetzen, `TeamController::Status` in einen schlanken Header auslagern und `TeamView.h` darauf beschränken.
- **Erwarteter Effekt**: Kürzere Compile-Zeiten (`TeamView.h` ist u. a. von `EditorUI`/Tests eingebunden), sauberere Signaturen.
- **Risiko / Verifikation**: Reine Aufräumarbeit; Build aller Ziele inkl. `he_tests` (`test_team_ui` nutzt `TeamView.h` direkt).

---

## Gelesen

Alle 24 Dateien des Slice wurden vollständig gelesen (inklusive `TaskBoardPanel.cpp` 1–1333, `TeamView.cpp` 1–1270, `TeamController.cpp` 1–971, `TerrainSync.cpp` 1–627); `stb_image_impl.cpp` ist eine Zwei-Zeilen-Datei ohne Befund. Zur Einordnung wurden außerhalb des Slice nur gezielt `EditorCommands.cpp:405-430`, `EditorApplication.cpp` (Aufruf- und Diffstellen), `EditorUI.cpp:285-330` und das Handoff in `docs/taskboard-team-interop-2026-10-15.md` angesehen.

## Editor Slice 17 – TerrainTools … TypeAssetPanel (src/HE_Editor)

## Zusammenfassung
Der Slice (16 Dateien, 8698 Zeilen) ist insgesamt sauber und gut kommentiert. Die relevanten Probleme sitzen an drei Stellen: (1) die Sculpt-Brushes in `TerrainTools.cpp` iterieren pro Frame über das gesamte Höhenraster (bis 1025², Smooth kopiert zusätzlich den ganzen Vektor); (2) `TypeAssetPanel` scannt pro Struct-/Enum-Feld und pro Frame rekursiv beide Content-Roots samt Header-Reads (`listAssets`), obwohl die Combo meist geschlossen ist; (3) mehrere Panels erzeugen pro Frame vermeidbare Kopien, Allokationen und volle Grid-Scans (ToolSetup-Probes, Water-`wetCells`, Tutorial-Sampling, Theme-Styleliste). Dazu kommen Duplikation (Brush-Kernel, `track`-Lambdas, Asset-Tab-Boilerplate) und einige Kommentar-/Encoding-Fehler.
Funde: 2 Hoch, 9 Mittel, 8 Niedrig (19 gesamt). Keine Quelldateien geändert.

---

### EDT-1701 – Sculpt-Brushes laufen pro Frame über das ganze Höhenraster
- **ID**: EDT-1701
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TerrainTools.cpp:894-1016` (Kernel), `:896` (Smooth-Kopie), `:1018-1040` (Region-Dirty)
- **Problem**: Während eines Strokes (jeder Frame mit LMB) laufen Smooth/Flatten/Ramp/Roughen/Raise/Lower mit zwei verschachtelten Schleifen über `tcRes × tcRes` (max. `kTerrainMaxResolution = 1025`, also ~1,05 Mio. Vertices). Pro Vertex werden Weltposition, `d2` und `brushWeight` berechnet; erst danach verwirft `w <= 0` fast alle. Der Brush trifft aber nur das Quadrat `[hit ± (radius+falloff)]`. Smooth legt außerdem pro Frame eine Vollkopie (`tc.sculptHeights`, ~4 MB bei 1025²) an und verschiebt sie am Ende zurück. Das Dirty-Rechteck (`dirtyMinX…`) wird danach korrekt auf den Brushbereich begrenzt, d. h. die CPU-Schleife ist der einzige Teil, der noch O(res²) ist.
- **Code-Auszug**:
```cpp
std::vector<float> smoothed = tc.sculptHeights;            // 896: Vollkopie pro Frame
for (uint32_t zi = 0; zi < tcRes; ++zi)
    for (uint32_t xi = 0; xi < tcRes; ++xi)
    {
        const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
        ...
        const float w  = brushWeight(d2);
        if (w <= 0.0f) continue;
```
- **Lösung**: Einmal pro Frame das Texel-Rechteck berechnen (analog `TerrainPaint::paint`, `src/HE_Scene/src/TerrainPaint.cpp:56-60`): `x0 = floor((hit.x - r - origin.x)/stepX)`, `x1 = ceil(...)`, auf `[0,tcRes-1]` klemmen; alle fünf Kernel iterieren nur `z0..z1 × x0..x1`. Smooth schreibt in einen kleinen Puffer (Rechteck + 1 Texel Rand) statt den ganzen Vektor zu kopieren; alternativ zeilenweise mit zwei Zwischenzeilen. Für Ramp das Rechteck um die Korridor-Box (Start/End ± r) erweitern; das wird in `:1030-1034` für das Dirty-Rect schon berechnet und kann wiederverwendet werden. Siehe EDT-1710 (gemeinsamer Kernel-Helper).
- **Erwarteter Effekt**: Schätzung: bei res 1025 und Brush ~30 m auf 1 km Terrain schrumpft die Iteration von ~1 Mio. auf ~3-10 k Texel (>100×). Smooth spart zusätzlich ~4 MB Alloc+Copy pro Frame. Frame-Zeit beim Sculpten auf großen Terrains: spürbar (Schätzung mehrere ms); unsicher/messen.
- **Risiko / Verifikation**: Ergebnis muss bitgleich bleiben (außerhalb des Rechtecks gilt ohnehin `w <= 0`; Rundung am Rand mit +1 Texel Sicherheit). Test: Terrain-Fixture, alle Tools mit festem Hit/dt vor und nach der Änderung ausführen und `sculptHeights` byte-weise vergleichen (neuer Test, z. B. in `tests/` neben den Terrain-Tests; Kernel dafür in eine freie Funktion ziehen). Smooth: Randtexel beachten (3×3-Nachbarschaft liest außerhalb des Rechtecks).

---

### EDT-1702 – `typeNamePicker` scannt pro Feld und Frame beide Content-Roots
- **ID**: EDT-1702
- **Kategorie**: E / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TypeAssetPanel.cpp:260-282` (Aufruf `:619`), `src/HE_Editor/HcEditorUtil.cpp:93-100, 53-91` (`listAssets`/`scanAssetsInto`)
- **Problem**: Für jedes Feld vom Typ Enum/Struct wird in jedem Frame `HcEditorUtil::listAssets` aufgerufen. Das ist ein `recursive_directory_iterator` über Content und EngineContent, mit pro `.hasset` einem Header-Read (`readAssetTypeFromFile`) und `file_size`, plus `std::filesystem::relative` pro Treffer. Das Ergebnis wird nur gebraucht, wenn die Combo offen ist (`BeginCombo` Zeile 269). Bei 5 solchen Feldern und tausenden Assets sind das Dateisystem-Zugriffe im UI-Thread, jeden Frame.
- **Code-Auszug**:
```cpp
const auto  assets  = HcEditorUtil::listAssets(ctx.contentManager,
    wantEnum ? HE::AssetType::EnumType : HE::AssetType::StructType);
...
if (ImGui::BeginCombo("##typename", shown.c_str()))
{
    for (const auto& a : assets) {
```
- **Lösung**: `listAssets` erst innerhalb von `if (ImGui::BeginCombo(...))` aufrufen (dann einmal pro Öffnen und Frame der offenen Combo statt immer). Besser: das Ergebnis pro `(type)` im `PanelState` cachen (Invalidierung über `ctx.contentRefreshDone`/EditorAssetTypeCache-Revision) oder `ContentManager`/`EditorAssetTypeCache` fragen, die die Typen ohnehin kennen. Minimale Änderung: Zeile 264 hinter 269 verschieben.
```cpp
if (ImGui::BeginCombo("##typename", shown.c_str())) {
    const auto assets = HcEditorUtil::listAssets(ctx.contentManager, type);
    ...
```
- **Erwarteter Effekt**: Schätzung: eliminiert O(Felder × Assets) Dateizugriffe pro Frame bei geschlossener Combo, im Normalfall ~100 % der Kosten dieser Funktion. Für Projekte mit großen Content-Bäumen der Unterschied zwischen flüssig und ruckelnd; messen mit ≥ 2000 Assets.
- **Risiko / Verifikation**: Gering. Beim offenen Combo bleibt das Verhalten identisch. Manuell: Struct mit Enum-Feld, Combo öffnen, Einträge prüfen. Cache-Variante: neues Asset anlegen und prüfen, dass es erscheint.

---

### EDT-1703 – ToolSetupDialog: `rowState()` kopiert pro Aufruf die kompletten Probes
- **ID**: EDT-1703
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Editor/ToolSetupDialog.cpp:240-278` (`refresh`), `:292-294` (`rowState` ruft `refresh`), `:341-346, 382, 399, 414, 428, 722` (Aufrufer), `:617-618, 637-638, 697-699` (Log-Kopien); `src/HE_Editor/EditorApplication.cpp:12323, 12339`
- **Problem**: `refresh(ctx)` kopiert `*ctx.toolchainProbe` und `*ctx.gitProbe` (`GitProbe` enthält mehrere Strings, einen `std::filesystem::path` und einen `GitInstallPlan` mit Kommando-Strings) bei jedem Aufruf. `rowState()` ruft es jedes Mal auf. Bei offenem Dialog werden pro Frame rund 25 `rowState`-Aufrufe ausgeführt (`everythingSetUp` 5, `pumpQueue` ≤ 5+, `drawRow` 5, `installableCount` 4, `drawManual` 4, Footer) = ~25 Vollkopien. Zusätzlich läuft `DrawToolSetupDialog` jeden Frame, auch bei geschlossenem Dialog, und kopiert dann einmal. Außerdem kopiert `ctx.gitInstallLog()` / `toolchainInstallLog()` den kompletten Installationslog unter Mutex, pro Frame in `drawRow` (Installing/Failed) und nochmal in `drawLog`. `gitInstallLaunch()` wird in `canInstall`, `sourceLine`, `promptLine` und der Cancel-Prüfung mehrfach pro Zeile neu berechnet.
- **Code-Auszug**:
```cpp
void refresh(AppContext& ctx)
{
	if (ctx.toolchainProbe) { s_tc = *ctx.toolchainProbe; ... }
	if (ctx.gitProbe)       { s_git = *ctx.gitProbe; ...
...
RowState rowState(AppContext& ctx, Tool t) { refresh(ctx); ...
```
- **Lösung**: `refresh` nur einmal pro Frame ausführen (Guard über `ImGui::GetFrameCount()`; `rowState` bleibt ohne Seiteneffekt, die Kanten-Erkennung in `refresh` ist idempotent pro Frame) oder die Kopie nur bei geänderter Probe (Revision/Zeitstempel im Probe-Struct bzw. `if (&*ctx.gitProbe != last)`). Log pro Frame einmal holen und an `drawRow`/`drawLog` durchreichen; Log nur bei offenem Tree-Node und Installing/Failed.
- **Erwarteter Effekt**: Schätzung: ~25 Struct-Kopien (je ~10 Strings) weniger pro Frame bei offenem Dialog, 1 weniger bei geschlossenem. Absolut klein (zehn bis hundert µs), aber der Dialog ist ein Modal, das nur selten gezeigt wird; daher Mittel, nicht Hoch.
- **Risiko / Verifikation**: `tests/` hat einen headless UI-Test für den Dialog (`resetStateForTests`, `drawToolSetup`); Zustandsübergänge Checking → Installing → Failed damit prüfen. `s_checkingTc/Git` hängt davon ab, dass `refresh` bei null-Probe aufgerufen wird; der Frame-Guard darf das nicht überspringen, wenn sich `ctx.toolchainInstalling` zwischen zwei Aufrufen im selben Frame ändert (tut es nicht).

---

### EDT-1704 – Tutorial: Asset-Baum-Scan und Signal-Sampling jeden Frame
- **ID**: EDT-1704
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TutorialPanel.cpp:195-301` (`sample`), `:745-748` (Aufruf), `:227-228` (Material-Schleife), `:285-300` (Tabs)
- **Problem**: Bei offenem Tutorial läuft `sample()` jeden Frame. Für die Steps `AssetAdded`/`AssetOfTypeAdded` wird der gesamte Content-Baum unter dem Shared-Lock durchlaufen, pro Datei ein `EditorAssetTypeCache::assetTypeOf(file->fullPath)` (String-Hash-Lookup). Bei 10 k Dateien sind das 10 k Hash-Lookups pro Frame. Unabhängig vom Step werden außerdem jedes Frame alle `MaterialComponent`s iteriert (`materialsAssigned`), `openTabs` als String neu aufgebaut (Allokationen), pro Tab `assetTypeOf` aufgerufen und `s.visitedPanels` kopiert. Die gesamte `Signals`-Struktur wird neu angelegt (zwei Strings).
- **Code-Auszug**:
```cpp
for (auto [e, mat] : reg.view<MaterialComponent>().each())
    if (mat.materialAssetId != HE::UUID{}) ++s.materialsAssigned;
...
for (const HE::File* file : f->files) { ... tutAsset(EditorAssetTypeCache::assetTypeOf(file->fullPath)); }
```
- **Lösung**: (a) Asset-Scan auf ~4 Hz drosseln oder nur bei geänderter Content-Revision (Dirty-Flag/`contentRefreshDone`) neu berechnen und das letzte Ergebnis wiederverwenden; Baseline und Live-Wert bleiben konsistent, solange beide dieselbe Quelle nutzen. (b) `materialsAssigned` und die Tab-Summen nur berechnen, wenn `step->check` sie braucht (gleiches Muster wie `needsAssetScan`). (c) `openTabs`/`visitedPanels` nur für `TabOpen`/`PanelsVisited`-Steps aufbauen.
- **Erwarteter Effekt**: Schätzung: ~10 k Lookups + einige Allokationen pro Frame weniger auf Asset-Steps; sonst bleibt nur O(#Material) für die Schleife. Gesamtgewinn begrenzt, da nur bei offenem Tutorial; messen mit großem Content-Baum.
- **Risiko / Verifikation**: `tests/test_tutorial.cpp` deckt die reinen Prädikate ab, `sample()` selbst nicht. Drosselung kann die Anzeige von „Done“ um ≤ 250 ms verzögern; akzeptabel, vorher mit UX abklären.

---

### EDT-1705 – Landscape-Water-Tab: `wetCells()` scannt pro Body und Frame das ganze Wassergitter
- **ID**: EDT-1705
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TerrainTools.cpp:1688-1689, 1723`; `src/HE_Scene/src/WaterField.cpp:196-202`
- **Problem**: Im Water-Tab ruft die Panel-Zeichnung `tc.water.wetCells()` (Gesamtzahl) und in der Bodies-Schleife `tc.water.wetCells(b.id)` pro Body in jedem Frame auf. Die Funktion iteriert über das komplette Raster `coverage` (res bis 2048, also bis 4 M Zellen). Kosten = (Bodies + 1) × res² pro Frame, nur zur Anzeige einer Zahl.
- **Code-Auszug**:
```cpp
dimText("%zu water bod%s, %u wet cells", ..., tc.water.wetCells());
...
ImGui::Text("%s: level %.2f m, %u cells", label, b.level, tc.water.wetCells(b.id));
```
- **Lösung**: Zählwerte im Panel pro Revision cachen (`WaterField` hat eine Revision, siehe Kommentar „bumps the revision“ in `:1846`), oder im Field selbst Zellzähler pro Body mitführen (inkrementell bei `dab`/`clear`). Einfachste Variante: einmalige Einzelpass-Funktion `wetCellsPerBody()` (ein Durchlauf über `coverage/owner`, Ergebnis in kleinen Vektor) und Cache im Panel, solange sich die Revision nicht ändert.
- **Erwarteter Effekt**: Schätzung: bei res 512 und 3 Bodies ~1 M Elementzugriffe/Frame → 0 im Ruhezustand (~0,3-1 ms). Bei res 2048 deutlich mehr. Nur im Water-Tab relevant; messen.
- **Risiko / Verifikation**: Zahlen müssen während des Paintens aktuell bleiben (Cache an Revision/„dab“ koppeln). Test: `WaterField`-Tests (`tests/`) um `wetCellsPerBody == wetCells(id)` ergänzen.

---

### EDT-1706 – TypeAssetPanel: ungeclippte Zeilen, O(n²)-Duplikatcheck, Child-Fenster pro Feld
- **ID**: EDT-1706
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TypeAssetPanel.cpp:470-501` (Enum), `:483-484` (Duplikat), `:562-686` (Struct), `:568-570` (Child), `:578-579` (Duplikat)
- **Problem**: Alle Zeilen werden jeden Frame gezeichnet (kein Clipping, vgl. CLAUDE.md §4 „Row-heavy panels draw only visible rows“). Jede Zeile prüft den Duplikatnamen mit einer inneren Schleife über alle Zeilen: O(n²) String-Vergleiche pro Frame. Bei Structs/SaveGame-Templates ist jedes Feld ein eigenes `BeginChild(... AutoResizeY ...)`, also ein vollwertiges ImGui-Fenster mit Layout-Pass; die Höhe ist variabel (Array-/Map-/Transform-Zeilen), Clipping mit konstanter Zeilenhöhe ist daher nicht direkt möglich. Eine SaveGame-Vorlage mit 100-200 Feldern ist realistisch.
- **Code-Auszug**:
```cpp
bool dup = false;
for (int k = 0; k < static_cast<int>(st.structDef.fields.size()); ++k)
    if (k != i && st.structDef.fields[k].name == f.name && !f.name.empty()) dup = true;
```
- **Lösung**: Duplikate einmal pro Frame (oder nur wenn `st.dirty`-Änderung) über ein `unordered_set<string_view>`/Sortierung berechnen und `std::vector<bool> dupRow` im State halten: O(n). Für die Enum-Tabelle `ImGuiListClipper` verwenden (konstante Zeilenhöhe). Für Struct-Felder: Child-Fenster durch `Selectable`-freie Gruppe (`BeginGroup` + `GetWindowDrawList()->AddRect`) ersetzen oder Höhe pro Feld cachen und clipper mit `ItemsHeight`-Array nutzen; die zweite Variante ist invasiver, daher zuerst Duplikat-Fix.
- **Erwarteter Effekt**: Schätzung: bei 200 Feldern 40 k → 200 String-Vergleiche pro Frame; Child-Overhead (200 Fenster/Frame) bleibt bis zum zweiten Schritt. Messen mit großer Vorlage.
- **Risiko / Verifikation**: Rote Umrandung muss identisch bleiben. UI-Test nach Muster `tests/test_*_ui.cpp` (HE_UI_DUMP_DIR) mit Duplikatnamen.

---

### EDT-1707 – TypeAssetPanel: Registry-Kopien und Lock pro Frame
- **ID**: EDT-1707
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TypeAssetPanel.cpp:437` (`nameCollides`), `:230-233` (`getEnum` in `defaultValueEditor`); `src/HE_Core/src/Types/TypeRegistry.cpp:160-168, 222-231`
- **Problem**: `TypeRegistry::nameCollides` nimmt den Registry-Mutex und iteriert über alle Enums und Structs mit String-Vergleich, jeden Frame, für jeden Tab. `getEnum(f.typeName, ed)` kopiert pro Enum-Feld und Frame die komplette `EnumDef` (alle Einträge samt `formerNames`) unter Mutex, nur um die Combo-Vorschau zu zeigen. Der Lock konkurriert mit dem Hintergrund-Pfad (Loader/MCP), der die Registry schreibt.
- **Code-Auszug**:
```cpp
if (!st.isTemplate && HE::TypeRegistry::instance().nameCollides(st.name, st.relPath)) ...
HE::EnumDef ed;
if (f.typeName.empty() || !HE::TypeRegistry::instance().getEnum(f.typeName, ed) || ed.entries.empty())
```
- **Lösung**: `nameCollides` nur neu berechnen, wenn sich `st.name`/Registry-Revision ändert (Ergebnis im `PanelState` cachen, Revision-Zähler in `TypeRegistry` ergänzen oder bei Save/Reload invalidieren). Für den Enum-Default `ed` pro Feld nur beim Öffnen der Combo holen; die Vorschau (`shown`) braucht nur den ersten/aktuellen Eintragsnamen. Alternativ ein `getEnumShared()` mit `shared_ptr<const EnumDef>` anbieten.
- **Erwarteter Effekt**: Schätzung: pro Frame 1 Lock+Scan und N Enum-Kopien weniger; klein bei wenigen Typen, wächst mit Projektgröße.
- **Risiko / Verifikation**: Cache-Invalidierung bei Umbenennung im selben Projekt (anderer Tab speichert) beachten; Test: zwei Tabs mit gleichem Namen, Warnung erscheint/verschwindet nach Rename.

---

### EDT-1708 – TextureViewer: Dateisystem-Aufrufe pro Frame in `drawInfo` (Raw-Datei-Tab)
- **ID**: EDT-1708
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TextureViewerPanel.cpp:257-312`
- **Problem**: Für einen Tab auf eine noch nicht importierte Bilddatei werden pro Frame ausgeführt: `isEngineDefaultPath` (zweimal), `std::filesystem::relative(src.parent_path(), root, ec)` (ruft intern `weakly_canonical`, also Stat-Syscalls) und `Importer::resolveOutput(...)` zur Anzeige von „Writes …“. Nichts davon ändert sich, solange Pfad und Projekt gleich bleiben.
- **Code-Auszug**:
```cpp
relDir = std::filesystem::relative(src.parent_path(), root, ec);
...
ImGui::TextDisabled("Writes %s",
    Importer::resolveOutput({}, relDir, src.stem().string()).path.c_str());
```
- **Lösung**: Ziel (`root`, `relDir`, `engineLocked`, Zielpfad-String) einmalig in `load()` bzw. beim ersten Zeichnen in `State` ablegen (neue Felder `importRoot`, `importRelDir`, `importTarget`) und bei Re-Load neu berechnen.
- **Erwarteter Effekt**: Schätzung: mehrere Syscalls und ~6 String-Allokationen pro Frame weniger, nur solange ein Raw-Bild-Tab sichtbar ist. Auf Netzlaufwerken/Windows spürbar.
- **Risiko / Verifikation**: Gering; Projektwechsel bei offenem Tab muss `State` neu laden (`forget()`/Reload existiert). `tests/test_texture_viewer.cpp` deckt die reine Pixelarbeit ab, nicht diese Pfade.

---

### EDT-1709 – TextureViewer: Schachbrett als Tausende `AddRectFilled` pro Frame
- **ID**: EDT-1709
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TextureViewerPanel.cpp:202-220`
- **Problem**: Das Schachbrett ist standardmäßig an (`checker = true`) und zeichnet jede dunkle Zelle als einzelnes Rechteck. Die Zellgröße ist ≥ 6 px; ein Bild, das 1500×900 px einnimmt, ergibt ~19 k Rechtecke (~75 k Vertices, 110 k Indizes) in jedem Frame, selbst wenn nichts passiert.
- **Code-Auszug**:
```cpp
for (int cy = y0; p0.y + cy * kCell < b.y; ++cy)
    for (int cx = x0 + ((x0 + cy) & 1); p0.x + cx * kCell < b.x; cx += 2)
        ... dl->AddRectFilled(c0, c1, IM_COL32(105, 105, 105, 255));
```
- **Lösung**: Statt Rechtecken ein 2×2-Texel-Muster als Textur mit Wiederholung per UV nutzen (eine `AddImage` pro Bild, Zellgröße über UV-Skalierung). Falls der `IRenderer::CreateImGuiTexture` keinen Repeat-Sampler liefert (unsicher, pro Backend prüfen), Alternative: die dunklen Zellen pro Zeile zu einer Polyline/`PathLineTo`-Liste bündeln oder nur Zellen im Clip-Rechteck (schon umgesetzt) und bei Zoom-out die Zellgröße auf z. B. 12 px anheben, damit weniger Zellen entstehen.
- **Erwarteter Effekt**: Schätzung: 20-30 k Vertices weniger pro Frame, CPU-Zeit im ImGui-Draw-Aufbau grob 0,5-2 ms; unsicher/messen mit Profiler und 4K-Textur im Fit-Modus.
- **Risiko / Verifikation**: Optik (Zellgröße, Ausrichtung an der Bildecke, Kommentar Z. 197-201) bleibt Anforderung. Screenshot-Vergleich über `HE_UI_DUMP_DIR`; der Software-Rasterizer der Tests kennt kein Repeat-Sampling, die Textur-Variante braucht daher einen Test-Hook oder Fallback.

---

### EDT-1710 – Sculpt-Kernel: fünf Kopien derselben Rasterschleife
- **ID**: EDT-1710
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/TerrainTools.cpp:885-1016`, Gesamtfunktion `:339-1048`
- **Problem**: `sculptInViewport` ist eine ~700-Zeilen-Funktion mit Einrückung bis Ebene 6. Die Texel→Welt-Umrechnung, `d2` und `brushWeight(d2)` stehen fünfmal wortgleich in Smooth, Flatten, Ramp, Roughen und Raise/Lower. Die Hauptschleife unterscheidet sich nur im pro-Texel-Update. Der Block „Landscape brush cursor + sculpt“ ist zusätzlich in ein `#ifdef HE_IMGUI_ENABLED` und mehrere `if` verschachtelt.
- **Code-Auszug**:
```cpp
const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
const float wz = terrainWorldPos.z - tcHalfZ + static_cast<float>(zi) * tcStepZ;
const float d2 = (hitWS.x-wx)*(hitWS.x-wx) + (hitWS.z-wz)*(hitWS.z-wz);
const float w  = brushWeight(d2);
if (w <= 0.0f) continue;
```
- **Lösung**: Einen Helper `forEachBrushTexel(grid, rect, hit, fn(index, wx, wz, d2, w))` (Lambda/Template, kein `std::function`) einführen, der das Rechteck aus EDT-1701 iteriert; jedes Tool wird zu einem 3-Zeiler. Außerdem die Berechnung (Sampler, Hit-Test, Cursor, Stroke) in separate freie Funktionen (`findTerrainHit`, `drawBrushCursor`, `applyBrush`) zerlegen; Brush-Mathematik in `HE_Scene` (`TerrainSculpt.h`) verschieben, dann testbar ohne ImGui (Paint/Foliage/Water haben das bereits: `TerrainPaint`, `FoliagePaint`, `WaterBrush`).
- **Erwarteter Effekt**: ~120 Zeilen weniger, Voraussetzung für EDT-1701; Verhalten gleich (Vereinfachung).
- **Risiko / Verifikation**: Reihenfolge der Float-Operationen beibehalten, sonst bitweise Abweichungen; Regressionstest wie in EDT-1701 (Golden-Heights je Tool).

---

### EDT-1711 – Duplizierte Blöcke im Landscape-Panel
- **ID**: EDT-1711
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/TerrainTools.cpp:1800-1817, 1855-1864, 1908-1917` (`track`/`trackFog`/`trackCau`), `:1445-1451` vs `:1524-1530` (Clear Paint), `:592-612` vs `:511-522` (Water-Ring ohne `projectPt`), `:1819-1949` (Physics/Fog/Caustics-Zeilen)
- **Problem**: (a) Drei identische Lambdas (`IsItemActivated → capturePre+stashPre`, `IsItemDeactivatedAfterEdit → commitPending`), nur der Undo-Name unterscheidet sich. (b) Der „Clear Paint“-Handler ist an zwei Stellen mit gleichem Rumpf kopiert. (c) Der gestrichelte Wasserring projiziert den Punkt von Hand (`clip`, `n`, `rectMin + …`), obwohl `projectPt` genau das kann; der einzige Unterschied ist die Höhe (`level` statt `sampleH`).
- **Code-Auszug**:
```cpp
const auto trackFog = [&] {
    if (!ctx.undoSys) return;
    if (ImGui::IsItemActivated()) { ctx.undoSys->capturePre(); ctx.undoSys->stashPre("Underwater Fog"); }
    if (ImGui::IsItemDeactivatedAfterEdit()) ctx.undoSys->commitPending();
};
```
- **Lösung**: Eine freie Funktion `trackUndo(AppContext&, const char* name)` nach `EditorWidgets` oder in den Undo-Helper; `projectPt` um optionalen Höhen-Parameter ergänzen (`projectAt(x, y, z, out)`), Clear-Paint in kleine Lokalfunktion `clearPaint(ctx, tc)`. Gleiches Muster `physDrag` für Fog- und Caustics-Zeilen (`rowLabel + DragFloat + track + helpForLabel`), dadurch ~100 Zeilen weniger.
- **Erwarteter Effekt**: Schätzung ~100-150 Zeilen weniger; keine Laufzeitänderung.
- **Risiko / Verifikation**: `editor_help_audit.py --check` muss weiter alle Labels finden, da `helpForLabel(literal)`-Aufrufe in Helper wandern; Labels dann als Parameter durchreichen (Audit liest Literale; vorher prüfen, ob es Aufrufe über Helper erkennt, sonst `physDrag`-Muster beibehalten, das hier schon so funktioniert).

---

### EDT-1712 – ThemeAssetPanel: Styleliste baut pro Frame Strings und Vektoren für alle Widget-Typen
- **ID**: EDT-1712
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ThemeAssetPanel.cpp:320-343` (Typen-Schleife), `:382-394` (By-name), `:139-144` (`isTypeKey`)
- **Problem**: Pro Frame und Widget-Typ (zweistellig viele): `std::string type`, `tagsFor(type)` (neuer Vektor), `styleMut(type)` (Map-Lookup mit möglichem Insert), Header aus bis zu fünf Teil-Strings plus `std::to_string`. In der „By name“-Schleife ruft jeder Style-Key `uiThemeSelectorType(key)` (Allokation) und `isTypeKey`, das die gesamte Typregistry mit `uiWidgetTypeName` durchsucht: O(Styles × Typen) pro Frame. Zusätzlich `ImGui::Begin(("##themeasset_" + assetPath).c_str(), …)` pro Frame und `std::string("Radius ") + …` in der Größenschleife.
- **Code-Auszug**:
```cpp
const std::vector<std::string> tags = st.theme.tagsFor(type);
...
std::string header = type + "  (" + std::to_string(base.values.size()) + " values";
```
- **Lösung**: Die Typliste (Name, Header) pro Frame nur neu bilden, wenn sich `values.size()`/Varianten ändern (Cache im `PanelState`, Invalidierung über `dirty`/Zähler); `isTypeKey` über ein statisches `unordered_set<string>` der Typnamen; Fenster-ID im `PanelState` halten. Reicht oft: `tagsFor` nur innerhalb des geöffneten Headers aufrufen (die Anzahl im Header-Text wird gebraucht → dann doch Cache).
- **Erwarteter Effekt**: Schätzung: einige hundert Allokationen pro Frame weniger; nur im offenen Theme-Tab, daher Niedrig.
- **Risiko / Verifikation**: Header-ID `###type` bleibt stabil; UI-Test mit Variante hinzufügen/entfernen.

---

### EDT-1713 – Landscape-Panel: Kleine Per-Frame-Allokationen
- **ID**: EDT-1713
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TerrainTools.cpp:121-138` (`layerColour`), `:149-154, 157-170` (`upperCase`/`sectionLabel`), `:1292, 1297-1298` (`matName`, `layers`), `:1375` (Button-Label), `:2110-2133` (`id`-Puffer)
- **Problem**: `layers` ist eine Vollkopie von `lmat->graphLayerNames` pro Frame, `matName` eine Stringkopie, `layerColour` erzeugt pro Layer-Zeile und Frame einen Kleinbuchstaben-String und hasht ihn bei unbekannten Namen. `sectionLabel` allokiert pro Aufruf per `upperCase`. Der Material-Button baut `matName + "##lsmat"` jedes Frame.
- **Code-Auszug**:
```cpp
const std::vector<std::string> layers = lmat ? lmat->graphLayerNames : std::vector<std::string>{};
```
- **Lösung**: `const std::vector<std::string>* layers` bzw. `static const` leerer Vektor; `layerColour` Ergebnis im Panel pro Layer cachen (nur bei Materialwechsel neu); `sectionLabel` mit `const char*`-Großschreibung in lokalem Puffer (`char t[64]`).
- **Erwarteter Effekt**: Schätzung: ~10-20 Allokationen pro Frame weniger; unerheblich ohne Messung (nur Tool-Panel). Reine Hygiene.
- **Risiko / Verifikation**: Zeiger auf `lmat` nur innerhalb des Frames gültig (ContentManager kann Assets verschieben, vgl. Kommentar in `ThemeAssetPanel.cpp:444-445`): nicht über Frames halten.

---

### EDT-1714 – TutorialSteps::panelVisited allokiert zwei Strings pro Aufruf
- **ID**: EDT-1714
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TutorialSteps.cpp:103-120`; Aufrufer `TutorialSteps.cpp:1123`, `TutorialPanel.cpp:364, 397, 738, 868`
- **Problem**: Pro Aufruf werden `needle` und `hay` aufgebaut (zwei Heap-Allokationen), nur um einen Ganzwort-Match in `visited` zu machen. Bei einem `PanelsVisited`-Step mit vier Panels wird das pro Frame ≈ 4 × 4 = 16-mal aufgerufen (Prüfung, Highlight, Karte, Statuszeile).
- **Code-Auszug**:
```cpp
std::string needle; ... needle += '\n'; needle.append(name); needle += '\n';
std::string hay; ... hay += '\n'; hay.append(visited);
return hay.find(needle) != std::string::npos;
```
- **Lösung**: Ohne Allokation: Treffer an Zeilenanfang (`visited.starts_with(name)` und nächstes Zeichen `'\n'`) oder an beliebiger Stelle `"\n"+name+"\n"` durch Schleife über `visited.find(name, pos)` mit Prüfung der Nachbarzeichen. Alternativ zeilenweise iterieren und `==` vergleichen.
```cpp
for (size_t p = 0; (p = visited.find(name, p)) != npos; ++p)
    if ((p == 0 || visited[p-1]=='\n') && p+name.size() < visited.size() && visited[p+name.size()]=='\n') return true;
```
- **Erwarteter Effekt**: ~32 Allokationen pro Frame weniger auf PanelsVisited-Steps; nahezu unmessbar. Nur wegen Einfachheit vorschlagen.
- **Risiko / Verifikation**: `tests/test_tutorial.cpp` enthält Tests für `panelVisited` („Scene“ vs „Scene Settings“); diese decken die Kantenfälle (erster Eintrag, letzter Eintrag, Teilwort).

---

### EDT-1715 – TextureColourSpaceDialog: synchrones I/O im UI-Thread, mehrfache Header-Reads
- **ID**: EDT-1715
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/TextureColourSpaceDialog.cpp:173-198` (`openRetag`), `:79-119` (`applyImport`), `:121-149` (`applyRetag`), `:268-293` (Tabelle)
- **Problem**: `openRetag` liest pro Asset bis zu dreimal die Datei (`textureSrgbOf`, `sourceFileOf` in `suggestionFor`, `suggestTextureSrgb`) und sortiert danach; `applyImport` ruft `importSource` für jede Datei synchron im UI-Thread (Texturdekodierung, bei 100+ Bildern sekundenlanger Freeze); `applyRetag` schreibt jede Datei synchron. Die Tabelle zeichnet alle Zeilen ohne Clipper; `pending` wird jeden Frame per `count_if` berechnet und der Button-Text als String gebaut. Es sind einmalige Dialogaktionen, daher niedrige Schwere.
- **Code-Auszug**:
```cpp
r.current   = Importer::textureSrgbOf(a);
r.suggested = suggestionFor(a, Mode::Retag);   // sourceFileOf + suggestTextureSrgb
...
if (Importer::importSource(r.path, s_root, r.relDir, {}, options)) ++imported;
```
- **Lösung**: Pro Asset die Datei einmal öffnen und beide Werte (Flag, Quelle) aus einem Read ableiten (neue Importer-Funktion `readTextureMeta`). Import in den vorhandenen Job-/Hintergrund-Pfad (wie Content Browser Reimport mit `AssetWriteLease`) verlagern und Fortschritt im Dialog zeigen. Zeilen mit `ImGuiListClipper` (konstante Zeilenhöhe: `rowH`).
- **Erwarteter Effekt**: Schätzung: Retag-Öffnen bei 1000 Texturen 3000 → 1000 Reads; UI bleibt beim Import reaktiv.
- **Risiko / Verifikation**: Hintergrundimport berührt Collab-Arbitration (`beginBackgroundWrite`) und `contentRefreshPending`; Reihenfolge der Reward-Momente (`fire`) beibehalten. Mit `tests/` für Importer-Optionen prüfen (`textureSrgb`).

---

### EDT-1716 – ThemeAssetPanel: `ensureTypeStyles`/`defaultValueOf` konstruieren Elemente pro Eigenschaft
- **ID**: EDT-1716
- **Kategorie**: C / A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/ThemeAssetPanel.cpp:107-123, 128-134, 183-217`
- **Problem**: `ensureTypeStyles` ruft pro Typ `styleableProps(t)` (erzeugt ein Element über `makeUIElement`, Eigenschaftsliste kopiert, Duplikat-Check linear in `out`) und danach pro fehlender Eigenschaft `seedValue → defaultValueOf → makeUIElement(t)` erneut. Bei einem frischen Theme: Typen × Eigenschaften Elementkonstruktionen (hunderte `unique_ptr<UIElement>`), plus O(n²)-Dedup. Das passiert beim Öffnen und bei jedem „Start from“.
- **Code-Auszug**:
```cpp
const std::unique_ptr<HE::UIElement> e = HE::makeUIElement(type);   // defaultValueOf, pro Eigenschaft
```
- **Lösung**: Pro Typ ein Element erzeugen und `getPropAny` für alle Eigenschaften darauf aufrufen (`seedValue(const UIElement&, name)`); Dedup über `unordered_set<string>`.
- **Erwarteter Effekt**: Schätzung: hunderte → ~Typenzahl Allokationen beim Öffnen; einmaliger Vorgang (Millisekunden). Nur Vereinfachung/Hygiene.
- **Risiko / Verifikation**: Reihenfolge der Eigenschaften (Basis zuerst) beibehalten, da UI-Reihenfolge davon abhängt; Test: Theme laden, Eigenschaftsliste vergleichen.

---

### EDT-1717 – Asset-Tab-Boilerplate in Theme-/Type-Panel (und weiteren Panels) dupliziert
- **ID**: EDT-1717
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/ThemeAssetPanel.cpp:454-496, 498-535` vs `src/HE_Editor/TypeAssetPanel.cpp:286-395, 397-436`; Header `ThemeAssetPanel.h:20-32` vs `TypeAssetPanel.h:19-47`
- **Problem**: `isDirty`, `reloadFromDisk`, `appendDirtyPaths`, `appendSnapshots`, `save`, `forget`, `isXAsset` sowie Fenster-Kopf (`SetNextWindowPos/Size`, `Begin("##x_"+path)`, Toolbar `assetHeader`/`saveButton`) sind nahezu identisch; die Kommentare verweisen auf `InputAssetPanel` als weitere Kopie („same shape“, `TypeAssetPanel.cpp:307-309`). Jede neue Asset-Art kopiert ~100 Zeilen.
- **Code-Auszug**:
```cpp
bool ThemeAssetPanel::isDirty(const std::string& path) { return s_states.dirty(path); }
bool ThemeAssetPanel::reloadFromDisk(const std::string& assetPath) { auto* st = s_states.find(assetPath); if (!st) return false; st->loaded = false; st->dirty = false; return true; }
```
- **Lösung**: Template `AssetTabPanel<State, Traits>` (oder Makro-freie Basisfunktionen in `EditorPanelState.h`/`AssetAutosave.h`), das `isDirty/reload/forget/appendDirtyPaths/save` und den Fensterrahmen liefert; die Panels liefern nur `load`, `drawBody`, `encode` (Traits). Nicht auf einmal migrieren, erst Theme+Type, dann die übrigen Tab-Panels (Slices anderer Auditoren).
- **Erwarteter Effekt**: ~80 Zeilen weniger je Panel; kleineres Risiko bei neuem Panel (z. B. vergessenes `appendSnapshots`).
- **Risiko / Verifikation**: Reine Refaktorierung; Dirty-/Snapshot-Tests (`tests/`, Autosave) müssen unverändert grün sein. Einheitliche Semantik für „nicht meins = true“ beibehalten.

---

### EDT-1718 – Fehlerhaftes Encoding und veraltete/irreführende Kommentare
- **ID**: EDT-1718
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/TypeAssetPanel.cpp:543`; `src/HE_Editor/TutorialPanel.h:39-41` vs `TutorialPanel.cpp:726`; `src/HE_Editor/TerrainTools.cpp:2164-2166`; `src/HE_Editor/ThemeAssetPanel.cpp:539, 630-631`; `ThemeAssetPanel.cpp:146-178, 346`; `TutorialSteps.cpp:484, 517`
- **Problem**:
  - `TypeAssetPanel.cpp:543`: der Gedankenstrich ist doppelt kodiert (Bytes `C3 A2 C2 80 C2 94` statt `E2 80 94`), die UI zeigt „Project default â save.create() …“ (sichtbarer Fehler).
  - `TutorialPanel.h:39-41` sagt „Draws nothing while closed, but the signal sampling still runs“, der Code bricht bei `!s_open` vor `sample()` ab (`TutorialPanel.cpp:726`); nur die Play-Session-Zählung läuft weiter.
  - `TerrainTools.cpp:2164-2166` („the undo step before“) beschreibt `finishImport`, das keinen Undo-Schritt macht; er steckt in den Aufrufern.
  - `ThemeAssetPanel.cpp:539` sagt in der UI „Nine roles“, der Kommentar in `:630` „the ten roles“; `styleValueRow` hat ungenutzten Parameter `key` (`(void)key;`), `:346` nutzt `const_cast` statt `styleMut`.
  - `TutorialSteps.cpp:484, 517`: Kapitelnummer „7.“ zweimal.
- **Code-Auszug**:
```cpp
ImGui::TextUnformatted("Project default â save.create() uses this template.");  // Bytes C3 A2 C2 80 C2 94
```
- **Lösung**: Zeichen durch `"\xe2\x80\x94"` ersetzen (so macht es der Rest der Datei, z. B. `:252`); Kommentare an den Code anpassen; Parameter `key` entfernen, `const_cast` durch `styleMut(key)` ersetzen.
- **Erwarteter Effekt**: sichtbarer UI-Fix plus weniger Irreführung für Wartende.
- **Risiko / Verifikation**: Trivial. Der Help-Audit liest nur Labels, nicht diesen Text.

---

### EDT-1719 – Schwere Header für kleine Abhängigkeiten
- **ID**: EDT-1719
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Performance (Build-Zeit)
- **Ort**: `src/HE_Editor/TerrainTools.h:2`, `src/HE_Editor/TextureViewerPanel.h:2-3`
- **Problem**: `TerrainTools.h` zieht `HorizonScene/HorizonWorld.h` (ECS, entt) nur für `Entity`; `TextureViewerPanel.h` inkludiert `EditorUI.h` und `imgui.h` für `AppContext`/`ImVec2`, obwohl `struct AppContext;`/`struct ImVec2;` Vorwärtsdeklarationen genügen würden (so macht es `TerrainTools.h` schon für ImVec2). Jeder Includer erbt die Kosten. `TerrainTools.cpp` hat zudem `<cfloat>`/`<cctype>` etc. nötig, aber `<ContentManager/Assets.h>` und `<Types/Enums.h>` sind nur teilweise genutzt (unsicher).
- **Code-Auszug**:
```cpp
#include <HorizonScene/HorizonWorld.h>   // Entity
```
- **Lösung**: `Entity` über den schmalen Header deklarieren, der den Alias definiert (entt-Forward, `HorizonScene/Entity.h` falls vorhanden; sonst anlegen) bzw. `entt::entity` als Parameter; in `TextureViewerPanel.h` `EditorUI.h`/`imgui.h` durch Vorwärtsdeklarationen ersetzen, `std::function`-Hooks bleiben.
- **Erwarteter Effekt**: Schätzung: kürzere Inkrementalbuilds des Editors; nicht gemessen.
- **Risiko / Verifikation**: Include-Fehler in abhängigen `.cpp` (z. B. `EditorUI.cpp`) beim Build sichtbar; `g++ -fsyntax-only` je Includer.

---

## Gelesen
Ja, alle 16 Dateien des Slice vollständig gelesen (TerrainTools.cpp/.h, TextureColourSpaceDialog.cpp/.h, TextureViewerPanel.cpp/.h, ThemeAssetPanel.cpp/.h, ToolSetupDialog.cpp/.h, TutorialPanel.cpp/.h, TutorialSteps.cpp/.h, TypeAssetPanel.cpp/.h); zusätzlich gezielt nachgeschlagen: `HcEditorUtil.cpp:40-100`, `TypeRegistry.cpp` (getEnum/nameCollides), `WaterField.cpp:196`, `TerrainPaint.cpp:31-60`, `GitProbe.h`.

## Editor Slice 18 – UIEditorPanel.cpp/.h, UITimelineMath.h, UndoHistoryPanel.*, ViewportActions.*, ViewportOverlays.*, ViewportPanel.*

## Zusammenfassung

Der Slice ist funktional gut durchdacht und ausführlich kommentiert, aber `UIEditorPanel.cpp` (7731 Zeilen, vier Zuständigkeiten in einer anonymen Namespace) rechnet und allokiert im Designer jeden Frame Dinge, die sich nur bei Edits ändern: ein Verzeichnisbaum-Scan samt Header-Lesen für die Palette, ein O(n²)-Hierarchiebaum ohne Clipping, ein O(n·Tiefe·n)-Aufbau der Zeichenliste (lineares `UIWidgetTree::find`) und tiefe Kopien eingebetteter Widgets pro `WidgetRef` und Frame. Im Viewport läuft `EditorSelection::roots()` (quadratisch) jeden Frame, und auf macOS wird pro Frame ein kompletter Szenenwalk nur für ein Menü-Label gemacht. `UITimelineMath.h`, `ViewportActions.*` (außer `anyHidden`) und `UndoHistoryPanel` sind sauber; dort nur Kleinigkeiten. Funde: 5 Hoch, 9 Mittel, 7 Niedrig (21 gesamt). Alle Laufzeit-Schätzungen sind ohne Messdaten und als solche markiert.

---

### EDT-1801 – Palette scannt pro Frame den gesamten Content-Baum und liest jeden .hasset-Header
- **ID**: EDT-1801
- **Kategorie**: D (auch E: blockierende I/O im Draw)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:7378-7380` (Aufruf), `src/HE_Editor/HcEditorUtil.cpp:45-67, 93-100` (Implementierung)
- **Problem**: Im Designer-Zweig von `render()` wird die „User Defined"/„Components"-Liste der Palette in jedem Frame mit `HcEditorUtil::listAssets()` neu aufgebaut. Das ist ein `recursive_directory_iterator` über `contentRoot()` **und** `engineContentRoot()`, pro `.hasset` ein Datei-Open für `readAssetTypeFromFile`, dazu `std::filesystem::relative`, `file_size` und mehrere String-Allokationen pro Treffer. Die Palette ist praktisch immer sichtbar, solange ein Widget-Tab offen ist.
- **Code-Auszug**:
```cpp
const auto widgets = HcEditorUtil::listAssets(ctx.contentManager,
                                              HE::AssetType::Widget);
...
for (const auto& a : widgets)
    if (a.path != st.relPath && !isEngine(a.path)) drawOne(a);
```
```cpp
std::filesystem::recursive_directory_iterator it(root, ec), end;
...
if (HAsset::readAssetTypeFromFile(it->path().string(), assetType) && ...
```
- **Lösung**: Die Liste im `State` (oder in einem kleinen prozessweiten Cache) halten und nur neu aufbauen, wenn (a) der Tab neu geöffnet wird, (b) die Content-Generation sich ändert (es gibt `EditorAssetTypeCache` mit Invalidierung, dort einen Zähler hochziehen bzw. den vorhandenen Invalidierungs-Hook verwenden), oder (c) als Fallback höchstens alle ~2 s. Skizze:
```cpp
struct WidgetList { std::vector<HcEditorUtil::ClassRef> items; uint64_t gen = ~0ull; };
// im State: WidgetList paletteWidgets;
const uint64_t gen = EditorAssetTypeCache::generation();   // neu / vorhandener Invalidierungs-Zähler
if (st.paletteWidgets.gen != gen) { st.paletteWidgets.items = HcEditorUtil::listAssets(...); st.paletteWidgets.gen = gen; }
```
  Gleiches Muster für `assetSlot` (878) und das Theme-Popup (7096): dort läuft der Scan nur bei geöffnetem Combo/Popup, also deutlich billiger, aber auch pro Frame solange offen.
- **Erwarteter Effekt**: Entfällt ein Verzeichnis-Walk mit N Dateiöffnungen pro Frame. Bei Projekten mit tausenden Assets plausibel Millisekunden bis zweistellige ms pro Frame (Schätzung, OS-Dateicache abhängig; messen).
- **Risiko / Verifikation**: Neue Widgets/Umbenennungen müssen in der Palette erscheinen: Invalidierung bei Asset-Erzeugen/Löschen/Rename prüfen (oder Zeit-Fallback). UI-Test `tests/test_*_ui.cpp` für UIEditorPanel falls vorhanden; Messung mit `HE_PROFILE_SCOPE` um den Aufruf.

---

### EDT-1802 – Hierarchie: `childrenOf()` (O(n)) pro Knoten und Frame, kein Row-Clipping
- **ID**: EDT-1802
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:922-1082` (insb. 927, 934, 1078-1081), Aufrufer `7496-7499`; Ursache `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:21-26`
- **Problem**: `drawHierarchyNode` ruft pro Knoten `st.tree.childrenOf(nodeId)` auf – das scannt **alle** Elemente und baut einen `std::vector<int>`. Bei n Elementen sind das n Scans pro Frame, also O(n²) plus eine Heap-Allokation pro Knoten. Dazu pro Knoten ein `std::string label` (`elementName(*n) + "##hn" + to_string(id)`) und eine `std::string` für die Context-Popup-ID (`1049`) – allein beim Zeichnen. Es gibt kein Clipping (CLAUDE.md §4: „Row-heavy panels draw only visible rows"); `TreeNodeEx` wird für jede Zeile aufgerufen, auch die weit außerhalb des Fensters. `find(nodeId)` ist zusätzlich linear (siehe EDT-1803).
- **Code-Auszug**:
```cpp
UIElement* n = st.tree.find(nodeId);
...
const auto children = st.tree.childrenOf(nodeId);
...
const std::string label = elementName(*n) + "##hn" + std::to_string(nodeId);
```
```cpp
std::vector<int> UIWidgetTree::childrenOf(int parentId) const
{ std::vector<int> out; for (const auto& e : elements) if (e->parentId == parentId) out.push_back(e->id); return out; }
```
- **Lösung**: Einmal pro Frame (oder nur bei Struktur-Änderung, Revisionszähler am Tree/`commitEdit`) einen Kinderindex bauen: `std::unordered_map<int, std::vector<int>>` bzw. dicht indiziertes `std::vector<std::vector<int>>` über `id < tree.nextId`. `drawHierarchyNode` bekommt den Index und die `UIElement*` direkt (kein `find`). Für Clipping: Baum in eine flache Liste sichtbarer Zeilen (Tiefe, id, offen?) abflachen und mit `ImGuiListClipper` zeichnen; Open-State pro Knoten wie bisher über `TreeNodeEx`-ID (konstante Zeilenhöhe beibehalten, vgl. `OutlinerPanel.cpp`). Label ohne Allokation: `ImGui::PushID(nodeId)` + `TreeNodeEx(name.c_str(), flags)` statt String-Konkatenation; Popup-ID als `"##hctx"` unter PushID.
- **Erwarteter Effekt**: O(n²) → O(n) bzw. O(sichtbare Zeilen) pro Frame; bei Seiten mit einigen hundert Elementen vermutlich spürbar (Schätzung: n=500 → 250k Element-Besuche und 500 Vektor-Allokationen pro Frame heute).
- **Risiko / Verifikation**: Drag&Drop-Dropzonen (drei Zonen pro Zeile, 959-1025) und „DefaultOpen"-Verhalten müssen identisch bleiben; geclippte Zeilen dürfen keine Drop-Targets verlieren (während eines Drags Clipper auf volle Liste umschalten oder Zeilen nahe der Maus immer zeichnen). UI-Test mit `ImGuiSoftwareRaster` (CLAUDE.md §4).

---

### EDT-1803 – Zeichenliste des Canvas: Tiefenwalk mit linearem `find` pro Element und Frame
- **ID**: EDT-1803
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:5188-5206` (drawCanvas), `4755-4771` (drawEmbeddedTree), `5216-5218`, `5366-5382`; Ursache `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:16-19`, `src/HE_Core/include/.../GraphModel.h` (`findNodeById`, linearer Scan)
- **Problem**: Für jedes sichtbare Element läuft ein Eltern-Walk (`depth < 255`), und jeder Schritt ist ein lineares `st.tree.find(parentId)`. Das sind O(n · Tiefe · n) Vergleiche pro Frame – nur um die Paint-Reihenfolge zu bestimmen, die sich nur bei Struktur-/Layer-Edits ändert. `uiElementEffectiveVisible`, `uiElementEffectiveOpacity/Enabled`, `uiElementRect` (rekursiv über Eltern) und `uiElementClipRect` laufen pro Element ebenfalls über `find`. Danach kommt ein `std::stable_sort` über die ganze Liste (Allokation) – jeden Frame. Dasselbe Muster steckt im Embedded-Pfad (4757-4771) pro eingebettetem Widget.
- **Code-Auszug**:
```cpp
for (const auto& ep : st.tree.elements)
{
    const UIElement& n = *ep;
    if (!HE::uiElementEffectiveVisible(st.tree, n)) continue;
    int depth = 0;
    for (const UIElement* c = &n; c->parentId != 0 && depth < 255; ++depth)
    {
        const UIElement* p = st.tree.find(c->parentId);
        if (!p) break;
        c = p;
    }
    items.push_back({ &n, n.layer * 256 + depth, depth, elementCanvasRect(st.tree, n, layoutCanvas) });
}
std::stable_sort(items.begin(), items.end(), ...);
```
- **Lösung**: (1) In `UIWidgetTree` einen Id→Index-Lookup führen (Ids sind dichte Ints aus `nextId++`, siehe `appendNode`; ein `std::vector<UIElement*>` indiziert nach Id, bei `add`/`removeSubtree`/`moveElement` invalidiert, wird alle `find`-Aufrufer in allen Layout-Funktionen von O(n) auf O(1) bringen – das ist die eigentliche Ursache, ein Modul-übergreifender Fix, siehe Cross-Modul-Hinweise). (2) Im Panel Tiefe und Paint-Reihenfolge nur neu berechnen, wenn sich Struktur/Layer geändert haben (Revisionszähler, den `commitEdit`/`restoreSnapshot`/`markEdited` hochzählt; während eines Drags ändern sich nur Position/Größe, nicht die Reihenfolge); die Rects bleiben pro Frame (werden vom Drag und von AutoSize geändert). (3) Tiefe lässt sich in einem Pass über die Wurzeln berechnen (DFS), statt pro Element nach oben zu laufen.
- **Erwarteter Effekt**: Von kubisch-ähnlich (n·d·n) auf linear pro Frame; spürbar ab ~200–300 Elementen (Schätzung; messen mit `HE_PROFILE_SCOPE` um 5188-5206).
- **Risiko / Verifikation**: Paint-Reihenfolge muss bit-gleich bleiben (stabile Sortierung nach `layer*256+depth`; Gleichstand = Einfügereihenfolge). Test: Szene mit gemischten Layern/Tiefen vor/nach vergleichen; `tests/test_ui_widgets.cpp` (nur gezielt lesen) deckt Layout ab. Index-Invalidierung bei jedem `elements`-Mutator sicherstellen (Collab-Sync schreibt über `CollabDocSync::forUIWidgetTree`, ebenfalls prüfen).

---

### EDT-1804 – Eingebettete Widgets werden pro `WidgetRef` und Frame tief kopiert und neu gelayoutet
- **ID**: EDT-1804
- **Kategorie**: C / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:4641-4653` (laidEmbeddedCopy), `4698-4798` (drawEmbeddedTree, Kopie bei 4732), `4666-4690` (refreshDesignSlots, Kopie bei 4685), Aufrufe `5186`, `5232-5239`
- **Problem**: Für jede sichtbare `WidgetRef` erzeugt `drawEmbeddedTree` per `HE::UIWidgetTree laid = tree;` eine tiefe Kopie (alle Elemente klonen, Strings, Property-Maps, Animationen, Params) und führt `uiApplyAutoSize` + `uiUpdateScrollExtents` darauf aus – in jedem Frame. Danach folgen Item-Liste, Sortierung und – pro eingebettetem Element – nochmals `drawElementIn` (Textur-Lookup). `refreshDesignSlots` wiederholt die Kopie (+ Layout) für jede Ref mit Kindern, ebenfalls pro Frame, und schleift zusätzlich für jede Ref über alle Elemente („hasChild"). Der Kommentar nennt das Beispiel „a page of thirty form rows": dort sind das 30 tiefe Baumkopien plus Auto-Size pro Frame, obwohl sich weder der Embed-Asset noch die Parameter ändern. (Wegen Rekursion bis `kMaxDepth = 4` multipliziert sich das bei verschachtelten Komponenten.)
- **Code-Auszug**:
```cpp
HE::UIWidgetTree laid = tree;
if (ref && !ref->paramValues.empty())
    HE::uiApplyWidgetParams(laid, ref->paramValues);
applyDesignWrites(laid, designOffset);
HE::uiApplyAutoSize(laid, &canvas);
HE::uiUpdateScrollExtents(laid);
return laid;
```
- **Lösung**: Ergebnis cachen, Schlüssel = (Ref-Id, Embed-Asset-Revision, Parameter-Revision, Slot-Größe in Canvas-Einheiten, Design-Run-Revision). Cache lebt im `State` (oder `static unordered_map` analog `embeddedTreeFor`) und wird verworfen, sobald einer der Schlüssel wechselt; `laid` samt vorberechneter `items` (Rect, Reihenfolge) wird dann nur noch gezeichnet. Zusätzlich `refreshDesignSlots` und `drawEmbeddedTree` dieselbe gecachte `laid`-Kopie benutzen lassen (heute rechnen beide unabhängig dasselbe aus). Die Schlüssel-Komponenten existieren großteils schon (`designDocSeen`, `designEmbedNow`).
- **Erwarteter Effekt**: Pro Frame null Kopien/Layout-Pässe für unveränderte Refs statt K tiefer Kopien (Schätzung; für Seiten mit vielen Komponenten wahrscheinlich der größte Einzelposten im Canvas, messen).
- **Risiko / Verifikation**: Hoch genug, dass Tests nötig sind: Parameteränderung, Bearbeitung des eingebetteten Assets in anderem Tab, NamedSlot-Füllung durch Host-Kinder (4738-4753, hängt vom Host ab → in Schlüssel aufnehmen), Preview-Canvas-Größenwechsel, Animations-Scrub (`ScrubPreview` schreibt Werte der Host-Elemente; Embed-Werte nicht) müssen weiter aktualisieren. Gefährlich: veraltete Anzeige bei vergessenem Schlüssel.

---

### EDT-1805 – `EditorSelection::roots()` jeden Frame im Viewport, quadratisch in der Selektionsgröße
- **ID**: EDT-1805
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ViewportPanel.cpp:1535-1560` (Aufruf pro Frame im View-Mode), Ursache `src/HE_Editor/EditorSelection.cpp:75-102`
- **Problem**: In jedem Frame im View-Mode wird `movable = ctx.selection.roots(reg)` neu berechnet und danach gefiltert (Lock-Komponente, ggf. Collab-Locks). `roots()` läuft für jedes Mitglied die Elternkette hoch und ruft pro Schritt `contains(p)` auf – und `contains` ist ein linearer `std::find` über `m_entities`. Das ist O(S · Tiefe · S). Nach „Select All" (Ctrl+A, `ViewportActions::selectAll`) mit z. B. 10 000 Entities sind das ~10⁸·Tiefe Vergleiche **pro Frame**, nur um dem Gizmo eine Liste zu geben. Auch `appendSplineGuides` ruft `selection.contains(e)` pro Spline (ViewportOverlays.cpp:261) und hat denselben linearen Zugriff.
- **Code-Auszug**:
```cpp
std::vector<Entity> movable = ctx.selection.roots(ctx.world->registry());
```
```cpp
Entity p = e;
for (int steps = 0; steps < kMaxDepth; ++steps) {
    ...
    p = h->parent;
    if (contains(p)) { covered = true; break; }   // contains = std::find über m_entities
}
```
- **Lösung**: Zwei Hebel. (1) `EditorSelection` hält zusätzlich ein `std::unordered_set<Entity>` (oder einen Revisionszähler plus gecachte `roots`), `contains` wird O(1), `roots` O(S·Tiefe). (2) Im Viewport `movable` nur neu bauen, wenn sich Selektion, Hierarchie (`markHierarchyDirty`-Zähler) oder Locks geändert haben; sonst die gecachte Liste verwenden. Der Gizmo selbst braucht die Liste nur bei nicht-leerer Selektion.
- **Erwarteter Effekt**: Bei kleinen Selektionen vernachlässigbar, bei Massenselektion Unterschied zwischen flüssig und Sekundenstottern (Schätzung nach Operationszahl; messen mit Ctrl+A in großer Szene).
- **Risiko / Verifikation**: Reihenfolge von `roots()` muss erhalten bleiben (Reihenfolge der Selektion, Primär zuletzt). Test in `tests/` für `EditorSelection::roots` (vorhandene Selection-Tests) erweitern: Selektion mit Eltern/Kind-Mix, Zyklus-Guard (`kMaxDepth`) beibehalten.

---

### EDT-1806 – Jeder Commit serialisiert Baum + Graph doppelt; Design-Run vergleicht pro Frame den vollen Snapshot
- **ID**: EDT-1806
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:487-503` (makeSnapshot/pushUndo), `571-589` (applyToAsset/commitEdit), `4492-4494` (refreshDesignTimeRun), `6905-6921`
- **Problem**: `commitEdit` ruft `pushUndo` (→ `makeSnapshot` = `uiWidgetTreeToJson` + `HC::toJson(graph)`) und danach `applyToAsset` (serialisiert **dieselben** beiden Dokumente nochmals). Jeder Commit (Drag-Release, Feld-Release, Key-Edit, Tippen-Ende) zahlt also zweimal die komplette Serialisierung, dazu `DesignValuesOut` (nimmt Vorschauwerte raus und wieder rein, zweimal verschachtelt). Der Undo-Stack hält bis zu 64 komplette Snapshots (Baum+Graph als einen String) pro Tab. Außerdem vergleicht `refreshDesignTimeRun` jeden Frame `doc == st.designDocSeen` – ein `memcmp` über den ganzen Snapshot (Baum+Graph, potentiell hunderte KB) – nur um zu erkennen, ob `undoPos` weitergesprungen ist.
- **Code-Auszug**:
```cpp
void commitEdit(State& st, AppContext& ctx) { pushUndo(st); st.dirty = true; applyToAsset(st, ctx); }
...
a->treeJson  = HE::uiWidgetTreeToJson(st.tree);   // applyToAsset: erneut dieselben Daten
a->graphJson = HC::toJson(st.graph);
...
if (st.designWasOn && doc == st.designDocSeen && st.designEmbedNow == st.designEmbedSeen) return;
```
- **Lösung**: Snapshot als Paar `{treeJson, graphJson}` halten (statt `tree + '\x1f' + graph`), in `commitEdit` einmal serialisieren und `applyToAsset` mit denselben Strings befüllen (`a->treeJson = snap.tree;` bzw. move/copy). `pushUndo` vergleicht dann Teilstrings. Für `refreshDesignTimeRun`: einen `uint64_t st.docRevision` bei jedem `pushUndo`/`restoreSnapshot` erhöhen und statt String-Vergleich Revisionen vergleichen. `saveState` im Live-Preview-Pfad (EDT-1810) ebenfalls die Strings wiederverwenden.
- **Erwarteter Effekt**: Halbierte Commit-Kosten (Serialisierung ist der Hauptposten), kein O(Dokumentgröße)-Vergleich pro Frame. Editor-lokal, nicht jeder Frame außer dem Vergleich: Schweregrad Mittel.
- **Risiko / Verifikation**: Das Verhalten (Undo-Inhalt, Asset-Inhalt) bleibt identisch; Format der Snapshots ist nur intern (Undo wird nicht persistiert; aber `appendSnapshots` 6905 serialisiert separat → unverändert lassen). Test: Undo/Redo-Rundreise, `isDirty`, Autosave-Snapshot.

---

### EDT-1807 – Pro Element und Frame viele String-/Variant-Kopien in `drawElementPreview` / `drawElementIn`
- **ID**: EDT-1807
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:440-469` (prop*Or-Helfer), `3718-4400` (drawElementPreview), `4618-4627` (drawElementIn), `3893-3897`, `3943-3951`
- **Problem**: Jede Preview-Eigenschaft wird über `e.getProp("Name")` per String-Name geholt; das liefert einen `UIPropValue` **by value** (enthält `std::string s` und `std::vector<std::string> list`), und `propColorOr/propFloatOr` laufen zusätzlich durch `themedColor` → `uiThemeValueFor` (Theme-Style-Lookup). Ein Slider ruft das ~7×, ein Text/CheckBox mehrfach, `propStringOr` kopiert den String zusätzlich in einen `std::string`. TabBox und Accordion bauen pro Frame `std::vector<std::string> labels/bodies/slots`; `NamedSlot` baut den Caption-String; `DatePicker` ruft `dp->caption()` und `snprintf` pro Zelle (42 Zellen). `drawElementIn` baut pro Element mit Textur einen Pfad-String (`contentRoot() + "/" + n.texture`) und ruft `AssetThumbnailCache::image` (Map-Lookup mit `std::string`-Key + `EditorAssetTypeCache::is`) – jeden Frame. Es gibt kein Culling von Elementen außerhalb des sichtbaren Canvas-Bereichs (nur `PushClipRect`).
- **Code-Auszug**:
```cpp
float propFloatOr(const UIElement& e, const char* name, float fb)
{ const UIPropValue v = e.getProp(name);
  return themedFloat(e, name, v.type == UIPropType::Float ? v.f : fb); }
...
texHandle = AssetThumbnailCache::image(ctx.contentManager->contentRoot() + "/" + n.texture);
```
- **Lösung**: (1) Für die Preview-Pfade einen Ein-Aufruf-Reader bereitstellen, der pro Element die benötigten Properties einmal liest (oder `getProp` mit `const UIPropValue&`-Rückgabe/`tryGetFloat(name, out)`, ohne Kopie von String/List-Feldern). (2) Textur-Pfad einmal pro Element cachen (z. B. `n.textureAssetId` ist schon aufgelöst; `AssetThumbnailCache::image` zusätzlich mit asset-id/Hash-Schlüssel), mindestens den Absolutpfad nur bei Änderung neu bauen. (3) Elemente mit komplett außerhalb des Fensters liegendem `mn/mx` überspringen (Rect-Test gegen `dl->GetClipRectMin/Max`). (4) Label-/Body-Vektoren in TabBox/Accordion in wiederverwendeten `thread_local`/`static`-Puffern halten.
- **Erwarteter Effekt**: Weniger Heap-Traffic; je Element wenige Mikrosekunden, bei 300 Elementen messbar (Schätzung; messen).
- **Risiko / Verifikation**: Reine Lese-Optimierung, Ergebnisbild muss identisch bleiben: UI-Screenshot-Test (`HE_UI_DUMP_DIR`) vor/nach vergleichen.

---

### EDT-1808 – Ganze JSON-Dokumente werden pro Frame kopiert bzw. verglichen (Theme-Override, Embed-Cache)
- **ID**: EDT-1808
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:376-413` (resolvePreviewTheme), `4412-4428` (embeddedTreeFor); Aufrufer `5042`, `1854`, `4673`, `5235`, `6545`
- **Problem**: (a) `resolvePreviewTheme` läuft jeden Frame (aus `drawCanvas`, 5042) und macht `loadAsset(path)` plus `const std::string json = a ? a->json : std::string();` – eine **Kopie des gesamten Theme-JSON** (mehrere KB) – nur um sie danach mit `st.themeOverrideJson` zu vergleichen. Der Kommentar (400-401) begründet die Kopie damit, dass „one more loadAsset would move" den Zeiger; das gilt aber erst nach weiteren Loads, der Vergleich direkt nach dem Getter kommt ohne weiteren Load aus. (b) `embeddedTreeFor` ruft pro Aufruf `loadAsset(path)` und vergleicht `c.json != asset->treeJson` (voller memcmp über das Widget-JSON, ggf. 10–100 KB) – und wird pro Ref im Canvas (5235), in `refreshDesignSlots` (4673), pro verschachtelter Ref (4791) und in der Details-Ansicht (1854) pro Frame aufgerufen. Bei dreißig Refs sind das 30–60 volle Dokumentvergleiche pro Frame.
- **Code-Auszug**:
```cpp
const std::string json = a ? a->json : std::string();
if (path == st.themeOverrideFor && json == st.themeOverrideJson) return;
...
if (c.json != asset->treeJson) { c.json = asset->treeJson; c.ok = HE::uiWidgetTreeFromJson(...); }
```
- **Lösung**: (a) `const std::string* json = a ? &a->json : nullptr;` vergleichen und nur bei Änderung kopieren. (b) Cache-Schlüssel = Asset-Revision/Generation (falls `ContentManager` einen Änderungszähler pro Asset bietet; sonst einen Zähler in `UIWidgetAsset` beim Schreiben von `treeJson` hochzählen, `applyToAsset`/`saveAsset` setzen ihn) oder wenigstens Länge + 64-Bit-Hash alle 0,5 s (wie `designEmbedCheckAt` es schon tut). Alternativ den ganzen Lookup in `drawCanvas` einmal pro Frame pro Pfad memoisieren (`unordered_map<string, const UIWidgetTree*>` pro Frame).
- **Erwarteter Effekt**: Entfallende Mehr-KB-Kopien/-Vergleiche pro Frame (Schätzung: einige µs bis ~100 µs pro Ref, linear in Dokumentgröße).
- **Risiko / Verifikation**: Edits an eingebetteten Widgets in anderen Tabs müssen sofort sichtbar bleiben (der Kommentar verlangt das ausdrücklich) → Revisionszähler an allen Schreibstellen (`applyToAsset`, `saveAsset`, `reloadFromDisk`, Collab). Zeiger-Lebensdauer: `Cached&` zeigt in `static unordered_map`, Referenz stabil.

---

### EDT-1809 – Details-Panel kopiert Property-Tabellen und baut ID-Strings pro Frame
- **ID**: EDT-1809
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:1387-1516` (drawPropertyWidget), `1552-1557` (drawParamValues), `1839` (drawContentSection), `2544` (Tags), `2713-2715` (drawKeyEditor), `3517` (drawDetailsEvents), `3398`, `6301/6365/6687-6689`
- **Problem**: `n.properties()`, `target->properties()`, `allEvents()`, `allProperties()` und `uiBaseProperties()` liefern `std::vector<UIPropDesc>` bzw. Event-Vektoren **by value** (jeder Eintrag mit `std::string name`, evtl. Hilfetexten); sie werden pro Frame für das selektierte Element und – in `drawParamValues` – pro deklariertem Parameter jeder eingebetteten Komponente neu aufgebaut. Pro Property baut `drawPropertyWidget` zudem `"##p_" + pd.name` und `(pd.name + id).c_str()`-Strings. `drawKeyEditor` durchsucht pro Frame `allProperties()` linear nach dem Range des Tracks. `g_previewTheme->tagsFor(typeStyle)` (2545) kopiert die Tag-Liste jedes Frame für das Details-Panel.
- **Code-Auszug**:
```cpp
const std::vector<UIPropDesc> props = n.properties();          // 1839
const std::vector<UIPropDesc> props = target->properties();    // 1552, pro Parameter
const std::string id = "##p_" + pd.name;                       // 1389, pro Property
```
- **Lösung**: Die Descriptor-Tabellen sind pro Element-Typ konstant (`static const` pro `UIWidgetType` möglich) → `properties()` als `const std::vector<UIPropDesc>&` auf eine statische Tabelle liefern (Cross-Modul-Änderung in `HE_Core/UIWidget`), oder im Panel pro (Typ) cachen (`std::unordered_map<UIWidgetType, std::vector<UIPropDesc>>`). ID-Strings: `ImGui::PushID(pd.name.c_str())` plus kurzes festes Label `"##v"`.
- **Erwarteter Effekt**: Zehn bis hundert kleinere Allokationen pro Frame weniger im Details-Panel (Schätzung); nur bei selektiertem Element relevant → Mittel/Niedrig.
- **Risiko / Verifikation**: Wenn Descriptor-Tabellen dynamisch sind (Properties hängen vom Zustand des Elements ab, z. B. `ListView`-Spalten), darf nur gecacht werden, was zustandsunabhängig ist – `UIElement::properties()` vorher lesen (nicht in diesem Slice). Help-Audit: `python3 scripts/editor_help_audit.py --check` läuft über literale Labels, unverändert lassen.

---

### EDT-1810 – Live-Preview speichert auf dem UI-Thread synchron auf Platte
- **ID**: EDT-1810
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:7724-7726`, `551-569` (saveState)
- **Problem**: Bei Application-Projekten (`ctx.appLivePreview`) wird in `render()` jeden Frame geprüft, ob `st.dirty` und keine Maus/kein Feld aktiv ist; dann folgt `saveState` → `uiWidgetTreeToJson` + `HC::toJson` (dritte Serialisierung nach EDT-1806) + `contentManager->saveAsset()` (Datei-I/O) im Draw. Jeder Release/Feldende blockiert den UI-Thread für Serialisierung + Schreiben + anschließendes Preview-Rebuild des Hosts. Bei großen Widgets/Netzlaufwerken spürbar.
- **Code-Auszug**:
```cpp
if (ctx.appLivePreview && st.dirty &&
    !ImGui::IsAnyMouseDown() && !ImGui::IsAnyItemActive())
    saveState(st, ctx);
```
- **Lösung**: Entprellen (z. B. 300–500 ms nach dem letzten Commit) und die bereits aus `commitEdit` vorhandenen JSON-Strings wiederverwenden; das eigentliche Schreiben über den vorhandenen Autosave-Pfad (`AssetAutosave.h` ist im Header schon eingebunden) bzw. einen Hintergrund-Job laufen lassen. Das Verhalten „Preview aktualisiert sich nach Edit" bleibt, nur ohne Frame-Stall.
- **Erwarteter Effekt**: Kein Ruckeln beim Weiterarbeiten nach jedem Edit (Größenordnung: Dateischreibzeit + Serialisierung; unsicher, messen mit großem Widget).
- **Risiko / Verifikation**: Reihenfolge Preview-Rebuild/Save und der Satz „An unsaved change is an invisible one" (7716-7723) müssen gelten; Fehlerfall beim Schreiben wird heute über Rückgabewert gemeldet – bei asynchronem Pfad Fehlerbehandlung beibehalten.

---

### EDT-1811 – `entityActionState` macht auf macOS pro Frame einen vollständigen Szenenwalk für ein Menülabel
- **ID**: EDT-1811
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ViewportPanel.cpp:757-775`, `src/HE_Editor/ViewportActions.cpp:26-37, 133-142`, Aufrufer `src/HE_Editor/EditorUI.cpp:1991` (nativer Mac-Menüleisten-Sync pro Frame) und `2296` (nur bei geöffnetem Entity-Menü)
- **Problem**: `entityActionState()` berechnet alle Zustände auf einmal, u. a. `anyHidden` über `ViewportActions::anyHidden` (Rekursion per `std::function` über jede Entity der Hierarchie, je Entity sieben `try_get`-Zugriffe in `entityVisibility`, **kein Early-Exit** – die Lambda setzt nur `any = true`, der Walk läuft weiter) und `canUngroup`. Auf macOS ruft `EditorUI.cpp:1991` pro Frame `entityActionState(ctx).primaryLocked` auf, braucht also nur ein Bool, bezahlt aber den kompletten Walk. Selbst im ImGui-Menü (2296) wird der Walk nur zur Anzeige eines Enabled-Flags gemacht.
- **Code-Auszug**:
```cpp
st.anyHidden = editable && ViewportActions::anyHidden(*ctx.world);
...
forEachInScene(world, [&](Entity e)
{ if (!any && HE::entityVisibility(reg, e) == HE::Visibility::Hidden) any = true; });
```
```cpp
(ctx.world && sceneTabActiveNow && ViewportPanel::entityActionState(ctx).primaryLocked)
```
- **Lösung**: (1) `primaryLocked` als eigene billige Funktion (`isPrimaryLocked(ctx)`, nur zwei Registry-Zugriffe) anbieten und in `EditorUI.cpp:1991` verwenden. (2) `entityActionState` lazy machen: `anyHidden`/`canUngroup` nur berechnen, wenn das Menü offen ist (der Aufruf in 2296 ist ohnehin innerhalb `BeginMenu`, also bereits nur dann – dort bleibt es, ggf. mit Early-Exit). (3) `anyHidden` mit einem Early-Exit-Walk (iterativ mit Stack, ohne `std::function`) schreiben, `forEachInScene` ebenso ohne `std::function` (Template-Callable).
- **Erwarteter Effekt**: macOS: O(N·7 try_get) pro Frame entfällt; bei 50 k Entities grob 1–3 ms/Frame (Schätzung, nicht gemessen).
- **Risiko / Verifikation**: Nur Mac-Pfad bzw. Mac-spezifisch → auf macOS testen. `ViewportActions`-Tests (`tests/` nach „ViewportActions"/„isolate" suchen) prüfen `anyHidden`-Semantik (nur über die Hierarchie erreichbare Entities); ein Registry-View-Ansatz würde verwaiste Entities mitzählen → Verhalten ändern, nicht empfohlen ohne Test.

---

### EDT-1812 – Lokale Mesh-Bounds doppelt berechnet, vorberechnete Cooked-Bounds ignoriert, Caches ohne Invalidierung
- **ID**: EDT-1812
- **Kategorie**: A / C / D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Editor/ViewportPanel.cpp:246-272` (meshBounds), `1806` (einzige Invalidierung), `src/HE_Editor/ViewportOverlays.cpp:155, 197-220` (zweite Kopie), `src/HE_Core/include/ContentManager/Assets.h:144-152`
- **Problem**: `StaticMeshAsset` trägt für gekochte Meshes `boundsMin/boundsMax` („hold the precomputed AABB", Assets.h:146), und der Kommentar an `meshBounds` (249-250) sagt dasselbe („Cooked meshes … carry their bounds precomputed"). Der Code iteriert aber trotzdem über alle interleavten Vertices (`for i < vertexCount … box.expand(...)`). `ViewportOverlays::appendColliderWireframes` hat dieselbe Berechnung ein zweites Mal (eigener `static s_colliderMeshBox`, 212-218). Beide Caches kennen keine Invalidierung bei Reimport/Hot-Reload/Projektwechsel (nur `TerrainTools` löscht Einträge in `s_aabbCache`): nach einem Reimport bleibt die Box des alten Meshes stehen, was Picking, F-Framing, Marquee und Collider-Overlay verfälscht.
- **Code-Auszug**:
```cpp
if (mesh->cooked)
{
    for (uint32_t i = 0; i < mesh->vertexCount; ++i)
        box.expand({ mesh->interleaved[i * 8 + 0], mesh->interleaved[i * 8 + 1], mesh->interleaved[i * 8 + 2] });
}
```
- **Lösung**: Ein gemeinsamer Helper `HE::Ed::localMeshBounds(ContentManager&, const UUID&)` (z. B. in `ViewportPick.h/.cpp`), der für `cooked` direkt `boundsMin/boundsMax` liefert und sonst `AABB::fromPositions` nutzt; beide Aufrufstellen darauf umstellen, Cache an eine Content-Generation/Asset-Revision koppeln (Eintrag speichert Revision, Vergleich beim Zugriff).
- **Erwarteter Effekt**: Entfällt O(Vertexzahl) beim ersten Zugriff pro Mesh (einmalig, aber im UI-Thread-Frame), eine Code-Kopie weniger, korrekte Boxen nach Reimport.
- **Risiko / Verifikation**: Prüfen, dass `boundsMin/Max` beim Cooken exakt die Vertex-Hülle sind (Test: gekochtes Mesh, Box mit/ohne Helper vergleichen); ältere gepackte Assets ohne gesetzte Bounds (Nullwerte) → Fallback auf Iteration, wenn `boundsMin == boundsMax == 0`.

---

### EDT-1813 – `UIEditorPanel.cpp` ist ein 7700-Zeilen-Monolith mit dreifach duplizierter Drop-Logik und überlangen Funktionen
- **ID**: EDT-1813
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp` (gesamt); `drawCanvas` 5026-5751 (~725 Zeilen), `drawDetails` 2019-2649 (~630), `render` 6933-7729 (~800), `drawElementPreview` 3718-4400 (~680); Duplikate: Drop-Annahme `1026-1041`, `5731-5748`, `7472-7494`; „Eltern aus Selektion" `1031/1038`, `7361-7363`, `7420-7422`
- **Problem**: Designer, Timeline, Palette, Details und Graph-Modus liegen in einer anonymen Namespace. Die Funktionen sind lang und tief verschachtelt (z. B. Drag-/Snap-Logik in `drawCanvas` mit drei Einrückungsebenen Lambdas). Dreimal dieselbe Verarbeitung der Payloads `HE_UIWIDGET_NEW`/`HE_UIWIDGET_REF` (nur Ziel-Parent und Commit-Zeitpunkt variieren), und die Regel „in den Container, sonst neben die Selektion" (`acceptsChildren() ? id : parentId`) ist viermal ausgeschrieben. Das verlangsamt auch jeden Rebuild der Datei (große TU) und erschwert Tests.
- **Code-Auszug**:
```cpp
const int parent = n->acceptsChildren() ? nodeId : n->parentId;           // 1031
if (const UIElement* selN = st.tree.find(st.selected))
    parent = selN->acceptsChildren() ? selN->id : selN->parentId;         // 7362, 7421
```
- **Lösung**: (1) Helfer `int dropParentFor(const UIElement*)` und `bool acceptNewOrRefDrop(State&, int parent, const ImVec2* pt, bool& structureEdit)` einführen und an den drei Stellen verwenden. (2) Datei in Übersetzungseinheiten schneiden (`UIEditorTimeline.cpp`, `UIEditorDetails.cpp`, `UIEditorCanvas.cpp`, `UIEditorGraphMode.cpp`) mit einem internen Header für `State` und die gemeinsamen Helfer (`commitEdit`, `selectionIds`, …). (3) Aus `drawCanvas` das Interaktionsblock (Klick/Drag/Snap, 5384-5716) in eigene Funktionen (`beginPress`, `applyDragFrame`, `endPress`) ziehen.
- **Erwarteter Effekt**: Wartbarkeit, bessere inkrementelle Build-Zeiten; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Rein mechanisch, aber die Help-Scopes (`HE::Ed::Help::Scope`, per Funktion) und `editor_help_audit.py` hängen an Funktionsnamen/Literalen: nach Umbau `python3 scripts/editor_help_audit.py --check` laufen lassen; `tests/test_*ui*.cpp` und `test_editor_help.cpp`.

---

### EDT-1814 – `ViewportPanel::render` (~930 Zeilen) mit doppelter Snap-Probe und gemischten `static`/anonymous-Konventionen
- **ID**: EDT-1814
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/ViewportPanel.cpp:922-1850` (render), Duplikat `1467-1502` vs. `1564-1616`; `static`-Funktionen (`meshBounds`, `snapSelectionToGround`, …) neben `namespace {}` (413-541)
- **Problem**: `render()` enthält Toolbar, Kamera-Eingabe, RMB-Kontextmenü-Zustandsautomat, Drop-Spawn (1312-1454), Gizmo, Picking/Marquee, Kontextmenü und Overlays in einer Funktion mit vielen Lambdas. Die Vertex-Snap-Probe (Terrain-Filter, `nearestVertex`-Aufruf, `screenRay`/`probeSurface`) steht zweimal fast wortgleich (Spline-Modus 1471-1501 und View-Modus 1567-1615). Der Drop-Spawn für Mesh vs. Prefab (1366-1447) dupliziert Logging/Selektion/`markHierarchyDirty`. Dazu mischen sich `static`-Freifunktionen und eine anonyme Namespace (413).
- **Code-Auszug**:
```cpp
const HE::ScenePick::VertexHit hit = HE::ScenePick::nearestVertex(
    sceneSnapshot(ctx), meshLookup(*ctx.contentManager), viewProj,
    rmin, rsize, at, s_tb.snapVertexRadiusPx,
    [&](const RenderObject& obj) { if (!base(obj)) return false; ... terrain ... });
```
- **Lösung**: `makeSnapProbe(AppContext&, const std::unordered_set<uint32_t>& exclude, rectMin, rectMax, bool surfaceRest, const std::vector<Entity>* movable)` als eine Funktion, die beide Aufrufer teilen; Drop-Spawn in `spawnDroppedAsset(ctx, path, mouse, rect)` auslagern; Kamera-/Kontextmenü-Block in `driveEditorCamera(...)`. Nebenbei: `static` vs. anonymous Namespace vereinheitlichen (docs/coding-conventions.md).
- **Erwarteter Effekt**: Kürzere, testbare Einheiten; kein Laufzeitunterschied.
- **Risiko / Verifikation**: Lambda-Captures per Referenz (`rectMin`, `movable`) – beim Herausziehen auf Lebensdauer achten; Headless-Dumps (`HE_DUMP_*`) und `tests/` für ViewportPick/Marquee/Actions als Regression.

---

### EDT-1815 – Overlays: alle Splines und alle Collider werden jeden Frame neu aufgebaut
- **ID**: EDT-1815
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ViewportOverlays.cpp:254-303` (appendSplineGuides, Curve 268-275), `147-253` (appendColliderWireframes, `worldMatrixOf` bei 166)
- **Problem**: `appendSplineGuides` erzeugt für **jede** Spline der Welt (nicht nur die selektierten) pro Frame ein `HE::spline::Curve` (Konstruktor nimmt den Control-Point-Vektor **by value**, d. h. Kopie + Span-Aufbau), tastet es mit `kSamplesPerSpan` pro Span in einen neuen `std::vector<glm::vec3>` ab und transformiert jeden Punkt mit der Weltmatrix. `selection.contains(e)` ist linear (siehe EDT-1805). `appendColliderWireframes` ruft pro Collider `worldMatrixOf` (Eltern-Kette) auf – bei tausenden Collidern pro Frame O(N·Tiefe).
- **Code-Auszug**:
```cpp
for (auto [e, spline] : reg.view<SplineComponent>().each())
{
    const bool selected = selection.contains(e);
    const glm::mat4 model = HE::worldMatrixOf(world, e);
    const HE::spline::Curve curve(spline);
    ...
    const std::vector<glm::vec3> pts = curve.sample(SplineEdit::kSamplesPerSpan);
```
- **Lösung**: Spline-Linien pro Entity cachen (Schlüssel: Control-Point-Revision/`closed` und Weltmatrix) oder nur Splines innerhalb eines Radius um den Viewer abtasten; Punktvektor wiederverwenden (`curve.sampleInto(buf)`), `Curve` aus `const&` bauen. Collider: Weltmatrix aus `TransformComponent::worldMatrix` verwenden, wenn die Transform-Propagation in diesem Frame schon lief (Kommentar sagt „a frame old" → bewusst frisch; nur mit Dirty-Flag optimieren).
- **Erwarteter Effekt**: Nur relevant in Szenen mit sehr vielen Splines/Collidern; ohne Messung gering (Schätzung). Unsicher / messen.
- **Risiko / Verifikation**: Die Linie muss exakt der Pick-Kurve entsprechen (Kommentar 17-18 in der Header-Doku); Cache darf `SplineEdit`-Picking nicht auseinanderlaufen lassen. Tests: vorhandene Spline-Overlay-Tests (`HE_DUMP_SPLINETEST`).

---

### EDT-1816 – Tote Null-Checks auf `tree.elements`, inkonsistent zu ungeschützten Zugriffen, mit irreführendem Kommentar
- **ID**: EDT-1816
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:1693, 2105-2109, 4449, 4480-4482, 4668-4670, 4742-4743, 6533-6534, 6542, 6565` (geschützt) vs. `4759, 5193, 5894, 6269` (ungeschützt); Kommentar `2101-2103`; `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:24-25, 49-51`
- **Problem**: Der Kommentar bei 2101-2103 behauptet, der Element-Vektor sei „indexed by id and holds a hole wherever one was deleted". `UIWidgetTree::removeSubtree` entfernt Einträge aber per `erase` (kein Loch), `add` hängt per `push_back` an, und `childrenOf`/`removeSubtree` dereferenzieren `e->` ohne Null-Check. Ein Teil des Panel-Codes prüft dennoch `if (ep)`, ein anderer Teil nicht (`const UIElement& e = *ep;`). Entweder sind die Checks tot (dann irreführend) oder es gibt einen Weg zu `nullptr` (dann sind die ungeschützten Stellen und `childrenOf` Abstürze).
- **Code-Auszug**:
```cpp
// Both counted over the same loop: the element vector is indexed by
// id and holds a hole wherever one was deleted, so its size is not ...
for (const auto& ep : st.tree.elements) if (ep) { ++total; ... }
...
const UIElement& n = *ep;     // 5193, ohne Check
```
- **Lösung**: Klären, ob `elements` je `nullptr` enthalten kann (Collab-Sync/`uiWidgetTreeFromJson` prüfen). Falls nein: Checks und Kommentar entfernen, `assert` in `UIWidgetTree::add`/Deserialisierung. Falls ja: überall prüfen oder (besser) die Invariante im Tree erzwingen.
- **Erwarteter Effekt**: Klarheit, weniger Branches; Absturzschutz je nach Ergebnis.
- **Risiko / Verifikation**: Unsicher bis `elements`-Mutatoren (grep `elements` in `UIWidgetTree.cpp`, `CollabDocSync`, MCP-Widget-Tools) geprüft sind. Test: Elemente löschen/Undo/Collab-Delta durchspielen.

---

### EDT-1817 – Timeline: pro-Frame-Vektor in `snapped`, Clip-Rect pro Key, kein Key-Culling
- **ID**: EDT-1817
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:3163-3172` (snapped), `3246-3249`, `3319-3329` (PushClipRect pro Key), `3250-3331` (Key-Schleife)
- **Problem**: Die Lambda `snapped` baut bei jedem Aufruf `std::vector<float> targets{0, duration}` und hängt alle Keys an – sie wird während Ruler-Scrub bzw. Key-Drag jeden Frame und pro aktivem Key aufgerufen. Pro Key wird `PushClipRect`/`PopClipRect` und `PushID` ausgeführt; Keys außerhalb des Lanes werden nur per `continue` übersprungen (richtig), aber das Clip-Rect pro Diamant wäre mit einem einzigen Rect um die Zeile erledigt. `drawKeyEditor` durchsucht `allProperties()` linear (siehe EDT-1809).
- **Code-Auszug**:
```cpp
auto snapped = [&](float t, int skipTrack, int skipKey)
{
    if (ImGui::GetIO().KeyAlt) return t;
    std::vector<float> targets{ 0.0f, clip.duration };
    for (...) targets.push_back(clip.tracks[ti].keys[ki].time);
    return HE::Ed::uiTimelineSnap(t, targets, 6.0f / view.pixelsPerSecond());
};
```
- **Lösung**: `targets` als wiederverwendeten Puffer (`static thread_local` oder im `State`) mit `clear()`/`reserve()`; `uiTimelineSnap` auf ein Callback/Span-Interface ohne Sammelvektor umstellen (iteriert direkt über Tracks); ein `PushClipRect` pro Track-Zeile statt pro Key.
- **Erwarteter Effekt**: Vernachlässigbar bei wenigen Keys; spürbar nur bei hunderten Keys (Schätzung).
- **Risiko / Verifikation**: `UITimelineMath.h` hat eigene Tests (`tests/` nach „timeline"); Snap-Ergebnisse (Tie-Break: erster gewinnt, `<=` beim ersten Treffer) exakt erhalten.

---

### EDT-1818 – Palette: Tile-Layout (String-Teilung, Textbreiten) wird pro Tile und Frame neu berechnet
- **ID**: EDT-1818
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:7220-7228` (matches), `7233-7295` (tile), `4887-4898` (spacedTypeName), `4910-4926` (paletteScreenScale)
- **Problem**: Für jedes der ~25 Tiles baut jeder Frame `spacedTypeName(typeName(t))` (String), prüft `widthOf` (jeweils `CalcTextSizeA`), baut `std::vector<std::string> lines` und kürzt per Schleife (`pop_back` + `widthOf(line + "...")`, also pro Iteration eine Allokation). `matches` lowercased pro Typ und Gruppe einen frischen String. `paletteScreenScale` iteriert pro Frame alle Monitore. Alles ist abhängig von (Tile-Breite, Schriftgröße, Scale), die sich praktisch nie ändern.
- **Code-Auszug**:
```cpp
std::string full = spacedTypeName(typeName(t));
std::vector<std::string> lines;
...
while (line.size() > 3 && widthOf(line) > maxW) { line.pop_back(); if (widthOf(line + "...") <= maxW) {...} }
```
- **Lösung**: Pro Typ ein kleines Cache-Objekt `{ std::string lines[2]; float width[2]; }`, neu aufgebaut nur bei Änderung von `maxW`/`fs`/Fontatlas; Lowercase-Name der Typen einmal in einer `static`-Tabelle halten; `paletteScreenScale` höchstens bei Fenstergrößen-/Monitor-Wechsel neu berechnen.
- **Erwarteter Effekt**: Einige zehn Allokationen und ~50 `CalcTextSizeA`-Aufrufe pro Frame entfallen (Schätzung).
- **Risiko / Verifikation**: Optisch identisch; Cache-Invalidierung bei UI-Font-Scale-Änderung (Preferences) nicht vergessen. Screenshot-Test.

---

### EDT-1819 – Undo-History-Panel ohne Row-Clipping; Format-String mit totem Argument
- **ID**: EDT-1819
- **Kategorie**: D / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/UndoHistoryPanel.cpp:77-90`, `104-116`, `58-61`
- **Problem**: Jede Undo-/Redo-Stufe wird jeden Frame als `Selectable` mit `PushID`, Tooltip-Abfrage und `label.c_str()` gezeichnet; es gibt kein Clipping (CLAUDE.md §4 verlangt das für zeilenlastige Panels; die Tiefe von `EditorUndo` ist hier nicht begrenzt sichtbar). Die `snprintf`-Zeile übergibt vier Argumente (`nUndo, "s", nRedo, nRedo==1?"":"s"`) für einen Format-String mit nur drei Platzhaltern – das vierte ist tot und „1 to redo" bekommt trotzdem nie ein Plural-Suffix; Verwechslungspotenzial.
- **Code-Auszug**:
```cpp
std::snprintf(head, sizeof(head), "%zu step%s to undo, %zu to redo",
              nUndo, nUndo == 1 ? "" : "s", nRedo, nRedo == 1 ? "" : "s");
```
- **Lösung**: Zeilenhöhe ist konstant → `ImGuiListClipper` über `nUndo` und `nRedo` (Undo-Zeilen: Index direkt; Redo-Zeilen: `redoLabelAt(nRedo - 1 - k)`); Format-String auf `"%zu step%s to undo, %zu step%s to redo"` korrigieren (oder das tote Argument streichen).
- **Erwarteter Effekt**: O(sichtbar) statt O(Tiefe) pro Frame; relevant nur bei tiefem Undo-Stack (Schätzung/unsicher: `EditorUndo`-Tiefe prüfen).
- **Risiko / Verifikation**: Sprunglogik `jumpBack/jumpForward` bleibt identisch; UI-Test.

---

### EDT-1820 – Kleinkram im Viewport: Grid-Cache wird pro Frame in den Zielpuffer kopiert, Stats-Overlay baut Strings
- **ID**: EDT-1820
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/ViewportPanel.cpp:103-115, 214`, `444-540`
- **Problem**: `appendGroundGrid` cached zwar die ~2000 Linien bei unveränderter Kameraposition, kopiert sie aber jeden Frame per `out.append(s_grid.lines())` in den Debug-Puffer; sobald die Kamera sich bewegt, wird das gesamte Gitter (≈12 Segmente × 160 Linien mit `sqrt`) neu gebaut. `drawStatsOverlay` baut pro Frame (nur bei eingeschaltetem Stats-Flag) einen `std::vector<std::string>` mit mehreren Konkatenationen (`thousands()` baut je Zahl einen String).
- **Code-Auszug**:
```cpp
if (s_gridBuilt && eye == s_gridEye) { out.append(s_grid.lines()); return; }
```
- **Lösung**: Optional: Gitter als eigener Zeichenlayer ohne Kopie (z. B. `DebugDrawBuffer::appendShared`/zweiter Puffer, den der Renderer liest). Stats: feste `char`-Puffer mit `snprintf` statt Stringkonkatenation. Beides sehr niedriger Gewinn → nur mitnehmen, wenn sowieso angefasst.
- **Erwarteter Effekt**: Sub-0,1-ms-Effekte (Schätzung); nicht ohne Messung umbauen.
- **Risiko / Verifikation**: Gitter-Test (`WorldPreviewGrid`/Grid-Test, falls vorhanden) für Linienanzahl/Farben.

---

### EDT-1821 – `refreshDesignTimeRun` hasht pro Ref das Embed-JSON doppelt pro 0,5 s und startet Sandbox bei jedem Commit
- **ID**: EDT-1821
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/UIEditorPanel.cpp:4464-4525` (Hashing 4476-4491, Sandbox 4500-4524)
- **Problem**: Alle 0,5 s wird für **jede** `WidgetRef` (auch mehrfach dasselbe Asset, z. B. 30 gleiche Form-Rows) `std::hash` über `treeJson` und `graphJson` berechnet und ein Key-String zusammengebaut – ohne Dedup nach Pfad. Danach läuft bei jedem veränderten Undo-Snapshot (also jedem Commit, nicht jedem Frame – gut) eine komplette Sandbox: `Runtime` + `WidgetManager` konstruieren, `runDesignTimePreConstruct` über das gesamte Widget samt Embeds. Die Kosten dieses Laufs sind in diesem Slice nicht sichtbar.
- **Code-Auszug**:
```cpp
key += ref->widgetPath + '#' +
       std::to_string(std::hash<std::string>{}(a->treeJson)) + '#' +
       std::to_string(std::hash<std::string>{}(a->graphJson)) + ';';
```
- **Lösung**: Nach Pfad deduplizieren (`std::unordered_set<std::string>` der schon gehashten Pfade). Mit Revisionszählern aus EDT-1808 entfällt das Hashing komplett. Sandbox-Laufzeit messen (`HE_PROFILE_SCOPE_N` um 4514) und bei Bedarf auf „nach Ende des Commit-Bursts" entprellen.
- **Erwarteter Effekt**: Gering, sofern die Ref-Zahl klein bleibt; unsicher.
- **Risiko / Verifikation**: Ergebnis (Key) muss sich bei Asset-Änderung weiterhin ändern; Test: zwei Tabs, Embed-Asset ändern.

---

## Gelesen

Ja, alle Dateien des Slice vollständig gelesen: `UIEditorPanel.cpp` (1–7731 in Blöcken), `UIEditorPanel.h`, `UITimelineMath.h`, `UndoHistoryPanel.cpp/.h`, `ViewportActions.cpp/.h`, `ViewportOverlays.cpp/.h`, `ViewportPanel.cpp` (1–1862) und `ViewportPanel.h`; zur Verifikation zusätzlich gezielt `UIWidgetTree.cpp`, `GraphModel.h`, `HcEditorUtil.cpp`, `EditorSelection.cpp`, `EntityVisibility.cpp`, `TransformHierarchy.cpp`, `Assets.h`, `EditorUI.cpp` (nur die zitierten Zeilen).

## Editor Slice 19 – ViewportPick.cpp/.h, ViewportToolbar.cpp/.h, ViewportViewMode.h (+ src/HE_Editor/CMakeLists.txt)

## Zusammenfassung
Der Slice ist insgesamt sauber und gut kommentiert. Der einzige echte Hot Path ist `ViewportToolbar::render` (läuft jeden Frame). Die teure Hilfe-Suche pro Zelle ist bereits per Hash-Index gelöst (`EditorHelp.cpp:9634`, Perf-Audit 3.2), sodass keine schweren Per-Frame-Kosten übrig sind. Die Funde betreffen vor allem eine 460-Zeilen-Funktion mit doppelt gepflegter Layout-Arithmetik, wiederholte Textmessung pro Frame, eine handgepflegte Duplikatliste der Show-Flags und schwere Header. `ViewportPick` läuft nur bei Klick (nicht pro Frame) und ist dort korrekt; ein möglicher Randfall (ungültige Entity im Snapshot) ist als "unsicher" markiert. Funde: 0 Hoch, 3 Mittel, 9 Niedrig (EDT-1901 bis EDT-1912), dazu vier Niedrig-Befunde zur CMakeLists im Abschnitt "Zusatzprüfung".

## Funde

### EDT-1901 – `render()` ist eine 460-Zeilen-Funktion mit doppelt geführter Breitenarithmetik
- **ID**: EDT-1901
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ViewportToolbar.cpp:639-1096 (Fit 680-743, Left-Zone 745-862, Right-Zone 966-1092)
- **Problem**: Jede Zonenbreite wird zweimal berechnet und von Hand synchron gehalten: einmal in den Lambdas `leftWidth`/`rightWidth` (705-733) und ein zweites Mal beim tatsächlichen Zeichnen (z. B. `wView/wLand/wSpl` in 752-755 und 771, Tools in 786 und 802, Snap in 825 und 861, Kamera in 974, View in 1001, Mode in 1034, Show in 1059). Ändert jemand eine Zelle, muss er 2-3 Stellen anfassen; eine Abweichung verschiebt unbemerkt die Mitten-Zentrierung (`wLeft`/`wRight`, 742-743, 868-872). Dazu kommt `x += ...` / `rx += ...` als manuell mitgeführter Cursor.
- **Code-Auszug**:
```cpp
auto leftWidth = [&](bool labels, bool snap) { float w = kWellPad * 2.0f + cellWidth(m, labels ? "View" : nullptr) + ...
...
well(m, x, kWellPad * 2.0f + wView + kSegGap + wLand + kSegGap + wSpl);
...
x += kWellPad * 2.0f + wView + kSegGap + wLand + kSegGap + wSpl + kGroupGap;
```
- **Lösung**: Jede Gruppe als kleine Struktur `Group { float width(bool labels, bool snap) const; void draw(float x, ...); }` bzw. als Tabelle `{label, icon, width}`. Die Breite einmal pro Gruppe berechnen und für Messung, `well()` und den Cursor-Vorschub wiederverwenden (`const float gw = group.width(...)`). Alternativ ein Zwei-Pass-Layout mit einem `Layout`-Objekt (`advance(w)`), das beim Messdurchlauf nur summiert und beim Zeichendurchlauf zeichnet. `render()` in `drawLeftZone/drawTransport/drawRightZone` zerlegen.
- **Erwarteter Effekt**: Schätzung: ca. 80-120 Zeilen weniger, deutlich geringere Fehleranfälligkeit; Laufzeit unverändert.
- **Risiko / Verifikation**: Pixel-genaues Layout muss erhalten bleiben. Absicherung über das UI-Test-Muster (`tests/test_*_ui.cpp`, `HE_UI_DUMP_DIR`-BMP-Vergleich) bei drei Balkenbreiten (voll, ohne Labels, ohne Snap) und mit/ohne Play-Modus.

### EDT-1902 – Textbreiten werden pro Frame bis zu dreifach neu gemessen
- **ID**: EDT-1902
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/ViewportToolbar.cpp:685-743, 752-754, 1001/1034 (zusätzlich `cellImpl` in EditorToolbar.cpp:79)
- **Problem**: `fits()` ruft bis zu dreimal `leftWidth()` und `rightWidth()` auf. `leftWidth` ruft bei Labels dreimal `cellWidth()` (je `CalcTextSize`), danach misst der Zeichenblock dieselben drei Labels erneut (752-754). Dazu `cellWidth` für "World"/"Local" (686), "Perspective"/"Bottom"/"Ortho" (717), "Wireframe"/"G-Buffer"/"Unlit" (723) und sechs weitere `CalcTextSize` (687-696). Die Ergebnisse hängen nur von Font und UI-Skalierung ab, nicht vom Frame. Zusammen sind es grob 40-60 `CalcTextSize`-Aufrufe pro Frame nur für die Leiste (unsicher / messen; der Perf-Audit hat die Toolbar schon einmal als 12 % der Main-Thread-CPU gemessen, ein Teil davon war die Hilfe-Suche, die inzwischen gehasht ist).
- **Code-Auszug**:
```cpp
const float orientW  = std::max(cellWidth(m, "World"), cellWidth(m, "Local"));
const float snapValW = std::floor(ImGui::CalcTextSize("88.88\xc2\xb0").x + kCellPadX * 2.0f);
...
const float viewW    = std::max({ cellWidth(m, "Perspective"), cellWidth(m, "Bottom"), cellWidth(m, "Ortho") });
```
- **Lösung**: Alle label-abhängigen Breiten in eine `struct Widths` packen, die nur neu berechnet wird, wenn sich `ImGui::GetFontSize()`/`m.cell`/Font-Pointer ändern (Schlüssel: `FontSize`, `FontScale`). `static Widths s_w; static float s_key;` im anonymen Namespace. Zusätzlich `leftWidth`/`rightWidth` nur einmal je (labels, snap)-Kombination auswerten (Ergebnis des ersten `fits()` weiterverwenden).
- **Erwarteter Effekt**: Schätzung: weniger als 0,05 ms pro Frame; nur lohnend, wenn die Messung die Leiste noch relevant zeigt. Zusammen mit EDT-1901 fast kostenlos.
- **Risiko / Verifikation**: Cache-Schlüssel muss UI-Font-Scale-Wechsel (Preferences) und DPI-Wechsel erfassen. Messen mit dem Profiler-Panel vor/nach.

### EDT-1903 – Show-Flag-Liste doppelt gepflegt (Popup vs. `showFlagFields`)
- **ID**: EDT-1903
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ViewportToolbar.cpp:458-494 gegen ViewportPanel.cpp:72-90 (`showFlagFields`)
- **Problem**: Das Popup listet jede Flag von Hand mit eigenem Label (464-482), während `showFlagFields` bereits Tabelle mit `member` und `label` ist. "Hide All" und die "anyOff"-Anzeige (1061-1069) nutzen die Tabelle, das Popup nicht. Folge: `streamingCells` steht in der Tabelle (ViewportPanel.cpp:84), fehlt aber im Popup (bewusst? Er hat einen eigenen Schalter im Streaming-Debug-Panel, StreamingDebugView.cpp:442). "Hide All" setzt ihn trotzdem auf false, und "Show All" stellt ihn über die Defaults zurück. Eine neue Flag muss an drei Stellen eingetragen werden; vergisst man das Popup, leuchtet die Zelle "anyOff" ohne dass der Nutzer sehen kann warum.
- **Code-Auszug**:
```cpp
EditorWidgets::checkbox("Ground Grid",    &f.groundGrid);
EditorWidgets::checkbox("Editor Icons",   &f.editorIcons);
...
if (EditorWidgets::menuItem("Hide All Overlays")) { ... f.*(fields[i].member) = false; }
```
- **Lösung**: Gruppenüberschrift als weiteres Feld in die Tabelle (`group` Index), das Popup iteriert `showFlagFields`, `EditorWidgets::checkbox(fields[i].label, &(f.*fields[i].member))`; Flags ohne Popup-Eintrag (`streamingCells`) per `hidden`-Bit markieren. Die Help-Einträge hängen am Label ("Viewport Show/<Label>") und bleiben gültig, solange die Label-Strings gleich bleiben.
- **Erwarteter Effekt**: Schätzung: ca. 15 Zeilen weniger, eine Pflegeliste statt drei.
- **Risiko / Verifikation**: `python3 scripts/editor_help_audit.py --check` (der Audit erkennt Literal-Labels; nach dem Umbau sind sie Tabellen-Literale, also prüfen, ob der Audit sie noch findet) und Sichtprüfung der Gruppenreihenfolge.

### EDT-1904 – Drei handgebaute "Text-in-Zelle"-Items neben `cell()`
- **ID**: EDT-1904
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: ViewportToolbar.cpp:831-847 (Snap-Wert), 878-902 (Play), 953-963 (Uhr)
- **Problem**: Snap-Wert, Spiel-Uhr und Play-Knopf wiederholen jeweils `SetCursorScreenPos` + `InvisibleButton` + Hover-Füllung + zentrierter `AddText`/Icon, was `cellImpl` (EditorToolbar.cpp:46-97) bereits kann (nur der Text-Zentrierung wegen mit `label` ohne `icon` und Breite `w` machbar: `contentW = labelW`, `left` zentriert). Zusätzlich trägt der Snap-Wert eigene BeginDisabled/Tooltip-Logik (835-839), die in `cellImpl` ebenfalls steht (Tooltip bei disabled unterdrückt).
- **Code-Auszug**:
```cpp
ImGui::SetCursorScreenPos(ImVec2(vx, m.y + kWellPad));
if (!snapUsable) ImGui::BeginDisabled();
const bool pressed = ImGui::InvisibleButton("##vpSnapValue", ImVec2(snapValW, m.cell));
...
dl->AddText(ImVec2(std::floor(vx + (snapValW - tw) * 0.5f), std::floor(m.cy - ImGui::GetFontSize() * 0.5f)), fg, buf);
```
- **Lösung**: `cell(m, vx, snapValW, "##vpSnapValue", nullptr, buf, false, snapUsable, "Snap increment...")` für den Snap-Wert; die Farbe `kFgOn` bei aktivem Snap über `cellTinted` (`fg`-Parameter existiert). Die Uhr: `cellTinted(..., clock.text, fg, true, nullptr)` plus `helpForKey`-Variante (cellImpl nimmt `helpKey`, `cellTinted` reicht ihn nicht durch; Signatur um `helpKey` erweitern). Hover-Füllung bei der Uhr bewusst nicht (Kommentar 949-951) -> ein `noHoverFill`-Flag wäre nötig, daher nur Snap-Wert sicher.
- **Erwarteter Effekt**: Schätzung: ca. 15-20 Zeilen weniger.
- **Risiko / Verifikation**: Optisch identisch halten (Text-Rundung `std::floor`); UI-Screenshot-Test.

### EDT-1905 – `ViewportToolbar.h` zieht ImGui, ImGuizmo und IRenderer in jeden Gizmo-/Panel-Include
- **ID**: EDT-1905
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides (Build-Zeit)
- **Ort**: src/HE_Editor/ViewportToolbar.h:38-43
- **Problem**: Der Header bindet `<imgui.h>`, `<ImGuizmo.h>` und `Renderer/IRenderer.h` ein, weil `State` `ImGuizmo::OPERATION/MODE` und `HE::ViewMode` als Werte hält. Er wird von EditorTransformGizmo.cpp, SplineTool.cpp, LevelScriptPanel.cpp und ViewportPanel.cpp gebraucht (Treffer in `grep ViewportToolbar::`), die alle nur `State` brauchen, nicht `render/viewPopup/...`. IRenderer.h ist ein schwerer Header.
- **Code-Auszug**:
```cpp
#include <imgui.h>     // ImGuizmo.h uses ImVec2/ImU32/ImDrawList without declaring them
#include <ImGuizmo.h>
#include "Renderer/IRenderer.h"   // HE::ViewMode
```
- **Lösung**: `State` in `ViewportToolbarState.h` auslagern (nur `ImGuizmo.h`-Enums bzw. eigene `enum class GizmoOp/GizmoSpace` und `HE::ViewMode` per Forward-Deklaration einer `enum class ViewMode : int;`, falls IRenderer.h sie so deklarieren lässt - Vorab prüfen). Die Funktionsdeklarationen bleiben in `ViewportToolbar.h`.
- **Erwarteter Effekt**: Schätzung: spürbar kürzere Inkrementalbuilds für 3-4 Übersetzungseinheiten; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Der Kommentar bei `activeSnap()` (104-105) erklärt, dass der Test den Gizmo, aber nicht die Leiste linkt; das Auslagern ändert daran nichts. Build aller Targets inkl. `he_tests` prüfen.

### EDT-1906 – `ViewportPick.h`: schwerer Include und nicht-const Registry
- **ID**: EDT-1906
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ViewportPick.h:2, 46, 52; ViewportPick.cpp:20-21
- **Problem**: (a) `pick`/`pickAtScreen` nehmen `entt::registry&`, nutzen aber nur `valid`, `all_of` und `HE::terrainOwnerOf(const entt::registry&, ...)` (TerrainChunkComponent.h:39), alles const. Die Signatur lügt über Mutation und verhindert Aufrufe mit const-Welt. (b) Der Header zieht `HorizonWorld.h` (ganze ECS-Welt) nur für `Entity`/`entt::registry`.
- **Code-Auszug**:
```cpp
Entity pick(const RenderWorld& snapshot, entt::registry& reg, const BoxLookup& boxes, ...
```
- **Lösung**: `const entt::registry&` in beiden Funktionen. Optional `using Entity = entt::entity;` über `<entt/entity/fwd.hpp>` statt `HorizonWorld.h` (Aufrufer binden die Welt ohnehin ein). Der Aufruf in ViewportPanel.cpp:1670 übergibt `ctx.world->registry()`; `const` bindet dort ohne Änderung.
- **Erwarteter Effekt**: Schätzung: marginal (API-Ehrlichkeit, etwas Buildzeit).
- **Risiko / Verifikation**: Reiner Signaturwechsel; Tests, die `ViewportPick::pick` aufrufen (grep in `tests/`), kompilieren weiter.

### EDT-1907 – `pick`: pro Objekt `glm::inverse(mat4)` und `std::function`-Aufruf vor dem Billig-Reject
- **ID**: EDT-1907
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/ViewportPick.cpp:25-39
- **Problem**: Jedes Snapshot-Objekt bekommt eine volle 4x4-Inversion plus zwei Matrix-Vektor-Produkte und einen `std::function`-Aufruf (LRU-/Map-Lookup in `meshBounds`), bevor der Strahltest läuft. Bei Szenen mit zehntausenden Objekten (Terrain-Chunks, Foliage ist ggf. nicht im Snapshot; unsicher) kostet jeder Klick diese Schleife komplett. Läuft aber nur bei Mausklick/Kontextmenü (ViewportPanel.cpp:1759, 1783), nicht pro Frame, daher niedrige Schwere.
- **Code-Auszug**:
```cpp
const glm::mat4 invModel = glm::inverse(obj.transform);
const glm::vec3 o = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
```
- **Lösung**: Nur wenn gemessen relevant: grober Vorfilter im Weltraum (Weltraum-AABB aus `box.transformed(obj.transform)` ist teurer als die Inversion, daher lieber Bounding-Sphere-Radius um `obj.transform[3]`; ein Strahl-Kugel-Test ist exakt konservativ und billig), dann erst die Inversion. Alternativ affine Inversion (`glm::affineInverse`), falls Transforms garantiert affin sind (RenderObject-Transform ist es; vor Umstellung bestätigen, bei Projektionsanteil wäre das Ergebnis falsch).
- **Erwarteter Effekt**: Schätzung: bei 10k Objekten wenige 100 µs pro Klick eingespart; für den Nutzer nicht spürbar.
- **Risiko / Verifikation**: Trefferergebnis muss bit-identisch bleiben (Distanz `t` in Einheiten von |rayDir|). Bestehende Pick-Tests (`grep ViewportPick tests/`) reichen; zusätzlich einen Test mit skaliertem/rotiertem Objekt. Messen vor Umsetzung.

### EDT-1908 – Lock-/Terrain-Block in `pick` verschachtelt und mit überflüssigen `valid`-Prüfungen
- **ID**: EDT-1908
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ViewportPick.cpp:41-65
- **Problem**: Zwei `reg.valid`-Prüfungen (43 und 55), ein eigener Block-Scope nur für `owner`, und zwei getrennte Verzweigungen (58-65) über dieselbe Variable `terrainOwner`. Außerdem: Ist `e` ungültig (veraltete Entity im Snapshot), wird sie ohne Sperr-Check als Treffer zurückgegeben (siehe "Unsicher").
- **Code-Auszug**:
```cpp
if (reg.valid(e)) { terrainOwner = HE::terrainOwnerOf(reg, e); ... }
{
	const Entity owner = (terrainOwner != entt::null) ? terrainOwner : e;
	if (reg.valid(owner) && reg.all_of<EditorLockComponent>(owner)) continue;
}
```
- **Lösung**: Früh aussteigen und ein Treffer-Ziel bündeln:
```cpp
if (!reg.valid(e)) continue;                       // (Verhalten ändert sich, siehe Unsicher)
Entity owner = HE::terrainOwnerOf(reg, e);
const bool isTerrain = owner != entt::null || reg.all_of<TerrainComponent>(e);
if (owner == entt::null) owner = e;
if (reg.all_of<EditorLockComponent>(owner)) continue;
float& best = isTerrain ? terrainDist : meshDist;
Entity& hit = isTerrain ? terrainHit : meshHit;
if (t < best) { best = t; hit = owner; }
```
- **Erwarteter Effekt**: Schätzung: ca. 10 Zeilen weniger, eine Verzweigung weniger.
- **Risiko / Verifikation**: `continue` bei ungültiger Entity ändert beobachtbares Verhalten (heute: ungültige Entity kann gewinnen). Ohne diese Änderung bleibt der Rest gleich; absichern mit einem Pick-Test, der ein Snapshot-Objekt mit gelöschter Entity enthält.

### EDT-1909 – View-Mode-Kürzel stehen als Text in der Leiste, die Tasten im Panel
- **ID**: EDT-1909
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: ViewportToolbar.cpp:609-617 und 513-519; Gegenstück ViewportPanel.cpp:1283-1284, 806
- **Problem**: "Alt+4/3/2" und "Num 5/7/1/3" sind Literale in der Leiste, die tatsächlichen Tasten sind in ViewportPanel.cpp fest verdrahtet (`ImGuiKey_3`, `ImGuiKey_4`, `ImGuiKey_Keypad7`), nicht in der Shortcut-Tabelle `EditorShortcuts.cpp`. Ändert jemand die Tasten, zeigt das Menü Falsches. Die Kollision mit Bookmark-Tasten ("1".."0" bzw. Ctrl+Ziffer, 545-556) ist nur durch die Alt-Modifikation gelöst.
- **Code-Auszug**:
```cpp
{ VM::Lit, "Alt+4" }, { VM::Unlit, "Alt+3" }, { VM::Wireframe, "Alt+2" },
```
- **Lösung**: View-Mode-/Preset-Shortcuts als Einträge in `EditorShortcuts.cpp` (`viewport.viewmode.lit` usw.) anlegen und `shortcutLabel(...)` (EditorHelp.cpp:9703) für die Menüanzeige nutzen; ViewportPanel fragt dieselbe Tabelle.
- **Erwarteter Effekt**: Konsistenz, rebindbare Tasten; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Eingabe-Verhalten muss gleich bleiben (Alt+3 bleibt Unlit). `tests/` für Shortcuts prüfen, ob neue IDs Konflikt-Tests auslösen.

### EDT-1910 – Rohe `ImGui::`-Controls und Tooltips statt `EditorWidgets` im Options-Popup
- **ID**: EDT-1910
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: ViewportToolbar.cpp:415-427, 433-434, 440-442, 546, 557, 838
- **Problem**: CLAUDE.md Abschnitt 4 verlangt Editor-Controls über `EditorWidgets`. Hier stehen `ImGui::InputFloat`, `ImGui::SliderFloat`, `ImGui::MenuItem`, `ImGui::SetTooltip("The rotate gizmo's outer ring...")` und `SetItemTooltip` roh; der Tooltip bei 434 ist ein Literaltext statt eines Help-Eintrags (landet nicht in F1). Die Clamps (421-423, 427) laufen jeden offenen Popup-Frame auch ohne Edit.
- **Code-Auszug**:
```cpp
EditorWidgets::checkbox("Screen-space rotation ring", &st.rotateScreenRing);
if (ImGui::IsItemHovered())
	ImGui::SetTooltip("The rotate gizmo's outer ring, which rotates about the view axis");
```
- **Lösung**: Tooltip in einen `EditorHelp`-Eintrag ("Viewport Options/Screen-space rotation ring") verschieben (Scope ist bereits gesetzt, 409); Float-Eingaben über vorhandene EditorWidgets-Float-Variante, falls vorhanden (nicht geprüft), sonst belassen. Clamp in ein `sanitizeSnap(State&)` ziehen, das auch `snapPresets` (399) benutzt.
- **Erwarteter Effekt**: Konsistenz, F1-Hilfe; kein messbarer Laufzeiteffekt.
- **Risiko / Verifikation**: `python3 scripts/editor_help_audit.py --check`.

### EDT-1911 – `ViewportViewMode.h`: ungenutztes `<cstring>`, Namensvergleichskette
- **ID**: EDT-1911
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ViewportViewMode.h:22-24, 34-40
- **Problem**: `<cstring>` wird nicht benutzt (kein `str*`/`mem*`-Aufruf im Header). `viewModeFromName` besteht aus sieben if-Zeilen mit denselben Mustern wie `HE::viewModeName`; die Kurznamen ("basecolor", "roughspecmetal") sind eine eigene Tabelle, was eine Pflegestelle mehr ist als nötig. `viewModeOverrideFromEnv` hält zwei statische Zeiger mit Thread-Safe-Init-Guard, wird pro Frame aufgerufen (ViewportPanel.cpp:958) - der Guard-Check kostet ein Atomic-Load, unerheblich.
- **Code-Auszug**:
```cpp
#include <cstring>
...
if (s == "lit")            return HE::ViewMode::Lit;
```
- **Lösung**: `<cstring>` entfernen. Optional `constexpr std::pair<std::string_view, HE::ViewMode> kNames[]` + Schleife (spart 5 Zeilen, ermöglicht Test über die Tabelle). Kein Perf-Gewinn erwartet.
- **Erwarteter Effekt**: Schätzung: vernachlässigbar.
- **Risiko / Verifikation**: Bestehende Tests in tests/test_editorcamera.cpp:245ff decken `viewModeFromName`/`viewModeOverride` ab.

### EDT-1912 – Spielclock-Text und Icon-Trigonometrie werden jeden Frame neu gebaut
- **ID**: EDT-1912
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: ViewportToolbar.cpp:333-359, 135-163 (iconCube/iconGlobe), 106-120, 190-199
- **Problem**: `gameClockState` formatiert pro Frame mit `snprintf("%g×")` einen String, auch wenn sich der Wert nicht ändert; `iconGlobe` berechnet 24x `cos`/`sin`, `iconCube` 6x, `iconRotate`/`iconEye` weitere Bogenberechnungen, ebenfalls pro Frame. Summe: grob 100-150 Trigonometrie-Aufrufe und ein `snprintf` pro Frame. Das ist weit unter 0,05 ms (unsicher / messen); als Doku-Fund aufgenommen, weil die Leiste bereits einmal im Perf-Audit auffiel.
- **Code-Auszug**:
```cpp
for (int i = 0; i < 24; ++i)
{
	const float a = i * (2.0f * kPi / 24.0f);
	e[i] = ImVec2(c.x + std::cos(a) * r * 0.45f, c.y + std::sin(a) * r);
}
```
- **Lösung**: Nur wenn ein Profil es zeigt: `static const` Einheitskreistabelle (`constexpr`-Sinus/Cosinus-Tabelle für 24 Punkte) und Uhrtext nur bei geändertem `scale` neu formatieren (statisches Cache-Paar `lastScale/text`).
- **Erwarteter Effekt**: Schätzung: unter 0,01 ms; nicht umsetzen ohne Messung.
- **Risiko / Verifikation**: Pixelgleichheit der Icons (Rundung); sonst keines.

### Zusatzprüfung CMake / nicht gelisteter Dateien in src/HE_Editor
- Nicht in einer Slice-Liste von `Editor.txt` stehen (außer `vendor/`, laut CLAUDE.md übersprungen): nur `src/HE_Editor/CMakeLists.txt` (732 Zeilen). Keine `*.mm`, `Guides/` oder weiteren Dateitypen fehlen (`Guides/*.cpp`, `*.mm` und `EditorSystemMotion.*` sind in der Liste). `vendor/` (imgui, imguizmo, ImGuiColorTextEdit, stb_image.h) wurde absichtlich nicht gelesen.
- Befund CMake (Niedrig, Typ Vereinfachung, Kategorie F): `add_executable(HorizonEditor ...)` listet in Zeile 399 eine zweite Block-Liste von ca. 55 Headern (`"EditorUI.h" "EditorUI.cpp" "ScriptEditorPanel.h" ...`), die eine unvollständige, historisch gewachsene Header-Spiegelung ist (nur für IDE-Projektbäume). `EditorUI.cpp` steht nur dort, nicht bei den übrigen `.cpp`; ebenso `EditorUI.h` an `imgui_editor` (Zeile 67), obwohl `imgui_editor` ausschließlich Vendor-Quellen baut. Vorschlag: `EditorUI.cpp` in die normale Quellenliste verschieben, die Headerliste entfernen (CMake braucht Header nicht) oder per `file(GLOB ... CONFIGURE_DEPENDS *.h)` ersetzen; `"EditorUI.h"` aus `imgui_editor` entfernen. Risiko: IDE-Projektstruktur ändert sich; Build-Verifikation aller Plattformen nötig.
- Befund CMake (Niedrig, Performance Build, Kategorie D): `copy_directory "${EDITOR_DEPS_DIR}"` läuft bei jedem Link doppelt (Zeilen 519-524) und der gesamte `${DEPLOY_GAME}`-Baum (609-612) sowie die SDK-Header (632-645, inklusive `rm -rf` und vollständigem Neukopieren von `src/HE_Core/include` und `glm`) bei jedem Editor-Relink; `copy_directory` kopiert ohne Vergleich. Das verlängert jeden inkrementellen Editor-Link (unsicher / messen, Größe von `EditorDeps` und `DEPLOY_GAME` nicht geprüft). Vorschlag: `copy_directory_if_different` (CMake >= 3.26) für die Verzeichnisse und das `rm -rf` des SDK nur bei geänderten Headern; eigenes Custom-Target `editor_deploy`, das nicht am Link-Schritt hängt.
- Befund CMake (Niedrig, Kategorie F): `target_compile_definitions(HorizonCore PRIVATE HE_EDITOR)` (Zeile 489) setzt ein Flag auf ein Fremd-Target aus dem Editor-Verzeichnis; die Reihenfolge der `add_subdirectory`-Aufrufe wird dadurch implizit zur Voraussetzung. Besser in `src/HE_Core/CMakeLists.txt` hinter einer Option.
- Befund CMake (Niedrig, Kategorie F): Zeilen 36-43 und 87-101 pflegen die Vulkan-Auffindung (`find_package(Vulkan REQUIRED)`) in zwei Verzeichnissen parallel (Kommentar verweist auf `src/HE_Rendering/CMakeLists.txt`); ein gemeinsames `cmake/FindHeVulkan.cmake`-Include verhindert Drift.

## Gelesen
Vollständig gelesen: ViewportPick.cpp, ViewportPick.h, ViewportToolbar.cpp (1-1100), ViewportToolbar.h, ViewportViewMode.h sowie src/HE_Editor/CMakeLists.txt (1-732); für Querverweise nur gezielt Ausschnitte von EditorToolbar.cpp, ViewportPanel.cpp, EditorHelp.cpp (`findKey`) und AABB.h gelesen; `src/HE_Editor/vendor/**` absichtlich nicht gelesen (Fremdcode laut CLAUDE.md).

## Editor Slice 1b

Modul Editor (Kürzel EDT), Slice 1b: 16 Dateien aus `docs/audit/_slices/Editor_1b.txt` (AudioMixView, AudioWaveformView, BlendSpacePanel, BoneMaskPanel, BuildProgressDialog, CameraBookmarks, CinematicPanel, CinematicPreview, jeweils .cpp/.h).

## Zusammenfassung

Der Code ist durchgehend sauber kommentiert und die Datenmodelle sind gut getrennt (AudioWave/AudioMix ohne AppContext, Peak-Pyramide, Bracket-Muster im Cinematic-Preview). Die Funde liegen vor allem in per-Frame-Arbeit ohne Änderungserkennung (Cinematic-Preview, EQ-Kurve, Bone-Mask-Baum, Build-Log), in dupliziertem Panel-Boilerplate (BlendSpace/BoneMask/Cinematic) und in einer sehr langen Funktion (`AudioWave::draw`, ca. 540 Zeilen). Ein echter Lesefehler auf einen ungültigen Zeiger steckt in `BoneMaskPanel::jointRow`. Anzahl Funde: 1 Hoch, 7 Mittel, 6 Niedrig. `CameraBookmarks.*` ist unauffällig (kleine, geschlossene Datenklasse, Encode/Decode robust). Nichts davon wurde gemessen; Schweregrade sind Schätzungen aus dem Code.

---

### EDT-2001 – Cinematic-Preview rendert und schreibt/restauriert die Welt jeden Frame, auch im Stillstand
- **ID**: EDT-2001
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CinematicPanel.cpp:704-773` (drawPreview), `src/HE_Editor/CinematicPreview.cpp:130-242` (apply/restore)
- **Problem**: Solange der Cinematic-Tab vorne ist, läuft pro Frame: `Bracket::apply` (resolveBindings, evaluate, Save aller gebundenen Akteure inkl. Kopie von `boneMatrices`, Pose-Berechnung aller Skeletal-Sektionen), `RenderWorldPreview` (laut Header ein synchroner Round-Trip mit Extraktion der ganzen Welt), `Bracket::restore` (schreibt alles zurück, markiert Transform/Skelett/Material dirty und ruft `HE::propagateTransforms(world)` auf). Das geschieht auch, wenn nichts abgespielt wird, der Playhead stillsteht, nichts bearbeitet wird und die Fenstergröße gleich bleibt. Durch die Dirty-Markierungen in `restore` muss auch der Renderer Transform-/Skelett-/Material-Caches jedes Frame neu hochladen.
- **Code-Auszug**:
```cpp
s_preview.apply(*ctx.world, *ctx.contentManager, seq, st.view.playhead);
...
tex = ctx.renderer->RenderWorldPreview(*ctx.contentManager, *ctx.world, w, h, ov, ...);
s_preview.restore(*ctx.world, *ctx.contentManager);
// restore(): ... tc->dirty = true; smc->dirty = true; mc.dirty = true; ... HE::propagateTransforms(world);
```
- **Lösung**: Das Bild nur neu erzeugen, wenn sich eine Eingabe geändert hat: Playhead, Sequenz-Änderungszähler (`commit`/`st.dirty`-Edits/Undo/Restore erhöhen einen `uint32_t revision` im PanelState), Zielgröße, `throughCamera`, Editor-Kamera-Pose, Welt-Strukturepoche (`world.structureEpoch()` gibt es bereits). Sonst die zuletzt erhaltene Textur weiterzeichnen. Voraussetzung: Die Textur von `RenderWorldPreview` (Slot 0) bleibt gültig, bis jemand anderes Slot 0 benutzt; falls andere Panels Slot 0 überschreiben, den Cache dann verwerfen (Slot-Besitzer-Token oder „Frame-Zähler des letzten fremden Aufrufs“). Zusätzlich in `restore` die Re-Propagation nur ausführen, wenn mindestens ein `hasTransform` gesichert wurde.
- **Erwarteter Effekt**: Im Stillstand fast die gesamte Preview-Kosten weg (Schätzung: eine ganze Szenen-Extraktion + Render pro Frame; messen mit `HE_PROFILE_SCOPE`). Bei laufender Wiedergabe unverändert.
- **Risiko / Verifikation**: Hoch genug, um Tests zu brauchen. Gefahr: veraltetes Bild, wenn sich Welt oder Material von außen ändert (Kollaboration, anderer Tab). Kein bestehender UI-Test für den Preview-Pfad bekannt (`tests/test_*cinematic*` prüfen); Test: Bracket-Roundtrip unverändert lassen, neuen Test „zweiter Aufruf bei gleicher Revision ruft RenderWorldPreview nicht erneut auf“ mit Mock-Renderer.

### EDT-2002 – Build-Dialog: Mutex wird beim Zeichnen des Logs gehalten, Logzeilen pro Frame über alle Schritte gescannt
- **ID**: EDT-2002
- **Kategorie**: E (und D)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/BuildProgressDialog.cpp:830-877`, Kommentar dazu `:700-702`
- **Problem**: Der Kommentar sagt, das Modell werde unter dem Lock kopiert und sofort freigegeben, „holding a worker's mutex across [ImGui calls] is how a UI ends up waiting on a compiler“. Genau das passiert aber im Log-Child: `std::lock_guard<std::mutex> lk(s_mutex)` bleibt über `drawLine`, den ListClipper und `SetClipboardText` bestehen. Der Export-Worker (`Build::log`, `stepProgress`, `setActivity`) blockiert so für die Dauer des Zeichnens. Außerdem wird jeden Frame `s_log` komplett durchlaufen, um die Zeiger der gewählten Schritte zu sammeln (`lines`), obwohl der Pack-Schritt laut Kommentar „thousands of lines“ erzeugt und auch Zeilen aller anderen Schritte mitgescannt werden.
- **Code-Auszug**:
```cpp
std::lock_guard<std::mutex> lk(s_mutex);
...
std::vector<const LogLine*> lines;
for (const LogLine& l : s_log)
    if (l.step == s_selected) lines.push_back(&l);
```
- **Lösung**: Pro Schritt einen eigenen Logvektor führen (`std::vector<std::vector<LogLine>>` oder `Step::log`), dann entfällt der Scan. Beim Zeichnen nur kurz sperren: Anzahl lesen, `ListClipper` ohne Lock laufen lassen und pro sichtbarer Zeile (≈40) unter Lock kopieren, oder `std::deque<LogLine>` (stabile Referenzen bei `push_back`) verwenden und nur beim Größenabruf sperren. Kopieren für „Copy“ unter Lock in einen String, dann Lock lösen und erst danach `SetClipboardText`.
- **Erwarteter Effekt**: Kein UI-bedingtes Stocken des Export-Workers mehr; pro Frame O(sichtbare Zeilen) statt O(Gesamtlog). Schätzung; bei 10k Zeilen ein Scan von Zehntausenden Struct-Zugriffen pro Frame.
- **Risiko / Verifikation**: Gering bis mittel (Thread-Sicherheit). `snapshot()` und `McpToolsBuild` hängen am Log-Typ; bestehende Tests für das Modell (suchen nach `BuildProgressDialog` in `tests/`) müssen weiter grün sein.

### EDT-2003 – Asset-Tab-Panels: ca. 80 Zeilen Boilerplate pro Panel dreifach kopiert, Cinematic fehlt bei den Crash-Snapshots
- **ID**: EDT-2003
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `BlendSpacePanel.cpp:50-68, 230-307, 309-331`; `BoneMaskPanel.cpp:44-62, 237-277, 279-301`; `CinematicPanel.cpp:83-96, 777-828, 830-855`; Header `BlendSpacePanel.h`, `BoneMaskPanel.h`, `CinematicPanel.h`
- **Problem**: Die Funktionen `saveState`, `isXxxAsset`, `isDirty`, `reloadFromDisk`, `appendDirtyPaths`, `save`, `forget`, `stateByContentPath`, `isDirtyByContentPath`, `reloadByContentPath`, `appendSnapshots`, der Lade-Prolog in `render` (`toContentRelativePath`, `loadAsset`, Name aus Stem) und der identische `ImGui::Begin(("##x_" + assetPath)…)`-Flaggenblock sind in drei Dateien Zeile für Zeile gleich (die Kommentare sagen selbst „the same shape InputAssetPanel's and TypeAssetPanel's have“, also sind es mindestens fünf Kopien). Dabei ist schon eine Drift entstanden: `CinematicPanel` hat kein `appendSnapshots` (nicht im Header, nicht in `EditorUI.cpp:1063-1077` aufgerufen), ein ungespeicherter Cinematic-Tab ist also nicht in den Crash-Recovery-Kopien. Das ist vermutlich keine Absicht, sondern Vergessen (unsicher, bitte mit dem Panel-Autor bestätigen).
- **Code-Auszug**:
```cpp
bool BlendSpacePanel::reloadByContentPath(const std::string& contentPath)
{
    PanelState* st = stateByContentPath(contentPath);
    if (!st) return false;
    st->loaded = false;
    st->dirty  = false;
    return true;
}
```
- **Lösung**: Ein Template in `EditorPanelState.h` (neben `AssetPanelState`), z. B. `AssetTabOps<PanelState, Traits>`, mit Traits `assetType`, `getMutable(cm, id)`, `toJson(state)`, `fromJson`. Daraus entstehen die zehn Standardfunktionen einmal. Die Panel-Namespaces rufen nur noch `return Ops::isDirty(path);` o. Ä. oder, einfacher, die Panels exportieren ein `constexpr`-Interface-Objekt, das `EditorUI` in einer Tabelle statt in mehreren handgepflegten Aufruflisten (`EditorUI.cpp:992-1211`, `4161`) durchläuft. Zunächst klein anfangen: nur `stateByContentPath`/`reloadByContentPath`/`isDirtyByContentPath` als Member von `AssetPanelState` (`findByContentPath(relPath)` setzt voraus, dass `PanelState::relPath` in einem Basis-Struct liegt).
- **Erwarteter Effekt**: Ca. 150-250 Zeilen weniger, eine Stelle für künftige Änderungen (z. B. neue Pflichtfunktion wie `appendSnapshots`); keine Laufzeitwirkung.
- **Risiko / Verifikation**: Niedrig bis mittel; rein mechanisch, aber viele Aufrufer. Absichern über bestehende Dirty-/Reload-Tests (Suche `tests/` nach `isDirtyByContentPath`, `reloadFromDisk`); Build von `HorizonEditor` und `he_tests`.

### EDT-2004 – EQ-Graph berechnet die komplette Frequenzgangkurve jeden Frame neu (komplexe Arithmetik, 3 Heap-Vektoren)
- **ID**: EDT-2004
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/AudioMixView.cpp:195-233` (drawGraph), `src/HE_Core/src/Audio/AudioEdit.cpp:307-316` (magnitudeDb, außerhalb des Slice, nur zur Belegung)
- **Problem**: Pro Frame und Pixelspalte wird `sumAt(f)` über alle Bänder berechnet, und `magnitudeDb` nutzt `std::polar`, komplexe Division, `std::abs` und `log10`. Bei 600 Spalten × bis zu `kMaxBands` Bändern sind das einige Tausend solcher Aufrufe pro Frame, plus die zweite Kurve für das gewählte Band (`own`). `freqOfX` ruft pro Spalte `std::pow` (zweimal pro Spalte, wenn ein Band gewählt ist). Dazu drei Heap-Allokationen pro Frame (`coeffs`, `line`, `own`). Der Graph ändert sich nur, wenn EQ, Samplerate, Breite oder Auswahl sich ändern.
- **Code-Auszug**:
```cpp
for (int i = 0; i <= cols; ++i)
{
    const double f = freqOfX(float(i), w);
    line.emplace_back(o.x + float(i), o.y + yOfDb(sumAt(f), h));
}
```
- **Lösung**: (1) Frequenztabelle `std::vector<double> freq` pro Breite einmal berechnen (Member in `EqView`, gebaut wenn `cols` wechselt). (2) Die Kurven (`line` und `own` Y-Werte in dB) in `EqView` zwischenspeichern, mit einem Schlüssel aus (eq-Bänder, enabled, rate, cols, selBand). Schlüssel billig: ein FNV-Hash über `sizeof` der Bänder-POD-Felder oder ein vom Aufrufer gelieferter Änderungszähler. (3) Die Vektoren als Member wiederverwenden, nicht neu allokieren. Alternativ ohne Cache: `magnitudeDb` mit reellem Ausdruck (cos/sin von ω und 2ω einmal pro Frequenz für alle Bänder teilen).
- **Erwarteter Effekt**: Schätzung 0,3-1 ms pro Frame weniger solange das Audio-Editor-/Mixer-Fenster sichtbar ist; messen.
- **Risiko / Verifikation**: Gering; die Anzeige muss pixelgleich bleiben. Hinweis: der Header sagt, `AudioMix` sei von den Tests angesteuert (`graphMin/graphSize`, `xOfFreq`); die vorhandenen Tests (`tests/` nach `AudioMix`) decken die Mapping-Funktionen, ein Test „Kurve bleibt gleich nach Cache-Treffer“ ist zu ergänzen.

### EDT-2005 – `AudioWave::draw` ist eine einzige Funktion mit ca. 540 Zeilen
- **ID**: EDT-2005
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/AudioWaveformView.cpp:471-1011`
- **Problem**: Eingabe (Wheel, Presses, Drags, Cursor), Layout, Lineal, Selektion, Kanalspuren, Trim, Kurve, Overview und Cursorrückgabe stehen in einer Funktion mit Lambdas (`frameUnderPointer`, `overviewFrameAt`, `xAt`, `yOfGain`, `ox`) und rund 35 lokalen Variablen. Die Fallunterscheidung der Presses (`rulerActivated` / `lanesPressed && curveOn` / `lanesPressed` / `overActivated`) und der Drag-`switch` hängen über Zustände in `View` zusammen, die beim Lesen quer über 300 Zeilen verfolgt werden müssen. Die zwei Tick-Schleifen (Zeilen 770-777 und 788-797) wiederholen denselben Linien-/Label-Block.
- **Code-Auszug**:
```cpp
Result draw(const Clip& clip, const Peaks& peaks, View& view, const ImVec2& size, bool trackpad,
            HE::AudioEnvelope* curve, bool curveEditable)
{ ... // 540 Zeilen
```
- **Lösung**: Aufteilen in `struct Layout` (origin, width, height, rulerBottom, overTop, lanesBottom, lanesH, rate, curveOn) und private Funktionen in einem `namespace`: `handleWheel`, `handlePresses`, `handleDrags`, `drawRuler` (mit einem Lambda `tick(x, label)` für beide Modi), `drawLanes`, `drawTrim`, `drawCurve`, `drawOverview`. `draw` bleibt die Reihenfolge-Steuerung (≈40 Zeilen). Reine Umordnung, keine Verhaltensänderung.
- **Erwarteter Effekt**: Lesbarkeit und Testbarkeit (z. B. `handlePresses` ließe sich mit synthetischem `ImGuiIO` einzeln treiben); keine Laufzeitänderung.
- **Risiko / Verifikation**: Gering; die vorhandenen headless Draw-Tests (Audio-Editor-UI-Tests, `tests/test_*audio*`) sind das Sicherheitsnetz; zusätzlich BMP-Frames vor/nach vergleichen (`HE_UI_DUMP_DIR`).

### EDT-2006 – Bone-Mask-Baum: O(J²) Kindersuche, `vector`-Allokation je Knoten und Linearsuchen je Frame, kein Row-Clipping
- **ID**: EDT-2006
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/BoneMaskPanel.cpp:66-71, 144-195, 201-233, 340-341`
- **Problem**: (a) `jointTree` durchsucht für jeden Knoten das gesamte Skelett nach Kindern und legt dafür pro Knoten und Frame einen `std::vector<int>` an, das ist O(J²) und J Heap-Allokationen pro Frame. (b) `jointRow` ruft für jeden gezeichneten Joint `entryFor` (lineare String-Vergleiche über alle Einträge), also O(J·E). (c) `strayNames` vergleicht für jeden Eintrag Strings mit allen Gelenken (O(E·J)) und baut jeden Frame einen `vector<string>` mit Kopien. (d) Alle Knoten sind `DefaultOpen`, also werden bei einem Rig mit 150-300 Knochen jeden Frame alle Zeilen mit Checkbox + Text + Slider gebaut, ohne Clipper; CLAUDE.md Abschnitt 4 verlangt für zeilenlastige Panels Row-Clipping.
- **Code-Auszug**:
```cpp
std::vector<int> children;
for (size_t i = 0; i < mesh.skeleton.size(); ++i)
    if (mesh.skeleton[i].parent == index) children.push_back(static_cast<int>(i));
```
- **Lösung**: Pro Referenz-Mesh einmal `children` als CSR-Struktur (`std::vector<int> firstChild/next`, oder `vector<vector<int>>`) in `PanelState` aufbauen, Schlüssel `referenceMeshId` und `skeleton.size()`. Pro Frame (oder nur bei `dirty`/Mesh-Wechsel) ein `std::vector<int8_t>` / `std::vector<float*>` `entryOfJoint[J]` über eine einmal erzeugte `unordered_map<string,int>` (Name → Gelenk) füllen; `strayNames` leitet sich aus demselben Durchlauf ab (Einträge, die auf kein Gelenk abgebildet wurden). Für Clipping die Baumdarstellung in eine flache Liste „sichtbare Zeilen mit Tiefe“ (je Frame aus dem Aufklapp-Zustand) überführen und per `ImGuiListClipper` zeichnen; dafür braucht `TreeNode` einen eigenen Aufklapp-Zustand (z. B. `std::vector<bool> collapsed`).
- **Erwarteter Effekt**: Von O(J²) und ~J Allokationen pro Frame auf O(J) bzw. O(sichtbare Zeilen). Bei J≈300 vermutlich dominiert der ImGui-Widgetaufbau (≈900 Widgets); gemessen werden sollte vorher mit einem großen Rig (Schätzung: 0,5-2 ms/Frame).
- **Risiko / Verifikation**: Mittel für den Clipper-Teil (Tree-Verhalten ändert sich); Teil (a)-(c) risikoarm. Test mit `tests/test_*bone_mask*` (suchen), und mit synthetischem Skelett 500 Gelenke.

### EDT-2007 – Bone-Mask: nach `eraseEntry` wird `e->weight` aus einem ungültigen Zeiger gelesen
- **ID**: EDT-2007
- **Kategorie**: C (Dangling)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektur)
- **Ort**: `src/HE_Editor/BoneMaskPanel.cpp:110-136`
- **Problem**: `e` wird vor der Checkbox per `entryFor` gesucht. Schaltet die Checkbox auf „aus“, ruft der Code `eraseEntry(st.mask, j.name)` auf, das den Vektor verschiebt, `e` zeigt aber weiter auf den (jetzt verschobenen oder zerstörten) Slot. Direkt danach prüft `if (e)` auf einen nicht-null Zeiger und liest `float w = e->weight`. Der Kommentar in Zeile 131-133 sagt zwar, man suche den Eintrag neu, weil „a pointer into a vector that just grew is the oldest bug in this codebase“, tut das aber erst beim Schreiben (Zeile 134), nicht beim Lesen in Zeile 128. Praktisch zeigt der Slider für ein Frame ein Gewicht eines anderen Gelenks oder zeigt für ein frisch abgewähltes Gelenk noch einen Slider; formal ist es undefiniertes Verhalten.
- **Code-Auszug**:
```cpp
if (EditorWidgets::checkbox("Joint", &on))
{
    if (on) setEntry(st.mask, j.name, 1.0f);
    else    eraseEntry(st.mask, j.name);
    st.dirty = true;
}
...
if (e) { ... float w = e->weight; ... }
```
- **Lösung**: Nach dem Umschalten `e = on ? entryFor(st.mask, j.name) : nullptr;` setzen (eine Zeile), danach entfällt auch die erneute Suche in Zeile 134 (`e->weight = w; st.dirty = true;`).
- **Erwarteter Effekt**: Korrektheit (kein UB, kein Flackern); weniger Code.
- **Risiko / Verifikation**: Sehr gering. Test: UI-Test, der einen Joint abwählt und prüft, dass im selben Frame kein Slider gezeichnet wird (`tests/test_*_ui.cpp`-Muster).

### EDT-2008 – Cinematic-Tab: wiederkehrende Per-Frame-Allokationen und -Scans (Refs, Labels, Akteurzeile)
- **ID**: EDT-2008
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CinematicPanel.cpp:204-220, 407-427, 437-440, 896-909`
- **Problem**: Jeden Frame: (a) `ensureRefsResident` baut einen neuen `std::vector<HE::UUID> refs` per `sequenceAssetRefs` über alle Tracks, macht je Ref zwei Hash-Lookups und eine Linearsuche in `triedRefs`; die Sequenz ändert sich aber nur bei einem Commit/Undo/Reload. (b) `drawActors` baut je Akteur drei temporäre Strings (`shown`, Label mit `std::to_string` + Konkatenation, `CalcTextSize`), `entityOf` wird dabei pro Akteur aufgerufen; dieselbe Auflösung (+ `all_of<CameraComponent>`) passiert dann nochmals für `Labels` (Zeilen 897-902) und `Add at Playhead`/`cameraCombo` (Zeilen 363-368, 506). (c) `fresh` wird pro Frame neu gefüllt mit `std::any_of` über alle Bindings (O(S·B)). (d) `Labels` baut `missing`/`camera`-`vector<bool>` ohne `reserve`.
- **Code-Auszug**:
```cpp
std::vector<HE::UUID> refs;
HE::sequenceAssetRefs(*seq, refs);
for (const HE::UUID& id : refs)
{
    if (ctx.contentManager->getAnimationClip(id) || ctx.contentManager->getAudio(id)) continue;
    if (std::find(st.triedRefs.begin(), st.triedRefs.end(), id) != st.triedRefs.end()) continue;
```
- **Lösung**: `refs` als Member der PanelState mit `refsRevision`; neu berechnen nur nach `commit`/`restoreSnapshot`/Load (ein Zähler `st.revision`, derselbe wie in EDT-2001 vorgeschlagen). Die Auflösung `Binding → entity/isCamera` einmal pro Frame in einen `struct BindingInfo { entt::entity e; bool camera; }`-Vektor (Member, `clear()` + `push_back`) legen und in drawActors/Labels/Combo wiederverwenden (ein Helfer `isCameraBinding` entfernt auch die dreifach duplizierte Zeile). Label-Strings nur bei Namensänderung bauen oder `ImGui::PushID(b.slot)` statt `##cin_actor<slot>`-Konkatenation nutzen.
- **Erwarteter Effekt**: Einige Dutzend kleine Allokationen und einige Hundert Lookups pro Frame weniger; vermutlich < 0,1 ms; echter Gewinn ist eher Klarheit. Bei sehr vielen Akteuren (Zeile läuft nicht um, siehe Unsicher) wächst es linear.
- **Risiko / Verifikation**: Gering; `triedRefs` muss bei Reload zurückgesetzt bleiben (Zeile 842).

### EDT-2009 – Volumenkurve: pro Pixelspalte vier transzendente Aufrufe und neuer Vektor je Frame
- **ID**: EDT-2009
- **Kategorie**: D / A
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/AudioWaveformView.cpp:237-259, 880-910`
- **Problem**: `curveFracOfGain` berechnet bei jedem Aufruf `std::pow(10.0, kCurveFloorDb/20.0)` (Konstante) und ruft `curveTopDb()` zweimal (`20*log10(kMaxGain)`, ebenfalls Konstante), dazu `log10(gain)`. Dieselbe Funktion wird pro Pixelspalte der Kurve (Zeile 908 über `yOfGain`) und für jeden Punkt in `curvePointAt` aufgerufen, also bei ca. 1500 Spalten ≈ 6000 `pow/log10` pro Frame. Das ist genau das „constexpr statt Laufzeitberechnung“-Muster; `kMaxGain` ist `static constexpr float = 4.0f` (`AudioEdit.h:100`). Außerdem wird `std::vector<ImVec2> line` jeden Frame neu allokiert und `evalGain` (Binärsuche) pro Spalte aufgerufen, auch wenn die Kurve nur als ruhige Linie (`!curveOn`) dasteht.
- **Code-Auszug**:
```cpp
double curveTopDb() { return 20.0 * std::log10(double(HE::AudioEnvelope::kMaxGain)); }
...
const double floorGain = std::pow(10.0, kCurveFloorDb / 20.0);
```
- **Lösung**: `constexpr double kCurveTopDb = 12.0411998265592478;` (mit `static_assert(std::abs(kCurveTopDb - 20*log10(4.0)) < 1e-9)` zur Absicherung falls `std::log10` nicht `constexpr` ist; GCC erlaubt es, MSVC/Clang nicht) und `constexpr double kCurveFloorGain = 0.00398107170553497;` (10^-48/20 = 10^-2.4). Den Pixelvektor als `thread_local`/Member wiederverwenden (`line.clear()`).
- **Erwarteter Effekt**: Schätzung 50-100 µs pro Frame im Curve-Modus; eher Vereinfachung als Gewinn.
- **Risiko / Verifikation**: Sehr gering; Tests für `curveFracOfGain` und `curveGainOfFrac` Rundlauf existieren vermutlich (`tests/` nach `curveFracOfGain`).

### EDT-2010 – Waveform/Overview: Spaltenextrema pro Frame neu berechnet
- **ID**: EDT-2010
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/AudioWaveformView.cpp:843-860, 962-983`
- **Problem**: Für jeden Pixel und jeden Kanal ruft `draw` `columnExtent` (Spuren, Zeilen 843-860) und für die Overview ein weiteres Mal pro Spalte und Kanal (962-983), obwohl sich die Overview nur bei neuem Clip oder neuer Breite ändert. Die Pyramide begrenzt jeden Aufruf auf wenige Dutzend Bucket-Lesezugriffe (laut Header ≈ 2×16), das ist also keine Katastrophe, aber ≈ `width × channels × ~20` Reads pro Frame und `width × channels` einzelne `AddLine`-Aufrufe mit Anti-Aliasing.
- **Code-Auszug**:
```cpp
for (int c = 0; c < clip.channels; ++c)
{ int l = 0, h = 0; columnExtent(clip, peaks, f0, f1, c, l, h); ... }
```
- **Lösung**: Overview-Extrema (`std::vector<int16_t> lo, hi`) in `View` ablegen und nur bei Wechsel von (`peaks.frames`, `width`) neu bauen. Für die Spuren optional dasselbe mit Schlüssel (viewStart, framesPerPx, width). Zusätzlich messen, ob `AddRectFilled` für 1-px-Säulen billiger ist als `AddLine` mit AA-Pfad (siehe EDT-2004: dort wurde absichtlich auf Rechtecke gewechselt).
- **Erwarteter Effekt**: Unsicher; nur nach Messung (ImGui-Zeichenkosten dominieren vermutlich die Berechnung).
- **Risiko / Verifikation**: Gering; Cache-Invalidierung bei Edits, die Samples ändern (Clip-Wechsel liefert neue `Peaks`).

### EDT-2011 – Build-Dialog: doppelte Strukturen (Step/StepView, LogLine/LogView) und volle Modellkopie pro Frame
- **ID**: EDT-2011
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/BuildProgressDialog.cpp:31-45, 281-285, 578, 708-731`; `BuildProgressDialog.h:119-132`
- **Problem**: `Step`/`StepView` und `LogLine`/`LogView` sind Feld für Feld identisch. `snapshot()` und `brief()` kopieren Felder von der einen in die andere Form. `render()` kopiert pro Frame `s_steps` (mit Strings), `message`, `activity`, `exePath` und `s_hcInterpreted` (Vektor von Notices mit Strings), obwohl sich das Modell meist nur beim Wechsel des Fortschritts ändert. `brief()` baut zusätzlich pro Frame (nur im versteckten Zustand) einen `vector<StepView>` mit leeren Strings, nur um `overallProgress` zu füttern.
- **Code-Auszug**:
```cpp
steps    = s_steps;
...
hcInterpreted = s_hcInterpreted;
```
- **Lösung**: Ein einziger Typ `StepView`/`LogView` (die Zustandskonstanten `Pending…Failed` als `enum` im Header), `s_steps`/`s_log` direkt von diesem Typ. Einen `uint64_t s_version` im Modell bei jeder Schreibfunktion erhöhen; `render()` kopiert nur, wenn sich die Version seit dem letzten Frame geändert hat, und hält das Ergebnis in `static` UI-State. `overallProgress` auf `std::span<const StepView>` bzw. direkt eine Schleife unter dem Lock in `brief()`.
- **Erwarteter Effekt**: ca. 40 Zeilen weniger, Kopierkosten nur bei Änderungen (während eines Builds ändert sich das Modell mehrmals pro Sekunde, also eher gering).
- **Risiko / Verifikation**: Gering; `McpToolsBuild` konsumiert `Snapshot`, Typnamen bleiben erhalten.

### EDT-2012 – Cinematic-Undo: bis zu 64 vollständige JSON-Strings pro Tab
- **ID**: EDT-2012
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CinematicPanel.cpp:55-56, 99-108`
- **Problem**: Jeder Commit serialisiert die gesamte Sequenz (`HE::sequenceToJson`), vergleicht sie mit dem obersten Snapshot und legt sie im Vektor ab (max. 64, `erase(begin())` verschiebt 64 Strings). Bei langen Sequenzen mit vielen Keys sind das schnell einige MB pro Tab und je Edit eine ganze Serialisierung. Außerdem wird während einer `DragFloat`-Interaktion nicht committet (nur beim Loslassen), das ist richtig; die Kosten fallen also pro Gesten-Ende an, nicht pro Frame.
- **Code-Auszug**:
```cpp
std::string snap = HE::sequenceToJson(seq);
if (st.undoPos >= 0 && ... && st.undo[st.undoPos] == snap) return;
st.undo.resize(static_cast<size_t>(st.undoPos + 1));
st.undo.push_back(std::move(snap));
if (st.undo.size() > 64) st.undo.erase(st.undo.begin());
```
- **Lösung**: Ring (`std::deque<std::string>`) statt `erase(begin())`; optional einen Byte-Budget-Deckel (z. B. 16 MB pro Tab) zusätzlich zur Anzahl 64. Delta-Snapshots lohnen sich vermutlich nicht.
- **Erwarteter Effekt**: Speicherbegrenzung; Laufzeit unwesentlich.
- **Risiko / Verifikation**: Gering. Aufpassen auf `undoPos`-Index bei Deque.

### EDT-2013 – Inkonsistente ImGui-Guards (`__has_include`, `HE_IMGUI_ENABLED`) und streuende `#ifdef`s
- **ID**: EDT-2013
- **Kategorie**: B / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `AudioMixView.h:6-8` vs `:77-78, 90`; `AudioWaveformView.h:8-10, 226, 267` und `.cpp:9-11, 411, 1013`; `BuildProgressDialog.cpp:15-17, 344, 599, 603-612, 617, 671, 676, 944`
- **Problem**: `AudioMixView.h` schützt `#include <imgui.h>` mit `__has_include`, benutzt aber `ImVec2` ohne Guard in `EqResult` und `drawEq`; ohne ImGui bricht der Header also ohnehin. `AudioMixView.cpp` bindet `imgui.h` und `EditorWidgets.h` unbedingt ein. `AudioWaveformView` trennt dagegen sauber ImGui-freien und ImGui-Teil. `BuildProgressDialog.cpp` bindet `EditorWidgets.h`/`EditorHelp.h` unbedingt ein und zerlegt `FooterWidth`, `DrawFooter`, `render` in je eigene `#ifdef HE_IMGUI_ENABLED`-Rümpfe mit `[[maybe_unused]]`-Parametern, obwohl der Editor ohne ImGui nicht baut (CLAUDE.md §2 verwendet `-DHE_IMGUI_ENABLED` als Normalfall).
- **Code-Auszug**:
```cpp
void DrawFooter([[maybe_unused]] AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
    if (!hidden()) return;
```
- **Lösung**: Entscheiden: Entweder AudioMix wie AudioWave in einen ImGui-freien Teil (`busChoice`, Mapping, `newBand`) und einen ImGui-Teil trennen, oder die Guards entfernen. Im Build-Dialog die `#ifdef`-Rümpfe entfernen, wenn kein Target ohne `HE_IMGUI_ENABLED` diese Datei kompiliert (CMake prüfen).
- **Erwarteter Effekt**: Weniger Rauschen; evtl. ehrlichere Include-Struktur.
- **Risiko / Verifikation**: Gering; `HorizonEditor`-Build und `he_tests` (die AudioWave/Mix nutzen) bauen.

### EDT-2014 – Ungenutzte Includes, leere `if`-Rümpfe, doppelte Konstanten und Bandlogik
- **ID**: EDT-2014
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `BlendSpacePanel.cpp:14, 21, 175, 374-375, 380-382`; `BoneMaskPanel.cpp:14`; `CinematicPanel.cpp:27`; `AudioMixView.cpp:123-134, 314-323, 325-330, 449-454` vs `AudioWaveformView.cpp:428`
- **Problem**: (a) `Diagnostics/Log.h` wird in `BlendSpacePanel.cpp` und `BoneMaskPanel.cpp` nicht benutzt (kein `HE_LOG_*` in den Dateien). `<cmath>` in `BlendSpacePanel.cpp` scheint ungenutzt; dafür fehlt `<cstdio>` für `std::snprintf` (Zeile 175, kommt zufällig transitiv). `glm/gtc/matrix_transform.hpp` in `CinematicPanel.cpp` hat keinen sichtbaren Verwender (kein `translate`/`rotate`). `HorizonScene/AnimationPose.h` in `BlendSpacePanel.cpp` ist unklar (per Syntax-Check prüfen). (b) Leere Rümpfe: `if (EditorWidgets::Row::dragFloat("Cursor X", ...)) {}` (Zeilen 380, 382) sind Aufrufe, deren Rückgabe ignoriert wird, als `if` getarnt. (c) `weights` ist ein lokaler `std::vector<float>`, der jeden Frame neu allokiert wird (Zeile 374); `blendSpaceWeights` nimmt einen Out-Parameter, ein Member in `PanelState` genügt. (d) In `AudioMixView.cpp` sind „Band einfügen“ (Zeilen 314-319, Doppelklick) und `newBand` (Zeilen 100-117) zwei Pfade, die beide `type = Peak; q = 1.0f` setzen; „Band entfernen“ steht doppelt (Rechtsklick Zeile 327-329 und `removeAt` Zeilen 449-454). `kBg` ist in `AudioMixView.cpp:123` und `AudioWaveformView.cpp:428` identisch (`IM_COL32(20, 21, 25, 255)`) und könnte in `EditorTheme` stehen.
- **Code-Auszug**:
```cpp
if (EditorWidgets::Row::dragFloat("Cursor X", &st.cursorX, 0.01f, -10000.0f, 10000.0f, "%.3f")) {}
```
- **Lösung**: Includes entfernen (`g++ -fsyntax-only` nach jedem Entfernen), leere `if`-Rümpfe zu reinen Aufrufen machen, `weights` als Member, `bandAt(freq, gain)`/`removeBand(eq, v, i)` als kleine Hilfsfunktionen in `AudioMix`.
- **Erwarteter Effekt**: Kleinere Include-Graphen, weniger Duplikat.
- **Risiko / Verifikation**: Sehr gering. `editor_help_audit.py --check` unverändert, weil keine Labels sich ändern.

---

## Gelesen

Alle 16 Dateien der Slice-Liste wurden vollständig mit dem Read-Tool gelesen (jeweils ein Aufruf ohne Offset, keine abgeschnitten): `src/HE_Editor/AudioMixView.cpp` (463 Zeilen) ja, `AudioMixView.h` (92) ja, `AudioWaveformView.cpp` (1015) ja, `AudioWaveformView.h` (268) ja, `BlendSpacePanel.cpp` (423) ja, `BlendSpacePanel.h` (39) ja, `BoneMaskPanel.cpp` (354) ja, `BoneMaskPanel.h` (33) ja, `BuildProgressDialog.cpp` (947) ja, `BuildProgressDialog.h` (222) ja, `CameraBookmarks.cpp` (122) ja, `CameraBookmarks.h` (64) ja, `CinematicPanel.cpp` (937) ja, `CinematicPanel.h` (43) ja, `CinematicPreview.cpp` (265) ja, `CinematicPreview.h` (107) ja. Fremddateien nur punktuell zur Belegung angesehen (`AudioEdit.h/.cpp`, `TransformHierarchy.cpp`, `EditorUI.cpp`, `HorizonWorld.h`).
