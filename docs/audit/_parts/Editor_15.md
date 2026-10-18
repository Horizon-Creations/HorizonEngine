# Editor Slice 15 – ProjectSettingsPanel.cpp … SplineTool.h

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

## Cross-Modul-Hinweise

- `src/HE_SourceControl/src/GitCli.cpp:437-449`: `isValidBranchName` und `branchExists` sind synchrone Prozessaufrufe. Sie sollten in der API als „blockierend“ gekennzeichnet bzw. als asynchrone Variante über `GitService` angeboten werden (siehe EDT-1501). `GitCli::run` mit 10-s-Timeout wird sonst leicht aus UI-Code aufgerufen.
- `src/HE_Scene/src/WaterField.cpp:196-203` (`Field::wetCells`) zählt O(res²) bei jedem Aufruf; ein mitgeführter Zähler pro Body beim Schreiben von `coverage` würde den Aufruf O(1) machen (EDT-1502). `src/HE_Scene/src/WaterLake.cpp:673-690` (`whyNot`) rasterisiert in einer reinen Diagnosefunktion das Polygon.
- `src/HE_Editor/EditorPanelState.h` (`AssetPanelState`): Die Suche nach `relPath` fehlt und ist in mindestens sechs Panels kopiert (EDT-1514).
- `src/HE_Editor/EditorShortcuts.cpp`: `conflicts()`/`label()` sollten für Zeilenweise-Aufrufe pro Frame günstig sein (EDT-1509); nicht in diesem Slice gelesen.
- `src/HE_Editor/AppMetadataRows.*` (`iconPreviewTexture`, `drawIconFileRow`): Wird in `drawApplicationPage` jeden Frame gerufen; Dateisystemzugriffe/Texturaufbau dort wurden nicht geprüft.
- `src/HE_Editor/ViewportPanel.cpp` (`selectionBox`): Wird von jedem offenen Secondary-Viewport zusätzlich zum Hauptfenster pro Frame berechnet (SecondaryViewportPanel.cpp:93); Frame-weiter Cache wäre denkbar.
- `src/HE_Editor/GitController` / `HE_SourceControl::RepoStatus`: `status()` liefert eine Referenz mit festem Ort; `source`-Zeiger-Vergleiche (SourceControlPanel.cpp:340, :1957) tragen deshalb nichts bei. Falls sich `generation` je zurücksetzen kann, wäre eine Projekt-ID im Status die richtige Quelle.

## Unsicher / Messung nötig

- EDT-1504: Ob `RenderWorldPreview`/`RenderSkeletalPreview` eine Slot-Textur über Frames stabil halten und wie teuer ein Frame mit drei offenen Secondary-Viewports in einer realen Szene ist. Nur mit Profiler und Backend-Matrix entscheiden.
- EDT-1502: Reale Kosten von `whyNot` und `wetCells` bei Wassergitter 1024²/4096²; Zeiten im Profiler (Panel nur im Spline-Modus).
- EDT-1511: Anteil der reinen Serialisierung an der Autosave-Spitze bei großen Szenen; die Auslagerung des Schreibens lohnt nur, wenn I/O dominiert.
- EDT-1510: Tatsächliche Hash- und Lesezeit bei Szenendateien von ca. 12 MB (FNV-1a byteweise); Messung mit `remember()` nach `Save`.
- EDT-1508: Obergrenze von `GitController::recentCommits()` und Anzahl Remote-Branches in realen Repos.
- EDT-1509: Implementierung von `EditorShortcuts::conflicts()` (außerhalb des Slice) und Anzahl der Aktionen.
- ProjectSettingsPanel.cpp:596-597: `uiSetFontWeightBold(...)`/`uiSetFontScripts(...)` werden als Abfrage jeden Frame mit Seiteneffekt-Namen aufgerufen („Asking for what is already live answers yes“). Ob der Aufruf bei einer Änderung etwas anstößt (Rebake-Versuch pro Frame) wurde nicht geprüft.
- SceneRecoveryDialog.cpp:125-126: `Escape` wird neben dem Button ausgewertet; ob ImGui das Modal zusätzlich selbst per Escape schließt (und `deferRecovery` dann doppelt oder nie läuft), wurde nicht getestet.
- SequencerPanel.cpp:450-460: `previewActors` schreibt jeden Frame für alle Actors (Kommentar begründet das); bei sehr vielen Actors (> 1.000, ungeclippte Actor-Liste in `drawActors` :401-416) wäre das messbar, ist aber ein Randfall.

## Gelesen

Alle 32 Dateien des Slice wurden vollständig gelesen (ProjectSettingsPanel.cpp/.h, QuickSettingsFavorites.h, ReportIssueDialog.cpp/.h, SceneAutosave.cpp/.h, SceneDiskWatch.cpp/.h, SceneFileStamp.h, SceneRecoveryDialog.cpp/.h, ScriptEditorPanel.cpp/.h, SecondaryViewportPanel.cpp/.h, SelfUpdateDialog.cpp/.h, SequencerPanel.cpp/.h, SequencerTimeline.cpp/.h, ShortcutsPage.cpp/.h, SkeletalMeshEditorPanel.cpp/.h, SourceControlPanel.cpp/.h, SplineEdit.cpp/.h, SplineTool.cpp/.h); zur Verifikation wurden zusätzlich gezielt Ausschnitte aus GitCli.cpp, WaterField.cpp, WaterLake.cpp, EditorConfig.h, EditorGraphics.cpp und per Grep EditorPanelState.h gelesen.
