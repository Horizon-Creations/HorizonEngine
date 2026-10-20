# Core Slice 1 – AnimatorStateMachine, Application, Audio, BlendSpace, BoneMask, ContentManager (AssetRefRetarget/AssetRefScan/Assets.h)

## Zusammenfassung

Der Slice ist insgesamt sauber, gut kommentiert und überwiegend nicht heiß: der einzige echte Frame-Loop-Code ist `Application::Run` samt `Input`/`InputMapping`/`GameLoop`. Dort gibt es keine groben Verstöße; die relevanten Funde sind (a) das string-basierte Input-API, das pro Frame, Aktion und Spieler Heap-Strings und mehrfache Hash-Lookups erzwingt (Verbraucher in `PlayerHost.cpp`), (b) eine ~700 Zeilen lange `Run()`-Funktion mit eingebettetem Diagnose-Harness, (c) die Asset-Referenz-Werkzeuge (`AssetRefRetarget`/`AssetRefScan`), die viel dupliziert und an mehreren Stellen unnötig kopieren/neu suchen, sowie (d) `GameLogicLoader::load`, das die DLL auch im ausgelieferten Spiel immer kopiert. Die Audio-Envelope (`AudioEnvelope::apply`) läuft auf dem Audio-Thread und ist bereits gut gebaut (Rampen-Raster, templatisierte Schleife); es bleibt nur ein kleiner Doppelaufwand. Alle Funde sind ohne Messdaten geschätzt und entsprechend markiert.

Funde: Hoch 0 · Mittel 9 · Niedrig 15.

---

### CORE-101 – Input-API ist namensbasiert: pro Frame/Aktion/Spieler Heap-Strings und 1–3 Hash-Lookups
- **ID**: CORE-101
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Application/InputAssets.cpp:12-15`, `src/HE_Core/src/Application/InputMapping.cpp:179-220`; Verbraucher `src/HE_Scene/src/PlayerHost.cpp:388,409,410` (außerhalb des Slice)
- **Problem**: `inputEventPressed/Released/Axis/Axis2D` bauen bei jedem Aufruf `"Input." + a + ".Pressed"` neu auf. `PlayerHost` ruft sie im Per-Frame-Pump je Aktion und je Spieler (`fireInputEvent(i, HE::inputEventAxis(a.name), ...)`). Ab einem Aktionsnamen von 5 Zeichen übersteigt der String die SSO-Grenze (16 Byte inkl. "Input." + ".Axis") und allokiert. Zusätzlich löst `PlayerHost` für jede Button-Aktion `justPressed`, `justReleased`, `isPressed` einzeln auf (`getAction(name)` = je ein `unordered_map<std::string,...>::find` mit Hashing des Namens), für Achsen `axisValue`. Das Muster skaliert mit `Aktionen × Spieler × Frames`.
- **Code-Auszug**:
```cpp
std::string inputEventAxis(const std::string& a) { return "Input." + a + ".Axis"; }
...
const InputActionState* InputMapping::getAction(const std::string& name) const
{ auto it = m_actions.find(name); return it != m_actions.end() ? &it->second.state : nullptr; }
```
- **Lösung**: (1) Event-Namen einmal beim Aufbau der Aktionsliste (`PlayerHost.cpp:40-44`, dort wird pro Aktion ohnehin einmal geparst) in die Aktionsbeschreibung legen (`pressedEvent`, `releasedEvent`, `axisEvent`) und nur noch als `const std::string&` weiterreichen. (2) In `InputMapping` ein `const InputActionState* getAction(name)` einmal pro Aktion und Frame nutzen und `justPressed/justReleased/isPressed` aus demselben Zeiger lesen (`const InputActionState* s = m.getAction(a.name); pressed = s && s->justPressed; ...`). Öffentliche API bleibt unverändert, nur die Aufrufer ändern sich.
- **Erwarteter Effekt**: Schätzung: ca. 1–3 Heap-Allokationen und 2–3 Hash-Lookups weniger pro Aktion/Spieler/Frame; bei ~30 Aktionen im Bereich einiger µs/Frame, kein FPS-relevanter Gewinn, aber Wegfall von malloc-Druck im Main-Thread. Messen (PlayerHost-Tick im Profiler).
- **Risiko / Verifikation**: Gering; Event-Namen müssen byte-identisch bleiben (Editor-Katalog `HorizonCode.cpp:1095-1098` nutzt dieselben Funktionen, daher die Funktionen selbst nicht ändern). Bestehende Input-Tests (`tests/` zu InputMapping/PlayerHost) laufen lassen.

### CORE-102 – `retargetTree` blockiert den Editor-Frame: gesamter Content-Baum wird synchron gelesen
- **ID**: CORE-102
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/ContentManager/AssetRefRetarget.cpp:251-315` (Kommentar Zeile 261-262: "this runs inside an editor frame"); Aufrufer `ContentManager.cpp:2928`
- **Problem**: Verschieben/Umbenennen eines Assets läuft `retargetTree` auf dem UI-Thread: rekursiver Walk über alle `.hasset`, jede Datei wird per `fileMentions` komplett gestreamt (256-KB-Blöcke), bei Treffer ganz gelesen, neu geschrieben und per Rename getauscht. Bei einem Projekt mit mehreren GB Meshes/Texturen ist das ein Hitch im Sekundenbereich (Schätzung, nicht gemessen). Das Gegenstück `AssetRefScan::findReferrers` ist ausdrücklich worker-tauglich (`AssetRefScan.h:113-116`) und hat Abbruch-Callback; `retargetTree` hat beides nicht.
- **Code-Auszug**:
```cpp
for (; !ec && it != end; it.increment(ec))
{ ...
  if (!fileMentions(it->path(), rules)) continue;   // liest ggf. die ganze Datei
```
- **Lösung**: Entweder (a) auf einen Worker/Job verlagern und das Ergebnis marshallen (analog `findReferrers`, plus `isCancelled`), oder (b) die Kandidatenmenge vorab über denselben Streaming-Gate-Pass wie `findReferrers` einschränken und nur Treffer umschreiben. Mindestens: Fortschritt/Busy-Zustand anzeigen, damit der Hitch erklärt ist. Verhalten (Temp+Rename-Atomarität) beibehalten.
- **Erwarteter Effekt**: Schätzung: UI-Hitch entfällt; Gesamtzeit unverändert (I/O-gebunden). Messen mit großem Content-Ordner.
- **Risiko / Verifikation**: Mittel – Thread-Sicherheit von `ContentManager`-Folgeschritten (`retargetAssetReferencesInMemory`, Manifest-Update) beachten; Reihenfolge Disk-Rewrite → In-Memory-Update muss bleiben. Bestehende Tests zu `AssetRefs` (`tests/` nach `retarget` durchsuchen) erweitern.

### CORE-103 – `retargetBlob`/`retargetTree` kopieren jeden Chunk mehrfach, auch unveränderte; Datei wird zeichenweise gelesen
- **ID**: CORE-103
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `AssetRefRetarget.cpp:222-249` (insb. 231, 244), `:273-278`; analog `AssetRefScan.cpp:474-480`
- **Problem**: Für jede Datei, die einen Pfad erwähnt (z. B. jedes Mesh mit MREF auf das verschobene Material), liegen gleichzeitig vor: der Datei-`blob`, die vom `Reader` kopierten Chunk-Payloads, die Kopie `std::vector<uint8_t> data = chunk.data;` pro Chunk (auch für Vertex-/Pixel-Chunks, die nie geändert werden), die Kopie im `Writer::addChunk` und das Ergebnis von `toBytes`. Das sind 4–5× die Dateigröße. Zudem liest `blob.assign(std::istreambuf_iterator<char>(f), ...)` (und `readWhole` in `AssetRefScan.cpp:478`) Byte für Byte über den Streambuf; das ist deutlich langsamer als ein `read` in einen vorab dimensionierten Puffer.
- **Code-Auszug**:
```cpp
for (const auto& chunk : reader.chunks())
{
    std::vector<uint8_t> data = chunk.data;      // Kopie, auch wenn nichts geändert wird
    ...
    writer.addChunk(chunk.id, data.data(), data.size());
}
...
blob.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
```
- **Lösung**: Chunk nur bei Treffer kopieren (`const std::vector<uint8_t>* src = &chunk.data; std::vector<uint8_t> changedCopy; ... writer.addChunk(id, src->data(), src->size())`). Datei per `seekg(0,end)/tellg/read` in `blob.resize(size)` lesen (gemeinsamer Helfer `readFileBytes` für Retarget und Scan, siehe CORE-109).
- **Erwarteter Effekt**: Schätzung: Spitzen-RAM für große Assets um ~50 % kleiner, Lesen mehrfach schneller; nur bei Move/Delete-Dialog relevant.
- **Risiko / Verifikation**: Gering; Ausgabe muss byte-identisch bleiben (Test: Blob mit Mesh+MREF vor/nach vergleichen, außer dem geänderten Chunk).

### CORE-104 – Digit-Run-Gate erzeugt für jeden Ziffernlauf in Binärdaten einen `std::string` und hasht ihn
- **ID**: CORE-104
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/ContentManager/AssetRefScan.cpp:167-191` (`digitRunMatches`, `blockMentions`), ebenso `nextHit` `:412-421`
- **Problem**: Das Gate scannt jede Projektdatei (bis GB) byte-weise; bei UUID-Zielen wird für jeden Lauf aufeinanderfolgender Bytes `'0'..'9'` ein `std::string(hay.substr(...))` gebaut und per `unordered_set::count` gesucht. In Binärdaten (Vertexpuffer, Pixel) sind ~4 % der Bytes Ziffern, also startet etwa jedes 25. Byte einen Lauf mit Konstruktion plus Hash. Der Kommentar (Z. 167-170) nennt selbst, dass ein `hi` 19–20 Ziffern lang ist; Läufe anderer Länge können nie treffen.
- **Code-Auszug**:
```cpp
const std::size_t len = i - start;
if (len == 0) return false;
return q.uuidDigits.count(std::string(hay.substr(start, len))) > 0;
```
- **Lösung**: In `compile()` `minDigits/maxDigits` über `uuidDigits` merken und in `digitRunMatches` sofort `return false`, wenn `len < minDigits || len > maxDigits`; Lookup nur danach (mit `std::string_view`-Heterogenous-Lookup oder C++20 `unordered_set::contains` mit transparentem Hash, sonst ein auf dem Stack gehaltener Puffer). Ergebnisse bleiben identisch, weil kürzere/längere Läufe nie in der Menge sind.
- **Erwarteter Effekt**: Schätzung: Gate-Durchsatz auf Binärdaten spürbar höher (Hashing/Konstruktion entfällt für ~alle Läufe); Größenordnung 2–5× auf Mesh/Textur-Daten, zu messen (z. B. 1 GB Content, Delete-Dialog eines Ordners).
- **Risiko / Verifikation**: Gering; `tests/` zu `findReferrers` (UUID-Treffer in Scene/ASMG/PTGR/PFAB) müssen unverändert grün sein.

### CORE-105 – `nextHit` sucht bei jedem Treffer alle Needles ab `start` neu (potenziell quadratisch)
- **ID**: CORE-105
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `AssetRefRetarget.cpp:108-119` (genutzt in `:130-166`), `AssetRefScan.cpp:403-423` (genutzt in `:434-466`)
- **Problem**: Jeder Aufruf führt für jede Regel/jeden Needle `hay.find(needle, start)` aus. Hat eine Regel keinen oder nur einen weit entfernten Treffer, wird der Rest des Chunks bei jedem Treffer der anderen Regel erneut durchsucht: O(Treffer × Chunkgröße). `moveRules` erzeugt systematisch zwei Regeln, deren zweite (`Content/<rel>`) die erste als Teilstring enthält; jede Fundstelle der zweiten ist auch eine der ersten (Offset +8), die Suchen verdoppeln sich also ohne Not. In `AssetRefScan::nextHit` kommt der Ziffernscan bis `best` hinzu, der pro Aufruf wieder von `start` läuft.
- **Code-Auszug**:
```cpp
for (const Rule& r : rules)
{
    if (r.from.empty()) continue;
    const size_t p = hay.find(r.from, start);
    if (p < best) best = p;
}
```
- **Lösung**: Pro Regel die zuletzt gefundene Position cachen (`std::vector<size_t> next`) und nur neu suchen, wenn `next[i] < start`. Bei Scan zusätzlich die Position des nächsten UUID-Treffers cachen. Ergebnis (kleinster Offset) bleibt gleich.
- **Erwarteter Effekt**: Schätzung: bei Chunks mit vielen Treffern von O(n·k) zu O(n); in der Praxis selten extrem, daher nur Mittel. Messen mit einem MTRL-Chunk mit großem Node-Graph-JSON und vielen Pfaden.
- **Risiko / Verifikation**: Gering; Test mit Chunks, die Treffer beider Regeln mischen.

### CORE-106 – `Application::Run` ist ~700 Zeilen lang; Diagnose-Harness steckt inline im Frame-Loop
- **ID**: CORE-106
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Application/Application.cpp:231-934`; Harness-Blöcke `:448-473` (HE_EXIT_AFTER_FRAMES), `:475-548` (HE_PROFILE_CAPTURE), `:550-594` (HE_CAPTURE_FRAME), `:625-680` und `:686-690` (HE_PERF_INPUT_EVENTS), `:47-172` (InputLatencyProbe)
- **Problem**: Der Haupt-Loop mischt Fensterverwaltung, Event-Driven-Scheduling, Profiler, Hitch-Detektor, drei env-gesteuerte Test-/Perf-Hilfen (je mit eigenem `static const` Lambda-Parse und eigenem Zustand), GPU-Stats-Abholung und Frame-Cap/Background-Throttle. Das sind ~250 Zeilen reiner Diagnosecode im Hot-Path-Rumpf, die mit jedem Frame durchlaufen werden (zwar billig: Static-Guard-Check, Vergleich), aber den eigentlichen Kontrollfluss (`runThisFrame`/`present`) schwer lesbar machen. Dazu eine 125-Zeilen-`InputLatencyProbe`-Klasse im selben File.
- **Code-Auszug**:
```cpp
{
    static const unsigned long long kExitAfter = []() -> unsigned long long { ... getenv ... }();
    if (kExitAfter != 0 && m_frameIndex >= kExitAfter) { ...; m_running = false; }
}
{
    struct AutoCapture { ... };
    static const AutoCapture kAuto = [] { ... }();
    if (kAuto.frames != 0) { ... }
}
```
- **Lösung**: Neue interne Klasse (z. B. `Application/FrameDiagnostics.{h,cpp}`) mit `FrameDiagnostics::fromEnvironment()` (liest alle `HE_*` einmal im Konstruktor) und Methoden `beginFrame(Application&, frameIndex)`, `injectSyntheticInput(Window&)`, `onCommitted()`. `InputLatencyProbe` dorthin verschieben. `Run()` zerfällt in `initWindowAndRenderer()`, `runFrame()` (Entscheidung ob/was gezeichnet wird), `collectProfilerStats()`, `throttle()`. Verhalten unverändert; nur Extraktion.
- **Erwarteter Effekt**: Reine Lesbarkeit/Wartbarkeit; Laufzeit unverändert (minus ein paar Branch-Checks pro Frame, nicht messbar).
- **Risiko / Verifikation**: Gering-mittel (Reihenfolge der Blöcke im Frame ist semantisch: Capture VOR dem Zeichnen, Profiler `beginFrame` nach Hitch-Check, `WaitForFrame` vor `PollEvents`). Verifikation: `HE_EXIT_AFTER_FRAMES`, `HE_PROFILE_CAPTURE`, `HE_CAPTURE_FRAME` Läufe vor/nach vergleichen (Exit-Code, Dump, PPM).

### CORE-107 – `GameLogicLoader::load` kopiert die DLL immer, auch im ausgelieferten Spiel
- **ID**: CORE-107
- **Kategorie**: E / A
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/Application/GameLogicLoader.cpp:24-43`, Header-Kommentar `GameLogicLoader.h:13-15`
- **Problem**: Der Header sagt "In packaged game: load once, never reload". `load()` legt trotzdem bei jedem Start eine `GameLogic.hot-0001.<ext>`-Kopie neben der Original-DLL an (`copy_file`, `overwrite_existing`). Im Spiel ist das unnötiger Start-I/O und in nur lesbaren Installationsverzeichnissen ein stiller Fehlschlag (`if (!ec) loadPath = copy;` – Fallback greift, aber jedes Mal ein fehlgeschlagener Versuch); bei hartem Prozessende bleibt die Kopie liegen (Entfernung nur in `unload`). Auf macOS/Windows ist die Kopie im Editor nötig (dyld-Cache, Dateilock), im Spiel nicht.
- **Code-Auszug**:
```cpp
static unsigned s_loadCounter = 0;
std::filesystem::path loadPath = dllPath;
{
    ++s_loadCounter;
    ...
    std::filesystem::copy_file(dllPath, copy, std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) loadPath = copy;
}
```
- **Lösung**: Ladeoption (`enum class LoadMode { Direct, HotCopy }`, Default `Direct`) einführen und nur `reload`/`reloadAndStart` sowie den Editor-Erstladeaufruf mit `HotCopy` rufen. `s_loadCounter` dabei als `std::atomic<unsigned>` oder in die Instanz verlegen (statische, nicht thread-sichere Variable, falls je mehrere Loader).
- **Erwarteter Effekt**: Schätzung: Spielstart spart eine Dateikopie (Größe der Game-DLL, typ. wenige MB); keine Restdateien nach Crash.
- **Risiko / Verifikation**: Mittel – Editor muss weiter kopieren (Windows-Lock). Aufrufer von `load/loadAndStart` im Editor (`grep loadAndStart src/HE_Editor`) prüfen; Hot-Reload-Test im Editor manuell.

### CORE-108 – `meshSectionsOf` gibt die Section-Tabelle per Wert zurück (Heap-Kopie pro Aufruf)
- **ID**: CORE-108
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/ContentManager/Assets.h:191-201`; Hot-Path-Aufrufer `src/HE_Rendering/src/RenderExtractor.cpp:263` (`lod0SectionsOf`)
- **Problem**: Die Funktion kopiert `mesh.sections` (Vektor mit je einem `std::string materialPath`) bzw. baut für leere Tabellen ein temporäres Ein-Element-Vektor-Objekt (Heap). Im Extractor läuft sie in `extractMeshes` für Entitäten, deren gezeichnetes LOD nicht LOD0 ist, also potenziell pro Entität und Frame; der Kommentar dort sagt "A copy" bewusst (Zeigerstabilität), die Kopie geht aber über die Strings.
- **Code-Auszug**:
```cpp
template<typename Mesh>
std::vector<MeshSection> meshSectionsOf(const Mesh& mesh)
{
    if (!mesh.sections.empty()) return mesh.sections;
    MeshSection whole; ...
    return { whole };
}
```
- **Lösung**: Für den Extractor ist nur `materialId`/`materialPath` pro Slot nötig (`lod0Material(slot)`): LOD0-Material-Ids je LOD0-Mesh einmal pro Frame cachen (Map `lod0 meshId → vector<UUID>`), oder eine Variante `forEachMeshSection(mesh, fn)` anbieten, die die Tabelle ohne Kopie durchläuft. Die by-value-Variante für Inspector bleibt.
- **Erwarteter Effekt**: Schätzung: pro betroffener Entität/Frame 1–N Allokationen weniger; Gewinn nur bei vielen Entitäten mit nicht-LOD0-Darstellung. Messen mit LOD-Stresszene.
- **Risiko / Verifikation**: Gering-mittel; Slot-Mapping (`lodSlotMap`) darf sich nicht ändern. RenderExtractor-/LOD-Tests ausführen.

### CORE-109 – Retarget und Scan duplizieren Streaming-Gate, Chunk-Klassifikation und Binär-Walker; Chunk-Listen sind bereits auseinandergelaufen
- **ID**: CORE-109
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `AssetRefRetarget.cpp:32-37, 39, 61-76, 81-105, 108-119` vs. `AssetRefScan.cpp:29-36, 75, 204-248, 373-401, 403-423`
- **Problem**: `fileMentions` ≙ `gateFile`, `isJsonChunk` ≙ `isJsonPayloadChunk`, `readU32`, `enclosingJsonAt` und `nextHit` existieren zweimal mit leichten Abweichungen (Scan hat Lookback-Cap und `gaveUpAtCap`). Die Chunk-Listen divergieren bereits: `isJsonPayloadChunk` führt `CHUNK_STDF/ENDF/SGTP`, `isJsonChunk` nicht (der Scan-Kommentar `AssetRefScan.cpp:24-28` nennt das selbst). Wird ein Asset verschoben, das in einem dieser Chunk-Typen steht, behandelt der Retarget-Pfad es als Binärchunk; ob dort Pfade stehen, ist hier nicht verifiziert (siehe "Unsicher"). Jede künftige Chunk-Änderung muss an zwei Stellen nachgezogen werden.
- **Code-Auszug**:
```cpp
// Retarget
return id == HAsset::CHUNK_MGRF || id == HAsset::CHUNK_UIWT || ... || id == HAsset::CHUNK_SEQU;
// Scan
return id == HAsset::CHUNK_MGRF || ... || id == HAsset::CHUNK_STDF || id == HAsset::CHUNK_ENDF || id == HAsset::CHUNK_SGTP || id == HAsset::CHUNK_SEQU;
```
- **Lösung**: Internen Header `ContentManager/AssetRefChunks.h` (nicht `HE_API`) mit `isJsonPayloadChunk`, `readU32`, `enclosingJsonAt(…, maxLookback)` und einer gemeinsamen `streamContains(file, needles)`-Funktion; beide Dateien nutzen sie. Entscheidung treffen, ob STDF/ENDF/SGTP im Retarget als JSON zu behandeln sind (Test!). Oder, wenn bewusst anders: im gemeinsamen Header dokumentieren.
- **Erwarteter Effekt**: ~120 Zeilen weniger, ein Ort für Chunk-Klassifikation; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Mittel, wenn die Klassifikation vereinheitlicht wird (Verhaltensänderung für STDF/ENDF/SGTP). Test: Struct-/Enum-/SaveGame-Asset mit Pfadreferenz verschieben und Re-Scan.

### CORE-110 – `GameLoop::tick`: redundantes `if (logic)`
- **ID**: CORE-110
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Application/GameLoop.cpp:21,26`
- **Problem**: Nach dem Early-Return `if (!logic) { ...; return m_running; }` (Z. 21) ist `logic` garantiert nicht null; die Prüfung in der Schleife (Z. 26) ist tot.
- **Code-Auszug**:
```cpp
if (!logic) { m_accumulator = 0.0f; return m_running; }
...
if (logic) logic->onUpdate(world, m_config.fixedTimestep);
```
- **Lösung**: Zeile 26 zu `logic->onUpdate(world, m_config.fixedTimestep);`.
- **Erwarteter Effekt**: Nur Lesbarkeit.
- **Risiko / Verifikation**: Keines; `tests/` zu GameLoop.

### CORE-111 – Profiler-Capture: pro Frame drei Vektor-Allokationen/-Kopien der GPU-Pass-Liste
- **ID**: CORE-111
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `Application.cpp:809-830` (`gs = m_renderer->GetFrameGpuStats()`, `std::vector<ProfGpuPass> passes`), `EngineProfiler.cpp:458-465` (`m_current.gpuPasses = passes`)
- **Problem**: Während einer Aufnahme (oder bei offenem Live-HUD) wird pro Frame `FrameGpuStats` samt `passes`-Vektor zurückgegeben, dann in einen zweiten `ProfGpuPass`-Vektor (mit `reserve`) umkopiert und beim Setzen nochmals kopiert. Das stört genau die Messung, für die es gedacht ist (kleine, konstante Allokationslast im Capture).
- **Code-Auszug**:
```cpp
std::vector<ProfGpuPass> passes;
passes.reserve(gs.passes.size());
for (const auto& p : gs.passes) passes.push_back({ p.name, p.ms, p.approx });
profiler.setGpuTimes(gs.gpuFrameMs, passes, gs.gpuTimingMode);
```
- **Lösung**: Den `passes`-Vektor als Member von `Application` wiederverwenden (`m_profPassScratch.clear(); ...`) und `setGpuTimes` per `std::vector<ProfGpuPass>&&`/Span übernehmen lassen, oder `GpuPassTime` und `ProfGpuPass` auf einen Typ vereinheitlichen.
- **Erwarteter Effekt**: Schätzung: 2 Allokationen/Frame weniger nur während Capture; im µs-Bereich.
- **Risiko / Verifikation**: Gering; Profiler-Dump-Test.

### CORE-112 – `AudioEnvelope`: jeder Raster-Gain-Punkt wird pro Block doppelt ausgewertet
- **ID**: CORE-112
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Audio/AudioEdit.cpp:209-235` (Aufrufer auf dem Audio-Thread: `HE_Scene/src/AudioEngine.cpp:287` `curve->apply`)
- **Problem**: `rampBlock(env, block, sr)` wertet `g0 = evalGain(block*128)` und `g1 = evalGain((block+1)*128)` aus (je ein `upper_bound`, bei Exponential zusätzlich 2× `log10` + `pow`). Der nächste Block berechnet sein `g0` identisch zum vorigen `g1` neu. Die Schleife in `forEachRampedGain` kann `g1` mitnehmen. Pro 128 Frames ist das ein Binärsuch-/ggf. Transzendental-Aufruf zu viel; der Gain-Pfad pro Frame (Multiplikation, Konversion) ist deutlich teurer, also Niedrig. (Die Division durch `kRampFrames` ist eine Zweierpotenz-Konstante und wird vom Compiler exakt zur Multiplikation, daher kein Fund.)
- **Code-Auszug**:
```cpp
const double g0 = env.evalGain(static_cast<double>(f0) / sampleRate);
const double g1 = env.evalGain(static_cast<double>(f0 + AudioEnvelope::kRampFrames) / sampleRate);
return RampBlock{ g0, g1 - g0 };
```
- **Lösung**: In `forEachRampedGain` `g1` des vorigen Blocks als `g0` des nächsten wiederverwenden (innerhalb eines Aufrufs; über Aufrufe hinweg bleibt die Funktion rein, wie dokumentiert). Das Ergebnis ist bit-identisch, solange `evalGain` deterministisch ist.
- **Erwarteter Effekt**: Schätzung: ~50 % der `evalGain`-Aufrufe pro `apply`; im Verhältnis zur Frame-Schleife wenige Prozent. Messen auf Audio-Thread nur bei vielen Stimmen mit Kurve.
- **Risiko / Verifikation**: Gering; `tests/test_audio_edit.cpp` (bit-gleiche Gains für `rampedGain` vs. `apply` bleiben Pflicht – `rampedGain` selbst weiter auf `rampBlock` stützen).

### CORE-113 – `retargetJsonText` baut für jedes JSON-String-Literal eine neue `std::string`-Kopie
- **ID**: CORE-113
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `AssetRefRetarget.cpp:189-220`
- **Problem**: Der Scanner kopiert Zeichen für Zeichen jeden String-Body in `raw` (Heap ab 16 Zeichen), um ihn mit den Regeln zu vergleichen, und hängt ihn danach unverändert wieder an `out`. Bei MB-großen Node-Graph-JSONs entstehen so Zehntausende Allokationen, obwohl nur ein Bruchteil trifft.
- **Code-Auszug**:
```cpp
std::string raw;
...
raw += text[j++];
...
if (retargetValue(raw, rules)) changed = true;
out += '"'; out += raw; out += '"';
```
- **Lösung**: Body als `std::string_view` (Anfang/Ende) ermitteln, `retargetValue` auf `string_view` anbieten (Vergleich + Rückgabe von `optional<string>` nur bei Treffer) und unveränderte Bereiche per `out.append(text, i, j - i + 1)` blockweise kopieren.
- **Erwarteter Effekt**: Schätzung: wenige Allokationen statt einer pro String; nur beim Move relevant.
- **Risiko / Verifikation**: Gering; Ausgabe muss byte-identisch bleiben (inkl. Escapes).

### CORE-114 – `value(key, json::array())` kopiert das ganze Array bei jedem Laden
- **ID**: CORE-114
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `AnimatorStateMachineGraph.cpp:86,93`; `BlendSpace.cpp:50`; `BoneMask.cpp:30`; Zusatz `AnimatorStateMachineGraph.cpp:65-69`
- **Problem**: `nlohmann::json::value(key, default)` liefert für den Typ `json` eine Kopie des gesamten Teilbaums; die Range-for iteriert dann über die Kopie. Beim Laden jedes Assets wird das States-/Transitions-/Samples-Array einmal tief kopiert. Beim Schreiben wird `j["states"]` per `push_back` aufgebaut und bei leerem Array nachträglich auf `array()` gesetzt.
- **Code-Auszug**:
```cpp
for (const auto& sj : j.value("states", nlohmann::json::array()))
...
for (const auto& s : g.states) j["states"].push_back(stateToJsonObj(s));
if (g.states.empty()) j["states"] = nlohmann::json::array();
```
- **Lösung**: `if (auto it = j.find("states"); it != j.end() && it->is_array()) for (const auto& sj : *it) ...`. Beim Schreiben `j["states"] = nlohmann::json::array();` vorab setzen und dann pushen (entfällt der `empty()`-Fix).
- **Erwarteter Effekt**: Eine tiefe Kopie weniger pro Asset-Load; Lade-/Hot-Reload-Pfad, nicht pro Frame.
- **Risiko / Verifikation**: Gering; beachten, dass `value()` bei falschem Typ (z. B. `"states": 3`) eine Exception wirft, `find+is_array` überspringt still – bewusst entscheiden (gutmütiger). Round-trip-Tests für alle drei Formate.

### CORE-115 – `inputActionIsAxis`, `inputActionIsAxis2D`, `inputActionRunsWhilePaused` parsen dasselbe JSON dreimal
- **ID**: CORE-115
- **Kategorie**: D / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `InputAssets.cpp:19-39`; Aufrufmuster `PlayerHost.cpp:40-44`, `McpToolsInput.cpp:537-543,565-571`
- **Problem**: Die Aufrufer stellen immer dieselbe Dreier-Frage (Axis2D? Axis? runWhilePaused?) zu einem Payload; jeder Aufruf parst das JSON neu.
- **Code-Auszug**:
```cpp
const ActionKind k = HE::inputActionIsAxis2D(a->json) ? ActionKind::Axis2D
                   : HE::inputActionIsAxis(a->json)   ? ActionKind::Axis : ...
... HE::inputActionRunsWhilePaused(a->json)
```
- **Lösung**: `struct InputActionInfo { std::string valueType; bool runWhilePaused; }; InputActionInfo parseInputActionInfo(const std::string&)` einmal parsen; die drei bestehenden Funktionen bleiben als dünne Wrapper (API-kompatibel).
- **Erwarteter Effekt**: Zwei Parses weniger pro Aktion bei Sessionstart; vernachlässigbar, aber einfacher.
- **Risiko / Verifikation**: Gering; bestehende Tests der drei Funktionen.

### CORE-116 – `applyInputMappingContext`: redundante Vorab-Bedingung und wiederholte `contains`/`operator[]`-Lookups
- **ID**: CORE-116
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Application/InputAssets.cpp:188-231`, `:164-176,287`
- **Problem**: Die äußere Bedingung (`keys || gamepadButtons || mouseButtons` ist Array) ist überflüssig: die drei inneren `if`s prüfen dasselbe erneut, und bei leerem `binds` passiert ohnehin nichts. Jedes Feld wird zweimal nachgeschlagen (`contains` + `operator[]`), `j["entries"]` dreimal; `readAxes` wird je Entry als Lambda neu erzeugt.
- **Code-Auszug**:
```cpp
if ((e.contains("keys") && e["keys"].is_array()) ||
    (e.contains("gamepadButtons") && e["gamepadButtons"].is_array()) ||
    (e.contains("mouseButtons") && e["mouseButtons"].is_array()))
{ std::vector<ActionBinding> binds; if (e.contains("keys") && e["keys"].is_array()) ...
```
- **Lösung**: Hilfsfunktion `const json* arrayOf(const json& e, const char* key)` (find + is_array), `readAxes` und neue `readButtonBindings` als freie Funktionen; Vorab-Bedingung entfernen. Funktionsverhalten identisch.
- **Erwarteter Effekt**: ~25 Zeilen weniger, Funktion unter 100 Zeilen.
- **Risiko / Verifikation**: Gering; Mapping-Context-Tests inkl. Axis2D und Replace/Union.

### CORE-117 – Doppelte Hilfsfunktionen: `sameBinding` (2×), `xmlEscape` (3×), PNG-Dekodierung (2×)
- **ID**: CORE-117
- **Kategorie**: F / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `InputMapping.cpp:31-35` vs. `InputRebind.cpp:29-33`; `Autostart.cpp:25-39` vs. `DocumentTypes.cpp:8-22` (dritte Kopie `HE_Net/src/PortMapper.cpp:56`); `AppIcon.cpp:281-294` vs. `296-312`
- **Problem**: `ActionBinding`-Vergleich existiert privat und exportiert (`HE::sameBinding`); `xmlEscape` ist wortgleich in zwei Dateien dieses Slice; `heSetWindowIcon` wiederholt `exists`+`stbi_load` aus `heLoadPngRGBA`.
- **Code-Auszug**:
```cpp
bool sameBinding(const ActionBinding& a, const ActionBinding& b)
{ return a.key == b.key && a.gamepadButton == b.gamepadButton && a.mouseButton == b.mouseButton; }
```
- **Lösung**: `operator==` für `ActionBinding` ist laut Kommentar bewusst vermieden (Aggregat); stattdessen `sameBinding(ActionBinding, ActionBinding)` aus `InputRebind.h` auch in `InputMapping.cpp` verwenden (oder in `InputMapping.h` deklarieren). `xmlEscape` in eine interne `Util/XmlEscape.h` (inline) legen. `heSetWindowIcon` per `heLoadPngRGBA` + `SDL_CreateSurfaceFrom` auf dem Vektor (ein zusätzliches memcpy, vernachlässigbar bei Icon-Größe).
- **Erwarteter Effekt**: ~40 Zeilen weniger, eine Stelle für Escape-Regeln.
- **Risiko / Verifikation**: Gering; Tests für `heAutostartText`, `heInfoPlistDocumentTypes`, `sameBinding`.

### CORE-118 – `BindingCapture` scannt pro Frame alle 512 Scancodes dreifach; `bindingConflicts` kopiert und sortiert alle Namen
- **ID**: CORE-118
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `InputRebind.cpp:217-224,270-272` (Listening/Arming), `InputRebind.cpp:55-94`
- **Problem**: Während "Listening" läuft pro Frame `keyEdge` über alle `SDL_SCANCODE_COUNT` (~512) Codes plus `snapshot` (nochmals 512 + 26 Calls mit Cast und Geräte-Gate). Nur in einer Rebind-Session aktiv, also selten. `bindingConflicts` holt `actionNames()` und `axisNames()` (kopiert alle Strings, sortiert), schlägt dann jeden Namen erneut in der Map nach und sortiert/uniquet das Ergebnis nochmals; die Funktion läuft einmal pro Capture, nicht pro Frame.
- **Code-Auszug**:
```cpp
for (int sc = 1; sc < SDL_SCANCODE_COUNT && what == Result::None; ++sc)
    if (sc != SDL_SCANCODE_ESCAPE && keyEdge(sc)) ...
...
for (const std::string& name : mapping.actionNames()) { ... mapping.actionBindings(name) ...
```
- **Lösung**: Optional: eine Const-Iteration über die Binding-Maps als `forEachAction(fn)` in `InputMapping` anbieten und danach nur das (kleine) Ergebnis sortieren. Scancode-Scan lässt sich über ein `Input::keysDownBitset()` auf das Delta zum Snapshot reduzieren. Beides nur nötig, wenn Rebind-UI messbar auffällt.
- **Erwarteter Effekt**: Vernachlässigbar (µs, nur in Rebind-Dialog).
- **Risiko / Verifikation**: Gering; deterministische Reihenfolge (sortiert) beibehalten.

### CORE-119 – Splash: synchroner Alpha-BBox-Scan über 1024² Pixel und doppeltes UTF-8-Dekodieren je Textzeile
- **ID**: CORE-119
- **Kategorie**: D / F
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `SplashScreen.cpp:163-198` (BBox), `:242-279` (`textWidth` + `text`), `:289-407` (`draw`)
- **Problem**: Das Logo (1024² laut Kommentar Z. 158) wird beim Start vollständig dekodiert und zweimal verarbeitet (BBox-Scan mit 1 M Pixel-Zugriffen, dann `crop` per memcpy); `draw()` ruft `SDL_PumpEvents` und rendert 64+48+32 Rechtecke bei jedem `setStatus`. `text()` ruft `textWidth` auf (Dekodierung) und dekodiert danach nochmals. Kritisch ist nur, dass das alles auf dem kritischen Startpfad VOR dem Fenster liegt (Kommentar `Application.cpp:277-279`); in einem Debug-Build ist der BBox-Scan spürbar. Messen.
- **Code-Auszug**:
```cpp
for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
        if (px[(static_cast<size_t>(y) * w + x) * 4 + 3] > 8) { x0 = std::min(x0, x); ... }
```
- **Lösung**: BBox mit Zeilen-Early-Exit (obere/untere Kante von außen nach innen suchen, danach Spalten) oder den Crop beim Asset-Build vorab erledigen (HC_Logo.png schon beschnitten ablegen). `text()` den Rückgabewert aus der Zeichenschleife berechnen statt `textWidth` vorzuschalten (die Breite braucht nur der Aufrufer `draw`, der ohnehin `textWidth` für das Zentrieren ruft).
- **Erwarteter Effekt**: Schätzung: wenige ms Startzeit (Release), mehr in Debug. Unsicher, messen.
- **Risiko / Verifikation**: Gering; `HE_SPLASH_DUMP` Frames vor/nach vergleichen.

### CORE-120 – `Input::PollGamepads`: pro Pad und Frame ~32 einzelne SDL-Abfragen (jeweils mit Joystick-Lock)
- **ID**: CORE-120
- **Kategorie**: D / E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Application/Input.cpp:223-246`
- **Problem**: Pro offenem Pad und Frame werden 6 Achsen und ~26 Buttons einzeln über `SDL_GetGamepadAxis/Button` gelesen (SDL3 nimmt dabei intern den Joystick-Lock je Aufruf), danach `remerge()` über alle 8 Slots. Ohne Pad greift das frühe `anyOpenPad()`-Return, der Normalfall ist also billig; mit 1–4 Pads ca. 130 Locks/Frame (Schätzung, SDL-Internas nicht verifiziert).
- **Code-Auszug**:
```cpp
for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
    one.axes[a] = static_cast<float>(SDL_GetGamepadAxis(s.pad, static_cast<SDL_GamepadAxis>(a))) / 32767.0f;
for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
    one.buttons[b] = SDL_GetGamepadButton(s.pad, static_cast<SDL_GamepadButton>(b));
```
- **Lösung**: Nur messen; falls auffällig, Zustand aus `SDL_EVENT_GAMEPAD_AXIS_MOTION/BUTTON_*` ableiten (Events kommen ohnehin durch `PollEvents`), und `remerge()` nur bei Änderung. Das ändert aber die Semantik "Snapshot per Frame" und sollte nur mit Messbeleg angegangen werden.
- **Erwarteter Effekt**: Unsicher; vermutlich < 20 µs/Frame.
- **Risiko / Verifikation**: Mittel bei Umbau (Verhalten bei Hot-Plug und Fokusverlust); daher nicht ohne Messung.

### CORE-121 – Schwere Header: `Application.h` und `Assets.h` ziehen SDL, UIFont, Renderer, ContentManager, AudioEdit mit
- **ID**: CORE-121
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `Application.h:3-13` (u. a. `SplashScreen.h` → `Renderer/UIFont.h`, `Input.h` → `<SDL3/SDL.h>`), `Input.h:4` (`<unordered_map>` ungenutzt), `InputMapping.h:8,10` (`<algorithm>` im Header ungenutzt, `class Input;` redundant nach `#include Input.h`), `Assets.h:9-10` (`Scripting/ScriptTypes.h`, `Audio/AudioEdit.h` mit `<functional>`), `AudioBusConfig.h:3` (volles `AudioEdit.h` nur für `AudioEq`)
- **Problem**: `Assets.h` ist der Umbrella-Header aller Asset-Typen und wird breit eingebunden; `AudioEdit.h` wird nur für das by-value-Member `AudioAsset::edit` gebraucht. `SplashScreen.h` hält `BakedUIFont` by value und zwingt so `UIFont.h` in jede Datei, die `Application.h` einbindet.
- **Code-Auszug**:
```cpp
#include "Scripting/ScriptTypes.h"
#include "Audio/AudioEdit.h"        // Assets.h
```
- **Lösung**: Ungenutzte Includes entfernen (`<unordered_map>` in `Input.h`, `<algorithm>` und `class Input;` in `InputMapping.h`); `SplashScreen` per Pimpl/`unique_ptr<Impl>` kapseln, damit `Application.h` kein `UIFont.h` braucht; `AudioEq` in `AudioEq.h` abspalten (`AudioBusConfig.h` braucht nur das). Erst mit Build-Zeitmessung (z. B. `-ftime-trace`) priorisieren.
- **Erwarteter Effekt**: Kürzere Inkrementell-Builds; Größe unbekannt, messen.
- **Risiko / Verifikation**: Gering (Compilerfehler zeigen fehlende Includes); alle Targets bauen.

### CORE-122 – `AudioEq::responseDb` wird nur von Tests benutzt und rechnet pro Aufruf alle Bänder neu aus
- **ID**: CORE-122
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `AudioEdit.cpp:400-410`, Header `AudioEdit.h:239-240`
- **Problem**: Die Funktion ruft pro Frequenzpunkt `biquadCoefficients` (cos, sin, pow) pro Band neu auf. Im Editor macht `AudioMixView.cpp:199` es richtig (Koeffizienten einmal, dann `magnitudeDb` je Punkt); `responseDb` selbst hat im Produktivcode keinen Aufrufer (nur `tests/test_audio_bus_eq.cpp`, `test_audio_edit.cpp`, `test_ui_shot.cpp`).
- **Code-Auszug**:
```cpp
for (const AudioEqBand& b : bands)
{
    const BiquadCoeffs c = biquadCoefficients(b, sampleRate);
    if (!c.isIdentity()) db += c.magnitudeDb(freqHz, sampleRate);
}
```
- **Lösung**: Entweder als Test-Hilfsfunktion belassen und kommentieren, oder eine `responseCurveDb(const AudioEq&, rate, span<freq>, span<out>)`-Variante anbieten, die Koeffizienten einmal berechnet, und `AudioMixView` darauf umstellen.
- **Erwarteter Effekt**: Kein Produktivgewinn; verhindert, dass jemand `responseDb` in eine Zeichenschleife steckt.
- **Risiko / Verifikation**: Gering.

### CORE-123 – `AppIcon`: pro Pixel `sqrt` für die Plate-Coverage, zusätzliche Kopie im PNG-Encoder
- **ID**: CORE-123
- **Kategorie**: D / C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `AppIcon.cpp:28-41,88-101` (1024²-Rendering), `:52-60` (`encodePng` kopiert `stbi_write_png_to_mem`-Puffer in einen Vektor)
- **Problem**: Der Exportpfad (`ProjectExporter.cpp:1340-1384`, einmal je Export) rendert 7 Größen bis 1024² und berechnet für jedes Pixel `std::sqrt` auch im Inneren der Plate, wo die Coverage 1 ist. Außerdem wird das PNG-Ergebnis einmal von stb in den Vektor kopiert.
- **Code-Auszug**:
```cpp
const float d  = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
return std::clamp(r + 0.5f - d, 0.0f, 1.0f);
```
- **Lösung**: In `plateCoverage` zuerst auf quadratischen Abstand prüfen: `d2 <= (r-0.5)²` → 1.0 und `d2 >= (r+0.5)²` → 0.0 ohne `sqrt`. Encoder-Kopie ist bei Icon-Größe vernachlässigbar; belassen.
- **Erwarteter Effekt**: Schätzung: Icon-Rendering ~2× schneller; Exportdauer praktisch unverändert (Sekundenbruchteile).
- **Risiko / Verifikation**: Gering; Pixelvergleich-Test des bestehenden AppIcon-Tests (muss bit-gleich bleiben, die Randbereiche rechnen weiter wie bisher).

### CORE-124 – `Input::SetGamepadFrame(slot, frame)`: verschachtelter Ternary ersetzt einfache Zustandslogik
- **ID**: CORE-124
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Application/Input.cpp:248-261`
- **Problem**: Drei Zustände in einem verschachtelten `?:` plus nachgelagertes `if (!frame.connected && !s.pad) s.frame = ...`. Schwer zu lesen; die Zustandsänderung ist "connected → Connected; sonst Free bleibt Free, alles andere Reserved".
- **Code-Auszug**:
```cpp
s.state = frame.connected ? GamepadSlotState::Connected
        : s.state == GamepadSlotState::Free ? GamepadSlotState::Free
                                             : GamepadSlotState::Reserved;
```
- **Lösung**: `if (frame.connected) s.state = Connected; else if (s.state != Free) s.state = Reserved;` und bei `!connected` direkt `s.frame = GamepadFrame{}` im selben Zweig.
- **Erwarteter Effekt**: Lesbarkeit.
- **Risiko / Verifikation**: Keines; bestehende Pad-Slot-Tests (`tests/` zu SetGamepadFrame).

---

## Cross-Modul-Hinweise

- `src/HE_Scene/src/PlayerHost.cpp:388,409,410`: Event-Namen werden pro Frame, Aktion und Spieler neu gebaut (siehe CORE-101); `PlayerHost.cpp:40-44` ruft drei JSON-Parses je Aktion (CORE-115).
- `src/HE_Scene/src/AudioEngine.cpp:820-821`: `std::make_shared<const HE::AudioEnvelope>(*envelope)` kopiert pro Stimmenstart den Punkte-Vektor (Heap). Gehört zu Audio-Slice; ggf. `shared_ptr` direkt aus dem Asset teilen statt kopieren.
- `src/HE_Rendering/src/RenderExtractor.cpp:263`: `meshSectionsOf` per Wert im Extraktor (CORE-108).
- `src/HE_Core/src/ContentManager/ContentManager.cpp:2928`: UI-thread-Aufruf von `retargetTree` (CORE-102).
- `src/HE_Net/src/PortMapper.cpp:56`: dritte `xmlEscape`-Kopie (CORE-117).
- `Application.cpp:788` ruft `m_loop.tick` jedes Frames, auch ohne Spiellogik-Modul (billig, nur Beobachtung); Window-Event-Callback (`Application.cpp:290-326`) ist ein `std::function` pro Event, bei Maus-Events mit drei aufeinanderfolgenden `switch` (`ProcessMouseEvent`, `ProcessGamepadEvent`, `ProcessEvent`): nicht relevant, nur zur Information.

## Unsicher / Messung nötig

- CORE-104/105: Durchsatz des Referenz-Scans auf realen Projekten (GB-Größe) vor/nach; der Gewinn hängt vom Anteil Binärdaten ab.
- CORE-102: tatsächliche UI-Blockierdauer beim Verschieben in einem großen Projekt (Schätzung "Sekunden" nicht gemessen).
- CORE-109: Ob STDF/ENDF/SGTP-Chunks Asset-Pfade enthalten, die ein Move umschreiben müsste (Verhaltensfrage, nicht in diesem Slice verifiziert).
- CORE-120: Kosten der SDL-Gamepad-Abfragen mit 1–4 Pads (SDL-Internas nicht gelesen).
- CORE-119: Startzeit-Anteil des Splash-Logo-Scans (Release vs. Debug).
- `MaterialAsset`/`MaterialShaderVariant` (`Assets.h:15-44,243-386`): `syncMaterialInstance` kopiert laut Kommentar (Z. 357-362) die generierten GLSL-Strings des Parents in jede Instanz; bei vielen Instanzen sind das Duplikate im RAM (Hash-gleiche Quellen). Ob `shared_ptr<const std::string>` sinnvoll wäre, hängt von Instanzanzahl ab; messen (Speicherprofil einer Szene mit vielen Material-Instanzen).
- `StaticMeshAsset`/`SkeletalMeshAsset` (`Assets.h:121-181`): lose Assets halten `vertices/normals/uvs` als SoA, gecookte nur `interleaved`; `boneIDs` als `uint32_t` (4/Vertex). Speicher/Upload-Kosten (Interleaving beim Upload, 4× breitere Joint-Indizes als nötig) nur mit konkreten Messungen bewertbar; Umbau würde das Chunk-Layout berühren (siehe `HAsset.h` Versionsregeln) und ist daher hier nur als Beobachtung vermerkt.

## Gelesen

Alle 35 Dateien des Slice 1 vollständig gelesen: ja (`Application.cpp` in einem Block bis Zeile 1093, alle übrigen Dateien ganz); zusätzlich gezielte Greps/Auszüge in `PlayerHost.cpp`, `RenderExtractor.cpp`, `AudioEngine.cpp`, `ContentManager.cpp`, `InputAssetPanel.cpp`, `EngineProfiler.cpp` zur Verifikation von Aufrufern.
