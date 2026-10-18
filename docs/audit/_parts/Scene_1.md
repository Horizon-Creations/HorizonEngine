# Scene Slice 1 – Animation*, AnimatorHost, AntiCheat/*, AudioEngine, CameraRigController, CameraShake, Cell*, TransformComponent

## Zusammenfassung

Der Code ist insgesamt sauber, gut kommentiert und an den Stellen, die jemand schon gemessen hat, bereits optimiert (`worldMatrixOf` mit Stack-Chain, inkrementelles `propagateTransforms`, EQ/Envelope-Handover ohne Lock auf dem Mixer-Thread). Die größten offenen Hebel liegen im Animations-Pfad: Jede spielende Animation wird jedes Frame vollständig gesampelt und gepost (kein Sichtbarkeits-/Distanz-/Dirty-Gate), mit mehreren `std::vector`-Allokationen pro Entity und Frame, linearer Keyframe-Suche und einer 3-Matrix-Zerlegung pro Gelenk. Zweiter Block: `AudioEngine::startSound` kopiert die komplette Clip-Datei pro Stimme, und fertige Stimmen werden von der Engine nie selbst abgeräumt. Dritter Block: `CameraRigController::update` und `CellStreamer::update` laufen jedes Frame mit mehreren O(N)-Scans bzw. Allokationen ohne Early-out. AntiCheat und CellSplit sind überwiegend kalter Code (nur Kleinkram).
Funde: Hoch 4 (SCN-101, 102, 103, 112), Mittel 10 (SCN-104 bis 111, 113, 114), Niedrig 7 (SCN-115 bis 121), insgesamt 21. Die IDs folgen der Reihenfolge Animation, Audio, Kamera, Cell; Schweregrad steht je Fund. Alles ohne Messdaten; Schätzungen sind als solche gekennzeichnet.

---

### SCN-101 – Pro Entity und Frame mehrere Heap-Vektoren in allen drei Pose-Treibern
- **ID**: SCN-101
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationSystem.cpp:71`; `src/HE_Scene/src/AnimationBlendSystem.cpp:114,118,153`; `src/HE_Scene/src/AnimationStateMachineSystem.cpp:428,435,454`; `src/HE_Scene/src/AnimationPose.cpp:369` (blendPosesN)
- **Problem**: In der Schleife über alle Animator-Entities wird pro Entity und Frame mindestens ein `std::vector<JointTRS>` in Skelettgröße neu angelegt (Clip: 1, Blend: 3, StateMachine: 2–3 plus `final_trs`). Jedes davon ist ein malloc/free-Paar mit ~`joints*40 Byte` (JointTRS = vec3+quat+vec3). Bei 100 animierten Charakteren mit 60 Gelenken sind das 100–300 Allokationen je Frame allein hier, dazu kommen die Vektoren in `poseFinalize` (nicht Teil dieses Slice). Die Kapazität könnte zwischen Entities und Frames wiederverwendet werden, da alle Vektoren nur innerhalb der Iteration leben.
- **Code-Auszug**:
```cpp
// AnimationSystem.cpp:71
std::vector<JointTRS> localTRS(mesh->skeleton.size());
// AnimationBlendSystem.cpp:114-118,153
std::vector<JointTRS> trsA(jointCount);
std::vector<JointTRS> trsB(jointCount);
std::vector<JointTRS> blended;
// AnimationStateMachineSystem.cpp:428,435,454
std::vector<JointTRS> trsOut(jointCount);
std::vector<JointTRS> final_trs;
std::vector<JointTRS> trsIn(jointCount);
```
- **Lösung**: Ein kleiner Scratch-Struct (`struct PoseScratch { std::vector<JointTRS> a, b, out; }`) als Member des jeweiligen Systems (Systeme sind bereits Klassen mit `update(...)`) bzw. `thread_local`/Parameter, vor jeder Entity per `assign(jointCount, JointTRS{})` zurücksetzen (behält Kapazität). In `blendTRS` darf `out` auf `a` aliasen (siehe SCN-109), damit `blended` entfällt (`blendTRS(trsA, trsB, alpha, trsA)`; `overrideJoint` liest `a` vollständig bevor `out` geschrieben wird, nur in den Randfällen `out = a`/`out = b` ist Self-Assign bzw. Kopie unkritisch). `final_trs = std::move(trsOut)` bleibt möglich, wenn `final_trs` ebenfalls Scratch ist (swap statt move).
- **Erwarteter Effekt**: Schätzung: 3–5 malloc/free je Entity/Frame entfallen; ein Teil der Frame-Zeit der Animationsphase (messen, vermutlich einstellige Prozent der Animationsphase bei vielen Charakteren).
- **Risiko / Verifikation**: Gering. Ergebnis bit-identisch (reine Pufferwiederverwendung). Voraussetzung: Systeme laufen nicht parallel auf mehreren Threads (sonst `thread_local`). Tests: bestehende Animation-Tests (`tests/` Animation/Blend/StateMachine), plus ein Test, der zwei Entities mit unterschiedlicher Skelettgröße hintereinander poset (Reset des Scratch).

---

### SCN-102 – Kein Gate für unsichtbare / ferne / unveränderte Animatoren
- **ID**: SCN-102
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationSystem.cpp:23-28`; `src/HE_Scene/src/AnimationBlendSystem.cpp:36-39`; `src/HE_Scene/src/AnimationStateMachineSystem.cpp:294-301` (State Machine hat nicht einmal ein `playing`-Flag)
- **Problem**: Die einzigen Filter in der View-Schleife sind `playing` und `sequencePosed`. Jeder spielende Animator wird jedes Frame mit der vollen Kette Sample → Root Motion → Layer → FK → IBM bearbeitet, unabhängig davon, ob die Entity im Frustum ist, wie weit sie von der Kamera entfernt ist (kein Update-Rate-Gate / Animation-LOD) oder ob sich die Pose gegenüber dem Vorframe geändert hat (z. B. pausierte StateMachine mit `playbackSpeed == 0`, Clip mit Dauer-Pose). Das ist der klassische „ECS-View ohne Dirty-Flag“-Fall im größten per-Frame-System dieses Slice. Ob `poseFinalize`/der Renderer irgendwo Sichtbarkeit nutzt, ist in diesem Slice nicht erkennbar.
- **Code-Auszug**:
```cpp
// AnimationSystem.cpp:23-28
for (auto [e, animator, smc] : view.each())
{
    if (!animator.playing) continue;
    if (smc.sequencePosed) continue;
// AnimationStateMachineSystem.cpp:294-296
for (auto [e, sm, smc] : view.each())
{
    resolveConfigIfNeeded(sm, cm);
```
- **Lösung**: (1) Sichtbarkeits-/Distanzinformation, die der Renderer/LODSystem ohnehin hat, als Komponentenfeld (`smc.animLodLevel` o. ä.) bereitstellen; die Treiber überspringen das Posen für weit entfernte Entities oder aktualisieren nur jedes n-te Frame (Zeit akkumulieren, damit Root Motion/Notifies weiterhin die volle Spanne bekommen – diese Teile NICHT überspringen). (2) Bei `speed == 0` und unverändertem Zustand (kein Transition, keine Parameteränderung) Pose nicht neu berechnen und die letzten `boneMatrices` stehen lassen. (3) StateMachine: `playing`/`paused`-Flag analog zu den anderen.
- **Erwarteter Effekt**: Schätzung: potenziell größter Einzelgewinn der Animationsphase in großen Szenen (50 %+ bei überwiegend unsichtbaren Charakteren); ohne Messung nicht belegbar.
- **Risiko / Verifikation**: Mittel bis hoch – verändert beobachtbares Verhalten (Pose steht still, Notifies/Root Motion dürfen nicht ausfallen, Shadow-Caster außerhalb des Kamerafrustums). Absichern: Tests, dass Notifies und Root Motion bei übersprungenem Posen identisch bleiben; Messen mit Profiler-Szene (z. B. 500 Animatoren, 90 % außerhalb des Frustums).

---

### SCN-103 – Lineare Keyframe-Suche pro Kanal und Frame
- **ID**: SCN-103
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationEval.cpp:12-21` (findBracket), Aufruf `:38`
- **Problem**: `findBracket` läuft für jeden Kanal (typisch 3 pro Gelenk) in jedem Frame von Index 0 linear bis zum passenden Keyframe: O(Keys) statt O(log Keys). Bei Clips mit hunderten Keys je Kanal (Mocap, 30–60 Hz) ist das der dominante Teil von `sampleClip`; die Laufzeit hängt außerdem von der Abspielposition ab (gegen Ende des Clips am teuersten).
- **Code-Auszug**:
```cpp
static size_t findBracket(const std::vector<float>& times, float t)
{
    size_t k = 0;
    for (size_t i = 0; i + 1 < times.size(); ++i)
    {
        if (times[i + 1] >= t) { k = i; return k; }
        k = i + 1;
    }
    return k;
}
```
- **Lösung**: Semantisch identische Binärsuche (Ergebnis = Index des ersten `j >= 1` mit `times[j] >= t`, minus 1; sonst `n-1`; n==1 → 0):
```cpp
static size_t findBracket(const std::vector<float>& times, float t)
{
    if (times.size() < 2) return 0;
    const auto it = std::lower_bound(times.begin() + 1, times.end(), t);
    return static_cast<size_t>(it - times.begin()) - 1;
}
```
  Optional zusätzlich ein Cursor-Hint pro Kanal (vorheriger Index, da Playhead monoton läuft) – braucht aber veränderlichen Zustand in den Assets, daher nicht empfohlen.
- **Erwarteter Effekt**: Schätzung: O(log n) statt O(n); bei 100–300 Keys je Kanal Faktor 5–20 für diesen Teil von `sampleClip`.
- **Risiko / Verifikation**: Gering, solange `times` aufsteigend sortiert ist (die lineare Variante toleriert unsortierte Daten, die Binärsuche nicht; Importer/`.hasset`-Loader sollten das garantieren, ggf. beim Laden prüfen). Test: Property-Test, der alte und neue Funktion über zufällige sortierte Zeitreihen inkl. Duplikaten, n = 0/1/2 und t vor/hinter dem Bereich vergleicht; vorhandene Sampling-Tests laufen lassen.

---

### SCN-104 – `sampleJoint` scannt alle Kanäle des Clips, mehrfach pro Frame
- **ID**: SCN-104
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationEval.cpp:100-107`; Aufrufer `src/HE_Scene/src/RootMotion.cpp:44,45,89,157`
- **Problem**: Um EIN Gelenk (die Wurzel) zu samplen, wird die komplette Kanalliste (3 × Gelenkzahl) linear durchlaufen. Für Root Motion passiert das pro Frame und Entity mehrfach (Anfang, Ende der Spanne, Frame 0; beim Blend zusätzlich pro Clip), bei Blend/StateMachine mit zwei Clips noch häufiger. Der Wurzel-Kanal ist aber ein fester, pro Clip konstanter Satz von maximal drei Kanälen.
- **Code-Auszug**:
```cpp
JointTRS sampleJoint(const AnimationClipAsset& clip, uint32_t jointIndex, float t)
{
    JointTRS trs;
    for (const auto& channel : clip.channels)
        if (channel.jointIndex == jointIndex)
            applyChannel(channel, t, trs);
    return trs;
}
```
- **Lösung**: Beim Laden/Importieren der Clips die Kanäle nach `jointIndex` sortieren (stabil, damit die Reihenfolge je Gelenk erhalten bleibt) und `sampleJoint` per `std::lower_bound`/`equal_range` arbeiten lassen – oder alternativ einen kleinen `std::vector<uint32_t> firstChannelOfJoint` im `AnimationClipAsset` ablegen. Die Änderung sollte im Asset-Loader (HE_Core) passieren; `sampleClip` braucht keine Ordnung.
- **Erwarteter Effekt**: Schätzung: pro Aufruf von O(3·J) auf O(log) – bei 60 Gelenken ~180 Vergleiche gespart, mal 3–6 Aufrufe je Root-Motion-Entity und Frame.
- **Risiko / Verifikation**: Gering–Mittel (Asset-Layout/Serialisierung berühren; reine Umsortierung im Speicher ist ohne Formatänderung möglich, aber Reihenfolge bei mehrfachen Kanälen desselben Pfads beachten: späterer überschreibt früheren). Test: `sampleJoint` vs. `sampleClip[joint]` auf Test-Clips.

---

### SCN-105 – Blend-System bewertet immer beide Clips, auch bei `blendAlpha` 0 oder 1
- **ID**: SCN-105
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationBlendSystem.cpp:113-120,130-151`; `src/HE_Scene/src/AnimationPose.cpp:13-20` (overrideJoint liefert an den Enden exakt `a` bzw. `b`)
- **Problem**: `trsA` und `trsB` werden jedes Frame vollständig gesampelt (inklusive Allokation, siehe SCN-101), obwohl `overrideJoint` bei `alpha <= 0` bzw. `>= 1` den jeweils anderen Clip ignoriert (`out = a` / `out = b`). Bei den üblichen Fällen „Blend steht auf einem Ende“ wird die Hälfte der Samplingarbeit verworfen.
- **Code-Auszug**:
```cpp
std::vector<JointTRS> trsA(jointCount);
if (clipA && clipA->duration > 0.0f)
    sampleClip(*clipA, std::fmod(t, clipA->duration), trsA);
std::vector<JointTRS> trsB(jointCount);
if (clipB && clipB->duration > 0.0f)
    sampleClip(*clipB, std::fmod(t, clipB->duration), trsB);
```
- **Lösung**: Nur die Seite samplen, die zum Ergebnis beiträgt (`alpha <= 0` → nur A; `alpha >= 1` → nur B). Root Motion und Notifies bleiben davon getrennt, weil `rootMotionSampleClip` die `trs*`-Pose für `lockRootJoint` benutzt: Dort die ungesampelte Seite lazy oder nur für die Wurzel (`sampleJoint`) bereitstellen, oder die Optimierung nur anwenden, wenn die Entity keine `RootMotionComponent` hat.
- **Erwarteter Effekt**: Schätzung: bis zu −50 % Sampling im Blend-System an den Endpunkten.
- **Risiko / Verifikation**: Mittel – `blendRootMotion(dA, dB, alpha)` und die Notify-Bookkeeping-Zustände (`notifiesPrimedA/B`) müssen identisch bleiben. Tests: Blend-Test mit alpha 0 / 1 / 0.5 inkl. Root Motion und Notifies, Ergebnis bit-vergleichen.

---

### SCN-106 – State Machine: String-Vergleiche und String-Map-Lookups pro Entity und Frame
- **ID**: SCN-106
- **Kategorie**: B
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/AnimationStateMachineSystem.cpp:264-269` (findState), `:271-282` (evalTransition), `:313`, `:376-379`, `:442`
- **Problem**: Der aktuelle Zustand ist ein `std::string` (`currentStateName`). Jedes Frame wird per linearem Namensvergleich über alle States gesucht (`findState`), danach über *alle* Transitions des Graphen iteriert und `t.fromState != sm.currentStateName` als String verglichen; für jede passende Transition folgt ein Lookup `sm.params.find(t.paramName)` in einer String-Map. Bei großen Graphen (viele Zustände/Transitions) und vielen NPCs ist das reine String-Arbeit ohne Informationsgewinn, da sich der Zustand nur bei einer Transition ändert.
- **Code-Auszug**:
```cpp
for (const auto& s : sm.resolvedGraph.states)
    if (s.name == name) return &s;
...
for (const auto& t : sm.resolvedGraph.transitions)
{
    if (t.fromState != sm.currentStateName) continue;
    if (!evalTransition(sm, t)) continue;
```
- **Lösung**: Beim Auflösen des Graphen (`resolveConfigIfNeeded`) Indizes bauen: `stateIndexByName`, pro State eine Liste von Transition-Indizes mit bereits aufgelöstem Param-Slot (`std::vector<float>` params + Name→Slot nur beim Auflösen und für Script-Zugriff). `currentStateName` bleibt als serialisierte Wahrheit, dazu ein gecachter `currentStateIdx` (Invalidierung bei `configDirty`, Transition-Ende und Name-Setzen durch Skripte). Vereinfachung: die Zustandsmaschine hört auf, Namen zu vergleichen.
- **Erwarteter Effekt**: Schätzung: O(States + Transitions) String-Compares/Frame/Entity → O(Transitions des aktuellen States) Float-Vergleiche. Klein pro Entity, aber linear in der Zahl der NPCs.
- **Risiko / Verifikation**: Mittel (Script-API `animator.setParam`, `sm.currentStateName` wird extern geschrieben, Serialisierung). Tests: bestehende StateMachine-Tests; neuer Test „Skript setzt `currentStateName` → nächster Frame verwendet neuen State“.

---

### SCN-107 – `AnimatorHost::bind()` läuft pro Entity und Frame, danach String-Zugriffe für Parameter
- **ID**: SCN-107
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationStateMachineSystem.cpp:307-311`; `src/HE_Scene/src/AnimatorHost.cpp:15-24,41-55,108-128`
- **Problem**: Das System ruft bei laufendem Host für jede Entity jedes Frame `sync->bind(e)` auf, dessen Idempotenz-Prüfung `assetOf` (Registry-Lookups + `cm.getAnimatorStateMachine(id)` Content-Lookup), `m_assetOf.find` und `m_byEntity.find` ausführt, bevor es zurückkehrt – das sind 3–4 Hash-Lookups pro Entity und Frame für Entities, die längst gebunden sind. In `fireUpdate` kommen zwei weitere Lookups hinzu, und pro Float-Variable des Graphen ein `getVariable(inst, name)` (Namensauflösung per String) plus `sm->params[name] = …` (String-Hash/Map-Einfügen).
- **Code-Auszug**:
```cpp
// AnimationStateMachineSystem.cpp:307-311
if (sync && sync->running())
{
    sync->bind(e);
    sync->fireUpdate(e, dt);
}
// AnimatorHost.cpp:126-127
for (const std::string& name : pit->second)
    sm->params[name] = m_runtime->getVariable(it->second, name).f;
```
- **Lösung**: `bind` nur bei Änderung aufrufen: der Host lauscht auf `on_construct<AnimatorStateMachineComponent>` bzw. das System ruft `bind` nur, wenn `sm.configDirty` oder `sm.stateMachineAssetId != resolvedFromAssetId` (beides existiert bereits in `resolveConfigIfNeeded`). Parameter: Namen→Variablen-Slot einmal in `bind` auflösen (`std::vector<std::pair<VarSlot, float*>>`).
- **Erwarteter Effekt**: Schätzung: ~4 Hash-Lookups je Entity/Frame entfallen; Parameterweitergabe von String-Hash auf Direktzugriff.
- **Risiko / Verifikation**: Mittel – das Verhalten „Entity bekommt mitten in der Session eine State Machine / tauscht das Asset und wird erfasst“ (Kommentar Z. 303-306) muss erhalten bleiben; Test: Asset-Wechsel zur Laufzeit, Entity nachträglich mit Komponente ausstatten.

---

### SCN-108 – IK/LookAt: pro `rotateJointModel` eine Heap-Allokation und ein Sweep über das Restskelett
- **ID**: SCN-108
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationPose.cpp:86-112` (refreshModelSubtree, Allokation `:98`); Aufrufer `src/HE_Scene/src/AnimationIk.cpp:60-70,160,161,168,178,219,295,314`
- **Problem**: `refreshModelSubtree` legt bei jedem Aufruf `std::vector<char> dirty(jointCount, 0)` an und sweept von `root` bis zum Ende des Skeletts. Ein Zwei-Knochen-IK-Solve ruft `rotateJointModel` dreimal (+ evtl. ein viertes Refresh bei `weight < 1`), ein LookAt mit Kette ein Mal pro Glied, ein Fußausrichten einmal. Pro Bein und Frame also 3–4 Allokationen + 3–4 Skelettsweeps, obwohl nur wenige Gelenke unter `root` liegen.
- **Code-Auszug**:
```cpp
std::vector<char> dirty(jointCount, 0);
dirty[static_cast<size_t>(root)] = 1;
for (size_t i = static_cast<size_t>(root); i < jointCount; ++i)
{
    const int32_t parent = mesh.skeleton[i].parent;
    if (!dirty[i]) { if (parent >= 0 && dirty[static_cast<size_t>(parent)]) dirty[i] = 1; else continue; }
```
- **Lösung**: (a) Kleinstpuffer ohne Heap: bei `jointCount <= 256` ein `std::array<uint8_t,256>`/`std::bitset` auf dem Stack, sonst Vektor; (b) Alternative ohne Zusatzspeicher: da Eltern vor Kindern liegen, genügt es, für `i > root` zu prüfen, ob `parent >= root && model_updated(parent)` – dazu reicht ein Bitset mit `jointCount - root` Bits; (c) langfristig: Subtree-Ende pro Gelenk im `SkeletalMeshAsset` vorberechnen (bei Depth-First-Ordnung ist der Subtree zusammenhängend; bei beliebiger Reihenfolge nicht – daher erst prüfen).
- **Erwarteter Effekt**: Schätzung: 3–4 mallocs je IK-Kette und Frame entfallen; Sweep-Länge bei kleinen Subtrees sinkt (nur wenn (c)).
- **Risiko / Verifikation**: Gering (a/b). Ergebnis muss bit-identisch bleiben (Kommentar Z. 44-46: „refresh must not drift“). Bestehende IK-Tests verwenden; Test mit Skelett > 256 Gelenken für den Fallback.

---

### SCN-109 – `localMatrix`: drei 4×4-Matrizen und zwei volle Matrixprodukte pro Gelenk
- **ID**: SCN-109
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationPose.cpp:47-53`, genutzt in `composeModelMatrices` `:56-69` und `refreshModelSubtree` `:108-110`
- **Problem**: Jedes Gelenk in jedem Frame baut `T`, `R`, `S` als volle `mat4` und multipliziert sie mit `T * R * S` (2 × 64 Multiplikationen), obwohl die Struktur bekannt ist (Spalten von R skaliert, Translation in Spalte 3). Das ist die innere Schleife der gesamten Skelettauswertung für jede animierte Entity.
- **Code-Auszug**:
```cpp
inline glm::mat4 localMatrix(const JointTRS& trs)
{
    const glm::mat4 T = glm::translate(glm::mat4(1.0f), trs.translation);
    const glm::mat4 R = glm::mat4_cast(trs.rotation);
    const glm::mat4 S = glm::scale(glm::mat4(1.0f), trs.scale);
    return T * R * S;
}
```
- **Lösung**: `glm::mat4 m = glm::mat4_cast(rotation); m[0]*=s.x; m[1]*=s.y; m[2]*=s.z; m[3]=vec4(t,1);` – mathematisch identisch, ca. ein Zehntel der Operationen. Für die `TransformComponent` gibt es in `TransformHierarchy.cpp:309-314` dieselbe Dreier-Zerlegung (dort Editor-/Szenen-Hierarchie, nicht Teil dieses Slice).
- **Erwarteter Effekt**: Schätzung: FK-Phase pro Entity 2–4× schneller; absolute Wirkung nur messbar bei vielen Gelenken × vielen Entities.
- **Risiko / Verifikation**: Mittel – Die Ergebnisse sind nur bis auf Rundungsfehler (nicht bit-identisch) gleich, und der Kommentar in `AnimationEval.cpp:122-126` verlangt Bit-Gleichheit zwischen IK- und Nicht-IK-Pfad (beide nutzen aber dieselbe `localMatrix`, bleibt also erfüllt). Goldene Tests mit exakten Zahlenwerten (Skinning-Snapshots) müssen neu bewertet werden; Toleranz-Tests laufen lassen.

---

### SCN-110 – Blend-Space-Gewichte: Sortieren + Heap bei jedem Frame; `blendPosesN` kopiert Posen
- **ID**: SCN-110
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationPose.cpp:230-246` (capAndNormalise), `:249-274` (weights1D, Allokation `:252`), `:339-346` (blendSpaceEvalOrder), `:348-374` (blendPosesN, Allokationen `:352,369`); Aufrufer `PoseSource.cpp:64,154`
- **Problem**: Pro Blend-Space-State und Frame: `weights1D` allokiert `byX` (n Indizes) und sortiert sie stabil – obwohl die Sample-Reihenfolge nach `x` konstant ist; `capAndNormalise` allokiert `order` und sortiert erneut; `blendSpaceEvalOrder` sortiert ein drittes Mal; `blendPosesN` kopiert die erste Pose (`out = poses[order[0]]`) und legt pro weiterem Sample einen neuen `tmp`-Vektor an (`blendTRS(out, poses[i], …, tmp); out = std::move(tmp)`).
- **Code-Auszug**:
```cpp
std::vector<size_t> byX(n);
for (size_t i = 0; i < n; ++i) byX[i] = i;
std::stable_sort(byX.begin(), byX.end(), [&](size_t a, size_t b) { return space.samples[a].x < space.samples[b].x; });
...
std::vector<JointTRS> tmp;
blendTRS(out, poses[i], wi / denom, tmp);
out  = std::move(tmp);
```
- **Lösung**: (1) Sortierung nach `x` einmal beim Parsen des BlendSpace (`blendSpaceFromJson`) in einem Member `byX` ablegen. (2) `blendPosesN`: in-place `blendTRS(out, poses[i], t, out)` (Aliasing ist elementweise sicher; `out.resize(count)` ändert die Größe nicht, wenn gleich lang), so entfällt `tmp`. (3) `order`/`byX` auf kleine feste Größe (`kBlendSpaceMaxActiveSamples`) auf dem Stack begrenzen.
- **Erwarteter Effekt**: Schätzung: 3–6 Allokationen und 3 Sorts je Blend-Space-Entity/Frame entfallen; Blend-Space-Sampling selbst (n Clips) dominiert weiterhin.
- **Risiko / Verifikation**: Gering. Ergebnis identisch (stabile Reihenfolge beibehalten, Tie-Break nach Index). Tests: bestehende Blend-Space-Tests, plus Gleichstand-`x`-Fall.

---

### SCN-111 – `AudioEngine::startSound` kopiert die komplette Clip-Datei pro Stimme
- **ID**: SCN-111
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AudioEngine.cpp:722-723` (Kopie), `:821` (Envelope-Kopie), `:823`/`:967-978`/`:992-1003` (EQ/Envelope-Kopien); Begründung im Header `src/HE_Scene/include/HorizonScene/AudioEngine.h:108-113`
- **Problem**: Jede `play()`-Variante legt eine eigene `std::vector<uint8_t>`-Kopie der Audiodaten an (`snd->bytes = bytes;`). Für PCM16 ist das die ganze Clip-Länge (Header: „multi-megabyte PCM copy per press“), für Vorbis die komprimierte Datei. Bei gestapelten Einmal-Sounds (Schritte, Schüsse, Regen aus `WeatherAudio`) wird bei jedem Start malloc + memcpy mehrerer KB–MB auf dem Game-Thread ausgeführt und pro aktiver Stimme dauerhaft Speicher gehalten. Dasselbe gilt für die Envelope-Kopie (`make_shared<const AudioEnvelope>(*envelope)`).
- **Code-Auszug**:
```cpp
auto snd = std::make_unique<ActiveSound>();
snd->bytes = bytes;
```
- **Lösung**: Audiodaten im `ContentManager` als unveränderlichen, referenzgezählten Blob (`std::shared_ptr<const std::vector<uint8_t>>`) halten; `ActiveSound` hält nur die `shared_ptr` (Lebensdauer geschützt gegen Umlagerung, das ist der im Header genannte Grund für die Kopie). `ma_audio_buffer`/`ma_decoder` lesen direkt daraus (`ma_audio_buffer_config.pData` ist bereits ein Zeiger ohne Kopie, `ma_decoder_init_memory` ebenfalls). Envelope/EQ analog über `shared_ptr` aus dem Asset teilen statt zu kopieren.
- **Erwarteter Effekt**: Schätzung: Spawn-Kosten pro Sound von O(Clipgröße) auf O(1); Speicher pro Stimme von Clipgröße auf ~0 für PCM-Voices. Real nur bei kurzen, oft gestarteten Clips relevant (messen).
- **Risiko / Verifikation**: Mittel – ändert die Asset-API von `ContentManager` (anderes Modul) und alle `play()`-Overloads. Absichern: Test, der ein Asset während einer laufenden Stimme neu lädt/entlädt (kein Dangling), ASAN-Lauf.

---

### SCN-112 – Beendete Stimmen werden nie von der Engine abgeräumt (Script-One-Shots bleiben liegen)
- **ID**: SCN-112
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/AudioEngine.cpp:1011-1024` (einziges Entfernen: `stop`/`stopAll`), `:905-908` (nur Warnung); `src/HE_Scene/include/HorizonScene/AudioEngine.h:63-66`; Aufrufer `src/HE_Scene/src/EngineApi.cpp:2675,2682,2685`
- **Problem**: `m_impl->sounds` wird ausschließlich über `stop()`, `stopAll()` und `removeBus()` verkleinert. Der Header sagt es offen: „Finished voices are still counted until something stops or reaps them“. Reaping gibt es nur bei `SequenceSystem.cpp:593-595` und `WeatherAudio.cpp:241,319`. Die Script-API `audio.play(path)` / `audio.playAt(...)` (EngineApi.cpp:2675/2682) liefert ein Handle, aber ein Skript-Einmalsound (Schuss, Treffer, UI-Klick) wird typischerweise nie gestoppt. Jede solche beendete Stimme hält weiter `ma_sound` + `ma_audio_buffer`/Decoder + die Datenkopie (SCN-111) und verbleibt im `unordered_map` (und in `busVoiceCount`). Das erreicht irgendwann `kVoiceWarnThreshold` (128) – die Warnung beschreibt genau diesen Zustand, behebt ihn aber nicht. Ob andere Teile des Spiels (z. B. ein AudioSource-System) reapen, ist in diesem Slice nicht sichtbar; im gelesenen Code gibt es keinen periodischen Aufruf.
- **Code-Auszug**:
```cpp
void AudioEngine::stop(uint64_t handle) { ... m_impl->sounds.erase(it); }
...
if (m_impl->sounds.size() >= kVoiceWarnThreshold)
    HE_LOG_THROTTLE(Audio, Warning, 10.0, "%zu simultaneous voices are alive — sounds are being started "
                    "faster than they are stopped", m_impl->sounds.size());
```
- **Lösung**: Eine `AudioEngine::reapFinished()` (pro Frame oder alle ~0,25 s vom Game-Loop gerufen), die über `sounds` iteriert und Stimmen entfernt, die `!paused && ma_sound_at_end()` sind und nicht loopen; zusätzlich einen `fireAndForget`-Parameter bzw. `playOneShot`, der die Stimme beim Ende automatisch freigibt. Handle bleibt gültig für Stimmen, die noch laufen.
- **Erwarteter Effekt**: Verhindert unbegrenztes Wachstum von Speicher und Map in langen Sessions; Schätzung: bei vielen Einmal-Sounds der wichtigste Audio-Befund.
- **Risiko / Verifikation**: Mittel – nach dem Reapen liefert `isPlaying(handle)` weiter `false` (bleibt gleich), aber `getSoundBus`/`getSoundCursorFrames` auf alte Handles ändern ihr Ergebnis. Test: Sound kurzer Clip starten, Mixer ziehen (`readMixedFrames`), `reapFinished()`, Map-Größe und Handle-Abfragen prüfen; Pausierte Stimme darf NICHT entfernt werden (Kommentar `AudioEngine.h:228-234`).

---

### SCN-113 – `CameraRigController::update` propagiert die Transform-Hierarchie zweimal pro Frame
- **ID**: SCN-113
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/CameraRigController.cpp:410` und `:581` (plus `:645` in `blendTo`)
- **Problem**: Der Kommentar nennt es selbst: „the extractor propagating again later costs a scan of the transforms“. `propagateTransforms` ist inkrementell, scannt aber den gesamten `TransformComponent`-Storage (`TransformHierarchy.cpp:415-440`, `scanned`-Pfad), um schmutzige Matrizen zu finden – bei N Transforms also zwei O(N)-Scans je Frame allein für die Kamera, zusätzlich zu dem des Extraktors. Nur die Kamera-Entity (und ggf. das Ziel) hat sich geändert.
- **Code-Auszug**:
```cpp
// :410
HE::propagateTransforms(world);
...
// :581 (nach dem Schreiben der Kamera-Transform)
HE::propagateTransforms(world);
```
- **Lösung**: Für den ersten Aufruf: nur nötig, wenn das Ziel (oder seine Vorfahren) im selben Frame geändert wurde – Dirty-Hinweis über `tt.dirty`/Strukturepoche prüfen; sonst entfällt er. Zweiter Aufruf: statt der globalen Propagation nur die Kamera-Entity und ihre Kinder aktualisieren (`worldMatrixOf` / ein exponiertes `propagateSubtree(world, e)`; `propagateFrom` existiert bereits intern, `TransformHierarchy.cpp:68`). Alternative: dem Aufrufer die Verantwortung geben (die Frame-Reihenfolge ist dokumentiert) und nur *einmal* propagieren.
- **Erwarteter Effekt**: Schätzung: −1 bis −2 Scans über alle Transforms pro Frame (Größenordnung 10–100 µs bei 10k–100k Transforms; messen).
- **Risiko / Verifikation**: Mittel – Konsumenten (`SceneSystems` LOD/Niederschlag) setzen frische Weltposition der Kamera voraus (Kommentar `:578-580`). Test: Kamera-Rig-Test, der `ct.worldMatrix` nach `update()` gegen `worldMatrixOf` vergleicht; Profiling mit `Transforms::propagate`-Scope.

---

### SCN-114 – `CellStreamer::update`: pro Frame Scans, Allokationen und vollständige Iteration über geladene Zellen
- **ID**: SCN-114
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/CellStreamer.cpp:316-323` (Anchor-Vektor), `:333`, `:341-354`, `:359-382`, `:437-470`, `:477`, `:576-582`, `:584-593`
- **Problem**: Selbst wenn sich kein Anchor über eine Zellgrenze bewegt hat und nichts in Arbeit ist, läuft jeden Frame: Allokation `std::vector<Anchor>{cam}` (`:322`), `reachesOf` (Vektor), Schritt 0 prüft `registry().valid()` für jede geladene und angefangene Zelle, Schritt 1 durchsucht (2r+1)² Zellquadrate (mit 4 Hash-Lookups je Quadrat: `m_index`, `m_loaded`, `m_pending`, `m_failed`) pro Anchor *und* Lookahead-Punkt, danach Sort + `unique` auf `wanted`; Schritt 3 iteriert alle Pending-Zellen, Schritt 5 alle geladenen Zellen und berechnet `nearer()`; am Ende werden die Stats über alle Pending-Zellen neu gezählt. Außerdem wird `SceneSerializer ser;` jedes Frame konstruiert (`:477`), auch wenn `ready` leer ist.
- **Code-Auszug**:
```cpp
update(world, std::vector<Anchor>{ cam }, budgetMs);
...
SceneSerializer ser;   // :477, ohne Prüfung auf ready.empty()
...
for (const auto& [k, root] : m_loaded)
{
    const CellManifest::Cell& cell = m_manifest.cells[m_index[k]];
    if (nearer(cell.x, cell.z) > m_manifest.unloadRadius) gone.push_back(k);
}
```
- **Lösung**: (1) `SceneSerializer` erst konstruieren, wenn `ready` nicht leer ist, bzw. als Member. (2) Early-out: Signatur der Anchors (Zellindex von `here` und `ahead` je Anchor + Radius-Scale) merken; solange unverändert und `m_pending` leer ist und seit dem letzten Lauf kein Entity zerstört wurde (`world.structureEpoch()`), Schritte 1 und 5 überspringen (nur nach Zellwechsel neu berechnen). (3) Scratch-Vektoren als Member (`m_wanted`, `m_ready`, `m_gone`). (4) Schritt 0 nur bei geänderter `structureEpoch`.
- **Erwarteter Effekt**: Schätzung: Pro Frame ~5 Allokationen und (2r+1)²·4 Hash-Lookups weniger; im Stillstand nahezu 0 Aufwand. Bei kleinem `loadRadius` (wenige Zellen) nur wenige µs – messen.
- **Risiko / Verifikation**: Mittel – Early-out darf Lade-/Entlade-Reaktion auf Radiusänderung durch Skripte (Manifest wird nur in `begin` gesetzt, daher ok) nicht verzögern; Zellen, die von anderen zerstört werden (Schritt 0-Kommentar), müssen weiter erkannt werden. Tests: bestehende Cell-Streamer-Tests (`tests/` mit `CellStreamer`), plus „Anchor steht still, Zelle extern zerstört → wird neu geladen“.

---

### SCN-115 – `CellStreamer`: unnötige Kopien und `operator[]` auf Maps
- **ID**: SCN-115
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/CellStreamer.cpp:387,487,501,579` (`m_index[k]`, `m_pending[k]`), `:418` (`std::string(bytes.begin(), bytes.end())`), `:563` (`p->created.insert`), `:564,571` (Hooks bekommen Vektoren)
- **Problem**: `m_index[k]`/`m_pending[k]` sind `operator[]` auf `unordered_map` – bei fehlendem Key wird still ein Default eingefügt (`m_index` → Slot 0 → falsche Zelle), `find`/`at` wäre korrekt und ausdrückt „muss existieren“. Bei Text-Cells wird die gesamte Datei nochmals als `std::string` kopiert (nur Worker, aber doppelter Spitzenspeicher), `p->created` sammelt alle Entities der Zelle zusätzlich zu den Slice-Listen.
- **Code-Auszug**:
```cpp
const CellManifest::Cell& cell = m_manifest.cells[m_index[k]];
const std::shared_ptr<Pending> p = m_pending[k];
json scene = text ? parseSceneText(std::string(bytes.begin(), bytes.end())) : parseSceneCbor(bytes);
```
- **Lösung**: `m_index.at(k)` bzw. gespeicherten Index in `wanted` mitführen, `parseSceneText(std::string_view)`-Overload (bzw. `std::string(std::move…)`-freie Variante) einführen, falls das Parsing-Interface das erlaubt.
- **Erwarteter Effekt**: Robustheit (+), geringer Speicher-Peak beim Laden; Laufzeit vernachlässigbar.
- **Risiko / Verifikation**: Gering. Tests der Stream-Zustandsmaschine.

---

### SCN-116 – AudioEngine: Mixer-Thread-Kleinigkeiten (`runEq`, doppelter try_lock, String-Bus pro Stimme)
- **ID**: SCN-116
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AudioEngine.cpp:97-113` (runEq), `:236-256` (envRead Handover), `:355` (`std::string busName`), `:921-925,930-943` (routeFor/routeAsset)
- **Problem**: (a) `runEq` entdenormalisiert nach jedem Aufruf *alle* `kMaxBands * channels` Zustände, auch die ungenutzten Bänder – bei kleinen Puffern (512-Frame-Chunks, `kChunkFrames`) ist das relativ viel; die Schleife kann auf `eq.bands.size()*ch` begrenzt werden. (b) `envRead` nimmt für Kurve und EQ denselben Mutex nacheinander per `try_lock` (Kommentar Z. 204 behauptet „its own pending slot“, geteilt wird aber `pendingMutex`) – ein einziger `try_lock` mit anschließendem Swap beider Slots genügt. (c) Jede Stimme speichert `std::string busName`; `routeFor` und `routeAsset` bauen pro Play-Aufruf Strings (`std::function`-Lambda in `resolveBus`) und `m_impl->buses.find(busName)` hasht den String noch einmal.
- **Code-Auszug**:
```cpp
for (HE::BiquadState& s : state)
{
    if (std::fabs(s.z1) < 1.0e-20) s.z1 = 0.0;
    if (std::fabs(s.z2) < 1.0e-20) s.z2 = 0.0;
}
```
- **Lösung**: (a) Schleife über `state.begin() + eq.bands.size()*ch`; (b) beide Pending-Flags unter einem `try_lock` abarbeiten; (c) Bus als `BusData*`/Id statt String in `ActiveSound` halten (Namensvergleich nur in `removeBus`).
- **Erwarteter Effekt**: Sehr klein; nur wenn EQ-Voices zahlreich sind. Nicht ohne Messung angehen.
- **Risiko / Verifikation**: Gering. Audio-Render-Tests (`readMixedFrames`) vor/nach vergleichen.

---

### SCN-117 – `AnimationEval`: dupliziertes Wrapping, `composeBoneMatrices` alloziert `model` pro Aufruf
- **ID**: SCN-117
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Scene/src/AnimationBlendSystem.cpp:20-26,116,120`; `src/HE_Scene/src/AnimationEval.cpp:86-88`; `src/HE_Scene/src/AnimationStateMachineSystem.cpp:364-365,512-513`; `src/HE_Scene/src/AnimationEval.cpp:118-130`
- **Problem**: Die Regel „wrap in [0,d) mit Korrektur für negative Reste“ existiert in vier Kopien (`wrapTime` im Blend-System, `advancePlayback`, zwei Stellen im StateMachine-System), und `blend.playbackTime` wird in `advancePlayback` gewrappt und danach noch einmal mit `std::fmod(t, clip->duration)` (Z. 116/120) pro Clip. `composeBoneMatrices` legt pro Aufruf einen `model`-Vektor an (nur Preview/Tests, nicht Hot Path).
- **Code-Auszug**:
```cpp
float w = std::fmod(t, duration);
if (w < 0.0f) w += duration;
```
- **Lösung**: Eine gemeinsame `wrapPlayhead(float t, float duration)` in `AnimationEval.h` (inline), überall verwenden; `composeBoneMatrices` mit optionalem `model`-Scratch-Parameter oder `thread_local`.
- **Erwarteter Effekt**: Weniger Code, kein Performanceeffekt.
- **Risiko / Verifikation**: Gering; Tests der Wrapping-Fälle (negative Speed, Dauer 0).

---

### SCN-118 – AntiCheat: Kleinkram (`check()` hasht pro Aufruf, lineare Report-Suche, `m_buffer` Erase vorn)
- **ID**: SCN-118
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AntiCheat/AntiCheatHost.cpp:296-310` (check), `:370-377` (findReport); `src/HE_Scene/src/AntiCheat/AntiCheatService.cpp:541-548` (findReport), `:160,252,261,269` (Observation.rule); `src/HE_Scene/src/AntiCheat/AntiCheatTelemetry.cpp:122-129` (Erase vorn); `src/HE_Scene/include/HorizonScene/AntiCheat/AntiCheatTelemetry.h:130`
- **Problem**: (a) `AntiCheatHost::check()` ist laut Kommentar ein per-Hit-Aufruf, hasht aber bei jedem Aufruf `rule` in `m_checkLogged.insert(rule)`, nur um einmal ein Debug-Log zu schreiben; (b) `findReport` ist eine lineare Suche über eine nach `id` aufsteigend sortierte `std::deque` (id wird mit `++m_nextReportId` vergeben) und wird im `pump()` je Report sowie von Script-Readern (`reportRule/Detail/...`) wiederholt aufgerufen; (c) `Observation.rule` wird mit `kindName(kind)` (`const char*`) zu `std::string` kopiert – bei „IntegrityMismatch“ (17 Zeichen) Heap-Allokation; (d) `m_buffer.erase(m_buffer.begin(), …)` auf `std::vector<std::string>` verschiebt alle Strings.
- **Code-Auszug**:
```cpp
if (m_checkLogged.insert(rule).second)
    HE_LOG_DEBUG(AntiCheat, "check '%s': no rule table in this build, passes", rule.c_str());
...
for (const Report& r : m_reports) { if (r.id == id) return &r; }
```
- **Lösung**: (a) Den Insert nur ausführen, wenn Debug-Logging aktiv ist (`HE_LOG_ENABLED(AntiCheat, Debug)`) oder `return true` vor dem Insert bei leerer Regeltabelle; (b) `std::lower_bound` auf `m_reports` nach `id`; (c) `std::string_view`/`const char*` in `Observation` oder `kindName` statisch halten; (d) `std::deque<std::string>` für `m_buffer`.
- **Erwarteter Effekt**: Vernachlässigbar bis klein (kalter Code, `kMaxReports` begrenzt). Nur mitnehmen, wenn die Dateien ohnehin angefasst werden.
- **Risiko / Verifikation**: Gering.

---

### SCN-119 – `CellSplit`: Entity-IDs als `json::dump()`-Strings; Fixpunkt-Schleife quadratisch
- **ID**: SCN-119
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Scene/src/CellSplit.cpp:100` (idKey), `:313-322,371,417,425,504,517,523-529,546,563`, Fixpunkt `:511-532`, `std::function`-Rekursion `:332,364,393,489,570`
- **Problem**: Jede ID-Suche erzeugt per `v.dump()` einen neuen `std::string` und hasht ihn (mehrere hunderttausend Mal bei großen Szenen); die Schleife `for (bool changed = true; changed;)` geht in jeder Runde über alle Entities mit `idKey(...)` und mehr (`remove_if` mit `idKey(c)` je Kind) – O(Entities × Tiefe der Ordnerkette). `splitSceneIntoCells` ist eine Editor-/Offline-Operation (nicht pro Frame), daher niedrige Schwere. Außerdem wird `std::function` verwendet, ohne `<functional>` einzubinden (kommt transitiv).
- **Code-Auszug**:
```cpp
std::string idKey(const json& v) { return v.dump(); }
...
for (bool changed = true; changed;)
{
    changed = false;
    for (const json& e : *ents) { ... const std::string k = idKey(e["uuid"]); ...
```
- **Lösung**: IDs einmalig in `IdPair` (existiert schon: `asIdPair`/`IdPairHash`, `CellSplit.cpp:178-194`) umsetzen und alle Maps (`byId`, `nodeOfEntity`, `gone`, `moved`, `childrenLeft`) auf `IdPair` umstellen; Fixpunkt durch Bottom-up-Zählen der verbleibenden Kinder ersetzen (ein Durchlauf in umgekehrter Topologie). `#include <functional>` ergänzen.
- **Erwarteter Effekt**: Schätzung: Split großer Szenen (100k Entities) von Sekunden auf Bruchteile; kein Laufzeitgewinn im Spiel.
- **Risiko / Verifikation**: Mittel – Dateiformat und Reihenfolge der erzeugten Cell-Files müssen byte-identisch bleiben (Golden-Test: Split einer Beispielszene, Ausgabe gegen den bisherigen Stand diffen; Legacy-Zahl-IDs in `asIdPair` (`[hi, lo]` vs. „legacy number“, Z. 99) beachten).

---

### SCN-120 – `AnimationNotify`: Scan aller Notifies pro Segment und Entity
- **ID**: SCN-120
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationNotify.cpp:33-82` (emitSegment), `:92-143`
- **Problem**: Für jede spielende Entity mit Notifies läuft jedes Frame `emitSegment` über die komplette Notify-Liste des Clips (lineares Filtern mit `inSpan`), auch wenn die Zeitspanne (typisch 1/60 s) fast nie ein Ereignis enthält; bei Wrap/Lap-Spannen kommen weitere Segmente dazu. Das `pending`-Vektor-Heap-Objekt entsteht nur, wenn tatsächlich ein Ereignis trifft (erstes `push_back`), ist also kein Dauerbefund.
- **Code-Auszug**:
```cpp
std::vector<Pending> pending;
for (const AnimationNotify& n : notifies)
{
    if (n.duration > 0.0f) { ... }
    else if (inSpan(n.time)) pending.push_back({ n.time, Kind::Fire, &n.name });
}
```
- **Lösung**: Notifies beim Laden nach `time` sortieren (nur Reihenfolge der Ausgabe ist ohnehin durch `stable_sort` kanonisch) und per `lower_bound` auf das Intervall `[lo, hi]` einschränken; `Begin/End` für Zustände mit `duration > 0` bräuchten zusätzlich einen Sortierschlüssel für `time+duration` oder getrennte Liste.
- **Erwarteter Effekt**: Klein; Gewinn nur bei Clips mit vielen Notifies. Nicht ohne Messung umsetzen.
- **Risiko / Verifikation**: Mittel – Reihenfolge-/Randfallsemantik (`closedFrom`, Rewind) ist subtil; bestehende Notify-Tests (Wrap, Rewind, 64-Runden-Guard) müssen unverändert bestehen.

---

### SCN-121 – `resolveConfigIfNeeded` kopiert den geparsten Graphen
- **ID**: SCN-121
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Scene/src/AnimationStateMachineSystem.cpp:213-240`
- **Problem**: `graph = std::move(parsed)` (Z. 222) wird richtig bewegt, danach aber `sm.resolvedGraph = graph;` (Z. 240) als Kopie zugewiesen, und `graph` danach nie wieder gebraucht – ein kompletter Deep-Copy aller States/Transitions/Strings pro Auflösung. Läuft nur bei `configDirty`/Asset-Wechsel (selten), daher gering.
- **Code-Auszug**:
```cpp
sm.resolvedGraph       = graph;
```
- **Lösung**: `sm.resolvedGraph = std::move(graph);` (die Kopie ist durch das spätere Lesen von `sm.resolvedGraph` nicht nötig).
- **Erwarteter Effekt**: Einmalig pro Auflösung ein Deep-Copy weniger.
- **Risiko / Verifikation**: Keines; kompiliert unverändert, Tests laufen lassen.

---

## Cross-Modul-Hinweise

- `src/HE_Core/include/ContentManager/Assets.h:645-651`: `AnimationChannel` hält zwei `std::vector<float>` pro Kanal (3 Kanäle × Gelenkzahl × Clip → viele kleine Heap-Blöcke, Pointer-Chasing in `sampleClip`). Ein SoA-/Arena-Layout je Clip (ein `times`-/`values`-Block mit Offsets) würde Cache-Verhalten und Ladezeit verbessern; zusammen mit SCN-103/104 (sortierte Zeiten, Kanalindex pro Gelenk) im Asset-Loader lösbar.
- `src/HE_Scene/src/PoseSource.cpp:64,154` und `PoseFinalize.cpp:79,213-253`: rufen die hier gefundenen Blend-Space-/Namensauflösungsfunktionen. `resolveBoneMask` / `findJointByName` sind dort bereits gecacht (`lc.resolvedMasks`, `ic.resolvedChain`) – gut; `blendSpaceWeights`/`blendPosesN` sind es nicht (SCN-110). `PoseFinalize` selbst (Allokationen, `composeModelMatrices`-Scratch) gehört zu einem anderen Slice und wird vermutlich dieselben Vektor-Muster wie SCN-101 zeigen.
- `src/HE_Scene/src/EngineApi.cpp:2675,2682`: `audio.play`/`audio.playAt` liefern Handles, aber nichts reapt Einmal-Sounds (SCN-112).
- `src/HE_Scene/src/TransformHierarchy.cpp:309-314`: `localMatrix(TransformComponent)` benutzt dieselbe `translate*mat4_cast*scale`-Zerlegung wie SCN-109 (Pro-Entity-Kosten bei jeder schmutzigen Transform).
- `src/HE_Game/src/GameApplication.cpp:2171`: `cellHolds` wird dort als Lambda an `PhysicsWorld::setRegionHold` übergeben; wird pro Körper pro Physikschritt gerufen (nicht verifiziert) – dann sind die 3 Hash-Lookups in `CellStreamer::holdsAt` (`CellStreamer.cpp:277-286`) und die `dvec3`-Konversion in `CellPhysics.cpp:28-31` ein Hot-Path-Kandidat; in der Physik-Slice prüfen.

## Unsicher / Messung nötig

- SCN-102: Ob `poseFinalize`/Renderer Unsichtbares bereits ausspart (außerhalb dieses Slice), und wie viel Prozent der Frame-Zeit die Animationsphase in typischen Szenen überhaupt kostet.
- SCN-101/108/110: Tatsächlicher Anteil der malloc/free-Zeit gegenüber Sampling/FK; mit Profiler (`HE_PROFILE_SCOPE`) in einer Szene mit 100–500 Charakteren messen.
- SCN-103: Typische Keyframe-Anzahl je Kanal der Projekt-Clips (Mocap vs. handgemacht); Binärsuche lohnt erst ab ~16–32 Keys.
- SCN-112: Ob irgendwo außerhalb dieses Slice (AudioSource-System, Editor) beendete Stimmen abgeräumt werden; im gelesenen Code und per grep auf `isPlaying`/`stop` nur Sequence/Weather gefunden.
- SCN-113/114: Absolute Kosten von `propagateTransforms` im Leerlauf (`Transforms::propagate`-Profil) und von `CellStreamer::update` bei großem Manifest.
- `CellManifest::around`, `slicesEstimate` (`CellStreamer.cpp:156-157`): `1 + ceil(entities / k)` ergibt für `entities > k` immer eine Slice mehr als `ceil(entities / k)` – ob das beabsichtigt ist (z. B. Wurzel-Slice), ist ohne Blick in `sliceForAdditiveLoad` nicht klar; nur Schätzung für die UI.
- `AntiCheatTelemetry::~AntiCheatTelemetry` / `detach()` blockieren bis zum Timeout, falls ein `std::async`-Upload läuft (Future-Destruktor joint); da `kTimeoutMs` begrenzt, nur beim Beenden relevant – nicht gemessen.

## Gelesen

Ja, alle 20 Dateien des Slice vollständig gelesen (AnimationBlendSystem, AnimationEval.cpp/.h, AnimationIk, AnimationNotify, AnimationPose, AnimationPreview, AnimationStateMachineSystem, AnimationSystem, AnimatorHost, AntiCheat/{Host,Service,Telemetry}, AudioEngine (1250 Z.), CameraRigController, CameraShake, CellPhysics, CellSplit, CellStreamer, Components/TransformComponent.cpp); zusätzlich gezielt `TransformHierarchy.cpp:300-440`, `AudioEngine.h`-Kommentare und Aufrufer per grep geprüft.
