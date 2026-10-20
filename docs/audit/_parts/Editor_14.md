# Editor Slice 14 – McpToolsType.cpp … ProjectPreflight.h

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

## Cross-Modul-Hinweise

- `src/HE_Core/src/Diagnostics/GlobalState.cpp:403-407` – `removeKnownProject` mit Wert-Aliasing durch `std::remove` (siehe EDT-1420).
- `src/HE_Core/include/Diagnostics/EngineProfiler.h:210, 301-302` und `EngineProfiler.cpp:323` – `liveSnapshot()` gibt `m_live` per Wert zurück; der Ring ist ein `vector` mit `erase(begin())` pro Frame (O(240)); `std::deque`/Ringpuffer plus `span`-Zugriff wäre billiger. `ProfThreadTimeline::spans` hält bis zu ~250k Spans pro Lane ohne Sortierung nach Start (Voraussetzung für EDT-1403).
- `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:21-26` – `childrenOf` ist O(n) pro Aufruf; mehrere Aufrufer (MCP-Widget-Tools, WidgetManager.cpp:5926) rufen es pro Knoten auf.
- `src/HE_Editor/GraphEditor.h` – `GraphEditor::Model` liefert Vektoren/Strings per Wert pro Callback (betrifft Material-, HorizonCode- und Particle-Editor, siehe EDT-1412).
- `src/HE_Scene/src/EntityVisibility.cpp:51-65` – `subtreeVisibility` läuft bei vollständig verborgenem Teilbaum über den ganzen Teilbaum, wird aber vom Outliner pro sichtbarer Zeile und Frame aufgerufen (OutlinerPanel.cpp:1561-1562).

## Unsicher / Messung nötig

- Reale Restkosten der Outliner-Zeilenschleife bei 50k Entities nach dem Clipping (EDT-1404, Stufe 2 nur nach Messung).
- Kosten von `SceneSerializer::prefabInstancesBinding` / `prefabAddedHereCount` im Outliner-Rebuild für große Prefab-Instanzen (OutlinerPanel.cpp:668-675); evtl. O(Instanzen x Entities).
- `HE::subtreeVisibility` und `isEntityActive` pro sichtbarer Outliner-Zeile (Z.1232, 1561): Worst Case bei großen komplett verborgenen Teilbäumen O(Teilbaum) pro Frame; Normalfall bricht beim ersten sichtbaren Kind ab.
- Anzahl Spans und tatsächliche Frame-Zeit des Timeline-Tabs bei realen Captures (EDT-1403).
- Wirkung der `ParticleGraph`-JSON-Serialisierung pro Frame (EDT-1401) bei typischen Graphgrößen (vermutlich klein, aber Allokationen pro Frame).
- Ob der Windows-COM-Ordner-Dialog (EDT-1420) einen funktionalen Grund hat (Parenting/Verhalten gegenüber SDL).
- Ob `HcCodegen.h` in ProjectHubPanel.cpp für den Typ `ctx.toolchainProbe` zwingend ist (EDT-1419).

## Gelesen

Alle 34 Dateien des Slice wurden vollständig gelesen: ja (OutlinerPanel.cpp in zwei Blöcken 1-1000 und 1000-1948; keine Datei ausgelassen). Zusätzlich punktuell geprüft: `GlobalState.cpp` (removeKnownProject), `UIWidgetTree.cpp` (childrenOf), `EntityVisibility.cpp`, `EntityActive.cpp`, `EngineProfiler.h` (Live-Ring).
