# Editor Slice 10 – HcGraphHost … InputMappingModel (31 Dateien, 10 964 Zeilen)

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

## Cross-Modul-Hinweise

- `src/HE_Editor/HcEditorUtil.cpp:93-100, 105-127` (`listAssets`/`listScenes`): ungecachte rekursive Verzeichnis-Walks mit Header-Reads (`HAsset::readAssetTypeFromFile`), von vielen Draw-Pfaden aufgerufen (u. a. EDT-1001, EDT-1002, HcGraphHost-Combos, HcPullUi:286). Ein zentraler, über `EditorAssetTypeCache::invalidate*`/Datei-Watcher invalidierter Cache würde alle Aufrufer entlasten. Gehört in den Slice von HcEditorUtil.
- `src/HE_Editor/HcEditorUtil.cpp:1693-1790` (`drawPinDefaultEditor`): ruft die `choices`-Funktion vor `BeginCombo` (siehe EDT-1002); Signatur `ParamChoices` sollte in „hat Auswahl?“ und „Auswahl holen“ geteilt werden.
- `src/HE_Editor/HcEditorUtil.cpp:506-519` (`valueTypeName`) und `:383` (`pinTypeName`): weitere Kopien der Typnamen-Tabelle (EDT-1008).
- `src/HE_Editor/GraphEditor.cpp:283-360, 677`: ruft `model.pins`, `model.title`, `model.getPos`, … je Node und Frame; ein optionaler „NodeView“-Callback (liefert Pins, Titel, Farbe, Position in einem Aufruf mit einer Suche) würde das mehrfache `findNode` und die Signatur-Neuberechnung sparen (zu EDT-1004).
- `src/HE_Core/include/GraphCommon/GraphModel.h:40-62`: `findNodeById` ist linear; für Graphen >~200 Nodes wäre ein optionaler id→index-Cache im Graph (invalidiert bei add/remove) sinnvoll.
- `src/HE_Core/include/HorizonCode/HorizonCode.h:752-753`: `signatureOf` liefert vier Vektoren per Wert; viele Editor-Aufrufer brauchen nur Zählwerte (`signatureCountsOf`) oder eine einzelne PinDesc.
- `src/HE_Editor/UIEditorPanel.cpp:6594`, `LevelScriptPanel.cpp:1217`, `AnimatorStateMachineEditorPanel.cpp:764`: rufen `buildModel` je Frame (syncCreateWidgetPins + ~40 Lambdas + QuickSpawn-Vektor werden jeden Frame neu aufgebaut). Ein langlebiges Model mit aktualisierter `Host`-Referenz wäre billiger; nur messen, falls Frame-Zeit auffällt.

## Unsicher / Messung nötig

- EDT-1002: Wie viele Pins pro Frame tatsächlich `drawPinInlineEditor` erreichen (Viewport-Culling in GraphEditor?) und wie teuer `listScenes`/`listAssets` in einem realen Projekt sind.
- EDT-1001: Kosten von `listAssets` bei großem `Content/`-Baum (Disk-Cache-Effekte); ein Frame-Zeitprofil mit offenem Input-Mapping-Tab.
- EDT-1004: Ab welcher Node-Zahl der O(N²)-Anteil sichtbar wird (Schätzung 300-500+).
- EDT-1006: Ob `HorizonCode::Runtime` einen Versionszähler für den angehaltenen Run bietet (Voraussetzung für verlustfreies Caching).
- EDT-1007: Vorfilter per Substring-Check – Verhalten bei JSON-escapten Zeichen im Namen (Serializer-Einstellungen von `HorizonCode::toJson` prüfen).
- EDT-1003: Aufwand eines `resolveClassAsset` pro Frame (hängt von Kettenlänge/Graphgröße).
- HcGraphHost `buildModel` pro Frame: Kosten der ~40 `std::function`-Zuweisungen sind je nach STL-Implementierung (SBO-Größe: libstdc++ 16 B, libc++ 24 B, MSVC 56 B) unterschiedlich; Lambdas mit zwei Referenz-Captures passen überall, `quickSpawns` mit Capture `[&h, t]` ebenfalls – nicht gemessen.

## Gelesen

Vollständig gelesen: alle Dateien des Slice mit Ausnahme von `src/HE_Editor/HcNodeDocs.cpp`, von dem nur Zeilen 1-120 und 2080-2244 (Kopf, Tabellenende, Env-Felder, alle Funktionen) sowie per Skript Duplikat-IDs (keine) geprüft wurden; die ~2000 Tabellenzeilen dazwischen sind reine Daten (`{ id, doc }`) und wurden nicht einzeln gelesen. Zusätzlich punktuell gelesen (außerhalb des Slice, zur Belegung): HcEditorUtil.cpp (listAssets, drawPinDefaultEditor, engineParamChoices), GraphEditor.cpp (Model-Aufrufer), GraphViewStore.h, GraphModel.h, HorizonCode.h (PinType, NodeSig), InputMappingModel.h, EditorAssetTypeCache.cpp.
