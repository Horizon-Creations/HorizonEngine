# Editor Slice 18 – UIEditorPanel.cpp/.h, UITimelineMath.h, UndoHistoryPanel.*, ViewportActions.*, ViewportOverlays.*, ViewportPanel.*

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

## Cross-Modul-Hinweise

- `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:16-26` und `src/HE_Core/include/.../GraphModel.h:45-70` (`findNodeById`): lineare Suche; `UIWidgetTree::find` ist die Wurzel von EDT-1802/1803 und der Layout-Funktionen (`uiElementRect` ruft rekursiv `find` auf Eltern, `UIWidgetTree.cpp:239-258`). Ein Id→Index-Lookup (Ids sind dichte Ints) in `UIWidgetTree` würde auch Runtime-Layout (`WidgetManager`) beschleunigen. `childrenOf` und `removeSubtree` (`UIWidgetTree.cpp:21-52`) haben O(n²) bei großen Bäumen (`removeSubtree` mit `std::find` in `doomed` pro Element).
- `src/HE_Editor/EditorSelection.cpp:75-102`: `contains()` linear, `roots()` quadratisch (EDT-1805). Weitere Aufrufer (Outliner, Gizmo, MCP) profitieren von einem Set/Revisionszähler.
- `src/HE_Editor/HcEditorUtil.cpp:93-100`: `listAssets` als Dateisystem-Scan wird in mehreren Panels im Draw benutzt (hier Palette; `grep listAssets` für weitere Aufrufer außerhalb des Slice); ein zentraler, invalidierbarer Asset-Listen-Cache wäre die saubere Lösung.
- `src/HE_Editor/EditorUI.cpp:1991` (macOS): `entityActionState(ctx).primaryLocked` pro Frame (EDT-1811).
- `src/HE_Core/include/ContentManager/Assets.h:144-152`: `boundsMin/boundsMax` existieren, werden vom Editor nicht genutzt (EDT-1812). `UIElement::properties()` liefert by-value-Vektoren (EDT-1809); vermutlich auch in anderen Panels genutzt.
- `src/HE_Editor/ViewportOverlays.cpp:155`: zweiter, nie invalidierter Mesh-Box-Cache (EDT-1812) – auch relevant für `SecondaryViewportPanel` (nutzt `ViewportPanel::selectionBox`, dessen `sceneSnapshot` bei jedem Aufruf aus dem Menü extrahiert, falls der Scene-Frame noch nicht lief: `ViewportPanel.cpp:709-721`).

## Unsicher / Messung nötig

- Kosten von `HcEditorUtil::listAssets` bei realen Projekten (EDT-1801): Anzahl .hasset, OS-Cache.
- Anteil von `drawEmbeddedTree`/`laidEmbeddedCopy` an der Canvas-Zeit bei Seiten mit 20–30 Komponenten (EDT-1804).
- Kosten von `loadAsset(path)` für bereits geladene Assets (EDT-1808), `uiThemeValueFor` pro Property (EDT-1807).
- Dauer der PreConstruct-Sandbox pro Commit (`runDesignTimePreConstruct`, EDT-1821).
- Dateischreibzeit von `saveAsset` im Live-Preview (EDT-1810).
- Tiefe des Undo-Stacks von `EditorUndo` (EDT-1819) und Anzahl Splines/Collider in typischen Szenen (EDT-1815).
- Ob `UIWidgetTree::elements` je `nullptr` enthält (EDT-1816).
- `anyHidden`-Walk-Kosten auf macOS bei großen Szenen (EDT-1811).

## Gelesen

Ja, alle Dateien des Slice vollständig gelesen: `UIEditorPanel.cpp` (1–7731 in Blöcken), `UIEditorPanel.h`, `UITimelineMath.h`, `UndoHistoryPanel.cpp/.h`, `ViewportActions.cpp/.h`, `ViewportOverlays.cpp/.h`, `ViewportPanel.cpp` (1–1862) und `ViewportPanel.h`; zur Verifikation zusätzlich gezielt `UIWidgetTree.cpp`, `GraphModel.h`, `HcEditorUtil.cpp`, `EditorSelection.cpp`, `EntityVisibility.cpp`, `TransformHierarchy.cpp`, `Assets.h`, `EditorUI.cpp` (nur die zitierten Zeilen).
