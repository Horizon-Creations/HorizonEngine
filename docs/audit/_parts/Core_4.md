# Core Slice 4 – HorizonCodeRuntime, GameServices, Hpak (Aes/Reader/Writer/Config/Exporter), IGameLogic, IntegrityProbe, JobSystem, AutoLandscapeMaterial

## Zusammenfassung

Der Slice ist insgesamt sorgfältig und gut kommentiert; die Parallelitätslogik im JobSystem (CAS-Claim, Cancel, Dependencies) ist korrekt durchdacht und enthält keinen offensichtlichen Fehler. Die relevanten Befunde liegen an drei Stellen. (1) Der HorizonCode-Interpreter baut pro Event und pro Funktionsaufruf einen kompletten `Context` (37 `std::function`-Member) und sucht den behandelnden Level linear durch alle Knoten. Das läuft für `Tick` jeder Widget-, Entity- und Player-Instanz in jedem Frame. (2) Der Pak-Lesepfad hasht jeden gelesenen Eintrag mit byteweisem FNV-1a. Gemessen sind das 664 MB/s und damit der langsamste Schritt, noch vor Entschlüsseln und LZ4. (3) Der Pak-Writer/Exporter arbeitet strikt sequenziell, hält das gesamte Projekt im RAM und kopiert jedes Asset mehrfach. Das ist Editor/Export-Zeit, kein Frame-Pfad. Dazu kommen Duplikate und Vereinfachungspotenzial (MTRL-Layout dreifach, Dispatch-Signaturprüfung doppelt, Öffentlich-Variable-Prüfung fünffach).

Funde: **3 Hoch, 12 Mittel, 11 Niedrig** (26 gesamt). Verifiziert wurde nur durch Lesen. Einzige Messung ist der FNV-Durchsatz (g++ -O2, 4 Kerne, 256 MiB: 664 MB/s). Alle anderen Aufwandszahlen sind Schätzungen und entsprechend markiert.

---

## Funde

### CORE-401 – Pro Event/Funktionsaufruf wird ein kompletter `Context` (37 `std::function`) neu gebaut
- **ID**: CORE-401
- **Kategorie**: B / C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:1338-1586` (`makeContext`), `:2082-2089` (`runEventOnLevel`), `:1322-1336` (`classKeyAtLevel`), `:1565`/`:1633`/`:1694`/`:2114` (Aufrufer)
- **Problem**: Jeder interpretierte Aufruf (`fireTick`, `fireOnClicked`, `callFunction`, `callOwn`, Delay-Resume, `resumeRun`) erzeugt über `makeContext` ein neues `Context` und übergibt es dem `Runner` per Wert (`HorizonCode.h:1463: Runner(const Graph&, Context)`). Der `Context` enthält rund 37 `std::function`-Member, jeweils mit eigener Lambda-Konstruktion. Drei davon (`scheduleResume`, `getNodeState`, `setNodeState`) erfassen `[this,id,level]` = 24 Byte und landen je nach STL im Heap (libstdc++: ja, libc++/MSVC: wahrscheinlich im SBO). Zusätzlich liefert `classKeyAtLevel` einen `std::string` per Wert (Asset-Pfad, meist länger als SSO), der in `ctx.traceKey` kopiert wird. Der Context ist dabei zustandslos: der Kommentar `:1467-1472` sagt ausdrücklich, dass Services zur Aufrufzeit durchgereicht und nie kopiert werden. Der kompilierte Pfad macht es bereits richtig und bindet seinen Context einmal (`:150`, `bindContext(makeContext(id))`). Aufrufer pro Frame: `WidgetManager.cpp:2769,2773`, `PlayerHost.cpp:324`, `EntityHost.cpp:263`.
- **Code-Auszug**:
```cpp
void Runtime::runEventOnLevel(Inst& i, InstanceId id, const std::string& event, int elem, const Value& arg)
{
    const int lv = levelHandlingEvent(i, event, elem);
    if (lv < 0) return;
    Runner r(i.levels[lv], makeContext(id, (size_t)lv));   // 37 std::function + string-Kopie, pro Event
    r.fireEvent(event, elem, arg);
}
```
- **Lösung**: (1) Pro `Inst` und Level einen lazy erzeugten, gecachten `Context` halten (`std::vector<std::unique_ptr<Context>> ctxByLevel` in `Inst`). (2) `Runner` auf `const Context&` bzw. `std::shared_ptr<const Context>` umstellen. Das ist eine API-Änderung in `HorizonCode.h`, außerhalb dieses Slice. (3) `traceKey` als `const std::string*` auf `inst.cls.key` bzw. `chain[i]` statt als Kopie führen. Alternativ, falls `Runner` nicht anfassbar ist, `makeContext` in zwei Teile trennen: einen statischen Teil, der einmal pro Instanz/Level gebaut wird, und einen dünnen per-Call-Teil.
- **Erwarteter Effekt** (Schätzung, messen): ca. 0,2-0,5 µs und 3 Heap-Allokationen weniger pro Event auf libstdc++. Bei einigen tausend tickenden Instanzen sind das 0,5-2 ms pro Frame auf dem Spielthread.
- **Risiko / Verifikation**: Rekursion. Ein gecachter Context wird von verschachtelten Runnern derselben Instanz gleichzeitig benutzt; er muss deshalb unveränderlich sein (alle Lambdas lesen nur `this`/`id`/`level`, Zustand liegt in `Runtime`). Beim Entfernen einer Instanz (`m_doomed`) muss der Cache mit der `Inst` leben (tut er, solange er im `Inst` hängt). Tests: `tests/test_horizoncode_runtime.cpp`, `test_hc_bind_to.cpp`, `test_hc_pull_on_construct.cpp`, Codegen-Parity-Harness. Zusätzlich Mikrobenchmark 1000 Instanzen × `fireTick`.

---

### CORE-402 – Lineare Knotensuche pro Event/Call (`levelHandlingEvent`, `levelWithFunction`, `functionSignatureOf`)
- **ID**: CORE-402
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.cpp:2054-2068` (`levelHandlingEvent`), `:2070-2080` (`levelWithFunction`), `:1548-1555` (`callOwn`), `:1263-1277` (`functionSignatureOf`), `:2112`
- **Problem**: Jeder `fireTick`/`fireOn*`-Aufruf und jeder Funktionsaufruf durchläuft alle Knoten aller Level des Graphen leaf-first und vergleicht `n.s == event` bzw. `n.s == fn` (String-Vergleich). Bei `Tick` auf Instanzen, die gar keinen Tick-Handler haben, ist das die gesamte Arbeit eines Frames, denn `runEventOnLevel` kehrt danach zurück. Zusätzlich wird für `InputAction`-Knoten `inputActionChainFor(n, event)` aufgerufen. `callOwn` scannt nach `levelWithFunction` ein zweites Mal alle Knoten des gefundenen Levels, um den `FunctionEntry` für Run-On zu finden. Der Graph ist nach der Registrierung unveränderlich (`levels` liegt by value in `Inst`, `Runner` nimmt `const Graph&`).
- **Code-Auszug**:
```cpp
for (size_t lv = i.levels.size(); lv-- > 0; )
    for (const Node& n : i.levels[lv].nodes)
    {
        if (n.type == NodeType::Event && n.s == event && (n.elem == 0 || n.elem == elem))
            return (int)lv;
        if (n.type == NodeType::InputAction && inputActionChainFor(n, event) >= 0)
            return (int)lv;
    }
```
- **Lösung**: Pro `Inst` (besser pro Klasse, wenn Graphen geteilt werden, siehe CORE-408) zwei Indizes lazy aufbauen: `unordered_map<EventId, small_vector<{level, elem}>>` (leaf-first sortiert, InputAction-Ereignisse über `inputActionEventNames` eingetragen) und `unordered_map<std::string, {level, const Node*, access}>` für Funktionen. `levelHandlingEvent` wird dann zu einem Lookup plus einem Filter über `elem`. Die Semantik "leaf-most gewinnt, bei `elem` Filter 0 oder gleich" bleibt erhalten. Die öffentlichen/privaten Funktionen getrennt führen (`publicOnly`).
- **Erwarteter Effekt** (Schätzung): O(Knoten·Level) → O(1) pro Event. Bei Graphen mit 200+ Knoten und Tick pro Frame mehrere µs pro Instanz gespart. Zusammen mit CORE-401 der größte Hebel am Interpreter-Hot-Path.
- **Risiko / Verifikation**: Reihenfolge-Semantik (Override, Elem-Filter, InputAction-Ketten) muss exakt bleiben. Ein Differenztest "Index vs. alte lineare Funktion" über alle Graphen aus `tests/test_horizoncode_runtime.cpp` und den Inheritance-Tests ist günstig und aussagekräftig.

---

### CORE-403 – Byteweiser FNV-1a als Content-Hash im Streaming-Lesepfad (664 MB/s)
- **ID**: CORE-403
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Hpak/HpakFormat.h:110-119` (`hash64`), `src/HE_Core/src/Hpak/HpakReader.cpp:235` (Prüfung bei jedem Lesen), `:103` (TOC); Schreibseite `HpakWriter.cpp:653,1083,1113,1154`
- **Problem**: `readStoredBytes` hasht jeden gelesenen Eintrag vollständig und single-threaded, bevor entschlüsselt oder dekomprimiert wird. FNV-1a hat eine Abhängigkeitskette aus XOR + Multiplikation pro Byte (ca. 4 Zyklen/Byte). Gemessen (g++ -O2, 256 MiB): **664 MB/s**. AES-GCM mit AES-NI und LZ4-Decode liegen typischerweise im Bereich mehrerer GB/s (Schätzung, nicht gemessen). Der Hash ist damit der langsamste Schritt je Streaming-Job auf einem Pool-Thread, und ein 500-MB-Eintrag blockiert einen Worker ca. 0,75 s nur für die Prüfsumme. Auf der Schreibseite läuft derselbe Hash dreimal pro Asset (`inHash`, `srcHash`, `contentHash`) plus für Parent-Materialien.
- **Code-Auszug**:
```cpp
inline uint64_t hash64(const uint8_t* data, size_t len) noexcept {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) { h ^= data[i]; h *= 1099511628211ULL; }
    return h;
}
```
- **Lösung**: Option A (format-erhaltend, kleiner Gewinn): Für verschlüsselte Einträge die Prüfung nach dem GCM-Tag überspringen; der Tag authentifiziert bereits, und `contentHash` bleibt im Format. Option B (voller Gewinn): Wortweiser/mehrspuriger Hash (z. B. XXH3-64 oder 4 parallele FNV-Lanes über 8-Byte-Wörter, 5-10 GB/s). Das ändert den gespeicherten Wert und braucht deshalb `k_version = 3` mit Reader für v2 und v3 (v2-Hash weiter byteweise). `kPackPipelineVersion` (`ProjectExporter.cpp:316`) anheben, damit alte Manifeste ungültig werden. Option C: blockweises Hashen in den bereits vorhandenen 4-MiB-Leseblöcken (`HpakReader.cpp:203-224`), solange die Daten im Cache liegen.
- **Erwarteter Effekt** (Schätzung): Option B reduziert den Hash-Anteil um etwa 8-15×. Das Lesen eines großen Eintrags wird dann I/O-/Decompress-bound statt Hash-bound. Export: ca. 3 Hash-Durchläufe pro Asset fallen praktisch weg (bei 10 GB Projekt ca. 45 s Single-Thread-Zeit).
- **Risiko / Verifikation**: Formatänderung. Test, der ein v2-Pak aus dem Repository weiter lädt (CLAUDE.md verlangt das für Chunk-Layouts), plus Roundtrip v3. Tests: `tests/test_hpak.cpp`, `tests/test_project_exporter.cpp`. Der `HE_Tools`-Packer ist nur ein Redirect (`HE_Tools/src/Packer/HpakWriter.h`), also keine zweite Implementierung.

---

### CORE-404 – `exchangeState()`: pro Frame Wert-Kopien, String-Lookups und lineare Suche je Binding/Watch
- **ID**: CORE-404
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.cpp:412-497` (`exchangeState`), `:581-621` (`reportChanges`), `:164-196` (`readPullSource`), `:365-410` (`resolveBindSource`), `:534-539` (`findWatched`), `:1078-1086` (`getVariable`). Aufrufer pro Frame: `GameApplication.cpp:3620`, `EditorApplication.cpp:5136`
- **Problem**: Pro Frame und pro Binding laufen: `find(owner)` (2 Hashes, siehe CORE-406), `resolveBindSource` mit `std::string`-Vergleichen gegen `kPullFrom*`, `find(src)`, die Sichtbarkeitsprüfung per `findVarInfo`/`findVarInLevels` (lineare Suche mit String-Vergleich), `getVariable(src, var)` (**Wertkopie**, bei Strings/Arrays/Structs tief mit Heap) und `valuesEqual` gegen den Shadow. Bei kompilierten Instanzen läuft `findVarInfo` doppelt (`getVariable` `:1082` prüft, dann sucht `compiled->getVariable` erneut). `reportChanges` kopiert ebenfalls jede beobachtete Variable (`Value now = getVariable(...)`, `:591`) in jedem Frame, auch wenn nichts passiert. Außerdem geht jede Runde mit mindestens einer Änderung in eine zweite komplette Runde über alle Bindings (`:436-491`, `changedAny`).
- **Code-Auszug**:
```cpp
for (size_t i = 0; i < m_watched.size(); ++i) {
    const InstanceId owner = m_watched[i].owner;
    if (!find(owner)) continue;
    Value now = getVariable(owner, m_watched[i].name);   // Kopie pro Variable pro Frame
    ...
    if (valuesEqual(now, w.shadow)) continue;
```
- **Lösung**: (1) Lesezugriff ohne Kopie: `const Value* peekVariable(id, name)` (interpretiert: Zeiger in `vars`; kompiliert: ggf. Reflection-Referenz, sonst Fallback auf Kopie). `valuesEqual` direkt dagegen, Kopie nur bei Änderung. (2) Quelle und Variablen-Deklaration beim ersten Auflösen in `Binding` cachen (Zeiger auf `Variable`/`CompiledVarInfo`-Index, `boundTo`-Instanz). Neu auflösen nur, wenn sich die Quell-Instanz ändert. (3) Für interpretierte Instanzen ein Versionszähler pro `Inst`, den `setVariable` erhöht; unveränderte Quelle überspringt den Vergleich. Kompilierte Member werden direkt geschrieben, dort bleibt der Vergleich. (4) Weitere Runden nur über Bindings, deren Quelle in der letzten Runde geschrieben wurde. `findWatched` über `unordered_map<(owner,name)>`.
- **Erwarteter Effekt** (Schätzung, messen): Bei ca. 200 Bindings/Watches grob 30-80 µs pro Frame heute, mit Peek und Cache ein Faktor 3-10 weniger. Bei wenigen Bindings vernachlässigbar, daher Mittel.
- **Risiko / Verifikation**: Semantik "nur eine Bewegung schreibt" und die Reihenfolge der Handler (Notify on Change) müssen bleiben. `tests/test_hc_bind_to.cpp` deckt das ab; ein Test mit großem Array als Quelle ergänzen. Der Peek-Zeiger darf nicht über einen Handler-Aufruf gehalten werden (Rehash in `vars`).

---

### CORE-405 – Abgeleitete, unveränderliche Listen werden pro Aufruf neu berechnet (`eventBindingsOf`, `replicatedVariablesOf`)
- **ID**: CORE-405
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.cpp:1281-1317` (`eventBindingsOf`), `:1167-1197` (`replicatedVariablesOf`), `:1199-1227` (`savedVariablesOf`). Aufrufer außerhalb: `WidgetManager.cpp:2935` und `:3425` (Pointer-Hit-Test, je Element der Treffer-Kette), `PropertyReplicator.cpp:104,138,164,203,216` (pro Entity und Replikations-Tick)
- **Problem**: `eventBindingsOf` durchläuft alle Knoten aller Level, kopiert jeden Event-Namen in einen neuen `std::string` und dedupliziert mit `std::any_of` über das Ergebnis (O(n²)). `inputActionEventNames(n)` alloziert zusätzlich pro InputAction-Knoten. Der Hit-Test ruft das für jedes Element der Treffer-Kette bei Mausbewegung bzw. Klick auf. `replicatedVariablesOf` baut pro Aufruf einen `vector<ReplicatedVar>` mit Namenskopien (ebenfalls quadratisches `report`), `PropertyReplicator` ruft es sogar nur zur Mitgliedschaftsprüfung (`rv.name == name`, `:203-204`, `:216-217`). Die Ergebnisse hängen nur von den (unveränderlichen) Graphen/`varInfos` ab.
- **Code-Auszug**:
```cpp
for (auto lv = i->levels.rbegin(); lv != i->levels.rend(); ++lv)
    for (const auto& n : lv->nodes) {
        auto report = [&](const std::string& name, int elem) {
            const bool have = std::any_of(out.begin(), out.end(), ...);   // O(n^2)
            if (!have) out.push_back({ name, elem });
        };
```
- **Lösung**: Beide Listen (und `savedVariablesOf`) lazy in `Inst` cachen und als `const std::vector<...>&` zurückgeben. Die Aufrufer iterieren nur. Zusätzlich `bool isReplicated(id, name)` für die reine Mitgliedschaftsprüfung. Ein Event-Name-Set für den Hit-Test (`bool listensTo(id, name, elem)`) würde die Schleife in `WidgetManager` auf einen Lookup reduzieren.
- **Erwarteter Effekt** (Schätzung): pro Aufruf O(Knoten) mit Allokationen → O(1) ohne Allokation. Relevant bei vielen Widgets (Hit-Test) und vielen replizierten Entities.
- **Risiko / Verifikation**: Rückgabetyp ändert sich (Wert → const-Referenz), Aufrufer in `HE_Scene` und `HE_Core` anpassen. Der Cache lebt mit `Inst`; bei Re-Registrierung entsteht ein neuer. `PropertyReplicator`-Tests und `test_horizoncode_runtime.cpp`.

---

### CORE-406 – `remove()` ist O(Listener+Bindings+Watches+Pending); `find()` hasht zweimal
- **ID**: CORE-406
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.cpp:744-772` (`remove`), `:1011-1058` (`retainOnlyReachableFrom`), `:46-57` (`find`)
- **Problem**: Jedes `remove(id)` scannt alle `m_listeners` (äußere und innere Maps, `vec.erase(std::remove...)`), `m_pending`, `m_suspended`, `m_bindings`, `m_watched`. Der Level-GC (`retainOnlyReachableFrom`) ruft `destroy → remove` für jede nicht erreichbare Instanz auf: O(N·(L+B+W)). Eine Szene mit 5000 Scene-Scope-Objekten, die ein Levelwechsel verwirft, wird quadratisch. `find()` macht außerdem pro Aufruf zwei Hash-Lookups (`m_doomed.count(id)` und `m_insts.find(id)`) und wird von praktisch jeder Context-Lambda (z. B. `:1361,1396,1514,1575`) aufgerufen.
- **Code-Auszug**:
```cpp
Runtime::Inst* Runtime::find(InstanceId id)
{
    if (m_doomed.count(id)) return nullptr;
    auto it = m_insts.find(id);
    return it != m_insts.end() ? &it->second : nullptr;
}
```
- **Lösung**: (1) `bool doomed` direkt im `Inst`; `find` = ein Lookup + Flag-Prüfung. `m_doomed` bleibt als Liste für `purgeDoomed`. (2) Batch-Variante `removeMany(const unordered_set<InstanceId>&)` für den GC: ein Durchlauf über Listener/Bindings/Watches mit Set-Lookup, danach je Instanz `destroy` ohne erneutes Scannen. Alternativ Reverse-Index `listener → [(owner, ev)]` und `owner → Bindings/Watches` (multimap).
- **Erwarteter Effekt** (Schätzung): `find` ca. halbe Kosten. Level-GC von O(N²) auf O(N), erst ab einigen hundert Instanzen spürbar.
- **Risiko / Verifikation**: `destroy` ruft Handler, die weitere Instanzen entfernen können (Reentranz). Der Batch muss die Reihenfolge "Destruct vor Remove" je Instanz beibehalten und darf keine Iteratoren über Handler-Aufrufe halten. Tests: `test_horizoncode_runtime.cpp` (GC, Destroy-in-Destruct).

---

### CORE-407 – Registrierung pro Instanz baut Strings für Pull/Watch, obwohl meist nichts zu tun ist
- **ID**: CORE-407
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.cpp:198-231` (`pullOnConstruct`), `:508-532` (`watchDeclared`), `:121-156`, `:71-119`
- **Problem**: Jede neue Instanz (Create Widget/Object, Listenzeilen, Projektile) baut in `pullOnConstruct` für **jede** Instanzvariable ein `PullSpec` mit 7 `std::string` (beim kompilierten Pfad mit `str()` jeweils eine String-Konstruktion pro Feld), sucht per Namen linear nach einem bestehenden Eintrag (`report`, O(n²)) und verwirft danach per `remove_if` fast alles (`src.empty() || var.empty()`). `watchDeclared` baut zusätzlich einen `vector<string>`. `widgetCreatorsWithoutShow(g)` läuft für jede Instanz ohne Klassen-Key. Das Ergebnis hängt ausschließlich von der Klasse ab, nicht von der Instanz.
- **Code-Auszug**:
```cpp
for (const auto& vi : inst->compiled->varInfos())
    report({ str(vi.name), str(vi.pullSource), str(vi.pullVar), str(vi.pullMember),
             str(vi.pullClass), vi.bindTo, str(vi.pullRef) });   // 6 Strings pro Variable pro Instanz
```
- **Lösung**: (1) Zwei-Phasen-Auswertung ohne Kopien: Zeiger/`string_view` der leaf-most Deklaration pro Name sammeln, Strings erst für Variablen mit `pullSource` und `pullVar` kopieren. (2) Besser: pro Klassen-Key die Spec-Liste und die `Watched`-Namen einmalig berechnen und cachen (`unordered_map<std::string classKey, ClassRegInfo>`; Instanzen ohne Key weiterhin direkt). Die dann leere Pull-Liste erlaubt einen frühen Rückkehrpfad.
- **Erwarteter Effekt** (Schätzung): bei Klassen mit 20 Variablen ca. 100+ kurzlebige String-Konstruktionen (teils Heap) weniger pro Spawn.
- **Risiko / Verifikation**: Semantik "leaf-most Deklaration entscheidet; Re-Deklaration ohne Pull nimmt den Pull heraus" erhalten. `tests/test_hc_pull_on_construct.cpp`, `test_hc_pull_editor.cpp`.

---

### CORE-408 – Jede Instanz hält eine tiefe Kopie ihrer Graphen (`std::vector<Graph>` by value) – unsicher
- **ID**: CORE-408
- **Kategorie**: C
- **Schweregrad**: Mittel (unsicher / messen)
- **Typ**: Performance
- **Ort**: `HorizonCodeRuntime.h:651` (`std::vector<Graph> levels`), `:95-99` (`add(Graph graph, ...)`, `addLevels(std::vector<Graph>, ...)`), `HorizonCodeRuntime.cpp:59-69,71-112`
- **Problem**: `Inst::levels` besitzt die Graphen. Jeder `add`/`addLevels`-Aufruf überträgt (ggf. nach einer Kopie beim Aufrufer) einen kompletten `Graph` mit Knoten, Strings und Variablen in die Instanz. Zwei Instanzen derselben Klasse (z. B. 200 Listenzeilen-Widgets oder Gegner) halten damit zwei vollständige Kopien. Der `Runner` nimmt `const Graph&` (`HorizonCode.h:1463`), die Graphen werden also zur Laufzeit nicht verändert. Ob die Aufrufer (`WidgetManager`, `EntityHost`, `PlayerHost`, `HE_Scene`) kopieren oder schon verschieben, wurde nicht geprüft.
- **Lösung**: `std::vector<std::shared_ptr<const Graph>> levels`, vom Aufrufer pro Klasse geteilt (Cache nach Klassen-Key). Das ermöglicht zugleich die klassenweisen Indizes aus CORE-402/407. Der nicht-const Zugriff `Inst::leaf()` (`:656`) muss auf Mutationsstellen geprüft werden.
- **Erwarteter Effekt** (Schätzung): Speicher und Spawn-Zeit pro Instanz sinken um die Graphgröße (Knoten × Strings). Größe pro Graph ist nicht gemessen.
- **Risiko / Verifikation**: Prüfen, ob irgendwo in `Runtime` oder den Hosts Graph-Daten pro Instanz verändert werden (Hot-Reload, Editor-Debug). `add(Graph)` als Kompatibilitäts-Overload behalten. Messen: `sizeof`-Summe eines typischen Widget-Graphen × Spawn-Anzahl.

---

### CORE-409 – `HpakWriter` verarbeitet Pass 2 strikt sequenziell (Cook, ASTC/BC7, zstd-19, Verschlüsselung)
- **ID**: CORE-409
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Hpak/HpakWriter.cpp:1110-1199` (Schleife), `:574-656` (`addEntry`, zstd Level 19 `:614`), `:703-815` (`cookTexture`), `:42-68` (`encodeAstc4x4`), `:101-127` (`encodeBc7`)
- **Problem**: Alle teuren Schritte laufen assetweise hintereinander auf einem Thread: Ref-Rewrite, Mip-Kette, ASTC/BC7-Block-Encoding, zstd-19, AES-GCM, Hashes. Das Dateikommentar `HpakWriter.h:22-26` nennt genau diese Teile als "die Minuten". Die Assets sind voneinander unabhängig; die Encoder sind bereits für Multi-Threading vorbereitet (`std::call_once` für `bc7enc`, `:107-108`). `astcenc_context_alloc(&cfg, 1, &ctx)` pro Mip-Ebene erzeugt je einen Ein-Thread-Kontext. Das JobSystem (`parallel_for`, `schedule`) liegt im selben Modul.
- **Code-Auszug**:
```cpp
for (auto& pe : pending)
{
    ...
    std::vector<uint8_t> blob = rewriteRefsForPack(pe.bytes, pathToUuid, settings, &blobByPath, &deps);
    if (settings.cook) blob = cookForPack(blob, ...);   // Mips + ASTC/BC7
    ...
    if (!reused) addEntry(pe.id, blob, settings);        // zstd -19 + AES-GCM
```
- **Lösung**: Schleife in zwei Teile trennen. (a) Parallel: pro Asset ein Ergebnisobjekt `{PendingEntry or reused StoredEntry, srcHash, CachedEntry, warnings}`, berechnet über `parallel_for(pending.size(), …, "PackAsset", /*minGrain=*/1)` (nur ein Level parallelisieren, Nesting fällt per Design auf den Aufrufer zurück). (b) Seriell in `pending`-Reihenfolge: `m_entries`/`m_srcHashes`/`m_entryInfos` füllen, `warn()`, `onAsset`, `progress` aufrufen. Damit bleibt der Thread-Vertrag der Callbacks ("same thread as the pack call") und ein deterministisches Manifest. `cache->previousPak->readStoredEntry` nutzt einen geteilten `ifstream` (nicht thread-safe, `HpakReader.h:12`); dafür pro Job `openShared` (`HpakReader.h:33`) verwenden.
- **Erwarteter Effekt** (Schätzung): nahezu linear mit Kernen. zstd-19 liegt grob bei wenigen MB/s pro Kern, ein 1-GB-Projekt braucht single-threaded Minuten und auf 8 Kernen grob ein Achtel.
- **Risiko / Verifikation**: `compileShaderVariants`/`compileParticleShaderVariants` sind Editor-Callbacks (Shader-Cross-Compiler), deren Thread-Sicherheit unbekannt ist. Entweder serialisieren (Mutex) oder vorab prüfen. Ausgabe muss byte-identisch bleiben: Vergleichstest "Pak seriell vs. parallel" in `tests/test_hpak.cpp`/`test_project_exporter.cpp` (write() sortiert nach UUID, die Reihenfolge von `m_entries` ist unkritisch).

---

### CORE-410 – Pass 1 liest jedes `.hasset` komplett (zeichenweise) und hält das ganze Projekt im RAM
- **ID**: CORE-410
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HpakWriter.cpp:1001-1005` (Einlesen), `:941-944` (`pending`), `:1039`, `:1064-1070` (`blobByPath`), `:1204-1271` (`write`); ähnlich `ProjectConfig.cpp:121-123`, `ProjectExporter.cpp:159-165`
- **Problem**: (1) `std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>())` liest Zeichen für Zeichen über `sgetc/snextc`, typisch einige 100 MB/s statt GB/s, und wächst per Verdopplung (Spitzen-RAM bis 2× Dateigröße). (2) Der Scan braucht je Datei nur UUID, Pfad und Typ aus dem META-Chunk, behält aber alle Bytes in `pending`. Das gesamte Projekt liegt gleichzeitig im Speicher, danach zusätzlich Cook-Kopien und `m_entries` mit allen gespeicherten Daten bis `write()`. Bei mehreren GB Content ist das der Spitzenverbrauch des Editors beim Export.
- **Code-Auszug**:
```cpp
std::ifstream f(p.path(), std::ios::binary);
std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
...
pending.push_back({std::move(bytes), id, std::move(rel), std::move(path)});
```
- **Lösung**: (a) Sofort und ohne Verhaltensänderung: Größe per `seekg/tellg` bzw. `std::filesystem::file_size`, `resize`, ein `f.read`. (b) Strukturell: Pass 1 liest nur Header + META (Chunk-Header, `seekg` über Payloads) und speichert `{path, size, uuid, relPath, metaPath, type}`. Pass 2 lädt die Bytes erst, wenn der Eintrag dran ist (bei parallelem Pass 2 pro Job); `blobByPath` für Material-Parents durch einen On-Demand-Loader mit kleinem LRU ersetzen. `write()` kann die Nutzdaten statt aus `m_entries` aus einer temporären Datei streamen.
- **Erwarteter Effekt** (Schätzung): (a) Lesen 3-10× schneller. (b) Spitzen-RAM von Σ(Projekt) auf wenige Assets pro Thread. Das hat große Wirkung auf Rechnern mit wenig RAM.
- **Risiko / Verifikation**: (b) berührt die Level-1-Reuse-Logik (`inHash` braucht den Dateiinhalt; der Hash kann dabei beim Lesen mitlaufen). `test_hpak.cpp`/`test_project_exporter.cpp` (inkrementeller Export, Override/Collision-Warnungen). Sehr große Projekte als manueller Test.

---

### CORE-411 – Mehrfaches Voll-Kopieren jedes Assets durch `HAsset::Reader::openData` und `return blob`
- **ID**: CORE-411
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `HpakWriter.cpp:165-180` (`metaFromHasset`), `:290-309` (`rewriteRefsForPack`), `:823-834` (`cookForPack`), `:1145-1151`; Ursache `src/HE_Core/include/ContentManager/HAsset.h:437-477` (`openData` kopiert jede Chunk-Payload in einen eigenen Vektor)
- **Problem**: Pro Asset läuft: `metaFromHasset` (kopiert **alle** Chunks, nur um META zu lesen), danach `rewriteRefsForPack`: neuer Reader (Vollkopie), und für Texturen, Sounds und alle anderen Typen am Ende `return blob;` als **Kopie** (Parameter ist `const&`), dann `cookForPack`: wieder neuer Reader (Vollkopie) und `return blob;` (Kopie). Der Typ steht aber im Dateiheader (`stripImportSourceForPack` macht das bereits so, `:201-205`). Für eine 64-MB-Textur oder 200-MB-Mesh sind das 4-5 transiente Vollkopien, bevor auch nur ein Byte komprimiert wird.
- **Code-Auszug**:
```cpp
HAsset::Reader r;
if (!r.openData(blob)) return blob;                 // Vollkopie aller Chunks
...
if (!isMesh && !isMat && !isScene && !isParticles) return blob;   // und noch eine Kopie als Rückgabewert
```
- **Lösung**: (1) `HAsset::peekAssetType(const vector&)` bzw. `peekChunk(data, id)` (nicht kopierend, gibt `span` zurück). (2) `rewriteRefsForPack`/`cookForPack` nehmen `std::vector<uint8_t>&& blob` bzw. arbeiten in-place mit `bool changed`-Rückgabe. Frühaustritt vor dem Reader, wenn der Typ nichts zu tun hat. (3) `metaFromHasset` über `peekChunk(CHUNK_META)`.
- **Erwarteter Effekt** (Schätzung): 3-4 Vollkopien pro durchgereichtem Asset weniger (Zeit und Spitzen-RAM), bei texturlastigen Projekten deutlich.
- **Risiko / Verifikation**: Ausgabebytes müssen identisch bleiben (Goldtest: Pak vor/nach der Änderung binär vergleichen, `test_hpak.cpp`). `HAsset.h` liegt in einem anderen Slice (ContentManager), dort nur eine additive API.

---

### CORE-412 – Material-Layout (MTRL) dreifach nachgebaut; `rewriteRefsForPack` ist eine 280-Zeilen-Funktion
- **ID**: CORE-412
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `HpakWriter.cpp:248-275` (`mtrlGraphAndParent`), `:354-542` (MTRL-Zweig, ca. 190 Zeilen, Offsets `tailStart`, `graphTexOffset`, `afterTypes`, `afterGBuf`), `:514` (Magic-Größen 92/40/32), `:290-572` (gesamte Funktion)
- **Problem**: Die Feldreihenfolge des MTRL-Chunks wird an zwei Stellen in diesem File von Hand nachgelesen (`mtrlGraphAndParent` und der Zweig in `rewriteRefsForPack`), eine dritte Kopie liegt im Writer in `ContentManager::saveAsset`. Der Code warnt selbst (`:356-361`), dass ein Feld, das dort ergänzt wird und hier fehlt, ohne Fehler zu falschen Offsets in gepackten Materialien führt. Die Fall-Unterscheidung über die Größe des Approx-Blocks (`rem >= 92 ? 92 : ... 40 ... 32`) kodiert historische Revisionen als Magic Numbers. Die Funktion mischt vier Aufgaben (Mesh-MREF/MSEC, Material, Partikel, Szene) mit tiefer Verschachtelung.
- **Lösung**: Einen Skimmer `HAsset::skimMtrl(const std::vector<uint8_t>&) -> MtrlLayout{ offsets, shaderPath, texturePaths, graphJson, parentPath, switchNames, switchValues, ... }` neben dem MTRL-Writer in `HAsset`/`ContentManager` ablegen und von beiden Funktionen nutzen. Die Approx-Blockgrößen als benannte Konstanten. Danach `rewriteRefsForPack` in `rewriteMeshRefs`, `rewriteMaterial`, `rewriteParticles`, `rewriteScene` aufteilen.
- **Erwarteter Effekt**: Weniger Drift-Risiko, kürzere Funktionen. Reine Wartbarkeit, keine Laufzeitwirkung.
- **Risiko / Verifikation**: Hoch, weil Ausgabebytes exakt gleich bleiben müssen. Absichern mit (a) Goldtest über ein Pak mit Material, Material-Instanz (mit Parent), Switch-Overrides und alten Tail-Revisionen (92/40/32/0 Byte), (b) einem Roundtrip-Test "Writer schreibt MTRL, Skimmer liest dieselben Felder". `test_hpak.cpp`, `test_material_graph.cpp`.

---

### CORE-413 – Runtime-Binaries: pro Datei `std::system("strip …")`/`codesign`, sequenziell, jedes Mal neu
- **ID**: CORE-413
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Hpak/ProjectExporter.cpp:200-203` (`stripLocalSymbols`), `:210-217` (`resignMachO`), `:944-1027` (Kopierschleife), `:982-984`, `:992-1022`, `:1454`
- **Problem**: Für jede reguläre Datei der Runtime (auch `config.json` u. Ä.; die Magic-Prüfung `looksMachO` betrifft nur den Apple-Re-Sign) wird eine Shell mit `/usr/bin/strip -x` gestartet. Das schreibt jede Bibliothek komplett neu. Auf macOS folgt ein `codesign` pro Mach-O, und `signAppBundle` signiert danach `--deep` das ganze Bundle noch einmal. Alles seriell, bei jedem Export, auch wenn die Runtime unverändert ist (zuvor wird jede Zieldatei gelöscht und neu kopiert, `:982-984`).
- **Code-Auszug**:
```cpp
return std::system(("/usr/bin/strip -x " + shellQuote(p) + " 2>/dev/null").c_str()) == 0;
...
std::filesystem::remove(dst, ec); ec.clear();
std::filesystem::copy_file(dit->path(), dst, overwrite_existing, ec);
```
- **Lösung**: (1) Nur ELF/Mach-O strippen (Magic prüfen, bevor ein Prozess gestartet wird). (2) Ein einmal gestrippter Cache der Runtime (Schlüssel: Pfad + Größe + mtime) im Build-Cache, der Export kopiert nur noch. Das spart auch das Re-Signieren. (3) Die Dateien über `parallel_for` bearbeiten. (4) `posix_spawn` statt `std::system` (keine Shell, kein Quoting-Thema).
- **Erwarteter Effekt** (Schätzung): Der Export-Schritt "binaries" wird von Sekunden bis zu einer halben Minute (große Runtime, macOS-Signatur) auf nahe null bei Cache-Treffer.
- **Risiko / Verifikation**: Der Cache muss bei Runtime-Wechsel zuverlässig ungültig werden (Hash über Datei). Die Key-Patch-Reihenfolge (Patch nach Strip/Sign) bleibt. `tests/test_project_exporter.cpp`, auf macOS manuell: gestarteter Export läuft auf Apple Silicon.

---

### CORE-414 – AutoLandscape-Shader wertet alle Schichten und Bombing-Taps unbedingt aus – unsicher
- **ID**: CORE-414
- **Kategorie**: D
- **Schweregrad**: Mittel (unsicher / GPU-Messung nötig)
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp:109-142` (4 Schichten × 3 Maps, je `TextureArrayBombSample`), `:154-158`, `:179-212` (Blend-Kette), `:268-273` (drei `layerBlend`, die wieder alle Schichten referenzieren)
- **Problem**: Der Graph liest für Grass, Dirt, Rock und Snow je Albedo, Normal und Mask. Mit aktivem `Texture Bombing` (Standard) ist jede dieser 12 Lesungen ein Hex-Tiling mit je 3 Taps (laut Kommentar im Header), also rund 36 Array-Samples pro Pixel, dazu zwei `Fbm` und die Weather-Funktionen. Codegen kennt keine Verzweigung nach Masken: Schnee in der Ebene und Fels auf flachem Gelände werden trotzdem gesampelt, da Lerp-Zweige keine dynamische Verzweigung bilden. Die gemalte Variante (`layerBlend`) liest alle Schichten ein zweites Mal als Blend-Eingang, teilt aber die bereits gelesenen Knoten. Aufwand ist also nicht verdoppelt, aber hoch.
- **Lösung** (nur nach Messung): (a) Entfernungsabhängiger Schalter: Bombing nur nahe der Kamera, fern plain (der `StaticSwitch` existiert bereits als Permutation, Material-Instanz-Override). (b) Wenn `MaterialGraph`/Codegen eine `Branch`/`if`-Node mit Uniform-Control-Flow unterstützt, Snow-Lesungen hinter `snowAny > 0`. (c) Die `Mask`-Arrays für Fernbereiche aus einem niedrigeren Mip lesen.
- **Erwarteter Effekt**: Unbekannt; auf Mittelklasse-GPUs kann ein bildfüllendes Terrain mit 36+ Taps bandbreitenlimitiert sein. Ohne GPU-Zeitmessung (Pass-Timer des Editors) nur Vermutung.
- **Risiko / Verifikation**: Optik ändert sich (a)/(c). Die Witness-/Mask-Tests lesen nur die Masken, nicht die Samples. GPU-Timings pro Backend mit dem Stats-Card-Pass-Timer messen, bevor etwas geändert wird.

---

### CORE-415 – `dispatchChanged` und `dispatchDestroyed` sind zu großen Teilen Duplikate
- **ID**: CORE-415
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `HorizonCodeRuntime.cpp:623-703` (`dispatchChanged`) und `:883-966` (`dispatchDestroyed`)
- **Problem**: Beide Funktionen enthalten dieselben drei Blöcke: Listener kopieren und Tiefenwache, das Dispatch-Budget (`++m_dispatchFires > kMaxDispatchFires` mit einmaliger Warnung), und die Ermittlung der Handler-Signatur (`handles/wantsArg/argType/argTypeName` getrennt für kompilierte `eventInfos()` und interpretierte `Event`-Knoten). Dazu kommt das Verschicken (`compiled->fireEvent` vs. `runEventOnLevel`). Kleiner Nebeneffekt: `kOnDestroyed == std::string(e.name)` (`:916`) konstruiert pro Event-Info einen temporären String, die Schwesterfunktion nutzt `evName == e.name`.
- **Lösung**: `struct HandlerSig { bool handles, wantsArg; PinType argType; std::string argTypeName; }; HandlerSig handlerSignature(const Inst&, const std::string& ev) const;` und `template<class MakeArg> void forEachBoundListener(owner, EventId, const std::string& name, MakeArg)` mit Tiefe, Budget und Versand. Die beiden Funktionen reduzieren sich auf die jeweilige Argumentprüfung (Struct-Typ bzw. numerische Kompatibilität). Dieselbe Signatursuche sollte auch `levelHandlingEvent` (CORE-402) nutzen.
- **Erwarteter Effekt**: ca. 70 Zeilen weniger, eine Stelle für Budget-/Signatur-Regeln.
- **Risiko / Verifikation**: Unterschiede sind absichtlich (`dispatchDestroyed` ohne trailing Listener-Pass; Struct-Prüfung statt `pullShapesCompatible`). Tests: `test_hc_bind_to.cpp` (Notify on Change), Extract-on-Destruct-Tests in `test_horizoncode_runtime.cpp`.

---

### CORE-416 – "Ist öffentliche Instanzvariable?" und "über alle Variablen beider Backends laufen" mehrfach implementiert
- **ID**: CORE-416
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: Öffentlichkeitsprüfung: `HorizonCodeRuntime.cpp:169-182` (`readPullSource`), `:389-402` (`resolveBindSource`, Ref-Variante), `:1098-1113` (`setPublicVariable`), `:1115-1126` (`isPublicVariable`), `:1432-1448` (`ctx.getExternal`). Variablen-Iteration (leaf-most wins): `:213-225`, `:514-523`, `:1185-1195`, `:1213-1222`
- **Problem**: Der Satz "compiled: `findVarInfo` und `access == 0`, interpretiert: `findVarInLevels` und `access == 0 && scope == 0`" steht fünfmal. `getExternal` dupliziert `isPublicVariable` + `getVariable` samt zusätzlichen Warnungen. Die vier Iterationen "kompiliert über `varInfos()`, interpretiert über `levels[].variables` mit `scope == 0`, mit Ersetzen-nach-Namen" sind strukturell gleich, jede mit eigener linearer `report`-Suche (O(n²)).
- **Lösung**: `struct VarDecl { bool found, isPublic, isRef, isArray; PinType type; }; VarDecl declOf(const Inst&, const std::string&) const;` und `template<class F> void forEachInstanceVar(const Inst&, F f)` (liefert eine kleine View mit `name,type,access,replicated,saveGame,pull*`). Die genannten Funktionen sind dann Einzeiler.
- **Erwarteter Effekt**: ca. 60 Zeilen weniger; Verhalten identisch; leichter, CORE-404/405 umzusetzen.
- **Risiko / Verifikation**: Gering. Bestehende Tests (`test_horizoncode_runtime.cpp`, `test_scripting_binding.cpp`) decken Zugriffsregeln ab.

---

### CORE-417 – JobSystem: Kleinigkeiten (Header-Gewicht, 1-ms-Polling in `wait()`, Statistik-Uhr pro Task)
- **ID**: CORE-417
- **Kategorie**: E / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `JobSystem.h:7-14`; `JobSystem.cpp:410-413`, `:461-464`, `:475-517`
- **Problem**:
  1. `JobSystem.h` (in 19 Dateien eingebunden) zieht `<queue>` ("no longer used here", Zeile 12) und `<future>` mit. Nur `ContentManager.h` nutzt `std::queue`, und es inkludiert `<queue>` selbst (`ContentManager.h:15`; eine Repo-weite Suche in `src/` und `tests/` fand keinen weiteren Nutzer).
  2. `JobHandle::wait()` auf einem `Waiting`-Job, dessen Upstream auf anderen Threads läuft, schläft in einer 1-ms-Schleife (`wait_for(1ms)`), statt auf eine echte Benachrichtigung zu warten.
  3. Der Worker-Loop wertet `core->pick(prio)` nach dem Wait-Prädikat ein zweites Mal aus (`:461-464`) und liest für **jeden** laufenden Task zweimal `steady_clock::now()` plus drei bis vier Relaxed-Atomics, obwohl der Kommentar `:55-56` sagt, die Diagnose solle nicht zusätzliche Kosten pro Task verursachen. Bei µs-kleinen `parallel_for`-Helfern ist das ein messbarer Anteil.
- **Code-Auszug**:
```cpp
std::unique_lock<std::mutex> lock(job->m);
job->cv.wait_for(lock, std::chrono::milliseconds(1), [&] { return job->status.load(...) != JobStatus::Waiting; });
continue;
```
- **Lösung**: (1) `<queue>` entfernen; `<future>` hinter ein `JobSystem/JobSubmit.h` oder in den Aufrufer von `submit` verschieben (nur wenn der Build-Zeitgewinn gemessen wird). (2) Poolweiten Fortschrittszähler + `cv` (`core.progress`), den `finishJobs` hochzählt; `wait()` wartet auf Änderung dieses Zählers statt zu pollen. (3) Task-Zeit nur erfassen, wenn Stats/Profiler aktiv sind (`std::atomic<bool> statsOn`), `pick` im Prädikat in einen Merker schreiben.
- **Erwarteter Effekt** (Schätzung): (1) kürzere Compile-Zeit. (2) Keine 1000 Wakeups/s mehr bei blockierenden Dependency-Waits (selten, daher Niedrig). (3) ca. 40-60 ns pro Task.
- **Risiko / Verifikation**: Lost-Wakeup bei (2) gründlich prüfen (Zähler vor dem Test lesen). `tests/test_jobsystem.cpp` (Cancel/Dependency/Stats-Tests) deckt das ab.

---

### CORE-418 – `addPackedEntry` kopiert gespeicherte Daten; `cookTexture` legt mehrere Vollkopien an
- **ID**: CORE-418
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `HpakWriter.cpp:658-669` (`e.data = stored.data;`), Aufrufer `:1131`, `:1190`; `:743-752` (`rgbaLevels = px->data; prev = px->data;`)
- **Problem**: `addPackedEntry` bekommt `const StoredEntry&` und kopiert den ganzen Datenpuffer, obwohl die Aufrufer ihr lokales `se` danach nicht mehr benutzen. Bei vollständigem Reuse (häufigster inkrementeller Export) entspricht das einer Kopie des gesamten Paks im Speicher. In `cookTexture` werden Level 0 doppelt kopiert (`rgbaLevels` und `prev`), die Kette wächst ohne `reserve` (Faktor 4/3).
- **Lösung**: Overload `addPackedEntry(const HE::UUID&, HpakReader::StoredEntry&&)` mit `e.data = std::move(stored.data)`. In `cookTexture` `rgbaLevels.reserve(px->data.size() * 4 / 3 + 64)`, `prev` als `const uint8_t*` auf die zuletzt angehängte Ebene.
- **Erwarteter Effekt**: eine Pak-große Kopie weniger beim Reuse; bei 4K-Texturen zwei bis drei Kopien à 64 MB weniger.
- **Risiko / Verifikation**: Gering; Ausgabebytes gleich. `test_hpak.cpp`.

---

### CORE-419 – Pak, `project.hcfg` und Manifest werden nicht atomar geschrieben
- **ID**: CORE-419
- **Kategorie**: E / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `HpakWriter.cpp:1204-1277` (`write`, `trunc` direkt auf Zielpfad), `ProjectConfig.cpp:24-26`, `ProjectExporter.cpp:416` (Manifest). Vorbild im selben File: `patchEmbeddedPakKey` `:253-274`
- **Problem**: `write()` öffnet das Ziel mit `trunc`. Bricht der Export mittendrin ab (Absturz, Kill, volle Platte), sind das alte Pak **und** das passende Manifest verloren, und das Ziel ist ein halbes Pak. Der Exporter hat die Technik (Temp + `rename`) schon für die Executable, wendet sie aber auf die übrigen Dateien nicht an.
- **Lösung**: Ein Helfer `writeFileAtomic(path, writerFn)` (Temp-Datei im selben Verzeichnis, `flush/close`, `rename`) für Pak, hcfg und Manifest; `patchEmbeddedPakKey` nutzt ihn ebenfalls.
- **Erwarteter Effekt**: Robustheit; kein Performance-Effekt.
- **Risiko / Verifikation**: Windows: `rename` über eine geöffnete Datei schlägt fehl, der Exporter schließt `prevPak` bereits vorher (`ProjectExporter.cpp:842`). Test: Schreibfehler simulieren (read-only Verzeichnis) und prüfen, dass das alte Pak bleibt.

---

### CORE-420 – Schlüsselpatch liest die komplette Executable (zeichenweise) und sucht naiv
- **ID**: CORE-420
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `ProjectExporter.cpp:159-165` (`readWholeFile`), `:220-251` (`patchEmbeddedPakKey`), `:289-301` (`readEmbeddedPakKey`), Aufrufer `:646-649`
- **Problem**: Für das Auffinden eines 24-Byte-Magic in einem 20-50-MB-Executable wird die Datei per `istreambuf_iterator` komplett eingelesen (siehe CORE-410) und mit `std::search` durchsucht. `readEmbeddedPakKey` macht das bei jedem inkrementellen verschlüsselten Export auf der alten Executable. Das Patchen schreibt zusätzlich die komplette Datei neu (Temp + `rename` ist beabsichtigt und richtig).
- **Lösung**: Gemeinsamer `readFileBytes(path, out)` mit `file_size` + `read`. Für das reine Lesen des Schlüssels in 1-MiB-Fenstern mit 23 Byte Überlappung suchen (`std::boyer_moore_horspool_searcher`), ohne die Datei zu halten.
- **Erwarteter Effekt** (Schätzung): einige hundert ms pro Export und Executable.
- **Risiko / Verifikation**: Gering. `test_project_exporter.cpp` (Embedded-Key-Tests).

---

### CORE-421 – ProjectExporter: Duplikate, uneinheitliche Doku, unescapte Plist-Werte
- **ID**: CORE-421
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `ProjectExporter.cpp:1354-1364` und `:1406-1416` (identischer `#if`-Block), `:183-188` (`shellQuote`) vs. `:501-504` (Inline-Kopie), `:914-919` (`n.compare(...)` ohne Klammern um `&&`/`||`) vs. `:966-969` (`endsWith`), `:461-476` (`ident`/`ver` unescaped), `ProjectExporter.h:208-214` (`onStage`-Doku), Phasenkommentare `:1191/1314`, `:1255/1392` ("Phase 7b/7c" jeweils doppelt), `:886-1108` (220 Zeilen, bis zu 6 Verschachtelungsebenen)
- **Problem**: (1) Das Auflösen von `ExportPlatform::Host` steht zweimal wortgleich. (2) `signAppBundle` baut sein Shell-Quoting selbst, obwohl `shellQuote` existiert. (3) Dateiendungen werden an drei Stellen (hier zweimal, plus `IntegrityProbe.cpp:35-38`) mit eigenen Lambdas verglichen. Die unklammerte Mischung `A && (B) || C && (D)` ist korrekt, löst aber `-Wparentheses` aus. (4) `writeInfoPlist` escaped nur den Anzeigenamen; `bundleId` und `appVersion` gehen roh in das XML. Ein `&` oder `<` in der Version erzeugt ein ungültiges Plist. (5) `ExportSettings::onStage` listet sechs Stage-Namen, der Code meldet auch `"icon"`. (6) `copyRuntimeBinaries` ist die längste Funktion der Datei.
- **Lösung**: `ExportPlatform resolveHost(ExportPlatform)`, `shellQuote` überall, `hasSuffix(std::string_view, std::string_view)` in einem gemeinsamen Util, `xmlEscape` für alle Plist-Strings, `copyRuntimeFile(...)` und `shipSubdir(...)` aus `copyRuntimeBinaries` herauslösen, Kommentare angleichen.
- **Erwarteter Effekt**: Lesbarkeit, ein latenter Plist-Fehler weniger.
- **Risiko / Verifikation**: Gering. Plist-Ausgabe mit Sonderzeichen in einem Test prüfen (`test_project_exporter.cpp`).

---

### CORE-422 – Krypto: mbedTLS-`randomBytes` seedet pro Aufruf (pro Pak-Eintrag), duplizierte Konstanten, veralteter Kommentar
- **ID**: CORE-422
- **Kategorie**: D / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `Aes256Gcm.cpp:110-137` (mbedTLS-`randomBytes`), `HpakWriter.cpp:635` (Aufruf pro Eintrag), `Aes256Gcm.cpp:10-11` und `:105-106` (doppelte Konstanten), `KeyDerivation.h:8,31-32` und `KeyDerivation.cpp:28` (`hmacSha256` mit Vektor-Allokation)
- **Problem**: (1) Im mbedTLS-Fallback wird für jede 12-Byte-Nonce ein frischer Entropie-Kontext plus CTR-DRBG initialisiert und geseedet. Der Kommentar `:115-116` ("not in a hot loop") stimmt nicht, denn `addEntry` ruft es pro Asset. Bei 100.000 Einträgen sind das 100.000 Seeds. (2) `kTagLen`/`kNonceLen` stehen in beiden Backend-Blöcken identisch. (3) `KeyDerivation.h` behauptet "SHA-256 is implemented inline", die Implementierung liegt aber in `Crypto/Sha256.cpp` (`KeyDerivation.cpp:7-12`). (4) `hmacSha256` kopiert die Nachricht in einen neuen `std::vector` (`:28`); für den Handshake einmal pro Verbindung vernachlässigbar.
- **Lösung**: Pro Thread einen DRBG halten (`thread_local`, lazy) oder alle Nonces eines Packs in einem Aufruf anfordern. Konstanten vor die `#if`-Kette ziehen. Kommentare korrigieren. Falls die `Sha256`-Klasse einen Streaming-Modus hat, diesen für HMAC nutzen.
- **Erwarteter Effekt**: Nur mbedTLS-Builds und große Pakete; sonst Wartbarkeit.
- **Risiko / Verifikation**: DRBG-Zustand nicht zwischen Threads teilen. Bestehende AES-/HMAC-Vektor-Tests in `tests/test_hpak.cpp` bzw. Netz-Tests.

---

### CORE-423 – `HorizonGameServices.h`: `std::string`-Parameter, `strlen` trotz bekannter Länge, Makros statt `constexpr`
- **ID**: CORE-423
- **Kategorie**: F / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `HorizonGameServices.h:734-735` (`keyDown(const std::string&)`), `:759-762` (`gamepadButton`/`gamepadAxis`), `:526` (`std::string(small)`), `:38-54` (ABI-Konstanten als `#define`), `:58-66` vs. `IGameLogic.h:16-22` (zweimal `HE_GAME_API`)
- **Problem**: Die Wrapper nehmen `const std::string&`. Spielcode ruft `he::input::keyDown("W")` im Per-Frame-`onUpdate`, jede Aufrufstelle konstruiert einen String (SSO, bei Namen über 15 Zeichen Heap, z. B. `"leftshoulder"` ist 12, `"righttrigger"` 12, `"Escape"`, knapp). `fetchString` kennt nach dem Aufruf `need`, baut den Ergebnis-String aber mit `std::string(small)` (erneutes `strlen`). Die ABI-Versionen sind Makros, obwohl der Header ohnehin C++17-kompatibel bleiben muss (`inline constexpr` ist dort erlaubt). `HE_GAME_API` ist an zwei Stellen definiert (einmal mit `#ifndef`, einmal ohne), was bei identischem Text zufällig funktioniert.
- **Lösung**: Zusätzliche Overloads mit `const char*` (die `std::string`-Varianten rufen sie auf). `std::string(small, (size_t)need)`. `HE_*_ABI_VERSION` als `inline constexpr uint32_t` (die Makros nur dort behalten, wo sie im Präprozessor gebraucht werden). `HE_GAME_API` aus einer gemeinsamen kleinen Datei `HeGameApi.h`.
- **Erwarteter Effekt**: Spielcode ohne temporäre Strings; marginal.
- **Risiko / Verifikation**: ABI unverändert (nur inline Wrapper). Gilt für das C++17-Scaffold (`CppScaffold::cmakeLists`): kompilieren mit `-std=c++17`. Test der Wrapper existiert wahrscheinlich in `test_scripting_binding.cpp`/Game-Logic-Tests (nicht geprüft).

---

### CORE-424 – IntegrityProbe: Log kopiert das Manifest, Hash-Job mit Normal-Priorität, kleine Quadratik
- **ID**: CORE-424
- **Kategorie**: C / E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `IntegrityProbe.cpp:264` (`manifest().entries.size()`), `:224-226` (`submit` ohne Priorität), `:124-126` (`any_of` pro Datei), `:148-150` (`compare` kopiert und sortiert beide Listen)
- **Problem**: `publish` ruft für die Log-Zeile `manifest()` auf (Mutex + **Kopie** des gesamten Manifests), obwohl die Größe vor dem `std::move` bekannt ist. Der Start-Job hasht Executable und alle Bibliotheken (bis einige hundert MB SHA-256) als `Normal`-Job und konkurriert damit beim Spielstart mit dem Asset-Streaming. `JobPriority::Low` existiert (`submit` hat den Parameter) und bleibt hinter dem Frame-Pfad. Die weiteren Punkte sind klein (n < 100).
- **Lösung**: `const size_t n = m.entries.size();` vor dem Move loggen. `globalPool().submit(..., "IntegrityProbe", HE::JobPriority::Low)`. Optional: Bibliotheken parallel hashen, falls Startzeit zählt.
- **Erwarteter Effekt**: Vernachlässigbar bis klein; Low-Priorität nimmt dem Streaming beim Start einen Worker weg.
- **Risiko / Verifikation**: Gering; `join` wartet weiter über `Running`-Zustand. `tests/test_integrity.cpp`.

---

### CORE-425 – `ProjectConfig`: verschachtelte Versions-Ternary, doppelte Kommentare, tote Felder, verstreute Flag-Bits
- **ID**: CORE-425
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `ProjectConfig.cpp:31-49` (Kommentar doppelt, 5-stufige Ternary), `:61-85` vs. `:143-154` (Flag-Bits an zwei Stellen), `:51-52`/`:127` (`reserved`), `ProjectConfig.h:15` und `ProjectConfig.cpp:57,138-140` (`projectUuidBytes` "legacy (unused)")
- **Problem**: Die Versionswahl ist eine fünfstufige Verkettung; der Kommentar dazu steht doppelt (`:31-37`). Die Flag-Bits (1, 2, 4, 8, 16, 32, 64, … 4096) stehen als Zahlenliterale in `save` und `load`. Bit 4096 ist in `load` vor Bit 2 platziert. `projectUuidBytes` wird geschrieben und gelesen, aber laut Header nirgends benutzt (16 tote Byte im Format, nicht entfernbar ohne Formatänderung). Die einzelne Feldreihenfolge wird in `save`/`load` über `if (version >= N)` gespiegelt.
- **Lösung**: `enum HcfgFlag : uint32_t { ModSupport = 1, Encrypted = 2, … }` und `static uint16_t requiredVersion(const ProjectConfig&)` mit `std::max({ ... })`. Kommentar zusammenführen. `projectUuidBytes` als "reserviert, wird weiter geschrieben" kennzeichnen.
- **Erwarteter Effekt**: Lesbarkeit; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Dateiformat muss byte-identisch bleiben (Golden-Test: bekannte hcfg-Dateien v2..v7 laden und wieder schreiben). Test in `test_project_exporter.cpp`.

---

### CORE-426 – HorizonCode-`fire*`-Einstiegspunkte: fünf Makro-Familien und String-Temporäre
- **ID**: CORE-426
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `HorizonCodeRuntime.cpp:1711-1733`, `:1745-1761`, `:1765-1776`, `:1779-1850`, `:1855-1890`, handgeschrieben `:1807-1815`, `:1892-1990`; `:2002`, `:2019-2020`
- **Problem**: Rund 60 Einstiegspunkte entstehen aus fünf Makrofamilien plus 10 handgeschriebenen Varianten, die dieselbe Struktur wiederholen (`find`, kompiliert/interpretiert, `static const EventId`, `dispatchToListeners`). Der Namensparameter ist ein String-Literal, das für `runEventOnLevel(const std::string&)` und `dispatchToListeners(const std::string&)` jedes Mal einen `std::string` erzeugt. Namen mit mehr als 15 Zeichen (`"OnSelectionChanged"`, `"OnWindowFocusChanged"`, `"OnAnimationNotifyBegin"`, `"OnAnimationFinished"`) landen dabei bei jedem Event im Heap. `fireEvent` → `dispatchToListeners(id, event, arg)` hasht den Namen per `eventId(event)` bei jedem Aufruf neu.
- **Lösung**: Eine Funktionsvorlage `template<class Hook, class MakeArg> void fireBuiltin(InstanceId id, const BuiltinEvent& ev, int elem, Hook hook, MakeArg arg)` mit `static const BuiltinEvent ev{ "OnClicked", eventId("OnClicked") }`, wobei `BuiltinEvent` den `std::string` einmal hält. Damit entfallen die temporären Strings und die Makros. `Runtime`-Methoden bleiben Einzeiler.
- **Erwarteter Effekt**: Weniger Code (ca. 150 Zeilen), ein bis zwei Heap-Allokationen weniger pro längerem Event-Namen.
- **Risiko / Verifikation**: Gering; Semantik (Listener-Pass nur für bestimmte Familien, `HE_HC_LIFECYCLE_EVENT` ohne) per Parameter erhalten. `test_horizoncode_runtime.cpp`, UI-Widget-Tests.

---

## Cross-Modul-Hinweise

- `src/HE_Core/include/HorizonCode/HorizonCode.h:1463` – `Runner(const Graph&, Context ctx)` nimmt den Context per Wert; Voraussetzung für CORE-401 (`const Context&`/`shared_ptr`). `Context` hat ca. 37 `std::function`-Felder (`:1213`).
- `src/HE_Core/src/UIWidget/WidgetManager.cpp:2935`, `:3425` – ruft `eventBindingsOf` pro Element der Treffer-Kette (siehe CORE-405). Nach Einführung des Caches ggf. `listensTo(...)` nutzen.
- `src/HE_Scene/src/Net/PropertyReplicator.cpp:104,138,164,203,216` – `replicatedVariablesOf` pro Entity und Tick, teils nur zur Mitgliedschaftsprüfung (CORE-405).
- `src/HE_Core/include/ContentManager/HAsset.h:437-477` – `Reader::openData` kopiert alle Chunk-Payloads (CORE-411). Eine nicht kopierende `peekChunk`/`peekAssetType`-API wäre auch für `ContentManager` nützlich (jeder Typ-Sniff).
- `istreambuf_iterator` als Datei-Leser steht in mindestens 22 Dateien (u. a. `ContentManager.cpp`, `AssetRefScan.cpp`, `AssetRefRetarget.cpp`, `SceneSerializer.cpp`, `EngineApi.cpp`, `GameApplication.cpp`, `ExportDialogPanel.cpp`). Ein gemeinsamer `readFileBytes` in HE_Core wäre eine Querschnittsmaßnahme.
- `src/HE_Core/include/Hpak/HpakFormat.h` bindet `<functional>`, `<string>`, `<vector>` ein, weil `PackSettings` dort lebt. `HpakReader.h` braucht davon nur `EntryDesc`/`hash64`; `PackSettings` in `HpakWriter.h` auszulagern entlastet den Spiel-Runtime-Build (Runtime-Reader-Include-Kette).
- `src/HE_Game/src/GameApplication.cpp:794` – `IntegrityProbe::start` ohne Priorität (CORE-424).

## Unsicher / Messung nötig

- CORE-401: tatsächliche Kosten von `makeContext` pro Aufruf (Heap-Anteil hängt von der STL ab: libstdc++ SBO 16 B, libc++ 24 B, MSVC größer). Mikrobenchmark 1000 Instanzen × `fireTick`.
- CORE-404: Anzahl Bindings/Watches in realen Projekten; Kosten pro Frame heute.
- CORE-408: ob Aufrufer den `Graph` kopieren oder verschieben; Größe eines typischen Graphen; ob irgendwo `leaf()` mutiert wird.
- CORE-403: Verhältnis Hash-Zeit zu Gesamtzeit eines Streaming-Jobs auf der Zielhardware (der Wert 664 MB/s stammt von dieser 4-Kern-Testmaschine).
- CORE-409: Thread-Sicherheit der Editor-Callbacks `compileShaderVariants`/`compileParticleShaderVariants` und von `HAsset::Reader`/`Writer` (statische Zustände?).
- CORE-414: GPU-Kosten des AutoLandscape-Materials je Backend (Pass-Timer), erst danach Maßnahmen.
- `ParallelForJob`-Allokation pro `parallel_for` (`JobSystem.h:356`: `make_shared` mit Mutex + CV + `exception_ptr`, ca. 120 B pro Aufruf; zusätzlich `copies` Kopien eines `std::function` unter dem Queue-Lock, `JobSystem.cpp:573-577`) und 64 × `yield` in `parallel_for_wait` (`:715`): bei etwa 10-30 Aufrufen pro Frame vermutlich unter 10 µs, deshalb kein eigener Fund. Nur ändern, wenn der Profiler dort etwas zeigt.
- `JobState` ist schwer (Mutex, CV, drei `std::function`, zwei Vektoren, ca. 300 B). Für grobkörnige Streaming-Jobs unproblematisch, für feinkörnige `schedule`-Nutzung messen.
- Die `HpakWriter.cpp`-Annahme, dass einzelne Assets > 2 GiB nie vorkommen: `origSize`/`dataSize` sind `uint32_t` (`:579`, `:1236`), `LZ4_compressBound(int)` (`:593`). Der Reader lehnt >2 GiB ab (`HpakReader.cpp:182`), der Writer prüft es nicht. Wäre ein klarer Fehler statt stillem Abschneiden wert; Auftreten in der Praxis unbekannt.
- `HpakReader::open` lässt bei einem Fehler nach erfolgreichem `m_file.open` (Magic/Version/TOC) den Handle offen (`HpakReader.cpp:62-107`); harmlos bis zum nächsten `open` oder Destruktor, nicht geprüft, ob Aufrufer das ausnutzen.

## Gelesen

Alle 23 Dateien des Slice 4 vollständig gelesen: ja (`HorizonCodeRuntime.cpp` 1-2121, `HorizonCodeRuntime.h`, `HorizonGameServices.h`, `Aes256Gcm.cpp/.h`, `HpakFormat.h`, `HpakReader.cpp/.h`, `HpakWriter.cpp` 1-1277 / `.h`, `KeyDerivation.cpp/.h`, `ProjectConfig.cpp/.h`, `ProjectExporter.cpp` 1-1512 / `.h`, `IGameLogic.h`, `IntegrityProbe.cpp/.h`, `JobSystem.cpp/.h`, `AutoLandscapeMaterial.cpp/.h`). Außerhalb des Slice nur gezielt gegrept: `HAsset.h:437-477`, `HorizonCode.h:1463`, die Aufrufer von `exchangeState/eventBindingsOf/replicatedVariablesOf/fireTick`, `std::queue`-Nutzer.
