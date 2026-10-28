# Editor Slice 8 – EditorUI … GitController

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

## Cross-Modul-Hinweise
- `src/HE_Editor/EditorRowWidgets.cpp:429-444`: `Row::MixedScope` baut im Konstruktor eine neue `unordered_map` mit `fieldKey()`-Strings (Allokation je Eintrag) und `mixedMask` ruft pro Zeile `fieldKey(label)` (String-Bau) auf; bei Mehrfachauswahl läuft das für jedes Row-Widget jeden Frame. Normalisierte Keys einmal pro Selection-Änderung cachen.
- `src/HE_Editor/ContentBrowserPanel.cpp:834, 1686, 2045`: Aufrufer von `entryForAbsolutePath/folderHasChanges` (EDT-809); der Browser sollte den Repo-relativen Schlüssel pro Kachel beim Aufbau der Tile-Liste einmal bilden statt je Frame.
- `src/HE_Editor/ProjectHubPanel.cpp:303-` Kopie des Windows-Ordnerdialogs (EDT-814).
- `src/HE_Editor/HcEditorUtil.cpp:93` (`listAssets`): wird vom Picker beim Öffnen und vom Export (`ExportDialogPanel.cpp:1366, 1420`) aufgerufen; prüfen, ob es `EditorAssetTypeCache` nutzt und ob eine Typ-Index-Abfrage am `ContentManager` den Header-Sniff ersetzt.
- `src/HE_Editor/CinematicPanel.*`: hat keine `appendSnapshots`/Recovery-Schnittstelle, `AssetAutosave` deckt Cinematics damit vermutlich nicht ab (EDT-801).

## Unsicher / Messung nötig
- Kosten von 16 `AssetPanelState::dirty`-Lookups je Tab und Frame (EDT-801/806) bei realistisch 10-30 Tabs.
- Snapshot-Größe und Speicherbedarf von bis zu 64 Welt-Snapshots in `EditorUndo` (EDT-818).
- Dauer des rekursiven `.hescene`-Scans (EDT-811) bei Projekten mit `.git`/`Export`-Ordnern; Thread-Sicherheit von `ContentManager`/`syncPrefabInstances`, falls das Serialisieren in den Worker wandert.
- Ob `HcEditorUtil::listAssets` den Header-Sniff-Cache nutzt (EDT-808).
- Ob das Fehlen von `SkeletalMeshEditorPanel` in `reloadAssetTabFromDisk` und von `CinematicPanel` in `appendAssetSnapshots` Absicht ist (EDT-801).
- Verhalten, wenn `Row`-Aufrufer `assetDropSlot` mit `idSuffix == nullptr` rufen (EDT-807); nicht gesucht.

## Gelesen
Alle 26 Dateien des Slice vollständig gelesen: ja (EditorUI.cpp/.h, EditorUiShot.cpp/.h, EditorUndo.cpp/.h, EditorViewportNav.cpp/.h, EditorWidgets.cpp/.h, EngineContentPublishDialog.cpp/.h, EngineContentSyncBar.cpp/.h, EnvironmentPanel.cpp/.h, ExportDialogPanel.cpp/.h, FrustumLines.h, GameInstancePanel.h, GameLogicBuildPanel.cpp/.h, GitCloneDialog.cpp/.h, GitController.cpp/.h); außerhalb des Slice nur gezielte Stichproben (`EditorPanelState.h`, `EditorRowWidgets.cpp`, `PlayErrorReveal.h`, `RepoStatus.h`, `ProjectHubPanel.cpp`, vendor `imgui.cpp:13217`).
