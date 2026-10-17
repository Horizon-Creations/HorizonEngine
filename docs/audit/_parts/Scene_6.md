# Scene Slice 6 – PhysicsWorld.cpp … SceneJsonParse.cpp

## Zusammenfassung

Der Slice (PhysicsWorld, PlayerHost/-Settings, PoseFinalize/PoseSource, PropertyAnimationSystem, RootMotion*, RopeTrailSystem, SceneJsonParse) ist insgesamt sorgfältig und gut kommentiert. Gefunden wurden vor allem Skalierungsprobleme bei vielen Entities: `PhysicsWorld::step` besucht pro Schritt jeden Body (auch statische und schlafende), der Contact-Listener nimmt für jedes Kandidaten-Paar einen globalen Mutex, und Collider-Shapes werden pro Entity neu gebaut, nie geteilt. `RopeTrailSystem` und die Pose-/Root-Motion-Pfade arbeiten ohne Dirty-Flags und mit Heap-Allokationen pro Entity und Frame. Insgesamt 21 Funde: 4 Hoch, 11 Mittel, 6 Niedrig. `PlayerSettings.cpp` ist unauffällig (kein Hot Path, kein Fund). Es gibt keine Messdaten; Schätzungen sind als solche gekennzeichnet.

---

### SCN-601 – step(): Write-back-Schleife besucht jeden Body, auch statische und schlafende
- **ID**: SCN-601
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:3312-3406 (Snapshot, Reap, Write-back), 1002-1021 (`writeBackWorldPose`)
- **Problem**: Pro Physik-Schritt wird die komplette `entityToBody`-Map (`std::unordered_map`, knotenbasiert, cache-unfreundlich) in einen Vektor kopiert und durchlaufen. Dort stehen pro Body `reg.valid`, `TryGetBody` und der Motion-Type-Check. Die Schleife ist O(alle Bodies), obwohl meist nur wenige aktiv sind (in einem Level überwiegen statische Bodies, die nur wegen des „Reap" besucht werden). Dynamische **schlafende** Bodies werden nicht übersprungen: nur `Static` und gehaltene Bodies fallen raus. Für sie laufen `reg.all_of<CharacterControllerComponent>`, `try_get<TransformComponent>`, `localPositionForWorld` (Parent-Walk), bei Kindern zusätzlich `worldPoseOf(parent)` samt `quat_cast`/`normalize`, dazu `glm::eulerAngles` (Trigonometrie). Am Ende steht `transform.dirty = true` (Z. 1020). Damit wird jeder schlafende Body jeden Schritt als geändert markiert, und die Transform-Propagation und alles, was auf `dirty` hört, arbeiten für ruhende Objekte. Der Kommentar bei Z. 3300-3303 nennt selbst „ein Fünftel eines 20 000-Body-Schritts".
- **Code-Auszug**:
```cpp
m_impl->stepBodies.clear();
m_impl->stepBodies.reserve(m_impl->entityToBody.size());
for (const auto& entry : m_impl->entityToBody)
    m_impl->stepBodies.emplace_back(entry.first, entry.second);
for (const auto& [entityId, bodyId] : m_impl->stepBodies) {
    ... if (!reg.valid(entity)) { removeEntity(entityId); continue; }
    ... if (body->GetMotionType() == JPH::EMotionType::Static) continue;
    ... writeBackWorldPose(world, entity, *transform, ...);   // setzt transform.dirty = true
```
- **Lösung**:
  1. Write-back nur für aktive Bodies: Jolt liefert die Liste (`PhysicsSystem::GetActiveBodiesUnsafe(EBodyType::RigidBody)` und `GetNumActiveBodies`; Signatur gegen die Vendor-Version prüfen). Die Entity-ID kommt aus `body->GetUserData()`, die Map-Iteration entfällt. Wichtig: ein Body, der in diesem Schritt einschläft, war im Schritt davor aktiv, seine Endpose ist also schon geschrieben.
  2. Reap von Entities, die gelöscht wurden, nicht mehr pro Schritt über alle Bodies: `reg.on_destroy<RigidBodyComponent>()` bzw. `<TerrainComponent>` / `<CharacterControllerComponent>` setzt nur ein Flag oder merkt die ID (kein ContentManager-Zugriff im Listener nötig). Alternative mit weniger Risiko: den vollen Reap-Durchlauf nur alle N Schritte oder wenn `reg.storage<RigidBodyComponent>().size() < entityToBody.size()` ausführen.
  3. `dirty` nur setzen, wenn sich Position/Rotation tatsächlich ändern (Vergleich mit dem Alt-Wert).
- **Erwarteter Effekt**: (Schätzung) Bei 20 000 Bodies, davon wenige aktiv, fällt der Schritt-Overhead außerhalb von `Update()` von O(20 000) auf O(aktive). Für schlafende Szenen vermutlich Faktor 10-100 in diesem Abschnitt. Messen.
- **Risiko / Verifikation**: Verhaltensänderung: Skripte, die die Transform eines schlafenden Physik-Bodies direkt ändern, wurden bisher jeden Schritt von der Jolt-Pose überschrieben; nachher bleibt ihre Änderung bis zum Aufwecken stehen. Tests: `tests/` mit Physik-Sync (kinematisch, geparentet, Character+Proxy), Entity-Löschen mitten im Spiel (Raycast darf keine tote ID liefern), schlafender Body mit Transform-Änderung. Reihenfolge-Kommentar bei Z. 3393-3400 (Parent/Kind) bleibt gültig.

---

### SCN-602 – Contact-Listener: globaler Mutex pro Kandidaten-Paar und pro Contact
- **ID**: SCN-602
- **Kategorie**: E
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:214-224 (`OnContactValidate`), 236-264 (`OnContactAdded`), 266-280 (`OnContactRemoved`), 454-462
- **Problem**: `OnContactValidate` wird von allen Jolt-Workern für jedes neue Body-Paar aufgerufen und nimmt **immer** `m_mutex`, bevor überhaupt geprüft wird, ob `m_noCollidePairs` leer ist (der Normalfall ohne Joints). Das ist ein einziger Lock über alle Worker, den der Kommentar „uncontended in practice" (Z. 226-228) nur für die Schreibseite meint. `OnContactAdded`/`OnContactRemoved` serialisieren zusätzlich auf demselben Mutex und legen pro Contact einen `unordered_map`-Knoten an bzw. löschen ihn (Heap pro Kontakt in dichten Haufen). Mit `threads > 1` skaliert das Narrow-Phase-Ergebnis dadurch nicht.
- **Code-Auszug**:
```cpp
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_noCollidePairs.empty() &&
        m_noCollidePairs.count(bodyPairKey(b1.GetID(), b2.GetID())) != 0)
        return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
}
```
- **Lösung**:
  1. Validate lock-frei vorfiltern: `std::atomic<uint32_t> m_noCollideCount` (in `setPairCollides` unter Lock mitgeführt). Bei 0 sofort `AcceptAll...` ohne Lock. Verhalten identisch, da `setPairCollides` nie während `Update()` läuft (so dokumentiert).
  2. Für den Fall mit Joints: eigener `std::shared_mutex` oder ein während `Update()` unveränderliches Snapshot-Set (Jolt-Callbacks laufen nur innerhalb von `Update`, Schreiber nur davor/danach), dann ganz ohne Lock lesen.
  3. Added/Removed: pro Worker thread-lokale Puffer (`thread_local` oder Array pro Job-Index) und nach `Update()` in `step()` zusammenführen, statt jeden Contact unter dem globalen Lock zu verbuchen. Optional `m_active` als flache Hash-Map (offene Adressierung) statt Knoten-Map.
- **Erwarteter Effekt**: (Schätzung) Für (1) praktisch kostenlos und entfernt den Lock aus dem häufigsten Callback; für (3) spürbar in Szenen mit tausenden Kontakten pro Schritt und 4+ Threads. Messen mit `PhysicsWorld::stepStats` und einer Pile-Szene (`docs/physics-threading-baseline.md` nennt die Referenzszene).
- **Risiko / Verifikation**: (1) trivial. (3) betrifft die Ereignis-Reihenfolge: `take()` sortiert bereits nach Entity-Paar (Z. 438), die Ausgabe bleibt deterministisch, der Ref-Count (enter bei erstem, exit beim letzten Sub-Shape-Contact) muss zusammenführbar bleiben. Bestehende Collision-Event-Tests (Enter/Exit, Overlap, Limit/Carry-Backlog) müssen unverändert grün sein.

---

### SCN-603 – Collider-Shapes werden pro Entity neu gebaut und nie geteilt
- **ID**: SCN-603
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:1308-1446 (`buildColliderShape`), 1119-1161 (`buildMeshShape`), 1166-1209 (`buildConvexHullShape`), 1719, 1769
- **Problem**: `buildBodyFor` ruft für jede Entity `buildColliderShape` auf, das für Mesh/ConvexHull alle Vertices in `JPH::VertexList`/`Array<Vec3>` kopiert und eine neue `MeshShape` (BVH-Aufbau) bzw. Hull (Quickhull) erzeugt. 500 identische Häuser oder Felsen (gleiches Mesh, gleiche Skalierung) ergeben 500 identische Shapes im RAM und 500 Aufbauzeiten beim Laden und beim Streaming. Jolt-Shapes sind unveränderlich und referenzgezählt, also frei teilbar. Der Hull-Pfad versucht bei Überschreitung des 256-Punkte-Limits bis zu 3 Mal (`{1e-3, 0.05, 0.25}`) einen kompletten Aufbau aus allen (nicht dedupierten) Punkten. Auch Box/Sphere/Capsule erzeugen pro Entity ein neues Heap-Objekt.
- **Code-Auszug**:
```cpp
out.result = wantsMesh ? buildMeshShape(*mesh, worldScale)
                       : buildConvexHullShape(*mesh, worldScale, entityId);
...
for (const float tolerance : { 1.0e-3f, 0.05f, 0.25f }) {
    JPH::ConvexHullShapeSettings settings(points);
```
- **Lösung**: Shape-Cache in `Impl`, Schlüssel `(kind, meshAssetId, scale bitweise, ggf. Winding-Flip, halfExtents/radius/height bei Primitiven)` auf `JPH::Ref<JPH::Shape>`. `buildColliderShape` liefert bei Treffer `ShapeResult` aus dem Cache (Refcount). Der Cache wird in `clear()` geleert; alternativ schwache Einträge (nur halten, solange `GetRefCount() > 1`). Für den Hull-Pfad vorher Punkte dedupen (gequantisiert) und die Toleranzleiter beginnen lassen, wo sie bei gleichem Asset zuletzt erfolgreich war.
- **Erwarteter Effekt**: (Schätzung) Ladezeit und RAM für wiederholte Meshes sinken ungefähr um den Faktor der Instanzzahl; bei Streaming-Cells mit vielen identischen Props deutlich. Messen: Zeit und Speicher von `initialize()`/`addEntities` mit N identischen Mesh-Collidern.
- **Risiko / Verifikation**: Geteilte Shape ist beobachtbar identisch, solange `mustBeStatic`/Fallback-Entscheidungen mit im Cache-Wert stehen (Fehlerfälle nicht cachen oder mit Fehlerstatus). Pro Body unterschiedliche Daten (Material, UserData) liegen in `BodyCreationSettings`, nicht in der Shape. Wenn Content hot-reloaded wird (Mesh ersetzt), muss der Cache über die Asset-ID plus Revision invalidiert werden. Tests: zwei Entities, gleiches Mesh, gleiche und verschiedene Skalierung, gespiegelte Skalierung (Winding).

---

### SCN-604 – RopeTrailSystem: pro Frame Kopie, Hash und Aufräum-Sweep für jedes Seil, ohne Dirty-Flag
- **ID**: SCN-604
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/RopeTrailSystem.cpp:283-364, 63-87, 266-281, 47-56
- **Problem**: `update()` läuft jeden Frame (SceneSystems.cpp:147) und tut für **jede** `RopeComponent`, auch für unveränderte:
  1. `resolveControlPoints` kopiert `rope.controlPoints` (Heap-Allokation pro Seil und Frame, Z. 66), selbst wenn keine Attachments existieren und die Kopie unverändert zurückgegeben wird.
  2. `geometryHash` hasht bytewise (FNV-1a, 1 Multiplikation pro Byte) alle Punkte plus Parameter jeden Frame.
  3. Bei Attachments: `worldMatrixOf` + `glm::inverse` + 2× `findByEntityId` + 2× `worldPositionOf` pro Seil und Frame.
  4. `owned.find` und `cm.getStaticMesh(it->second)` (Hash-Lookup im ContentManager) pro Seil und Frame.
  5. Danach läuft immer der Sweep über **alle** `owned`-Einträge (`reg.valid` + `try_get`), obwohl sich die Seilmenge nur selten ändert.
  Bei Rebuild (z. B. ein Seil, das an eine bewegte Entity gebunden ist, also jeden Frame) kopiert `toStaticMesh` vier Vektoren aus `mesh`, das danach nicht mehr gebraucht wird (Z. 331, 339).
- **Code-Auszug**:
```cpp
for (auto [e, rope] : reg.view<RopeComponent>().each()) {
    const std::vector<glm::vec3> pts = resolveControlPoints(world, e, rope);   // Kopie
    const uint64_t hash = geometryHash(rope, pts);                              // bytewise FNV
    ...
    if (ours && hash == rope.builtHash) continue;
```
- **Lösung**:
  1. `resolveControlPoints` für den Fall ohne Attachments als Referenz auf `rope.controlPoints` liefern (z. B. `const std::vector<glm::vec3>&` plus optionaler Scratch-Vektor in einer wiederverwendeten Struktur, nur bei Attachment befüllt).
  2. Hash wortweise (64-bit XOR/Multiply pro 8 Byte) oder nur über geänderte Eingaben: Komponenten-Revision, die der Inspector/Setter hochzählt (`rope.revision`), plus beim Attachment den Vergleich der Endpunkte statt Neuhash aller Punkte.
  3. Sweep nur bei Strukturänderung: `reg.on_destroy<RopeComponent>().connect` setzt lediglich ein `bool g_ropesChanged` (kein ContentManager-Zugriff, womit der Einwand im Kommentar Z. 345-350 nicht greift), oder `owned.size() != storage.size()`. Entity-Handle-Recycling bleibt über den ID-Vergleich im Sweep abgedeckt.
  4. `toStaticMesh(HE::spline::MeshData&&)` mit `std::move` der vier Vektoren.
- **Erwarteter Effekt**: (Schätzung) Pro statischem Seil sinken die Kosten von Kopie + ca. `12·N` Hash-Schritten + 2 Hash-Lookups auf nahezu 0 pro Frame. Relevant ab dutzenden Seilen/Kabeln (Leitungen, Brücken); bei einzelnen Seilen im Mikrobereich. Messen mit dem `RopeTrail`-Profil-Scope.
- **Risiko / Verifikation**: Revisionszähler müssen von allen Schreibern (Serializer, Undo, Kollaborations-Sync, Skript-API) bedient werden; sonst veraltete Geometrie. Daher zuerst nur (1), wortweiser Hash, (3) und (4), die Verhalten exakt erhalten. Tests: Rope mit/ohne Attachment, Wechsel der Control Points, Entity-Handle-Recycling, `releaseWorld`.

---

### SCN-605 – Pose-Pipeline: mehrere Heap-Vektoren pro Entity und Frame
- **ID**: SCN-605
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PoseFinalize.cpp:149-156, 178-180, 311, 408-409, 502; src/HE_Scene/src/PoseSource.cpp:66-68, 93-95, 130-139
- **Problem**: Für jede animierte Entity und jeden Frame werden neu angelegt: `model` (`vector<mat4>` in `poseFinalize`), pro Layer `layerTRS`, ggf. `refTRS`, `out` (danach `pose = std::move(out)`, also ein Allokations-/Freigabe-Zyklus pro Layer), in `makePoseSource` für Blend Spaces `clips`, `durations`, `weights` und `order`, in `PoseSource::evaluate` nochmal `order` (die Sortierung `blendSpaceEvalOrder` läuft zweimal), `poses` (vector<vector<JointTRS>>, ein Vektor je aktivem Sample, je Skelettgröße zeroinitialisiert), `deltas` und `haveDelta` (`vector<bool>`). Bei IK kommen `toSolve`, `chain`, `chainWeights` hinzu. Bei 100 Joints à 40 Byte sind das viele kleine bis mittlere malloc/free pro Entity und Frame, plus Memset.
- **Code-Auszug**:
```cpp
std::vector<JointTRS> layerTRS;
...
std::vector<JointTRS> out;
applyLayerPose(pose, layerTRS, ref, maskWeights, layer.weight, layer.mode, rootJoint, out);
pose = std::move(out);
...
std::vector<std::vector<JointTRS>> poses(space->samples.size());
```
- **Lösung**: Eine `PoseScratch`-Struktur (Vektoren für `model`, `layerTRS`, `refTRS`, `out`, `poses`, `deltas`, `order`, IK-Listen), pro Treiber-`update()` einmal angelegt (oder `thread_local`) und durch `poseFinalize`/`PoseSource::evaluate` gereicht; Vektoren nur `clear()`/`resize()`. `order` einmal in `makePoseSource` berechnen und in `PoseSource` ablegen (entfällt in `evaluate`). `haveDelta` als `std::array`/`std::unique_ptr<bool[]>` oder in `RootMotionDelta` integrieren. `applyLayerPose` in-place bzw. Ping-Pong zwischen zwei Scratch-Puffern.
- **Erwarteter Effekt**: (Schätzung) Bei einigen hundert animierten Charakteren sinkt die Allokationsrate von ca. 10-30 auf ca. 0 pro Entity und Frame; messbar vor allem in Crowd-Szenen und durch geringere Speicherfragmentierung. Messen mit `AnimationStateMachine`-Scope und Allocator-Zähler.
- **Risiko / Verifikation**: Schnittstellenänderung von `poseFinalize`/`evaluate` (drei Treiber). Ergebnisbits müssen identisch bleiben (Kommentar Z. 497-500: „same bits"): vorhandene Animationstests ausführen (Layer, Additive, Blend Space, IK).

---

### SCN-606 – PlayerHost::tick: Strings, Event-Namen und Zustandsvektor pro Aktion und Frame
- **ID**: SCN-606
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PlayerHost.cpp:286-315 (`who`), 363-425, 388, 398, 409-410, 425
- **Problem**: Jeden Frame werden (a) pro Spieler `who` als `std::string` gebaut, bei mehr als einem Spieler mit zwei Allokationen, obwohl sie nur in den seltenen Fällen `Captured`/`Cancelled` gebraucht wird; (b) pro Aktion und Spieler die Event-Namen per String-Konkatenation erzeugt (`HE::inputEventAxis(a.name)`, `inputEventAxis2D`, `inputEventPressed`, `inputEventReleased`), an `fireInputEvent(const std::string&)` übergeben und dort im Runtime per String-Hash aufgelöst, danach noch einmal für den Pawn; (c) `std::vector<ActionState> states` mit `reserve` neu angelegt, `st.name = a.name` kopiert (Heap bei Namen >15 Zeichen) und per `setActions(std::move(...))` das alte Vektorpaket samt Strings freigegeben. Achsen-Events feuern absichtlich jeden Frame (Kommentar Z. 383), die Namenserzeugung dazu aber nicht.
- **Code-Auszug**:
```cpp
const std::string who = m_players.size() > 1 ? "Player " + std::to_string(i + 1) + ": " : "";
...
fireInputEvent(i, HE::inputEventAxis(a.name), HorizonCode::Value::ofFloat(v));
...
std::vector<HE::api::input::ActionState> states;
states.reserve(m_actions.size());
```
- **Lösung**: `ActionInfo` bekommt beim `begin()` vorberechnete Felder `evAxis`, `evAxis2D`, `evPressed`, `evReleased` (nur je nach `kind`). `fireInputEvent` nimmt sie ohne Neubau. `who` erst in den `case`-Zweigen erzeugen. `states` als Member behalten, Elemente in-place aktualisieren (`name` nur beim `begin()` setzen) und per `setActions(const std::vector&)` oder Swap übergeben. Die Text-Script-Aufrufe (Z. 390, 401, 416) siehe Cross-Modul.
- **Erwarteter Effekt**: (Schätzung) Eliminiert ca. 2-4 Heap-Allokationen und ebenso viele String-Hashes pro Aktion und Frame, bei 20-60 Aktionen und 60 FPS ein paar tausend Allokationen pro Sekunde. Im Mikrosekundenbereich pro Frame; niedriger Gewinn, aber trivial und riskoarm.
- **Risiko / Verifikation**: Eventnamen müssen byteidentisch zu `HE::inputEventAxis()` & Co. sein. Bestehende Input-/Player-Tests prüfen Event-Zustellung (Pause, UIOnly, Rebind, Multi-Player).

---

### SCN-607 – addEntity: `removeEntityImpl` immer, dadurch O(Joints) pro Spawn; lineare Pending-Joint-Listen
- **ID**: SCN-607
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:2533 (`addEntity`), 2307-2346 (`destroyJointsInvolving`), 608-629, 648-654, 2348-2418 (`resolvePendingJoints`), 2550
- **Problem**: `addEntity` ruft bedingungslos `removeEntityImpl(entityId, Rebuild)`. Das läuft in `destroyJointsInvolving`, das **alle** Joints linear durchsucht (Z. 2319-2321), außerdem acht Hash-Erases (Character, Water, Owed …). `addEntities` vermeidet das bereits mit `hasPhysics` (Kommentar Z. 2588-2591), `addEntity`/`addEntityTree` nicht. Projektile, Pickups und Effekte, die einzeln gespawnt werden, zahlen den Scan pro Spawn, in Szenen mit vielen Joints (Ragdolls, Ketten). Zusätzlich: `queueJoint`/`queuePatientJoint`/`unqueueJoint` sind lineare Scans über `pendingJoints`, und `resolvePendingJoints` läuft bei jedem erfolgreichen `addEntity` und macht pro „patient"-Eintrag `findByEntityId` plus vier Hash-Lookups, also O(P) pro Spawn, solange gestreamte Zellen mit wartenden Joints nicht geladen sind (diese Einträge werden nie aufgegeben).
- **Code-Auszug**:
```cpp
removeEntityImpl(entityId, JointFate::Rebuild);
bool built = buildBodyFor(world, entityId);
...
for (const auto& [owner, record] : m_impl->joints)
    if (owner == entityId || record.partner == entityId)
        owners.push_back(owner);
```
- **Lösung**: (1) In `addEntity` wie in `addEntities` nur bei `hasPhysics(entityId)` entfernen. Das ist äquivalent, denn ein Joint-Eintrag existiert nur, wenn beide Bodies existieren; hat die Entity keinen Body, gibt es auch keinen Joint, der sie nennt. (2) Reverse-Index `bodyEntity → owners` (oder `unordered_multimap<partner, owner>`) für `destroyJointsInvolving`. (3) `pendingJoints` zusätzlich als `unordered_set<uint32_t>` indizieren; patient-Einträge nach Partner-UUID (`unordered_multimap<UUID, entity>`) bündeln, so dass ein Spawn nur die Wartenden dieses Partners prüft.
- **Erwarteter Effekt**: (Schätzung) (1) macht Einzel-Spawns in jointreichen Szenen von O(J) auf O(1); (2)/(3) nur relevant bei vielen Joints bzw. vielen wartenden Zellen-Joints. Messen: 1000× `addEntity` bei 500 Joints.
- **Risiko / Verifikation**: (1) gering; Test „addEntity zweimal = Replace" und „Joint-Partner wird neu gebaut" sichern es ab. (2)/(3) berühren die Rebuild-/Wait-Semantik, daher nur mit den vorhandenen Joint-Tests (Chain, Streaming-Wait, Drop vs. Rebuild).

---

### SCN-608 – `purgeEntity` pro entferntem Body: O(aktive Kontakte) unter dem globalen Lock
- **ID**: SCN-608
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:371-400, 3054, 2705-2745 (`removeEntityTree`)
- **Problem**: Jeder `destroyBodyFor` ruft `contactListener.purgeEntity`, das `m_active` komplett linear durchläuft und alle vier Event-Queues filtert, unter `m_mutex`. Der Kommentar (Z. 368-370) nimmt „Gameplay-Rate" an. Ein Streaming-Unload (`removeEntityTree` mit `requeueJoints`) entfernt aber tausende Bodies in einem Frame: M Bodies × |m_active| Kontakte. Bei 5000 Bodies und einigen 10 000 aktiven Kontakten sind das 10⁸ Vergleiche in einem Frame, während der Mutex gehalten wird.
- **Code-Auszug**:
```cpp
for (auto it = m_active.begin(); it != m_active.end(); )
    if (it->second.event.entityA == entityId || it->second.event.entityB == entityId)
        it = m_active.erase(it);
    else ++it;
```
- **Lösung**: Batch-Variante `purgeEntities(const std::unordered_set<uint32_t>&)` mit einem einzigen Durchlauf über `m_active` und die Queues; `removeEntityTree`/`clear`-ähnliche Massenpfade sammeln die IDs und rufen sie einmal am Ende. `destroyBodyFor` bekommt dafür einen Parameter bzw. eine Variante ohne Purge. Alternativ Reverse-Index `entity → Schlüssel` in `ActiveContact`, falls Einzelentfernungen häufig sind.
- **Erwarteter Effekt**: (Schätzung) Zellen-Unload mit vielen Kontakten von O(M·C) auf O(M+C). Messen mit einem Streaming-Unload in einer Szene mit großem Haufen.
- **Risiko / Verifikation**: Ereignis-Semantik gleich (alle Kontakte der entfernten Entities verschwinden ohne Exit-Events); „carry"-Bookkeeping in `drop()` bleibt. Test: Unload eines Baums mit aktiven Kontakten, danach `pollCollisionExit` ohne tote Entity.

---

### SCN-609 – `initialize()` fügt Bodies einzeln in die Broad Phase statt als Batch
- **ID**: SCN-609
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:2460-2467, 1809 (`CreateAndAddBody`), 2606-2617 (Batch in `addEntities`), 2498
- **Problem**: `addEntities` nutzt `AddBodiesPrepare/Finalize` (Kommentar: „one subtree per layer to merge", Z. 2606-2609). `initialize()` ruft dagegen `buildBodyFor(world, id)` ohne Batch, also `CreateAndAddBody` je Body mit inkrementellem Tree-Insert, und baut danach mit `OptimizeBroadPhase()` den gesamten Tree sowieso neu. Bei zehntausenden Bodies (der Code-Kommentar spricht von 65 536) ist das die teuerste Phase des Szenenstarts.
- **Code-Auszug**:
```cpp
for (uint32_t entityId : bodies)
    buildBodyFor(world, entityId);          // batch == nullptr -> CreateAndAddBody
...
m_impl->physicsSystem.OptimizeBroadPhase();
```
- **Lösung**: In `initialize()` ein `BodyBatch` verwenden: `buildBodyFor(world, id, &batch)` für `bodies`, danach einmal `AddBodiesPrepare`/`AddBodiesFinalize(..., EActivation::Activate)` (statische Bodies aktiviert Jolt nicht), dann `OptimizeBroadPhase()` wie bisher. Der Code dafür existiert in `addEntities` bereits; in eine Hilfsfunktion `commitBatch()` ziehen und in beiden Pfaden nutzen.
- **Erwarteter Effekt**: (Schätzung) Deutlich kürzerer Szenenstart bei großen Szenen. Messen: `initialize()` bei 20 000 Bodies, vorher/nachher.
- **Risiko / Verifikation**: Die Tree-Struktur ändert sich, damit potenziell die Reihenfolge von Kontakt-Callbacks und Raycast-Gleichstand-Auflösung; Determinismus-Tests und Snapshots mit festen Seeds prüfen (Jolt-Determinismus gilt pro Aufbau, nicht zwischen zwei Aufbauarten). Joint-Phase muss nach dem Batch-Commit laufen (tut sie ohnehin als zweite Phase).

---

### SCN-610 – applyWater: Container-Allokationen pro Schritt und ein 360-Zeilen-Monolith
- **ID**: SCN-610
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:3765-4124, bes. 3811-3817, 3878-3880, 4003-4006, 4070, 4120-4121
- **Problem**: Pro Schritt mit Wasser: je Region ein `AllHitCollisionCollector` (Heap-Array), `waterSeen` (`unordered_set`) wird neu befüllt, `waterOwedNext`/`waterTrackNext`/`waterTrack` sind `unordered_map<uint32_t,…>` mit einem Knoten-`new` pro nassem Body und Schritt (Insert in `…Next`, Swap, Clear gibt Knoten frei). Dazu `std::sort` + `unique` über alle Kandidaten und zweifach `reg.try_get<RigidBodyComponent>` je Kandidat (Z. 3843 und 4033). Die Funktion enthält drei Lambdas (`raiseRings`, `pushEvent`, `allowSleepAgain`) und ist ca. 360 Zeilen lang (Kategorie A).
- **Code-Auszug**:
```cpp
impl.waterTrackNext[c.entity] = Impl::WaterTrack{ share, factor * share };
...
impl.waterOwed.swap(impl.waterOwedNext);
impl.waterTrack.swap(impl.waterTrackNext);
```
- **Lösung**: Kandidaten sind nach (owed, entity) sortiert; `waterOwed`/`waterTrack` als nach Entity sortierte flache Vektoren (`vector<pair<uint32_t,T>>`, `std::lower_bound`) oder offene Adressierung führen, `waterSeen` durch Marker-Generation im Track-Eintrag ersetzen. Collector-Vektor wiederverwenden (Member). Die Funktion in `gatherWaterCandidates`, `serveWaterBodies`, `emitWaterExits` und die Ripple-Logik in eine eigene Klasse/Datei trennen (Vereinfachung).
- **Erwarteter Effekt**: (Schätzung) Nur in Szenen mit Wasser und vielen schwimmenden Bodies relevant; entfernt O(nasse Bodies) Heap-Operationen pro Schritt. Messen mit `budgetStats().buoyancyMs`.
- **Risiko / Verifikation**: Die Reihenfolge der Events und Ringe ist durch die Sortierung nach Entity bereits deterministisch (Z. 4109); beim Wechsel der Container muss sie erhalten bleiben. Wasser-Tests (Eintritt/Austritt, Owed-Budget, Held-Body, Entity-Löschen) laufen lassen.

---

### SCN-611 – Character-Schleife: Neuaufbau und Sortierung pro Schritt, `operator[]`-Inserts, voller Pose-Walk für die Rotation
- **ID**: SCN-611
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:3411-3414, 3441-3449, 3477-3488, 3501, 3591, 3616-3619
- **Problem**: (a) `stepCharacters` wird jeden Schritt aus der Map neu gebaut und, mit Budget (Standard offenbar aktiv), per `std::stable_sort` sortiert, wobei der Komparator pro Vergleich zwei `owedOf.find` macht. Solange niemand etwas schuldet, ist der Komparator konstant `false` und die Sortierung ein reiner Leerlauf. (b) `characterOwed[entityId]` (Z. 3501) legt für **jeden** Character einen Eintrag mit 0 an (Kommentar Z. 693 sagt „Absent = nothing owed"); danach läuft am Ende zusätzlich eine Schleife über alle Einträge (Z. 3616-3619). (c) `holdNeeded()` (ruft u. a. die `std::function` `holdProbe`) wird pro Character statt einmal pro Schritt ausgewertet. (d) Für `MoveKinematic` wird `worldPoseOf(world, entity).rotation` berechnet: kompletter Hierarchie-Walk, Matrix-Zerlegung, `quat_cast`, `normalize`, nur um die Rotation zu erhalten.
- **Code-Auszug**:
```cpp
std::stable_sort(m_impl->stepCharacters.begin(), m_impl->stepCharacters.end(),
                 [&owedOf](uint32_t a, uint32_t b) { const auto ia = owedOf.find(a), ib = owedOf.find(b); ... });
...
float& owed = m_impl->characterOwed[entityId];
...
const glm::quat q = worldPoseOf(world, entity).rotation;
```
- **Lösung**: (a) Sortieren nur, wenn `characterOwed` Einträge > 0 hat (Zähler `owedPositive`); Ergebnis identisch (stabile Sortierung mit konstantem Komparator ändert nichts). (b) Nur bei tatsächlicher Schuld einfügen (`find` statt `[]`), bei 0 erasen. (c) `holdNeeded()` einmal vor der Schleife auswerten und nach einem `insert` in `heldCharacters` neu. (d) Rotation einmalig pro Schritt aus dem Matrixpfad ableiten oder den vorhandenen Rotationspfad (nur Rotation, kein `decomposeWorld` mit Skalierung/Determinante) nutzen.
- **Erwarteter Effekt**: (Schätzung) Für wenige Characters vernachlässigbar, für KI-Massen (hunderte CharacterVirtuals) O(n log n) Hash-Lookups pro Schritt weniger. Niedrig bis mittel; messen.
- **Risiko / Verifikation**: Reihenfolge muss exakt gleich bleiben, wenn keine Schuld existiert: `stepCharacters` folgt dann der Map-Iterationsreihenfolge, also nur den Sortierschritt weglassen, nicht die Reihenfolge ändern. Tests: Budget-Deferral (Owed, kGuaranteedCharacters) und Region-Hold für Characters.

---

### SCN-612 – Poll-Queues verlieren bei jedem Poll ihre Kapazität
- **ID**: SCN-612
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:321-343, 428-452 (`take`), 1626-1633 (`pollWaterEvents`), 2952-2959 (`pollJointBroken`), 4952-4974
- **Problem**: `take()` tauscht die Queue mit einem leeren `result` (`result.swap(queue)`), danach hat die Queue Kapazität 0 und die Worker-Callbacks `push_back`en im nächsten Schritt wieder von vorn (mehrfaches Wachstum/Neuallokation unter dem globalen Lock). Dasselbe bei `pollWaterEvents`/`pollJointBroken`. Vier Event-Vektoren pro Schritt; der Aufrufer bekommt jedes Mal einen neuen `std::vector` zurück. Zusätzlich dupliziert `take` den Komparator aus `sortEvents` (Z. 285-290, siehe SCN-619).
- **Code-Auszug**:
```cpp
if (m_limit == 0 || queue.size() <= m_limit) {
    result.swap(queue);
    carried = 0;
    return result;
}
```
- **Lösung**: Die Poll-Funktionen mit Out-Parameter `void pollX(std::vector<CollisionEvent>& out)` (alt: `out.clear(); out.swap(queue)` plus `queue.reserve(out.capacity())`), oder den Puffer pro Poll zurückgeben und der Queue einen vorallokierten Ersatz geben. Die vorhandene Rückgabe-Variante kann als dünner Wrapper bleiben (API-kompatibel).
- **Erwarteter Effekt**: (Schätzung) Ein paar Allokationen pro Schritt weniger; klein, aber im Callback-Hot-Path unter Lock.
- **Risiko / Verifikation**: Öffentliche API in `PhysicsWorld.h` und Aufrufer (CollisionSystem, EngineApi) betroffen; die Wrapper-Lösung vermeidet Änderungen. Collision-Event-Tests (inkl. Limit/Backlog) laufen lassen.

---

### SCN-613 – PropertyAnimationSystem: Komponenten-Lookups pro Kanal statt pro Entity
- **ID**: SCN-613
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PropertyAnimationSystem.cpp:56-61, 63-129, 131-148
- **Problem**: `applyAt` ruft pro Kanal `applyChannel`, das jedes Mal `try_get<TransformComponent>` und `try_get<MaterialComponent>` (zwei Sparse-Set-Lookups) ausführt, auch bei reinen Transformkanälen. Ein Clip mit 9 Transformkanälen macht 18 Lookups pro Entity und Frame. Der `Visible`-Kanal löst sechs weitere `try_get`s aus. Es gibt kein „Wert unverändert"-Skip: auch ein gehaltener Wert setzt `tc->dirty`/`mc->dirty` und überschreibt Material-Assets (siehe Cross-Modul). `cm.getPropertyAnimClip(pa.clipId)` (Hash-Lookup) läuft zusätzlich pro Entity und Frame.
- **Code-Auszug**:
```cpp
void PropertyAnimationSystem::applyChannel(HorizonWorld& world, ContentManager& cm, entt::entity e,
                                           PropTarget target, float v)
{
    auto& reg = world.registry();
    TransformComponent* tc = reg.try_get<TransformComponent>(e);
    MaterialComponent*  mc = reg.try_get<MaterialComponent>(e);
```
- **Lösung**: In `applyAt` einmal `tc`/`mc` ermitteln und als `struct Targets{ TransformComponent* tc; MaterialComponent* mc; }` an `applyChannel` geben; die übrigen Typen (Camera, Mesh, Light …) nur lazy im Kanal-Zweig. `tc->dirty = true` einmal am Ende von `applyAt`, wenn mindestens ein Transformkanal gesetzt wurde. Optional: Wert gegen den bisherigen vergleichen und `dirty` nur bei Änderung setzen.
- **Erwarteter Effekt**: (Schätzung) Etwa 2-3× weniger Registry-Lookups pro animierter Entity; relevant bei vielen animierten Props (Türen, Plattformen, Lichter); sonst klein.
- **Risiko / Verifikation**: Verhaltensgleich, solange die Setzung von `dirty` unverändert bleibt. Bestehende PropertyAnimator-Tests (Step-Kanal `Visible`, Looping, Material) prüfen.

---

### SCN-614 – Begin-/End-Frame-Durchläufe für Per-Frame-Flags
- **ID**: SCN-614
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/PoseFinalize.cpp:434-440, 525-548; src/HE_Scene/src/RootMotionApply.cpp:83-87, 174-185; src/HE_Scene/src/SceneSystems.cpp:213, 217
- **Problem**: `tickAnimation` startet jeden Frame mit zwei vollständigen View-Durchläufen (`rootMotionBeginFrame`, `poseBeginFrame` mit zwei Views) nur um Flags zurückzusetzen, und endet mit zwei weiteren (`rootMotionEndFrame`, `poseEndFrame`). `poseEndFrame` ist reine Diagnose („Entity hat Layers, aber keinen Animator") und läuft trotzdem in Release jeden Frame über alle Layer- und IK-Komponenten. Bei Crowds mit `RootMotionComponent`/`IkComponent` an hunderten Entities sind das vier Komponenten-Scans pro Frame, ohne Nutzen für unkritische Fälle.
- **Code-Auszug**:
```cpp
for (auto [e, lc] : world.registry().view<AnimationLayerComponent>().each())
    lc.finalizedThisFrame = false;
```
- **Lösung**: Statt Bool-Flag ein Frame-Stempel: `uint32_t finalizedFrame`/`appliedFrame` im Component, `tickAnimation` zählt `frameId` hoch; „schon gemacht" ist `finalizedFrame == frameId`. Die Begin-Durchläufe entfallen vollständig. Für `rootMotionEndFrame` nur über Entities iterieren, die zuletzt Velocity geschrieben haben (kleine Menge in `RootMotionContext`). `poseEndFrame` nur in Editor-/Debug-Builds oder alle N Frames (Log ist ohnehin gedrosselt).
- **Erwarteter Effekt**: (Schätzung) Nur messbar bei vielen tausend solchen Komponenten; sonst im Rauschen. Dafür weniger Code im Kernpfad.
- **Risiko / Verifikation**: Das Flag ist Teil der Komponenten-Daten, daher Serialisierung und Editor-Inspector prüfen (nicht serialisiert?). Der Doppelfahrer-Fall („zwei Treiber, einer gewinnt") muss weiterhin warnen: Test mit Entity, die zwei Treiber trägt.

---

### SCN-615 – Fuß-IK: Raycast pro Fuß und Frame ohne Distanz-/Sichtbarkeits-Skip, Allokationen in `applyIk`
- **ID**: SCN-615
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PoseFinalize.cpp:275-430 (bes. 311, 333-337, 408-409); src/HE_Scene/src/PhysicsWorld.cpp:4375-4436 (`raycast`)
- **Problem**: Jede animierte Entity mit `IkComponent` und `feet` macht pro Fuß und Frame einen vollständigen Physik-Raycast (`physics->raycast`), unabhängig davon, ob die Entity sichtbar oder weit weg ist. Jeder Raycast bezahlt `QueryCharge` (zwei `steady_clock::now`, zwei Atomics) und wird dem **Physik-Budget** angerechnet (Z. 3426); viele IK-Charaktere verringern so den Anteil für Character-Schritte (Kopplung Animation → Physik-Budget). Zusätzlich je Aufruf `toSolve`, `chain`, `chainWeights` als Heap-Vektoren, `glm::inverse(entityWorld)` und ein zusätzlicher `worldMatrixOf` (RootMotion ruft ihn kurz davor schon, siehe SCN-617).
- **Code-Auszug**:
```cpp
const PhysicsWorld::RaycastHit hit =
    physics->raycast(footWorld + glm::vec3(0.0f, f.traceUp, 0.0f),
                     glm::vec3(0.0f, -1.0f, 0.0f), std::max(0.0f, f.traceUp + f.traceDown),
                     static_cast<uint32_t>(e));
```
- **Lösung**: Optionaler Distanz-/Sichtbarkeits-Cutoff: Ab Abstand X zur Kamera (oder wenn die Entity gecullt ist) das Foot-IK-Gewicht auf 0 ausblenden und nicht traceen; den bereits geglätteten Offset ausfaden. Raycasts über Frames verteilen (jeder Fuß alle 2 Frames, dazwischen interpoliert). Die Query nicht dem Physik-Budget zuschlagen (eigener Zähler). Vektoren in `PoseScratch` (siehe SCN-605).
- **Erwarteter Effekt**: (Schätzung) Für Crowds mit IK proportional zur Zahl der IK-Füße, die dadurch nicht mehr jeden Frame casten; hängt vom Spieldesign ab. Messen.
- **Risiko / Verifikation**: Ändert das sichtbare Verhalten (ferne Charaktere ohne IK). Muss als Option/Standard-Schwelle bewusst entschieden werden, die Editor-Vorschau (physics == null, Fuß-IK aus) bleibt unberührt.

---

### SCN-616 – Trails: Löschen am Vektoranfang und Hierarchie-Walk pro Trail und Frame
- **ID**: SCN-616
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/RopeTrailSystem.cpp:234-264, 293-294
- **Problem**: `stepTrail` altert alle Punkte (`p.age += dt`), löscht abgelaufene Punkte per `erase(begin, alive)` am Vektoranfang und kürzt auf `maxPoints` ebenfalls am Anfang. Beides ist O(Punkte) Verschiebung pro Frame und Trail (Ringpuffer/`deque` wären O(1)). `worldPositionOf` läuft für jeden Trail jeden Frame, auch wenn `emitting == false` und keine Punkte mehr vorhanden sind (Wert wird nur im `emitting`-Zweig gebraucht). Das Altern aller Punkte ließe sich durch einen Zeitstempel pro Punkt ersetzen.
- **Code-Auszug**:
```cpp
for (auto [e, trail] : reg.view<TrailComponent>().each())
    stepTrail(trail, HE::worldPositionOf(world, e), dt);
...
trail.points.erase(trail.points.begin(), alive);
```
- **Lösung**: `worldPositionOf` erst im `emitting`-Zweig aufrufen (Lambda/Callback übergeben); Punkte mit Geburtszeit statt Alter speichern (`age = now - born`, beim Bauen der Geometrie berechnet), dann entfällt die Alterungsschleife; Speicher als `std::deque` oder Ring.
- **Erwarteter Effekt**: (Schätzung) Klein; relevant nur bei vielen Trails mit hoher `maxPoints`.
- **Risiko / Verifikation**: `TrailComponent::Point` ist evtl. serialisiert/exponiert (Editor/Skript); Änderung der Punktdarstellung prüfen. `buildTrailGeometry` liest `age` direkt.

---

### SCN-617 – Root Motion: wiederholte Clip-Samples und Hierarchie-Walks pro Entity und Frame
- **ID**: SCN-617
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/RootMotion.cpp:89, 157, 67-78; src/HE_Scene/src/RootMotionApply.cpp:61, 114-133; src/HE_Scene/src/PoseFinalize.cpp:474
- **Problem**: Pro Entity und Frame: `findRootJoint` (lineare Stringvergleiche über das Skelett) wird in `rootMotionSampleClip` je Clip/Sample aufgerufen, in `poseFinalize` nochmal pro Entity; `extractRootMotion` sampelt den Root bei t=0 für `invRest` und `lockRootJoint` sampelt ihn wieder bei t=0 (`first`); beides sind konstante Werte pro Clip. In `rootMotionApply` laufen bis zu drei Hierarchie-Walks (`entityWorldYaw` → `worldMatrixOf`, `worldPositionOf`, `localPositionForWorld`), danach in `applyIk` ein weiterer `worldMatrixOf` plus `glm::inverse`.
- **Code-Auszug**:
```cpp
const glm::quat invRest = glm::inverse(sampleJoint(clip, joint, 0.0f).rotation);   // RootMotion.cpp:89
const JointTRS first = sampleJoint(clip, static_cast<uint32_t>(rootJoint), 0.0f);   // RootMotion.cpp:157
```
- **Lösung**: Root-Joint-Index im `RootMotionComponent` cachen, invalidiert wie `resolvedForMeshId` in `IkComponent`; Root-Frame-0-Pose und `invRest` einmal pro Clip im `AnimationClipAsset` (beim Laden/Cooking). In `rootMotionApply` die Weltmatrix einmal holen und Yaw und Position daraus ableiten. Der Aufwand pro Aufruf ist klein, die Summe pro Frame bei Crowds nicht.
- **Erwarteter Effekt**: (Schätzung) Klein (Prozentbereich der Root-Motion-Kosten). Nur mit Messung angehen; Hauptkosten sind `sampleClip` über das Skelett.
- **Risiko / Verifikation**: Cache-Invalidierung bei Mesh-/Clip-Wechsel und Hot-Reload; vorhandene Root-Motion-Tests (Wrap/Laps, Blend Space, Charakter-Controller-Modus).

---

### SCN-618 – Physik-Queries: Shape- und Set-Allokationen pro Aufruf
- **ID**: SCN-618
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:4188-4210 (`HEOverlapCollector`), 4450, 4473, 4498, 4513, 4531, 4553, 4603
- **Problem**: Jeder `sphereCast`/`boxCast`/`capsuleCast`/`overlap*`-Aufruf macht `new JPH::SphereShape/BoxShape/CapsuleShape` (Heap + Refcount). `HEOverlapCollector` pflegt für die Deduplizierung ein `unordered_set` (Knotenallokation je Treffer), `raycastAll` ebenso (`seen`). Skripte rufen solche Queries oft pro Frame und Entity auf. Die Zahl der Treffer ist meist klein.
- **Code-Auszug**:
```cpp
const JPH::Ref<JPH::Shape> sphere = new JPH::SphereShape(radius);
...
if (m_seen.insert(m_current).second) m_entities.push_back(m_current);
```
- **Lösung**: Statt `unordered_set` bei bis zu ~32 Treffern lineare Suche im bereits vorhandenen `m_entities` (kein Zusatzcontainer); Shapes in kleinem thread-lokalen Cache nach Parametern. Erst nach Messung (Skript-Last).
- **Erwarteter Effekt**: (Schätzung) Klein; nur bei hunderten Queries pro Frame sichtbar.
- **Risiko / Verifikation**: Reihenfolge der Treffer (Einfügereihenfolge) bleibt bei linearer Suche gleich.

---

### SCN-619 – PhysicsWorld.cpp: 5000-Zeilen-Datei, toter Code, doppelte Includes/Komparatoren
- **ID**: SCN-619
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Scene/src/PhysicsWorld.cpp:8 und 74 (`<unordered_set>` doppelt), 285-290 (`sortEvents`, nirgends benutzt), 438-440 (gleicher Komparator in `take`), 1999-2066 vs. 2960 ff., ganze Datei (5038 Zeilen)
- **Problem**: `HEContactListener::sortEvents` ist definiert und ungenutzt (`grep` findet nur die Definition); `take()` enthält denselben Komparator inline. `<unordered_set>` ist zweimal eingebunden. Die Datei mischt Layer-Filter, Listener, Shape-Bau, Joints, Character, Region-Hold, Wasser und Queries in einer Übersetzungseinheit (lange Compile-Zeit, schwer zu reviewen). `jumpCharacter(entityId)` schlägt Component und Character nach und ruft dann die Überladung, die beides noch einmal nachschlägt (Z. 4838-4871).
- **Code-Auszug**:
```cpp
static void sortEvents(std::vector<PhysicsWorld::CollisionEvent>& events)   // ungenutzt
```
- **Lösung**: `sortEvents` entfernen oder in `take()` verwenden (eine Quelle für den Komparator), Doppel-Include entfernen. Mittelfristig Aufteilung in `PhysicsWorld.cpp` (Kern/Step), `PhysicsJoints.cpp`, `PhysicsWater.cpp`, `PhysicsQueries.cpp`, `PhysicsShapes.cpp` mit gemeinsamer interner Header für `Impl`.
- **Erwarteter Effekt**: Lesbarkeit und inkrementelle Build-Zeit; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Rein mechanisch; Build und `he_tests` genügen.

---

### SCN-620 – PlayerHost: ungenutzte Includes, Durchreich-Wrapper, Start lädt jede HorizonCode-Klasse
- **ID**: SCN-620
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/PlayerHost.cpp:15, 17, 21-28, 79-97
- **Problem**: `<filesystem>` und `<unordered_set>` werden eingebunden, aber nicht verwendet (nur die Include-Zeilen treffen im Grep). `discoverAssets` im anonymen Namespace ist ein reiner Durchreich-Wrapper um `cm.discoverAssets`. Wichtiger: `begin()` ruft `resolveClassAsset` für **jede** HorizonCode-Klasse des Projekts (Kommentar Z. 72-78: das lädt alle Vorfahren), nur um PlayerController/PlayerCharacter zu finden. Bei großen Projekten kostet das beim Sessionstart Zeit und Speicher (Level-Graphen werden pro Klasse zusammengesetzt).
- **Code-Auszug**:
```cpp
for (const std::string& assetPath : classPaths) {
    HorizonCode::ResolvedClass rc = HorizonCode::resolveClassAsset(cm, assetPath);
    if (HorizonCode::engineClassIsA(rc.engineBase, "PlayerCharacter")) { ... }
```
- **Lösung**: Includes und Wrapper entfernen. Für den Start: `engineBase` pro Klasse aus einer leichten Metadatenquelle (Asset-Header oder `TypeRegistry`) lesen, oder `resolveClassAsset` erst nach billiger Vorprüfung aufrufen, und nur die Treffer vollständig auflösen.
- **Erwarteter Effekt**: (Schätzung) Einmalig beim Start; unsicher, ob messbar. Nur bei großen Class-Beständen relevant.
- **Risiko / Verifikation**: Die Vorprüfung muss auch abgeleitete Klassen finden (Kommentar Z. 88-91); Test mit Klasse → Klasse → PlayerController.

---

### SCN-621 – SceneJsonParse: Index-Vektor ohne `reserve`, zeichenweiser Scanner
- **ID**: SCN-621
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/SceneJsonParse.cpp:34-117 (bes. 96), 20-28
- **Problem**: `findEntities` (Text) füllt `elements` per `push_back` ohne `reserve`, während die CBOR-Variante vorab reserviert (Z. 225). Die Scannerschleife geht zeichenweise mit `isSpace` und `switch`; `skipString` sucht Anführungszeichen/Backslash byteweise. Das ist ein serieller Vorlauf vor dem parallelen Parse (Amdahl). Bei mehreren 100 MB Szenentext liegt er im Bereich einer Sekunde, im Vergleich zum DOM-Parse jedoch klein (unsicher, messen).
- **Code-Auszug**:
```cpp
if (inArray && depth == 2) { elements.push_back({ elemBegin, i + 1 }); }
```
- **Lösung**: `elements.reserve(text.size() / 512)` o. ä. als Heuristik; `skipString` mit `memchr`/zwei-Zeichen-Suche beschleunigen. Nur nach Messung weitergehen.
- **Erwarteter Effekt**: (Schätzung) Kleiner einstelliger Prozentanteil am Ladevorgang.
- **Risiko / Verifikation**: Verhalten gleich; bestehende `parseSceneText`-Tests (Fallback bei Sonderformen, doppelte Keys, Escapes) laufen lassen.

---

## Cross-Modul-Hinweise

- `src/HE_Scene/src/SceneSystems.cpp:213,217,230`: Reihenfolge `rootMotionBeginFrame`/`poseBeginFrame` und `PropertyAnimationSystem::update` (siehe SCN-614, SCN-613).
- `src/HE_Scene/include/HorizonScene/Components/{RopeComponent,TrailComponent,RootMotionComponent,AnimationLayerComponent,IkComponent}.h`: ein Revisions-/Frame-Stempel (SCN-604, SCN-614) gehört in die Komponenten; Serialisierung beachten.
- `src/HE_Scene/src/PlayerHost.cpp:390,401,416`: `callOnInputAxis/-Axis2D/-Pressed` werden für **jede** Lua/Python-Instanz je Aktion und Frame aufgerufen (O(Aktionen × Skript-Instanzen)). Prüfen, ob `ScriptContext` ohne Handler schnell zurückkehrt (Handler-Cache pro Instanz); sonst Aufrufe nur an Instanzen mit `onInputAxis`. Unsicher, Code in `ScriptContext.cpp`.
- `src/HE_Scene/src/PropertyAnimationSystem.cpp:92`: Material-Kanäle schreiben in das **geteilte** `MaterialAsset` (`getMaterialMutable`) und setzen nur `mc->dirty` der animierten Entity. Mehrere Entities mit demselben Material überschreiben sich gegenseitig, und das Asset im Speicher wird verändert (Persistenz?). Das Verhalten steht im Kommentar („written directly to the shared MaterialAsset"), ist aber ein Designrisiko; Prüfung bei MaterialSystem.
- `HE::water::WaterIndex::refresh(world)` (aufgerufen in `PhysicsWorld.cpp:3795`) läuft jeden Schritt; ob der Aufruf bei unveränderten Landscapes billig ist (Revision-Check), steht in `WaterQuery.cpp`, nicht geprüft.
- `src/HE_Core/include/Application/…` (`HE::inputEventAxis` & Co. in `InputAssets.h`): Event-Name-Erzeugung liefert `std::string`; ein `string_view`/interned ID wäre die Grundlage für SCN-606.
- `HorizonCode::Runtime::fireEvent(inst, std::string, …)`: String-Schlüssel-Lookup pro Event; interned Event-IDs würden den Aufruf verbilligen.

## Unsicher / Messung nötig

- SCN-601: Anteil der Write-back-Schleife an der Frame-Zeit bei 20 000 Bodies (Quelle: Kommentar Z. 3300-3303, „ein Fünftel"); Verfügbarkeit/Signatur von `GetActiveBodiesUnsafe` in der vendorten Jolt-Version.
- SCN-602: tatsächliche Lock-Contention bei 4-8 Jolt-Workern und dichten Haufen (Referenz: `docs/physics-threading-baseline.md`).
- SCN-603: Häufigkeit identischer Mesh/Hull-Collider in realen Szenen; RAM-Ersparnis pro Shape.
- SCN-604: Anzahl Seile in realen Szenen; Kosten des Hashs vs. Rebuild.
- SCN-605: Allokationsanteil an `AnimationStateMachine`/`AnimationBlend`-Scope bei Crowds.
- SCN-609: Zeitanteil der Einzel-Inserts an `initialize()`; Determinismus bei geänderter Tree-Struktur.
- SCN-611: Defaultwert von `PhysicsBudgetConfig::frameMs` (ob die Sortierung standardmäßig läuft) steht in `PhysicsBudget*.h`, nicht in diesem Slice.
- SCN-615: Wie viele IK-Charaktere realistisch gleichzeitig sichtbar sind; Verhalten bei Distanz-Cutoff ist eine Designentscheidung.
- SCN-620: Gesamtzahl/Tiefe der HorizonCode-Klassen in großen Projekten.
- SCN-621: Anteil des seriellen Scanners an der Szenenladezeit (Messung mit `piecesUsed`/Profil-Scope).
- `PlayerSettings.cpp`: Die drei fast gleichen `effectiveX/applyX`-Paare (Deadzone, VSync, Fullscreen) ließen sich über eine kleine Hilfsvorlage zusammenfassen. Das wurde als Banalität nicht aufgenommen.

## Gelesen

Alle 13 Dateien des Slice vollständig gelesen: ja (PhysicsWorld.cpp 1-5038 in vier Blöcken, PlayerHost.cpp, PlayerSettings.cpp, PoseFinalize.cpp/.h, PoseSource.cpp/.h, PropertyAnimationSystem.cpp, RootMotion.cpp, RootMotionApply.cpp/.h, RopeTrailSystem.cpp, SceneJsonParse.cpp). Zusätzlich gezielt per Grep geprüft: `BlendSpaceCache` (ist eine `std::deque`, Zeigerstabilität bestätigt), Aufrufstellen in `SceneSystems.cpp`, Nutzung von `sortEvents` und `PropertyAnimationSystem::advance`.
