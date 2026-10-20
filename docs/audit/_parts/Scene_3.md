# Scene Slice 3 – GameReplication, HcCodegen, HiddenShell, Animation-/Audio-/Camera-/Cell-/Collision-/AntiCheat-Header, Components/*

## Zusammenfassung

Der Slice besteht zu etwa 55 % aus `HcCodegen.cpp` (Build-/Export-Zeit-Generator plus Toolchain-Probe, kein Frame-Pfad), zu 10 % aus `GameReplication.cpp` (Netzwerk-Tick) und sonst aus Headern und reinen Daten-Komponenten. Die Komponenten sind überwiegend schlank und nutzen bereits Dirty-Flags oder Revisionszähler (`MeshComponent::dirty`, `MaterialComponent::dirty`, `FoliageComponent::revision`, `AnimatorStateMachineComponent::configDirty`). Die relevanten Funde liegen deshalb in drei Bereichen:
- Per-Frame-Pfade ohne Änderungserkennung: Client-Interpolation, Server-Snapshots, `AudioSystem::updateSpatial`.
- Pro-Entity-Heap für Daten, die aus einem geteilten Asset abgeleitet sind (State-Machine-Graph, Blend-Spaces, Masken, Partikel-Config, 2,5 KB Zufallsgenerator).
- Die vom Codegen erzeugte Laufzeit (String-Dispatch, `std::vector<hc::Value>` pro Engine-Call) und eine Datenrasse in der cmake-Auflösung.

Funde: 2 Hoch, 9 Mittel, 7 Niedrig (18 gesamt). Alle Zeilennummern wurden gegen den aktuellen Stand der Dateien geprüft. Es wurde nichts gemessen. Alle Effekte sind Schätzungen.

---

### SCN-301 – Client-Interpolation schreibt jeden Frame jede Entity und setzt `dirty`, auch im Stillstand
- **ID**: SCN-301
- **Kategorie**: D (ECS ohne Dirty-Flag) / C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/GameReplication.cpp:932-981` (Schleife), `:954-955` (Position), `:979` (`dirty`)
- **Problem**: `advanceInterpolation` läuft pro Frame über alle `m_interp`-Einträge, also über alle replizierten Fremd-Entities. Pro Eintrag kostet das mindestens vier Lookups: `m_byNetId.find`, `reg.valid`, `try_get<TransformComponent>` und in `wireOrigin` ein weiteres `try_get<HierarchyComponent>` samt `rootEntity()`-Vergleich. Danach wird Position und Rotation neu berechnet und `tc->dirty = true` gesetzt, auch wenn `t == 1` ist und `previous == current` gilt. Das trifft statische Props und alles, was zwischen zwei Snapshots steht. Jede dieser Entities kostet damit in `propagateTransforms` (siehe `HierarchyComponent.h:6-9`: "skipping the subtrees that did not change") und in der Render-Extraktion jeden Frame Arbeit, obwohl sich nichts bewegt hat. Der Dirty-Mechanismus wird so für den gesamten Client unterlaufen. Zusätzlich werden `m_interp`-Einträge nur in `unregisterEntity` (`:273-285`) gelöscht. Wird eine Entity ohne Unregister zerstört, bleibt ein toter Eintrag, den die Schleife dauerhaft per `continue` überspringt.
- **Code-Auszug**:
```cpp
for (auto& [netId, st] : m_interp) {
    ...
    st.elapsed += dt;
    const float t = st.hasPrevious ? std::min(1.0f, st.elapsed / span) : 1.0f;
    tc->position = fromWire(glm::mix(st.previous.position, st.current.position, t),
                            wireOrigin(m_world, it->second));
    ...
    tc->dirty    = true;
}
```
- **Lösung**:
  1. Pro Eintrag ein `settled`-Flag führen. Es wird gesetzt, sobald `t >= 1` und die Pose einmal geschrieben wurde. Es wird gelöscht, wenn `applySnapshot` den Puffer verschiebt oder `applyBaseline` ihn setzt.
  2. Bei `settled` den Eintrag komplett überspringen. Dann entfällt `dirty`, und die vier Lookups gelten nur noch für bewegte Entities.
  3. Den Ursprungs-Offset (`wireOrigin`) einmal pro Frame bzw. pro Floating-Origin-Wechsel cachen (`world->origin()` ist pro Frame konstant). Die `const_cast`-Konstruktion in `:29` entfällt, wenn `wireOrigin` eine nicht-konstante Welt oder eine bereits geprüfte Registry bekommt.
  4. Zerstörte Entities (`!reg.valid`) aus `m_interp` entfernen (Schleife mit Iterator-erase).
  5. Alternativ `HierarchyComponent`/`EntityHandle` im `InterpState` ablegen, damit `m_byNetId.find` pro Frame wegfällt (Handle-Gültigkeit weiter über `reg.valid` prüfen).
  ```cpp
  if (st.settled) continue;
  ...
  if (t >= 1.0f) st.settled = true;
  ```
- **Erwarteter Effekt**: Schätzung: bei überwiegend ruhenden Entities (Props, Pickups, NPCs in Idle) entfallen fast alle Arbeitskosten dieser Funktion und der davon ausgelöste Transform-Propagate. Bei 1000 replizierten Entities sind das 4000 Hash-/Sparse-Set-Zugriffe pro Frame. Bei vollständig bewegter Szene kein Unterschied.
- **Risiko / Verifikation**: Das Verhalten bleibt gleich, wenn `settled` bei jedem neuen Sample zurückgesetzt wird. Eine Entity, die eine Skript-Änderung an Position erfährt, wird heute vom Interpolator überschrieben. Das wäre mit `settled` nur noch einmal der Fall. Zu prüfen ist, ob das gewollt ist ("Server ist Wahrheit"). Dann wäre das Verhalten leicht verschieden, und die Änderung braucht eine Entscheidung. Tests: GameReplication-Tests (Interpolation, Baseline, Wrap-Fall 179° → -179°) um "Position nach t=1 unverändert und `dirty` nicht erneut gesetzt" ergänzen.

---

### SCN-302 – Assetabgeleitete Daten werden pro Entity kopiert (State-Machine-Graph, Blend-Spaces, Masken, Partikel-Config)
- **ID**: SCN-302
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance (Speicher/Heap) + Vereinfachung
- **Ort**: `Components/AnimatorStateMachineComponent.h:21-23, 30, 33, 49, 76, 86-93`; `Components/AnimationLayerComponent.h:105-120`; `AnimationPose.h:223-235`; `Components/ParticleSystemComponent.h:49-51, 60-73`
- **Problem**: Jede Entity mit State Machine hält eine private Kopie des geparsten Graphen (`HE::AnimatorStateMachineGraph resolvedGraph` mit States, Transitions, Strings). Dazu kommen `unordered_map<string,float> params`, zwei `std::string` (`currentStateName`, `transitionTarget`) und einen `deque` mit je einer eigenen Kopie jedes referenzierten `BlendSpace`. Dazu kommt eine ungenutzte `LegacyConfig` mit zwei Vektoren und einer Map, die in jeder Instanz mitgetragen wird. `AnimationLayerComponent` legt pro Entity `vector<vector<float>> resolvedMasks` plus eigene `blendSpaces` an. `ParticleSystemComponent` kopiert die `ParticleEmitterConfig` pro Emitter. Die Kommentare nennen den Zweck ("same discipline as ParticleSystemComponent::resolvedConfig", "assets shared by many entities"), aber der Cache ist pro Komponente statt pro Asset. Bei 200 Crowd-Charakteren mit gleichem State-Machine-Asset werden Graph und Blend-Spaces 200 Mal geparst und gehalten, und der Cache wird bei jeder Asset-Änderung für jede Komponente einzeln über `configDirty`/`markConfigDirty` invalidiert. Der Zustand pro Frame (`params` als `unordered_map<string,float>`) bedeutet außerdem String-Hash pro Parameterzugriff (Lesezugriff durch Sync-Graph und Transition-Bedingungen).
- **Code-Auszug**:
```cpp
HE::AnimatorStateMachineGraph resolvedGraph;   // per entity
std::unordered_map<std::string, float> params;
HE::BlendSpaceCache blendSpaces;               // deque of copies, per entity
```
- **Lösung**:
  1. Einen geteilten, unveränderlichen Resolve-Cache pro Asset einführen: `std::shared_ptr<const ResolvedStateMachine>` (Graph + vorgeparste Blend-Spaces + Parameter-Indextabelle), geführt in einem System-Cache `unordered_map<UUID, {assetRevision, shared_ptr}>`. Die Komponente hält nur den `shared_ptr` und die Asset-Revision, mit der sie aufgelöst wurde.
  2. Invalidierung über eine Asset-Revision statt `configDirty` pro Komponente. Das ersetzt `markConfigDirty` und die "Belt and braces"-Doppelprüfung der Masken (`AnimationLayerComponent.h:108-114`).
  3. Laufzeit-Parameter als `std::vector<float>` mit Index aus der geteilten Tabelle (Name → Index einmal beim Graph-Resolve). Namensbasierter Zugriff (`animator.setParam`) wandelt einmal in einen Index um.
  4. `LegacyConfig` in eine separate, selten benutzte Komponente auslagern oder nach dem Migrationslauf entfernen (`registry.remove`) – oder `std::unique_ptr<LegacyConfig>`, damit nicht jede Instanz Vektoren und Maps trägt.
  5. Gleiches Muster für `resolvedConfig` des Partikel-Systems. Bei diesem ist zu beachten, dass `RandomRange`-Knoten beim Resolve einmal gewürfelt werden (Kommentar `:44-48`). Das bleibt gewahrt, wenn der gewürfelte Wert Teil des geteilten Resolve-Ergebnisses ist; gewollt ist heute vermutlich "einmal pro Emitter" – sonst bekäme jeder Emitter gleiche Werte. Dort deshalb nur den unveränderlichen Teil teilen.
- **Erwarteter Effekt**: Schätzung: Speicher pro animierter Entity sinkt um den Graph/Blend-Space-Anteil (je nach Asset einige KB bis zehn KB und mehrere Heap-Blöcke); weniger Cache-Misses in der Komponenten-Iteration, weil die Komponente kompakter wird. Parse-/Resolve-Kosten bei Asset-Reload sinken von N auf 1.
- **Risiko / Verifikation**: Mittel bis hoch (öffentliche Komponenten, Serializer, Editor-Panels, `AnimationStateMachineSystem.cpp`, außerhalb dieses Slices). Zuerst Speicher und Parse-Zeit mit 200 Instanzen messen. Danach schrittweise umbauen, erst Graph, dann Blend-Spaces. Tests: bestehende State-Machine-/Blend-Space-Tests plus "zwei Entities, ein Asset, Asset editieren → beide re-resolven genau einmal".

---

### SCN-303 – Host-Snapshot: pro Client alle Entities, 3 Lookups je Entity, Samples pro Client neu berechnet
- **ID**: SCN-303
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `GameReplication.cpp:679-772` (`sendSnapshots`), speziell `:701-730`, `:751-764`
- **Problem**: Alle `tickHz` (typisch 30 Hz) läuft für jeden Client die Schleife über alle `m_byNetId`. Pro Entity gibt es `reg.valid`, `try_get<NetworkComponent>`, `try_get<TransformComponent>`, einen Distanztest und `toWire` + `wireOrigin` (weiteres `try_get<HierarchyComponent>`). Die Wire-Samples hängen nicht vom Client ab, sondern nur der Relevanz-Test. Sie werden aber für jeden Client neu berechnet. Der Vektor `relevant` wird pro Client und Tick neu angelegt (kein `reserve`). Pro Datagramm entsteht ein `BitWriter` (vermutlich mit eigenem Heap-Puffer, in diesem Slice nicht sichtbar). Es gibt keine Delta- oder Dirty-Logik: Entities, die sich nicht bewegt haben, werden trotzdem 30 Mal pro Sekunde gesendet. `replicateTransform` ist nur ein manueller Schalter (`NetworkComponent.h:41`). Kosten wachsen mit O(Clients × Entities).
- **Code-Auszug**:
```cpp
for (const ConnectionId conn : m_net->connections()) {
    ...
    std::vector<std::pair<std::uint32_t, Sample>> relevant;
    for (const auto& [netId, entity] : m_byNetId) {
        if (!reg.valid(entity)) continue;
        auto* nc = reg.try_get<NetworkComponent>(entity);
        auto* tc = reg.try_get<TransformComponent>(entity);
        ...
        s.position = toWire(tc->position, wireOrigin(m_world, entity));
```
- **Lösung**:
  1. Einmal pro Tick eine Tabelle `{netId, entity-Position (vec3), Sample, relevanceRadius²}` bauen (ein Durchlauf, ein `reserve`). Danach pro Client nur noch Distanztest über das flache Array.
  2. Optional: Quantisierte Samples vergleichen, nur bei Änderung gegenüber dem letzten pro Client gesendeten Stand senden (Delta/Dirty). Das ändert das Wire-Verhalten und braucht eine Client-seitige Anpassung (Extrapolationsende). Ist ein Protokolländerungs-Thema, nicht nur Aufräumen.
  3. `BitWriter` und `relevant` als Member-Scratchpad wiederverwenden.
- **Erwarteter Effekt**: Schätzung: bei 8 Clients und 2000 Entities 8×2000×3 Lookups weniger (48 000 pro Tick, 1,4 M pro Sekunde); Schritt 1 allein bringt ungefähr den Faktor Client-Anzahl für den Lookup-Anteil.
- **Risiko / Verifikation**: Schritt 1 und 3 bytegleich auf dem Draht. Test mit bestehender Snapshot-Test-Suite (Reihenfolge der Entities im Snapshot muss gleich bleiben – `unordered_map`-Reihenfolge ist nicht definiert, siehe Kommentar `:594-596`; nach netId sortieren, falls Tests darauf bauen). Schritt 2 nur mit Protokollversion und Regressionstest "Entity bleibt nach Stillstand stehen".

---

### SCN-304 – `AudioEngine::play` kopiert den kompletten PCM-/Ogg-Puffer pro Aufruf
- **ID**: SCN-304
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonScene/AudioEngine.h:105-115, 123-125, 225-228, 288-293` (`startSound(const std::vector<uint8_t>& bytes, ...)`)
- **Problem**: Der Header dokumentiert selbst: "PCM16 copies the samples into the voice … Both copy because the asset lives in ContentManager's dense storage, which the next loadAsset() may relocate". Jeder `play`/`playSpatial` (Schuss, Schritt, UI-Klick) kopiert damit die gesamte Clip-Datenmenge auf dem Aufrufer-Thread (Update-/Spiel-Thread) und allokiert dafür. Der Kommentar bei `pauseSound` (`:225-228`) beziffert es selbst: "a multi-megabyte PCM copy per press". Eine Musikspur mit mehreren MB wird bei jedem Start einmal kopiert, ein oft ausgelöster Effekt bei jedem Auslösen.
- **Code-Auszug**:
```cpp
// PCM16 copies the samples into the voice (as the raw overload below); Vorbis copies only the Ogg bytes ...
// Both copy because the asset lives in ContentManager's dense storage ...
uint64_t play(const AudioAsset& clip, float volume = 1.0f, ...);
```
- **Lösung**:
  1. `AudioAsset::audioData` als `std::shared_ptr<const std::vector<uint8_t>>` (bzw. unveränderlichen, referenzgezählten Puffer) halten. Die Voice hält eine Referenz statt einer Kopie. Das behebt auch die Relokations-Gefahr, die der Grund der Kopie ist.
  2. Alternativ im Mixer-Backend (`miniaudio` `ma_audio_buffer_ref`) auf die geteilte Referenz zeigen und die Lebensdauer über den `shared_ptr` im Voice-Impl sichern.
  3. Gleichzeitig die Strings: `busName` ist `const std::string&`, `routeFor` baut pro Play einen neuen `std::string`. Das ist klein, aber im selben Pfad.
- **Erwarteter Effekt**: Schätzung: pro Sound-Start entfällt ein Memcpy der Clip-Größe (für 1 s Stereo 44,1 kHz PCM16 = 176 KB, für lange Clips MB) und eine Allokation; reduziert Hitches bei Gewehrfeuer/Effekt-Spam.
- **Risiko / Verifikation**: `AudioAsset` ist in `ContentManager/Assets.h` (anderes Modul/Slice) – Änderung der Asset-Struktur betrifft Serializer/Importer. Test: bestehende Audio-Tests (Headless `readMixedFrames`), zusätzlich "Asset während Wiedergabe neu laden" bleibt hörbar identisch und stürzt nicht ab.

---

### SCN-305 – `AudioSystem::updateSpatial` ruft pro Frame für jede Quelle `setSoundPosition` auf, ohne Änderungserkennung
- **ID**: SCN-305
- **Kategorie**: D / B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonScene/AudioSystem.h:51-78`
- **Problem**: Die View `AudioSourceComponent, TransformComponent` wird jeden Frame vollständig durchlaufen, und jede räumliche Quelle bekommt `engine.setSoundPosition(...)`, auch wenn sie sich nicht bewegt hat oder `handle` längst beendet ist. Die Engine-Implementierung (miniaudio-Wrapper) macht dafür sehr wahrscheinlich einen Handle-Lookup (Hash) und eine Sperre gegen den Audio-Thread (in diesem Slice nicht sichtbar, unsicher). Es gibt weder "Position geändert"-Prüfung noch Rückmeldung, wenn der Sound fertig ist; `src.handle` bleibt auf dem toten Wert. Außerdem wird `t.position` benutzt, also die lokale Position (`TransformComponent.h:20`, `worldMatrix` separat `:29`), obwohl für gekindete Entities die Weltposition gemeint ist. Eine Quelle unter einem verschobenen Parent klingt dann an der falschen Stelle. Das gilt analog für den Listener. Das ist ein Korrektheitsrisiko, kein reiner Performancepunkt, aber durch den Fund in `AudioSystem.h:61-67` direkt sichtbar (unsicher, ob alle Quellen bewusst Root-Level sind).
- **Code-Auszug**:
```cpp
auto srcView = reg.view<AudioSourceComponent, TransformComponent>();
for (auto [entity, src, t] : srcView.each()) {
    if (!src.spatial || src.handle == 0) continue;
    engine.setSoundPosition(src.handle, t.position.x, t.position.y, t.position.z);
}
```
- **Lösung**:
  1. Letzte gesendete Position in der Komponente (`glm::vec3 lastPos`) halten, nur bei Änderung (> Epsilon) senden. Nach Möglichkeit `isPlaying(handle)` selten prüfen oder in `AudioEngine` ein Rückruf/Reaping einführen, damit `handle` auf 0 gesetzt wird.
  2. Statt `t.position` die Weltposition aus `worldMatrix[3]` verwenden. Damit das stimmt, muss die Transform-Propagation vor dem Audio-Update gelaufen sein (Reihenfolge in `GameApplication.cpp:3294` / `EditorApplication.cpp:3950` prüfen).
  3. Der `break` in der Listener-Schleife ist korrekt, aber `view.each()` + `break` kann durch `view.front()`/`*view.begin()` ersetzt werden (Lesbarkeit).
- **Erwarteter Effekt**: Schätzung: bei 100 statischen Quellen 100 Engine-Aufrufe pro Frame weniger; Wirkung hängt von den Kosten von `setSoundPosition` ab (messen).
- **Risiko / Verifikation**: Schritt 1 verhaltensgleich. Schritt 2 ändert hörbares Verhalten für gekindete Quellen (Fehlerkorrektur) – mit einem Test belegen: Quelle als Kind eines verschobenen Parents, erwartete Weltposition im Engine-Mock.

---

### SCN-306 – `FoliageComponent` hält Instanzen doppelt (`cachedInstances` als `vector<mat4>` und `store`)
- **ID**: SCN-306
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance (Speicher)
- **Ort**: `Components/FoliageComponent.h:38, 45, 50`
- **Problem**: `cachedInstances` speichert jede Instanz als Welt-`mat4` (64 Byte). Dazu hält die Komponente das bereits sortierte, terrain-lokale `store`. Laut Kommentar liest der Renderer nur `store`; `cachedInstances` bedient nur "editor read-outs and the scatter tests" (`:41-43`). Bei Grasfeldern mit Hunderttausenden Instanzen sind das mehrere zehn MB, die in jedem Spiel-Build ungenutzt bleiben. `densityMask` (`maskRes²` Bytes) und `cachedInstances` werden außerdem bei jedem Komponenten-Move/-Copy (Undo-Snapshot, Szenen-Clone) mitkopiert. Das gleiche Muster gilt für `std::vector<glm::mat4>` im Play-Snapshot des Editors (Cross-Modul, unsicher).
- **Code-Auszug**:
```cpp
std::vector<glm::mat4> cachedInstances;
std::shared_ptr<const FoliageStore> store;
```
- **Lösung**:
  1. `cachedInstances` nur im Editor/Test-Build halten oder als Funktion `instancesWorld(const FoliageStore&, const glm::mat4& terrainWorld)` bei Bedarf aus dem `store` ableiten.
  2. Mindestens `cachedInstances` nach dem Scatter leeren, wenn kein Editor-Konsument sie braucht, und die Tests auf die Ableitungsfunktion umstellen.
- **Erwarteter Effekt**: Schätzung: 64 B × Instanzanzahl pro Foliage-Layer (z. B. 500 000 Instanzen ≈ 32 MB) weniger RAM.
- **Risiko / Verifikation**: Editor-Panels und Scatter-Tests lesen das Feld (Aufrufer in `FoliageSystem.cpp`, Tests; nicht in diesem Slice). Mit `grep cachedInstances` alle Leser prüfen, Verhalten pro Test absichern.

---

### SCN-307 – `ParticleSystemComponent` trägt einen `std::mt19937` (≈ 2,5 KB) inline
- **ID**: SCN-307
- **Kategorie**: C / F
- **Schweregrad**: Mittel
- **Typ**: Performance (Speicher/Cache) + Header-Gewicht
- **Ort**: `Components/ParticleSystemComponent.h:5, 42, 49, 60-73`
- **Problem**: `std::mt19937 rng { 42 }` hat 624 × 4 Byte Zustand. Jede Komponente ist dadurch über 2,5 KB groß, plus `resolvedConfig`, `LegacyConfig`, `std::vector<Particle>`. Die ECS-Pool-Elemente werden bei Swap/Move beim Entfernen von Komponenten (entt: swap-and-pop) mit diesem Brocken bewegt. Eine Iteration über alle Emitter streift pro Element ≥ 2,5 KB. Der Header zieht `<random>` in jede Datei, die die Komponente sieht (Serializer, Extractor, Editor-Panels).
- **Code-Auszug**:
```cpp
std::vector<Particle> particles;
std::mt19937          rng { 42 };
```
- **Lösung**: Durch einen kleinen Zähler-basierten Generator ersetzen (z. B. PCG32 oder xorshift, 8-16 Byte). Die Folge ändert sich dadurch, was bei deterministischen Replays oder Tests mit festen Seeds sichtbar sein kann. Falls bitgleiche Ergebnisse gebraucht werden, `mt19937` hinter `std::unique_ptr` legen (lazy beim ersten Emit).
- **Erwarteter Effekt**: Schätzung: Komponentengröße sinkt von ≈ 2,7 KB auf ≈ 0,3 KB; weniger Cache-Druck in Partikel-Update und -Extraktion; `<random>` fällt aus den Include-Ketten.
- **Risiko / Verifikation**: Das Ergebnis einer Partikelsimulation mit festem Seed ändert sich bei Generatorwechsel. Prüfen, ob Tests oder Replays auf konkrete Werte vergleichen (`grep -rn "rng" tests/`), sonst Variante mit `unique_ptr`.

---

### SCN-308 – Generierter Event-Dispatch: String-Kette pro Event, `RunState` mit `hc::Value` pro Aufruf
- **ID**: SCN-308
- **Kategorie**: B / C
- **Schweregrad**: Mittel
- **Typ**: Performance (Laufzeit des generierten Codes)
- **Ort**: `src/HE_Scene/src/HcCodegen.cpp:3488-3527` (Emission von `fireEvent`), `:3270-3283` (RunState), `:3534-3563` (Engine-Hooks)
- **Problem**: Der Generator emittiert für jede Klasse `fireEvent(const std::string& name, int elem, const hc::Value& arg)` als `if (name == "A") … else if (name == "B") …`. Wer per Name feuert (Runtime-Seam, Netzwerk, Bind-Event), zahlt O(#Events) `std::string`-Vergleiche pro Aufruf; bei Basisklassen kommt der Vergleichslauf der Eltern hinzu. Pro Aufruf wird ein `RunState rs;` auf dem Stack angelegt. Es enthält `hc::Value eventArg` (Value trägt laut Nutzung `items`, `keys`, `s`, `typeName`, also mehrere Container/Strings), und `rs.eventArg = arg` kopiert das Argument vollständig. Für die Engine-Hooks (`OnTick`, `OnHit` …) umgeht der Generator das bereits (direkte Overrides `:3534-3563`), nur die Value-Boxing-Schritte bleiben (`rs.eventArg = hc::toValue(arg)`, `:3555`). Der Generator hat die Gegenseite schon gelöst: Emit/Bind-Events werden zu `hc::EventId` internalisiert (`:3329-3337`), der Dispatch geht aber trotzdem über Strings.
- **Code-Auszug**:
```cpp
c += std::string("    ") + (first ? "if" : "else if") + " (name == " + strLit(name) + ")\n    {\n";
...
if (m_usesEventArg) c += "    rs.eventArg = arg;\n";
```
- **Lösung**:
  1. `fireEvent` zusätzlich als `fireEventId(EventId, …)` mit `switch` über die internalisierten Ids erzeugen und die Runtime auf Ids umstellen (die Id-Tabelle existiert, siehe `kEvt_…`).
  2. Alternativ den Namensvergleich über einen vorab generierten, nach Länge/erstem Zeichen gestaffelten Schalter (`switch (name.size())`) beschleunigen – weniger invasiv, aber weniger Wirkung.
  3. `RunState::eventArg` als `const hc::Value*` statt Wertkopie führen (Argument lebt während des Aufrufs).
- **Erwarteter Effekt**: Schätzung: pro Event-Dispatch ein Wert-Copy und eine Handvoll String-Vergleiche weniger; relevant für Klassen mit 10+ Events und vielen Instanzen (z. B. OnTick für Hunderte Entities). Vor der Umsetzung messen, wie viele Dispatches pro Frame wirklich über den Namenspfad laufen.
- **Risiko / Verifikation**: Hoch für Semantik-Parität: der Generator hat einen "Parity-Harness" (Kommentare zu `hcgen_C_math_ops`, "traced parity text"). Jede Änderung der Emission muss dessen Tests bestehen. Nur mit Messung (Profil mit `fireEvent` im Hot-Path) angehen.

---

### SCN-309 – Generierte Engine-Calls: `std::vector<hc::Value>` und Namens-Dispatch pro Aufruf
- **ID**: SCN-309
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance (Laufzeit des generierten Codes)
- **Ort**: `HcCodegen.cpp:2248-2274` (`emitValueCall`), `:1885-1898` (pure EngineCall), `:2707-2712`, `:1815-1823` (`hc::getProperty(m_ctx, elem, "name")`), `:2403-2410`
- **Problem**: Jeder Engine-Call im generierten C++ packt seine Argumente in einen frischen `std::vector<hc::Value>` (Heap), ruft `hc::callApi(m_ctx, "api.name", args)` über den Namen (Hash- oder Tabellensuche zur Laufzeit) und entpackt das Ergebnis aus einem weiteren Vector. Pure Calls werden "on every data-out read" dispatcht (`:1889-1897`, `std::vector<hc::Value>{...}` als Ausdruck). `getProperty`/`setProperty`/`getExternal` laufen ebenfalls per Namensstring. Damit sind genau die häufigsten Knoten in Gameplay-Graphen (Transform lesen/schreiben, Distanz, Input) heap- und string-lastig. Dass der Generator Direktaufrufe zwischen kompilierten Klassen bereits typisiert emittiert (`directFn`, `directVar`), zeigt, dass das Muster für Engine-APIs fehlt.
- **Code-Auszug**:
```cpp
std::string args = "std::vector<hc::Value>{";
...
return fromValueCall("hc::callApi(m_ctx, " + strLit(n.s) + ", " + args + ")", (size_t)outIdx, out);
```
- **Lösung**:
  1. `HE::api::ApiFn` auf eine aufgelöste Funktionsadresse/Id erweitern, die der Generator beim Erzeugen kennt (er ruft `HE::api::find(n.s)` schon, `:1332`). Dann `static const hc::ApiId kApi_n42 = hc::apiId("name");` pro Call (analog zu `kEvt_…`) und `hc::callApi(m_ctx, kApi_n42, args)`.
  2. Argumente in ein `std::array<hc::Value, N>` oder `hc::ArgPack` mit Small-Buffer legen statt `std::vector`.
  3. Häufige Standard-APIs (Transform get/set) direkt typisiert über einen Fast-Path (`hc::getPosition(m_ctx, ref)`) emittieren.
- **Erwarteter Effekt**: Schätzung: pro Engine-Call eine bis zwei Heap-Allokationen und eine String-Suche weniger; relevant bei Tick-Graphen mit vielen Calls pro Frame und Instanz. Messen: Profil eines kompilierten Tick-Graphen.
- **Risiko / Verifikation**: Die Dispatch-Zählung ist Teil der Parität ("its dispatch count is part of the trace (§3.4)", `:1855`). Ersetzen des Lookups ändert die Anzahl der Dispatches nicht, aber jede Änderung an der Emission braucht den Parity-Test. API-Registry liegt außerhalb dieses Slices (`EngineApi.h/.cpp`).

---

### SCN-310 – Datenrasse auf den globalen cmake-Cache in `HcCodegen.cpp`
- **ID**: SCN-310
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektheit)
- **Ort**: `HcCodegen.cpp:4222-4276` (`g_bundledCmakeDir`, `g_resolvedCmake`, `g_cmakeResolved`, `resolveCmake`, `setBundledCmakeDir`)
- **Problem**: Der Kommentar in `setBundledCmakeDir` (`:4271-4272`) räumt selbst ein, dass der Editor die Funktion "possibly while an export worker is inside resolveCmake()" aufruft. `g_bundledCmakeDir` (ein `std::filesystem::path`), `g_resolvedCmake` (`std::string`) und `g_cmakeResolved` (`bool`) sind aber ungesicherte Globals. Gleichzeitiges Schreiben (UI) und Lesen/Schreiben (Worker) auf `std::string`/`path` ist undefiniertes Verhalten und kann bei Pfadwechsel crashen. Die Absicht (nur geänderter Pfad leert den Cache) schützt nur den Gleichheitsfall.
- **Code-Auszug**:
```cpp
std::filesystem::path g_bundledCmakeDir;
std::string g_resolvedCmake;
bool        g_cmakeResolved = false;
```
- **Lösung**: Alle drei hinter einen `std::mutex` legen (kurzer Lock um `resolveCmake` und `setBundledCmakeDir`; die Prozessaufrufe in `cmakeAnswers` außerhalb des Locks, Ergebnis danach unter Lock speichern) und `resolveCmake` einen Wert statt `const std::string&` zurückgeben lassen (die Referenz auf das Global wird sonst nach einem Reset zum Dangling-Zugriff).
  ```cpp
  std::string resolveCmake() { std::lock_guard l(g_mx); ... return g_resolvedCmake; }
  ```
- **Erwarteter Effekt**: Kein Performance-Effekt; beseitigt eine seltene, schwer reproduzierbare Absturzquelle im Export-Pfad.
- **Risiko / Verifikation**: Niedrig. Test: zwei Threads, einer `setBundledCmakeDir` mit wechselnden Pfaden, einer `toolchainAvailable()`; TSan-Lauf.

---

### SCN-311 – Schwere Header: Game-Logic-Dispatcher als Header-only mit großen Includes
- **ID**: SCN-311
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Build-Zeit)
- **Ort**: `AnimationNotifySystem.h:1-9`, `CollisionSystem.h:1-9`, `AntiCheat/AntiCheatEvents.h:2-10`, `AudioSystem.h:1-9`, `Components/ParticleSystemComponent.h:1-6`, `AnimationPose.h:1-9`, `CellStreamer.h:1-2`
- **Problem**: Mehrere "Dispatcher"-Strukturen sind komplett im Header implementiert und ziehen `HorizonWorld.h`, `PhysicsWorld.h`, `ScriptEngine.h`, `HorizonCodeRuntime.h`, `ScriptContext.h`, `ContentManager.h` herein. Jede Datei, die nur `AnimationNotifySystem::dispatch` aufruft, kompiliert damit die gesamte Welt-, Physik- und Script-API mit. `AudioSystem.h` inkludiert `ContentManager.h` und `AudioEngine.h` nur für zwei Funktionen; `ParticleSystemComponent.h` zieht `<random>` und das ganze `ParticleGraph.h`; `CellStreamer.h` zieht `JobSystem/JobSystem.h`. Alle Dispatcher wiederholen dasselbe Gerüst "Script-Instanz, HorizonCode-Instanz, Sync-Graph" mit dem Default-Argument `const HcInstanceMap& = {}`. Das konstruiert in jedem Aufruf eine temporäre `unordered_map` (auf MSVC allokiert der Default-Konstruktor den Sentinel-Knoten). Zudem existiert `InstanceMap` als identischer Alias in drei Strukturen.
- **Code-Auszug**:
```cpp
static void dispatch(HE::NotifyQueue& queue, HorizonWorld& world, ScriptContext* scripts,
                     const InstanceMap& instances, HorizonCode::Runtime* runtime = nullptr,
                     const HcInstanceMap& hcInstances = {}, AnimatorHost* animators = nullptr)
```
- **Lösung**: Dispatcher nach `.cpp` verlagern (`AnimationNotifySystem.cpp`, `CollisionSystem.cpp`, `AntiCheatEvents.cpp`; im Header bleiben Vorwärtsdeklarationen), `AudioSystem` ebenso. `InstanceMap`/`HcInstanceMap` in einen gemeinsamen kleinen Header (`ScriptInstanceMaps.h`). Der Default `= {}` durch einen Überlad ohne Parameter oder `const HcInstanceMap* = nullptr` ersetzen.
- **Erwarteter Effekt**: Schätzung: spürbar kürzere inkrementelle Builds (Änderungen an `ScriptEngine.h`/`HorizonCodeRuntime.h` triggern weniger Dateien). Laufzeit kaum.
- **Risiko / Verifikation**: Niedrig, rein mechanisch. Auswirkung per `-ftime-trace`/Include-Zählung (`clang -H`) vorher/nachher belegen.

---

### SCN-312 – `AnimatorHost` hält drei Hash-Maps mit demselben Schlüssel
- **ID**: SCN-312
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `AnimatorHost.h:96-103` (`m_byEntity`, `m_assetOf`, `m_paramsOf`), `:86-90` (`instanceOf`)
- **Problem**: Pro Entity gibt es drei parallele `unordered_map<uint32_t, …>` (Instanz-Id, Asset-Uuid, Parameternamen als `vector<string>`). `bind`/`fireUpdate` (pro Frame und animierter Entity, laut Kommentar `:57-61`) und `instanceOf` (pro Notify) machen mehrere Lookups mit identischem Schlüssel; `unbind` muss alle drei konsistent löschen.
- **Code-Auszug**:
```cpp
std::unordered_map<uint32_t, HorizonCode::InstanceId> m_byEntity;
std::unordered_map<uint32_t, HE::UUID> m_assetOf;
std::unordered_map<uint32_t, std::vector<std::string>> m_paramsOf;
```
- **Lösung**: Eine Map `unordered_map<uint32_t, Binding>` mit `struct Binding { InstanceId id; HE::UUID asset; std::vector<std::string> params; }` (oder Parameter-Indizes aus SCN-302). Ein Lookup, ein Erase.
- **Erwarteter Effekt**: Schätzung: zwei Hash-Lookups pro `fireUpdate`/`bind`-Aufruf weniger; weniger Konsistenzfehler-Potenzial.
- **Risiko / Verifikation**: Niedrig; `.cpp` liegt außerhalb dieses Slices. Vorhandene Animator-Host-Tests nutzen.

---

### SCN-313 – Frame-Flags, die ein Pass pro Frame zurücksetzen muss (`finalizedThisFrame`, `appliedThisFrame`)
- **ID**: SCN-313
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance + Vereinfachung
- **Ort**: `Components/AnimationLayerComponent.h:126`, `Components/IkComponent.h:118`; Gegenstelle `src/PoseFinalize.cpp:434-440` (`poseBeginFrame`, nicht Teil dieses Slices)
- **Problem**: Beide Komponenten tragen ein `bool …ThisFrame`, das `poseBeginFrame` zu Beginn der Animationsphase in einer eigenen vollständigen View-Iteration (zwei Views) auf `false` setzt. Das ist ein zusätzlicher Pass über alle Layer- und IK-Komponenten pro Frame, nur um doppelte Pose-Treiber zu erkennen.
- **Code-Auszug**:
```cpp
bool finalizedThisFrame = false;   // AnimationLayerComponent / IkComponent
```
- **Lösung**: Statt Bool einen `uint32_t finalizedFrame` führen und gegen einen von der Welt/dem System gezählten `frameIndex` vergleichen (`if (c.finalizedFrame == frame) …; c.finalizedFrame = frame;`). `poseBeginFrame` und die Rücksetz-Passes entfallen.
- **Erwarteter Effekt**: Schätzung: zwei View-Iterationen pro Frame weniger; praktisch relevant erst bei vielen Charakteren.
- **Risiko / Verifikation**: Niedrig; Reihenfolge-Test mit Entity, die zwei Treiber trägt (heute geloggt, `PoseFinalize.cpp:458`).

---

### SCN-314 – Entity-weise Heap in Basis-Komponenten (`HierarchyComponent`, `NameComponent`, `MaterialComponent`, `LODComponent`)
- **ID**: SCN-314
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance (Speicher)
- **Ort**: `Components/HierarchyComponent.h:14-17`, `Components/NameComponent.h:5`, `Components/MaterialComponent.h:26, 34`, `Components/LODComponent.h:16`
- **Problem**: Jede Entity mit Kindern hat einen `std::vector<entt::entity> children`. Jeder Name ist ein `std::string` (heap ab 16 Zeichen, "Rock_Large_Mossy_03_LOD0" liegt schon darüber). `MaterialComponent` hat zwei Vektoren (`paramOverrides` mit je einem `std::string name`, `slotOverrides`), `LODComponent` einen Vektor. Bei 100 000 Szenen-Entities (Streaming-Zellen) summiert sich das auf viele kleine Allokationen und verschlechtert Cache-Lokalität beim Traversieren (`propagateTransforms` läuft über `children`).
- **Code-Auszug**:
```cpp
struct HierarchyComponent { entt::entity parent = entt::null; std::vector<entt::entity> children; };
struct NameComponent { std::string name; };
```
- **Lösung**: Nur mit Messung angehen. Optionen: Geschwister als intrusive verkettete Liste (`firstChild`, `nextSibling`, `prevSibling` als `entt::entity`, 12 Byte, keine Allokation, O(1) Reparent), Namen in einer String-Tabelle (`uint32_t nameId`) oder `SmallString<24>`. Das ändert APIs (`children` wird in Editor und Serializer direkt gelesen).
- **Erwarteter Effekt**: Schätzung: eine Allokation pro Eltern-Entity und pro langem Namen weniger. Nur relevant in sehr großen Szenen; vor Umbau Heap-Profil einer Zelle mit 50 000 Entities ziehen.
- **Risiko / Verifikation**: Hoch (Hierarchie-API wird überall genutzt, Serialisierung); kein Umbau ohne Messdaten. Nach Umbau Round-Trip-Tests der Szene-Serialisierung und Reihenfolge der Kinder (`SceneSerializer`-Tests).

---

### SCN-315 – `AnimationNotifyEvent` trägt `std::string name` pro gefeuertem Event
- **ID**: SCN-315
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `AnimationNotify.h:21-32`, `AnimationNotifySystem.h:66-69, 99-101`
- **Problem**: Jedes Event kopiert den Notify-Namen in einen eigenen `std::string`. Namen bis 15 Zeichen sind SSO (z. B. "footstep_l"), längere (`"Attack_Hit_Window_Start"`) allozieren pro Auslösung. Danach ruft der Dispatcher `callOnAnimationNotify(inst, ev.name)` und HorizonCode-`fire*` mit dem String, ggf. wird dort erneut kopiert oder hashed. Das passiert pro Frame und Entity, die Notifies feuert.
- **Code-Auszug**:
```cpp
struct AnimationNotifyEvent { uint32_t entity = 0; std::string name; Kind kind = Kind::Fire; };
```
- **Lösung**: `const std::string*` oder `std::string_view` auf den Namen im `AnimationNotify` des Clips (Clip-Asset lebt mindestens bis zum Dispatch im selben Frame, der Queue-Kommentar `:30-31` garantiert "filled this frame, emptied this frame"). Oder Notify-Namen beim Asset-Laden internieren (`uint32_t nameId`) und erst an der Skript-Grenze in Text auflösen.
- **Erwarteter Effekt**: Schätzung: eine mögliche Allokation pro Event weniger; gering, weil Events selten sind.
- **Risiko / Verifikation**: Lebensdauer des Clip-Assets ist die einzige Gefahr (Asset-Reload im selben Frame). Wenn nicht sicher, bei `std::string` bleiben. Kein Eingriff ohne Profil.

---

### SCN-316 – `HcCodegen.cpp`: Emitter, Probe-Doppeldurchlauf, Link-Suche in O(Links)
- **ID**: SCN-316
- **Kategorie**: A / D / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung (primär)
- **Ort**: `HcCodegen.cpp:1119-1130` (`execLinkFrom`/`dataLinkTo`), `:3833-3862` (Probe-Pass), `:1455-1466` (`std::function`-DFS), `:290-330` (`valueLit`, `Value item = src[i]`), ganze Datei 4917 Zeilen
- **Problem**:
  - Die Datei vermischt vier Aufgaben: Typ-/Enum-Header-Emission, Klassen-Emitter (`ClassEmitter`, ≈ 2300 Zeilen), Build-Manifest und Toolchain-Verwaltung (SDK-Suche, cmake/vswhere, Paketmanager-Installation, `buildDylib`, ab `:4062`). Letzteres hat mit Szenenlogik nichts zu tun.
  - `execLinkFrom` und `dataLinkTo` durchsuchen für jedes Statement/jede Eingabe linear alle Links (`for (const auto& l : m_g.links)`). Aufrufer sind `chain`, `input`, `targetOf`, `slotRef`. Das ist O(Knoten × Links) je Klasse – bei Graphen mit mehreren Tausend Links spürbar im Editor-Klick "Generate".
  - Der Probe-Durchlauf (`:3833-3862`) emittiert jede Klasse komplett (Header und Impl-String), um nur die öffentliche Oberfläche zu bekommen, und wirft das Ergebnis weg. Danach läuft der echte Durchlauf. Die Emission wird damit verdoppelt.
  - `valueLit` kopiert pro Array-Element den ganzen `Value` (`Value item = src[i]`), obwohl nur `isArray`/`container`/`type` geändert werden.
  - `checkCycles` nutzt rekursives `std::function` und `unordered_map` mit `operator[]` in der Schleife (legt Einträge an).
- **Code-Auszug**:
```cpp
const Link* execLinkFrom(int nodeId, int pin) const {
    for (const auto& l : m_g.links) if (l.srcNode == nodeId && l.srcPin == pin) return &l;
    return nullptr; }
```
- **Lösung**:
  1. Beim Konstruktor des `ClassEmitter` einmal `unordered_map<uint64_t(node<<32|pin), const Link*>` für Exec-out-Links und Data-in-Links bauen.
  2. Toolchain-Teil (ab `:4062`) in `HcToolchain.cpp` auslagern; `HiddenShell` gehört mit dorthin.
  3. Probe-Pass: Oberflächenberechnung (`m_publicFn`, `publicMethodSigs`, `variableMembers`) aus `run()` herausziehen (`buildNames()` genügt dafür), ohne die Bodies zu emittieren.
  4. `Value item` als Referenz plus lokale Typ-Overrides (`valueLit` mit Parameter statt Kopie).
- **Erwarteter Effekt**: Schätzung: Generate-Zeit im Editor sinkt bei großen Projekten, Faktor bis ~2 durch den Probe-Pass; Lesbarkeit durch Aufteilung deutlich besser. Kein Laufzeiteffekt im Spiel.
- **Risiko / Verifikation**: Die Ausgabe muss bytegleich bleiben (Parity-/Snapshot-Tests der generierten Dateien, `hcgen_*`-Fixtures). Für Schritt 3 vorher prüfen, dass `buildNames()` allein die Tabellen vollständig füllt (die Warnungen des Probe-Passes werden ohnehin verworfen).

---

### SCN-317 – Toolchain-Aufrufe starten pro Aufruf mehrere Prozesse
- **ID**: SCN-317
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `HcCodegen.cpp:4314-4322` (`commandExists`), `:4462-4497` (`chooseGenerator`, 2 Prozesse unter Windows), `:4553-4600` (`probeToolchain`), `:4877` (`buildDylib` ruft `chooseGenerator` erneut), `:4226-4234` (`cmakeAnswers`)
- **Problem**: `probeToolchain` und jeder `buildDylib`-Lauf rufen `chooseGenerator` auf, was unter Windows `vswhere` und `cmake -E capabilities` startet (zwei Prozesse mit `cmd.exe /c`), jedes Mal neu. `commandExists` startet eine Shell pro Prüfung. Alles synchron über `popen`, also auf dem aufrufenden Thread. Die Aufrufer sind Worker laut Kommentar (`:4148-4151`), aber die Ergebnisse (VS-Instanz) ändern sich nur bei Installation.
- **Code-Auszug**:
```cpp
runStreaming(vswhere + " -products * -prerelease ..." , nullptr, listed);
runStreaming(cmake + " -E capabilities", nullptr, caps);
```
- **Lösung**: `GeneratorChoice` einmal pro Session cachen (invalidieren bei `setBundledCmakeDir` und explizitem "Recheck"). Mit dem Mutex aus SCN-310 koppeln.
- **Erwarteter Effekt**: Schätzung: pro Generate/Build 2 Prozessstarts (je ≈ 100-500 ms unter Windows) gespart.
- **Risiko / Verifikation**: Cache-Invalidierung beim Installieren eines Visual Studios (`installToolchain`) beachten. Test: nach `installToolchain` wird neu geprobt.

---

### SCN-318 – Kleinigkeiten (Container, Casts, Versionstabellen)
- **ID**: SCN-318
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `GameReplication.cpp:26-31, 144-149, 849-854`; `CellStreamer.h:289`; `CameraRigController.h:62-89`; `AudioEngine.h:226-232`
- **Problem**:
  - `wireOrigin` nimmt `const HorizonWorld*` und macht dann `const_cast<HorizonWorld*>(world)->registry()`. Das ist ein Const-Bruch, nur um `try_get` aufzurufen.
  - `m_pendingInputs` (64 Elemente) ist ein `std::vector` mit `erase(begin())` (`:148`) und `remove_if`-erase (`:849`). Ein `std::deque` oder Ring passt besser zur FIFO-Semantik.
  - `std::unordered_map<Key, bool> m_failed` ist ein Set (`CellStreamer.h:289`).
  - `CameraRigController::update` hat zwei Überladungen, nur um die alte Signatur zu erhalten; ein einziger Aufrufpunkt mit Aggregat genügt, sobald alle Aufrufer migriert sind.
- **Code-Auszug**:
```cpp
const auto* h = const_cast<HorizonWorld*>(world)->registry().try_get<HierarchyComponent>(e);
...
m_pendingInputs.erase(m_pendingInputs.begin());
```
- **Lösung**: `registry()` const-Überlast in `HorizonWorld` bereitstellen (oder `const entt::registry&` übergeben), `std::deque`/Ringpuffer für Inputs, `unordered_set<Key>` für `m_failed`.
- **Erwarteter Effekt**: Kosmetisch; kleine Verbesserung in Lesbarkeit und bei 64-Element-Verschiebungen pro Eingabe.
- **Risiko / Verifikation**: Niedrig.

---

## Cross-Modul-Hinweise

- `src/HE_Scene/src/PoseFinalize.cpp:434-440` (`poseBeginFrame`): zwei vollständige View-Läufe pro Frame nur zum Zurücksetzen von Bools (siehe SCN-313).
- `src/HE_Scene/src/AnimationStateMachineSystem.cpp`, `AnimationSystem.cpp`, `AnimationBlendSystem.cpp`: per-Entity `params` per String-Hash und per-Entity Blend-Space-Cache (SCN-302). Dort prüfen, ob pro Frame `std::string`-Vergleiche (`currentStateName`, `transitionTarget`) laufen.
- `src/HE_Scene/src/FoliageSystem.cpp` und Editor-Foliage-Paint: Nutzer von `cachedInstances` (SCN-306).
- `src/HE_Scene/src/SceneSystems.cpp`, `HE_Game/src/GameApplication.cpp:3294`, `HE_Editor/EditorApplication.cpp:3950`: Aufruf von `AudioSystem::updateSpatial` — Reihenfolge zur Transform-Propagation prüfen (SCN-305).
- `ContentManager/Assets.h` (`AudioAsset::audioData`): Umstellung auf geteilten, unveränderlichen Puffer (SCN-304).
- `EnvironmentComponent.h:11-13`: Das Environment wird laut Kommentar "pushed to the renderer each frame via IRenderer::SetEnvironmentSettings". Die Struktur ist rund 400 Byte groß. Ein Revisionszähler am Environment würde Vergleich/Upload bei unveränderten Werten sparen (Rendering/Editor prüfen).
- `AntiCheatService.h:296-297` und `AntiCheatTelemetry.h:130`: `Report` enthält `vector<Observation>` mit je zwei Strings; Reports werden in `deque<Report>` und `m_telemetry` kopiert (nur bei Ereignissen, daher unkritisch, aber bei Event-Sturm teuer).
- `GameReplication.h:348` / `NetworkComponent.h`: Bei Umbau nach SCN-301/303 Protokoll- und Client-Tests in `tests/` mitziehen.

## Unsicher / Messung nötig

- SCN-301: Anteil statischer Entities in `m_interp` im realistischen Spiel (Verhältnis ruhend zu bewegt) und tatsächliche Kosten von `propagateTransforms` bei gesetztem `dirty`.
- SCN-302: Speicher und Parse-Zeit pro State-Machine-Instanz; Größe von `AnimatorStateMachineGraph` und `BlendSpace`.
- SCN-303: Reale Snapshot-Zeit bei 8 Clients × 2000 Entities; Kosten von `BitWriter`-Konstruktion.
- SCN-304: Kosten von `startSound` (Kopie plus Mixer-Setup) bei langen Clips; ob die Kopie wirklich auf dem Spiel-Thread läuft.
- SCN-305: Kosten und Sperren von `setSoundPosition` im miniaudio-Wrapper (`AudioEngine.cpp`, nicht in diesem Slice). Ob gekindete Audioquellen/Listener vorkommen (lokale vs. Weltposition).
- SCN-308/309: Wie oft generierter Code über den Namenspfad (`fireEvent(name)`, `callApi`) statt über direkte Hooks läuft; nur mit Profil eines kompilierten Tick-Graphen bewertbar.
- `AnimationPose.h:207-214` (`blendPosesN(const std::vector<std::vector<JointTRS>>&, …)`): ob die Aufrufer die Pose-Puffer pro Frame wiederverwenden oder verschachtelte Vektoren neu anlegen (Implementierung in `AnimationPose.cpp`, außerhalb dieses Slices).
- Pure-Ausdruck-Neuberechnung im Codegen (`HcCodegen.cpp:1698` "re-emitted at every read"): Code-Größe wächst bei Fan-out potenziell exponentiell mit der Tiefe. Das ist durch die Parität zum Interpreter gebunden und nicht ohne Parity-Tests änderbar. Messung: Größe generierter Dateien bei typischen Graphen.

## Gelesen

Ja, alle 56 Dateien des Slices wurden vollständig gelesen, darunter `HcCodegen.cpp` (1-4917) und `GameReplication.cpp` (1-981). Ergänzend wurden nur zur Verifikation `PoseFinalize.cpp:430-460`, `TransformComponent.h` (Feldzeilen) und die Aufrufstellen von `AudioSystem::updateSpatial` gelesen.
