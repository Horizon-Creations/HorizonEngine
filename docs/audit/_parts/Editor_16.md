# Editor Slice 16 – StaticMeshEditorPanel … TerrainSync

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

## Cross-Modul-Hinweise

- `src/HE_Editor/EditorCommands.cpp:419-428` (`entityByUuid`/`uuidOf`): O(Entities) mit `snprintf` pro Entity – wird vom Task Board, den MCP-Tools und `EditorApplication` benutzt (siehe EDT-1602). Ein `UUID`-Overload wäre modulweit nützlich.
- `src/HE_Editor/EditorApplication.cpp:4915-4925`: baut pro Frame ein `TeamSyncController::Project` (Path-Kopie + 3 Strings); `EditorApplication.cpp:11400-11560` (`syncStructuralChanges`) ist der eigentliche Verursacher von EDT-1601.
- `src/HE_SourceControl/include/SourceControl/TeamSyncService.h:183-189`: `status()/lastResults()/history()/lost()` liefern Wert-Kopien; ein Revisionszähler fehlt (EDT-1610).
- `src/HE_Scene/include/HorizonScene/HorizonWorld.h:106`: `cellManifestJson()` ohne Revisionszähler (EDT-1614).
- `src/HE_Editor/EditorUI.cpp:291-325` (`docsPanelOpener`) – Quelle der doppelt gepflegten Panelliste (EDT-1615); `EditorUI.cpp:3502` ruft `footerOnlineWidth` pro Frame.
- `src/HE_Editor/MeshMaterialSlots.*` (nicht im Slice): `MS::tableOf(*mesh)` kopiert pro Frame die Slot-Tabelle (`StaticMeshEditorPanel.cpp:472`); Größe/Kosten dort prüfen.
- `TaskBoardTeam`/`TaskTeamLink` sind im Editor nicht verdrahtet (`setTeam`/`setLink` nur in `tests/`, Plan in `docs/taskboard-team-interop-2026-10-15.md`): bis dahin ist der Team-Code in `TaskBoardPanel.cpp` (Zeilen 972-1022, 1145-1179) Totlast im Produktivbuild.

## Unsicher / Messung nötig

- EDT-1601: tatsächliche ms/Frame von `syncStructuralChanges` bei 10k/100k Entities in einer Session; ob `markHierarchyDirty` alle Hierarchieänderungen abdeckt.
- EDT-1602: Hitch-Größe bei realer Szenengröße; Anzahl typischer Entity-Links.
- EDT-1603/1604/1606: absolute Kosten bei realistischen Boards (< 100 Tasks wahrscheinlich unter 50 µs/Frame; Gewinn überwiegend bei sehr großen Boards).
- EDT-1608: reale Vertex-/CPU-Zeit der Avatar-Rechtecke im Footer; ob Run-Length oder Textur-Pfad lohnender ist.
- EDT-1611: ImGui-Kosten bei 120k Segmenten (Vertex-Puffer-Obergrenze, `ImDrawIdx` 16-bit → ImGui splittet Draw-Lists ggf.); Fenster-Verhalten bei 16-bit-Indizes prüfen.
- EDT-1612/1613: Serialisierungsgröße der Landscape-Entity, `memcmp`-Durchsatz auf Zielhardware, ob Dirty-Signale aller Werkzeuge (Pinsel, Fill Layer, Heightmap-Import, MCP, Undo) verlässlich gesetzt werden.
- `StaticMeshEditorPanel::drawMeshView` rendert die Vorschau jeden Frame neu (`RenderWorldPreview`); ob der Renderer bei unveränderter Kamera/Szene cached, liegt außerhalb des Slice (Backend).
- `StreamingDebugView::drawJobs`: Kosten und Locking von `ThreadPool::stats()` unbekannt (Quelle `HE_Core/JobSystem`).

## Gelesen

Alle 24 Dateien des Slice wurden vollständig gelesen (inklusive `TaskBoardPanel.cpp` 1–1333, `TeamView.cpp` 1–1270, `TeamController.cpp` 1–971, `TerrainSync.cpp` 1–627); `stb_image_impl.cpp` ist eine Zwei-Zeilen-Datei ohne Befund. Zur Einordnung wurden außerhalb des Slice nur gezielt `EditorCommands.cpp:405-430`, `EditorApplication.cpp` (Aufruf- und Diffstellen), `EditorUI.cpp:285-330` und das Handoff in `docs/taskboard-team-interop-2026-10-15.md` angesehen.
