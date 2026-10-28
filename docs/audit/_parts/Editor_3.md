# Editor Slice 3 – CollabPresenceBar … DocsPanel

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

## Cross-Modul-Hinweise

- `src/HE_Core/src/ContentManager/ContentManager.cpp:1015-1029` (`isEngineDefaultPath`/`isEngineOverridePath`) und `:977-981` (`isEngineContentDevMode`: `getenv` + `atoi` je Aufruf) sind die eigentliche Ursache der Syscalls in EDT-305. Lexikalische Präfixprüfung und gecachte Dev-Mode-Flag wären auch für `ContentManager::resolveAbsolutePath` (mehrfach `std::filesystem::exists` pro Aufruf, `:983-1000`) sinnvoll.
- `src/HE_Editor/GitController.cpp:436-466` (`toRepoRelative`): normalisiert bei jedem Aufruf beide Pfade (`normaliseSlashes(absolutePath)` und `normaliseSlashes(s.root)`); `s.root` könnte einmal pro Status-Refresh normalisiert gehalten werden. Relevant, weil es pro Kachel und Baumknoten läuft.
- `src/HE_Editor/EditorHelp.cpp:9717-9736` (`panelForTopic`): linearer Scan über `kPanelTopics` mit String-Vergleichen, von DocsPanel pro Sektion und Frame aufgerufen (EDT-302). Ein `unordered_map<string_view, ...>` beim ersten Zugriff würde reichen.
- `AssetThumbnailCache::get` und `EditorAssetTypeCache::assetTypeOf/is` (nicht in diesem Slice) werden pro Kachel und Frame mit dem vollen Pfad-String aufgerufen; ihre Lookup-Kosten (Hash des Pfads, ggf. Mutex) entscheiden mit, wie viel EDT-301 spart.
- `HE::Net::Participant::avatar` (Header außerhalb des Slice): ein vorberechneter Hash würde EDT-312 lösen.
- `HcNodeReference.cpp:225`, `EditorReference.cpp:151`, `EditorGuides.cpp:614`: alle drei rufen `appendPage` je Seite auf (EDT-313).

## Unsicher / Messung nötig

- Tatsächliche Frame-Kosten pro Kachel (EDT-301) hängen von `AssetThumbnailCache::get` und `EditorAssetTypeCache` ab; ohne Profil kein Faktor angebbar. Zuerst mit 2 000/5 000 Dateien profilieren.
- Wie stark `fs::relative` auf den Zielplattformen kostet (EDT-305): libstdc++ und MSVC rufen `weakly_canonical`; libc++ (macOS) verhält sich ähnlich, aber nicht identisch. Messung mit `strace -c` / Process Monitor / `dtruss`.
- Die Größe der Node-Reference-Seite (EDT-302: "ca. 300 Sektionen" laut Kommentar `DocsPanel.cpp:1081-1086`) und der Anteil der CPU-Zeit in `splitWords` gegenüber ImGui-Item-Submission.
- Reale Textmenge von `he-docs.json` samt generierten Seiten (EDT-313/314); 500 KB stammt aus dem Kommentar in `DocsLibrary.h:223-225`.
- Ob die Log-Flood-Szenarien (EDT-308/311) in der Praxis die Konsole offen halten; Verhalten bei 5 000 Zeilen + aktiver Suche messen.
- EDT-307: Soll ein Reload ungespeicherte lokale Änderungen verwerfen? Kommentar sagt ja ("anything unsaved here is stale"), Entscheidung beim Fix bestätigen.

## Gelesen

Alle 19 Dateien des Slice 3 vollständig gelesen (ja), `ContentBrowserPanel.cpp` in fünf Blöcken (1-1300, 1301-2300, 2301-3250, 3251-4150, 4151-5062); nur ergänzend gezielt nachgeschlagen: `ContentManager.cpp:975-1030`, `GlobalState.cpp:132-141`, `GitController.cpp:430-490`, `EditorHelp.cpp:9717-9736`, `EditorReference.cpp`/`EditorGuides.cpp` (Aufrufstellen von `appendPage`).
