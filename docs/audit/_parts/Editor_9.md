# Editor Slice 9 – GitHubSignIn* · GitMissingDialog · GraphEditor* · GraphViewStore · Guides/* · Hc* (EditorUtil, ExecTrace, Extract, ExtractUi, ExtractUiBind, FallbackReport, GraphClipboard)

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

## Cross-Modul-Hinweise
- `src/HE_Core/include/GraphCommon/GraphModel.h:45-57`: `findNodeById` ist ein linearer Scan; wird von `HorizonCode::Graph::findNode` (`src/HE_Core/src/HorizonCode/HorizonCode.cpp:1228`) und damit von allen `Model`-Callbacks in `HcGraphHost.cpp:684-699` pro Knoten/Frame genutzt (Teil von EDT-903). Ein Index (id→Position) im Graph-Container oder eine Frame-Map im Host wäre die zentrale Abhilfe auch für Material-/Partikel-/Animator-Hosts.
- `src/HE_Editor/HcGraphHost.cpp:899-905` und `src/HE_Editor/UIEditorPanel.cpp:6523` (Host-`paramChoices`) liefern die Listen für EDT-901 ebenfalls pro Frame; dort prüfen, ob eigene Scans (Widget-Animationen/Elemente) stattfinden.
- `src/HE_Editor/EditorWidgets.cpp:121-190`: Asset-Picker mit Scan beim Öffnen (korrekt), aber Filter/Label pro Frame ohne Clipper (EDT-910). Ähnliche Muster in `MaterialEditorPanel.cpp:1323/1762`, `TypeAssetPanel.cpp:264`, `InputAssetPanel.cpp:704`, `LevelScriptPanel.cpp:1832/3031`, `InspectorPanel.cpp:3212`, `ExportDialogPanel.cpp:1366/1420`, `HcGraphHost.cpp:1817/2185/2207/2236` rufen `listAssets`/`listHorizonCodeClasses` auf – jeweils prüfen, ob im Draw pro Frame statt einmalig beim Öffnen.
- `src/HE_Editor/MaterialEditorPanel.cpp:135`: zweite `categoryColor`-Implementierung (EDT-913).
- `src/HE_Core/include/HorizonCode/HorizonCode.h:754-761`: Allokationsfreie Accessoren existieren; „HcCodegen and the editor panels still build full NodeSigs; they can adopt these too“ – weitere Panel-Aufrufer von `signatureOf` mit `grep` prüfen.

## Unsicher / Messung nötig
- EDT-903: tatsächlicher Anteil von Callbacks/`findNode` an der Frame-Zeit bei realen Graphen (z. B. 200/1000 Knoten); Profil mit Tracy/`perf` vor Umbau.
- EDT-901/902: Kosten von `listScenes`/`listAssets` je Projektgröße (Dateianzahl, Netzlaufwerk/Windows-Defender); Messung der Frame-Zeit mit offenem Combo bzw. 10 `scene.*`-Knoten.
- EDT-906: Anteil des Exec-Listeners an der Interpreter-Laufzeit im Play-Modus (Microbenchmark mit Schleifengraph).
- EDT-907: ob `ContentManager` einen Revisionszähler pro Asset bereitstellt (nötig für Cache-Invalidierung).
- EDT-916: ob der Compiler `std::fill` + `clear()` in `wipe()` entfernt (nur per Disassembly der Release-Build-Binärdatei prüfbar).
- EDT-915: Größe typischer Installer-Logs (winget/brew) und Threading des `gitInstallLog`-Getters.

## Gelesen
Alle 26 Dateien des Slice wurden vollständig gelesen: ja (`GraphEditor.cpp` in vier Blöcken, `HcEditorUtil.cpp` in vier Blöcken, alle Guides-Dateien komplett); zusätzlich gezielte Greps in `HcGraphHost.cpp`, `EditorWidgets.cpp`, `GraphModel.h`, `HorizonCode.h`, `DocsPanel.cpp`, `EditorGuides.cpp` zur Verifikation von Aufrufern.
