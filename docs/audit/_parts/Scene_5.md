# Scene Slice 5 – SplineCurve.h … ParticleSystem.cpp (Spline*, Terrain*, Timer, TransformHierarchy.h, UI*, Water*, Weather*, HorizonWorld, LOD, Movement, Navigation, Net/*, NotifyCollect.h, ParticleSystem)

## Zusammenfassung

Der Slice besteht zur Hälfte aus gut dokumentierten, reinen Deklarations-Headern (Water*, Terrain*, Spline*, UI*) und zur Hälfte aus Laufzeitcode (HorizonWorld, LOD/Movement/Navigation/Particle, Net-Replikation). Der Code ist insgesamt sauber und sehr gut kommentiert; die Dirty-Flag-Disziplin im Terrain/Water-Bereich ist vorbildlich (Region-Dirty, Revision, Hash). Die Funde konzentrieren sich auf die Per-Frame-Systeme: `PropertyReplicator::update` allokiert pro Frame und pro replizierter Entity mehrere Vektoren samt String-Kopien, `LODSystem`, `MovementSystem` und `NavigationSystem` iterieren jeden Frame alle Komponenten ohne Änderungserkennung, `ParticleSystem` führt pro Partikel einen Raycast aus, und `HorizonWorld::clear()` ist bei flachen Hierarchien quadratisch. Insgesamt 16 Funde: 1 Hoch, 7 Mittel, 8 Niedrig. Alle Laufzeitangaben sind Schätzungen; es lagen keine Messdaten vor.

---

### SCN-501 – PropertyReplicator::update allokiert pro Frame und Entity
- **ID**: SCN-501
- **Kategorie**: C (auch D)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/Net/PropertyReplicator.cpp:356-468 (Kern 360, 386-400, 410, 423, 465-466); Hilfsfunktion 92-124
- **Problem**: `update()` läuft jeden Frame auf dem Host (`NetGameSession::update` → `m_properties->update()`). Für JEDE replizierende Entity mit Tabelle wird `collect()` aufgerufen. Dabei entstehen pro Entity und Frame drei neue Vektoren (`names`, `notify` als `vector<bool>`, `current`), jeder Variablenname wird als `std::string` kopiert und jeder Wert (`HorizonCode::Value`, eine große Struktur mit String/Vektoren) kopiert, nur um anschließend per `names != t.names` (String-Vergleiche) festzustellen, dass sich nichts geändert hat. Zusätzlich wird die Variablenliste der Runtime (`replicatedVariablesOf(inst)`) pro Entity und Frame abgefragt und jeder Wert über den Namen (`getVariable(inst, rv.name)`, String-Lookup) geholt. Dazu kommen pro Frame: `joinedConnections()` (Vektor), ein zweiter Durchlauf über alle `m_tables` mit `entityOf()`, ein dritter Durchlauf über die `NetworkComponent`-View mit `m_tables.find`, und pro Änderung `noteCost(t.names[idx], …)` mit String-Hash. Es gibt keinen Dirty-Hinweis der Quelle (Runtime-Variable / `ReplicatedVarsComponent`), jede Entity wird jeden Frame komplett neu eingesammelt.
- **Code-Auszug**:
```cpp
for (auto [entity, nc] : m_world->registry().view<NetworkComponent>().each())
{
    ...
    std::vector<std::string> names;
    std::vector<bool>        notify;
    std::vector<Value>       current;
    collect(entity, names, notify, current);
    ...
    if (names != t.names) { broadcastTableFor(nc.netId); continue; }
```
- **Lösung**:
  1. Scratch-Puffer (`m_scratchNames/…`) als Member halten und in `collect` nur `clear()` aufrufen (Kapazität bleibt) – entfernt die Heap-Allokationen sofort, Verhalten identisch.
  2. Zweistufig vergleichen: zuerst nur die Deklaration prüfen, ohne Werte und Namen zu kopieren (Anzahl + Referenzen auf `rv.name` gegen `t.names`), erst bei Gleichheit die Werte in-place gegen `t.values` vergleichen (`getVariable` liefert dann nur noch einen Wert pro Variable; `valuesEqual` direkt auf dem Runtime-Wert).
  3. Entities ohne replizierte Variablen (leere Tabelle, `t.names.empty()` bei unveränderter Deklaration) in `Table` als `noVars` markieren und überspringen, bis eine Deklarationsänderung gemeldet wird (Runtime-Hook „Variable deklariert/Klasse neu geladen“, oder billiger Zähler `Runtime::replicatedRevision(inst)`).
  4. `conns` und die Tabellen-Aufräumschleife (379-384) nur bei Roster-/Netzid-Änderung statt jedes Frames ausführen.
  5. `m_costs` mit Index (`uint8_t idx` je Tabelle) statt `std::string`-Schlüssel führen.
- **Erwarteter Effekt**: Schätzung: bei 500 replizierten Entities mit je 3 Variablen entfallen ~2.000-3.000 Heap-Allokationen pro Frame (3 Vektoren + 3 Namen + ≥3 Values × 500); Host-Frametime-Anteil dieser Funktion sinkt vermutlich um eine Größenordnung. Messen mit HE_PROFILE_SCOPE um `m_properties->update()`.
- **Risiko / Verifikation**: Die Reihenfolge (HorizonCode-Variablen zuerst, dann `ReplicatedVarsComponent`, Dubletten übersprungen) und die Index-Semantik müssen erhalten bleiben; der Fall „Typ während der Session geändert“ (valueTypesMatch) und „Deklaration wächst mitten in der Session“ braucht Tests (bestehende Net-Tests unter `tests/` für PropertyReplicator, z. B. Tabelle neu senden bei neuer Variable). Die Dirty-Variante (Punkt 3) braucht einen Hook im Runtime und ist daher riskanter als 1+2.

### SCN-502 – LODSystem: Weltposition jeder LOD-Entity jeden Frame über die Elternkette
- **ID**: SCN-502
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/LODSystem.cpp:13-62 (Kern 33-35); src/HE_Scene/src/TransformHierarchy.cpp:321-367 (Aufrufer-Seite, Referenz)
- **Problem**: Die View `LODComponent, MeshComponent` wird jeden Frame vollständig durchlaufen. Pro Entity wird `worldPositionOf` aufgerufen: das läuft die Elternkette mit `try_get<TransformComponent>` + `try_get<HierarchyComponent>` pro Ebene hoch und multipliziert pro Ebene eine `mat4` (`cachedLocalMatrix`), nur um die Translation zu lesen. Es gibt keine Änderungserkennung: Kamera still und Entity unverändert ergibt dennoch dieselbe Rechnung jeden Frame. Terrain-Chunks (Hunderte) sind der Hauptfall. Der Kommentar nennt den Grund (frische Entities, `worldMatrix` noch veraltet); das rechtfertigt aber nicht, den Vollpfad für alle statischen Entities jeden Frame zu nehmen. `TransformHierarchy.cpp` erwähnt selbst „baseline §4.5“, dass dies messbar war.
- **Code-Auszug**:
```cpp
auto view = registry.view<LODComponent, MeshComponent>();
for (auto [entity, lod, mesh] : view.each())
{
    if (lod.levels.empty()) continue;
    const glm::vec3 pos = HE::worldPositionOf(world, entity);
    const float dist = glm::distance(cameraPos, pos);
```
- **Lösung**:
  1. Pro LOD-Entity die zuletzt berechnete Weltposition + `structureEpoch` cachen (Laufzeitfeld in `LODComponent`, nicht serialisiert). Neu berechnen nur wenn `structureEpoch` anders ist, `tc.dirty`/Wertvergleich der Kette anschlägt (derselbe Mechanismus wie `propagateTransforms`), oder die Entity frisch ist (Cache leer). Alternative: erst `propagateTransforms(world)` (inkrementell, nur gescannt) am Anfang von `LODSystem::update` aufrufen und dann `tc.worldMatrix[3]` lesen – das beseitigt auch die Sonderbehandlung.
  2. Zusätzlich: Bleibt die Kamera um weniger als der kleinste Abstand zur nächsten Schwelle (pro Entity `distToNextBoundary`) in Bewegung, die Entity überspringen.
  3. Mit quadrierten Distanzen (`maxDistance²` vorab) vergleichen, spart `sqrt` – nur zusammen mit 1 oder 2 sinnvoll, allein eine Mikro-Optimierung.
- **Erwarteter Effekt**: Schätzung: bei ruhiger Kamera und statischer Welt entfällt >90 % der Arbeit der Funktion; bei 1.000 Chunks/Props und Tiefe 2-3 sind das ~3.000 Sparse-Set-Lookups + 2.000 mat4-Produkte pro Frame. Messen (Profiler-Scope „LOD“ existiert in `SceneSystems.cpp:151`).
- **Risiko / Verifikation**: Frisch erzeugte Chunks (TerrainSystem baut am Anfang des Ticks) müssen im selben Tick korrekt eingestuft werden – genau der im Kommentar beschriebene Fehler (alle Chunks vom Ursprung aus gemessen). Test: bestehender Terrain-LOD-Test plus neuer Test „Entity im selben Tick erzeugt und verschoben, Level stimmt“. Floating-Origin (`shiftWorldOrigin`) muss den Cache invalidieren (`invalidateWorldMatrices`).

### SCN-503 – ParticleSystem: ein Physik-Raycast pro Partikel und Frame
- **ID**: SCN-503
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/ParticleSystem.cpp:164-194
- **Problem**: Bei `collisionEnabled` ruft `stepPool` für jedes lebende Partikel jeden Frame `physics->raycast(oldPos, delta, dist)` auf. Bei einem Emitter mit 1.000 Partikeln sind das 1.000 Jolt-Raycasts pro Frame, bei mehreren Emittern ein Vielfaches. Die meisten Partikel bewegen sich pro Frame nur Millimeter bis Zentimeter. Es gibt weder Budget noch Staffelung (alle N Frames) noch eine grobe Vorprüfung (z. B. Emitter-AABB gegen Broadphase).
- **Code-Auszug**:
```cpp
if (collide)
{
    const glm::vec3 delta = newPos - oldPos;
    const float     dist  = glm::length(delta);
    if (dist > 1e-6f)
    {
        const PhysicsWorld::RaycastHit hit = physics->raycast(oldPos, delta, dist);
```
- **Lösung**: (a) Pro Emitter ein Raycast-Budget pro Frame (z. B. `maxCollisionRays`, Round-Robin über den Pool: Partikel i wird nur geprüft, wenn `(i + frame) % k == 0`; das Segment wird dann auf `k·dt` verlängert, so dass kein Tunneling entsteht). (b) Vorprüfung: einmal pro Emitter und Frame die AABB der Partikel (oder Emitter-Radius) gegen die Physik prüfen (Overlap-Test); trifft sie nichts, werden alle Raycasts übersprungen. (c) Batch-API in `PhysicsWorld` (ein Lock/Query-Setup für viele Strahlen).
- **Erwarteter Effekt**: Schätzung: (a) mit k = 2-4 halbiert bis viertelt die Kosten; (b) lässt Partikel in freier Luft ganz kostenlos werden. Messen mit Szene „Regen/Funken 5 Emitter × 1.000 Partikel, Kollision an“.
- **Risiko / Verifikation**: Sichtbares Verhalten ändert sich leicht (Reflexion mit ≤ k Frames Verzögerung). Für exakte Erhaltung nur (b) und (c). Bestehende Particle-Tests (Reflexion/`killOnCollision`) prüfen; neuer Test mit Wand und schnellem Partikel (Tunneling).

### SCN-504 – ParticleSystem: Zustand, Konfig-Auflösung und Pool pro Emitter
- **ID**: SCN-504
- **Kategorie**: C (auch F)
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/ParticleSystem.cpp:63-98 (Auflösung), 107-142, 196-200; src/HE_Scene/include/HorizonScene/Components/ParticleSystemComponent.h:41-42 (Komponente, nur gelesen)
- **Problem**: (1) `resolveConfig` legt vor jedem Auflösen mit `HE::ParticleGraph::makeDefault()` einen Default-Graphen an (Heap), obwohl er direkt danach durch den geparsten ersetzt wird (74-81). (2) Jeder Emitter parst das Graph-JSON selbst; 100 gespawnte Treffer-Effekte (der Kommentar nennt „every few frames“) parsen dasselbe JSON 100-mal und evaluieren 100-mal. (3) `ParticleSystemComponent` enthält ein `std::mt19937 rng {42}` (~2,5 KB Zustand, 624-Wort-Initialisierung bei jeder Konstruktion/Kopie) plus die `LegacyConfig` (~150 B) inline; die Komponentenstride der View beträgt damit >3 KB und der Frame-Durchlauf über viele Emitter (nur `playing`, `emitAccumulator`, `particles` werden gelesen) trifft pro Emitter mehrere Cachelines. (4) `particles` wird nie mit `reserve(maxParticles)` vorbelegt (mehrfaches Wachsen beim ersten Füllen) und `erase(remove_if)` läuft jeden Frame auch wenn kein Partikel starb. (5) Der Kommentar „Remove dead particles (swap with back for O(1) removal)“ beschreibt etwas anderes als den Code (`remove_if` ist stabil und O(n)).
- **Code-Auszug**:
```cpp
HE::ParticleGraph graph = HE::ParticleGraph::makeDefault();
if (const ParticleGraphAsset* asset = cm.getParticleGraph(ps.particleAssetId);
    asset && !asset->nodeGraphJson.empty())
{
    HE::ParticleGraph parsed;
    if (HE::particleGraphFromJson(asset->nodeGraphJson, parsed))
        graph = std::move(parsed);
```
- **Lösung**: (1) `graph` erst im Fehler-/Fallback-Zweig mit `makeDefault()` belegen. (2) Geparsten Graphen pro Asset-ID cachen (`unordered_map<UUID, shared_ptr<const ParticleGraph>>` im System oder im `ContentManager`, invalidiert durch `markConfigDirty`/Asset-Revision); `evaluateParticleGraph` bleibt pro Emitter, weil es `ps.rng` verbraucht (RandomRange). (3) RNG durch einen kleinen Generator ersetzen (PCG32/xoshiro, 8-16 B) oder die `LegacyConfig` per `unique_ptr` auslagern (nur für Migration nötig); der RNG-Tausch ändert die Zufallsfolge. (4) `particles.reserve(min(maxParticles, 256))` beim ersten Emittieren; Erase nur, wenn ein Partikel in der Schleife starb (Flag). (5) Kommentar korrigieren.
- **Erwarteter Effekt**: Schätzung: (1)+(2) beseitigen pro neu gespawntem Effekt ein JSON-Parse + ~100 Heap-Allokationen; (3) senkt den Speicher pro Emitter um ~2,5 KB (bei 1.000 Emittern 2,5 MB) und verbessert die Cache-Dichte beim Durchlauf; (4) entfernt Reallokationen.
- **Risiko / Verifikation**: (3) ändert deterministische Partikelfolgen (Tests mit festem Seed 42, Particle-Graph-Editor-Vorschau „same seed same result“); daher als separaten Schritt und mit Test-Updates. (2) Cache-Invalidierung bei Asset-Edit (Particle Graph Editor) testen.

### SCN-505 – HorizonWorld::clear() ist bei flachen Hierarchien quadratisch
- **ID**: SCN-505
- **Kategorie**: D (auch C)
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/HorizonWorld.cpp:535-548 (clear), 496-511 (destroyEntity), 480-494 (destroyRecursive)
- **Problem**: `clear()` ruft für jedes Kind der Wurzel `destroyEntity(c)`. Jedes `destroyEntity` löscht die Entity per `erase(remove(...))` aus `children` der Wurzel (linear). Bei n Wurzelkindern sind das ~n²/2 Vergleiche; `createEntity` hängt alles zuerst an die Wurzel, daher sind flache Szenen (Prefab-Spawns, Foliage-/Cell-Entities, Tests) üblich. Bei 100.000 Wurzelkindern ≈ 5·10⁹ Elementvergleiche (Schätzung). Danach baut `clear()` zusätzlich einen `strays`-Vektor aller Entities und durchläuft sie noch einmal; `destroyRecursive` kopiert pro Knoten den `children`-Vektor (Heap-Allokation pro Entity mit Kindern). Da jede Entity einen `EntityIdComponent` hat, feuert zusätzlich pro Zerstörung der `on_destroy`-Hook (siehe SCN-506).
- **Code-Auszug**:
```cpp
const std::vector<Entity> children = rh->children;
for (Entity c : children)
    destroyEntity(c);
std::vector<Entity> strays;
for (auto e : m_registry.view<entt::entity>())
    if (e != m_rootEntity) strays.push_back(e);
```
- **Lösung**: In `clear()` zuerst `rh->children.clear()` (die Wurzel behält am Ende keine Kinder, dasselbe Ergebnis) und dann alles außer der Wurzel direkt zerstören – die zweite Schleife (`strays`) erledigt das bereits vollständig, die `destroyEntity`-Schleife ist damit überflüssig (der Built-in-Guard wird ohnehin umgangen, siehe Kommentar 547). Alternativ `m_registry.destroy(view.begin(), view.end())`-Bereich, wobei die Wurzel auszunehmen ist. In `destroyRecursive` die Kinder vom Ende her abbauen (`while(!ch.empty()){ c = ch.back(); ch.pop_back(); destroyRecursive(c);}`) statt zu kopieren; wenn die Kinder-Liste des Elternknotens anschließend ohnehin freigegeben wird, spart das die Kopie.
- **Erwarteter Effekt**: Schätzung: Szenenwechsel/PIE-Stop mit 100k flachen Entities von Sekunden/Minuten auf Millisekunden bis ~100 ms; Einsparung einer Heap-Allokation je Entity mit Kindern.
- **Risiko / Verifikation**: `m_structureEpoch`/`m_hierarchyDirty` müssen weiter gesetzt werden (Zeilen 548, 562); `fireLevelUnloaded`/Widgets/Level-Skript-Reihenfolge bleibt unberührt. Test: `clear()` mit 50k Wurzelkindern (Laufzeit), Sky + Sonne/Mond wird entfernt, Wurzel bleibt mit leerem `children`, `findByEntityId` danach leer.

### SCN-506 – Entity-ID-Index: zwei Hash-Knoten pro Entity
- **ID**: SCN-506
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/HorizonWorld.cpp:449-478 (+ 408-424 createEntity); src/HE_Scene/include/HorizonScene/HorizonWorld.h:294-301 (nur gelesen)
- **Problem**: Jede Entity erzeugt zwei Heap-Knoten: einen in `m_idIndex` (`unordered_multimap<UUID, Entity>`) und einen in `m_idIndexed` (`unordered_map<Entity, UUID>`). Dazu läuft pro Konstruktion/Update/Zerstörung ein Signal-Delegat (`on_construct`, `on_update`, `on_destroy`), und `unindexEntityId` macht `equal_range` + lineare Suche über die Halter derselben ID. Beim Laden großer Szenen (100k Entities) sind das 200k kleine Allokationen und doppeltes Hashing (UUID 128 Bit); `createEntity` ruft zusätzlich `UUID::generate()` und kopiert den Namen (`NameComponent{ name }`, Heap bei > SSO) sowie legt `HierarchyComponent` mit einem Kinder-Vektor an. Die Rückwärtsabbildung dient nur dazu, bei `on_update` die alte ID zu kennen.
- **Code-Auszug**:
```cpp
m_idIndex.emplace(id, entity);
m_idIndexed[entity] = id;
...
const auto holders = m_idIndex.equal_range(indexed->second);
```
- **Lösung**: Die Rückwärtsabbildung als dichten Vektor über den Entity-Index führen (`std::vector<HE::UUID> m_indexedId` indexiert mit `entt::to_entity(e)`, Null-UUID = nicht indexiert): kein Knoten pro Entity, O(1) ohne Hash. Für `m_idIndex` `reserve()` beim Szenenladen (Anzahl bekannt) bzw. eine offene Adressierung (flat map) verwenden; die seltenen Duplikate (zwei Halter derselben ID) in einer kleinen Nebenliste führen, so dass der Normalfall eine einfache `unordered_map<UUID, Entity>` ist. `createEntity` mit `const std::string&` + `std::move`-Überladung für Namen.
- **Erwarteter Effekt**: Schätzung: Halbierung der Allokationen beim Laden/Spawnen (−1 Knoten/Entity), weniger Fragmentierung; messbar im Scene-Load-Profil (nicht gemessen).
- **Risiko / Verifikation**: Die Semantik „zwei Halter derselben ID bleiben findbar“ (Kommentar 469) und „Null-ID wird nicht indexiert“ müssen bleiben. Tests: bestehende World-ID-Tests (Prefab zweimal, collab), plus Fall „emplace_or_replace mit anderer ID“ und „Entity-Index wird wiederverwendet (Generation)“.

### SCN-507 – MovementSystem: pro Mover und Frame Physik-Aufruf und Komponentenabfragen ohne Änderungsprüfung
- **ID**: SCN-507
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/MovementSystem.cpp:54-158 (Kern 110-119, 130-132, 156-157)
- **Problem**: Die View `MovementComponent, TransformComponent` wird komplett jeden Frame durchlaufen, auch für Entities, die ruhen (`moveInput == 0`, `lookYaw == 0`). Für jede wird (a) `reg.all_of<CharacterControllerComponent>` und `physics->setCharacterVelocity(id, …)` aufgerufen, also pro Frame ein Aufruf in die Physik-Schicht (ID-Lookup) mit dem identischen Wert wie im Vorframe, (b) `reg.try_get<RootMotionComponent>(e)` ausgewertet, obwohl das Ergebnis nur im `orientToMovement`-Zweig gebraucht wird (rmc/`rootOwnsYaw` bei ausgeschaltetem `orientToMovement` ungenutzt), (c) `moveInput`/`lookYaw` zurückgesetzt. Idle-NPCs (hunderte) kosten somit jeden Frame zwei Sparse-Set-Lookups und einen Physikaufruf.
- **Code-Auszug**:
```cpp
if (physics && reg.all_of<CharacterControllerComponent>(e))
{
    const auto& cc = reg.get<CharacterControllerComponent>(e);
    physics->setCharacterVelocity(static_cast<uint32_t>(e),
                                  glm::vec3(planar.x, cc.velocity.y, planar.z));
}
...
const auto* rmc = reg.try_get<RootMotionComponent>(e);
const bool rootOwnsYaw = rmc && rmc->mode != RootMotionComponent::Mode::Off
                      && rmc->options.extractYaw;
```
- **Lösung**: (a) `rmc`/`rootOwnsYaw` in den `if (mv.orientToMovement …)`-Zweig verschieben (reine Umordnung, identisches Verhalten). (b) Zuletzt geschriebene planare Geschwindigkeit in einem Laufzeitfeld (`MovementComponent::lastPlanar`, nicht serialisiert) halten und `setCharacterVelocity` nur bei Änderung oder bei `physics`-Neuaufbau aufrufen; der Zustand „einmal 0 schreiben beim Stoppen“ bleibt erhalten. (c) Optional: eine Entity mit `moveInput == 0 && lookYaw == 0 && lastPlanar == 0` früh per `continue` verlassen.
- **Erwarteter Effekt**: Schätzung: für 200 idle Charaktere ~400 Lookups + 200 Physikaufrufe pro Frame weniger; (a) allein ist kostenlos und sofort umsetzbar.
- **Risiko / Verifikation**: (b) setzt voraus, dass die Physik die gesetzte Geschwindigkeit hält (Kommentar in NavigationSystem 623-624 sagt „A character keeps the velocity it was last given“) und dass nach einem Neuaufbau des Charakters (Streaming, Respawn) neu geschrieben wird – Invalidierung bei `hasCharacter`-Wechsel. Test: Charakter stoppt (Velocity 0 einmal), Sprung (Y bleibt), Charakter wird neu erzeugt.

### SCN-508 – NavigationSystem: fehlgeschlagene Pfadsuche wird jeden Frame wiederholt
- **ID**: SCN-508
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/NavigationSystem.cpp:644-664 (Repath), 422-494 (planPath)
- **Problem**: Schlägt `planPath` fehl (Ziel/Agent außerhalb der NavMesh, Insel ohne Verbindung), bleibt `agent.hasPath == false` und `agent.moving == true`; im nächsten Frame wird erneut `planPath` aufgerufen: zwei `findNearestPoly` (je Box-Query über die BV-Tree), im Insel-Fall zusätzlich `findPath` mit 2048 Knoten. Der Kommentar (629-634) beschreibt genau dieses Szenario („the repath fails on every frame from then on“) und behebt nur das Gleiten, nicht die Kosten. Ein Verfolger, dessen Ziel außerhalb der Mesh steht, kostet so jeden Frame eine volle Suche, auch wenn das Ziel sich nicht bewegt hat.
- **Code-Auszug**:
```cpp
if (!agent.hasPath && !planPath(agent, worldPos, *query, id, glm::vec3(world.origin())))
{
    releaseCharacter();
    continue;
}
```
- **Lösung**: Nach einem Fehlschlag in einem Laufzeitfeld (`NavAgentComponent::nextPlanTime` bzw. `failedTarget` + Zähler) merken und erst neu planen, wenn (a) sich `targetPos` um mehr als `kRepathDistance` gegenüber dem fehlgeschlagenen Ziel bewegt hat, oder (b) ein Cooldown (z. B. 0,25-0,5 s) abgelaufen ist (der Agent bewegt sich selbst, daher kann sich die Start-Polygon ändern). Das Log bleibt gedrosselt wie bisher.
- **Erwarteter Effekt**: Schätzung: Faktor 15-30 weniger Suchen für gestrandete Agenten (bei 60 FPS und 0,25-s-Cooldown).
- **Risiko / Verifikation**: Reaktionszeit des Agenten, sobald das Ziel wieder auf der Mesh liegt, wird um bis zu den Cooldown verzögert; (a) ohne Cooldown erhält das Verhalten fast exakt. Test: Ziel off-mesh → kein `findPath` pro Frame (Zähler), dann Ziel zurück auf Mesh → Pfad innerhalb Cooldown.

### SCN-509 – NavigationSystem::update: Mehrfach-Durchläufe und doppelte Kettenwanderung
- **ID**: SCN-509
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/NavigationSystem.cpp:576-591 (autoStart), 599-602 (Warnung), 655, 764-778 (Transform-Bewegung)
- **Problem**: (1) Mit `physics` wird jeden Frame die komplette `NavAgentComponent`-View nur für das einmalige Latchen von `autoStart` durchlaufen. (2) Ohne NavMesh folgt jeden Frame eine zweite View nur für die Warnung. (3) Für bodenlose Agenten (Transform-Pfad) wird `worldPositionOf` (Kettenwanderung) einmal in 655 und dann in `localPositionForWorld` (`worldMatrixOf(parent)` + `glm::inverse`, 379-380) erneut pro Frame berechnet; jeder Agent mit Elternteil kostet also zwei Ketten und eine 4×4-Inversion pro Frame.
- **Code-Auszug**:
```cpp
for (auto [e, agent] : reg.view<NavAgentComponent>().each())
{
    if (!agent.autoStart || agent.autoStarted) continue;
    agent.autoStarted = true;
```
- **Lösung**: (1) autoStart in die Hauptschleife (639-642) einfalten bzw. nur bei „Play gestartet“-Flanke ausführen (Zähler/Flag der App, das `physics != nullptr` bereits signalisiert); (2) Warnung aus der ersten View-Iteration herleiten (zählt `moving` ohnehin); (3) `localPositionForWorld` eine Überladung mit bereits bekannter Parent-Weltmatrix geben (einmal `worldMatrixOf(parent)` und daraus `worldPos`, spart die zweite Kette); die Inversion nur berechnen, wenn der Parent nicht Identität ist.
- **Erwarteter Effekt**: Schätzung: gering (je ~N Iterationen); relevant erst bei tausenden Agenten. Reine Vereinfachung/Mikroeinsparung.
- **Risiko / Verifikation**: autoStart-Reihenfolge („before any early return“, Zeile 568-570) muss erhalten bleiben; Test: autoStart-Agent startet im ersten Tick auch ohne baked NavMesh (Warnung kommt).

### SCN-510 – Replikatoren: joinedConnections() pro Entity und dreifach duplizierte Hilfsfunktionen
- **ID**: SCN-510
- **Kategorie**: A (auch C)
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/Net/SpawnReplicator.cpp:61-68, 114-115, 48-51; src/HE_Scene/src/Net/PropertyReplicator.cpp:33-38, 55-58, 80-88; src/HE_Scene/src/Net/RpcRouter.cpp:50-55, 69-72, 108-116
- **Problem**: In `bindSceneEntities` wird in der Schleife über alle Kandidaten für jede Entity `joinedConnections()` (liefert einen neuen `vector` per Wert) aufgerufen; bei 5.000 Szenen-Entities sind das 5.000 Vektor-Allokationen beim Hosten bzw. beim Streamen einer Zelle. Außerdem existieren drei fast identische Kopien von `isAuthority()`, `joinedConnections()` und `alreadyLogged()` (Linearsuche in `vector<string>`) in den drei Replikatoren.
- **Code-Auszug**:
```cpp
for (const ConnectionId conn : joinedConnections())
    sendBind(conn, netId, uuid);
```
- **Lösung**: `joinedConnections()` vor der Schleife einmal bestimmen (bleibt gültig, es werden keine Verbindungen verändert). Die drei Hilfen in einen kleinen gemeinsamen Header (`Net/ReplicatorCommon.h`: `isAuthority(NetRole)`, `joinedConnections(NetSession*, const JoinedFilter&)`, `LogOnce`) auslagern. Die Varianten sind verhaltensgleich (Spawn: `!m_joined || m_joined(conn)`; Property/Rpc: ohne Filter alle Verbindungen).
- **Erwarteter Effekt**: Schätzung: −N Allokationen beim Host-Start; deutlich weniger Duplikat-Code.
- **Risiko / Verifikation**: Gering; bestehende Net-Tests (Bind/Spawn/Join) genügen.

### SCN-511 – PropertyReplicator: Tabellen-Broadcast sendet doppelt, Probe kodiert jeden Wert zweimal
- **ID**: SCN-511
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/Net/PropertyReplicator.cpp:337-341 (broadcastTableFor), 247-335 (sendTableFor, insb. 261-271, 296-333)
- **Problem**: `broadcastTableFor` ruft `sendTableFor(netId, conn)` je Verbindung auf. Jeder Aufruf führt `collect` + Probe-Kodierung (`BitWriter probe; writeValue(probe, …)` je Variable, danach nochmals in `writeTable`) aus. Ändert sich die Namensliste (`listChanged`), schickt der erste Aufruf die Tabelle an ALLE verbundenen Clients (318); die folgenden Aufrufe für Verbindung 2…N sehen die Liste nun als unverändert und senden sie an ihre Verbindung erneut (331). Clients 2…N erhalten die Tabelle zweimal (zuverlässiger Kanal, `handleTable` ist idempotent, daher kein Fehler, aber doppelter Verkehr und N× Collect/Probe).
- **Code-Auszug**:
```cpp
void PropertyReplicator::broadcastTableFor(std::uint32_t netId)
{
    for (const ConnectionId conn : joinedConnections())
        sendTableFor(netId, conn);
}
```
- **Lösung**: `sendTableFor` in zwei Teile trennen: `buildTable(netId) → optional<Table>` (collect + Filter, einmal) und `sendTable(conn|alle)`. `broadcastTableFor` baut einmal und sendet mit `sendTo(joined, …)`; Probe und Schreiben in einem Durchgang (Wert in den Writer schreiben, bei Fehlschlag Writer-Position zurücksetzen oder Vorprüfung per `isReplicableType` + Strukturcheck ohne Bits).
- **Erwarteter Effekt**: Schätzung: halbiert den Traffic dieses (seltenen) Ereignisses für N ≥ 2 Clients und senkt die CPU-Kosten bei Join-Wellen (Tabellen aller Entities × Clients).
- **Risiko / Verifikation**: Reihenfolge/Inhalt der Nachrichten ist testrelevant (Join-Sequenz Welcome → Binds → Tabellen → Baseline). Test: zwei Clients, Variable mitten in der Session deklariert → jeder Client empfängt genau eine Tabelle.

### SCN-512 – RpcRouter::accept: Schlüssel-String pro Aufruf, unbegrenzt wachsende Log-Liste
- **ID**: SCN-512
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/Net/RpcRouter.cpp:523-534 (Schlüssel), 443-446 (Signatur), 345 (args), 50-55 (alreadyLogged)
- **Problem**: Für jeden CallServer an ein Lua/Python/Native-Ziel (`!sig.found`) wird `std::to_string(netId) + ":" + fn` gebaut (Heap) und linear in `m_missingLogged` (`vector<string>`) gesucht. Die Liste wächst um einen Eintrag je (Entity, Funktion) über die ganze Session und wird nur in `clear()` geleert; bei vielen gespawnten Entities mit Lua-Handlern wächst sie unbegrenzt und die Suche wird O(n). Zusätzlich liefert `functionSignatureOf` eine Struktur mit Vektoren per Wert und `handleRpc` legt `std::vector<Value> args(argc)` an (große `Value`-Objekte); bei durch `kRpcRateWindowSec` begrenzter Rate unkritisch.
- **Code-Auszug**:
```cpp
const std::string key = std::to_string(netId) + ":" + fn;
if (!alreadyLogged(m_missingLogged, key))
    HE_LOG_INFO(Replication, "CallServer '%s' on net id %u: no HorizonCode signature ...
```
- **Lösung**: `std::unordered_set<uint64_t>` mit `hash(netId, fn)` (z. B. `netId * 0x9E37… ^ std::hash<string>{}(fn)`) statt String-Schlüssel; Obergrenze (z. B. 256 Einträge, danach nur noch Zähler), oder den Logeintrag je Funktionsname statt je Entity.
- **Erwarteter Effekt**: Schätzung: Entfernt eine Allokation je RPC und das unbeschränkte Wachstum; praktisch gering, solange RPC-Raten klein bleiben.
- **Risiko / Verifikation**: Nur Loghäufigkeit ändert sich; kein Testbedarf außer „Hinweis erscheint einmal pro Name“.

### SCN-513 – NetGameSession: doppelter `dropConnection`-Aufruf und kleine Redundanzen
- **ID**: SCN-513
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Scene/src/Net/NetGameSession.cpp:1060-1067, 891-912, 1022-1025/1124-1130
- **Problem**: In der Schleife über `m_rejected` steht `if (m_rpc) m_rpc->dropConnection(conn);` zweimal (Zeile 1064 und 1066), die zweite mit falsch eingerücktem, verwaistem Kommentar (Rest eines Merge). `dropConnection` ist idempotent (`m_rate.erase`), der zweite Aufruf ist reiner Ballast. `reapSeveredLinks` läuft jeden Frame auf dem Host mit O(Spieler × Verbindungen) und `update()` liest `steady_clock::now()` bis zu zweimal pro Frame (Browser + Announcer); beides vernachlässigbar.
- **Code-Auszug**:
```cpp
if (m_rpc) m_rpc->dropConnection(conn);
// A connection id the transport reuses must inherit no rate history.
if (m_rpc) m_rpc->dropConnection(conn);
```
- **Lösung**: Zeile 1065-1066 löschen und den Kommentar vor Zeile 1064 stellen. Optional `nowMs` einmal pro `update()` bestimmen.
- **Erwarteter Effekt**: Keiner messbar; Lesbarkeit.
- **Risiko / Verifikation**: Keines; kompilieren und Net-Tests.

### SCN-514 – ValueWire: Temporär-`Value` je Container-Element, tote Zweige im JSON-Pfad
- **ID**: SCN-514
- **Kategorie**: C (auch F)
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Scene/src/Net/ValueWire.cpp:222-233, 134-139, 345-353
- **Problem**: `readValueAt`/`readScalarPayload` konstruieren pro Element ein vollwertiges `Value` (`Value e;`, `Value k;`, `Value f;`) – eine große Struktur mit Strings, Vektoren und mehreren glm-Typen – und verschieben es dann in `items`. Bei großen Float-Arrays (≤ 65.535 Elemente) bedeutet das viele große Konstruktionen/Verschiebungen; Allokationen entstehen nur bei Strings/Strukturen. Im JSON-Pfad ist der Zweig `case PinType::Struct` in `appendScalarJson` unerreichbar (Struct wird vorher in `appendJson` behandelt), und `Ref`/`Struct`/`Exec` führen alle zu `"null"` (zwei getrennte Fälle).
- **Code-Auszug**:
```cpp
Value e;
if (!readScalarPayload(r, e, type, depth)) return false;
out.items.push_back(std::move(e));
```
- **Lösung**: `out.items.emplace_back()` und direkt in `out.items.back()` lesen (bei Fehler `pop_back`). Dann `Ref/Exec/Struct` in `appendScalarJson` zu einem Fall zusammenfassen und den toten Struct-Zweig entfernen.
- **Erwarteter Effekt**: Schätzung: gering; spart pro Element eine Move-Konstruktion einer großen Struktur. Unsicher / messen (Arrays dieser Größe werden selten repliziert).
- **Risiko / Verifikation**: Fehlerpfad (halb gelesenes Element) muss `items` konsistent lassen; Test mit abgeschnittenem Datagramm (bestehende Malformed-Tests).

### SCN-515 – Schwere Includes in öffentlichen Headern
- **ID**: SCN-515
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: TransformHierarchy.h:3 (`entt/entt.hpp`, 31 Includer), UIInputSystem.h:2 (`entt/entt.hpp`, ungenutzt), UISystem.h:3, WaterQuery.h:5, src/NotifyCollect.h:8, WaterLake.h:3-4 (`TerrainSculpt.h`, `HorizonWorld.h`), WaterMesh.h:2 und TerrainMeshGenerator.h:2 (`ContentManager/Assets.h`), WeatherAudio.h:2 (`WeatherComponent.h`), UICursorSDL.h:2 (`SDL3/SDL.h`), SplineCurve.h:2-3
- **Problem**: Mehrere Header ziehen die komplette entt-Bibliothek, `Assets.h` oder `HorizonWorld.h` nur für einen Typnamen (`entt::entity`, `StaticMeshAsset` als Rückgabetyp, `Entity`). `UIInputSystem.h` benutzt keinen einzigen entt-Typ. `entt/entity/fwd.hpp` (vorhanden unter `src/HE_Scene/vendor/entt/entity/fwd.hpp`, enthält `enum class entity`) genügt für `entt::entity` und `entt::registry&`. Funktionsdeklarationen mit `StaticMeshAsset` als Rückgabetyp brauchen nur eine Vorwärtsdeklaration.
- **Code-Auszug**:
```cpp
// UIInputSystem.h
#include <entt/entt.hpp>
#include <cstdint>
```
- **Lösung**: `UIInputSystem.h`: Include entfernen. `TransformHierarchy.h`, `UISystem.h`, `WaterQuery.h`, `NotifyCollect.h`: `<entt/fwd.hpp>`; die .cpp-Dateien, die entt-Operationen brauchen, binden `entt.hpp` selbst ein. `TerrainMeshGenerator.h`: `struct StaticMeshAsset;` (alle Aufrufer binden `Assets.h` bereits für den Rückgabewert). `WaterLake.h`: `TerrainSculpt::Result` per Vorwärtsdeklaration ist nicht möglich (Rückgabe per Wert in Deklaration geht, aber Aufrufer brauchen den Typ) – dort nur `HorizonWorld.h` gegen `class HorizonWorld; using Entity = entt::entity;` tauschen, falls `Entity` ein Alias ist.
- **Erwarteter Effekt**: Schätzung: spürbar kürzere Kompilierzeiten bei Änderungen an diesen Headern; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Reines Compile-Risiko (fehlende Includes in Einbindern). Gesamtbuild (`HorizonEditor`, `he_tests`, `HorizonGame`) auf allen Plattformen.

### SCN-516 – NavMesh-Bake blockiert den aufrufenden (Editor-)Thread
- **ID**: SCN-516
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Scene/src/NavigationSystem.cpp:106-163 (collectStaticGeometry), 166-395 (bake), 91-103 (movesAtRuntime)
- **Problem**: `collectStaticGeometry` und `bake` laufen synchron im Aufrufer (Editor-Panel): `HE::propagateTransforms(world)`, dann pro Mesh `movesAtRuntime` (drei `try_get` je Vorfahre, O(Meshes × Tiefe)), Geometrie in `vector<float>/vector<int>` kopieren, danach Recast-Pipeline bis zur Grenze `kMaxGridCells = 4096² = 16,7 M` Zellen (Heightfield mit Span-Zeigern: hunderte MB). Der Code selbst verweist auf „the editor simply stops responding“ und `HE_LOG_SLOW_SCOPE(250 ms)`. Editor-only, nicht pro Frame, daher niedrig.
- **Code-Auszug**:
```cpp
constexpr long long kMaxGridCells = 4096ll * 4096ll;
...
rcHeightfield* hf = rcAllocHeightfield();
```
- **Lösung**: Bake als Job (JobSystem) mit eigener `rcContext` und Fortschrittsanzeige/Abbruch ausführen; Geometriesammlung (World-Zugriff) bleibt im Hauptthread, `bake(nmc.geometry)` ist bereits ein reiner Funktionsaufruf auf Kopien und lässt sich auslagern. Ergebnis (`navMesh`/`navQuery`) im Hauptthread übernehmen.
- **Erwarteter Effekt**: Editor bleibt während des Bakes bedienbar; Gesamtdauer unverändert.
- **Risiko / Verifikation**: Lebensdauer von `nmc` während des Bakes (Undo, Szenenwechsel); Ergebnis nur anwenden, wenn die Komponente noch existiert. Test: Bake im Hintergrund + Szenenwechsel.

---

## Cross-Modul-Hinweise

- `src/HE_Scene/src/WaterLake.cpp` (`syncSplines`, WaterLake.h:278-284): laut Header wird pro Frame für jeden See ein Fingerabdruck aus Spline-Punkten, Closed-Flag und BEIDEN Weltmatrizen berechnet (`worldMatrixOf` Spline + Terrain). Ohne Dirty-Hinweis (z. B. `SplineComponent`-Revision + Transform-`structureEpoch`) ist das ein ECS-Durchlauf ohne Änderungsmarker. Implementierung liegt in einem anderen Slice; dort prüfen.
- `src/HE_Scene/src/WaterQuery.cpp` (`WaterIndex::refresh`, WaterQuery.h:60-90): wird laut Header einmal pro Physikschritt aufgerufen und berechnet Landscape-`localToWorld`/`worldToLocal` (Inverse) neu; `WaterIndex::build(world)` baut für Einzelabfragen (Skript) einen kompletten Index inklusive Wet-Rect-Scan (O(res²)). Prüfen, ob Skript-Wasserabfragen pro Frame/Entity darauf laufen (EngineApi).
- `src/HE_Scene/src/UISystem.cpp`: `nestingDepth` (UISystem.h:30) und `computeScreenRect` (:37) laufen pro Element die UI-Elternkette hoch; `extract` ruft sie vermutlich pro Element und Frame auf – O(Elemente × Tiefe) ohne Layout-Cache. Dort mit Dirty-Layout prüfen.
- `src/HE_Core/.../HorizonCode` (`Runtime::replicatedVariablesOf`, `getVariable`): von `PropertyReplicator::update` pro Entity und Frame aufgerufen (SCN-501). Eine `replicatedRevision(inst)` und ein Zugriff per Slot-Index statt Namen würden SCN-501 vollständig lösen.
- `src/HE_Scene/include/HorizonScene/Components/ParticleSystemComponent.h:41-42`: `std::mt19937` (~2,5 KB) und `LegacyConfig` inline (SCN-504).
- `src/HE_Scene/src/TransformHierarchy.cpp:321-362`: `worldMatrixOf` ist korrekt allokationsfrei für Tiefe ≤ 32; jeder Aufrufer (LOD, Particle, Navigation, Water) zahlt die Kettenwanderung pro Aufruf erneut. Ein per-Frame-Cache (Epoche + Entity) würde alle vier entlasten.

## Unsicher / Messung nötig

- SCN-501: tatsächliche Kosten von `replicatedVariablesOf`/`getVariable` (Kopie oder Referenz, interpretiertes vs. generiertes Backend) – Profil `NetGameSession::update` mit 200-1.000 replizierten Entities.
- SCN-502: Anteil von `LODSystem::update` an einem Frame mit ~1.000 Terrain-Chunks/Props; ob `propagateTransforms` im selben Tick vorgezogen werden kann.
- SCN-503: Kosten eines `PhysicsWorld::raycast`-Aufrufs (Locking, Broadphase) und typische Partikelzahl mit `collisionEnabled`.
- SCN-506: Szenenladezeit-Anteil von Index-Pflege und `UUID::generate()` (RNG-Quelle, evtl. Mutex/`random_device`).
- SCN-514: ob je große Float-Arrays repliziert werden.
- `std::shared_ptr` für `nmc.navMesh`/`navQuery` mit Lambda-Deleter (NavigationSystem.cpp:380, 389): Eigentum wird offenbar geteilt (Komponentenkopien in Undo/Szenenladen); ob `unique_ptr` genügt, ist in diesem Slice nicht entscheidbar.
- `WaterField::Delta` (WaterField.h:545-554) kopiert komplette `Body`-Listen (inkl. `polygon`, `GroundPatch::delta`) vorher/nachher je Strich: Speicher pro Undo-Schritt; nur relevant, falls Delta tatsächlich verwendet wird (Header sagt: Undo nutzt Welt-Snapshot).

## Gelesen

Ja, alle Dateien des Slice 5 vollständig gelesen (HorizonWorld.cpp, LODSystem.cpp, MovementSystem.cpp, NavigationSystem.cpp, Net/NetGameSession.cpp, Net/PropertyReplicator.cpp, Net/RpcRouter.cpp, Net/SpawnReplicator.cpp, Net/ValueWire.cpp, NotifyCollect.h, ParticleSystem.cpp sowie alle Header von SplineCurve.h bis WeatherSystem.h); zusätzlich punktuell TransformHierarchy.cpp (321-386), SceneSystems.cpp (100-155) und ParticleSystemComponent.h zur Verifikation.
