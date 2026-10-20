# Editor Slice 1 – src/HE_Editor (Panels A–V, Mcp*, Hc*, Task*, Team*, Viewport*, UIEditorPanel, Inspector, Outliner u. a.)

## Zusammenfassung

Der Editor-Code ist an den bekannten Hot-Spots bereits bewusst optimiert (Outliner-Row-Clipping, Profiler-Analysecache, Notification-Frame-Cache, 1-s-Polling in SceneDiskWatch/TaskBoard, Thumbnail-Budgets). Die belegten Funde konzentrieren sich deshalb auf (1) Panels, die das Muster "Liste/Baum jeden Frame neu bauen und ungeclippt zeichnen" noch nicht übernommen haben (SourceControl-Baumansicht, Inspector-Entity-Picker), (2) lineare Suchen über Entity-/Elementlisten mit String-Formatierung pro Element (`entityByUuid`, `UIWidgetTree::find`), (3) MCP-Tools, die auf dem UI-Thread unnötig viel serialisieren (`entity_list`), und (4) Duplikation (MCP-Helfer, Mesh-Bounds-Caches, 13x `appendSnapshots`, 2865-Zeilen-Funktion im Inspector). Insgesamt 18 Funde: 3 Hoch, 9 Mittel, 6 Niedrig. Fast alle Performance-Funde sind ohne Messdaten geschätzt und entsprechend markiert.

---

### EDT-101 – SourceControl-Baumansicht (Default) baut Verzeichnisbaum jeden Frame neu und zeichnet alle Zeilen ungeclippt
- **ID**: EDT-101
- **Kategorie**: D (auch C)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/SourceControlPanel.cpp:659-712 (insertRow/drawDirNode/drawRows), :69 (`s_treeView = true`), :872-955 (drawChangesTab)
- **Problem**: `s_treeView` ist standardmäßig `true`. In diesem Modus legt `drawRows` pro Frame und pro Sektion einen neuen `DirNode`-Baum an (`std::map<std::string, DirNode>`, pro Pfadsegment ein `substr`-String und ein Map-Knoten) und ruft dann für JEDE Änderung `rowFn` auf. Nur der Flat-Zweig benutzt `ImGuiListClipper`. Der Kommentar dort nennt selbst das Problem ("a freshly imported project can have thousands of untracked rows"), im Default-Modus greift der Schutz aber nicht. Das widerspricht der Regel aus CLAUDE.md Abschnitt 4 (nur sichtbare Zeilen zeichnen). Das Modell (`modelFor`) wird dagegen korrekt nur bei neuer `generation` gebaut.
- **Code-Auszug**:
```cpp
if (s_treeView)
{
    DirNode root;
    for (const ChangeRow& r : rows) insertRow(root, r);          // Allokationen pro Frame
    for (const auto& [name, sub] : root.dirs) drawDirNode(name, sub, rowFn);
    ...
}
else { ImGuiListClipper clip; ... }                               // nur hier geclippt
```
- **Lösung**: Den `DirNode`-Baum im `Model` (`rebuildModel`) mit aufbauen und je Sektion cachen (hängt nur an `generation`/Filter/Suche, die `modelFor` schon prüft). Zum Zeichnen den Baum einmal in eine flache Liste "sichtbarer Knoten" (Ordner-Header + Datei, mit Tiefe und Faltzustand) abflachen und diese mit `ImGuiListClipper` zeichnen; Faltzustand in einem `unordered_set<pathKey>` statt im ImGui-TreeNode-Zustand halten (oder, kleinerer Schritt: nur den Baum cachen, ungeclippt lassen und Row-Höhe konstant halten).
- **Erwarteter Effekt**: (Schätzung) Bei einigen tausend Änderungen von Millisekunden pro Frame auf O(sichtbare Zeilen). Bei wenigen Dutzend Änderungen unmerklich.
- **Risiko / Verifikation**: Faltverhalten ("Single-child chains fold") und DefaultOpen müssen identisch bleiben. Ein UI-Test im Stil `tests/test_outliner_ui.cpp` (Sektion mit 5000 Zeilen, Frame-Zeit/Zeilenzahl prüfen) fehlt für dieses Panel; zuerst per Profiler mit 5000 untracked Dateien messen.

---

### EDT-102 – Inspector: vier Entity-Picker durchlaufen bei jedem Frame ALLE benannten Entities
- **ID**: EDT-102
- **Kategorie**: D (auch A: Copy-Paste)
- **Schweregrad**: Hoch
- **Typ**: Beides
- **Ort**: src/HE_Editor/InspectorPanel.cpp:1863-1873 (LookAt), :2472-2482 (Camera Rig), :2648-2657 (Attachments), :2996-3005 (Joint)
- **Problem**: Jeder gezeichnete Component-Abschnitt mit einem Entity-Verweis baut pro Frame zwei Vektoren (`names`, `ids`) über `registry.view<NameComponent>()` über die gesamte Szene, mit `all_of<TransformComponent>`-Test und `world.entityId(e)` (valid + try_get) je Entity — auch wenn die Combo gar nicht geöffnet ist. Bei der in CLAUDE.md/Outliner-Kommentaren genannten Größenordnung (10k-50k Entities) ist das pro Abschnitt ein O(N)-Durchlauf plus zwei Heap-Allokationen mit Wachstum, jeden Frame, solange eine Entity mit so einer Komponente ausgewählt ist. Der Code ist viermal fast identisch.
- **Code-Auszug**:
```cpp
std::vector<const char*> names{ "(none)" };
std::vector<HE::UUID>    ids{ HE::UUID{} };
for (auto [e, name] : registry.view<NameComponent>().each())
{
    if (e == entity || e == world.rootEntity()) continue;
    if (!registry.all_of<TransformComponent>(e))  continue;
    const HE::UUID id = world.entityId(e);
    ...
    names.push_back(name.name.c_str());
    ids.push_back(id);
}
```
- **Lösung**: Einen Helfer `Row::entityCombo(label, HE::UUID& target, EntityFilter, noneLabel)` in `EditorWidgets` anlegen, der (a) das aktuelle Ziel per `world.findByEntityId(target)` auflöst (O(1), Index existiert: HorizonWorld.cpp:434) und als Vorschau-Text zeigt und (b) die Liste NUR innerhalb von `ImGui::BeginCombo` aufbaut (mit `ImGuiListClipper` bei vielen Einträgen). Die vier Stellen auf den Helfer umstellen. Für die Hilfe-Audit (`editor_help_audit.py --check`) die Labels unverändert lassen.
- **Erwarteter Effekt**: (Schätzung) von O(N) pro Frame und Abschnitt auf O(1) im Normalfall; deutlich weniger Code (4x ca. 15 Zeilen).
- **Risiko / Verifikation**: Reihenfolge der Einträge (Registry-Reihenfolge) und "(world point)"/"Player (possessed)" als Index 0 beibehalten. Prüfen, ob `Row::combo` Items per Index braucht (dann `BeginCombo` + `Selectable` selbst schreiben). Manuell mit Szene mit 10k Entities + Camera Rig testen; gibt es einen `test_inspector_ui`, dort Auswahl-Roundtrip ergänzen.

---

### EDT-103 – MCP `entity_list` serialisiert alle Komponenten JEDER Entity, auch jenseits von `limit`/Filter
- **ID**: EDT-103
- **Kategorie**: D (auch C)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/McpToolsEntity.cpp:145-174 (`componentsOf`/`entitySummary`), :330-362 (Handler)
- **Problem**: `entitySummary` ruft `componentsOf`, das die komplette Entity per `SceneSerializer::serializeEntityComponents` in CBOR bringt und dann zu `json` parst — nur um die Schlüsselnamen der Komponenten zu bekommen. Das passiert für alle Entities der Szene, bevor `nameContains`/`component`/`limit` greifen; der Handler zählt über `limit` hinaus weiter (`++truncated`) und serialisiert dabei trotzdem jede weitere Entity. Ein Terrain trägt laut Kommentar in InspectorPanel.cpp:109-114 ein Viertel Million Floats. Der Handler läuft synchron im Frame-Thread (McpBridge::update), blockiert also den Editor.
- **Code-Auszug**:
```cpp
json summary = entitySummary(*world, e);          // volle Serialisierung -> CBOR -> json
if (summary["uuid"].get<std::string>().empty()) continue;
if (!needle.empty()) { std::string name = summary["name"]...; ... }
if (static_cast<int>(out.size()) >= limit) { ++truncated; continue; }
```
- **Lösung**: Filter vor der teuren Arbeit: erst Name (`nameOf`) und uuid prüfen (billig), `limit` früh abbrechen bzw. `truncated` nur zählen, ohne zu serialisieren. Die Komponenten-Schlüssel nicht aus dem JSON gewinnen, sondern aus `InspectorPanel::listComponents(ctx, world, e, out)` bzw. einer `SceneSerializer`-Funktion "Komponentenschlüssel einer Entity" (Schlüssel-Liste gibt es in der Szene-Serialisierung bereits; `listComponents` ist im Inspector vorhanden). Vorher prüfen, dass die Schlüsselmengen identisch sind (z. B. `__name` wird in :163 ausgeschlossen).
- **Erwarteter Effekt**: (Schätzung) bei 10k Entities von mehreren Sekunden/Hitch auf wenige Millisekunden bei Namensfilter bzw. kleinem `limit`.
- **Risiko / Verifikation**: Ausgabe von `entity_list` muss unverändert bleiben (Reihenfolge der `components`-Schlüssel). Golden-Test: gleiche Szene vorher/nachher vergleichen (es gibt MCP-Tool-Tests in `tests/`; per `grep -l entity_list tests/*.cpp` prüfen und ergänzen).

---

### EDT-104 – `entityByUuid`: lineare Suche mit `snprintf`+String-Allokation pro Entity statt vorhandenem Hash-Index
- **ID**: EDT-104
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/EditorCommands.cpp:406-429 (`uuidOf`, `entityByUuid`); Aufrufer TaskBoardPanel.cpp:538-550 (alle 1 s pro Entity-Link), :579, :603; McpToolsEntity.cpp:206 (jeder MCP-Call), McpToolsPrefab.cpp:377/523; EditorApplication.cpp:10613
- **Problem**: Jede Auflösung formatiert für jede Entity der Szene die UUID als 32-Zeichen-Hex-String (`snprintf` + `std::string`) und vergleicht sie mit dem Suchstring; ein früher Abbruch fehlt (`each` läuft weiter, der Lambda kehrt nur sofort zurück). `HorizonWorld::findByEntityId(const UUID&)` (HorizonWorld.cpp:434) nutzt bereits einen über Signale gepflegten Hash-Index (`m_idIndex`). Das Task-Board löst z. B. jede Sekunde für jeden Entity-Link einmal in O(N) auf (refreshOrphans).
- **Code-Auszug**:
```cpp
reg.view<const EntityIdComponent>().each([&](auto e, const EntityIdComponent&) {
    if (found != entt::null) return;
    if (uuidOf(world, e) == uuid) found = e;     // snprintf + std::string pro Entity
});
```
- **Lösung**: Hex-String einmal zu `HE::UUID{hi,lo}` parsen (Gegenstück zu `uuidOf`, 16+16 Hexziffern, ungültig -> `entt::null`) und `world.findByEntityId(id)` aufrufen. Beim Task-Board die Link-Id direkt als `HE::UUID` übergeben (sie liegt schon als `l.id` vor; `uuidToHex(l.id)` -> Parsing entfällt).
- **Erwarteter Effekt**: O(N) -> O(1); bei 50k Entities spart das pro Auflösung ca. 50k Allokationen (Schätzung).
- **Risiko / Verifikation**: `m_idIndex` ist ein `unordered_multimap` — bei doppelten Ids (zweimal eingefügtes Prefab erhält laut HorizonWorld.cpp:411-415 eigene Ids, ein Szenenladen könnte aber Duplikate enthalten) kann sich die Trefferreihenfolge gegenüber "erste in View-Reihenfolge" unterscheiden. Test: Round-Trip `uuidOf` -> neue Funktion für alle Entities einer geladenen Szene; Duplikat-Fall explizit festlegen.

---

### EDT-105 – UI-Designer-Canvas: O(N²·Tiefe) pro Frame durch lineares `UIWidgetTree::find` in Tiefen-/Layout-Berechnung
- **ID**: EDT-105
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/UIEditorPanel.cpp:5190-5206 (drawCanvas, Hauptcanvas), :4754-4772 (eingebettetes Widget); src/HE_Core/include/GraphCommon/GraphModel.h:40-47 (`findNodeById`, linear); src/HE_Core/src/UIWidget/UIWidgetTree.cpp:16-19, :1022-1048 (`uiElementRectUnbounded`)
- **Problem**: Für jedes sichtbare Element wird die Tiefe durch Hochlaufen der Elternkette bestimmt, jeder Schritt ruft `tree.find(parentId)` (lineare Suche über alle Elemente). Danach rechnen `elementCanvasRect`/`uiElementRect` (rekursiv über die Eltern, jeweils wieder `tree.find`, dazu eine `dynamic_cast`-Kette) und später `uiElementClipRect`/`uiElementEffectiveVisible` ebenfalls entlang der Eltern. Das Ergebnis: grob N · Tiefe · N Vergleiche pro Frame, solange der Designer-Tab offen ist. `GraphModel.h:40` begründet die lineare Suche mit "Graphs are small (tens of nodes)" — UI-Widgets können deutlich größer sein. Zusätzlich laufen `uiApplyAutoSize(st.tree, …)` und `refreshDesignSlots(...)` bei jedem Frame (:5183-5187), und `items` wird ohne `reserve` gefüllt.
- **Code-Auszug**:
```cpp
for (const auto& ep : st.tree.elements) {
    ...
    int depth = 0;
    for (const UIElement* c = &n; c->parentId != 0 && depth < 255; ++depth) {
        const UIElement* p = st.tree.find(c->parentId);   // O(N)
        if (!p) break; c = p;
    }
    items.push_back({ &n, n.layer * 256 + depth, depth, elementCanvasRect(st.tree, n, layoutCanvas) });
}
```
- **Lösung**: (1) Im Editor vor der Schleife einmal eine Map `id -> Element*` (oder sortierten Index-Vektor) bauen und Tiefe per Memoisierung (`depthOf[id]`) in O(N) bestimmen. (2) Für den nachhaltigen Fix im Core `UIWidgetTree::find` über einen lazily gepflegten Index beschleunigen (Invalidierung bei `appendNode`/Löschen/Undo — siehe Kommentar in GraphModel.h:41-42, daher Core-Entscheidung). Die rekursive Rect-Berechnung bleibt dann O(N·Tiefe).
- **Erwarteter Effekt**: (Schätzung) bei 200-500 Elementen von einigen 100k-Millionen Vergleichen auf wenige Tausend pro Frame; messen mit Profiler-Scope um `drawCanvas`.
- **Risiko / Verifikation**: Zeichenreihenfolge (stable_sort nach `layer*256+depth`) darf sich nicht ändern. `tests/test_ui_widgets.cpp` (nicht ganz lesen, per Grep auf "uiElementRect") deckt die Layout-Funktionen ab; Index-Variante im Core braucht Tests für Einfügen/Löschen/Umhängen.

---

### EDT-106 – Outliner: pro sichtbarer Zeile Subtree-Walk (`subtreeVisibility`) und O(N)-Setup pro Frame trotz Clipping
- **ID**: EDT-106
- **Kategorie**: D (auch C)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/OutlinerPanel.cpp:1561-1562 (`subtreeVisibility`), :1100-1101 (`visibleRows.reserve`), :1114-1130 (`selectedPath`), :1133-1183 (Schleife über alle Cache-Knoten); src/HE_Scene/src/EntityVisibility.cpp:51-66
- **Problem**: (a) `HE::subtreeVisibility(regC, node.entity)` läuft rekursiv durch den ganzen Teilbaum, bis ein sichtbarer Renderer gefunden wird. Für eine sichtbare Zeile mit großem Teilbaum ohne Renderer (Ordner/Logik-Entities, oder komplett versteckter Teilbaum) ist das ein voller Walk — jede Frame, für jede gezeichnete Zeile (Wurzel ausgenommen). Die Kommentare im Panel behaupten "state is read from the registry every frame", aber ein Dirty-Flag/Cache fehlt. (b) `visibleRows` reserviert `s_outlinerCache.size()` pro Frame und wird für alle nicht gefilterten Zeilen (auch geclippte) befüllt, obwohl es nur für den Shift-Klick gebraucht wird. (c) Die Schleife besucht auch bei Clipping alle N Knoten (je `rowId`-Hash + `GetStateStorage()->GetInt`), (d) `selectedPath` (`unordered_set`) und `pathIds`/`pathEnt` werden pro Frame neu allokiert.
- **Code-Auszug**:
```cpp
visibleRows.reserve(s_outlinerCache.size());               // jeden Frame ~4 Byte * N
...
const HE::Visibility vis = isRootRow ? HE::Visibility::None
                                     : HE::subtreeVisibility(regC, node.entity);   // O(Teilbaum)
```
- **Lösung**: `visibleRows` nur befüllen/auflösen, wenn `shiftRangeTarget` gesetzt ist (Klick-Frame: Schleife-Information über zweiten Durchlauf oder `ImGui::IsMouseClicked && io.KeyShift` als Vorbedingung). `subtreeVisibility` nur für Zeilen im Sichtbereich ist bereits gegeben; zusätzlich den Eye-Zustand cachen (z. B. `Visibility` pro Knoten im `OutlinerNode`, neu berechnet in dem bereits vorhandenen Slice-Sweep `s_sweepAt`, 2048 Knoten/Frame) statt synchron zu walken. Container (`selectedPath`, `pathIds`) als `static` wiederverwenden (`clear()`).
- **Erwarteter Effekt**: (Schätzung) bei Szenen mit großen Gruppen-Entities (z. B. 10k Kinder in einem Ordner) vom Walk über 10k Entities pro Frame auf O(1) pro Zeile; bei 50k Entities ca. 200 KB weniger Allokation/Frame.
- **Risiko / Verifikation**: Das Auge muss sofort reagieren, wenn `visible` im Inspector oder per Skript geändert wird (Kommentar :1789-1790) — daher Sweep-Cache nur mit kurzer Latenz akzeptabel; sonst belassen und nur (b)/(d) umsetzen. `tests/test_outliner_ui.cpp` deckt Klick-Verhalten ab; Shift-Range und Filter-Fälle dort prüfen.

---

### EDT-107 – NotificationBar/Popups kopieren pro Frame den vollen Notification-Vektor (bis 200 Einträge mit Strings und `std::function`)
- **ID**: EDT-107
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/NotificationBar.cpp:54-80 (`refresh`), src/HE_Editor/NotificationStore.cpp:164-168 (`snapshot`), :191-198 (`liveToasts`), :200-210 (`nextModal`); NotificationPopups.cpp:150, :173
- **Problem**: Der Frame-Cache der Bar kopiert jeden Frame die komplette Liste (`k_maxEntries = 200`; jeder Eintrag: `title/text/detail/assetPath/key` Strings, `actions`-Vektor mit `std::function`s) unter dem Store-Mutex, den auch Worker-Threads nehmen. Obendrein legen die Popups pro Frame eigene Kopien an (`liveToasts` -> neuer Vektor, `nextModal` -> Kopie eines `Notification`). Wirklich gebraucht werden `unseen`, `worstUnseen`, `newestProblemMs` und bei Öffnen des Panels die Liste.
- **Code-Auszug**:
```cpp
s_cache.entries = ctx.notifications->snapshot();     // Kopie aller Einträge, jeden Frame
std::vector<Notification> live = ctx.notifications->liveToasts(now);
```
- **Lösung**: Im Store einen `m_revision`-Zähler (bei post/resolve/settle/clear) und O(1)-Aggregate (`unseen`, `worstUnseen`, `newestProblemMs`) pflegen; `NotificationBar::refresh` liest nur Aggregate (ein Lock, keine Kopie) und holt die Liste (`snapshot()`) nur, wenn das Notification-Center sichtbar ist und sich `revision` geändert hat. `liveToasts`/`nextModal` über Zähler "gibt es überhaupt einen aktiven Toast/Modal" vorab abfragen.
- **Erwarteter Effekt**: (Schätzung) Bei leerem/kleinem Store kaum messbar; bei vielen Einträgen (nach langen Sessions, 200 Stück mit Detailtext) Wegfall von mehreren hundert Heap-Allokationen pro Frame und weniger Mutex-Konkurrenz mit Worker-Threads.
- **Risiko / Verifikation**: Die "ein Snapshot pro Frame"-Konsistenz (Kommentar :24-39) muss erhalten bleiben: Aggregate und angezeigte Liste müssen aus demselben Stand stammen. Test: vorhandene Notification-Tests (`grep -l NotificationStore tests/*.cpp`) um Revision/Aggregat-Fälle ergänzen.

---

### EDT-108 – Asset-Autosave sammelt jeden Frame Snapshot-Quellen aus 15 Panels (13 nahezu identische `appendSnapshots`)
- **ID**: EDT-108
- **Kategorie**: A (Duplikation) / D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Editor/EditorApplication.cpp:~13700-13722 (`updateAssetAutosave`), src/HE_Editor/EditorUI.cpp:1061-1078 (`appendAssetSnapshots`), AssetAutosave.cpp:240-290 (`update`); Beispiele: ScriptEditorPanel.cpp:118-126, BoneMaskPanel.cpp:254-268, BlendSpacePanel.cpp:284-298, ThemeAssetPanel.cpp:471-486, TypeAssetPanel.cpp:345+, InputAssetPanel.cpp:420+, SequencerPanel.cpp:513-527, AudioEditorPanel.cpp:619-633, ParticleGraphEditorPanel.cpp:90-104, UIEditorPanel.cpp:6905+, MaterialEditorPanel.cpp:1971+
- **Problem**: Pro Frame wird `appendAssetSnapshots` aufgerufen (15 Panel-Funktionen). Für jedes dirty Asset entsteht ein `AssetSnapshotSource` mit `cm->resolveSavePath(...)` (Stringbau) und einer `std::function` mit Capture (Heap). Danach baut `AssetAutosave::update` pro Frame ein `unordered_set<string>` und ruft `keyFor` (Cache-Lookup) für jeden Eintrag — nur um das Pruning ("Datei verließ die Dirty-Liste") und eine Intervall-Prüfung (60 s) auszuführen. Die Panel-Funktionen sind zudem fast Copy-Paste (Muster: `forEach` -> `if (!dirty) return` -> `resolveSavePath` -> Lambda mit `getXxx(id)` -> `copy = *a; copy.json = toJson(st); writeAssetTo`).
- **Code-Auszug**:
```cpp
s_states.forEach([&](const std::string&, PanelState& st) {
    if (!st.dirty || st.relPath.empty()) return;
    out.push_back({ cm->resolveSavePath(st.relPath), [cm, &st](const std::string& dest) { ... } });
});
```
- **Lösung**: Zweistufig. (1) Pull statt Push: `AssetAutosave` bekommt eine Callback-Funktion; die Panels liefern pro Frame nur eine billige Menge dirty-Pfade (`appendDirtyPaths` existiert bereits, z. B. ScriptEditorPanel.cpp:113) für das Pruning. Writer-Lambdas werden nur gebaut, wenn das Intervall abgelaufen ist. (2) Duplikation: gemeinsame Vorlage `template<class State, class Asset> appendDirtyAssets(s_states, cm, getFn, encodeFn, out)` in `AssetPanelState.h`, die die Panels nur noch mit `getFn`/`encodeFn` parametrisieren.
- **Erwarteter Effekt**: Pro Frame bei 0 dirty Assets nur 15 leere `forEach`-Läufe + leeres Set (geringer Gewinn); bei mehreren dirty Tabs entfallen Strings/`std::function`-Allokationen pro Frame. Hauptgewinn: ca. 150-200 Zeilen weniger Boilerplate (Schätzung).
- **Risiko / Verifikation**: Das Pruning muss weiterhin "Save irgendwo entfernt die Kopie" leisten (Kommentar EditorApplication.cpp ~13708-13711). Test: vorhandene Autosave-Tests (`grep -l AssetAutosave tests/*.cpp`) um "speichern entfernt Live-Kopie" erweitern.

---

### EDT-109 – Multi-Selektion im Inspector: Voll-Refresh (alle Mitglieder serialisieren) alle 30 Frames; zusätzlich Kopien pro Frame
- **ID**: EDT-109
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/InspectorPanel.cpp:162-203 (`refreshMultiSelection`), :245-252 (Periode und Kopien), :254 (`membersToSkip` pro Frame)
- **Problem**: Der Cache wird bei kleinen Selektionen (<=256 Mitglieder) alle 30 Frames (~0,5 s) komplett neu berechnet: pro Mitglied `listComponents` (läuft die gesamte `renderForImpl`-Kette im Collect-Modus) plus `EditorMultiEdit::state` (JSON des ganzen Mitglieds). Bei 256 Mitgliedern ist das ein periodischer Hitch. Die Periode ist framebasiert (bei 144 Hz also 0,2 s). Außerdem werden `common`/`partial` pro Frame kopiert (`const std::vector<std::string> common = s_multi.common;`) und `membersToSkip(...)` (mit Collab-Lock-Abfragen pro Mitglied) läuft jeden Frame über alle Mitglieder.
- **Code-Auszug**:
```cpp
const int period = members.size() <= 256 ? 30 : 240;   // frames
if (s_multi.stale || ... || ImGui::GetFrameCount() - s_multi.frame >= period)
    refreshMultiSelection(ctx, world, primary, members);
const std::vector<std::string> common  = s_multi.common;     // Kopie pro Frame
const std::vector<std::string> partial = s_multi.partial;
```
- **Lösung**: Periode zeitbasiert (z. B. 1 s, per `ImGui::GetTime()`), Refresh auf mehrere Frames verteilen (Mitglieder-Slices wie `s_sweepAt` im Outliner) und `common`/`partial` als `const&` verwenden (falls ein Refresh mitten im Frame die Referenz entwertet, den Refresh erst nach dem Zeichnen auslösen). `membersToSkip` nur neu berechnen, wenn Selektions-Revision oder Collab-Zustand wechselt.
- **Erwarteter Effekt**: (Schätzung, messen) Wegfall eines periodischen Spikes von potenziell zweistelligen Millisekunden bei 256 Mitgliedern; kleine Allokationsersparnis pro Frame.
- **Risiko / Verifikation**: Änderungen von Gizmo/Skript/Peers sollen weiterhin zeitnah sichtbar sein (Kommentar :143-148). Messen mit Profiler (Select-All auf 256 Entities), danach Latenz der "(mixed)"-Anzeige prüfen.

---

### EDT-110 – Szenen-Autosave serialisiert die gesamte Szene als JSON synchron im Frame-Thread; `SceneFileStamp::remember` hasht die ganze Datei synchron
- **ID**: EDT-110
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/EditorApplication.cpp:~13687-13693 (`updateAutosave`, `SerializeFormat::JSON`), src/HE_Editor/SceneAutosave.cpp:96-165; src/HE_Editor/SceneFileStamp.h:24-38, :65-82 (`hashFile`)
- **Problem**: Alle `AutosaveIntervalSec` (Default 60 s, SceneAutosave.h:123) wird bei dirty Szene `serializer.save(*m_editorWorld, path, JSON)` im UI-Thread ausgeführt, zusätzlich `create_directories`, `rename`, Manifest schreiben. JSON ist das größte/langsamste der Formate und für eine Recovery-Kopie nicht nötig, wenn ein binäres Format vorhanden ist (Szenen werden laut CLAUDE.md "lossless" serialisiert, `saveToMemory` existiert: OutlinerPanel/StreamingDebugView benutzen `SceneSerializer().saveToMemory`). `SceneFileStamp::remember` liest und FNV-hasht die komplette Szene byteweise nach jedem Speichern/Öffnen auf dem UI-Thread (64 KB-Puffer auf dem Stack).
- **Code-Auszug**:
```cpp
m_autosave.update(nowMs, dirty, m_undo.revision(), info,
    [this](const std::string& path) {
        SceneSerializer serializer;
        return serializer.save(*m_editorWorld, path, SerializeFormat::JSON);
    });
```
- **Lösung**: (1) Szenen-Snapshot im Frame-Thread nur in den Speicher serialisieren (`saveToMemory`, binär) und das Schreiben (`write`+`rename`+Manifest) an einen Worker-Job übergeben. (2) Prüfen, ob das Recovery-Laden ein binäres Format akzeptiert (Recovery-Dialog/`SceneRecoveryDialog.cpp`) und dieses nehmen. (3) `hashFile` kann die bereits serialisierten Bytes des Saves wiederverwenden bzw. nur bei geändertem `size/mtime` laufen (ist in `changed()` bereits so, nur `remember` hasht immer).
- **Erwarteter Effekt**: Entfernt einen periodischen Hitch bei großen Szenen (Schätzung: 10-100+ ms bei 50k Entities; messen, `HE_PROFILE_SCOPE` um `updateAutosave`).
- **Risiko / Verifikation**: Die Welt darf während des Worker-Schreibens nicht verändert werden (daher Serialisierung im Frame-Thread, nur I/O auslagern). Crash-Konsistenz-Reihenfolge (tmp -> rename -> Manifest zuletzt) beibehalten. Tests: `grep -l SceneAutosave tests/*.cpp`.

---

### EDT-111 – `InspectorPanel::renderForImpl`: eine Funktion mit 2865 Zeilen
- **ID**: EDT-111
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/InspectorPanel.cpp:845-3710
- **Problem**: Alle Komponenten-Abschnitte (Transform, Mesh, Light, Camera Rig, LookAt, Joint, Attachments, Terrain ...) stehen in einer einzigen Funktion, durchgesetzt über Lambdas (`componentHeader`, `trackEdit`) und viele Flag-Parameter (`only`, `collect`, `removeMatching`). Das macht Code-Review und Tests schwer und erzwingt, dass im Collect-/Remove-Modus (`listComponents`, `removeComponent`, `refreshMultiSelection`, EDT-109) die gesamte Funktion für jede Entity durchlaufen wird. Die "komplette-by-construction"-Eigenschaft (Kommentar :903-905) bleibt nur erhalten, solange jede Sektion durch `componentHeader` läuft.
- **Code-Auszug**:
```cpp
bool renderForImpl(AppContext& ctx, HorizonWorld& world, Entity entity, EditorUndo* undo,
                   const char* only, std::vector<std::string>* collect,
                   bool removeMatching = false)
```
- **Lösung**: Pro Komponente eine kleine Funktion (`bool drawLight(Ctx&, LightComponent&)`) und eine statische Tabelle `{ label, has(registry,e), draw }`; `renderForImpl` iteriert die Tabelle. Collect/Remove-Modus braucht dann nur `has()` und keine UI-Pfade. Die vier Entity-Picker (EDT-102) wandern als erste in einen Helfer. Schrittweise (eine Komponente pro Commit), jeweils mit `test_inspector_*`-Lauf.
- **Erwarteter Effekt**: Lesbarkeit/Testbarkeit; Collect-Modus (`listComponents`) wird von O(gesamte Funktion) auf O(Tabelle) (Schätzung: Faktor 5-20 pro Aufruf, siehe EDT-109).
- **Risiko / Verifikation**: Reihenfolge der Abschnitte und die Help-Scopes (`HE::Ed::Help::setScope(label)` in `componentHeader`) müssen identisch bleiben; `editor_help_audit.py --check` und UI-Screenshots (`HE_UI_DUMP_DIR`) vor/nach vergleichen.

---

### EDT-112 – Drei unabhängige statische Mesh-Bounds-Caches; ignorieren vorberechnete `boundsMin/Max`; nie invalidiert
- **ID**: EDT-112
- **Kategorie**: A / C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Editor/ViewportOverlays.cpp:155, :195-219 (`s_colliderMeshBox`); src/HE_Editor/ViewportPanel.cpp:247-272 (`s_aabbCache`, `meshBounds`), :1806 (einziger Invalidierungspunkt); src/HE_Editor/LevelScriptPanel.cpp:2296-2326 (`previewMeshBounds`)
- **Problem**: Dieselbe Information (lokale AABB eines Mesh-Assets) wird in drei Dateien je mit eigener `static unordered_map` und eigener Vertex-Schleife berechnet. Die Kommentare behaupten, gekochte Meshes trügen ihre Bounds "precomputed" (ViewportPanel.cpp:~246), tatsächlich iteriert der Code für `cooked` über alle `vertexCount` Vertices, obwohl `StaticMeshAsset` `boundsMin[3]/boundsMax[3]` besitzt (HE_Core/include/ContentManager/Assets.h:~146-152). Nur `s_aabbCache` wird (bei Sculpt) invalidiert; `s_colliderMeshBox` und LevelScriptPanels Cache veralten nach Re-Import (gleiche UUID, neue Geometrie) und bleiben über Projektwechsel erhalten.
- **Code-Auszug**:
```cpp
if (mesh->cooked)
    for (uint32_t i = 0; i < mesh->vertexCount; ++i)
        box.expand({ mesh->interleaved[i * 8 + 0], mesh->interleaved[i * 8 + 1], mesh->interleaved[i * 8 + 2] });
```
- **Lösung**: Eine gemeinsame Funktion `const HE::AABB* ViewportPick::meshLocalBounds(ContentManager&, UUID)` (ViewportPick ist bereits Heimat von `fallbackBox`) mit einem Cache, der gekochte Assets aus `boundsMin/Max` liest und nur lose Assets scannt; Invalidierung am Content-Reload-/Re-Import-Hook (dort, wo `EditorAssetTypeCache` bereits zurückgesetzt wird) und beim Projektwechsel. Die drei Stellen darauf umstellen.
- **Erwarteter Effekt**: Weniger Code (ca. 60 Zeilen), keine Vertex-Schleife über 100k Vertices bei gekochten Meshes beim ersten Zugriff, korrekte Bounds nach Re-Import.
- **Risiko / Verifikation**: Prüfen, dass `boundsMin/Max` bei gekochten Assets immer gefüllt sind (Packer: `hpak_packer`/`asset_compiler`); Fallback auf Scan bei `boundsMax == boundsMin == 0`. Test: Re-Import einer Mesh-Datei und Bounds-Abfrage (vorhanden in `tests/` nur indirekt; neu: kleiner Test auf der gemeinsamen Funktion).

---

### EDT-113 – MCP-Tool-Dateien: kopierte Helfer (`lowered`, `objectSchema`, `strArg`, `vec3Prop`, `strField`) trotz vorhandenem `McpToolCommon`
- **ID**: EDT-113
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `lowered`: McpToolsApi.cpp:44, McpToolsBuild.cpp:29, McpToolsHc.cpp:203, McpToolsSettings.cpp:43; `objectSchema`: McpToolsEntity.cpp:234, McpToolsHc.cpp:379 (Original McpToolCommon.cpp:51); `strArg`: McpToolsEntity.cpp:95, McpToolsHc.cpp:363 (Original McpToolCommon.cpp:17); `vec3Prop`: McpToolsEntity.cpp:223, McpToolsPrefab.cpp:139, McpToolsScreenshot.cpp:83; `strField`: McpBridge.cpp:52; zusätzlich `lower/lowerCopy/folded` in SourceControlPanel.cpp:217, TaskBoardPanel.cpp:436, HcGraphHost.cpp:403, HcWatchPanel.cpp:36, TeamSyncController.cpp:14
- **Problem**: `McpToolCommon.h` stellt `strArg`, `objectSchema`, `stringProp`, … bereit. McpToolsEntity.cpp, McpToolsHc.cpp und McpToolsApi.cpp inkludieren es nicht und tragen byte-identische Kopien (z. B. `objectSchema` in drei Dateien, `strArg` in drei). Ein Fix am Original (z. B. `s["required"] = required;` kopiert statt zu verschieben) erreicht die Kopien nicht. Mindestens neun verschiedene `tolower`-String-Helfer im Editor.
- **Code-Auszug**:
```cpp
json objectSchema(json properties, std::vector<std::string> required)   // 3x identisch
{
    json s{ { "type", "object" }, { "properties", std::move(properties) }, { "additionalProperties", false } };
    if (!required.empty()) s["required"] = required;
    return s;
}
```
- **Lösung**: `McpToolCommon.h` um `vec3Prop`, `intProp`, `lowered` erweitern; in den drei Dateien `#include "McpToolCommon.h"` und die lokalen Kopien löschen. Für UI-Dateien ein gemeinsames `EditorStrings.h` (`toLowerAscii(std::string)`) bzw. das vorhandene Core-Hilfsmittel nutzen (vorher per `grep -rn "toLower\|lowerAscii" src/HE_Core` prüfen). `McpBridge.cpp::strField` kann `strArg` aufrufen (gleiche Semantik: leerer String bei falschem Typ).
- **Erwarteter Effekt**: ca. 80-100 Zeilen weniger, ein Ort für Fehlerbehebung (Schätzung).
- **Risiko / Verifikation**: Gering; ADL/Namenskollision im anonymen Namespace beachten. Build + MCP-Tests (`tests/` mit `McpToolRegistry`).

---

### EDT-114 – Material-Editor: Gruppierungslogik doppelt und mit String-Kopien pro Aufruf
- **ID**: EDT-114
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Editor/MaterialEditorPanel.cpp:907-925 (`drawParamConstPanel`), :2043-2062 (`drawInstanceOverridePanel`)
- **Problem**: Beide Panels bauen eine `groupOrder` ("first-seen, leere Gruppe zuerst") mit identischem Code und laufen danach O(Gruppen·Parameter) über die Parameter. In `drawInstanceOverridePanel` liefert `groupOf` pro Aufruf eine `std::string`-Kopie (`-> std::string`), im Master-Panel kopiert `auto groupOf = [](const MatGraphNode* n){ return n->group; }` ebenfalls (Lambda ohne `const&`-Rückgabe). Pro Frame mit N Parametern und G Gruppen: etwa N·(G+1) String-Kopien. Läuft jeden Frame, solange das Panel sichtbar ist.
- **Code-Auszug**:
```cpp
auto groupOf = [&](size_t i) -> std::string
{ return i < inst.graphParamGroups.size() ? inst.graphParamGroups[i] : std::string(); };
```
- **Lösung**: `groupOf` mit `-> const std::string&` (statische leere Zeichenkette für den Fehlerfall) liefern; die First-Seen-Ordnung in eine gemeinsame Funktion `std::vector<std::string_view> groupOrder(range)` auslagern und beide Panels nutzen lassen.
- **Erwarteter Effekt**: Gering (Schätzung): wenige Dutzend Allokationen/Frame bei großen Materialien; Hauptgewinn Duplikatabbau.
- **Risiko / Verifikation**: Gering; Lebensdauer der `string_view`s an `groupParamNames` binden. Sichtprüfung per UI-Dump.

---

### EDT-115 – Task-Board: `matches()` kopiert und kleinschreibt bei aktivem Filter alle Felder, achtmal pro Frame und Task
- **ID**: EDT-115
- **Kategorie**: D / C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/TaskBoardPanel.cpp:442-450 (`matches`), :1050-1074 (`drawList`: je Spalte einmal zum Zählen, einmal zum Zeichnen), :1077+ (`drawColumns`)
- **Problem**: Bei nichtleerem Filter ruft `matches` pro Task `lower(s)` (Kopie + Transform) für Titel, Beschreibung, Link-Labels und Assignees auf; `drawList` tut das in 4 Spalten × 2 Durchläufen = 8× pro Task und Frame. Mit 300 Tasks mit langen Beschreibungen sind das tausende Allokationen pro Frame, solange der Filter aktiv ist.
- **Code-Auszug**:
```cpp
auto has = [&](const std::string& s) { return lower(s).find(lowerFilter) != std::string::npos; };
if (has(t.title) || has(t.description)) return true;
```
- **Lösung**: Treffer pro Task einmal je Frame (oder je Filter/`s_store.generation`) in einem `std::vector<char> hit` ablegen und `drawList` daraus füttern; oder case-insensitive Suche ohne Kopie (`std::search` mit `tolower`-Prädikat wie `OutlinerFilter::nameMatches`, OutlinerFilter.cpp:170).
- **Erwarteter Effekt**: Von 8·N Heap-Kopien pro Frame auf 0 (Schätzung); nur relevant bei vielen Tasks.
- **Risiko / Verifikation**: Gering; identisches Trefferverhalten (ASCII-`tolower`) per Unit-Test auf `matches`.

---

### EDT-116 – McpBridge/Registry: tiefe JSON-Kopien pro Request, lineares `find`, `tools/list` wird jedes Mal neu gebaut
- **ID**: EDT-116
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/McpBridge.cpp:377-383 (`params`, `rpcId` Kopien), :422-423 (`args` Kopie), :454-460/:476 (`payload` kopiert für `dump(2)` und `structuredContent`); src/HE_Editor/McpToolRegistry.cpp:96-101 (`find`), :113-123 (`listPayload`)
- **Problem**: `msg["params"]`, `params["arguments"]` und `msg["id"]` werden tief kopiert (Argumente können große Arrays enthalten, z. B. Terrain-Höhen/Batch-Operationen); `payload` wird einmal als String mit Einrückung (`dump(2)`) und einmal als `structuredContent` ausgeliefert (die Antwort enthält die Daten doppelt, das ist laut Kommentar :473-475 beabsichtigt, kostet aber Serialisierung und Bytes). `McpToolRegistry::find` ist ein linearer Stringvergleich über alle (mehrere hundert) Tools; `listPayload` baut bei jedem `tools/list` das ganze JSON inkl. `inputSchema`-Kopien neu.
- **Code-Auszug**:
```cpp
const json params = (msg.contains("params") && msg["params"].is_object()) ? msg["params"] : json::object();
const json args = (params.contains("arguments") && ...) ? params["arguments"] : json::object();
{ "text", payload.dump(2) },
```
- **Lösung**: Per `std::move(msg["params"])` bzw. Referenz auf `msg` arbeiten (`const json& params`), `dump()` ohne Einrückung (Text-Block wird vom Modell gelesen — Einrückung kostet Tokens und Zeit; falls gewollt, bewusst belassen), `find` über `std::unordered_map<std::string,size_t>` neben dem Vektor, `listPayload` bis zur nächsten `add()` cachen.
- **Erwarteter Effekt**: Klein pro Call; relevant bei Batch-/Terrain-Tools mit MB-großen Argumenten (Schätzung).
- **Risiko / Verifikation**: Ausgabeformat ändert sich bei `dump()` ohne Einrückung nur kosmetisch; vorhandene MCP-Bridge-Tests prüfen.

---

### EDT-117 – Dateisystemzugriffe im Draw: ProjectHub (`fs::exists` pro Eintrag/Frame) und App-Icon-Vorschau (`last_write_time` + Key-String pro Frame)
- **ID**: EDT-117
- **Kategorie**: E / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/ProjectHubPanel.cpp:456-466 (`std::filesystem::exists(p)` je bekanntem Projekt je Frame, ungeclippt); src/HE_Editor/AppMetadataRows.cpp:121-150 (`is_regular_file` + `iconPreviewTexture`: `last_write_time` und Bau eines 5-Teile-Key-Strings pro Frame)
- **Problem**: Blockierende Syscalls im Draw-Pfad. Auf Netzlaufwerken/ruhenden Platten kann ein einzelnes `exists` den UI-Thread spürbar blockieren (Hub-Liste mit vielen "bekannten" Projekten, teils auf getrennten Laufwerken). Ebenso in den App-Metadaten-Einstellungen pro Frame.
- **Code-Auszug**:
```cpp
std::string label = name + "\n" + known[i];
bool exists = std::filesystem::exists(p);        // pro Eintrag, pro Frame
```
- **Lösung**: Existenz beim Öffnen des Hubs und alle ~2 s (wie `s_store.nextPoll` im TaskBoard, TaskBoardPanel.cpp:211-214) cachen, am besten asynchron im Worker; für den Icon-Key `file.string()`+mtime nur alle 0,5 s neu prüfen.
- **Erwarteter Effekt**: Vermeidet gelegentliche Frame-Hänger bei langsamem Dateisystem (nicht messbar auf lokaler SSD).
- **Risiko / Verifikation**: Gering; rote "fehlt"-Markierung darf bis zu 2 s verzögert erscheinen.

---

### EDT-118 – Viewport-Overlays: alle Splines werden pro Frame neu gesampelt; Collider-Wireframes ohne Culling
- **ID**: EDT-118
- **Kategorie**: D / C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/ViewportOverlays.cpp:254-277 (`appendSplineGuides`: `Curve curve(spline)`, `curve.sample(...)` -> neuer `vector` pro Spline pro Frame), :147-170 (`appendColliderWireframes`: `worldMatrixOf` je Collider je Frame)
- **Problem**: Bei eingeschalteten Guides (`show.guides`) wird jede Spline der Szene jeden Frame neu aufgebaut und gesampelt, auch nicht ausgewählte und nicht sichtbare. Collider-Overlay: `worldMatrixOf` (Eltern-Kette) pro Collider und kein Frustum-Culling der erzeugten Linien. Beides ist per Schalter abschaltbar (EditorApplication.cpp:4355, :4500-4507), also Editor-Komfort, nicht Pflicht-Hotpath.
- **Code-Auszug**:
```cpp
const HE::spline::Curve curve(spline);
if (curve.spanCount() > 0) {
    const std::vector<glm::vec3> pts = curve.sample(SplineEdit::kSamplesPerSpan);   // Heap pro Spline/Frame
```
- **Lösung**: Samples pro Spline cachen (Schlüssel: Revision der Spline-Komponente/`controlPoints`-Hash und Welt-Matrix), unselektierte Splines außerhalb des Sichtbereichs überspringen; für Collider die Linien nur für Entities im Sichtkegel erzeugen. Erst nach Messung (Szene mit 500 Splines/Collidern) umsetzen.
- **Erwarteter Effekt**: Unsicher; nur bei sehr großen Level-Szenen relevant. Messen.
- **Risiko / Verifikation**: Cache muss bei Spline-Änderung (Undo, Skript, Peer) invalidieren; Fehlinvalidierung zeigt veraltete Linien.

---

### EDT-119 – `McpToolRegistry.h` (1592 Zeilen, zieht `nlohmann/json.hpp` und `std::function`-Hook-Structs) wird in 28 Übersetzungseinheiten eingebunden
- **ID**: EDT-119
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/McpToolRegistry.h:24-36 (Includes), :201-1592 (17 `Mcp*Hooks`-Structs); eingebunden von allen McpTools*.cpp, McpBridge.*, McpToolCommon.h, McpToolsApi.h
- **Problem**: Jede Änderung an einem der 17 Hook-Structs (z. B. `McpTerrainHooks`) kompiliert alle Mcp-Tool-Dateien neu; jede Einheit parst `nlohmann/json.hpp` (nötig) und `Renderer/IRenderer.h` (nur für die Screenshot-Hooks nötig, :26). Außerdem koppelt es alle Tool-Gruppen an einen einzigen Header.
- **Code-Auszug**:
```cpp
#include <Renderer/IRenderer.h>   // EditorCameraOverride — the screenshot hooks speak it
#include <nlohmann/json.hpp>
```
- **Lösung**: Hook-Structs je Tool-Gruppe in eigene Header (`McpTerrainHooks.h`, …) verschieben; `McpToolRegistry.h` behält nur `ToolResult`, `McpTool`, `McpToolRegistry`. `IRenderer.h` nur im Screenshot-Header einbinden. Ein reiner Mechanik-Refactor.
- **Erwarteter Effekt**: Kürzere Inkrementalbuilds (Schätzung; per `-ftime-trace` vorher/nachher messen).
- **Risiko / Verifikation**: Gering; Build aller Targets (`HorizonEditor`, `he_tests`).

---

## Cross-Modul-Hinweise

- `src/HE_Core/include/GraphCommon/GraphModel.h:40-62`: `findNodeById` ist linear mit der Begründung "tens of nodes"; UI-Widget-Bäume (`UIWidgetTree::find`, UIWidgetTree.cpp:16-19) sind größer. Relevant für EDT-105 (Core-Entscheidung über Index).
- `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1033-1044`: `uiElementRectUnbounded` nutzt eine `dynamic_cast`-Kette (6 Casts) pro Layout-Kind-Abfrage; ein virtuelles `slotRect()` oder ein Typ-Enum (vorhanden: `UIWidgetType`) würde das ersetzen (Core/UI-Audit).
- `src/HE_Scene/src/EntityVisibility.cpp:51-66`: `subtreeVisibility` hat keinen Cache/Dirty-Flag; wird vom Outliner jeden Frame je Zeile aufgerufen (EDT-106).
- `src/HE_Scene/src/HorizonWorld.cpp:434`: `findByEntityId` (Hash-Index) wird vom Editor-Code `entityByUuid` nicht genutzt (EDT-104).
- `src/HE_Editor/EditorApplication.cpp` (anderer Slice): `updateAutosave` (~13680) und `updateAssetAutosave` laufen im Frame-Thread (EDT-108/110).

## Unsicher / Messung nötig

- EDT-101: tatsächliche Frame-Kosten der Baumansicht bei 1k/5k Änderungen (Profiler-Scope um `DrawSourceControlWindow`).
- EDT-102: Anteil an der Frame-Zeit bei 10k+ Entities mit ausgewähltem Camera-Rig (Outliner-Baseline-Doc `docs/world-streaming-baseline-2026-10-06.md` als Vergleichswert).
- EDT-105: reale Elementzahlen großer Widgets; erst ab ca. 150+ Elementen relevant.
- EDT-106/109: Wirkung von Eye-Cache bzw. Zeit-Refresh nur mit Messung beurteilbar (Latenz vs. Kosten).
- EDT-110: Dauer von `SceneSerializer::save(..., JSON)` bei Produktions-Szenen.
- EDT-118: Anzahl Splines/Collider, ab der Overlay-Aufbau relevant wird.
- Nicht untersucht, aber vom Muster verdächtig (ohne Beleg nicht aufgeführt): `McpToolsMaterial.cpp` (3737 Zeilen), `LevelScriptPanel.cpp`, `HcGraphHost.cpp`-Spawn-Pin-Refresh (HcGraphHost.cpp:~480-520 baut pro Frame `ids`-Vektor, `std::string path` und `cache[path]`-Lookup je CreateWidget-Knoten; nur Niedrig).

## Gelesen

Nein, der Slice (ca. 106k Zeilen, ~230 Dateien) wurde NICHT vollständig gelesen: vollständig gelesen wurden OutlinerPanel.cpp, McpBridge.cpp, NotificationStore.cpp, McpToolRegistry.cpp, SceneDiskWatch.cpp, SceneFileStamp.h sowie große Ausschnitte von ViewportOverlays.cpp, AssetThumbnailCache.cpp (532-870), NotificationBar.cpp (1-140), InspectorPanel.cpp (100-260, 845-1000 und gezielte Stellen), UIEditorPanel.cpp (5026-5225 u. a.), TaskBoardPanel.cpp, SourceControlPanel.cpp (286-350, 655-960), ProfilerPanel.cpp (60-160), TerrainTools.cpp (339-470), BuildProgressDialog.cpp (225-300); alle übrigen Dateien (u. a. HcNodeDocs, McpTools*-Gruppen, MaterialEditorPanel, LevelScriptPanel, TeamView/TeamController, Sequencer/Cinematic, Audio*, Tutorial*, ToolSetupDialog, MacMenuBar.mm) wurden nur per Grep auf Hot-Path-Muster (Dateisystem im Draw, Sortierung, Regex, Prozess, Kopien, Caches) geprüft.
