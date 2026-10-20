# Scene Slice 2 – EngineApi.cpp, EntityActive/EntityHost/EntityVisibility, EnvironmentPush, FloatingOrigin, FlyCameraController, FoliagePaint, FoliageSystem, GameInstanceHost

## Zusammenfassung

Der Slice ist funktional sauber und sehr gut kommentiert; die Probleme sind weniger Logikfehler als Kosten an den Rändern. Der größte Hebel ist die Skript-Grenze in `EngineApi.cpp`: jeder Registry-Aufruf läuft über `std::function`, einen `std::vector<Value>` für Argumente und einen für das Ergebnis, mit einem 280-Byte-`Value` (drei `std::string`, zwei `std::vector`). Der Self-Default-Wrapper kopiert diese Argumentliste bei jedem unverdrahteten Entity-Pin ein zweites Mal. Das ist pro Tick und pro Aufruf aus HorizonCode, Lua und Python relevant. Daneben gibt es mehrere O(n)-Pfade pro Aufruf oder Frame ohne Dirty-Flag (`EntityHost::tick`, `findByName`, Outliner-Zeilen, `FoliageSystem`), synchrone I/O auf dem Update-Thread (Prefs, Watch-Polling) und eine vollständige Foliage-Neuverteilung pro Mal-Frame im Editor. Funde: 1 Hoch, 9 Mittel, 8 Niedrig. Alle Zeilenangaben sind gegen den aktuellen Stand verifiziert; Größenordnungen sind Schätzungen und unter "Unsicher / Messung nötig" aufgeführt.

---

### SCN-201 – Skript-Dispatch: zwei Vektor-Allokationen und ein 280-Byte-`Value` pro API-Aufruf
- **ID**: SCN-201
- **Kategorie**: B / C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:5958-6017` (Leser `aF/aI/aS/aV3`), `:6021-6029` und fast alle Zeilen bis `:8580` (Registry), `src/HE_Core/include/HorizonCode/HorizonCode.h:93-124` (`Value`)
- **Problem**: Jede Registry-Zeile ist ein `[](Ctx&, const std::vector<Value>&) -> std::vector<Value>`. Der Aufrufer baut einen Argumentvektor (Heap), der Rückgabewert ist wieder ein `std::vector<Value>` (Heap, `VV{ Value::ofFloat(...) }`). `Value` hat 280 Byte (hand nachgerechnet, passt zu `sizeof`): drei `std::string` (`s`, `keyTypeName`, `typeName`), zwei `std::vector<Value>` (`items`, `keys`) und vier Vektorfelder, die nur für einen Typ benutzt werden. `Value::ofX` konstruiert und kopiert diesen Block, `aS()` kopiert Strings pro Argument (`return k < a.size() ? a[k].s : std::string()`). Ein `transform.getPosition` aus einem Tick-Graph bewegt so 12 Byte Nutzdaten über mindestens zwei Heap-Allokationen und ca. 560 Byte Kopierarbeit. Die Registry ist der einzige Weg für Lua/Python/HorizonCode, das trifft also jeden Skript-Frame.
- **Code-Auszug**:
```cpp
t.push_back({ "transform.getPosition", "Transform", false, {{"entity", P::Int}}, {{"position", P::Vec3}}, "HE::api::transform::getPosition",
    [](Ctx& c, const VV& a){ return VV{ v3(transform::getPosition(c, (Entity)aI(a, 0))) }; } });
```
- **Lösung**: (1) Billig und verhaltenserhaltend: `Value` verschlanken, indem Container-/Typnamen-Zustand in einen selten genutzten Unterblock wandert (`std::unique_ptr<ContainerPayload>`), das Skalar-`Value` schrumpft dann auf ca. 100 Byte und `ofX` bleibt allokationsfrei. (2) `aS()` als `const std::string&` mit statischem leerem String zurückgeben (und die C++-Funktionen nehmen ohnehin `const std::string&`), das spart die Stringkopie pro Argument. (3) Optional und größerer Umbau: ein zweiter, typisierter Fast-Path für die häufigsten Zeilen (Getter/Setter mit Int/Float/Vec3), den der HorizonCode-Interpreter und die Codegen bevorzugen (`cppCall` existiert bereits als Feld `"HE::api::..."`, der generierte C++-Code umgeht die Registry also schon).
- **Erwarteter Effekt**: Schätzung: 2 bis 4 Heap-Allokationen und etwa 0,3 bis 1 KB Kopie weniger pro Aufruf; sichtbar erst bei hunderten Aufrufen pro Frame. Messen mit einem Graph, der 1000 `getPosition` pro Tick ausführt.
- **Risiko / Verifikation**: `Value` ist ABI-nah (Hc-Codegen `HorizonCodeGenSupport.h` dupliziert Teile, Kommentar in `HorizonCode.h:99-103`). Tests: `test_engine_api` (Arity pro Zeile), die HorizonCode-Paritätstests, Save-Roundtrip (`valueToJson/valueFromJson`).

### SCN-202 – Self-Default-Wrapper kopiert die komplette Argumentliste und verdoppelt die `std::function`-Ebene
- **ID**: SCN-202
- **Kategorie**: C / B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:8488-8527` (Entity), `:8550-8577` (Widget)
- **Problem**: Der Post-Pass umhüllt jede Zeile mit erstem Parameter `entity` (rund 60) in ein weiteres `std::function`, das die Original-`invoke` per Move einfängt. Der Kommentar sagt, der Pin sei bewusst meist unverdrahtet ("for every read of its own speed"), der Pfad `a[0].i == 0` ist also der Normalfall für Self-Aufrufe. Dort passiert `VV withSelf = a;`: tiefe Kopie aller Argumente (je 280 Byte plus String-Heap) nur, um Element 0 zu überschreiben. Danach `entity::self(c)` mit Runtime-Lookup. Zusätzlich zwei verschachtelte `std::function`-Aufrufe pro API-Aufruf.
- **Code-Auszug**:
```cpp
VV withSelf = a;
withSelf[0] = Value::ofInt(static_cast<int>(me));
return inner(c, withSelf);
```
- **Lösung**: Die Substitution an die Aufrufstelle verlegen, an der `a` ohnehin beschreibbar ist (ApiFn::invoke nimmt `const VV&`; eine zweite Signatur `invoke(Ctx&, VV&)` oder ein `selfDefault`-Flag, das der Dispatcher selbst auswertet). Minimal: `inner` mit einem veränderbaren Argumentvektor aufrufen (`VV withSelf(a.begin(), a.end())` ist dieselbe Kopie, deshalb besser den Aufrufer `VV` per Wert übergeben lassen, wo er es ohnehin gebaut hat). Alternativ den Flag `params[0].selfDefault` (existiert schon) im Dispatcher prüfen, dann entfallen beide Wrapper-Lambdas.
- **Erwarteter Effekt**: Schätzung: eine Allokation plus Kopie von ca. 300 Byte pro Argument weniger bei jedem Self-Aufruf; ein indirekter Aufruf weniger.
- **Risiko / Verifikation**: Das Verhalten ist durch Tests belegt (Kommentar nennt `test_engine_api`); die Reihenfolge der Substitution im Dispatcher (Lua/Python/HorizonCode teilen `ApiFn::invoke`) muss erhalten bleiben. Test: Entity-Pin leer lassen, Aufruf aus Klasse und aus Text-Skript (Warnpfad `me == 0`).

### SCN-203 – `EntityHost::tick`: jeden Frame alle gebundenen Entities validieren, kopieren und ticken, ohne Ereignis- oder Dirty-Pfad
- **ID**: SCN-203
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EntityHost.cpp:228-264`, `src/HE_Scene/include/HorizonScene/EntityHost.h:110,151-155`
- **Problem**: Pro Frame: (a) alle Einträge von `m_byEntity` (`std::unordered_map`, knotenbasiert, cache-unfreundlich) durchlaufen und `registry().valid()` prüfen, nur um zerstörte Entities zu finden; (b) alle Instance-IDs in `m_tickScratch` kopieren (hier immerhin ohne Neuallokation); (c) `alive()` + `fireTick()` für jede Instanz, egal ob die Klasse überhaupt ein Tick-Ereignis besitzt. Der Kommentar begründet das Reaping mit "every path that removes an entity is covered": das geht auch ereignisgetrieben (`registry.on_destroy<>()` bzw. ein Entity-Destroy-Hook in `HorizonWorld::destroyEntity`), dann kostet ein Frame ohne Zerstörung nichts.
- **Code-Auszug**:
```cpp
for (const auto& [raw, inst] : m_byEntity)
    if (!m_world->registry().valid(static_cast<Entity>(raw)))
    { dead.push_back(inst); ... }
...
for (const auto& [raw, inst] : m_byEntity) m_tickScratch.push_back(inst);
for (const HorizonCode::InstanceId inst : m_tickScratch)
    if (m_runtime->alive(inst)) m_runtime->fireTick(inst, dt);
```
- **Lösung**: (1) Reaping über `on_destroy` einer ohnehin immer vorhandenen Komponente (`EntityIdComponent` ist bereits mit Signalen verdrahtet, siehe `HorizonWorld.cpp:30-32`) in eine kleine Todesliste schreiben; `tick` arbeitet nur diese Liste ab. (2) `m_byEntity` als flacher Vektor `{raw, inst}` halten (`instanceOf` mit sortiertem Vektor oder `flat_map`), ein einziger linearer Durchlauf für Tick. (3) Instanzen ohne Tick-Handler beim Binden markieren und nicht in die Tick-Liste aufnehmen (braucht Auskunft von `Runtime`, siehe Cross-Modul).
- **Erwarteter Effekt**: Schätzung: bei 1.000 Instanzen etwa 20 bis 60 µs pro Frame für Reaping/Kopie; der Tick selbst bleibt. Wächst linear mit Entity-Klassen-Instanzen (Projektile, NPCs).
- **Risiko / Verifikation**: Reihenfolge der Ticks und "Spawned während Tick tickt ab nächstem Frame" müssen erhalten bleiben. Tests zu EntityHost (Spawn/Destroy während Tick, Destruct-Ereignis nach Outliner-Löschung).

### SCN-204 – Lineare Suchen pro Skript-Aufruf: `findByName`, `mainCameraEntity`, `envOf`
- **ID**: SCN-204
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:346-353` (`findByName`), `:2378-2389` (`mainCameraEntity`, von allen 20 `camera::*`-Funktionen und `rigOf` genutzt), `:2587-2593` (`envOf`, von allen `env::get*/set*`)
- **Problem**: `entity.findByName` iteriert alle `NameComponent`s mit `std::string`-Vergleich pro Aufruf, O(n) pro Aufruf, und "Find By Name im Tick" ist das typische Skript-Muster. `mainCameraEntity` iteriert bei jedem Kamera-Getter/Setter (mehrere pro Frame bei einer Kamerasteuerung) die Kameras, `envOf` die `EnvironmentComponent`s. Beide Views sind klein, kosten aber pro Aufruf Pool-Lookup plus Iteration, und es gibt keinen Dirty-Flag oder Cache.
- **Code-Auszug**:
```cpp
auto view = c.world->registry().view<NameComponent>();
for (auto [e, nc] : view.each())
    if (nc.name == name) return (Entity)e;
```
- **Lösung**: `findByName`: Namensindex in `HorizonWorld` (`unordered_multimap<string, entity>`), gepflegt über `on_construct/on_update/on_destroy<NameComponent>` genau wie bereits der Id-Index (`HorizonWorld.cpp:30-32`); "erste Übereinstimmung in Pool-Reihenfolge" wird durch kleinste Pool-Position oder Entity-ID beim Mehrfachtreffer erhalten. Kamera/Environment: pro Frame im `Ctx` oder in `HorizonWorld` die Main-Camera- und Env-Entity cachen und per `on_construct/on_destroy` oder Versionszähler ungültig machen.
- **Erwarteter Effekt**: Schätzung: `findByName` von O(n) auf O(1); bei 10.000 benannten Entities ca. 50 bis 150 µs pro Aufruf gespart. Kamera/Env: klein, aber ohne Risiko.
- **Risiko / Verifikation**: Doppelte Namen (Auswahlregel) und `on_update` bei Umbenennung im Inspector (schreibt `nc.name` direkt, ohne Signal; dort müsste `registry.patch<NameComponent>` genutzt werden oder der Index verifiziert beim Treffer). Test: umbenennen, zerstören, doppelte Namen.

### SCN-205 – Foliage: `cachedInstances` hält eine dritte Kopie aller Matrizen, die nur gezählt wird
- **ID**: SCN-205
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/FoliageSystem.cpp:180-184,224-235`, `src/HE_Scene/include/HorizonScene/Components/FoliageComponent.h:40-45`
- **Problem**: Bei jeder Neuverteilung entstehen drei `mat4`-Arrays zu je N×64 Byte: `cachedInstances` (Welt), `local` (transient) und `store->local` (nach Buckets sortiert). Außerhalb der Szene-Tests wird `cachedInstances` nur für `.size()` gelesen (`TerrainTools.cpp:1596,1599`, `InspectorPanel.cpp:3517`, `EditorApplication.cpp:7563`); der Renderer liest ausdrücklich `store`. Bei 200.000 Instanzen (die Warnschwelle im Code) sind das 12,8 MB nur für Zählen. Zusätzlich `reserve(count)` auch bei starker Maske, wo nur ein Bruchteil überlebt.
- **Code-Auszug**:
```cpp
foliage.cachedInstances.reserve(static_cast<size_t>(count));
...
local.push_back(m);
...
m[3] = glm::vec4(worldPos, 1.0f);
foliage.cachedInstances.push_back(m);
```
- **Lösung**: `cachedInstances` durch `uint32_t placedCount` ersetzen und die Welt-Matrizen bei Bedarf aus `store->local` + Terrain-Origin ableiten (Tests, die die Welt-Matrizen vergleichen, `tests/test_foliage.cpp:78,146,232,244,260`, bekommen einen Hilfs-Accessor, der `store->local` zurückrechnet; Bit-Gleichheit ist gegeben, da laut Kommentar `m[3]` nur um `origin` verschoben wird). `local` direkt in das sortierte Ziel schreiben, indem zuerst nur Zellen-IDs plus Index gesammelt werden, oder `local` nach dem Sortieren sofort freigeben (`std::vector<glm::mat4>().swap(local)`), damit der Spitzenverbrauch nicht drei Kopien beträgt.
- **Erwarteter Effekt**: Schätzung: Spitzen-RAM der Neuverteilung ca. ein Drittel geringer, dauerhaft minus N×64 Byte pro Layer; weniger Cache-Druck beim Scatter.
- **Risiko / Verifikation**: Public-Feld der Komponente (Editor, Tests, evtl. MCP/Serializer: `grep` zeigt nur die genannten Stellen, aber Hot-Reload-Module könnten es lesen). Tests: `test_foliage` (Anzahl, Bit-Gleichheit Maske vs. uniform, Origin-Verschiebung).

### SCN-206 – Foliage-Malen: komplette Neuverteilung des Layers in jedem Frame mit gedrückter Maustaste
- **ID**: SCN-206
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/FoliagePaint.cpp:116` (`fol.dirty = true`), `src/HE_Scene/src/FoliageSystem.cpp:143-235`, Aufrufer `src/HE_Editor/TerrainTools.cpp:737-756`
- **Problem**: Jede Pinselbewegung setzt das gesamte Layer dirty; `FoliageSystem::update` verwirft Instanzen und Store und verteilt O(count) neu, inklusive `terrainHeightAt` (bei `seed != 0` ein fbm pro Kandidat), `FoliagePaint::sample` (bilinear) und Wang-Hash, danach Counting-Sort und Bucket-Bounds. Das passiert synchron im Update-Thread pro Frame, solange gemalt wird. Der Code warnt selbst ab 200.000 Instanzen vor "multi-second editor freeze". Die Maske ändert aber nur ein Rechteck (`x0..x1`, `z0..z1` sind in `paint` schon berechnet), und Kandidat i hängt nur von `seed` und `i` ab, nicht von seinen Nachbarn.
- **Code-Auszug**:
```cpp
if (!foliage.dirty) continue;
foliage.dirty = false;
foliage.cachedInstances.clear();
foliage.store.reset();
++foliage.revision;
```
- **Lösung**: Stufe 1 (einfach): Während eines Strichs das Neuverteilen drosseln (z. B. höchstens alle 100 ms oder erst bei Maustaste-los, Vorschau nur über die Debug-Anzeige). Stufe 2: Dirty-Rechteck in der Maske mitführen (`dirtyMinX..dirtyMaxZ`); der Scatter ist deterministisch pro Kandidat, es lassen sich also nur Kandidaten neu bewerten, deren (lx, lz) im Rechteck liegen, und der Bucket-Store inkrementell aktualisieren. Stufe 2 ist aufwendig, Stufe 1 reicht meistens.
- **Erwarteter Effekt**: Schätzung: bei 100.000 Instanzen mehrere ms pro Frame während des Malens, bei 500.000 mehrere hundert ms. Messen mit `Foliage` unter einem 1 km-Terrain bei Dichte 0,1/m².
- **Risiko / Verifikation**: Gedrosselte Variante verzögert nur die Anzeige, nicht den Dateninhalt (Maske ist sofort aktuell). Test: Undo-Snapshot pro Strich (`snapshotNow`), `test_foliage`-Maskenfälle bleiben unverändert.

### SCN-207 – Spawn-Pfad: Wegwerf-`HorizonWorld`, doppeltes `resolveClassAsset`, Blob-Kopie pro Spawn
- **ID**: SCN-207
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EntityHost.cpp:129-156` (spawn), `:88-117` (bind), `:310-335` (`inheritedComponents`), `:337-412` (`defaultComponents`)
- **Problem**: Pro `spawn` einer Klasse: `loadAsset(classPath)` (String-Hash), `inheritedComponents` kopiert den ganzen `componentBlob` als `std::vector<uint8_t>` (`return asset.componentBlob;`), bei Klassen ohne eigenen Blob folgt `resolveClassAsset` und für jeden Vorfahren ein `loadAsset(ancestor)`; dann `bind()` macht `loadAsset(classPath)` nochmals und ruft `resolveClassAsset` erneut auf. Gibt es keinen vererbten Blob, baut `defaultComponents` jedes Mal eine komplette `HorizonWorld` (Konstruktor legt Entity, Komponenten, `reserveComponentStorage()` mit 12 Pools und das Widget-/Script-Subsystem an, `HorizonWorld.cpp:21-51`), fügt Komponenten hinzu, serialisiert und verwirft alles. Bei Projektilen, die pro Schuss gespawnt werden, ist das eine Spawn-Hitch-Quelle.
- **Code-Auszug**:
```cpp
HorizonWorld scratch;
const Entity root = scratch.createEntity(baseClass.empty() ? "Entity" : baseClass);
...
SceneSerializer ser;
return ser.serializeSubtree(scratch, root);
```
- **Lösung**: (1) `defaultComponents` ergebnisweise cachen, Schlüssel `baseClass` (Ergebnis hängt nur von `baseClass` ab, ist also reine Funktion; `static std::unordered_map<std::string, std::vector<uint8_t>>`, beim Hot-Reload/Projektwechsel leeren). (2) `inheritedComponents` als `const std::vector<uint8_t>&`/`std::span` auf den Blob zurückgeben, solange der Pool nicht umzieht (der Kommentar warnt vor Pointer-Invalidierung: dann pro Klasse einen `shared_ptr<const vector>` im Aufrufer halten, der Blob wird sowieso nur an `instantiatePrefab` gereicht). (3) `ResolvedClass` einmal pro Spawn lösen und an `bind` durchreichen (`bind(entity, classPath, const ResolvedClass&)`).
- **Erwarteter Effekt**: Schätzung: das Cachen von `defaultComponents` entfernt je Spawn eine komplette Welt-Konstruktion samt Serialisierung (vermutlich dreistellige µs bis ms); zweiter Resolve spart Pool-Lookups und Vektorkopien.
- **Risiko / Verifikation**: Cache-Invalidierung bei Änderung der Engine-Klassentabelle (`engineClassIsA`) ist statisch; sonst keine. Tests: `test_entity_host`/Create-Object-Tests, Spawn von Player-Character-Klasse ohne eigenen Blob.

### SCN-208 – `fs.watch`: bis zu 32 Verzeichnisscans mit `stat` pro Eintrag auf dem Update-Thread
- **ID**: SCN-208
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:3022-3042` (`stampOf`), `:3081-3102` (`readChildren`), `:3151-3214` (`pollWatches`); Grenzen `include/HorizonScene/EngineApi.h:1750-1756` (`kWatchIntervalSeconds = 1.0`, `kMaxWatches = 32`, `kMaxWatchEntries = 4096`)
- **Problem**: `pollWatches(dt)` wird aus dem Frame aufgerufen. Einmal pro Sekunde läuft pro Watch ein `directory_iterator` mit je einem `std::filesystem::status` + `file_size` + `last_write_time` pro Eintrag, also bis zu 3 Syscalls × 4096 × 32 = rund 390.000 Syscalls in einem Frame, danach wird eine `std::map<std::string, WatchStamp>` mit Heap-Knoten pro Eintrag neu aufgebaut. Der Frame, in dem das Intervall fällig wird, hitcht (blockierende I/O im Update-Thread). Die Queue-Helfer sind ebenfalls linear (`queueChange` dedupliziert O(n) gegen bis zu 256, `takeChange` ruft `erase(begin())`).
- **Code-Auszug**:
```cpp
for (WatchEntry& w : watches())
{
    const WatchStamp now = stampOf(w.path);
    ...
    std::map<std::string, WatchStamp> fresh;
    if (!readChildren(w.path, fresh)) { ... }
```
- **Lösung**: Das Scannen auf einen Hintergrund-Thread (ein bestehender `JobSystem`-Worker oder der HTTP-Worker-Stil) verlegen, der nur Änderungsereignisse in `changeQueue()` (hinter dem Mutex) einstellt; oder native Watcher (`inotify`/`ReadDirectoryChangesW`/FSEvents) pro Plattform. Billig: die Watches reihum verteilen (ein Watch pro Frame statt alle auf einmal), `directory_entry::status()/file_size()` aus dem Iterator nutzen (spart auf manchen Plattformen den zweiten Syscall) und `changeQueue` als `std::deque<std::string>` mit Set für das Dedup.
- **Erwarteter Effekt**: Schätzung: Hitch von möglicherweise zig ms (kaltes Dateisystem, Netzlaufwerk: Sekunden) pro Sekunde entfällt; nur relevant, wenn ein Projekt `fs.watch` benutzt (Standardfall: keine Watches, dann ist `pollWatches` ein früher Rücksprung).
- **Risiko / Verifikation**: Semantik "ein Ereignis pro Pfad und Drain", "ein Verzeichnis-Zeitstempel zählt nicht" und der Degradationspfad `tooBig` müssen gleich bleiben. Die vorhandenen fs.watch-Tests mit manipulierten mtimes (falls vorhanden; sonst neu schreiben) absichern.

### SCN-209 – `prefs.set*` schreibt bei jedem Aufruf die gesamte Datei synchron
- **ID**: SCN-209
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:4747-4753` (`store`), `:4771-4774,4776-4783`
- **Problem**: Jeder `setString/setNumber/setBool/remove/clear` ruft `store()`: `doc().dump(2)` (Pretty-Print), `create_directories`, `ofstream` öffnen/truncaten/schließen. Ein Einstellungsdialog, der beim Ziehen eines Sliders pro Frame `prefs.setNumber` ruft (sehr naheliegend), macht 60 Dateischreibzugriffe pro Sekunde auf dem Game-Thread. Der Kommentar nennt die Begründung (Absturzsicherheit), die ein Write-Behind mit Flush am Frame-Ende und beim Beenden ebenso erfüllt. Außerdem schreibt `fs::writeText` nicht atomar (truncate vor dem Schreiben), im Gegensatz zu `save::write`, das temp+rename nutzt: ein Absturz mitten im Schreiben zerstört die Prefs.
- **Code-Auszug**:
```cpp
void setString(Ctx&, const std::string& key, const std::string& value)
{ doc()[key] = value; store(); }
```
- **Lösung**: `store()` setzt nur `s_dirty = true`; ein `flush()` läuft entweder einmal pro Sekunde über die Frame-Schleife (ähnlich `pollWatches`) oder bei Shutdown/Fokusverlust, und schreibt atomar (temp + rename wie in `save::write`). Wert nur schreiben, wenn er sich wirklich ändert (`doc()[key] == value` → kein `dirty`).
- **Erwarteter Effekt**: Schätzung: pro Aufruf ein Datei-Open/Write/Close weniger (typisch 50 bis 500 µs, auf Netz-/Cloud-Sync-Ordnern ms); bei Slider-Ziehen 60× pro Sekunde.
- **Risiko / Verifikation**: Daten gehen bei hartem Absturz zwischen Flushes verloren (Zeitfenster ≤ 1 s), das ist eine Verhaltensänderung und gehört dem Projekt zur Entscheidung. Alternative ohne Verhaltensänderung: nur das "kein Update bei gleichem Wert" und atomares Schreiben. Tests: `prefs`-Roundtrip, Shutdown-Flush.

### SCN-210 – Outliner: `subtreeVisibility` (rekursiv, alle 7 Komponenten) und `isEntityActive` (Eltern-Kette) pro sichtbarer Zeile und Frame
- **ID**: SCN-210
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EntityVisibility.cpp:28-64`, `src/HE_Scene/src/EntityActive.cpp:13-27`; Aufrufer `src/HE_Editor/OutlinerPanel.cpp:1232,1562`
- **Problem**: Der Outliner zeichnet nur sichtbare Zeilen (Clipping), ruft aber pro Zeile und Frame `subtreeVisibility`, das den ganzen Teilbaum der Zeile durchläuft, solange keine sichtbare Komponente gefunden wird. Eine zugeklappte Gruppe mit vielen Kindern ohne Renderable (Trigger, Collider, leere Ordner) oder mit ausgeblendetem Inhalt kostet je Frame O(Teilbaum) × 7 `try_get`. `isEntityActive` läuft pro Zeile die Eltern-Kette hoch (bis 256 Hops, je zwei Lookups). Zusätzlich hat `setSubtreeVisible/subtreeVisibility` keine Tiefenbegrenzung, im Gegensatz zu `isEntityActive` (der Kommentar dort setzt den Baum als zyklenfrei voraus; `reparentEntity` verhindert Zyklen, aber eine korrupte Szene würde hier den Stack sprengen).
- **Code-Auszug**:
```cpp
for (entt::entity child : h->children)
{
    const Visibility cv = subtreeVisibility(reg, child);
    if (cv == Visibility::Visible) return cv;
    if (cv == Visibility::Hidden)  v = Visibility::Hidden;
}
```
- **Lösung**: Im Outliner den Teilbaum-Status nur neu berechnen, wenn sich etwas geändert hat (Versionszähler, den `setEntityVisible`/Komponenten-Änderungen bumpen, oder einmal pro Frame ein Bottom-up-Durchlauf über die sichtbaren Wurzelzeilen mit Ergebnis-Cache `unordered_map<entity, Visibility>`, der am Frame-Ende geleert wird). Für die Aktiv-Kette: Eltern-Status beim Zeilenabstieg mitreichen statt pro Zeile neu hochzulaufen (der Outliner geht ohnehin von oben nach unten). Iterativ mit explizitem Stack statt Rekursion.
- **Erwarteter Effekt**: Schätzung: relevant nur bei großen, zugeklappten Teilbäumen; dort von O(n) pro Frame auf O(1) pro Zeile. Messen mit einer Szene mit 20.000 Entities unter wenigen Gruppen.
- **Risiko / Verifikation**: Zeilenhöhe bleibt konstant (Projektregel). UI-Test `test_outliner_ui` mit Hidden/Visible-Mischung; Cache nur frame-lokal, damit keine Staleness.

### SCN-211 – `water.*`-Abfragen bauen pro Aufruf einen frischen `WaterIndex`
- **ID**: SCN-211
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:1574-1580` (`surfaceNow`), Nutzer `:1583-1601,1622-1640`; `src/HE_Scene/src/WaterQuery.cpp:33-82`; `PhysicsWorld.cpp:703` hält bereits einen eigenen, pro Physikschritt aktualisierten Index
- **Problem**: `water.surfaceAt/depthAt/flowAt/disturb` bauen jedes Mal `WaterIndex::build(world)`: `view<TerrainComponent>`, pro Landschaft mit Wasser `worldMatrixOf` (Hierarchie-Walk), `glm::inverse`, 3 Spaltenlängen, danach `std::vector<Entry>` (Heap) und `std::sort`. Viele schwimmende Objekte, die pro Frame ihre Wasserhöhe per Skript abfragen, zahlen das je Objekt. Das Grid wird nicht neu gescannt (`ensureBounds` nur in `regions()`), die Kosten sind also moderat.
- **Code-Auszug**:
```cpp
const HE::water::WaterIndex index = HE::water::WaterIndex::build(*c.world);
return index.surfaceAt(x, z, c.physics ? c.physics->simTime() : 0.0, out);
```
- **Lösung**: Den Index des `PhysicsWorld` (oder einen im `Ctx` gehaltenen, einmal pro Frame via `refresh` aufgefrischten) wiederverwenden; der Kommentar im Header sagt selbst "Call it once per physics step, and before a batch of queries". Nur Ersatz, wenn `c.physics` fehlt (Editor-Vorschau).
- **Erwarteter Effekt**: Schätzung: eine Allokation, eine Sortierung und eine 4×4-Inversion pro Landschaft und Aufruf weniger; nur messbar bei hunderten Abfragen pro Frame.
- **Risiko / Verifikation**: Der Kommentar nennt als Grund "cannot be left holding the component pointers of a landscape that a script has just deleted"; ein pro Frame aufgefrischter Index hat dasselbe Problem innerhalb des Frames. Eine Version/`signal`-Prüfung auf Terrain-Pools (on_destroy) oder `refresh` bei Strukturänderung nötig. Test: Landschaft im selben Frame löschen und abfragen.

### SCN-212 – `json.*`-Getter parsen den gesamten Text bei jedem Aufruf
- **ID**: SCN-212
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:4675-4713` (`getString/getNumber/getBool/has/count`), `:4655-4672` (`setAt`)
- **Problem**: Wer 6 Felder aus derselben JSON-Antwort (z. B. HTTP-Body) liest, parst sie 6×; jeder `setX` parst, ändert und serialisiert komplett. Jeder Aufruf kopiert zusätzlich den Text (`aS()` in der Registry, SCN-201).
- **Code-Auszug**:
```cpp
const nlohmann::json doc = parse(text);
if (doc.is_discarded()) return fallback;
const nlohmann::json* v = walk(doc, path);
```
- **Lösung**: Ein-Eintrags-Cache des zuletzt geparsten Textes (Länge + Hash/Inhalt vergleichen, 64-Bit-FNV über `text` kostet weniger als ein Parse) im Namespace; `thread_local`, falls Skripte parallel laufen. `walk` baut außerdem pro Schritt `std::string key`/`num` per `substr`; mit `std::string_view` entfällt das.
- **Erwarteter Effekt**: Schätzung: Faktor 3 bis 10 bei mehrfachem Zugriff auf dasselbe große JSON; sonst nichts.
- **Risiko / Verifikation**: Rein intern; Test: abwechselnd zwei Texte, leerer Text, ungültiger Text.

### SCN-213 – Input-Snapshot: pro Frame Namensvektor und `unordered_set<std::string>`, Action-Suche linear mit Stringvergleich
- **ID**: SCN-213
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:5740-5757` (`Snapshot::keys`), `:5762-5778` (`pushSdlSnapshot`), `:5909-5922` (`findAction`)
- **Problem**: `pushSdlSnapshot` läuft jeden Frame über alle Scancodes (`n` ca. 512), ruft für jede gedrückte Taste `SDL_GetScancodeName` und legt `std::string` in einen `std::vector<std::string>` ab, `setKeysDown` leert dann das `unordered_set<std::string>` (Knoten freigeben) und fügt neu ein (Knoten allokieren). `keyDown(name)` hasht den Namen pro Aufruf; `actionDown/Pressed/...` suchen linear mit `a.name == name` über alle Actions. Alles im Normalfall ein paar gedrückte Tasten und ein paar Dutzend Actions, also klein, aber es ist ein Pro-Frame-Allokationsmuster ohne Notwendigkeit.
- **Code-Auszug**:
```cpp
for (int sc = 0; sc < n; ++sc)
    if (ks[sc]) { const char* name = SDL_GetScancodeName((SDL_Scancode)sc); if (name && name[0]) down.emplace_back(name); }
...
s.keys.clear(); for (const auto& n : names) s.keys.insert(n);
```
- **Lösung**: Gedrückte Tasten als `std::bitset<SDL_SCANCODE_COUNT>`/`std::array<bool>` halten; `keyDown(name)` löst den Namen über `SDL_GetScancodeFromName` (oder eine statische Name→Scancode-Map, einmal gebaut) in den Index auf. Die Action-Liste als `unordered_map<string, ActionState>` oder sortierter Vektor.
- **Erwarteter Effekt**: Schätzung: 0 Allokationen pro Frame statt ca. 2 bis 8 bei gedrückten Tasten; Nutzen klein, Code wird nicht komplizierter.
- **Risiko / Verifikation**: Namensauflösung muss groß-/kleinschreibungs- und Aliasverhalten der bisherigen Strings (SDL-Namen wie "Left Shift") exakt erhalten. Test: `input.keyDown` mit allen vom Editor geschriebenen Namen.

### SCN-214 – Save-Pfad: JSON → String → JSON, tiefe Kopie der Entity-Blobs und synchrones Pretty-Print
- **ID**: SCN-214
- **Kategorie**: C / E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:539-563` (`saveState`: `j.dump()`), `:5310-5318` (`setEntityState`: erneutes `parse`), `:5319-5324` (`entityState`: `dump()`), `:571-584` (`applySavedState`: erneutes `parse`), `:5147-5157` (`write`: `j["entities"] = active()->entities`, `dump(2)`)
- **Problem**: Jede Entity wird beim Speichern als `json` gebaut, zu String serialisiert, wieder geparst und gespeichert; beim Laden umgekehrt (`dump` → `parse`). `write()` kopiert das gesamte `entities`-Objekt tief, nur um es in ein neues Wurzelobjekt zu hängen, und schreibt es dann pretty-printed auf dem Game-Thread. Bei 1.000 gespeicherten Entities sind das tausende Knoten-Allokationen und ein Frame-Hitch beim Autosave.
- **Code-Auszug**:
```cpp
return save::setEntityState(uuid, j.dump());          // 562
nlohmann::json j = nlohmann::json::parse(json, ...);  // 5314
j["entities"] = active()->entities;                   // 5157
```
- **Lösung**: Intern eine zweite Schnittstelle `setEntityState(uuid, nlohmann::json&&)` / `const nlohmann::json* entityStateJson(uuid)` ergänzen, die C-ABI-Variante (String) bleibt als dünne Hülle. `write()`: `entities` per `std::swap` in das Ausgabeobjekt heben und nach `dump` zurücksetzen, oder die Teile getrennt serialisieren. `dump(2)` → `dump()` für Spielstände (Größe, Zeit) oder Pretty-Print optional.
- **Erwarteter Effekt**: Schätzung: etwa halbierte Speicher-/Ladezeit der Entity-Zustände, kein zusätzliches Vollkopie-Spitzen-RAM.
- **Risiko / Verifikation**: Dateiformat und Schlüsselreihenfolge (nlohmann sortiert Objekte alphabetisch) müssen byte-identisch bleiben, wenn die Doku "save → load → save byte-identical" verspricht (Kommentar `:200-203`). Test: Roundtrip save→load→save vergleichen.

### SCN-215 – Globaler Prozess-Zustand in Funktions-Statics, ungleich synchronisiert
- **ID**: SCN-215
- **Kategorie**: F / E
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:2747` (Debug-Queue), `:3054-3057` (Watches), `:4475-4477` (Timer), `:4845` (aktiver Spielstand), `:5335-5338` (Szenen-Requests/Zonen), `:5695` (Player-Tabelle), `:5750` (Input-Snapshot); gesichert nur `http` (`g_mtx`)
- **Problem**: Mehr als zehn unabhängige Singletons ohne Besitzer (Welt/Session) und ohne Sperre, während der Kommentar bei `datetime::localParts` (`:4789`) von "script threads" im Plural spricht, und der HTTP-Teil ausdrücklich Mutex und Worker hat. Zusätzlich bleibt Zustand zwischen Editor-Play-Sessions erhalten (nur `clearWatches`, `cancelAll`, `clear` je Gruppe, ein einzelner Reset-Punkt fehlt). Das ist vor allem ein Wartbarkeits-/Korrektheitsrisiko; Performance-Auswirkung gering.
- **Code-Auszug**:
```cpp
std::vector<Timed>& queue() { static std::vector<Timed> q; return q; }
std::vector<WatchEntry>&  watches()     { static std::vector<WatchEntry> w;  return w; }
```
- **Lösung**: Eine `struct ScriptApiState` bündeln (Timer, Watches, Debug-Queue, Input, Player, Szenen-Requests, Save), vom Host pro Session erzeugt und über `Ctx` erreichbar, mit einer `reset()`-Methode; die freien Funktionen ohne `Ctx` (`timer::after`, `input::keyDown`, ...) holen sie über einen einzelnen `thread_local`/globalen Pointer. Das ist ein größerer Umbau und nur sinnvoll, falls mehrere Welten oder Threads vorkommen; ansonsten dokumentieren "nur Main-Thread".
- **Erwarteter Effekt**: Keiner in der Performance; Reduktion von Zustandslecks zwischen Sessions und Race-Risiko.
- **Risiko / Verifikation**: Hoch für den vollständigen Umbau (öffentliche API im Header, C-ABI-Tabellen), daher nur nach Entscheidung. Unsicher, ob wirklich mehrere Threads Skripte ausführen (siehe Messung/Klärung).

### SCN-216 – Kleinigkeiten in Pro-Frame-Systemen: FlyCamera, EnvironmentPush, FloatingOrigin
- **ID**: SCN-216
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/FlyCameraController.cpp:92`, `:15-19` (findCamera); `src/HE_Scene/src/EnvironmentPush.cpp:8-57`; `src/HE_Scene/src/FloatingOrigin.cpp:44-55`
- **Problem**: (a) `FlyCameraController::update` setzt `t.dirty = true` jeden Frame, auch ohne Maus- oder Tastenbewegung, und `findCamera` iteriert pro Frame alle Kameras; ein dauerhaft schmutziger Transform hält die Kamera in jedem Frame im Propagations-Pfad. (b) `makeEnvironmentSettings` kopiert eine Struktur mit ca. 70 Feldern pro Frame feldweise, und ändert dabei die Komponente (`env.timeOfDay`, `moonPhase`): ein "make"-Name mit Nebenwirkung; die Felderliste existiert parallel in `EnvironmentComponent`, in `IRenderer::EnvironmentSettings` und in den `HE_ENV_FIELDS_*`-X-Listen des API, jede neue Einstellung muss vier Stellen treffen. (c) `shiftWorldOrigin` subtrahiert den Shift von jedem `worldMatrix` (`:46-47`) und ruft direkt danach `invalidateWorldMatrices`, dessen Kommentar sagt, dass der nächste Durchlauf ohnehin alles neu berechnet; die handgeschriebene Subtraktion ist nur für Leser zwischen Shift und Propagation sinnvoll. Das Ereignis ist selten (einmal pro Kilometer).
- **Code-Auszug**:
```cpp
t.dirty = true;               // FlyCameraController.cpp:92
f.driven = true;
```
- **Lösung**: (a) `t.dirty |= (f.dx != 0 || f.dy != 0 || glm::dot(move, move) > 0.0f);`. (b) Aufteilen in `advanceDayNight(env, dt)` (mutiert) und `toSettings(const EnvironmentComponent&)`; mittelfristig dieselbe X-Liste (`HE_ENV_FIELDS_*`) auch für das `EnvironmentSettings`-Mapping verwenden. (c) Nichts tun oder Schleife streichen, falls kein Leser zwischen Shift und `propagateTransforms` existiert (zu prüfen).
- **Erwarteter Effekt**: Gering; (a) spart bei ruhender Kamera die Propagation und eventuelle Folgearbeit an `dirty`.
- **Risiko / Verifikation**: (a) Der Flug-Controller schreibt `rotation` auch bei dt>0 ohne Eingabe nicht neu; die Änderung ist verhaltensgleich. Tests für den Fly-Controller (Pitch-Clamp, Sprint) und Floating-Origin-Test (`worldMatrix` nach Shift) beibehalten.

### SCN-217 – Foliage: Einstellungs-Hash pro Frame und Layer, `coverage()` scannt die ganze Maske pro UI-Frame
- **ID**: SCN-217
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/FoliageSystem.cpp:36-56,130-136`; `src/HE_Scene/src/FoliagePaint.cpp:147-153`; Aufrufer `src/HE_Editor/TerrainTools.cpp:1599`, `InspectorPanel.cpp:3526`
- **Problem**: (a) `FoliageSystem::update` hasht pro Frame und Layer bytewise (FNV-1a, ca. 70 Byte) alle Darstellungs-Einstellungen, um `revision` hochzuzählen, weil Felder direkt (Inspector) beschrieben werden und kein Dirty-Flag existiert. Das ist robust und billig (wenige Layer), aber O(Layer) pro Frame ohne Änderung. (b) `FoliagePaint::coverage` summiert die komplette Maske (bis 4096² = 16,7 MB) bei jedem UI-Zeichnen, solange das Foliage-Panel offen ist; bei Standard 128² trivial. (c) `sortIntoBuckets` reserviert `store->buckets` nicht (`push_back` pro nicht-leerer Zelle; Obergrenze `min(cellCount, n)` bekannt).
- **Code-Auszug**:
```cpp
const uint64_t key = settingsKeyOf(foliage);
if (key != foliage.settingsKey) { foliage.settingsKey = key; ++foliage.revision; }
```
- **Lösung**: (a) Lassen, solange Layer-Anzahl klein; sonst Setter/`registry.patch` im Inspector. (b) Coverage in `FoliageComponent` cachen und zusammen mit Maskenänderungen (nur in `paint/fill/clear/ensure`, alles in einer Datei) aktualisieren, oder im Panel alle 0,25 s neu rechnen. (c) `store->buckets.reserve(std::min(cellCount, n))`.
- **Erwarteter Effekt**: (a)/(c) vernachlässigbar; (b) bei 4096² ca. 5 bis 10 ms pro Panel-Frame (Schätzung).
- **Risiko / Verifikation**: Gering; (b) braucht einen Test, dass `coverage` nach `paint`/`fillMask`/`clearMask` stimmt.

### SCN-218 – `EngineApi.cpp` ist ein 9.210-Zeilen-Monolith mit schweren Includes
- **ID**: SCN-218
- **Kategorie**: F / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/EngineApi.cpp:1-97` (Includes), gesamte Datei
- **Problem**: Eine Übersetzungseinheit enthält 40+ Gruppen (Physik, Wasser, Audio, Netz, HTTP, SQLite, PDF-Druck, Dialoge, Save, JSON, Zeit ...) und zieht `sqlite3.h`, `SDL3`, `nlohmann/json.hpp`, `Net/HttpsClient.h`, `AntiCheatHost.h`, `NetGameSession.h` und ca. 30 Komponenten-Header ein. Jede Änderung an einer Gruppe baut die ganze Datei neu; der Registry-Block (`:6021-8580`) ist 2.560 Zeilen `t.push_back({...})` plus eine zweite, parallele Tabelle `kNames` (ca. 480 Einträge, Kommentare bis `:8444`), die per doppelter Schleife (`strcmp` jeder Zeile gegen jeden Eintrag, ca. 800 × 480 einmalig beim Start) zusammengeführt wird. Namen und Zeile sind getrennt und können driften (der Fallback `fn.displayName = fn.id` verdeckt vergessene Namen).
- **Code-Auszug**:
```cpp
for (auto& fn : t)
{
    fn.displayName = fn.id; // fallback: never null, worst case the id shows
    for (const auto& [id, name] : kNames)
        if (std::strcmp(fn.id, id) == 0) { fn.displayName = name; break; }
}
```
- **Lösung**: Pro Gruppe eine Datei (`EngineApi_Physics.cpp`, `_Net.cpp`, `_Db.cpp`, `_Save.cpp`, ...), jede mit ihrer eigenen `registerXxx(std::vector<ApiFn>&)`-Funktion, in der die Zeilen auch ihren Anzeigenamen als Feld tragen (Aggregat-Initialisierung `{ id, "Get Name", ... }`); die Post-Pässe (Self-Default) bleiben zentral. Das ändert kein Laufzeitverhalten und halbiert Inkrementbuild-Zeiten bei Änderungen an einer Gruppe.
- **Erwarteter Effekt**: Build-Zeit; Lesbarkeit; keine Laufzeit.
- **Risiko / Verifikation**: Reihenfolge der Registry-Zeilen (`find` nimmt die erste bei doppelter ID, Kommentar `:8779`, und Editor-Paletten zeigen Registry-Reihenfolge) muss erhalten bleiben; `test_engine_api` (Eindeutigkeit der IDs, `cppCall`-Invariante) deckt das ab.

---

## Cross-Modul-Hinweise

- `src/HE_Core/include/HorizonCode/HorizonCode.h:93-124`: Die Struktur `Value` ist die Ursache von SCN-201 und gilt für das gesamte Skript-Subsystem (Interpreter, Codegen, Variablen, Events). Eine Verkleinerung gehört dem Core-Slice, die Messung dort (`Runtime::fireTick`, `callFunction`) sollte `Value`-Kopien zählen.
- `HorizonCode::Runtime::fireTick/alive` (aufgerufen aus `EntityHost::tick`): Ob für Klassen ohne Tick-Ereignis ein früher Rücksprung existiert, ist im Core-Slice zu prüfen; falls nicht, wäre ein `hasTickHandler(inst)`-Flag die größere Ersparnis zu SCN-203.
- `src/HE_Editor/TerrainTools.cpp:737-756` (SCN-206), `:1596-1599` und `InspectorPanel.cpp:3517,3526` (SCN-205, SCN-217): Editor-Aufrufer, die `cachedInstances`/`coverage` pro Frame lesen.
- `src/HE_Editor/OutlinerPanel.cpp:1232,1562` (SCN-210): Zeilenweise Aufrufe von `isEntityActive`/`subtreeVisibility`.
- `src/HE_Game/src/GameApplication.cpp:3138-3140` und `src/HE_Editor/EditorApplication.cpp:3138`: Rufen `pushSdlSnapshot` jeden Frame (SCN-213).
- `src/HE_Scene/src/PhysicsWorld.cpp:703`: Der eigene `WaterIndex` kann für SCN-211 mitgenutzt werden.
- `HE::api::find(const std::string&)` (`EngineApi.cpp:8773`) nimmt `const std::string&`; Aufrufer mit `const char*` (z. B. `UIEditorPanel.cpp:6753`) allozieren bei Namen über 15 Zeichen. Eine `std::string_view`-Überladung wäre kostenfrei (der Index ist ohnehin `string_view`-gekeyt).

## Unsicher / Messung nötig

- Größenordnung von SCN-201/202: Mikro-Benchmark (1.000 bis 10.000 Aufrufe `transform.getPosition` aus HorizonCode-Tick, Lua, Python) vor und nach der `Value`-Verkleinerung.
- SCN-203: Anzahl gebundener Entity-Instanzen in realen Projekten (Zielgröße); Profil von `EntityHost::tick` bei 1.000 Projektilen.
- SCN-204: Verbreitung von `entity.findByName` in Tick-Graphen; Szenengrößen (Anzahl `NameComponent`).
- SCN-206: Dauer einer Neuverteilung bei 50k/200k/500k Instanzen mit und ohne `seed != 0` (fbm) im Editor.
- SCN-208: Ob gängige Projekte `fs.watch` überhaupt nutzen; Kosten eines Scans von 4.096 Einträgen auf Windows (NTFS, `status` je Eintrag) und Netzlaufwerk.
- SCN-210: Verhalten bei Szenen mit sehr großen zugeklappten Gruppen ohne Renderables.
- SCN-215: Ob Skripte (Lua/Python) je außerhalb des Main-Threads laufen; der Kommentar bei `datetime::localParts` behauptet es, die Statics sprechen dagegen.
- `entity::distance` (`EngineApi.cpp:341-345`) benutzt `ScriptApi::getPosition`, laut `transform`-Gruppe (`:634,7075-7092`) die LOKALE Position; für parentete Entities wäre die Distanz damit nicht die Welt-Distanz. Das ist keine Audit-Kategorie, aber verdächtig; `ScriptApi::getPosition` (außerhalb dieses Slices) prüfen.
- `EntityHost::bind` (`EntityHost.cpp:117`): Schlägt `addCompiled/addLevels` fehl (`inst == 0`), kehrt die Funktion ohne Log mit 0 zurück, `spawn` zerstört die Entity dann stillschweigend; nur gelesen, nicht getestet.

## Gelesen

Ja, alle zehn Dateien des Slice vollständig gelesen: `EngineApi.cpp` (Zeilen 1 bis 9210 in Blöcken), `EntityActive.cpp`, `EntityHost.cpp`, `EntityVisibility.cpp`, `EnvironmentPush.cpp`, `FloatingOrigin.cpp`, `FlyCameraController.cpp`, `FoliagePaint.cpp`, `FoliageSystem.cpp`, `GameInstanceHost.cpp`; zusätzlich gezielt (nur per Suche und kurzen Ausschnitten) `HorizonCode.h` (`Value`), `WaterQuery.cpp`, `EntityHost.h`, `FoliageComponent.h`, `HorizonWorld.cpp`, `OutlinerPanel.cpp` und `TerrainTools.cpp` zur Verifikation der Aufrufer.
