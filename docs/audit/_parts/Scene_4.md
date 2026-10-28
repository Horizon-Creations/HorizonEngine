# Scene Slice 4 – Components (ReplicatedVars…WeatherComponent), EngineApi.h … SequenceSystem.h (Kürzel SCN)

## Zusammenfassung

Slice 4 besteht fast nur aus Headern (70 Dateien, ca. 10 900 Zeilen, davon `EngineApi.h` 3 094 und `PhysicsWorld.h` 933). Die Header sind inhaltlich sehr gut dokumentiert; die Komponenten selbst sind überwiegend schlanke POD-Structs mit Dirty-Flags (`TransformComponent`, `TerrainComponent`, `SkeletalMeshComponent`). Die eigentlichen Kosten stecken in den dazugehörigen Systemen, deren Signaturen hier deklariert sind. Ich habe die jeweilige `.cpp` nur punktuell als Beleg gelesen (die `.cpp`-Dateien liegen in anderen Slices; dort bitte gegenprüfen). Auffällig sind (a) zwei per-Frame-Pfade ohne Dirty-Tracking mit Heap pro Entity (`PropertyReplicator::update`, `LODSystem::update`), (b) mehrere kleinere per-Frame-Allokationen in Script-/Input-/Rope-Pfaden und (c) ein unbegrenzt wachsender Log-Merker im RPC-Router. Funde: 2 Hoch, 9 Mittel, 9 Niedrig. Alle Performance-Schätzungen sind Schätzungen ohne Messung.

---

### SCN-401 – PropertyReplicator::update baut pro Frame und Entity Namen-/Wert-Listen neu auf
- **ID**: SCN-401
- **Kategorie**: C, D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/Net/PropertyReplicator.h:117` (`update()`), `:184-194` (`Table`), `:202-203` (`collect`); Beleg `src/HE_Scene/src/Net/PropertyReplicator.cpp:356-452`, `:92-124`
- **Problem**: Auf dem Host läuft `update()` jeden Frame. Für jede replizierte Entity (View über `NetworkComponent`, kein Dirty-Flag) wird `collect()` aufgerufen. Das füllt drei frische Vektoren (`std::vector<std::string> names`, `std::vector<bool> notify`, `std::vector<Value> current`) und kopiert dabei jeden Variablennamen (String) und jeden Wert (`Value` mit evtl. Strings/Containern). Danach wird `names != t.names` (String-Vergleich je Eintrag) geprüft und pro Property mit `std::find` über `names` nach dem Namen gesucht (O(n²) je Entity). Zusätzlich allokiert `joinedConnections()` jeden Frame einen Vektor, und die Schleife über `m_tables` prüft jeden Frame per `m_rep->entityOf()` (Hash-Lookup) alle Tabellen. Das ist Heap-Verkehr pro Entity und Frame, selbst wenn sich nichts geändert hat (der Normalfall).
- **Code-Auszug**:
```cpp
// PropertyReplicator.cpp:397-400, 410, 426
std::vector<std::string> names;
std::vector<bool>        notify;
std::vector<Value>       current;
collect(entity, names, notify, current);
if (names != t.names) { broadcastTableFor(nc.netId); continue; }
...
const auto at = std::find(names.begin(), names.end(), t.names[i]);
```
- **Lösung**:
  1. Namen/Notify nicht pro Frame neu kopieren: die Deklaration ändert sich nur, wenn eine Variable deklariert wird (`ReplicatedVarsComponent::declare`) oder der HorizonCode-Klassenwechsel/Hot-Reload passiert. Einen `declRevision`-Zähler (uint32) in `ReplicatedVarsComponent` (bei `declare` erhöhen) und einen Zähler/Hash der Runtime-Replikationsliste (`replicatedVariablesOf(inst)`) in `Table` merken; `names` nur bei Änderung neu aufbauen.
  2. Pro Frame nur die Werte vergleichen: `collectValuesInto(entity, t, scratchValues)` mit wiederverwendetem Member-Vektor `m_scratchValues` (kein Namens-Lookup, Positionen sind bereits durch die Tabelle fixiert). Bei unveränderter Deklaration entfällt das `std::find`.
  3. `joinedConnections()` einmal in einen wiederverwendeten Member-Vektor schreiben; die `m_tables`-Reap-Schleife nur bei geänderter `GameReplication`-Registrierung/`structureEpoch` ausführen.
  4. Kleine Skizze: `if (t.declRev != currentDeclRev(entity)) { rebuildNames(...); broadcastTableFor(...); continue; } m_rep->readValues(entity, t, m_scratchValues); for (i...) if (!valuesEqual(m_scratchValues[i], t.values[i])) ...`
- **Erwarteter Effekt** (Schätzung): pro Frame keine Heap-Allokationen mehr im Normalfall (drei Vektoren + N Strings je Entity). Bei einigen Hundert replizierten Entities mit je mehreren Variablen im Bereich von zehn bis hundert µs pro Frame auf dem Host; nur im Multiplayer-Host relevant.
- **Risiko / Verifikation**: Die Reihenfolge der Indizes (Wire-Format, „insertion order ist load-bearing“, siehe `ReplicatedVarsComponent.h:13-19`) darf sich nicht ändern. Die Erkennung „Deklaration geändert“ muss alle bisherigen Auslöser abdecken (Skript deklariert mitten in der Session, Hot-Reload, Typänderung). Absicherung: `tests/test_net_property_replication.cpp` (Deklaration nach Join, Typwechsel, Reorder) erweitern um „späte Deklaration“ und „Hot-Reload entfernt Variable“.

---

### SCN-402 – LODSystem::update läuft jeden Frame über alle LOD-Entities und komponiert jedes Mal die Eltern-Kette
- **ID**: SCN-402
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/LODSystem.h:10`; Beleg `src/HE_Scene/src/LODSystem.cpp:11-62`, `src/HE_Scene/src/TransformHierarchy.cpp:321-367`
- **Problem**: Der View `LODComponent, MeshComponent` wird jeden Tick komplett iteriert, ohne Dirty-Flag oder Early-out. Pro Entity wird `HE::worldPositionOf` → `worldMatrixOf` aufgerufen: das sammelt die komplette Parent-Kette (mit `registry.valid` + `try_get` je Stufe) und multipliziert Matrizen von oben nach unten; erst danach wird `glm::distance` zur Kamera gebildet. Terrain-Chunks (hunderte statische Kinder), Foliage-/Prop-LODs und alle statischen Meshes werden damit jeden Frame vollständig neu ausgewertet, obwohl sich weder ihre Position noch (bei stehender Kamera) die Distanz geändert hat. Die Dokumentation von `worldMatrixOf` nennt diesen Aufrufer ausdrücklich als messbaren Anteil (`TransformHierarchy.cpp:331-333`, „baseline §4.5“).
- **Code-Auszug**:
```cpp
// LODSystem.cpp:11-30
auto view = registry.view<LODComponent, MeshComponent>();
for (auto [entity, lod, mesh] : view.each())
{
    if (lod.levels.empty()) continue;
    const glm::vec3 pos = HE::worldPositionOf(world, entity);
    const float dist = glm::distance(cameraPos, pos);
```
- **Lösung**:
  1. Frame-kohärenter Early-out auf Weltebene: `LODSystem` merkt sich `lastCameraPos`, `world.structureEpoch()` und einen von `propagateTransforms` gepflegten „Transform-Änderungs-Zähler“ (oder `lastPropagateStats(world).changed`). Ist die Kamera um weniger als `eps` bewegt, die Epoche gleich und nichts transformiert/dirty, wird das System übersprungen (Ergebnis ist identisch, weil `lod.current` bereits stimmt). `eps` muss kleiner als der kleinste Abstand zu einer LOD-Grenze gewählt werden oder die Prüfung nutzt eine Distanz-Hysterese nur als Beschleuniger (exakt identisch: bei Kamerabewegung läuft die volle Schleife).
  2. Alternativ/zusätzlich: Weltposition aus `TransformComponent::worldMatrix[3]` lesen, wenn die Entity `localCacheValid` hat und der Tick nach `propagateTransforms` läuft; nur für frisch erzeugte Entities (`!localCacheValid`) den Pfad `worldPositionOf` nehmen. Das braucht eine Reihenfolgeprüfung in `SceneSystems::tickWorld` (LOD läuft am Ende, Propagate nicht davor; siehe Kommentar `LODSystem.cpp:20-28`).
  3. Optional: Distanz quadriert vergleichen (`maxDistance²`), spart `sqrt`/`glm::distance` je Entity; verändert aber `<=`-Grenzfälle numerisch minimal, daher nur mit Test.
- **Erwarteter Effekt** (Schätzung): bei ruhender Kamera und statischer Szene nahezu 100 % der LOD-Kosten (mehrere mat4-Multiplikationen pro Entity und Frame); bei laufender Kamera Schritt 2 deutlich billiger pro Entity. Bei 200 k-Entity-Szenen (siehe `docs/world-streaming-baseline-2026-10-06.md`) wahrscheinlich im ms-Bereich.
- **Risiko / Verifikation**: Das Verhalten „Chunk-Raster wird in demselben Tick richtig gelevelt“ (Kommentar `LODSystem.cpp:20-28`) darf nicht regressieren; Early-out darf nie eine neu erzeugte Entity überspringen (Epochen-Prüfung). Tests: `tests/test_lod.cpp`, `tests/test_transform_propagation.cpp`; neuer Test „Kamera still + Entity bewegt via Gizmo (ohne `dirty`) → LOD korrekt“, da viele Schreiber `dirty` nicht setzen (`TransformComponent.h:36-39`). Messen vor Umsetzung (Profiler-Scope „LOD“).

---

### SCN-403 – GameReplication::sendSnapshots: O(Clients × Entities) pro Tick, Samples pro Client neu berechnet
- **ID**: SCN-403
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/GameReplication.h:306` (`sendSnapshots`), `:344-348` (`m_byNetId`, `m_interp`); Beleg `src/HE_Scene/src/GameReplication.cpp:679-730`
- **Problem**: Je Snapshot-Tick (30 Hz) und je verbundenem Client wird `m_byNetId` (ein `unordered_map`, knotenbasiert) komplett iteriert; pro Eintrag `reg.valid`, `try_get<NetworkComponent>`, `try_get<TransformComponent>`, `wireOrigin(...)` und `toWire(...)`. Die Wire-Quantisierung (Position/Rotation) ist client-unabhängig und wird trotzdem für jeden Client neu berechnet; nur der Relevanz-Test (Distanz) ist clientspezifisch. Der Vektor `relevant` wird pro Client ohne `reserve` aufgebaut (wiederholtes Wachsen).
- **Code-Auszug**:
```cpp
// GameReplication.cpp:701-729
for (const ConnectionId conn : m_net->connections())
{
    ...
    std::vector<std::pair<std::uint32_t, Sample>> relevant;
    for (const auto& [netId, entity] : m_byNetId)
    {
        if (!reg.valid(entity)) continue;
        auto* nc = reg.try_get<NetworkComponent>(entity);
        auto* tc = reg.try_get<TransformComponent>(entity);
        ...
        s.position = toWire(tc->position, wireOrigin(m_world, entity));
```
- **Lösung**:
  1. Einmal pro Tick eine Tabelle `m_tickSamples` (reservierter Member-Vektor aus `{netId, Sample, relevanceRadius², position}`) über `m_byNetId` bauen, dann pro Client nur noch filtern und die Teilmenge in die Datagramme schreiben.
  2. `relevant` und die Tick-Tabelle als Member halten (`clear()` statt neu allokieren).
  3. Mittelfristig `m_byNetId` als dichten Vektor (netId ist ein fortlaufender Zähler `m_nextNetId`) statt `unordered_map` führen.
- **Erwarteter Effekt** (Schätzung): Aufwand sinkt von Clients×Entities×(3 Lookups + Quantisierung) auf Entities×(3 Lookups + Quantisierung) + Clients×Entities×(ein Distanztest). Bei 8 Clients ca. Faktor 3–6 weniger Arbeit im Sendepfad.
- **Risiko / Verifikation**: Bytes auf der Leitung müssen identisch bleiben (Reihenfolge der Entities im Datagramm hängt bisher an der Hash-Iterationsreihenfolge – nicht garantiert, Tests dürfen also nicht darauf bauen). `tests/test_game_replication.cpp` (Budget-/Split-Tests) muss grün bleiben.

---

### SCN-404 – RopeTrailSystem: pro Frame und Rope eine Vektor-Kopie, Matrix-Inverse und Hash; Trail-Geometrie als Wert pro Frame
- **ID**: SCN-404
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/RopeTrailSystem.h:38-39` (`resolveControlPoints`), `:58-59` (`buildTrailGeometry`), `:70` (`geometryHash`); Beleg `src/HE_Scene/src/RopeTrailSystem.cpp:63-87`, `:293-318`
- **Problem**: Der Header verspricht „a rope nobody touched costs one hash per frame“ (`:67-69`). Tatsächlich kostet sie pro Frame: eine vollständige Kopie von `rope.controlPoints` (`std::vector<glm::vec3> pts = rope.controlPoints;` – Heap-Allokation), bei Attachments zusätzlich `glm::inverse(worldMatrixOf(...))` (Parent-Kette + 4×4-Inverse), je Attachment `findByEntityId` + `worldPositionOf` (weitere Kettenwanderung), dann `owned.find(entityKey(e))` und `cm.getStaticMesh(...)` (zwei Lookups) bevor überhaupt `hash == builtHash` geprüft wird. Zusätzlich liefert `buildTrailGeometry` laut Header-Kommentar jeden Frame durch den Extractor ein `MeshData` **by value** (mehrere Vektoren) pro Trail.
- **Code-Auszug**:
```cpp
// RopeTrailSystem.cpp:66, 76, 300-317
std::vector<glm::vec3> pts = rope.controlPoints;           // Heap pro Rope und Frame
const glm::mat4 toLocal = glm::inverse(HE::worldMatrixOf(world, entity));
...
const std::vector<glm::vec3> pts = resolveControlPoints(world, e, rope);
const uint64_t hash = geometryHash(rope, pts);
...
if (ours && hash == rope.builtHash) continue;
```
- **Lösung**:
  1. `resolveControlPoints(world, e, rope, std::vector<glm::vec3>& out)` mit einem pro `update()` wiederverwendeten Scratch-Vektor (wie `m_tickScratch` in `EntityHost.h:155`); der alte Wert-Rückgabe-Overload bleibt als dünner Wrapper für Editor/Tests.
  2. Billigen Vorab-Test: Hat das Rope keine Attachments, ist der Hash eine reine Funktion der Komponente → Hash über `rope.controlPoints` direkt (ohne Kopie) bilden. `glm::inverse` nur berechnen, wenn ein Attachment tatsächlich vorhanden **und** dessen Weltposition sich gegenüber dem letzten Frame geändert hat (letzte Attachment-Weltpositionen in der Komponente merken).
  3. `buildTrailGeometry(trail, cameraPos, MeshData& out)` mit wiederverwendetem Puffer im Extractor (`out.clear()` behält Kapazität).
- **Erwarteter Effekt** (Schätzung): eine Heap-Allokation und mehrere Hash-/Chain-Lookups pro Rope und Frame weniger. Bei wenigen Ropes vernachlässigbar, bei Hunderten (Ketten, Seile) spürbar.
- **Risiko / Verifikation**: Der Hash muss bitgleich bleiben (er wird in Szenen nicht gespeichert, ist aber im Test `tests/test_rope_trail.cpp` gepinnt). Rebuild-Auslösung für bewegte Attachments darf nicht entfallen: Test „Attachment bewegt sich → Mesh wird ersetzt“ und „Rope unberührt → kein `replaceStaticMesh`“.

---

### SCN-405 – Entity-Id-Index: zwei Hash-Knoten pro Entity, ohne `reserve`
- **ID**: SCN-405
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/HorizonWorld.h:300-301`; Beleg `src/HE_Scene/src/HorizonWorld.cpp:408-424`, `:449-478`
- **Problem**: Jede Entity erzeugt über die `on_construct<EntityIdComponent>`-Signale einen `unordered_multimap`-Knoten (`m_idIndex`) **und** einen `unordered_map`-Knoten (`m_idIndexed`), also zwei Heap-Allokationen plus Rehashes (kein `reserve`), zusätzlich zu `NameComponent` (String), `HierarchyComponent::children` (Vektor) und `UUID::generate()`. Bei Szenen-Loads mit 200 k Entities (siehe `docs/world-streaming-baseline-2026-10-06.md`) sind das 400 k kleine Allokationen allein für den Index. `m_idIndexed` existiert nur, weil das Update-Signal den alten Schlüssel nicht liefert.
- **Code-Auszug**:
```cpp
// HorizonWorld.h:300-301
std::unordered_multimap<HE::UUID, Entity> m_idIndex;
std::unordered_map<Entity, HE::UUID>      m_idIndexed;
```
- **Lösung**:
  1. `m_idIndexed` durch einen Vektor `std::vector<HE::UUID> m_idOf` ersetzen, indiziert mit `entt::to_entity(e)` (entt-Entities sind dichte Indizes, der Vektor wird bei `onEntityIdSet` bei Bedarf vergrößert). Spart einen Knoten pro Entity und macht `unindexEntityId` zu einem Array-Zugriff.
  2. Für `m_idIndex` eine flache Hash-Map (z. B. `entt::dense_map` oder `std::vector` + offene Adressierung) verwenden; das Mehrfach-Halter-Verhalten (zwei Entities, eine UUID) bleibt über Mehrfacheinträge erhalten.
  3. `reserveEntities(size_t n)` an `HorizonWorld` anbieten und von den Scene-Loadern (n = Länge des `entities`-Arrays) aufrufen; ruft `m_registry.storage<...>().reserve(n)` und `m_idIndex.reserve(n)`.
- **Erwarteter Effekt** (Schätzung): beim Laden großer Szenen 30–50 % weniger Allokationen im Index-Pfad und weniger Rehash-Spitzen; Lookup `findByEntityId` bleibt O(1).
- **Risiko / Verifikation**: Semantik „zwei Halter einer ID bleiben beide auffindbar“ (`HorizonWorld.h:51-52`) muss erhalten bleiben. Tests: `tests/test_scene_entity_ids.cpp` (Kollisionen, Zerstörung des ersten Halters). Load-Zeit mit `SceneSerializer::lastLoadTiming()` vorher/nachher messen.

---

### SCN-406 – Jeder HE::api-Aufruf der Interpreter-/Registry-Pfade allokiert zwei `std::vector<Value>`
- **ID**: SCN-406
- **Kategorie**: B, C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/EngineApi.h:3040` (`ApiFn::invoke`), `:3050` (`find`), `:3090-3092` (`designTimeCallApi`); Beleg `src/HE_Scene/src/EngineApi.cpp:6021-6033`, `:8773-8795`
- **Problem**: Jede Registry-Zeile ist `std::function<std::vector<Value>(Ctx&, const std::vector<Value>&)>`. Jeder Aufruf aus dem HorizonCode-Interpreter, aus Lua/Python-Gruppen (`horizon.<group>.<fn>`) und aus generiertem C++ (`hc::callApi(ctx, "<id>", …)`) baut also (a) einen Argument-Vektor, (b) einen Ergebnis-Vektor (`VV{ Value::ofFloat(...) }`) und geht (c) über `std::function`-Indirektion und einen String-Lookup `find(const std::string&)` (Hash über den Id-String pro Aufruf, sofern der Aufrufer die Zeile nicht cacht). Für reine Mathe-Zeilen (`math.sin`, `math.clamp`, `str.length`) ist das ein Vielfaches der eigentlichen Arbeit, und genau diese Zeilen stehen in heißen Graph-Schleifen. Der Header-Kommentar `EngineApi.h:39-44` sagt selbst „no std::function … on the boundary“, die Registry tut aber das Gegenteil.
- **Code-Auszug**:
```cpp
// EngineApi.h:3040
std::function<std::vector<Value>(Ctx&, const std::vector<Value>&)> invoke;
// EngineApi.cpp:6033
[](Ctx& c, const VV& a){ return VV{ Value::ofString(entity::getName(c, (Entity)aI(a, 0))) }; }
```
- **Lösung**:
  1. Zusätzlich zu `invoke` einen optionalen, allokationsfreien Pfad anbieten: `using InvokeInto = void(*)(Ctx&, const Value* args, size_t n, Value* out)` (Funktionszeiger statt `std::function`, Ergebnis in vom Aufrufer bereitgestellten kleinen Puffer; `results.size()` ist je Zeile statisch bekannt). Zeilen ohne `invokeInto` fallen auf `invoke` zurück.
  2. Den Interpreter und `hc::callApi` so umstellen, dass sie `const ApiFn*` einmal pro Node cachen (Pointer ist laut `EngineApi.cpp:8775-8778` stabil) statt pro Aufruf per String zu suchen.
  3. Zunächst nur die reinen Mathe-/String-Gruppen (`math`, `str`, `time`, `random`) migrieren; dort ist der Gewinn am größten und die Thunks trivial.
- **Erwarteter Effekt** (Schätzung): für reine Zeilen Faktor 3–10 weniger Overhead pro Aufruf; für schwere Zeilen (Physik, Content) nicht relevant. Gesamteffekt hängt davon ab, wie viele API-Knoten pro Frame laufen (unsicher / messen mit Profiler-Scope um `invoke`).
- **Risiko / Verifikation**: Verhalten „Missing/extra args are tolerated“ (`EngineApi.h:3038-3039`) und die Parity zwischen Interpreter und Codegen muss gleich bleiben. Absicherung: die Invarianten-Tests in `tests/test_engine_api.cpp` (eine Callee-Zeile je Registry-Zeile) und die Codegen-Parity-Tests.

---

### SCN-407 – `apiCtx(L)` baut bei jedem Lua-Aufruf ein ~1,2 KB großes `Ctx` mit drei `std::function`-Kopien
- **ID**: SCN-407
- **Kategorie**: C, B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/EngineApi.h:60-218` (`struct Ctx`), Aufrufer `src/HE_Scene/src/ScriptContext.cpp:82-108`, `:145`, `:164` …
- **Problem**: `Ctx` enthält rund 35 `std::function`-Member (Fenster, Tray, Menü, Autostart …, je 32 Byte, zusammen > 1 KB). `apiCtx()` initialisiert das Struct bei jedem Aufruf (`HE::api::Ctx c{}`), **kopiert** dabei `createObject`, `destroyObject` und `requestQuit` (`std::function`-Kopien, bei größeren Captures mit Heap-Allokation) und zerstört alles wieder. Das passiert z. B. bei jedem `horizon.setPosition`/`setRotation` eines Skripts pro Frame.
- **Code-Auszug**:
```cpp
// ScriptContext.cpp:85-108
HE::api::Ctx c{};
c.world    = getWorld(L);
...
c.createObject  = hs.createObject;     // std::function-Kopie
c.destroyObject = hs.destroyObject;    // std::function-Kopie
...
if (const std::function<void()>* quit = getQuit(L)) c.requestQuit = *quit;
```
- **Lösung**:
  1. Host-Callbacks aus `Ctx` herausziehen in ein einmal pro Session gebautes `struct HostHooks` und in `Ctx` nur `const HostHooks* hooks` halten; die zeilen-spezifischen Zugriffe (`c.hooks->setWindowTitle`) ändern sich mechanisch. Alternativ `Ctx` pro `ScriptContext` einmal bauen (Member `m_apiCtx`, bei `setHostServices`/`setQuitHandler` neu) und per Referenz weiterreichen.
  2. Übergang: `apiCtx` liefert eine `const Ctx&` auf den gecachten Wert; Zeilen, die `Ctx&` mutieren wollen (`c.self`), bekommen eine billige Kopie nur dort.
- **Erwarteter Effekt** (Schätzung): pro Lua-Schreibaufruf mehrere hundert ns bis ~1 µs; bei vielen Skript-Instanzen mit Transform-Writes pro Frame messbar.
- **Risiko / Verifikation**: „Ctx wird mit positioneller Aggregat-Initialisierung gebaut, neue Member ans Ende“ (`EngineApi.h:112-117`) – die beiden Applikationen müssen mit angepasst werden. Messen (Mikrobenchmark 100 k `horizon.setPosition`) vor und nach; `tests/test_scripting.cpp`, `tests/test_engine_api.cpp`.

---

### SCN-408 – PlayerHost::tick erzeugt pro Frame Event-Namen-Strings und eine neue Aktions-Zustandsliste
- **ID**: SCN-408
- **Kategorie**: C, B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/PlayerHost.h:233-241` (`ActionInfo`), `:258` (`fireInputEvent`), `:271` (`m_actions`); Beleg `src/HE_Scene/src/PlayerHost.cpp:289`, `:337-425`; Gegenstück `EngineApi.h:2943-2949` (`setActions(std::vector<ActionState>)`)
- **Problem**: Für jede Axis-/Axis2D-Aktion wird **jeden Frame** `HE::inputEventAxis(a.name)` aufgerufen (neuer `std::string` aus `"Input." + name + ".Axis"`), und `Runtime::fireEvent(inst, event, …)` sucht den String in der Event-Tabelle (und `dispatchToListeners` hasht ihn erneut), selbst wenn kein Graph einen Handler hat. Außerdem wird pro Frame ein `std::vector<ActionState>` angelegt, jedes `ActionState` bekommt eine Kopie von `a.name` und der Vektor wird per Move in `setActions` ersetzt (`actions() = std::move(states)` gibt den alten Vektor samt Strings frei). In `tick` wird weiterhin pro Spieler `const std::string who = …` gebaut (bei > 1 Spielern Heap-Allokation pro Frame).
- **Code-Auszug**:
```cpp
// PlayerHost.cpp:352-358, 388, 423-425
std::vector<HE::api::input::ActionState> states;
states.reserve(m_actions.size());
for (const ActionInfo& a : m_actions) {
    HE::api::input::ActionState st;
    st.name = a.name;
    ...
    fireInputEvent(i, HE::inputEventAxis(a.name), HorizonCode::Value::ofFloat(v));
...
HE::api::input::setActions(std::move(states));
```
- **Lösung**:
  1. `ActionInfo` um die drei Event-Namen (`evAxis`, `evAxis2D`, `evPressed`, `evReleased`) erweitern, einmal in `begin()` füllen; `fireInputEvent` bekommt `const std::string&` darauf.
  2. `m_states` als Member-Vektor halten (Größe = `m_actions.size()`, Namen einmal setzen); pro Frame nur `down/pressed/released/x/y` zurücksetzen und `setActions` durch `publishActions(const std::vector<ActionState>&)` (Kopie der Zahlen, Namen unverändert) ersetzen.
  3. Axis-Events nur feuern, wenn ein Handler existiert oder sich der Wert geändert hat – **nur** wenn das beobachtbare Verhalten das erlaubt: Graphen nutzen `Input.<Action>.Axis` laut Kommentar als „Movement-Pump“ (`PlayerHost.cpp:381-383`), daher bleibt das Feuern pro Frame; nur die Namensbildung entfällt.
  4. `who` nur im Capture-Zweig bauen.
- **Erwarteter Effekt** (Schätzung): bei 10–20 Aktionen mehrere dutzend Allokationen pro Frame weniger; vor allem Cache- und Allokator-Entlastung im Game-Loop.
- **Risiko / Verifikation**: Reihenfolge der Events und die Aussage „stille Aktion wird als released mit Nullachse publiziert“ (`PlayerHost.cpp:345-348`) muss erhalten bleiben. Tests: `tests/test_player_host.cpp`, `tests/test_input_rebind.cpp`.

---

### SCN-409 – RpcRouter: `m_missingLogged` wächst unbegrenzt mit Client-gesteuerten Namen und wird linear durchsucht
- **ID**: SCN-409
- **Kategorie**: C, D (Speicher/Robustheit)
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Scene/include/HorizonScene/Net/RpcRouter.h:258-260`; Beleg `src/HE_Scene/src/Net/RpcRouter.cpp:523-534`, `src/HE_Scene/src/Net/PropertyReplicator.cpp:33-38`
- **Problem**: Für jeden eingehenden `CallServer`, dessen Funktion keine HorizonCode-Signatur hat, wird ein Schlüssel `std::to_string(netId) + ":" + fn` gebaut und per `alreadyLogged` linear in einem `std::vector<std::string>` gesucht und angehängt. `fn` stammt vom Client (bis 128 Zeichen, nur Länge geprüft). Ein Client, der eine eigene Entity besitzt, kann mit wechselnden Namen (bis 60 Aufrufe/s laut Rate-Limit, Standard `kMaxRpcPerSecond`) die Liste beliebig wachsen lassen; jeder Aufruf kostet dann O(n) und die Liste wird erst durch `clear()` am Sessionende frei. Dasselbe Muster (`alreadyLogged`) gilt für `m_localWriteLogged`/`m_doubleDeclareLogged` (dort Skript-gesteuert, harmloser) und `NetGameSession::m_refusedSpawnClasses`.
- **Code-Auszug**:
```cpp
// RpcRouter.cpp:528-529 + PropertyReplicator.cpp:33-37
const std::string key = std::to_string(netId) + ":" + fn;
if (!alreadyLogged(m_missingLogged, key)) HE_LOG_INFO(...);

bool alreadyLogged(std::vector<std::string>& seen, const std::string& key) {
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) return true;
    seen.push_back(key);
    return false;
}
```
- **Lösung**: Gemeinsamen Helfer `HE::OnceLog` (bounded set) einführen: `std::unordered_set<std::string>` mit Obergrenze (z. B. 256 Einträge); ab der Grenze wird nicht mehr gespeichert und nur noch ein einziges „further names suppressed“ geloggt. Die drei Vektoren und `alreadyLogged` durch diesen Helfer ersetzen.
- **Erwarteter Effekt**: begrenzter Speicher (Schutz vor Hostile-Client-Wachstum), O(1) statt O(n) je Aufruf; Logverhalten im Normalfall identisch.
- **Risiko / Verifikation**: Ausgabe „einmal pro (Entity, Name)“ muss bis zur Grenze identisch bleiben. Test: `tests/test_net_property_replication.cpp` / RPC-Tests um „300 verschiedene Namen → Speicher begrenzt, genau eine Unterdrückungsmeldung“ erweitern.

---

### SCN-410 – `prefs::set*` schreibt bei jedem Aufruf die komplette Datei synchron
- **ID**: SCN-410
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/EngineApi.h:2286-2303` (Doku: „written on every change“); Beleg `src/HE_Scene/src/EngineApi.cpp:4745-4750`, `:4766-4783`
- **Problem**: Jede `prefs.setString/setNumber/setBool/remove/clear`-Zeile ruft `store()`: `doc().dump(2)` (vollständiges, eingerücktes Serialisieren) plus `fs::writeText` (synchrones Schreiben der ganzen Datei) im Frame-Thread. Ein Skript, das während eines Slider-Drags oder pro Frame einen Wert setzt, erzeugt pro Aufruf einen Disk-Schreibvorgang. Die Datei wird dabei nicht atomar (temp + rename wie bei `save::write`) geschrieben; bei einem Absturz mitten im Schreiben ist die Datei beschädigt – das Gegenteil dessen, was der Kommentar („losing the user's settings to a crash“) verhindern soll.
- **Code-Auszug**:
```cpp
// EngineApi.cpp:4745-4750, 4766
void store() { if (!fs::writeText(kFile, doc().dump(2))) HE_LOG_WARN(...); }
void setString(Ctx&, const std::string& key, const std::string& value)
{ doc()[key] = value; store(); }
```
- **Lösung**: Dirty-Flag + verzögertes Schreiben: `setX` setzt nur `g_dirty = true` und merkt die Zeit; geschrieben wird (a) vom Host im Frame-Ende-Hook höchstens alle ~0,5 s (`fs::pollWatches`/`timer::poll` zeigen, dass es bereits einen Pro-Frame-Hook gibt), (b) beim Beenden/Play-Stop sicher (`prefs::flush()`), und (c) atomar über temp+rename. Die Zeile `prefs.save`-Semantik bleibt für Aufrufer unsichtbar.
- **Erwarteter Effekt** (Schätzung): Disk-I/O und JSON-Dump von pro Aufruf auf maximal zwei pro Sekunde; bei häufigem Schreiben von Einstellungen deutlicher Frame-Zeit-Gewinn.
- **Risiko / Verifikation**: Wer unmittelbar nach `setX` die Datei liest (Tests, `fs::readText("Prefs.json")`) sieht sonst den alten Stand → Flush-Punkte (Play-Stop, Shutdown, `fs`-Zugriff auf `Prefs.json`) festlegen und testen. Datenverlust-Fenster (Absturz) wird größer, aber durch atomares Schreiben bleibt die Datei konsistent.

---

### SCN-411 – Schwere Header: `HorizonWorld.h` und `EngineApi.h` ziehen ~3 900 Zeilen HE_Core in 190 bzw. 55 Dateien
- **ID**: SCN-411
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/include/HorizonScene/HorizonWorld.h:6-7,11,316-319`; `EngineApi.h:2-3`; `HcWatchEvents.h:2-3`; `Net/NetEvents.h:2-12`; `Net/NetGameSession.h:31-46`; `HorizonScene.h:1-60`
- **Problem**: `HorizonWorld.h` (inkludiert in ca. 190 Dateien in `src/` und `tests/`) inkludiert `HorizonCode/HorizonCode.h` (1 551 Zeilen), `HorizonCode/HorizonCodeRuntime.h` (886) und `UIWidget/WidgetManager.h` (1 441), weil `HorizonCode::Runtime`, `WidgetManager` und `HorizonCode::Graph` **by value** Member sind (`:316-323`). `EngineApi.h` (55 Dateien) zieht dieselben zwei HorizonCode-Header für `Ctx::runtime` (nur ein Zeiger) und `Value`. `HcWatchEvents.h` inkludiert `EngineApi.h` und `ScriptContext.h`, obwohl es nur Inline-Funktionen über Tokens enthält; `Net/NetEvents.h` und `Net/NetGameSession.h` inkludieren `ScriptEngine.h`, `AntiCheatHost.h`, `ProjectSettings.h` u. a. Jede Änderung an `WidgetManager.h` oder `HorizonCode.h` rebuildet damit den Großteil der Scene-Schicht, der Tests und beider Apps.
- **Code-Auszug**:
```cpp
// HorizonWorld.h:6-11, 316-319
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <UIWidget/WidgetManager.h>
...
HorizonCode::Runtime  m_ownScripts;
WidgetManager         m_ownWidgets;
```
- **Lösung**:
  1. In `HorizonWorld`: `std::unique_ptr<HorizonCode::Runtime> m_ownScripts` und `std::unique_ptr<WidgetManager> m_ownWidgets` (Konstruktor/Destruktor in der `.cpp`), `HorizonCode::Graph m_levelScript` ebenso; im Header nur Vorwärtsdeklarationen. `levelVariables()` (liefert eine `unordered_map<string, Value>`-Referenz, `:276`) in die `.cpp` verlegen.
  2. `EngineApi.h`: `Ctx::runtime` als `HorizonCode::Runtime*` bleibt Vorwärtsdeklaration; `Value`/`PinType` aus einem schlanken `HorizonCode/Value.h` (falls vorhanden/abtrennbar) statt der gesamten `HorizonCode.h`.
  3. `HcWatchEvents.h` und `NetEvents.h` auf Vorwärtsdeklarationen + `.cpp`-Implementierung umstellen oder nur die benötigten Header nennen.
- **Erwarteter Effekt** (Schätzung): spürbar kürzere Inkrementalbuilds (typisch 10–30 % weniger Parse-Last für Dateien, die nur `HorizonWorld` brauchen); keine Laufzeitwirkung.
- **Risiko / Verifikation**: Destruktionsreihenfolge (`m_ownScripts` muss vor den Widgets leben, Kommentar `:315`) bleibt zu beachten; Include-What-You-Use-Lauf bzw. ein voller Build von `he_tests`, `HorizonEditor`, `HorizonGame`. Hot-loaded GameLogic-DLLs: ABI/Layout von `HorizonWorld` ändert sich → neu bauen.

---

### SCN-412 – EntityHost::tick: zwei Durchläufe über eine `unordered_map` plus Snapshot pro Frame
- **ID**: SCN-412
- **Kategorie**: C, D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/EntityHost.h:110`, `:151-155`; Beleg `src/HE_Scene/src/EntityHost.cpp` (`EntityHost::tick`)
- **Problem**: Pro Frame wird `m_byEntity` (`unordered_map<uint32_t, InstanceId>`, knotenbasiert) zweimal komplett iteriert: einmal für den Reap (`registry().valid` je Eintrag) und einmal, um den Tick-Snapshot zu kopieren; danach pro Instanz `m_runtime->alive(inst)` und `fireTick`. Der Snapshot ist wiederverwendet (gut), der Reap ist aber O(Instanzen) pro Frame, obwohl ein Entity-Destroy-Signal (`registry.on_destroy`) den Fall in O(1) pro tatsächlichem Löschvorgang abdecken würde. Der Header selbst nennt die Wiederverwendung als Optimierung (`:153-155`).
- **Code-Auszug**:
```cpp
// EntityHost.cpp (tick)
for (const auto& [raw, inst] : m_byEntity)
    if (!m_world->registry().valid(static_cast<Entity>(raw))) { dead.push_back(inst); ... }
...
for (const auto& [raw, inst] : m_byEntity) m_tickScratch.push_back(inst);
```
- **Lösung**: `m_byEntity` als dichter Vektor `{entity, instance}` mit Index-Map nur für Lookups führen (Iteration cache-freundlich), Reap und Snapshot in einem Durchlauf erledigen (`valid()`-Prüfung und Kopie in dieselbe Schleife). Optional: `on_destroy`-Hook auf `ScriptComponent` markiert Instanzen als tot.
- **Erwarteter Effekt** (Schätzung): gering bei < 1 000 Instanzen; relevant erst bei großen Entity-Klassen-Szenen. Messen, bevor umgesetzt wird.
- **Risiko / Verifikation**: Ablauf „Reap vor Tick, Snapshot statt Live-Map“ ist sicherheitskritisch (Kommentar im Code); Reihenfolge der Ticks darf sich nicht ändern, falls Graphen darauf bauen (bisher hash-order, also nicht garantiert). Tests rund um `EntityHost` (Spawn/Destroy während Tick).

---

### SCN-413 – Input-Snapshot: pro Frame `vector<string>` + `unordered_set<string>` neu aufgebaut; Gamepad-Namen werden pro Abfrage geparst
- **ID**: SCN-413
- **Kategorie**: C, D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/EngineApi.h:2800-2838`; Beleg `src/HE_Scene/src/EngineApi.cpp:5738-5800`, `:5918-5931`
- **Problem**: `pushSdlSnapshot` läuft laut Header jeden Frame im Game: Es iteriert über alle SDL-Scancodes (ca. 500), baut einen `std::vector<std::string>` der gedrückten Tasten (Namen per `SDL_GetScancodeName`), und `setKeysDown` leert danach ein `std::unordered_set<std::string>` und fügt jeden Namen wieder ein (Knoten-Allokation pro gedrückter Taste und Frame). `keyDown(name)` hasht den Namen pro Abfrage; `gamepadButton`/`gamepadAxis` rufen pro Abfrage `SDL_GetGamepadButtonFromString` (String-Parsing). `findAction` in `actionDown/actionPressed/…` ist eine lineare Namenssuche über die Aktionsliste pro Abfrage.
- **Code-Auszug**:
```cpp
// EngineApi.cpp:5754-5757, 5762-5772
void setKeysDown(const std::vector<std::string>& names)
{ Snapshot& s = snap(); s.keys.clear(); for (const auto& n : names) s.keys.insert(n); }
void pushSdlSnapshot(float dx, float dy) {
    ...
    std::vector<std::string> down;
    for (int sc = 0; sc < n; ++sc)
        if (ks[sc]) { const char* name = SDL_GetScancodeName((SDL_Scancode)sc); ... down.emplace_back(name); }
```
- **Lösung**: Zustand als `std::bitset<SDL_SCANCODE_COUNT>`/`bool[512]` im Snapshot halten; `keyDown(name)` löst den Namen einmalig über eine statische `unordered_map<string_view, SDL_Scancode>` (beim ersten Aufruf gebaut) in einen Scancode auf. Gamepad-Namen analog in zwei kleine statische Tabellen. `setKeysDown(vector<string>)` bleibt als Kompatibilitäts-Pfad für Tests/Editor.
- **Erwarteter Effekt** (Schätzung): einige Allokationen und Hashes pro Frame weniger; klein, aber im Game-Loop konstant.
- **Risiko / Verifikation**: Namenssemantik („SDL scancode names“, Groß-/Kleinschreibung) exakt erhalten; Tests `tests/test_input_gamepad.cpp`, `tests/test_scripting.cpp`.

---

### SCN-414 – Poll-APIs der PhysicsWorld liefern `std::vector` pro Aufruf und Frame
- **ID**: SCN-414
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/PhysicsWorld.h:170`, `:738`, `:793`, `:806`, `:813-814`; Abfragen `:484`, `:532`, `:538`, `:560`
- **Problem**: `pollCollisionEnter/Exit`, `pollOverlapEnter/Exit`, `pollJointBroken`, `pollWaterEvents` geben jeweils einen neuen `std::vector` zurück und werden laut Doku „every frame“ aufgerufen. Bei Kontaktaktivität bedeutet das mehrere Heap-Allokationen pro Frame (sechs Vektoren) statt einer wiederverwendeten Liste. Dasselbe gilt für `overlapSphere/Box/Capsule`/`raycastAll` pro Skript-Abfrage (dort aber sachgerecht).
- **Code-Auszug**:
```cpp
// PhysicsWorld.h:793, 806, 813-814
std::vector<CollisionEvent> pollCollisionEnter();
std::vector<CollisionEvent> pollCollisionExit();
std::vector<CollisionEvent> pollOverlapEnter();
std::vector<CollisionEvent> pollOverlapExit();
```
- **Lösung**: Zusätzliche Überladung `void drainCollisionEnter(std::vector<CollisionEvent>& out)` (Tausch mit internem Puffer via `out.clear(); std::swap(out, m_events)`), vom `CollisionSystem` mit einem Member-Vektor aufgerufen; die Wert-Rückgabe bleibt als dünner Wrapper für Tests/Skripte.
- **Erwarteter Effekt** (Schätzung): bei Frames ohne Kontakte keiner (leere Vektoren allokieren nicht); bei Kontaktstürmen eine bis sechs Allokationen pro Frame weniger.
- **Risiko / Verifikation**: Semantik „drained, jedes Event genau einmal“ und Budget `eventsPerPoll` (`PhysicsBudget.h:35`) bleiben; `tests/test_physics_budget.cpp`.

---

### SCN-415 – Heap pro Entity bei Anlegen einiger Komponenten (Default-Member mit Allokation)
- **ID**: SCN-415
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `Components/SkeletalMeshComponent.h:11`, `Components/ScriptComponent.h:15`, `Components/RopeComponent.h:32`, `Components/UIButtonComponent.h:12`
- **Problem**: `SkeletalMeshComponent::boneMatrices = { glm::mat4(1.0f) }` allokiert bei **jeder** Konstruktion (auch beim Deserialisieren, wo der Vektor gleich danach ersetzt wird) 64 Byte auf dem Heap; derselbe Vektor wird beim Setzen durch den Animator ein zweites Mal neu dimensioniert. `ScriptComponent::properties` ist eine `unordered_map<string, ScriptPropValue>` pro Entity: libstdc++ allokiert für eine leere Map nichts, **MSVC allokiert beim Default-Konstruieren einen Sentinel-Knoten** (unsicher / messen auf Windows). `RopeComponent::controlPoints` hat einen Default mit zwei Punkten (Heap pro Rope, vertretbar), `UIButtonComponent::onClickFunction` ist ein `std::string` (SSO, kein Problem).
- **Code-Auszug**:
```cpp
// SkeletalMeshComponent.h:11
std::vector<glm::mat4> boneMatrices  = { glm::mat4(1.0f) };
// ScriptComponent.h:15
std::unordered_map<std::string, ScriptPropValue> properties;
```
- **Lösung**: `boneMatrices` leer lassen und die Konsumenten (`RenderExtractor`, Animationssysteme) „leer = Bind-Pose = eine Identität“ behandeln lassen, oder den Default lazy beim ersten Zugriff setzen; `ScriptComponent::properties` zu `std::vector<std::pair<std::string, ScriptPropValue>>` oder einem optionalen, erst bei Bedarf angelegten Container machen (Eigenschaftenzahl pro Skript ist klein).
- **Erwarteter Effekt** (Schätzung): bei 100 k Skelett-Entities beim Laden 100 k kleine Allokationen weniger; sonst gering.
- **Risiko / Verifikation**: Konsumenten, die `boneMatrices[0]` annehmen, würden bei leerem Vektor lesen – alle Zugriffe prüfen (Grep `boneMatrices`). Szenen-Roundtrip-Tests (`tests/test_scene_*.cpp`) und Extractor-Tests.

---

### SCN-416 – Zwei überlappende API-Schichten (ScriptApi und HE::api) und tote `cppCall`-Metadaten
- **ID**: SCN-416
- **Kategorie**: A, F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/include/HorizonScene/ScriptApi.h:19-204` gegenüber `EngineApi.h:224-324`, `:327-378`, `:1001-1018`, `:1029-1132`; `EngineApi.h:3031-3036` (`cppCall`); `:112-117` (Ctx-Positionsregel)
- **Problem**: `ScriptApi` (Doku: „single implementation behind the `horizon` module“) und `HE::api` (Doku: „single, engine-wide C++ gameplay API“) deklarieren dieselben Operationen (Name, Position/Rotation/Skalierung, `spawn/destroy`, UI-Text/Farbe/Position, ~25 Widget-Funktionen, Theme, Cursor). Die `HE::api`-Zeilen sind in 70 Fällen nur Weiterleiter an `ScriptApi::…` (`EngineApi.cpp:247-249`, `:634-637`, …); die Lua-Bindings rufen teils `ScriptApi`, teils `HE::api` (`ScriptContext.cpp:117-447`). Jede Operation existiert dadurch dreifach (Deklaration `ScriptApi`, Deklaration `HE::api`, Registry-Zeile) plus die Binding-Shims. `ApiFn::cppCall` ist laut Header-Kommentar „nothing emits it today“ (`:3033`), kostet aber einen String-Zeiger pro Zeile und beim Pflegen eine zusätzliche Redundanz, die nur ein Invarianten-Test absichert. Die Aussage in `EngineApi.h:3055-3058` („until ScriptApi is inverted onto HE::api“) bestätigt den Übergangszustand.
- **Code-Auszug**:
```cpp
// EngineApi.h:3036
const char* cppCall;   // "nothing emits it today"
// EngineApi.cpp:247
std::string getName(Ctx& c, Entity e) { return c.world ? ScriptApi::getName(*c.world, e) : std::string(); }
```
- **Lösung**: Die Inversion zu Ende führen (Lua/Python-Shims rufen ausschließlich `HE::api`, `ScriptApi.cpp` schrumpft auf interne Helfer oder verschwindet), und `cppCall` entfernen, bis der Codegen es tatsächlich benutzt (oder es per Makro nur in Test-Builds führen). Reihenfolge: zuerst Transform/Entity/UI (dünne Weiterleiter), dann Widgets.
- **Erwarteter Effekt**: weniger Redundanz (~500 Zeilen `ScriptApi.cpp` + Header), eine Stelle pro neuer Funktion; keine Laufzeitänderung.
- **Risiko / Verifikation**: Python-Backend (`PyScriptBackend`) und der CPython-Plugin-ABI-Pfad nutzen die gleichen Funktionen – Kompatibilität prüfen (`tests/test_python_scripting.cpp`, `tests/test_scripting_binding.cpp`, `tests/test_engine_api.cpp`).

---

### SCN-417 – Prefab-Index mit linearer Suche; Clipboard-Check parst bei jedem Menü-Frame das ganze JSON
- **ID**: SCN-417
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/include/HorizonScene/SceneSerializer.h:303-317` (`PrefabRecordIndex::find`), `:424-427` (`componentKeyOfText`); Beleg `src/HE_Scene/src/SceneSerializer.cpp:3613-3626`
- **Problem**: (1) `PrefabRecordIndex::find` ist eine lineare Suche über `records`; der Kommentar sagt, der Inspector frage jeden Frame nach der Struktur (`:300-303`), `prefabStructureOf`/`prefabRemovedHereCount` rufen `find` pro Binding → O(n²) bei großen Prefabs. (2) `componentKeyOfText` ist „cheap enough to ask every frame a menu is open“ (`:425-426`), parst aber nach einem `find` auf den Envelope-Schlüssel das gesamte Clipboard als JSON; kopiert man eine Terrain-Komponente (Base64-Höhenfeld, mehrere MB), wird dieses Parsen pro Frame wiederholt, solange das Menü offen ist. Beides läuft nur im Editor.
- **Code-Auszug**:
```cpp
// SceneSerializer.h:311-316
const Record* find(const HE::UUID& uuid) const
{
    for (const Record& r : records)
        if (r.uuid == uuid) return &r;
    return nullptr;
}
// SceneSerializer.cpp:3617-3618
if (text.size() < 2 || text.find(kComponentEnvelopeKey) == std::string::npos) return {};
const json envelope = json::parse(text, nullptr, false);
```
- **Lösung**: `PrefabRecordIndex` bekommt beim Aufbau (`indexPrefabRecords`) eine `unordered_map<UUID, uint32_t>` (oder sortierten Index); `componentKeyOfText` liest nur die ersten ~200 Bytes des Textes per Mini-Scanner nach `"horizonComponent": "<key>"` und parst erst bei `importComponentText` voll; zusätzlich Ergebnis pro Clipboard-Inhalt (Hash/Länge + Zeiger) cachen.
- **Erwarteter Effekt**: Editor-Only; kleine ms-Spitzen bei großen Prefabs/Clipboards entfallen.
- **Risiko / Verifikation**: Mini-Scanner muss exakt dieselben Texte akzeptieren/ablehnen wie `json::parse` (Escapes, Reihenfolge der Keys) – mit Tests in `tests/test_scene_serializer*.cpp`, die die aktuelle Akzeptanzmenge pinnen; im Zweifel nur das Caching umsetzen.

---

### SCN-418 – Toter API-Code in `HorizonWorld`: `hasComponent`/`removeComponent` ohne Aufrufer, `decltype(component)`-Trick
- **ID**: SCN-418
- **Kategorie**: A, F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/include/HorizonScene/HorizonWorld.h:197-209`
- **Problem**: `hasComponent(Entity, auto&& componentType)` und `removeComponent(Entity, auto&& componentType)` erwarten eine **Instanz** der Komponente (`std::decay_t<decltype(componentType)>`) nur, um daraus den Typ abzuleiten – man muss also ein Objekt konstruieren, um zu fragen, ob eine Entity es hat. Ein Grep über `src/` und `tests/` findet für beide Funktionen **keinen einzigen Aufrufer**; `addComponent` hat ca. 620 Aufrufer. Die API ist damit toter Code, der außerdem unnatürlich zu benutzen wäre; `registry().all_of<T>(e)` bzw. `.remove<T>(e)` sind die tatsächlich verwendeten Wege.
- **Code-Auszug**:
```cpp
bool hasComponent(Entity entity, auto&& componentType)
{ return m_registry.all_of<std::decay_t<decltype(componentType)>>(entity); }
```
- **Lösung**: `hasComponent` und `removeComponent` löschen. Für `addComponent` ein typbasiertes `template<class T, class... A> T& add(Entity e, A&&... a)` ergänzen (`emplace<T>`), das auch die Referenz zurückgibt, und `addComponent` nur als dünnen Alias behalten.
- **Erwarteter Effekt**: weniger API-Fläche; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Grep in `vendor`-fremdem Projekt-/Plugin-Code und in GameLogic-Templates (`ProjectTemplates`) auf externe Nutzung; Build aller Targets.

---

### SCN-419 – Veraltete bzw. verrutschte Kommentare
- **ID**: SCN-419
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `Components/SequencePlayerComponent.h:66-68`; `HorizonWorld.h:181-183`; `Components/TerrainComponent.h:7-16`; `Net/NetGameSession.h:329-337`
- **Problem**:
  1. `SequencePlayerComponent.h:66-68` sagt, die Slot-Auflösung sei „a scan over every EntityIdComponent“ und deshalb nicht pro Frame – `HorizonWorld::findByEntityId` ist aber seit dem Id-Index O(1) (`HorizonWorld.h:40-47`, `SequenceEval.h:88-91`). Die Begründung für die „lazy, einmal“-Auflösung stimmt nicht mehr; die Vorsicht wegen eines noch streamenden Assets bleibt der eigentliche Grund.
  2. `HorizonWorld.h:181-183`: „environmentEntity() / weatherEntity() … (scans the registry)“ – tatsächlich liest `view<EnvironmentComponent>` das erste Element des Pools (`HorizonWorld.cpp:322-336`), also O(1).
  3. `TerrainComponent.h:7-8`: der Kommentar „Paintable landscape layers: two RGBA8 weightmap pages…“ steht über dem Text zu `kTerrainMaxResolution` und gehört zu `kTerrainWeightPages` (Zeile 17).
  4. `NetGameSession.h:329-337`: der Block „Hand the RUNTIME the door back into the session…“ beschreibt `installRpcHook()`/`setVariableSource`, steht aber vor `localCharacter()`; ein Leser bezieht ihn auf die falsche Funktion.
- **Code-Auszug**:
```cpp
// SequencePlayerComponent.h:66-68
// Slot → entity, resolved at play() and on bindSlot. Not per frame: the
// lookup is a scan over every EntityIdComponent.
```
- **Lösung**: Kommentare korrigieren bzw. an die richtige Deklaration verschieben; kein Code-Effekt.
- **Erwarteter Effekt**: weniger Fehlinformation beim Lesen (insbesondere Punkt 1 verleitet dazu, die Auflösung „aus Performancegründen“ nie zu wiederholen).
- **Risiko / Verifikation**: keines.

---

### SCN-420 – `HcCodegen.h`: `std::function` ohne `<functional>`, Toolchain-/Prozessverwaltung im Scene-Header
- **ID**: SCN-420
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/include/HorizonScene/HcCodegen.h:2-5`, `:64`, `:197-198`, `:211-212`
- **Problem**: Der Header benutzt `std::function` (`Options::onClass`, `installToolchain`, `buildDylib`), inkludiert aber nur `HorizonCode/HorizonCode.h`, `<filesystem>`, `<string>`, `<vector>`. Er kompiliert nur, weil `HorizonCode.h` transitiv `<functional>` bringt. Inhaltlich mischt der Header Codegen (`generate`), Toolchain-Erkennung/Installation (`probeToolchain`, `installToolchain` mit winget/brew/pkexec), Visual-Studio-Auswahl und GameLogic-Build (`gameLogicSpec`). Das ist Editor-/Tool-Infrastruktur, hat keine Abhängigkeit zum ECS, liegt aber in `HE_Scene/include/HorizonScene` und wird von `hc_codegen`, Editor und Tests inkludiert.
- **Code-Auszug**:
```cpp
// HcCodegen.h:2-5, 64
#include <HorizonCode/HorizonCode.h>
#include <filesystem>
#include <string>
#include <vector>
...
std::function<void(const std::string& label, size_t index, size_t count)> onClass;
```
- **Lösung**: `#include <functional>` ergänzen; mittelfristig Toolchain-Teil (`probeToolchain` … `gameLogicSpec`, ~150 Zeilen) in einen eigenen Header (`HorizonScene/HcToolchain.h` oder ins Tools-Modul) verschieben.
- **Erwarteter Effekt**: robustere Includes; klarere Modulgrenze.
- **Risiko / Verifikation**: Include-Pfade in `src/HE_Tools/hc_codegen`, `src/HE_Editor` und Tests (`tests/test_hc_codegen*.cpp`) anpassen; voller Build.

---

## Cross-Modul-Hinweise

- `src/HE_Scene/src/PhysicsWorld.cpp:3231-3470` (`PhysicsWorld::step`): Der Write-Back iteriert pro Schritt über **alle** Einträge von `entityToBody` (knotenbasierte `unordered_map`, plus Kopie in `stepBodies`), prüft `registry.valid`, `TryGetBody`, Motion-Type und schreibt für jeden nicht-statischen Körper (auch schlafende) die Transform zurück (`writeBackWorldPose` inkl. Welt→Lokal-Umrechnung). Skip für schlafende Körper (`!body->IsActive()`) würde die in `EngineApi.h:331-335` dokumentierte Eigenschaft ändern, dass der Step die Transform jedes Frames überschreibt – nur mit Test und Absicht ändern. Gehört in den Physik-Slice.
- `src/HE_Scene/src/TransformHierarchy.cpp:321-367` (`worldMatrixOf`): Wird von `LODSystem`, `RopeTrailSystem::resolveControlPoints`, `PhysicsWorld` (Joints), `EngineApi` (jede `transform.*`-Zeile) pro Aufruf frisch komponiert. Ein generationsbasierter Cache (invalidiert durch `propagateTransforms`) würde SCN-402 und SCN-404 gleichzeitig entlasten – Entscheidung für den Transform-Slice.
- `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp` (`Runtime::fireEvent`/`dispatchToListeners`): Jeder Aufruf hasht/interniert den Event-String (`eventId(name)`); relevant für SCN-408.
- `src/HE_Core/include/UIWidget/WidgetManager.h` (1 441 Zeilen) und `HorizonCode/HorizonCode.h` (1 551 Zeilen) werden über `HorizonWorld.h` in fast jede Scene-Datei gezogen (SCN-411); eine Aufteilung in „Typen“ und „Manager“ im Core wäre die Wurzellösung.
- Editor: `src/HE_Editor/InspectorPanel.cpp:4041` ruft `componentKeyOfText` im Menü-Pfad auf (SCN-417).
- `src/HE_Game/src/GameApplication.cpp` / `src/HE_Editor/EditorApplication.cpp`: beide rufen `pushSdlSnapshot`, `PlayerHost::tick`, `NetGameSession::update` pro Frame; die Frame-Reihenfolge ist im Header teils nur als Prosa dokumentiert (`SceneSystems.h:43-54`) – ein gemeinsames `HE::Frame`-Modul wäre Entscheidung für den App-Slice.

## Unsicher / Messung nötig

- SCN-406: Wie viele HE::api-Aufrufe pro Frame laufen tatsächlich über `invoke` (Interpreter vs. kompilierte Klassen, die laut `HcCodegen` Direktpfade haben)? Profiler-Scope um `ApiFn::invoke` einbauen.
- SCN-407: Ob die `std::function`-Kopien in `apiCtx` heap-allokieren, hängt von der Capture-Größe der Host-Lambdas und der STL ab (libstdc++ SSO 16 Byte, MSVC 56 Byte); Mikrobenchmark nötig.
- SCN-402: Anteil der LOD-Phase am Frame bei typischen Szenen (Terrain 256 Chunks + Props) aus dem Profiler (`HE_PROFILE_SCOPE_N`) ablesen; `docs/world-streaming-baseline-2026-10-06.md` §4.5 nennt den Posten, aber ohne aktuelle Zahl.
- SCN-415: `std::unordered_map` Default-Konstruktor auf MSVC (Sentinel-Allokation) – nur auf Windows prüfen; ggf. `ScriptComponent` beim Laden von 100 k Entities profilieren.
- `TransformComponent` (`Components/TransformComponent.h:19-54`) ist ca. 200 Byte (zwei `mat4`, vier `vec3`, Flags) in einem dichten Pool, obwohl viele Leser nur `position/rotation/scale` brauchen. Aufteilen von `worldMatrix`/`localCache` in einen eigenen Pool (SoA) könnte Cache-Misse in Physik-Write-Back und Gameplay senken; nur mit Messung (großer Umbau, viele Aufrufer).
- `WeatherComponent` (`Components/WeatherComponent.h:143-145`) trägt einen `std::mt19937` (~2,5 KB) und einen Partikelvektor inline; da es nur ein Weather-Entity gibt, ohne Wirkung, aber der Pool-Platz pro Instanz ist unnötig groß, falls je mehrere Instanzen erlaubt werden.
- X-Macro-Listen für Environment-Felder (`EngineApi.h:1452-1513`): vier Listen, die an mindestens vier Stellen expandiert werden (Deklaration, Implementierung, Registry, Anzeigenamen). Das ist bewusst „single source of truth“; ein constexpr-Array von Deskriptoren wäre lesbarer, aber ohne Typsicherheit der generierten Getter/Setter – kein Handlungsbedarf ohne Anlass.
- SCN-403: Ob `GameReplication::sendSnapshots` bei realistischen Spielerzahlen (≤ 8) und Entities (≤ 2 000) tatsächlich messbar ist, hängt vom Spiel ab; Stats `entitiesSent/entitiesCulled` liefern die Größenordnung.

## Gelesen

Ja, alle 70 Dateien des Slice 4 (`docs/audit/_slices/Scene.txt`, Abschnitt `## slice 4`) wurden vollständig gelesen, `EngineApi.h` (3 094 Zeilen) in fünf aufeinanderfolgenden Blöcken ohne Auslassung; die zugehörigen `.cpp`-Dateien (`PropertyReplicator.cpp`, `RpcRouter.cpp`, `GameReplication.cpp`, `RopeTrailSystem.cpp`, `LODSystem.cpp`, `PlayerHost.cpp`, `EntityHost.cpp`, `ScriptContext.cpp`, `HorizonWorld.cpp`, Teile von `EngineApi.cpp`, `PhysicsWorld.cpp`, `TransformHierarchy.cpp`, `SceneSerializer.cpp`) wurden nur gezielt als Beleg gelesen, nicht vollständig.
