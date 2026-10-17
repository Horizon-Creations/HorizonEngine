# Scene Slice 7 – SceneSerializer, SceneSystems, ScriptApi, ScriptContext, Sequence*, Spline*, Terrain*

## Zusammenfassung

Der Slice (13 Dateien, ca. 10,6k Zeilen) ist insgesamt sauber und sehr gut kommentiert. Die Probleme konzentrieren sich auf drei Stellen. (1) Der Szenen-Serializer macht beim Speichern/Undo mehrfach tiefe JSON-Kopien, darunter Multi-MB-Base64-Strings des Terrains. Er parst beim Prefab-Sync dasselbe CBOR-Blob pro Instanz neu. Er hat mehrere quadratische Suchen über lineare `Binding`-Vektoren. (2) Die Lua-Brücke baut pro Aufruf einen schweren `HE::api::Ctx` (ca. 25 `std::function`-Member, drei davon kopiert) und allokiert `vector<Value>`. (3) Per-Frame-Systeme (`SequenceSystem::begin/apply`) iterieren ohne Dirty-Flag über alle `SkeletalMeshComponent` und allokieren pro Spieler/Frame. Die Terrain-Werkzeuge kopieren pro Pinselstrich ganze Felder (Smooth, Weightmap). Gefunden: 4 Hoch, 9 Mittel, 10 Niedrig. Alle Aussagen sind aus dem Code belegt. Messdaten gibt es nicht, Schätzungen sind als solche markiert.

---

### SCN-701 – buildSceneJson/buildSubtreeJson kopieren den gesamten JSON-Baum mehrfach (Undo-Snapshot)
- **ID**: SCN-701
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/SceneSerializer.cpp:1084-1093`, `:721`, `:2588-2597` (analog in `buildSubtreeJson`)
- **Problem**: Jede Entity-Ebene wird per Copy-Zuweisung statt per Move in den Baum gehängt. `comps` wird nach `eJson` kopiert, `eJson` nach `entities` kopiert, `entities` nach `scene` kopiert. Zusätzlich wird in `serializeComponents` `comps["terrain"] = tc;` kopiert. Ein Terrain mit `sculptHeightsB64` (1025² Floats = 4 MB, base64 ca. 5,6 MB), Layer-Weights und Wasser-Zellen wird dadurch bis zu vier Mal deep-kopiert (tc→comps, comps→eJson, eJson→entities, entities→scene). Bei 200k Entities kommen vier komplette Kopien aller nlohmann-Knoten hinzu (je Knoten ein `std::map`-Node plus Strings). `saveToMemory` ist der Undo-Pfad: `EditorUndo.cpp:10,42,84,118` ruft ihn pro Kommando auf. Auch `{ "points", pts }`/`{ "levels", levels }`/`{ "layers", ... }` in Initializer-Listen kopieren (nlohmann-Initializer-Lists kopieren immer).
- **Code-Auszug**:
```cpp
json comps = serializeComponents(registry, entity);
if (!comps.is_null())
    eJson["components"] = comps;          // Kopie
entities.push_back(eJson);                // Kopie
...
scene["entities"] = entities;             // Kopie des ganzen Arrays
...
comps["terrain"] = tc;                    // Kopie inkl. MB-Strings (Z.721)
```
- **Lösung**: Überall `std::move` setzen: `eJson["components"] = std::move(comps); entities.push_back(std::move(eJson)); scene["entities"] = std::move(entities); eJson["children"] = std::move(children); comps["terrain"] = std::move(tc);`. In `buildSubtreeJson` genauso (2588, 2593, 2595, 2597). Optional `entities.get_ref<json::array_t&>().reserve(view.size())`. Verhalten bleibt bit-identisch, weil nur Kopien durch Moves ersetzt werden.
- **Erwarteter Effekt** (Schätzung): Snapshot-Zeit und Spitzen-Speicher sinken bei großen Szenen um grob 30-60 % (Kopien dominieren neben dem CBOR-Encode). Bei Terrain-Szenen kommen ca. 20 MB weniger memcpy pro Snapshot dazu.
- **Risiko / Verifikation**: Sehr gering. `tests/test_scene_serializer*` (Round-Trip) plus ein Test, der die Bytes von `saveToMemory` vor/nach der Änderung vergleicht (Byte-Gleichheit des CBOR). Zeit messen mit `lastLoadTiming`/`HE_LOG_SLOW_SCOPE` an einer 100k-Entity-Szene.

### SCN-702 – syncPrefabInstances parst pro Instanz das CBOR neu und löst Bindings quadratisch auf (läuft bei jedem Speichern)
- **ID**: SCN-702
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:3650`, `:3734-3739`, `:3752`, `:3778`, `:3930-3931`, `:4030`, `:4116-4169`; Aufrufer `src/HE_Editor/EditorApplication.cpp:13604` (jeder Save), `13751/13858` (Open)
- **Problem**: `syncPrefabInstances` cached die Blob-Bytes pro Prefab-Asset (`blobs`), aber `syncPrefabInstance` ruft pro Instanz `json::from_cbor(assetBlob)` auf. 1000 Platzierungen desselben Prefabs bedeuten 1000 identische CBOR-Parses. Innerhalb einer Instanz gilt: `recordPresent(key)` ist eine lineare Suche über alle Records (aufgerufen pro Binding und pro Subtree-Entity), `hasBinding` ist `std::any_of` über `work.bindings` pro Record, und `instanceOf/templateOf` sind lineare Scans (`PrefabInstanceComponent.h:104-115`). Das ergibt O(R²) pro Instanz. Dazu kommt pro Record ein volles `serializeComponents(registry, e)` (ca. 47 `try_get` plus JSON-Aufbau) und `json merged = block;` (tiefe Kopie), auch wenn am Ende nichts zu tun ist (Kommentar Z.4026: „a sync that changes nothing also touches nothing"). Der „nichts hat sich geändert"-Normalfall zahlt also den vollen Preis.
- **Code-Auszug**:
```cpp
const json scene = json::from_cbor(assetBlob, /*strict=*/true, /*allow_exceptions=*/false); // pro Instanz
...
auto recordPresent = [&](const HE::UUID& key) {
    for (const json* r : records) if (entityKeyOf(*r) == key) return true; return false; };
...
const bool hasBinding = std::any_of(work.bindings.begin(), work.bindings.end(), ...);
const json iComps = fresh ? json::object() : serializeComponents(registry, e);
```
- **Lösung**: (1) Geparstes JSON pro Asset-Id in `syncPrefabInstances` cachen (`unordered_map<UUID, json>`) und eine `syncPrefabInstance(world, root, const json& scene, ...)`-Überladung nutzen; die bytes-Variante bleibt als Wrapper. (2) Pro Aufruf einmal `unordered_set<UUID> recordKeys` und `unordered_map<UUID, UUID>` für `templateEntity→instanceEntity` und umgekehrt aufbauen und `recordPresent`/`hasBinding`/`counterpartOf` darauf umstellen (Reihenfolge der Ergebnisse bleibt, da nur Lookups ersetzt werden). (3) Optional einen Content-Hash/Dirty-Check pro Instanz (Asset-Version + Override-Liste unverändert → überspringen); das ändert Verhalten nur dann, wenn Hand-Edits an der Instanz relevant wären, daher separat absichern.
- **Erwarteter Effekt** (Schätzung): Save-Zeit bei Szenen mit vielen Prefab-Instanzen sinkt von O(Instanzen × Blobgröße) auf O(Blobs + Instanzen × Records). Bei 1000 Instanzen mit je 50 Records vermutlich Faktor 5-20 für den Sync-Anteil des Speicherns. Messen.
- **Risiko / Verifikation**: Mittel, weil die Funktion komplex ist (Nested-Placements). Die vorhandenen Tests in `tests/test_scene_serializer*`/Prefab-Sync-Tests müssen unverändert grün bleiben. Zusätzlich ein Test „1000 Instanzen, Zeitbudget" und ein Vergleich `PrefabSyncReport` vorher/nachher.

### SCN-703 – Lua-Dispatch: schwerer `HE::api::Ctx` und zwei `vector<Value>` pro Aufruf
- **ID**: SCN-703
- **Kategorie**: C
- **Schweregrad**: Hoch (Hot Path Skripte; Einschränkung: ohne Messdaten)
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/ScriptContext.cpp:82-109` (`apiCtx`), `:834-861` (`lua_engine_dispatch`), `:142-170` (`setPosition/setRotation`); Ctx-Definition `include/HorizonScene/EngineApi.h:60-150`
- **Problem**: Jeder Lua-Aufruf in die Engine-API ruft `apiCtx(L)` auf. Der Ctx enthält ca. 25 `std::function`-Member (`requestQuit`, `createObject`, `destroyObject`, Window-/Tray-/Menu-Callbacks …). Davon werden `requestQuit`, `createObject` und `destroyObject` pro Aufruf aus `HostServices` kopiert (Heap-Allokation, wenn der Lambda-Capture größer als der SBO-Puffer ist). Die übrigen werden default-konstruiert und wieder zerstört. Dazu kommen 3 `lua_getfield(LUA_REGISTRYINDEX, "…")` mit String-Hash pro Aufruf (`getWorld/getPhysics/getContent`) plus `getQuit`. `lua_engine_dispatch` allokiert `args` (reserve) und `res` (`vector<Value>`; `Value` trägt `std::string s`, `typeName`, mehrere Vektoren, also ein dickes Objekt) pro Aufruf. Skripte, die pro Frame viele `horizon.transform.*`/`math.*`-Aufrufe machen, zahlen das alles pro Aufruf.
- **Code-Auszug**:
```cpp
HE::api::Ctx c{};  c.world = getWorld(L); ...
c.createObject  = hs.createObject;      // std::function-Kopie
c.destroyObject = hs.destroyObject;     // std::function-Kopie
if (const std::function<void()>* quit = getQuit(L)) c.requestQuit = *quit;   // Kopie
...
std::vector<HorizonCode::Value> args; args.reserve(fn->params.size());
const std::vector<HorizonCode::Value> res = fn->invoke(c, args);
```
- **Lösung**: (a) Den Ctx pro `ScriptContext` einmal vorbauen und nur die veränderlichen Zeiger (`world/physics/content`) nachziehen. Die Registry-Slots kosten dann nichts, und der Cache wird in `setPhysicsWorld/setContentManager/setHostServices/setQuitHandler` invalidiert; Lua holt ihn per `lua_touserdata` aus einem Upvalue des Closures statt aus der Registry. Oder `Ctx` bekommt `const Services*` statt der Callback-Member. (b) `args`/`res` in einen kleinen Stack-Puffer (`absl::InlinedVector`-artig, `std::array` bis 8) oder als `thread_local` wiederverwendeten Vektor halten. (c) `lua_horizon_setPosition/setRotation` (Z.142-170) bauen den Ctx ebenfalls pro Aufruf; sie profitieren vom Cache.
- **Erwarteter Effekt** (Schätzung): pro Aufruf einige 100 ns bis ca. 1 µs, plus 0-3 Heap-Allokationen. Bei 5000 Calls/Frame grob 1-5 ms. **Messen**: Mikro-Benchmark `horizon.transform.setPosition` in Schleife.
- **Risiko / Verifikation**: Mittel. Lebensdauer der Services (Dangling nach Scene-Wechsel, siehe Kommentar Z.1385-1392) muss erhalten bleiben. Cache bei `~ScriptContext`/`setHostServices` invalidieren. Tests: Lua-Script-Tests plus Test „Quit-Handler nach `setQuitHandler` sichtbar".

### SCN-704 – collectAssetRefs(world, onlyEntities) durchsucht für jeden Slice die ganze Welt
- **ID**: SCN-704
- **Kategorie**: D
- **Schweregrad**: Hoch (bei Weltstreaming), sonst Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/SceneSystems.cpp:244-319`; Aufrufer `src/HE_Game/src/GameApplication.cpp:1504-1505`
- **Problem**: Die Teilmengen-Variante läuft über alle ca. 20 Component-Views der gesamten Welt und filtert erst danach per `keep(e)` (Hash-Lookup in einem frisch gebauten `unordered_set<uint32_t>`). Wird sie pro Streaming-Slice (`onlyEntities`) aufgerufen, ist der Aufwand O(Slices × Weltgröße) statt O(Slices × Slicegröße). Zusätzlich wird für jeden Slice ein neues `unordered_set` aufgebaut und `seen` neu allokiert.
- **Code-Auszug**:
```cpp
for (auto [e, c] : reg.view<MeshComponent>().each())  if (keep(e)) add(c.meshAssetId);
...  // 20 weitere Views, alle über die ganze Welt
const std::unordered_set<uint32_t> only(onlyEntities.begin(), onlyEntities.end());
```
- **Lösung**: Für die Teilmenge direkt über `onlyEntities` iterieren und pro Entity `reg.try_get<T>(e)` für jeden Typ aufrufen (O(|only| × 20)). Die Welt-Variante bleibt wie sie ist. Dafür die Typ-Liste einmal als generische Funktion `forEachRef(const Entity&, add)` ausdrücken (z. B. ein Template `visitRefs(reg, e, add)`) und für beide Varianten wiederverwenden; das ersetzt die 20 Zeilen Views durch eine Typliste. Reihenfolge der Ergebnisse (first-seen) ändert sich bei der Teilmengen-Variante (View-Reihenfolge vs. Slice-Reihenfolge). Prüfen, ob Aufrufer auf Reihenfolge angewiesen sind (vermutlich nicht, es ist ein Set).
- **Erwarteter Effekt** (Schätzung): bei Zellstreaming mit n Slices Faktor Weltgröße/Slicegröße, bei 200k Entities und 1000er Slices bis zu zwei Größenordnungen weniger Arbeit beim Nachladen.
- **Risiko / Verifikation**: Gering/Mittel (Reihenfolge). Test: gleiche Menge (nicht Reihenfolge) für Teilmenge und Voll-Welt vergleichen; vorhandene `collectAssetRefs`-Tests.

### SCN-705 – Laden: `c.value("key", json())` kopiert pro Feld ein JSON (Heap pro Entity)
- **ID**: SCN-705
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:1168-1170` (transform), `:1186`, `:1196`, `:1258-1260`, `:1290`, `:1307`, `:2012`, `:2154-2156`, `:2169-2186`, `:1856` (`c.value("layers", json::array())`), u. a.
- **Problem**: `json::value(key, json())` liefert das JSON **by value**. Bei einem Array wie `position` wird dadurch das ganze Array (3 Knoten plus Vektor-Allokation) kopiert, nur um es sofort in `jsonToVec3` zu zerlegen und zu verwerfen. Bei jeder Entity mit Transform sind das 3 tiefe Kopien. Material, Mesh, Light, UI usw. kommen dazu. Das ist reine Heap-Arbeit pro Entity beim Szenen-Load, bei Undo-Restore und beim Streaming (`applyAdditiveJson`).
- **Code-Auszug**:
```cpp
t.position = jsonToVec3(c.value("position", json()), t.position);
...
m.meshAssetId = jsonToUuid(c.value("asset", json()));
```
- **Lösung**: Hilfsfunktion `const json& field(const json& obj, const char* key)` mit `static const json kNull`, gibt `obj.find(key)` oder `kNull` zurück (keine Kopie). `jsonToVec3/jsonToUuid/...` nehmen bereits `const json&`. Mechanisch ersetzen: `jsonToVec3(field(c,"position"), t.position)`. Für das `layers`-Array `c.find("layers")` verwenden. Semantik identisch: nicht-Array → Fallback.
- **Erwarteter Effekt** (Schätzung): pro Entity 5-15 Heap-Allokationen weniger. Beim Laden großer Szenen einige Prozent der Build-Zeit (`buildMs`). Nach SCN-706 und SCN-701 messen.
- **Risiko / Verifikation**: Gering. Round-Trip-Tests plus Alt-Format-Tests (der Header `HAsset.h`/CLAUDE.md verlangt Test für alte Layouts).

### SCN-706 – applyComponents: bis zu 47 `contains()`-Lookups in `std::map` pro Entity
- **ID**: SCN-706
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance + Vereinfachung
- **Ort**: `SceneSerializer.cpp:1161-2192`, `:1117-1131`, `:2744-2836`
- **Problem**: `applyComponents` ist eine 1030 Zeilen lange Kette von `if (comps.contains("x")) { ... comps["x"] ... }`. Jede Entity macht ca. 47 String-Lookups (`contains`) in einer `std::map<string,json>`, und jedes Treffer-Feld macht erneut `comps["x"]` (zweiter Lookup). `serializeComponents` ist spiegelbildlich 47 `try_get`. Zusätzlich gibt es drei hand-gepflegte Schlüssellisten (`isKnownComponentKey`, `HE_SCENE_COMPONENT_KEYS`, die Ketten selbst). Der Kommentar bei 3011-3023 benennt die Drift-Probleme selbst.
- **Lösung**: Statt 47 Lookups: über `comps.items()` iterieren und per statischer `unordered_map<string_view, void(*)(registry&, Entity, const json&)>` dispatchen (eine Funktion pro Komponente, aus dem bestehenden Block herausgelöst). Die Reihenfolge der Anwendung muss gleich bleiben (z. B. `terrain` hängt von nichts ab, aber `prefab`/`navmesh` evtl.); sonst feste Reihenfolge per Tabelle. Danach kann `isKnownComponentKey` aus derselben Tabelle abgeleitet werden. Das ist ein größerer Umbau, daher nur mit Tests empfohlen.
- **Erwarteter Effekt** (Schätzung): kleiner als SCN-705; vor allem Wartbarkeit (eine Tabelle statt drei Listen) und einige Prozent Load-Zeit.
- **Risiko / Verifikation**: Mittel (Reihenfolge, `warnUnknownComponents`). `test_scene_serializer` deckt „jeder Key" ab.

### SCN-707 – applyPrefabJson: Stale-Bereinigung O(n·m) mit linearem `std::find`
- **ID**: SCN-707
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:2706-2723`
- **Problem**: `remove_if` über alle `children` der Welt-Root (Kommentar bei 2400: „it can hold a hundred thousand"), und für jedes Kind ein `std::find` in `stale`. Bei einem Prefab mit 2000 Entities in einer Szene mit 100k Root-Kindern sind das ca. 2·10⁸ Vergleiche pro Instanziierung (Kopieren/Einfügen, Undo-Redo, Collab). `applyAdditiveJson` löst dasselbe Problem schon sauber über den Tail der Liste (2463-2482).
- **Code-Auszug**:
```cpp
rc.erase(std::remove_if(rc.begin(), rc.end(), [&stale](Entity e)
         { return std::find(stale.begin(), stale.end(), e) != stale.end(); }), rc.end());
```
- **Lösung**: Entweder wie im Additiv-Pfad nur den Tail der Root-Liste ab `rootChildrenBefore` (vor der Schleife merken) betrachten, oder `stale` in ein `unordered_set<Entity>` legen (O(n+m)). Ergebnis (erhaltene Reihenfolge) bleibt identisch.
- **Erwarteter Effekt** (Schätzung): von O(n·m) auf O(n+m). Bei großen Szenen von Sekunden-Bruchteilen auf <1 ms.
- **Risiko / Verifikation**: Gering. Prefab-Instanziierungstests, plus Test „großes Root-Children + 1000er Prefab".

### SCN-708 – prefabInstancesBinding / addedHereUnder: O(Entities × Instanzen × Bindings)
- **ID**: SCN-708
- **Kategorie**: D
- **Schweregrad**: Mittel (Editor, Outliner-Rebuild)
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:4252-4263`, `:4325-4342`, `:4382-4389`; Aufrufer `src/HE_Editor/OutlinerPanel.cpp:670-674`; Datenstruktur `Components/PrefabInstanceComponent.h:104-115`
- **Problem**: `prefabInstancesBinding(world, entity)` iteriert alle `PrefabInstanceComponent`s der Welt und ruft jeweils das lineare `templateOf` über deren `bindings` auf. `addedHereUnder` ruft das für jedes Kind jedes Instanz-Roots auf, `prefabAddedHereCount` und `prefabRemovedHereCount` laufen zusätzlich pro Outliner-Knoten einer Instanz. Das ist O(E_unter_Instanzen × I × B) beim Outliner-Rebuild (Kommentar im Outliner: er läuft bei Strukturänderung). `prefabRemovedHereCount` ruft zudem pro Binding `findByEntityId`.
- **Lösung**: Pro Rebuild einmal einen Reverse-Index `unordered_map<UUID instanceEntity, small_vector<Entity root>>` aus allen `PrefabInstanceComponent`s aufbauen (O(I·B)) und `prefabInstancesBinding` darüber beantworten; an der Outliner-Stelle als Parameter durchreichen (API-Erweiterung `PrefabBindingIndex`). Die vorhandenen Einzel-Funktionen bleiben als Wrapper bestehen.
- **Erwarteter Effekt** (Schätzung): Outliner-Rebuild in Szenen mit vielen Prefab-Instanzen von quadratisch auf linear. Messen mit 500 Instanzen × 50 Kindern.
- **Risiko / Verifikation**: Gering. Outliner-UI-Tests (`tests/test_outliner_ui.cpp`) plus ein Test mit dem Index gegen die alte Funktion.

### SCN-709 – ScriptContext::destroyInstance durchsucht die gesamte Cooldown-Map
- **ID**: SCN-709
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/ScriptContext.cpp:1537-1544`, Struktur `include/HorizonScene/ScriptContext.h:351-364`
- **Problem**: Jedes `destroyInstance` iteriert die ganze `m_callbackReportNext`-Map, um die Einträge dieser Instanz zu entfernen. Beim Szenen-Teardown oder Massen-Despawn (n Instanzen) ist das O(n·m), und die Map enthält pro fehlerhaftem Callback einen Eintrag. Normalerweise ist die Map klein (nur fehlschlagende Callbacks), im Fehlerfall aber (ein defektes `onUpdate` auf 5000 Instanzen) wird sie groß.
- **Code-Auszug**:
```cpp
for (auto it = m_callbackReportNext.begin(); it != m_callbackReportNext.end(); )
    it = (it->first.id == id) ? m_callbackReportNext.erase(it) : std::next(it);
```
- **Lösung**: Early-out `if (m_callbackReportNext.empty()) return;` (billig, deckt den Normalfall ab), und für den Fehlerfall zweistufig `unordered_map<InstanceId, small array of (cb, next)>` oder die Map nach `id` gruppieren. Die einfache Variante (Early-out) reicht vermutlich.
- **Erwarteter Effekt**: Normalfall null Kosten; Fehlerfall O(1) pro Instanz statt O(m).
- **Risiko / Verifikation**: Gering. Test: Destroy mit Report-Cooldown, danach neu fehlschlagen → wird sofort gemeldet.

### SCN-710 – SequenceSystem::begin löscht `sequencePosed` auf allen Skeletal-Meshes, jeden Frame, auch ohne Sequenz
- **ID**: SCN-710
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/SequenceSystem.cpp:563-567`, `:699`, `:716-729`; Aufrufer `SceneSystems.cpp:222`
- **Problem**: `begin()` läuft über **alle** `SkeletalMeshComponent` (typisch einige hundert bis tausend Charaktere), um ein Bool zu löschen, bevor es auf `if (!ctx) return;` bzw. auf „keine SequencePlayer in der Szene" prüft. Das ist eine View ohne Dirty-Flag über die dichte Komponenten-Liste, und `sequencePosed` ist nur in Szenen mit laufender Sequenz überhaupt je `true`. Danach iteriert `begin` über alle `SequencePlayerComponent`, `apply` nochmal (`:700`), und `locksPlayerInput` (`:765`) pro Aufruf wieder.
- **Code-Auszug**:
```cpp
for (auto [e, smc] : reg.view<SkeletalMeshComponent>().each()) smc.sequencePosed = false;
if (!ctx) return;
```
- **Lösung**: Flag „es wurde im letzten Frame etwas geclaimt" als Zähler in `reg.ctx()` oder als `bool` im `SequenceContext` halten und die Löschschleife nur ausführen, wenn der Zähler > 0 war (nach dem Löschen auf 0 setzen, beim Claim in `begin` inkrementieren). Alternativ: kein Löschen, sondern ein Frame-Stempel (`uint32_t sequencePosedFrame`), den die anderen Treiber gegen den aktuellen Frame vergleichen. Das erfordert Änderungen in den drei Skeletal-Treibern (außerhalb dieses Slices). Die Zähler-Variante ist lokal und verhaltensgleich.
- **Erwarteter Effekt** (Schätzung): O(#SkeletalMesh) pro Frame → O(1) in Szenen ohne Sequenzen. Bei 1000 Charakteren grob 10-30 µs/Frame (Cache-Misse auf großen Komponenten). **Messen**.
- **Risiko / Verifikation**: Gering. Sequenz-Tests (`tests/test_sequence*`), insbesondere „Claim wird beim Stop gelöscht".

### SCN-711 – SequenceSystem: Heap-Allokationen pro Spieler und Frame
- **ID**: SCN-711
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `SequenceSystem.cpp:104-150` (`startSounds`), `:712` + `SequenceEval.cpp:73-102` (`evaluate`), `:660-662` + `:716-729` (`skeletalPoseAt` doppelt)
- **Problem**: (a) `startSounds` baut pro Audio-Track und Frame einen `starts`-Vektor mit `std::to_string(i)` (je ein String, ggf. Heap) und parst die Indizes per `std::stoul` zurück. Eine Notify-Namens-Rundreise nur, um den Index zu transportieren; `HE::NotifyQueue fired` und `starts` werden pro Aufruf neu angelegt. (b) `SequenceEval::evaluate` gibt ein `Result` mit `std::vector<PropertyWrite> writes` zurück, das pro Spieler und Frame wächst (push_back ohne reserve). (c) `skeletalPoseAt` (Slot-Lookup, `try_get`, `getAnimationClip`, `getSkeletalMesh`, `activeSection`-Scan) läuft pro Skeletal-Track doppelt (begin + apply). (d) `fireEvents`/`startSounds` laufen jeden Frame für alle Tracks, auch wenn der Span `[tPrev,tEnd]` keine Events enthält (`collectNotifySpan` entscheidet das erst nach dem Aufbau).
- **Code-Auszug**:
```cpp
for (size_t i = 0; i < tr.audio.size(); ++i)
    starts.push_back({ std::to_string(i), tr.audio[i].start, 0.0f });
...
const SequenceAudioSection& s = tr.audio[std::stoul(ev.name)];
```
- **Lösung**: (a) Für Audio-Sektionen direkt die Span-Regel auf `tr.audio[i].start` anwenden (eine kleine `spanContains(start, seq.duration, tPrev, tEnd, includeStart)`-Funktion, die `collectNotifySpan` bereits intern nutzt), ohne Namen-Rundreise. (b) `evaluate` bekommt einen `Result&`-Out-Parameter (`thread_local`/Member, `writes.clear()` statt neu allokieren). (c) Das Ergebnis von `skeletalPoseAt` aus `begin` in einem Scratch-Vektor pro Spieler für `apply` mitgeben. (d) Früh aussteigen, wenn der Track keine Events im Zeitfenster hat.
- **Erwarteter Effekt** (Schätzung): 3-10 Heap-Allokationen pro Spieler/Frame weniger, solange eine Sequenz spielt. Eher klein pro Frame, aber im Cutscene-Frame (viele Tracks) relevant.
- **Risiko / Verifikation**: Mittel für (a), weil die Span-Regel (Laps, geschlossener Ursprung) exakt gleich bleiben muss: Tests mit rückwärts/loop/Lap-Fällen aus den Sequenz-Tests übernehmen und gegen die alte Implementierung als Referenz vergleichen.

### SCN-712 – TerrainSculpt::apply (Smooth) kopiert das gesamte Höhenfeld pro Pinselstich
- **ID**: SCN-712
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/TerrainSculpt.cpp:243-246`, `:274-290`
- **Problem**: `before` ist eine Kopie von `tc.sculptHeights`. Bei Auflösung 1025 sind das 4 MB Allokation plus Kopie **pro Dab**, obwohl der Pinsel nur ein kleines Rechteck `[x0..x1]×[z0..z1]` verändert. Beim Ziehen des Smooth-Pinsels sind das viele Dabs pro Sekunde.
- **Code-Auszug**:
```cpp
const std::vector<float> before = (op == Op::Smooth) ? tc.sculptHeights
                                                     : std::vector<float>{};
```
- **Lösung**: Nur das Rechteck plus 1 Vertex Rand kopieren (`(w+2)*(h+2)` Floats, in einen wiederverwendeten `thread_local`-Puffer), und `before[...]` über Rect-lokale Indizes lesen. Das Ergebnis ist identisch, weil außerhalb des Rechtecks (+Rand) nie gelesen wird.
- **Erwarteter Effekt** (Schätzung): ca. 4 MB memcpy+Alloc pro Dab → wenige KB. Bei interaktivem Smoothen auf großen Terrains ein spürbarer Frame-Zeit-Gewinn (Größenordnung 1 ms/Dab bei 1025²).
- **Risiko / Verifikation**: Gering. Test: Smooth-Dab auf zufälligem Feld, Ergebnis bit-identisch zur alten Version (auch an Gitterrändern).

### SCN-713 – Weightmap: jede Pinselbewegung baut und lädt die komplette Textur neu
- **ID**: SCN-713
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/TerrainPaint.cpp:169-201`, `:132`; Verbraucher `TerrainSystem.cpp:396-420` (nicht Teil des Slices, zur Einordnung gelesen)
- **Problem**: `paint()` setzt nur ein globales `weightsDirty`. Der nächste Tick macht daraufhin `computeAverageLayerWeights(tc)`, `buildWeightTexture` (`usesSecondPage` scannt per `std::any_of` die komplette zweite Page; danach Kopie der gesamten `layerWeights`, bei zwei Pages row-weises Zusammenbauen), `cm.replaceTexture` und `InvalidateTexture`. Bei Standard 512² sind das 1 MB pro Page, bei `weightRes` bis 4096 bis zu 64 MB pro Page, **pro Frame während des Malens**. Anders als beim Sculpt (`regionDirty` in `TerrainSculpt.cpp:20-33`) gibt es kein Dirty-Rect für die Weights.
- **Lösung**: Dirty-Rect (`weightsDirtyX0/Z0/X1/Z1`) analog zu `markRegionDirty` führen, in `paint()` setzen und im System für Teil-Upload (`UpdateTextureRegion`-artige Backend-API, falls vorhanden; sonst nur das Rect neu zusammensetzen und die Durchschnittsberechnung inkrementell). `usesSecondPage` als gecachtes Bool im Component pflegen (`paint()` setzt es bei `layer>=4`; `fillLayer` ebenso) statt jedes Mal zu scannen. Der erste Schritt (gecachtes Flag, Rect für `computeAverageLayerWeights`) ist lokal und verhaltensgleich; der Teil-Upload braucht Renderer-Unterstützung.
- **Erwarteter Effekt** (Schätzung): bei 512² ca. 1-3 ms/Frame weniger beim Malen, bei 2048²+ deutlich mehr. **Messen** (Backends unterscheiden sich).
- **Risiko / Verifikation**: Mittel (Renderer-API). `tests/test_terrain*` für Paint plus Test, dass `buildWeightTexture` nach Teil-Update identisch zum Vollbau ist.

### SCN-714 – TerrainHeightmap::importPixels: Taps pro Zeile neu berechnet, Verzweigungen pro Sample
- **ID**: SCN-714
- **Kategorie**: D
- **Schweregrad**: Niedrig (nur Import, einmalig)
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/TerrainHeightmap.cpp:35-61`, `:142-161`
- **Problem**: `axisTaps(xi, ..., tx)` hängt nur von `xi` ab, wird aber für jede Zeile `zi` neu berechnet (res² statt res Aufrufe, je mit `std::floor/ceil` und `push_back`). `GreyReader::channel` verzweigt pro Sample auf `bytesPerChannel` und rechnet bei RGB 3 Mal. Beim Downsampling (4k → 513) sind das ca. 17 Mio. `grey()`-Aufrufe.
- **Lösung**: Die Taps aller Spalten einmal vor der Zeilenschleife in einen `vector<vector<Tap>>` (oder flach mit Offsets) vorberechnen. Die Verzweigung auf `bytesPerChannel`/`channels` per Template-Parameter `GreyReader<Bytes, Channels>` auflösen oder vorab in einen `vector<float>` Grauwerte (wie `greyFromTexture` schon macht) konvertieren und darauf samplen. Ergebnis identisch.
- **Erwarteter Effekt** (Schätzung): Importzeit großer Heightmaps -30 bis -60 %. Einmalige Editor-Aktion, daher Niedrig.
- **Risiko / Verifikation**: Gering. Bestehende `test_terrain_heightmap`-Tests plus Bit-Vergleich der Ausgabe.

### SCN-715 – ID-Maps ohne `reserve` beim Laden großer Szenen
- **ID**: SCN-715
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:2292`, `:2376`, `:2607` (`idMap`), `:2524` (`buildSubtreeJson`)
- **Problem**: `unordered_map<UUID, Entity> idMap` wächst bei 200k Entities über ca. 17 Rehash-Schritte; in `sliceForAdditiveLoad` (`:3199`) wird `at.reserve(n)` dagegen schon gemacht, die Absicht ist also bekannt.
- **Lösung**: `idMap.reserve(scene["entities"].size())` an den drei Stellen, `idMap.reserve(entities.size())` in `buildSubtreeJson`.
- **Erwarteter Effekt**: einige Prozent der Build-Zeit bei sehr großen Szenen (Schätzung).
- **Risiko / Verifikation**: Keines.

### SCN-716 – buildRibbon/buildTube allokieren pro Aufruf rund zehn Vektoren
- **ID**: SCN-716
- **Kategorie**: C
- **Schweregrad**: Niedrig (Hot Path nur, falls pro Frame für Trails/Seile aufgerufen; Aufrufer liegen in RopeTrailSystem, außerhalb des Slices)
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/SplineGeometry.cpp:197-243`, `:254-301`, `:303-367`, `:369-381`
- **Problem**: `buildRibbon` legt `pts`, `frames`, `tangents` (in `buildFrames`), `width`, `normal` sowie die vier `MeshData`-Vektoren pro Aufruf neu an. Für kamera-ausgerichtete Trails ändert sich das Mesh jeden Frame, also pro Trail ca. 9 Allokationen/Frame. `interleave` kopiert das Ergebnis dann nochmals. `mesh.positions.insert(end, {…})` mit Initializer-List ist langsamer als direktes Schreiben nach `resize`.
- **Lösung**: Eine `RibbonScratch`-Struktur (Vektoren, wiederverwendet) als optionalen Parameter; `buildFrames` mit Out-Vektor. `MeshData` am Aufrufer wiederverwenden (`clear()` statt neu). Optional direkt interleaved ausgeben. Nur sinnvoll nach Messung im RopeTrailSystem.
- **Erwarteter Effekt**: pro Trail und Frame ca. 8-10 Allokationen weniger (Schätzung); nur relevant bei vielen Trails.
- **Risiko / Verifikation**: Gering. `test_spline_geometry`-Tests; Ergebnis bitgleich.

### SCN-717 – ScriptApi: wiederholter Boilerplate und String-Kopien in Gettern
- **ID**: SCN-717
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/ScriptApi.cpp:159-259`, `:47-54`, `:167-174`; `ScriptContext.cpp:251-263` vs `:371-383`
- **Problem**: 15 Funktionen wiederholen `auto& reg = world.registry(); entt::entity e = toEntity(id); if (!reg.valid(e)) return …; reg.try_get<T>(e)`. Es gibt bereits `transformOf` als Helfer, aber nur für Transform. `getName/getUIText` geben `std::string` by value zurück, und die Lua-Shims machen daraus gleich `.c_str()` (eine Kopie pro Aufruf; bei Namen <16 Zeichen kein Heap, sonst schon). `lua_horizon_setMaterialParam` und `lua_horizon_setUIMaterialParam` sind bis auf den Aufruf identisch (vec4 aus 4 Lua-Zahlen).
- **Lösung**: Ein `template<class T> T* componentOf(HorizonWorld&, uint32_t)` ersetzt die 15 Wiederholungen; eine `readVec4(L, first)`-Funktion für die beiden Material-Shims. Für die String-Getter `const std::string*`/`std::string_view` zurückgeben (Python-Backend nutzt dieselbe API, daher Signaturänderung koordinieren; die Python-Seite liegt außerhalb dieses Slices).
- **Erwarteter Effekt**: ca. 60 Zeilen weniger, weniger Kopien. Kleine Laufzeitwirkung.
- **Risiko / Verifikation**: Gering; die API-Signaturen sind von `HE_Python` genutzt (Cross-Modul). Python- und Lua-Script-Tests.

### SCN-718 – SceneSystems: doppelte Präzipitations-Logik und mehrfaches Suchen der WeatherComponent
- **ID**: SCN-718
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/SceneSystems.cpp:54-96`, `:138-140`, `:184-198`
- **Problem**: Die Berechnung „Regen/Schnee-Menge, isSnow, Cap aus WeatherComponent oder 20000" steht zweimal (in `pushGpuParticleParams` und `pushProfilerSceneCounters`, Letzteres mit `uint32_t cap = 20000; // same default as…`). Die erste `WeatherComponent` wird pro Frame in `tickWorld` (`:139`) und noch einmal in `pushGpuParticleParams` (`:79`) per `view.each() { …; break; }` gesucht. Alle drei Stellen müssen bei einer Änderung gleichzeitig angepasst werden.
- **Lösung**: Eine kleine `struct PrecipState { bool active; bool isSnow; float amount; int cap; float clock; }` plus `PrecipState readPrecip(HorizonWorld&)` und die drei Stellen darauf umstellen (eine Suche pro Frame).
- **Erwarteter Effekt**: Weniger Code, eine View-Suche statt drei.
- **Risiko / Verifikation**: Gering. Wetter-Tests (`test_weather*`).

### SCN-719 – removeComponentForKey/resetComponentForKey: String-Vergleichskette; weitere lineare Binding-Suchen
- **ID**: SCN-719
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SceneSerializer.cpp:2791-2820`, `:2859-2873`, `:3737-3747`
- **Problem**: Die Makros erzeugen eine Kette von 46 `if (key == "…")` pro Aufruf (bis zu 46 String-Vergleiche). Sie wird pro Komponente und Entity im Prefab-Sync aufgerufen. `anyOverrideOn`/`anyOverrideOnRecord` sind lineare Scans über `overrides`.
- **Lösung**: Die X-Macro-Liste zu einer `static const unordered_map<string_view, Handler>` aufbauen (gleiches Makro, anderer Ausdruck), die auch `isKnownComponentKey` und `removeComponentsAbsentFrom` bedient. Zusammen mit SCN-706 umsetzen.
- **Erwarteter Effekt**: gering; Hauptnutzen ist eine Quelle für die Schlüsselliste.
- **Risiko / Verifikation**: Gering; `test_scene_serializer` deckt jeden Key ab.

### SCN-720 – computeTerrainHeightField initialisiert den Vektor doppelt
- **ID**: SCN-720
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/TerrainMeshGenerator.cpp:72-94`
- **Problem**: `std::vector<float> heights(vertCount, 0.0f);` wird sofort nach `heights = tc.sculptHeights;` überschrieben (zwei Allokationen/Initialisierungen plus Kopie von bis zu 4 MB). `generateTerrainMesh` (`:106`) und `ensureHeights` rufen das auf.
- **Lösung**: `if (sculptHeights.size() == vertCount) return tc.sculptHeights;` am Anfang (NRVO/Kopie nur einmal) und erst danach den Nullvektor bzw. fBm-Schleife. Verhalten gleich. Für `generateTerrainMesh` bei vorhandenem Sculpt-Feld könnte die Funktion direkt `const std::vector<float>&` verwenden (Referenz statt Kopie).
- **Erwarteter Effekt**: eine 4-MB-Initialisierung weniger pro Aufruf.
- **Risiko / Verifikation**: Keines.

### SCN-721 – exportComponentText/serializeEntityComponents serialisieren die ganze Entity für eine Komponente
- **ID**: SCN-721
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:3504-3516`, `:3597-3611`, `:3613-3639`
- **Problem**: `exportComponentText` baut per `serializeComponents` alle ca. 47 möglichen Komponenten, nur um einen Schlüssel zu lesen. `serializeEntityComponents` (Collab-Pfad, pro Edit) serialisiert jedes Mal alle Komponenten, auch wenn nur eine geändert wurde. `componentKeyOfText` parst bei geöffnetem Menü jeden Frame den Clipboard-Text voll, wenn er den Envelope-String enthält (Cheap-Reject nur per `find`). `importComponentText` parst ihn danach nochmal.
- **Lösung**: `serializeComponents(registry, entity, std::string_view onlyKey = {})` (Filter pro Block) für `exportComponentText`. Einen Parse-Cache (letzter Text → Schlüssel) in `componentKeyOfText` oder im Aufrufer (`InspectorPanel.cpp:4041`). Kollaborations-Delta (nur geänderte Schlüssel) wäre eine Protokolländerung (außerhalb des Scopes).
- **Erwarteter Effekt**: klein (Editor-Aktion); sinnvoll bei Collab mit vielen Edits.
- **Risiko / Verifikation**: Gering; Clipboard-Tests.

### SCN-722 – adoptLegacyInstance: O(n²) und ganze Entity-Serialisierung pro Record
- **ID**: SCN-722
- **Kategorie**: D
- **Schweregrad**: Niedrig (Legacy-Pfad, einmalig pro alter Instanz)
- **Typ**: Performance
- **Ort**: `SceneSerializer.cpp:2896-2972`
- **Problem**: Pro Record werden alle Records erneut nach Geschwistern mit gleichem Namen durchsucht (O(R²), jeweils mit `value("name", …)`-String-Kopien), und danach `serializeComponents(registry, e)` komplett. Läuft nur für Instanzen ohne Bindings (Altbestand).
- **Lösung**: Einmal vorab `unordered_map<pair<parentKey,name>, count>` aufbauen. Nur anfassen, wenn alte Szenen messbar langsam sind.
- **Erwarteter Effekt**: nur für alte Szenen mit großen Prefabs.
- **Risiko / Verifikation**: Gering. Legacy-Adoption-Tests.

### SCN-723 – Spline: redundante Kurvenauswertungen in `refineSpan`/`at()`
- **ID**: SCN-723
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/SplineCurve.cpp:32-54`; `SplineGeometry.cpp:93-105`
- **Problem**: `refineSpan` wertet `at(u)` für 3 Prüfpunkte aus und beim Teilen nochmal `at(um)` (identisch mit dem 0.5-Prüfpunkt), und die Kind-Aufrufe bewerten ihre Prüfpunkte neu. `CatmullRomSpan::at` macht pro Aufruf 8 Divisionen, deren Nenner (`t1-t0`, `t2-t1`, …) pro Span konstant sind.
- **Lösung**: Die Kehrwerte im Konstruktor vorberechnen und `at` mit Multiplikationen schreiben (Ergebnis kann sich im letzten Ulp ändern; Tests mit Toleranz prüfen), und den 0.5-Punkt aus der Prüfschleife an `refineSpan` weiterreichen. Ohne Messdaten ist das eine Mikro-Optimierung. Nur umsetzen, wenn Polyline-Erzeugung im Profil auftaucht.
- **Erwarteter Effekt**: gering.
- **Risiko / Verifikation**: Kleine numerische Abweichungen; Spline-Tests mit Toleranz.

### SCN-724 – startWorldScripts fragt `assetType` doppelt pro Entity
- **ID**: SCN-724
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/ScriptContext.cpp:1560-1561` und `:1607`
- **Problem**: `startWorldScripts` prüft `cm.assetType(sc.scriptAssetId) == HorizonCodeClass` und `startEntityScript` prüft es für dieselbe Entity nochmals. Ebenso `active.off(entity)` (einmal in der Schleife, dann nicht mehr in `startEntityScript`, hier unkritisch).
- **Lösung**: Interne Variante `startEntityScriptUnchecked(entity, sc, cm)` für `startWorldScripts`, oder die Schleifenprüfung entfernen und stattdessen `kInvalidInstance` ohne Zählung unterscheiden (gleiche Zähllogik: HorizonCode-Klassen dürfen nicht in `attempted`).
- **Erwarteter Effekt**: ein Hash-Lookup pro Script-Entity beim Start; vernachlässigbar.
- **Risiko / Verifikation**: Gering.

---

## Cross-Modul-Hinweise

- `src/HE_Scene/include/HorizonScene/Components/PrefabInstanceComponent.h:104-115`: `instanceOf/templateOf` sind lineare Vektorscans über `bindings`. Ein Index (oder zumindest `unordered_map`) hier würde SCN-702 und SCN-708 an der Wurzel lösen.
- `src/HE_Editor/OutlinerPanel.cpp:670-674`: ruft pro Instanz-Zeile `prefabRemovedHereCount` + `prefabAddedHereCount` und pro Zeile unter einer Instanz `prefabInstancesBinding` auf (siehe SCN-708).
- `src/HE_Editor/EditorApplication.cpp:13604`: `syncPrefabInstances("save")` läuft bei **jedem** Speichern über alle Instanzen (SCN-702). Ein Dirty-Flag „Prefab-Asset seit letztem Sync geändert" würde den Sync im Normalfall ganz überspringen.
- `src/HE_Editor/EditorUndo.cpp:10,42,84,118`: Vollszenen-Snapshot (`saveToMemory`) pro Undo-Schritt (SCN-701). Langfristig wären Delta-Snapshots (nur geänderte Entities) die größere Lösung.
- `src/HE_Game/src/GameApplication.cpp:1504-1505`: verwendet die Teilmengen-Variante von `collectAssetRefs` (SCN-704).
- `src/HE_Scene/src/TerrainSystem.cpp:396-420`: Weightmap-Neuaufbau und `computeAverageLayerWeights` laufen bei jedem `weightsDirty` über die ganze Textur (SCN-713).
- Das Ctx-Design in `EngineApi.h:60-150` (25+ `std::function`-Member, „New members still go at the END: … positional aggregate init") begünstigt SCN-703 strukturell. Ein `Services`-Pointer wäre die saubere Lösung, auch für das Python-Backend (`HE_Python`).
- `RopeTrailSystem` (nicht in diesem Slice) ist der vermutliche Hauptaufrufer von `buildRibbon` pro Frame (SCN-716).

## Unsicher / Messung nötig

- SCN-703: tatsächliche Kosten von `apiCtx` pro Aufruf (SBO der `std::function`-Kopien, Größe von `Ctx`/`Value` auf MSVC/libc++/libstdc++); Mikro-Benchmark nötig.
- SCN-701: Anteil der JSON-Kopien an der Snapshot-Zeit gegenüber CBOR-Encode und `serializeComponents` selbst.
- SCN-702: wie viele Instanzen/Records in realen Projekten vorkommen; ob `syncPrefabInstances` beim Speichern in der Praxis dominiert.
- SCN-710: ob die Schleife über `SkeletalMeshComponent` bei realistischen Charakterzahlen messbar ist.
- SCN-713: Verhalten pro Backend bei `replaceTexture` + `InvalidateTexture` (ob Teil-Upload existiert).
- SCN-716/SCN-723: Aufrufhäufigkeit pro Frame (RopeTrailSystem außerhalb des Slices).
- Nicht untersucht (außerhalb des Slices): `HE::rootMotionBeginFrame/poseBeginFrame` und die drei Skeletal-Treiber (jeweils eigene Views pro Frame ohne Dirty-Flag).

## Gelesen

Alle 13 Dateien des Slices wurden vollständig gelesen: ja (SceneSerializer.cpp 1-4562 in Blöcken, SceneSystems.cpp, ScriptApi.cpp, ScriptContext.cpp, SequenceEval.cpp, SequenceSystem.cpp, SplineCurve.cpp, SplineGeometry.cpp, TerrainGenerate.cpp, TerrainHeightmap.cpp, TerrainMeshGenerator.cpp, TerrainPaint.cpp, TerrainSculpt.cpp). Zur Einordnung wurden punktuell gelesen: `EngineApi.h` (Ctx), `ScriptContext.h`, `PrefabInstanceComponent.h`, `TerrainSystem.cpp:375-425`, `OutlinerPanel.cpp:640-675`, `EditorApplication.cpp:13369-13392, 13598-13606`.
