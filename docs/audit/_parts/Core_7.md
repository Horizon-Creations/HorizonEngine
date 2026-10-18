# Core Slice 7 – UIWidget (UIWidgetBinding.h, UIWidgetTree.{h,cpp}, UIWindowFrame.{h,cpp}, WidgetManager.cpp)

## Zusammenfassung

Der Slice ist funktional sauber und sehr gut kommentiert; `UIWindowFrame.*` und `UIWidgetBinding.h` sind unauffällig (reine Geometrie bzw. drei Deklarationen, keine Funde). Das Hauptproblem ist architektonisch: Layout und alle vererbten Zustände (Rect, Sichtbarkeit, Enabled, Opacity, Rotation, Clip, Tiefe/Sortier-Key) sind bewusst *zustandslos* und werden pro Element über Parent-Ketten mit linearem `tree.find()` neu berechnet. Jede Frame-Schleife (`extract`, `topmostHit`, `uiApplyAutoSize`, `processPointer`) ruft das für jedes Element auf, was grob O(n²·Tiefe) pro Frame ergibt. Dazu kommen List-Zeilen-Grafting (`graftChildRef`), das pro Zeile den ganzen Baum neu themed/übersetzt/Assets auflöst, und mehrere pro-Frame-Vollscans mit `dynamic_cast`. Funde: 4 Hoch, 6 Mittel, 9 Niedrig. Alle Aussagen sind statisch gelesen, ohne Messdaten; wo Größenordnungen geschätzt sind, ist das markiert.

## Funde

### CORE-701 – Layout ist zustandslos: jede Rect-Abfrage läuft Parent-Kette + lineare Suche + Geschwister-Scans
- **ID**: CORE-701
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:16-19` (find), `:230-259` (parentRectOf), `:464-527` (boxSlotRect), `:793-818` (gridSlotRect/solveGrid `:566-711`), `:831-896` (wrapSlotRect), `:1012-1063` (uiElementRect/Unbounded); Verbraucher u. a. `WidgetManager.cpp:6165-6171`, `:2969-3006`, `UIWidgetTree.cpp:1277-1285`
- **Problem**: `uiElementRect(e)` ruft für Kind-eines-Containers `boxSlotRect`, das `uiElementRect(box)` (rekursiv bis zur Wurzel) aufruft und zweimal über *alle* `tree.elements` läuft; `tree.find` ist ein linearer Scan (`GraphModel.h:findNodeById`). Pro Rect-Abfrage also O(Tiefe·n); `GridSolved` wird sogar pro Kind komplett neu gelöst (`gridSlotRect` → `solveGrid`, enthält `cellOf`-O(rows·cols) je Auto-Track-Kind). Jeder Frame fragt das Rect für jedes Element ab (extract, topmostHit, AutoSize, Scroll-Thumb, Wheel) → O(n²·Tiefe), bei Grids/Wraps/Listen zusätzlich O(n²) pro Container. Kommentar im Code ("no cached slot table to invalidate") nennt die Entscheidung, aber ohne Messung.
- **Code-Auszug**:
```cpp
// UIWidgetTree.cpp:491-499 (boxSlotRect), 511-525 – zwei volle Durchläufe pro Kind
for (const auto& sp : tree.elements)
{
    if (!sp || sp->parentId != box.id || !sp->visible) continue;
    ++count; ...
}
// :1028 und :237
if (const UIElement* p = tree.find(e.parentId); p && p->laysOutChildren())
```
- **Lösung**: Pro Frame (oder pro „Tree-Version“) ein einziger Top-down-Layoutpass, der für alle Elemente `UIWidgetRect` (inkl. Bounds) in ein flaches `std::vector<UIWidgetRect>` (Index = Elementindex) schreibt: (1) `id→index`-Tabelle und `children`-Listen (CSR-Array parent→Kinder, in Vektorreihenfolge) beim Strukturwechsel bauen; (2) Wurzeln auflösen, dann je Container die Slots seiner Kinder in *einem* Durchlauf verteilen (Box: Summen einmal, Grid: `solveGrid` einmal pro Grid, Wrap: Linien einmal). `uiElementRect(tree,e,canvas)` bleibt als dünner Wrapper (Cache-Treffer oder Fallback auf alten Pfad), damit Editor/Designer-Aufrufer unverändert funktionieren. Cache-Invalidierung über einen `tree.revision`-Zähler, der von allen Mutatoren (setPropAny, add/remove/move, AutoSize/Scroll-Änderungen) erhöht wird – oder simpler: Cache nur innerhalb eines Aufrufs von `extract`/`topmostHit` halten (Scope-Objekt `UILayoutScope`), dann entfällt jede Invalidierungslogik.
- **Erwarteter Effekt**: Schätzung: von O(n²·d) auf O(n) pro Frame; bei Seiten mit einigen hundert Elementen vermutlich ein Faktor 10-100 weniger CPU-Zeit im UI-Pfad (messen).
- **Risiko / Verifikation**: Hohes Verhaltensrisiko, weil Layout exakt gleich bleiben muss (Einheiten-Skalierung in WidgetRefs, Min/Max-Bounds, DesignSlots, Scroll-Offsets). Absicherung: Differential-Test, der für alle Beispiel-/Asset-Bäume (`tests/test_ui_widgets.cpp`, nur mit Offset/Limit lesen) Rect aus Alt- und Neupfad für jedes Element vergleicht (Toleranz 0), inkl. Grid/Wrap/List/Tab/Splitter/Accordion/WidgetRef-Verschachtelung. Zunächst nur den Cache einführen, Algorithmus unverändert lassen.

### CORE-702 – `topmostHit` macht pro Frame für jedes Element die teuerste Arbeit vor dem billigen Punkt-Test
- **ID**: CORE-702
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/WidgetManager.cpp:2951-3056` (Hauptschleife `:2969-3006`); zweiter Vollscan in `windowHitAt` `:3090-3123`; Aufrufer `HE_Game/src/GameApplication.cpp:2347`, `HE_Editor/EditorApplication.cpp:4188`
- **Problem**: `processPointer` wird laut Kommentar jeden Frame aufgerufen („An application calls this every frame whether the mouse stirred or not“), auch wenn sich Zeiger und Baum nicht geändert haben. Darin läuft `topmostHit` über alle Elemente aller Widgets und ruft *vor* dem Punkt-in-Rect-Test nacheinander `uiElementEffectiveVisible` (rekursiv), `uiElementEffectiveEnabled`, `uiElementEffectiveOpacity`, `uiElementRect`, `uiElementRotation` (bis zu 3 Kettenwanderungen) und `uiElementClipRect` auf – jeweils mit linearem `find`. Die Mehrheit der Elemente liegt gar nicht unter dem Zeiger. `windowHitAt` (aus dem OS-Hit-Test-Callback, potentiell pro Mausbewegung) wiederholt dieselbe Struktur und ruft zusätzlich vorher `topmostHit`.
- **Code-Auszug**:
```cpp
if (!HE::uiElementEffectiveVisible(w.tree, e)) continue;
if (!e.hitTestable) continue;
if (!HE::uiElementEffectiveEnabled(w.tree, e)) continue;
if (HE::uiElementEffectiveOpacity(w.tree, e) <= 0.001f) continue;
HE::UIWidgetRect r = HE::uiElementRect(w.tree, e, &canvas);
...
if (HE::uiElementClipRect(w.tree, e, clip, &canvas)) { ... }
```
- **Lösung**: (1) Early-out im Aufrufer: wenn `valid`, Zeigerposition, Viewport, Baum-Revision (siehe CORE-701) und Grab-Stack gegenüber dem letzten Frame unverändert sind, das letzte `PointerHit` wiederverwenden. (2) Reihenfolge ändern: `hitTestable` zuerst (billig), dann Rect (aus Cache), dann den groben Punkt-Test gegen das Rect *erweitert um die Rotation*: nur wenn `rot`-Kette leer ist (Flag im Layout-Cache), sofort ablehnen; erst danach Visible/Enabled/Opacity/Clip. Da alle Filter reine Konjunktionen sind, bleibt das Ergebnis identisch. (3) Den Clip-Schnitt (identischer Block in `:2994-3002` und `:3105-3117`) in eine Hilfsfunktion auslagern.
- **Erwarteter Effekt**: Schätzung: pro Frame von O(n²·d) auf ~O(n) bzw. bei ruhendem Zeiger O(1); in UI-lastigen Szenen (Menüs, Listen) spürbar, vor allem im Editor-Viewport und im Spiel mit eingeblendetem HUD (messen).
- **Risiko / Verifikation**: Mittel. Zeiger-Cache darf Hover/Cursor nicht „einfrieren“, wenn sich Elemente ohne Revisionszähler ändern (Skript-Writes, Animationen, Scroll) → Zähler muss alle Pfade abdecken (siehe CORE-705). Tests: bestehende Pointer-Tests in `tests/test_ui_widgets.cpp`; zusätzlich Test „gleiche Eingabe, geänderter Baum (Visible/Enabled/Opacity per Skript) ⇒ neuer Hit“.

### CORE-703 – `extract` ermittelt vererbte Zustände pro Element mit separaten Parent-Wanderungen
- **ID**: CORE-703
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Beides
- **Ort**: `WidgetManager.cpp:19-29` (elementSortKey), `:6163-6173`, `:6229`, `:6243`, `:6358-6360`, `:6390`; `UIWidgetTree.cpp:1372-1421` (ClipRect), `:1521-1533`, `:1624-1639`, `:1641-1706`, `:1717-1730`
- **Problem**: Für jedes sichtbare Element läuft `extract` mindestens: `uiElementEffectiveVisible` (rekursiv), `elementSortKey` (Tiefen-Walk), `uiElementRect`, `uiElementClipRect` (Walk + Rect pro Clip-Ahn), `uiElementUnitScale` (Walk, ruft ggf. `uiElementRect(ref)` + `uiResolveCanvasFor`), `uiElementEffectiveOpacity`, `uiElementEffectiveEnabled`, `uiElementRotation` (2-3 Walks + Rect pro rotierendem Ahn). Das sind ~8 Parent-Ketten mit je linearem `find` pro Schritt, also O(n·d·n) pro Widget und Frame. Zusätzlich wird `items` (mit je `UIWidgetRect`) pro Widget pro Frame neu allokiert und stable-sortiert, `out` ohne `reserve`.
- **Code-Auszug**:
```cpp
for (const auto& ep : w.tree.elements)
{
    if (!HE::uiElementEffectiveVisible(w.tree, e)) continue;
    items.push_back({ &e, elementSortKey(w.tree, e), HE::uiElementRect(w.tree, e, &canvas) });
}
...
const float alpha = HE::uiElementEffectiveOpacity(w.tree, e) * ...;
const bool  usable = HE::uiElementEffectiveEnabled(w.tree, e);
```
- **Lösung**: Ein einziger Top-down-Pass (derselbe wie in CORE-701, Struktur `UIResolvedState {rect, unitScaleX/Y, depth, visible, enabled, opacity, clip(optional), rotation(optional)}` pro Element), jeweils aus dem Zustand des Parents abgeleitet (O(1) pro Element). `extract` und `topmostHit` lesen nur noch aus dieser Tabelle; die öffentlichen `uiElementEffective*`-Funktionen bleiben für Editor/Tests als Wrapper bestehen. `items`/`out` mit `reserve` bzw. als Member-Scratch wiederverwenden (CORE-C: Heap pro Frame).
- **Erwarteter Effekt**: Schätzung: wie CORE-701; zusätzlich entfallen pro Frame und Widget 1 Vektor-Allokation sowie das Sortieren neu berechneter Keys.
- **Risiko / Verifikation**: Gleiche Semantik der Kette (Cycle-Guards, fehlende Parents ⇒ `break`, Tab-Box-`hidesChild`). Differentialtest wie in CORE-701 für jede der Funktionen gegen den alten Pfad; ein Test mit zyklischem/ungültigem `parentId`.

### CORE-704 – `graftChildRef` wendet Theme, Text-Katalog und Asset-Auflösung auf den GANZEN Baum an (pro List-Zeile)
- **ID**: CORE-704
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:750-838` (Kern `:818-824`), `:223-401` (embedWidgetRefs, `:244-247`, `:278-283`, `:356-362`, `:395-398`), `:1913-1927` (Zeilen-Schleife in `syncLists`)
- **Problem**: Jede neu realisierte Zeile (Scrollen erzeugt laufend neue, bis zu `kMaxRealizedRows = 256`) führt aus: `embedWidgetRefs` (scannt alle Elemente nach WidgetRefs, parst die Zeilen-JSON komplett neu mit `uiWidgetTreeFromJson`, scannt noch zweimal alle Elemente für Slots/`given`), danach `uiApplyTheme(w.tree, …)` (alle Elemente × alle Properties), `uiApplyTextCatalog(w.tree, …)` und `refreshElementAssets` für *alle* Elemente (je drei `loadAsset(path)`-Auflösungen: Material, Textur, Font). Kosten pro Zeile also O(n_gesamt), pro Füllung einer Liste O(rows·n). Außerdem überschreibt das Re-Theming bestehende Elemente erneut (kann Skript-gesetzte Farben zurücksetzen – evtl. unbeabsichtigter Nebeneffekt).
- **Code-Auszug**:
```cpp
HE::uiApplyTheme(w.tree, themeFor(w), themeMode());
HE::uiApplyTextCatalog(w.tree, m_catalog, m_language);
for (const auto& e : w.tree.elements)
    if (e && e->id > 0) refreshElementAssets(w, *e);
```
- **Lösung**: (1) Alle drei Schritte nur auf die neu eingefügten Elemente (Id-Bereich `(offset, em.idMax]` plus das Ref-Element selbst) anwenden; Hilfsfunktionen mit Bereichsparameter bzw. Elementliste. (2) Den geparsten Teilbaum pro Asset-Pfad cachen (analog `m_columnInfo`) und klonen statt JSON neu zu parsen; Cache beim Reload/`createWidget` invalidieren. (3) `embedWidgetRefs` beim Graft nur die neuen Elemente nach Refs durchsuchen statt `w.tree.elements` komplett.
- **Erwarteter Effekt**: Schätzung: Zeilen-Graft von O(n) auf O(Größe der Zeilen-Vorlage); Scrollen durch große Listen/Tabellen ruckelfrei statt linear in der Seitengröße (messen: Frame-Zeit beim Scrollen einer 10 000-Zeilen-Liste mit 30 sichtbaren Zeilen).
- **Risiko / Verifikation**: Mittel. Verhaltensänderung nur beim Überschreiben vorhandener, skript-gesetzter Theme-/Textwerte (de facto Bugfix, aber beobachtbar). Tests: List-Test, der eine Zeile realisiert, vorher ein Skript-Property auf einem Nicht-Zeilen-Element setzt und prüft, dass es erhalten bleibt (bzw. bewusst dokumentiert); Test auf identisches Ergebnis des Zeilen-Teilbaums vs. bisher.

### CORE-705 – `setProperty`/`setPropertyOn` setzen `m_visualDirty` bedingungslos
- **ID**: CORE-705
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:121-138` (`:132`), `:151-162` (`:159`)
- **Problem**: Jeder Skript-Schreibzugriff markiert das Bild als verändert, auch wenn der Wert identisch ist (typisch: ein Tick-Graph schreibt jeden Frame denselben Text/Position). Die event-getriebene Schleife (A2) kommt dadurch nie zur Ruhe, und `extract` + `topmostHit` laufen jeden Frame (siehe CORE-701/702/703). Der Code liest den alten Wert ohnehin schon (`e->getPropAny(prop).type`) und besitzt `samePropValue` (`:583`).
- **Code-Auszug**:
```cpp
e->setPropAny(prop, HE::uiHcValueToProp(v, e->getPropAny(prop).type));
m_visualDirty = true;
```
- **Lösung**: Alten Wert einmal holen (`const auto cur = e->getPropAny(prop)`), neuen Wert konvertieren, nur bei `!samePropValue(cur, next)` schreiben und `m_visualDirty` setzen. `afterScriptWrite` und `refreshElementAssets` unverändert für den Änderungsfall; für den Gleichheitsfall nur das Dirty-Flag weglassen (Wert wurde ohnehin nicht geändert). Spart nebenbei den zweiten String-Lookup und die zwei `std::string`-Vergleiche `prop == "Material"` nur wenn geändert.
- **Erwarteter Effekt**: Schätzung: bei Skripten mit Tick-Writes gleicher Werte kein Redraw mehr → Idle-CPU/GPU-Last in Apps sinkt deutlich; sonst neutral.
- **Risiko / Verifikation**: Niedrig, solange Properties mit Seiteneffekt beim Setzen (Grid-Tracks parsen neu, `setPropAny` mit Normalisierung) beim Vergleich den *normalisierten* Zustand sehen – daher Vergleich nach dem Schreiben (alter Wert vorher kopieren, `samePropValue(alt, getPropAny(prop))`). Test: Skript schreibt zweimal denselben Wert, `consumeVisualDirty()` ist nach dem zweiten false.

### CORE-706 – `syncLists()` läuft in jedem Pointer-/Extract-/Wheel-/Drop-Aufruf und hat O(rows·n)-Teile
- **ID**: CORE-706
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:1732-1736`, `:1816-2016` (Spalten-Schleife `:1963-1991`), Aufrufer `:3300`, `:4255`, `:4278`, `:5629`, `:6023`, `:6106`
- **Problem**: Pro Aufruf und Widget: Scan aller Elemente nach `ListView` (virtueller `type()`-Aufruf je Element), pro Liste 2-3× `listRowsOf` (`dynamic_cast` über alle Elemente), und – bei sichtbarem Header – pro *Zeile* zwei komplette Baumdurchläufe, um Row-Root und Zellen zu finden. `rows` (bis 256) × `n` ergibt O(rows·n) pro Sync, und der Sync läuft jeden Frame (`processPointer`, `extract`), auch wenn sich weder Scroll noch Itemanzahl noch Viewport geändert haben.
- **Code-Auszug**:
```cpp
for (HE::UIWidgetRef* r : rows)
{
    const HE::UIElement* rowRoot = nullptr;
    for (const auto& ep : w.tree.elements)
        if (ep && ep->parentId == r->id) { rowRoot = ep.get(); break; }
    ...
    for (const auto& ep : w.tree.elements) { if (!ep || ep->parentId != rowRoot->id) continue; ... }
}
```
- **Lösung**: (1) Dirty-Gate pro Liste: Sync nur, wenn sich `(itemCount, scrollOffset, innerHeight, rowWidget, Viewport)` oder die Elementstruktur (Revision) seit dem letzten Sync geändert haben, sonst früh zurück. (2) Wo gesynct wird, einmal pro Aufruf eine `parentId → Kinder`-Map aufbauen und für Row-Root/Zellen nutzen (statt zwei Scans pro Zeile). (3) Liste der ListView-Ids pro Widget cachen (Strukturrevision).
- **Erwarteter Effekt**: Schätzung: Leerlauf-Sync von O(n) auf O(#Listen); Spaltenpflege von O(rows·n) auf O(n).
- **Risiko / Verifikation**: Gate muss alle Eingänge kennen (auch Skript-Änderungen an `rowWidget`, `showHeader`, Spaltenanteilen); ein vergessener Eingang ließe Listen veralten. Test: `setListCount`, Scrollen, Resize, Spaltenbreite ziehen, Template-Wechsel jeweils → Zeilen korrekt (vorhandene List-Tests).

### CORE-707 – `isInteractive` alloziert pro Aufruf `std::vector<EventBinding>` mit `std::string`-Namen
- **ID**: CORE-707
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:2921-2945`, genutzt in `:3012`, `:3327`, `:5451` (isFocusable); Quelle `HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:1281-1330` (`eventBindingsOf`)
- **Problem**: `rt().eventBindingsOf(id)` baut jedes Mal einen neuen Vektor mit Namens-Strings (compiled: Kopie aller `eventInfos`; interpretiert: Durchlauf aller Nodes aller Klassenlevel mit O(k²)-Duplikatprüfung per `std::any_of`). Anschließend wird linear mit sieben `std::string`-Vergleichen gesucht. Aufgerufen für jeden Treffer-Kandidaten im Pointer-Hit-Test (jeden Frame), beim Bubbling nach oben und für jedes Element in `navigate`/`focusNext` (`isFocusable`).
- **Code-Auszug**:
```cpp
for (const auto& b : rt().eventBindingsOf(target.scriptId))
    if (b.elem == 0 || b.elem == target.elem)
    {
        const std::string& n = b.name;
        if (n == "OnClicked" || n == "OnPressed" || n == "OnReleased" || ...
```
- **Lösung**: In `Runtime` pro Instanz eine vorberechnete Menge „Pointer-Bindungen“ pflegen (z. B. sortiertes `std::vector<int>` der `elem`-Ids plus Flag „any element“), beim `add`/Graph-Änderung/`addCompiled` aufgebaut; API `bool hasPointerBinding(InstanceId, int elem) const` ohne Allokation. WidgetManager ruft nur noch diese.
- **Erwarteter Effekt**: Schätzung: pro Hit-Kandidat von mehreren Heap-Allokationen + String-Kopien auf einen binären Suchlauf; entfällt komplett als Allokationsquelle im Frame.
- **Risiko / Verifikation**: Niedrig-Mittel (Cross-Modul, Runtime-Klassenhierarchie „leaf-most wins“ muss gleich bleiben). Test: bestehende Interaktionstests + neuer Test, dass ein vererbtes Event (Basis-Klasse) weiterhin `interactive` macht.

### CORE-708 – Skript-Property-Zugriffe laufen über lineare Suchen (Owner, Embed-Offset, Element, Name)
- **ID**: CORE-708
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:32-41` (find), `:49-70` (scriptTargetFor/resolveScriptOwner), `:80-100` (elementOfScript), `:109-162` (Bindings), `:2934` (isInteractive)
- **Problem**: Jedes `getProperty`/`setProperty` aus einem HorizonCode-Graph: `resolveScriptOwner` durchläuft alle Instanzen *und* deren komplette `embeds` (bei Listen bis ~256 Einträge pro Widget), danach `tree.find(elem+off)` linear über alle Elemente. `getPropertyOn/setPropertyOn` (per Name) scannen zusätzlich alle Elemente mit `std::string`-Vergleich (`elementOfScript`). `scriptTargetFor` läuft rückwärts linear durch alle Embeds und wird bei jedem Pointer-Event, jeder Aktivierung und in `isInteractive` pro Kandidat aufgerufen. Bei einer Liste mit vielen Zeilen-Graphen, die in `OnRowBind`/`Tick` Properties schreiben, wird jeder Zugriff O(Embeds + Elemente).
- **Code-Auszug**:
```cpp
for (auto& w : m_instances)
{
    if (w.scriptId == scriptId) return &w;
    for (const Instance::Embed& em : w.embeds)
        if (em.scriptId == scriptId) { idOffset = em.idOffset; return &w; }
}
```
- **Lösung**: `unordered_map<InstanceId, {widgetIndex, embedIndex}>` (oder sortierter Vektor) beim Registrieren/Entfernen pflegen; Embeds sind nach `idOffset` aufsteigend – `scriptTargetFor` kann per `std::upper_bound` suchen. Elementzugriff über die `id→index`-Tabelle aus CORE-701; für `…On` pro Instanz `name → elemId`-Cache (Invalidierung bei Struktur-/Namensänderung).
- **Erwarteter Effekt**: Schätzung: konstante statt lineare Kosten je Skript-Zugriff; wirkt vor allem bei vielen Embeds/Zeilen (messen).
- **Risiko / Verifikation**: Niedrig, aber Invalidierung nach `removeChild`/`graftChildRef`/`destroyWidget` (Vektor-Reallokation von `m_instances` – Zeiger/Indizes!). Test: Zeilen entfernen/hinzufügen und danach Property-Zugriffe der verbliebenen Zeilen.

### CORE-709 – Pro-Frame-Vollscans mit `dynamic_cast` über alle Elemente
- **ID**: CORE-709
- **Kategorie**: B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:2705-2717` (Spinner-Scan in `tick`), `:3547-3597` (drei Scans in `processPointer`), `UIWidgetTree.cpp:1248-1258`, `:1299-1303`, `:1537-1606` (`uiUpdateScrollExtents`: bis zu 4 `dynamic_cast` je Element), `:1832-1833`
- **Problem**: Jeder Frame (tick, processPointer, extract) läuft mehrfach über alle Elemente, um nur wenige Typen (ProgressBar-indeterminate, ListView, TreeView, DatePicker, TabBox, Accordion, ScrollBox, Box mit sizeToContent) zu finden – per RTTI. Bei n Elementen und ~8 Scans sind das 8·n `dynamic_cast` pro Frame und Widget.
- **Code-Auszug**:
```cpp
for (auto& ep : w.tree.elements)
{
    auto* lv = dynamic_cast<HE::UIListView*>(ep.get());
    if (!lv) continue;
```
- **Lösung**: Beim Strukturwechsel (add/remove/move/graft/clear) einmal Typlisten pro Widget aufbauen (`std::vector<UIListView*>`, `…TreeView*`, `…DatePicker*`, `…TabBox*`, `…Accordion*`, `…ScrollBox*`, `…ProgressBar*`) und per Revision invalidieren; die Scans iterieren dann nur diese Listen. Alternativ `UIElement::type()` (virtuell, bereits vorhanden) statt `dynamic_cast` zum billigen Vorfiltern verwenden.
- **Erwarteter Effekt**: Schätzung: pro Frame ~8·n RTTI-Casts weniger; absolut klein pro Scan, summiert sich aber mit CORE-701/702/703 auf (messen).
- **Risiko / Verifikation**: Niedrig; gefährlich sind nur gespeicherte Rohzeiger über Vektor-Realloc/`erase` – daher Zeiger nie über eine Frame-Grenze halten (Revision prüfen).

### CORE-710 – `uiApplyAutoSize` rechnet für jedes Element ein Rect und baut Label-Strings jeden Frame neu
- **ID**: CORE-710
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1239-1370` (`:1248-1258` Tab-Labels, `:1277-1294` Elementschleife, `:1299-1315` Tiefenberechnung+Sort), `:1563-1573` (Accordion-Labels)
- **Problem**: (a) Für *jedes* Element wird `uiElementUnitScale` und `uiElementRect(...).w` berechnet, bevor `applyAutoSize` aufgerufen wird; die Basisimplementierung ist ein No-op (`UIElement.h:948`), `autoSizedAxes()` ist für die meisten Typen 0. (b) Für jede TabBox werden `tabLabels` jeden Frame geleert und neu per `push_back(std::string)` gefüllt, ebenso Accordion `sectionLabels`/`sectionBodies` – O(n) je Tab/Accordion und Heap-Allokationen pro Frame, auch wenn sich nichts geändert hat. (c) Die Tiefe der selbstmessenden Boxen wird per Parent-Walk mit linearem `find` bestimmt und sortiert, jeden Frame.
- **Code-Auszug**:
```cpp
e->applyAutoSize(uiElementRect(tree, *e, canvas).w / std::max(1e-4f, us), fontScale);
```
- **Lösung**: (a) Nur Elemente behandeln, die tatsächlich messen: Vorfilter `e->autoSizedAxes() != 0` – unsicher, ob alle `applyAutoSize`-Überschreibungen auch bei `autoSizedAxes()==0` Arbeit leisten (z. B. UIElements.h:187/364/840/1993) → vor Umsetzung die vier Overrides prüfen oder ein `virtual bool needsAutoSize()` einführen. (b) Labels nur schreiben, wenn sich Namen/Anzahl der Kinder ändern (in-place vergleichen statt `clear()` + neu bauen; Strukturrevision). (c) Tiefe aus dem Top-down-Pass (CORE-701/703) beziehen.
- **Erwarteter Effekt**: Schätzung: spart pro Frame ein Rect (O(d·n) wegen find) je nicht-messendem Element sowie Strings/Allokationen der Tab/Accordion-Leisten.
- **Risiko / Verifikation**: Mittel wegen Override-Semantik (siehe oben). Test: Text-Auto-Size mit/ohne Wrap, SizeToContent-Boxen verschachtelt, Tab-Beschriftung nach Umbenennen einer Seite.

### CORE-711 – Menüleiste: Breiten werden bei jeder Abfrage neu gemessen (O(k²) Text-Messungen)
- **ID**: CORE-711
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:1384-1394` (menuTitleRect), `:1396-1427` (menuPopupRect), `:1429-1461` (menuTitleAt/menuItemAt), `:6590-6611` (drawMenuBar)
- **Problem**: `menuTitleRect(i)` summiert `menuTextWidth` aller vorherigen Titel; `menuTitleAt` ruft es in einer Schleife (O(k²) `measureUIText`-Aufrufe), `drawMenuBar` ruft es erneut pro Titel und dann `menuPopupRect`, das jede Zeile misst und per `uiParseShortcut`/`uiFormatShortcut` (String-Allokationen) formatiert; `menuItemAt` ruft `menuPopupRect` pro Zeigerframe, solange ein Menü offen ist. Unsicher/messen: Kosten von `measureUIText` (Font-Cache?).
- **Code-Auszug**:
```cpp
for (std::size_t k = 0; k < i; ++k)
    pen += menuTextWidth(m_menuBar[k].label, mm.fontPx) + 2.0f * mm.titlePad;
```
- **Lösung**: Layout-Struct `MenuLayout {vector<float> titleX, titleW; popup rect; vector<string> chordText}` pro (`m_menuBar`-Version, `m_fontScale`, `m_menuOpen`) cachen und in `setMenuBar`/`setFontScale`/`openMenuAt` invalidieren; alle vier Funktionen lesen daraus.
- **Erwarteter Effekt**: Schätzung: ein paar µs pro Frame, bei kleiner Menüzahl kaum messbar; vor allem Vereinfachung (eine Quelle für Bild und Hit-Test, wie im Kommentar gefordert).
- **Risiko / Verifikation**: Niedrig. Vorhandene Menü-Tests; zusätzlich Test mit Font-Scale-Wechsel bei offenem Menü.

### CORE-712 – Theme-Anwendung: Stilkette, Property-Namen und `getPropAny`-Kopien pro Property
- **ID**: CORE-712
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1089-1129` (uiThemeStylesFor/DecidedProps), `:1131-1207` (uiThemeValueFor), `:1209-1237` (uiApplyTheme)
- **Problem**: `uiApplyTheme(Element)` ruft `uiThemeDecidedProps` (baut `std::vector<const Style*>` + `typeName()`-String + `styleFor`-Lookups + `vector<string>` mit O(p²)-Duplikatprüfung), und danach pro Property `uiThemeValueFor`, das `getPropAny(prop)` (liefert `UIPropValue` mit String/Liste *by value*) und *erneut* `uiThemeStylesFor` aufruft. Das läuft bei `setTheme`, Modus-Wechsel, `registerInstance` und – siehe CORE-704 – bei jedem Zeilen-Graft über alle Elemente.
- **Code-Auszug**:
```cpp
for (const std::string& prop : uiThemeDecidedProps(e, theme))
    if (e.themeRoleFor(prop).empty()) write(prop);   // write → uiThemeValueFor → uiThemeStylesFor
```
- **Lösung**: Stilkette einmal pro Element bestimmen und an eine interne `uiThemeValueFor(e, styles, mode, prop, out)` weiterreichen; Property-Namen als `std::string_view`/Pointer in den Style-Daten statt Kopien; Dedup über kleines `flat_set` o. ä.
- **Erwarteter Effekt**: Schätzung: 2-4× weniger Allokationen pro Element beim Theme-Wechsel; wirkt hauptsächlich in Kombination mit CORE-704 (dort Hauptgewinn durch Bereichsbeschränkung).
- **Risiko / Verifikation**: Niedrig. Bestehende Theme-Tests (Rollen-Bindung, `kUIThemeLiteral`, Kaskade); Ausgabe muss bytegleich bleiben.

### CORE-713 – `uiElementToJsonObj` baut pro Element ein Prototyp-Element
- **ID**: CORE-713
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1820-1826`; außerdem `:1754`/`:1758` (zweimal `uiAnchorLegacyPointOf`)
- **Problem**: Für jede Serialisierung (Speichern, Undo-Snapshots des Designers, Collab pro Element, Clipboard) wird `makeUIElement(e.type())` heap-allokiert und konstruiert, nur um `cornerRadius` zu vergleichen. `uiAnchorLegacyPointOf` (9-Punkt-Schleife) wird doppelt ausgewertet.
- **Code-Auszug**:
```cpp
if (const std::unique_ptr<UIElement> proto = makeUIElement(e.type());
    !proto || e.cornerRadius != proto->cornerRadius)
```
- **Lösung**: Statisches Array `const glm::vec4 kDefaultRadius[UIWidgetType::COUNT]` lazily (function-local static) befüllen, oder einen virtuellen `defaultCornerRadius()`-Getter am Element; Ergebnis von `uiAnchorLegacyPointOf` in lokaler Variable halten.
- **Erwarteter Effekt**: Schätzung: eine Allokation + Konstruktion pro Element und Save eingespart; Editor-Pfad, daher niedrig.
- **Risiko / Verifikation**: Niedrig; die vorhandenen Round-Trip-Tests (byte-identisches Speichern) decken es ab.

### CORE-714 – `uiElementRotation`: drei fast identische Parent-Walks für die Früh-Ausstiegsprüfung
- **ID**: CORE-714
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1641-1706` (Walks `:1647-1656`, `:1661-1672`, `:1685-1702`)
- **Problem**: Die Prüfung „gibt es überhaupt eine Rotation in der Kette“ benötigt zwei separate Wanderungen (Summe der Winkel, dann nochmal „irgendein Eltern-Winkel > 1e-4“), danach eine dritte für die eigentliche Berechnung. Die Funktion wird für jedes Element in `extract`/`topmostHit` aufgerufen (Common Case: false) und kostet damit zwei Ketten mit linearem `find`.
- **Code-Auszug**:
```cpp
if (std::fabs(total) < 1e-4f && std::fabs(e.rotation) < 1e-4f)
{
    bool any = false;
    int g2 = 0;
    const UIElement* c2 = &e;
    while (c2->parentId != 0 && g2++ < ...) { ... if (std::fabs(p->rotation) > 1e-4f) { any = true; break; } ... }
    if (!any) return false;
}
```
- **Lösung**: Eine einzige Schleife, die `total` *und* `any` gleichzeitig berechnet; Rückgabe `false` genau dann wenn `|total|<1e-4 && |e.rotation|<1e-4 && !any` (identische Semantik). Mit CORE-703 entfällt die Funktion im Common Case ganz (Flag „Kette rotiert“ aus dem Top-down-Pass).
- **Erwarteter Effekt**: Schätzung: halbiert die Kettenwanderungen im Common Case; kleiner Lesbarkeitsgewinn.
- **Risiko / Verifikation**: Niedrig; Test mit Kette aus sich aufhebenden Rotationen (+30°/−30°) und mit vielen sehr kleinen Winkeln (Sonderfall `total ≥ 1e-4` ohne einzelnen Winkel > 1e-4).

### CORE-715 – `processPointer` (~1070 Zeilen) und `WidgetManager.cpp` (6810 Zeilen) bündeln unabhängige Zuständigkeiten
- **ID**: CORE-715
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `WidgetManager.cpp:3127-4198` (processPointer), gesamte Datei
- **Problem**: `processPointer` enthält Menüleiste, Dropdown-Grab, Hover, Listen-/Tree-/Datepicker-Hover, Press-Behandlung pro Widget-Typ (Slider, Splitter, TextInput, DatePicker, ColorPicker, TabBox, Accordion, Tree, Liste) mit bis zu 8 Einrückungsebenen, plus fünf Drag-Zweige; `resolveCanvas(w.tree, …)` wird ~12× inline neu berechnet, identische Rect→Pixel-Rect-Umrechnung (`pxr{ r.x*scaleX … }`) steht ~8× kopiert. Die Datei mischt zudem Instanzverwaltung, Menüleiste, Listen, Animation, Textbearbeitung, Drag&Drop, Fokus/Navigation und Zeichnen.
- **Code-Auszug**:
```cpp
const HE::UIWidgetRect pxr{ r.x * canvas.scaleX, r.y * canvas.scaleY,
                            r.w * canvas.scaleX, r.h * canvas.scaleY };
```
- **Lösung**: Hilfsfunktion `pixelRectOf(tree, e, canvas)`; `processPointer` in Teilfunktionen je Phase (`handleMenuBarPointer`, `handleDropdownPointer`, `updateHover`, `handlePress(w, hot, …)` mit je einer Funktion pro Widget-Typ, `handleDrags`) zerlegen; Datei in `WidgetManager_{Menu,Lists,Anim,Text,Drag,Focus,Draw}.cpp` aufteilen. Reine Verschiebung, keine Logikänderung.
- **Erwarteter Effekt**: Keine Laufzeitänderung; deutlich bessere Les-/Testbarkeit und Compile-Zeit (Teilneubauten).
- **Risiko / Verifikation**: Niedrig bei mechanischer Zerlegung; Absicherung durch die bestehende Test-Suite (`tests/test_ui_widgets.cpp`, Pointer-/Drag-Szenarien) vor und nach jedem Teilschritt. Nur nach CORE-701-703 sinnvoll, sonst Merge-Konflikte.

### CORE-716 – Editor-/Hilfsoperationen am Baum sind O(n²) (removeSubtree, Clipboard, Selection)
- **ID**: CORE-716
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIWidgetTree.cpp:21-26` (childrenOf allokiert), `:42-52` (removeSubtree), `:2563-2584` (uiSelectionRoots), `:2603-2621` (uiElementsToClipboard), `:2623-2671` (uiElementsFromClipboard `:2655-2661`)
- **Problem**: `removeSubtree` ruft pro Knoten `childrenOf` (voller Scan + neuer Vektor) und sucht danach pro Element linear in `doomed`; `uiElementsToClipboard` macht `std::find` in `take` pro Element; `uiSelectionRoots` O(k·Tiefe·n); `uiElementsFromClipboard` sucht in `remap` linear und ruft `tree.add`/`tree.find` (linear). Nur Editor, aber bei großen Seiten (Löschen/Duplizieren großer Teilbäume) O(n²).
- **Code-Auszug**:
```cpp
return std::find(doomed.begin(), doomed.end(), e->id) != doomed.end();
```
- **Lösung**: Einmal `parent → children`-Index (oder `unordered_set<int>` für `doomed`/`take`/`selected`) bauen; `remap` als `unordered_map<int,int>`.
- **Erwarteter Effekt**: Schätzung: O(n²) → O(n); spürbar erst bei >1000 Elementen.
- **Risiko / Verifikation**: Niedrig; Reihenfolge der Ergebnisse (Tree-Vektorreihenfolge) bleibt identisch. Vorhandene Designer-/Clipboard-Tests.

### CORE-717 – `UITextCatalog::find` sucht linear nach Sprache und Schlüssel
- **ID**: CORE-717
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIWidgetTree.cpp:2287-2315`, `:2373-2398` (`uiApplyTextCatalog`)
- **Problem**: `find` läuft pro gebundener Property durch alle Sprachen und alle Einträge der Sprache (String-Vergleich pro Eintrag); beim Sprachwechsel und (siehe CORE-704) pro Zeilen-Graft wird das für alle gebundenen Elemente wiederholt: O(Elemente·Katalogeinträge). Die Reihenfolge in den Vektoren muss für die Datei erhalten bleiben (Kommentar), aber das schließt einen zusätzlichen Index nicht aus.
- **Code-Auszug**:
```cpp
for (const auto& [k, text] : entries) if (k == key) return &text;
```
- **Lösung**: Pro Sprache zusätzlich `unordered_map<string,size_t>` (Index in `entries`) lazy aufbauen und bei `set()` pflegen; `languages` bleibt der Datei-Reihenfolge-Träger.
- **Erwarteter Effekt**: Schätzung: O(1) statt O(Katalog) je Lookup; relevant nur bei großen Katalogen (≥ tausend Keys).
- **Risiko / Verifikation**: Niedrig; Round-Trip-Test der Katalog-JSON (Reihenfolge) bleibt maßgeblich.

### CORE-718 – `focusNext`: rekursive `std::function`-Wanderung mit O(n)-`childrenOf` pro Knoten
- **ID**: CORE-718
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `WidgetManager.cpp:5923-5933` (und `#include <functional>` `:10`), `:5945-5951`
- **Problem**: Die Tab-Reihenfolge wird über eine rekursive `std::function` aufgebaut, die pro Knoten `tree.childrenOf` (voller Scan + Vektor-Allokation) und danach `find` ruft – O(n²) und n Allokationen pro Tab-Druck. Außerdem ruft jedes Kandidatenelement `isFocusable` → `isInteractive` (siehe CORE-707) und mehrere Kettenwanderungen.
- **Code-Auszug**:
```cpp
const std::function<void(int)> walk = [&](int parent)
{
    for (int child : w->tree.childrenOf(parent))
```
- **Lösung**: Mit dem `parent → Kinder`-Index (CORE-701) iterativ (expliziter Stack) durchlaufen; `std::function`-Include entfällt. Nutzereingabe-getrieben, daher niedrige Priorität.
- **Erwarteter Effekt**: Gering (Tastendruck), vor allem Vereinfachung.
- **Risiko / Verifikation**: Niedrig; Reihenfolge Depth-first, Eltern vor Kindern, muss identisch sein (vorhandene Tab-Order-Tests).

### CORE-719 – Unnötige Kopien in Konstruktions-/Design-Pfaden
- **ID**: CORE-719
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `WidgetManager.cpp:254` (`const std::vector<std::string> chain = queue[qi].chain;`), `:501-502` (`HorizonCode::Graph graph` by value), `:608-625` (`w.tree = tree` + zusätzlicher `before`-Vollklon + Vergleich aller Properties per `getPropAny`), `:526` (`const std::string json = ta->json;`)
- **Problem**: `embedWidgetRefs` kopiert pro Ref die Kette (`vector<string>`); `runDesignTimePreConstruct` kopiert die ganze Baumstruktur zweimal (tiefe `clone()` je Element) und vergleicht anschließend jede Property aller Elemente über string-basierte `getPropAny`; `registerInstance` nimmt den Graph by value (Aufrufer mit `const&` kopieren ihn). Die Kopien an manchen Stellen (`:526`, `:254`) sind aus Reallokations-Sicherheitsgründen kommentiert und teils nötig; nur die Designer-Variante ist wirklich vermeidbar.
- **Code-Auszug**:
```cpp
w.tree = tree;
registerInstance(content, w, graph);
...
const HE::UIWidgetTree before = m_instances.back().tree;
```
- **Lösung**: `chain` als Index/`shared_ptr<const Chain>` bzw. verkettete Liste (Eltern-Zeiger) statt Kopie je Queue-Eintrag; im Designer-Lauf `before` als kompakte Liste der Property-Werte statt Vollklon speichern oder die Änderungen per Dirty-Markierung am Setzen erfassen.
- **Erwarteter Effekt**: Klein (Designer-Preview, Widget-Erzeugung); nur bei sehr großen Widgets relevant.
- **Risiko / Verifikation**: Niedrig; Designer-Preview-Tests (`runDesignTimePreConstruct`-Schreibliste muss identisch bleiben).

## Cross-Modul-Hinweise

- `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:1281-1330` (`eventBindingsOf`): allokiert pro Aufruf; Bedingung für CORE-707. Außerdem unsicher, ob `fireTick(scriptId)` (WidgetManager.cpp:2766-2774) für Instanzen ohne Tick-Event früh zurückkehrt – andernfalls teuer für jede Embed-Instanz (Listenzeilen) pro Frame.
- `src/HE_Core/include/GraphCommon/GraphModel.h:45-71` (`findNodeById`): linear; dieselbe Struktur nutzen Node-Graphen. Ein optionaler `id→index`-Index wäre dort wiederverwendbar.
- `src/HE_Rendering/src/RenderExtractor.cpp:1589`: `world.widgets().extract(...)` läuft pro Fenster/Frame; ob `extract` bei `!consumeVisualDirty()` übersprungen wird, liegt außerhalb dieses Slices (Event-getriebene Schleife in `EditorApplication`/`GameApplication`) – bei fehlendem Gate wären CORE-701/703 doppelt kritisch.
- `src/HE_Editor/EditorApplication.cpp:4188` und `src/HE_Game/src/GameApplication.cpp:2347`: rufen `processPointer` ohne Gate auf (relevant für CORE-702).

## Unsicher / Messung nötig

- Reale Größenordnung von n (Elemente pro Widget-Seite) in den ausgelieferten Apps/Templates: davon hängt ab, ob CORE-701/702/703 Millisekunden oder Mikrosekunden kosten. Profil mit einer Seite mit 500+ Elementen und einer Tabelle mit 30 sichtbaren Zeilen.
- Kosten von `measureUIText` / `sharedUIFont` (CORE-711): Cache vorhanden?
- Ob alle `applyAutoSize`-Überschreibungen bei `autoSizedAxes()==0` wirkungslos sind (UIElements.h:187, 364, 840, 1993) – Voraussetzung für den Vorfilter in CORE-710.
- Ob `Runtime::fireTick` für Instanzen ohne Tick-Binding schnell zurückkehrt.
- Ob `processPointer`/`extract` in Editor und Spiel tatsächlich jeden Frame laufen oder durch `consumeVisualDirty` gegated werden (siehe Cross-Modul).
- Ob das Re-Theming des ganzen Baums bei Row-Graft (CORE-704) ein gewollter Effekt ist (z. B. um Skript-gesetzte Werte zurückzusetzen).

## Gelesen

Ja, alle sechs Dateien vollständig gelesen: `UIWidgetBinding.h` (16 Z.), `UIWidgetTree.h` (675), `UIWidgetTree.cpp` (2788), `UIWindowFrame.h` (93), `UIWindowFrame.cpp` (156), `WidgetManager.cpp` (6810, in Blöcken). Zusätzlich gezielt (nur per grep/kurzem Ausschnitt) `GraphModel.h`, `UIElement.h`, `UIElements.h`, `WidgetManager.h`, `HorizonCodeRuntime.cpp` (`eventBindingsOf`) und die Aufrufstellen in `GameApplication.cpp`/`EditorApplication.cpp`. `UIWindowFrame.*` und `UIWidgetBinding.h` ohne Befund.
