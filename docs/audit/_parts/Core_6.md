# Core Slice 6 – Sequence/SequenceJson, SlotMap, Types/*, UIWidget/{AppMenu, UIElement, UIElements, UIShortcut, UITheme, UIWidgetAnim, UIWidgetBinding}

## Zusammenfassung

Der Slice (20 Dateien, ~11 000 Zeilen) ist insgesamt sauber und sehr gut kommentiert; die Property-Tabellen (`uiprop::slot`), die „eine Rechnung für Zeichnen und Hit-Test"-Helfer und die kleinen Parser (Shortcut, Ease, Binding) sind einfach und korrekt. Die relevanten Probleme liegen an drei Stellen: (1) Frame-Loop-Kosten im UI-Baum (`hidesChild` ist O(N) und wird über die Vorfahrenkette rekursiv gefragt → quadratisch; `UIText::applyAutoSize` misst jedes Label jeden Frame neu), (2) die doppelt/dreifach geführte Liste der Basis-Properties samt String-If-Kette auf dem Animationspfad, (3) Robustheit und Kopien in den JSON-Pfaden (TypeRegistry, Theme, Sequence, UIElement::readJson). Zusätzlich ein echter Logikfehler (Zyklusschutz in `fieldDefault` wird für Struct-Arrays umgangen). Funde: 2 Hoch, 5 Mittel, 11 Niedrig. Alle Performance-Aussagen sind Schätzungen ohne Messdaten und als solche gekennzeichnet.

---

### CORE-601 – `hidesChild` ist O(N) und wird je Vorfahr rekursiv gefragt (quadratisch pro Frame)
- **ID**: CORE-601
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIElement.cpp:3022-3031` (TabBox), `:3148-3154` (Splitter), `:3296-3303` (Accordion); Helfer `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1501-1518`; Aufrufer `UIWidgetTree.cpp:1522-1531` (`uiElementEffectiveVisible`)
- **Problem**: `uiElementEffectiveVisible` ruft für jeden Vorfahren `p->hidesChild(tree, e)` auf und rekurriert dann zum nächsten Vorfahren. Bei TabBox/Splitter/Accordion löst `hidesChild` über `uiChildIndexOf` (lineare Suche über *alle* `tree.elements`) bzw. zusätzlich `uiChildCountOf` (nochmal alles) den Kindindex auf. Für jedes Element unterhalb eines solchen Containers kostet „ist sichtbar?" also O(Tiefe_unter_Container × N); über alle Elemente O(N²). `uiElementEffectiveVisible` hat 16 Aufrufstellen (Extract, Hit-Test, Designer). Typische App-Shells (Splitter + TabBox an der Wurzel) liegen in genau dieser Situation.
- **Code-Auszug**:
```cpp
bool UITabBox::hidesChild(const UIWidgetTree& tree, const UIElement& child) const
{
    const int idx = uiChildIndexOf(tree, id, child.id);          // O(N)
    if (idx < 0) return false;
    const int active = activeTab >= 0 && activeTab < uiChildCountOf(tree, id) ? activeTab : 0; // O(N)
    return idx != active;
}
// UIWidgetTree.cpp
int uiChildIndexOf(const UIWidgetTree& tree, int parentId, int childId)
{ int i = 0; for (const auto& ep : tree.elements) { if (!ep || ep->parentId != parentId) continue; if (ep->id == childId) return i; ++i; } return -1; }
```
- **Lösung**: Kindindex einmal pro Frame/Änderung berechnen statt pro Abfrage. Einfachste Variante: `UIWidgetTree` hält einen lazy aufgebauten Cache `std::unordered_map<int,int> childIndex` (childId → Index unter dem Elternteil) und `std::unordered_map<int,int> childCount`, invalidiert über einen `structureVersion`-Zähler, der in jedem add/remove/reparent/reorder erhöht wird (die Tree-Mutatoren sind bereits zentral). `uiChildIndexOf/CountOf` lesen dann aus dem Cache; `hidesChild`-Signatur bleibt unverändert. Alternative ohne Cache: `uiElementEffectiveVisible` iterativ aufwärts laufen lassen und den Index des Kindes beim Wandern berechnen, dies bleibt aber O(N) pro Schritt.
- **Erwarteter Effekt** (Schätzung): O(N²) → O(N·Tiefe) für Bäume mit Tab/Splitter/Accordion-Vorfahren; bei ~1000 Elementen unter einem TabBox grob 10⁶ Elementvergleiche pro Abfragedurchgang entfallen. Unsicher/messen, ob es bei realen Widgets sichtbar ist.
- **Risiko / Verifikation**: Cache muss bei jeder strukturellen Änderung (auch Graft von Embeds, NamedSlot-Umhängen, Designer-Drag) invalidiert werden, sonst „Klick auf unsichtbaren Button" (der im Header ausdrücklich verhinderte Fehler). Absichern mit `tests/test_ui_widgets.cpp` (TabBox/Splitter/Accordion-Hit-Test-Fälle) plus einem neuen Test, der nach Reparent/Reorder die Sichtbarkeit prüft; vorher Benchmark mit 2000-Elemente-Baum.

### CORE-602 – `UIText::applyAutoSize` misst jedes Label jeden Frame neu (Default `autoSize = true`)
- **ID**: CORE-602
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIElement.cpp:1428-1466`; Default `src/HE_Core/include/UIWidget/UIElements.h:118`; Aufrufer `WidgetManager.cpp:6157` → `UIWidgetTree.cpp:1239-1300` (läuft pro Frame über *alle* Elemente)
- **Problem**: Jedes neu angelegte Text-Element hat `autoSize = true`; `uiApplyAutoSize` ruft pro Frame für jedes Element `applyAutoSize`, bei Text also `HE::measureUIText` (bzw. `uiLayoutRichText` mit Markup-Parse-Cache-Prüfung `m_parsedFrom != text`, `UIElement.cpp:1393`) – obwohl sich Text, Schriftgröße, Wrap-Breite und Font fast nie ändern. Dazu kommen pro Frame `uiElementUnitScale` + `uiElementRect` je Element. Ergebnis wird danach nur in `sizeX/sizeY` geschrieben.
- **Code-Auszug**:
```cpp
void UIText::applyAutoSize(float resolvedWidth, float fontScale)
{
    if (!autoSize) return;
    ...
    else
        m = f ? HE::measureUIText(*f, text, fs, wrapW, opts)
              : HE::measureUIText(text, fs, wrapW, opts);   // jeden Frame
    if (!wordWrap && !stretchX) sizeX = m.x + fs * 0.25f;
    if (!stretchY)              sizeY = m.y + fs * 0.35f;
}
```
- **Lösung**: Gemessenes `glm::vec2 m` zusammen mit seinem Schlüssel in `mutable`-Membern der UIText ablegen: `{text-Hash oder Kopie, fs, wrapW, alignH/alignV, wordWrap, richText, fontAtlasKey, BakedUIFont*-Zeiger/ok-Flag}`. Bei Treffer nur `m` wiederverwenden und die `sizeX/sizeY`-Zuweisung wie bisher ausführen (die Zuweisung muss bleiben, weil Skripte/Stretch `sizeX` dazwischen ändern können). Font-Zeiger im Schlüssel, damit ein später fertig gebackener Font neu misst. Gleiche Technik für CheckBox/RadioButton/ComboBox (`:1739`, `:2366`, `:3963`), dort nur bei aktivem `autoSize`.
- **Erwarteter Effekt** (Schätzung): Pro statischem Label entfällt die komplette Glyphen-Messung pro Frame; bei UI-Szenen mit hunderten Labels vermutlich spürbar (Größenordnung: ein Teil der UI-Layout-Kosten). Unsicher/messen mit Profiler über `uiApplyAutoSize`.
- **Risiko / Verifikation**: Schlüssel muss *alle* Eingaben der Messung enthalten (Font, Scale, Wrap), sonst veraltete Größen. Tests: `tests/test_ui_widgets.cpp` (Auto-Size-Fälle, fontScale 1.5, Wrap-Text, Rich-Text); neuer Test „Text ändern → Größe folgt im selben Frame".

### CORE-603 – Basis-Properties sind dreifach/vierfach geführt (String-If-Ketten) und liegen auf dem Animationspfad
- **ID**: CORE-603
- **Kategorie**: A (und D)
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `UIElement.cpp:878-947` (`getBaseProp`), `:951-1013` (`uiBaseProperties`), `:1017-1098` (`setBaseProp`), `:1101-1181` (`allProperties`); Hot-Path-Aufrufer `WidgetManager.cpp:2382-2392` (`applyClipAt`, pro Tick und laufendem Clip, `:2565`)
- **Problem**: Die ~55 Basis-Properties stehen als 4 unabhängige Listen (Getter-If-Kette, Setter-If-Kette, Deskriptorliste, nochmal Deskriptorliste in `allProperties`). Die Listen driften bereits: `allProperties()` enthält `Accepts Drop`, `Draggable`, `Drag Payload` nicht (fehlen in `:1104-1136`), obwohl `getBaseProp/setBaseProp/uiBaseProperties` sie führen; `WidgetManager.cpp:2321` benutzt `allProperties()` als „kennt die Property?"-Test für `rememberOriginal`. Laufzeit: `getPropAny` + `setPropAny` laufen pro Sample und Frame (`applyClipAt`), jeweils bis zu ~55 `std::string == const char*`-Vergleiche (jeder mit strlen) bevor die typeigene Tabelle (ebenfalls linear) drankommt; Rückgabe jeweils als fette `UIPropValue` (String + Vector).
- **Code-Auszug**:
```cpp
bool getBaseProp(const UIElement& e, const std::string& n, UIPropValue& out)
{
    if (n == "Visible")      { out = UIPropValue::ofBool(e.visible);     return true; }
    if (n == "Hit Testable") { out = UIPropValue::ofBool(e.hitTestable); return true; }
    ... // ~55 weitere
```
- **Lösung**: Basis-Properties als `static const UIPropTable kBaseTable` mit denselben `uiprop::slot<&UIElement::visible>(...)`/`uiprop::custom(...)`-Zeilen deklarieren (wie die Typtabellen; Clamping in den `custom`-Settern beibehalten). Daraus `uiBaseProperties()`, `getBaseProp`, `setBaseProp` ableiten (eine Quelle). `allProperties()` = Typtabelle + gefilterte Basistabelle (Filter `hasSurfaceStyle()/hasMaterialSlot()/...` über ein Flag-Feld je Zeile statt Hand-Liste). Für den Hot Path zusätzlich ein Lookup `unordered_map<string_view, const UIPropSlot*>` je Tabelle (einmalig `static`), oder – billiger – `UIAnimTrack` löst den Slot einmal beim Start auf (`const UIPropSlot*`) und `applyClipAt` ruft `slot->set` direkt.
- **Erwarteter Effekt** (Schätzung): ~170 Zeilen weniger, eine Quelle der Wahrheit; pro animiertem Sample entfallen ~100 String-Vergleiche + 2 temporäre `UIPropValue`. Bei wenigen Clips nicht spürbar, bei Listen-/Menü-Animationen mit vielen Tracks messbar (unsicher).
- **Risiko / Verifikation**: Die Namen sind On-Disk-Format; `allProperties()` um die drei fehlenden Properties zu erweitern ändert die MCP-/Editor-Ausgabe (bewusste Verhaltensänderung, ggf. getrennt machen). Reihenfolge muss erhalten bleiben (Editor-Anzeige). Absicherung: bestehender Test, der `uiBaseProperties` gegen `getBaseProp` pinnt (`tests/test_ui_widgets.cpp`), plus Round-Trip-Test je Basis-Property.

### CORE-604 – Zyklusschutz in `fieldDefault` wird für Struct-Arrays umgangen (unbegrenzte Rekursion)
- **ID**: CORE-604
- **Kategorie**: A (Korrektheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Types/TypeRegistry.cpp:328-331` (Array-Zweig), Eingang über `:670-672` (`structFromJson`)
- **Problem**: Die Kopfkommentare (`TypeRegistry.h:137-141`) versprechen, dass jeder rekursive Konsument einen Visited-Guard trägt. Im Array-Zweig ruft `fieldDefault` für Struct-Elemente `reg.makeDefaultValue(f.typeName)` auf, das ein *neues* `visiting`-Set anlegt – der Guard des laufenden Aufrufs geht verloren. `structFromJson` legt für ein `isArray`-Feld Slots aus `"default"` an, auch für `PinType::Struct` (`defaultFromJson` ignoriert den Wert, der Slot entsteht trotzdem). Eine handeditierte Definition `A { arr: Array<A>, "default": [0] }` rekursiert damit `structDefault(A) → fieldDefault → makeDefaultValue(A) → …` bis zum Stack-Overflow (Editor/Spiel-Absturz beim ersten `makeDefaultValue`). `structToJson` schreibt solche Slots nie (`:596`), der Fall entsteht nur durch Handarbeit/MCP.
- **Code-Auszug**:
```cpp
else if (f.type == PinType::Struct)
{
    it = reg.makeDefaultValue(f.typeName);   // elements use their own defaults   <- neues visiting-Set
}
```
- **Lösung**: `visiting` durchreichen: `it = structDefault(reg, f.typeName, visiting);` (dafür steht `structDefault` schon vor `fieldDefault`). Zusätzlich in `structFromJson` Struct-Slots nicht anlegen (nur zählen, damit `items.size()` konsistent bleibt oder ganz verwerfen, passend zu „Struct elements carry no slot payload").
- **Erwarteter Effekt**: Kein Absturz mehr bei zyklischen Handdaten; Verhalten für nicht-zyklische Definitionen identisch (jedes Element bekommt weiter seinen eigenen Default).
- **Risiko / Verifikation**: Test in der TypeRegistry-Testdatei (siehe `tests/CMakeLists.txt`): Selbstreferenz mit Array-Default, direkter Zyklus A→B→A; erwartet Terminierung und leeres/abgeschnittenes Struct.

### CORE-605 – Inkonsistente Fehlertoleranz der JSON-Lader (Wurf aus `value()`/`get<float>()` in handeditierten Dateien)
- **ID**: CORE-605
- **Kategorie**: A/F (Robustheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `UITheme.cpp:30-35, 443, 474, 478-479`; `TypeRegistry.cpp:633-643, 457-482`; `SequenceJson.cpp:175-176, 221-231, 239-243, 297-304`; `UIElement.cpp:270-307, 2429-2435`
- **Problem**: Die Loader behaupten „fehlerhafte Einträge werden verworfen, nicht der ganze Asset", verhalten sich aber verschieden: `sequenceFromJson` fängt `json::exception` und lehnt dann das *ganze* Dokument ab (ein String in `"start"` macht die komplette Sequence unlesbar, `dropped` bleibt ungesetzt). `uiThemeFromJson` hat gar kein try/catch: `readColor` ruft `j[0].get<float>()` ohne Zahlenprüfung (`:33`), `j.value("name", …)` (`:443`) und `s->value("blur", …)` (`:474`) werfen bei falschem Typ. `TypeRegistry::structFromJson/enumFromJson` parsen mit `allow_exceptions=false`, rufen danach aber `e.value("type", 1)` (`:634`) und `j[0].get<float>()` (`:458`) ungeschützt. `uiPropValueFromJson` (`:286-288`) und `colFrom` (`:2432-2433`) casten Array-Elemente ungeprüft. Ob die Aufrufer fangen, ist je Aufrufer verschieden (z.B. `UIEditorPanel.cpp:410`, `McpToolsType.cpp:216` zeigen keinen try/catch) → unsicher/prüfen.
- **Code-Auszug**:
```cpp
bool readColor(const nlohmann::json& j, glm::vec4& out)
{
    if (!j.is_array() || j.size() != 4) return false;
    out = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };  // wirft bei "x"
    return true;
}
```
- **Lösung**: Ein gemeinsamer kleiner Helfer (z.B. in `GraphCommon/GraphJson.h`, das `sequenceFromJson`/Theme schon einbinden): `template<class T> T jget(const json& j, const char* key, T def)` = `find` + `is_number()/is_string()`-Prüfung statt `value()`, und `bool jfloat4(const json&, glm::vec4&)` mit `is_number` je Element. Dann entfällt der Sonder-try/catch in `sequenceFromJson` und das Verwerfen geschieht pro Eintrag wie dokumentiert. Alternativ pragmatisch: an jedem öffentlichen `*FromJson`-Eintritt ein einheitliches `try { … } catch (const json::exception&) { return false; }`.
- **Erwarteter Effekt**: Keine Abstürze/Totalverluste bei handeditierten Assets; weniger Duplikat-Absicherung. Kein Laufzeitgewinn.
- **Risiko / Verifikation**: Verhalten bei *gültigen* Dateien unverändert; bei kaputten ändert sich „ganzes Dokument abgelehnt" → „Eintrag verworfen" (Doku in `SequenceJson.h:31-37` verspricht Letzteres). Tests: je Loader ein Fuzz-artiger Test mit falschen Typen (`"duration":"2"`, `"colors":{"Text":{"Light":["a",1,2,3]}}`).

### CORE-606 – `TypeRegistry` liefert tiefe Kopien: `getStruct/getEnum` je Verschachtelungsebene und je Array-Element, `structs()/enums()` kopieren + sortieren pro Aufruf
- **ID**: CORE-606
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `TypeRegistry.cpp:160-178` (Kopie unter Lock), `:194-220` (Kopie + `sort`), `:292-293` (`structDefault`), `:318-324`, `:344-347` (EnumDef-Kopie pro Element), `:403-407`
- **Problem**: `getEnum` kopiert die komplette `EnumDef` (Entries + `formerNames`-Vektoren), `getStruct` die komplette `StructDef` inkl. aller `StructField`-`HorizonCode::Value`s. `structDefault` macht das für jedes Struct auf jeder Ebene, `fieldDefault` für Enum-Arrays/Map-Keys sogar *pro Element* (die Schleifen `for (Value& it : v.items)` / `for (Value& k : v.keys)` rufen `reg.getEnum(...)` im Schleifenkörper). `enums()/structs()` kopieren alles und sortieren bei jedem Aufruf; Aufrufer sind u.a. Editor-Dropdowns (`HcGraphHost.cpp:1249,1300,1694`, `HcEditorUtil.cpp:554-627`), potenziell pro Frame in offenen Panels (unsicher).
- **Code-Auszug**:
```cpp
for (Value& it : v.items)
{
    ...
    if (f.type == PinType::Enum)
    {
        EnumDef ed;                          // pro Element
        ...
        if (reg.getEnum(f.typeName, ed)) { ... }
```
- **Lösung**: (a) Kurzfristig `getEnum` aus der Schleife herausziehen (einmal vor der Schleife laden). (b) Strukturell: `Impl` speichert `std::shared_ptr<const EnumDef/StructDef>`; Getter geben `shared_ptr<const T>` zurück (Kopie = Refcount) und `enums()/structs()` können einen gecachten, bei `register*/remove/clear` invalidierten sortierten Snapshot (`shared_ptr<const vector<…>>`) liefern. Die Signatur `bool getX(path, T& out)` kann als dünner Wrapper bleiben.
- **Erwarteter Effekt** (Schätzung): Kopien pro Default-Wert-Erzeugung von O(Tiefe·Felder) auf O(1) pro Zugriff; für UI-Dropdowns pro Frame Wegfall von Kopie+Sort (Größenordnung: Projekte mit dutzenden Typen). Unsicher/messen, ob außerhalb des Editors relevant.
- **Risiko / Verifikation**: API-Änderung berührt ~30 Aufrufer (Editor, Python, Scripting, Codegen); den Kopier-Wrapper beibehalten, dann nur interne Änderung. Tests: bestehende TypeRegistry-/HorizonCode-Struct-Tests.

### CORE-607 – `TreeView`: `visibleRows()`, `parseIntList()` und Text-Vergleich bei jedem Aufruf (mehrfach pro Frame)
- **ID**: CORE-607
- **Kategorie**: C/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `UIElements.h:2086, 2102-2123`; `UIElement.cpp:4063-4067` (`nodes()`), `:4112-4117` (`isCollapsed`), `:4133-4155` (`visibleRows`), `:4229-4230, 4260` (`render`)
- **Problem**: `nodes()` vergleicht bei *jedem* Aufruf `m_parsedFor == items` (O(Textlänge)). `visibleRows()` parst `collapsed` neu (`parseIntList`, Heap), legt `hidden` und `rows` an und läuft O(Knoten). Es wird aufgerufen von `measuredExtent()` → `maxScroll()` → `maxScrollAmount()` und `scrollBar()` (Aufrufer: `uiUpdateScrollExtents` pro Frame `UIWidgetTree.cpp:1556`, `uiScrollThumbRect` im Render und bei Drag), von `rowAt()` (jede Mausbewegung), `scrollToRow` und `render`. `render` ruft zusätzlich `parseIntList(collapsed)` und dann pro Pfeil ein lineares `std::find` (O(Zeilen·Folds)). Pro Frame also ≥3× volle Neuberechnung mit mehreren Heap-Allokationen; bei einem Baum mit tausenden Knoten (Dateibrowser, Outliner – genau der beworbene Einsatz) spürbar.
- **Code-Auszug**:
```cpp
float measuredExtent() const
{ return static_cast<float>(visibleRows().size()) * rowHeight; }   // allokiert + O(n)
int rowAt(float localY) const { ... return i < static_cast<int>(visibleRows().size()) ? i : -1; }
```
- **Lösung**: Fold-Zustand und sichtbare Zeilen mitcachen, analog zu `m_nodes`: `mutable std::string m_collapsedFor; mutable std::vector<int> m_rows; mutable std::vector<char> m_folded;` Neuaufbau nur, wenn sich `items` oder `collapsed` geändert haben (Stringvergleich genügt, oder Versionszähler in `setCollapsed`/Property-Setter, aber beachte, dass `collapsed` ein öffentliches Feld ist → Vergleich behalten). `visibleRows()` gibt `const std::vector<int>&` zurück, `isCollapsed` nutzt `m_folded[node]` (O(1)). Gleiches Muster für die Container-Render-Allokationen: `UIAccordion::render` baut pro Frame zwei Vektoren (`:3342-3348`), `contentHeight/headerAt` je eine `std::vector<Slot>` (`:3278, 3289`).
- **Erwarteter Effekt** (Schätzung): Pro Frame und TreeView von mehreren O(n)-Neuaufbauten + Allokationen auf einen Stringvergleich; größter Gewinn bei >1000 Knoten.
- **Risiko / Verifikation**: Cache-Invalidierung bei direkter Schreibweise auf `collapsed`/`items` (Skript, Property) – über den Stringvergleich robust. Tests in `tests/test_ui_widgets.cpp` (TreeView Fold/Selection/Scroll) + neuer Test: Fold ändern über `setProp("Collapsed")` und Zeilenliste aktualisiert sich.

### CORE-608 – JSON-Lesepfade kopieren Teilbäume (`j.value(key, json())`, `t.value("sections", kEmpty)`)
- **ID**: CORE-608
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIElement.cpp` (56 Stellen, z.B. `:2438`, `:2809-2811`, `:3139-3143`), Helfer `:2429-2435`; `SequenceJson.cpp:221, 235, 254, 265`, `:173`
- **Problem**: `nlohmann::json::value(key, default)` liefert den Wert *by value*. `colFrom(j.value("color", nlohmann::json()), color)` kopiert das 4-elementige Array (Heap) und konstruiert zudem ein temporäres `json()`; in `parseSequence` kopiert `t.value("sections", kEmpty)` bzw. `"cuts"/"events"` die komplette Section-Liste jedes Tracks. Läuft beim Laden jedes Widgets (viele Farben pro Element) bzw. jeder Sequence; kein Frame-Pfad, aber unnötige Allokationen auf dem Ladepfad.
- **Code-Auszug**:
```cpp
void UIPanel::readJson(const nlohmann::json& j)  { color = colFrom(j.value("color", nlohmann::json()), color); }
...
for (const auto& s : t.value("sections", kEmpty))   // Kopie der Section-Liste
```
- **Lösung**: `colFrom(const json& parent, const char* key, const glm::vec4& def)` mit `find` + `is_array()`; in `parseSequence` `const json* arr = find(t, "sections")` / `if (it != end && it->is_array())` iterieren. Zusammen mit CORE-605 umsetzen (gemeinsamer Helfer).
- **Erwarteter Effekt** (Schätzung): Weniger Kleinallokationen beim Laden; nur bei sehr großen Widgets/Sequences messbar.
- **Risiko / Verifikation**: Rein mechanisch; bestehende Lade-/Round-Trip-Tests in `tests/test_ui_widgets.cpp` genügen.

### CORE-609 – `UIElement::copyBaseTo` ist toter Code (und ein Drift-Risiko)
- **ID**: CORE-609
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/include/UIWidget/UIElement.h:979-1025`
- **Problem**: Der 45-Zeilen-Block kopiert von Hand ~60 Felder („used by clone()"), aber alle `clone()`-Implementierungen in `UIElements.h` nutzen den Kopierkonstruktor (`std::make_unique<UIPanel>(*this)`); `grep -rn copyBaseTo src` findet nur die Definition. Bei neuen Basisfeldern würde der Block veralten (z.B. enthält er schon nicht `shadow…` identisch zur Reihenfolge, aber jedes neue Feld fehlt) und der Kommentar lügt.
- **Code-Auszug**:
```cpp
protected:
    // Copy the shared base fields into a freshly-constructed subclass (used by clone()).
    void copyBaseTo(UIElement& dst) const { dst.id = id; dst.parentId = parentId; ... }
```
- **Lösung**: Block und Kommentar löschen.
- **Erwarteter Effekt**: −47 Zeilen, ein Drift-Risiko weniger.
- **Risiko / Verifikation**: Keine Referenz → Build genügt; zusätzlich `grep -rn copyBaseTo` über `src/` und `tests/` (Tests wurden nicht durchsucht, vorher prüfen).

### CORE-610 – Caret-/Selektions-Hit-Test ist O(n²) (Substring + Messung je Zeichen), `selectRows` misst jeden Frame
- **ID**: CORE-610
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIElement.cpp:1611-1625` (`byteAtRowX`), `:1677-1718` (`UIText::caretAtPoint`), `:2352-2360` (`UITextInput::caretAtPoint`), `:1627-1675` + `:1493` (`selectRows` je Frame bei `selectable`)
- **Problem**: Für jedes Zeichen werden zwei Teilstrings gebildet und je komplett gemessen (`widthTo(i)`, `widthTo(nx)`); Gesamt O(L²) pro Klick/Drag-Event (und pro Pfeiltaste in `byteAtRowX`). Bei einer langen Einzelzeile (z.B. 10 000 Zeichen, `wrapText` aus) ~10⁸ Glyphschritte pro Mausbewegung beim Ziehen. Außerdem misst `UIText::render` für `selectable`-Labels pro Frame jede Zeile (`selectRows`), obwohl sich Text/Breite nicht ändern. Nur Interaktions-/Opt-in-Pfade → niedrig.
- **Code-Auszug**:
```cpp
for (size_t i = lo; i < hi; )
{
    const size_t next = uiUtf8Next(text, i);
    const float a = runWidth(text.substr(lo, i - lo));
    const float b = runWidth(text.substr(lo, next - lo));
    if (localX < (a + b) * 0.5f) return i;
```
- **Lösung**: Einmal pro Zeile die kumulativen Caret-Positionen berechnen (eine Messung der Zeile liefert i.d.R. per-Glyph-Advance; falls `measureUIText` nur Summen liefert: Binärsuche über Byte-Offsets mit O(log L) Messungen) und das vorherige `a` als `b` wiederverwenden (halbiert die Messungen sofort). `selectRows` mit `(text, sizePx, px.w, alignH)`-Schlüssel cachen.
- **Erwarteter Effekt** (Schätzung): O(L²) → O(L log L) pro Klick; Frame-Messung von Selectable-Labels entfällt.
- **Risiko / Verifikation**: Mittelpunktsregel („linke Hälfte → davor") und UTF-8-Grenzen müssen identisch bleiben; Tests `tests/test_ui_widgets.cpp` (Caret-Positionierung, fontScale 1.5, Multiline-Wrap).

### CORE-611 – Menü-Shortcuts werden bei jedem Tastendruck für jede Zeile neu geparst
- **ID**: CORE-611
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/UIWidget/UIShortcut.cpp:141-165` (`uiShortcutMatches`, `uiShortcutMatchesText`), Aufrufer `WidgetManager.cpp:1323`; `include/UIWidget/AppMenu.h:42` (`std::string shortcut`)
- **Problem**: `menuItemForShortcut` ruft pro Tastendruck für jeden Menüeintrag `uiShortcutMatchesText` auf: `squeeze` + `lower` + Token-Zerlegung + `canonicalKey` (mehrere `std::string`-Allokationen), dann `squeeze(lower(...))` auf beiden Seiten des Matches. Nur auf Tastendruck, aber O(Einträge) Parses; die Chord-Struktur `UIShortcut` existiert bereits.
- **Code-Auszug**:
```cpp
bool uiShortcutMatchesText(const std::string& text, const std::string& keyName, bool ctrl, bool shift, bool alt)
{
    UIShortcut s;
    if (!uiParseShortcut(text, s)) return false;      // bei jedem Tastendruck
    return uiShortcutMatches(s, keyName, ctrl, shift, alt);
}
```
- **Lösung**: `AppMenuItem` bekommt ein `mutable UIShortcut parsed; mutable bool parsedValid`, gefüllt beim Aufbau/`setMenuBar` (der Text bleibt führend). `uiShortcutMatches` vergleicht die kanonische `key` mit dem normalisierten `keyName` ohne Zwischenstrings (`keyName` einmal pro Tastendruck normalisieren, nicht pro Eintrag).
- **Erwarteter Effekt**: Pro Tastendruck O(Einträge) Allokationen → O(Einträge) Vergleiche; absolut klein (Schätzung).
- **Risiko / Verifikation**: Nur Cache-Invalidierung bei Änderung von `shortcut`; Tests `tests/test_ui_widgets.cpp` / Shortcut-Tests (Ctrl+S vs. Ctrl+Shift+S, „Page Up" vs. „PageUp", keypad Enter).

### CORE-612 – `SlotMap`: `clear()` verliert alle Slot-Indizes, kein Invalid-Handle, doppelte Generationserhöhung
- **ID**: CORE-612
- **Kategorie**: C/F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/include/SlotMap.h:14-32, 6-9, 80-85`
- **Problem**: (1) `clear()` leert `m_freeList` und erhöht nur die Generationen; `m_slots` bleibt behalten, aber keiner der Slots ist je wieder in der Freiliste, `insert` hängt also immer neue Slots an → bei wiederholtem `clear()`/Neuaufbau (Projektwechsel, Asset-Reload) wächst `m_slots` unbegrenzt (8 Byte/Slot). (2) `remove` erhöht `generation`, `insert` erhöht sie nochmal → pro Zyklus +2 (harmlos, aber Wrap-Risiko doppelt so früh; Generationsüberlauf wird nirgends behandelt). (3) `SlotHandle{}` hat keine Default-Initialisierer – ein unbelegtes Handle ist unbestimmt, es gibt (anders als `RenderHandle::invalid()`) keinen Sentinel (Generation 0 wird nie vergeben, wäre aber möglich). (4) Globaler Namespace, widerspricht `docs/coding-conventions.md` (Legacy).
- **Code-Auszug**:
```cpp
void clear() {
    m_data.clear(); m_erase.clear(); m_freeList.clear();
    for (auto& s : m_slots) s.generation++;     // Slots nie mehr in der Freiliste
}
```
- **Lösung**: `clear()`: `m_freeList` mit allen Slot-Indizes (absteigend) füllen; in `insert` die zweite `generation++` entfernen (Generation wird beim `remove` bereits erhöht; Initialwert 1 bleibt gültig). `SlotHandle` Default `index = 0, generation = 0` (nie gültig).
- **Erwarteter Effekt**: Kein Speicherwachstum bei Reload-Zyklen; sonst neutral.
- **Risiko / Verifikation**: Handles aus der Zeit vor `clear()` müssen weiter ungültig sein (Generationen steigen weiter) – Test: insert, clear, insert → altes Handle `get()==nullptr`. `ContentManager` ist der einzige große Nutzer (`ContentManager.h:789-792`).

### CORE-613 – `UUID.h` zieht `<random>`/`<functional>` in 89 Dateien und trägt einen 2,5 KB-Zustand pro Thread
- **ID**: CORE-613
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/include/Types/UUID.h:2-24`
- **Problem**: `HE::UUID` (reiner POD mit `hi/lo`) ist einer der meistinkludierten Header (89 Dateien in `src/` laut `grep`), enthält aber `static UUID generate()` inline mit `std::mt19937_64` (`thread_local`, ~2,5 KB je Thread, seed per `random_device`). `<random>` ist ein schwerer Header. Der Hash `h1 ^ (h2 << 1)` ist für Zufalls-UUIDs ok, für deterministisch erzeugte/abgeleitete UUIDs mit gleichem `hi` (falls es welche gibt – unsicher) schwach.
- **Code-Auszug**:
```cpp
static UUID generate()
{
    static thread_local std::mt19937_64 rng{ std::random_device{}() };   // 64-Bit-Seed für 312×64-Bit-State
```
- **Lösung**: `generate()` nur deklarieren, in `UUID.cpp` (`HE_API`) mit denselben Bits implementieren (oder xoshiro/`splitmix` – dann ändert sich die Verteilung nicht beobachtbar); `<random>` raus. Hash auf `hi ^ (lo * 0x9E3779B97F4A7C15)` o.ä. – nur falls Kollisionen gemessen werden.
- **Erwarteter Effekt** (Schätzung): Kürzere Compile-Zeit in ~89 Übersetzungseinheiten; Laufzeit unverändert.
- **Risiko / Verifikation**: Linkabhängigkeit: UUID wird teils in Tools/Tests ohne HorizonCore verwendet? (unsicher) → vorher `grep` der Nutzer. Messen: Build-Zeit vorher/nachher.

### CORE-614 – `uiHcValueToProp`: verschachtelte Ternaries und eine dritte Kopie der Coercion-Regel
- **ID**: CORE-614
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/UIWidget/UIWidgetBinding.cpp:42-76` (Zeilen 60-65 die Ternaries)
- **Problem**: Die Float/Int/Bool-Zweige sind je 2 Zeilen dreifach verschachtelter `?:`; der Kommentar gibt selbst zu, dass es die „dritte Implementierung" der HorizonCode-Coercion-Regel ist (neben `HorizonCode.cpp` und `HorizonCodeGenSupport.h`). Schwer zu lesen, und ein neuer `PinType` (z.B. `Vec3`→Float) müsste an drei Stellen nachgezogen werden.
- **Code-Auszug**:
```cpp
case UIPropType::Float:  out.f = v.isArray ? v.f : (v.type == P::Int ? (float)v.i : (v.type == P::Bool ? (v.b ? 1.0f : 0.0f)
                                                 : (v.type == P::Double ? (float)v.d : v.f))); break;
```
- **Lösung**: Lesbar, ohne Verhaltensänderung: kleiner Helfer `double asNumber(const Value&)` (switch über `v.type`, `isArray → 0`-Sonderfall getrennt behandeln) bzw. pro Zielwert eine kurze `switch`-Funktion:
```cpp
static float toFloat(const Value& v) {
    if (v.isArray) return v.f;
    switch (v.type) { case P::Int: return (float)v.i; case P::Bool: return v.b ? 1.f : 0.f;
                      case P::Double: return (float)v.d; default: return v.f; }
}
```
  Nebenbei prüfen, ob die geteilte Regel aus `HorizonCode` (Header-only) direkt nutzbar ist.
- **Erwarteter Effekt**: Lesbarkeit; keine Laufzeitänderung.
- **Risiko / Verifikation**: Reihenfolge der Bool/Int/Double-Fälle exakt erhalten; Test: bestehender Binding-Test in `tests/test_ui_widgets.cpp` (Array-Passthrough, Int→Float) plus Tabelle aller Kombinationen.

### CORE-615 – `uiAnimEvaluate` / `UIAnimSample`: Heap-Kopien pro Track und Frame
- **ID**: CORE-615
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIWidgetAnim.cpp:200-235`; `UIWidgetAnim.h:121-126`; Aufrufer `WidgetManager.cpp:2382-2392`
- **Problem**: Je Track und Frame entsteht ein `UIAnimSample` mit kopiertem `std::string prop` (Property-Namen wie `"Inner Shadow Color"`, `"Hovered Color"` überschreiten SSO → Heap) und einer kopierten `UIPropValue` (String + `vector<string>` Member). `out` wird nicht `reserve`d. Zusammen mit CORE-603 (Namensauflösung pro Frame) wird jedes Sample zweimal per Namen aufgelöst.
- **Code-Auszug**:
```cpp
UIAnimSample s;
s.element = tr.element;
s.prop    = tr.prop;                 // String-Kopie je Track und Frame
...
out.push_back(std::move(s));
```
- **Lösung**: `UIAnimSample` speichert statt Kopie `const UIAnimTrack* track` (oder Trackindex) und `UIPropValue value`; `out.reserve(out.size() + clip.tracks.size())`. Konsumenten (`applyClipAt`, Designer-Overlay) lesen `track->prop`.
- **Erwarteter Effekt** (Schätzung): Pro Frame einige Heap-Allokationen weniger pro laufendem Clip; gering.
- **Risiko / Verifikation**: Lebensdauer des Zeigers (Clip darf während Evaluate nicht verändert werden – im Designer Scrub-Pfad prüfen). Tests: `tests/test_ui_widgets.cpp` (Evaluate-Randfälle: vor erster Key, zwei Keys gleiche Zeit, Typmix).

### CORE-616 – Theme: Amber-Palette mit duplizierten Literalen, lineare String-Suche in Styles, Substring-Allokationen in `tagsFor`
- **ID**: CORE-616
- **Kategorie**: A/F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `UITheme.cpp:210-248` (Rollen und danach dieselben 12 Farbpaare nochmal als Locals), `UITheme.h:154-170, 231-264`; `UITheme.cpp:129-142`
- **Problem**: In `uiAmberTheme()` stehen die Rollenfarben (`:210-221`) und direkt danach dieselben Werte als `surfaceL/D … accentL/D` (`:243-248`) – derselbe Zahlenwert zweimal, eine Änderung der Palette muss an zwei Stellen erfolgen. `UIThemeStyle::find/set` und `UITheme::styleFor/styleMut` suchen linear mit `std::string ==` (geordneter Vektor aus Absicht, `UITheme.h:149-152`); `tagsFor` zerlegt jeden Key mit `uiThemeSelectorTag`/`Type` in zwei neue `std::string`s (`:258-262`). Auflösung läuft nur bei Theme-/Moduswechsel und Widget-Erzeugung (nicht pro Frame, unsicher bei `uiThemeStylesFor` je Element), daher niedrig.
- **Code-Auszug**:
```cpp
set(UIThemeRole::Surface,    { 1.00f, 0.99f, 0.96f, 1.0f }, { 0.14f, 0.13f, 0.11f, 1.0f });
...
const glm::vec4 surfaceL{ 1.00f, 0.99f, 0.96f, 1.0f }, surfaceD{ 0.14f, 0.13f, 0.11f, 1.0f };
```
- **Lösung**: Locals aus `d.color[role][mode]` lesen (`const glm::vec4 surfaceL = d.color[(int)UIThemeRole::Surface][0]` …) oder zuerst Locals definieren und `set(...)` damit füttern. Für `tagsFor`: `std::string_view` für Typ/Tag (`find('.')` ohne Substring). Linearer Style-Lookup bei Bedarf später per `unordered_map` neben dem geordneten Vektor – nur wenn gemessen.
- **Erwarteter Effekt**: −12 duplizierte Zeilen, keine Drift der Palette; Laufzeit vernachlässigbar.
- **Risiko / Verifikation**: Ergebniswerte identisch halten; vorhandener `test_ui_theme.cpp` (Rollen pinnen, Kontrast) deckt es ab.

### CORE-617 – Kleinigkeiten SequenceJson: fragile Enum-Iteration, Pretty-Print großer Float-Arrays
- **ID**: CORE-617
- **Kategorie**: A/F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SequenceJson.cpp:31-39, 112-115, 164`
- **Problem**: `kindFromName` iteriert `0..static_cast<int>(SequenceTrackKind::Audio)` – ein neuer Track-Typ nach `Audio` wird beim Lesen stillschweigend verworfen, solange diese Konstante nicht mitwandert (der `switch` in `kindName` warnt dagegen per `-Wswitch`). `j.dump(2)` schreibt die `times`/`values`-Arrays einer Property-Spur mit je einer Zeile pro Float (nlohmann pretty-print); eine Spur mit tausenden Keys erzeugt entsprechend große Chunk-Texte (und `MCP`-Antworten).
- **Code-Auszug**:
```cpp
for (int i = 0; i <= static_cast<int>(SequenceTrackKind::Audio); ++i)   // „letzter Enumerator" von Hand
```
- **Lösung**: `kindFromName` über eine `constexpr`-Tabelle `{kind, name}` führen (dann `kindName` daraus ableiten, `static_assert` auf die Größe), Float-Arrays kompakt schreiben (z.B. eigenes Dump mit Arrays in einer Zeile oder `dump(2)` nur für Struktur). Letzteres ist optisch/Größen-Optimierung – messen, ob Sequenzen überhaupt groß werden.
- **Erwarteter Effekt**: Kein stilles Verwerfen neuer Track-Typen; kleinere Dateien (Schätzung, nur bei langen Spuren).
- **Risiko / Verifikation**: Datei-Byte-Identität für bestehende Sequences ändert sich bei kompaktem Dump (Diff-Rauschen in Git, Collab-Sync-Diffs); Test: Round-Trip `sequenceToJson → sequenceFromJson` bleibt identisch (vorhandene Sequence-Tests).

### CORE-618 – TabBox-Labels werden pro Frame neu aufgebaut und pro Render/Klick neu gemessen
- **ID**: CORE-618
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `UIElement.cpp:3033-3073, 3107-3108`; Aufbau in `UIWidgetTree.cpp:1239-1257` (`uiApplyAutoSize`)
- **Problem**: `uiApplyAutoSize` ersetzt pro Frame für jede TabBox `tabLabels` (`clear` + Schleife über *alle* Elemente → O(TabBoxen·N) mit `std::string`-Kopien). `tabLayout` misst in `render()` (und in `tabAtPoint` bei jedem Pointer-Event) alle Labels neu mit `measureUIText`. Bei einer TabBox mit 10 Tabs sind das pro Frame 10 Messungen + 10 Stringkopien.
- **Code-Auszug**:
```cpp
for (const std::string& label : labels)
{
    const float textW = label.empty() ? 0.0f : (f ? HE::measureUIText(*f, label, sizePx, 0.0f, opts) : ...).x;
```
- **Lösung**: Labels nur ersetzen, wenn sich Kindnamen geändert haben (Vergleich statt Neuaufbau) und die Breiten mit `(labels, sizePx, fontAtlasKey)`-Schlüssel im Element cachen (`mutable std::vector<float> m_tabW`). Zusammen mit dem Kindindex-Cache aus CORE-601 kann die O(N)-Kindschleife entfallen.
- **Erwarteter Effekt** (Schätzung): Gering; relevant nur bei vielen TabBoxen/großen Bäumen.
- **Risiko / Verifikation**: Cache-Schlüssel muss `fontScale` (über `sizePx`) und Font-Wechsel abdecken; Tests `tests/test_ui_widgets.cpp` (Tab-Klick auf zugeschnittene Labels).

---

## Cross-Modul-Hinweise

- `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1501-1518, 1522-1531` (`uiChildIndexOf`, `uiChildCountOf`, `uiElementEffectiveVisible`): Ursache von CORE-601; Cache/Index gehört in `UIWidgetTree` (Slice mit UIWidgetTree).
- `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:1239-1300` (`uiApplyAutoSize`): läuft pro Frame mit `dynamic_cast` je Element, `uiElementUnitScale` + `uiElementRect` je Element, TabBox-Labelaufbau O(TabBoxen·N); Kandidat für Dirty-Flags (nur bei geänderten Elementen/Canvas) – Zusammenhang zu CORE-602/618.
- `src/HE_Core/src/UIWidget/WidgetManager.cpp:2382-2392, 2565` (`applyClipAt`): löst jede Animations-Property pro Frame zweimal per Namen auf (vgl. CORE-603); Slot-Zeiger beim Start cachen.
- `src/HE_Core/src/UIWidget/WidgetManager.cpp:1323` (`menuItemForShortcut`): siehe CORE-611.
- `src/HE_Editor/UIEditorPanel.cpp:3398, 6365, 6687` und `McpToolsWidget.cpp:361,464,660,855`: rufen `allProperties()` (~50 `UIPropDesc` mit Strings) teils pro Frame bzw. je Element; nach CORE-603 als statische Tabelle ohne Kopie lieferbar.
- `src/HE_Editor/HcGraphHost.cpp:1249,1300,1694` und `HcEditorUtil.cpp:554-627`: `TypeRegistry::structs()/enums()` (Kopie + Sort) in Editor-Pfaden, evtl. pro Frame – siehe CORE-606.
- `src/HE_Core/src/HorizonCode/HorizonCode.cpp:1316-1344` und `HorizonCodeRuntime.cpp:817-824`: `getEnum/getStruct/makeDefaultValue` kopieren Definitionen bei Wert-Erzeugung; profitieren von CORE-606.
- `src/HE_Editor/UIEditorPanel.cpp:410`, `McpToolsType.cpp:216`, `TypeAssetPanel.cpp:411-413`: rufen `uiThemeFromJson`/`structFromJson` ohne erkennbaren try/catch – siehe CORE-605.

## Unsicher / Messung nötig

- CORE-601: tatsächlicher Anteil an der Frame-Zeit; Benchmark mit synthetischem Baum (z.B. 2000 Elemente unter Splitter+TabBox), Aufrufzahlen von `uiElementEffectiveVisible` pro Frame zählen.
- CORE-602: Kosten von `measureUIText` je Label (Profiler über `uiApplyAutoSize` in einer realen Widget-Szene); Verhältnis Labels/Frame.
- CORE-603: ob Animationen in realen Projekten viele Tracks pro Frame ausführen; sonst nur Vereinfachungsnutzen.
- CORE-606: ob `TypeRegistry::structs()/enums()` aus Panels pro Frame aufgerufen werden (Aufruferliste prüfen) und Größe typischer Projekte.
- CORE-605: welche Aufrufer die Loader in try/catch hüllen (ContentManager-Ladepfad für `CHUNK_STDF/ENDF`, `CHUNK_THEM` wurde nicht verfolgt).
- CORE-613: ob UUID-Hash-Kollisionen bei strukturierten UUIDs auftreten; Compile-Zeit-Effekt von `<random>` messen (`-ftime-trace`).
- CORE-617: reale Größe/Länge von Property-Spuren in Sequences.

## Gelesen

Alle 20 Dateien des Slice wurden vollständig gelesen (Sequence/SequenceJson.{h,cpp}, SlotMap.h, Types/{Defines,Enums,Handle,TypeRegistry,UUID}.{h,cpp}, UIWidget/{AppMenu.h, UIElement.{h,cpp} in Blöcken bis Zeile 4348, UIElements.h bis Zeile 2215, UIShortcut.{h,cpp}, UITheme.{h,cpp}, UIWidgetAnim.{h,cpp}, UIWidgetBinding.cpp}); Aufrufer in `UIWidgetTree.cpp`/`WidgetManager.cpp` nur gezielt (grep + ~40 Zeilen) zur Belegung der Funde gelesen.
