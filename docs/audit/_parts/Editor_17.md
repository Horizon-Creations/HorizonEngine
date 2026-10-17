# Editor Slice 17 – TerrainTools … TypeAssetPanel (src/HE_Editor)

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

## Cross-Modul-Hinweise
- `src/HE_Editor/UIEditorPanel.cpp:878, 7097, 7378` und `ExportDialogPanel.cpp:1366` rufen ebenfalls `HcEditorUtil::listAssets` (rekursiver Scan + Header-Reads); falls dort pro Frame statt nur in geöffneten Combos aufgerufen wird, dasselbe Problem wie EDT-1702 (nicht geprüft; Aufgabe für die Slices dieser Dateien). Ein gemeinsamer gecachter `listAssets` (Content-Revision) würde alle beheben (`src/HE_Editor/HcEditorUtil.cpp:93`).
- `src/HE_Scene/src/WaterField.cpp:196` (`Field::wetCells`) ist O(res²) und wird auch von `EngineApi.cpp:1565` (Skript/MCP) gerufen; ein Zellzähler pro Body im Field würde Editor und API entlasten (EDT-1705).
- `src/HE_Core/src/Types/TypeRegistry.cpp:160-168, 222-231`: `getEnum/getStruct` liefern Kopien, `nameCollides` scannt linear; ein `shared_ptr<const Def>`-Zugriff bzw. ein Namensindex würde auch `McpToolsType.cpp:320` entlasten.
- `src/HE_Editor/InputAssetPanel.*` ist laut Kommentar eine weitere Kopie des Asset-Tab-Boilerplates (EDT-1717).
- `src/HE_Editor/EditorApplication.cpp:12323, 12339`: `gitInstallLog`/`toolchainInstallLog` geben den Log by value zurück (Kopie unter Mutex pro Aufruf); ein `string_view`-Snapshot mit Versionszähler wäre günstiger.

## Unsicher / Messung nötig
- EDT-1701: tatsächliche ms pro Frame beim Sculpten bei res 513/1025 (Release) und Anteil Smooth-Vollkopie; Einfluss von `updateTerrains` (Chunk-Rebuild) dürfte dominieren, wenn der Brush groß ist.
- EDT-1709: Kosten von ~20 k `AddRectFilled` im Fit-Modus eines 4K-Bildes; Verfügbarkeit eines Repeat-Samplers in allen sechs Backends (`CreateImGuiTexture`).
- EDT-1705: Standard-Rastergröße des Wassers und typische Body-Anzahl.
- EDT-1704: Größe typischer Content-Bäume beim Tutorial (Sandbox ist klein, daher in der Praxis gering).
- `TextureViewerPanel::load` lädt Texturen vollständig im UI-Thread (`cm.loadAsset`, `toDisplayRgba` kopiert Level 0, 16k-Texturen ≈ 1 GB, Raw-Datei komplett eingelesen und dekodiert); nicht gemessen, bei sehr großen Bildern vermutlich spürbarer Freeze. Kanalwechsel erzeugt pro Klick eine volle CPU-Maskenkopie und neue GPU-Textur (`drawCanvas:320-333`).
- `ToolSetupDialog`: Größe von `GitProbe`/`GitInstallPlan` nicht ausgemessen, Kopierkosten daher geschätzt.

## Gelesen
Ja, alle 16 Dateien des Slice vollständig gelesen (TerrainTools.cpp/.h, TextureColourSpaceDialog.cpp/.h, TextureViewerPanel.cpp/.h, ThemeAssetPanel.cpp/.h, ToolSetupDialog.cpp/.h, TutorialPanel.cpp/.h, TutorialSteps.cpp/.h, TypeAssetPanel.cpp/.h); zusätzlich gezielt nachgeschlagen: `HcEditorUtil.cpp:40-100`, `TypeRegistry.cpp` (getEnum/nameCollides), `WaterField.cpp:196`, `TerrainPaint.cpp:31-60`, `GitProbe.h`.
