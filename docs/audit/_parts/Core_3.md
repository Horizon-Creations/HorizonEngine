# Core Slice 3 – Diagnostics, Events, GraphCommon, HorizonCode (Interpreter, Klassen-Auflösung, Compiled/GenSupport)

## Zusammenfassung

Der Slice ist insgesamt sehr sorgfältig kommentiert und fachlich konsistent (Semantik-Parität Interpreter/Codegen ist dokumentiert und bewusst). Die relevanten Probleme liegen im HorizonCode-Interpreter (`HorizonCode.cpp`, `HorizonCode.h`) und in der Klassen-Auflösung: Der Interpreter benutzt für jeden Knoten lineare Suchen über `links`/`nodes` und baut dabei pro Zugriff die komplette Pin-Signatur neu auf; `Value` ist 280 Byte groß und wird überall per Wert kopiert (gemessen mit g++ auf Linux: `sizeof(Value)=280`, `sizeof(Node)=336`, `sizeof(Variable)=528`, `sizeof(Context)=1128`); `resolveClass` parst und verschmilzt bei JEDEM Spawn die komplette Klassenkette neu (kein Cache, auch dann, wenn danach die kompilierte Klasse genommen wird). Diagnostics/Events/GraphCommon sind überwiegend unkritisch (Editor-/Ladepfade), mit einigen kleinen Fallstricken (Log-Makros `HE_LOG_ONCE`/`THROTTLE`, `EventBus::publish`-Snapshot, nicht-werfende Loader, die trotzdem werfen können).
Anzahl Funde: 3 Hoch, 11 Mittel, 6 Niedrig (20 gesamt). Alle Messwerte ohne Laufzeit-Profiling; Größenangaben sind echte `sizeof`-Messungen, Zeitgewinne sind Schätzungen.

---

### CORE-301 – Interpreter: lineare Suchen und Pin-Signatur-Neuaufbau bei jedem Knoten und jeder Eingabe
- **ID**: CORE-301
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:3822-3827` (`execLinkFrom`), `3934-3955` (`runExecChain`), `4430-4474` (`evalInput`, `inputLinked`), `4232-4234` (FunctionCall-Entry-Suche), `603-643` (`sigScratch`, `signatureCountsOf`, `dataPinDescOf`), `1198-1224` (`pinRanges`, `dataPinType`); `src/HE_Core/include/GraphCommon/GraphModel.h:44-57` (`findNodeById`)
- **Problem**: Pro ausgeführtem Exec-Knoten und pro gelesenem Daten-Eingang laufen mehrere O(Links)- bzw. O(Nodes)-Scans (`execLinkFrom`, `m_graph.findNode`, Schleife über alle `links` in `evalInput`/`inputLinked`) und mehrfach `signatureInto` (ein ~510 Zeilen langer `switch`, der vier `std::vector<PinDesc>` per Initializer-Liste neu beschreibt). Ein einziger `evalInput` ruft `pinRanges(n)`, `pinRanges(*src)` und `dataPinType(n,…)` auf = 3 komplette Signatur-Builds, bei ungebundenem Pin mit Container-Check 3 weitere. Der Kommentar in `GraphModel.h:41` („Graphs are small (tens of nodes)") gilt für den Editor, nicht für Event-Handler, die pro Frame (`Tick`) und pro Instanz laufen. Die Kosten wachsen quadratisch mit der Graphgröße: jeder Knoten × alle Links × (Signatur-Builds pro Treffer).
- **Code-Auszug**:
```cpp
Value Runner::evalInput(const Node& n, int dataInIndex, int depth)
{
    const PinRanges r = pinRanges(n);                 // Signatur-Build #1
    const int pin = r.dataIn0 + dataInIndex;
    for (const auto& l : m_graph.links)               // O(Links)
    {
        if (l.dstNode != n.id || l.dstPin != pin) continue;
        const Node* src = m_graph.findNode(l.srcNode); // O(Nodes)
        ...
        return coerce(evalData(*src, l.srcPin - pinRanges(*src).dataOut0, depth), // Build #2
                      dataPinType(n, true, dataInIndex));                           // Build #3
```
- **Lösung**:
  1. Pro Graph einmal (lazy, z. B. beim ersten `Runner`-Lauf oder beim Laden) einen unveränderlichen `RunIndex` bauen: `std::vector<const Node*> byId` (Ids sind kleine, nie wiederverwendete ints, `nextId` aufwärts → dichter Vektor oder `unordered_map<int,uint32_t>`), pro Node vorberechnete `PinRanges`, pro Node ein Array der Pin-Typen (`dataInTypes`, `dataOutTypes`, `isArray`), pro `(dstNode,dstPin)` den speisenden `Link*` und pro `(srcNode,execOutPin)` den Ziel-Link (z. B. sortierter `std::vector<Link>`-Index oder `unordered_map<uint64_t,int>`).
  2. `Runner` liest nur noch aus dem Index. Der Index lebt neben dem `Graph` (z. B. `std::shared_ptr<const GraphRunIndex>` in `Graph`, bei jeder Mutation `reset()`; der Editor mutiert über `addNode/removeNode/connect`, diese setzen ihn zurück).
  3. `signatureInto` bleibt für Editor/Codegen unverändert.
- **Erwarteter Effekt**: (Schätzung) pro Knoten-Ausführung von O(N+L) mit großer Konstante auf O(1); bei Graphen mit 50-200 Knoten pro Event ein Faktor 5-20 weniger CPU im Interpreter. Messen: `test_horizoncode_runtime.cpp` mit synthetischem 200-Knoten-Tick-Graph, 10 000 Instanzen.
- **Risiko / Verifikation**: Cache-Invalidierung ist die Gefahr (Graph wird in Editor, Collab-Sync `nodeFromJson`, `syncTypeSignatures` etc. mutiert; Runtime hält `levels` je Instanz). Daher Index nur im Runtime-Pfad bauen (nicht für editierbare Graphen) oder strikt über `Graph`-Mutatoren invalidieren. Verhalten bleibt bitgleich; absichern mit den vorhandenen Paritäts-/Trace-Tests (`test_horizoncode_runtime.cpp`, `test_hc_exec_trace.cpp`, `test_horizoncode_codegen.cpp`, `test_hc_breakpoint_play_path.cpp`) plus einem neuen Test „mehrere Links auf einen Eingang / Doppel-Ids nach Merge“.

---

### CORE-302 – `Value` ist 280 Byte groß und wird überall per Wert kopiert
- **ID**: CORE-302
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/HorizonCode/HorizonCode.h:93-130` (Struct), `src/HE_Core/src/HorizonCode/HorizonCode.cpp:3572-3575` (`coerce(Value v, …)`), `4442-4447`, `4489-4492`, `4895-4921`; `HorizonCode.h:1215-1266` (`std::function<Value(…)>` Rückgaben)
- **Problem**: `Value` enthält alle Varianten gleichzeitig (`f`, `d`, `b`, `i`, `v2`, `col`, `v3`, `v4`, `ref`, `tpos/trot/tscl`) plus drei `std::string` (`s`, `keyTypeName`, `typeName`) und zwei `std::vector<Value>` (`items`, `keys`). Gemessen: `sizeof(Value)==280`. Jede Skalar-Operation (`Value::ofFloat(…)`, `coerce`) initialisiert/kopiert/zerstört diese 280 Byte inklusive dreier Strings und zweier Vektoren. `coerce` nimmt `Value` per Wert: bei Lvalue-Aufrufen (`coerce(it->second, …)`, `coerce(m_eventArg, …)`, `coerce(v.items[i], …)`) wird tief kopiert, auch wenn `v.type == want` (dann wird die Kopie zurückgegeben). Ein `Add`-Knoten erzeugt drei Values (2× `evalInput`, 1× `ofFloat`) = ~840 Byte Initialisierung für eine Addition.
- **Code-Auszug**:
```cpp
Value coerce(Value v, PinType want)
{
    if (v.isArray) return v;
    if (v.type == want) return v;
    Value r; r.type = want;
```
- **Lösung**: Stufenweise, ohne Formatänderung:
  1. `coerce` auf `const Value& v` + Ausgabe umstellen (`Value coerce(const Value&, PinType)` und `Value coerce(Value&&, PinType)`-Überladung, die bei Typgleichheit verschiebt). Sofort-Gewinn bei Lvalue-Aufrufern, kein Semantikunterschied.
  2. Mittelfristig `Value` auf ein Tag + `union`/`std::variant`-artiges Layout für Skalare umbauen (float/double/bool/int/ref/vec2/vec3/vec4/transform teilen Speicher) und die selten genutzten Container-/Typnamen-Felder in einen lazy allozierten `std::unique_ptr<ContainerPayload>` auslagern. Das ändert die öffentliche Struktur (`v.f`, `v.col` …) und damit viele Aufrufer (HE_Scene `EngineApi`, UIWidgetBinding, Replikation) – nur mit Accessor-Migration machbar.
  3. Kurzfristig reicht oft: `evalInput`-Fast-Path für `type == want`, der `Value` direkt weiterreicht (siehe CORE-301).
- **Erwarteter Effekt**: (Schätzung) Schritt 1: weniger Kopien um 30-50 % in Container-/Struct-lastigen Graphen; Schritt 2: `sizeof(Value)` ~280 → ~48 Byte, Interpreter-Skalarpfad 2-4× schneller. Messen mit Mikro-Benchmark (1 Mio. `Add`-Auswertungen).
- **Risiko / Verifikation**: Schritt 1 niedrig (rein mechanisch). Schritt 2 hoch: `Value` wird in Replikation, Savegame, Editor-Watch, Codegen-Parität (`hc::raw/toValue`) und `UIWidgetBinding::uiHcValueToProp` feldweise gelesen; siehe `test_horizoncode_containers.cpp`, `test_horizoncode_types.cpp`, `test_horizoncode_compiled.cpp`, `test_hc_watch*.cpp`, Replikationstests. Als eigenes Teilprojekt behandeln.

---

### CORE-303 – `resolveClass` parst und verschmilzt bei jedem Spawn die ganze Klassenkette neu (auch für kompilierte Klassen)
- **ID**: CORE-303
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HcClassResolve.cpp:146-193` (`walk`), `196-224` (`resolveClass`), `306-316` (`contentLoader`); Aufrufer `src/HE_Scene/src/EntityHost.cpp:110-114`, `src/HE_Scene/src/PlayerHost.cpp:91`, `src/HE_Game/src/GameApplication.cpp:1190,1237`
- **Problem**: Jeder Spawn einer Klasse ruft `resolveClassAsset` auf. Das lädt pro Vorfahre den JSON-String (`json = a->graphJson;` Kopie), parst ihn per `fromJson` (inkl. aller Nachbearbeitungs-Pässe, siehe CORE-305/306), kopiert die Graphen mehrfach (`out.levels = a.graphsRootFirst;` Kopie, `out.graph = a.graphsRootFirst.front();` Kopie, `mergeDerivedInto(out.graph, a.graphsRootFirst[i])` nimmt `Graph derived` per Wert = weitere Kopie) und verschmilzt die Kette zu `out.graph`, das die Spawn-Aufrufer gar nicht benutzen (`addLevels(std::move(rc.levels), …)`; `graph` ist laut Header nur für Editor/Codegen). Bei kompilierten Klassen (`compiledClasses().create(assetPath)` → `addCompiled`) wird das gesamte Ergebnis außer `engineBase` und `chain` verworfen. Bei 100 gespawnten Gegnern derselben Klasse = 100 JSON-Parses + 100 Merges. Zusätzlich hält danach jede `Runtime::Inst` ihre eigene Kopie `std::vector<Graph> levels` (`HorizonCodeRuntime.h:651`).
- **Code-Auszug**:
```cpp
    out.levels = a.graphsRootFirst;                 // Kopie
    out.graph = a.graphsRootFirst.front();          // Kopie
    for (size_t i = 1; i < a.graphsRootFirst.size(); ++i)
        mergeDerivedInto(out.graph, a.graphsRootFirst[i]);  // Kopie (by value)
```
- **Lösung**:
  1. Cache `ContentManager`-seitig (oder in einem `ClassResolveCache` in HE_Core): Schlüssel Klassenpfad (+ Content-Version/Reload-Zähler der beteiligten Assets), Wert `std::shared_ptr<const ResolvedClass>`; Invalidierung bei `HorizonCodeClassAsset`-Reload/Änderung (im Editor bei jeder Graph-Bearbeitung/Play-Start).
  2. `Runtime::Inst::levels` auf `std::shared_ptr<const Graph>` (oder `shared_ptr<const std::vector<Graph>>`) umstellen, damit Instanzen nicht jeweils tief kopieren (Cross-Modul, Runtime ist Slice 4).
  3. Kurzfristig und lokal: `merged`-Graph lazy/optional machen (`resolveClass(key, load, bool wantMerged=false)` oder getrennte Funktion `resolveLevels`), `out.levels` erst nach dem Merge verschieben (`mergeDerivedInto(out.graph, std::move(...))` bzw. Parameter `const Graph&`), und in `EntityHost::bindClass` zuerst `compiledClasses().find(assetPath)` prüfen und `chain/engineBase` aus `CompiledInstance::classChain()/baseClassKey()` nehmen statt zu parsen.
- **Erwarteter Effekt**: (Schätzung) Spawn-Kosten pro Instanz von „JSON-Parse + Merge + 3 Graph-Kopien“ auf einen `shared_ptr`-Copy; bei Spawn-Wellen (Projektile, Gegner) sichtbare Frame-Spikes weg. Messen: Spawn von 1000 Instanzen einer abgeleiteten Klasse.
- **Risiko / Verifikation**: Cache-Invalidierung bei Hot-Reload/Editor-Änderung; geteilte `const Graph` darf nie mutiert werden (Runtime schreibt heute in `leaf()` – `HorizonCodeRuntime.h:656` hat nicht-const `leaf()`, prüfen, wer das nutzt). Tests: `test_hc_inheritance.cpp`, `test_horizoncode_runtime.cpp`, `test_horizoncode_compiled.cpp`; neuer Test „zwei Spawns derselben Klasse liefern gleiche Levels, Änderung des Assets invalidiert“. Der Wechsel „Chain aus Compiled statt aus Asset“ nur nach Paritätstest beider Quellen (die Doku nennt Chain als Teil der Kompilat-Metadaten).

---

### CORE-304 – Container-Knoten kopieren den ganzen Container für einen Lesezugriff; ForEach alloziert pro Iteration
- **ID**: CORE-304
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:4610-4619` (`ArrayGet`), `4665-4680` (`ArrayContains/IndexOf`), `4711-4716`, `4794-4809` (Set/Map-Lesen), `4326-4376` (ForEach*), `4132`, `4019-4049` (`unwindFrame`)
- **Problem**: Lesende Knoten holen den Container als `const Value arr = evalInput(…)`. Bei einem `GetVariable`-gespeisten Array bedeutet das: `m_ctx.getVariable(n.s)` liefert bereits eine Kopie (`std::function<Value(const std::string&)>`), `coerce(v, …)` dann ggf. eine zweite (Lvalue). Ein `ArrayGet` in einer Schleife über N Elemente kopiert also N-mal das ganze Array: O(N²) Value-Kopien à 280 Byte. `ForEach` kopiert das Array einmal (`const Value arr = evalInput`) und weist dann pro Iteration `m_execOutputs[n.id] = { arr.items[i], Value::ofInt((int)i) }` zu: Hash-Lookup + `std::vector<Value>`-Allokation mit zwei Values (Element wird erneut kopiert, ggf. Strings/verschachtelte Vektoren) pro Iteration.
- **Code-Auszug**:
```cpp
case T::ArrayGet:
{
    const Value arr = evalInput(n, 0, depth + 1);   // tiefe Kopie
    const int idx = evalInput(n, 1, depth + 1).i;
    if (idx >= 0 && idx < (int)arr.items.size()) return arr.items[idx];
...
for (size_t i = 0; i < arr.items.size(); ++i)
{
    m_execOutputs[n.id] = { arr.items[i], Value::ofInt((int)i) };  // Heap pro Iteration
```
- **Lösung**:
  1. Für `GetVariable`-Quellen einen Referenz-Pfad ergänzen: `const Value* Runner::peekInput(n, idx)` (liefert Zeiger auf Instanz-/Frame-Variable ohne Kopie, sonst `nullptr`), den `ArrayGet/Length/Contains/IndexOf`, `Set*/Map*`-Lesezugriffe zuerst probieren (die Context-Hooks brauchen dafür eine `const Value* getVariableRef(const std::string&)`-Variante).
  2. `ForEach*`: Element/Index in einen wiederverwendeten Slot schreiben (`std::vector<Value>& slot = m_execOutputs[n.id]; slot.resize(2); slot[0] = arr.items[i]; slot[1].i = i;` einmalig außerhalb der Schleife anlegen) – Allokation entfällt, Element-Kopie bleibt (Semantik: Body darf `arr` nicht beeinflussen).
- **Erwarteter Effekt**: (Schätzung) `ArrayGet`-Schleifen O(N²) → O(N); `ForEach` ohne Heap pro Iteration, ~2-3× schneller bei kleinen Elementen.
- **Risiko / Verifikation**: Aliasing: Zeiger auf Variable kann während der Auswertung ungültig werden (Subgraph schreibt Variable) – Zeiger nur direkt vor dem Lesen verwenden, nie über einen weiteren `evalInput` hinweg halten (`ArrayGet` liest Index zuerst). Tests: `test_horizoncode_containers.cpp` (Container-Semantik/Reihenfolge), Paritätstests gegen Codegen.

---

### CORE-305 – `fromJson`/`nodeFromJsonObj`: tiefe JSON-Kopien durch `value()`, `findNode` je Link, lineare Knotentyp-Suche
- **ID**: CORE-305
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:2243-2310` (`fromJson`), `1775-1782` (`nodeTypeFromStoredName`), `1934-2007` (`nodeFromJsonObj`), `2011-2115` (`variableFromJsonObj`)
- **Problem**: (a) `for (const auto& e : j.value("nodes", nlohmann::json::array()))` – `json::value(key, default)` liefert **per Wert**: das gesamte `nodes`-Array (und `links`, `variables`, `events`, `comments`) wird einmal tief kopiert, bevor iteriert wird. Dasselbe in `nodeFromJsonObj` (`e.value("params", json::array())`, `e.value("pos", …)`, `e.value("f", …)`, `e.value("xform", …)`, `e.value("pinDefaults", …)`: Kopie je Knoten). (b) Pro Link zwei `findNode` (O(L·N)) plus eine zweite Schleife für die CreateObject-Migration mit weiteren `findNode`. (c) `nodeTypeFromStoredName` iteriert pro Knoten über alle ~150 `NodeType`s, ruft jeweils den `nodeDisplayName`-`switch` und vergleicht `std::string == const char*`.
- **Code-Auszug**:
```cpp
for (const auto& e : j.value("nodes", nlohmann::json::array()))   // tiefe Kopie
...
bool nodeTypeFromStoredName(const std::string& stored, NodeType& out)
{
    for (NodeType t : nodeRegistry())
        if (stored == nodeDisplayName(t)) { out = t; return true; }
```
- **Lösung**: `if (auto it = j.find("nodes"); it != j.end() && it->is_array()) for (const auto& e : *it)` (kein Kopieren; Zugriffsmuster ist schon in `commentFromJsonObj` mit `e.find("pos")` vorhanden). `static const std::unordered_map<std::string_view, NodeType>` aus `nodeRegistry()` + `kLegacyNodeNames` einmalig aufbauen. Für Links einmal eine `unordered_set<int>` der vorhandenen Node-Ids bzw. ein sortierter Vektor erzeugen statt `findNode` je Link.
- **Erwarteter Effekt**: (Schätzung) Ladezeit eines 500-Knoten-Graphen ~2-3× kürzer, Allokationen deutlich weniger. Wirkt sich verstärkt aus, solange CORE-303 (Parse bei jedem Spawn) nicht gelöst ist.
- **Risiko / Verifikation**: Verhalten identisch (nur Lesepfad). Absichern mit Roundtrip-Tests (`test_horizoncode_types.cpp`, `test_horizoncode_reroute.cpp`, `test_hc_inheritance.cpp`) und einem Test, der ein Asset mit unbekanntem Knotentyp und mit altem Namen „Array Add“ lädt.

---

### CORE-306 – `fromJson`-Nachbearbeitung: mehrere quadratische/kubische Pässe über Nodes × Links
- **ID**: CORE-306
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:3242-3296` (`inferUserTypeNames`), `3416-3469` (`traceBody`/`assignSubgraphs`), `1547-1570` (`propagateRerouteTypes`), `2594-2608` (`syncFunctionSignatures`), `2701-2764` (`remapLinksFromSnapshot`)
- **Problem**: `inferUserTypeNames` iteriert bis zum Fixpunkt über alle Nodes, baut je Node `signatureOf(n)` (vier Vektor-Allokationen) und scannt für jeden Node alle Links, davon je Treffer `findNode` + `pinRanges(*peer)` + `dataPinDescOf` → O(Pässe·N·(L·N)). `assignSubgraphs` ruft für jeden Event- und Function-Knoten `traceBody` auf, das pro Stack-Element alle Links scannt und `pinRanges` aufbaut (O(E·N·L)). `propagateRerouteTypes` läuft bis zu `nodes.size()+1` Pässe über alle Links, je Link `findNode`×2 + `dataPinDescOf`. `syncFunctionSignatures` sucht für jeden Call/Return linear die Entry (O(N²)) und kopiert `params/results` auch wenn unverändert. All das läuft bei jedem `fromJson` (und, bis CORE-303 gelöst ist, bei jedem Spawn).
- **Code-Auszug**:
```cpp
for (Node& n : g.nodes)
{
    if (!n.typeName.empty()) continue;
    const NodeSig  mine = signatureOf(n);
    ...
    for (const Link& l : g.links)
    {
        ...
        const Node* peer = g.findNode(weAreSrc ? l.dstNode : l.srcNode);
```
- **Lösung**: Gemeinsamen, einmalig aufgebauten Hilfsindex innerhalb von `fromJson` verwenden (`id→Node*`, `Node-id→eingehende/ausgehende Link-Listen`, Funktionsname→Entry `unordered_map`). Frühausstieg: `inferUserTypeNames` nur für Nodes mit Enum/Struct-Pins (Typ-Whitelist) anstatt für jeden; `propagateRerouteTypes` komplett überspringen, wenn kein Reroute-Node existiert (`any_of`); `syncFunctionSignatures` Entry-Map statt Scan und Vergleich vor Zuweisung.
- **Erwarteter Effekt**: (Schätzung) für typische Graphen (<100 Nodes) 10-40 % der Ladezeit; für große Graphen (500+ Nodes) Faktor >5. Messen mit großem Asset.
- **Risiko / Verifikation**: Reihenfolge der Pässe und die Link-Remap-Semantik exakt bewahren (das Kommentarwerk beschreibt subtile Wechselwirkungen, z. B. „snapshot sonst doppelt remappen“). Tests: `test_horizoncode_types.cpp`, `test_horizoncode_reroute.cpp`, `test_hc_function_rename.cpp`, `test_hc_rename_scope.cpp`.

---

### CORE-307 – `Runner::fireEvent` scannt bei jedem Feuern alle Nodes; InputAction-Knoten allozieren Strings
- **ID**: CORE-307
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:3829-3861` (`fireEvent`), `1092-1107` (`inputActionEventNames`, `inputActionChainFor`); `HorizonCode.h:1468`
- **Problem**: `fireEvent(const std::string&, …)` läuft bei jedem Aufruf (z. B. jeden Frame `Tick`, jede Pointer-Bewegung) über alle Nodes. Für jeden `InputAction`-Knoten ruft es `inputActionChainFor` → `inputActionEventNames`, das bis zu zwei `std::string`s per `HE::inputEventPressed/Released(n.s)` zusammensetzt und einen `std::vector<std::string>` zurückgibt (mehrere Heap-Allokationen pro Knoten und Fire), nur um `names[i] == eventName` zu prüfen. Zusätzlich vergleicht jeder Event-Knoten `n.s != eventName` als String. Es gibt keinen Index „Eventname → Einstiegsknoten“, obwohl das Runtime-Layer Events sonst bereits interniert (`EventId`).
- **Code-Auszug**:
```cpp
for (const auto& n : m_graph.nodes)
{
    if (n.type == T::InputAction)
    {
        const int chain = inputActionChainFor(n, eventName);   // baut vector<string>
```
- **Lösung**: Einmal pro Graph (zusammen mit dem Index aus CORE-301) eine Tabelle `eventName/EventId → [(nodeId, execOutPin, elem)]` aufbauen, inkl. vorab berechneter Input-Action-Eventnamen. `fireEvent` schlägt nur noch nach. Wenn das Runtime-Layer schon `EventId` hat, `fireEvent(EventId, …)` ergänzen (Namensversion als Wrapper).
- **Erwarteter Effekt**: (Schätzung) `Tick` auf einer Instanz ohne passenden Handler wird O(1) statt O(N) mit Stringvergleichen; bei Input-Action-Graphen entfallen ~2 Heap-Allokationen pro Knoten und Fire.
- **Risiko / Verifikation**: Reihenfolge mehrerer gleichnamiger Handler und die Mehrfach-Entry-Semantik (`deliverSuspension` je Entry) müssen erhalten bleiben (Tabelle in Knoten-Reihenfolge). Tests: `test_hc_exec_trace.cpp`, `test_hc_breakpoint_play_path.cpp`, `test_horizoncode_shortcuts.cpp`.

---

### CORE-308 – `Context` (1128 Byte, ~35 `std::function`) wird pro Fire gebaut und per Wert weitergereicht
- **ID**: CORE-308
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/HorizonCode/HorizonCode.h:1213-1373` (`Context`), `1463` (`Runner(const Graph&, Context ctx)`), `HorizonCodeCompiled.h:343` (`bindContext(Context ctx)`); Aufrufer `HorizonCodeRuntime.cpp:1565,1633,1694,2087,2114` (Slice 4)
- **Problem**: `sizeof(Context)==1128` (g++/libstdc++, Linux; MSVC/libc++ ähnlich oder größer). Die Runtime erzeugt für jeden Fire/Resume/Inherited-Call per `makeContext(id, level)` einen frischen `Context` (jeder Eintrag ist ein Lambda mit `this`+`id`+`level`, meist >16 Byte → `std::function` allokiert auf dem Heap) und `Runner` nimmt ihn per Wert. Das ist pro Event pro Instanz Dutzende Heap-Allokationen, obwohl der Inhalt bis auf `traceInstance/traceKey/traceLevel` instanzkonstant ist.
- **Code-Auszug**:
```cpp
Runner::Runner(const Graph& graph, Context ctx) : m_graph(graph), m_ctx(std::move(ctx)) {}
...
Runner r(i->levels[lv], makeContext(id, (size_t)lv));   // HorizonCodeRuntime.cpp:1565
```
- **Lösung**: `Runner` hält eine `const Context&` (Runtime besitzt pro Instanz/Level einen langlebigen, einmal gebauten `Context`; die Lifetimes sind klar: der Runner lebt nur für die Dauer eines Aufrufs). Alternativ `Context` in eine kleine Schnittstelle (`struct IHost { virtual … }` mit einem Zeiger + `instanceId`) umbauen und die 35 `std::function` durch virtuelle Aufrufe ersetzen – spart Heap und Speicher, ändert aber die API (Tests/Codegen/GenSupport benutzen `c.getProperty(...)` etc.).
- **Erwarteter Effekt**: (Schätzung) pro Event 20-40 Heap-Allokationen weniger; relevante Entlastung bei vielen Instanzen mit Tick/Overlap-Events. Messen: Allokationszähler in `fireTick` über 10 000 Instanzen.
- **Risiko / Verifikation**: Lifetime der Lambdas (`this`, Instanz-Referenz) bei Instanz-Zerstörung während des Laufs; `Runner` kann `destroyObject` aufrufen → Context darf nicht mit dem Inst-Container verschwinden. Tests: `test_horizoncode_runtime.cpp`, `test_hc_on_changed.cpp`, `test_hc_bind_to.cpp`. Unsicher / messen: tatsächliche Zahl der Allokationen hängt von der Standardbibliothek-SBO ab.

---

### CORE-309 – GenSupport: Array-/Set-/Map-Operationen nehmen den Container per Wert (O(n) Kopie je Operation)
- **ID**: CORE-309
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/HorizonCode/HorizonCodeGenSupport.h:323-335` (`arrAdd/arrSet/arrInsert/arrRemove`), `399-410` (`setAdd/setRemove`), `457-473` (`mapSet/mapRemove`), `415` (`setToArray`), `488-499` (`mapKeys/mapValues`)
- **Problem**: Jede Operation nimmt den Container per Wert und gibt ihn per Wert zurück (Copy-Semantik des Interpreters, bewusst). Die generierte Schleife `for … arr = hc::arrAdd(arr, x)` kopiert dadurch pro Iteration den gesamten Vektor, wenn der Codegen kein `std::move` emittiert. Das macht aus einem Append-in-Schleife O(n²) – in genau dem Code, der laut Doku die Performance-Variante des Interpreters sein soll. Ob der Codegen schon `std::move` setzt, ist in diesem Slice nicht sichtbar (Codegen ist `HE_Tools/hc_codegen`).
- **Code-Auszug**:
```cpp
template <typename T> inline Array<T> arrAdd(Array<T> a, const Same<T>& v)
{ a.push_back(v); return a; }
```
- **Lösung**: Die Signaturen sind bereits „sink“-Parameter und damit korrekt für `hc::arrAdd(std::move(arr), x)`. Der Codegen sollte den letzten Lesezugriff auf die lokale Variable/Zwischenvariable erkennen und `std::move` emittieren (bzw. den Wert direkt in den temporären Ausdruck einsetzen). Hier im Header nichts ändern; optional `static_assert`/Kommentar „callers must move“.
- **Erwarteter Effekt**: (Schätzung) Append-/Set-Schleifen von O(n²) auf O(n) in generiertem Code.
- **Risiko / Verifikation**: Move von Variablen ändert die Semantik, wenn dieselbe Variable später noch gelesen wird (Interpreter „Copy-Semantik“): nur bei bewiesener letzter Verwendung. Absichern über die Paritätstests `test_horizoncode_codegen.cpp` / `test_horizoncode_compiled.cpp`.

---

### CORE-310 – GenSupport: namensbasierte Zugriffe konstruieren `std::string` und suchen linear
- **ID**: CORE-310
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/HorizonCode/HorizonCodeGenSupport.h:648-664` (`getProperty/setProperty/getVariableCtx/setVariableCtx`), `686-693` (`getExternal/setExternal/bindEvent/emitEvent`), `709-713` (`callExternal/callApi`), `859-899` (`findSlot/getVar/setVar/collectVarRefs`); `HorizonCode.h:1215-1266` (Context-Signaturen `const std::string&`)
- **Problem**: Alle `Context`-Callbacks verlangen `const std::string&`, die generierten Aufrufer reichen `const char*` (String-Literal). Jeder Aufruf konstruiert also ein temporäres `std::string` (Heap-Allokation bei Namen > SSO-Grenze, z. B. 15 Zeichen unter libstdc++/MSVC; Property-Namen wie `"backgroundColor"` liegen genau an der Grenze). `findSlot` vergleicht linear `name == s.name` für jeden `GetExternal/SetExternal`, jeden `getVariable`-Aufruf der Runtime und jeden GC-Scan. `collectVarRefs` ruft `s.get(self)` und baut dadurch für jede Ref-/Map-Variable eine vollständige `Value`-Kopie (inkl. kompletter Arrays), nur um Refs herauszulesen.
- **Code-Auszug**:
```cpp
inline Value getVariableCtx(const Context& c, const char* name)
{ return c.getVariable ? c.getVariable(name) : Value{}; }   // std::string-Temporary
...
inline const VarSlot* findSlot(const VarSlots& slots, const std::string& name)
{
    for (const VarSlot& s : slots) if (name == s.name) return &s;
```
- **Lösung**: In `Context` zusätzlich `std::string_view`-Varianten (oder interned `NameId`) anbieten; `GenSupport` ruft zuerst diese. `findSlot`: einmalig beim Aufbau (`concatSlots`) einen `unordered_map<std::string_view, uint32_t>` bzw. sortierten Vektor mit `string_view`-Suche halten. `collectVarRefs`: pro Slot einen typed `collectRefs`-Callback (`void(*)(const CompiledInstance*, std::vector<uint32_t>&)`) analog zu `get/set`, damit keine `Value`-Kopie nötig ist.
- **Erwarteter Effekt**: (Schätzung) pro Property-/Variablenzugriff in generiertem Code 1 Heap-Allokation weniger bei langen Namen; GC-Scan ohne Array-Kopien. Messen: Allokationszähler im Tick einer generierten Klasse.
- **Risiko / Verifikation**: API-Erweiterung additiv (alte Signaturen bleiben als Fallback, wie bei `emitEventId`). Tests: `test_horizoncode_compiled.cpp`, `test_horizoncode_codegen.cpp`.

---

### CORE-311 – `EventBus::publish` kopiert bei jedem Publish die Handlerliste (inkl. `std::function`s)
- **ID**: CORE-311
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Events/EventBus.h:81-92`, `61-78`
- **Problem**: `publish` macht `auto snapshot = it->second;` – eine Kopie des `std::vector<Entry>` samt jeder `std::function` (Heap-Allokation für Vektor und für jeden Handler mit Capture > SBO) bei jedem Publish, nur damit Handler während des Dispatch subscriben/unsubscriben dürfen. Das ist die öffentliche Gameplay-API („typed pub/sub bus for game code“), also potenziell pro Frame und pro Ereignis. Die Hash-Suche über `std::type_index` kommt dazu.
- **Code-Auszug**:
```cpp
// Copy list so handlers can safely subscribe/unsubscribe during dispatch.
auto snapshot = it->second;
for (auto& entry : snapshot)
    entry.fn(&event);
```
- **Lösung**: Per Index und Größe iterieren, Mutation während des Dispatch über einen `m_dispatchDepth`-Zähler abfangen: `subscribe` hängt hinten an (Größe am Start merken → neue Handler feuern erst beim nächsten Publish, wie heute), `unsubscribe` markiert den Eintrag (`id = 0`) und kompaktiert erst bei Tiefe 0. Handler als `std::shared_ptr<const std::function>` oder per `unique_ptr` stabil halten, wenn Reallokation während des Dispatch möglich bleibt. Beobachtbares Verhalten (neue Handler in diesem Publish nicht aufgerufen; zwischenzeitlich entfernte Handler werden – wie heute – noch aufgerufen, falls im Snapshot) beibehalten oder bewusst anpassen und dokumentieren.
- **Erwarteter Effekt**: (Schätzung) pro Publish 1 + #Handler Heap-Allokationen weniger.
- **Risiko / Verifikation**: Subtile Semantik „entfernter Handler im laufenden Dispatch wird noch gerufen“ (Snapshot) ändert sich bei der Markierungs-Variante. Der Header sagt, nur die Tests nutzen den Bus – dort (`tests/` EventBus-Test; per `grep EventBus tests` zu finden) Fälle „unsubscribe im Handler“, „subscribe im Handler“ prüfen.

---

### CORE-312 – Log-Wiederholungs-Makros: Zustand wird vor dem Filter verbraucht / Uhr wird vor dem Filter gelesen
- **ID**: CORE-312
- **Kategorie**: B
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Core/include/Diagnostics/Log.h:272-277` (`HE_LOG_ONCE`), `280-287` (`HE_LOG_EVERY_N`), `290-300` (`HE_LOG_THROTTLE`); Kopfkommentar `25-29`
- **Problem**: Der Kopfkommentar verspricht, dass bei ausgefiltertem Level nur „one predictable branch“ anfällt. Bei den Varianten stimmt das nicht: `HE_LOG_ONCE` setzt das Flag (`exchange(true)`) **vor** dem `enabled`-Test – wird die Kategorie später per `configureFromString`/Konsole hochgedreht, erscheint die Meldung nie mehr (Funktionsfehler). `HE_LOG_THROTTLE` ruft `monotonicMillis()` (exportierte Funktion, `steady_clock::now()`) bei jedem Aufruf, auch wenn die Kategorie ausgeschaltet ist; `EVERY_N` macht einen atomaren `fetch_add` pro Aufruf. Im Repo gibt es ~90 Verwendungen von `THROTTLE/EVERY_N`, viele vermutlich in Frame-Schleifen.
- **Code-Auszug**:
```cpp
#define HE_LOG_ONCE(cat, level, ...)
	do {
		static ::std::atomic<bool> heLogOnceFired_{false};
		if (!heLogOnceFired_.exchange(true, ::std::memory_order_relaxed))
			HE_LOG(cat, level, __VA_ARGS__);
	} while (0)
...
		const int64_t heNow_ = ::HE::Log::detail::monotonicMillis();
```
- **Lösung**: Reihenfolge umkehren: erst `if (::HE::Log::enabled(...))`, dann Once/Counter/Zeit prüfen (bei ONCE: `if (enabled && !exchange(true))`). Beobachtbares Verhalten bleibt für aktivierte Kategorien gleich; für deaktivierte entfällt Uhr/Atomic, und ONCE geht nicht mehr verloren.
- **Erwarteter Effekt**: (Schätzung) pro ausgefiltertem THROTTLE-Aufruf ~20-30 ns Uhr + Cross-DLL-Call gespart; Korrektheit von ONCE.
- **Risiko / Verifikation**: EVERY_N zählt künftig nur noch aktivierte Treffer (Trefferzahl in der Meldung ändert sich bei Zwischenzeitlich-Aktivierung) – akzeptabel, dokumentieren. `tests/test_logging.cpp` ergänzen: „ONCE bei Off, dann aktivieren, dann erneut“.

---

### CORE-313 – „Nicht-werfende“ Loader werfen bei Typfehlern doch (`value()`/`get<T>()`)
- **ID**: CORE-313
- **Kategorie**: B
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/include/GraphCommon/GraphJson.h:69-75` (`linkFromArray`), `19-28` (Kommentar „must not throw“); `src/HE_Core/src/HorizonCode/HorizonCode.cpp:1934-2007`, `2011-2115`, `1724-1755`; `src/HE_Core/src/Diagnostics/ProfilerCaptureFile.cpp:101-167`
- **Problem**: `parseGraphObject` parst ohne Exceptions und der Kommentar begründet das mit einer früheren Crash-Schleife. Danach greifen die Lader aber mit `e.value("id", 0)`, `j[0].get<int>()`, `p[0].get<float>()` zu: nlohmann wirft `type_error.302`, wenn der Schlüssel existiert, aber einen anderen Typ hat (z. B. `"id": "abc"` oder ein String statt Zahl in einem Link-Array). Eine beschädigte oder handeditierte `.hasset` kann so aus `fromJson` herauswerfen – genau der Fall, den die Header-Doku ausschließen will. `LoadedCapture::load` fängt nur das `operator>>` (Zeilen 63-72); alle `jf.value(…)`-Zugriffe danach sind ungeschützt.
- **Code-Auszug**:
```cpp
inline bool linkFromArray(const nlohmann::json& j, int& sn, int& sp, int& dn, int& dp)
{
    if (!j.is_array() || j.size() < 4) return false;
    sn = j[0].get<int>(); sp = j[1].get<int>();   // wirft bei String/null
```
- **Lösung**: Entweder (a) den gesamten Dokument-Decode in `try { … } catch (const nlohmann::json::exception&) { return false; }` fassen (eine Stelle je Loader: `fromJson`, `nodeFromJson`, `variableFromJson`, `LoadedCapture::load`), oder (b) kleine Helfer `getOr<T>(j, key, def)` mit `is_number`-Check einführen. (a) ist die einfachste Lösung und lässt alle Lesepfade unverändert.
- **Erwarteter Effekt**: Robustheit; keine Laufzeitänderung auf dem Gutpfad (try-Block kostenlos auf modernen ABIs).
- **Risiko / Verifikation**: Teil-geladenes Ergebnis darf nicht in `out` landen (heute wird in lokale `Graph g` gebaut und erst am Ende verschoben – bleibt so). Test: Graph-JSON mit `"id":"x"` und Link-Eintrag `["a",1,2,3]` darf nicht werfen, Rückgabe `false`/Eintrag übersprungen.

---

### CORE-314 – Capture-Laden: DOM + drei Sortierungen derselben Daten + String-Schlüssel je Sample
- **ID**: CORE-314
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Diagnostics/ProfilerCaptureFile.cpp:60-67`, `110-125`, `203-218`, `16-24`; `src/HE_Core/include/Diagnostics/ProfilerStats.h:94-103`, `143-155`, `206-234`, `238-260`
- **Problem**: (a) `in >> j` baut das komplette DOM der Capture-Datei im Speicher auf und die Strukturen werden danach ein zweites Mal gefüllt (Spitzenverbrauch ≈ 2× Dateigröße, mal nlohmann-Overhead). (b) Aus `deltas` werden `computePercentiles(deltas)`, `computeFpsLows(deltas)` und `findHitches(frames)` unabhängig berechnet – jedes kopiert und sortiert dieselben Frame-Zeiten neu. (c) `accumulateFrameScopes` konstruiert pro Scope-Sample `const std::string key(s.name ? s.name : "?");` (Heap bei Namen > 15 Zeichen) und schlägt danach zweimal in `unordered_map<std::string,…>` nach (`agg[key]`, `samples[key]`), plus `intern(js.value("n", std::string("?")))` je Sample (temporäres Default-`std::string`, Hash-Lookup). Bei großen Captures (zehntausende Frames × hunderte Scopes) sind das Millionen Allokationen und Hashes. Ob das Laden im UI-Thread blockiert, ist aus diesem Slice nicht ersichtlich (Aufrufer im Editor).
- **Code-Auszug**:
```cpp
const std::string key(s.name ? s.name : "?");
ScopeAggregate& a = agg[key];
...
samples[key].push_back(s.ms);
```
- **Lösung**: (b) `deltas` einmal sortieren und an `percentileOfSorted`/`FpsLows`/Median weiterreichen (Überladungen `...FromSorted`). (c) Schlüssel `std::string_view` auf `s.name` (statische bzw. internierte Strings, siehe Lifetime-Kommentar in `ProfilerCaptureFile.h`) verwenden, ein einziges Map-Lookup auf `struct { ScopeAggregate agg; std::vector<double> samples; }`. In `intern` `std::unordered_set<std::string>` statt `deque`+`map` (Knoten-stabil, kein doppeltes Halten der Strings). (a) Für sehr große Dateien SAX-Parser / `json::parse` mit Callback oder Warnung ab Größe X; nur nötig, wenn Messung Probleme zeigt.
- **Erwarteter Effekt**: (Schätzung) Ladezeit großer Captures 2-4× kürzer, Speicherspitze −30 %.
- **Risiko / Verifikation**: Ergebnisse müssen bitgleich bleiben (Sortier-Stabilität/Tie-Breaker `name` im Aggregat-Sort bleibt). `tests/test_profiler_stats.cpp`, `tests/test_profiler.cpp` (Dump→Load-Roundtrip). Unsicher / messen: Dateigrößen realer Captures.

---

### CORE-315 – `CompiledInstance`-Standard-Hooks bauen pro Aufruf `std::string` + 280-Byte-`Value` und gehen über `fireEvent` per Namen
- **ID**: CORE-315
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/HorizonCode/HorizonCodeCompiled.h:166-169`, `172-305`, `319-320`
- **Problem**: Die Default-Implementierungen aller Hooks lauten z. B. `virtual void onTick(float dt) { fireEvent("Tick", 0, Value::ofFloat(dt)); }`. Für Klassen, die nur *einen Teil* der Hooks überschreiben (laut Kommentar der Normalfall), erzeugt jeder nicht überschriebene Hook pro Aufruf ein `std::string` aus dem Literal und ein `Value` (280 Byte), ruft die virtuelle `fireEvent` – deren Standard ist ein No-Op (Zeile 319-320). Bei `onTick` ist das pro Frame und Instanz. Ob die Runtime bei bekanntem Fehlen eines Handlers den Hook gar nicht erst aufruft, liegt außerhalb des Slices.
- **Code-Auszug**:
```cpp
virtual void onTick(float dt) { fireEvent("Tick", 0, Value::ofFloat(dt)); }
virtual void fireEvent(const std::string& name, int elem, const Value& arg)
{ (void)name; (void)elem; (void)arg; }
```
- **Lösung**: Eine Methode `virtual bool handles(EventId)`/Bitmaske `handledHooks()` (vom Codegen generiert, ist bereits aus `eventInfos()` ableitbar) anbieten; die Runtime ruft `onTick` nur, wenn der Hook belegt ist. Alternativ `fireEvent(const char*, …)` mit Literal ohne `std::string`.
- **Erwarteter Effekt**: (Schätzung) pro Frame und Instanz ohne Tick-Handler ~50-100 ns gespart; bei 10 000 Instanzen ~0.5-1 ms.
- **Risiko / Verifikation**: Handgeschriebene `CompiledInstance`s (Tests) die nur `fireEvent` überschreiben dürfen weiter funktionieren → Default muss „alles handled“ sein. `test_horizoncode_compiled.cpp`.

---

### CORE-316 – Zu lange Funktionen und wiederholte Muster im Interpreter
- **ID**: CORE-316
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCode.cpp:89-601` (`signatureInto`, ~510 Zeilen), `4095-4428` (`execNode`, ~330), `4476-5078` (`evalData`, ~600); Wiederholungen: `4224-4226`, `4243-4245`, `4270-4272`, `4283-4284`, `4320-4322`, `4981-4983` (Argument-Auswertung), `3892-3902` vs. `4279-4287` (Frame-Aufbau), `4326-4376` + `4019-4049` (Loop-Varianten), `4692-4780` (`isArray=true; container=…; type=…`-Re-Stamping 8×), `4810-4842` (`MapKeys/MapValues/MapBreak`)
- **Problem**: Drei Riesen-Switches, die jeweils mehrere Verantwortlichkeiten mischen; sechs Kopien der Schleife „Parameter auswerten und coercen“; `callFunction` und `FunctionCall` bauen den `CallFrame` getrennt (nur in der Reihenfolge von Zeilen verschieden, Gefahr des Auseinanderlaufens, wie der Kommentar in `execNode` selbst andeutet); `ForEach/ForEachSet/ForEachMap` und `unwindFrame` enthalten dieselbe Schleife dreimal; die kleinen Makros `HC_SET_PIN/HC_MAP_PIN/HC_KEY_PIN` (Zeilen 334-387) sind lokal und korrekt `#undef`et, ließen sich aber als `auto`-Lambdas/`constexpr`-Funktionen ausdrücken.
- **Code-Auszug**:
```cpp
std::vector<Value> args(n.params.size());
for (size_t i = 0; i < n.params.size(); ++i)
    args[i] = coerce(evalInput(n, (int)i, depth + 1), n.params[i].type);   // 6× im File
```
- **Lösung**: Hilfsfunktionen `evalArgs(const Node&, int firstPin, const std::vector<FuncParam>&, int depth)`, `makeFrame(const Node& entry, std::vector<Value> args)`, `runLoop(n, items/keys, r, depth)` und `restamp(Value&, ContainerKind, PinType)`; `execNode`/`evalData` in Gruppen (Variablen, Container, Funktionen, Vektoren) mit je eigener Funktion aufteilen. Keine Verhaltensänderung.
- **Erwarteter Effekt**: Lesbarkeit/Wartbarkeit; weniger Divergenzrisiko zum Codegen. Keine Performance-Änderung zu erwarten.
- **Risiko / Verifikation**: Rein mechanisch; Auswertungsreihenfolge der Eingänge (Seiteneffekte, Trace) muss gleich bleiben – Tests: `test_hc_exec_trace.cpp`, `test_horizoncode_runtime.cpp`, Paritätstests.

---

### CORE-317 – `Logger::setSink`: Race bei zwei parallelen Installationen, komplizierter Handle-Tanz
- **ID**: CORE-317
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Diagnostics/Logger.cpp:25-77`
- **Problem**: `setSink` entscheidet unter dem Lock, ob ein Add nötig ist (`handleToAdd = 1`), führt `addSink` aber erst außerhalb des Locks aus und schreibt `g_legacyHandle` danach. Rufen zwei Threads gleichzeitig `setSink(non-null)` auf, sehen beide `g_legacyHandle == 0` und installieren zwei Bridges (doppelte Zustellung, ein Handle geht verloren). `handleToAdd` ist ein Flag mit dem Wert `1` statt `bool`. Der Aufwand (Mutex + zwei Handle-Variablen) steht in keinem Verhältnis zu dem, was nötig wäre.
- **Code-Auszug**:
```cpp
else if (sink && !g_legacyHandle)
{
    handleToAdd = 1;
}
...
const int h = HE::Log::addSink(&legacyBridge, nullptr);
std::lock_guard<std::mutex> lk(g_legacySinkMutex);
g_legacyHandle = h;
```
- **Lösung**: Die Bridge einmal dauerhaft registrieren (`static const int h = HE::Log::addSink(&legacyBridge, nullptr);` in `setSink`) und `g_legacySink`/`g_legacySinkUser` als ein `std::atomic<const Slot*>` bzw. unter dem bestehenden Mutex halten; `nullptr` setzt nur den Zeiger zurück. Keine `removeSink`-Aufrufe, keine Handle-Variablen. Die Bridge ist billig, wenn kein Sink gesetzt ist (ein atomarer Load).
- **Erwarteter Effekt**: Entfernt ein Race und ~15 Zeilen.
- **Risiko / Verifikation**: Bridge läuft dauerhaft mit (ein zusätzlicher Funktionsaufruf pro Log-Record unter dem Log-Mutex). Test: `tests/test_logging.cpp` – Sink setzen/entfernen/neu setzen, doppelte Zustellung ausschließen.

---

### CORE-318 – `Subscription` hält einen rohen `EventBus*` (Dangling, wenn der Bus zuerst stirbt)
- **ID**: CORE-318
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/include/Events/EventBus.h:22-47`, `141-152`
- **Problem**: `~Subscription()` ruft `release()` → `m_bus->unsubscribe(...)` auf einen eventuell bereits zerstörten Bus (Reihenfolge-Abhängigkeit bei Mitgliedern/Statics, z. B. Bus als Member des Spiel-Objekts, Subscription in Systemen, die länger leben). Das Verhalten ist in der Header-Doku nicht erwähnt. Die Move-Zuweisung ist selbstzuweisungs-unsicher (`x = std::move(x)` ruft `release()` und kopiert dann Nullzustand – harmlos, aber unbeabsichtigt).
- **Code-Auszug**:
```cpp
inline void Subscription::release()
{
    if (m_bus) { m_bus->unsubscribe(m_type, m_id); m_bus = nullptr; }
}
```
- **Lösung**: Entweder dokumentieren („Subscription darf den Bus nicht überleben“) oder `EventBus` mit einem `std::shared_ptr<bool> m_alive` ausstatten, den die Subscription als `weak_ptr` hält. Move-Zuweisung mit `if (this != &o)` absichern.
- **Erwarteter Effekt**: Vermeidet use-after-free in Abbau-Reihenfolgen; sonst keiner.
- **Risiko / Verifikation**: Gering. Test: Bus vor Subscription zerstören (mit ASan).

---

### CORE-319 – `HE::Log::enabled` ist ein exportierter Nicht-Inline-Aufruf pro Log-Stelle
- **ID**: CORE-319
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Diagnostics/Log.h:107`, `256-261`; `src/HE_Core/src/Diagnostics/Log.cpp:382-390`
- **Problem**: Jede `HE_LOG_*`-Stelle (≈ 114 TRACE/DEBUG-Stellen im Repo, dazu alle anderen) ruft `Log::enabled` als echten Funktionsaufruf über die DLL-Grenze (Windows: Import-Thunk) mit Bereichsprüfung, Funktions-Static-Guard (`verbosities()`) und zwei atomaren Loads. Der Header-Kommentar verspricht „one predictable branch“. Für ausgefiltertes Trace/Debug in Hot Paths ist ein Call + 2 Loads vermeidbar.
- **Code-Auszug**:
```cpp
bool enabled(Cat cat, Level level)
{
    const int i = static_cast<int>(cat);
    if (i < 0 || i >= kCatCount) return false;
    ...
```
- **Lösung**: Nur messen, wenn überhaupt. Bei Bedarf: eine einzige `HE_API const std::atomic<uint8_t>* effectiveThresholds()`-Adresse einmal pro TU cachen (`static const auto* p = ...;`) und im Makro inline `lv >= p[cat]` prüfen, wobei der effektive Schwellwert (`max(catVerbosity, globalFloor)`) bei jeder Änderung in `setVerbosity` vorberechnet wird. Der Kommentar „kein exportiertes Datum“ bleibt gewahrt, weil nur ein Zeiger geholt wird.
- **Erwarteter Effekt**: (Schätzung) wenige ns pro ausgefilterter Stelle – nur relevant bei dichten Trace-Logs in Schleifen; ohne Messung nicht umsetzen.
- **Risiko / Verifikation**: Niedrig. Unsicher / messen: Mikrobenchmark 100 Mio. ausgefilterte `HE_LOG_TRACE`.

---

### CORE-320 – Kleinkram: veraltete/verirrte Kommentare, ignorierte Rückgaben, kleine O(n²)-Stellen, Header-Gewicht
- **ID**: CORE-320
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: siehe Liste
- **Problem**: 
  1. `HorizonCode.h:962-976`: zwei Doku-Blöcke (zu `syncTypeSignatures` und `inferUserTypeNames`) stehen verwaist vor `engineEvents`, die zugehörigen Deklarationen sind erst in Zeilen 1116 und 1162.
  2. `HorizonCodeGenSupport.h:249` („variableDefaultValue :702“) und `676` („logs the fail like :1019“) verweisen auf Zeilennummern, die längst nicht mehr stimmen; Zeilennummern in Kommentaren verrotten.
  3. `HcClassResolve.cpp:176`: `if (!json.empty()) fromJson(json, g);` ignoriert den Rückgabewert; ein defektes Asset wird still zur leeren Klasse (kein Log).
  4. `HcClassResolve.cpp:94-99`: pro abgeleitetem Entry-Knoten Schleife über alle Basis-Knoten + je Treffer `removeMember` mit vollen `erase/remove_if`-Pässen über `nodes` und `links` (O(E·(B+N+L))); `overridableMembers/inheritedVariables/inheritedFunctions` prüfen Duplikate per `any_of` über `out` (O(n²)). Nur Lade-/Editor-Zeit, Größen klein.
  5. `ProfilerCaptureFile.cpp:183-197`: `std::any_of` über alle `threads` pro `summary.threads`-Eintrag (O(T²), T klein).
  6. `ProfilerCaptureFile.cpp:226-228`, `HcCompiledLoader.cpp:20-22,28-31,41-44,51-52`, `HorizonCodeGenSupport.cpp:29-31,41-43`: Muster `HE_LOG_*(cat, "%s", (a + b + c).c_str())` baut die Meldung per String-Konkatenation, obwohl das Log-System printf-Formatierung hat (`"… %s …", x.c_str()`); funktional gleich, aber unnötige Temporaries. Nur an Fehlerpfaden → Lesbarkeit.
  7. `HorizonCode.h:4`: `#include <glm/glm.hpp>` (der gesamte glm-Kern) in einem Header, den jede generierte Klasse über `HorizonCodeGenSupport.h` einbindet; `Value` braucht nur `vec2/3/4` (`glm/vec2.hpp`, `vec3.hpp`, `vec4.hpp`) → kürzere Compile-Zeiten.
  8. `Profiler.h:46-49`: unter `TRACY_ENABLE` expandieren `HE_PROFILE_SCOPE()`/`_N`/`_DYN` zu zwei Anweisungen ohne Klammerung (`ZoneScoped; HE_PROF_SCOPE_IMPL(...)`) – in `if (x) HE_PROFILE_SCOPE();` ohne Klammern wirkt nur die erste. Und `HE_PROFILE_FRAME()` ist ohne Tracy leer, aber ohne Semikolon-Absicherung (`((void)0)`).
  9. `GraphModel.h:36-71`: vier `findNodeById`-Überladungen (value/unique_ptr × const/non-const) plus vier `nodeAddr`-Überladungen; ließe sich mit einer einzigen Template-Funktion über `decltype(nodeAddr(*nodes.begin()))` als Rückgabetyp und `auto&&`-Parameter halbieren. Nur Vereinfachung, geringer Nutzen.
- **Code-Auszug**:
```cpp
if (!json.empty()) fromJson(json, g);   // Ergebnis ignoriert
```
- **Lösung**: Je Punkt wie beschrieben; Punkt 3 mit `HE_LOG_WARN(HorizonCode, "class '%s': graph JSON invalid", cur.c_str())`, 7 Includes verschlanken, 8 die Makros als `do {} while` geht nicht (Scope-Objekt) → Kommentar „nur als eigenständige Anweisung verwenden“.
- **Erwarteter Effekt**: Wartbarkeit, minimal kürzere Compile-Zeit (Punkt 7).
- **Risiko / Verifikation**: Gering; Punkt 3 ändert nur Logging; Punkt 7 per Build aller Targets prüfen (Aufrufer verlassen sich evtl. transitiv auf `glm.hpp`).

---

## Cross-Modul-Hinweise

- `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:76-77` (Slice 4): `Inst::levels` ist `std::vector<Graph>` je Instanz (tiefe Kopie, `HorizonCodeRuntime.h:651`) – mit CORE-303 auf geteilte, immutable Graphen umstellen.
- `src/HE_Scene/src/EntityHost.cpp:110-114`, `src/HE_Scene/src/PlayerHost.cpp:91`, `src/HE_Game/src/GameApplication.cpp:1190,1237`: `resolveClassAsset` pro Spawn auch dann, wenn danach `compiledClasses().create()` gewinnt.
- `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:1565,1633,1694,2087,2114`: `Runner` + `makeContext(...)` pro Event/Fire (CORE-308); Makro-erzeugte `fireOn*`-Funktionen (`~1712-1730`) übergeben den Eventnamen als `const char*` an `runEventOnLevel(const std::string&)` → temporäres `std::string` pro Aufruf.
- `src/HE_Core/include/Application/InputAssets.h:15-17`: `inputEventPressed/Released/Axis` geben `std::string` per Wert zurück und werden in `inputActionEventNames` pro Fire gebaut (CORE-307).
- `src/HE_Tools/hc_codegen` (Codegen): Move-Semantik für `hc::arrAdd/setAdd/mapSet` (CORE-309) und typisierte `collectRefs` je Slot (CORE-310) wären dort zu erzeugen.
- `UIWidgetBinding.cpp` (`uiHcValueToProp`): dritte Kopie der Coerce-Regel neben Interpreter und `hc::coerce*` – bewusst dokumentiert, aber bei jeder Typregel-Änderung dreifach zu pflegen. Eine gemeinsame, `constexpr`-fähige Konvertierungsmatrix (`scalarConvert(srcType, dstType)`) in HE_Core würde die drei Stellen verbinden.

## Unsicher / Messung nötig

- Tatsächlicher Anteil der Signatur-Neubauten (CORE-301) an der Interpreter-Laufzeit: Profil mit `HE_PROFILE_SCOPE` um `evalInput` / Instruments / perf.
- Gewinn von `Value`-Verschlankung (CORE-302 Stufe 2) vs. Migrationsaufwand: Mikro-Benchmark der `Add`-/`Branch`-Kette, 1 Mio. Läufe.
- Allokationen pro `makeContext`-Aufruf (CORE-308) hängen von der SBO-Größe der Standardbibliothek ab (libstdc++ 16 Byte, MSVC 16, libc++ 24).
- Ladezeit/Speicher großer Profiler-Captures und ob der Editor `LoadedCapture::load` im UI-Thread aufruft (CORE-314).
- Kosten von `Log::enabled` über die DLL-Grenze unter Windows (CORE-319).
- Ob `EventBus` in der Praxis in Frame-Schleifen publiziert wird (CORE-311) – im Repo nur Tests.

## Gelesen

Dateien vollständig gelesen: ja – alle 21 Dateien des Slice (Log.h, Logger.cpp/.h, Profiler.h, ProfilerCaptureFile.cpp/.h, ProfilerStats.h, EventBus.cpp/.h, GraphJson.h, GraphModel.h, HcClassResolve.cpp/.h, HcCompiledLoader.cpp/.h, HorizonCode.cpp (1-5080), HorizonCode.h (1-1551), HorizonCodeCompiled.cpp/.h, HorizonCodeGenSupport.cpp/.h); zusätzlich punktuell `Log.cpp:382-390,696-700`, `HorizonCodeRuntime.cpp` (Aufrufer), `EntityHost.cpp:98-130` und `sizeof`-Messung per g++ zur Verifikation.
